# Action environmental effects

The Fillmore Act 1 implementation audit and its performance/portability
validation are recorded in [fillmore-forest-effects-audit.md](fillmore-forest-effects-audit.md).

## Player control

**Effects → Environmental effects** is a live, saved On/Off setting, default
On (`action_environmental_effects`, environment override
`AR_ACTION_ENVIRONMENTAL_EFFECTS`). It controls scenery enhancements in flat
and diorama presentation independently of Action spell lighting/particles.
It is unavailable when the renderer cannot draw effects.

The first implementation gives the control immediate behavior: it owns the
existing map-derived torch accents, Aitos lava lighting/particles and flat-mode
heat refraction, waterfall splash/veil, and diorama waterfall mist. Spell,
projectile, enemy and boss enhancements keep their existing controls. Native
game graphics and simulation are unaffected. Scenery accents now remain enabled
when only the spell controls are disabled; turn Environmental effects off to
remove them.

Source observation and gameplay clocks continue while effects are off, so
re-enabling them uses current state. The waterfall room's authored layer section
is selected independently of the toggle. Off removes decorations from the
published effect frame and skips their flat masks, geometry and heat pass.

The **Fillmore forest** is implemented and visually approved. The first
**Fillmore Act 2 cave/temple/tower pass** is described in
[fillmore-cave-effects.md](fillmore-cave-effects.md). Bloodpool Act 1 now has a
[marsh treatment](bloodpool-marsh-effects.md) with layered moonlight, rose-silver
reflections on the red lake, timber drips, post ripples, bank insects, low mist,
wet timber edges and a slow cloud veil. The [Bloodpool castle pass](bloodpool-castle-effects.md)
adds darker interiors, window light, warm torch bounce, dust and supported floor haze.
Other additions remain planned. They must all honor this same switch,
including any future refraction or atmosphere passes. Existing lava heat
currently applies only in flat mode.

## Fillmore forest prototype

Room `01/01` uses **diagonal godrays from offscreen upper right**, distributed
across a separate scrolling light layer. Twelve authored canopy openings alternate
small groups of fine shafts and broad, softer openings with longer shaded gaps.
Source spacing ranges from 125 to 665 pixels (previously 280–360), half-widths
from 12 to 112 (previously 34–68), and relative strength from 0.32 to 1.00
(previously 0.76–1.00). Their shoulder brightness also varies: broad light has
a fuller profile while fine cracks have a tighter core. Width and intensity
are deliberately independent, so wide openings can be faint and thin rays bright.
Three forest groups share offscreen origins, at world Y −1,024 or −2,048.
The final clearing has a fourth fan with its source at (4,700, −2,560).
Their cores and edges fan outward as they descend, with each group sharing one
sway phase so its source stays connected. The other shafts remain independent.
All sources are fixed in layer/world coordinates; the camera only selects which
ones to draw. Gentle sway and widening toward the ground soften the shafts.
The background rays peak at 0.26 opacity in the forest and 0.32 in the boss
clearing, with transparent edges and dark gaps between openings.

The same openings produce a foreground surface-light pass over actors and
terrain, with peaks of 60% red, 56% green and 39% blue additional brightness
in the forest, rising to 88% / 79% / 53% for the clearing fan.
This preserves black gaps and destination alpha. Only its HUD fade follows the
view; the ray positions and widths scroll with the level. This is a
surface-light approximation, not geometry-aware shadow casting. The background
rays and falling leaves remain behind foreground scenery.

The boss approach has a soft opening followed by three broader, stronger shafts
at light-layer X 3,110 / 3,265 / 3,420 (intercepts at Y=0). They cross the
Centaur's battle lane at rider, horse and ground heights. Deliberate gaps retain
light/shadow changes as the boss moves. Their foreground fade clears the first
32 native screen rows and reaches full strength by row 64, allowing the taller
rider to catch light; the ordinary forest fade is unchanged. The effect is fixed
to the clearing and does not depend on boss position, health or combat state.

