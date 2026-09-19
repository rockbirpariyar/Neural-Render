#pragma once

#include <windows.h>
#include <d3d9.h>

namespace enr
{
    struct motion_report
    {
        bool initialized = false;
        bool processed = false;
        bool valid = false;
        HRESULT result = S_OK;
        UINT width = 0, height = 0;
    };

    // The caller supplies its existing state-preserving fullscreen draw path.
    // Inputs use point/clamp sampling with sRGB disabled. Constants are float4s.
    using motion_draw_callback = HRESULT (*)(void *context,
        IDirect3DSurface9 *target, IDirect3DPixelShader9 *shader,
        IDirect3DVertexBuffer9 *vertices, UINT width, UINT height,
        IDirect3DTexture9 *const *inputs, UINT input_count,
        const float *constants, UINT constant_count);

    class motion_d3d9 final
    {
    public:
        motion_d3d9() = default;
        ~motion_d3d9();
        motion_d3d9(const motion_d3d9 &) = delete;
        motion_d3d9 &operator=(const motion_d3d9 &) = delete;

        HRESULT process(IDirect3DDevice9 *device,
            IDirect3DTexture9 *current_color, IDirect3DTexture9 *previous_color,
            IDirect3DTexture9 *current_depth, IDirect3DTexture9 *previous_depth,
            UINT color_width, UINT color_height, UINT depth_width, UINT depth_height,
            bool history_valid, bool reversed_depth,
            motion_draw_callback draw, void *context, motion_report &report);

        // Borrowed RGBA16F texture at ceil(color dimensions / 8). RG stores
        // current-to-previous displacement in full-resolution color pixels:
        // previous_uv = current_uv + RG / color_dimensions.
        // B is confidence [0,1] (0 means invalid), A is always opaque to avoid
        // inherited vendor alpha-to-coverage state. A right-moving image has
        // negative horizontal correspondence. Invalid texels have RGB = 0.
        IDirect3DTexture9 *texture() const { return valid_ ? motion_ : nullptr; }
        bool valid() const { return valid_; }
        UINT width() const { return width_; }
        UINT height() const { return height_; }
        void invalidate() { valid_ = false; }
        void reset();

    private:
        struct vertex { float x, y, z, rhw, u, v; };
        HRESULT initialize(IDirect3DDevice9 *device, UINT color_width, UINT color_height,
            UINT depth_width, UINT depth_height);
        IDirect3DDevice9 *device_ = nullptr; // Borrowed identity.
        IDirect3DTexture9 *pyramid_[2] = {};
        IDirect3DSurface9 *pyramid_surface_[2] = {};
        IDirect3DTexture9 *coarse_ = nullptr;
        IDirect3DSurface9 *coarse_surface_ = nullptr;
        IDirect3DTexture9 *motion_ = nullptr;
        IDirect3DSurface9 *motion_surface_ = nullptr;
        IDirect3DPixelShader9 *downsample_shader_ = nullptr;
        IDirect3DPixelShader9 *coarse_shader_ = nullptr;
        IDirect3DPixelShader9 *refine_shader_ = nullptr;
        IDirect3DVertexBuffer9 *vertices_ = nullptr;
        UINT color_width_ = 0, color_height_ = 0;
        UINT depth_width_ = 0, depth_height_ = 0;
        UINT width_ = 0, height_ = 0;
        bool attempted_ = false;
        bool valid_ = false;
        HRESULT initialization_result_ = S_OK;
    };
}
