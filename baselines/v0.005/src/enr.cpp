// Windows headers must precede ReShade for MinGW's __uuidof declarations.
#include <windows.h>
#include <reshade.hpp>
#include "grayscale_d3d9.hpp"
#include "depth_binding.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <new>

static_assert(sizeof(void *) == 4, "ENR v0.005 is a 32-bit add-on.");

namespace
{
    SRWLOCK state_lock = SRWLOCK_INIT;
    HANDLE log_file = INVALID_HANDLE_VALUE;
    std::uint64_t present_count = 0, reshade_present_count = 0;
    std::uint64_t grayscale_draw_count = 0, failure_count = 0, last_logged_present = 0;
    std::uint64_t history_update_count = 0, history_failure_count = 0;
    std::uint64_t history_last_updated_frame = 0;
    bool initialized = false;
    enr::grayscale_d3d9 *grayscale = nullptr;
    enr::depth_binding *depth_binding = nullptr;

    bool depth_debug = false, depth_linearize = true, depth_reversed = false;
    unsigned history_debug = 0; // 0 = current, 1 = previous color, 2 = previous depth.
    bool history_error_logged = false;
    reshade::api::effect_runtime *history_runtime = nullptr;
    IDirect3DSwapChain9 *history_swapchain = nullptr; // Identity only.
    ULONGLONG depth_unavailable_since = 0;
    reshade::api::effect_runtime *effects_runtime = nullptr;
    std::uint64_t effects_frame = UINT64_MAX;
    bool depth_detected_logged = false, no_depth_logged = false, probe_failure_logged = false;
    bool depth_changed_logged = false, depth_unchanged_logged = false, empty_depth_logged = false;
    UINT logged_depth_width = 0, logged_depth_height = 0;
    D3DFORMAT logged_depth_format = D3DFMT_UNKNOWN;
    bool have_depth_sample = false;
    std::uint64_t sampled_generation = 0;
    unsigned previous_valid = 0, previous_signature1 = 0, previous_signature2 = 0;

    void invalidate_history(const char *reason);

    void write_line(const char *message)
    {
        DWORD written = 0;
        WriteFile(log_file, message, static_cast<DWORD>(std::strlen(message)), &written, nullptr);
        WriteFile(log_file, "\r\n", 2, &written, nullptr);
    }

    template <typename... Args>
    void write_formatted(const char *format, Args... args)
    {
        char message[256];
        const int length = std::snprintf(message, sizeof(message), format, args...);
        if (length >= 0 && static_cast<std::size_t>(length) < sizeof(message))
            write_line(message);
    }

    bool open_log_and_config(HMODULE addon_module)
    {
        wchar_t path[32768];
        const DWORD length = GetModuleFileNameW(addon_module, path, 32768);
        if (length == 0 || length >= 32768) return false;
        wchar_t *const filename = std::wcsrchr(path, L'\\');
        if (filename == nullptr || (filename - path) + 1 + 8 > 32768) return false;
        std::wcscpy(filename + 1, L"enr.ini");
        depth_debug = GetPrivateProfileIntW(L"ENR", L"DepthDebug", 0, path) != 0;
        depth_linearize = GetPrivateProfileIntW(L"ENR", L"DepthLinearize", 1, path) != 0;
        depth_reversed = GetPrivateProfileIntW(L"ENR", L"DepthReversed", 0, path) != 0;
        history_debug = GetPrivateProfileIntW(L"ENR", L"HistoryDebug", 0, path);
        if (history_debug > 2) history_debug = 0;
        std::wcscpy(filename + 1, L"enr.log");
        log_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return log_file != INVALID_HANDLE_VALUE;
    }

    void log_completed_frames()
    {
        // Some presentation paths can omit the final ReShade callback. Such a
        // completed frame cannot leave an older image advertised as N-1.
        if (grayscale != nullptr && grayscale->history_valid() && history_last_updated_frame != present_count)
            invalidate_history("presentation completed without a history update");
        if (present_count == 0 || present_count % 300 != 0 || last_logged_present == present_count)
            return;
        char message[512];
        const int length = std::snprintf(message, sizeof(message),
            "present callbacks=%llu\r\n"
            "reshade_present callbacks=%llu\r\n"
            "grayscale draws=%llu\r\n"
            "failures=%llu\r\n"
            "history valid=%u\r\n"
            "history updates=%llu\r\n"
            "history failures=%llu\r\n",
            static_cast<unsigned long long>(present_count),
            static_cast<unsigned long long>(reshade_present_count),
            static_cast<unsigned long long>(grayscale_draw_count),
            static_cast<unsigned long long>(failure_count),
            grayscale != nullptr && grayscale->history_valid() ? 1u : 0u,
            static_cast<unsigned long long>(history_update_count),
            static_cast<unsigned long long>(history_failure_count));
        if (length > 0 && static_cast<std::size_t>(length) < sizeof(message))
        {
            DWORD written = 0;
            WriteFile(log_file, message, static_cast<DWORD>(length), &written, nullptr);
        }
        last_logged_present = present_count;
    }