Warm ivory motes use up to 22 candidates in stable world-X cells, with slow
upward drift and fades. They are larger and brighter than revision 4 and remain
visible between rays, brightening when they enter the shafts. A second, finer
ivory dust population is visible only inside the light: each ray has four
irregularly placed world-space pockets, with eight independently phased
candidates per pocket. Their radii range from 0.45 to 0.75 pixels, compared with
1.30–1.70 for the original motes. Slow upward/lateral drift and small curls keep
them moving independently of ray sway; their brightness fades with local light
strength and their 1,024-tick lifetime. The same BG2 additive pass and masks draw
them behind trees. The six-ray cull limits fine dust to 192 candidates, with no
additional native records, allocations, passes or uploads.

Falling leaves use 16 candidates, twice the previous count, with larger pointed shapes,
lateral sway and edge-on turning. Each has a dark alpha-blended body and a narrow
olive-gold rim so it remains readable against the dark forest background.

All ray sources and particles use world coordinates with a camera halfway
between BG1 and BG2. Diorama draws background light, then alpha foliage after
BG2 and before foreground terrain and actors. Both use the backdrop's finite
projected footprint and explicit room bounds, so foreground trees occlude them
and the room's empty border stays unlit. Flat mode uses the existing BG2 winner
mask. In forest frames its black pixels are converted to zero alpha in bounded
scratch storage, allowing the same upload to mask both illumination and foliage.
Both passes sample the alpha mask directly as geometry texture coordinates;
no intermediate render target or full-screen mask/composition pass is needed.

Capture validates room dimensions and a fixed metatile readiness signature,
then publishes three records in the existing 16-record decoration list. At most
six ray strips intersect the captured window: any seven sources span at least
1,655 pixels, exceeding a conservative 1,476-pixel reach over the room camera
heights, including fan slopes of 0.50–0.68, width and sway.
Rays are clipped as triangles in source space against the room and the published visible plane before projection.
Intersections interpolate positions and colors; vertices are never pushed onto
the border. This preserves diagonal edges and brightness as beams scroll out of
view. Interior triangles retain shared vertices. No new draw pass, render target
or texture upload is needed.

Before clipping, background light plus both mote populations uses at most
1,036 vertices / 2,004 indices; foliage uses 144 / 240 and foreground light 180 / 720. Clipping can split
a triangle into a polygon with up to seven vertices. The conservative clipped
bound is 2,536 vertices / 4,884 indices for the background pass including motes,
and 1,680 / 3,600 for foreground light. Each fits the existing shared scene
scratch buffer; a per-layer guard permits one forest-ray record. There are no
native-object or per-frame heap allocations. Environmental effects Off skips
field generation; spell settings remain independent. Gameplay clocks freeze
animation on pause and retained frames remain deterministic.

Revision 10 validation, 2026-09-27 (boss clearing):

- Release build and all five focused tests pass. Clipped-field equivalence now
  covers both ordinary forest and the arena. New coverage samples rider, horse
  and floor heights across the battle lane through four sway phases, requiring
  substantial lit coverage, strong highlights and distinct shaded gaps, while
  keeping the foreground HUD rows clear. Both changed C files meet the style
  baseline. The six-ray bound and existing scratch capacity are documented in
  `runs/fillmore-environment-effects-v10/layout-bounds.json`.
- Paired early-forest flat/diorama captures at settings 32 and 64 match native
  WRAM, preserve effects-Off images, and leave checked black gaps and padding
  unchanged. Their effects-On images also match revision 9 exactly, confirming
  the early forest's appearance is unchanged. Supplemental boss stills cover
  diorama at 32 extra rows and flat mode; the primary boss replay uses diorama
  at 64. Evidence: `validation.json`, `early-forest-unchanged.json` and
  `boss-other-views.png` in the revision 10 directory.
