# Bloodpool Act 1 environmental effects

2026-09-28. First treatment of the moonlit marsh, room `02/01`, controlled by
**Effects → Environmental effects** independently of spell lighting/particles.

## Visual treatment

Low violet mist stays close to the eight foreground lake spans. Stable
world cells vary patch width, height, opacity, lean and slow drift; dry bank gaps
remain clear. Rose-silver horizontal reflections vary width, phase, cadence and intensity
along the exposed water, preserving the lake's red-magenta character. A brighter core,
soft surrounding shimmer and nonzero minimum exposure keep shoreline glints
visible at reduced review resolutions. These are surface accents, not a
scene-refraction effect. No global scene or sprite brightening is added.
The mist is confined to roughly 40 native pixels above the foreground water;
climbing higher naturally moves this low atmosphere out of view.

Six localized details complete the marsh treatment:

- Occasional drops form beneath verified timber silhouettes, accelerate toward
  the first solid pixel below, and make expanding rose ripples when they reach
  exposed water. Drops stop on intervening platforms.
- Submerged timber posts produce irregularly phased ripple pairs. Anchors require
  both native low-priority pixels at Y=479 and a recognized post-water metatile;
  adjacent silhouette fragments merge into one anchor.
- Sparse insect clusters hover near banks over open water. Moon exposure picks
  out their small warm highlights, with infrequent faint glows.
- Extra mist gathers at water level beneath low walkways with a clear path to
  the lake. It follows the same shoreline clipping as the base mist.
- Short silver highlights follow exposed timber top runs, receiving the existing
  moonlight and softened platform shadows.
- A soft cloud veil crosses the moon on a 16,384-tick cycle (about 4.5 minutes at
  60 Hz). Its shallow shading also modulates the rays, reflections and wet timber;
  it never extinguishes the scene.

The spans, decoded from BG1 at world Y=480, are X ranges 176–880, 960–1184,
1248–1360, 1440–2144, 2240–2368, 2464–2560, 2688–2864 and 2912–4096. Capture
checks room identity, both layer dimensions, separate bank/timber signatures and
every water metatile in each span. Changed material drops that span. Incomplete
or inherited room data publishes no marsh effects. This is authored water
placement validated against native maps, not blue-pixel detection.

## Moonlight and occlusion

The visible moon is centered at BG2 (112,62); BG2's camera stays at (0,0) on
this route. Light remains attached to that source while BG1 platforms scroll.
Two overlapping ray families spread from that common origin: six broad rear
lobes reach the lower lake, while five narrower, brighter middle fans fade out
around the platform area. Their widths, spacing, strength and reach differ.
The middle contribution has a .40 exposure multiplier versus .24 for the rear
family, keeping it readable where the fans overlap.
The renderer clips triangles at the published plane bounds, avoiding endpoint
clamps and screen-edge bending. Slow intensity variation follows the paused
scene clock.

Capture preserves the actual low-priority BG1 silhouette as merged opaque
pixel rectangles, including native tile priority, H/V flips and transparent
holes. High-priority scenery is excluded from this background shadow field.
The 2 KiB opacity catalogue belongs only to validated Bloodpool 02/01 graphics.
Room, map, definition and moon signatures reject inherited/unsupported data.
The camera window is bounded to 768 pixels (rounded out to whole CHR tiles) by
352 rows; it publishes at most 2,048 rectangles, or fails closed. Definition
words are decoded once per eight-row band. Native memory is never modified.

Presentation projects those silhouettes through the actual BG1 plane and the
moon through BG2. This matters in Diorama, where their different depths change
both scale and screen position. Background projections also include the exact
uniform translation of a successfully generated in-between texture. Paused,
discontinuous, disabled and failed generation clear that translation; OBJ
motion is not approximated with a background offset.

The light mesh stays fixed at 129 angular columns and 17 radial rows. Visibility
changes its alpha, never its vertex positions or endpoints. The projected
silhouettes are rasterized into an 800-by-224 CPU coverage field, with
fractional horizontal coverage and two vertical samples per row. Bilinear
coverage sampling softens subpixel movement. Nine deterministic, stratified
samples across a six-pixel moon disk create penumbras and partial illumination
through narrow gaps. No animated sampling noise or lagging temporal smoothing
is used. Adjacent mesh strips reuse shared vertices.

