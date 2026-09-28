# Bloodpool Act 2 environmental effects

2026-09-28. First castle treatment for rooms `02/02` through `02/08`, controlled
by **Effects → Environmental effects**. Existing torches, projectile accents and
boss lightning retain their established identity and settings contracts.

## Lighting and atmosphere

Interior masonry is dimmed before the added light: 36% in rooms 3, 4 and 7,
42% in the deeper interior (room 5), and 30% in the boss chamber. The entrance
and exterior crossing (rooms 2 and 6) retain their original brightness. Dimming
uses the existing BG1 plane/winner mask; it does not change native palettes,
HUD, actor brightness or gameplay. Unsupported/inherited room data cannot
activate the dimming, and Environmental effects Off restores the original view.

- Rooms 2 and 6: a soft halo and the same two angular ray families as Act 1
  behind the native battlements, plus thin haze along the supported walkway
  floor. Broad lower rays overlap shorter, brighter middle rays.
- Room 3: window light in the tall shaft and lower hall, ivory-brown dust visible
  within those fields, and warm wall bounce around the existing three torches.
- Room 4: diffuse cool offscreen fill and sparse dust, with low haze above the
  safe central floor. The spike pits are excluded.
- Room 5: restrained light at five narrow windows, warm bounce around the three
  existing torches, and low violet mist in the bottom corridor. This interior
  has no sky/moon field. The native blue moat now has soft travelling surface
  highlights, smaller glints and occasional expanding ripples on the resolved water surface.
  The electric traps' broad light radius grows from 31 to 76 pixels, with
  more vertical spill and the original narrow core and peak brightness.
- Room 7: all eleven arches emit edge scattering, with visible dust and low
  floor haze. A narrower spread per opening preserves dark pillar gaps.
  BG2 moon rays shine through the native transparent arches; pillars and
  masonry occlude them through the original layer order/winner mask.
- Room 8: the visible moon casts the exterior ray pattern behind the large
  rear window. A broader lower ray fan, concentrated airborne dust and thin
  floor haze remain inside the chamber. Actor/boss
  brightness remains separate; existing lightning provides its own combat accents.

Window fields are authored in BG1 world coordinates and project through that
same layer, including generated-frame offsets. The user selected **A, soft
moonwash, for regular windows** and **B, broken shafts, for the boss** from the
native preview study. Regular openings now emit a broad smooth wash instead
of twenty fine strands; the boss retains three broad concentrations with dark
intervals, brightest at the sill and continuously fading out. A faint glow
inside each opening and a shallow highlight on the stone sill establish the
source. Upper bounce follows the measured arch contours and has much less
reach than the downward light. Background moon rays remain a separate field.

Geometry stays fixed in world space; only intensity changes slowly (2% breath)
and motes drift within the same shortened volumes. Triangle clipping lets light
leave the screen without moving its anchors. The source catalogue still finds
stack neighbors independently of viewport visibility and clips an upper
window's spill before the next opening. Missing intermediate sources break the
join. This is stylized scattered and bounced light, not optical simulation.

The gallery additionally receives narrow, cool highlights on the inner column
edges and soft pools across the walkway and its stone face. Each pool is tied
to its validated arch, spans y=204–226 and has a maximum 19-pixel half-width;
40-pixel window spacing leaves dark intervals under the pillars. Column-edge
light stays in x=centre±17.5, with a 1.25-pixel radius, between y=132–196.
The rear moon-ray exposure is 1.6 times the other castle sky fields, capped
before saturation. Its source position and silhouettes are unchanged, and
native pillar artwork still occludes it. These additions remain behind actors
in Diorama and use native winner masks in flat mode.