- A continuous, silent 20-second boss clip (frames 4000–5198) and previous/new
  lighting comparison show charges through the fan. They run at 30 fps, with
  health/jump assists labeled and no cuts. All fourteen previous/new native
  snapshots through frame 6000 match. An additional Off/On comparison is saved;
  its native snapshots diverge from frame 4200, so the existing asynchronous
  audio comparison limitation remains open. No full replay determinism claim is
  made. Scripts, the previous binary and source captures are retained locally.
- Pipeline counters during the On/Off replays report zero steady presentation
  fallbacks, failures and upload-mirror reallocations (46/48 windows after the
  first action window). Video capture prevents a clean timing comparison, and
  GPU duration is unavailable; no CPU/GPU speed claim is made. See
  `runs/fillmore-environment-effects-v10/pipeline-counters.json`.

Revision 9 validation, 2026-09-27 (fine clustered ray dust):

- Release build and all five focused tests pass. Geometry tests distinguish
  fine dust from the original larger motes, sample the rendered light at every
  fine particle centre to check confinement, preserve the large-mote brightness
  check, and verify retained-frame and clock-wrap equality. The full camera
  sweep checks the updated bounded geometry totals. Both changed C files meet
  the style baseline.
- Paired flat/diorama captures at settings 32 and 64 match native WRAM, preserve
  previous effects-Off images exactly, and leave checked black gaps and padding
  unchanged. Evidence: `runs/fillmore-environment-effects-v9/validation.json`.
- That directory contains a 30-second before/after dust comparison, an effects
  On walkthrough and an Off/On comparison. Each uses normal-speed early, middle
  and boss excerpts at 64 extra rows, labeled cuts at 10s/20s and health/jump
  assists. The comparisons are 1440×492 at 30 fps (900 frames). The first twelve
  paired native snapshots through frame 4100 match both On/Off and revision 8;
  the later 4400 snapshot differs, retaining the documented asynchronous-audio
  comparison limitation. `dust-before-after.png` provides three still pairs.
- An isolated CPU smoke of the complete environmental effects On/Off path
  reports zero steady presentation fallbacks, failures and upload-mirror
  reallocations. Flat median PPU/capture plus presentation adds about 0.27 ms;
  diorama samples are noisy enough that On measures faster, so no speedup is
  inferred. This does not isolate the dust's cost, and GPU timing is unavailable.
  Raw samples and exclusions are in `runs/fillmore-environment-effects-v9/`.

Revision 8 validation, 2026-09-27 (varied layout and shared fan origins):

- Release build and all five focused capture, observer, geometry, presentation
  and render-device tests pass. A rendered-field test checks two separate rays
  across three sway phases: their separation grows downward and both widths
  grow in the same proportion, placing their common source above the view.
  Existing clipping, scrolling, pause and camera-budget coverage remains intact.
- The two changed C files meet the style baseline. Source spacing is 125–665
  pixels, half-width parameters 12–112 and strength 0.30–1.00. Three fan groups
  coexist with isolated shafts. The conservative five-ray bound is verified in
  `runs/fillmore-environment-effects-v8/layout-bounds.json`; scratch capacity,
  draw passes and uploads are unchanged. This revision adds no new timing claim.
- Paired flat/diorama captures at settings 32 and 64 match native WRAM, preserve
  previous effects-Off images exactly, and leave checked black gaps and room
  padding unchanged. Evidence: `runs/fillmore-environment-effects-v8/validation.json`.
- Updated silent 30-second walkthrough and Off/On comparison are in
  `runs/fillmore-environment-effects-v8/`, with normal-speed early/middle/boss
  excerpts, 64 extra rows, labeled cuts at 10s/20s, and health/jump assists.
  The comparison is 1440×492 at 30 fps (900 frames). Twelve paired WRAM snapshots
  through frame 4100 match; the later 4400 snapshot differs, so the existing
  asynchronous-audio comparison limitation remains unresolved. The previous
  more regular layout and this revision are shown in `variation-before-after.png`.