    void close_log()
    {
        initialized = false;
        if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
        log_file = INVALID_HANDLE_VALUE;
    }

    const char *depth_format_name(D3DFORMAT format)
    {
        switch (static_cast<DWORD>(format))
        {
        case MAKEFOURCC('I', 'N', 'T', 'Z'): return "INTZ";
        case MAKEFOURCC('D', 'F', '1', '6'): return "DF16";
        case MAKEFOURCC('D', 'F', '2', '4'): return "DF24";
        case MAKEFOURCC('R', 'A', 'W', 'Z'): return "RAWZ";
        case D3DFMT_R32F: return "R32_FLOAT";
        case D3DFMT_R16F: return "R16_FLOAT";
        case D3DFMT_D16: return "D16";
        case D3DFMT_D24S8: return "D24S8";
        case D3DFMT_D24X8: return "D24X8";
        case D3DFMT_D32: return "D32";
        default: return "unlisted";
        }
    }

    void log_depth_source(const enr::depth_source &source, ULONGLONG now)
    {
        if (source.texture != nullptr)
        {
            depth_unavailable_since = now;
            if (!depth_detected_logged || source.width != logged_depth_width ||
                source.height != logged_depth_height || source.format != logged_depth_format)
            {
                write_line("Depth buffer detected");
                write_formatted("Depth resolution: %ux%u", source.width, source.height);
                write_formatted("Depth format: %s (0x%08X)", depth_format_name(source.format),
                    static_cast<unsigned>(source.format));
                depth_detected_logged = true;
                logged_depth_width = source.width;
                logged_depth_height = source.height;
                logged_depth_format = source.format;
            }
        }
        else
        {
            have_depth_sample = false;
            if (!no_depth_logged && present_count >= 300 && now - depth_unavailable_since >= 5000)
            {
                write_line("No usable depth buffer available");
                no_depth_logged = true;
            }
        }
    }

    void handle_depth_probe(const enr::depth_probe_result &result)
    {
        using state = enr::depth_probe_result::state;
        if (result.status == state::unavailable)
        {
            if (!probe_failure_logged)
            {
                write_formatted("Depth change validation unavailable: HRESULT 0x%08X",
                    static_cast<unsigned>(result.error));
                probe_failure_logged = true;
            }
            return;
        }
        if (result.status != state::ready) return;
        write_formatted("Depth samples: valid=%u/256", result.valid);
        if (result.valid == 0 && !empty_depth_logged)
        {
            write_line("No non-clear scene depth in sampled positions");
            empty_depth_logged = true;
        }
        if (have_depth_sample && sampled_generation == result.generation)
        {
            const bool changed = result.valid != previous_valid ||
                result.signature1 != previous_signature1 || result.signature2 != previous_signature2;
            if (changed && !depth_changed_logged)
            {
                write_line("Depth changes observed (sampled GPU signature)");
                depth_changed_logged = true;
            }
            else if (!changed && !depth_unchanged_logged)
            {
                write_line("Depth samples unchanged (change not confirmed)");
                depth_unchanged_logged = true;
            }
        }
        have_depth_sample = true;
        sampled_generation = result.generation;
        previous_valid = result.valid;
        previous_signature1 = result.signature1;
        previous_signature2 = result.signature2;
    }

    void invalidate_history(const char *reason)
    {
        if (grayscale == nullptr) return;
        if (grayscale->history_valid())
            write_formatted("History reset: %s", reason);
        grayscale->invalidate_history();
        history_error_logged = false;
    }

    void log_temporal_result(const enr::temporal_report &report)
    {
        if (report.history_reset)
        {
            write_formatted("History reset: %s", report.reset_reason != nullptr ? report.reset_reason : "resource change");
            history_error_logged = false;
        }
        if (report.history_initialized)
        {
            write_line("Temporal history initialized");
            write_formatted("Color history: %ux%u", report.color_width, report.color_height);
            write_formatted("Depth history: %ux%u", report.depth_width, report.depth_height);
        }
        if (FAILED(report.history_result))
        {
            ++history_failure_count;
            if (!history_error_logged)
            {
                write_formatted("Temporal history unavailable: HRESULT 0x%08X",
                    static_cast<unsigned>(report.history_result));
                history_error_logged = true;
            }
        }
        if (report.valid_after)
        {
            ++history_update_count;
            history_last_updated_frame = present_count;
        }
        if (report.became_valid) write_line("History became valid");
    }