Regular soft fans use 5×4 grids; the boss uses 9×4 for its three concentrations.
The opening uses 5×4 and the sill 5×3. Regular windows total 44 grid cells,
the boss 68, and gallery stone light adds 16 per arch. Every cell is a convex
quad; the conservative rectangle-clipping bound is 12 vertices/24 indices.
Compile-time budgets include all eleven gallery windows, dust, the shaft's
eight windows and torches, and the water-room combined batch. They fit the
existing geometry workspace. No new pass, texture, native object or larger
buffer is introduced.

All 25 window emitters retain the decoded native-art anchors. Each sill
origin is the first pixel below the bottom frame. The narrow pointed arches,
wide gallery/shaft caps and stepped boss crown retain their distinct measured
contours. The paired narrow windows in room 3 use separate centers at x=168
and x=184. Root widths remain within each actual opening, with the shallow
sill wash extending onto the nearby stone.

Exterior rays reuse Act 1's ray profile without its marsh-specific opacity
catalogue or CPU shadow field. Native layer ordering and flat winner masks
occlude them with the castle silhouette. The sky mesh uses 65 angular columns
and 9 radial rows; Act 1 retains its 129-by-17 mesh for fine platform penumbras.
Both exterior ray families share one additive batch and remain attached to BG2's
moon even while BG1 scrolls. They can extend beyond the native 256-pixel sky map,
with clipping supplied by the actual presentation plane.
The gallery and boss chamber also use this BG2 ray fan. Its opaque BG1 back wall hides the
outer portions; the real transparent window artwork reveals central and partial
rays, including the arch shape. Flat view uses the existing BG2 winner mask to
exclude wall and actor pixels. Diorama draws the BG1 wall and actors over the
BG2 fan. No rectangular replacement aperture or extra rendering pass is needed;
the separate arch/sill scattering still provides the interior bounce.

Torch bounce is a wide, soft amber falloff around the existing native torch
sources. Its horizontal radius is now 100–110 native pixels (previously 60–66)
and its vertical radius is 86 (previously 54), retaining the same peak strength.
It adds no extra flame sprites. Dust is small but deliberately visible
against the darker stone, with independent phases, curls and smooth fades.
Mist stays within 18 native pixels above verified supporting floors. Every
16-pixel column must have solid collision below and a non-solid, non-spike tile
above; a changed span is omitted instead of bridging a gap. No mist or light is
anchored to an actor or its allocation pool.

## Capture and rendering

Room dimensions and two map witnesses reject stale transition data. Shared
window definitions are also checked, and each authored source has its own top
and lower metatile witnesses. Windows additionally require the actual bottom
frame metatile before emitting sill rays. Capture publishes up to three aggregate records
alongside the existing torches; a per-room bitmask validates the source set.
The source table is bounded and immutable. Optional capture reads native WRAM
without changing it. Its clock pauses with gameplay and every animation period
divides the 16-bit scene-clock wrap.

Water capture validates the native surface tile definition and every surface
and overhead metatile in eight independent 144-pixel strips. A missing/replaced
strip is omitted, with no effect anchored to background masonry. The geometry
stays within the first 16 pixels of water. Although the native water tiles
come from BG2-low, color math folds them into BG1's resolved scenery; the
accents therefore use BG1's projection and winner mask, before actors. Matching
BG1/BG2 cameras are required. Drawing them behind the resolved BG1 would hide
them even though geometry was being generated. This is surface shimmer and ripples, not scenery refraction.

Lighting, water and dust share the existing BG1 additive batch with torches.
Sky uses BG2 additive geometry; low mist uses the existing BG1 alpha slot. Flat mode
also uses one masked quad for masonry dimming. Both masks use direct geometry
composition, including devices without render-target support. There are no new
shaders, textures, scene resolves, readbacks, native objects or per-frame heap
allocations. Diorama dims the existing scenery draw, without a separate pass.

## Validation

