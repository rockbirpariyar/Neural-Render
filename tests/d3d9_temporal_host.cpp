// Direct GPU retention regression. Build with the real renderer implementation:
// g++ -std=c++20 -O2 -Wall -Wextra -DWIN32_LEAN_AND_MEAN -DNOMINMAX -municode
// -static tests/d3d9_temporal_host.cpp src/grayscale_d3d9.cpp src/motion_d3d9.cpp -ld3dcompiler_47
// -o build/d3d9_temporal_host.exe
// Only asynchronous occlusion DWORDs reach the CPU. No image readback/map.
#define wmain existing_gpu_fixture_entry_point
#include "d3d9_gpu_host.cpp"
#undef wmain
#include "../src/grayscale_d3d9.hpp"
#include <d3dcompiler.h>
#include <cstring>
#include <vector>
#include <cmath>

namespace
{
    template <typename T> void drop(T *&object) { if (object) object->Release(); object = nullptr; }
    bool require_temporal(bool value, const char *message)
    {
        if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
        return value;
    }
    constexpr D3DFORMAT intz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
    struct quad_vertex { float x, y, z, w, u, v; };
    struct gpu_predicate
    {
        IDirect3DDevice9 *device;
        com_ptr<IDirect3DPixelShader9> shader;
        com_ptr<IDirect3DSurface9> target;
        com_ptr<IDirect3DVertexBuffer9> vertices;
        std::vector<IDirect3DQuery9 *> queries;
        std::vector<std::string> labels;
        explicit gpu_predicate(IDirect3DDevice9 *value) : device(value) {}
        ~gpu_predicate() { for (auto *query : queries) query->Release(); }
        bool initialize()
        {
            constexpr char source[] = R"(
sampler2D image : register(s0);
float4 colors[4] : register(c0);
float4 mask : register(c4);
float4 main(float2 uv : TEXCOORD0) : COLOR0 {
    float4 wanted = uv.x < .25 ? colors[0] : uv.x < .5 ? colors[1] : uv.x < .75 ? colors[2] : colors[3];
    float4 difference = abs(tex2D(image, uv) - wanted) * mask;
    clip(0.000002 - max(max(difference.r, difference.g), max(difference.b, difference.a)));
    return 1;
})";
            com_ptr<ID3DBlob> bytecode, errors;
            if (!succeeded(D3DCompile(source, sizeof(source) - 1, "temporal_gpu_predicate", nullptr, nullptr,
                    "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bytecode.put(), errors.put()), "Compile(predicate)"))
            {
                if (errors) std::fprintf(stderr, "%s\n", static_cast<const char *>(errors->GetBufferPointer()));
                return false;
            }
            if (!succeeded(device->CreatePixelShader(static_cast<const DWORD *>(bytecode->GetBufferPointer()), shader.put()), "CreatePixelShader(predicate)") ||
                !succeeded(device->CreateRenderTarget(8, 8, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, target.put(), nullptr), "CreateRenderTarget(predicate)") ||
                !succeeded(device->CreateVertexBuffer(4 * sizeof(quad_vertex), D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_TEX1,
                    D3DPOOL_MANAGED, vertices.put(), nullptr), "CreateVertexBuffer(predicate)")) return false;
            const quad_vertex quad[] = {{-.5f,-.5f,0,1,0,0}, {7.5f,-.5f,0,1,1,0}, {-.5f,7.5f,0,1,0,1}, {7.5f,7.5f,0,1,1,1}};
            void *upload = nullptr;
            if (!succeeded(vertices->Lock(0, sizeof(quad), &upload, 0), "Lock(predicate vertex upload)")) return false;
            std::memcpy(upload, quad, sizeof(quad));
            return succeeded(vertices->Unlock(), "Unlock(predicate vertex upload)");
        }
        bool compare(IDirect3DTexture9 *input, const float *expected, bool red_only, std::string label)
        {
            IDirect3DQuery9 *query = nullptr;
            if (!succeeded(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query), "CreateQuery(predicate)")) return false;
            queries.push_back(query); labels.push_back(std::move(label));
            const D3DVIEWPORT9 viewport {0, 0, 8, 8, 0, 1};
            const float mask[] = {1, red_only ? 0.0f : 1.0f, red_only ? 0.0f : 1.0f, red_only ? 0.0f : 1.0f};
            if (!succeeded(device->SetDepthStencilSurface(nullptr), "SetDepthStencilSurface(predicate)") ||
                !succeeded(device->SetRenderTarget(0, target), "SetRenderTarget(predicate)") ||
                !succeeded(device->SetViewport(&viewport), "SetViewport(predicate)") ||
                !succeeded(device->SetTexture(0, input), "SetTexture(predicate)") ||
                !succeeded(device->SetPixelShader(shader), "SetPixelShader(predicate)") ||
                !succeeded(device->SetVertexShader(nullptr), "SetVertexShader(predicate)") ||
                !succeeded(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1), "SetFVF(predicate)") ||
                !succeeded(device->SetStreamSource(0, vertices, 0, sizeof(quad_vertex)), "SetStreamSource(predicate)") ||
                !succeeded(device->SetPixelShaderConstantF(0, expected, 4), "SetConstants(expected)") ||
                !succeeded(device->SetPixelShaderConstantF(4, mask, 1), "SetConstants(mask)")) return false;
            for (const auto state : {render_state{D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE},
                    {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_SCISSORTESTENABLE, FALSE},
                    {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_COLORWRITEENABLE, 0}, {D3DRS_SRGBWRITEENABLE, FALSE}})
                if (!succeeded(device->SetRenderState(state.type, state.value), "SetRenderState(predicate)")) return false;
            for (const auto sampler : {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER})
                if (!succeeded(device->SetSamplerState(0, sampler, D3DTEXF_POINT), "SetSamplerState(predicate)")) return false;
            if (!succeeded(device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE), "SetSamplerState(mip)") ||
                !succeeded(device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE), "SetSamplerState(sRGB)") ||
                !succeeded(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP), "SetSamplerState(addressU)") ||
                !succeeded(device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP), "SetSamplerState(addressV)") ||
                !succeeded(query->Issue(D3DISSUE_BEGIN), "Issue(predicate begin)") ||
                !succeeded(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2), "DrawPrimitive(predicate)") ||
                !succeeded(query->Issue(D3DISSUE_END), "Issue(predicate end)")) return false;
            return succeeded(device->SetTexture(0, nullptr), "Unbind(predicate)");
        }
        bool resolve()
        {
            const ULONGLONG start = GetTickCount64();
            std::vector<bool> complete(queries.size(), false);
            unsigned remaining = static_cast<unsigned>(queries.size());
            while (remaining != 0 && GetTickCount64() - start < 5000)
            {
                for (unsigned index = 0; index < queries.size(); ++index)
                {
                    if (complete[index]) continue;
                    DWORD pixels = 0;
                    const HRESULT result = queries[index]->GetData(&pixels, sizeof(pixels), 0);
                    if (result == S_FALSE) continue;
                    if (!succeeded(result, "GetData(nonblocking predicate)")) return false;
                    if (pixels != 64)
                    {
                        std::fprintf(stderr, "FAIL: GPU comparison '%s': %lu/64 samples match\n", labels[index].c_str(), pixels);
                        return false;
                    }
                    complete[index] = true; --remaining;
                }
                if (remaining)
                {
                    // Normal presentation advances command submission. No query
                    // flush flag, GPU fence, spin wait, or pixel readback.
                    if (!succeeded(device->Present(nullptr, nullptr, nullptr, nullptr), "Present(predicate progress)")) return false;
                    Sleep(10);
                }
            }
            if (!require_temporal(remaining == 0, "asynchronous predicate results timed out")) return false;
            std::printf("PASS: %zu GPU-only texture/display predicates (64 samples each)\n", queries.size());
            return true;
        }
    };

    struct frame_images
    {
        IDirect3DDevice9 *device;
        IDirect3DTexture9 *color = nullptr, *depth = nullptr, *output_copy = nullptr;
        IDirect3DSurface9 *color_surface = nullptr, *depth_surface = nullptr, *depth_target = nullptr, *output_surface = nullptr;
        UINT width = 0, height = 0, depth_width = 0, depth_height = 0;
        D3DFORMAT depth_format = intz;
        ~frame_images() { reset(); }
        void reset()
        {
            drop(color_surface); drop(depth_surface); drop(depth_target); drop(output_surface);
            drop(color); drop(depth); drop(output_copy);
        }
        bool initialize(UINT w, UINT h, UINT dw, UINT dh, D3DFORMAT format = intz)
        {
            reset(); width = w; height = h; depth_width = dw; depth_height = dh; depth_format = format;
            return succeeded(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &color, nullptr), "CreateTexture(color)") &&
                succeeded(color->GetSurfaceLevel(0, &color_surface), "GetSurfaceLevel(color)") &&
                succeeded(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &output_copy, nullptr), "CreateTexture(display copy)") &&
                succeeded(output_copy->GetSurfaceLevel(0, &output_surface), "GetSurfaceLevel(display copy)") &&
                succeeded(device->CreateTexture(dw, dh, 1, format == intz ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET,
                    format, D3DPOOL_DEFAULT, &depth, nullptr), "CreateTexture(depth source)") &&
                succeeded(depth->GetSurfaceLevel(0, &depth_surface), "GetSurfaceLevel(depth source)") &&
                succeeded(device->CreateRenderTarget(dw, dh, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth_target, nullptr), "CreateRenderTarget(depth fill)");
        }
        bool fill(unsigned phase, float z)
        {
            for (unsigned stripe = 0; stripe < 4; ++stripe)
            {
                const RECT area {static_cast<LONG>(stripe * width / 4), 0, static_cast<LONG>((stripe + 1) * width / 4), static_cast<LONG>(height)};
                if (!succeeded(device->ColorFill(color_surface, &area, palettes[phase % 3][stripe]), "ColorFill(current color)")) return false;
            }
            if (depth_format != intz)
            {
                // R32F source-format transition, populated by a GPU target clear.
                return succeeded(device->SetDepthStencilSurface(nullptr), "UnbindDepth(float fill)") &&
                    succeeded(device->SetRenderTarget(0, depth_surface), "SetRenderTarget(float fill)") &&
                    succeeded(device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, static_cast<unsigned>(std::round(z * 255)), 0, 0), 1, 0), "Clear(float depth)") &&
                    succeeded(device->SetRenderTarget(0, color_surface), "SetRenderTarget(color after float fill)");
            }
            return succeeded(device->SetDepthStencilSurface(nullptr), "UnbindDepth(fill)") &&
                succeeded(device->SetRenderTarget(0, depth_target), "SetRenderTarget(depth fill)") &&
                succeeded(device->SetDepthStencilSurface(depth_surface), "SetDepthStencilSurface(fill)") &&
                succeeded(device->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, z, 0), "Clear(INTZ depth)") &&
                succeeded(device->SetDepthStencilSurface(nullptr), "UnbindDepth(after fill)") &&
                succeeded(device->SetRenderTarget(0, color_surface), "SetRenderTarget(color)");
        }
    };

    void expected_colors(unsigned phase, float (&out)[16])
    {
        for (unsigned stripe = 0; stripe < 4; ++stripe)
        {
            const DWORD c = palettes[phase % 3][stripe];
            out[stripe * 4] = ((c >> 16) & 255) / 255.0f;
            out[stripe * 4 + 1] = ((c >> 8) & 255) / 255.0f;
            out[stripe * 4 + 2] = (c & 255) / 255.0f;
            out[stripe * 4 + 3] = ((c >> 24) & 255) / 255.0f;
        }
    }
}

