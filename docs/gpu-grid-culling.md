# Coarse GPU land-grid culling experiment — 2026-09-12

Status: **enabled by default within the now-default GPU land/cliff path**, as requested
after reviewing the holistic tradeoff. `AR_SIM3D_WORLD_GPU_GRID=0` opts out of that path;
set `AR_SIM3D_WORLD_GPU_GRID_CULL=0` to keep the full GPU source.
The complete retained-surface compatibility renderer and multicore support remain available.

The experiment removes 18–28% of submitted vertices in the tested views with
exact images, but **does not demonstrate an FPS or CPU-throughput improvement**.
The Deck's repeated Palace CPU median is about 1% slower. The default accepts
that small measured wall-time difference for less GPU geometry work at identical
quality. It is not a claim of improved GPU elapsed time, energy use or throughput.

## Ownership, bounds and submission

- Presentation packs the immutable land source into 64 chunks of 16x16 cells.
  Each chunk bounds all emitted source directions and base elevations, including
  raised terrain. Registered cliff replacement cells remain excluded. Geography,
  relief and source changes rebuild these bounds with the source.
- Six-plane interval tests reject only wholly out-of-frustum chunks. Double
  accumulation and conservative float-operation padding cover the shader's
  reference-radius cancellation. Nonfinite, overflowing or uncertain bounds
  retain geometry. This is not approximate mountain-height testing or horizon
  occlusion, and it never clips a partly visible primitive on the CPU.
- Visibility is reconsidered only when the copied radial transform/source
  changes. Adjacent surviving source ranges merge, preserving their order.
  The backend copies at most 64 ranges and knows nothing about towns, settings,
  camera policy, source revisions or the game clock.
- `Sim3DDepthPass_SelectSurfaceMesh` compacts instances **on the GPU** only when
  the range selection changes. Source uploads precede the ordered copy pass;
  only its first destination copy cycles the buffer. Ground, shadows, blur and
  haze bind the same compact stream. There is still one draw per material,
  not one draw per chunk. Shader blobs and vertex layout are unchanged.
- Range updates validate before changing state; invalid ranges, count overflow,
  allocation failure and queued mutation retain the previous selection. Source
  updates restore the full source. One zero-length range selects nothing;
  NULL/zero restores everything. Reset invalidates source and selection storage.
- Optional selection failure restores the full source and latches off culling,
  not the entire enhanced view. Whole-source rejection still has its existing
  complete CPU fallback. No extra opaque handle, runner ABI, render vtable,
  native-handle exposure, runtime shader compiler, readback, CPU fence or async
  compute queue is introduced.

The extra grow-only GPU buffer is bounded by the existing per-handle source
budget: at most 14 MiB before backend cycling, or 3.5 MiB for the present
128x128 land grid. There is no second CPU source or transfer-buffer copy.
This is additional residency, not free memory. Detailed overlay/log counters
now separate GPU-local `depth-copy-MiB`/`depth-copy-calls` from CPU uploads.
They are traffic counts, **not GPU timers**.

## Correctness checks

- All 165 local app tests pass, including actual Metal and layering/ABI checks.
- A 4,096-case independent float/FMA point oracle covers six clip directions,
  rotations, negative/raised heights, large-radius cancellation and negative W.
  Exact plane contacts, rounding edges and invalid inputs must retain geometry.
- GPU selection tests compare against an independently packed source, exactly:
  reordered/duplicate ranges, copied caller lifetime, empty/full selection,
  growth/shrink, warm reuse, source update, reset, queued-mutation rejection and
  total-budget overflow. Ground, blur, haze and shadows share these checks.
  Upload/copy byte counts and unchanged draw counts are asserted separately.
- The real world fixture compares explicitly unculled, default cold culled and warm culled
  images through 13 lighting, weather, relief, geography, view-family and
  Advent-close-zoom states. It requires actual compaction and zero rejection.
- Against `8823d26a` with its GPU grid enabled, all 12 Palace and 16 navigation
  full-game captures are byte-identical, with identical final WRAM. This is
  culling parity with the previous **GPU prototype**, not resolution of that
  prototype's older isolated CPU/GPU coverage differences.
- The standalone Vulkan fixture passes on Deck in 0.61 seconds, with over
  11.7 GiB RAM available and about 169 MiB peak incremental GPU allocation.
  Two in-game Palace captures at GF600/GF900 also match culling-off exactly.
  D3D12 runtime is unavailable; unchanged generated shaders are not a substitute
  for testing buffer compaction on that backend.

## Local measurements