The following revision 6 evidence covers the retained triangle-clipping fix,
with the earlier source arrangement:

Revision 6 validation, 2026-09-27:

- The edge/boss beam bends came from clamping individual vertices to room and
  capture-plane bounds. Earlier bounds-only tests missed the distortion.
  New tests sample the triangle/color field across eight rectangular crops for
  both light layers, checking that interior brightness and shape are preserved,
  and that fully clipped beams disappear. Scrolling is checked by sampling the
  translated light field instead of requiring identical vertex layouts.
- The release build and five focused capture, observer, geometry, presentation
  and render-device tests pass. Existing fractional-plane bounds, particle
  rejection, pause, clock-wrap and full-camera geometry-budget checks remain.
- Seven changed C/header files satisfy the style baseline. Paired flat/diorama
  captures at settings 32 and 64 preserve previous effects-Off images exactly,
  match native WRAM, and leave checked black gaps and room-edge padding intact.
  Evidence: `runs/fillmore-environment-effects-v6/validation.json`.
- The 30-second walkthrough and Off/On comparison in
  `runs/fillmore-environment-effects-v6/` show early forest, middle forest, and
  boss approach/arena (frames 3800–4398). They use 64 extra rows, normal-speed
  excerpts, labeled cuts at 10s/20s and health/jump assists. A before/after still
  at frames 1100 and 4400 is `clipping-before-after.png`. The twelve paired WRAM
  snapshots through frame 4100 match; the later 4400 boss snapshot differs in
  this run, consistent with the previously documented unresolved asynchronous
  audio comparison limitation. No full-replay determinism claim is made.
- The isolated CPU smoke reports zero steady presentation fallbacks, failures
  and upload-mirror reallocations in both views. Flat On/Off adds roughly
  0.12 ms to median PPU/capture plus presentation; diorama timing is noisy
  enough that On measured faster than Off, so no speedup is inferred. GPU
  duration remains unavailable. Raw samples are in
  `runs/fillmore-environment-effects-v6/perf-summary.json`.

The following revision 5 results describe the superseded vertex-clamping path:

Revision 5 validation, 2026-09-27:

- Release build and five focused capture, observer, geometry, presentation and
  render-device tests pass. The changed C files meet their style baseline.
  A regression test verifies that both ray layers move left by the mean camera
  delta instead of staying fixed onscreen. A complete camera/height sweep
  verifies geometry limits; particle coverage checks brighter motes, consistent
  leaf presence, dark bodies, lit rims and continuity across the 16-bit clock
  wrap. Existing pause, clipping and environmental-setting checks remain in
  place.
- Paired flat/diorama captures at settings 32 and 64 match native WRAM and
  preserve the previous effects-Off images exactly. Checked black gaps and
  room-edge padding remain unchanged. Evidence:
  `runs/fillmore-environment-effects-v5/validation.json`.
- The refreshed silent 30-second walkthrough and Off/On comparison are in
  `runs/fillmore-environment-effects-v5/`, with the same three normal-speed
  forest excerpts, 64 extra rows, labeled cuts at 10s/20s, and health/jump
  assists. All thirteen paired WRAM snapshots through frame 4400 match in
  this run, including the twelve covering the review footage. This does not
  resolve the earlier asynchronous-audio timing finding documented below.
  Scripts, inputs and source captures are retained alongside the videos.

The following revision 4 results describe the superseded broad wash and
camera-anchored foreground shaft:

Revision 4 validation, 2026-09-27:

- Release build and all five focused capture, observer, geometry, presentation
  and render-device tests pass. Fourteen touched C/header files meet their
  style baseline. New
  coverage verifies a broad light footprint, brighter bounded motes,
  deterministic dark foliage, additive/alpha draw ordering, alpha winner-mask
  conversion, premultiplied composition, target reuse, state restoration,
  foreground shaft strength/draw ordering, and the shared environmental setting.
  Existing world/plane bounds and pause
  checks remain in place.
