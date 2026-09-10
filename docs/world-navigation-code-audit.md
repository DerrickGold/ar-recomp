# World-navigation code-quality and performance audit

Audited 2026-09-08 on `world-navigation`, initially against checkpoint `380772a`
and subsequently against `4f1741b` plus the atlas-publication fix below.
This is a focused source/runtime audit,
not a claim that every possible save, GPU, allocation failure or camera path
has been exhaustively proven. The initial pass was read-only; follow-up changes
and their verification are identified below.

The later [globe cache and ownership follow-up](world-navigation-cache-audit.md)
records the remediation against `427fa7a`, including compact model storage,
cache-owned recency, presentation ownership separation and measured reuse of
model projections and cloud-shadow receiver geometry.

## Findings and follow-ups

1. **Boundary-check coverage gap — confirmed and closed.**
   `tools/check_render_boundary.cmake` protects the presenter and selected SIM
   modules, but does not include the new `sim_world_navigation_*` helpers,
   `sim_town_ground_art` decoder or all shared voxel authoring/cache helpers in
   its no-SDL scan. In a disposable copy of `src`, adding
   `SDL_FPoint navigation_boundary_probe;` to `sim_world_navigation_globe.c`
   still returns success. The unchanged copy also passes. The real helper has
   no such dependency; this was a regression-guard gap, not an existing runtime
   leak. The guard now includes these families (both sources and headers),
   with duplicate paths removed. The new
   `actraiser_render_backend_boundary_negative` CTest first checks a clean
   disposable source copy, then verifies four isolated injected dependencies
   are rejected and the offending file is identified. Its cases include a
   newly created navigation helper header, not just existing filenames.
   This extends the existing no-SDL token policy; it does not add legitimate
   documentation-only SDL references elsewhere to that strict inventory.

2. **Incomplete stage attribution — identified gaps closed.**
   In `PresentWorldNavigation3D`, preparation of the projection/model envelope,
   the space backdrop, atmospheric shell, ocean mesh and mountain projection
   sit outside their own performance scopes. Total presentation time includes
   them, but the named stage totals cannot attribute all the work. In
   particular, the `terrain` timer starts after the ocean has already been
   built/submitted. Cloud-shell work is correctly inside the weather scope.
   The follow-up adds `world-prepare`, `world-atmosphere` and `world-ocean`,
   plus existing `backdrop` and `depth-mountain` scopes around the missing
   stages. Ocean includes depth-pass setup; preparation sums two sequential
   scopes (terrain-field and projection/model-envelope preparation). Each
   closes before success or early failure. A profiled presenter CTest verifies
   scope restoration on failed space/atmosphere draws and depth setup.
   Lightweight output/UI work remains outside these scopes; do not infer that
   any remaining unattributed difference is GPU execution time.

3. **Single-instance cache coupling — maintainability risk, not a current leak.**
   `present_world_nav.c` coordinates many file-static atlas, terrain, cliff,
   mountain, model-bound, projection and weather caches. Invalidation ordering
   is substantive: changing mountain ownership invalidates terrain/cliffs and
   ground art, while a lava-only update must not invalidate those surfaces.
   Existing tests exercise toggle/reset restoration and failed transfers.
   Group related state and invalidation operations by owner before extending
   this to simultaneous SIM/globe rendering. Do not turn this into a generic
   scene framework or relocate game policy into the backend.

4. **Shell selector obscures intent — small code smell, closed.**
   `DrawWorldNavigationSphereShell` accepted an `int` kind, while its main
   callers passed `true` for atmosphere and `false` for ocean despite named
   shell constants already existing. It now uses a private typed shell enum
   and named constants at every call site. Its unused `FrameSlot` argument
   was also removed: the helper needs only the viewport, prepared projection
   and shell kind. This cleanup was applied separately, after benchmarking
   the lighting change; it changes no public contract or ABI. For that cleanup
   alone, the optimized presenter's `__TEXT,__text` bytes were identical to the
   measured object (before the later profiling/ocean changes below).

