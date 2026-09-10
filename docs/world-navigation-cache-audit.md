# Globe cache and ownership follow-up

2026-09-08, `world-navigation`, against fallback `427fa7a`. This closes the
shared-recency, oversized-model-storage, ownership and repeated CPU-work
findings from the globe/Sky Palace review. It does not add globe-under-SIM
composition or change town entry/exit controls.

## Ownership and contracts

- The render-thread model cache owns one 64-bit recency clock. SIM and globe
  callers no longer supply unrelated frame counters. Unsigned age comparison
  also handles clock wrap.
- Cached models retain exactly their compiled faces, materials and corner
  brightnesses, not Ultra-capacity authoring boxes/arrays. The read-only CPU
  view is borrowed until an evicting lookup, directory growth or reset;
  neither renderer keeps that pointer across passes. A failed allocation
  preserves the victim and returns no partially published view. SIM also
  refuses to publish a held-view cache when a model allocation fails.
- The model-cache directory still starts at 512 entries and can grow to 8192.
  Growth moves ownership of payloads, not their contents. Reset releases both
  the payloads and expanded directory. Stats report capacity, allocation
  failures and owned bytes, including both directory allocations.
- `present_world_nav_geometry.[ch]` owns the private projection/clipping
  vocabulary. `present_world_nav_sky.[ch]` owns Palace backdrop/mist/cloud-bank
  drawing and atlas-publication state, with an explicit render-device input.
  The main presenter still owns output setup, depth composition and native UI.
- Remaining presenter state is grouped by composition, art publication,
  mountains, model bounds/projections, terrain, shells and weather. These
  owners describe the current mutually exclusive globe/Palace view; they are
  not a simultaneous-scene API. Future globe-under-SIM must share the town's
  depth composition, not call this standalone presenter as an underlay.
- No runner ABI, captured frame schema, settings contract, backend vertex
  format or shader blob changed. Authored models, LOD thresholds, colors,
  cloud density/samples, terrain resolution and effect switches are retained.
  Both CMake test sources and the canonical `snesbuild.ini` source manifest
  include the extracted modules.
- The no-backend-type guard now discovers the entire `present_world_nav*`
  source/header family. Negative tests inject forbidden types into both new
  modules and a newly named helper header in a disposable source copy.

## Retained optimizations

| Work | Reuse rule and fallback |
| --- | --- |
| Palace cloud-slice ordering | Prepare each fixed quality order once; moving bank positions still update each frame. |
| Frozen cloud-shell UVs | Key on projection, viewport and cloud rotation; moving weather still remaps. |
| Ground submission | Reject only quads whose four corners share an outside plane, before constructing attributes. Straddling geometry is retained. |
| Projected town models | Key on view, model/style/LOD revision, registered surface revision, height/light settings and windmill phase. A second held frame warms the cache; subsequent matches replay owned portable vertices. Continuous camera motion does not copy a speculative cache every frame. |
| Cloud-shadow receivers | Prepare exact visible ground/cliff/ocean geometry and six-plane clipping once per view/surface revision. All nine bank/softness samples reuse positions and clipping weights while retaining original UV interpolation and submission order. |

Projected model storage is capped at 8 MiB; receiver storage at 4 MiB.
Allocation or budget failure disables that optimization until resource reset,
without reducing detail or publishing partial geometry. Receivers fall back
to the original uncached builder. Cache storage is released on reset. These
budgets are additional to the compact compiled-model cache, not total renderer
or GPU memory limits.

The 1600-Low-house cache fixture with a 4096-entry directory owns **2,701,603
bytes (2.58 MiB)**, including directory overhead. The old fixed representation
reserved roughly 121.8 MiB for the expanded directory alone. This fixture is
not a whole-process RSS measurement or a bound for an all-Ultra scene.

## Measurements and evidence

Offscreen Metal on this Mac; optimized `RelWithDebInfo` builds, populated
isolated save, 1792x1344, Quality/full effects. Four independent runs per
variant in ABBA-ABBA order, with no concurrent builds, tests or encodes. Each
run uses presentation-count-weighted complete steady windows; mixed entry
windows are excluded. These are CPU-side timers, not GPU timestamps or an
FPS guarantee.

Frozen-weather Palace (`palace.rec`, 800 game frames):

