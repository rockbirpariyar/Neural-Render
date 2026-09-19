#include <windows.h>
#include <d3d9.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <array>
#include <bit>

#include <cstdio>
#include <cwchar>

// MINGW32: g++ -std=c++20 -O2 -Wall -Wextra -DWIN32_LEAN_AND_MEAN -DNOMINMAX
// -municode -static -isystem "../reshade/include" tests/d3d9_gpu_host.cpp
// -o build/runtime-gpu-v005/d3d9_gpu_host.exe
// Run beside the ReShade DLL and ENR. Tests API success, state and integer counters only.
static_assert(sizeof(void *) == 4, "The real ReShade host test must be 32-bit.");

namespace
{
    template <typename T> struct com_ptr
    {
        T *value = nullptr;
        ~com_ptr() { if (value != nullptr) value->Release(); }
        T *operator->() const { return value; }
        operator T *() const { return value; }
        T **put() { return &value; }
    };

    struct test_objects
    {
        HMODULE module = nullptr;
        HWND window = nullptr;
        IDirect3D9 *d3d = nullptr;
        IDirect3DDevice9 *device = nullptr;

        ~test_objects()
        {
            if (device != nullptr) device->Release();
            if (d3d != nullptr) d3d->Release();
            if (window != nullptr) DestroyWindow(window);
            if (module != nullptr) FreeLibrary(module);
        }
    };

    int windows_failure(const char *operation)
    {
        std::fprintf(stderr, "FAIL: %s (Windows error %lu)\n", operation, GetLastError());
        return 1;
    }

    bool succeeded(HRESULT result, const char *operation)
    {
        if (SUCCEEDED(result)) return true;
        std::fprintf(stderr, "FAIL: %s (HRESULT 0x%08lX)\n", operation, static_cast<unsigned long>(result));
        return false;
    }

    struct render_state { D3DRENDERSTATETYPE type; DWORD value; };
    constexpr render_state game_states[] = {
        { D3DRS_SCISSORTESTENABLE, TRUE },
        { D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALPHA },
        { D3DRS_ALPHABLENDENABLE, TRUE },
        { D3DRS_SRCBLEND, D3DBLEND_SRCALPHA },
        { D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA },
        { D3DRS_ZENABLE, D3DZB_FALSE },
        { D3DRS_ALPHATESTENABLE, TRUE },
        { D3DRS_ALPHAFUNC, D3DCMP_GREATER },
        { D3DRS_ALPHAREF, 200 },
        { D3DRS_CULLMODE, D3DCULL_CW },
        { D3DRS_SRGBWRITEENABLE, TRUE },
    };

