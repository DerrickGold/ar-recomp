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
- The initial CPU-only experiment did not include Windows execution. Native
  Windows validation and timings were added in the GPU follow-up below.

## Next target identified after the CPU pass

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
not solve them. The follow-up below measures the first proposal; upload preparation remains a separate target.


## GPU full-add capture (2026-10-18)

The prior work is committed as `0e50e060`. Eligible full-add capture is now
enabled by default with the normal `AR_GPU_BG_CAPTURE=owned` path, after the
rollout checks below. `AR_GPU_BG_FULL_ADD=0` retains the previous CPU full-add
path without disabling ordinary GPU capture. Unset, empty, `auto`, and `1`
enable the extension; other values disable it. Pixel/reference capture modes
do not request GPU ownership, and `tiles-validate` keeps CPU pixels as an oracle.
The extension
the existing GPU tile decoder to select the background subscreen winner on
full-add rows. This moves bitplane expansion, palette lookup and competing
background priority comparisons into the existing GPU decode pass. It adds
no render pass, synchronous readback, ray-tracing dependency, or game-specific
runtime rule.

CPU scanout records the non-background main/subscreen winners and keeps OBJ
pixels/attributes. It samples competing BG keys only where sprites need an
ownership decision, including the late apron pass. Native priority and
presentation bands remain separate. Painted priority, black and transparent
edits participate in both CPU and shader decisions. Changed OBJ context
invalidates both dependent BG surfaces and any skybox alias. Fragment and
compute shaders share one decoder source.

The initial eligibility check unnecessarily required a far-band surface for
BG2; Marahna does not bind one. The packet writer already maps unbound semantic
bands to the primary surface. That restriction was removed and covered by
regression tests. Partial windows, unsupported capture masks/formats,
mismatched geometry and authentic comparison captures retain CPU ownership.
A provider discovering a native VRAM patch midway through a row triggers a
bounded CPU retry for both competing backgrounds. GPU ownership is never
inferred merely from the environment flag: accepted runs log `owned=3` and
`full-add-rows=352`.

### Steam Deck results

Same awake-panel, 1280×800, 90 FPS cap and all-effects protocol as above;
measurement window is native ticks 1600–2500, with no epoch boundaries.
Marahna uses **the same executable**, alternating full-add off/on/on/off.
The Fillmore moving control compares the committed executable against the
prototype, including the changed decoder on ordinary non-full-add rooms.

| Case | Pairs | Producer mean, before → after | Host presentation p99, before → after |
| --- | ---: | ---: | ---: |
| Marahna 05:01, interpolated, ABBA | 2 | 13.658 → 8.182 ms | 13.885 → 11.464 ms |
| Marahna 05:01, active sprites / fixed camera | 1 | 13.518 → 8.372 ms | 13.637 → 11.436 ms |
| Marahna 05:01, native frames | 1 | 13.579 → 8.003 ms | 13.592 → 11.391 ms |
| Fillmore 01:01, moving control | 1 | 8.184 → 8.324 ms | 11.577 → 11.512 ms |

ABBA values are medians of run means/p99s, not pooled percentiles. The repeated
Marahna producer saving is **40.1%**. Its off runs had 193 and 197 measured host
intervals above 12.11 ms; both on runs had **zero**. The on-run maxima were
11.670 and 11.789 ms. All runs averaged approximately 90 host completions per
second and paired runs ended with identical WRAM hashes. Native-frame tests
skipped no source ticks; their fitted cadence mismatch was 1/1349 before and
3/1349 after, so this is not a claim of a new cadence algorithm or perfection.

The producer still runs at 60 Hz and overlaps presentation. These scopes must
not be added, and its budget is 16.67 ms. Host p99 remains slightly above the
11.11 ms display period. These are host timestamps, not GPU execution times
or panel latency; the experiment does not establish every-room/Game Mode
90 Hz delivery. The single Fillmore pair shows a 0.14 ms producer increase
with no presentation regression; repeat ordinary-room controls before
promoting the option to a default.

Fresh-frame upload preparation is still a useful next target. In the repeated
interpolated comparison, median upload-call means were 1.758 → 1.720 ms and
host draw means 0.557 → 0.587 ms. The benefit comes primarily from removing
CPU scanout work, rather than eliminating presentation-side preparation.

### Native Windows results