| CPU scope | Fallback median (range), ms | Updated median (range), ms |
| --- | --- | --- |
| Total presentation | 7.666 (7.650-7.749) | 5.132 (5.120-5.150) |
| Weather | 3.273 (3.267-3.313) | 1.720 (1.718-1.725) |
| Model projection/submission | 0.998 (0.997-1.012) | 0.293 (0.291-0.297) |
| Terrain | 0.287 (0.286-0.291) | 0.219 (0.218-0.220) |
| Depth submission | 0.712 (0.708-0.733) | 0.706 (0.701-0.713) |

Total presentation falls **33.1%**, weather **47.4%**. Depth-submission ranges
overlap: no independent submission speedup is claimed. Both frame-700 and
frame-800 images match across all eight runs. Those two native animation
frames are compared by frame number, not to each other.

Moving-weather Palace, another four runs per build in the same interleaved
order: presentation **7.824 ms (7.767-7.866) -> 5.519 ms (5.459-5.652)**,
approximately **29.5% lower**. Weather is **3.537 -> 2.175 ms**. These runs use
host-clock cloud motion, so cross-run screenshot equality is not asserted;
the injected-clock GPU sweep supplies the exact moving-weather comparison.
`final-moving-runs.json` records these eight runs.

Navigation flight (`worldnav-visual-tour.rec`, 2240 frames, fixed weather),
another four runs per build in ABBA-ABBA order:

| CPU scope | Fallback median (range), ms | Updated median (range), ms |
| --- | --- | --- |
| Total presentation | 7.948 (7.936-7.973) | 7.332 (7.299-7.342) |
| Weather | 2.457 (2.449-2.465) | 2.283 (2.280-2.305) |
| Model projection/submission | 1.214 (1.210-1.214) | 1.029 (1.025-1.038) |
| Depth submission | 1.009 (0.979-1.027) | 0.944 (0.927-0.953) |

Total presentation falls **7.75%** with non-overlapping run ranges. All **19
captures match exactly across all eight flights**. The held-model cache is
deliberately conservative during camera movement, so this smaller improvement
is expected; the Palace result must not be applied to navigation generally.
`final-navigation-runs.json` and `benchmark-commands.json` retain the inputs.
Depth draw counts remain seven for navigation and eight for Palace. Navigation
cold-entry peaks overlap (fallback 92-101 ms, updated 82-95 ms); this does not
establish a cold-start improvement, and the existing fade/loading behavior is
unchanged.

The compact model storage alone was also tested in a separate eight-run
comparison: presentation medians 7.708 -> 7.666 ms; model lookup/compilation/
shading 0.0591 -> 0.0390 ms. Its primary benefit is memory and correct ownership,
not a large standalone frame-time improvement.

Evidence root: `/private/tmp/actraiser-world-findings.qPrfXE/`. It contains
fallback/intermediate/final binaries, run manifests, analysis scripts, logs
and controlled GPU captures. `final-frozen-runs.json` identifies the eight
runs above. The ordinary debug GPU sweep has **252 byte-identical captures**
against the fallback, covering native foreground, all towns, weather motion,
light/quality/effect changes, clipping and resource-reset restoration.

All **149 CTests pass**, including compact-cache eviction/wrap/fault-injection,
directory growth/reset and compiler-face/material/shading parity across every
model kind and LOD. Receiver interpolation is byte-compared with the original
clipper across all six planes and the unclipped path. Cache storage and the
full GPU sweep also pass AddressSanitizer/UndefinedBehaviorSanitizer.
With identical sanitizer/compiler flags on both revisions, all 252 sanitizer
captures also match exactly. Different optimization flags are not used as an
image-equivalence baseline.

## Selected 3x Palace diameter

The follow-up against `b07b03e` adopts the user's 3x Palace preview, retaining
daylight, cloud decks and selected-town framing. Navigation remains at the
original radius. Portable `*AtRadius` chart/transition functions accept an
explicit metric, with default wrappers preserving existing callers. Only the
presenter selects the Palace radius; no mode dependency enters the math layer
and no runner ABI, frame schema, settings, backend or shader contract changes.