    // Leave ordinary application state which would break a pass that fails to
    // establish its own state, and check that the pass restores it afterwards.
    bool set_game_state(IDirect3DDevice9 *device, UINT width, UINT height,
        IDirect3DTexture9 *texture, IDirect3DVertexBuffer9 *vertices)
    {
        const D3DVIEWPORT9 viewport { 7, 11, width / 3, height / 4, 0.125f, 0.875f };
        const RECT scissor { 17, 23, static_cast<LONG>(width / 2), static_cast<LONG>(height / 2) };
        if (!succeeded(device->SetViewport(&viewport), "SetViewport(game)") ||
            !succeeded(device->SetScissorRect(&scissor), "SetScissorRect(game)") ||
            !succeeded(device->SetTexture(0, texture), "SetTexture(game)") ||
            !succeeded(device->SetStreamSource(0, vertices, 0, 20), "SetStreamSource(game)") ||
            !succeeded(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE), "SetFVF(game)") ||
            !succeeded(device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP), "SetSamplerState(game)"))
            return false;
        for (const auto state : game_states)
            if (!succeeded(device->SetRenderState(state.type, state.value), "SetRenderState(game)")) return false;
        return true;
    }

    bool check_game_state(IDirect3DDevice9 *device, UINT width, UINT height,
        IDirect3DTexture9 *texture, IDirect3DVertexBuffer9 *vertices)
    {
        D3DVIEWPORT9 viewport {};
        RECT scissor {};
        DWORD fvf = 0, address = 0;
        UINT offset = 0, stride = 0;
        com_ptr<IDirect3DBaseTexture9> current_texture;
        com_ptr<IDirect3DVertexBuffer9> current_vertices;
        if (!succeeded(device->GetViewport(&viewport), "GetViewport") ||
            !succeeded(device->GetScissorRect(&scissor), "GetScissorRect") ||
            !succeeded(device->GetTexture(0, current_texture.put()), "GetTexture") ||
            !succeeded(device->GetStreamSource(0, current_vertices.put(), &offset, &stride), "GetStreamSource") ||
            !succeeded(device->GetFVF(&fvf), "GetFVF") ||
            !succeeded(device->GetSamplerState(0, D3DSAMP_ADDRESSU, &address), "GetSamplerState"))
            return false;
        // Real ReShade 6.8 alone resets the scissor rectangle to the entire
        // backbuffer when restoring its render targets, before reshade_present.
        // ENR must preserve that observed baseline as well as all other state.
        const RECT expected_scissor { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        if (viewport.X != 7 || viewport.Y != 11 || viewport.Width != width / 3 ||
            viewport.Height != height / 4 || viewport.MinZ != 0.125f || viewport.MaxZ != 0.875f ||
            !EqualRect(&scissor, &expected_scissor) || current_texture.value != texture ||
            current_vertices.value != vertices || offset != 0 || stride != 20 ||
            fvf != (D3DFVF_XYZRHW | D3DFVF_DIFFUSE) || address != D3DTADDRESS_WRAP)
        {
            std::fprintf(stderr, "FAIL: application viewport/scissor/texture/stream/FVF/sampler state changed\n");
            std::fprintf(stderr, "viewport actual=(%lu,%lu %lux%lu %.3f..%.3f) expected=(7,11 %ux%u 0.125..0.875)\n",
                viewport.X, viewport.Y, viewport.Width, viewport.Height, viewport.MinZ, viewport.MaxZ, width / 3, height / 4);
            std::fprintf(stderr, "scissor actual=(%ld,%ld,%ld,%ld) expected=(%ld,%ld,%ld,%ld)\n",
                scissor.left, scissor.top, scissor.right, scissor.bottom,
                expected_scissor.left, expected_scissor.top, expected_scissor.right, expected_scissor.bottom);
            std::fprintf(stderr, "texture actual=%p expected=%p; stream actual=%p expected=%p offset=%u expected=0 stride=%u expected=20\n",
                static_cast<void *>(current_texture.value), static_cast<void *>(texture),
                static_cast<void *>(current_vertices.value), static_cast<void *>(vertices), offset, stride);
            std::fprintf(stderr, "FVF actual=0x%08lX expected=0x%08lX; addressU actual=%lu expected=%u\n",
                fvf, static_cast<unsigned long>(D3DFVF_XYZRHW | D3DFVF_DIFFUSE), address, static_cast<unsigned>(D3DTADDRESS_WRAP));
            return false;
        }
        for (const auto state : game_states)
        {
            DWORD value = 0;
            if (!succeeded(device->GetRenderState(state.type, &value), "GetRenderState")) return false;
            if (value != state.value)
            {
                std::fprintf(stderr, "FAIL: render state %u changed from %lu to %lu\n",
                    static_cast<unsigned>(state.type), state.value, value);
                return false;
            }
        }
        return true;
    }

    // Submit changing colored inputs without inspecting rendered output.
    constexpr std::array<std::array<D3DCOLOR, 4>, 3> palettes {{
        { D3DCOLOR_ARGB(0, 255, 0, 0), D3DCOLOR_ARGB(64, 0, 255, 0),
          D3DCOLOR_ARGB(128, 0, 0, 255), D3DCOLOR_ARGB(255, 229, 131, 43) },
        { D3DCOLOR_ARGB(255, 19, 71, 211), D3DCOLOR_ARGB(128, 181, 23, 97),
          D3DCOLOR_ARGB(64, 38, 197, 83), D3DCOLOR_ARGB(0, 249, 193, 53) },
        { D3DCOLOR_ARGB(64, 37, 173, 229), D3DCOLOR_ARGB(0, 213, 41, 131),
          D3DCOLOR_ARGB(255, 237, 211, 29), D3DCOLOR_ARGB(128, 67, 11, 157) },
    }};

    std::string read_log(const std::filesystem::path &path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open()) return "<missing enr.log>";
        return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
    }

    bool check_counters(const std::filesystem::path &log_path, unsigned frame)
    {
        const auto actual = read_log(log_path);
        if (!actual.starts_with("ENR v0.005 initialized\r\n"))
        {
            std::fprintf(stderr, "FAIL: missing v0.005 initialization log\n%s\n", actual.c_str());
            return false;
        }
        for (unsigned boundary = 300; boundary <= frame; boundary += 300)
        {
            const std::string expected = "present callbacks=" + std::to_string(boundary) + "\r\n"
                + "reshade_present callbacks=" + std::to_string(boundary) + "\r\n"
                + "grayscale draws=" + std::to_string(boundary) + "\r\nfailures=0\r\n"
                + "history valid=0\r\nhistory updates=0\r\nhistory failures=0\r\n";
            const auto occurrence = actual.find(expected);
            if (occurrence == std::string::npos || actual.find(expected, occurrence + expected.size()) != std::string::npos)
            {
                std::fprintf(stderr, "FAIL: counters at frame %u\nExpected once:\n%sActual:\n%s\n",
                    frame, expected.c_str(), actual.c_str());
                return false;
            }
        }
        if (frame >= 600)
        {
            const std::string unavailable = "No usable depth buffer available\r\n";
            const auto occurrence = actual.find(unavailable);
            if (occurrence == std::string::npos || actual.find(unavailable, occurrence + unavailable.size()) != std::string::npos ||
                actual.find("Depth buffer detected") != std::string::npos)
            {
                std::fprintf(stderr, "FAIL: missing or duplicate no-depth fallback log\n%s\n", actual.c_str());
                return false;
            }
        }
        return true;
    }

    bool run_frames(IDirect3DDevice9 *device, UINT width, UINT height,
        unsigned first_frame, const std::filesystem::path &log_path)
    {
        com_ptr<IDirect3DTexture9> game_texture;
        com_ptr<IDirect3DTexture9> game_texture1;
        com_ptr<IDirect3DVertexBuffer9> game_vertices;
        com_ptr<IDirect3DSurface9> application_depth;
        if (!succeeded(device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8,
                D3DPOOL_MANAGED, game_texture.put(), nullptr), "CreateTexture(game0)") ||
            !succeeded(device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8,
                D3DPOOL_MANAGED, game_texture1.put(), nullptr), "CreateTexture(game1)") ||
            !succeeded(device->CreateVertexBuffer(80, 0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
                D3DPOOL_MANAGED, game_vertices.put(), nullptr), "CreateVertexBuffer(game)") ||
            !succeeded(device->GetDepthStencilSurface(application_depth.put()), "GetDepthStencilSurface(game)"))
            return false;
        for (unsigned index = 0; index < 300; ++index)
        {
            com_ptr<IDirect3DSurface9> backbuffer;
            if (!succeeded(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO,
                    backbuffer.put()), "GetBackBuffer(game)")) return false;
            D3DSURFACE_DESC description {};
            if (!succeeded(backbuffer->GetDesc(&description), "GetDesc(backbuffer)") ||
                description.Width != width || description.Height != height ||
                description.Format != D3DFMT_A8R8G8B8)
            {
                std::fprintf(stderr, "FAIL: backbuffer dimensions/format changed\n");
                return false;
            }
            for (UINT stripe = 0; stripe < 4; ++stripe)
            {
                const RECT rectangle { static_cast<LONG>(stripe * width / 4), 0,
                    static_cast<LONG>((stripe + 1) * width / 4), static_cast<LONG>(height) };
                if (!succeeded(device->ColorFill(backbuffer, &rectangle,
                        palettes[(first_frame + index) % palettes.size()][stripe]), "ColorFill")) return false;
            }
            if (!set_game_state(device, width, height, game_texture, game_vertices) ||
                !succeeded(device->SetTexture(1, game_texture1), "SetTexture(game1)") ||
                !succeeded(device->BeginScene(), "BeginScene") ||
                !succeeded(device->EndScene(), "EndScene") ||
                !succeeded(device->Present(nullptr, nullptr, nullptr, nullptr), "Present(DISCARD)") ||
                !check_game_state(device, width, height, game_texture, game_vertices)) return false;
            com_ptr<IDirect3DBaseTexture9> restored_texture1;
            com_ptr<IDirect3DSurface9> restored_depth;
            com_ptr<IDirect3DSurface9> restored_target;
            if (!succeeded(device->GetTexture(1, restored_texture1.put()), "GetTexture(game1)") ||
                !succeeded(device->GetDepthStencilSurface(restored_depth.put()), "GetDepthStencilSurface(restored)") ||
                !succeeded(device->GetRenderTarget(0, restored_target.put()), "GetRenderTarget(restored)")) return false;
            if (restored_texture1.value != game_texture1.value ||
                restored_depth.value != application_depth.value || restored_target.value != backbuffer.value)
            {
                std::fprintf(stderr, "FAIL: application texture1/depth/render target binding changed\n");
                return false;
            }
            if ((index == 0 || index == 298 || index == 299) &&
                !check_counters(log_path, first_frame + index + 1)) return false;
            // CPU fixture pacing permits ENR's five-second availability grace
            // period to expire. This does not synchronize with the GPU.
            Sleep(20);
        }
        return succeeded(device->SetTexture(0, nullptr), "SetTexture0(unbind)") &&
            succeeded(device->SetTexture(1, nullptr), "SetTexture1(unbind)") &&
            succeeded(device->SetStreamSource(0, nullptr, 0, 0), "SetStreamSource(unbind)");
    }
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "Usage: d3d9_gpu_host.exe ABSOLUTE_PATH_TO_RESHADE_D3D9_DLL\n");
        return 1;
    }
    const auto log_path = std::filesystem::absolute(argv[1]).parent_path() / "enr.log";
    test_objects objects;
    objects.module = LoadLibraryW(argv[1]);
    if (objects.module == nullptr) return windows_failure("LoadLibraryW(ReShade d3d9.dll)");
    using direct3d_create9 = IDirect3D9 *(WINAPI *)(UINT);
    const auto create_d3d = std::bit_cast<direct3d_create9>(GetProcAddress(objects.module, "Direct3DCreate9"));
    if (create_d3d == nullptr) return windows_failure("GetProcAddress(Direct3DCreate9)");
    objects.window = CreateWindowExW(0, L"STATIC", L"ENR v0.005 D3D9 missing-depth fallback test",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (objects.window == nullptr) return windows_failure("CreateWindowExW");
    objects.d3d = create_d3d(D3D_SDK_VERSION);
    if (objects.d3d == nullptr) return 1;

    D3DPRESENT_PARAMETERS parameters {};
    parameters.BackBufferWidth = 640;
    parameters.BackBufferHeight = 480;
    parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
    parameters.BackBufferCount = 1;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = objects.window;
    parameters.Windowed = TRUE;
    parameters.EnableAutoDepthStencil = TRUE;
    parameters.AutoDepthStencilFormat = D3DFMT_D24S8;
    parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    // Match Warrior Within's requested behavior flags; ENR uses no depth data.
    constexpr DWORD behavior = D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED |
        D3DCREATE_PUREDEVICE | D3DCREATE_HARDWARE_VERTEXPROCESSING;
    static_assert(behavior == 0x56);
    if (!succeeded(objects.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            objects.window, behavior, &parameters, &objects.device), "CreateDevice")) return 1;
    if (!check_counters(log_path, 0) || !run_frames(objects.device, 640, 480, 0, log_path)) return 1;
    parameters.BackBufferWidth = 800;
    parameters.BackBufferHeight = 600;
    if (!succeeded(objects.device->Reset(&parameters), "Reset(800x600)") ||
        !run_frames(objects.device, 800, 600, 300, log_path)) return 1;
    // Device destruction exercises ENR cleanup before the final log check.
    objects.device->Release();
    objects.device = nullptr;
    if (!check_counters(log_path, 600)) return 1;
    std::printf("PASS: 600 real ReShade D3D9 DISCARD presents; flags 0x56; 600 final callbacks/draws; "
        "zero failures; exact 300-frame counters; missing-depth logged once; application state/texture/RT/depth bindings restored; "
        "Reset 640x480 -> 800x600 passed; no output inspection\n");
    return 0;
}