- Paired flat/diorama captures at settings 32 and 64 preserve the previous
  effects-Off images exactly and produce identical native WRAM snapshots.
  Diorama logs confirm 32/64 extra rows; flat framing remains unchanged.
  Black gaps and room-edge padding remain unchanged in the checked images.
  Evidence: `runs/fillmore-environment-effects-v4/validation.json`.
- Review evidence is saved under `runs/fillmore-environment-effects-v4/`.
  The matching 30-second walkthrough and Off/On comparison use the same early,
  middle and late forest route as revision 2, with 64 extra rows, labeled cuts
  at 10s/20s, and health/jump assists. Scripts and source inputs are retained.
  All twelve paired native WRAM snapshots through frame 4100 match, covering
  the review clips. The later asynchronous-audio caveat below remains separate.
- A short isolated CPU smoke at 720×448 measured median PPU/capture plus
  presentation deltas of about 0.28 ms flat and 0.19 ms diorama. The first
  action window was omitted from each run. Steady windows report zero
  presentation fallbacks, failures and upload-mirror reallocations. These are
  noisy local measurements; GPU duration remains unavailable. Raw evidence:
  `runs/fillmore-environment-effects-v4/perf-summary.json`.

The following revision 2 results describe the superseded repeated shafts:

Revision 2 validation, 2026-09-27:

- Release build and all five focused effect capture, observer, geometry,
  presentation and diorama projection tests pass. The ten touched C/header
  files satisfy their style baseline. Tests cover independent parallax,
  directional geometry, pause clocks, stable identities over the full camera
  range, bounded record use, read-only WRAM, optional settings, finite room and
  fractional plane bounds, and rejection of invalid clipping rectangles.
- Paired flat/diorama captures with settings 32 and 64 pass with spell effects
  disabled. Diorama logs confirm the requested extra rows; flat mode retains its
  normal framing. Environmental effects Off images match the previous Off
  baseline exactly, and paired native WRAM snapshots match. Revised room bounds
  keep illumination out of the side padding. Evidence:
  `runs/fillmore-environment-effects-v2/validation.json`.
- The revised silent 30-second walkthrough and matching Off/On comparison are
  `runs/fillmore-environment-effects-v2/fillmore-forest-effects.mp4` and
  `fillmore-forest-effects-comparison.mp4`. They show three normal-speed
  10-second excerpts from the early, middle and late forest, with labeled cuts
  at 10s and 20s. Extension is 64; health/jump assists simplify traversal and
  native knockback remains enabled. All twelve paired WRAM snapshots through
  frame 4100 match, covering the review clips. Scripts, inputs, source captures
  and metadata are retained alongside the videos.

- A short local CPU smoke at 720×448 measured median PPU/capture plus
  presentation deltas of about 0.40 ms in flat mode and 0.80 ms in diorama mode.
  Warmup windows and one Off run overlapping video encoding were excluded;
  these are noisy local observations, not a cross-device guarantee. All measured
  steady windows report zero presentation fallback, failures and upload-mirror
  reallocations. GPU duration remains unavailable in this backend. Raw logs,
  exclusions and samples: `runs/fillmore-environment-effects-v2/perf-summary.json`.

The initial BG2-anchored prototype was superseded after visual review requested
an offscreen directional source and more separation from the backdrop. The
following measurements and diagnostic apply to that initial version, not the
revised geometry:

Initial prototype validation, 2026-09-27:

- Release build and all four focused capture, effect, geometry and presentation
  tests pass. The ten touched C/header files satisfy the existing style
  baseline. The final whole-workspace style run reports two long lines in
  concurrent, unrelated `tests/sim_background_voxel_models_test.c` edits.
  Tests cover read-only WRAM capture, malformed/changed source rejection,
  pause clocks, deterministic retained frames and plane-edge bounds, including
  fractional cropped-plane UVs.