This is a layered 2.5D approximation. The rear haze receives light before it
reaches the foreground platform plane; native painter order hides it only where
opaque platform artwork covers it. The middle fans sample three finite haze
depths, using authored light-to-platform intersection ratios of .74, .85 and .96.
Those fans receive local shadows, rather than the infinite shadow below the
first screen-space blocker used by the previous implementation. Light can
reappear beneath a ledge, and a foreground post covering the visible moon does
not extinguish all the haze behind it. Both ray families share the same mesh
and additive batch; they are not separate volumetric or GPU passes.

The low rays also illuminate the exposed foreground water at world Y=488–511,
clipped to the same eight validated water spans. A five-row mesh samples every
four native pixels; a soft vertical profile preserves the shoreline and lower
tile boundary. It adds a rose-silver light footprint over the original animated water
art, alongside the existing independent glints. Its angular profile is shared
with the long rays. Three projected BG2 reference directions recover the ray
slope under perspective, so the footprint follows the actual BG1-high receiver
instead of comparing coordinates from different layers.

The surface receives local shadows at an authored .88 light-to-platform
intersection ratio, softened by the nine moon-disk samples. Unlike the haze,
this is one receiving depth rather than an integration through three depths.
Opaque silhouettes suppress the added illumination and transparent gaps admit
it. This remains a 2.5D depth approximation. Each receiving layer rebuilds the
shared CPU coverage scratch for its own projected bounds; there is no persistent
cross-callback shadow cache that can lag scrolling or generated frames. Water
light joins the existing BG1-high batch and uses its existing flat winner mask.

The moon fan and its broader, broken reflection trail over the distant lake are
submitted after BG2-low and before foreground scenery. Native platforms, banks
and actors occlude both. Flat presentation reuses the existing BG2 winner mask.
The captured silhouette payload is about 16 KiB; the retained render scratch is
about 201 KiB. Scratch is part of the existing presentation workspace, with no
per-frame allocation or large thread-stack buffer. The renderer's ordinary
capacity checks still reject overflow. No new shader, texture, scene resolve,
readback or backend-specific rendering path is required.

Regressions cover exact opaque/transparent pixels, native priority and flips,
world anchoring, source invalidation, malformed/capacity-exceeding silhouettes,
local middle-fan shadowing, lower-beam recovery beneath a ledge, a foreground
post covering the moon without a scene-wide blackout, partial transmission
through a four-pixel opening,
subpixel motion without topology changes, separate BG1/BG2 transforms, clock
wrap, and clipping at thin viewport-edge slivers. Frame-generation tests cover
both endpoint owners, multi-tick pairs and clearing offsets when disabled.
Water regressions cover beam/receiver alignment under perspective and independent
foreground movement, opaque and open platform silhouettes, source invalidation,
native water bounds and the paused clock's periodic wrap.
Detail regressions cover verified timber definitions, drops stopping at solid
surfaces, submerged-post anchors, warm ripple colors and lake bounds, wet-edge
alignment, smooth cloud shading and periodic motion.
Review clips remain 480 pixels wide / 20 fps with a bitrate cap for remote access.

## Layer ownership and cost

The repeating BG2 sky/lake has its own raster movement and cannot supply the
world position of the foreground shoreline. Both effects use BG1 coordinates;
reflections use its exact priority-1 plane. Mist projects at BG1's source depth
but is submitted in the BG2-high callback slot, after low-priority bank geometry
and before BG1-high water/scenery. The regression checks this callback explicitly.
The distant lake itself is BG2-low, as confirmed from its native tile words.
Later high-priority foreground scenery and actors remain in front of the mist.
Flat rendering uses native BG2-winner alpha masking for mist and BG1-winner
masking for reflections, excluding actor/foreground winners. The BG1 mask uses
the existing packed visible-main-winner path, avoiding the owning-screen
reference sampler for this main-screen-only room.

