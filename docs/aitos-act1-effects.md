# Aitos Act 1 atmosphere

Implemented 2026-10-27 for rooms 04:01–03 in all three regional terrain
profiles. Definitions live in `action-effects.ini` and use the existing
Environmental effects setting and the shared native/editor renderer.

| Room | Treatment |
| --- | --- |
| 04:01 | Irregular distant lightning behind the cliffs, a warm-grey upper cloud shelf, warm-white billows with pale rose shadows over the distant peaks, sparse outdoor wind-carried moisture and bubbling lava with cooling crust, warm receiver lighting and localized heat shimmer. |
| 04:02 | Stronger spray on the native animated splash contacts, rounded impact plumes, airborne mist and a rolling bank below the waterfall crossing. Broad overcast light reaches the falls and gently lights platforms and actors. The cave remains covered by the authored scenery. |
| 04:03 | Fuller cool mist beneath the arena, fine falling droplets and rounded plumes on the splashing platforms. Broad warm sky light enters from above; the waterfall and platform artwork remain visible. |

## Authoring

`storm-lightning` is a reusable emitter, available as **Distant storm lightning**
in the editor. Its controls include strike-region width/height, mean interval,
active fraction, tint, intensity, pattern seed, channel bends and one main bolt
with up to three forks. Each cycle picks a different strike position, path and
delay. Scrubbing backward reproduces the same result; camera visibility never
starts or restarts a strike. Sustained return strokes reuse its lingering channel and
reveal additional forks, with one local cloud glow.

Room 1's sky effects use `bg2-point` anchoring and background placement. This
keeps each bolt rigid while the underlying cloud art uses raster scrolling,
and lets foreground cliffs cover it. Lightning uses `parallax-x=.125` for the distant cloud/mountain pair
and `.25` for the nearer pair, instead of BG2's half-speed base scroll. Its root selects the sky projection, including during
interpolation; the lower channel does not shear across raster bands.
Six weather pockets occupy **unwrapped source coordinates**, rather than
repeating one emitter every 256 pixels. Select
BG2 to edit their anchors; airborne particles and lava heat regions use BG1.
The room-wide foreground cloud bank is removed: cloud cover stays behind the
cliffs and outside enclosed lava passages. Moisture stops before the cave at
X3200. Waterfall impact mist in rooms 2 and 3 is retained.

Lightning defaults to atmospheric illumination only. Scenery, player and enemy
receiver flags remain independently available. The native Aitos weather leaves
them off so a distant flash does not brighten actors in front of a cliff.

Four finer strikes have one fork, and two stronger strikes have two forks.
The finer strikes originate in the eighth-speed shelf; the stronger ones
originate in the quarter-speed shelf. The regions have 112–128px maximum
reach, allowing channels to meet a mountain at their own depth. The frontmost/top cloud band has no lightning.

The storm controls expose `parallax-x` (0–1 of playfield camera travel; unset
inherits BG2), `ground-palette` (0–7) and `ground-colors` (indices 1–15, or
`none` for airborne lightning). Aitos uses palette 1, indices 1–10: mountain
art rather than the grey clouds or bright horizon. Capture walks each active
channel through the native raster scroll and edit-resolved tile pixels, stopping
at its first terrain contact whose native raster camera rate matches the
source cloud. A mismatched source band or missing depth metadata suppresses
an explicitly depth-bound strike rather than guessing. Forks stop independently; branches originating
below the main contact are omitted. No receiver within the maximum reach means
no floating strike. Extending the reach does not inflate the cloud halo or
stroke thickness. These are bounded CPU tile lookups only during active
flashes, with no GPU readback. Game and editor use the same contact logic.

In skybox-only mode, active weather is composited with the captured BG2 art
into a reusable source-sized target **before the existing skybox blur**. The
Metal/Vulkan/DX12 compute projector inverse-maps rigid channels into each
source strip, including generated camera motion; WASM uses the same mapping
on the CPU. Both the native art and lightning therefore receive the same
filtering, palette grading and cloud transmission. There is no sharp duplicate
on top. Quiet or fully offscreen weather adds no composition pass. Finite BG2/combined modes keep
the existing plane attachment, and named ROM replacement skies are unaffected.
No new shader program or readback is introduced; the existing projection
shader is still prepared by the normal boot-time shader setup.

Depth/composition validation (2026-10-28): native and WASM builds pass, including
real BG2 terrain contacts for all three regional profiles at 16:9 and 16:10,
missing-ground suppression, source-before-blur ordering, reverse seeking and
texture reuse. Seven focused native test suites pass. The Metal CPU/source
comparison covers 27 camera/sky/interpolation combinations: maximum channel
difference 2/255, no pixels above that tolerance, and identical retained
packet/fade results. All three shader formats compile.

M2, 1280×800, 600-frame scrolling comparison with a GPU-idle wait per frame:
CPU submission averages 3.428 → 3.386 ms; total completion averages
6.962 → 7.294 ms, medians 6.210 → 6.196 ms (isolated final runs). A deliberately frozen active flash
costs 3.086 → 5.446 ms because the former static backdrop could retain its blur,
whereas the composite must refresh it. This is a feature cost during active
weather, not a claimed pacing improvement or Steam Deck measurement. These
numbers include the complete room preview, not just bolt generation.

Three separated **Cloud-break lighting** regions now replace those small
free-standing fans. Each combines a luminous cream-gold edge, amber spill,
broad oblique shafts and a restrained reflection on bright lower-cloud crests.
The cloud artwork breaks up the light so it retains texture. A shared moving
veil changes all four components together; cloud-bank tint and transparency
sample the same field. Independent seeded positions replace the opening grid:
each opening has its own width, brightness, height offset and short spill length.
The level combines a separated pair, a broad cluster and a single opening, with
unequal region sizes and dark gaps. Adding openings preserves the existing
ones. Their positions remain fixed in unwrapped BG2 coordinates.

`cloud-light` is an ordinary movable/resizable editor emitter with BG2-point
anchoring and background placement. Edit its two colors, intensity, angle,
softness, opening count (1–5), seed, cycle and four component strengths. The
edge/reflection heights are fractions of the full rectangle, so resizing scales
the entire composition. Moving cloud cover controls transmission variation;
beam falloff controls how quickly the sky wash disappears. The map shows
an edge guide, each opening's source width, widening beam envelope and direction,
and a reflection guide. These guides query the shared C layout through WASM. Copy/paste, Shift-reset,
undo and INI/project export use the standard editor paths.

At most four enabled cloud-light regions may be authored per room/terrain;
validation rejects excess regions rather than silently dropping them. This is
a distant BG2 material, not an actor light: foreground terrain covers it and
player/enemy receiver controls are intentionally unavailable. It affects live
captured BG2 in plane, skybox-only and combined modes. Named ROM replacement
skies are unaffected. Periodic waterfall skyboxes now evaluate this field in
unwrapped world coordinates, so the light does not repeat with the artwork.

The six storm sources retain their timing and identities. Their
solid ribbons are replaced by feathered half-ribbons, with higher softness for
the horizon strikes (0.85) than the closer cloud strikes (0.6). Their cloud glow
is slightly wider; culling includes that entire glow. This matches the visual
softness of the distant backdrop without adding a separate blur pass.

Rolling textured cloud banks now support BG2 anchoring when body wakes and
canopy response are off. Those terrain/actor interactions still require BG1;
switching a cloud to BG2 clears them and disables their controls. All banks and
spray regions can be moved, resized, copied and reset using normal editor tools.

Waterfall splash fields retain their native tile witnesses. Their particles
are finer and travel farther, with extra plumes on selected impact ledges. The
old camera-relative bottom mist is disabled in these two rooms and replaced
with ordinary world-anchored cloud-bank recipes. The existing waterfall
animation and 1:1 background strategy remain intact.

The complete effects document contains 906 records. It stays
within the existing document, per-room emitter and geometry budgets.

## Rendering cost

Cloud illumination extends the existing skybox/scenery material shaders; there
is no additional render pass, texture asset, GPU readback or native sprite
allocation. Lightning retains its vertex budget. The existing boot preparation
compiles the updated material and resident-skybox pipelines. Metal, SPIR-V and
DXIL are generated from the same GLSL; the browser adaptation expands the same
shared cloud math for WebGL2.

The first 1080p check exposed unnecessary work in room 1: full-length cloud banks
were resubmitted for every skybox scroll band. Two changes address that:

- Rolling banks and storm regions generate only intersecting geometry, with
  the existing motion apron. Resident skyboxes use conservative bounds from
  their raw captured mapping, because their fitted bands are resolved on the
  GPU. Retained packets refresh when those generation bounds change.
- Unshadowed packets containing exclusively authored fixed-point BG2 weather
  draw once over the union of skybox bands. Native lighting, shadow/transport
  jobs, mixed packets and raster-attached effects keep the original per-band
  path. No shader or platform-specific branch is added.