- Paired flat/diorama captures at extension 32 and 64, with spell effects Off,
  match the pre-effect build exactly when Environmental effects is Off. On/Off
  native WRAM snapshots also match. All eight anchor signatures match the live
  room data. Evidence: `runs/fillmore-environment-effects/validation.json`.
- A short local CPU smoke measurement at 720×448, extension 64, uses two Off/On
  pairs per view, omitting the first measurement window of each run. Median
  PPU/capture plus presentation increases by about 0.18 ms in flat mode and
  0.11 ms in diorama mode. No presentation fallback or upload-mirror
  reallocation is reported after warmup. These are local observations, not a
  cross-device performance guarantee. GPU duration is unavailable in this
  backend and remains unmeasured. Raw logs and samples are in
  `runs/fillmore-environment-effects/perf-results.json`.
- Video review: `runs/fillmore-environment-effects/fillmore-forest-effects.mp4`
  and `fillmore-forest-effects-comparison.mp4` are silent 30-second, 30 fps
  reviews of the early, middle and late forest in diorama mode with extension
  64. The comparison places Off on the left and On on the right. Each contains
  three normal-speed 10-second excerpts, with cuts at 10s and 20s. Infinite
  health and jump assists simplify traversal; native knockback remains enabled.
  The source capture reaches the boss arena. Twelve paired native WRAM
  snapshots through game frame 4100 match exactly, covering the review clips
  (800–1398, 1750–2348, 3450–4048). Capture/replay/encoding scripts and
  `video-review.json` are saved alongside the videos.
- A later boss snapshot at frame 4400 differed by one movement tick with the
  normal asynchronous audio consumer running: the Centaur's X at `$0BA2` was
  3986 with effects On versus 3987 Off, alongside its timer, OAM and scratch
  bytes (46 differing bytes total). Both repeated runs reproduced it; the
  committed pre-shaft binary with effects On matched the Off state. Removing
  only `AudioSession_StartOutput` in a private diagnostic executable made all
  13 On/Off snapshots, including frame 4400, byte-identical, with boss X 3986.
  Pure headless runs also matched On/Off. This isolates an asynchronous audio
  dependency in the comparison; it is not evidence of scenery allocating
  native enemy objects. The precise audio/handshake timing mechanism remains
  a separate follow-up, not a claim of full replay determinism. The shipping
  audio path is unchanged. Evidence lives in `video-repeat-{on,off}`,
  `video-baseline-on`, `video-headless-{on,off}-probe` and
  `video-no-consumer-{on,off}` under the same local evidence directory.

The forest revisions and final implementation audit are approved. Fillmore Act 2
now has a first water, drip, dust and tower-light pass awaiting visual review.

## Art direction

Use one principal environmental effect and at most one quiet supporting accent
per room. Preserve the native palette, deep shadows and readable silhouettes.
Attacks, hazards and pickups should remain more visually distinct than ambient
particles. Prefer local effects with believable sources; keep the HUD clear.

| Environment | Candidate treatment |
| --- | --- |
| Fillmore forest | World-anchored godrays scroll through uneven canopy openings behind trees; a foreground pass illuminates actors/terrain. Bright drifting motes, fine dust pockets inside rays, and falling leaf silhouettes with lit edges. |
| Fillmore caves | Slow drips from selected stalactites/damp ledges, gentle pool ripples, cool surface glints; scene refraction remains a future experiment. |
| Stone interiors | Local window light and slowly drifting dust where openings justify it. |
| Bloodpool exterior | Layered moonlight, rose-silver glints and ripples on red water, thin violet mist, timber drips and bank insects, behind foreground scenery and actors. |
| Bloodpool castle | Cool light through windows, balanced against the existing warm torch accents. |
| Kasandora desert | Mild distant-background heat shimmer and occasional low sand wisps. |
| Kasandora interiors | Isolated warm shafts and dust near visible openings. |
| Aitos bamboo | Subdued moving canopy light and occasional leaves. |
| Aitos lava/waterfall | Preserve the existing signature effects; refine local environmental light spill before adding more activity. |
| Marahna jungle/temple | Warm green-gold canopy light and faint haze in distant vegetation/ruins. |
| Northwall | Sparse drifting snow/spindrifts outdoors; restrained cool highlights in icy rooms. |
| Death Heim | Subtle halos tied to existing portals/eclipses and very slow distant wisps; preserve the deep blacks. |

