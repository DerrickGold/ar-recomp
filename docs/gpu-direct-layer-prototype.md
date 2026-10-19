# Direct interpolation layer output prototype

This records the initial opt-in experiment and its measurements. Direct layer
output is now the default for native GPU interpolation, with lazy fallback
atlases. See [production integration](gpu-rendering-integration.md) for the
current policy, controls and additional validation.

## What changes

The old path warps eight global-motion bands into an atlas and four actor
priorities plus their material metadata into two more atlases. A copy pass
extracts the active bands into the compositor's padded layer textures.

The prototype writes those same final textures directly:

- One background compute dispatch binds eight separate storage images. The
  workgroup's plane index selects an explicit binding; no descriptor indexing,
  texture arrays, new dispatch per plane, or platform-specific extension is
  required. Inactive bands do no work.
- The existing per-priority actor render passes write color and material data
  directly into their final targets. The mesh projection uses the destination's
  dimensions and origin; the source atlas, motion field, endpoint choice, and
  fragment sampling are unchanged.
- Background padding is cleared on initialization/region changes and retained
  thereafter. Actor targets clear their complete backing before each warp.
  A failed direct encode invalidates padding before fallback, including targets
  that may have cycled while the cancelled command buffer was being recorded.
- A private material target remains available when a sprite appears before its
  companion metadata, or material lighting is not requested. Only requested,
  valid pairs are published to the compositor.

The compositor still sees the same texture size, apron, UVs and format. Lighting,
rim shading, skybox sampling, snapshots and other consumers need no atlas-view
coordinate changes. Endpoint packing and motion search remain as before. There
are no new production readbacks or CPU waits. The old output atlases remain
allocated for fallback in this prototype; **this is not a VRAM reduction claim**.

Storage output uses the existing SDL GPU API and externally owned textures
wrapped for the renderer. The additional storage-write usage is checked before
allocation. See SDL's [storage binding contract](https://wiki.libsdl.org/SDL3/SDL_GPUStorageTextureReadWriteBinding)
and [texture wrapping properties](https://wiki.libsdl.org/SDL3/SDL_CreateTextureWithProperties).

`[gpu-layer-output]` reports direct frames, unpack frames and avoided copy payload
bytes on teardown. Bytes count the source region once, not read-plus-write bus
traffic, driver overhead or measured device bandwidth. The replay validator
requires direct activation and rejects any fallback diagnostic.

## Correctness (2026-10-19)

The native GPU comparison checks all 16 color/material planes, including
transparent RGB, fractional phases, invalid motion, partial edge blocks,
nonzero destination origins, sparse/inactive masks, re-entry and growing then
shrinking captures. Metal, Windows Vulkan and Windows DX12 pass all 15 cases.
Global compute output and integer endpoints are bit-exact. Synthetic fractional
actor meshes can differ by one UNORM step (1/255) when projected into the smaller
target; no larger changes are accepted. Metal reports 5,019 such pixels and each
Windows backend 6,879 across the 54,067,200 compared pixels.

Full-game Metal comparisons use the same binary, assets, inputs, camera states
and final WRAM hash on each side:

| Replay | Phases | Frames compared | Exact |
| --- | --- | ---: | ---: |
| Marahna 05:01, 1,024 px horizontal / 147 px vertical travel | 0.25, 0.5, 0.75 | 288 | 288 |
| Fillmore 01:01, 1,064 px horizontal / 188 px vertical travel | 0.5 | 86 | 86 |

These exercise extended rows, background layers, sprites, skybox, rim lighting,
DOF and the existing composite output. Fillmore also exercises resident source
projection and environmental effects: 471,033 primitives, zero motion metadata
downloads. Marahna uses the reference projection mode; its unchanged 256-byte
metadata readbacks are not evidence for the resident mode. All candidate captures
report zero unpack frames. Actual avoided payload averages about 2.81 MiB per
synthesized Marahna image and 3.01 MiB in Fillmore.

The frame-generation integration tests pass on Metal, Vulkan and DX12. The
software fallback and all existing frame-generation policies pass on Mac.
ASan/UBSan pass the direct output and integration tests (system leak checking
disabled). All three changed shader headers reproduce from their GLSL sources
for MSL, SPIR-V and DXIL. The performance-tool regression suite passes.

Two complete Windows game replays additionally exercise the direct path through
the real streamed presenter, one on Vulkan and one on DX12. Both finish without
render/session errors and produce the same WRAM SHA-256 as the atlas baseline.
They are compatibility evidence only: neither obtained foreground focus.

## Isolated throughput

`actraiser_gpu_motion_layers_test --benchmark` measures synthesis and handoff
without motion search, compositing, the swapchain or readbacks in the timed
interval. It uses eight syntheses per command buffer/fence, 64 warmup syntheses
per round, then 1,024 measured syntheses. Order is atlas/direct/direct/atlas.
GPU validation is disabled for timing, enabled for the ordinary correctness
test. Values below are medians of the two per-mode round means.

**These are CPU encode-through-GPU-completion wall times per synthesis, not GPU
timestamps, whole-frame timings or a prediction of Steam Deck frame pacing.**
Windows uses Release `-O3` on i9-9900K / GTX 1080 Ti. Metal uses `-O2` on M2.

| Backend | Active output planes | Atlas + unpack | Direct | Reduction |
| --- | ---: | ---: | ---: | ---: |
| Windows Vulkan | 8 | 0.206 ms | 0.124 ms | 40% |
| Windows Vulkan | 16 | 0.314 ms | 0.182 ms | 42% |
| Windows DX12 | 8 | 0.189 ms | 0.135 ms | 29% |
| Windows DX12 | 16 | 0.288 ms | 0.199 ms | 31% |
| Mac Metal | 8 | 0.623 ms | 0.369 ms | 41% |
| Mac Metal | 16 | 0.877 ms | 0.545 ms | 38% |

The 8-plane case contains four global bands and two actor priorities with their
companions; the 16-plane case fills every band. Windows CPU encode/submit also
drops: Vulkan 0.032 → 0.024 ms / 0.055 → 0.035 ms, DX12 0.043 → 0.032 ms /
0.068 → 0.046 ms. Mac CPU results are noisier, including a regression in the
8-plane case (0.052 → 0.062 ms); its completion measurements also vary more
between rounds. Do not infer a stable Mac CPU saving from this sample.

## Decision at the end of the prototype

The component result supports continuing this architecture: it removes actual
copies and reduces isolated synthesis cost on all three backends. Keep the
switch opt-in pending foreground gameplay pacing and Deck validation.

The Windows pacing harness rejected the first live A/B replay because Windows
kept another application in the foreground for all 40 focus samples. It stopped
before running the counterpart. That trace is retained as excluded evidence;
there is no valid new present-p99 comparison. The previous early-submit
prototype is disabled on both sides of this experiment.

Next, repeat foreground 90 Hz and uncapped game comparisons on Windows and Deck,
including Fillmore's effects and Marahna's full-add scrolling. Check present-p99,
producer/presenter timing and source age. If the full-game result holds, remove
unused output atlas allocation from the normal direct path while retaining a
lazy fallback, then consider enabling it by default.

The [integration follow-up](gpu-rendering-integration.md) implements lazy atlas
allocation and enables direct output after additional Mac parity and pacing
checks. Deck and high-refresh throughput validation remain separate work;
the isolated gains above are not a whole-frame budget claim.

Evidence, capture commands, isolated Windows scripts, input/binary hashes and
raw results are in `runs/gpu-direct-layers-20261019/`. `isolated-timings.json`
retains individual round means. Files prefixed `debug-` are earlier exploratory
measurements with validation enabled and are excluded from the table.