Apple M2 / Metal, 1920×1080, two runs per configuration, 330 frames each with the
first 30 excluded. Animations advance throughout; screenshot readbacks are
disabled. Values are paired-run mean milliseconds:

| View | CPU before | CPU after | CPU + GPU completion before | CPU + GPU completion after |
| --- | ---: | ---: | ---: | ---: |
| Cloud sea | 2.922 | 2.995 | 5.218 | 6.187 |
| Waterfall crossing | 1.464 | 1.440 | 2.612 | 2.764 |
| Boss arena | 1.428 | 1.450 | 2.742 | 2.940 |

The original unbounded weather version took roughly 9–11 ms in room 1. The
final version adds about 0.7 ms at the median, or 1.0 ms in the paired means,
relative to the scene without the new atmosphere. Small CPU improvements in
other views are run variation, not an optimization claim. These are native
shared-renderer submission/completion timings, **not** game presentation pacing
or Steam Deck/D3D12 measurements.

## Validation and review

- Native Release game and standalone WASM editor rebuild successfully.
- Recipe/parser, authored rendering, receiver-mask and presentation-cache tests
  cover quiet intervals, reverse seeking, geometry bounds, all receiver masks,
  offscreen-centre clouds, resident sky bounds and paused-view resizing.
- Editor interaction tests cover storm placement, resizing, copy/paste,
  defaults and the BG2 cloud restrictions.
- Whole-room tests cover 150 regional rooms, 900 native/WASM surface/source
  matches, authored round trips and resource reuse.
- All 243 Aitos GPU comparison views render. Retained cold/warm frames,
  fade/restoration and invalid-slot fallback match exactly. Bounding source
  geometry preserves the previous GPU image hashes; combining fixed-point
  sky draws changes only one-value raster rounding. The strict CPU/GPU oracle
  still reports a single differing pixel beyond tolerance in 11 views, with
  a maximum channel difference of 48 at a fine particle edge. The baseline
  also has isolated particle-edge differences; this is not bit-exact CPU/GPU
  parity.
- The final weather-only batching guard preserves exact prior GPU image hashes
  in 108 regression views of Fillmore 01:01, Bloodpool 02:01/08 and Marahna
  05:01, including the shadowed moonlight path.

Local artifacts are under `runs/aitos-act1-20261027/`. The
`aitos-act1-preview-640x360.mp4` review is a 26-second, approximately 1.6 MB
shared-renderer camera tour, not a recording of gameplay. It shows room 1's
storm/cloud sea, room 2's crossing and the boss arena.

The later distance calibration is shown in
`runs/aitos-storm-distance-20261027/aitos-distant-lightning-640x360.mp4`, a
10-second 640×360 shared-renderer preview. Strikes were also checked behind
the late-level cliffs. Native game/editor rebuilds, authored-field and recipe
tests, and editor interaction checks pass after this calibration.

The layered storm update is shown in
`runs/aitos-storm-layers-20261027/aitos-storm-layers-640x360.mp4` (12 seconds).
Its native/WASM checks cover all 150 regional rooms and 900 surface/source
comparisons. The final short-ray GPU oracle covers 81 Aitos views across regional profiles,
sky modes, camera poses and interpolation phases: maximum channel difference
1, with exact retained/fade/restoration parity.

Background fans with fixed BG2 anchors and no shadows share the existing
weather batching path. Only intersecting fans generate geometry. Recipes with
all light receivers disabled no longer reserve unused foreground light
geometry; visible passes retain the unchanged 4096-vertex limit, and enabling
receivers still reserves their full cost. No new shader, render pass, texture
or readback is introduced. Legacy fans retain their old zero-modulation output.

1080p Metal samples (330 frames, first 30 excluded) have substantial run-to-run
variation, so they do not establish a speedup or a reliable incremental GPU
cost. They measure shared-renderer submission plus GPU idle completion, not
swapchain pacing or Steam Deck performance. The softer radial fans reserve
3276 vertices in total, including source feathering and soft backdrops. With
the other effects in this pass, the room reserves 3852 of the existing 4096
vertex limit; the normal visibility bounds still skip offscreen fans.

The latest composition review is
`runs/aitos-cloudlight-visibility-20261027/aitos-cloudlight-640x360.mp4`.
This is a data-only adjustment using the same renderer and editor controls;
other rooms' fans and all lightning definitions are unchanged.

## Lava pits

The four native Act 1 pits now have slowly moving cooling patches, hot seams,
swelling bubbles, short sprays of molten droplets and settling ripples. Each
pit has its own deterministic clock and seed; entering the viewport does not
start a burst. Droplets launch from the visible surface and return within the
pit banks. The original animated lava remains visible between the patches.
The local spill is wider and warmer, with scenery, player and enemy receivers
enabled independently of the visible surface detail.

The viscosity tuning uses a 600-frame base flow period (twice the initial
period). Crust deformation follows that same editable clock. Glow shape moves
at half the original rate, while the swell and small flicker run four and six
times slower respectively. Lower pulse amplitudes preserve the mean light
level while reducing rapid brightness changes. Bubble burst timing remains
independent of the slow surface flow.

The canonical definition is `assets/effects/lava-pit-field.ini`, used by all
three regional profiles. In the editor, select a native lava marker and choose
**Edit complete lava / waterfall definition**. The additional controls are:

- `molten-surface`: seam strength, cell count, flow period, depth, drift and
  surface height relative to the captured lava rectangle.
- `molten-bursts`: bubble count, interval, active fraction, droplet height and
  bubble radius.
- `molten-color`: surface tint and opacity. The existing spill/body controls
  and receiver checkboxes govern illumination separately.

Native tile witnesses still locate the pits. Explicit sources support custom
placement and bounds. Molten detail uses alpha draw layer 8 so cooling crust
can darken the lava, while light is routed to the existing BG1 material and
actor receiver maps. Complete definitions validate this separation. Earlier
saved definitions import with molten detail disabled and keep their existing
draw layer, colors and receivers; exporting fills in the new editable controls.
Act 2 lava lakes and their flat heat-refraction path are unchanged. The
separate editable heat regions below add distortion above Act 1 pits.

All geometry uses the existing CPU/WASM and resident GPU paths, with no new
shader, texture, readback or gameplay sprite allocation. The detail shares the
existing alpha pass. Bounds reserve up to 16 cells and four bubble sites per
pit, including overlapping swell/ripple geometry, and tests exercise 16
simultaneous sources against both CPU and GPU packet capacities. The larger
CPU batch reserve is static; unused vertices are not cleared or uploaded.

Apple M2 / Metal, 1920×1080, two alternating runs per configuration, 330 frames
with the first 30 excluded and screenshot readbacks disabled:

| View | CPU mean before / after | CPU + GPU completion median before / after |
| --- | ---: | ---: |
| Small pits | 3.003 / 3.020 ms | 5.685 / 6.067 ms |
| Wide pit | 3.414 / 3.462 ms | 6.055 / 6.442 ms |

These are averages of the two run statistics. The small-pit measurements were
repeated after concurrent editor validation finished; the original noisy runs
are also retained. Presentation pacing, Steam Deck and D3D12 were not measured.

Validation includes native and WASM builds, editor placement/export/undo,
legacy field migration, boundary containment, reverse seeking, translation,
all receiver masks, and 150-room native/WASM parity (900 matching surfaces and
source packets). All 81 targeted real-GPU views render across regional data,
sky modes, orbit and interpolation; retained cold/warm, fade/restoration and
invalid-slot fallback match exactly. CPU/GPU comparison is not bit-exact:
interpolated wide-pit detail differs by at most three pixels with a maximum
three-channel-value difference. A larger sky-mode-1 discrepancy, peaking at
six channel values, is identical when using the legacy lava definition and
predates this change.

Artifacts are in `runs/aitos-lava-20261027/`. The review clip
`aitos-lava-preview-640x360.mp4` is an 18-second shared-renderer preview with
before/after close-ups followed by a scroll past the smaller pits.

## Local lava heat and cloud cleanup

Four ordinary `heat-haze` emitters cover the native pits in all regional
profiles. The three narrow regions are 104×112 pixels at 1.8-pixel amplitude;
the cave pit is 176×128 at 2.2 pixels. Their bottom boundaries meet the lava
surface, with slowly rising waves and smooth margins. The four native pit
positions were checked against the US, Japanese and European maps.

Heat now supports BG1 as well as the existing BG2 anchor. BG1 low/high/far
scenery samples the same displaced world position, including compact captures
and complementary priority filtering. The ordinary material shader handles
this per fragment, so narrow plumes do not slide across a camera-relative mesh.
Actors and HUD stay sharp. Offscreen centres do not cull partially visible
regions. Heat requests no CPU attachment masks. BG2 retains its existing
skybox mapping and culling behavior.