5. **Stale publication after a failed full atlas upload — reproduced and fixed.**
   A full artwork rebuild overwrites the retained CPU image before the GPU
   upload. If that transfer fails, the old style/phase key previously remained
   valid. Returning to that old style at the same animation phase could skip
   rebuilding the CPU image; a later sparse animation patch then used the
   wrong retained artwork. The regression reproduces a detailed-to-basic
   failed upload followed by a detailed-style reversal and animated water.
   Its full CPU/GPU image mirror fails against checkpoint `4f1741b`, after
   ordinary animation phases pass. Distinct static native-ground pixels keep
   this mismatch from being hidden by the synthetic fixture's black artwork.
   Full rebuilds now invalidate publication before mutating CPU pixels; only
   a successful upload republishes the key. Existing reset, mountain-surface
   and failed incremental-transfer invalidation share the private helper.
   Recovery requires one complete upload, then frozen frames upload nothing
   and forward/reverse water animation resumes its original 1 KiB patches.
   No extra atlas, allocation, public contract or backend dependency is added.

No unauthorized runtime layer/runner-ABI access was found in the inspected
paths. The confirmed rendering defect above is closed; the single-instance
maintainability risk remains. Source inspection does not prove every backend.

## Ownership and contract checks

### Sky Palace and cloud-shadow follow-up

The optional Palace view (`sim3d_sky_palace` / `AR_SIM3D_SKY_PALACE`) is gated
by world navigation in both settings availability and the game-side producer.
It reuses the globe's developed geography, native mountains, authored model
LOD, atlas and depth resources. A Palace-only blue daylight gradient and
drifting sky deck replace navigation's starfield. The revised sky deck uses
twelve lit 3D density banks, sixteen depth-tested slices each, replacing the
earlier 384-quads stretched-noise backdrop. Cloud enable/density/drift controls
apply; zero drift freezes both decks. `sim3d_sky_palace_volumetric` /
`AR_SIM3D_SKY_PALACE_VOLUMETRIC` defaults on, gated by navigation, Palace and
clouds. Off selects one precomposited slice per bank, not the old noisy art.

The portable density baker owns no allocation or frame/renderer state. Four
96×48×16 volumes use coherent 3D erosion and six sun-density probes per lit
voxel. Their 384×960 ARGB atlas includes gutters and four cheap fallback
tiles (1,474,560 bytes). The presenter allocates/bakes/uploads only when the
captured light/camera key changes; quality/drift/density changes do not rebake.
Failed publication is not treated as cached success or retried every frame;
a changed key or resource reset permits another attempt. No retained CPU
atlas, per-frame heap allocation, new render target or readback is introduced.

`VolumeCloud` is an appended project-private semantic material, preserving
existing numeric IDs. Its pass-owned atlas uses existing linear sampling and
the existing portable alpha/depth-test/no-depth-write pipeline and shader
blobs (Metal/Vulkan/D3D12); no backend handle escapes the pass. All 192 slice
planes sort together back-to-front, then use the shared six-plane clipper.
Opaque planet/terrain/models occlude them. Decorative Palace banks and the
spherical weather deck remain separate transparent layers, not a unified
physical weather simulation; these banks do not add terrain-shadow receivers.

The ten-vertex gradient holds indigo-blue into the visible Palace windows
before reaching pale blue at the projected sea horizon. The Palace-only
camera's minimum standoff is 0.75 globe radii and its aim is 0.03 radians
above the sea tangent, placing that horizon about 53% down the viewport.
It retains the global terrain/model/atmosphere clearance bound and authored
scale; navigation's camera is unchanged. The atmosphere toggle controls a
narrower, lower-opacity three-quad translucent
horizon veil, an artistic aerial-perspective approximation, not physical
scattering. It is below native UI and before master fade. Palace composition
explicitly uses the existing premultiplied-alpha draw contract for the depth
target, preventing dark cloud fringes from double alpha multiplication.
Navigation retains its previous composition and space appearance.

