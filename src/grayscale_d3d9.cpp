#include "grayscale_d3d9.hpp"

#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <iterator>

namespace
{
    template <typename T>
    void release(T *&object)
    {
        if (object != nullptr)
            object->Release();
        object = nullptr;
    }

    // Like ReShade's d3d9_impl_state_block.cpp, preserve RT/depth and the
    // viewport explicitly: D3DSBT_ALL does not preserve render targets.
    class saved_state final
    {
    public:
        saved_state(IDirect3DDevice9 *device, IDirect3DStateBlock9 *block,
            bool mixed_vertex_processing) : device_(device), block_(block),
            mixed_vertex_processing_(mixed_vertex_processing) {}
        ~saved_state()
        {
            if (captured_)
                restore();
            for (auto &target : targets_)
                release(target);
            release(depth_);
            release(stream_);
        }

        HRESULT capture(const D3DCAPS9 &caps)
        {
            HRESULT result = block_->Capture();
            if (FAILED(result)) return result;
            result = device_->GetViewport(&viewport_);
            if (FAILED(result)) return result;
            result = device_->GetScissorRect(&scissor_);
            if (FAILED(result)) return result;

            target_count_ = std::min<DWORD>(caps.NumSimultaneousRTs, std::size(targets_));
            if (target_count_ == 0) return D3DERR_NOTAVAILABLE;
            for (DWORD index = 0; index < target_count_; ++index)
            {
                result = device_->GetRenderTarget(index, &targets_[index]);
                if (FAILED(result) && (index == 0 || result != D3DERR_NOTFOUND))
                    return result;
            }
            result = device_->GetDepthStencilSurface(&depth_);
            if (FAILED(result) && result != D3DERR_NOTFOUND) return result;
            result = device_->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgb_write_);
            if (FAILED(result)) return result;
            result = device_->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &srgb_texture_);
            if (FAILED(result)) return result;
            result = device_->GetStreamSource(0, &stream_, &stream_offset_, &stream_stride_);
            if (FAILED(result)) return result;

            stream_count_ = std::min<UINT>(caps.MaxStreams, std::size(stream_frequencies_));
            for (UINT index = 0; index < stream_count_; ++index)
            {
                result = device_->GetStreamSourceFreq(index, &stream_frequencies_[index]);
                if (FAILED(result)) return result;
            }

            if (mixed_vertex_processing_)
                software_vertex_processing_ = device_->GetSoftwareVertexProcessing();

            captured_ = true;
            return S_OK;
        }

        HRESULT restore()
        {
            if (!captured_) return S_OK;
            captured_ = false;
            HRESULT result = S_OK;
            const auto remember_failure = [&result](HRESULT next)
            {
                if (SUCCEEDED(result) && FAILED(next)) result = next;
            };

            // Remove temporary attachments before restoring potentially different
            // sizes/MSAA settings. SetRenderTarget resets the viewport/scissor.
            remember_failure(device_->SetDepthStencilSurface(nullptr));
            for (DWORD index = 1; index < target_count_; ++index)
                remember_failure(device_->SetRenderTarget(index, nullptr));
            remember_failure(device_->SetRenderTarget(0, targets_[0]));
            for (DWORD index = 1; index < target_count_; ++index)
                remember_failure(device_->SetRenderTarget(index, targets_[index]));
            remember_failure(device_->SetDepthStencilSurface(depth_));
            remember_failure(block_->Apply());
            if (mixed_vertex_processing_)
                remember_failure(device_->SetSoftwareVertexProcessing(software_vertex_processing_));

            // Explicit restores also protect against overlay state-block quirks.
            remember_failure(device_->SetRenderState(D3DRS_SRGBWRITEENABLE, srgb_write_));
            remember_failure(device_->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, srgb_texture_));
            remember_failure(device_->SetStreamSource(0, stream_, stream_offset_, stream_stride_));
            for (UINT index = 0; index < stream_count_; ++index)
                remember_failure(device_->SetStreamSourceFreq(index, stream_frequencies_[index]));
            remember_failure(device_->SetViewport(&viewport_));
            remember_failure(device_->SetScissorRect(&scissor_));
            return result;
        }

        DWORD target_count() const { return target_count_; }
        UINT stream_count() const { return stream_count_; }
        bool mixed_vertex_processing() const { return mixed_vertex_processing_; }

    private:
        IDirect3DDevice9 *device_;
        IDirect3DStateBlock9 *block_; // Borrowed from the reusable renderer resources.
        IDirect3DSurface9 *targets_[4] = {};
        IDirect3DSurface9 *depth_ = nullptr;
        IDirect3DVertexBuffer9 *stream_ = nullptr;
        D3DVIEWPORT9 viewport_ = {};
        RECT scissor_ = {};
        UINT stream_offset_ = 0;
        UINT stream_stride_ = 0;
        UINT stream_frequencies_[16] = {};
        UINT stream_count_ = 0;
        DWORD target_count_ = 0;
        DWORD srgb_write_ = 0;
        DWORD srgb_texture_ = 0;
        BOOL software_vertex_processing_ = FALSE;
        bool mixed_vertex_processing_ = false;
        bool captured_ = false;
    };
}