Focused capture, geometry and presenter tests cover source invalidation, missing
floor support, pause, periodic motion, world bounds, duplicate aggregate
rejection, all-room geometry capacity, interior-only dimming and settings Off.
Geometry tests cover separate arch/sill bounce, bounded opening glow, soft
regular-window symmetry, broad boss concentrations and the absence of a comb
of fine rays. All 25 native-art anchors retain lit sill/arch regions. Stack
tests cover clipping with both neighbors offscreen and rejection of a strong
curtain across the next opening. Gallery tests require floor pools and both
column edges, dark pillar intervals, missing-source suppression and invariance
under viewport cropping. Increased gallery sky exposure preserves the original
ray positions. Water bounds, camera translation and periodic motion remain
covered alongside all-room capacity and widened electric spill.
The flat composition test disables render-target capability, confirms both
masks exclude actor/other-layer winners, and checks four geometry submissions
including dimming, two cached masks, and no target resolves/restores. A moat
fixture also renders through the direct BG1 mask without render targets.

The focused effects, presentation, projection, frame-generation, upload and
render-boundary tests pass. Capture, geometry and presenter suites also pass
ASan/UBSan, and the affected sources compile as strict C11 objects for arm64 and
x86_64 macOS. This is not native Steam Deck/Vulkan or Windows/D3D12 validation.
Local CPU probes using seven native room snapshots measure roughly 0.009–0.051 ms
for all environmental geometry, including existing torches, and 0.001–0.015 ms
for capture. These are CPU construction measurements, not GPU timings.

Native replay covered all seven rooms in flat view with 32 extra rows and
Diorama with 64 extra rows, each with environmental effects On and Off. All 28
paired full-WRAM snapshots match byte for byte. The four completed runs report
zero background preflight mismatches and zero outside-map preflight samples.
After the lighting revision, both On routes were recaptured and all 28 snapshots
again matched their stored Off baselines (`revision-validation.json`).
The 20-strand revision also covers both seven-room routes: all 28 snapshots
match the Off baselines, with no background preflight errors
(`hair-replay-validation.json`). Its compact review clip is 12 seconds,
480×338 at 20 fps, and about 251 KiB (`bloodpool-castle-hair-rays-small.mp4`).
The soft-backdrop follow-up recaptures all seven Diorama rooms and the first
four rooms in flat mode, including both stacked-window layouts. All 22 native
WRAM snapshots match their Off baselines, with zero background preflight
errors (`haze-replay-validation.json`). Its 12-second clip is
`bloodpool-castle-rays-haze-small.mp4`, 480×338 at 20 fps and about 253 KiB.
The arch-contour update repeats those seven Diorama/four flat rooms; all 22
WRAM snapshots match, with zero preflight errors
(`arch-roots-replay-validation.json`). Its clip is
`bloodpool-castle-arch-origins-small.mp4`, 12 seconds at the same size/frame rate,
about 250 KiB. `window-arch-roots-comparison.jpg` shows the root placement change.
The flat view retains native winner-mask occlusion; light inside transparent
window openings is more restrained than in the separated Diorama planes.
The shorter, softer strand adjustment passes the three focused suites and the
release build. Its seven-room Diorama replay preserves all 14 Off-baseline WRAM
snapshots with zero background preflight errors (`soft-rays-replay-validation.json`).
`bloodpool-castle-soft-rays-small.mp4` shows the shaft, narrow windows and boss
chamber in 12 seconds at 480×338/20 fps, 255,387 bytes. The paired crop
`window-soft-rays-comparison.jpg` compares the previous and softened treatment.
This tuning retains the same geometry budget, batches and portability paths;
it does not add a new flat-mode or physical-device validation run.