Selected-town/upper-cloud follow-up (base `dddfb25`): after native A/B comparison
the user selected daylight. The orbital prototype remains isolated in
`runs/20260908-162052/`; it is not a setting or a shipped alternate style.
The daylight camera, gradient, mist and scale remain unchanged. Its private
projection now consumes the captured active-region centre (captured travel
focus only when no region exists), samples the retained surface height and
rotates that point onto a ray 0.045 radians below the sea tangent. The near
sphere intersection retains the visible hemisphere, placing the raised centre
about 56.5% down the viewport across town datums. No native coordinates,
producer state, public ABI, backend or output/foreground ownership changes.
The existing projection cache key includes the rotated frame. The height query
is cache-backed; the solver uses bounded scalar arithmetic and no allocation.
With relief disabled, the new height query is skipped entirely.

Three repeating upper banks replace the previous lone upper cloud: two extra
banks, 32 extra slices in high mode, two extra single slices in low mode.
All use the existing atlas, global depth ordering, clipping and cloud gates.
No new draw pass, material, texture, per-frame allocation or cloud bake is
introduced. Tests track selected-centre mesh UVs through all six towns,
0/100/200% relief and 0/96/192 cloud altitudes, require the centre above the
menu and verify explicit-region precedence over stale travel focus. They also
cover no-region map-edge fallback, upper cloud geometry/motion, upper-deck
wrap boundaries, low mode and cloud-off restoration.

Validation: the full suite passes all 148 CTests (48.67 s), followed by fresh
presenter/profiled and GPU checks after the final fast-path/wrap-test additions.
The ASan/UBSan Metal sweep in `/private/tmp/actraiser-palace-town-horizon.v1Iemq/`
retains all 246 navigation references exactly and checks the six selected
towns. The final relief-off guard's fresh sanitized sweep in
`/private/tmp/actraiser-palace-town-final.z03JWC/` passes and reproduces all
54 overlapping images exactly, including all six Palace captures. Upper/lower
cloud wrap checks and long host uptime change at most 5/255 per channel per
8 ms step, with means .115–.139/255. Six-town visual inspection is retained
in `towns.png` in the full sweep directory.

Six serial release runs use the same frozen 1792×1344 replay in order
baseline/current/current/baseline/baseline/current: `164021`, `164036`,
`164052`, `164123`, `164137`, `164153`. Baseline CPU presentation averages
6.924, 7.039, 6.939 ms; current 7.720, 7.699, 7.798 ms. The median cost is
about +0.781 ms (11.2%), with weather +0.384 ms and submission +0.147 ms.
The visible view submits 601,000 versus 482,128 vertices, still in eight
depth draws. This is a measured extra-content cost, not machine noise or an
FPS claim; the existing quality/effect controls remain available. Four to six
complete windows per run remain after consistently excluding the first mixed
transition window. Every capture within each variant matches exactly. No
competing builds, GPU tests or encodes ran during timing; all runs and
`analyze.mjs` are retained in the full sweep directory.

Native preview: `runs/20260908-164237/selected-town-daylight.gif` (560×420,
31 frames, 6.2 s, 615,794 bytes, full-frame 256-color palette) and the matching
unquantized PNG. The moving replay WRAM matches the prior daylight replay;
both original and isolated save hashes are unchanged. True 16:9 was inspected
in `runs/20260908-164354/selected-town-wide.png` (2390×1344 source, 43 columns
per side). Non-Metal device acceptance remains open.

Earlier framing/color follow-up (checkpoint before this pass: `9355104`): the changes
are confined to private Palace projection, gradient and mist functions.
No settings, ABI, backend, resource lifetime, native foreground ownership,
terrain scale or navigation rendering contract changed. The extra gradient
row adds two vertices/two triangles, with no new draw call or allocation.
Cloud shapes, density and speed are unchanged. Tests now require the richer
blue below the native roof, horizon placement across focus/cloud altitudes,
and the flat-blue backdrop-off fallback. All 148 CTests pass (48.28 s).
The ASan/UBSan Metal sweep in `/private/tmp/actraiser-palace-framing.2K02YP/`
retains all 246 navigation references exactly and checks all six Palace
views, gates, weather continuity, clipping, reset and return parity.