Private projection keys include the chart radius. Mountain transitions and
samples, cliff normals, model bounds and ground-cloud UVs invalidate on a
view-family switch. Mountain sources rebuild before lava animation so their
transition tint stays deterministic. Compiled town models and cloud atlases
remain reusable. Neither town selection nor ordinary held frames rebuild
radius-dependent source geometry. This remains a single mutually exclusive
view owner, not the future simultaneous globe-under-SIM composition.

All 149 CTests pass. Added coverage checks both chart metrics, unchanged default
wrappers, finite/invalid radius handling, matching native mountain joins and
continuation limits, protected building cells, and radius-independent material
pixels. The six-town GPU test switches Palace/navigation without resetting
resources and requires exact restoration of both views, alongside existing
cloud/quality/shadow toggles. The ordinary and ASan/UBSan sweeps each pass with
252 captures: **246 navigation images remain byte-identical** to `b07b03e`;
only the six Palace views change. The production frame-800 capture is also
byte-identical to the selected 3x preview, including native foreground. Its
WRAM dump and the isolated/original save hashes are unchanged.

Repeated CPU measurements use the same host, resolution, quality and replay
methodology above, four independent runs per diameter per weather mode in
ABBA-ABBA order, with no concurrent builds/tests/encodes (16 runs total):

| Complete steady presentation windows | 1x median (range), ms | 3x median (range), ms |
| --- | --- | --- |
| Frozen clouds | 5.150 (5.101-5.220) | 4.980 (4.960-4.999) |
| Moving clouds | 5.660 (5.640-5.679) | 5.499 (5.480-5.519) |

The selected framing shows no steady CPU regression in this scene (about 3%
lower presentation time); this is different visible geometry, not a standalone
optimization or a cross-platform FPS claim. The existing fade/entry handling
is untouched; these steady windows do not establish view-switch latency.
Evidence, commands and run manifest: `/private/tmp/actraiser-palace-3x.UDZaQP/`.
Production capture: `runs/20260908-181433/shot_800.ppm`; selected preview:
`runs/20260908-175911/shot_800.ppm`.

## Centered navigation camera

The navigation follow-up on `wip-localization` replaces the inherited oblique
town camera with a radial eye through the travel location and planet center.
Normal travel, zoom and manual globe inspection now share centered framing;
the original top-down Palace sprite faces correspondingly top-down terrain.
The separate 3x Sky Palace horizon camera is unchanged. No art, terrain
resolution, model detail, LOD threshold, weather sample or effect control is
reduced. Existing zoom can still crop a close planet symmetrically.

The obsolete inspection-centering blend is removed from application-owned
camera/`FrameSlot` state and retained-frame refresh. Runner ABI, backend,
settings and native gameplay contracts remain unchanged. No new cache, draw
pass, allocation or graphics dependency is introduced. Shared view/eye math
continues to own horizon culling, occlusion, clouds and Advent clearance.

Debug/Release builds and all 149 CTests pass. Both ordinary and ASan/UBSan GPU
sweeps pass with 252 captures each, covering all six towns, orbit/return,
native markers/UI, quality and weather toggles, and raised-terrain Advent.
The six ordinary Sky Palace captures remain byte-identical to the selected
3x baseline. The oblique tall-roof/offscreen-anchor fixture now exercises the
still-oblique Palace camera; the fake depth pass binds its current device
context directly instead of retaining a prior test's expired stack context.
The source and isolated save SHA256 values remain
`480a8375b6255ad681202986640c5b06889458125ec0f82641a0e4fb425c0d45`.

Four independent runs per camera use ABBA-ABBA order, the same 2240-frame
six-town replay, 1792x1344 Metal output and frozen clouds. No builds, tests or
image encodes from this task overlap timing; unrelated localization work was
active on the shared machine. Each replay supplies 18 complete steady windows
after entry. All 19 gf400-2200 screenshots repeat exactly within each camera
variant, and final WRAM is identical across all eight replays.

| CPU presentation time | Before | Centered radial |
| --- | --- | --- |
| Median of four runs | 7.519 ms | 8.256 ms |
| Run-average range | 7.331-7.676 ms | 8.115-8.395 ms |

