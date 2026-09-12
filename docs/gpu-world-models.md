# GPU-resident globe models

Status: **enabled by default** in navigation and Sky Palace at the user's
request for offline testing. `AR_SIM3D_WORLD_GPU_MODELS=0` restores the previous
CPU model path; unset or `1` enables GPU-resident models when supported. The
existing scene/model settings still apply. Capability/resource failure keeps
the enhanced view and automatically uses the existing CPU/multicore renderer.
The ordered SDL/custom-GPU adapter and other established defaults remain intact.
This is not a claim that terrain or moving SIM models have been offloaded, nor
that native Steam Deck/Vulkan testing or exact legacy raster parity is complete.

## Work moved

The scene still selects authored town LODs, checks the viewport and conservatively
culls whole objects behind the globe. Visible or uncertain objects use the real
town model compiler, biome palettes, proportions and lighting. Source chart
normals and elevations are prepared only when a model/LOD first becomes resident.
The GPU then rotates the normals, applies radial height, projects and clips the
complete geometry against the shared D32 depth buffer.

`present_world_nav_model_mesh.c` owns the optional presentation cache. Its key
includes the immutable model/terrain revisions, chart/local scale, height
setting, style and lighting. Each capture object/LOD has an owned source range;
no borrowed compiler view or FrameSlot pointer survives a call. Camera motion
does not invalidate those ranges. Returning to a previously used LOD reuses its
source. A newly encountered model/LOD extends the bounded source pool and
republishes it; this is not a claim of incremental vertex-buffer updates.

CPU visibility changes update an ordered **index selection**, not all the
vertices. Both source and index selection stay resident when unchanged.
Visibility/LOD work is also skipped when the complete existing projection key
matches; held Palace frames need only a copied pose/transform uniform. All
three native windmill poses are retained with per-quad variant tags; one uniform
selects the current pose. Inactive quads degenerate outside the frustum. The
complete visible model set remains one draw, including the 72-windmill stress
scene. No per-building draws, asynchronous readbacks or GPU/CPU fences are added.

## Boundary and resource audit

- The project-private depth contract receives only normals, elevations, colors,
  variant tags, index ranges and a copied transform. It contains no town/map
  identity, source-art policy, LOD policy, clock, SDL objects or runner state.
- Source vertices are 40 bytes; the adapter packs a 128-byte std140 uniform.
  GLSL is compiled offline into committed Metal, SPIR-V and DXIL blobs. There
  is no runtime compiler or new minimum storage-buffer/compute capability.
- The four-opaque-handle and 64-opaque-sample limits are unchanged. The globe
  model path uses one handle beside its ground handle and the two optional SIM
  handles. Transparent weather keeps its separate resource budget.
- The CPU source pool is capped at 8 MiB. The object/LOD lookup and staging
  arrays are bounded by the capture capacity, with large scratch allocations
  on the heap rather than adding hundreds of KiB to the presentation stack.
  At maximum allocation the optional mesh has 20 MiB of source GPU/transfer
  storage and at most 3 MiB of selection GPU/transfer storage, before driver
  resource cycling. No unbounded accumulation of old LODs is allowed.
- Update copies data and rejects queued handles. Selection validates every
  range and the complete index budget before changing anything. Failed
  selection retains the old selection; source updates reset selection to the
  complete mesh. Queued destruction invalidates the entire pass.
- Finite/unit-normal/tag checks and conservative double-precision bounds reject
  potentially overflowing shader arithmetic. Partial or behind-eye geometry
  is **not** rejected merely because it needs clipping.
- Shader/resource availability is checked before source compilation. On source
  capacity/resource failure, nothing is queued, the optional handle is released,
  and the normal held-view/multicore path resumes until resource reset. The
  application does not switch to authentic graphics for this optional failure.
- Reset invalidates source and selection GPU contents; retained handles can be
  republished. The scene reset frees its owned CPU cache and handle.

## Validation

Both debug and release builds succeed. The full CTest suite passes 163/163
after default activation, including real Metal GPU tests (not headless skips).
The committed radial shader's offline regeneration check also passes.

The focused adapter suite checks 144 radial transform/pose/clipping cases,
selection ordering and offsets, unchanged source uploads across camera/viewport
changes, copied input lifetime, invalid inputs, overflow, reset/republishing,
queued-update rejection, sample budgets and shared depth. Its placement oracle
evaluates the shader's specified fused radial arithmetic on the CPU, then uses
the same hardware-clipping shader policy. The comparison is exact, without a
relaxed pixel threshold. The existing independent CPU/hardware clipping tests
remain unchanged.

This distinction matters: the first combined CPU-clip/selection fixture exposed
one-channel-value differences on translucent clipped edges. Some also came
from evaluating the CPU reference with different fused arithmetic. Neither
should be mistaken for an index reorder, nor should an exact legacy-renderer
oracle be declared passed because these focused policy tests pass.

Presentation tests cover resident LOD returns, source/light revision changes,
selection-only updates, renderer reset and rejection before any GPU draw.
A separate CTest runs the complete portable globe fixture with the prototype
requested but the adapter declining it, preserving the normal CPU cache tests.
The real GPU integration test exercises 72 interleaved windmills and factories
in both navigation and Sky Palace, exact cold/warm/rewound-pose images, no warm
model-cache lookups/publications and bounded draw counts. Render-boundary
negative tests include the new presentation module/header. Default-on and
explicit opt-out selection have dedicated coverage; the GPU residency fixture
unsets the override, while legacy CPU-cache tests explicitly select `0`.

