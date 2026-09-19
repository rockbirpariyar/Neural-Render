#include <windows.h>
#include <reshade.hpp>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <map>
static_assert(sizeof(void *) == 4);

namespace {
void require(bool ok, const char *message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
bool reject_registration = false;
void *registered_module = nullptr;
void *present_callback = nullptr;
std::map<reshade::addon_event, void *> registered_events;
unsigned registration_calls = 0, unregistration_calls = 0;
std::string read_log(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.is_open(), "enr.log was not created beside the add-on");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}
extern "C" {
__declspec(dllexport) bool ReShadeRegisterAddon(void *module, std::uint32_t version) {
    ++registration_calls;
    require(module && !registered_module, "invalid or duplicate registration");
    require(version == RESHADE_API_VERSION, "SDK version mismatch");
    if (reject_registration) return false;
    registered_module = module;
    return true;
}
__declspec(dllexport) void ReShadeUnregisterAddon(void *module) {
    require(module == registered_module && !present_callback && registered_events.empty(), "incorrect unregister ordering");
    registered_module = nullptr;
    ++unregistration_calls;
}
__declspec(dllexport) void ReShadeRegisterEvent(reshade::addon_event event, void *callback) {
    require(registered_module && callback && !registered_events.contains(event), "incorrect event registration");
    require(event == reshade::addon_event::present || event == reshade::addon_event::reshade_present
        || event == reshade::addon_event::finish_present
        || event == reshade::addon_event::reshade_finish_effects
        || event == reshade::addon_event::reshade_reloaded_effects
        || event == reshade::addon_event::destroy_effect_runtime
        || event == reshade::addon_event::destroy_swapchain || event == reshade::addon_event::destroy_device,
        "unexpected event registration");
    registered_events.emplace(event, callback);
    if (event == reshade::addon_event::present) present_callback = callback;
}
__declspec(dllexport) void ReShadeUnregisterEvent(reshade::addon_event event, void *callback) {
    require(registered_events.contains(event) && registered_events[event] == callback, "incorrect event unregistration");
    registered_events.erase(event);
    if (event == reshade::addon_event::present) present_callback = nullptr;
}
__declspec(dllexport) void ReShadeLogMessage(void *, int, const char *message) {
    std::cerr << "ReShade: " << message << '\n';
}
}
int wmain(int argc, wchar_t **argv) {
    require(argc == 2 || argc == 3, "usage: smoke_host.exe ADDON_PATH [--reject|--exit-process]");
    const std::wstring mode = argc == 3 ? argv[2] : L"";
    reject_registration = mode == L"--reject";
    const auto addon_path = std::filesystem::absolute(argv[1]);
    const auto log_path = addon_path.parent_path() / "enr.log";
    const HMODULE module = LoadLibraryW(addon_path.c_str());
    require(module != nullptr, "LoadLibrary failed");
    require(registration_calls == 0, "bare DLL load registered the add-on");
    const auto init = std::bit_cast<bool (*)(HMODULE, HMODULE)>(GetProcAddress(module, "AddonInit"));
    const auto uninit = std::bit_cast<void (*)(HMODULE, HMODULE)>(GetProcAddress(module, "AddonUninit"));
    const auto name = reinterpret_cast<const char **>(GetProcAddress(module, "NAME"));
    require(init && uninit && name && *name, "missing entry point or NAME export");
    require(std::string(*name) == "ENR v0.006", "unexpected add-on name");
    const HMODULE host = GetModuleHandleW(nullptr);
    const bool initialized = init(module, host);
    require(registration_calls == 1, "initialization must register once");
    if (reject_registration) {
        require(!initialized && !registered_module && !present_callback, "registration failure ignored");
        require(FreeLibrary(module) != 0, "unload after rejection failed");
        std::cout << "PASS: rejected registration\n";
        return 0;
    }
    require(initialized && registered_module == module && present_callback && registered_events.size() == 8, "initialization failed");
    std::string expected = "ENR v0.006 initialized\r\n";
    require(read_log(log_path) == expected, "initialization log mismatch");
    const auto present = reinterpret_cast<reshade::addon_event_traits<reshade::addon_event::present>::decl>(present_callback);
    const auto reshade_present = reinterpret_cast<reshade::addon_event_traits<reshade::addon_event::reshade_present>::decl>(
        registered_events.at(reshade::addon_event::reshade_present));
    const auto finish_present = reinterpret_cast<reshade::addon_event_traits<reshade::addon_event::finish_present>::decl>(
        registered_events.at(reshade::addon_event::finish_present));
    const auto diagnostics = [](unsigned frame) {
        return "present callbacks=" + std::to_string(frame) + "\r\n"
            + "reshade_present callbacks=" + std::to_string(frame / 2) + "\r\n"
            + "grayscale draws=0\r\nfailures=0\r\n"
            + "history valid=0\r\nhistory updates=0\r\nhistory failures=0\r\n"
            + "Motion frames processed: 0\r\nMotion failures: 0\r\n";
    };
    for (unsigned frame = 1; frame <= 900; ++frame) {
        present(nullptr, nullptr, nullptr, nullptr, 0, nullptr);
        // Missing finish_present at frame 600 defers that completed frame's
        // diagnostics to the next present, before its counters advance.
        if (frame == 601) expected += diagnostics(600);
        if (frame % 300 == 0)
            require(read_log(log_path) == expected, "diagnostics logged before the final callback");
        // Counter bookkeeping includes callbacks with no usable runtime, while
        // successful draws remain zero. No fake native graphics object is used.
        if (frame % 2 == 0) reshade_present(nullptr);
        if (frame != 600) {
            finish_present(nullptr, nullptr);
        }
        if (frame % 300 == 0 && frame != 600) {
            expected += diagnostics(frame);
            finish_present(nullptr, nullptr); // Do not duplicate a boundary.
        }
        if (frame <= 11 || frame % 300 == 299 || frame % 300 == 0 || frame == 601)
            require(read_log(log_path) == expected, "incorrect present log boundary");
    }
    if (mode == L"--exit-process") {
        std::cout << "PASS: 900 callbacks; testing process-exit cleanup\n";
        return 0; // Deliberately skip AddonUninit/FreeLibrary.
    }
    uninit(module, host);
    require(read_log(log_path) == expected, "unload must not write runtime diagnostics");
    require(!present_callback && !registered_module && unregistration_calls == 1, "incomplete cleanup");
    require(FreeLibrary(module) != 0, "FreeLibrary failed");
    require(read_log(log_path) == expected, "DLL detach must not write runtime diagnostics");
    std::cout << "PASS: exact 300-frame logging, 900 presents / 450 runtime callbacks / 0 draws / 0 failures, "
        "missing/duplicate finish_present boundaries, v0.006 startup, quiet unload\n";
    return 0;
}