Review scripts, captures and validation records live in
`runs/bloodpool-castle-effects/` (ignored). The route uses labeled assisted
movement and scene cuts, with native camera/streaming, rather than a complete
ordinary playthrough. White player flashes are native damage-protection
flashes from the review assist, not actor brightening by the lighting effects.
The revised `bloodpool-castle-light-revision-small.mp4` is a 28-second H.264 clip
at 480×338 and 20 fps, 716,837 bytes, with fast-start metadata for remote playback.
The brighter-window follow-up is `bloodpool-castle-brighter-windows-small.mp4`:
16 seconds at the same resolution and frame rate, 358,658 bytes. Its native
replay preserves all 14 full-WRAM snapshots and has zero background preflight
mismatches; the geometry and presenter tests pass after the intensity adjustment.
The boss moon-ray follow-up is `bloodpool-boss-moon-rays-small.mp4`: an 8-second
before/after comparison at the same resolution and frame rate, 146,297 bytes.
Flat and Diorama native replays match all 28 stored Off WRAM snapshots and have
zero background preflight mismatches (`boss-moon-validation.json`). Sampled
Diorama frame differences are confined to the window opening; masonry and
actors outside it are unchanged. Geometry and presenter tests pass, including
the boss fan's periodicity and the existing masked composition without render
targets or extra geometry submissions.

The sill-anchor audit checked all 17 sources against the native bottom-frame
pixels (`sill-anchor-audit.json`), removed the transparent launch row and oval
receiving glow, and separated the paired narrow windows. Its review clip is
`bloodpool-castle-sill-rays-small.mp4`: 16 seconds, 480×338, 20 fps, 356,009 bytes.
Focused capture/geometry/presenter tests, ASan/UBSan and strict arm64/x86_64
compilation pass. Compile-time budgets include worst-case rectangle clipping
of the light ribbons and dust diamonds. Fourteen native room snapshots measured
0.009–0.052 ms for complete environmental geometry on this host; those local
CPU timings include concurrent capture/export activity and are not GPU or
Steam Deck/D3D12 measurements. The change uses the existing batches and masks.
New flat/32-row and Diorama/64-row native replays preserve all 28 full-WRAM
snapshots from the stored Off baselines, with zero background preflight
mismatches or outside-map samples (`sill-replay-validation.json`). Visual
comparisons confirm attached, separated sill rays in both presentation modes.

The brighter upper-ray follow-up passes the geometry and presenter tests. Its
Diorama replay preserves all 14 stored WRAM snapshots and reports zero background
preflight mismatches or outside-map samples (`arch-validation.json`). The focused
review clip, `bloodpool-castle-arch-rays-small.mp4`, shows the shaft and narrow
windows: 8 seconds, 480×338, 20 fps, 199,494 bytes.

The stacked-window/finer-strand revision passes capture, geometry and presenter
tests, all three ASan/UBSan suites, and strict C11 arm64/x86_64 compilation.
Local full-environment geometry probes remain approximately 0.008–0.042 ms
across seven native room snapshots; these are CPU measurements, not GPU or
Steam Deck/D3D12 timings. The final review clip is
`bloodpool-castle-stacked-rays-small.mp4`: 8 seconds, 480×338, 20 fps, 200,942 bytes.
Focused native replays through rooms 2–5 cover both stacked-window areas in
Diorama with 64 extra rows and flat view with 32. All 16 full-WRAM snapshots
match the stored Off baselines, with zero background preflight mismatches or
outside-map samples (`stack-replay-validation.json`).

The centered-fan/gallery/moat revision passes the three focused suites and
the release build. Capture, geometry and presenter suites pass ASan/UBSan;
affected sources compile as strict C11 for arm64 and x86_64. Native Metal
captures cover all seven Diorama rooms, plus focused flat and Diorama routes
through the lower water corridor. Twenty-eight comparable full-WRAM snapshots
match stored effects-Off baselines, with zero background preflight errors
(`gallery-moat-validation.json`). The flat route includes the new moat effects;
its first six snapshots use the original route baseline, while its two altered
water-route snapshots are visual checks. Final water brightness tuning was
recaptured in Diorama after the flat check.