## Measurement record

Evidence directory: `/private/tmp/actraiser-world-radial.Cs88W3/`.
The pinned `control` is the previous committed default renderer. All reported
timings are real-game CPU wall scopes on this Mac/Metal, not GPU timestamps,
uncapped game FPS or predicted Steam Deck gains. Captures and timings are
separate, with isolated saves/settings and identical final WRAM checked by the
existing comparison tool.

The first eight serial ABBAABBA runs (`timing-navigation`) measured
3.272 → 3.328 ms render CPU: a small regression. That version cached the entire
visible batch, so visibility changes re-uploaded too much. It was replaced by
the resident object/LOD pool plus index selection described above.

`timing-navigation-resident` is exploratory only: a test rebuild overlapped its
last control run. Do not use that batch as final uncontaminated timing evidence.

Final isolated measurements use `candidate-held`, eight serial ABBAABBA runs
per row (32 runs total), CRT enabled, and matching worker settings in both
versions. No builds, captures or other test batches overlapped these timings.
The first matching reporting window is discarded before calculating each run's
frame-weighted stage means; the table gives medians across the four runs per
version.

| View | Helpers | Previous render CPU | Prototype render CPU | Reduction |
| --- | ---: | ---: | ---: | ---: |
| Navigation | 3 | 3.239 ms | 2.866 ms | 11.5% |
| Navigation | 0 | 4.146 ms | 3.304 ms | 20.3% |
| Palace | 3 | 2.771 ms | 2.772 ms | Neutral |
| Palace | 0 | 2.931 ms | 2.895 ms | 1.2%, overlapping ranges |

These are `final-navigation-3`, `final-navigation-0`, `final-palace-3` and
`final-palace-0`. Navigation ranges separate: with three helpers, control
3.222–3.259 ms versus candidate 2.762–2.926 ms; without helpers,
4.092–4.175 ms versus 3.200–3.386 ms. Model projection with three helpers drops
from 0.451 to 0.049 ms, and depth submission from 0.278 to 0.218 ms. Other
unchanged stages vary (cloud preparation rises from 0.510 to 0.575 ms), so the
total-render figure, not the isolated model-stage percentage, is the result.
Palace ranges overlap in both worker configurations; its already-retained held
view does not establish a meaningful benefit from this model prototype.

The larger navigation percentage without helpers is relative to its slower
baseline, not evidence that disabling helpers improves throughput. Three-helper
absolute times are lower in both versions, although worker counts were separate
cohorts rather than an interleaved same-binary worker-count experiment. This
does not establish an optimal helper count or increased GPU utilization.
`render CPU` sums non-overlapping top-level wall scopes. `job helpers*` instead
sums worker callback wall durations that can overlap each other and the owner;
it must not be added to frame time, and is not OS-measured CPU-seconds. Dispatch,
join waiting and memory contention can outweigh parallelism for small jobs;
existing minimum-work thresholds and multicore fallback remain in place.

Full-game captures (`verify-navigation-final`, `verify-palace-final`):

| View | Paired captures | Different pixels | Range per 1792×1344 frame | Maximum channel delta |
| --- | ---: | ---: | ---: | ---: |
| Navigation tour | 19 | 727 / 45,760,512 | 16–60 | 140/255 |
| Palace | 14 | 560 / 33,718,272 | 40 | 103/255 |

Final WRAM is identical for each pair. Differences are localized to model
rasterization; these are not all low-amplitude color changes (coverage changes
can have high contrast). The strict legacy-renderer image oracle **fails**,
and these statistics do not establish exact raster parity. The user subsequently
requested default activation for offline testing with these differences known. The
held-view shortcut (`candidate-held`) matches the preceding candidate exactly
in all 33 captures (`verify-held-navigation`, `verify-held-palace`). The unchanged
CPU opt-out path has six exact reference captures and identical WRAM in
`verify-default`.

After activation, `verify-shipping-navigation` and `verify-shipping-palace`
run the release build without any `AR_SIM3D_WORLD_GPU_MODELS` override. All
19 navigation and 14 Palace captures are byte-identical between zero and three
helpers, with identical final WRAM; they also match the corresponding validated
GPU-candidate captures. This verifies default selection, not legacy raster parity.

The subsequent [bounded Deck Palace verification](steam-deck-hidden-present.md)
exercises the default path with SDL 3.4.14/Vulkan after correcting hidden-window
resource retirement. It completed a no-readback stability probe and two separate
composite checkpoints, with GPU reuse active and no fallback/rejection. This is
targeted native-backend validation, not a navigation tour, a legacy pixel-parity
result or a measured improvement in visible Deck FPS.

## Remaining work

Assess the full-game raster differences and native Vulkan/Deck behavior during
offline testing; default activation does not resolve those validation gaps.
A bounded LOD pool can still fill on unusually dense/high-detail tours; it
deliberately falls back instead of reducing art or
raising an unbounded budget. A measured eviction/partial-publication policy is a
possible follow-up, not implemented here.

Ground/ocean/cliffs and moving weather receiver positions remain CPU prepared.
They need a textured source-surface contract with explicit UV, lighting and
depth/interpolation semantics; this untextured solid contract cannot silently
stand in for those materials. Moving SIM town models also retain their distinct
facing, pixel-snap and bridge depth-placement requirements.
