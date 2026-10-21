# Frame pacing review: Steam Deck attribution, October 20, 2026

Independent review of the [frame pacing handover](frame-pacing-claude-handoff.md),
the [Deck validation](steam-deck-prototype-validation.md) and the
[presentation timing](gpu-presentation-timing.md) evidence, plus one new
protocol-level Deck capture. No renderer code, default, commit or push was
changed. Facts are labelled as measured; everything else is inference.

## Answers first

- **The host present-return excess is not a GPU bottleneck.** On the Deck the
  owner's swapchain wait is `max(previous frame's GPU fence, compositor buffer
  release)`, and the release dominates in every steady-state sample. The GPU
  finishes the previous frame before KWin hands a buffer back. Freeing CPU time
  therefore moves into that wait; it cannot improve display cadence, which is
  already a steady 90 Hz.
- **Latency, not throughput, is the Deck problem.** Commit-to-display is
  38.2 ms (measured on the compositor's own clock). That is three refreshes of
  queue plus KWin's compositing lead, set by Mesa's three-image floor for FIFO
  swapchains on Wayland and by KWin applying one queued commit per painted
  frame. Neither SDL's one-frame-in-flight setting nor any game-side CPU saving
  changes it.
- **Left alone, the pipeline does not miss.** Two 25-minute arms run the same
  evening (clear-only control, then stationary Bloodpool) produced zero display
  misses outside the harness's own poll windows: 0 in 269,000 display intervals.
  Every one of the 20 misses that did occur fell 36 to 54 ms before a
  `kscreen-doctor` poll ended; that tool is a Qt Wayland client, and a new client
  connecting made KWin drop a frame in 20 of 53 polls. The three misses in the
  earlier short runs (one flip-stage pair, one system-wide CPU slowdown) did not
  recur and are plausibly external too, but that is not proven.
- **The deep queue is what hides host jitter.** A commit can arrive about
  20 ms late before it misses its FIFO slot. In the 25-minute game arm the host
  loop had 11 natural hiccups of 13 to 24.6 ms (one per 2.3 minutes) and none
  reached the display. Any latency reduction removes that cushion, so host
  jitter attribution becomes the gating work.
- **Desktop KWin results do not transfer to Game Mode.** KWin composites every
  frame (no direct scanout in any sample). Gamescope has a different queue,
  scanout and pacing model and has not been measured.

## Long arms, October 20, 22:03 to 22:54

Two sequential 25-minute arms on the same Deck desktop, same observer build
(capacities raised to 200,000 feedback and 450,000 scope records), no sysfs or
SMU telemetry during the run, run as a transient systemd user service. The
harness polled the panel every 60 s with `kscreen-doctor --dpms show` plus a
`gdbus` lock query, and a keepalive called PowerDevil's `wakeup` every 60 s to
stop dimming; both are timestamped. Evidence:
`runs/frame-pacing-claude-20261020/deck/runs/feedback-claude-long-{control,game}-20261021-050337*`,
`analysis-long-control.json`, `analysis-long-game.json`, `clocks-*`, `keepalive-*`.

| | Clear-only control | Stationary Bloodpool |
|---|---:|---:|
| Duration analysed | 1,489.8 s | 1,504.2 s |
| Presentation requests / presented / discarded | 134,085 / 134,075 / 10 | 135,383 / 135,373 / 10 |
| Display interval p99 / max | 11.111 / 22.222 ms | 11.111 / 22.222 ms |
| Display gaps over 16.67 ms | 10 | 10 |
| Gaps inside a harness poll window | 10 | 10 |
| Gaps outside any harness window | **0** | **0** |
| Host interval p99 / max | 12.12 / 17.29 ms | 11.91 / 24.60 ms |
| Return → display, slew-corrected mean | 38.10 ms | 38.68 ms |
| Presentation flags | 7 on every frame | 7 on every frame |
| Clock slew removed | 12.8 ppm | 13.0 ppm |

What the arms establish, as measured:

- Every miss has the compositor signature (one frame held one refresh, the next
  request discarded) and sits 30 to 54 ms before the recorded end of a panel
  poll, i.e. while `kscreen-doctor` and `gdbus` were running. The keepalive's
  phase drifted from 0.1 s to 1.7 s away from the misses across the run, so it is
  not the trigger. `kscreen-doctor` is a Qt Wayland client; the inference is
  that a client connecting and querying outputs costs KWin's main thread enough
  to miss a flip about 38% of the time.
- With those windows removed, neither arm missed a refresh: 0 events in
  about 0.83 hours of 90 Hz. The 95% upper bound on a Poisson rate with zero
  events is about 3.6 per hour combined. The earlier short runs showed about 47
  per hour (3 in 20,600 intervals); that rate would have produced about 39 misses
  here. Those earlier captures ran with a 1 Hz sysfs/SMU sampler, a 5 s `gdbus`
  poll, and a person at the Deck; one of the three misses fell inside a sampler
  read. External triggers are the most economical explanation, but the two
  signatures described above remain real observations.
- Host jitter is real and absorbed. The game arm had 23 loop intervals over
  13 ms (12 inside poll windows, 11 natural, largest 24.6 ms), two draw-work
  spikes and two slow producer frames out of 90,400. None reached the display.
  A commit can be about 20 ms late before KWin's apply point for its slot passes.
- The return-to-display latency is flat at 38 ms. Without the slew correction
  the control appears to drift from 39.8 to 58.0 ms over 25 minutes; that is the
  single-offset CLOCK_MONOTONIC_RAW to CLOCK_MONOTONIC conversion, now removed by
  fitting the slope of (presented − returned) and reported as `clock_slew_ppm_removed`.
- Direct scanout never engaged in either arm (no zero_copy flag), including the
  SDL_WINDOW_FULLSCREEN control.

Harness lessons, applied in `runs/frame-pacing-claude-20261020/deck/harness-v3/`
and staged on the Deck:

1. SteamOS sets `KillUserProcesses=True`; anything launched from an ssh session
   dies when it closes, even under `setsid nohup`. Launch with
   `systemd-run --user --unit=<name> --collect ...`. The first attempt
   (`feedback-claude-long-control-20261021-045410`, 12 s) is kept as evidence.
2. `systemd-inhibit` from ssh needs interactive polkit authentication. PowerDevil's
   `org.kde.Solid.PowerManagement.wakeup` DBus call wakes a DPMS-off panel,
   restores brightness and resets the idle timers; calling it every 60 s kept the
   panel on for 51 minutes and did not correlate with any miss.
3. Never run a Qt or Wayland client during a measured window. The v3 runners poll
   only `/sys/class/backlight/amdgpu_bl0/actual_brightness` during a run and do
   the DPMS and lock checks at start and end.
4. The Bloodpool replay keeps a valid stationary scene for at least 25 minutes
   past the end of its 2,600-record input file with the invulnerability cheats.

## New evidence

`runs/frame-pacing-claude-20261020/deck/` (ignored, main workspace) holds one
Bloodpool observer replay with `WAYLAND_DEBUG=1`, its clock snapshots, the
harness log, and the analysis scripts used below (`gap_timeline.py`,
`spike_census.py`, `wayland_protocol_timeline.py`). The replay failed the
harness (timed out at 180 s) because the Deck panel was DPMS-off at launch; see
the instrumentation section. Its 38 s of post-wake steady state is valid for
protocol structure and latency, not for miss-rate statistics.

Measured in that capture (2,646 steady-state commits):

| Quantity | Value |
|---|---:|
| Swapchain buffers in rotation | 3 (wl_buffer 79, 80, 85) |
| Commits carrying fifo set_barrier + wait_barrier | all |
| Commits carrying drm-syncobj acquire + release points | all |
| Presentation flags | 7 on every frame (vsync, hw clock, hw completion; **no zero_copy**) |
| Commit → compositor display time | mean 38.202 ms, p50 38.027, p99 39.157 |
| Undisplayed earlier commits at each commit | 3 in 2,632 of 2,647 |
| Display → feedback event dispatched in the client | mean 3.8 ms |
| vkQueuePresentKHR wall time (observer submit scope) | 0.15 to 0.31 ms mean across runs |

Upstream sources consulted (downloaded, not modified): SDL `release-3.4.16`
`SDL_gpu_vulkan.c`, Mesa `wsi_common_wayland.c` at `mesa-26.1.2`, KWin v6.4.3
`surfaceitem_wayland.cpp`, `surface.cpp`, `renderloop.cpp`,
`drm_commit_thread.cpp`. Deck software: SteamOS holo, kernel 6.18.50-valve2,
Mesa 26.1.2 / RADV 26.2-devel, KWin 6.4.3, SDL 3.4.16 (SteamRT build).

## Instrumentation and reduction flaws

1. **Panel power is unobserved.** The harness checks the screensaver lock and
   samples `bl_power`, which stayed 0 while `kscreen-doctor --dpms show`
   reported `off`. In the new run KWin held the first two frames for 141.7 s
   and the owner blocked in the first acquire for 141.66 s until a person
   woke the panel (brightness 73,909 → 246,362; four touchscreen taps and a
   repeated Shift key arrived at 21:28). PowerDevil dims after 2 min and turns
   the panel off after 5 min on battery; on AC the Plasma defaults apply.
   Before any run: require `dpms mode ... on`, require brightness above the
   dimming level, and hold an idle inhibitor for the whole series. Treat any
   held first frame as an invalid run.
2. **Clock domains differ by 11 ppm.** SDL's ticks use `CLOCK_MONOTONIC_RAW`
   on Linux; Wayland feedback uses `CLOCK_MONOTONIC`. The observer converts
   with one offset captured at init. Measured slew: 11.2 ppm on the Deck, and
   2.10 ms across the 181 s run (clock snapshots before and after). Display
   intervals are unaffected; absolute return-to-display drifts by about
   0.4 ms over a 35 s window. Record `CLOCK_MONOTONIC` directly in the observer.
3. **"Discarded" is a compositor catch-up, not a client double-submit.** KWin
   clears a surface's fifo barrier in `handleFramePainted`, every time it
   paints, whether or not that paint's flip lands. After a missed flip the
   late frame is shown one refresh late and the commit behind it is replaced
   before it is ever shown. Every steady-state miss in the data has exactly
   this shape: one 22.2 ms display interval and one discarded request. The
   analyzer counts both correctly; the interpretation should be "one
   compositor miss costs a held frame plus a dropped frame".
4. **`wl_buffer.release` is not a timing signal here.** Under explicit sync
   KWin sends it immediately after each commit (Mesa logs it as discarded).
   The real release is the syncobj release point, which is what acquire waits
   on. The observer's acquire-end time is that signal and is the only
   compositor timing available without root.
5. **The harness telemetry sampler is not causing the misses.** The three
   steady-state misses occurred 36.8, 480 and 940 ms after the nearest 1 Hz
   sample start; six host spikes across observer runs were spread uniformly
   relative to sample times (one within 60 ms, expectation 0.4). The sysfs
   reads do go through the SMU, so keep the sampler out of any run whose
   purpose is a miss-rate comparison.
6. **Miss-rate claims need far longer windows.** Pooled: 3 misses in about
   20,600 game display intervals (one per ~76 s); 0 in 5,089 control
   intervals. At the game's rate the control would have shown 0.74 misses,
   so P(0) ≈ 0.48. Control and game are indistinguishable. Distinguishing a
   two-fold rate change needs on the order of twenty events per arm, i.e.
   25 minutes or more per arm.
7. **Host interval p99 reflects KWin paint completion.** In steady state the
   acquire returns when the release point signals, 5.3 ms (first run) or
   7.7 ms (new run) before each vblank, with ±1 ms jitter. That jitter is the
   whole "12.1 ms host p99" story and carries no information about the game's
   GPU or the display.

## Mechanism of the Deck desktop presentation path (measured, with sources)

```text
owner: offscreen render → acquire (wait prev-frame fence, then Mesa waits release point)
       → blit → submit (vkQueuePresentKHR: syncobj points, fifo set+wait barrier, commit; 0.25 ms)