The same prototype was cross-built as a native Windows x86-64 executable and
run through `ssh windows` on Windows 10 Pro (19045), i9-9900K and GTX 1080 Ti
(driver 576.28), using SDL 3.4.18. SSH enters WSL, but the tests launch Windows
executables through PowerShell, with confirmed Direct3D 12 and Windows Vulkan
backends. The isolated deployment is
`C:\Users\Derrick\AppData\Local\Temp\ar-full-add-20261018`.
Existing Windows ROM/media are reused locally; the installed game is untouched.

The protocol uses a 1920×1080 window, 16:9 square pixels, 64 extra rows,
all environmental/post effects except CRT, dummy audio and a 90 FPS host cap.
Ticks 1600–2500 provide 901 producer samples and approximately 15 seconds of
steady-state data per run after warmup, with no epoch boundaries. The desktop
is 3840×2160 at **60 Hz**. The launcher could not acquire foreground focus;
that condition is consistent across cases. These results measure native
Windows host work/pacing, not physical 90 Hz presentation or display latency.
Windows thread-CPU counters are too coarsely quantized here; the reported
producer and upload durations are wall-clock scopes.

| Marahna 05:01 case | Pairs | Producer mean, off → on | Host presentation p99, off → on |
| --- | ---: | ---: | ---: |
| DX12, interpolated, ABBA | 2 | 12.204 → 8.299 ms | 14.106 → 12.103 ms |
| Vulkan, interpolated, ABBA | 2 | 12.522 → 8.473 ms | 14.519 → 12.356 ms |
| DX12, native frames | 1 | 12.086 → 8.281 ms | 14.131 → 12.124 ms |
| Vulkan, native frames | 1 | 12.545 → 8.472 ms | 14.518 → 12.308 ms |

The repeated comparisons use medians of run means/p99s. Producer savings are
**32.0% on DX12 and 32.3% on Vulkan**. Every GPU-on diagnostic confirms both
BGs GPU-owned and all 352 full-add rows active, with no rejected sources or
mismatches. Paired runs and cross-backend runs have identical final WRAM.
The native-frame pairs skip no source ticks; fitted cadence mismatch changes
from 0/1349 to 6/1349 on DX12 and 3/1349 to 10/1349 on Vulkan. This is a producer
and interval-tail improvement, not a claim of perfect native presentation cadence.

Upload-call means increase from 1.779 to 2.017 ms on DX12 and 1.888 to 2.156 ms
on Vulkan, consistent with the additional full-add packet data. Mean host draw
work increases only about 0.01 ms. P99 remains above the 11.11 ms host period,
and isolated longer intervals remain. Upload preparation and remaining pacing
jitter are still useful targets; the producer saving must not be added to or
subtracted from parallel presenter scopes as a serial frame total.

A separate build of committed `0e50e060`, with the same generated game code,
compiler/flags and test data, checks ordinary-room overhead in scrolling
Fillmore 01:01. Each backend has one baseline/prototype pair:

| Fillmore control | Producer mean, committed → prototype | Host presentation p99, committed → prototype |
| --- | ---: | ---: |
| DX12 | 6.981 → 7.077 ms | 12.091 → 12.124 ms |
| Vulkan | 7.232 → 7.310 ms | 12.616 → 12.504 ms |

The small 0.08–0.10 ms producer increase does not produce a material
presentation regression in these pairs. Both versions reach identical WRAM.
Additional same-prototype full-add flag off/on Fillmore runs agree closely;
they are flag-scope checks, not substitutes for the committed-build control.
In total, seven native Windows regression checks, two live pixel-validation
replays and eighteen timing runs pass. No Windows-specific implementation
change was required.

Windows scripts, source/binary hashes, machine metadata, per-case environments,
logs, traces and analyses are under `runs/gpu-full-add-20261018/windows/`.
`package/run-tests.ps1`, `validation.json`, `matrix.json`, `controls.json` and
`package/run-perf.ps1` reproduce the test cases from the isolated Windows
folder. `summarize.py` analyzes the collected traces using the repository's
frame-pacing analyzer. The live validation route has the same Marahna
fixed-camera limitation described below; it does not close the scrolling
full-add walkthrough gate.

### Validation and reproduction

- Stationary and scripted-input Marahna 05:01 live validation replays pass
  without background mismatches or rejected sources; all 352 full-add rows
  are active. Camera-proof snapshots show the scripted route reaches an
  obstacle at player X=216: BG cameras remain (98,287). Treat `moving-*` run
  names as historical labels, not evidence of scrolling. The active-sprite
  result tests changing OBJ competition, not a full scrolling walkthrough.