The updated scenery shader ships as Metal, SPIR-V and DXIL and uses the existing
boot prewarm. WebGL2 uses the same GLSL through the shared editor bridge.
No additional draw pass, full-screen target, texture upload or readback is
required. Heat-only rooms reuse the existing single black texel illumination
binding. The four-region room limit bounds the fragment work; pixels outside
all regions skip the wave trigonometry.

Metal at 1920×1080, two alternating 330-frame runs per view, first 30 excluded,
no screenshot readbacks. Mean of per-run completion medians (not presentation
pacing or Steam Deck measurements):

| View | Previous shader, heat off | New shader, heat off | Heat on |
| --- | ---: | ---: | ---: |
| Small pits | 6.131 ms | 6.098 ms | 6.676 ms |
| Cave pit | 6.225 ms | 6.246 ms | 6.768 ms |

Checks include actual GPU pixels against the CPU UV reference, unchanged pixels
outside the plume, compact-source mapping, world-stable camera offsets, no
additional shared-renderer passes, environment disable, editor placement/
resize/export, and all 900 native/WASM room surface comparisons. Preview:
`runs/aitos-heat-20261027/aitos-lava-heat-640x360.mp4`.

## Cloud material polish (2026-10-27)

The cloud-edge treatment described above supersedes the earlier fan-only
calibrations. The full native game and standalone editor build successfully.
The real WebGL2 shared preview was checked at camera 1000,712 / frame 700 in
16:9. Native and WASM checks cover all 150 regional rooms, 900 surface/source
matches and 150 authored round trips. Cloud controls additionally cover BG2
placement, sparse defaults, resizing, angle, copy/paste and Shift-reset/export.

The cloud material GPU test agrees with shared C field sampling within one
channel value across animation phases and compact-source UV mappings. The
Aitos resident/reference oracle passes 27 views across backdrop modes, camera
poses and interpolation phases (maximum channel difference 1). Retained cold,
warm, fade/restoration and invalid-slot fallback comparisons are exact. This
check caught and fixed the resident shader's missing interpolated light-origin
offset. The real browser check also caught and fixed the generated sampler
shim's declaration order; shader compilation errors now appear in the preview.

Apple M2 / Metal, 1920×1080, two runs per configuration, 330 frames with 30
warm-up frames excluded, camera 1000→1320 at Y712 with animation running:

| Cloud treatment | CPU median range | Submit + GPU completion median range | Maximum effect primitives |
| --- | ---: | ---: | ---: |
| Unlit cloud field | 3.14–3.19 ms | 6.50–6.58 ms | 2783 |
| Previous short rays | 3.19–3.20 ms | 6.49–6.51 ms | 4585 |
| Cloud material | 3.29–3.34 ms | 6.55–6.64 ms | 2783 |

The first cloud-material run had a noisy completion tail (12.67 ms p99 versus
7.69 ms in the second); these short GPU-idle samples do not establish swapchain
pacing, a tail-latency regression or handheld performance. Raw timings are in
`runs/aitos-cloud-polish-20261027/bench-*.log`. No readbacks occur inside the
measured benchmark. The cloud material is bounded to four regions and five
openings per region, with an early rectangle reject and no work when disabled.

Review clip: `runs/aitos-cloud-polish-20261027/aitos-cloud-polish-640x360.mp4`.
It is a 10-second 640×360 native shared-renderer camera tour, about 350 KiB,
showing separated openings, warm cloud-sea echo and the softened lower-shelf
strike. It is not a gameplay recording. Metal was exercised; SPIR-V and DXIL
were generated/validated, but this pass did not run on Windows or Steam Deck.

The new standalone C/header/include files pass the house style checks. The
repository-wide style ratchet still reports existing violations across unrelated
source and test files; that broader gate is not claimed as passing here.

The irregular-opening revision has a 14-second, 640×360 review clip at
`runs/aitos-cloud-distribution-20261027/aitos-varied-cloud-light-640x360.mp4`
(610 KB). Shared integer layout hashing replaces the indexed spacing and
mandatory bright middle opening. Widths vary by more than 4:1 across the seed
space; the shipped regions use two, four and one openings respectively.

Native/editor builds, actual WebGL rendering, the shared material pixel test
and all 150 regional room/900 native-WASM surface comparisons pass. The 27-view
Aitos GPU oracle has maximum channel difference 1, with exact retained,
fade/restoration and fallback parity. Metal, SPIR-V and DXIL headers are current.

Two paired 1080p Metal tours (330 frames, 30 warmup frames, no image readbacks)
measured CPU medians of 3.38–3.44 ms before and 3.53–3.64 ms after; submission
plus GPU completion medians were 6.68–6.89 ms before and 6.89–7.07 ms after.
The changed distribution and individual falloffs add roughly 0.2 ms in these
local samples, with unchanged peak geometry (3705 primitives) and no additional
passes or readbacks. Tail timings vary substantially between runs; these are
shared-renderer checks, not game presentation pacing or Deck measurements.

The 2026-10-28 cloud-band pass adds a fourth editable cloud-light region:
broad, softer cream openings on the upper shelf, horizontally staggered from
the warmer middle openings. The lower cloud sea receives stronger reflected
highlights from the existing three regions. The upper field has its own seed,
widths and slower veil cycle; its lower echo is disabled to avoid adding a
second wash over the cloud sea. All three regional profiles use these values.
This uses the existing four-region material budget and adds no passes,
geometry, shader variants or special editor controls.

The upper shelf uses three broad openings in its own region; the middle regions
keep their two/four/one arrangement. The 1080p Metal comparison (two paired
330-frame tours, 30-frame warmup, no readbacks) measured mean-of-run CPU medians
of 3.35 ms before and 3.38 ms after; completion medians were 6.87 ms before and
7.17 ms after. Peak geometry remained 3705 primitives. As above, these are
local shared-renderer measurements, not end-to-end presentation pacing.

The full-opening revision replaces the narrow, periodically striped rays with
one continuous shaft per opening. Its source width matches the luminous cloud
edge, and its soft envelope widens into the distant sky with a longer fade.
The start follows the wavy cloud contact while the shaft keeps a fixed axis.
Cloud cover varies brightness without creating gaps or moving the source.
Warm airborne scattering remains independent of the grey-cloud material mask,
so the light also fills the orange horizon and dark sky. Foreground cliffs
still cover it. Both cloud shelves use this broader composition.

The editor labels the existing controls **Sky beam strength** and **Sky beam
falloff**; their INI keys remain `streak-strength` and `falloff`. Map guides query
source and landing half-widths from the shared C math, and clip the outlined
beam envelopes to the authored region. They scale and rotate with the material.

The revision passes the native shared-field tests, editor controls and actual
WASM receiver tests. The GPU material test additionally checks isolated shafts
over warm and dark backgrounds. Its C/GPU error remains at most one channel
value. All 27 Aitos resident/reference views pass, including retained packets,
fade/restoration and fallback. The rebuilt WebGL editor renders the rays without
shader errors. Native Release and all three generated shader formats build.

Two paired 1080p Metal tours compare this composition with the narrow-strand
version (330 frames, first 30 excluded). The later pair measures CPU medians of
4.04 ms before and 4.06 ms after; submission plus GPU completion medians are
8.07 ms before and 7.83 ms after. Earlier samples have substantial completion
stalls (15.14 ms baseline median and 16.07 ms revised p99), so this does not
establish a speedup or presentation-pacing result. Peak geometry remains 3705
primitives, with no added passes, texture samples or readbacks. The shader also
removes the periodic strand-mask sine. These are Mac measurements, not Steam
Deck or Windows runtime measurements.

Current review: `runs/aitos-cloud-wash-20261028/aitos-cloud-wash-640x360.mp4`
(14 seconds, 640×360, approximately 612 KB). This is a shared-renderer camera
tour, not gameplay. `comparison.png` shows the previous narrow strands beside
the full-opening composition at the same camera and animation frame.


The 2026-10-28 lightning revision replaces the evenly spaced zigzag with a
seeded coarse channel, unequal two-dimensional steps and thinner forks at
separate junctions. The initial flash decays to a faint residual; a return
stroke 4–7 simulation ticks later retraces the same channel and reveals new
forks. Some events have a third pulse. Peaks align to simulation ticks so a
short flash cannot disappear between captures. Paths change only for the next
storm event, independently of camera motion or scrubbing. The six distant
Aitos emitters retain their positions, sizes, softness and receiver settings;
they now have one or two forks. All three regional profiles match. The approved
cloud-opening lighting is unchanged.

The targeted flash comparison exposed a pre-existing discrepancy in the
billboard cloud halo: the CPU/editor path redrew the complete halo for each
intersecting skybox band, while resident static weather drew the union once.
The storm halo now uses a clipped source-space fan, like its channel. This
keeps both paths consistent and reduces its GPU source primitives from 160 to
16. The conservative per-room storm vertex reservation, including maximum
rectangle clipping, is 3808 of the existing 4096-vertex BG2 budget. Return
strokes reuse geometry; there are no new passes, textures, shader variants,
readbacks or actor allocations.