Native water inspection caught both an unsigned camera-relative coordinate
wrap and highlights hidden behind resolved color-math scenery. Regression
tests now cover translated water geometry and the direct BG1 mask path. Pixel
comparison confirms the new highlights occupy the visible surface band.
The full gallery validates all eleven source bits, including the alternating
arch layouts; sampled geometry budgets remain inside the existing scratch.
Local seven-room CPU probes measured about 0.008–0.045 ms for environmental
geometry; the lower-water snapshot measured about 0.027 ms. These are CPU
construction measurements on this Mac, not GPU timings or physical
Steam Deck/Windows validation.

`bloodpool-castle-gallery-moat-small.mp4` shows centered window fans, water
shimmer with widened electric spill, and the gallery: 11.35 seconds,
480×338 at 20 fps, 273,228 bytes. The clip uses the labeled assisted review
route; native white damage flashes are unrelated to the environmental light.

## Window art-direction previews (selection recorded)

The user requested alternatives to choose from after the fine-strand treatment
continued to feel too artificial. Three private native builds are recorded in
`runs/bloodpool-window-study/`. The user subsequently selected A for regular
windows and B for the boss; both are now integrated, with the gallery additions
described above.
`make_study.py` links a private castle effect object into the existing assisted
review executable, with the original game, cameras and other effects intact.

- A, soft moonwash: broad smooth spill, a lightly illuminated opening and sill,
  and much shorter upper bounce.
- B, broken shafts: three broad, uneven concentrations separated by dark gaps,
  brightest at the sill and continuously fading outward. The initial detached
  intensity peak was removed after inspecting the boss-room capture.
- C, luminous windows: brighter openings and stone sills, shorter diffuse
  spill, and a more visible arch halo.

All three use existing geometry batches, clipping and scratch buffers, with no
new shaders, textures, resolves or readbacks. Full-room geometry probes across
128 phases pass ASan/UBSan and buffer-capacity checks. Native Metal replays cover
all seven rooms: all 42 full-WRAM snapshots match the effects-Off baseline, with
zero background preflight errors (`replay-validation.json`). These visual
direction studies preceded the production integration and updated flat-mode
and visual tests recorded below.

`window-light-options-small.mp4` compares current/A/B/C in narrow-window,
gallery and boss scenes at 480×338, 20 fps, for 24 seconds. Enlarged stills are
`window-options.jpg`, `gallery-options.jpg` and `boss-options.jpg` in the same
directory. The current gallery reference uses the earlier lower-water review
run's matching gallery camera; the new options share the original review route.

## Selected window styles and gallery moonlight validation

A is integrated for ordinary windows and B for the boss. The gallery adds
stronger rear moon rays, cool highlights along the inner column edges, and
soft pools on the stone below each opening, separated by dark pillar intervals.
All additions use existing geometry batches, masks and scratch buffers.

The release build, three focused capture/geometry/presenter suites, all three
ASan/UBSan suites, and strict C11 arm64/x86_64 compilation pass. Tests cover
symmetry, stacked windows, boss shaft separation, all eleven gallery sources,
missing-source suppression, clipping invariance and fixed buffer capacities.
Native flat/32-row and Diorama/64-row replays through all seven rooms preserve
all 28 full-WRAM snapshots from the stored effects-Off baselines, with zero
background preflight mismatches or outside-map samples. Results are recorded
in `runs/bloodpool-gallery-final/replay-validation.json`.

Visible-scene CPU probes measured about 0.057 ms for complete environmental
geometry in the eleven-window gallery and 0.038 ms in the boss room on this
Mac. Peak sampled gallery geometry was 2,366 vertices and 8,625 indices, within
the existing 10,610/40,980 limits. These are CPU construction measurements,
not GPU timings or physical Steam Deck/Windows validation.

`runs/bloodpool-gallery-final/gallery-moonlight-small.mp4` compares gallery
before/after and shows the selected regular and boss treatments: 13.35 seconds,
480×338 at 20 fps, 286,715 bytes. `gallery-comparison.jpg` provides a matching
still. Both presentation modes were visually checked. White player flashes
in the assisted review route are native damage protection, not added lighting.
