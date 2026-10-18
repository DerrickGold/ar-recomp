# PPU capture performance follow-up

The October 18, 2026 optimization reduces CPU work in full-add subscreen
capture without changing the recent painted-scenery, HUD, or sprite-priority
fixes. Its baseline is the working tree containing those correctness fixes,
not the preceding commit. GPU effect command batching remains opt-in; see
[its separate experiment](gpu-effect-command-batching.md).

## Change

Full-add capture must identify both the main-screen color-math eligibility
and the complete subscreen winner, including painted tiles and OBJ priority.
The optimized path applies painted edits to reusable scratch rows, merges
packed source rows in bulk, and exports one source at a time. Each export loop
keeps the same routing and attribute policy instead of selecting them again
for each winning pixel. Semantic priority tags stay with their source until
winner selection finishes. Authentic source rows remain untouched.

This is game-agnostic C in the shared `snesrecomp-go` PPU. It adds 3 KiB of
per-PPU scratch storage and no per-frame allocation, GPU pass, readback, or
backend dependency. A more complex chunked merge prototype offered little
additional benefit and was not retained.

## Measurements

Local evidence, scripts, source hashes, baseline copies and logs are in
`runs/ppu-capture-optimization-20261018/` (ignored). Deck gameplay uses Vulkan
on the OLED Deck's 1280x800, 90 Hz desktop display, 16:10, 64 extra rows,
and the same effects and authored INI data for both binaries. Only code and
test inputs were transferred; the tests use the already installed game assets.
The installed game and its settings were not replaced.

Ten valid gameplay runs completed, each about 44 seconds. Timings below cover
ticks 1600–2500, after loading and warmup. Marahna uses room 05:01 and a
stationary replay; Fillmore uses a scrolling 01:01 replay as a control.
The panel was verified awake throughout every accepted run. Every paired
comparison produced identical final WRAM hashes with no runtime diagnostics.

| Scene / presentation | Runs per binary | Producer mean, before → after | Completed-present p99, before → after |
| --- | ---: | ---: | ---: |
| Marahna, interpolation, 90 FPS cap | 2, ABBA order | 14.175 → 13.686 ms | 14.136 → 13.815 ms |
| Marahna, native, 90 FPS cap | 1 | 14.334 → 13.800 ms | 13.433 → 13.493 ms |
| Marahna, interpolation, VSync | 1 | 15.170 → 13.831 ms | 12.225 → 12.266 ms |
| Fillmore, interpolation, 90 FPS cap | 1 | 8.067 → 8.187 ms | 11.520 → 11.533 ms |

The repeated comparison uses the median of run means and of run p99s, not a
pooled percentile. It saves about 0.49 ms (3.4%) of producer work. The larger
VSync producer change is a single pair and should not be treated as the
expected gain. Fillmore's PPU scanout stayed at approximately 5.268 ms in both
versions; the control does not show a material presentation regression or a
speedup from this full-add optimization.

All accepted runs averaged approximately 90 host completions per second.
That does **not** mean every interval met the 11.11 ms deadline. Native and
VSync p99 did not improve materially. These are host completion measurements,
not GPU timestamps or physical display latency. The buffered producer runs
at the native 60 Hz rate, with a 16.67 ms budget; it overlaps presentation and
must not be added to presenter timings as if they were serial work.

The isolated boss-snapshot scanout benchmark confirms the CPU saving:

| CPU-only snapshot benchmark | Median before → after |
| --- | ---: |
| Mac, Apple clang, ABBA twice | 5.646 → 5.406 ms (4.3%) |
| Deck, Linux Release, core 3, ABBA three times | 10.968 → 10.646 ms (2.9%) |

These scanout measurements exclude game logic, uploads and presentation.
Each process warms up 20 frames and measures 200; the Deck also has two
initial warmup processes excluded from the table. No local build ran during
the final Mac timings.

Excluded attempts: initial gameplay tests ran with DPMS off, an attempted
direct warp to 05:03 failed the existing native asset-load boundary check on
the baseline, and the first cross-compiled microbenchmark omitted assert
setup under `NDEBUG`. None contributes to the tables. The final microbenchmark
keeps assertions enabled, and final gameplay rejects any sleeping-panel run.

## Correctness and portability

- Exact native/reference comparison of `snap_00_gf6653` passes across all ten
  capture planes, preserving the repaired boss pixels at the painted edge.
- Full-add regression coverage compares native and reference output across
  windows, main/subscreen ownership, multiple captured sources, missing
  priority surfaces, black backing and painted priority/transparent tiles.
  Authentic output and indexed GPU exports are also checked.
- PPU pipeline and sprite-ownership tests, plus the focused ASan/UBSan
  regression, pass.
- Mac and Linux Release builds pass. WASM builds with warnings as errors;
  the whole-room suite passes 150 regional rooms and 900 native/WASM surface
  and source comparisons, including receiver, camera and policy checks.
- Windows execution was not repeated for this change. No graphics API or
  shader interface changed.

## Next target

The substantial remaining cost is CPU scanout in full-add scenes. Steady
Marahna diagnostics after this optimization put it around 11.1–11.3 ms per
native frame, with emulation around 1.4 ms. Fresh-frame diorama upload
preparation costs about 1.5–1.6 ms on the presenter, while host draw work is
about 0.54 ms. These are overlapping/nested scopes, not additive frame totals.
Pipeline logs normalize by presents; per-call values require multiplying by
`frames / calls` before comparing native-frame work.

Full-add and winner-mask captures currently disable GPU tile decoding because
their ownership decisions require competing source pixels. A worthwhile next
prototype is a GPU winner-resolution path covering color math, painted
priority, windows, relocated OBJ and extended margins. Keep the CPU reference
as the exact oracle and compare moving scenes before enabling that path.
Separately, investigate fresh-frame upload preparation: upload-bearing
presentations have the larger tails, and the CPU capture saving alone did
not solve them. Neither proposal has a measured gain yet.