- Real boss snapshot `snap_00_gf6653`: exact agreement across all ten capture
  planes for native, reference, GPU-owned materialization and validation
  modes; the same check passes ASan/UBSan.
- Twelve full-add policy scenarios exercise windows, both-screen ownership,
  black backing, absent bands, painted edits and mid-row provider fallback.
  PPU pipeline, sprite ownership and frame queue tests pass.
- Explicit fragment and compute decode tests pass on Metal and Deck Vulkan,
  including changing palettes, padding, OBJ-dependent invalidation, full-add
  skybox aliases, malformed offsets and resource cycling. The ordinary GPU
  CTest now explicitly selects `fragment`; previously both named tests could
  select the default compute backend.
- Mac/Linux/Windows Release builds and generated MSL/SPIR-V/DXIL compile.
  Native Windows DX12 and Vulkan pass explicit fragment and compute pixel
  parity, plus the PPU pipeline, sprite ownership and frame queue tests.
  Separate live Marahna validation replays on both Windows backends report
  all 352 full-add rows, no rejected sources or mismatches, and identical
  final WRAM. Validation compares the packet decoder against CPU scanout;
  the GPU tests separately compare shader output against that decoder.
- The shared editor builds with warnings as errors and passes 150 regional
  rooms / 900 native-WASM surface and source comparisons.

Evidence and replay plans are under `runs/gpu-full-add-20261018/`; Deck logs,
CSV traces, analyses and telemetry are copied into its `deck/` subdirectory.
`matrix.json` and `moving-perf.json`, with `run-perf.py`, reproduce the ten pacing runs in the isolated
`/tmp/ar-gpu-full-add-20261018` Deck folder. The separate `camera-proof.json` plan records positions without contributing
timings. Existing Deck ROM/media assets are referenced locally; none was transferred. `pilot-validate-v5` and `moving-validate` are pixel
validation replays, not performance samples. Earlier pilots did not activate
the full-add path and are excluded. Validation mode duplicates CPU work and
must never be included in the timing table.

### Default rollout validation

The previously open scrolling and room-coverage gates are closed. No further
renderer or shader correction was needed during this validation. Eligibility
has not been widened; unsupported policies and the DeathHeim hub's native
post-scanout sprite promotion retain their existing CPU fallback.

- **Moving camera, source frames:** a revised Marahna 05:01 route moves both
  BG cameras **1,024 pixels horizontally and 147 vertically**. All **951
  consecutive final composites** (game frames 950–1900) are byte-identical
  between full-add off/on on Metal. Repeating with the rebuilt default and
  `AR_GPU_BG_FULL_ADD` absent also matches all 951 frames. Final WRAM matches.
- **Interpolation:** all **191** midpoint samples along that route match
  exactly, including **182 GPU-generated frames**; candidate plane masks,
  camera positions and final WRAM also match. The route uses existing
  moonjump/invulnerability cheats to clear obstacles. It is an assisted
  rendering traversal, not a gameplay/physics or complete-level walkthrough.
- **Regional/policy coverage:** all **150 exported room states** (US, JP and
  EU terrain; 147 unique room/profile IDs plus three alternate DeathHeim hub
  states) pass **1,800 camera/configuration cases / 9,000 scanouts**. Each case
  compares the reference pixel renderer, native scanout, ordinary packets,
  full-add ownership and full-add validation, including the six BG bands,
  backdrop, independent skybox and requested native pages. Both built-in and
  authored `diorama-layers.ini` policies are covered, with subtile scroll,
  animated raster clocks, camera edges, native/extended views and BG aprons.
  The 108 full-add cases cover Marahna 05:01–03 in all regions; 558 cases also
  exercise partial or complete CPU fallback. Every compared pixel matches.
- **Regression controls:** policy defaults/overrides, PPU pipeline, sprite
  ownership, frame queue and explicit Metal fragment/compute tests pass.
  Prior Deck Vulkan and native Windows DX12/Vulkan fragment/compute results,
  boss snapshot sanitizer checks and native/WASM comparisons remain applicable
  to the unchanged decoder. Release builds succeed for Mac, Deck and Windows.
