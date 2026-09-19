#pragma once

#include <windows.h>
#include <d3d9.h>
#include <cstdint>

namespace enr
{
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
        // Call after render/render_depth. Only three asynchronous integer query
        // results reach the CPU; no image is copied, mapped, or retained.
        depth_probe_result poll_depth_probe(IDirect3DDevice9 *device,
            IDirect3DTexture9 *depth, std::uint64_t generation, ULONGLONG now);
        void reset();

    private:
        HRESULT initialize(IDirect3DDevice9 *device, const D3DSURFACE_DESC &description);
        HRESULT render_impl(IDirect3DDevice9 *device, IDirect3DSurface9 *backbuffer,
            IDirect3DTexture9 *depth, bool linearize, bool reversed, bool &initialized_now);
        HRESULT initialize_depth_shader();
        HRESULT initialize_probe();
        HRESULT draw_pass(IDirect3DSurface9 *target, IDirect3DTexture9 *input,
            IDirect3DPixelShader9 *shader, IDirect3DVertexBuffer9 *vertices,
            UINT width, UINT height, const float *constants, DWORD color_mask,
            IDirect3DQuery9 **queries = nullptr);

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