namespace enr
{
    grayscale_d3d9::~grayscale_d3d9()
    {
        reset();
    }

    void grayscale_d3d9::reset()
    {
        for (auto &query : probe_queries_)
            release(query);
        release(probe_vertices_);
        release(probe_shader_);
        release(probe_target_);
        release(depth_shader_);
        release(state_block_);
        release(vertices_);
        release(scratch_surface_);
        release(scratch_);
        release(shader_);
        device_ = nullptr;
        width_ = height_ = 0;
        format_ = D3DFMT_UNKNOWN;
        caps_ = {};
        mixed_vertex_processing_ = false;
        attempted_device_ = nullptr;
        attempted_description_ = {};
        initialization_result_ = E_FAIL;
        depth_shader_attempted_ = false;
        depth_shader_result_ = S_OK;
        probe_attempted_ = probe_disabled_ = probe_pending_ = probe_sampled_ = false;
        probe_error_ = S_OK;
        probe_last_sample_ = probe_last_poll_ = 0;
        probe_generation_ = 0;
    }

    HRESULT grayscale_d3d9::initialize(IDirect3DDevice9 *device,
        const D3DSURFACE_DESC &description)
    {
        reset();
        HRESULT result = device->GetDeviceCaps(&caps_);
        if (FAILED(result)) return result;
        D3DDEVICE_CREATION_PARAMETERS parameters = {};
        result = device->GetCreationParameters(&parameters);
        if (FAILED(result)) return result;
        mixed_vertex_processing_ = (parameters.BehaviorFlags & D3DCREATE_MIXED_VERTEXPROCESSING) != 0;
        if (caps_.PixelShaderVersion < D3DPS_VERSION(2, 0))
            return D3DERR_NOTAVAILABLE;

        constexpr char source[] =
            "sampler2D frame : register(s0);\n"
            "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
            "    float4 color = tex2D(frame, uv);\n"
            "    float gray = dot(color.rgb, float3(0.299, 0.587, 0.114));\n"
            // Destination alpha is preserved by the RGB-only write mask. Opaque
            // shader output avoids inherited alpha-to-coverage affecting coverage.
            "    return float4(gray, gray, gray, 1.0);\n"
            "}\n";
        ID3DBlob *bytecode = nullptr;
        ID3DBlob *diagnostics = nullptr;
        const char *profile = caps_.PixelShaderVersion >= D3DPS_VERSION(3, 0) ? "ps_3_0" : "ps_2_0";
        result = D3DCompile(source, sizeof(source) - 1, "ENR grayscale", nullptr,
            nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &diagnostics);
        release(diagnostics);
        if (SUCCEEDED(result))
            result = device->CreatePixelShader(static_cast<const DWORD *>(bytecode->GetBufferPointer()), &shader_);
        release(bytecode);
        if (SUCCEEDED(result))
            result = device->CreateTexture(description.Width, description.Height, 1,
                D3DUSAGE_RENDERTARGET, description.Format, D3DPOOL_DEFAULT, &scratch_, nullptr);
        if (SUCCEEDED(result))
            result = scratch_->GetSurfaceLevel(0, &scratch_surface_);
        // Upload the fixed fullscreen geometry only at initialization. This is a
        // CPU-to-GPU vertex upload, never a frame readback or pixel inspection.
        const float right = static_cast<float>(description.Width) - 0.5f;
        const float bottom = static_cast<float>(description.Height) - 0.5f;
        const vertex fullscreen_vertices[] = {
            { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
            { right, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
            { -0.5f, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
            { right, bottom, 0.0f, 1.0f, 1.0f, 1.0f }
        };
        const DWORD vertex_usage = D3DUSAGE_WRITEONLY |
            ((parameters.BehaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0 ?
                D3DUSAGE_SOFTWAREPROCESSING : 0);
        if (SUCCEEDED(result))
            result = device->CreateVertexBuffer(sizeof(fullscreen_vertices), vertex_usage,
                D3DFVF_XYZRHW | D3DFVF_TEX1, D3DPOOL_DEFAULT, &vertices_, nullptr);
        if (SUCCEEDED(result))
        {
            void *destination = nullptr;
            result = vertices_->Lock(0, sizeof(fullscreen_vertices), &destination, 0);
            if (SUCCEEDED(result))
            {
                std::memcpy(destination, fullscreen_vertices, sizeof(fullscreen_vertices));
                result = vertices_->Unlock();
            }
        }
        if (SUCCEEDED(result))
            result = device->CreateStateBlock(D3DSBT_ALL, &state_block_);
        if (FAILED(result))
        {
            reset();
            return result;
        }

        device_ = device;
        width_ = description.Width;
        height_ = description.Height;
        format_ = description.Format;
        return S_OK;
    }

    HRESULT grayscale_d3d9::render(IDirect3DDevice9 *device,
        IDirect3DSurface9 *backbuffer, bool &initialized_now)
    {
        return render_impl(device, backbuffer, nullptr, false, false, initialized_now);
    }

    HRESULT grayscale_d3d9::render_depth(IDirect3DDevice9 *device,
        IDirect3DSurface9 *backbuffer, IDirect3DTexture9 *depth,
        bool linearize, bool reversed, bool &initialized_now)
    {
        if (depth == nullptr)
        {
            initialized_now = false;
            return E_INVALIDARG;
        }
        return render_impl(device, backbuffer, depth, linearize, reversed, initialized_now);
    }

    HRESULT grayscale_d3d9::render_impl(IDirect3DDevice9 *device,
        IDirect3DSurface9 *backbuffer, IDirect3DTexture9 *depth,
        bool linearize, bool reversed, bool &initialized_now)
    {
        initialized_now = false;
        if (device == nullptr || backbuffer == nullptr) return E_INVALIDARG;

        D3DSURFACE_DESC description = {};
        HRESULT result = backbuffer->GetDesc(&description);
        if (FAILED(result)) return result;
        if (description.Width == 0 || description.Height == 0 ||
            (description.Usage & D3DUSAGE_RENDERTARGET) == 0)
            return D3DERR_INVALIDCALL;

        if (device_ != device || width_ != description.Width || height_ != description.Height ||
            format_ != description.Format)
        {
            // A persistent allocation/shader failure must not compile or create
            // resources on every presentation. A reset or new configuration can
            // retry initialization; ordinary frames only return the saved error.
            if (attempted_device_ == device &&
                attempted_description_.Width == description.Width &&
                attempted_description_.Height == description.Height &&
                attempted_description_.Format == description.Format &&
                FAILED(initialization_result_))
                return initialization_result_;
            result = initialize(device, description);
            attempted_device_ = device;
            attempted_description_ = description;
            initialization_result_ = result;
            if (FAILED(result)) return result;
            initialized_now = true;
        }

        if (depth != nullptr)
        {
            if (!depth_shader_attempted_)
            {
                depth_shader_attempted_ = true;
                depth_shader_result_ = initialize_depth_shader();
            }
            if (FAILED(depth_shader_result_)) return depth_shader_result_;
        }
        else
        {
            // A backbuffer cannot be sampled while writing to it. The temporary
            // texture contains only this frame and is overwritten before every draw.
            // StretchRect also resolves an MSAA backbuffer to this non-MSAA texture.
            result = device->StretchRect(backbuffer, nullptr, scratch_surface_, nullptr, D3DTEXF_NONE);
            if (FAILED(result)) return result;
        }

        const float depth_options[] = { linearize ? 1.0f : 0.0f, reversed ? 1.0f : 0.0f, 0.0f, 0.0f };
        return draw_pass(backbuffer, depth != nullptr ? depth : scratch_,
            depth != nullptr ? depth_shader_ : shader_, vertices_, width_, height_,
            depth != nullptr ? depth_options : nullptr,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    }

    HRESULT grayscale_d3d9::draw_pass(IDirect3DSurface9 *target, IDirect3DTexture9 *input,
        IDirect3DPixelShader9 *shader, IDirect3DVertexBuffer9 *vertices,
        UINT width, UINT height, const float *constants, DWORD color_mask,
        IDirect3DQuery9 **queries)
    {
        IDirect3DDevice9 *device = device_;
        saved_state previous(device, state_block_, mixed_vertex_processing_);
        HRESULT result = previous.capture(caps_);
        if (FAILED(result)) return result;

        const auto draw = [&]() -> HRESULT
        {
#define ENR_CHECK_STATE(call) do { const HRESULT checked_result = (call); if (FAILED(checked_result)) return checked_result; } while (false)
            ENR_CHECK_STATE(device->SetDepthStencilSurface(nullptr));
            for (DWORD index = 1; index < previous.target_count(); ++index)
                ENR_CHECK_STATE(device->SetRenderTarget(index, nullptr));
            ENR_CHECK_STATE(device->SetRenderTarget(0, target));
            const D3DVIEWPORT9 viewport = { 0, 0, width, height, 0.0f, 1.0f };
            ENR_CHECK_STATE(device->SetViewport(&viewport));
            if (previous.mixed_vertex_processing())
                ENR_CHECK_STATE(device->SetSoftwareVertexProcessing(FALSE));
            for (UINT index = 0; index < previous.stream_count(); ++index)
                ENR_CHECK_STATE(device->SetStreamSourceFreq(index, 1));

            const struct { D3DRENDERSTATETYPE state; DWORD value; } render_states[] = {
                { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE },
                { D3DRS_STENCILENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE },
                { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_SEPARATEALPHABLENDENABLE, FALSE },
                { D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_FILLMODE, D3DFILL_SOLID },
                { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_FOGENABLE, FALSE },
                { D3DRS_LIGHTING, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE },
                { D3DRS_DITHERENABLE, FALSE }, { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_WRAP0, 0 },
                { D3DRS_MULTISAMPLEANTIALIAS, TRUE }, { D3DRS_MULTISAMPLEMASK, 0xffffffff },
                // Preserve the destination alpha bits exactly, including on MSAA targets.
                { D3DRS_COLORWRITEENABLE, color_mask }
            };
            for (const auto &state : render_states)
                ENR_CHECK_STATE(device->SetRenderState(state.state, state.value));

            const struct { D3DSAMPLERSTATETYPE state; DWORD value; } sampler_states[] = {
                { D3DSAMP_MINFILTER, D3DTEXF_POINT }, { D3DSAMP_MAGFILTER, D3DTEXF_POINT },
                { D3DSAMP_MIPFILTER, D3DTEXF_NONE }, { D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP },
                { D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP }, { D3DSAMP_SRGBTEXTURE, FALSE },
                { D3DSAMP_MIPMAPLODBIAS, 0 }, { D3DSAMP_MAXMIPLEVEL, 0 }
            };
            for (const auto &state : sampler_states)
                ENR_CHECK_STATE(device->SetSamplerState(0, state.state, state.value));
            ENR_CHECK_STATE(device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE));
            ENR_CHECK_STATE(device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0));
            ENR_CHECK_STATE(device->SetVertexShader(nullptr));
            ENR_CHECK_STATE(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
            ENR_CHECK_STATE(device->SetStreamSource(0, vertices, 0, sizeof(vertex)));
            ENR_CHECK_STATE(device->SetPixelShader(shader));
            ENR_CHECK_STATE(device->SetTexture(0, input));
            if (constants != nullptr)
                ENR_CHECK_STATE(device->SetPixelShaderConstantF(0, constants, 1));

            if (queries != nullptr)
            {
                // Query zero counts non-clear samples; the other two count
                // different deterministic predicates of those depth samples.
                // There are only 768 probe fragments once every five seconds.
                constexpr float probe_options[3][4] = {
                    { 0.0f, 0.0f, 0.0f, 0.0f },
                    { 65521.0f, 1.0f, 0.0f, 1.0f },
                    { 131071.0f, 73.0f, 19.0f, 1.0f }
                };
                for (unsigned index = 0; index < 3; ++index)
                {
                    ENR_CHECK_STATE(device->SetPixelShaderConstantF(0, probe_options[index], 1));
                    ENR_CHECK_STATE(queries[index]->Issue(D3DISSUE_BEGIN));
                    const HRESULT draw_result = device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
                    // End a begun query even if the draw failed.
                    const HRESULT end_result = queries[index]->Issue(D3DISSUE_END);
                    if (FAILED(draw_result)) return draw_result;
                    if (FAILED(end_result)) return end_result;
                }
                return S_OK;
            }

            return device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
#undef ENR_CHECK_STATE
        };

        result = draw();
        const HRESULT restore_result = previous.restore();
        return FAILED(result) ? result : restore_result;
    }

    HRESULT grayscale_d3d9::initialize_depth_shader()
    {
        constexpr char source[] =
            "sampler2D scene_depth : register(s0);\n"
            "float4 options : register(c0);\n"
            "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
            "    float depth = tex2D(scene_depth, uv).r;\n"
            "    depth = options.y > 0.5 ? 1.0 - depth : depth;\n"
            "    float shown = options.x > 0.5 ? rcp(1000.0 - depth * 999.0) : depth;\n"
            "    shown = saturate(shown);\n"
            "    return float4(shown, shown, shown, 1.0);\n"
            "}\n";
        ID3DBlob *bytecode = nullptr;
        ID3DBlob *diagnostics = nullptr;
        const char *profile = caps_.PixelShaderVersion >= D3DPS_VERSION(3, 0) ? "ps_3_0" : "ps_2_0";
        HRESULT result = D3DCompile(source, sizeof(source) - 1, "ENR depth visualization",
            nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &bytecode, &diagnostics);
        release(diagnostics);
        if (SUCCEEDED(result))
            result = device_->CreatePixelShader(static_cast<const DWORD *>(bytecode->GetBufferPointer()), &depth_shader_);
        release(bytecode);
        return result;
    }

    HRESULT grayscale_d3d9::initialize_probe()
    {
        constexpr char source[] =
            "sampler2D scene_depth : register(s0);\n"
            "float4 options : register(c0);\n"
            "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
            "    float depth = tex2D(scene_depth, uv).r;\n"
            // Exactly zero and one are common clear values. This tests actual
            // sampled depth, not application draw counts or resource handles.
            "    clip((depth > 0.0 && depth < 1.0) ? 1.0 : -1.0);\n"
            "    float2 cell = floor(uv * 16.0);\n"
            // A depth-dependent value is compared with a permutation of all
            // 256 thresholds. Uniform depth changes therefore change coverage,
            // unlike a random spatial phase whose coverage stays near one half.
            "    float cell_index = dot(cell, float2(1.0, 16.0));\n"
            "    float threshold = frac((cell_index * options.y + options.z + 0.5) / 256.0);\n"
            "    float signature = frac(depth * options.x);\n"
            "    clip(options.w > 0.5 ? signature - threshold : 1.0);\n"
            "    return float4(1.0, 1.0, 1.0, 1.0);\n"
            "}\n";
        ID3DBlob *bytecode = nullptr;
        ID3DBlob *diagnostics = nullptr;
        const char *profile = caps_.PixelShaderVersion >= D3DPS_VERSION(3, 0) ? "ps_3_0" : "ps_2_0";
        HRESULT result = D3DCompile(source, sizeof(source) - 1, "ENR depth integer probe",
            nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &bytecode, &diagnostics);
        release(diagnostics);
        if (SUCCEEDED(result))
            result = device_->CreatePixelShader(static_cast<const DWORD *>(bytecode->GetBufferPointer()), &probe_shader_);
        release(bytecode);
        if (FAILED(result)) return result;

        result = device_->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8,
            D3DMULTISAMPLE_NONE, 0, FALSE, &probe_target_, nullptr);
        if (FAILED(result)) return result;
        for (auto &query : probe_queries_)
        {
            result = device_->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query);
            if (FAILED(result)) return result;
        }

        D3DDEVICE_CREATION_PARAMETERS parameters = {};
        result = device_->GetCreationParameters(&parameters);
        if (FAILED(result)) return result;
        const vertex probe_vertices[] = {
            { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
            { 15.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
            { -0.5f, 15.5f, 0.0f, 1.0f, 0.0f, 1.0f },
            { 15.5f, 15.5f, 0.0f, 1.0f, 1.0f, 1.0f }
        };
        const DWORD vertex_usage = D3DUSAGE_WRITEONLY |
            ((parameters.BehaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0 ?
                D3DUSAGE_SOFTWAREPROCESSING : 0);
        result = device_->CreateVertexBuffer(sizeof(probe_vertices), vertex_usage,
            D3DFVF_XYZRHW | D3DFVF_TEX1, D3DPOOL_DEFAULT, &probe_vertices_, nullptr);
        if (FAILED(result)) return result;
        void *destination = nullptr;
        result = probe_vertices_->Lock(0, sizeof(probe_vertices), &destination, 0);
        if (SUCCEEDED(result))
        {
            std::memcpy(destination, probe_vertices, sizeof(probe_vertices));
            result = probe_vertices_->Unlock();
        }
        return result;
    }

    depth_probe_result grayscale_d3d9::poll_depth_probe(IDirect3DDevice9 *device,
        IDirect3DTexture9 *depth, std::uint64_t generation, ULONGLONG now)
    {
        depth_probe_result output;
        output.generation = generation;
        const auto unavailable = [&output](HRESULT error)
        {
            output.status = depth_probe_result::state::unavailable;
            output.error = error;
            return output;
        };
        if (device == nullptr || depth == nullptr || device_ != device || state_block_ == nullptr)
            return unavailable(E_INVALIDARG);
        if (probe_disabled_)
            return unavailable(probe_error_);

        if (!probe_attempted_)
        {
            probe_attempted_ = true;
            probe_error_ = initialize_probe();
            if (FAILED(probe_error_))
            {
                probe_disabled_ = true;
                return unavailable(probe_error_);
            }
        }
        if (probe_generation_ != generation)
        {
            // Discard results belonging to an old depth selection. Reissuing a
            // query replaces its previous result without waiting for the GPU.
            probe_pending_ = false;
            // Keep the last issue time: alternating depth selections must not
            // turn an infrequent probe into per-frame GPU work.
            probe_generation_ = generation;
        }
        if (probe_pending_)
        {
            if (now - probe_last_poll_ < 100)
                return output;
            probe_last_poll_ = now;
            DWORD results[3] = {};
            for (unsigned index = 0; index < 3; ++index)
            {
                // Flags zero is deliberately nonblocking. S_FALSE leaves this
                // sample pending until a later frame; there is no spin or flush.
                const HRESULT result = probe_queries_[index]->GetData(&results[index], sizeof(DWORD), 0);
                if (result == S_FALSE) return output;
                if (FAILED(result))
                {
                    probe_error_ = result;
                    probe_disabled_ = true;
                    probe_pending_ = false;
                    return unavailable(result);
                }
            }
            probe_pending_ = false;
            output.status = depth_probe_result::state::ready;
            output.valid = results[0];
            output.signature1 = results[1];
            output.signature2 = results[2];
            output.generation = probe_generation_;
            return output;
        }
        if (probe_sampled_ && now - probe_last_sample_ < 5000)
            return output;

        probe_error_ = draw_pass(probe_target_, depth, probe_shader_, probe_vertices_,
            16, 16, nullptr, 0, probe_queries_);
        if (FAILED(probe_error_))
        {
            probe_disabled_ = true;
            return unavailable(probe_error_);
        }
        probe_pending_ = true;
        probe_sampled_ = true;
        probe_last_sample_ = probe_last_poll_ = now;
        return output;
    }
}