Validation: native Release and editor/WASM rebuilds pass, as do authored-field,
recipe-budget, editor-control, compositor-build and actual WASM receiver tests.
New regressions cover fixed channel coordinates across flashes, delayed fork
brightness, attached branches, extreme-size culling bounds and clipping the
cloud glow to disjoint view bands. The targeted Metal oracle at camera
1100,712 and clocks 703/706/709 checks the initial flash, residual and return
stroke across 27 sky/plane/motion combinations. Maximum channel difference is
2, no pixels exceed tolerance, and retained packets, fade/restoration and
fallback match exactly. Windows and Steam Deck were not run for this revision.

Two alternating before/after 1920×1080 Metal camera tours (330 frames each,
first 30 excluded, no readbacks in the benchmark) measured CPU medians of
4.00–4.14 ms before and 4.27–4.51 ms after; submission plus GPU completion
medians were 7.54–8.00 ms before and 7.87–8.07 ms after. Peak source primitives
fell from 3484 to 2244 despite the added branches. Completion p99 varied from
13.26–17.09 ms before and 15.29–17.52 ms after. These short local samples do not
establish a speedup or game presentation/handheld pacing result; the richer
animation has a modest CPU cost in these samples.

Lightning review: `runs/aitos-lightning-channels-20261028/aitos-lightning-640x360.mp4`.
This is a 14-second 640×360, 60 fps shared-renderer camera tour, approximately
0.6 MB. The higher temporal rate preserves the brief return strokes without
increasing the download resolution. It is not a gameplay recording.


The subsequent timing pass gives the distant strikes more weight: the first
and return strokes hold bright for several frames, decay gradually and leave
a visible channel between discharges. Return spacing now scales with the
whole active duration (30–36% of it), followed by an optional softer third
stroke. A smooth final taper removes the abrupt end of a still-visible pulse.
The existing Active fraction control scales the holds, return spacing and
afterglow together. The Aitos definitions retain their existing 0.63–1.03 second
event windows, positions and shapes; substantially more of that time is now
visibly illuminated. Quiet intervals, geometry budgets and passes are unchanged.

Native Release and editor/WASM builds, field timing/clipping tests, recipe
budgets, editor controls and actual WASM checks pass. Timing assertions require
a multi-frame bright hold, a readable channel between discharges, at least
28 visible frames in a 42-frame event and a smooth fade to near zero. The
27-view Metal comparison at clocks 703/714/716 passes with maximum channel
difference 1 and exact retained/fade/fallback parity. This is a timing-only
change; no new shader or resource path was added.

Slower-lightning review:
`runs/aitos-lightning-linger-20261028/aitos-lightning-linger-640x360.mp4`
(14 seconds, 640×360, 60 fps, approximately 0.6 MB; shared-renderer camera tour).


The distant-source attachment correction moves the four small storm regions
from Y112–114 to Y88–90 in all three regional profiles. Their actual channel
roots were at Y100–103, below the cloud bank in open horizon; they now begin
at Y77–78 inside the lower cloud artwork. This was a placement problem, not a
change to the rigid BG2-point projection. The two larger cloud strikes and all
cloud-opening light regions keep their definitions. Sizes, softness, seeds,
branching, quiet intervals and sustained return-stroke timing are unchanged.

The cloud flash is now centred on the channel root rather than offset below
it. The admission envelope includes its higher extent, preserving offscreen
cloud illumination. The editor shows a dashed Cloud attachment guide over the
possible source positions (the middle 60% of the width, 12% below the top).
It follows moving, resizing and copy/paste. There are no new shader variants,
passes, texture samples or readbacks; per-strike geometry budgets are unchanged.

Validation: native Release and editor/WASM rebuilds, authored-field, recipe,
editor-control, compositor-build and actual WASM receiver tests pass. Tests
cover halo/source alignment and the halo's full culling extent. All four
small emitters were captured at their initial and return flashes. The 27-view
Metal oracle at camera 1940,712 and clocks 1044/1057/1063 passes with maximum
channel error 2, no pixels outside tolerance, and exact retained/fade/fallback
comparisons. Windows and Steam Deck were not run for this placement correction.

Attachment review:
`runs/aitos-lightning-anchors-20261028/aitos-lightning-cloud-anchors-640x360.mp4`
(14 seconds, 640×360, 60 fps, approximately 0.6 MB; shared-renderer camera tour).


## Editor and cost audit (2026-10-28)

All three additions use the shared native/WASM renderer and serialize through
`action-effects.ini`, including project ZIPs. Lightning and cloud-break lighting
are regular map emitters; lava's molten surface/burst/color vectors are exposed
by the complete lava-pit field inspector. Native pit contacts can be extracted,
resized or supplemented with placed sources. Heat shimmer remains a separate
BG1/BG2 emitter. Cloud-break lighting requires BG2 and is limited to four regions
with up to five openings each. Older complete lava definitions keep molten detail
disabled until explicitly enabled; the bundled pit definition enables it.

A small CPU cleanup evaluates the lava ring silhouette once per angular segment
and reuses it across its three concentric rings: 8 evaluations instead of 24.
Before/after molten vertex hashes match exactly across the animation sweep,
including colours and alpha; primitive counts, ordering and passes are unchanged.
No frame-time improvement is claimed from that operation-count reduction.

The next useful measured prototype is preparing cloud-opening invariants once
per region/frame (layout hashes, centre-edge height, transmission and reflected
centre) instead of repeating them in `CloudLightWeights` for every sample.
Compare native GPU and WebGL output, plus high-resolution GPU timings, before
adopting it: driver uniform-expression optimization may already recover some
of that work. Cloud light already shares the scenery/sky material pass, and
fixed-point weather already draws once over the skybox band's union. Inactive
lightning exits before channel construction, so additional path caches are a
lower priority.

The native Release and editor builds, editor controls/export tests, actual WASM
receiver tests, authored-field and recipe checks pass. The existing optimized
surface oracle has a separate pre-existing raw-float checksum mismatch:
`847eeed9` versus its pinned `957be29d`. The renderer from HEAD reproduces the
same mismatch before this cleanup. An instrumented copy records that checksum
instead of aborting there and verifies the remaining surface tests, including
`c1003c3e` for unchanged molten geometry before and after. The stock failing
assertion has not been loosened. It needs investigation or a portable numerical
oracle before claiming the entire Release suite is green. Audit artifacts are
in `runs/aitos-editor-audit-20261028/`.

## Cloud volume pass (2026-10-28)

The two 04:01 cloud banks now opt into `cloud-bank`'s `volume` control in all
three regional profiles. The old rolling-mist crests changed width/height and
used flat porous masks, producing a thin, rubbery sheet. Volume mode uses eight
pairs of density/illumination masks integrated from small 3D cloud fields at
startup. Crests and shaded undersides share identical, immutable geometry.
Slow common drift and smaller depth-dependent offsets move whole billows;
the saved region encloses their full motion. Sky clouds retain BG2 fixed-point
anchors, and existing lightning/cloud-opening definitions are unchanged.

Cloud opening light modulates the baked colors rather than replacing both
with its tint, preserving shaded volume under the strong sun wash. Aitos's
lower bank has warmer silver crests and cooler undersides, with reduced lift,
less lateral drift and a longer cycle. Existing waterfall/floor mist keeps its
previous rendering unless volume is explicitly enabled.

The editor exposes Cloud volume, retains crest/body tint and motion controls,
and supports resizing, copy/paste, shift-reset, undo and project export. Enabling
volume enables rolling; disabling texturing/rolling or tracing a ground path
clears the dependent control. The shared renderer previews the same masks.
Native Release and editor/WASM builds pass, along with authored-field, recipe,
editor controls, actual WASM receiver/motion/seek and compositor-build checks.
Tests cover shape invariance, matched shading, region containment, offscreen
centre visibility and preserving dark cloud bodies in strong cloud light.
The 27-view Metal oracle reports maximum channel error 2 and no pixels beyond
that tolerance, with exact retained/fade/fallback parity.

Geometry remains two quads per lobe in the existing alpha batch, with no new
shader, pass, per-frame volume integration or readback. The atlas expands from
1024×512 to 1024×768 (+1 MiB GPU, +256 KiB cached CPU alpha); a single 500 KiB
startup scratch buffer is reused for all volume variants. The original atlas
pixels match HEAD exactly (`32349dc5`). A standalone cold-bake check on this
M2 measured 23.09 ms CPU before and 60.74 ms after, paid once at startup.

Two alternating 1920×1080 Metal camera sweeps (330 frames, first 30 excluded)
produced the following results. Completion includes CPU submission and waiting
for GPU idle; this is a relative renderer test, not display/present latency.

| Measure (ms) | Before run 1 / 2 | Volume run 1 / 2 |
| --- | --- | --- |
| CPU median | 3.277 / 3.276 | 3.450 / 3.238 |
| Completion median | 7.313 / 7.289 | 7.545 / 7.343 |
| Completion p95 | 14.930 / 7.524 | 15.470 / 7.725 |
| Completion p99 | 15.685 / 7.697 | 16.262 / 8.613 |