    HRESULT draw_frame(reshade::api::effect_runtime *runtime)
    {
        auto *const device = runtime->get_device();
        if (grayscale == nullptr || device == nullptr ||
            device->get_api() != reshade::api::device_api::d3d9)
        {
            invalidate_history("render device unavailable or incompatible");
            return E_NOINTERFACE;
        }
        auto *const native_device = reinterpret_cast<IDirect3DDevice9 *>(
            static_cast<std::uintptr_t>(device->get_native()));
        auto *const native_swapchain = reinterpret_cast<IDirect3DSwapChain9 *>(
            static_cast<std::uintptr_t>(runtime->get_native()));
        if (native_device == nullptr || native_swapchain == nullptr)
        {
            invalidate_history("native device or swapchain unavailable");
            return E_POINTER;
        }
        if (history_runtime != runtime || history_swapchain != native_swapchain)
        {
            invalidate_history("presentation runtime or swapchain changed");
            history_runtime = runtime;
            history_swapchain = native_swapchain;
        }

        if (runtime->is_key_down(VK_CONTROL))
        {
            if (runtime->is_key_pressed(VK_F6))
            {
                depth_debug = !depth_debug;
                history_debug = 0;
                write_line(depth_debug ? "Depth visualization enabled" : "Grayscale mode enabled");
            }
            if (runtime->is_key_pressed(VK_F7))
            {
                depth_linearize = !depth_linearize;
                write_line(depth_linearize ? "Depth display: linearized" : "Depth display: raw");
            }
            if (runtime->is_key_pressed(VK_F8))
            {
                depth_reversed = !depth_reversed;
                write_line(depth_reversed ? "Depth orientation: reversed" : "Depth orientation: conventional");
            }
            if (runtime->is_key_pressed(VK_F9))
            {
                history_debug = history_debug == 1 ? 0 : 1;
                write_line(history_debug == 1 ? "Previous-frame color visualization enabled" : "Current-frame visualization enabled");
            }
            if (runtime->is_key_pressed(VK_F10))
            {
                history_debug = history_debug == 2 ? 0 : 2;
                write_line(history_debug == 2 ? "Previous-frame depth visualization enabled" : "Current-frame visualization enabled");
            }
        }

        enr::depth_source source = {};
        const ULONGLONG now = GetTickCount64();
        if (depth_binding != nullptr)
            depth_binding->acquire(runtime, effects_runtime == runtime && effects_frame == present_count, source);
        log_depth_source(source, now);

        IDirect3DSurface9 *backbuffer = nullptr;
        HRESULT result = native_swapchain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
        if (SUCCEEDED(result))
        {
            const enr::temporal_view view = history_debug == 1 ? enr::temporal_view::previous_color :
                history_debug == 2 ? enr::temporal_view::previous_depth :
                depth_debug ? enr::temporal_view::current_depth : enr::temporal_view::current_grayscale;
            enr::temporal_report report = {};
            result = grayscale->render_temporal(native_device, backbuffer, source.texture,
                source.generation, present_count, view, depth_linearize, depth_reversed, report);
            log_temporal_result(report);
        }
        else invalidate_history("backbuffer acquisition failed");
        if (backbuffer != nullptr) backbuffer->Release();

        if (SUCCEEDED(result) && source.texture != nullptr)
            handle_depth_probe(grayscale->poll_depth_probe(native_device, source.texture, source.generation, now));
        return result;
    }

    void on_present(reshade::api::command_queue *, reshade::api::swapchain *,
        const reshade::api::rect *, const reshade::api::rect *, std::uint32_t, const reshade::api::rect *)
    {
        AcquireSRWLockExclusive(&state_lock);
        if (initialized)
        {
            log_completed_frames();
            ++present_count;
        }
        ReleaseSRWLockExclusive(&state_lock);
    }