Three serial frozen 1792×1344 replays (`155806`, `155823`, `155838`) average
7.000, 6.899, 6.879 ms CPU presentation. A counterbalanced old/new/new/old/old/new
series then rebuilt only the checkpoint's presenter against the same release
objects/options. Runs `160203`, `160219`, `160235`, `160304`, `160319`, `160334`
give old averages 6.720, 6.680, 6.619 ms and new 6.940, 6.920, 6.880 ms.
The median increase is about 0.239 ms (3.6%), chiefly weather (about 0.161 ms).
This is a small repeatable cost of the revised view, not dismissed as noise;
the visual framing is retained with that measured tradeoff. Each run uses
five complete Palace windows after excluding the initial mixed transition
window. All images within each variant match exactly, and the rebuilt old
variant matches the prior checkpoint capture. This measures CPU presentation,
not GPU time or a portable FPS guarantee. Analysis is in the sweep directory;
the isolated checkpoint build is `/private/tmp/actraiser-palace-framing-baseline.vFqQVA/`.

The revised native daylight comparison is
`runs/20260908-155946/daylight-comparison.gif` (800×328, 31 frames, 6.2 s,
584,498 bytes), encoded with a full-frame 256-color palette. Its WRAM matches
the native-original replay exactly. Native 16:9 framing was also inspected in
`runs/20260908-160040/framing-wide.png` (2390×1344 source, 43-column margins).
The original and isolated populated saves remain unchanged. The user-supplied
ActRaiser 2 image suggests a subsequent orbital/indigo-sky alternative;
this checkpoint keeps the refined daylight look for comparison.

The earlier passing-cloud follow-up adds two repeating decks between the globe and
the native foreground: three softer middle banks and three fuller lower
banks, plus the four distant sky/horizon banks. The nearer deck moves faster
for parallax; each deck's three instances share a drift rate and wrap fully
offscreen, leaving organic gaps without emptying the lower sky. Their view
depths remain real and globally sorted. Wind is evaluated once per bank,
not once per slice. This adds no atlas, material, pass, setting or ABI change;
low mode uses ten single-slice banks. Native angel/pillars/menu composition
is unchanged and remains above every cloud. The color-corrected motion
preview is `runs/20260908-153916/passing-clouds-corrected.gif`
(480×360, 31 frames, 588,740 bytes). The original preview's 96-color,
motion-weighted GIF palette muted static HUD colors; captures and game
pixels were unaffected. Use a full-frame 256-color palette for comparisons.
The follow-up passes all 148 CTests. A fresh ASan/UBSan Metal sweep in
`/private/tmp/actraiser-palace-passing-clouds.BHr05Z/` retains all 246
navigation references exactly and verifies six Palace towns, lower-view
motion, quality/freeze/cloud gates, depth separation and reset/return parity.
Eight-millisecond steps around the lower decks' wrap boundaries and a long
host-clock uptime change any color channel by at most 4/255, with mean
changes .117–.138/255: no visible wrap discontinuity in these cases.
Three additional serial native runs (`153606`, `153620`, `153634`) at the
same frozen 1792×1344 Fillmore view average 6.660, 6.540 and 6.599 ms CPU
presentation, with five complete Palace windows per run and identical
captures. Weather averages 2.742, 2.706 and 2.719 ms. These remain around
the earlier ~6.6 ms result; they are not a paired GPU-time comparison or an
FPS guarantee. Analysis is retained alongside the new sanitizer sweep.

Initial six-bank volume-cloud validation (2026-09-08): all 148 CTests pass, including boundary
guards, bake determinism/padding/invalid-input/light-alpha checks, sorted
clipped slices, quality/cloud gates, failed-publication recovery, explicit
alpha composition and real-GPU depth/no-depth-write checks. The ASan/UBSan
Metal sweep in `/private/tmp/actraiser-palace-volume.Xg3tHM/` preserves all
246 navigation reference PPMs exactly and verifies six revised Palace views,
motion/freeze, high/low clouds, shadow toggles and reset/return parity.

