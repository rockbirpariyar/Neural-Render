#pragma once

#include <windows.h>
#include <reshade.hpp>
#include <cstddef>
#include <cstring>
#include <type_traits>

// Official ReShade is built with MSVC. On x86, its struct-return methods keep
// 'this' in ECX and put an output pointer on the stack. MinGW GCC's implicit
// struct returns use a different ABI, so only these two calls need a bridge.
// See clang/test/CodeGenCXX/thiscall-struct-return.cpp in llvm/llvm-project.
#if !defined(__MINGW32__) || !defined(__i386__)
#error This bridge is for the MINGW32 build of ENR only.
#endif

namespace enr::sdk
{
    static_assert(RESHADE_API_VERSION == 20, "Review the ReShade vtable layout before updating the SDK.");
    static_assert(sizeof(void *) == 4 && sizeof(reshade::api::resource) == 8);
    static_assert(sizeof(reshade::api::resource_desc) == 48);
    static_assert(offsetof(reshade::api::resource_desc, texture) == 8);
    static_assert(offsetof(reshade::api::resource_desc, heap) == 32);
    static_assert(offsetof(reshade::api::resource_desc, usage) == 36);
    static_assert(offsetof(reshade::api::resource_desc, flags) == 40);

    template <typename Function, typename Object>
    Function virtual_function(Object *object, std::size_t slot)
    {
        // Read the ABI representation without aliasing an interface object.
        const void *const *table = nullptr;
        std::memcpy(&table, object, sizeof(table));
        Function function;
        static_assert(sizeof(function) == sizeof(*table));
        std::memcpy(&function, table + slot, sizeof(function));
        return function;
    }

    // Keep the explicit ABI type non-dependent so GCC's intentional thiscall
    // extension is processed once rather than warned on template instantiation.
    __extension__ typedef reshade::api::resource *(__attribute__((thiscall)) *back_buffer_function)(
        void *, reshade::api::resource *, std::uint32_t);

    template <typename Swapchain>
    inline reshade::api::resource get_back_buffer(Swapchain *swapchain, std::uint32_t index)
    {
        static_assert(std::is_same_v<Swapchain, reshade::api::swapchain>
            || std::is_same_v<Swapchain, reshade::api::effect_runtime>);
        // SDK 20: both interfaces have the same first 8 virtual methods.
        // api_object has 3 slots, get_device is 3, get_hwnd is 4.
        reshade::api::resource result = {};
        virtual_function<back_buffer_function>(swapchain, 5)(swapchain, &result, index);
        return result;
    }

    inline reshade::api::resource_desc get_resource_desc(const reshade::api::device *device, reshade::api::resource resource)
    {
        // SDK 20: get_resource_desc follows destroy_resource at device slot 10.
        __extension__ typedef reshade::api::resource_desc *(__attribute__((thiscall)) *function)(
            const reshade::api::device *, reshade::api::resource_desc *, reshade::api::resource);
        reshade::api::resource_desc result;
        virtual_function<function>(device, 10)(device, &result, resource);
        return result;
    }
}
