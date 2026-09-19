# ENR v0.005

A 32-bit Direct3D 9 add-on for ReShade 6.8. The established final-stage
grayscale remains the default. This milestone stores previous-frame color
and raw scene depth entirely on the GPU while preserving v0.004 depth debugging.

The confirmed v0.004 binary and source snapshot are preserved in
`baselines/v0.004`, with SHA-256 checksums in `SHA256SUMS.txt`. Its add-on hash
matches the v0.004 binary installed in Warrior Within. To roll back, close
the game and restore `baselines/v0.004/enr.addon32` to the game directory.

## Controls

- **Ctrl+F6** switches between ordinary grayscale and fullscreen depth.
- **Ctrl+F7** switches depth display between linearized and raw values.
- **Ctrl+F8** switches conventional/reversed depth orientation.
- **Ctrl+F9** toggles previous-frame color. This shows original color, before
  ENR's grayscale/depth debug display, with a one-frame delay.
- **Ctrl+F10** toggles previous-frame depth; F7/F8 also apply to this view.

F9/F10 are mutually exclusive; pressing the active toggle returns to the
selected current-frame view. F6 exits history display and toggles the current
depth view.

If usable depth is absent, ENR keeps the ordinary grayscale path. These keys
use ReShade's input API. They take effect in the running process and are not
written back to configuration.

Optional `enr.ini` beside the add-on sets startup defaults:

```ini
[ENR]
DepthDebug=0
DepthLinearize=1
DepthReversed=0
HistoryDebug=0
```

`HistoryDebug=1` starts in previous-color mode; `2` starts in previous-depth
mode. The default `0` keeps v0.004's current-frame display selection.

The depth view maps sampled depth to grayscale, preserves destination alpha,
and keeps the backbuffer resolution. Linearization uses a fixed 1000:1
far/near ratio for visibility; it is a debug view, not calibrated world-space
distance. Raw mode shows the original depth values directly. There is no
motion vectors, optical flow, AI, ONNX/DirectML, enhancement or frame generation.

## GPU temporal history

For presentation N, ENR first copies the unmodified current backbuffer into
its reusable scratch texture. It then renders the selected debug view,
reading N-1 history only when valid. Finally, it stores the scratch color and
current raw depth as history for N+1. Debug output is never captured as the
next history frame, which prevents recursive grayscale/depth feedback or a
frozen previous-frame display.

Color history uses the current backbuffer's dimensions and format. Depth
history is R32F at the selected depth buffer's own dimensions, which may
differ from color. Color copies use GPU `StretchRect`. D3D9 cannot copy an
INTZ depth texture into a color render target with ordinary `StretchRect`,
so the depth copy uses a point-sampled GPU shader that writes raw `.r` to
R32F. This follows ReShade's native depth-copy approach. Linearization and
reversal affect the debug display only, never the stored depth.

The textures, surfaces, copy shader and geometry persist across frames. ENR
does not allocate/destroy textures per frame. Compatible source changes
invalidate and reseed history without reallocating it. Device reset, resize
or incompatible storage configuration recreates the affected resources.
The existing scratch image and two history textures are approximately
24 MiB total at 1920x1080 with 32-bit color/depth, before driver overhead.

`grayscale_d3d9::history_valid()` exposes paired color/depth validity.
`previous_color()` and `previous_depth()` expose borrowed GPU textures.
Before processing a frame they hold N-1; after a successful store they hold
N for the next call. First use, skipped frames, missing depth, changed depth
generation, effect reload, runtime/swapchain changes, copy failures, reset
and resize invalidate history. A successful pair of stores makes it valid
for the next frame. Invalid history is never sampled for display; ENR uses
the existing current-frame view until it has a valid pair.

Temporal history performs no CPU pixel readback, staging copy, mapping,
validation probe, flush or GPU wait. Normal D3D9 command ordering keeps reads
of old history ahead of the subsequent overwrite. The infrequent v0.004
integer depth checks below remain separate from temporal storage.

## Depth source and validation

ReShade's built-in generic-depth add-on selects the scene depth buffer and
exposes its shader view through the public `DEPTH` semantic. ENR enumerates
effect texture variables and obtains that view with `get_texture_binding`.
It uses the native D3D9 texture only after a current-frame
`reshade_finish_effects` event, so skipped effect processing cannot reuse a
stale selection. Texture references are cached across unchanged bindings and
released on effect reload, runtime destruction, swapchain reset or shutdown.

ENR writes the bundled `ENR_Depth.addonfx` beside itself on first startup if
that file does not exist. This small, hidden, disabled effect declares a
`DEPTH` texture; its technique does not draw. ReShade's `.addonfx` handling
keeps the depth selection path available when ordinary effects are disabled.
Existing companion files are preserved. No ReShade settings are rewritten.

The companion directory must be included in ReShade's effect search paths.
The current Warrior Within configuration includes `.` already. If a custom
configuration omits it, copy `build/ENR_Depth.addonfx` into an existing effect
search folder and reload effects. Standard ReShade `DepthBufferTex` declarations
can also supply the binding. Supported sampled formats are INTZ, DF16, DF24
and R32_FLOAT; unsupported or absent sources fall back to ordinary grayscale.

Every five seconds at most, three tiny GPU draws sample a 16x16 grid from
the selected depth texture. Reusable occlusion queries return three integer
counts: non-clear samples and two depth-dependent signatures. Query results
are checked on later frames no more often than every 100 ms using
`GetData(..., 0)`. A pending result is skipped rather than waited for.