Six serial native runs (volume/low/low/volume/volume/low), with builds/tests
idle, use frozen drift and identical 1792×1344 Fillmore input. Five complete
Palace windows per run remain after excluding the first mixed transition
window. CPU presentation averages for volume are 6.660, 6.579, 6.519 ms;
low mode 6.559, 6.540, 6.559 ms. Their medians differ by about .020 ms,
smaller than run-to-run noise; this is not a GPU-time or FPS claim. Weather
stage medians are 2.719 versus 2.699 ms. All captures within each quality
mode match exactly. Runs `151754`, `151808`, `151823`, `151838`, `151854`,
`151908` and `analyze.mjs` in the sweep directory retain every result.
The native screenshot is `runs/20260908-151754/sky-palace-volumetric.png`.
Native 16:9 was also inspected in `runs/20260908-152050/` (2390×1344
scanout, 43 margin columns per side); both original and isolated testing
saves retain SHA-256 `480a8375b6255ad681202986640c5b06889458125ec0f82641a0e4fb425c0d45`.
Non-Metal device performance/visual acceptance remains open.

The native BG1 winner mask is observational: original scanout is never removed
or recolored. The producer respects existing capture owners. The capture
adapter checks public runner capabilities, structure sizes, frame generation,
palette/color-math eligibility and exact capture flags/extents. Presentation
validates immutable slot surface spans and pitches, then publishes a bounded
512×240 ARGB foreground texture only after a successful upload. Retained
presentations never revisit borrowed producer pixels. Any missing/unsupported
capture or failed optional Palace draw uses the untouched native frame. Output
restoration failures remain fatal under the existing output contract.

The shared globe scene pass does not own output setup or native UI. Navigation
retains its existing complete presenter; Palace invokes only the scene inside
its caller-owned output. Both modes are mutually exclusive, so this does not
authorize simultaneous globe-under-SIM use of the single-instance caches.

Low horizon cameras require clipping before perspective division. Portable
`scene3d_math` clips triangles against six homogeneous planes using bounded
stack polygons and barycentric values; no material/backend/runner knowledge
crosses that boundary. Globe submission interpolates its own attributes and
clips ground, mountains, models and weather. Opaque ground and shadow/haze
receivers share exact clipped geometry. Navigation retains its previous
unclipped path. Randomized math tests, fake-backend receiver/ownership checks,
real-PPU foreground parity and six-town GPU/motion/reset tests cover these
contracts. A tiny negative interpolated alpha at a zero-haze endpoint was
found by the fake backend and fixed by bounding the final interpolated color.

Three eight-flight ABBA–ABBA series checked the shared renderer against
`c156a8a`, with Palace disabled in both builds to isolate normal navigation.
All 19 replay images match across all 24 flights. The initial implementation
added roughly 0.26 ms to median CPU presentation time. Keeping large clipping
helpers behind small navigation wrappers and moving Palace clip copies out
of cloud per-vertex loops reduced that cost. The retained candidate's four-run
median is **8.184 ms versus 8.041 ms** for the checkpoint, approximately
**0.144 ms / 1.8% slower**, not a speedup or performance-parity claim. Weather
medians are 2.531 versus 2.458 ms; mountain medians .331 versus .304 ms.
The last pair in the first two series shows broad CPU slowdowns in both
builds; all runs remain recorded. Retained-series ranges are 8.144–9.501 ms
versus 8.009–12.092 ms; their overlap does not erase the earlier repeatable
small regression. These are CPU-side measurements, not GPU timestamps.

A final cache-packing experiment used the same allocation count/footprint,
but its baseline-relative overhead remained about .125 ms and the small
additional difference did not justify explicit tail-layout bookkeeping. It
was removed. Frozen binaries, full run manifests, per-run timings and the
rejection rationale remain in `/private/tmp/actraiser-palace-regression.78KlaZ/`,
`/private/tmp/actraiser-palace-hotpath.YHKM6F/` (retained), and
`/private/tmp/actraiser-palace-packed.4v0sEl/` (rejected). The retained renderer
passes 148 CTests and ASan/UBSan with all 252 GPU images identical to the
pre-cleanup Palace build, including all 246 original navigation references.

