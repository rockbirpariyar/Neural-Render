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
class device_identity final : public reshade::api::api_object {
public:
    reshade::api::device_api api=reshade::api::device_api::d3d9;
    std::uint64_t get_native() const override { return 0; }
    void get_private_data(const uint8_t[16],uint64_t *) const override { require(false,"unexpected device private-data read"); }
    void set_private_data(const uint8_t[16],uint64_t) override { require(false,"unexpected device private-data write"); }
    virtual reshade::api::device_api get_api() const { return api; }
};
device_identity api_device;
// Only the shared api_object/device_object scalar vtable prefix is exercised.
// Lifecycle callbacks must never inspect a destroyed object's full runtime or
// swapchain interface, and this fixture supplies no native graphics objects.
class identity_object final : public reshade::api::device_object {
public:
    reshade::api::device *device = reinterpret_cast<reshade::api::device *>(&api_device);
    std::uint64_t native = 0x56780;
    bool forbid_reads = false;
    std::uint64_t get_native() const override { require(!forbid_reads,"native metadata read after initialization/destruction"); return native; }
    reshade::api::device *get_device() override { require(!forbid_reads,"device metadata read after initialization/destruction"); return device; }
    void get_private_data(const uint8_t[16],uint64_t *) const override { require(false,"unexpected private-data read"); }
    void set_private_data(const uint8_t[16],uint64_t) override { require(false,"unexpected private-data write"); }
};
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
        || event == reshade::addon_event::init_effect_runtime
        || event == reshade::addon_event::init_swapchain
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
    require(std::string(*name) == "ENR v0.006.1", "unexpected add-on name");
    const HMODULE host = GetModuleHandleW(nullptr);
    const bool initialized = init(module, host);
    require(registration_calls == 1, "initialization must register once");
    if (reject_registration) {
        require(!initialized && !registered_module && !present_callback, "registration failure ignored");
        require(FreeLibrary(module) != 0, "unload after rejection failed");
        std::cout << "PASS: rejected registration\n";
        return 0;
    }
    require(initialized && registered_module == module && present_callback && registered_events.size() == 10, "initialization failed");
    std::string expected = "ENR v0.006.1 initialized\r\n";
    require(read_log(log_path) == expected, "initialization log mismatch");
    const auto present = reinterpret_cast<reshade::addon_event_traits<reshade::addon_event::present>::decl>(present_callback);
    const auto reshade_present = reinterpret_cast<reshade::addon_event_traits<reshade::addon_event::reshade_present>::decl>(
        registered_events.at(reshade::addon_event::reshade_present));
    const auto finish_present = reinterpret_cast<reshade::addon_event_traits<reshade::addon_event::finish_present>::decl>(
        registered_events.at(reshade::addon_event::finish_present));
    const auto init_swapchain = reinterpret_cast<reshade::addon_event_traits<reshade::addon_event::init_swapchain>::decl>(registered_events.at(reshade::addon_event::init_swapchain));
    const auto init_runtime = reinterpret_cast<reshade::addon_event_traits<reshade::addon_event::init_effect_runtime>::decl>(registered_events.at(reshade::addon_event::init_effect_runtime));
    const auto destroy_runtime = reinterpret_cast<reshade::addon_event_traits<reshade::addon_event::destroy_effect_runtime>::decl>(registered_events.at(reshade::addon_event::destroy_effect_runtime));
    const auto destroy_swapchain = reinterpret_cast<reshade::addon_event_traits<reshade::addon_event::destroy_swapchain>::decl>(registered_events.at(reshade::addon_event::destroy_swapchain));
    const auto destroy_device = reinterpret_cast<reshade::addon_event_traits<reshade::addon_event::destroy_device>::decl>(registered_events.at(reshade::addon_event::destroy_device));
    const auto finish_effects = reinterpret_cast<reshade::addon_event_traits<reshade::addon_event::reshade_finish_effects>::decl>(registered_events.at(reshade::addon_event::reshade_finish_effects));
    const auto reload_effects = reinterpret_cast<reshade::addon_event_traits<reshade::addon_event::reshade_reloaded_effects>::decl>(registered_events.at(reshade::addon_event::reshade_reloaded_effects));
    identity_object swap_identity,runtime_identity;
    auto *swapchain=reinterpret_cast<reshade::api::swapchain *>(&swap_identity);
    auto *runtime=reinterpret_cast<reshade::api::effect_runtime *>(&runtime_identity);
    auto *poison_swapchain=reinterpret_cast<reshade::api::swapchain *>(1);
    auto *poison_runtime=reinterpret_cast<reshade::api::effect_runtime *>(1);
    const auto unknown_callbacks=[&]() {
        present(nullptr,poison_swapchain,nullptr,nullptr,0,nullptr);
        finish_effects(poison_runtime,nullptr,{},{});
        reshade_present(poison_runtime);
        finish_present(nullptr,poison_swapchain);
        reload_effects(poison_runtime);
        destroy_runtime(poison_runtime);
        destroy_swapchain(poison_swapchain,true);
        destroy_device(reinterpret_cast<reshade::api::device *>(1));
    };
    unknown_callbacks();
    require(read_log(log_path)==expected,"unregistered callbacks changed startup state");
    api_device.api=reshade::api::device_api::d3d11;
    init_swapchain(swapchain,true);init_runtime(runtime);
    swap_identity.forbid_reads=runtime_identity.forbid_reads=true;
    present(nullptr,swapchain,nullptr,nullptr,0,nullptr);
    finish_effects(runtime,nullptr,{},{});reshade_present(runtime);finish_present(nullptr,swapchain);
    require(read_log(log_path)==expected,"unsupported graphics API entered presentation bookkeeping");
    destroy_swapchain(swapchain,true);
    api_device.api=reshade::api::device_api::d3d9;
    const auto diagnostics = [](unsigned frame) {
        return "present callbacks=" + std::to_string(frame) + "\r\n"
            + "reshade_present callbacks=0\r\n"
            + "grayscale draws=0\r\nfailures=0\r\n"
            + "history valid=0\r\nhistory updates=0\r\nhistory failures=0\r\n"
            + "Motion frames processed: 0\r\nMotion failures: 0\r\n";
    };
    const auto begin_epoch=[&]() {
        swap_identity.forbid_reads=runtime_identity.forbid_reads=false;
        init_swapchain(swapchain,true);
        init_runtime(runtime);
        swap_identity.forbid_reads=runtime_identity.forbid_reads=true;
        // Even a live runtime is not eligible outside an open presentation.
        finish_effects(runtime,nullptr,{},{});
        reshade_present(runtime);
        expected=read_log(log_path);
    };
    unsigned epoch=0;
    const auto run_epoch=[&](unsigned count,bool deferred_finish) {
      ++epoch;
      for (unsigned frame = 1; frame <= count; ++frame) {
        present(nullptr, swapchain, nullptr, nullptr, 0, nullptr);
        if(frame==1)expected+="Runtime active: epoch="+std::to_string(epoch)+"\r\n";
        // Missing finish_present at frame 600 defers that completed frame's
        // diagnostics to the next present, before its counters advance.
        if (deferred_finish && frame == 601) expected += diagnostics(600);
        if (frame % 300 == 0)
            require(read_log(log_path) == expected, "diagnostics logged before the final callback");
        // Null runtime callbacks are ignored. No fake native graphics object is used.
        if (frame % 2 == 0) reshade_present(nullptr);
        unknown_callbacks();
        if (!(deferred_finish && frame == 600)) {
            finish_present(nullptr, swapchain);
        }
        if (frame % 300 == 0 && !(deferred_finish && frame == 600)) {
            expected += diagnostics(frame);
            finish_present(nullptr, swapchain); // Do not duplicate a boundary.
        }
        if (frame <= 11 || frame % 300 == 299 || frame % 300 == 0 || frame == 601)
            require(read_log(log_path) == expected, "incorrect present log boundary");
      }
    };
    begin_epoch();
    run_epoch(900,true);
    if (mode == L"--exit-process") {
        std::cout << "PASS: 900 callbacks; testing process-exit cleanup\n";
        return 0;
    }
    destroy_runtime(runtime);
    expected=read_log(log_path);
    for(unsigned i=0;i<20;++i){
        present(nullptr,swapchain,nullptr,nullptr,0,nullptr);
        finish_effects(runtime,nullptr,{},{});
        reshade_present(runtime);
        finish_present(nullptr,swapchain);
        reload_effects(runtime);
        unknown_callbacks();
    }
    require(read_log(log_path)==expected,"callbacks after runtime destruction were counted or logged");
    destroy_swapchain(swapchain,true);
    begin_epoch();run_epoch(300,false);
    destroy_device(swap_identity.device);
    expected=read_log(log_path);
    present(nullptr,swapchain,nullptr,nullptr,0,nullptr);
    reshade_present(runtime);reload_effects(runtime);finish_present(nullptr,swapchain);
    require(read_log(log_path)==expected,"callbacks after device destruction were counted or logged");
    begin_epoch();run_epoch(300,false);
    swap_identity.forbid_reads=false;
    init_swapchain(swapchain,true);
    swap_identity.forbid_reads=true;
    expected=read_log(log_path);
    present(nullptr,swapchain,nullptr,nullptr,0,nullptr);
    finish_effects(runtime,nullptr,{},{});reshade_present(runtime);finish_present(nullptr,swapchain);reload_effects(runtime);
    require(read_log(log_path)==expected,"duplicate swapchain init reactivated an old runtime lifetime");
    runtime_identity.forbid_reads=false;
    init_runtime(runtime);
    runtime_identity.forbid_reads=true;
    run_epoch(300,false);
    uninit(module, host);
    require(read_log(log_path) == expected, "unload must not write runtime diagnostics");
    require(!present_callback && !registered_module && unregistration_calls == 1, "incomplete cleanup");
    present(nullptr,swapchain,nullptr,nullptr,0,nullptr);
    finish_effects(runtime,nullptr,{},{});reshade_present(runtime);finish_present(nullptr,swapchain);
    unknown_callbacks();
    require(read_log(log_path)==expected,"saved callbacks after AddonUninit were not inert");
    require(FreeLibrary(module) != 0, "FreeLibrary failed");
    require(read_log(log_path) == expected, "DLL detach must not write runtime diagnostics");
    std::cout << "PASS: 1800 registered presents across 4 epochs; exact 300-frame logging and missing/duplicate finish boundaries; "
        "unknown/closed/destroyed runtime callbacks ignored without dereferencing; same-address reset reuse; device destruction; "
        "unsupported graphics API ignored; duplicate swapchain init cannot revive old runtime; "
        "saved callbacks after AddonUninit inert; v0.006.1 startup and quiet unload\n";
    return 0;
}
