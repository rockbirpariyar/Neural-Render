#pragma once

#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include "motion_d3d9.hpp"

namespace enr
{
    enum class temporal_view { current_grayscale, current_depth, previous_color, previous_depth, motion };

    struct temporal_report
    {
        bool renderer_initialized = false;
        bool history_initialized = false;
        bool history_reset = false;
        bool became_valid = false;
        bool valid_before = false;
        bool valid_after = false;
        bool displayed_history = false;
        const char *reset_reason = nullptr;
        HRESULT history_result = S_OK;
        bool motion_resources_initialized = false;
        bool motion_processed = false;
        HRESULT motion_result = S_OK;
        UINT motion_width = 0, motion_height = 0;
        UINT color_width = 0, color_height = 0;
        UINT depth_width = 0, depth_height = 0;
    };

    struct depth_probe_result
    {
        enum class state { none, ready, unavailable } status = state::none;
        unsigned valid = 0;
        unsigned signature1 = 0;
        unsigned signature2 = 0;
        HRESULT error = S_OK;
        std::uint64_t generation = 0;
    };

    // One same-frame scratch image and reusable shader, geometry and state block.
    // Reset releases the state block's captured game references before device reset.
    class grayscale_d3d9 final
    {
    public:
        grayscale_d3d9() = default;
        ~grayscale_d3d9();
        grayscale_d3d9(const grayscale_d3d9 &) = delete;
        grayscale_d3d9 &operator=(const grayscale_d3d9 &) = delete;

        HRESULT render(IDirect3DDevice9 *device, IDirect3DSurface9 *backbuffer,
            bool &initialized_now);
        HRESULT render_depth(IDirect3DDevice9 *device, IDirect3DSurface9 *backbuffer,
            IDirect3DTexture9 *depth, bool linearize, bool reversed, bool &initialized_now);
        HRESULT render_temporal(IDirect3DDevice9 *device, IDirect3DSurface9 *backbuffer,
            IDirect3DTexture9 *depth, std::uint64_t depth_generation, std::uint64_t frame_index,
            temporal_view view, bool linearize, bool reversed, temporal_report &report);
        bool history_valid() const { return history_valid_; }
        bool history_initialized() const { return history_color_ != nullptr && history_depth_ != nullptr; }
        IDirect3DTexture9 *motion_texture() const { return motion_.texture(); }
        bool motion_valid() const { return motion_.valid(); }
        // Borrowed GPU resources. After render_temporal these hold the current
        // frame, ready to be consumed as previous-frame inputs on the next call.
        IDirect3DTexture9 *previous_color() const { return history_color_; }
        IDirect3DTexture9 *previous_depth() const { return history_depth_; }
        // Effect reloads/source interruptions invalidate without reallocating.
        void invalidate_history();
        // Call after render/render_depth. Only three asynchronous integer query
        // results reach the CPU; no image is copied, mapped, or retained.
        depth_probe_result poll_depth_probe(IDirect3DDevice9 *device,
            IDirect3DTexture9 *depth, std::uint64_t generation, ULONGLONG now);
        void reset();

    private:
        HRESULT initialize(IDirect3DDevice9 *device, const D3DSURFACE_DESC &description);
        HRESULT prepare_renderer(IDirect3DDevice9 *device, IDirect3DSurface9 *backbuffer,
            D3DSURFACE_DESC &description, bool &initialized_now);
        HRESULT initialize_history(const D3DSURFACE_DESC &depth_description);
        HRESULT initialize_copy_shader();
        void release_history();
        HRESULT render_impl(IDirect3DDevice9 *device, IDirect3DSurface9 *backbuffer,
            IDirect3DTexture9 *depth, bool linearize, bool reversed, bool &initialized_now);
        HRESULT initialize_depth_shader();
        HRESULT initialize_motion_display_shader();
        HRESULT initialize_probe();
        HRESULT draw_pass(IDirect3DSurface9 *target, IDirect3DTexture9 *input,
            IDirect3DPixelShader9 *shader, IDirect3DVertexBuffer9 *vertices,
            UINT width, UINT height, const float *constants, DWORD color_mask,
            IDirect3DQuery9 **queries = nullptr);
        HRESULT draw_pass_inputs(IDirect3DSurface9 *target, IDirect3DPixelShader9 *shader,
            IDirect3DVertexBuffer9 *vertices, UINT width, UINT height,
            IDirect3DTexture9 *const *inputs, UINT input_count,
            const float *constants, UINT constant_count, DWORD color_mask,
            IDirect3DQuery9 **queries = nullptr);
        static HRESULT draw_motion_pass(void *context, IDirect3DSurface9 *target,
            IDirect3DPixelShader9 *shader, IDirect3DVertexBuffer9 *vertices,
            UINT width, UINT height, IDirect3DTexture9 *const *inputs, UINT input_count,
            const float *constants, UINT constant_count);

        struct vertex { float x, y, z, rhw, u, v; };

        IDirect3DDevice9 *device_ = nullptr; // Identity only; never an owned reference.
        IDirect3DTexture9 *scratch_ = nullptr;
        IDirect3DSurface9 *scratch_surface_ = nullptr;
        IDirect3DPixelShader9 *shader_ = nullptr;
        IDirect3DPixelShader9 *depth_shader_ = nullptr;
        IDirect3DVertexBuffer9 *vertices_ = nullptr;
        IDirect3DStateBlock9 *state_block_ = nullptr;
        D3DCAPS9 caps_ = {};
        bool mixed_vertex_processing_ = false;
        IDirect3DDevice9 *attempted_device_ = nullptr;
        D3DSURFACE_DESC attempted_description_ = {};
        HRESULT initialization_result_ = E_FAIL;
        UINT width_ = 0;
        UINT height_ = 0;
        D3DFORMAT format_ = D3DFMT_UNKNOWN;
        bool depth_shader_attempted_ = false;
        HRESULT depth_shader_result_ = S_OK;
        motion_d3d9 motion_;
        IDirect3DPixelShader9 *motion_display_shader_ = nullptr;
        bool motion_display_attempted_ = false;
        HRESULT motion_display_result_ = S_OK;

        IDirect3DTexture9 *history_color_ = nullptr;
        IDirect3DSurface9 *history_color_surface_ = nullptr;
        IDirect3DTexture9 *history_depth_ = nullptr;
        IDirect3DSurface9 *history_depth_surface_ = nullptr;
        IDirect3DVertexBuffer9 *history_depth_vertices_ = nullptr;
        IDirect3DPixelShader9 *copy_shader_ = nullptr;
        D3DSURFACE_DESC history_depth_description_ = {};
        D3DSURFACE_DESC history_color_description_ = {};
        bool history_attempted_ = false;
        bool history_valid_ = false;
        HRESULT history_initialization_result_ = S_OK;
        IDirect3DTexture9 *history_source_ = nullptr; // Identity only, not retained.
        std::uint64_t history_generation_ = 0;
        std::uint64_t history_frame_ = 0;

        IDirect3DSurface9 *probe_target_ = nullptr;
        IDirect3DPixelShader9 *probe_shader_ = nullptr;
        IDirect3DVertexBuffer9 *probe_vertices_ = nullptr;
        IDirect3DQuery9 *probe_queries_[3] = {};
        bool probe_attempted_ = false;
        bool probe_disabled_ = false;
        bool probe_pending_ = false;
        bool probe_sampled_ = false;
        HRESULT probe_error_ = S_OK;
        ULONGLONG probe_last_sample_ = 0;
        ULONGLONG probe_last_poll_ = 0;
        std::uint64_t probe_generation_ = 0;
    };
}