#ifndef ENR_TEMPORAL_FIXTURE_HELPERS_ONLY
int wmain()
{
    test_objects objects;
    wchar_t system_path[MAX_PATH] {};
    GetSystemDirectoryW(system_path, MAX_PATH);
    const auto dll_path = std::filesystem::path(system_path) / L"d3d9.dll";
    objects.module = LoadLibraryW(dll_path.c_str());
    if (!objects.module) return windows_failure("LoadLibrary(system D3D9)");
    using create_fn = IDirect3D9 *(WINAPI *)(UINT);
    const auto create_d3d = std::bit_cast<create_fn>(GetProcAddress(objects.module, "Direct3DCreate9"));
    objects.window = CreateWindowExW(0, L"STATIC", L"ENR temporal GPU regression", WS_OVERLAPPEDWINDOW,
        0, 0, 256, 192, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    objects.d3d = create_d3d(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS parameters {};
    parameters.BackBufferWidth = 256; parameters.BackBufferHeight = 192;
    parameters.BackBufferFormat = D3DFMT_A8R8G8B8; parameters.BackBufferCount = 1;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD; parameters.hDeviceWindow = objects.window;
    parameters.Windowed = TRUE; parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (!succeeded(objects.d3d->CreateDevice(0, D3DDEVTYPE_HAL, objects.window,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &parameters, &objects.device), "CreateDevice(native)")) return 1;
    enr::grayscale_d3d9 renderer;
    frame_images images {objects.device};
    gpu_predicate predicate {objects.device};
    if (!images.initialize(128, 96, 64, 48) || !predicate.initialize()) return 1;
    com_ptr<IDirect3DTexture9> application_texture;
    com_ptr<IDirect3DVertexBuffer9> application_vertices;
    if (!succeeded(objects.device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, application_texture.put(), nullptr), "CreateTexture(application)") ||
        !succeeded(objects.device->CreateVertexBuffer(80, 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, application_vertices.put(), nullptr), "CreateVertexBuffer(application)")) return 1;
    IDirect3DTexture9 *stable_color = nullptr, *stable_depth = nullptr;
    std::uint64_t frame = 0, generation = 1;
    unsigned prior_phase = 0;
    float prior_depth = 0;
    const auto render_frame = [&](unsigned phase, float z, enr::temporal_view mode, bool provide_depth, bool expected_before, bool compare_display)
    {
        ++frame;
        if (!images.fill(phase, z) || !set_game_state(objects.device, images.width, images.height, application_texture, application_vertices)) return false;
        RECT before_scissor {}; D3DVIEWPORT9 before_viewport {};
        objects.device->GetScissorRect(&before_scissor); objects.device->GetViewport(&before_viewport);
        const float constants[] = {.123f, .456f, .789f, .321f, .937f, .823f, .719f, .617f};
        objects.device->SetPixelShader(nullptr);
        objects.device->SetPixelShaderConstantF(0, constants, 2);
        if (!succeeded(objects.device->BeginScene(), "BeginScene(temporal)")) return false;
        enr::temporal_report report;
        const HRESULT result = renderer.render_temporal(objects.device, images.color_surface, provide_depth ? images.depth : nullptr,
            generation, frame, mode, false, false, report);
        if (!succeeded(result, "render_temporal") || !succeeded(report.history_result, "history capture") ||
            !require_temporal(report.valid_before == expected_before, "incorrect previous-history validity") ||
            !require_temporal(report.valid_after == provide_depth && renderer.history_valid() == provide_depth, "incorrect captured-history validity") ||
            !require_temporal(report.displayed_history == (expected_before && (mode == enr::temporal_view::previous_color || mode == enr::temporal_view::previous_depth)), "incorrect debug history selection")) return false;
        RECT after_scissor {}; D3DVIEWPORT9 after_viewport {}; float after_constants[8] {};
        objects.device->GetScissorRect(&after_scissor); objects.device->GetViewport(&after_viewport);
        objects.device->GetPixelShaderConstantF(0, after_constants, 2);
        if (!require_temporal(EqualRect(&before_scissor, &after_scissor) &&
                std::memcmp(&before_viewport, &after_viewport, sizeof(before_viewport)) == 0 &&
                std::memcmp(constants, after_constants, sizeof(constants)) == 0, "temporal renderer changed viewport/scissor/PS constants")) return false;
        for (const auto state : game_states)
        {
            DWORD value = 0; objects.device->GetRenderState(state.type, &value);
            if (!require_temporal(value == state.value, "temporal renderer changed application render state")) return false;
        }
        if (!succeeded(objects.device->EndScene(), "EndScene(temporal)")) return false;
        if (compare_display && !succeeded(objects.device->StretchRect(images.color_surface, nullptr, images.output_surface, nullptr, D3DTEXF_NONE), "GPU copy(display predicate)")) return false;
        if (!succeeded(objects.device->BeginScene(), "BeginScene(predicates)")) return false;
        float expected[16] {}; expected_colors(phase, expected);
        if (provide_depth)
        {
            if (!predicate.compare(renderer.previous_color(), expected, false, "captured color frame " + std::to_string(frame))) return false;
            for (unsigned stripe = 0; stripe < 4; ++stripe) expected[stripe * 4] = z;
            if (!predicate.compare(renderer.previous_depth(), expected, true, "raw depth frame " + std::to_string(frame))) return false;
            D3DSURFACE_DESC depth_description {};
            renderer.previous_depth()->GetLevelDesc(0, &depth_description);
            if (!require_temporal(depth_description.Format == D3DFMT_R32F && depth_description.Width == images.depth_width &&
                    depth_description.Height == images.depth_height, "depth history format/resolution differs")) return false;
        }
        if (compare_display)
        {
            expected_colors(prior_phase, expected);
            for (unsigned stripe = 0; stripe < 4; ++stripe)
            {
                expected[stripe * 4 + 3] = ((palettes[phase % 3][stripe] >> 24) & 255) / 255.0f;
                if (mode == enr::temporal_view::previous_depth)
                    expected[stripe * 4] = expected[stripe * 4 + 1] = expected[stripe * 4 + 2] = std::round(prior_depth * 255) / 255;
            }
            if (!predicate.compare(images.output_copy, expected, false, "displayed previous frame " + std::to_string(frame))) return false;
        }
        if (!succeeded(objects.device->EndScene(), "EndScene(predicates)") ||
            !succeeded(objects.device->Present(nullptr, nullptr, nullptr, nullptr), "Present(native)")) return false;
        prior_phase = phase; prior_depth = z;
        return true;
    };
    if (!require_temporal(!renderer.history_valid(), "fresh renderer history is valid") ||
        !render_frame(0, .2f, enr::temporal_view::previous_color, true, false, false)) return 1;
    stable_color = renderer.previous_color(); stable_depth = renderer.previous_depth();
    for (unsigned index = 1; index <= 18; ++index)
    {
        if (!render_frame(index % 3, index % 2 ? .8f : .2f,
                index % 5 ? enr::temporal_view::previous_color : enr::temporal_view::previous_depth, true, true, true) ||
            !require_temporal(renderer.previous_color() == stable_color && renderer.previous_depth() == stable_depth, "history texture churn on steady frames")) return 1;
    }
    ++generation;
    if (!render_frame(1, .8f, enr::temporal_view::previous_color, true, false, false) ||
        !require_temporal(renderer.previous_color() == stable_color && renderer.previous_depth() == stable_depth, "generation-only change recreated textures") ||
        !render_frame(2, .2f, enr::temporal_view::previous_color, false, false, false) ||
        !render_frame(0, .8f, enr::temporal_view::previous_color, true, false, false) ||
        !require_temporal(renderer.previous_color() == stable_color && renderer.previous_depth() == stable_depth, "missing-depth interval recreated compatible textures")) return 1;
    frame += 2;
    if (!render_frame(1, .2f, enr::temporal_view::previous_color, true, false, false)) return 1;
    if (!images.initialize(160, 120, 96, 64) ||
        !render_frame(2, .8f, enr::temporal_view::previous_color, true, false, false) ||
        !render_frame(0, .2f, enr::temporal_view::previous_depth, true, true, true)) return 1;
    stable_color = renderer.previous_color(); stable_depth = renderer.previous_depth();
    if (!images.initialize(160, 120, 96, 64, D3DFMT_R32F) ||
        !render_frame(1, .8f, enr::temporal_view::previous_color, true, false, false) ||
        !require_temporal(renderer.previous_color() == stable_color && renderer.previous_depth() == stable_depth,
            "compatible source-format transition recreated history textures") ||
        !render_frame(2, .2f, enr::temporal_view::previous_depth, true, true, true)) return 1;
    if (!images.initialize(1920, 1080, 1920, 1080) ||
        !render_frame(0, .8f, enr::temporal_view::previous_color, true, false, false) ||
        !render_frame(1, .2f, enr::temporal_view::previous_color, true, true, true) ||
        !render_frame(2, .8f, enr::temporal_view::previous_depth, true, true, true)) return 1;
    if (!predicate.resolve()) return 1;
    renderer.reset();
    if (!require_temporal(!renderer.history_valid(), "reset retained history validity")) return 1;
    if (!render_frame(1, .8f, enr::temporal_view::previous_color, true, false, false) ||
        !render_frame(2, .2f, enr::temporal_view::previous_color, true, true, true) || !predicate.resolve()) return 1;
    std::printf("PASS: GPU-only N-1 color/depth, 18 changing debug frames without feedback, RGBA/raw INTZ-to-R32F accuracy, "
        "first-frame/reset/resize/format/generation/gap/missing-depth invalidation, distinct depth resolution and 1920x1080 pairs, "
        "stable texture identities, render-state restoration; no CPU image readback\n");
    return 0;
}
#endif