- **Default on real backends:** separate scrolling `tiles-validate` runs on
  Deck Vulkan and native Windows DX12/Vulkan pass with the full-add flag
  **unset**, no mismatches/rejected sources and all 352 rows active. Deck camera
  dumps independently confirm movement on both axes. Normal default runs
  confirm `owned=3`, including Deck with interpolation disabled. All eight
  final remote runs have identical WRAM, including the same-build opt-out.

The final Deck scrolling pair uses the rebuilt executable, the same assisted
route and the established 90 Hz desktop protocol. Producer mean improves
**13.612 → 8.660 ms (36.4%)** and host presentation p99 **14.561 → 11.479 ms**.
The default native-frame run records **8.365 ms** producer mean and **11.412 ms**
presentation p99, no skipped source ticks, and two fitted cadence mismatches
out of 1,349 samples. Default Windows smoke runs record **8.413/8.654 ms**
producer mean and **12.163/12.407 ms** presentation p99 for DX12/Vulkan.
These final Windows runs acquired foreground focus; they verify the default
but are not a controlled timing comparison against the earlier unfocused runs.
All timing windows cover ticks 1600–2500 with no epoch changes. Validation
runs duplicate CPU work and are excluded. The improvement is substantial,
but p99 remains above the **11.11 ms** 90 Hz interval; this does not close the
remaining pacing work.

The room exporter now preserves duplicate catalogue states instead of silently
overwriting the first DeathHeim hub state. `actraiser_action_room_capture_test`
uses the real editor planner, interposing only its scanout call in a separate
test object; no test hooks were added to the runtime. Reproduce with:

```sh
cmake --build build-tests --target actraiser_action_room_capture_test
node tools/action_editor/export_gpu_rooms.mjs build/action-editor/ar-action-layer-editor.html /tmp/ar-room-check-us 0
build-tests/actraiser_action_room_capture_test diorama-layers.ini /tmp/ar-room-check-us/*.arscene
```

Repeat the export/check with terrain profiles 1 and 2. The composite harness
now supports `--keep-replay`, `--full-add`, `--no-compare-binding`,
`--require-full-add` and `--minimum-camera-travel`; a stuck route or inactive
capture path must not count as scrolling validation. Exact commands, input and
binary hashes, camera states and image hashes are retained in each capture's
`provenance.json` and `capture.json` under `runs/gpu-full-add-20261018/`.
`scroll-comparison`, `scroll-mid-comparison`, `scroll-default-comparison`,
`room-capture-matrix.log`, `room-capture-hub-variants.log` and
`rollout-manifest.json` hold the rollout evidence.
`rollout-deck.json` and `windows/rollout.json` reproduce the final remote runs;
their retrieved logs/traces, `rollout-results.json` and
`rollout-camera-proof.json` preserve default activation and timing evidence.

Do not widen eligibility without reference comparisons for the new policy.

## Packet preparation and pacing evidence (2026-10-19)

The next measured cost was presenter-side packet preparation. New nested host
scopes separate validation, content comparison, transfer-buffer writes, pending
renderer submission, retained-packet copies and coverage generation. Validation
costs only about 0.002–0.005 ms per resolve in the Windows samples, so its bounds
and offset checks remain intact. These scopes overlap their parent scopes and
are not GPU execution measurements.

Three exact, portable changes are enabled by default:

- Compare a shared palette again only when either the current or previous
  scanline's palette offset changes. Values are still compared across packets;
  equal offsets alone never establish equality.
- Retain the existing CPU packet copy when every source remains equal. Changes
  to unrequested sources still invalidate their textures and update the copy.
  Retained bytes and their offsets stay together when equal data moves within
  the incoming packet arena.
- Compare integer payloads with bounded 16-word equality reductions and a short
  tail. The Windows cross-toolchain's linked variable-length `memcmp` was a
  byte-at-a-time loop. The replacement permits compiler vectorization without
  platform intrinsics, stronger alignment assumptions, hashes or overreads.

`AR_GPU_BG_PACKET_CACHE=0` selects the previous comparison/copy behavior for
same-binary A/B tests; unset or `1` enables the optimization. This does not add
render passes, readbacks, allocations or buffering. Transfer memory remains
write-only; the comparison snapshot lives in ordinary CPU memory.

### Relative Windows Vulkan comparison

