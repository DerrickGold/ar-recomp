# Additional CPU preparation optimizations — 2026-09-11

Follow-up to `pipeline-performance-audit.md`. These changes do not reduce
graphics quality, disable effects, alter emulation timing or change runner ABI.

## Town terrain rows

Scene rebuilds classify 16-pixel chunks by uniform/mixed replacement and alpha
ownership. Uniform rows compare contiguous pixels directly. Mixed masks retain
the original per-pixel decisions. Dirty bounds and changed-pixel counts stay
exact; clean mountain scratch is still refreshed from raw terrain art.

The application supplies an optional synchronous row dispatcher. The owner
prepares river metatiles and any fallback cell indices before dispatch. The
callback receives explicit immutable inputs and separate output rows; it has
no WRAM pointer, renderer, source-cache access or global publication work.
Counters are row-local and reduced after joining. Mountain source refresh,
dirty publication and serial updates remain on the owner.

The existing scalar API runs the identical kernel. The application lazily owns
a group of up to three helpers, capped by available cores and the existing
`AR_RENDER_WORKERS=0..3` override. Allocation/setup failure uses scalar work.
Idle helpers sleep, dispatch allocates nothing, shutdown joins and releases
the group. Producer and presentation groups never run jobs simultaneously.
The domain module does not import SDL or the host pool. This is application
composition, not a new runner or render-device ABI entry.

The chunk plan adds 48 KiB of fixed storage; prepared work uses bounded stack
storage only for the duration of the synchronous call.

## Exact upload bounds

The upload mirror checks whole rows as before, but only searches for horizontal
changes outside the bounds already found. Full-width changes need no further
horizontal searches. Portable 32-byte `memcmp` blocks replace byte-at-a-time
edge scans, with byte tails retaining arbitrary alignment/pitch support. No
extra cache or allocation is introduced; dirty rectangles and upload contents
are unchanged. An independent byte oracle covers 3,000 generated cases,
including unaligned pointers, unequal pitches, padding and clean/sparse/dense
changes.

An isolated 512×512 scan harness ran 20,000 iterations per pattern, ABBAABBA.
Median clean/small/large/distributed-change times in microseconds were
28.576/33.475/87.390/12.361 before and 28.564/28.725/18.450/1.681 after. Output
hashes matched in every run. These are kernel measurements, not FPS estimates;
the original town-only aggregate comparison was below 1% and inconclusive.

## Navigation town capture

Navigation and Palace now reuse their classified six-town scene while its
complete inputs are unchanged: all six paged cell maps, development words and
structure records. Inputs are retained by value, not by WRAM pointer/address.
Comparing all four record bytes conservatively invalidates action-only changes
instead of introducing a second interpretation of record ownership.

Each frame still receives a complete scene value. The cache is owner-only,
bounded below 192 KiB, invalidated by metadata reset/null input, and uses exact
input comparison rather than a collision-prone hash. The uncached classifier
remains the independent reference. All six towns, quadrant boundaries, first/
last record slots, development gating and null/reset cases have regression
coverage. Windmill animation remains a presentation-time input.

## Measurements

Matched `-O2` binaries share every non-candidate link input, including concurrent
audio work; link inputs were hashed before/after. Each comparison is ABBAABBA,
four runs per variant, no competing local build/test/replay. Measurements are
frame-weighted averages of the last five matching scene windows, then medians
across runs. Paired variants use identical effects/quality, with town clouds
enabled. The actual hidden Mac GPU compositor
still runs near 120 Hz despite Unlimited policy.

**These are CPU wall times, not GPU timestamps, uncapped FPS or Deck estimates.**
Render CPU sums non-overlapping PPU, map, metadata, town canvas, snapshot,
upload and presentation stages. It excludes emulation and explicit waits.

