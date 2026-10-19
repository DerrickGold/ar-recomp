# GPU rendering integration

Integrated 2026-10-19 from the [direct layer](gpu-direct-layer-prototype.md) and
[Metal drawable-pool](gpu-presentation-queue-experiments.md) experiments.

## Enabled paths

Native GPU interpolation writes the eight background/skybox bands and four
actor color/material pairs directly into their final compositor textures.
This removes the intermediate output-atlas writes and unpack copies without
changing sampling, projection, lighting receivers or the compositor interface.
Endpoint atlases and motion analysis are unchanged. It adds no GPU readback,
CPU fence wait, per-frame allocation or additional rendering pass.

The three output atlases are now allocated only when the reference path is
actually used. `Resize` configures motion dimensions and the actor index grid;
`EnsureOutput` explicitly requests atlas storage. SDL wrappers are also lazy,
and are destroyed before resizing their borrowed native textures. Direct
capture uses no output-atlas storage; the matched replay reference allocated
14,237,696 bytes (13.58 MiB), excluding driver cycling/overhead. This is an
allocation reduction, not a measurement of total process GPU memory.

Shader preparation uses the prospective action-room policy before the first
capture initializes live room state. Direct preparation is attempted once,
independently of common motion preparation. Missing pipeline/storage support
or failed encoding selects the atlas path. A cancelled direct encode invalidates
all generated padding before retrying in a fresh command buffer; fallback is
bounded, not recursive. Requested layers alone are published to consumers.

Mac ordered presentation uses two `CAMetalLayer` drawables under VSync, and
three in immediate/mailbox modes. This is independent of SDL's GPU frames in
flight and the application output texture count. It applies after window
claim, on present-mode changes and before acquiring a drawable, so replacing
the native view does not leave stale policy or object pointers.