Capture publishes seven bounded aggregate records, separate from actor slots.
An additional immutable payload of about 1.3 KiB holds at most 64 timber edges,
32 posts and 96 native water-row offsets. Timber classification verifies all four native tile-definition
words; exact opacity and tile flips determine exposed tops and drop undersides.
The camera window rounds out to whole metatiles, at most 784 pixels wide.
Rendering uses the existing geometry workspace and alpha/additive submissions;
there are no new shaders, textures in Diorama, intermediate render targets,
readbacks or per-frame allocations. Flat mode reuses the existing two persistent
winner-mask textures; each changed mask uploads at most one rectangle and
unchanged masks upload nothing. It adds five geometry submissions; moon rays
and distant-water reflections share one BG2-low additive batch. Insects and
drops share the existing mist alpha batch; ripples share the water batch.
The cloud alpha layer and wet-timber additive layer account for the two new
submissions, without adding textures or target resolves.

Geometry is clipped as triangles at the shoreline and published plane boundaries,
so moving beyond the viewport does not clamp individual vertices or bend the
shape. Hashes are world-cell-based; animation follows the paused scene clock and
all periods divide its 16-bit wrap. The render source window is 768 pixels wide.
Duplicate aggregate fields fail closed and compile-time bounds cover worst-case
mist clipping. The former `Bg2Foliage` layer is named `Bg2Alpha` to describe its
blend contract; forest behavior is unchanged.

## Validation and review

- Release build and 15 focused effects, presentation, projection, frame-generation,
  PPU, upload and SDL/device/boundary tests pass. The existing macOS `__DATA`
  alignment warning remains.
- Capture, geometry and presenter suites pass ASan/UBSan. Tests also preserve
  immutable WRAM, settings independence, callback order and direct flat
  composition without render-target support.
- Affected capture/render modules compile under strict C11 for arm64 and x86_64
  macOS. This is object compilation, not native Windows, Linux or Deck execution.
- CPU measurements and geometry counts including all six details are
  recorded in `runs/bloodpool-marsh-detail/perf-results.txt`. These probes
  use the captured BG1/BG2 cameras and include all environmental render layers.
  All environmental geometry takes about 0.56–0.71 ms; capture including existing
  actor observation takes about 0.08–0.11 ms. Geometry peaks at 5,318 vertices /
  23,013 indices across the layers. The moonlight builder plus its shared coverage
  helper uses less than 2.5 KiB of stack on both compiled architectures. These
  measurements are CPU construction, not GPU execution or cross-device performance.
- The 64-row Diorama On/Off replay preserves all seven paired native WRAM
  snapshots. The 720 captured source frames cover three normal-speed eight-second
  sections: early walkways, middle crossings and final approach. Assisted
  traversal, infinite health and scene cuts are labeled. The flat 32-row On/Off
  replay also preserves all seven native WRAM snapshots and completes without capture/session errors.

Review media, source-map inspection, CPU probes, compiler/sanitizer logs and
pixel/WRAM comparisons are under `runs/bloodpool-act1-effects/` (ignored local
artifacts). `bloodpool-act1-effects.mp4` shows the treatment;
`bloodpool-act1-comparison.mp4` shows matched Off/On footage. This is an atmosphere
review route, not a complete normal-play traversal or boss-combat validation.
Native Deck/Vulkan and Windows/D3D12 GPU profiling with the shipped SDL version
remains pending under the existing effects-audit acceptance criteria.

The moonlight revision's media, replay checks and CPU/sanitizer measurements are
in `runs/bloodpool-moonlight/`. `bloodpool-moonlight-small.mp4` is the 24-second
review; `bloodpool-moonlight-comparison-small.mp4` compares the previous treatment
with this revision. Both are under 1 MB. Review routes still use labeled assisted
traversal rather than demonstrating an ordinary complete playthrough.

The scrolling/transparency correction is reviewed in
`runs/bloodpool-moonlight-stability/`. Its compact video compares the fixed mesh
and projected silhouettes against the earlier sparse first-hit implementation.
That earlier implementation could jump entire ray endpoints, miss cutout pixels,
and disagree with the displayed platform position in Diorama.

