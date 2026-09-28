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
An additional immutable payload of less than 1 KiB holds at most 64 timber edges
and 32 posts. Timber classification verifies all four native tile-definition
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