The centered view has a measured 0.737 ms (9.8%) steady CPU cost increase,
not a performance improvement. Cloud work rises from 2.336 to 2.671 ms,
depth projection from 1.041 to 1.227 ms, and submission from 1.007 to
1.113 ms. This is consistent with the different visible globe coverage;
quality/effect reductions were not used to hide that cost. It is not an
isolated GPU timing, moving-weather benchmark or cross-platform FPS claim.
The existing fade timing and entry/leave controls are unchanged.

Evidence and reproducible timing analysis:
`/private/tmp/actraiser-centered-navigation.2e0hx3/`.

## Marahna sanctuary and coastal opacity

The Marahna follow-up fixes two independent source/coverage mistakes. The
populated retained map contains a complete `$C2/$C3/$CA/$CB` cathedral at
cell (17,17), but navigation previously searched Marahna only for the `$C0`
temple variant. Sanctuary capture now selects the shared authored model from
the complete four-cell signature, matching the full town classifier. Both
variants retain the same protected 2x2 footprint and development gate.

The ten-cell chart-boundary fade previously made actual land translucent.
Marahna's window starts at world row 96 and reaches row 127, so its southern
plots and coastline fell inside that fade. Every corner of a non-open-water
cell now stays opaque. Only adjacent pure ocean supplies the transition to
the full-globe sea; no shoreline, terrain height, model LOD or atlas texel is
reshaped to conceal the defect.

`SimWorldMap` owns a 256-entry categorical source-tile table. Its public
`CellIsOpenWater` query returns true only for all-`$10/$11` ocean/wave palette
samples; even a single non-water texel protects a mixed shore. Animated tiles
must qualify in every native wave phase, keeping opacity independent of time
and RGB lighting. The presenter queries only incident cells in the boundary
strip while rebuilding an existing projection cache. Its geography key now
tracks the published map directly, so shoreline changes invalidate opacity
even when optional relief is disabled. This adds no per-frame pixel scan,
heap allocation, texture, draw pass, backend type or runner/frame ABI field.
Authored town models and material classification remain in portable SIM
modules; presentation owns the boundary-fade policy.

Coverage includes both sanctuary variants in every town, cross-page 2x2
signatures, incomplete/stale data rejection, protected ground masks, palette
identity versus merely blue RGB, mixed shores, all four map edges, wave
phases, held-frame replay, resource reset and live map changes with relief
On/Off. The captured Low cathedral test keeps ground cleanup and the safety
envelope enabled in both images, removing only drawable model geometry, so
erasing its old 2D glyph cannot produce a false pass.

Debug/Release builds and all 149 CTests pass. The ordinary and ASan/UBSan
GPU sweeps each pass with 254 captures, including six-town quality/effect
matrices, cloud motion, view-family restoration and raised-terrain Advent.
Four independent runs per revision use ABBA-ABBA order, the same 2240-frame
six-town replay, 1792x1344 Metal output, frozen clouds and one frame-1800
screenshot per run. All four screenshots repeat exactly within each revision,
and final WRAM is identical across all eight runs. The original and isolated
save hashes remain unchanged.

Across 18 complete steady windows per run, median CPU presentation time is
8.252 ms before (run-average range 8.193-8.432) and 8.316 ms after
(8.269-8.784). The 0.064 ms/0.8% median difference and overlapping ranges do
not establish a material steady regression on this host. There were no
concurrent builds/tests/encodes from this task; unrelated localization work
remained active on the shared machine. These are CPU timings, not isolated
GPU measurements or a cross-platform FPS guarantee. No visual quality or
effect settings were reduced for this fix.

Evidence: `/private/tmp/actraiser-marahna-fixes.AuTVyL/`. In-game six-town
visual replay: `runs/20260908-212206/`. Close-up of the production GPU capture:
`runs/marahna-globe-fixes/marahna.png`.

## Remaining measurement limits

The geometry stream still reaches the backend each frame. Its roughly 0.7 ms
Palace submission cost is not eliminated by CPU projection caches. Further
GPU residency/vertex-format work needs a separately measured portable backend
change; this patch does not expose GPU storage or bypass the render contract.

This host has no `xctrace` installation, and no non-Metal GPU is available.
Therefore isolated GPU timing and Vulkan/D3D12 hardware acceptance remain
unverified. Boundary tests and unchanged shader/backend contracts are not
substitutes for those measurements. Do not describe the local CPU gains as a
cross-platform FPS guarantee.
