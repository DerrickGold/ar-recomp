# Conditional upload-bounds shortcut — 2026-09-12

Status: shared exact change-detection path, no toggle, new allocation, worker
restriction, runner ABI or graphics-quality change. This is a small follow-up
to the GPU cloud work, not a claim of another large whole-frame speedup.

Once the accumulated dirty rectangle spans the full width, the scan needs only
the final changed row. It searches upward from the bottom and skips the already
enclosed interior. Narrow/sparse changes retain forward whole-row comparisons
and bounded edge refinements. Pixel alignment, arbitrary pointer alignment,
unequal pitches, ignored padding, texture/destination identity, upload failure
invalidation and grow-only mirror storage remain unchanged.

An initial unconditional top/bottom scan was **rejected**: it sped wide changes
but slowed a localized 512x512 update from 29.12 to 34.64 microseconds. The final
conditional version preserves streaming locality for that case. No experiment
branch or unconditional version remains in the application.

`Scan MiB` now counts requested comparison operands (both inputs, including
edge refinements), excluding skipped rows. It is **not measured memory/DRAM
traffic**: libc can exit early and the CPU can serve cached lines. The old
counter charged the entire image pair and omitted refinements. This semantic
correction is documented in the overlay guide; do not call old/new counter
ratios measured memory-bandwidth savings.

## Measurements

Six 512x512 kernel patterns, 100 warmups and 20,000 iterations each, serial
ABBAABBA, four runs per binary, `-O2`. All output hashes match. Final candidate
includes the corrected counters. Median microseconds:

| Pattern | Control | Conditional bounds |
| --- | ---: | ---: |
| Clean | 28.912 | 29.348 |
| Small localized change | 29.084 | 29.193 |
| Large partial-width change | 18.942 | 19.086 |
| Distributed full-width changes | 1.737 | .029 |
| Two distant narrow changes | 29.352 | 28.958 |
| Two distant full-width changes | 29.028 | 13.442 |

Tiny/clean-case differences are retained, not discarded. The broad-change
improvement is specific to the shortcut, not a general claim that every scan
is faster.

Two full-game local Metal ABBAABBA cohorts use pinned release binaries, Quality,
three helpers, 1,800 ticks, identical settings and no own overlapping work.
Settled windows are frame-weighted and then run medians compared:

| Scene | Render CPU control, ms [range] | Candidate, ms [range] | Upload scope control → candidate |
| --- | --- | --- | --- |
| Palace | 2.584 [2.530–2.615] | 2.507 [2.360–2.580] | .247 → .237 ms |
| Town, clouds on | 2.692 [2.648–2.700] | 2.672 [2.638–2.691] | .425 → .421 ms |

The apparent aggregate Palace reduction is 3%, but much of the difference is in
unrelated PPU work; it is **not attributed wholly to this optimization**. Town
aggregate is below 1% with overlapping ranges. Cadence remains approximately
120 Hz. This is a diminishing-return stopping point for this particular scan
refinement, not evidence that larger world animation/atmosphere costs are done.
No Deck percentage is claimed or extra Deck timing required for this small
portable change.

## Validation and evidence

All 165 local CTests pass. The independent byte oracle now covers 3,000 cases
up to 97x73 with arbitrary alignment, unequal pitches, padding and clean/sparse/
dense patterns. A deterministic full-width test proves that the final dirty
row is not skipped and that scan counters exclude the interior. Existing
render-device tests cover texture identity, upload contents and failure retries.
Nine full-game town composites are byte-identical; final WRAM matches in the
visual comparison and every timed cohort.

Scratch: `/private/tmp/actraiser-upload-bounds.iFUqeB/`. All three kernel cohorts
(including the rejected variant), per-run full-game logs, captures and tests
are retained. Reproduction uses `tools/compare_pipeline_performance.py` with
the cloud work's Palace inputs; town uses the repository D7-voxel-town fixture
with `AR_SIM3D_CLOUDS=on` and `AR_REPLAY_NOSTOP=1`.