The median change is small; the first pair has large tail variation in both
versions, so these runs do not establish an improvement in pacing or a Deck
90 Hz guarantee. Peak submitted primitives decreased slightly (3178 to 3154)
with the changed cloud footprints. Windows and Steam Deck were not measured.
The earlier unrelated optimized surface checksum issue remains as recorded above.

Review: `runs/aitos-cloud-volume-20261028/aitos-volumetric-clouds-640x360.mp4`
(14 seconds, 640×360, 60 fps, 593335 bytes; shared-renderer camera tour).
Build logs, before/after timings, atlas comparison and GPU oracle output are in
`runs/aitos-cloud-volume-20261028/`.

## Shared cloud implementation (2026-10-28)

Cloud materials now live under `src/render/`, without game state or a graphics
API dependency. This is a consolidation, not an art retune: INI definitions,
editor controls, seeds, paused clocks, colours and blend order are unchanged.

| Shared component | Consumers / responsibility |
| --- | --- |
| `cloud_material.c/.h` | Billowed and crown density profiles; paired alpha-mask and lit-slice baking; wispy masks; planar sheet material; conservative atlas support bounds |
| `cloud_noise.h` | Seeded action noise, periodic SIM noise, weighted volume noise and cached spherical scanlines; original interpolation order retained |
| `cloud_motion.h` | Bounded rolling/volumetric banks and orbiting moon clouds; scene adapters supply their original clocks |
| `cloud_mesh.h` | Soft patches, billowed mist slices and smoke/spray puffs, including their original shading and silhouette profiles |

Aitos and Sky Palace use the same density/material model with explicit existing
presets. Either density profile can be baked into paired masks or real slices.
Dimensions, lobe shapes, erosion, light direction, extinction and material
colours remain separate from the atlas layout and scene placement. SIM and
world navigation share their density-to-colour mapping and noise primitives;
the spherical baker retains its scanline cache and pole/seam handling.

Action fog, waterfall spray, moon/shoreline/castle patches and smoke retain their
existing meshes through the shared kernels. Placement adapters still own floor
and collision support, actor wakes, emission lifetime, light sampling, culling,
projection and ordering. The resident GPU floor-mist primitive, SIM's fused
three-bank draw, Sky Palace's slice sorting, and Death Heim's authored nebula
texture/curved surfaces remain specialized rendering paths. They are not
replaced by a generic ray marcher or forced into a different visual profile.

The shared bakers allocate no memory and retain no global scratch. Action's
atomic atlas preparation owns the same 500 KiB scratch and publishes once;
Sky Palace keeps its existing atlas/light cache. Runtime helpers inline into
the existing batches. There are no new shaders, textures, passes, readbacks or
per-frame allocations. Native and WASM builds include the same source through
their manifests; saved editor projects require no conversion.

Validation in `runs/cloud-consolidation-20261028/`:

- All seven full texture comparisons are byte-identical: action masks, SIM
  sheet, world sheet, and Sky Palace under three light directions plus unlit.
- All 21 rendered frames match byte-for-byte across Aitos 04:01/02/03,
  Fillmore 01:02, Bloodpool 02:01/06 and Marahna 05:01, including camera/time
  changes. Peak geometry in the Aitos sweep remains 3,154 primitives.
- The 27-view Metal CPU/resident projection comparison has maximum channel
  error 2, no pixels outside tolerance, and exact retained/fade/fallback reuse.
- Native game and editor/WASM builds, authored-field/render/recipe checks,
  world material and SIM cache/fused-draw checks, editor controls, actual WASM
  preview tests, compositor tests and source-manifest checks pass. The new
  cloud test covers original material samples, alpha-over reconstruction,
  alternative layouts, padding, invalid inputs, footprint bounds and stable
  bank shapes; it also passes ASan/UBSan.
- Two older optimized legacy checksum assertions already fail before this
  refactor. Rebuilding the original geometry confirms identical results:
  atmosphere `84f71706` / `3ec5d3e5`, marsh `f253f37f` with 828,519 vertices and
  3,063,978 indices. Diagnostic copies skipping only those stale assertions
  pass the remaining recipe/export checks in both versions. The tracked
  assertions were not loosened.

Two alternating 1920×1080 Metal sweeps, 330 frames with 30 warmup frames:

| Measure (ms) | Before run 1 / 2 | Consolidated run 1 / 2 |
| --- | --- | --- |
| CPU median | 3.517 / 3.596 | 3.412 / 3.565 |
| Render completion median | 7.624 / 7.610 | 7.431 / 7.621 |
| Completion p95 | 16.307 / 7.934 | 7.883 / 7.912 |
| Completion p99 | 17.425 / 8.263 | 10.095 / 8.493 |

No median regression is evident. Tail variation remains; this establishes
neither a pacing improvement nor a display/90 Hz guarantee. Completion includes
CPU submission and GPU-idle wait, not presentation. Windows and Deck were not
rerun for this portable C refactor.

## Native cloud palette match (2026-10-28)

The two room-1 cloud banks use the native 04:01 BG2 palette as their starting
point. The upper shelf uses sampled warm-grey crests (`7b736a`) and charcoal
shadows (`292931`). The lower cloud sea lifts the sampled rose-tan/brown colours
toward warm white (`e3d0c4`) and pale rose-taupe shadows (`b49a8e`) to match the
brighter native clouds visible at the bottom in game. This keeps their warmth
without the previous pale cream/slate separation or the overly brown first
palette-match pass.

Only these colour pairs changed, across the six records for all three terrain
profiles. Opacity, volume, shapes, placement, motion, cloud-opening light and
waterfall mist are unchanged. The shared renderer was checked in sunlit,
shaded and end-of-level views; the editor defaults were rebuilt and the recipe
validator passed. This is a data-only adjustment with no rendering cost change.

Preview: `runs/aitos-cloud-color-20261028/aitos-cloud-colors-640x360.mp4`
(10 seconds, 640×360, 60 fps). Native palette samples, before/after images and
the isolated INI diff are retained in the same review directory.

Lighter lower-cloud revision:
`runs/aitos-cloud-white-20261028/aitos-lighter-clouds-640x360.mp4`
(10 seconds, 640×360, 60 fps). Only the lower bank's two colours changed across
the three terrain profiles; sun/shade previews, rebuilt editor defaults and
recipe validation were checked again.

## Weather composition optimization and waterfall review (2026-10-28)

The weather source pass now drops empty raster bands and merges adjacent bands
with the same source mapping. The final skybox retains its original raster and
depth bands. Horizontal scroll differences, vertical gaps and different
vertical scales remain separate, so rigid bolts still follow their own cloud
and mountain depth. This reduces repeated source projection and draw work
without adding shaders, allocations or readbacks. It applies to both the
resident GPU projector and the shared CPU/WASM path.

In the active-weather views, ten resident source bands reduce to three or five.
Total primitive submissions in the 27-case comparison fall by 10–14% for
skybox-only mode. Against the approved image, 24 cases are byte-identical; the
remaining three differ by at most 1/255 in only 5–8 color channels per frame.
CPU/resident parity remains within 2/255 with no pixels beyond that tolerance,
and retained packet/fade checks remain exact.

M2 Metal, 1280×800, the same complete-room harness with GPU-idle waits:

| Measurement | Before | Merged source bands |
| --- | ---: | ---: |
| Frozen active flash, completion mean | 5.602 ms | 3.696 ms |
| Frozen active flash, CPU submission mean | 0.683 ms | 0.602 ms |
| Scrolling, completion median, two paired diagnostic runs | 8.455 / 6.252 ms | 6.246 / 6.202 ms |
| Scrolling, completion p99, same runs | 17.893 / 9.734 ms | 15.753 / 17.670 ms |

The focused active-flash result is approximately 34% faster. Scrolling still
has variable tails in both versions; this is not evidence of a general pacing
fix or a 90 Hz guarantee. The harness does not measure swapchain presentation
or the game's producer thread. No new Windows or Steam Deck measurements were
taken for this pass. Ground tracing remains bounded to active strikes and is
too small to justify a new invalidation-sensitive cache.

Validation and timings are in `runs/aitos-effects-opt-20261028/`, including the
paired diagnostic binaries, per-frame timing logs and visual comparison JSON.
Native/editor builds, seven focused native suites, actual WASM receiver and
terrain checks, and the source manifest pass.

Waterfall review: `runs/aitos-effects-opt-20261028/aitos-waterfalls-640x360.mp4`
(12 seconds, 640×360, 30 fps, 995699 bytes). It shows the 04:02 waterfall
crossing and 04:03 boss arena through the shared renderer, without gameplay or
boss simulation. Existing waterfall, splash and mist styling is unchanged in
this optimization pass.

## Softer waterfall mist and overcast sky lighting (2026-10-28)