Three additional native Palace replays with moving clouds enabled measure
6.874–6.912 ms CPU presentation time (median 6.874 ms), with weather at
3.018–3.034 ms. These are this saved Fillmore viewpoint at 1792×1344, not a
cross-town or cross-platform guarantee. The first mixed entry window is
excluded consistently; eight full Palace windows remain per run. Moving
weather uses the host clock, so screenshots from independent runs need not
match; exact-image tests above freeze/inject that clock. Runs `143232`,
`143249`, `143308` and analysis live in the retained candidate's evidence
directory. The rebuilt final executable's machine-code section matches the
retained benchmark binary exactly; only non-code binary data differs.

Cloud shadows already existed and were retained, not duplicated. They use the
same spherical cloud atlas on exact terrain/cliff/ocean receiver geometry,
depth-test without depth writes, and batch into one additional material draw.
Three banks use three weighted softness samples each (one with softness zero).
Buildings and native mountain cutouts occlude those receivers; roofs and
mountain facades do **not** yet receive their own cloud-shadow projection.
Removing receiver depth or painting complete mountain bounding quads would
break occlusion/cutout contracts and was not used as a shortcut.

Eight serial on/off flights in ABBA–ABBA order isolate the existing shadow
cost, with no concurrent builds/tests, unchanged full-quality replay/settings,
1792×1344 output and frozen weather. Four runs per variant:

| CPU-side measurement | Shadows on median (range), ms | Shadows off median (range), ms |
| --- | --- | --- |
| Total presentation | 7.991 (7.983–8.020) | 5.338 (5.325–5.375) |
| Weather construction | 2.436 (2.435–2.456) | 0.507 (0.506–0.508) |
| Depth submission | 1.033 (1.025–1.045) | 0.471 (0.468–0.473) |

This is a **quality-toggle cost**, not a quality-preserving optimization or
GPU timestamp measurement. All 19 images repeat exactly within each variant
and intentionally differ between on/off. Evidence and frozen binary:
`/private/tmp/actraiser-cloud-shadow-audit.2c5U5k/`; manifest lists runs
`132442`, `132508`, `132533`, `132601`, `132626`, `132654`, `132718`, `132742`
under `runs/20260908-*/`. Shadows remain independently switchable for low-end
systems. Future optimization should target repeated receiver staging without
reducing the current surface coverage or softness samples.

### Existing navigation ownership

- World/town capture uses the public runner API, checks capability/structure
  sizes and borrowed-span generations, and publishes copied semantic values.
  Presentation does not read live `g_ram`, `g_ppu` or `g_settings`.
- Globe math, native art decoding, terrain classification, authored model
  bounds and mountain scene construction contain no SDL/GPU resource calls.
  Shared model bounds remain a portable value API, not a renderer-cache handle.
- Atlas transfer and vertex conversion remain in the SDL adapter. The public
  seam carries ARGB integer pixels, pitches, rectangles, semantic materials
  and opaque render handles. Native-endian ARGB8888 to RGBA32 conversion stays
  behind that seam; there is no new runner ABI or platform SIMD requirement.
- Failed full or animated atlas transfers invalidate the CPU/GPU publication
  key for retry. Lava updates retain their dirty rectangle after a failed transfer.
  Optional native geometry/art falls back through existing ownership gates.
- Future globe-under-SIM must submit into the town's shared depth composition.
  The current standalone presenter owns output setup, composition and UI;
  calling it as an underlay would violate those responsibilities. This future
  integration is still deferred, as are new town entry/exit gestures.

## Measured performance and low-risk candidates

The existing four-current/four-baseline ABBA–ABBA runs were re-analyzed, not
replaced with one new noisy run. Inputs: the isolated populated save,
`worldnav-visual-tour.rec`, the optimized `build-release` configuration
(`RelWithDebInfo`, `-O2 -g -DNDEBUG`), 1792×1344, Quality, full effects, fixed
weather phase. Current-build weighted presentation averages range
8.173–8.193 ms, median 8.184 ms. All 19 captures match across all eight runs.
The immediately preceding baseline overlaps (8.186–8.270 ms): do not claim
the latest culling correction is a speedup.

