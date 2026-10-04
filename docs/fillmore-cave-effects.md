# Fillmore Act 2 environmental effects

Environmental treatment, updated 2026-09-30. Environmental fields use **Effects → Environmental effects**
and are independent of Action spell lighting/particles. The moving statue-orb accents
use the existing Action lighting/particles switches. Forest visuals are unchanged.

The [September 30 polish pass](action-environmental-effects-plan.md#fillmore-and-bloodpool-polish--september-30)
supersedes the earlier cloud-dust and spike-excluding mist revisions recorded
below. Wet patches now span 33 measured rock columns; landing dust uses fixed-size
brown grains; waterfall and floor mist use three shaded, independently drifting
density layers. Both temple rooms have grounded fog, including at spike-pit
bottoms. Room `01/02` also dims its lower/right temple masonry with a world-space
ramp. Actor and HUD base colours remain unchanged.

## Room treatment

| Room | Source | Treatment |
| --- | --- | --- |
| `01/02`, deep cave / brick temple | BG1 and BG2 2048×1280 | Cool glints and small elliptical rings on three pools; sparse falling streaks and soft blue splash light on four waterfalls; staggered ceiling drops with water rings or tiny rock splashes; readable drifting dust, damp-stone highlights, impact mist, protected water ripples, broad ambient surface light and occasional falling grit. |
| `01/03`, stone climb | BG1 1024×1792; independent BG2 256×512 | Dimmed blue stone, restrained broad reflected light, warm mineral dust, visible cool floor mist and falling grit in the climbing shaft and galleries. Diorama hides the native red BG2 water planes. No invented water or visible beam sources. |
| `01/04`, Minotaur tower | BG1 512×256; star backdrop 256×256 | Three soft blue surface-light fans from the arch openings, broadening across the battle floor; a little drifting dust and slow cloud-shadow modulation. The moving boss and player catch the same light as the stone. |

Water sources come from decoded BG2 metatiles, not color guesses. Metatile `01`
is the surface; paired `02`/`06` columns are falls. Pool extents `(left, right, Y)`
are `(0,880,896)`, `(896,1104,688)`, `(1088,1312,304)`. The fall centers/end heights
are `(736,896)`, `(1008,688)`, `(1120,304)`, `(1264,304)`. The temple's blue spikes
are hazards and receive no water treatment.

Eight selected foreground stalactite/ledge anchors have explicit landing heights
and per-pixel receiving contours, checked against the complete BG1 map. Capture
validates the authored metatiles at both endpoints before enabling each source.
Decorative blue boulders share BG1-low with some foreground rock edges, so a
priority-bit test alone cannot establish a physical surface. Their artwork is
excluded, and drops continue through that background to the actual pool below. Drops gather, accelerate downward, briefly splash,
then wait through the remainder of their staggered 256/512-tick cycles. They use
BG1 projection in the world overlay: the empty cave air has no BG2 pixels and
would erase the drops if they used the flat BG2 winner mask. This is authored
placement, not new collision simulation. Temple dust is likewise an
intentional air overlay; it does not cast shadows or collide with scenery.

Water uses the exact BG2 **priority-1** projection and draw callback, matching
the native water and its authored depth/stack. Drawing after BG2-low hid the
prototype beneath the water itself; pixel comparisons caught this and explicit
high-plane projection/submission tests now cover it. Flat presentation masks
the effects to winning BG2 pixels. Ambient and tower illumination use destination-
color surface blending, preserving black gaps and destination alpha. This is an
artistic approximation, not physical shadow casting. The tower bands begin at
world Y=88 and fade completely by Y=232, inside the short room's finite footprint.

The water-only refraction prototype was removed after the controlled visibility
review below: resampling mostly uniform blue artwork provided negligible visual
benefit. Flat and Diorama presentation now upload native pixels directly, without
the sampler's per-pixel work or 880 KiB scratch buffer. Pool glints, ripple rings,
waterfall accents and splash mist remain. Refraction of visible scenery beneath
water is a future effect, requiring a clear visual payoff, correct layer/shoreline
occlusion and profiling on Steam Deck/Vulkan and Windows/D3D12.

## State, capacity and portability

Capture checks the current room, exact dimensions and two distinct BG1 metatiles;
the cave also checks its BG2 pool/fall signatures. Inherited or incomplete maps
fail closed during transitions. Room `01/02` publishes seven aggregate records,
`01/03` three and `01/04` two. Both temple rooms also publish collision-derived
mist runs, including stone beneath non-solid spike artwork. Mist is capped at
fourteen runs; together with six landing bursts and seven ambient records this
fits the 27-record host decoration budget. The actor-effect budget remains 16.
If a changed map exceeds that cap, only the mist family is omitted. Native
WRAM and the enemy allocation pool are never written by these effects.

Positions are fixed in layer/world coordinates. The owning camera only chooses
a bounded visible field. Seeds are stable and animation uses the existing paused
scene clock. All periods divide the 16-bit clock range, avoiding a discontinuity
at wraparound. No mutable particle pool or per-frame heap allocation is added.

| Geometry layer | Conservative maximum vertices / indices |
| --- | --- |
| All cave water sources | 1,100 / 2,988 |
| Drips and temple dust | 2,260 / 3,390 |
| Ambient surface light after triangle clipping | 4,032 / 8,640 |
| Contact particles, waterfall mist and grit, combined | 3,552 / 7,056 |
| Damp-stone highlights | 1,320 / 1,980 |
| Clipped lower-temple mist, fourteen-run cap | 9,408 / 20,160 |
| Tower light after triangle clipping | 672 / 1,440 |

Compile-time bounds and per-family record limits protect the existing shared
geometry scratch. Water cells are culled before trigonometry/projection. Tower
light reuses the forest's source-triangle clipping, preserving the interior light
field when cropping rather than bending edges onto the viewport border.

Flat water shares the existing alpha BG2 mask upload and samples it directly in
one additive geometry submission. It needs no full-screen masking pass. Surface
lighting retains the existing optional-blend fallback: an unsupported custom
blend disables only surface light. Pure C geometry and the portable render-device
API are used; there are no new platform APIs or backend-specific shaders.

## Validation and review

The chronological reviews below describe their respective revisions. Current
validation and compact before/after clips are recorded in the linked September
30 pass. Native Steam Deck/Vulkan and Windows/D3D12 performance acceptance is
still required; local Metal capture and CPU geometry checks do not establish it.

Release build and seven focused capture, observer, geometry, presenter, render-device,
SDL-state and diorama-projection tests pass. Coverage includes read-only capture, map readiness,
room changes, independent settings, pause, retained frames, clock wrap, bounded
camera sweeps, duplicate rejection, cropped light-field equivalence and direct
flat water masking without render-target support. Geometry and presenter tests
also pass AddressSanitizer/UndefinedBehaviorSanitizer. Changed C/header files meet
the style baseline; private-header and render-boundary checks pass.

All 16 diorama checks pass, including the optional GPU frame-generation test
when rerun with graphics-device access. On/Off captures cover flat and diorama
views at extension settings 32 and 64. All 52 paired native WRAM snapshots match
(13 per view/settings pair), water-detail pixel checks pass, and the checked
short-room bottom padding is unchanged. No steady presentation failures or
fallbacks were reported. Diorama upload-mirror reallocations peak at the same
0.01 per-present measurement window with effects both On and Off; flat samples
report zero. These capture logs do not isolate rendering time.

An optimized local CPU-only probe builds all environmental layers across 13
captured camera positions, sweeping 10,000 animation ticks at each. The combined
build costs approximately 0.4–5 microseconds per sampled view and reaches at most
514 vertices; no extra native objects or per-frame particle allocations are used.

The silent review is 32.33 seconds at 30 fps, with six normal-speed excerpts:
ceiling drops, pools, falls/pillars, temple, stone climb, and Minotaur arena.
`fillmore-act2-effects.mp4` shows effects On, and
`fillmore-act2-effects-comparison.mp4` places Off beside On. Scene cuts,
scripted traversal and infinite health are labeled in both. All 13 paired native
snapshots from the review runs match. See `video-review.json`, `validation.json`
and `geometry-cost.txt` beside the videos for exact frames and measurements.

Local evidence lives in `runs/fillmore-act2-effects/`: decoded maps/atlases,
source snapshots, capture scripts, source-only geometry benchmark, renderer logs,
and review media. Temporary review binaries contain assisted movement and room
warps; shipping code does not. Large instantaneous debug camera jumps were rejected
because they leave the native tile stream stale. The retained review route moves
gradually through the cave and shaft, then uses native movement in the boss room.
This footage tests visual presentation, not unaided level traversal or transitions.

GPU duration is unavailable on this backend. CPU geometry measurements exclude
GPU execution and mask capture/upload; screenshot capture timings are not treated
as performance benchmarks. Cross-platform builds and visual art approval remain
outside the local validation performed here.

## Temple visibility revision

The first dust pass was too faint in the narrow shaft. The revised field uses
32×48-pixel jittered cells with three of every four admitted, instead of
48×64 cells with one of four admitted: six times the expected density. Flecks
vary from 0.75 to 1.30 pixels in horizontal radius and 0.50–0.78 peak alpha,
with slow drift and staggered fades. They remain one quad each in the existing
world-overlay batch; no new draw pass, texture, particle pool or allocation is
required. The same temple treatment applies to the tower's small dust region.

For `01/03`, the room's diorama manifest now sets both BG2 priority bands to
zero alpha. This removes the native red pool artwork, independently of the
Environmental effects switch. The room's existing selected tower-star backdrop
is preserved. BG1 stone/spikes and OBJ enemies/projectiles are unaffected. This
is an editable diorama presentation override; authentic flat rendering retains
its native background.

The revision passes the Release build, geometry/presenter and layer order/manifest
tests, plus AddressSanitizer/UndefinedBehaviorSanitizer for geometry and presenter.
Matching before/after captures preserve all six checked WRAM snapshots. The review
shows red pools removed while blue spikes and red projectile hazards remain.
The updated CPU-only geometry probe covers the same 13 cameras and 10,000 phases,
measuring approximately 1.8–6.1 microseconds and at most 588 vertices across all
emitted environmental layers. No additional draw submission is introduced.
Evidence and the 15.33-second assisted before/after clip are in
`runs/fillmore-act2-effects-v2/`; the earlier full-level review remains available.

## Jump landing dust

Player and ground-enemy jump landings kick up three to five warm brown puffs
and up to six lifted grains. Each burst varies its lobe count, asymmetry, onset,
expansion, drift, height and diffusion seed. Clouds remain at the contact point
and spread sideways before rising, with 33–48 gameplay ticks of visible lifetime
(0.55–0.8 seconds at 60 Hz). Fall speed selects a small or medium burst; the Minotaur gets the
largest. Walking does not continuously emit dust, and takeoff adds no puff.

The producer reads active, identity-checked actors and the loaded BG1 collision
LUT at `$05A0`, the same metatile attributes used by native `$00:91C3`. It
requires observed downward travel and an exposed stone surface: solid floors
(attribute 15) or the cave column-cap metatiles `54..57` (attribute 3, one-way
tops). Non-solid column artwork above a capital is allowed. Blue spikes are
separate metatiles `18/20`, with zero collision attributes; these explicitly
reject a contact, including when placed on a capital. Water, slopes, unknown
one-way artwork and interior wall tiles are not contacts. In room `01/02`, the treatment begins in the temple half at X=1160.
Room `01/03` uses its stone ledges and `01/04` the tower floor. Supported enemy
roots retain sources `$B041/$B0B4/$B28D/$B2FD` on animation bank/base `$7E:4000`,
and the original Minotaur `$AF5D` on `$7E:5000`; the running recompiled game
retains these US identities with region-selected animation data. Projectile
children, pickups, flying creatures and inactive objects are excluded. This is
presentation observation, not a replacement for native physics/collision.

Slot reuse, changed pose extents, large position jumps, missing capture intervals,
room handoff, session reset and disabling Environmental effects clear contact
history. Native pause freezes both contact observation and cloud age. A short
jump's peak cannot count as a landing without a stone contact. No native object,
WRAM byte, collision, damage, or RNG state is changed.

The observer owns six detached event slots, separately from both native actors
and the existing 16 actor-accent records. Twelve bounded surface patches retain
a 120–180-tick settling cooldown (2–3 seconds at 60 Hz), shared across actors
within 20 pixels at the same floor height. Nearby platforms at different heights
remain independent. Pause freezes the cooldown, and clock wrap is safe. A full
cloud or patch buffer drops a new cosmetic burst. Puffs append to the existing bounded decoration list and reuse the shared
geometry scratch. Their maximum is 1,254 vertices / 5,616 indices for all six
simultaneous bursts. Clouds use one additional source-alpha geometry submission
only while active, beneath the HUD, with BG1 projection in flat and diorama
views. No texture, render target, per-frame allocation, shader or platform API
is added. The existing soft-cloud mesh also continues to render Aitos mist.

The focused observer, capture adapter, geometry and presenter tests cover player,
ground-enemy and boss impacts, no first-frame events, read-only WRAM, paused
captures, 16-bit clock wrap, detached world placement, expiration, six-event
saturation, source reuse, teleport/pose/hit rejection, room changes, setting
reset, spike rejection, projectile exclusion, geometry capacity and source-alpha
submission without render targets. The observer, geometry and presenter tests
also pass AddressSanitizer/UndefinedBehaviorSanitizer.

Review scripts and footage are in `runs/fillmore-landing-dust/`. The approach
uses private position assistance and room warps, then the shown player jumps
use recorded native input and enemies run their normal AI. Infinite health is
labeled; the native hit flashes remain. The comparison disables only landing
dust, retaining ambient dust, water and tower lighting on both sides.

The initial review exercised diorama with 64 extra rows and flat presentation
with 32. Its eight paired native-memory snapshots matched. That first cloud
was too faint, and its collision filter incorrectly classified attribute 3 as
spikes, excluding the wall capitals. The revised five-puff burst grows sideways faster, uses a lighter tint and
stronger opacity, and retains its body longer before fading. It keeps the same 48-tick lifetime and six-event limit;
no additional draw submission or allocation is introduced by this revision.

The wall-platform regression cases use the decoded `54..57` caps, their `38/39`
column backdrop, and the native X−8/X+7 foot probes. They also reject spike
artwork above both capitals and ordinary stone, interior walls, unknown surface
identities and unloaded collision attributes. In the native review, repeated
player landings produce dust at `(952,1616)` on the right wall ledge, and a
ground enemy produces dust at `(904,1664)` on the center capital. Boss landings
remain larger. Paired captures confirm that the effect has fully disappeared
after its lifetime; all eight checked native-memory snapshots still match.

The updated 15-second, 30-fps normal-speed comparison is in
`runs/fillmore-landing-dust-v2/`, alongside the capture scripts, contact event
log, pixel checks and native-memory comparison. It shows the climbing ledge
and Minotaur arena with dust Off/On. The approach and room warps use a private
review binary; the shown jumps use native recorded inputs. Infinite health
and scene cuts are labeled. The shipping game contains no review automation.

The full Release build, four focused tests, observer/geometry/presenter
AddressSanitizer and UndefinedBehaviorSanitizer runs, private-header and render
boundary checks pass. Changed source/tests meet the style baseline. The prior
concurrent SaveName link failure is resolved in the current workspace build.
The six-burst optimized CPU geometry probe measures about 15.4 microseconds
per build (100,000 iterations), bounded at 1,254 vertices / 5,616 indices.
This excludes GPU time; the draw count remains one alpha batch while active.

### Landing origin adjustment

The initial cloud now stays shallow against the surface, with vertical growth
trailing its sideways expansion. Upward drift eases in instead of lifting the
whole cloud immediately. This removes the floating appearance above the feet
while preserving the brighter five-puff treatment, lifetime and geometry budget.
The existing surface-bound geometry and presenter tests pass, as does the
Release build. An eight-second before/after review of the wall ledge and
Minotaur landings is recorded in `runs/fillmore-landing-dust-v3/`.

## Cave and palace atmosphere revision

Four staggered, irregular mist puffs per waterfall spread from the actual water
impact, starting low and drifting gently above the surface. They use the same
BG2-high projection as the water, but the shared alpha atmosphere pass permits
wisps in the air above it. Tint, timing and spread vary between sources; opacity
was increased after reviewing the initial capture. Broken, slowly changing wet
highlights sit on the exposed stone lips beneath selected authored drips.

The initial nine temple shafts were replaced with broad, soft ambient light.
A full-map review found that one shaft crossed two solid ledges, and several
sources lacked visible openings. The current lighting uses the real overhead
cavern gaps as a direction cue, with diffuse fill deeper inside; it does not
trace beams through the masonry. Existing dust brightens within the light pools. Six selected masonry sources occasionally shed
three staggered grains that accelerate down clear paths, then produce a small,
surface-hugging puff on a safe capital/ledge. Spike clusters are excluded by
placement. The tower windows keep their existing floor coverage while a common
slow cloud shadow and slightly staggered window response vary their intensity.
All animation follows the paused scene clock; periods divide its 16-bit range.

Mist and grit share the existing source-alpha dust submission. Ambient light
uses the existing surface-light pass, preserving black recesses; drips and dust
share the world additive batch. Wet highlights use direct alpha winner-mask geometry in
flat presentation and the existing BG1 callback in Diorama. The flat path needs
no intermediate full-screen effect target.

The following sampler details and validation describe the historical prototype,
removed after the water visibility review. It owned one fixed ABI-sized upload
workspace (640×352 ARGB, about 880 KiB), copied only when a water pixel changed,
precomputed column strengths and used integer RGB blends. It did not allocate per
frame or retain borrowed surfaces. Frame-generation analysis received the same
filtered pixels as the upload. Both paths now receive unmodified native pixels.

Validation includes all four focused tests plus ASan/UBSan runs for observer,
geometry and presenter; the full Release build, private-header, render-boundary
and whitespace checks pass. All 13 atmosphere source/test files meet the style
baseline. The repository-wide style check currently reports unrelated violations
in voxel model code, voxel tests and the voxel model-sheet tool. New checks cover every family’s
geometry budget, camera sweeps, duplicate rejection, paused/periodic animation,
light-field clipping invariance, spike-safe landing contacts, immutable source pixels,
water mask/donor exclusion, alpha preservation (including the opaque flat
framebuffer's zero-alpha RGB pixels), missing masks, independent
settings and direct BG1 geometry without render-target support or stale masks.

The 29-second normal-speed review and Off/On comparison are in
`runs/fillmore-cave-atmosphere/`. They cover waterfall mist/ripples, damp ledges,
temple light, a complete grit fall and impact during the stone climb, and tower
lighting. Scripted traversal, room cuts and infinite health are labeled. All
13 paired native WRAM snapshots match. A full flat-view probe with extension 32
also completes without reported rendering failures. Instrumented gameplay
captures confirm that real water pixels are resampled in both Diorama and flat
presentation; the flat winner mask, rather than framebuffer alpha, determines
which pixels are safe to sample. Local CPU probes measure
about 2–12 microseconds to build environmental geometry at the 13 captured views,
peaking at 1,034 vertices. Deliberately busy water-upload inputs cost about
0.33 ms at 512×352 and 0.57 ms at the ABI maximum 640×352. These are CPU-only
measurements; they do not include GPU execution and are not cross-platform
performance guarantees. Capture scripts, measurements and review metadata remain
beside the media; shipping code contains no traversal assistance or review logging.

## Diffuse light revision

The entrance, narrow waterfall fissure and upper cavern in `01/02` have actual
openings at the top of the map. Their light is suggested with broad patches,
without drawing a cone or a source marker. Eight irregular pools cover the
upper openings, cave water, upper temple and deeper masonry; the lower cave
receives weaker fill. Four more pools vary the illumination along `01/03`'s
climb. Tower-window lighting remains unchanged. This is authored atmospheric
illumination from implied offscreen sources, not a physical bounce simulation.

Each pool is a small seven-by-seven mesh with a cubic falloff on both axes, a
slight lean and no apex. Brightness changes by only two percent over a slow
1024-tick cycle. Pools retain fixed world positions as the camera scrolls in
either direction. Source-triangle clipping preserves the original gradient at
viewport edges. Destination-color surface blending brightens stone, actors and
existing scenery while keeping black pixels black. It adds no render target,
texture, shader or per-frame allocation. Dust exposure uses the same authored
positions and falloff, so particles become more visible in illuminated areas.

Focused coverage checks broad falloff, unchanged interior values after clipping,
and exact two-axis camera translation of the rendered light field.

The updated 28-second normal-speed review and Off/On comparison are in
`runs/fillmore-soft-light/`. The clips cover the waterfall, upper cavern, temple,
climbing room and tower, with assisted traversal and room cuts labeled. All 13
paired native-memory snapshots match. Release build, four focused tests,
observer/geometry/presenter ASan/UBSan, render-boundary and private-header checks
pass. The six source/test files revised for diffuse light meet the style baseline.
The revised geometry probe peaks at 1,205 vertices across the same 13 captured
views, taking roughly 3–19 microseconds of CPU time per view on this machine.
This excludes GPU work. Ambient illumination uses one surface-light geometry
submission when visible, alongside the existing dust/particle submission.

The revised flat renderer also completes the full three-room probe without
reported rendering failures; screenshots confirm the same surface-light treatment.

## Room 2: enclosed, damp ruin

The second room of Act 2 (`01/03`) now has a separate treatment from the cave.
Its blue masonry starts 45% dimmer before illumination. Four broad ambient
fields use neutral stone-colored light with constant brightness, so they do not
suggest visible openings or moving beams. Surface lighting is now 60% weaker
than the initial stronger mood pass: bright player/enemy palettes receive a
gentle lift instead of intense highlights. Mist keeps its separate field exposure
and opacity, so reducing surface gain does not make the floor atmosphere disappear.
Actors and the HUD retain their original base brightness. The first cave room and boss tower
keep their existing treatments. Dust uses a neutral tint; lower-hall flecks are
quieter while airborne dust and occasional grit remain readable higher in the shaft. Existing native
projectile/spell accents and contact dust retain their independent behavior.

Mist follows exposed native collision tops in the lower hall, rather than the
decorative ledge artwork. Capture checks BG1 metatiles and the loaded `$05A0`
collision table in the authored lower-hall region X=512..960, Y=1664..1712.
It merges adjacent columns at the same height and splits at steps or gaps.
Solid stone and one-way column capitals qualify; obstructed tops, unproved
surface types and spike-covered floors do not. The native room yields:

| Floor run X (right edge exclusive) | Collision height Y |
| --- | --- |
| 528..592 | 1664 |
| 592..656 | 1680 |
| 688..736 | 1680 |
| 768..832 | 1680 |
| 864..928 | 1664 |

The original constant-Y prototype left the three lower sections 16 pixels too
high. Each captured run now contains two wisps whose dense bases touch its
actual floor. Only their upper edges and lateral shape drift; the entire cloud
cannot bob away from the ground. Height remains below 26 native pixels, alpha
per wisp stays below 0.42, and the four intervening arch/pit gaps remain clear.
The cooler, brighter tint and roughly 70% higher opacity make the mist readable
against the dimmed stone without enlarging its footprint or lifting its base.
The renderer consumes immutable run bounds, never live collision memory.
All coordinates belong to BG1; nothing follows the viewport bottom.
The shared source-triangle clipper preserves soft edges when a pocket scrolls
partially offscreen, and no fog geometry is emitted when the player climbs away.

Diorama dims BG1 low/high/far faces and depth geometry through their existing
vertex colors; it adds no draw for the grade. It inserts mist alpha geometry
after BG1-low, before later object/high terrain bands. Flat mode draws directly
through BG1's native winner mask, so actors occlude the mist; only visible BG1
scenery receives it in that mode.
The mask uses the existing bounded upload buffer/texture and needs no intermediate
render target. A failed mask upload suppresses the mist instead of reusing stale
occlusion. The same mask supports one four-vertex alpha draw to dim scenery
before mist and ambient light. It is therefore captured throughout room 2,
including the ascent, even when the floor mist is offscreen. Geometry uses the
existing shared scratch, with no per-frame allocation or native object slot. Everything
is gated by Environmental effects.

Room 2's flat mask uses the new `MARK_VISIBLE_MAIN_WINNER` capture policy:
it selects the main-screen winner after extracted HUD text/icons are removed.
An observational mask would leave bright glyph-shaped holes at the HUD's old
position. Ordinary actors still exclude BG1. Existing observational and owning-
screen flags retain their behavior for other consumers, including subscreen
wall lights. The optimized PPU path exports its already resolved final winners,
without additional tile sampling or a reference-renderer fallback.

Regression checks cover both floor heights, map edits that split an existing
run, fragmented-map budget rejection, pit gaps over the full animation cycle,
ground contact and exclusion of solid-floor pixels throughout that cycle,
clipping invariance, two-axis camera translation, disappearance during ascent,
room gating, native-memory immutability, alpha composition behind actors,
missing/stale masks, environment toggling and rendering without target support.
The dimming tests also cover wrong/inherited room fields, the upper shaft and
HUD/actor mask holes. A real-PPU test verifies post-extraction masks, retained
actor occlusion, unchanged framebuffer pixels, and packed/reference parity.

Validation: Release build and all four focused observer/capture/geometry/presenter
tests pass, as do the PPU rendering pipeline and runtime PPU/ABI tests.
Observer, geometry and presenter also pass ASan/UBSan. The thirteen
touched game source/test files meet the style ratchet; private-header, render-boundary
and whitespace checks pass. Native Diorama and flat captures complete without
reported rendering or dispatch failures. Nine paired Diorama snapshots have
identical native WRAM with environmental effects On and Off. Both views also
match all nine corresponding native WRAM snapshots from the previous treatment.

The updated review movies are `runs/fillmore-temple-room/fillmore-temple-room-soft-light.mp4`
and `fillmore-temple-room-soft-light-comparison.mp4`: 26.33 seconds at 30 fps, covering the
bottom hall, column base, ascent and upper shaft. The comparison shows the stronger
lighting beside the revised softer surface response. Assisted traversal, infinite
health and scene cuts are labeled. The preceding grounded-mist CPU geometry probe across
nine captured cameras and 10,000 phases per camera, clipped to the capture's
extended viewport bounds, emits at most 1,037 vertices and takes roughly
6–17 microseconds locally. This measures CPU geometry only;
it excludes GPU execution and mask capture/upload. The mood revision changes
light strength and mist color/opacity without increasing their geometry. The
mist adds one alpha draw while visible and reuses the existing mask texture and
geometry scratch. Flat scenery dimming adds one masked quad and extends mask
capture/upload through the ascent; Diorama dimming has no extra draw or upload.
The surface-response follow-up changes only vertex brightness, adding no geometry,
draws, uploads or resources. Its Release build and geometry test pass; fresh native
captures in both views preserve all nine sampled WRAM states.


## Contours, splash contact, statue orbs and dust variation (2026-09-28)

The former wet-stone diamonds assumed a horizontal receiving ledge. They now
use narrow highlights inside thirteen sampled opaque columns at each rock
contact, skipping open columns rather than joining across gaps. A shared,
immutable surface catalogue feeds capture validation and pure rendering.
Unchanged background boulders can no longer become wet edges or drip origins.
The catalogue also removes the old temple/spike anchors and directs two drops
past decorative rock into their native pool. No live pixel decoding is added.

Waterfall mist starts around the native splash foot at the pool surface, then
spreads sideways with a small rise. Its blue tint is distinct from brown dust;
the lower temple's collision-aligned mist also uses the cooler blue palette.
Airborne dust, ceiling grit, landing clouds and grains all use warm mineral
colors. One grit path that terminated on spike artwork was removed.

Statue balls are identified by source `$B3BF`, animation `$7E:4000`, saved resume
`$B3E9`, native rolling/falling handlers `$B406/$B42F` and states `$0A/$0B`.
Visuals `$1B..$1E` map to compositions `$48F0/$48FC/$4908/$4914`. On first
effect admission, an aligned backlink must identify a live parent statue in
state `$24`; the shared source by itself is insufficient. The admitted ball then
owns its lifetime, so a changed, retired or reused statue slot cannot extinguish
its enhancement. Its effect clock freezes when its position stops, even if native
velocity remains nonzero. The [RAM contract](ram-map.md#projectile-identity-and-lifetime)
distinguishes this spawn evidence from continuous attachment. These identities
retain the loaded regional runtime contract. Capture follows the room's `$008F`
OBJ band. A broad red-orange glow,
restrained warm core and short ember wake move with the projectile. They use the
existing two-glow budget and half the usual fireball particle count, without a
new render pass or texture. The native red sprite remains readable.

Landing bursts no longer form the same symmetric five-lobe pattern. Per-event
seeds affect macro shape and timing as well as the cloud mesh, while the local
settling cooldown prevents alternating player/enemy impacts from farming one
patch. The six-event geometry ceiling is unchanged. Capture remains read-only,
and all additional state is fixed-size host memory, never native enemy slots.

The four focused capture/observer/geometry/presenter tests and Release build pass.
Observer, geometry and presenter pass ASan/UBSan. Regression coverage includes
background-material substitution, sloped glints, missing source masks, thirty-two
distinct cloud bursts, cross-actor settling, independent patches, pause/wrap,
all eight orb animation combinations, wrong-room/parent/composition rejection,
and independent lighting/particle switches. The final matching On/Off native
review preserves all nineteen WRAM snapshots. Diorama at 64 extra rows and flat
presentation at 32 report zero background-preflight mismatches, renderer
fallbacks and failed presents. The seven-camera CPU geometry probe peaks at
1,011 vertices and takes roughly 4–17 microseconds per sampled view on this
machine; this excludes actor accents, contact history and GPU work. Boundary, private-header and
changed-file style checks pass. Evidence and the updated silent review are in
`runs/fillmore-cave-polish/fillmore-act2-polish.mp4` (30.17 seconds at 30 fps).
Route assistance and infinite health are labeled. The jump segment uses native
vertical physics and input; only horizontal position is held on a safe ledge.
Logged player dust births occur at frames 4891, 5085 and 5279, with intervening
landings suppressed while the patch settles; an enemy emits at frame 5485.
Review timings include frame capture and are not GPU benchmarks.

## Portability and performance audit

See [the 2026-09-28 audit](fillmore-effects-audit.md) for the current source
review, cleanup, maximum-size pixel tests, CPU measurements and remaining
native platform validation.

## Water visibility verification

The 2026-09-28 refraction-only On/Off replay confirms that the sampler reaches
the final Diorama image, but its visible contribution is extremely small.
The control bypasses only `PresentActionWater_Pixels`; all other environmental
effects remain enabled. All four paired native WRAM snapshots match.

At game frame 2100, the 624×352 BG2-high source changes 1,233 pixels, averaging
2.34 RGB channel levels among changed pixels. In the final 720×448 composed
image, 4,947 pixels differ, but their mean absolute channel difference is only
0.58/255 and the peak is 5/255. Filtering spreads and attenuates the change.
The eight-second, 240-frame comparison and a separately labeled x16 difference
view are in `runs/fillmore-water-visibility/`, with raw water planes and metrics.

The reason is structural as well as strength: displacement is capped at 0.34
native pixels away from falls (0.84 at an impact), and the input is mostly a
uniform blue fill beneath a narrow patterned surface strip. The pass resamples
that water artwork only; it does not distort scenery or actors seen beneath
water. It read as a faint surface shimmer. Execution/pixel tests do
not establish that this meets the intended refraction appearance. Meaningful
scene refraction needs an explicit decision about which submerged scene layers
to distort, retaining water boundaries/occlusion and the Deck/D3D12 upload and
composition constraints. No strength-only adjustment was made during this check.


## Water sampler removal

Following that review, the water-only CPU sampler, its flat/Diorama upload hooks,
source manifest entries and sampler-specific tests were removed. No replacement
composition pass or platform dependency was added. The existing cave lighting,
glints, ripples, drips, wet-rock highlights and mist are retained. Native lava heat
refraction is separate and unchanged.

`CaveMaskUploadBudget` keeps the relevant SDL/Deck/D3D12 regression guard on the
surviving flat cave-mask path: scattered changes produce one update per texture,
unchanged masks produce none, and resize reuses each texture. It also exercises
unaligned/padded input without modifying the source. General upload-mirror tests
continue to cover allocation reuse.

Removal validation: the rebuilt 64-row Diorama replay matches the earlier
sampler-disabled control pixel-for-pixel across all 240 frames; all four paired
native WRAM snapshots also match. The 64-row flat replay completes without
reported capture/session failures. Release build, 12 focused rendering/effect/
upload tests and presenter ASan/UBSan pass, as do style, private-header,
render-boundary and whitespace checks. The linker retains the existing macOS
`__DATA` alignment warning. The eight-second normal-speed review and validation
records are in `runs/fillmore-water-removal/`; traversal assistance/infinite
health are labeled and confined to the review executable. Native Deck/Windows
GPU performance validation remains pending.
