// Reuse the established non-readback state checks and RAII helpers. The original
// entry point is retained under a different name and is not run by this fixture.
// MINGW32: g++ -std=c++20 -O2 -Wall -Wextra -DWIN32_LEAN_AND_MEAN -DNOMINMAX
// -municode -static -isystem "../reshade/include" tests/d3d9_depth_host.cpp
// -o build/runtime-depth-v006/d3d9_depth_host.exe
// Run in that isolated folder with ReShade d3d9.dll and enr.addon32 beside it:
// ./d3d9_depth_host.exe "E:/code project/Dlls 5/exovyn-neural-renderer/build/runtime-depth-v006/d3d9.dll"
// ReShade.ini: [GENERAL] EffectSearchPaths=.\; enr.ini: [ENR] DepthDebug=0 or 1.
// The add-on creates ENR_Depth.addonfx; keep it in this isolated effect-search folder.
#define wmain grayscale_only_fixture_entry_point
#include "d3d9_gpu_host.cpp"
#undef wmain

#include <cstring>

namespace
{
    struct scene_vertex { float x, y, z, rhw; D3DCOLOR color; };

    bool create_scene_geometry(IDirect3DDevice9 *device, IDirect3DVertexBuffer9 **buffer)
    {
        // Three depth phases, uploaded once. Sixteen separate draws per
        // frame make this a real scene candidate for Generic Depth's heuristics.
        scene_vertex vertices[192] {};
        for (unsigned phase = 0; phase < 3; ++phase)
            for (unsigned stripe = 0; stripe < 16; ++stripe)
            {
                // The changed phase covers half the depth image. This makes a
                // changed valid-sample count deterministic, independent of any
                // collisions in the bounded statistical depth signatures.
                const unsigned position = phase == 2 ? stripe % 8 : stripe;
                const float left = static_cast<float>(position * 40) - 0.5f;
                const float right = left + 40.0f;
                const float depth = phase == 0 ? 0.2f : 0.8f;
                const D3DCOLOR color = palettes[phase][stripe % 4];
                const scene_vertex quad[] = {
                    { left, -0.5f, depth, 1.0f, color },
                    { right, -0.5f, depth, 1.0f, color },
                    { left, 479.5f, depth, 1.0f, color },
                    { right, 479.5f, depth, 1.0f, color }
                };
                std::memcpy(vertices + phase * 64 + stripe * 4, quad, sizeof(quad));
            }
        if (!succeeded(device->CreateVertexBuffer(sizeof(vertices), D3DUSAGE_WRITEONLY,
                D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, buffer, nullptr), "CreateVertexBuffer(scene)"))
            return false;
        void *destination = nullptr;
        if (!succeeded((*buffer)->Lock(0, sizeof(vertices), &destination, 0), "Lock(scene upload)")) return false;
        std::memcpy(destination, vertices, sizeof(vertices));
        return succeeded((*buffer)->Unlock(), "Unlock(scene upload)");
    }

