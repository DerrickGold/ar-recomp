# Ordered SDL/custom-GPU submission — 2026-09-11

Status: **enabled by default**. `AR_SDL_GPU_ORDERED=0` at startup explicitly
selects the former window-backed SDL renderer for diagnosis. This is a submission-ordering
fix and architectural foundation, not moving-model GPU projection, instancing
or async compute. It does not remove any CPU worker path.

The integrated default paths at this checkpoint are:

| Path | Default | Diagnostic override |
| --- | --- | --- |
| Ordered SDL/custom-GPU submission | On | `AR_SDL_GPU_ORDERED=0` |
| Held globe ground / solid retention | On | `AR_SIM3D_RETAINED_GROUND=0` / `AR_SIM3D_RETAINED_SOLIDS=0` |
| Grouped town model / terrain-shadow retention | On | `AR_SIM3D_TOWN_RETAINED=0` |
| GPU town cloud composition | On when clouds are enabled and supported | `AR_SIM3D_CLOUD_GPU=0` |
| Globe spherical shadow UV mapping | Automatic GPU path when shadows are enabled | Resource/capability fallback only |
| CPU helpers for remaining work | Automatic, up to three helpers within the core count and work-size threshold | `AR_RENDER_WORKERS=0`–`3` caps helpers |

Player-selected visual effects/quality are unchanged. The standalone GPU model
projection experiment is not an integrated game path and is part of the next
moving-world/terrain work, not a disabled shipping optimization.

## Problem and boundary

`SDL_FlushRenderer` drains the 2D command list into SDL's GPU command buffer,
but does not submit that buffer. A separately submitted custom depth pass can
therefore sample the previous shadow mask. A readback between passes hides the
defect by forcing submission and waiting.