Only three scalar values are compared on the CPU. There is no depth-image
readback, pixel scanning, staging texture, forced GPU flush, wait loop, or
per-frame probe. Different signatures confirm sampled depth changed; equal
signatures are inconclusive, and an all-clear grid does not prove usable
scene depth. A static scene can legitimately produce unchanged samples.
Probe failure disables validation until resources reset; grayscale and the
depth view remain independent of query availability.

ReShade may preserve scene depth with its own GPU copy before the game
clears it. ENR does not implement a separate depth-buffer replacement or
CPU copy path.

## Rendering and state

The stable default display still performs its fullscreen grayscale draw.
Current-depth and previous-frame views use the same final-stage placement
and RGB-only write mask, leaving destination alpha untouched. History stores
the original color alpha and raw depth. Shader, geometry, texture, state block
and query resources are reused until reset or configuration changes.

The local official ReShade 6.8 source establishes this presentation order:

1. Native D3D9 `BeginScene`, then `addon_event::present`.
2. ReShade effects, then `reshade_finish_effects`.
3. ReShade overlay, final resolved-buffer copyback, and state restoration.
4. `addon_event::reshade_present`: ENR renders into native `GetBackBuffer(0)`.
5. Native `EndScene`, native D3D9 `Present`, then `finish_present`.

Sources: `../reshade/source/runtime.cpp`,
`../reshade/source/d3d9/d3d9_swapchain.cpp`, and
`../reshade/examples/09-depth/generic_depth_addon.cpp`.

A reusable state block plus explicit restoration preserves render targets,
depth binding, viewport, scissor, stream state, shader constants, sRGB state
and mixed vertex processing. Lifecycle callbacks release held DEFAULT-pool
resources before D3D9 Reset. Normal D3D9 presentation submits work; ENR never
forces GPU completion. Initialization failures are cached rather than
recompiling/reallocating on each frame.

## Build and deploy

Use the verified MSYS2 **MINGW32** toolchain:

```sh
cd "/e/code project/Dlls 5/exovyn-neural-renderer"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Output:

```text
E:\code project\Dlls 5\exovyn-neural-renderer\build\enr.addon32
```

CMake builds a C++20 shared library with no `lib` prefix and the `.addon32`
suffix, using `../reshade/include`. Compiler runtimes are linked statically.
Initial shader compilation uses Windows `D3DCompiler_47.dll`. CMake also
places the bundled companion effect in `build/ENR_Depth.addonfx`.

With Warrior Within closed, replace:

```text
E:\SteamLibrary\steamapps\common\Prince of Persia The Warrior Within\enr.addon32
```

Check **ENR v0.005** in ReShade's Add-ons tab. During active gameplay, use
Ctrl+F9 while moving the camera to check previous-frame color. Toggle back,
then check Ctrl+F6 current depth and Ctrl+F10 previous depth. Check pause/resume
and resolution changes too. ReShade's screenshot
hotkey runs before ENR's final callback, so it does not capture this final
debug pass.

## Logging

`enr.log` is recreated beside the add-on. Example entries are:

```text
ENR v0.005 initialized
Depth buffer detected
Depth resolution: 1920x1080
Depth format: INTZ (0x5A544E49)
Temporal history initialized
Color history: 1920x1080
Depth history: 1920x1080
History became valid
Depth samples: valid=.../256
Depth changes observed (sampled GPU signature)
present callbacks=300
reshade_present callbacks=300
grayscale draws=300
failures=0
history valid=1
history updates=...
history failures=0
```

Resolution, format, sample counts, order and counter values depend on the
scene. Acquisition metadata is logged once, then again only if its dimensions
or format changes. After five consecutive seconds without usable depth
(and at least 300 total frames), `No usable depth buffer available` is logged
once. Reset/reload restarts that grace period. Availability can still be
reported later if gameplay supplies depth after a menu.

Sample counts are logged at most once per five-second probe. Changed and
unchanged observations are each logged once; unchanged is explicitly described
as unconfirmed. Mode switches and a validation failure also receive a short
message. History invalidation logs `History reset: ...`; reseeding logs
`History became valid`. Allocation dimensions are logged on initialization
or recreation. History-copy errors have a separate HRESULT message and
failure counter, allowing the stable current-frame display to continue.

The four existing counters plus history validity/update/failure counters are
logged at 300-frame boundaries. `grayscale draws` counts successful final
fullscreen passes in every display mode, excluding history copies and tiny
validation draws. These counters report API success, not visual proof of the
displayed result.

## Tests

`tests/smoke_host.cpp` checks registration, callback counts, log boundaries,
rejected registration and unload/process-exit cleanup.

`tests/d3d9_gpu_host.cpp` exercises the no-depth fallback through the actual
ReShade DLL with Warrior Within's device flags and DISCARD presentation.
`tests/d3d9_depth_host.cpp` renders controlled changing depth through the real
ReShade depth-selection path and checks acquisition, GPU query results,
state restoration and device reset. `tests/d3d9_temporal_host.cpp` checks that
stored color/depth advance correctly, history invalidation, compatible texture
reuse and previous-frame display without CPU pixel inspection. Test commands
are documented in the source files.

These tests use API/state checks and integer queries, without frame readback.
The v0.005 run passed 85 GPU comparisons of 64 samples each, including
1920x1080 color/depth, raw INTZ-to-R32F accuracy and both previous-frame views.
The final add-on also passed 1,814 presentations through real ReShade,
including device reset and history reacquisition, with zero display/history
failures. Previous-color, current-depth, missing-depth and lifecycle fixtures
passed as well. Native GPU results are recorded in
`build/temporal-v005.stdout.txt`; final ReShade results are in
`build/runtime-history-depth-v005/stdout.txt` and its `enr.log`.

The final previous-frame appearance and performance still need confirmation
in the actual game; v0.004 grayscale/depth was already confirmed there.