| Comparison | Scope | Before, ms | After, ms |
| --- | --- | ---: | ---: |
| Town: original row sources → uniform/parallel rows | Render CPU | 2.8228 | 2.6616 |
| Same town comparison | Canvas enhancement | 0.5022 | 0.3005 |
| Navigation: uncached → cached town scene | Render CPU | 2.8869 | 2.8477 |
| Same navigation comparison | SIM metadata | 0.11032 | 0.02330 |
| Palace: uncached → cached town scene | Render CPU | 2.9865 | 2.9881 |
| Same Palace comparison | SIM metadata | 0.09315 | 0.00894 |
| Action: original → block/bounded upload scan | Render CPU | 2.3408 | 2.0917 |
| Same action comparison | Upload | 0.38800 | 0.24067 |

The town comparison shows **5.7% less render CPU** and **40.2% less canvas
enhancement time**. The navigation comparison shows **1.4% less render CPU**.
Palace metadata is substantially cheaper, but unrelated PPU/presentation
variation offsets it in the whole-render median: **no aggregate Palace gain
is claimed**. These increments must not be added to earlier percentages.

Action upload time fell **38.0%**, with disjoint per-run ranges
(0.3491–0.4282 versus 0.2321–0.2704 ms). Its whole-render median fell 10.6%, but
unrelated scanout variation also contributes; the upload-stage change is the
stronger attribution. Draw content and uploaded regions remain identical.

A bounded exact projected-vertex cache for town models was tested and removed:
eight runs gave 2.9942 → 2.9879 ms render CPU (~0.2%). Existing held projection
caching already eliminates much of its opportunity. Removing that experiment
does not remove any multicore path.

The reusable comparison command also ran eight same-binary 0-versus-3-helper
town replays. Canvas enhancement fell **0.42758 → 0.30188 ms (29.4%)**; the
owner's row work fell 0.19359 → 0.06423 ms, with 0.01218 ms median join wait.
Whole-render medians were 2.8470 → 2.5025 ms, but substantial unrelated PPU
variation contributes to that difference, so it must not all be attributed to
the helpers. This test verifies that the multicore path remains useful and
that the serial fallback is exercised, not a universal core-scaling ratio.

## Validation

All 162 application CTests passed, including the complete town pixel fixture
suite in normal serial order, reverse/uneven row ranges and real host helpers.
Five focused address/undefined-behavior sanitizer tests passed (including both
dispatch variants, navigation metadata and upload scanning). SDL/driver leak
checking was disabled. A separately instrumented ThreadSanitizer run of the
real threaded town pixel suite also passed.

The whole-game screenshot matrix produced **76 byte-identical comparisons**
against the matched control: nine each for town, navigation, Palace and action,
plus two native/menu frames, each compared with zero and three helpers. Cloud
drift was frozen only for image comparisons; timing runs retained animation.
Image readback was not concurrent with performance measurements. Actual Metal
composites, not only synthetic renderer tests, were compared.

The portable-render boundary inventory now explicitly includes the voxel
builder and its dispatch header, with negative dependency probes. No source in
the runner ABI/runtime was changed for these optimizations. Concurrent audio
changes in this working tree remain separate.

## Repeatability and remaining boundaries

`tools/compare_pipeline_performance.py` makes the repeated replay procedure
reusable, with isolated fixture copies, input hashes, sample validation and
per-stage/range JSON output. Its ROM-free tests cover weighting, nested scopes,
scene selection, settled tails and missing evidence. See `performance-overlay.md`
for invocation and helper-scaling comparisons.

No claim is made that all possible optimizations are exhausted. GPU model-space
projection/instancing and globe weather-coordinate evaluation could remove more
CPU work, but require explicit immutable vertex inputs and portable backend
contracts. They are not safe drop-in async-compute switches: depth, bridge
contact, UV seams, clipping and pixel rounding need parity validation. The new
overlay and repeated runner provide evidence for those larger changes without
asking someone to manually reproduce every test scene.

Temporary evidence: `/private/tmp/actraiser-finish-opt.R0SKMq/` contains matched
sources/binaries, per-run logs/JSON, visual comparisons and sanitizer/test logs.
Temporary evidence may be cleaned by the host; regression tests and the
comparison command remain in the repository.
