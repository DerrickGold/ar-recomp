# Steam Deck: retire GPU work when a hidden window has no image

## Finding and fix — 2026-09-12

Profiling the real Deck revealed a resource-lifecycle defect in the default
ordered SDL adapter. In a hidden Wayland window, replay performance degraded
over time while AMD GPU virtual-address allocation/free operations dominated
sampled CPU cycles. This distorted the SSH/headless benchmark operating point;
it is not evidence that visible gameplay normally spends that time allocating.

The adapter submits SDL's offscreen work, then acquires a terminal command
buffer and requests the window image. SDL can successfully return no image for
a hidden/minimized window. We previously cancelled that command buffer.

In the bundled [SDL 3.4.14 Vulkan backend](https://github.com/libsdl-org/SDL/blob/release-3.4.14/src/gpu/vulkan/SDL_gpu_vulkan.c),
`VULKAN_INTERNAL_AcquireSwapchainTexture` marks the command buffer as having
requested the swapchain **before** its hidden-window early return.
`VULKAN_Submit` uses that marker to trigger retirement of completed submissions
when the device owns a window. The preceding offscreen submissions do not have
the marker. `VULKAN_Cancel` only cleans the cancelled buffer, leaving the older
completed work retained. Its resources consequently cannot be recycled normally.
The offscreen submission sequence is also visible in SDL's
[GPU renderer implementation](https://github.com/libsdl-org/SDL/blob/release-3.4.14/src/render/gpu/SDL_render_gpu.c).

The correction is to **submit after successful acquisition, even with no image**.
No-image frames skip the blit. Failed acquisition still cancels and reports
failure; failed submission also reports failure. No readback, explicit fence
wait, device-idle wait or new buffer pool is introduced. The nonblocking
completed-fence checks are SDL's existing retirement mechanism.

This changes only `src/platform/sdl/render_sdl.c`. The render vtable, runner ABI,
game-layer contracts, worker selection, quality settings and enabled GPU paths
are unchanged. It applies to every mode using the ordered adapter, not just
action. Externally bound legacy renderers preserve their existing behavior.
The installed Deck binary and user saves were not replaced.

## Validation method

- Control: the installed Deck binary, SHA-256
  `a38e659a0fa535983d565f5d4b3ddb28223e018a554cf1d9ec91e8acb97e7ee1`,
  byte-identical to the preserved local pre-change Linux build. Source baseline
  is `203605ee` (including `067a5a2c` and the default GPU globe-model path).
- Candidate: hermetic `x86_64-linux-gnu` build with this adapter correction,
  SHA-256 `158d05f653180ad265a111ff58baa4913860aa695bb54d3f3103c677781cdfc6`.
- Both use the same bundled SDL 3.4.14, SHA-256
  `4f0b58199bb42cf428e2494db80902613c65a55a3aaf133756c17ff6d38e4a1f`,
  with `LD_LIBRARY_PATH=/home/deck/argame`,
  `XDG_RUNTIME_DIR=/run/user/1000`, `WAYLAND_DISPLAY=wayland-0`.
- Serial ABBAABBA comparisons use `tools/compare_pipeline_performance.py`,
  isolated identical save/settings copies, three helpers for both variants,
  no capture/profiler during timing, and input hashes plus final-WRAM checks.
- An initial action batch overlapped source-report processing on the Deck at
  the start of its first control run. It is excluded from the reported results;
  a separate clean batch replaces it. No heavy diagnostic work runs alongside
  the clean target batches. Steam/compositor activity remains uncontrolled.
- Composite captures must be a separate, untimed comparison. Their readbacks can
  change resource retirement, so captures are a visual oracle, **not** the
  regression's performance test.

Evidence lives on the Deck under
`/home/deck/argame/comparison-20260912.Ra0eem/`, with source/profile evidence
and a local copy of results under `/private/tmp/actraiser-deck-profile.2Y9hIv/`.
These are local diagnostic artifacts, not shipped assets.

## Results and validation limits

The clean action batch completed all eight 3,000-tick runs, using the natural
Aitos replay with interpolation enabled and the `Action 3D`, `04/04` filter.
Every final WRAM hash is
`c97d01974a22892ec1c5d208eb0dcba636f5567eb8d5d770ab31a5f5bf275187`.
The result file is `timing-action-clean/results.json` in the evidence directory.

| CPU wall scope | Control median [min–max], ms | Candidate median [min–max], ms |
| --- | --- | --- |
| Render CPU (tool-defined sum) | 38.496 [38.153–39.111] | 6.512 [6.505–6.516] |
| PPU scanout (nested scope) | 4.020 [4.008–4.059] | 3.935 [3.933–3.941] |
| Upload | 19.710 [19.512–20.084] | 2.272 [2.257–2.277] |
| Presentation | 14.677 [14.432–14.842] | 0.179 [0.177–0.180] |

These describe the **hidden-window regression**, not a visible-game FPS gain.
The output was 1080×672 on the live Wayland desktop. The tool takes five final
settled reporting windows per scene/room, weighted by presents; their frame
counts differ with wall-time reporting cadence. They are not an exact
frame-by-frame microbenchmark. Nested scopes must not be added to their parent.
This fix changes retirement, not the PPU algorithm or visual quality.

The subsequent Quality Sky Palace **control** did not complete. Its log reports
`VK_ERROR_DEVICE_LOST` and a fatal Wayland disconnection, with only 1,110 of
1,800 requested tick-presents. The Deck kernel reported GPU command-submission
memory exhaustion, then OOM-killed `steamwebhelper`, `plasmashell` and
`kwin_wayland`. The candidate could not start because no Wayland video device
remained. This is consistent with the identified retention defect, but there
is **no valid Palace timing comparison or Palace candidate stability result**.
Do not rerun this unsafe old-build hidden Palace control.

The game returned zero after that interrupted control. The benchmark tool now
rejects logged Vulkan/Wayland failures and missing/short/extra presentation
schedules before reducing timing samples, even with a zero process exit code.
Its regression tests cover this case. All eight completed action logs pass the
new checks; the interrupted Palace log is rejected. This guards evidence
acceptance; it is not a memory watchdog and cannot prevent an OOM during a run.

The adapter protocol regression checks 512 alternating image/no-image frames,
exact submit/cancel/blit counts, legacy behavior, and error propagation. It
fails against the old cancellation branch and passes with the correction.
It also passes as an optimized `-DNDEBUG` Linux executable on the Deck without
opening a GPU device/window. Mac Debug tests and the Release game build pass;
the full local suite contains 165 tests, including the existing Metal tests.

At this checkpoint Deck composite parity and globe-specific candidate validation
were pending graphical-session recovery; the targeted follow-up below verifies
the fixed Palace path. D3D12 runtime validation and normal visible Deck pacing
remain unmeasured. The installed Deck game is still the old control; the fixed
executable is isolated at
`/home/deck/argame/comparison-20260912.Ra0eem/candidate`.

### Authorized recovery and targeted Palace probe

With the user's approval, the follow-up restored the prior Wayland desktop using
`steamosctl switch-to-desktop-mode plasma.desktop`. SteamOS had automatically
recovered into Gaming Mode after the OOM. No persistent login setting was changed.
The user also clarified the testing policy: build, test and compare primarily
on the Mac; use the Deck for short targeted probes and final direction checks.
Do not repeat long Deck benchmark matrices for each local iteration.

A single fixed-build Quality Palace run completed all 1,000 requested ticks in
8.15 seconds, with no logged rendering failure or fallback. Settled path counters
show 11 GPU reuse events/present with no optional-path rejection; CPU helpers
remain active. This is runtime validation of the default path on Vulkan, not
an old/new visual-parity result or a claim about visible-game FPS.

The temporary probe wrapper checked available RAM and total GPU VRAM/GTT usage
every 100 ms, with a 45-second timeout, a 6 GiB available-RAM floor and a 2 GiB
GPU-memory-growth ceiling. It would terminate only its own process group on a
limit. Available RAM never fell below 12,506,300,416 bytes (11.65 GiB); total GPU
usage peaked at 691,933,184 bytes (660 MiB), from 413,548,544 initially. No guard
fired. Never use process RSS alone to guard a workload allocating GPU buffers.

Evidence: `fixed-palace-probe/{game.log,probe.json}` under the same Deck directory,
copied locally to `/private/tmp/actraiser-scanout-followup.yIG0Bq/`. The wrapper
is `probe-fixed.py` on the Deck (`probe.py` in that local directory). No profiler
or screenshot readback ran during this stability probe. The old broken hidden
Palace control was not run again.

A separate second guarded run captured game frames 600 and 900 through the
strict final-composite path, with cloud drift held for reproducibility. It also
completed all 1,000 ticks, in 8.05 seconds, with no guard/fallback/failure. The
900-frame image was inspected locally: globe terrain/town objects, atmospheric
clouds, angel, palace columns and dialogue/HUD all render. These are candidate
checkpoints, not a paired legacy pixel oracle. See `fixed-palace-visual/` and
Deck run `runs/20260912-060051/`; a local PNG is `sky-palace-deck.png` beside the
probe evidence. No further Deck timing matrix was run.

The next local scanout trial and its rejection are recorded in
[scanout-export-experiment.md](scanout-export-experiment.md).

## Profiles and next targets

An initial 199 Hz DWARF-stack profile lost samples and is not used for numeric
attribution. A repeat using `perf record -e cycles:u -F 49` without call stacks
recorded 2,283 samples with zero lost samples. More than half of sampled user
cycles were in AMD virtual-address allocation/free instructions. A matching
light candidate profile recorded 867 samples with zero lost samples; those
allocator functions no longer appeared among entries above 0.5%.

With that distortion removed, the largest game-side symbols were
`render_native_fast_line` (26.40%) and `native_resolve_virtual_bg_span` (8.32%).
Source-line samples also land in packed overlay export/palette selection and
final color math inside the inlined fast scanout path. These whole-replay
sample shares include startup and transitions; they are **not** exclusive
settled-stage timings and must not be converted directly into projected gains.

This supports profiling/export work next, not blindly threading the tile-span
loop. `FindGlobalMotion` also appears (4.97%): the handover's host-only conclusion
that search is free still needs a controlled Deck copy/search perturbation.
Do not remove it or change interpolation sampling quality on this profile alone.

Producer dirty tracking and indexed/paletted planes remain separate explicit
runner-contract projects. Do not infer write dirtiness from content masks or
put renderer/SDL handles into the runner. Globe terrain/weather GPU work and
moving-town batching remain on the optimization track; this fix removes a
shared measurement/lifecycle fault before evaluating them.
