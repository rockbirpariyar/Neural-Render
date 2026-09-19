#pragma once

#include <windows.h>
#include <d3d9.h>
#include <reshade.hpp>
#include <cstdint>

namespace enr
{
    struct depth_source
    {
        // Borrowed from depth_binding; valid until its next acquire/reset call.
        IDirect3DTexture9 *texture = nullptr;
        UINT width = 0;
        UINT height = 0;
        D3DFORMAT format = D3DFMT_UNKNOWN;
        std::uint64_t generation = 0;
    };

    // Reads the selected DEPTH semantic through the official effect-runtime API.
    // It does not select depth attachments or inspect ReShade private data.
    class depth_binding final
    {
    public:
        depth_binding() = default;
        ~depth_binding();
        depth_binding(const depth_binding &) = delete;
        depth_binding &operator=(const depth_binding &) = delete;

        HRESULT acquire(reshade::api::effect_runtime *runtime,
            bool effects_processed_this_frame, depth_source &out);
        void reset();

    private:
        void release_source();

        reshade::api::effect_runtime *runtime_ = nullptr;
        reshade::api::effect_texture_variable variable_ = {};
        reshade::api::resource_view view_ = {};
        bool searched_ = false;
        IDirect3DTexture9 *texture_ = nullptr;
        D3DSURFACE_DESC description_ = {};
        HRESULT acquisition_result_ = S_FALSE;
        std::uint64_t generation_ = 0;
    };

    // Startup-only companion installation. Never overwrites an existing file.
    HRESULT ensure_depth_effect(HMODULE addon_module);
}
