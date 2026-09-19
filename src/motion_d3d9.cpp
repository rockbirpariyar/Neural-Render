#include "motion_d3d9.hpp"

#include "motion_shaders_bytecode.hpp"
#include <cstring>

namespace
{
    template <typename T> void release(T *&object)
    {
        if (object != nullptr) object->Release();
        object = nullptr;
    }
}

namespace enr
{
    motion_d3d9::~motion_d3d9() { reset(); }

    void motion_d3d9::reset()
    {
        // Never expose a field while its underlying textures are being released.
        valid_ = false;
        for (auto &surface : pyramid_surface_) release(surface);
        for (auto &texture : pyramid_) release(texture);
        release(coarse_surface_);
        release(coarse_);
        release(motion_surface_);
        release(motion_);
        release(downsample_shader_);
        release(coarse_shader_);
        release(refine_shader_);
        release(vertices_);
        device_ = nullptr;
        color_width_ = color_height_ = depth_width_ = depth_height_ = 0;
        width_ = height_ = 0;
        attempted_ = valid_ = false;
        initialization_result_ = S_OK;
    }

    HRESULT motion_d3d9::initialize(IDirect3DDevice9 *device, UINT color_width,
        UINT color_height, UINT depth_width, UINT depth_height)
    {
        D3DCAPS9 caps = {};
        HRESULT result = device->GetDeviceCaps(&caps);
        if (FAILED(result)) return result;
        if (caps.PixelShaderVersion < D3DPS_VERSION(3, 0)) return D3DERR_NOTAVAILABLE;
        width_ = (color_width + 7) / 8;
        height_ = (color_height + 7) / 8;
        if (width_ < 4 || height_ < 4 || depth_width == 0 || depth_height == 0)
            return E_INVALIDARG;
        result = device->CreatePixelShader(motion_bytecode::prefilter, &downsample_shader_);
        if (SUCCEEDED(result)) result = device->CreatePixelShader(motion_bytecode::coarse, &coarse_shader_);
        if (SUCCEEDED(result)) result = device->CreatePixelShader(motion_bytecode::refine, &refine_shader_);
        for (unsigned index = 0; index < 2 && SUCCEEDED(result); ++index)
        {
            result = device->CreateTexture(width_, height_, 1, D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &pyramid_[index], nullptr);
            if (SUCCEEDED(result)) result = pyramid_[index]->GetSurfaceLevel(0, &pyramid_surface_[index]);
        }
        if (SUCCEEDED(result)) result = device->CreateTexture(width_, height_, 1,
            D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &coarse_, nullptr);
        if (SUCCEEDED(result)) result = coarse_->GetSurfaceLevel(0, &coarse_surface_);
        if (SUCCEEDED(result)) result = device->CreateTexture(width_, height_, 1,
            D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &motion_, nullptr);
        if (SUCCEEDED(result)) result = motion_->GetSurfaceLevel(0, &motion_surface_);
        const vertex vertices[] = {
            {-0.5f, -0.5f, 0, 1, 0, 0},
            {static_cast<float>(width_) - 0.5f, -0.5f, 0, 1, 1, 0},
            {-0.5f, static_cast<float>(height_) - 0.5f, 0, 1, 0, 1},
            {static_cast<float>(width_) - 0.5f, static_cast<float>(height_) - 0.5f, 0, 1, 1, 1}
        };
        if (SUCCEEDED(result)) result = device->CreateVertexBuffer(sizeof(vertices),
            D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_TEX1, D3DPOOL_DEFAULT, &vertices_, nullptr);
        void *upload = nullptr;
        if (SUCCEEDED(result)) result = vertices_->Lock(0, sizeof(vertices), &upload, 0);
        if (SUCCEEDED(result))
        {
            std::memcpy(upload, vertices, sizeof(vertices));
            result = vertices_->Unlock();
        }
        return result;
    }

    HRESULT motion_d3d9::process(IDirect3DDevice9 *device,
        IDirect3DTexture9 *current_color, IDirect3DTexture9 *previous_color,
        IDirect3DTexture9 *current_depth, IDirect3DTexture9 *previous_depth,
        UINT color_width, UINT color_height, UINT depth_width, UINT depth_height,
        bool history_valid, bool reversed_depth, motion_draw_callback draw,
        void *context, motion_report &report)
    {
        report = {};
        valid_ = false;
        if (!history_valid) return S_FALSE;
        if (device == nullptr || current_color == nullptr || previous_color == nullptr ||
            current_depth == nullptr || previous_depth == nullptr || draw == nullptr ||
            color_width == 0 || color_height == 0 || depth_width == 0 || depth_height == 0)
            return report.result = E_INVALIDARG;
        const bool changed = device_ != device || color_width_ != color_width ||
            color_height_ != color_height || depth_width_ != depth_width || depth_height_ != depth_height;
        if (changed) reset();
        if (!attempted_)
        {
            device_ = device;
            color_width_ = color_width;
            color_height_ = color_height;
            depth_width_ = depth_width;
            depth_height_ = depth_height;
            attempted_ = true;
            initialization_result_ = initialize(device, color_width, color_height, depth_width, depth_height);
            report.initialized = SUCCEEDED(initialization_result_);
        }
        report.width = width_;
        report.height = height_;
        if (FAILED(initialization_result_)) return report.result = initialization_result_;
        const float constants[] = {
            static_cast<float>(color_width), static_cast<float>(color_height),
            1.0f / color_width, 1.0f / color_height,
            static_cast<float>(width_), static_cast<float>(height_), 1.0f / width_, 1.0f / height_,
            static_cast<float>(depth_width), static_cast<float>(depth_height), reversed_depth ? 1.0f : 0.0f, 0.0f
        };
        IDirect3DTexture9 *inputs[] = { current_color, previous_color, current_depth, previous_depth, coarse_ };
        HRESULT result = draw(context, pyramid_surface_[0], downsample_shader_, vertices_, width_, height_,
            &inputs[0], 1, constants, 3);
        if (SUCCEEDED(result)) result = draw(context, pyramid_surface_[1], downsample_shader_, vertices_,
            width_, height_, &inputs[1], 1, constants, 3);
        if (SUCCEEDED(result)) result = draw(context, coarse_surface_, coarse_shader_, vertices_,
            width_, height_, pyramid_, 2, constants, 3);
        if (SUCCEEDED(result)) result = draw(context, motion_surface_, refine_shader_, vertices_,
            width_, height_, inputs, 5, constants, 3);
        report.result = result;
        report.processed = report.valid = valid_ = SUCCEEDED(result);
        return result;
    }
}