The layered-depth revision is reviewed in `runs/bloodpool-moonlight-depth/`.
Its compact clips show the long rear rays and shorter middle fans, and compare
them with the stable but infinitely shadowed previous treatment. Both clips
remain below 1 MB. The 64-row Diorama and 32-row flat replays each preserve all
seven native WRAM snapshots against the effects-off baseline. As above, these
are assisted atmosphere review routes rather than normal-play validation.

The foreground-water revision is reviewed in `runs/bloodpool-moonlight-water/`.
`bloodpool-moonlight-water-small.mp4` shows the added receiving-surface light
and brighter middle rays at 480 pixels wide, 20 fps and 24 seconds (612 KB).
All 14 native WRAM snapshot pairs match current-build effects-off baselines
across the 64-row Diorama and 32-row flat routes.

The marsh-detail revision is reviewed in `runs/bloodpool-marsh-detail/`.
`bloodpool-marsh-detail-small.mp4` includes all six additions and the warmer
red-lake treatment: 480×338, 20 fps, 24 seconds, 613 KB. All 14 complete native
WRAM snapshots match the preceding revision's effects-off baselines across the
same 64-row Diorama and 32-row flat routes. Native replay reports no capture or
session errors. `validation.json` records the comparisons and media metadata.
The repository style check reports existing violations in unrelated SIM voxel
files; the changed Bloodpool sources introduce none.

### Distant water wave caps — September 30

Twelve sparse rows of tapered rose-silver caps now catch moonlight on the BG2
lake, with small intermittent glints along the crests. Width increases toward
the viewer and exposure is strongest beneath the moon. Native red-purple water
remains visible between the highlights; no white foam or detached airborne
sparkles are added. Environmental effects controls the treatment.

Each cap moves with its native water row. Capture validates the retained
`$6000` HDMA table (127 fixed sky rows followed by 96 single-row offsets), then
copies its 10-bit offsets into the immutable frame. The 256-pixel artwork repeat
also repeats the caps. Rows below the table retain the last offset, as native
scanout does. The scene clock changes brightness only, so pausing, hit-stop and
camera movement cannot introduce a separate lateral drift. Invalid raster data
omits the caps while preserving the existing marsh effects.

Caps stay within individual source scanlines and below the distant horizon.
They use BG2's existing projection and winner mask, including skybox-only mode,
and share the moon/reflection additive submission. The addition needs 192 bytes
of scroll values and a validity flag per captured frame, bounded geometry, and
no extra GPU submission, texture, shader, native object or per-frame allocation.

Release and ASan/UBSan capture, geometry and presenter tests pass. New tests cover
exact per-row translation, seamless 256-pixel wrapping, retained-frame stability,
independent shimmer, malformed tables and combined ray/reflection geometry limits.
The changed sources pass arm64/x86_64 macOS syntax checks and the style ratchet.
Native Metal captures cover skybox-only and backdrop-plane modes; all four
before/after native WRAM snapshots match. Deck/Vulkan and D3D12 execution remain
part of the broader hardware validation, not established by these checks.

`runs/bloodpool-wavecaps/bloodpool-wave-caps-small.mp4` is the 480×300, 20 fps
before/after preview (238 KB), including a backdrop-plane segment. It uses an
isolated save/settings directory and labeled assisted traversal.

### Act 1 boss fireballs — October 3

The boss's launched fireballs now use the existing `fireball-field` warm glow
and twelve fading ember streaks. Trails follow velocity; the captured room OBJ
priority keeps the effect attached in Diorama. Lighting and particles remain
independently controlled by their existing Effects settings. The room editor's
`fireball-field` response can tune this effect without a new recipe format.