Rooms 04:02 and 04:03 now use the shared volume-cloud material for the lower
banks, hanging mist and localized splash plumes. All 39 records across the
three terrain profiles retain their authored positions and ranges. Slower
rolling motion, rounded cloud bodies, softer pale-blue illumination and lower
foreground opacity replace the stretched sheet appearance. This uses the
existing immutable body/illumination atlas, without a new volumetric shader or
per-frame texture generation. Native waterfall and splash animation remains
unchanged.

Each room adds one broad `cloud-light` field and one gentle `light-gradient`
bounce, for 12 new records across the terrain profiles. The cloud-light
origins sit above the visible falls; cloud-edge, halo and reflection components
are disabled, leaving broad overlapping sky illumination. The cloud-light
field stays on BG2, while the bounce uses independent scenery, player and
enemy receiver controls, all enabled here. Both remain standard movable and
resizable editor effects, gated by Environmental effects.

Periodic waterfall skyboxes previously bypassed the cloud-light material.
They now pass the field through the resident skybox draw and the shared
CPU/WASM material path. Their unwrapped UVs evaluate lighting in world space;
only the artwork repeats every 256 pixels. The mapping also removes the
native one-row sampling offset. Interpolated motion is applied once, avoiding
light sliding across the falls. The resident path adds no render pass or
readback; the fallback uses the existing scenery-light shader and cached
one-pixel unlit map. Existing boot-time shader preparation still covers both
programs; Metal, SPIR-V and DXIL artifacts were rebuilt.

Validation in `runs/aitos-waterfall-light-20261028/`:

- Native game and editor/WASM builds pass. Seven focused native suites pass,
  including 134 compositor scenarios, along with source-manifest checks.
- The actual WASM receiver test checks both shipped waterfall rooms, world
  mapping, movement without texture creation, and Environmental effects off.
- The 54-view Metal CPU/resident comparison covers both rooms, three camera
  poses, three sky modes and three interpolation phases. The strict image
  oracle reports 11 individual pixels beyond 2/255. Repeating it with the
  original effects produces the same 11 pixel locations in the same views;
  these are existing particle-edge differences, not new lighting errors.
  Maximum difference is 64/255 versus the baseline's 75/255. All retained,
  fade/restored and invalid-slot comparisons remain byte-identical.

Paired M2 Metal measurements at 1280×800, 330 frames with 30 warmup frames:

| Room / run | Completion mean before → after | CPU mean before → after |
| --- | --- | --- |
| Crossing, first pair | 4.275 → 4.644 ms | 2.807 → 2.870 ms |
| Crossing, reverse-order pair | 4.200 → 6.131 ms | 2.876 → 3.030 ms |
| Crossing, confirmation pair | 4.208 → 4.649 ms | 2.776 → 2.877 ms |
| Arena, first pair | 2.700 → 3.281 ms | 1.448 → 1.448 ms |
| Arena, reverse-order pair | 2.694 → 3.276 ms | 1.439 → 1.485 ms |

The repeatable feature cost is about 0.4–0.6 ms in this harness; the slower
crossing run remains recorded rather than discarded. Geometry rises by only
62 primitives in the crossing and 36 in the arena. These measurements include
CPU submission and a GPU-idle wait, not presentation or the game producer.
They establish neither a 90 Hz guarantee nor Steam Deck/Windows runtime
performance; those platforms were not rerun for this styling change.

Preview: `runs/aitos-waterfall-light-20261028/aitos-waterfall-atmosphere-640x360.mp4`
(12 seconds, 640×360, 30 fps, 912739 bytes). The first eight seconds show the
crossing, followed by the arena. This is a shared-renderer scene preview,
without gameplay or boss simulation.

## Warmer waterfall light and faint rainbows (2026-10-28)

The waterfall sky wash now uses a honey-gold crest (`ffd99b`) and amber spill
(`edab60`). Strength is 1.2 in the crossing and 1.15 in the arena. Their soft
platform/actor bounce uses `ffcf8f` / `d7c9ae` at 0.40, retaining the independent
receiver flags. These edits preserve the existing light positions and shapes.

Four local spray banks, two in each room, add a low-strength `rainbow` control
(0.20 or 0.32). Their short spectral arcs are revealed by the same moving mist
density and cloud-light transmission as the existing scene. They stay fixed
inside the bank's world rectangle, instead of cycling colors or following the
camera. Foreground mist samples background light with the captured BG1-to-BG2
translation, shared by native recipe application and the editor. It is a subtle
stylized scattering accent, not physical optical simulation.

The reusable editor control supports resizing, copy/paste, Shift-reset,
undo and project/INI transport. It requires textured rolling mist and a
cloud-light field; zero, absent light, empty mist or the lighting toggle adds no
rainbow geometry. A bounded 8×6 mesh joins the existing alpha batch and samples
the immutable mist atlas on the CPU. There is no image readback, new shader,
texture allocation or full-screen pass. Geometry admission reserves the fully
clipped worst case; all 918 shipped records still fit their room budgets.

Validation: native game and editor/WASM builds, authored-field and recipe
tests, editor terrain/control tests, actual WASM receiver tests and the source
manifest pass. Tests cover light/density gating, separate BG camera movement,
cropping without color shifts, disabled fast paths, seek and texture reuse.
The 54-view CPU/resident GPU oracle retains the same 11 isolated particle-edge
pixels outside 2/255 as the previous waterfall preview (maximum difference
76/255 after recoloring); their coordinates are unchanged. Strict image parity
therefore still reports those existing failures. Retained, fade/restored and
invalid-slot results are all exact, with no new mismatching pixels.

M2 Metal, 1280×800, 330 frames with 30 warmup frames:

| Room | Completion mean before → after | CPU mean before → after | Peak primitive count before → after |
| --- | --- | --- | --- |
| Crossing | 5.599 → 4.589 ms | 2.940 → 2.761 ms | 5384 → 5576 |
| Arena | 3.288 → 3.285 ms | 1.448 → 1.446 ms | 2986 → 3178 |

No meaningful extra cost is evident in this local pair. The crossing baseline
has variable tails, so its apparent improvement is not a claimed speedup.
These completion measurements include submission and GPU-idle waits, not
display pacing or the game producer; Windows/Steam Deck were not measured.

Artifacts: `runs/aitos-waterfall-rainbow-20261028/`. Preview:
`aitos-warm-waterfalls-640x360.mp4` (12 seconds, 640×360, 30 fps, 914015 bytes).

### Visibility and boss-light balance follow-up

The boss-room sky field is now 1400 pixels wide (previously 1000) at 0.90
strength (previously 1.15). Moving its center from X256 to X192 keeps the
clustered openings over the arena as their coverage widens. The platform/actor
bounce widens from 560 to 680 pixels and drops from 0.40 to 0.35. Warm colors,
timing and receiver settings are retained.

The earlier rainbow settings disappeared too easily in ordinary gameplay and
the small video. The two strengths now use 0.90 and 0.65. In the crossing,
the accents move from mist banks `0a400321`/`0a400323` to the existing sunlit
spray banks `0a400320`/`0a400322`; the mist itself does not move. At the review
time, these positions receive about 0.42–0.47 incident light, compared with
0.08–0.14 at the former pockets. The boss keeps its existing two spray accents.
Every regional terrain profile receives the same adjustments.

This is a data-only calibration: the emitter count, shaders, geometry budgets
and rendering passes are unchanged. Recipe validation and the rebuilt shared
editor check the adjusted regions. Review images, data diff and the small
video are in `runs/aitos-waterfall-balance-20261028/`. The actual WASM receiver
suite passes with the updated data. `aitos-balanced-waterfalls-640x360.mp4` is
12 seconds, 640×360, 30 fps and 907940 bytes; the encoded video was checked for
rainbow visibility in both rooms.

## Directional waterfall spray spectrum (2026-10-28)

The earlier implementation fitted the same little arch to every mist rectangle.
Sunlight only modulated its opacity, so the visible curve had no relationship to
the light direction or camera. This has been replaced by a shared optical
response: the viewing ray and incoming sunlight determine the spectral band;
the captured moving spray masks determine which fragments become visible.

- Cloud-break lights now expose `sun-depth` (−85° to 85°, default 48°), alongside
  their existing 2D beam angle. Positive values send sunlight from behind the
  viewer into the scene. The control is editable, copied/exported and resettable
  in the editor; it does not rearrange the cloud openings.
- `rainbow` remains the spray response. Resizing a bank changes its coverage,
  not the spectrum's curvature. Direct shaft illumination, density and viewing
  angle determine the result. Ambient halos and reflected light cannot generate
  a rainbow. All seven local Aitos waterfall spray banks now use 0.9, across
  all three regional profiles, with a shared 48° sun depth in both rooms.
- Native packets retain four sunlight weights per vertex and four directions.
  The existing effect projection compute shader evaluates the spectrum using
  the current matrix and GPU-resident interpolated motion. Rotation, zoom and
  interpolation do not require rebuilding/uploading the optical mesh.
