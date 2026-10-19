# Early offscreen submission prototype

`AR_GPU_EARLY_SUBMIT=1` enables an opt-in experiment for software-paced streamed
presentation. Unset or `0` preserves the existing schedule. VSync, unlimited,
menus, synchronous/headless capture and the editor keep their existing paths.

## Scheduling change

The existing path records the final offscreen draws, waits for the software
deadline, submits those draws, then acquires and blits to the window swapchain.
The prototype submits the final draws before the deadline wait. The window
acquisition, blit and terminal submission still happen at the deadline.

This gives the GPU an opportunity to execute final drawing during an existing
host wait. It does not eliminate rendering work or introduce an additional
buffered frame. It changes no shaders, texture sizes, playout delay, interpolation
phase calculation, frames-in-flight limit, or readbacks. Earlier native GPU/SDL
ordering boundaries remain intact.

`PreparePresent` and `PresentPrepared` form an owner-thread pair. Only the
deadline wait may occur between them: no rendering, target changes, event pump
or resize. The adapter retains no command buffer or swapchain image across that
wait. Failed preparation does not proceed to window presentation or count as
a completed frame. Legacy adapters flush during preparation and retain their
ordinary presentation call. Hidden windows still submit a terminal command
buffer to retire earlier Vulkan resources.

The existing adaptive draw lead includes the early submission's CPU cost.
There is exactly one final offscreen submission per presentation; the prepared
path skips the late flush instead of submitting a second empty batch.

## Measurement

`AR_FRAME_PACING_TRACE` appends two fields:

- `offscreen_submit_ns`: CPU wall duration of early submission, already nested
  in `draw_work_ns` and included in the presenter CPU scope.
- `offscreen_complete_ns`: the CPU timestamp when that call returned, **not**
  GPU execution completion.

The analyzer reports `offscreen_overlap_window_ms`, the gap from that return to
final presentation entry. This is available overlap time, not measured GPU work.
`backend_flush_ns` is zero for prepared presents. Compare `draw_work_ns + swap_ns`
to account for moving work between scopes. Completed-present intervals and
source age still use the final backend return, not physical scanout.

## Validation and evidence

Protocol tests cover exactly one offscreen submission, no early window
acquisition, hidden/visible alternation, failed recording/submission/acquisition,
legacy adapters and trace boundaries. Real GPU tests compare 32 normal/prepared
frame pairs with changing uploads and alpha compositing, reading pixels only
after final submission. All pairs match exactly on Metal, DX12 and Vulkan.
The pacing analyzer's 17 tests and host pacing tests pass.

The reproducible Windows harness and traces live under
`runs/gpu-early-submit-20261019/windows/`. Both sides use the same Release
executable, changing only `AR_GPU_EARLY_SUBMIT`. Measurements use the existing
i9-9900K / GTX 1080 Ti Windows machine, 1920×1080 output, a 90 Hz software cap,
full environmental effects and ticks 1600–2500 after warmup. Steam Deck is
offline; these are relative Windows results, not Deck budget measurements.

The real GPU tests validate the submission boundary, not complete-level image
parity. Live replay runs separately check final game-memory hashes, backend
selection, actual prototype activation and zero synchronous motion waits.
Readback-based headless screenshot capture does not exercise this software-paced
streamed path and must not be presented as its visual parity evidence.

## Results and decision (2026-10-19)

Keep the experiment **off by default**. The valid foreground samples do not
establish a repeatable pacing improvement:

| Scene | Valid runs, off / on | Completed-present p99, off → on | Mean draw + backend, off → on | Mean source age at return, off → on |
| --- | --- | --- | --- | --- |
| Marahna 05:01, scrolling | 2 / 2 | 12.475 → 12.331 ms | 0.487 → 0.493 ms | 21.468 → 21.487 ms |
| Fillmore 01:01, scrolling | 1 / 1 | 12.466 → 12.506 ms | 0.854 → 0.851 ms | 21.721 → 21.688 ms |

Repeated values are medians of per-run statistics, not pooled percentiles.
Marahna used off/on/on/off ordering. Its two baseline p99 values were 12.577
and 12.374 ms: that baseline spread exceeds the 0.144 ms median change. Fillmore
has only one valid pair and moves slightly in the other direction. Neither
scene demonstrates a meaningful reduction in total host work.

The early-submission opportunity averages 0.404 ms in Marahna and 0.455 ms in
Fillmore. The early submission itself averages 0.029 / 0.025 ms, respectively,
and replaces the late offscreen flush. All eligible on-rows have an early
timestamp and zero late flush. Source age remains essentially unchanged, with
no synchronous motion readback waits.

Fourteen Vulkan replays completed with matching final WRAM hashes within each
room (Marahna, Fillmore and Bloodpool). Only six are eligible for the table.
The remaining eight lost foreground focus and are excluded from performance
conclusions. The initial harness checked focus at startup; retry harnesses
also sampled it every second and rejected runs when Windows did not activate
the game. Both standard `SetForegroundWindow` and application-activation
retries were denied. No synthetic key input or system policy changes were
used. The broader foreground matrix, native-mode timing control and VSync
control remain uncompleted. DX12 is covered by the real GPU correctness test,
not by a new game pacing comparison.

`baseline.json` and `source-manifest.json` identify the checkout, changed sources
and tested executables. `windows/summary.json` contains only foreground-eligible
timings; `windows/analysis.json` retains the excluded runs with their validation
metadata. `pilot.json.results.json`, `extended.json.results.json`, and the two
retry result files preserve the distinction between replay success and timing
eligibility. The unexecuted `controls.json` is a planned follow-up, not evidence.

The next architectural experiment should remove interpolation atlas-to-plane
unpack copies, reducing actual memory traffic and dispatch work. This scheduling
experiment can be remeasured on Deck when available, but the Windows evidence
does not justify making it the default or claiming a 90 Hz budget improvement.