The Deck was offline. Native Windows Vulkan was tested on the i9-9900K / GTX
1080 Ti machine above, using a separate temporary deployment. The final matrix
has twelve runs: off/on/on/off for each of scrolling Marahna 05:01, scrolling
Fillmore 01:01 and stationary animated Bloodpool 02:01. All use the same
executable, acquired foreground focus, 1920×1080, 64 extra rows, interpolation,
environmental effects and post-processing except CRT. The 90 FPS host cap runs
on a 60 Hz desktop: this is a relative work/compatibility comparison, not proof
of physical 90 Hz delivery or a prediction of Deck timings.

Ticks 1600–2500 supply 901 producer samples per run after warmup. Values below
are medians of the two run means or p99s per mode, not pooled percentiles.
“Preparation” is the existing host upload-call scope, including CPU work before
GPU submission, not PCIe transfer time.

| Room | Preparation mean, off → on | Reduction | Preparation p99, off → on | Completed-present p99, off → on |
| --- | ---: | ---: | ---: | ---: |
| Marahna 05:01 | 2.137 → 1.790 ms | 16.2% | 3.381 → 2.146 ms | 12.424 → 12.380 ms |
| Fillmore 01:01 | 2.008 → 1.763 ms | 12.2% | 2.936 → 2.163 ms | 12.478 → 12.508 ms |
| Bloodpool 02:01 | 2.999 → 2.541 ms | 15.2% | 3.803 → 3.053 ms | 12.463 → 12.464 ms |

Packet comparison itself falls by approximately 79–82% in the settled overlay
windows. Equal-packet reuse eliminates about 24% of retained copies in Fillmore
and fewer in the continuously changing other rooms. Overlay subscopes use the
last ten settled reporting windows, normalized by call counts; their windows
are not identical to the CSV tick range and should not be summed into that
table. Transfer-write and draw costs remain essentially unchanged. Paired final
WRAM matches, and no synchronous motion readback is introduced.

Keep this as preparation headroom: mean preparation improves 12–16% and its
p99 improves 20–37%, but completed-present p99 does not materially improve.
Producer work and source age remain broadly unchanged. A Mac word-comparison
microbenchmark also favors the block implementation, but is not a whole-game
timing result. Deck performance still requires a fresh on-device comparison.

### Correctness and remaining pacing work

All 553 sampled final composites match exactly with the optimization off/on:
191 source and 191 interpolated Marahna frames, plus 171 interpolated Fillmore
frames with forest effects. The Marahna camera travels 1,024 pixels horizontally
and 147 vertically; Fillmore BG1 travels 1,064 and 188 pixels, with independently
scrolling BG2. These are assisted rendering traversals, not complete-level
gameplay tests.

Metal fragment/compute tests and ASan/UBSan checks pass. Native Windows passes
all eight DX12/Vulkan × fragment/compute × off/on pixel checks. Expanded cases
exercise shared and relocated palettes, changes to either compared offset,
unrequested sources, comparison-block boundaries and tails. Mac/Windows Release
builds, performance-metrics tests, 16 pacing-tool tests and 19 pipeline-tool
tests pass. Shaders, packet layout and editor/WASM code are unchanged by this
optimization.

The pacing analyzer now accounts for uploads staged during idle loops between
two presentations, rather than attributing only the final loop's upload. It
also decomposes completion spacing into submission spacing plus the change in
backend duration. Reanalysis of the existing Deck default trace yields p99
11.505 ms for intervals with uploads and 11.307 ms for intervals without them.
Its longest interval includes an earlier 1.967 ms staged upload that the
same-loop field alone misses; another long interval has no intervening upload.
This is reanalysis of prior evidence, not a new Deck run.

Windows tails likewise include both late submissions and ordinary submission
spacing followed by longer host backend calls. Presentation/submission
variability remains the next investigation; these host scopes cannot establish
a specific GPU shader, driver or compositor cause. No scheduling policy is
changed in this pass.

Evidence is under `runs/bg-packet-preparation-20261019/`. `source-manifest.json`
records the production sources and binaries; `windows/blocks.json` and
`windows/run-perf.ps1` reproduce the final matrix, with results in
`windows/blocks.json.results.json`, `windows/summary.json` and per-run traces.
Earlier profile and cache-only trials are retained separately and excluded from
the final table. `windows/tests/results.json`, the three `comparison.json`
files, sanitizer logs and `deck-existing-trace-reanalysis.json` retain validation.
Initial captures rejected for an inapplicable resident-effect check or too few
replay ticks are excluded; the corrected captures satisfy the original pixel,
camera-travel and frame-count requirements.