- C/WASM and GLSL share the angular response and view-ray math. The RGB bands
  approximate the primary rainbow around 40–42°, broadened for fine spray and
  the original low-resolution art. This is not a full droplet-size simulation.
- Empty spray/light triangles are omitted independently of viewing angle.
  There is no extra render pass, texture, readback or shader pipeline. The
  updated projection shader uses the existing startup prewarming path.

Validation: native game and shared editor rebuilt; authored-field and recipe
checks passed, including opposite light direction, ambient-only suppression,
brightness response, camera-independent source packets and spectral order.
Editor copy/paste/reset/export and actual WASM receiver tests passed. All 918
bundled records fit their room budgets. Metal, Vulkan and DX12 shader binaries
were regenerated successfully.

The full-room comparison covered 54 views (two rooms, three camera poses,
three skybox modes and three interpolation phases). Retained/cold/warm packets,
presentation fades, restoration and invalid-slot fallback matched exactly.
The strict CPU/GPU image oracle still reports four isolated edge pixels above
2/255 (maximum 48/255); those exact pixels also differ with rainbows disabled.
The new spectrum introduced no additional above-tolerance pixels. This is not
reported as a strict image-oracle pass.

Final paired Mac M2/Metal checks used 1280×800, 630 frames per run, discarded the
first 30, and ran before/current/current/before for each room. Means below average
both runs. Completion time includes CPU submission and a GPU-idle wait; these
are offscreen renderer measurements, not presentation latency or Deck timings.

| Room | CPU before → current | Completion before → current | Max primitives before → current |
| --- | --- | --- | --- |
| 04:02 crossing | 2.101 → 2.163 ms | 3.977 → 4.060 ms | 5,560 → 5,620 |
| 04:03 arena | 1.487 → 1.493 ms | 3.331 → 3.286 ms | 3,178 → 3,158 |

An earlier arena run reached 4.72 ms average completion, and a repeat reached
3.70 ms, before empty-triangle removal. The final runs remain variable in their
tails on both versions; these results do not establish a pacing improvement.
They show a small CPU cost for the expanded crossing coverage and effectively
unchanged arena cost. Retained raw summaries are under
`runs/aitos-waterfall-optics-20261028/`.

Preview: `aitos-light-directed-spray-640x360.mp4` in that run directory, 12 seconds
at 640×360/30 fps and approximately 0.91 MB. It shows a shared-renderer camera
walkthrough, not a gameplay replay.

The lower mist banks in 04:02 and 04:03 now also use `rainbow=0.65` in all
three regional profiles. The local impact spray retains 0.9. Only the main
background bank is enabled; foreground wisps do not duplicate the response.
This uses the same sun/view geometry and direct illumination as the spray.
Colour remains faint on the crossing crests and especially weak on the bottom
of the default boss view, where most of the mist lies outside the spectral
angle. No light direction, cloud placement, shader or render pass was changed.

Bundled recipe validation still passes for all 918 records and room budgets.
The editor was rebuilt with these defaults, and the actual WASM receiver/mist
suite passes. Native Metal previews checked camera positions and animation
phases in both rooms. Review: `runs/aitos-bottom-mist-rainbows-20261028/`, with
`aitos-bottom-mist-rainbows-640x360.mp4` (12 seconds, 891029 bytes).

## Boss funnel flood (2026-10-28)

Room 04:03 now uses one vertical opening centred on the waterfall funnel. The
source is above the visible room, with a broad soft edge; the foreground cliff
walls retain their normal coverage. The crossing's angled lighting is unchanged.
The boss field uses X259, Y300, 1200×1000, angle 0°, seed 517, intensity 1.2 and
streak strength 1.5. Its 41.5° sun depth brings the spectrum into the lower mist.
These definitions are shared by all three regional terrain profiles.

The initial flood was too washed out. Its direct strength was reduced by 44%
and its width by 25%, restoring cooler waterfall shadows and detail. The lower
mist's rainbow response increases from 0.65 to 1, so the spectrum can remain
visible without bleaching the entire background. Platform/actor bounce retains
0.35 strength with a warmer cream tint.

Rolling BG1 mist can now sample Cloud-break sunlight through its existing
`canopy-response` property, labelled **Light response** in the editor. The lower
bank uses 0.8, airborne mist 0.55 and local platform spray 0.5. Existing body and
crest vertex colours respond to the shared field, with stronger crest lighting
and preserved alpha and geometry. BG1 samples translate into BG2 world space.
The response remains opt-in; there are no new shader programs, textures,
readbacks, geometry batches or rendering passes.

Native game and shared editor rebuilds pass. Focused C checks cover warm tint,
unchanged opacity/geometry, relative BG scrolling, partial crops and disabling
lighting/response. Editor interaction and actual WASM checks pass, including
the lower bank at 16:9 and 16:10 with texture reuse. All 918 recipe records fit
the existing room budgets. The final native comparison renders all 54 views
and retains exact cached/faded/restored results. Its four known CPU/GPU edge
pixels remain at the same coordinates as the preceding baseline (maximum
48/255); this is not a strict image-oracle pass.

The initial stronger flood was measured in four alternating 630-frame M2/Metal
runs at 1280×800, with the first 30 frames excluded. CPU means were 1.58/1.64 ms
before and 1.52/1.52 ms with the flood; completion means were 4.25/4.43 ms and
3.64/3.65 ms, respectively. Both versions emitted at most 3242 primitives. The
subsequent brightness-only calibration was not separately timed. These are
offscreen submission/GPU-idle measurements, not display pacing or Deck results.

Review files: `runs/aitos-boss-flood-20261028/`. The final
`aitos-boss-vertical-flood-balanced-640x360.mp4` is a 10-second shared-renderer
preview with a player stand-in, 640×360 at 30 fps, 708613 bytes.

## Golden-hour boss crevice (2026-10-28)

The boss waterfall now keeps the warm palette of the preceding rooms. Its
Cloud-break field uses cream `ffe6b5` and amber `eeb278`, intensity 1.5 and
streak strength 1.6 (33% more direct strength than the balanced flood above).
The new `background-brightness=0.76` lowers the waterfall's base by 24% before
illumination, deepening the sides without dimming away the central sunlight.
The opening's position, vertical direction, width and sun depth are unchanged.
The scenery/actor bounce rises to 0.5 with `ffe4b8` / `d8b995` colours. Lower
and hanging mist, plus local platform spray, use warmer cream body colours.
The crossing remains unchanged; all three boss terrain profiles match.

Background brightness is a reusable Cloud-break inspector control. It operates
inside the existing BG2 plane/skybox material, fades over 10% of each region's
borders, and merges overlaps by maximum darkness. Default 1 is neutral. It adds
one vec4 of uniforms, no render pass, texture, readback or geometry. Existing
boot shader preparation covers the updated shaders. Metal, SPIR-V and DXIL
artifacts were regenerated; runtime GPU validation was on Mac/Metal.

Native game and shared editor builds pass. C recipe/field tests, editor
copy/paste/reset/export checks, real WASM tests at 16:9/16:10, and Metal shader
pixel tests pass. GPU pixel tests cover dim-before-light order, overlapping
regions, smooth borders, neutral defaults and compact capture coordinates.
The unchanged crossing renders byte-for-byte identically with the old and new
binaries. The 54-view room comparison retains the same four single-pixel edge
differences at the same coordinates (peak 48/255); cached, faded and restored
results remain exact. It remains a known non-strict image-oracle result.

Four alternating 630-frame runs at 1280×800 excluded the first 30 frames.
Before CPU means: 1.5713 / 1.5707 ms; after: 1.6342 / 1.5237 ms. Completion
means: 3.9093 / 3.6771 ms before and 4.3665 / 3.6648 ms after. These noisy
offscreen submission/GPU-idle samples do not establish a small timing delta
or display pacing. Both versions still emit at most 3242 primitives.

Review and validation: `runs/aitos-boss-golden-crevice-20261028/`. The final
`aitos-boss-golden-hour-640x360.mp4` is a 10-second shared-renderer preview with
a player stand-in, 640×360 at 30 fps, 614686 bytes.

## Sunlit waterfall and mist sparkles (2026-10-28)

The crossing's Cloud-break intensity rises from 1.2 to 1.3 and its bounced
light from 0.40 to 0.44. The boss uses 1.65 and 0.55, up from 1.5 and 0.5.
Their light geometry, warm colours and background dimming remain unchanged.

Both backgrounds now have an ordinary BG2-anchored particle region using
`pattern=water-glints`. Small reflections fall with the water and twinkle only
where the same direct sunlight illuminates the background. Foreground scenery
covers them. The existing fixed surface-glint pattern remains unchanged.
The lower foreground mist and local impact spray use `sparkle=1`: at most
four independently timed droplets per body lobe follow its motion, sample its
density mask and catch direct sunlight. Empty air, dark mist and ambient halo
alone produce no sparkle. BG1 droplets translate into BG2 coordinates for light
sampling. All three regional profiles match; only 04:02 and 04:03 data changed.

