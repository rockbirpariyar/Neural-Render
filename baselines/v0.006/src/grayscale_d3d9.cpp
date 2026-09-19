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
            for (DWORD index = 0; index < std::size(srgb_textures_); ++index)
            {
                result = device_->GetSamplerState(index, D3DSAMP_SRGBTEXTURE, &srgb_textures_[index]);
                if (FAILED(result)) return result;
            }
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
            // A sampled INTZ input can be the game's original depth target.
            // Unbind our sampler slots before reattaching the saved targets;
            // the full state block restores the game's textures afterward.
            for (DWORD index = 0; index < std::size(srgb_textures_); ++index)
                remember_failure(device_->SetTexture(index, nullptr));
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
            for (DWORD index = 0; index < std::size(srgb_textures_); ++index)
                remember_failure(device_->SetSamplerState(index, D3DSAMP_SRGBTEXTURE, srgb_textures_[index]));
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
        DWORD srgb_textures_[5] = {};
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
        motion_.reset();
        release(motion_display_shader_);
        motion_display_attempted_ = false;
        motion_display_result_ = S_OK;
        release_history();
        release(copy_shader_);
        history_attempted_ = false;
        history_initialization_result_ = S_OK;
        history_depth_description_ = {};
        history_color_description_ = {};
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
        D3DSURFACE_DESC description = {};
        HRESULT result = prepare_renderer(device, backbuffer, description, initialized_now);
        if (FAILED(result)) return result;

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

    HRESULT grayscale_d3d9::prepare_renderer(IDirect3DDevice9 *device,
        IDirect3DSurface9 *backbuffer, D3DSURFACE_DESC &description, bool &initialized_now)
    {
        initialized_now = false;
        if (device == nullptr || backbuffer == nullptr) return E_INVALIDARG;

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
        return S_OK;
    }

    void grayscale_d3d9::invalidate_history()
    {
        motion_.invalidate();
        history_valid_ = false;
        history_source_ = nullptr;
        history_generation_ = history_frame_ = 0;
    }

    void grayscale_d3d9::release_history()
    {
        invalidate_history();
        release(history_depth_vertices_);
        release(history_depth_surface_);
        release(history_depth_);
        release(history_color_surface_);
        release(history_color_);
    }

    HRESULT grayscale_d3d9::initialize_copy_shader()
    {
        constexpr char source[] =
            "sampler2D source_image : register(s0);\n"
            "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
            // Display writes RGB only. Opaque alpha prevents inherited vendor
            // alpha-to-coverage from masking either display or depth-store pixels.
            "    return float4(tex2D(source_image, uv).rgb, 1.0);\n"
            "}\n";
        ID3DBlob *bytecode = nullptr;
        ID3DBlob *diagnostics = nullptr;
        const char *profile = caps_.PixelShaderVersion >= D3DPS_VERSION(3, 0) ? "ps_3_0" : "ps_2_0";
        HRESULT result = D3DCompile(source, sizeof(source) - 1, "ENR GPU history copy",
            nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &bytecode, &diagnostics);
        release(diagnostics);
        if (SUCCEEDED(result))
            result = device_->CreatePixelShader(static_cast<const DWORD *>(bytecode->GetBufferPointer()), &copy_shader_);
        release(bytecode);
        return result;
    }

    HRESULT grayscale_d3d9::initialize_history(const D3DSURFACE_DESC &depth_description)
    {
        release_history();
        HRESULT result = copy_shader_ != nullptr ? S_OK : initialize_copy_shader();
        if (SUCCEEDED(result))
            result = device_->CreateTexture(width_, height_, 1, D3DUSAGE_RENDERTARGET,
                format_, D3DPOOL_DEFAULT, &history_color_, nullptr);
        if (SUCCEEDED(result))
            result = history_color_->GetSurfaceLevel(0, &history_color_surface_);
        // INTZ is sampleable depth but not a color render target. Store its raw
        // red component in a full-precision R32F texture, entirely on the GPU.
        if (SUCCEEDED(result))
            result = device_->CreateTexture(depth_description.Width, depth_description.Height, 1,
                D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &history_depth_, nullptr);
        if (SUCCEEDED(result))
            result = history_depth_->GetSurfaceLevel(0, &history_depth_surface_);

        D3DDEVICE_CREATION_PARAMETERS parameters = {};
        if (SUCCEEDED(result))
            result = device_->GetCreationParameters(&parameters);
        const float right = static_cast<float>(depth_description.Width) - 0.5f;
        const float bottom = static_cast<float>(depth_description.Height) - 0.5f;
        const vertex depth_vertices[] = {
            { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
            { right, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
            { -0.5f, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
            { right, bottom, 0.0f, 1.0f, 1.0f, 1.0f }
        };
        const DWORD usage = D3DUSAGE_WRITEONLY |
            ((parameters.BehaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0 ?
                D3DUSAGE_SOFTWAREPROCESSING : 0);
        if (SUCCEEDED(result))
            result = device_->CreateVertexBuffer(sizeof(depth_vertices), usage,
                D3DFVF_XYZRHW | D3DFVF_TEX1, D3DPOOL_DEFAULT, &history_depth_vertices_, nullptr);
        if (SUCCEEDED(result))
        {
            void *destination = nullptr;
            result = history_depth_vertices_->Lock(0, sizeof(depth_vertices), &destination, 0);
            if (SUCCEEDED(result))
            {
                std::memcpy(destination, depth_vertices, sizeof(depth_vertices));
                result = history_depth_vertices_->Unlock();
            }
        }
        if (FAILED(result)) release_history();
        return result;
    }

    HRESULT grayscale_d3d9::render_temporal(IDirect3DDevice9 *device,
        IDirect3DSurface9 *backbuffer, IDirect3DTexture9 *depth,
        std::uint64_t depth_generation, std::uint64_t frame_index,
        temporal_view view, bool linearize, bool reversed, temporal_report &report)
    {
        report = {};
        const bool had_history = history_attempted_;
        const bool had_valid_history = history_valid_;
        D3DSURFACE_DESC color_description = {};
        HRESULT result = prepare_renderer(device, backbuffer, color_description, report.renderer_initialized);
        const auto invalidate = [this, &report](const char *reason)
        {
            if (history_valid_)
            {
                report.history_reset = true;
                report.reset_reason = reason;
            }
            invalidate_history();
        };
        if (FAILED(result))
        {
            invalidate("renderer unavailable");
            if ((had_valid_history || (had_history && !history_attempted_)) && !report.history_reset)
            {
                report.history_reset = true;
                report.reset_reason = "renderer unavailable";
            }
            return result;
        }
        if (report.renderer_initialized && had_history)
        {
            report.history_reset = true;
            report.reset_reason = "color resolution, format, or device changed";
        }
        report.color_width = width_;
        report.color_height = height_;

        D3DSURFACE_DESC depth_description = {};
        if (depth != nullptr)
        {
            report.history_result = depth->GetLevelDesc(0, &depth_description);
            if (SUCCEEDED(report.history_result) && (depth_description.Width == 0 || depth_description.Height == 0))
                report.history_result = D3DERR_INVALIDCALL;
            if (FAILED(report.history_result))
            {
                invalidate("depth description unavailable");
                depth = nullptr;
            }
        }
        if (depth == nullptr)
            invalidate("depth unavailable");
        else
        {
            report.depth_width = depth_description.Width;
            report.depth_height = depth_description.Height;
            const auto same_configuration = [](const D3DSURFACE_DESC &left, const D3DSURFACE_DESC &right)
            {
                return left.Width == right.Width && left.Height == right.Height && left.Format == right.Format &&
                    left.MultiSampleType == right.MultiSampleType && left.MultiSampleQuality == right.MultiSampleQuality;
            };
            if (history_attempted_ && (!same_configuration(history_depth_description_, depth_description) ||
                !same_configuration(history_color_description_, color_description)))
            {
                report.history_reset = true;
                report.reset_reason = "color or depth resource configuration changed";
                // Input depth formats all store into the same R32F texture, and
                // MSAA color resolves into a non-MSAA history texture. Only a
                // storage size/format change needs new allocations. A changed
                // failed configuration is allowed one fresh allocation attempt.
                const bool storage_compatible = SUCCEEDED(history_initialization_result_) &&
                    history_depth_description_.Width == depth_description.Width &&
                    history_depth_description_.Height == depth_description.Height &&
                    history_color_description_.Width == color_description.Width &&
                    history_color_description_.Height == color_description.Height &&
                    history_color_description_.Format == color_description.Format;
                if (storage_compatible)
                    invalidate_history();
                else
                {
                    release_history();
                    history_attempted_ = false;
                }
                history_color_description_ = color_description;
                history_depth_description_ = depth_description;
            }
            if (!history_attempted_)
            {
                history_attempted_ = true;
                history_color_description_ = color_description;
                history_depth_description_ = depth_description;
                history_initialization_result_ = initialize_history(depth_description);
                report.history_initialized = SUCCEEDED(history_initialization_result_);
            }
            report.history_result = history_initialization_result_;
            if (history_valid_ && (history_generation_ != depth_generation || history_source_ != depth))
                invalidate("depth source changed");
            if (history_valid_ && frame_index != history_frame_ + 1)
                invalidate("presentation discontinuity");
        }
        report.valid_before = history_valid_;

        // Always snapshot the CURRENT final color before rendering any debug
        // view. Feeding displayed history back into history would freeze it.
        result = device->StretchRect(backbuffer, nullptr, scratch_surface_, nullptr, D3DTEXF_NONE);
        if (FAILED(result))
        {
            report.history_result = result;
            invalidate("current color capture failed");
            return result;
        }

        // Motion must compare the untouched current scene against frame N-1.
        // Both debug display and the history replacement happen after these
        // commands; this also prevents a displayed motion/history image from
        // entering the next frame's motion inputs.
        motion_report motion_status;
        report.motion_result = motion_.process(device, scratch_, history_color_,
            depth, history_depth_, width_, height_, depth_description.Width,
            depth_description.Height, history_valid_, reversed,
            &grayscale_d3d9::draw_motion_pass, this, motion_status);
        report.motion_resources_initialized = motion_status.initialized;
        report.motion_processed = motion_status.processed;
        report.motion_width = motion_status.width;
        report.motion_height = motion_status.height;

        IDirect3DTexture9 *display_input = scratch_;
        IDirect3DPixelShader9 *display_shader = shader_;
        const float depth_options[] = { linearize ? 1.0f : 0.0f, reversed ? 1.0f : 0.0f, 0.0f, 0.0f };
        const float motion_options[] = { motion_.valid() ? 1.0f : 0.0f, 1.0f / 8.0f, 0.0f, 0.0f };
        const float *display_constants = nullptr;
        if (view == temporal_view::motion)
        {
            if (!motion_display_attempted_)
            {
                motion_display_attempted_ = true;
                motion_display_result_ = initialize_motion_display_shader();
            }
            if (SUCCEEDED(motion_display_result_))
            {
                // A first frame, discontinuity, or failed estimate is dark,
                // never a stale motion field from before the invalidation.
                display_input = motion_.valid() ? motion_.texture() : scratch_;
                display_shader = motion_display_shader_;
                display_constants = motion_options;
            }
            else if (SUCCEEDED(report.motion_result))
                report.motion_result = motion_display_result_;
        }
        else if (view == temporal_view::previous_color && history_valid_)
        {
            display_input = history_color_;
            display_shader = copy_shader_;
            report.displayed_history = true;
        }
        else if ((view == temporal_view::current_depth || view == temporal_view::previous_depth) && depth != nullptr)
        {
            if (!depth_shader_attempted_)
            {
                depth_shader_attempted_ = true;
                depth_shader_result_ = initialize_depth_shader();
            }
            if (FAILED(depth_shader_result_))
            {
                invalidate("depth visualization unavailable");
                return depth_shader_result_;
            }
            const bool use_previous = view == temporal_view::previous_depth && history_valid_;
            display_input = use_previous ? history_depth_ : depth;
            display_shader = depth_shader_;
            display_constants = depth_options;
            report.displayed_history = use_previous;
        }
        result = draw_pass(backbuffer, display_input, display_shader, vertices_, width_, height_,
            display_constants, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        if (FAILED(result))
        {
            invalidate("frame display failed");
            return result;
        }

        if (depth != nullptr && SUCCEEDED(report.history_result))
        {
            // GPU command order guarantees the previous-frame draw above reads
            // history before these writes replace it. No fence, flush or wait.
            report.history_result = device->StretchRect(scratch_surface_, nullptr,
                history_color_surface_, nullptr, D3DTEXF_NONE);
            if (SUCCEEDED(report.history_result))
                report.history_result = draw_pass(history_depth_surface_, depth, copy_shader_, history_depth_vertices_,
                    depth_description.Width, depth_description.Height, nullptr, D3DCOLORWRITEENABLE_RED);
            if (SUCCEEDED(report.history_result))
            {
                report.became_valid = !history_valid_;
                history_valid_ = true;
                history_source_ = depth;
                history_generation_ = depth_generation;
                history_frame_ = frame_index;
            }
            else
                invalidate("GPU history store failed");
        }
        report.valid_after = history_valid_;
        return result;
    }

    HRESULT grayscale_d3d9::draw_pass(IDirect3DSurface9 *target, IDirect3DTexture9 *input,
        IDirect3DPixelShader9 *shader, IDirect3DVertexBuffer9 *vertices,
        UINT width, UINT height, const float *constants, DWORD color_mask,
        IDirect3DQuery9 **queries)
    {
        return draw_pass_inputs(target, shader, vertices, width, height, &input, 1,
            constants, constants != nullptr ? 1 : 0, color_mask, queries);
    }

    HRESULT grayscale_d3d9::draw_motion_pass(void *context, IDirect3DSurface9 *target,
        IDirect3DPixelShader9 *shader, IDirect3DVertexBuffer9 *vertices,
        UINT width, UINT height, IDirect3DTexture9 *const *inputs, UINT input_count,
        const float *constants, UINT constant_count)
    {
        return static_cast<grayscale_d3d9 *>(context)->draw_pass_inputs(target, shader,
            vertices, width, height, inputs, input_count, constants, constant_count,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
            D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    }

    HRESULT grayscale_d3d9::draw_pass_inputs(IDirect3DSurface9 *target,
        IDirect3DPixelShader9 *shader, IDirect3DVertexBuffer9 *vertices,
        UINT width, UINT height, IDirect3DTexture9 *const *inputs, UINT input_count,
        const float *constants, UINT constant_count, DWORD color_mask,
        IDirect3DQuery9 **queries)
    {
        if (inputs == nullptr || input_count == 0 || input_count > 5 ||
            (constants != nullptr && (constant_count == 0 || constant_count > 224)))
            return E_INVALIDARG;
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
            for (UINT index = 0; index < input_count; ++index)
                for (const auto &state : sampler_states)
                    ENR_CHECK_STATE(device->SetSamplerState(index, state.state, state.value));
            ENR_CHECK_STATE(device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE));
            ENR_CHECK_STATE(device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0));
            ENR_CHECK_STATE(device->SetVertexShader(nullptr));
            ENR_CHECK_STATE(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
            ENR_CHECK_STATE(device->SetStreamSource(0, vertices, 0, sizeof(vertex)));
            ENR_CHECK_STATE(device->SetPixelShader(shader));
            for (UINT index = 0; index < input_count; ++index)
                ENR_CHECK_STATE(device->SetTexture(index, inputs[index]));
            if (constants != nullptr)
                ENR_CHECK_STATE(device->SetPixelShaderConstantF(0, constants, constant_count));

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

    HRESULT grayscale_d3d9::initialize_motion_display_shader()
    {
        constexpr char source[] =
            "sampler2D motion_field : register(s0);\n"
            "float4 options : register(c0);\n"
            "float4 main(float2 uv : TEXCOORD0) : COLOR0 {\n"
            "    if (options.x < 0.5) return float4(0.0, 0.0, 0.0, 1.0);\n"
            "    float4 field = tex2D(motion_field, uv);\n"
            "    float2 v = field.rg * options.y;\n"
            // Positive/negative horizontal correspondence uses red/cyan;
            // positive/negative vertical correspondence uses green/magenta.
            // Zero vectors and rejected history remain black. RGB-only output
            // preserves the backbuffer's original alpha just like grayscale.
            "    float3 color = float3(max(v.x, 0.0) + max(-v.y, 0.0),\n"
            "        max(-v.x, 0.0) + max(v.y, 0.0),\n"
            "        max(-v.x, 0.0) + max(-v.y, 0.0));\n"
            "    return float4(saturate(color) * sqrt(saturate(field.b)) * saturate(field.a), 1.0);\n"
            "}\n";
        ID3DBlob *bytecode = nullptr;
        ID3DBlob *diagnostics = nullptr;
        const char *profile = caps_.PixelShaderVersion >= D3DPS_VERSION(3, 0) ? "ps_3_0" : "ps_2_0";
        HRESULT result = D3DCompile(source, sizeof(source) - 1, "ENR motion visualization",
            nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &bytecode, &diagnostics);
        release(diagnostics);
        if (SUCCEEDED(result))
            result = device_->CreatePixelShader(static_cast<const DWORD *>(bytecode->GetBufferPointer()),
                &motion_display_shader_);
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