## Water and drips

Water needs explicit room/source regions and surface heights. Identify or author
those from decoded room tiles, never by treating every blue pixel as water.
Share the renderer while giving still cave pools, rippling Bloodpool surfaces
and falling Aitos water distinct parameters.

- Glints follow surface motion and local illumination; cave reflections stay
  sparse and cool rather than resembling glitter or pickups.
- Future refraction must be clipped to water and distort scenery visible through
  it. The water-artwork-only CPU prototype was removed after its On/Off review
  showed negligible benefit; stronger displacement of the same flat fill is not
  sufficient. Shorelines and above-water actors stay crisp. Caustics are a later optional
  addition for plausibly illuminated submerged rock or nearby surfaces.
- Drips gather briefly at chosen ceiling/ledge anchors, detach and fall, then
  make a small ripple on water or a brief splash on rock. Pre-authored landing
  surfaces are sufficient for the first prototype; no new gameplay collision
  objects are needed.
- Stagger drip timing and cap visible drops. Background drips can establish
  depth; foreground drops must remain quiet enough not to resemble projectiles.

## Rendering and lifecycle constraints

Keep sources anchored to room/world coordinates, projected through their owning
layer, with explicit occlusion and water masks. Background light is occluded by
foreground terrain; intentional foreground light should brighten surface colors
without filling black gaps. Effect bounds follow actual captured rows and per-layer
world limits, including short rooms and Aitos's existing waterfall cap.

Use separate bounded host-side storage; never allocate ambient effects in the
native enemy/object pool or displace combat-effect records. The existing
16-record decoration list can already be full in the waterfall scene, so new
ambient families require an explicit capacity plan, not unconditional appends.
Use stable seeds and gameplay time, freeze on native/host pause, and reset or
reconstruct correctly at room/load boundaries. Re-presenting a retained frame
must not advance the simulation.

Batch geometry by layer/material. Gate new capture/generation work early when
the setting is off, while retaining any metadata needed for room layout. Scope
refraction/haze passes to relevant layers or regions and measure their GPU cost;
do not assume that a small particle count makes a large translucent pass cheap.

Carry forward the [Deck/D3D12 constraints](fillmore-effects-audit.md#steam-deck-and-sdld3d12-constraints)
for every new environment. Bound texture-update calls as well as bytes; avoid
per-row/per-emitter updates, new optional device requirements, upload-memory
reads and extra full-screen resolves. Keep supported resource/blend fallbacks.
Mac compilation or CPU timing does not establish performance on Deck/Vulkan or
Windows/D3D12: retain explicit native On/Off acceptance, including frame-time
spikes, the packaged SDL version, and fixed power/pacing settings.

## Suggested implementation order

1. Review the implemented Fillmore canopy-shaft prototype and tune its strength
   using matching original/enhanced gameplay videos.
2. Add a shared water-region description, cave glints/ripples and sparse drips.
3. Add Bloodpool water mist and masked Kasandora background heat shimmer.
4. Assess localized water refraction, then adapt the approved primitives to the
   other environments.

Acceptance for each effect includes On/Off behavior with spell settings in both
states; flat and diorama views; 32/64 extension and world edges; camera movement;
pause/re-present/load boundaries; visible sprite/platform readability; and CPU,
GPU and allocation measurements. These checks apply as each visual is added,
not as a claim that the planned effects already exist.

For every level as its effects are implemented, generate a short gameplay video
traversing representative affected scenery for the user's visual review. Include
normal-speed motion and a matching effects-Off comparison; identify any cuts or
developer movement assists used in the capture. Save the video and reproducible
replay/capture details with the local validation evidence.
