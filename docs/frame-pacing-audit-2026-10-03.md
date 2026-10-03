# Frame-pacing audit — 2026-10-03

## Findings and changes

The reported newest-completed selection problem is real: completion jitter can
change the selected game tick while presentation intervals remain regular.
Native streamed playback now selects against source schedule time, retaining
future packets. Its target is the next capped deadline or an estimated VSync
refresh, minus 0.75 source periods (12.48 ms at NTSC). Waiting for a due packet
stops two milliseconds before the predicted refresh. This bounds the wait; it
cannot guarantee a refresh when rendering or the OS takes longer. Unlimited
presentation preserves the old latest-completed behavior. Interpolation and
game/audio clocks are unchanged; SIM/flat/unsupported scenes keep their existing
synchronous paths.

Mouse motion/wheel/drag, polled camera bindings, and harmless expose, focus,
enter/leave, move and occlusion notifications no longer stop production.
Bindings shared with host edge actions, camera reset, menus, display changes,
resizes and resource resets retain ownership handoffs. Free-camera pose is now
presentation-owned, with settings synchronization during acknowledged owner
service. This also prevents presentation-side camera edits from writing shared
settings during production.

The maintenance claim was partly stale: the existing code already resumes the
producer immediately after housekeeping without draining queued endpoints. That
behavior remains, with its existing regression test.

macOS production now requests user-interactive QoS on the producer thread.
Apple identifies animation work as appropriate for this class in its
[QoS guidance](https://developer.apple.com/library/archive/documentation/Performance/Conceptual/EnergyGuide-iOS/PrioritizeWorkWithQoS.html).
Other platforms' scheduling policies are unchanged. Per-thread CPU time is
collected only when pacing tracing is enabled. A wall/CPU gap demonstrates time
not executing on that thread; it does **not** distinguish blocking, helper waits,
or runnable-thread starvation by itself. It is therefore premature to rule out
all future producer optimizations.

The overlay and log now distinguish actual native source holds/skips from
retained-frame draw calls. The analyzer adds a native cadence threshold, phase
fit, hold histogram, source age at present return, and producer CPU/unaccounted
wall time. Interpolated endpoint repetition is not treated as native judder.

## Live evidence

Isolated Fillmore Act 1 replay, 16:10, 64 extended rows, effects/CRT enabled.
Mac uses windowed Metal with VSync; Deck uses fullscreen Vulkan in the existing
Wayland desktop session with a 90 Hz software cap. Same binary within each
platform's comparisons; no background compilation during timed Mac runs.
Source ticks **1825–3000** give a common settled region at least ten seconds
after the latest stream start across these runs. Cold samples are retained in
raw traces, not mixed into this table. Durations are milliseconds.

| Run | Irregular selection | Mean source age | Producer wall p99 / max | Producer CPU max | Present interval p99 / max |
| --- | ---: | ---: | ---: | ---: | ---: |
| Mac / latest / default QoS | 3.50% | 28.77 | 5.61 / 54.61 | 5.60 | 17.14 / 32.59 |
| Mac / latest / interactive | 4.43% | 28.32 | 5.04 / 5.93 | 5.82 | 17.26 / 27.67 |
| Mac / timeline / interactive | 1.54% | 21.05 | 4.83 / 6.86 | 6.17 | 17.28 / 40.60 |
| Mac / timeline / initiated | 1.11% | 21.17 | 6.50 / 44.65 | 6.14 | 17.25 / 32.30 |
| Mac / timeline / interactive, noisy repeat | 4.31% | 21.74 | 20.57 / 73.60 | 8.24 | 28.22 / 67.36 |
| Deck / latest / 90 Hz | 0.23% | 16.39 | 7.79 / 10.02 | 8.14 | 12.69 / 13.40 |
| Deck / timeline / 90 Hz | 0.00% | 22.05 | 7.71 / 8.20 | 8.10 | 12.64 / 14.30 |

“Irregular selection” is the fraction inconsistent with the best single
source/display phase in each epoch, using actual present-return times. It is a
lower bound, not the third-party audit's exact repeat/skip formula; its reported
percentages cannot be compared directly to this table. It accepts the normal
60.0988/60 drift and the normal 1/2 holds at 90 Hz.

The first Mac timeline run selected every native endpoint correctly for its
predicted target in this window. Its remaining phase-fit mismatches include
presentation-time prediction error (0.68 ms p99, up to 24.0 ms in that run).
Source age decreased because predicting the next refresh allows the presenter
to wait briefly for the appropriate tick instead of immediately selecting an
older one. This is source-schedule age, **not input-to-photon latency**.

On Deck the newer renderer's baseline was already cleaner than the old traces
cited in the external audit. Both new runs held 585 presents and skipped zero
ticks; the holds became consistent with one phase in the timeline run. The cost
was **5.66 ms greater mean source age**, not the external replay's suggested
2 ms. The measured present p99 remained about 12.6–12.7 ms, above a perfect
11.11 ms 90 Hz cadence; frame-selection improvements do not remove that jitter.

QoS is useful, but not a guarantee: default QoS had a 54.61 ms producer call
with at most 5.60 ms CPU per uploaded endpoint in the measured window.
User-initiated still hit 44.65 ms. The first interactive runs stayed below
7 ms there, but the repeat hit **73.60 ms wall / 8.24 ms maximum CPU**, along with
67.36 ms present intervals. Its CPU work was also substantially higher overall,
so this is not a controlled attribution to QoS. The window was covered during
that run; an ignored-by-normal-loop occlusion event caused a handoff. That
additional classification fix was applied afterward. Do not discard this run
or claim that elevated QoS eliminates OS/compositor stalls. Further scheduling
attribution needs OS scheduler/blocking evidence during a comparable visible
run, not simply another PPU optimization or a CPU/wall subtraction.

## Validation and reproduction

- Eight focused native tests pass in Release and ASan/UBSan builds: playout,
  packet queue, maintenance retention, producer lifecycle, input routing,
  settings/bindings, camera settings ownership, and performance diagnostics.
- Native timeline tests cover 90 seconds at 30/60/90/120 Hz with varying capture
  costs, normal holds/skips, late-source bounded waiting, and refresh prediction.
- Nine analyzer tests include native phase drift, epoch resets, interpolation
  exclusion, CPU versus wall scopes, and threshold pass/fail exit status.
- macOS game build and Linux x86-64 cross-build succeed. The Windows producer
  translation unit cross-compiles; Windows runtime scheduling is unchanged and
  no Windows runtime benchmark was performed.
- Deck interpolation smoke replay completes with the existing interpolation
  policy. No native cadence percentage is assigned to its synthesized frames.
- Aitos 04/02 waterfall completes using the established Aitos save/replay and
  room-load timing. After warmup (ticks 2625–3500), native phase-fit
  mismatch is 0.00%, producer wall p99 is 8.11 ms, and
  present interval p99 is 12.69 ms. This is a coverage check, not a
  paired before/after waterfall benchmark.

Evidence is under `runs/frame-pacing-audit-2026-10-03/`: per-run CSV, log,
summary/binary hash, `analysis.json`, and combined `comparison.json`.
`run_audit.py` records diagnostic environment and creates isolated saves/settings.
The initial sandboxed Mac launch had no display and is excluded. An initial
unseeded Aitos direct warp hit the known terrain asset-load guard before action
streaming and is not a pacing result.

See [performance-overlay.md](performance-overlay.md#source-cadence-traces) for
trace commands, metric definitions and diagnostic overrides. No settings,
saves, or installed executable on the Deck were replaced by these tests.