    bool draw_scene(IDirect3DDevice9 *device, IDirect3DVertexBuffer9 *vertices, unsigned phase)
    {
        const D3DVIEWPORT9 viewport { 0, 0, 640, 480, 0.0f, 1.0f };
        const render_state scene_states[] = {
            { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE },
            { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_SRGBWRITEENABLE, FALSE },
            { D3DRS_COLORWRITEENABLE, 15 }, { D3DRS_CULLMODE, D3DCULL_NONE },
            { D3DRS_LIGHTING, FALSE }, { D3DRS_FOGENABLE, FALSE },
            { D3DRS_ZENABLE, D3DZB_TRUE }, { D3DRS_ZWRITEENABLE, TRUE },
            { D3DRS_ZFUNC, D3DCMP_ALWAYS }, { D3DRS_STENCILENABLE, FALSE }
        };
        if (!succeeded(device->SetViewport(&viewport), "SetViewport(scene)") ||
            !succeeded(device->SetPixelShader(nullptr), "SetPixelShader(scene)") ||
            !succeeded(device->SetVertexShader(nullptr), "SetVertexShader(scene)") ||
            !succeeded(device->SetTexture(0, nullptr), "SetTexture(scene)") ||
            !succeeded(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE), "SetFVF(scene)") ||
            !succeeded(device->SetStreamSource(0, vertices, 0, sizeof(scene_vertex)), "SetStreamSource(scene)") ||
            !succeeded(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), "SetTextureStageState(scene)") ||
            !succeeded(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), "SetTextureStageState(scene)") ||
            !succeeded(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), "SetTextureStageState(scene)") ||
            !succeeded(device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE), "SetTextureStageState(scene)"))
            return false;
        for (const auto state : scene_states)
            if (!succeeded(device->SetRenderState(state.type, state.value), "SetRenderState(scene)")) return false;
        if (!succeeded(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0), "Clear(scene)") ||
            !succeeded(device->BeginScene(), "BeginScene(scene)")) return false;
        for (unsigned stripe = 0; stripe < 16; ++stripe)
            if (!succeeded(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, phase * 64 + stripe * 4, 2), "DrawPrimitive(scene)"))
                return false;
        return succeeded(device->EndScene(), "EndScene(scene)");
    }

    bool check_depth_log(const std::filesystem::path &path)
    {
        const std::string log = read_log(path);
        if (log.find("No usable depth buffer available") != std::string::npos)
        {
            std::fprintf(stderr, "FAIL: transient acquisition/reset incorrectly logged unavailable depth\n%s\n", log.c_str());
            return false;
        }
        for (const char *required : { "ENR v0.006 initialized", "Depth buffer detected",
                "Depth resolution: 640x480", "Depth format:",
                "Depth changes observed (sampled GPU signature)", "present callbacks=300",
                "Depth samples: valid=128/256", "reshade_present callbacks=300", "grayscale draws=300", "failures=0",
                "Temporal history initialized", "Color history: 640x480", "Depth history: 640x480",
                "History became valid", "history valid=1", "history failures=0",
                "Motion resources initialized", "Motion estimation ACTIVE", "Motion failures: 0" })
            if (log.find(required) == std::string::npos)
            {
                std::fprintf(stderr, "FAIL: missing log text '%s'\n%s\n", required, log.c_str());
                return false;
            }
        for (std::size_t offset = 0; (offset = log.find("failures=", offset)) != std::string::npos; ++offset)
            if (log.compare(offset, 12, "failures=0\r\n") != 0)
            {
                std::fprintf(stderr, "FAIL: nonzero draw failure count\n%s\n", log.c_str());
                return false;
            }
        for (std::size_t offset = 0; (offset = log.find("history failures=", offset)) != std::string::npos; ++offset)
            if (log.compare(offset, 20, "history failures=0\r\n") != 0)
            {
                std::fprintf(stderr, "FAIL: nonzero history failure count\n%s\n", log.c_str());
                return false;
            }
        for (std::size_t offset = 0; (offset = log.find("Motion failures: ", offset)) != std::string::npos; ++offset)
            if (log.compare(offset, 20, "Motion failures: 0\r\n") != 0)
            {
                std::fprintf(stderr, "FAIL: nonzero motion failure count\n%s\n", log.c_str());
                return false;
            }
        const std::string motion_marker = "Motion frames processed: ";
        bool processed_motion = false;
        for (std::size_t offset = 0; (offset = log.find(motion_marker, offset)) != std::string::npos; ++offset)
            processed_motion |= std::strtoul(log.c_str() + offset + motion_marker.size(), nullptr, 10) != 0;
        if (!processed_motion)
        {
            std::fprintf(stderr, "FAIL: no motion frames processed\n%s\n", log.c_str());
            return false;
        }
        const std::string marker = "Depth samples: valid=";
        bool valid_samples = false;
        for (std::size_t offset = 0; (offset = log.find(marker, offset)) != std::string::npos; ++offset)
            valid_samples |= std::strtoul(log.c_str() + offset + marker.size(), nullptr, 10) != 0;
        if (!valid_samples)
        {
            std::fprintf(stderr, "FAIL: no nonzero depth sample count\n%s\n", log.c_str());
            return false;
        }
        return true;
    }
}