The small C bridge resolves SDL's documented
[Cocoa window and Metal view tag](https://wiki.libsdl.org/SDL3/SDL_GetWindowProperties),
then reads/sets Apple's public
[`maximumDrawableCount`](https://developer.apple.com/documentation/quartzcore/cametallayer/maximumdrawablecount).
It runs on the main thread, verifies the layer class and retains no native
objects. Typed Objective-C runtime calls allow the same C-only hermetic build
on Apple Silicon and Intel without requiring AppKit SDK headers or a new
Objective-C build route. No method interception, private selector, SDL patch
or synchronization wait is shipped. Missing native access retains platform
defaults and logs once. Non-Mac builds use a no-op bridge; Vulkan/DX12 policy
is unchanged.

## Diagnostic controls

| Control | Default | Alternative |
| --- | --- | --- |
| `AR_GPU_DIRECT_LAYERS` | Unset, empty, `auto`, or `1`: use direct output when available | `0`: atlas synthesis/unpack; unknown values also disable direct output |
| `AR_METAL_DRAWABLE_COUNT` | Unset, empty, or `auto`: VSync 2, otherwise 3 | `2` or `3`: fixed pool; invalid values log and use automatic policy |
| `AR_GPU_EARLY_SUBMIT` | Off | `1`: existing diagnostic prototype only |

Early submission has not shown a reliable foreground pacing win. It remains
off. The rejected multiple-output-texture experiment remains removed.

`[gpu-layer-output]` records direct/unpack counts, avoided copy payload and
allocated fallback bytes at teardown. `[render] Metal drawable pool=...`
records the applied pool. Debug observers are not dependencies of either path.

## Validation

- Release game and test targets build. The bridge and both motion modules
  compile with strict warnings for arm64 macOS, x86-64 macOS, Windows and Linux
  through the release Zig toolchain. These cross-compiles are not runtime tests.
- Thirteen focused real-Metal/renderer tests pass, including all frame-generation
  policies, atlas fallback, GPU layer comparisons and native window lifetimes.
  Startup prewarming is exercised before any capture; the default resident
  test rejects unpack frames or allocated output atlases.
- The direct layer test compares all 16 color/material outputs across 15 cases,
  including size changes, sparse masks, nonzero origins and fractional phases.
  It begins with no atlases and confirms direct output keeps them unallocated.
- Full-game Metal comparisons at phase 0.5 are exact for all 48 Marahna 05:01
  and 43 Fillmore 01:01 captured frames. BG1 camera travel is 1,024×147 and
  1,044×188 pixels respectively. Both sides use the same binary, assets, inputs
  and final game-state hash. Direct output is confirmed active, with no fallback.
  Fillmore retains resident effect projection and zero motion metadata reads.
- The native Metal pool test exercises VSync/immediate toggles, explicit pool
  overrides, resize, hide/show, minimize/restore, fullscreen, two connected
  displays and renderer destruction/recreation on the same window.
- Private-header and renderer-boundary checks pass. New bridge/test files have
  no style violations; the integration adds none against its turn-start source
  baseline. The repository-wide style gate still reports existing debt, which
  was not hidden by changing the style baseline.

The [prototype report](gpu-direct-layer-prototype.md) retains the earlier native
Vulkan/DX12 runtime parity and isolated throughput results. The integration
changes no shader binaries. Windows/Deck gameplay timing and high-refresh/VRR
Mac presentation are not validated by the 60 Hz Mac checks. Do not extrapolate
these results to a guaranteed Steam Deck 90 Hz frame budget.

## Integrated presentation measurements

Apple M2, SDL 3.4.18, Metal, 1920×1080 backing in a 960×540 logical window,
internal 60 Hz display, VSync. Measurements select ticks 1600–2500 from each
2600-tick replay, excluding loading and shader preparation. All eight captures
are foreground/unlocked, use the same binary and observer, and have matching
final WRAM and effect/layer configuration within each scene. All selected
commands complete, all presents have distinct nonzero OS display timestamps,
and all integrated interpolated runs report zero unpack frames/atlas bytes.

| Replay | Reference mean delay | Integrated mean delay | OS display interval p99, both |
| --- | ---: | ---: | ---: |
| Marahna scrolling, first pair | 65.262 ms | 48.603 ms | 16.667 ms |
| Marahna scrolling, reversed pair | 65.277 ms | 31.929 ms | 16.667 ms |
| Bloodpool environmental effects | 48.558 ms | 31.879 ms | 16.667 ms |
| Marahna, interpolation disabled | 48.593 ms | 31.920 ms | 16.667 ms |

Delay is measured from present API return to the OS-reported display time,
not input-to-photon latency. There are 899 selected presents per run, except
the integrated native control has 900. Maximum display interval is 16.671 ms
or less in every run. Native source cadence shows no held frames, with two
skipped source ticks in the reference versus one integrated; this small
difference is not a long-session cadence claim.

The launch-dependent extra refresh remains visible, now also between the two
integrated Marahna runs. Together with the earlier reversible within-process
experiment, the evidence supports roughly one refresh less presentation delay
from the smaller pool, not a guaranteed fixed latency or two-refresh gain.
The display still limits these replays to 60 Hz. The unchanged p99 therefore
shows preserved cadence here, not an improvement in unrestricted GPU frame
time. Direct-output throughput evidence remains the separate isolated probe.

## Reproduction

Ignored evidence is in `runs/gpu-production-20261019/`: build/test logs, matched
frame captures, replay scripts, timing traces, environment/binary hashes and
the turn-start source baseline. `capture.py` performs the two image comparisons.
`timing.py` runs reference/integrated Marahna in ABBA order, then Bloodpool and
native-frame controls. The reference uses `AR_GPU_DIRECT_LAYERS=0` and
`AR_METAL_DRAWABLE_COUNT=3`; the integrated run has neither override. The Metal
observer only records timestamps/resources and does not change the pool.

The authoritative `snesbuild.ini`, CMake renderer test closures and standalone
SIM voxel-sheet build all include the native bridge. Production shaders remain
portable SDL GPU compute/render pipelines with MSL, SPIR-V and DXIL variants.