| Pre-hoist steady stage | Observed time per presentation |
| --- | --- |
| Weather | 2.46–2.48 ms |
| Terrain projection/shading/submission | 1.23–1.24 ms |
| Model projection/submission | 1.16–1.17 ms |
| Depth submission | 1.06–1.08 ms |
| Artwork preparation/upload | 0.62–0.63 ms |
| Model-cache lookup/compilation/shading scope | 0.078–0.081 ms |

Scopes are not all exclusive: artwork includes animation/transfer subscopes.
These are CPU-side timers around rendering operations, not isolated GPU
timestamp measurements. Cold-entry peaks remain 99–109 ms in these runs;
the separate native fade replay verifies that normal setup occurs under black.

### Retained light-direction optimization

`WorldNavigationSurfaceShade` previously recomputed the same sun-direction
trigonometry for every ground/cliff sample. It now receives the identical
vector computed once inside the existing projection/light-key rebuild. No
persistent cache, normal approximation, model LOD, ABI field or geometry was
added or changed. Existing lighting-disabled behavior is retained.

Four independent runs per variant in ABBA–ABBA order, using the same inputs
as above and no concurrent builds/tests:

| CPU-side scope | Baseline median (range), ms | Updated median (range), ms |
| --- | --- | --- |
| Total presentation | 8.151 (8.132–8.189) | 8.042 (8.022–8.067) |
| Terrain | 1.230 (1.224–1.242) | 1.154 (1.151–1.159) |

This is a modest **1.34% presentation-time reduction** and **6.21% terrain-time
reduction** in this flight, with non-overlapping four-run ranges. It is not an
FPS guarantee or an isolated GPU timing. Cold peaks overlap (baseline
96–102 ms, updated 93–96 ms); no cold-start improvement is claimed. Draw count
remains seven. Weighted vertex-count averages vary slightly with profiler
window boundaries; no geometry was removed.

All **19 replay captures match exactly across all eight flights**. Before/after
ASan/UBSan GPU sweeps also pass, with all **246 captures byte-identical**,
including six light directions on synthetic and native ground, lighting-off
invariance, held-frame and resource-reset restoration, moving weather/poles,
all six towns and all ten effect toggles. Manifest and before/after binaries:
`/private/tmp/actraiser-nav-light-hoist.Y9zZHg/`. Runs, in order:
`120650`, `120745`, `120812`, `120838`, `120905`, `120932`, `120959`, `121025`
under `runs/20260908-*/`.

### Retained ocean batching and repeated noise control

The closed 48×96 ocean shell now submits its original 9,120 triangle-shaped
quads through 143 bounded `AppendQuads` calls instead of 9,120 `AppendQuad`
calls. The existing portable value-copy API receives the same vertices in the
same order, including the duplicate fourth corner. A 64-quad local buffer is
9 KiB with the current portable vertex type; there is no persistent cache,
allocation, backend storage borrowing, geometry reduction or ABI change.
First/second-batch failures abort the presentation and a subsequent attempt
rebuilds the complete shell; tests verify output and profiling-scope recovery.

Both compared binaries include identical new profiling scopes and the retained
light hoist. The initial ABBA–ABBA set became noisy in its last two flights:
weather, terrain, model and artwork costs all increased. A subsequent read-only
CPU snapshot showed substantial Spotlight/media-analysis activity; this is
evidence of competing load, not proof of the sole cause. No service was stopped
or system setting changed. All eight original runs remain in the evidence.

A complete second ABBA–ABBA set (four new runs per variant), without changing
binary, replay, save, settings or output, produced:

| CPU-side scope | Baseline median (range), ms | Batched median (range), ms |
| --- | --- | --- |
| Ocean including depth setup | 0.16826 (0.16809–0.17488) | 0.15124 (0.15091–0.15199) |
| Total presentation | 8.05251 (8.03238–8.31473) | 8.01632 (7.95425–8.06436) |