16 serial real-game ABBAABBA runs, eight per view, 2160x1344 Metal, Quality,
unchanged weather/effects, three helpers in both variants. The control is
`8823d26a`'s opt-in GPU grid. Last five settled reporting windows are weighted
by presents, then summarized across four runs per variant. Captures, builds
and GPU fixtures were kept out of timing intervals. macOS photo analysis and
indexing were active; retain all runs and do not claim quiet-machine precision.

| Per presentation | Palace, full → culled | Navigation route, full → culled |
| --- | --- | --- |
| Submitted vertices | 935,596 → 765,147 (−18.2%) | 998,511 → 715,155 (−28.4%) |
| Draws | 29 → 29 | 28 → 28 |
| CPU geometry upload | 2.036 → 2.036 MiB | 4.391 → 4.391 MiB |
| New GPU-local copies | 0 in settled held view | 0.085 MiB / 0.092 calls |
| Render CPU median | 2.804 → 2.885 ms | 2.445 → 2.425 ms |
| Render CPU range | 2.757–2.848 → 2.694–2.979 ms | 2.372–2.563 → 2.228–2.473 ms |
| Cadence median | 8.337 → 8.336 ms | 8.352 → 8.340 ms |

The CPU ranges overlap; navigation's median reduction is only 0.8%, and
Palace's median worsens 2.9%. Cadence remains approximately 120 Hz. Neither
vertex reduction nor present/wait is a measurement of GPU execution time.

## Short Deck direction check

Eight Palace runs in two serial ABBA sets, 1000 ticks each, same freshly built
binary with only `AR_SIM3D_WORLD_GPU_GRID_CULL=0/1` changing. Real bundled SDL,
Wayland/Vulkan, hidden 1080x672 output, Quality and three helpers. Two separate
untimed captures precede timing. These are **not visible-game FPS measurements**
or directly comparable with the user's original 22-FPS session.

Render CPU median [range]: **5.987 [5.962–6.933] → 6.049 [6.024–6.094] ms**.
One repeat control is slower; it is retained, not deleted. Cadence median is
7.196 → 7.263 ms. Vertices fall 932,443 → 761,995, draws remain 29, and settled
copies are zero. Lower submitted work did not produce a throughput gain here.

Every process has a 45-second cap, an 8-GiB available-RAM start gate, a 6-GiB
RAM abort gate and a 2-GiB GPU-allocation-growth abort gate. All finished normally
in about six seconds with matching final WRAM, zero failed/re-present frames,
over 11.6 GiB available RAM and less than 176 MiB peak incremental GPU storage.
No installed executable/settings/save was replaced; no test process remains.

## Evidence and next step

Local: `/private/tmp/actraiser-grid-culling.nulEwI/`. `timing-*` includes complete
hashes/environments and `work-summary.json`; `verify-final-*` records all 28
capture hashes. `verify-gated-*` repeats all 28 exact captures after making
culling explicit opt-in; `ctest-gated-final.log` records the final 165 passes.
`deck-direction{,-repeat}/analysis.json` uses the same settled
window parser, and `deck-visual` records exact pixel hashes. At checkpoint
`b38415f4`, the final gate required the explicit culling flag too. The subsequent
user-approved default restores culling with the grid flag, without changing
the algorithm. Its verification and next optimization are recorded in
[GPU-world CPU stream reuse](gpu-world-stream-cache.md).

- Mac control: `0bdd4e7b9952b47b461d740c1d7ad972e5d7fd59deea0bad4416d51dc1337dd6`.
- Measured Mac candidate: `d26e18341f51eb5d1410aa1c002989dd1a970bdfe7a45e2f9b5f8811f364bb33`.
- Final opt-in-gated Mac build: `2125fac78bfafcc372d243bee6a3d5682324adde5b60216139d8c42b37aa05ff`.
- Deck game: `b6a91d87278553946be2cd4335523b4045d286fc485f2e8346e0ee1341f4fe3b`.
- Deck fixture: `c0058c738713a3426b92c39cf5e95ff2912626007c55050714a03d4ef1425c1d`.
- Deck in-game directory: `/home/deck/argame/grid-cull-probe-20260912.70j077/`.

The shared-grid prototype's lost held ocean/mountain retention is now partly
addressed by [bounded CPU stream reuse](gpu-world-stream-cache.md). It reduces
the repeated clipping/staging cost but not uploads; complete GPU-source
integration is still needed. Preserve the existing opaque-handle budget and
exact surface/shadow association. Further visibility complexity should wait
for measured GPU pressure or a demonstrated frame gain.