    void on_finish_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *,
        reshade::api::resource_view, reshade::api::resource_view)
    {
        AcquireSRWLockExclusive(&state_lock);
        if (initialized)
        {
            effects_runtime = runtime;
            effects_frame = present_count;
        }
        ReleaseSRWLockExclusive(&state_lock);
    }

    void on_reshade_present(reshade::api::effect_runtime *runtime)
    {
        AcquireSRWLockExclusive(&state_lock);
        if (initialized)
        {
            ++reshade_present_count;
            if (runtime != nullptr)
            {
                // The established final rendering point remains unchanged.
                if (SUCCEEDED(draw_frame(runtime))) ++grayscale_draw_count;
                else ++failure_count;
            }
            else invalidate_history("effect runtime unavailable");
        }
        ReleaseSRWLockExclusive(&state_lock);
    }

    void on_finish_present(reshade::api::command_queue *, reshade::api::swapchain *)
    {
        AcquireSRWLockExclusive(&state_lock);
        if (initialized) log_completed_frames();
        ReleaseSRWLockExclusive(&state_lock);
    }

    void reset_depth_tracking()
    {
        if (depth_binding != nullptr) depth_binding->reset();
        effects_runtime = nullptr;
        effects_frame = UINT64_MAX;
        have_depth_sample = false;
        depth_unavailable_since = GetTickCount64();
    }

    void on_reloaded_effects(reshade::api::effect_runtime *)
    {
        AcquireSRWLockExclusive(&state_lock);
        reset_depth_tracking(); // All public effect variable handles were invalidated.
        invalidate_history("ReShade effects reloaded");
        ReleaseSRWLockExclusive(&state_lock);
    }

    void release_resources(const char *reason)
    {
        AcquireSRWLockExclusive(&state_lock);
        if (grayscale != nullptr && grayscale->history_initialized() && !grayscale->history_valid())
            write_formatted("History reset: %s", reason);
        invalidate_history(reason);
        reset_depth_tracking();
        if (grayscale != nullptr) grayscale->reset();
        history_runtime = nullptr;
        history_swapchain = nullptr;
        ReleaseSRWLockExclusive(&state_lock);
    }

    void on_destroy_effect_runtime(reshade::api::effect_runtime *) { release_resources("effect runtime destroyed or reset"); }
    void on_destroy_swapchain(reshade::api::swapchain *, bool resize) { release_resources(resize ? "swapchain resized or reset" : "swapchain destroyed"); }
    void on_destroy_device(reshade::api::device *) { release_resources("device destroyed or reset"); }
}

extern "C"
{
    __declspec(dllexport) const char *NAME = "ENR v0.005";
    __declspec(dllexport) const char *DESCRIPTION = "Stable grayscale and depth debugging with GPU-only previous-frame color and depth.";

    __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
    {
        if (!reshade::register_addon(addon_module, reshade_module)) return false;
        AcquireSRWLockExclusive(&state_lock);
        grayscale = new (std::nothrow) enr::grayscale_d3d9;
        depth_binding = new (std::nothrow) enr::depth_binding;
        if (grayscale == nullptr || depth_binding == nullptr || !open_log_and_config(addon_module))
        {
            delete grayscale;
            delete depth_binding;
            grayscale = nullptr;
            depth_binding = nullptr;
            close_log();
            ReleaseSRWLockExclusive(&state_lock);
            reshade::unregister_addon(addon_module, reshade_module);
            return false;
        }
        present_count = reshade_present_count = grayscale_draw_count = failure_count = 0;
        last_logged_present = 0;
        history_update_count = history_failure_count = 0;
        history_last_updated_frame = 0;
        history_error_logged = false;
        history_runtime = nullptr;
        history_swapchain = nullptr;
        depth_unavailable_since = GetTickCount64();
        depth_detected_logged = no_depth_logged = probe_failure_logged = false;
        depth_changed_logged = depth_unchanged_logged = empty_depth_logged = false;
        logged_depth_width = logged_depth_height = 0;
        logged_depth_format = D3DFMT_UNKNOWN;
        effects_runtime = nullptr;
        effects_frame = UINT64_MAX;
        have_depth_sample = false;
        sampled_generation = 0;
        initialized = true;
        write_line("ENR v0.005 initialized");
        if (depth_debug)
            write_line(depth_linearize ? "Depth visualization enabled (linearized)" : "Depth visualization enabled (raw)");
        if (history_debug != 0)
            write_line(history_debug == 1 ? "Previous-frame color visualization enabled" : "Previous-frame depth visualization enabled");
        const HRESULT helper_result = enr::ensure_depth_effect(addon_module);
        if (FAILED(helper_result))
            write_formatted("Depth helper unavailable: HRESULT 0x%08X", static_cast<unsigned>(helper_result));
        ReleaseSRWLockExclusive(&state_lock);

        reshade::register_event<reshade::addon_event::present>(on_present);
        reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
        reshade::register_event<reshade::addon_event::finish_present>(on_finish_present);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish_effects);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
        return true;
    }

    __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
    {
        reshade::unregister_event<reshade::addon_event::present>(on_present);
        reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present);
        reshade::unregister_event<reshade::addon_event::finish_present>(on_finish_present);
        reshade::unregister_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
        reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
        reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(on_finish_effects);
        reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
        AcquireSRWLockExclusive(&state_lock);
        const bool was_initialized = initialized;
        delete grayscale;
        delete depth_binding;
        grayscale = nullptr;
        depth_binding = nullptr;
        close_log();
        ReleaseSRWLockExclusive(&state_lock);
        if (was_initialized) reshade::unregister_addon(addon_module, reshade_module);
    }
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_DETACH && reserved != nullptr) close_log();
    return TRUE;
}