KWin:  paint for vblank V starts at V − (render estimate + safety + 1 ms)
       → paint reads commit j, clears j's barrier, applies commit j+1
       → j's buffer release point signals when the paint's GPU fence completes (≈ V − 5.3 ms)
       → DrmCommitThread sleeps to V − (vblank time + 1.5 ms + adaptive ≤ 3 ms), commits if fences ready
display: V
```

- SDL requests `allowedFramesInFlight` images (1 under VSync) and is clamped up
  to Mesa's `minImageCount`. Mesa returns 3 for FIFO when `wp_fifo_manager_v1`
  exists (`WSI_WL_DEFAULT_NUM_IMAGES`), 4 only for mailbox. There is no Wayland
  image-count override; `vk_x11_override_min_image_count` is X11-only.
  `MESA_VK_WSI_PRESENT_MODE` exists and can force fifo/mailbox/immediate.
- Three buffers ⇒ the client runs three frames ahead; each queued commit waits
  one paint to be applied and a second to be composited, so commit→display is
  three refreshes plus the compositing lead: 33.3 + ~5 = 38.2 ms.
- KWin composites every frame (flags never include zero_copy). The composite's
  GPU work competes with the game's for the same hardware queue.
- Game-side consequence: `SDL_SetGPUAllowedFramesInFlight(1)` serialises the
  owner against the previous frame's GPU completion but does not shrink the
  present queue.

## Ranked explanations for the three steady-state Deck misses

### 1. Compositor flip-stage miss (gaps 3858 and the new run's commit 619)

Facts. Host loop intervals within ±0.5 ms of 11.11; draw work 3.2 to 4.7 ms;
no late commit. KWin's release point for the slot that was missed signalled
5.4 ms (3858) and 7.7 ms (619) before that vblank, the same phase as every
neighbouring frame. The frame was shown one refresh late, the next request was
discarded, cadence resumed.

Inference. Compositing finished with margin, so the flip itself slipped:
KWin's realtime commit thread waking late, `drmModeAtomicCommit` taking longer
than its 1.5 to 4.5 ms safety margin (amdgpu DC atomic checks, display clock
changes), or a kernel vblank handler delay. Nothing the game submits touches
this stage.

Smallest experiments. (a) Long clear-only control versus long game run (25 min
each, sampler off) to test whether game GPU load changes the rate at all.
(b) `KWIN_DRM_OVERRIDE_SAFETY_MARGIN` raised (needs a KWin restart; owner's
call) to see whether the flip-stage misses disappear. (c) Repeat under Game
Mode, where gamescope owns the flip.

### 2. System-wide CPU slowdown (gap 4080)

Facts. Owner draw wall 5.3, 6.0, 9.3 ms with draw CPU 5.2, 3.8, 8.5 ms
(normal 3.1 ms): the thread was executing, not blocked, and the same work took
up to 2.7× longer. Producer wall 14.1 and 15.9 ms against 10.9 ms CPU (normal
8.5 to 10 ms), so it also slowed and was descheduled. Acquire waits collapsed
to 0.3 to 0.7 ms (the pipeline drained). KWin's release phase drifted from
−5.4 ms to +0.4 ms after the vblank it targeted, then settled at a new,
earlier phase. The 1 Hz telemetry shows one core at 1.62 GHz 469 ms earlier and
`gpu_busy_percent` at 99; sclk stayed at 1040 MHz; no sample fell inside the
event.

Inference. Something slowed every CPU consumer for about 40 ms: CPU frequency
transitions under `amd-pstate-epp` with the `powersave` governor and
`balance_performance` EPP, SMT or memory-bandwidth contention while the GPU was
saturated, or another process waking (Steam client runs in the background).
Detailed scheduler accounting is disabled, so runnable-wait cannot be read.

Smallest experiments. (a) A per-frame owner-thread sample of
`CLOCK_THREAD_CPUTIME_ID` plus `scaling_cur_freq` of the owner's CPU read
inside the observer (one sysfs read per frame, no SMU) to catch the frequency
at the moment of a slow draw. (b) Same workload in Game Mode, where the
power policy differs. Changing the governor needs root and is the owner's call.

### 3. Game-compositor GPU contention

Facts. The game uses about half the GPU (5 to 6 ms of an 11.1 ms budget at
1040 MHz); KWin composites every frame on the same queue. In both flip-stage
misses the composite fence had already signalled early, so contention did not
delay compositing in those events. No GPU timestamps exist for the Type 2
event.

Inference. Contention could lengthen KWin's paint on heavier scenes, but the
evidence available does not show it as the cause of any observed miss.

Smallest experiments. Long runs with the CRT, DOF, rim and lighting effects
disabled (host work unchanged, GPU work halved) against the normal set; if the
miss rate is unchanged, retire this explanation. Direct scanout (fullscreen
state that KWin can scan out, flags & 8) removes the composite entirely.

### 4. Harness telemetry sampler

Tested above; not supported. Keep it out of miss-rate runs anyway.

### 5. Game-side late submission

Not observed in any steady-state miss. In gap 4080 the one late commit still
displayed at its normal slot; the dropped frame was the earlier, on-time one.
Startup and diorama-entry transitions do produce late commits and gaps, and
the existing exclusion windows are correct.

## Latency: what can and cannot move the 38 ms

| Lever | Effect on Deck desktop | Cost / risk |
|---|---|---|
| Keep FIFO, 3 images (current) | 38 ms; absorbs ~15 to 20 ms of host lateness | none |
| Fewer FIFO images | impossible: Mesa floor is 3 on Wayland | — |
| Mailbox + self-pacing locked to `wp_presentation` feedback | ~6 to 17 ms; no barrier wait; compositor still +1 paint | beat/drift between the game's clock and the display unless the pacing follows feedback; every host hiccup over ~5 ms becomes a dropped or repeated frame |
| Run 2-ahead under FIFO by self-throttling after one deliberate skip | ~27 ms | fragile; same jitter exposure as mailbox |
| Direct scanout (fullscreen state KWin accepts) | removes KWin's composite from the GPU queue; latency unchanged or slightly lower | window-mode change; must verify flags & 8 |
| Acquire-before-select (handover question 3) | ≤ 5.5 ms of content age on Deck (the mean acquire wait); 16.5 ms measured on Mac | the Mac win does not transfer; on Deck the queue dominates |

Two game-side observations fall out of the SDL code path:

- Non-VSync refresh modes request `SDL_GPU_PRESENTMODE_IMMEDIATE` whenever the
  window supports it, falling back to mailbox otherwise. On this Deck Mesa
  advertises immediate because KWin exposes tearing control and
  `allowTearing` is true, so those modes will tear in fullscreen rather than
  behave like mailbox. Prefer mailbox explicitly on Wayland when the intent is
  low latency without tearing.
- The acquire wait already encodes the critical dependency the handover asked
  about (question 4): it is the compositor's release point, measured per frame
  as `backend_swapchain_ns`.

## Prototype promotion (applied October 20, evening)

Camera-guided search, effect batching and stationary actor rejection are now
on by default for Metal and Vulkan through
`src/platform/sdl/gpu_prototype_policy.h`; `=0` restores each reference path
and D3D12 keeps the reference paths until a Windows runtime pixel comparison
exists. All three are exact (1,152 oracle checks, 134/134 whole-game frames)
and reduce work; none affects display cadence, and batching's larger acquire
wait is the expected redistribution of saved CPU time into the release-point
wait. Early offscreen submission and the off-thread preparation worker were
removed along with their switches, pacing-trace columns and tests: neither
produced a measured win on any platform, and the worker produced the only
observed multi-frame preparation stall. The promotion pixel A/B (new
defaults against all three forced to `0`, same binary, Marahna phase 0.5 and
Fillmore phases 0.25 and 0.75) matched 134 of 134 frames exactly; it lives in
`runs/frame-pacing-claude-20261020/mac-promotion-ab/`. All 27 affected tests
pass, including the new policy test and an explicit reference-shortcuts run.

## Mac notes

Nothing new was run on the Mac. The Deck result rhymes with the Mac evidence:
the display pipeline, not the application, sets both latency and the rare
holds, and holds occur after the application's GPU work has completed. The
one untested Mac axis worth a controlled reversal is direct-to-display versus
composited presentation (a windowed 960×540 layer is always composited;
`MTL_HUD_ENABLED=1` reports which path is active and shows present-interval
statistics). The 32 versus 49 ms states may be WindowServer adding a frame of
latency; that is a hypothesis, not a finding.

## Next steps, in order

1. Done: panel checks, keepalive, sampler-off harness; two 25-minute arms (see
   the long-arms section). One confirmation pair with the v3 passive poller
   would close the last gap in that result (expected: zero misses, no exclusions).
2. Game Mode (gamescope) session at 90 Hz with the same observer and rooms.
   This is the shipping compositor, it owns the flip, and nothing above predicts
   it. Switching sessions needs the owner at the Deck.
3. One run with the window in a state KWin can scan out directly; check
   `flags & 8`, the release-point phase and commit→display.
4. Only then decide on a mailbox-plus-feedback-paced presentation mode; its
   gate is the host jitter census (11 natural hiccups of 13 to 25 ms per
   25 minutes today) under the chosen power policy.

## Reproduction

```sh
# Deck, after confirming dpms on and an unlocked, unused desktop:
cd /home/deck/argame/prototypes-20261020
qdbus org.kde.Solid.PowerManagement /org/kde/Solid/PowerManagement org.kde.Solid.PowerManagement.wakeup
systemd-run --user --unit=arms-$(date -u +%Y%m%d-%H%M%S) --collect \
  --working-directory=$PWD sh run-arms.sh <stamp>   # control then game, v3 runners
python3 run-deck.py <plan.json>      # original short-run harness; see feedback-claude-wldebug-*.json
# Mac, local reduction:
python3 -I runs/frame-pacing-claude-20261020/deck/gap_timeline.py <run-dir> 1600 3100
python3 -I runs/frame-pacing-claude-20261020/deck/wayland_protocol_timeline.py <run-dir> <clocks-before.json>
python3 -I runs/frame-pacing-claude-20261020/deck/spike_census.py runs/deck-prototype-validation-20261020/deck
python3 -I runs/frame-pacing-claude-20261020/deck/analyze_long_arms.py <run-dir> --clocks <clocks.json> --keepalive <keepalive.log>
```

The handover's tracked tooling checks (`tests/pipeline_performance_tool_test.py`,
`tests/metal_presentation_tool_test.py`, `git diff --check`) were run on the
imported working tree; results are in the session summary.