int wmain(int argc, wchar_t **argv)
{
    // Buffered diagnostics prevent redirected per-character console writes from
    // becoming part of the fixture's real-time scene/probe windows.
    std::setvbuf(stdout,nullptr,_IOFBF,16384);
    std::setvbuf(stderr,nullptr,_IOFBF,16384);
    if (argc != 2)
    {
        std::fprintf(stderr, "Usage: d3d9_depth_host.exe ABSOLUTE_PATH_TO_RESHADE_D3D9_DLL\n");
        return 1;
    }
    const auto log_path = std::filesystem::absolute(argv[1]).parent_path() / "enr.log";
    test_objects objects;
    objects.module = LoadLibraryW(argv[1]);
    if (objects.module == nullptr) return windows_failure("LoadLibraryW(ReShade d3d9.dll)");
    using direct3d_create9 = IDirect3D9 *(WINAPI *)(UINT);
    const auto create_d3d = std::bit_cast<direct3d_create9>(GetProcAddress(objects.module, "Direct3DCreate9"));
    if (create_d3d == nullptr) return windows_failure("GetProcAddress(Direct3DCreate9)");
    objects.window = CreateWindowExW(0, L"STATIC", L"ENR v0.006 depth API/state test",
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
    constexpr DWORD behavior = D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED |
        D3DCREATE_PUREDEVICE | D3DCREATE_HARDWARE_VERTEXPROCESSING;
    if (!succeeded(objects.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            objects.window, behavior, &parameters, &objects.device), "CreateDevice")) return 1;
    com_ptr<IDirect3DVertexBuffer9> scene_vertices;
    com_ptr<IDirect3DTexture9> game_texture;
    com_ptr<IDirect3DTexture9> game_texture1;
    com_ptr<IDirect3DSurface9> application_depth;
    if (!create_scene_geometry(objects.device, scene_vertices.put()) ||
        !succeeded(objects.device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, game_texture.put(), nullptr), "CreateTexture(game0)") ||
        !succeeded(objects.device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, game_texture1.put(), nullptr), "CreateTexture(game1)") ||
        !succeeded(objects.device->GetDepthStencilSurface(application_depth.put()), "GetDepthStencilSurface(game)")) return 1;

    unsigned frame = 0;
    unsigned reset_frame = 0;
    ULONGLONG maximum_frame_ms=0,maximum_log_ms=0;
    const auto present_scene = [&](unsigned phase)
    {
        const ULONGLONG frame_started=GetTickCount64();
        if (!draw_scene(objects.device, scene_vertices, phase) ||
            !set_game_state(objects.device, 640, 480, game_texture, scene_vertices) ||
            !succeeded(objects.device->SetTexture(1, game_texture1), "SetTexture(game1)")) return false;
        const float game_constants[] = { 0.123f, 0.456f, 0.789f, 0.321f,
            0.937f, 0.823f, 0.719f, 0.617f };
        if (!succeeded(objects.device->SetPixelShaderConstantF(0, game_constants, 2), "SetPixelShaderConstantF(game)")) return false;
        com_ptr<IDirect3DSurface9> before_target;
        if (!succeeded(objects.device->GetRenderTarget(0, before_target.put()), "GetRenderTarget(before)")) return false;
        const ULONGLONG present_started=GetTickCount64();
        const HRESULT present_result=objects.device->Present(nullptr,nullptr,nullptr,nullptr);
        const ULONGLONG present_ms=GetTickCount64()-present_started;
        if (!succeeded(present_result, "Present") ||
            !check_game_state(objects.device, 640, 480, game_texture, scene_vertices)) return false;
        com_ptr<IDirect3DBaseTexture9> restored_texture1;
        com_ptr<IDirect3DSurface9> restored_depth;
        com_ptr<IDirect3DSurface9> restored_target;
        com_ptr<IDirect3DPixelShader9> restored_shader;
        float restored_constants[8] {};
        if (!succeeded(objects.device->GetTexture(1, restored_texture1.put()), "GetTexture(game1)") ||
            !succeeded(objects.device->GetDepthStencilSurface(restored_depth.put()), "GetDepthStencilSurface(restored)") ||
            !succeeded(objects.device->GetRenderTarget(0, restored_target.put()), "GetRenderTarget(restored)") ||
            !succeeded(objects.device->GetPixelShader(restored_shader.put()), "GetPixelShader(restored)") ||
            !succeeded(objects.device->GetPixelShaderConstantF(0, restored_constants, 2), "GetPixelShaderConstantF(restored)")) return false;
        if (restored_texture1.value != game_texture1.value || restored_depth.value != application_depth.value ||
            restored_target.value != before_target.value || restored_shader.value != nullptr ||
            std::memcmp(game_constants, restored_constants, sizeof(game_constants)) != 0)
        {
            std::fprintf(stderr, "FAIL: application RT/depth/texture1/PS/constants changed\n");
            return false;
        }
        const ULONGLONG checked_at=GetTickCount64();
        ++frame;
        // Pace the CPU fixture only, so the runtime's five-second async sample
        // interval elapses. This never waits for or explicitly flushes the GPU.
        Sleep(10);
        const ULONGLONG frame_ms=GetTickCount64()-frame_started;
        if(frame_ms>maximum_frame_ms)maximum_frame_ms=frame_ms;
        const ULONGLONG log_started=GetTickCount64();
        if(frame<=5 || (reset_frame!=0 && frame-reset_frame<=10) || frame_ms>1000)
            std::printf("Fixture frame %u%s: setup=%llu, Present=%llu, checks=%llu, total including CPU pacing=%llu ms\n",
                frame,reset_frame?" after reset":"",static_cast<unsigned long long>(present_started-frame_started),
                static_cast<unsigned long long>(present_ms),static_cast<unsigned long long>(checked_at-present_started-present_ms),
                static_cast<unsigned long long>(frame_ms));
        const ULONGLONG log_ms=GetTickCount64()-log_started;
        if(log_ms>maximum_log_ms)maximum_log_ms=log_ms;
        return true;
    };
    const ULONGLONG start = GetTickCount64();
    bool uniform_change_checked = false;
    do
    {
        const ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed >= 12000 && !uniform_change_checked)
        {
            uniform_change_checked = true;
            if (read_log(log_path).find("Depth changes observed (sampled GPU signature)") == std::string::npos)
            {
                std::fprintf(stderr, "FAIL: uniform .2 -> .8 scene-depth change not detected before coverage change\n%s\n",
                    read_log(log_path).c_str());
                return 1;
            }
        }
        if (!present_scene(elapsed < 7000 ? 0 : elapsed < 12000 ? 1 : 2)) return 1;
    } while (GetTickCount64() - start < 22000 || frame < 600);

    if (!check_depth_log(log_path)) return 1;
    const std::size_t before_reset_log_size = read_log(log_path).size();
    // Drop all application references to default-pool surfaces. The add-on's
    // destroy/reset events must also release its source and reusable resources.
    if (!succeeded(objects.device->SetTexture(0, nullptr), "SetTexture0(before reset)") ||
        !succeeded(objects.device->SetTexture(1, nullptr), "SetTexture1(before reset)") ||
        !succeeded(objects.device->SetStreamSource(0, nullptr, 0, 0), "SetStreamSource(before reset)") ||
        !succeeded(objects.device->SetDepthStencilSurface(nullptr), "SetDepthStencilSurface(before reset)")) return 1;
    application_depth.value->Release();
    application_depth.value = nullptr;
    const ULONGLONG reset_started=GetTickCount64();
    const HRESULT reset_result=objects.device->Reset(&parameters);
    std::printf("Fixture native Reset CPU wall: %llu ms\n",static_cast<unsigned long long>(GetTickCount64()-reset_started));
    reset_frame=frame;
    if (!succeeded(reset_result, "Reset(with acquired depth)") ||
        !succeeded(objects.device->GetDepthStencilSurface(application_depth.put()), "GetDepthStencilSurface(after reset)")) return 1;
    const ULONGLONG reset_time = GetTickCount64();
    do
    {
        if (!present_scene(0)) return 1;
    } while (GetTickCount64() - reset_time < 7000);
    std::printf("Fixture post-reset window: %u frames in %llu ms\n",frame-reset_frame,static_cast<unsigned long long>(GetTickCount64()-reset_time));
    std::printf("Fixture max whole-frame CPU wall=%llu ms; max buffered log call=%llu ms\n",
        static_cast<unsigned long long>(maximum_frame_ms),static_cast<unsigned long long>(maximum_log_ms));
    const auto after_reset_log = read_log(log_path).substr(before_reset_log_size);
    if (after_reset_log.find("Depth samples: valid=256/256") == std::string::npos ||
        after_reset_log.find("Temporal history initialized") == std::string::npos ||
        after_reset_log.find("History became valid") == std::string::npos || !check_depth_log(log_path))
    {
        std::fprintf(stderr, "FAIL: scene depth not reacquired after reset\n%s\n", after_reset_log.c_str());
        return 1;
    }
    std::printf("PASS: %u real ReShade presents; 16 scene-depth draws/frame; changing depth acquired; "
        "nonzero asynchronous depth samples; zero pass failures; application state/PS constants restored; "
        "depth reacquired after Reset; no CPU image inspection or forced GPU synchronization\n", frame);
    return 0;
}