The **10.12% ocean-stage reduction is approximately 0.017 ms**, with disjoint
repeat-run ranges. This is the retention criterion; total-presentation ranges
overlap, so **no end-to-end/FPS improvement is claimed**. Unchanged atmosphere
and weather medians were approximately 0.145 and 2.473 ms respectively in both
repeat variants. Seven instrumented depth draws remain; other output calls
are not part of that counter. No cold-start improvement is claimed.

All **19 replay images match exactly across all 16 flights**. The fresh
ASan/UBSan six-town/weather sweep passes and all **246 captures** match the
pre-batching reference. Full CTest: **147/147 pass**, including the new
profiling-enabled presenter variant and batch failure/recovery tests.

Initial manifests, frozen binaries, sanitizer captures, noise notes and the
per-stage/exact-image analyzer: `/private/tmp/actraiser-nav-ocean-batch.gm92O7/`.
Repeat manifest: `/private/tmp/actraiser-nav-ocean-repeat.YT1Mjl/`. All console
logs and replay images remain in the project run directories named there.
Original and isolated populated saves retain their original SHA256.

The newly visible repeat medians also put the remaining work in proportion:
preparation is about 0.005 ms, space 0.011 ms, atmosphere 0.145 ms and mountain
projection 0.302 ms. Weather (2.473 ms), ground and model projection remain
larger targets than further small ocean-only tuning.

### Remaining candidates, not yet measured improvements

1. **Reuse fixed sector directions.** The 96 longitude sine/cosine pairs are
   recomputed across rings in each shell. A tiny immutable table can retain
   exactly those values. This is distinct from the larger moving cloud-UV
   cache experiment that previously regressed; do not resurrect that cache
   merely because weather is the largest named stage.

Keep candidates separate. Require exact image comparisons, moving-weather/
pole and six-town toggle/reset coverage, then at least four independent runs
per variant in interleaved ABBA–ABBA order with identical settings/output and
no concurrent build/test workload. A result inside machine variance is not a
win. Do not lower model detail, terrain resolution or effect quality to make
these optimizations pass. The already-small model compilation and atlas
transfer scopes are lower priorities than the work above.

## Evidence retained

- Debug and optimized builds succeed. Latest full CTest: **147/147 pass**
  (18.16 seconds),
  including the new negative boundary check and terrain light
  direction/toggle/held-frame/resource-reset coverage.
- Atlas-publication follow-up: the failure-injected presenter regression
  passes with ASan/UBSan and profiling enabled. Its complete-image mirror
  checks the failed full-style reversal, frozen recovery, 1 KiB forward/reverse
  patches and the pre-existing failed incremental/clock-rewind path. The
  sanitized six-town/weather GPU sweep passes; all **246 images** match the
  pre-fix reference byte-for-byte. One unchanged optimized populated-save
  replay, `runs/20260908-125647/`, matches all **19 images** from the existing
  reference flight. This is a correctness replay, not a new performance
  comparison or speedup claim. Failing baseline, passing sanitizer binaries,
  image evidence and reproduction notes are retained in
  `/private/tmp/actraiser-nav-art-publication.siWVhg/`.
- Fresh ASan/UBSan GPU executable built from current sources passes the native
  six-town/24-view matrix, all ten independent switch restorations, weather
  sequence, resize/reset checks, crater views and raised-geometry camera cases.
  Diagnostics: `/private/tmp/actraiser-nav-quality-audit.p8d13V/gpu/`.
- Boundary negative probe and per-stage analysis script:
  `/private/tmp/actraiser-nav-quality-audit.p8d13V/`. Only its copied source
  contains the deliberate SDL violation; it was not compiled into the game.
- Benchmark inputs and exact-capture comparison:
  `/private/tmp/actraiser-viewport-cull.xrQXBw/runs.json` and
  `/private/tmp/actraiser-native-lava.AIGLid/analyze.py`.
- Fillmore native-menu round trip: `runs/20260908-111902/`, with flat-navigation
  control `runs/20260908-112052/`; 77 complete public state snapshots match.
- Physical mouse/controller acceptance is not proven by these tests. The Mac
  was locked and no gamepad was connected. Other GPU backends were not tested
  by the local Metal captures. Original saves and checkpoint `380772a` remain
  unchanged.