Both controls are editable, resizable and included in copy/paste, Shift-reset
and project/INI export. Fixed room/pass budgets reserve the maximum geometry.
The water uses the existing additive BG2 decoration pass; mist droplets share
the existing alpha batch. There are no new shaders, render targets or readbacks.

Focused native field/recipe tests, editor interactions and actual WASM checks
pass, including both rooms at 16:9/16:10, all three background modes, light
disable, camera translation, reverse seeking and texture reuse. All 924 bundled
records fit their budgets. The editor and an isolated optimized AR1 game build
pass. The regular play build was blocked by an unrelated optional AR2 generator
error assigning `bank_80_82C3_entry_M1X0` to a stable PC chunk; that work was left
untouched. The AR1 build used `-DAR2_CONTENT_PACK=` in its own run directory.

All 54 native comparison views render, with exact retained, faded and restored
results. Four known single-pixel CPU/GPU edge differences remain (peak 48/255).
Two additional edge pixels in the crossing differ by 4/255 and 3/255; this is
not a strict image-oracle pass. Their coordinates and the full summary are in
`runs/aitos-water-sparkles-20261028/oracle-summary.json`.

Four alternating 630-frame runs per room at 1280×800 excluded the first 30
frames. These are offscreen CPU submission/GPU-idle measurements on M2/Metal,
not display pacing or Steam Deck results:

| Room | CPU before / current means (ms) | Completion before / current means (ms) | Max primitives before → current |
| --- | --- | --- | --- |
| 04:02 | 2.804, 4.530 / 4.483, 4.557 | 7.025, 8.586 / 8.585, 8.553 | 5,688 → 5,761 |
| 04:03 | 2.487, 2.280 / 1.918, 1.914 | 6.629, 6.223 / 5.831, 5.868 | 3,242 → 3,301 |

The baseline variation prevents a reliable small timing delta or speedup claim.
The final crossing baseline is comparable to the updated runs; primitive counts
rise by 1.3% and 1.8%, respectively. Raw results are in `perf-summary.json`.

Preview: `runs/aitos-water-sparkles-20261028/aitos-sunlit-water-sparkles-640x360.mp4`,
12 seconds at 640×360/30 fps, 751935 bytes. It is a shared-renderer camera
walkthrough of the crossing and boss crevice, with a player stand-in in the boss
view, rather than a gameplay replay.

## Crossing sunlight contrast (2026-10-28)

Room 04:02's direct sunlight now uses intensity 1.65 and streak strength 1.35
(previously 1.3 and 1.2). Background base brightness is 0.86, retaining deeper
waterfall shadows beneath the warmer highlights. The broad foreground bounce
decreases from 0.44 to 0.40 and hanging haze from 0.25 to 0.18, so the stronger
sunlight does not turn the whole composition into a pale wash. Existing light
positions, angles, colours, lower mist and sparkles are retained. All three
regional profiles match; this is an INI-only adjustment using existing passes.

Comparison renders checked both ends of the crossing. Bundled recipe validation
and actual WASM receiver checks pass, including 16:9/16:10 and all background
modes. The editor was rebuilt with the new defaults. Review files are in
`runs/aitos-room2-lighting-20261028/`; the scrolling shared-renderer preview is
`aitos-room2-warm-sunlight-640x360.mp4` (8 seconds, 640×360, 582803 bytes).

## Native environment sweeps and Deck upload audit (2026-10-28)

`actraiser_action_room_benchmark` and `tools/action_editor/benchmark_rooms.py`
exercise the shared complete-room renderer directly on Metal, Vulkan and DX12.
The 17-route plan covers Marahna 05:01–08 and Aitos 04:01–03, including lower
corridors, upper paths, lava and both waterfall rooms. Capture review corrected
three interior routes that initially passed through solid scenery. Death Heim
is intentionally excluded from this optimization pass.

Each 1280×800 execution warms the entire 240-frame route and animation interval
before repeating it for measurement. Three repeats alternate effects off/on
and backend order. The Deck display is awakened and checked before every run;
telemetry rejects sleeping-display runs. Windows has an off/locked display, so
its alternating DX12/Vulkan runs establish compatibility and gross relative
backend behaviour only. No screenshots, encoding or readbacks occur in timed
frames. Capture mode instead writes small-resolution compressed PNG sequences.

Two independent costs were found:

- The cloud-light GLSL helper took a 24-vec4 uniform array by value. Optimized
  SPIR-V still materialized a private array and 24 stores per fragment. Helpers
  now read the existing uniform block directly. The lighting math and uniform
  layout are unchanged; there are no new passes, textures, readbacks or quality
  reductions. The intermediate CPU-precomputed-opening experiment was removed
  because it added complexity without improving the measurements.
- SDL's fresh upload allocations caused alignment-sensitive CPU overhead on
  the Deck. User-mode instructions and cycles were nearly unchanged between
  builds, but minor page faults rose from 27,509 to 321,560 over identical
  240-warmup/240-measured effects-off sweeps. `perf` located 95.9% of candidate
  faults in libc copying SDL texture upload data, rather than scene/light math.
  The two layouts selected different forward/backward copy loops. Reusing
  staging removes the repeated mapping cost: three alternating Aitos trials
  reduced CPU preparation from about 15.2 to 9.1 ms with unchanged pixels.
  Vulkan now uses the existing DX12 retained-upload path. SDL cycles buffers
  still in flight, D3D12 row alignment is retained, and upload memory is only
  written. Metal's default policy is unchanged.

Validation includes real GPU material/reference tests on Metal, Vulkan and
DX12; all 16 original/optimized full-frame Windows comparisons have zero
changed pixels across both backends. Upload ordering tests pass on Deck Vulkan,
Windows Vulkan/DX12 and Mac Metal, including odd-width subrectangles, alternating
updates/draws and destruction while work is in flight. The actual WASM shared
editor receiver tests and an isolated optimized AR1 game build also pass.
The whole-repository render boundary check still reports the pre-existing SDL
interop include in `src/host/imported_world_travel.c`; that file is unchanged.

Timing fields deliberately separate CPU room rasterization/upload preparation
from effect composition/submission and the remaining GPU-idle wait. The latter
is not a hardware GPU timestamp. This serial harness does not measure game
logic, the game's overlapped producer, interpolation, CRT or display pacing;
its results alone cannot establish 90 Hz gameplay.

Raw CSVs, logs, hashes, telemetry, shader comparisons and captures are under
`runs/environment-sweeps-20261028/`. Historical run folders removed by the user
were not restored. Regular screenshots, F2 snapshots, scene-asset images and
shared-renderer captures now use compressed lossless PNG; native memory/PPU
dumps remain binary. Legacy explicit PPM artifact contracts remain supported.
Encoding is exercised only on requested captures, outside benchmark timings.

Final Deck validation completed 102 route sweeps (17 routes × off/on × three
repeats) and 12 fresh alternating control/current runs. No run crashed, lost
the awake-display check or logged a shader build during measurement. Real
Deck/Vulkan cloud material comparisons against the CPU reference are within
1/255 per channel. Values below are medians of three per-run statistics:

| Deck route, effects on | Serial mean before → current | Serial p99 before → current | Composition + completion mean before → current |
| --- | ---: | ---: | ---: |
| Aitos cloud route | 27.45 → 14.23 ms | 29.38 → 15.94 ms | 17.47 → 5.14 ms |
| Aitos waterfall boss | 25.01 → 13.89 ms | 25.72 → 14.28 ms | 15.32 → 5.12 ms |

Across all 17 effects-on routes, composition plus remaining GPU completion
means range from 2.96 to 6.22 ms; median per-run p99s range from 3.21 to 7.96 ms.
The native editor's CPU tile preparation still takes 5.54–13.01 ms by route.
That is the next harness bottleneck; it should not be mistaken for the game's
GPU background producer. End-to-end gameplay at 90 Hz remains unverified by
this offscreen harness. Full per-route values are in `results-summary.json`,
with `deck-results/retained-sweeps` and `retained-paired` holding the raw runs.

The final Windows matrix completed 72 runs over six representative routes,
three repeats of effects off/on on both backends, with no crashes or late
shader builds. DX12 composition/completion means with effects enabled were
1.12–2.07 ms; Vulkan was 0.95–1.60 ms. These off-display measurements establish
no gross backend slowdown, not comparative hardware performance or pacing.
After both fixes, all 24 final full-frame original/current comparisons
(8 Deck Vulkan, 8 Windows Vulkan, 8 Windows DX12) are pixel-identical, including
clouds, lava, waterfalls, swamp and a lit temple gallery. The old executable's
BMP evidence was converted to PNG and its decoded RGB verified before removing
the local BMP copies. New capture code writes compressed PNG directly.