Launched boss shots also shed broad smoke billows every four gameplay ticks.
Two overlapping soft lobes start at roughly the fireball's size, expand, curl
upward and fade over 144 ticks (2.4 seconds). Their world-space anchors remain
after the projectile disappears. A bounded, captured pool of 96 puffs freezes
on pause and clears on room changes; recycled actor slots do not bridge trails.
The existing moon-ray field and cloud transmission tint exposed smoke pale
blue. This is scattered light on airborne smoke, without foreground surface
shadow masking. Smoke uses alpha blending and the Particles switch, including
the `fireball-field` particle component; it remains neutral grey when the moon
effect is disabled. Both CPU and deferred projection use the captured tint.

Capture validates the `$B786` family, `$B90D` flight handler, state 1, both
authored fireball compositions and the retained boss identity in room `02/01`.
Stationary formation, boss-body poses and death fragments do not acquire a
flight trail. The exact native identity is recorded in `ram-map.md`; observation
does not write WRAM or allocate native actors.

The five focused capture, geometry, projectile-recipe and presenter tests pass,
including deferred projection. Capture, geometry and presenter tests also pass
ASan/UBSan, and the game build succeeds. Tests cover four retained spawn resumes,
both artwork frames, both facings, room-supplied depth, pause, retirement,
invalid identities, detached smoke lifetime, pool capacity and moonlight tint.
Review regressions compare one-, two-, four- and eight-tick capture intervals
and preserve the smoke's Diorama OBJ plane after the last projectile retires.
Isolated assisted Metal replays in `runs/bloodpool-act1-boss-fire/` show the
effect in flat and Diorama views. All four paired full-WRAM snapshots from the
Diorama effects-on/off replay match. These runs pin player position to exercise
the boss; they are visual checks, not an unassisted gameplay walkthrough.
The final billowing smoke was visually checked in flat and Diorama views; the
final Diorama replay also matches all four reference WRAM snapshots.
`runs/bloodpool-act1-boss-fire/billowing-smoke-preview.mp4` shows the final
Diorama trail at 20 fps.
The repository-wide style gate has existing violations; changed C files add none.

The 96-puff smoke pool and its separate render pass leave actor geometry
unchanged at the full 16-actor, three-sword-stream limit. A longer assisted
boss replay did reproduce the missing sword enhancement: 22 live boss
fireballs, only one visible, overflowed the 16-record actor list and cleared
every actor accent. Native shots keep flying outside the activation window.
Their observer tracks now continue without publishing invisible boss shots
into the render list. Regression coverage retains the beam, four visible
fireballs and full smoke pool alongside twenty invisible fireballs.
The corrected long replay restores the sword glow and extended trail with 22
native fireballs still live; `runs/bloodpool-act1-boss-fire/sword-restored.png`
records the result. The long before/after runs differ in native timing state,
so this comparison establishes visual recovery, not byte-exact WRAM parity.

Native OAM in the assisted boss replay also places the player beam in priority
2; its enhancement now inherits `$008F` instead of assuming priority 0. All
101 paired WRAM snapshots match before and after that projection correction.

Fireball effects now survive the root boss's death-state change to `$A593`
and flags `$0032`. Recognition checks the child's flight identity and retained
root backlink at first admission, without requiring the parent to remain active
or keep its boss flag and composition. The shared lifecycle contract now retains
that admission if the root slot is subsequently reused as well; the child's own
identity and continuity govern its remaining lifetime. Cold observation still
requires retained root evidence. See [projectile identity and lifetime](ram-map.md#projectile-identity-and-lifetime).
A moving shot keeps its normal trail.
When its world position stops changing, its effect clocks and owned smoke
puffs freeze; clearing native velocity preserves the last trail direction.
Frozen smoke retires when that shot becomes hidden, disappears or is replaced.
Ordinary detached smoke still fades, and each shot advances independently.

Capture regressions cover a 180-tick stop, movement resuming, a retired parent,
fresh observation during death, independent neighbouring shots, visibility
and slot reuse. Capture, CPU/deferred geometry and presenter checks pass under
ASan/UBSan. Assisted Metal replay `20261003-200802` confirms that the launched
fireball retains glow, embers and smoke during the boss's death animation;
`runs/bloodpool-act1-boss-fire/death-fireball-after.png` records the result.
All 57 paired full-WRAM snapshots match the earlier replay.