Inspected implementation: [SDL 3.4.12 GPU renderer](https://github.com/libsdl-org/SDL/blob/release-3.4.12/src/render/gpu/SDL_render_gpu.c),
`GPU_RunCommandQueue`, `GPU_RenderPresent`, and `GPU_RenderReadPixels`.

The adapter uses the public `SDL_CreateGPURenderer(device, NULL)` API.
Offscreen `SDL_RenderPresent` submits SDL work without presenting a window.
The SDL platform adapter submits preceding 2D work before each custom GPU pass,
including a queued consumer of the preceding reused depth target. The terminal
present submits the remaining 2D work and blits the owned output to the window's
swapchain exactly once. There is no readback or GPU-idle/fence wait in this path;
normal terminal swapchain acquisition can still wait for presentation pacing.

The application render-device vtable, runner ABI and game/presentation contracts
are unchanged. Native device/window/texture ownership stays in the SDL adapter.
Externally bound test renderers preserve their existing flush behavior; they do
not acquire this ordering guarantee by merely calling the helper.

## Lifecycle and compatibility

- The adapter owns the GPU device, offscreen renderer and default output target.
  The output replaces SDL's normal full-size backbuffer; it is not an extra
  full-size target on top of that backbuffer. CRT retains its existing scene.
- An invalid portable target still means default output. Saved target states
  encode that semantic target, not an expiring private texture pointer. Resize
  replaces the output only after successful allocation/binding. Zero drawable
  extent keeps the last valid allocation; a null acquired swapchain skips blit.
  Successful no-image acquisition still submits the terminal command buffer:
  cancelling it prevents completed offscreen work from retiring on SDL/Vulkan.
  See the [Deck lifecycle regression and validation](steam-deck-hidden-present.md).
- VSync applies to the window swapchain, not the offscreen renderer. Off uses
  Immediate where supported, otherwise Mailbox; unsupported settings fail.
  The existing frames-in-flight policy still owns the queue-depth setting.
- Device creation preserves SDL renderer debug/low-power hints, supported shader
  formats and its compatibility baseline for unused optional GPU features.
  Shader clip-distance outputs are not required for ordinary hardware frustum
  clipping. No new Vulkan/D3D12 feature requirement is imposed by taking over
  submission. Existing depth/shader capability checks remain authoritative.
- Startup failure is reported, not silently relabeled successful. There
  is no automatic renderer switch mid-frame or expansion of resource budgets.

## Correctness evidence

Evidence directory: `/private/tmp/actraiser-ordered-gpu.PQJiTA/` (Mac/Metal).

The focused GPU test verifies 48 alternating current-frame shadow masks without
an intervening readback or window present; only the final consumer is read.
Four additional two-pass chains reuse the custom target with SDL consumers
between passes and read only the terminal composite. VSync on/off, queue-depth
settings, resize, saved default-target restoration and destruction are tested.
All 162 application tests pass with the final default policy. The focused test
also verifies scoped viewport/clip restoration and that only explicit zero
selects the legacy adapter.

Initial full-game captures: navigation 19/19 and Palace 14/14 are byte-identical
to the legacy adapter. Towns differ at one of nine sampled frames, and all 11
frames around that point match with shadows disabled. Disabling retention does
not remove the difference. Intermediate captures confirm complete model data.
Pixel analysis identifies small shadow-shading changes, not lost geometry.

To establish the correct oracle, a temporary diagnostic binary forced submission
and readback in the **legacy** renderer before every custom pass. All 11 ordered
town frames (995–1005, shadows and CRT enabled) match this synchronized reference
byte-for-byte, with identical final WRAM. This demonstrates correction of stale
shadow sampling rather than accepting an unexplained visual regression. The
blocking diagnostic and intermediate dumps are not in shipping source.

With the final adapter default and no opt-in environment flag, another 11 town
captures match the synchronized reference with **zero** helpers and CRT enabled.
Another 19 navigation and 14 Palace captures match legacy with CRT disabled,
exercising the owned default output without the CRT scene target. These runs
also have identical final WRAM. The original CRT-enabled globe comparisons and
the final CRT-disabled comparisons cover both output routes.
The wide Aitos action replay additionally matches all 15 captures across native
action, enhanced action, and a room transition, with identical final WRAM and
CRT disabled. That makes 59 paired final-default captures. The action range is
the fixture's documented game frames 1200–2600; runner tick 3000 does not reach
game frame 2900, so an earlier overlong capture schedule was rejected, not
counted as a visual result.

## Performance and default policy

Benchmarks use isolated saves/settings, serial eight-run ABBAABBA batches, no
captures during timing, and equal queue depth/presentation settings. CPU wall
scopes are not GPU timestamps or Deck FPS. Terminal `present/wait` includes
compositor pacing and is not counted as CPU rendering work; do not mistake a
change there for a measured GPU execution-time change.

Three-helper town render CPU measured 2.632 [2.504–2.722] →
2.470 [2.441–2.544] ms (median [min–max]); a 6.1% lower median with overlapping
ranges. Its presentation scope was more consistent: 0.702 [0.688–0.713] →
0.645 [0.641–0.664] ms. Zero-helper towns measured
2.656 [2.479–2.812] → 2.617 [2.287–2.682] ms: a 1.5% lower median with substantial
noise, not a reliable whole-pipeline speedup. Navigation measured 3.247 →
3.239 ms, effectively neutral (0.2%). These numbers compare against the already
optimized grouped-retention baseline, not the original Steam Deck report, and
must not be added to earlier percentages. The three-helper town batch precedes
the final device-compatibility bootstrap; the later batches include it. Sky
Palace measured 3.226 [3.176–3.241] → 3.137 [3.120–3.167] ms, a 2.8% reduction
with separated ranges. Four batches total 32 timed runs.

At the user's direction, the correctness fix is enabled by default, even where
render CPU timings are effectively neutral. Grouped retention and lazy cloud
fallback preparation are also independently enabled by default. Native
Vulkan/Deck hidden-window lifecycle evidence is now recorded in the
[Deck follow-up](steam-deck-hidden-present.md); normal visible Deck pacing and
D3D12 validation remain outstanding. The Mac measurements are not claims of
measured gains on those backends. Explicit zero
is a diagnostic escape hatch, not an automatic return to stale-shadow behavior.
