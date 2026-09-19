#include "depth_binding.hpp"

#include <cstring>
#include <string>

namespace
{
    // Keep this byte-for-byte identical to shaders/ENR_Depth.addonfx.
    constexpr char depth_effect[] = R"ENR(// ENR v0.004: public DEPTH binding only; image rendering stays in enr.addon32.
texture ENRDepthSource : DEPTH;
sampler ENRDepthSampler { Texture = ENRDepthSource; };

void ENRDepthVertex(uint id : SV_VertexID, out float4 position : SV_Position,
    out float2 uv : TEXCOORD0)
{
    uv = float2(id == 1 ? 2.0 : 0.0, id == 2 ? 2.0 : 0.0);
    position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 ENRDepthBindingPixel(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return tex2D(ENRDepthSampler, uv).rrrr;
}

technique ENRDepthBinding < bool hidden = true; bool enabled = false; >
{
    pass
    {
        VertexShader = ENRDepthVertex;
        PixelShader = ENRDepthBindingPixel;
        ColorWriteMask = 0;
        StencilEnable = false;
        BlendEnable = false;
    }
}
)ENR";

    struct variable_search
    {
        reshade::api::effect_texture_variable selected = {};
        bool is_own = false;
    };

    void inspect_texture(reshade::api::effect_runtime *runtime,
        reshade::api::effect_texture_variable variable, void *user_data)
    {
        auto &search = *static_cast<variable_search *>(user_data);
        if (search.is_own)
            return;

        char name[128] = {};
        runtime->get_texture_variable_name(variable, name);
        if (std::strcmp(name, "ENRDepthSource") == 0)
        {
            char effect_name[MAX_PATH] = {};
            runtime->get_texture_variable_effect_name(variable, effect_name);
            if (std::strcmp(effect_name, "ENR_Depth.addonfx") == 0)
            {
                search.selected = variable;
                search.is_own = true;
            }
        }
        else if (search.selected == 0
            && (std::strcmp(name, "DepthBufferTex") == 0
                || std::strcmp(name, "ReShade::DepthBufferTex") == 0))
        {
            // Compatibility with the installed standard ReShade.fxh declaration.
            search.selected = variable;
        }
    }

    bool supported_depth_format(D3DFORMAT format)
    {
        // These formats expose depth directly in the sampled red channel.
        // RAWZ requires a different unpacking shader, so is deliberately excluded.
        return format == D3DFMT_R32F
            || format == static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'))
            || format == static_cast<D3DFORMAT>(MAKEFOURCC('D', 'F', '1', '6'))
            || format == static_cast<D3DFORMAT>(MAKEFOURCC('D', 'F', '2', '4'));
    }
}

namespace enr
{
    depth_binding::~depth_binding()
    {
        reset();
    }

    void depth_binding::release_source()
    {
        if (texture_ != nullptr)
            texture_->Release();
        texture_ = nullptr;
        description_ = {};
        view_ = {};
        acquisition_result_ = S_FALSE;
    }

    void depth_binding::reset()
    {
        release_source();
        runtime_ = nullptr;
        variable_ = {};
        searched_ = false;
        ++generation_;
    }

    HRESULT depth_binding::acquire(reshade::api::effect_runtime *runtime,
        bool effects_processed_this_frame, depth_source &out)
    {
        out = {};
        if (runtime != runtime_)
        {
            reset();
            runtime_ = runtime;
        }
        if (runtime == nullptr || !effects_processed_this_frame)
        {
            if (view_ != 0)
            {
                release_source();
                ++generation_;
            }
            return S_FALSE;
        }
        if (runtime->get_device()->get_api() != reshade::api::device_api::d3d9)
            return D3DERR_NOTAVAILABLE;

        if (!searched_)
        {
            variable_search search;
            runtime->enumerate_texture_variables(nullptr, inspect_texture, &search);
            variable_ = search.selected;
            searched_ = true;
        }
        if (variable_ == 0)
            return S_FALSE;

        // ReShade leaves these untouched when a declared semantic is unbound.
        reshade::api::resource_view selected_view = {};
        runtime->get_texture_binding(variable_, &selected_view, nullptr);
        if (selected_view != view_)
        {
            release_source();
            ++generation_;
            view_ = selected_view;
            if (selected_view != 0)
            {
                // D3D9 SRVs are native texture pointers with an optional sRGB bit.
                // Only COM scalar/out-parameter APIs are used: no x86 C++ struct
                // returns cross the MinGW/MSVC ABI boundary.
                auto *object = reinterpret_cast<IUnknown *>(
                    static_cast<std::uintptr_t>(selected_view.handle & ~1ull));
                acquisition_result_ = object->QueryInterface(__uuidof(IDirect3DTexture9),
                    reinterpret_cast<void **>(&texture_));
                if (SUCCEEDED(acquisition_result_))
                    acquisition_result_ = texture_->GetLevelDesc(0, &description_);
                if (SUCCEEDED(acquisition_result_)
                    && (description_.Width == 0 || description_.Height == 0
                        || description_.MultiSampleType != D3DMULTISAMPLE_NONE
                        || !supported_depth_format(description_.Format)))
                    acquisition_result_ = D3DERR_NOTAVAILABLE;
                if (FAILED(acquisition_result_) && texture_ != nullptr)
                {
                    texture_->Release();
                    texture_ = nullptr;
                }
            }
        }

        if (acquisition_result_ == S_OK && texture_ != nullptr)
        {
            out.texture = texture_;
            out.width = description_.Width;
            out.height = description_.Height;
            out.format = description_.Format;
            out.generation = generation_;
        }
        return acquisition_result_;
    }

    HRESULT ensure_depth_effect(HMODULE addon_module)
    {
        wchar_t module_path[32768] = {};
        const DWORD length = GetModuleFileNameW(addon_module, module_path,
            static_cast<DWORD>(sizeof(module_path) / sizeof(module_path[0])));
        if (length == 0)
            return HRESULT_FROM_WIN32(GetLastError());
        if (length >= sizeof(module_path) / sizeof(module_path[0]))
            return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);

        std::wstring path(module_path, length);
        const std::size_t separator = path.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
            return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
        path.resize(separator + 1);
        path += L"ENR_Depth.addonfx";

        const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
                ? S_FALSE : HRESULT_FROM_WIN32(error);
        }

        DWORD written = 0;
        constexpr DWORD size = static_cast<DWORD>(sizeof(depth_effect) - 1);
        const BOOL success = WriteFile(file, depth_effect, size, &written, nullptr);
        const DWORD error = success ? ERROR_WRITE_FAULT : GetLastError();
        CloseHandle(file);
        if (!success || written != size)
        {
            // This invocation created the file exclusively; remove its partial output.
            DeleteFileW(path.c_str());
            return HRESULT_FROM_WIN32(error);
        }
        return S_OK;
    }
}
