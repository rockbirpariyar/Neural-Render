#pragma once

#include <reshade.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace smoke_test
{
    using namespace reshade::api;

    [[noreturn]] inline void unexpected_call(const char *method)
    {
        std::fprintf(stderr, "FAIL: unexpected SDK call: %s\n", method);
        std::abort();
    }

    // Real SDK subclasses supply scalar virtual calls and reject operations
    // beyond the small read-only metadata surface immediately.
#define ENR_UNEXPECTED(result, method, parameters, qualifiers) \
    result method parameters qualifiers override { unexpected_call(#method); }

    class mock_device final : public device
    {
    public:
        resource_desc description { resource_type::surface, 1920, 1080, 1, 1,
            format::b8g8r8x8_unorm, 1, memory_heap::default_,
            resource_usage::render_target | resource_usage::copy_source };
        mutable unsigned description_reads = 0;
        mutable unsigned api_reads = 0;
        bool forbid_reads = false;
        static constexpr uint64_t current_handle = 0xa2;

        device_api get_api() const override
        {
            if (forbid_reads) unexpected_call("get_api after detection");
            ++api_reads;
            return device_api::d3d9;
        }
        resource_desc get_resource_desc(resource value) const override
        {
            if (forbid_reads) unexpected_call("get_resource_desc after detection");
            if (value.handle != current_handle) unexpected_call("wrong backbuffer resource");
            ++description_reads;
            return description;
        }

        ENR_UNEXPECTED(uint64_t, get_native, (), const)
        ENR_UNEXPECTED(void, get_private_data, (const uint8_t[16], uint64_t *), const)
        ENR_UNEXPECTED(void, set_private_data, (const uint8_t[16], uint64_t), )
        ENR_UNEXPECTED(bool, check_capability, (device_caps), const)
        ENR_UNEXPECTED(bool, check_format_support, (format, resource_usage), const)
        ENR_UNEXPECTED(bool, create_sampler, (const sampler_desc &, sampler *), )
        ENR_UNEXPECTED(void, destroy_sampler, (sampler), )
        ENR_UNEXPECTED(bool, create_resource, (const resource_desc &, const subresource_data *, resource_usage, resource *, void **), )
        ENR_UNEXPECTED(void, destroy_resource, (resource), )
        ENR_UNEXPECTED(bool, create_resource_view, (resource, resource_usage, const resource_view_desc &, resource_view *), )
        ENR_UNEXPECTED(void, destroy_resource_view, (resource_view), )
        ENR_UNEXPECTED(resource, get_resource_from_view, (resource_view), const)
        ENR_UNEXPECTED(resource_view_desc, get_resource_view_desc, (resource_view), const)
        ENR_UNEXPECTED(bool, map_buffer_region, (resource, uint64_t, uint64_t, map_access, void **), )
        ENR_UNEXPECTED(void, unmap_buffer_region, (resource), )
        ENR_UNEXPECTED(bool, map_texture_region, (resource, uint32_t, const subresource_box *, map_access, subresource_data *), )
        ENR_UNEXPECTED(void, unmap_texture_region, (resource, uint32_t), )
        ENR_UNEXPECTED(void, update_buffer_region, (const void *, resource, uint64_t, uint64_t), )
        ENR_UNEXPECTED(void, update_texture_region, (const subresource_data &, resource, uint32_t, const subresource_box *), )
        ENR_UNEXPECTED(bool, create_pipeline, (pipeline_layout, uint32_t, const pipeline_subobject *, pipeline *), )
        ENR_UNEXPECTED(void, destroy_pipeline, (pipeline), )
        ENR_UNEXPECTED(bool, create_pipeline_layout, (uint32_t, const pipeline_layout_param *, pipeline_layout *), )
        ENR_UNEXPECTED(void, destroy_pipeline_layout, (pipeline_layout), )
        ENR_UNEXPECTED(bool, allocate_descriptor_tables, (uint32_t, pipeline_layout, uint32_t, descriptor_table *), )
        ENR_UNEXPECTED(void, free_descriptor_tables, (uint32_t, const descriptor_table *), )
        ENR_UNEXPECTED(void, get_descriptor_heap_offset, (descriptor_table, uint32_t, uint32_t, descriptor_heap *, uint32_t *), const)
        ENR_UNEXPECTED(void, copy_descriptor_tables, (uint32_t, const descriptor_table_copy *), )
        ENR_UNEXPECTED(void, update_descriptor_tables, (uint32_t, const descriptor_table_update *), )
        ENR_UNEXPECTED(bool, create_query_heap, (query_type, uint32_t, query_heap *), )
        ENR_UNEXPECTED(void, destroy_query_heap, (query_heap), )
        ENR_UNEXPECTED(bool, get_query_heap_results, (query_heap, query_type, uint32_t, uint32_t, void *, uint32_t), )
        ENR_UNEXPECTED(void, set_resource_name, (resource, const char *), )
        ENR_UNEXPECTED(void, set_resource_view_name, (resource_view, const char *), )
        ENR_UNEXPECTED(bool, create_fence, (uint64_t, fence_flags, fence *, void **), )
        ENR_UNEXPECTED(void, destroy_fence, (fence), )
        ENR_UNEXPECTED(uint64_t, get_completed_fence_value, (fence), const)
        ENR_UNEXPECTED(bool, wait, (fence, uint64_t, uint64_t), )
        ENR_UNEXPECTED(bool, signal, (fence, uint64_t), )
        ENR_UNEXPECTED(bool, get_property, (device_properties, void *), const)
        ENR_UNEXPECTED(uint64_t, get_resource_view_gpu_address, (resource_view), const)
        ENR_UNEXPECTED(void, get_acceleration_structure_size, (acceleration_structure_type, acceleration_structure_build_flags, uint32_t, const acceleration_structure_build_input *, uint64_t *, uint64_t *, uint64_t *), const)
        ENR_UNEXPECTED(bool, get_pipeline_shader_group_handles, (pipeline, uint32_t, uint32_t, void *), )
    };

    class mock_swapchain final : public swapchain
    {
    public:
        device *attached_device = nullptr;
        uint32_t buffer_count = 2;
        uint32_t current_index = 1;
        resource current_resource { mock_device::current_handle };
        mutable unsigned metadata_reads = 0;
        bool forbid_reads = false;

        void record_read() const
        {
            if (forbid_reads) unexpected_call("swapchain metadata after detection");
            ++metadata_reads;
        }
        device *get_device() override
        {
            record_read();
            return attached_device;
        }
        resource get_back_buffer(uint32_t index) override
        {
            record_read();
            if (index != current_index || index != 1 || index >= buffer_count)
                unexpected_call("get_back_buffer did not use valid current index");
            return current_resource;
        }
        uint32_t get_back_buffer_count() const override
        {
            record_read();
            return buffer_count;
        }
        uint32_t get_current_back_buffer_index() const override
        {
            record_read();
            return current_index;
        }

        ENR_UNEXPECTED(uint64_t, get_native, (), const)
        ENR_UNEXPECTED(void, get_private_data, (const uint8_t[16], uint64_t *), const)
        ENR_UNEXPECTED(void, set_private_data, (const uint8_t[16], uint64_t), )
        ENR_UNEXPECTED(void *, get_hwnd, (), const)
        ENR_UNEXPECTED(bool, check_color_space_support, (color_space), const)
        ENR_UNEXPECTED(color_space, get_color_space, (), const)
    };

#undef ENR_UNEXPECTED

    // ReShade's Windows DLL uses MSVC's x86 ABI. GCC's aggregate-return ABI
    // differs, so a plain GCC-built mock would hide the interoperability bug.
    // Keep genuine SDK subclass vtables for scalar methods, replacing only the
    // two aggregate-return slots with explicit MSVC-style thiscall functions:
    // ECX = this, first stack argument = caller-owned result buffer.
    class msvc_aggregate_abi
    {
    public:
#if defined(__GNUC__) && defined(__i386__)
        msvc_aggregate_abi(mock_device &device, mock_swapchain &swapchain)
            : device_(device), swapchain_(swapchain)
        {
            static_assert(RESHADE_API_VERSION == 20, "Review SDK vtable slots after an API upgrade.");
            std::memcpy(&original_device_table_, &device_, sizeof(original_device_table_));
            std::memcpy(&original_swapchain_table_, &swapchain_, sizeof(original_swapchain_table_));
            std::memcpy(device_table_.data(), original_device_table_, sizeof(device_table_));
            std::memcpy(swapchain_table_.data(), original_swapchain_table_, sizeof(swapchain_table_));
            device_table_[10] = reinterpret_cast<void *>(&get_resource_desc_msvc);
            swapchain_table_[5] = reinterpret_cast<void *>(&get_back_buffer_msvc);
            void **table = device_table_.data();
            std::memcpy(static_cast<void *>(&device_), &table, sizeof(table));
            table = swapchain_table_.data();
            std::memcpy(static_cast<void *>(&swapchain_), &table, sizeof(table));
        }
        ~msvc_aggregate_abi()
        {
            std::memcpy(static_cast<void *>(&device_), &original_device_table_, sizeof(original_device_table_));
            std::memcpy(static_cast<void *>(&swapchain_), &original_swapchain_table_, sizeof(original_swapchain_table_));
        }
        msvc_aggregate_abi(const msvc_aggregate_abi &) = delete;
        msvc_aggregate_abi &operator=(const msvc_aggregate_abi &) = delete;

    private:
        static resource_desc *__attribute__((thiscall)) get_resource_desc_msvc(
            const device *object, resource_desc *output, resource value)
        {
            if (output == nullptr) unexpected_call("missing MSVC resource_desc return buffer");
            *output = static_cast<const mock_device *>(object)->mock_device::get_resource_desc(value);
            return output;
        }
        static resource *__attribute__((thiscall)) get_back_buffer_msvc(
            swapchain *object, resource *output, uint32_t index)
        {
            if (output == nullptr) unexpected_call("missing MSVC resource return buffer");
            *output = static_cast<mock_swapchain *>(object)->mock_swapchain::get_back_buffer(index);
            return output;
        }
        mock_device &device_;
        mock_swapchain &swapchain_;
        std::array<void *, 44> device_table_ {};
        std::array<void *, 10> swapchain_table_ {};
        void **original_device_table_ = nullptr;
        void **original_swapchain_table_ = nullptr;
#else
        msvc_aggregate_abi(mock_device &, mock_swapchain &) {}
#endif
    };
}
