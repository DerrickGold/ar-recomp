# Continuous globe town experiment

Status: **continuous globe is the default connected SIM path**, September 13,
2026. Six-town visual acceptance and repeated Mac/Steam Deck performance
comparisons are complete; see [performance acceptance](sim-globe-performance-acceptance.md).
The prototype compilation switch, plateau compositor, transition collar and
obsolete whole-image cache have been removed, including their test-only
selection paths. The supported choices are ordinary flat SIM when the globe
setting is off, and the current continuous globe when it is on. The shared
landscape factory default is **40%** and the SIM radius is 3× navigation;
explicit saved preferences are preserved. Earlier experiments below are
historical records, not current build instructions or shipping alternatives.

The experiment now has a matched **current enhanced SIM versus continuous
globe background** capture, using a real paused Aitos WRAM/VRAM/CGRAM snapshot.
This is not a native-2D comparison or a full gameplay acceptance test.

## Question

Can the existing navigation globe become the SIM scene, with higher detail in
the active town and gentler curvature, instead of placing a separate flat town
canvas over the globe?

The first experiment deliberately keeps the globe's actual town tiles,
terrain, cliff and mountain representations. It removes the active-town hole
and the eight-cell flat-to-spherical transition collar. This isolates how far
radius, camera and detail selection get us before changing authored geometry.

## Implementation

- `SimGlobeMapping` has explicit connected-town and continuous-surface modes.
  The existing builder still defaults to connected-town placement.
- `PresentSimGlobe_TestTownScene`, compiled only with `AR_SIM_GLOBE_TESTING`,
  reuses the world renderer's resource owner, surface publication, model cache,
  GPU depth pass and retained-image policy. It accepts the SIM camera/matrix;
  it does not invoke the navigation camera or UI compositor.
- The active town uses the capture's `background_voxel_detail`; nearby towns
  remain Low. The nearby-model/mountain window is not expanded with radius.
- Radius is explicit through mountain transitions, cliffs and atlas recovery,
  not changed only in the final vertex transform. Switching radii invalidates
  the appropriate retained sources/images and restores ordinary navigation.
- Active-town detail and the active Aitos lava atlas participate in image
  invalidation. Background-wide haze/dimming is not applied over the active town.
- `PresentSimGlobe_TestFacingTownScene` adds the active town's existing SIM
  per-kind facing axes and shading. Model footprints stay on the continuous
  ground; only authored model height uses the camera-facing direction. Bridges
  and neighbouring Low-detail models retain geometric radial placement.
- Facing models use the existing generic linear-displacement GPU contract.
  Static models are published once, and all three windmill poses are prepared
  up front in separate retained mesh sets. Camera/facing/pose changes update
  uniforms or select resident poses, not model vertices. The one active-town
  source selection replaces its predecessor; it does not accumulate travel
  history. Mesh sets use the existing bounded/chunked publication contract.
- Both representations share model preparation, proportions and palette
  inputs. Material-aware face colors reuse the same calculation as SIM, not
  a new copy of its lighting policy. Image keys include facing/shading;
  camera-dependent axes are deliberately excluded from source-cache keys.
- No native gameplay coordinates, runner ABI, capture structure, settings
  persistence, save or release artifacts change. The detailed comparison adds
  a generic borrowed texture option to the private depth-surface contract,
  described below; existing callers retain their layer-atlas defaults.

## Reproducing the captures

Build the existing GPU integration test in a desktop test build:

```sh
cmake --build <test-build> --target actraiser_present_world_nav_gpu_test
```

With an existing output directory, local original-game ROM, and a populated
navigation WRAM dump (`$18=0`, `$19=9`):

```sh
<test-build>/actraiser_present_world_nav_gpu_test \
  <ROM> <navigation-WRAM> <output-directory> --sim-globe-prototype
```

The inputs are read-only. The test first runs its synthetic regression suite,
then captures six towns at two SIM pitches and three radii. Outputs are named
`globe-town-<town>-view-<view>-radius-<scale>.ppm`. View 0 uses pitch -0.575;
view 1 uses -1.15. Both keep camera distance 4.5, yaw 0, the same centered town
and source rectangle, and Ultra active-town detail.

Scales 1, 2 and 3 mean 96, 192 and 288 chart tiles: multiples of **today's
navigation radius**, not multiples of the original 48-tile globe. Local tile
scale is normalized at the town center; this is not a camera zoom comparison.

An additional camera-only comparison fixes radius at 2x navigation and uses
pitches -0.575, -0.750, -0.900 and -1.050, without changing the buildings.
Outputs are `globe-town-<town>-camera-<positive-milliradians>-radius-2.ppm`.

The selected continuation uses pitch **-0.750**, radius 2x navigation, and the
same distance 4.5. Compare `globe-town-<town>-camera-750-radius-2.ppm` with
`globe-town-<town>-camera-750-radius-2-sim-facades.ppm`. The latter enables
PerModel facing, Ultra active-town models, Varied style and MaterialAware
shading; nearby towns remain Low. This pair changes both facing and material
shading. It is not a camera-only or native-versus-enhanced comparison.

These are clean geometry comparisons with a constant blue clear. They do not
contain SIM actors, menus, particles, cloud cover or final focus lighting.
They are not screenshots of a complete playable SIM mode.

## Initial observations

The six-town Metal capture matrix completed with exact direct/retained-image
parity. Revisiting previous radii, the shipped underlay and navigation also
restored identical pixels without a resource reset. Captured game data and the
published world-map serial remained unchanged.

The 2x and 3x variants visibly reduce the local roll-off while keeping the town
near the same scale. At low angles they expose more distant geography, so
focus haze and a deliberate visibility policy will matter. The globe volcano
and mountain silhouettes still differ from the authored close-up SIM facade;
increasing radius alone does not solve that difference. No performance gain
or cost is established by these frozen correctness captures.

The four-angle comparison keeps building meshes unchanged. The moderate
-0.750/-0.900 pitches show more of the fronts while retaining more town depth
than -1.050. This supports evaluating camera/framing before adding facade
deformation; it does not establish an exact match to native SIM art.

The active-town facing continuation improves front-face readability at the
selected -0.750 pitch without changing the ground or the camera. Trees use
SIM's existing, weaker per-kind correction; this is not a full billboard of
every object. Aitos still needs a separate close-up mountain/volcano pass.

Validation includes exact direct/retained-image parity, shading/facing and
lighting invalidation, three windmill poses and rewind, unchanged bridge
geometry and unchanged visible neighbouring Low models. The warm camera/pose
sweep records zero geometry publications. All 170 desktop tests passed with
the facing continuation, and the six-town facing captures passed separately on
Metal. No release was rebuilt as part of this experiment. These are correctness
and upload-traffic tests, not clean frame-rate benchmarks or gameplay tests.

## Movement and remaining integration

Keep native pathfinding, collision and movement in native town coordinates.
Interpolate displayed chart positions first, then evaluate the curved surface
and altitude. Straight interpolation between already-mapped 3D endpoints makes
a chord and can put a walking actor below the terrain. Mapping tests cover
continuous walking and ground-relative flight over all six towns at four radii,
including varying terrain height, constant flight altitude and radial GPU
round trips. These tests exercise the mapping, not live actor integration.

Before making the prototype playable:

1. Validate the selected -0.750 camera and active-town-only SIM facade treatment
   in live building/miracle views. Keep rigid anchors, foundations and bridge
   rules; do not compensate terrain or native gameplay coordinates for model
   presentation. The synthetic GPU test checks zero geometry publications
   during camera motion and windmill animation.
2. Expand the matched SIM-quality mountain/volcano validation beyond Aitos,
   including native atlas refreshes and all six towns' terrain boundaries.
3. Give actors, shadows, cursor/map-plane graphics, projectiles and effects one
   explicit curved placement/depth contract. Preserve native priority bands
   and multipart sprite ordering. Fixed menus must remain screen-space.
4. Distinguish ground-following altitude from authored aerial trajectories.
   Volcano fireballs must launch at the displayed crater and follow their
   intended arc; ordinary flight must not acquire unintended terrain-following
   vertical motion. Verify interpolated walking, angel flight and selection
   against visible ground at low pitch, zoom limits, cliffs and town edges.
5. Add spatial focus lighting/haze and curved clouds without bringing back a
   rectangular visual boundary. Recheck nearby visibility and all-effects cost.

Choose the visual base before adding a shipping settings path. Keep the
existing connected-town mode as the reference until those checks pass.

## Matched background comparison

The new `--sim-pair` capture mode uses the production SIM terrain and voxel
renderer for the current side, over its shipping connected globe underlay.
The alternate side uses the continuous globe with these additions:

- The centre's native SIM elevation is preserved. Keeping the same camera
  must not silently lower the whole town when changing surface shape.
- Active models use the published SIM scene's exact identities and individual
  animation phases, including stopped windmills. Neighbours retain navigation
  identities and Low detail. The active town is never drawn twice.
- The SIM mountain builder exports its authored relief, rounded caps and
  skirts before projection. A presentation-owned adapter embeds their bases
  on the globe and applies the existing mountain facing axis only to relief
  height. These replace the active town's navigation mountains and share
  terrain/model D32 occlusion, while sampling the real SIM mountain atlas.
- The existing gradient backdrop and curved cloud renderer are shared by both
  sides, with the same settings and weather clock.

The mountain export is synchronous and retains no callback or input pointer.
It neither emits eruption effects nor changes the projected-face cache/crater
anchor. Its adapter owns one bounded source selection. Changed camera axes
rebuild textured mountain geometry; unchanged views reuse it. This is not a
zero-upload or performance claim for camera motion. Model geometry retains
the existing linear-displacement path. No runner ABI was extended.

### Active ground-art and lighting parity

The first matched captures exposed missing Aitos pen fences and darker ground
and cliffs. Navigation's reconstruction marked non-model structures for grass
replacement, although fields/support artwork has no corresponding 3D model.
Its terrain lighting also multiplied already-shaded cliff faces a second time.

The detailed experiment now exports the same SIM terrain tops, skirts, UVs and
slope/contact lighting before projection. A bounded, cached adapter embeds
them on the curved globe using the existing world datum and border registration.
Authored town terrain now takes priority over the inferred coastal ramp, as
described below. It samples the already-uploaded cleaned SIM canvas, preserving
pens and other non-model artwork. Active globe grid/cliff faces are excluded;
this is a replacement surface, not a duplicate overlay or a flat plateau.
Active model/mountain floor anchors use the same registration. Ordinary globe
navigation and neighbouring Low-detail models remain unchanged.

`Sim3DDepthSurfaceBatch.texture` is a backend-neutral, optional borrowed
material handle. Its invalid default selects the existing layer atlas. The
backend validates renderer ownership for every batch before queue mutation,
borrows the texture only through submission, and preserves layer filtering,
depth and alpha policies. Shadows and overlays retain their own textures.
Texture binds change only when necessary, with no new atlas copy/upload,
shader variant, or persistent ownership transferred to the depth backend.
Ground source geometry is rebuilt only when map/terrain inputs change, not
for ordinary camera movement or animated ground texture updates.

To share actual presentation components with the capture test, unchanged
camera/terrain helpers moved from `present_sim3d_project.c` to
`present_sim3d_geometry.c`, and the backdrop wrapper moved to
`present_sim3d_backdrop.c`. Pure cloud/cull coverage functions moved from
metadata tracking to `sim_render_visibility.c`. Actor/effect placement and
metadata producer ownership remain in their existing layers. All new game
translation units are listed in `snesbuild.ini`.

### Reproduction

Use a real town snapshot prefix containing `.wram.bin`, `.vram.bin` and
`.cgram.bin`, such as a game `AR_VRAMDUMP_GF` snapshot. VRAM/CGRAM words are
decoded explicitly as little-endian. The test rebuilds the developed globe
with the production pure HLE compositor from the **same WRAM**, rather than
reading town-mode scratch as if it were a navigation map.

```sh
<test-build>/actraiser_present_world_nav_gpu_test \
  <ROM> <snapshot-prefix>.wram.bin <existing-output-directory> \
  --sim-pair <snapshot-prefix>
```

Both sides use 800x600 output, a 360x224 source, pitch -0.750, yaw zero for
stills, Ultra/Fixed active models, MaterialAware/Varied/PerModel presentation,
Native render scale, 100% landscape/model height and light azimuth 0° /
elevation 85°. The globe is 2x today's navigation radius. Stills cover:

| View | Native camera X/Y | Distance |
| --- | --- | --- |
| overview | 128 / 144 | 4.5 |
| north-close | 128 / 64 | 3.2 |
| south-edge | 128 / 256 | 4.5 |

Each produces a clear pair and a cloudy pair. Backdrop is pinned to
`#305888`, strength 100%, horizon 50%; clouds use 55% opacity, 96px altitude,
24px inset, 96px falloff and an identical frozen weather clock. The 24-frame
camera sweep adds a small yaw/pan/zoom and advances the cloud clock equally,
while simulation and each structure's pose remain frozen.

Actors, menus, miracle targeting, cast shadows, focus haze/dimming, scene
particles and separate volcano glow/smoke are omitted on **both** sides.
Material shading, SIM contact geometry and each renderer's ground treatment
remain visible; these are representations being compared, not a promise of
identical rasterization. Known grey cliff triangles remain out of scope.

### Validation and limitations

The September 13 Aitos capture contains 156 active SIM objects and 398 mountain
cells. All six still pairs passed exact repeat/direct-retained checks, exact
return-to-SIM and underlay restoration, and immutable captured-input checks.
The camera/cloud sweep completed successfully. The complete 170-test desktop
suite passed with the actual Metal GPU tests running, and a separate full game
build linked successfully. No release or shipping settings were replaced.

The capture prepares all required depth/model pipelines before either side,
matching game boot. Without this, the first reference draw used the CPU model
path and the first facing-globe draw enabled the linear GPU path, invalidating
the comparison. That harness issue was corrected rather than accepting a
pixel tolerance for mismatched paths.

This is sufficient to compare the background visual base, not to enable the
continuous scene for gameplay. Live actors, effects, shadows, priority bands
and picking still need one curved placement/composition contract. Ground
focus treatment and close-up joins outside Aitos also remain to be validated.
Builds and correctness captures overlapped other work on this machine; their
durations are not frame-rate benchmarks.

## Six-town integration and ownership audit

The next pass uses independent copies of `backup-testing-2.srm` (SHA-256
`480a8375b6255ad681202986640c5b06889458125ec0f82641a0e4fb425c0d45`).
The original save is unchanged. Native transition requests `AR_WARP=0001`
through `0006` at game frame 1800, following `saves/aitos-eruption.rec` with
replay-nostop, load each town's actual WRAM/VRAM/CGRAM. Snapshots at frame 2600
are checked for `$18=00`, `$19=town` before paired rendering. These are native
town loads, not hand-authored terrain/object state. They do not exercise the
complete natural travel/menu flow.

All six towns are enabled, with 1,055 captured world objects. Active detailed
scenes contain 251 / 231 / 175 / 162 / 196 / 2 objects respectively, and
183 / 133 / 202 / 398 / 0 / 302 mountain cells. Northwall is accessible but
mostly undeveloped; a populated Northwall performance/gameplay check remains.

### Corrected competing terrain transforms

The common registered plain was already 4 units for both Fillmore and Bloodpool.
The old coast prior then multiplied the entire combined height by an inferred
four-cell inland ramp. It lowered their otherwise level connection to 1.3125
units at world (81,66), and pulled Kasandora's western land down toward the sea.
This was not an intentional town altitude difference or extra globe curvature.

`FloorOwned` now applies the coastal prior only to the *inferred* contribution.
The native contribution retains its registered land, water floors and hard
cliffs. Existing town weights grade to inferred ocean outside the footprint;
there is no extra inland coastal deformation. Tests cover the level coastal
connection, native Kasandora contours, outside ocean datum, inland lakes and
cliff ownership. This correction is shared by navigation, the connected SIM
underlay and the continuous experiment.

A trial that simply narrowed the coastal mask was discarded: it moved the
deformation into a steeper band and stretched source artwork. The retained
fix assigns precedence to authored data instead. Ocean color/material seams
at the native canvas boundary are a separate remaining issue; do not fix them
by fading valid land into ocean or deforming its height.

### Removed redundant work

- Detailed models and mountain vertices select either the owned native floor
  or the inferred outside floor. They no longer sample the inferred floor
  only to overwrite it immediately with the native result.
- Mountain source caching uses the resolved relief detail, facing axis and
  stack direction, not the full camera matrix. Pan and fixed-detail zoom reuse
  the source. Pitch/yaw or effective-detail changes still rebuild it.
- A changed SIM scene serial compares owned mountain/cap data before forcing
  a rebuild, so unrelated scene publications need not invalidate that source.
  This remains one bounded cache with explicit teardown, not a history cache.
- All-town capture tests compare warm and forced-cold mountain images at 12
  pan/zoom positions plus orientation/detail changes. They require exact
  pixels and the expected revision reuse/invalidation; retained-image caching
  is disabled for this oracle.

### Transformation ownership

| Operation | Owner and audit result |
| --- | --- |
| Native contour plus shared town datum | Terrain registration, once; inferred coast no longer modifies the authored contribution |
| Town chart into the curved world | `SimGlobeMapping`, once per published point; local scale and center reference are coordinate conversions, not additional relief |
| Model height/proportions | Shared model preparation, once; facade axes act on model rise only, not the foundation |
| Mountain rise | Native source emitter exports relief before terrain lift/projection; the adapter supplies one registered base and one facing displacement |
| Ground/cliff shade | Native SIM source already includes it; the detailed GPU material is ambient 1 / diffuse 0 |
| Camera and normalized depth | One final composed matrix; radial encoding/decoding is a GPU representation round trip, not a second globe bend |

`PresentSimGlobeProject` now supplies an owned, backend-neutral projection
value for the pending actor/effect integration. It accepts interpolated native
XY, an explicit registered support datum and already-scaled radial altitude.
It returns world/clip/screen coordinates, matching GPU depth and billboard
scale from one transform. Positions are not clamped at town edges; fixed menus
bypass world projection. Input failures leave outputs untouched.

Tests cover 18,576 walking/flight samples across six towns, four radii and three
angles, including edge-crossing and stable-flight-datum trajectories. Paired
GPU captures additionally compare projection against the actual registered
terrain corners and GPU source encoding in every captured view. This validates
the shared math contract, **not** live actors, sprite priority or effect timing.

### Still required before cutover

1. Wire native actors, effects, targeting and shadows through the shared
   placement/depth contract while preserving native priority/multipart order.
   Publish the curved displayed crater for eruption trajectories.
2. Remove prototype settings coupling: navigation relief currently also affects
   continuous active-town height, and navigation model/lighting toggles can
   affect the detailed town. Active SIM presentation needs its own existing
   feature policy, not competing navigation settings. Test independent toggles.
3. Unify native-canvas and globe ocean material without changing land geometry.
   The six-town matrix exposes the seam clearly in Marahna and Northwall.
4. Add spatial focus lighting/haze and audit all-effects depth/occlusion/cost.
5. Audit changing captured windmill phases: the detailed source currently
   embeds individual captured poses in its source key, unlike the separate
   resident-pose navigation experiment. Frozen-pose captures cannot establish
   zero uploads during live animation. Also benchmark genuine mountain-facing
   changes, which still rebuild source geometry.
6. Exercise natural travel, live building/miracles, settings extremes and a
   populated Northwall, then run uncontended local benchmarks and Deck probes.

Unrestricted 360-degree town viewing is explicitly out of scope. The selected
moderate camera and existing local bounds remain unchanged.

Final validation for this pass: all 171 desktop tests passed, the separate
full game build linked, and all six final paired Metal captures passed their
repeat/cache/cold-source/restoration/input-integrity checks. Removing the two
overwritten height queries left all 120 Fillmore/Kasandora capture files
byte-identical to the authored-coast fix before that cleanup. The original
save hash is unchanged. Evidence and compact comparison images are in
`runs/sim-globe-integration-20260913/`; raw PPMs remain in the temporary capture
directory. No release artifact or default mode was changed, and no FPS gain
is claimed from these correctness runs.

## Landscape magnitude and single-height audit

The original enhanced SIM and continuous-globe experiment use the same
`SimTownTerrain` corner data and the same `landscape_height_pct` control.
The initial paired comparisons explicitly pinned **both** sides to 100%;
they did not reproduce the user's local configured magnitude. A subsequent
read-only check found `settings.ini` at **40%**. Thus the comparison terrain
was 2.5 times that configured source elevation, despite there being no
double-applied height map. "Same calculation" must not be taken to mean
"same configured appearance" in those initial comparisons.
Marahna's maximum in that data is 6.296875 cells: 100.75 native pixels at
100%, 75.5625 at 75%, and 50.375 at 50%. Those are source-space elevations
above the town datum, not framebuffer pixels or the height of every cliff.
The baked data is unchanged. The later approved factory-default adjustment
to 40% is documented below; these initial comparisons used 100% explicitly.

The height audit confirms:

- The terrain emitter exports **unscaled** elevation. The percentage given
  to it controls its slope/contact shading, not another geometry scale.
- `RegisterTownFloor` substitutes the explicitly owned height for that
  town's sampled value. It does not add the sampled town value again.
  Registration offsets are 0 / 3 / 4 / 4 / **0** / 4 cells; Marahna has no
  extra town-datum lift. Boundary contributions are weighted, not stacked.
- The detailed town replaces active-town globe ground/cliffs and mountain
  geometry; it is not rendered on top of a second elevated town surface.
- Model bases use one registered ground height, followed by their authored
  model rise. Mountain sources export their rise **without** the original
  projected SIM ground lift; their adapter supplies that ground once.
- The radial GPU pass decodes already embedded vertices with height scale 1
  and reference height 0. Camera reference subtraction moves the common
  origin; it does not add relief to individual terrain points.

There is one real numerical difference from flat SIM: the globe normalizes
its chart into local SIM-cell units. Ground spans use `height / map.metric`,
whereas flat SIM uses `height` directly. At the paired prototype's 192-cell
chart radius, Marahna's metric is approximately 0.982935, making radial relief
spans **1.736% larger** at the same landscape setting. The center's reference
offset preserves the original center elevation. Thus the height calculation
is not literally identical, but there is no doubling: the major visual
difference is the curved placement and the newly exposed surrounding ocean.
The old SIM comparison also shows the tall cliffs.

### Reproducible height comparison

```sh
<test-build>/actraiser_present_world_nav_gpu_test \
  <ROM> <snapshot-prefix>.wram.bin <existing-output-directory> \
  --sim-pair <snapshot-prefix> --sim-height-sweep
```

This captures 100%, 75% and 50% at identical radius, pitch, camera distance,
model height/detail, light and weather time. It also exercises 0% and returns
to 100%, requiring exact retained/direct/rebuilt-source/restored images and
unchanged captured input. Both clear and cloudy variants cover overview,
north-close and south-edge views. It does not edit settings or saves.

The source regression now checks all six towns at seven settings (0..150 in
25-point steps): unchanged raw heights, one registered elevation, one scale,
and the radial encode/decode contract. Paired captures now use the game's
ordered SDL adapter: the previous legacy test binding left streaming ground
uploads pending on cold entry, producing an uninitialized first image until
a present. No warmup frame is used to bypass that ordering check in the
height sweep, and no new game-side flush, fence or readback was added.

Evidence is in `runs/sim-globe-height-20260913/`. The original SIM/globe
comparison is a frozen background-only render, not native 2D gameplay; actors,
UI, cast shadows and independent eruption effects remain omitted. The
landscape variants are visual candidates, **not** a changed factory default
or an enabled playable globe SIM mode.

### Globe-size terminology

Radius and diameter have the same multiplier, but two different reference
sizes had been called "1x". To make comparisons explicit:

| View | Chart radius (cells) | Relative to original globe | Relative to current navigation |
| --- | ---: | ---: | ---: |
| Navigation / shipped SIM connected underlay | 96 | 2x | 1x |
| Sky Palace | 144 | 3x | 1.5x |
| Current continuous SIM prototype | 192 | 4x | 2x |
| Larger continuous SIM candidate | 288 | 6x | 3x |

`--sim-radius-scale 3` on a paired capture selects the last row. It can be
combined with `--sim-height-sweep`; keep outputs in separate directories.
Only the continuous side changes: the original SIM reference retains its
shipped connected underlay. Camera distance, native-cell scale, model detail
and landscape percentage do not change with this option.

The 3x candidate reduces curvature while keeping town-scale framing. It
does not remove the derived cliff relief. Marahna's ground-unit conversion
at radius 288 is 1.007716 versus 1.017361 at radius 192: less than a 1% change
to relief spans. Reducing landscape height and enlarging the globe therefore
address different visual effects, and should be judged independently.

The height sweep passed for all six copied-save towns. The 3x candidate
passed for Marahna, Kasandora and Fillmore, and the full original/continuous
Marahna pair passed on the ordered adapter. Four focused terrain/projection
tests and both world-navigation/depth-pass GPU suites passed. The original
`backup-testing-2.srm` hash remains unchanged.
These are correctness/visual tests, not frame-rate benchmarks. From these
captures, 3x navigation size with 75% landscape is the proposed next visual
baseline at that point; it was not made a gameplay or capture default.

### Comparing with the user's configured relief

The follow-up compares 75% and 50% on the same 3x globe, and the existing
SIM at the user's configured **40%** with continuous SIM at 40% and 50%.
The lower setting is appreciably closer to the user's tamer relief: 50%
is 25% higher than the configured 40%, whereas the earlier 100% reference
was 150% higher. Model dimensions, authored mountain rise, camera and
native gameplay coordinates remain independent of the landscape setting.

Use an explicit percentage to avoid confusing fixed test presets with
local gameplay settings:

```sh
<test-build>/actraiser_present_world_nav_gpu_test \
  <ROM> <snapshot-prefix>.wram.bin <existing-output-directory> \
  --sim-pair <snapshot-prefix> --sim-radius-scale 3 --sim-landscape-height 40
```

`--sim-landscape-height` accepts 0..150 and cannot be combined with
`--sim-height-sweep`. The paired log records the actual percentage and
radius multiplier. The harness uses the shared factory-default constant
rather than implicitly loading or modifying the user's settings file.
The 50% candidate superseded the earlier 75% recommendation; the user then
selected **40%** as the factory baseline. The larger 3x navigation globe
remains a separate candidate, not a changed gameplay or capture radius.
When integrated, the prototype must respect the existing landscape control,
including explicitly saved preferences.

The 40% Marahna pair passed its three viewpoints, clear/cloudy variants,
24-frame camera/weather sequence, warm/cold mountain geometry checks,
retained/direct image checks, restoration and captured-input integrity.
The new CLI rejects percentages outside 0..150 before renderer startup.
Both the settings and original save hashes remained unchanged.

### Approved 40% default

`kSimTownTerrainLandscapeHeightDefaultPct` is now 40. New settings and
reset-to-default use that value; loading a saved value (including 100) or
an explicit environment override continues to respect it. The single
existing setting controls landscape height for SIM and globe consumers.
There is no second multiplier, per-town special case, model resize or
change to the baked contours. The paired-capture default shares the same
constant, while synthetic stress tests and explicitly pinned 100% fixtures
keep their original diagnostic magnitudes. Regression checks cover initial
defaults, missing-key settings, save/reload, environment precedence and reset.

All six targeted settings/menu/terrain/GPU tests passed. The default-height
Marahna capture also passed; all 60 images are byte-identical to the earlier
explicit `--sim-landscape-height 40` run. User settings and save hashes are
unchanged. No commit, release build or playable prototype cutover was made.

## Live integration pass — September 13

The explicit CMake experiment now renders native SIM actors and selection
graphics over the continuous detailed town at **3× navigation radius**. It
uses the captured landscape setting (factory default 40%), not a second
height multiplier. Normal builds still select the existing SIM compositor.

### Implemented

- Reuse native priority bands, reverse-OAM ties, ground-depth sorting,
  multipart anchors, structure overlays and height-pop policy. Grounded
  actors test the shared world depth; authored overhead/projectile classes
  retain their existing readability exception. Map-plane selectors curve
  with the floor; fixed menus/HUD bypass world projection.
- Share an owned, synchronous `SimSceneProjection` between ordinary/curved
  effect lighting, particles, fireball heads and path samples. Raw audited
  support units are registered once; object altitude remains independent.
  Actual crater publication and eruption acceptance are recorded below.
- Borrow the already uploaded OBJ atlas for a bounded GPU billboard stream
  (maximum 4,096 quads). Preserve native transparent ordering with depth tests
  against the world but no sprite depth writes. The later directional-rim
  material adds one startup-prepared shader; there is still no GPU readback,
  native handle, runner ABI or live gameplay-state dependency.
- Prepare actor/model shadows in a bounded native-town XY mask before the
  world's shared depth pass. Reuse the existing blur and actual facade heights;
  do not prepare a fake camera or invalidate mountain projection to make the
  mask. Its source-space footprint/softness retains the ordinary SIM scale.
  The mask is sampled only on the retained curved ground's top range, never
  cliff faces. The existing two-target shadow budget remains capped at 4M
  pixels per target. The generic surface overlay contract borrows a texture
  through Submit and keeps normal depth/ownership/atomic-rejection policy.
- Active detailed SIM relief/models/shading no longer inherit navigation's
  respective enable switches. Neighbour overview detail remains independently
  reducible. Centre framing references the native registered floor, not the
  navigation mountain estimate.
- Captured windmill movement now republishes only its animated stream,
  preserving independently stopped/constructing windmills. Static buildings
  no longer re-upload on each captured phase change. In the synthetic
  24-cathedral/two-windmill test, cold model publication is **827,360 bytes**,
  a rotor tick **28,640 bytes**, and a held frame **0 bytes**: **96.54% less
  model-upload traffic** for motion, not a measured FPS gain. Warm/cold,
  construction-phase and rewind images match exactly.

Dynamic content bypasses only retained color-image reuse; camera-independent
terrain/models remain resident. The world renderer accepts synchronous
prepare/append callbacks, not actor types or gameplay policy. Neither callback
retains a FrameSlot/view pointer. Preparation may render optional masks;
append may only collect portable depth geometry, never nest/submit passes.

### Validation and limits

- 7,344 actual-height placement cases cover six towns, 0/40/100/150% height,
  flat/curved projection, screen/world/record-local effects and flight/crater
  support policies. The earlier independent 18,576 mapping samples remain.
- Metal tests cover cold borrowed-atlas/mask uploads, transparent texels,
  world-depth occlusion, native ordering, copied inputs, same-device texture
  ownership, malformed/over-budget atomic rejection and reset recovery.
- The native ground receiver partition prepares/reuses every town at
  0/40/100/150%. A live-entry regression exposed cancellation in an
  absolute-coordinate polygon-area test; local edge vectors now keep
  collapsed cliff faces exactly zero under fused arithmetic.
- Both experimental and normal full-game builds were checked. Evidence is
  recorded in `runs/sim-globe-live-20260913/`. Replay captures use isolated
  copies of the save/settings; no personal settings or original save writes.
- Final full desktop suite: **172/172 pass**. Six widescreen town-warp replays
  reached GF2650 without fatal renderer errors, followed by a final Marahna
  rerun after calibrating native-mask blur softness. The normal build's SIM
  entry replay also passed. See the evidence directory's `verification.md`
  for exact scope, raw capture directories and the unchanged input hashes.

### Effects and boundary integration follow-up

The experimental live composition now includes the previously missing rim,
curved crater effects and active-town focus. These changes are still behind
the existing experiment, not a shipping/default cutover.

- Native and curved relief share a bounded authored crater/effect recipe.
  The continuous renderer resolves the displayed mouth through the inverse
  globe mapping and removes its registered support exactly once before
  publishing the native-coordinate trajectory anchor. Towns without a
  volcano explicitly clear it. Glow and smoke retain the original detail,
  style and eight-tick flashing policy; animation does not rebuild relief.
- Billboard rims now use a directional inward-alpha material, replacing the
  initial all-edge treatment. It shares ordinary SIM's light-direction recipe
  and warm colour. A copied 32-byte uniform holds UV offset and colour/strength;
  the MSL/SPIR-V/DXIL shader and pipeline are prepared at startup. Styles form
  bounded, order-preserving runs in the existing 4,096-quad stream, without
  extra per-vertex data or mask readback. Premultiplied composition keeps rim
  strength independent of half-alpha sprite bodies. Four-direction raster
  checks ensure the lit/opposite edges, alpha, depth and ordering are correct.
  Atlas-space sampling is not a pixel-identical screen-mask operation, so
  final visual acceptance remains important. Action-mode rims are unchanged.
- Geographic focus leaves the entire active town clear and gradually dims
  and hazes neighbouring surfaces/models. Terrain reuses existing material
  uniform space (304 bytes; 352 for batched shadows), without extra draws or
  static source uploads. Neighbour model colours use the same transfer at
  source publication. Active detailed model identity/facing remains independent
  of this focus policy. Captured feature switches still disable the effect.
- The initial four-cell water transition used whole-water cells. The revised
  continuous-only material below replaces it; the old connected-town renderer
  remains unchanged, not a recipient of this fix.

That initial water transition softened Marahna's rectangular blue-to-ocean
edge, but hard differences remained where native and overview coastlines
disagreed and the whole-cell classifier refused to paint over land. Fading
land was not an acceptable remedy.

The subsequent camera-boundary screenshots exposed a more visible water seam:
Marahna's lighter native water ends in angular/rectangular patches against the
darker overview ocean. The user noticed these as apparent holes in cloud cover.
An identical-camera, clouds-disabled capture retains those exact cut-outs,
isolating them to the water composition rather than cloud coverage. Matching
cached and rebuilt images proves consistency, not visual correctness; this
water discontinuity prompted the continuous-only revision below. Cache parity
alone is not visual acceptance.

#### Water boundary revision

- Keep the native town's ground unchanged. Outside it, mask the borrowed live
  water material by **texel identity**, not by whole-tile or RGB guesses. Mixed
  coastal cells can participate without recolouring their non-water pixels.
  A nearest all-water native source supplies the actual live palette and wave
  phase when the border cell itself contains land. This removes rectangular
  gaps caused by rejecting an entire coastal/source tile.
- Both the developed overview and any captured native neighbour must agree
  that a destination pixel is water in every animation phase. Object cells,
  polluted water, marsh, transparency, sand and shoreline foam are protected.
  A one-native-pixel guard across cell boundaries accounts for Scale2x and
  filtering. Pristine geography is never consulted. The immutable classifiers
  add 48 KiB of bounded masks; they do not track animation frames or colours.
- Compact the safe mask into non-overlapping source rectangles, splitting at
  the parent globe quad's diagonal where needed. Copy positions from the
  already-built globe grid; do not independently sample a second coastal height
  field. The initial independent sampler could bury parts of the water material
  in the globe's sloping coastal triangles. Only a 0.0005-cell material separation
  bias remains. Off-chart water follows the existing sea datum.
- The world surface owner rebuilds the material on its geometry revisions or
  changed captured ground; water owns no second geography revision scheme.
  The temporary 642-square coverage mask is freed after source publication.
  Allocate vertices for the counted rectangles, not the worst-case mask size.
  Geometry remains resident across camera/light/animation changes. There is no
  new shader, backend/runner ABI, animated texture cache or per-frame CPU bake.
- Marahna's identical-camera cloud-disabled comparison removes the large square
  cutouts and the coastal height-intersection slivers. The protected authored
  shore and its distinct shallow-water colours remain; this is a local material
  transition, not a replacement of all water artwork worldwide.
- Validation: **174/174** desktop/Metal tests pass. Independent source-space
  raster checks cover every pixel of four 640-square masks, protected neighbour
  objects/coasts, non-coplanar quad diagonals, cache identity and failed resource
  publication; the same test passes AddressSanitizer/UndefinedBehaviorSanitizer.
  Native masks are checked against the independent live town decoder across
  every tile/variant/animation phase. All six frozen-town comparisons, 96
  camera/settings cases and retained/cold comparisons pass. All **180** old
  connected-renderer reference captures remain byte-for-byte unchanged.
- The rebuilt experimental game completes a 3,000-host-frame live close-zoom
  globe-toggle replay with 2,172 valid SIM frames and zero metadata errors.
  Its final WRAM/SRAM match the prior grounded-billboard replay exactly.
  Raw capture: `runs/20260913-214017`; durable logs/images and scope are in
  `runs/sim-globe-effects-20260913/verification.md`. This is not a new benchmark.

#### Grounded billboard orientation

Grounded actors/effects now keep the globe's local east tangent as their sprite
horizontal axis and face the camera only in pitch, preserving the ground's
projected roll and yaw foreshortening. Foot anchors, multipart offsets and height
pop are unchanged. Flyers, HUD, ground decals and authored overhead/structure
overlays retain their existing policies. The directional rim converts the
screen light through the same sprite basis. No new shader or resource is needed.

The projection test covers 9,288 grounded secant comparisons and 15 analytic
centre poses in addition to its existing walking/flying cases. A 3,000-frame
live toggle replay has zero metadata errors; final WRAM/SRAM are identical to
the pre-orientation replay. This does not claim pixel-identical live clouds or
screen-space effects, whose wall-clock background changes between runs.

#### Follow-up correctness evidence

- Final desktop suite before the additional live-crater raster assertion:
  **172/172 pass**. Surface material and billboard tests cover depth,
  transparency, copied inputs, invalid/over-budget rejection, style ordering,
  warm reuse and startup/reset lifetime. Both optimized game configurations
  compile from the same source state.
- All six final 3×/40% frozen town comparisons pass their clear/cloudy views,
  camera sweep, source/cache restoration and captured-input immutability.
  Focus on/off/held/restored images match their respective cold/direct
  oracles without changing the mountain source revision. Northwall's copied
  save has only two model objects: it is not a developed-Northwall acceptance.
- The independent water classifier checks every tile in all native palette/
  development variants and all four animation phases against the town canvas,
  with both synthetic input and the locally supplied ROM.
- The shared inverse mapping now covers 45,738 checks. Each authored crater
  reconstructs to its displayed mouth within 0.002 chart cells; no stale mouth
  is published in other towns. A separate live-content raster test confirms
  visible glow/smoke, exact held images, changing animation, unchanged mountain
  source revision, and complete Low-detail omission. Frozen background pairs
  remain deliberately free of actors, shadows and independent crater effects.
- A natural 6,000-host-frame `sim-actions.rec` replay completed normally using
  isolated fixture save/settings. Its 5,163 valid SIM metadata frames have zero
  accounting errors, height-slew violations or separated-pixel mismatches.
  It covers 277 curved map-plane picker frames, 240 lightning-miracle instance
  ticks, 33 blue-dragon-lightning ticks, and 41 ground-fire generations. This
  validates exercised metadata/input paths, not every possible gameplay action
  or a pixel-identical curved-versus-flat composition.
- A natural 20,000-host-frame eruption replay completed with no fatal renderer
  errors. Its 19,172 valid SIM frames contain 1,069 airborne-fireball generations
  and 1,063 ground-fire generations. The metadata tool now explicitly recognises
  the already-authored ballistic source (world-tier identity `0x0E01`, airborne
  compositions `0xE7D0`/`0xE7A6`); its XY and altitude intentionally diverge from
  the static source anchor and fixed-height slew. The exception also requires
  matching record/effect identity. All ordinary actors, ground fire, malformed
  coordinates and unrelated effects retain strict checks. Six new tool tests
  cover those boundaries. The full trace has zero metadata errors, height-slew
  violations or separated-pixel mismatches under that corrected contract.
  This does not independently prove every ballistic sample mathematically;
  inverse-mapping/crater tests provide the separate placement oracle.
- After the directional material and metadata-tool changes, the full desktop
  suite passes **173/173** and generated billboard-rim shader blobs pass `--check`.
- Connected SIM's yaw ceiling is now shared by input, capture and rendering
  (350 mrad each side). Older wider saved baselines are resolved without writes.
  Free rotation, held dynamic orbits and release animation start from the
  visible pose after enabling the globe, preventing hidden reversal travel.
  Ordinary flat SIM retains 700 mrad; navigation retains its visit-local orbit.
  Camera tests cover both signs/modes, saved poses, setting transitions and
  immediate partial input at the limits.
- Six frozen towns each pass eight camera-bound corners and eight captured
  settings variants (96 cases total), including 0/150% landscape, Low models,
  navigation detail/lighting disabled, cloud altitude 0/256, opacity 0/100 and
  full neighbour dim/haze. Held and explicitly rebuilt images match exactly;
  captures remain immutable. This does not assert aesthetic acceptance of
  those extreme settings or fix the water discontinuity described above.
- A final 3,000-host-frame live replay enters SIM at pitch -1350 mrad, saved yaw
  700 mrad (correctly captured at 350) and distance 2. It disables the globe at
  GF1300 and enables it at GF1600 without a fatal error, collecting real final
  composite screenshots. Its 2,172 valid SIM metadata frames have zero errors.
  A final authorized Metal desktop suite passes **173/173** with no skips.
  The preceding sandbox-only run could not initialize Cocoa displays for
  shader/GPU tests; it is not the final suite result.

### Final visual acceptance and performance comparison

The user approved the six-town comparison gallery on September 13: the continuous
globe wins every comparison. The texel-masked water transition and directional
billboard presentation are accepted. Final timing uses freshly rebuilt variants,
not the earlier baseline binaries. Per the user's requested order, start the
Steam Deck batch first, then run the Mac batch concurrently on the other device.
Repeated alternating trials, isolated saves/settings and real drawable-size
checks now support the completed default cutover described below.

Previously, at the user's request, performance investigation was paused while the
remaining implementation/correctness work is completed. Keep bounded resident
geometry, immutable captures, shared passes and startup-prepared shaders; do not
introduce new timing gates for each functional change.

Earlier alternating local and Deck runs are baseline evidence only, not final
performance sign-off. They predate the final directional-rim/camera changes.
The Mac fixed-view test used 1440×896 output; the Deck's hidden Wayland windows
actually rendered at 720×448 and then 1080×672 because of window-fit clamping.
Neither Deck run establishes native 1280×800 performance. Existing CPU scopes
are wall-time measurements, not GPU timestamps. Record the real drawable size
and use the finished implementation for the final local/Deck comparison.

### Final cutover status (supersedes earlier checklists)

1. Visual acceptance is complete for the six-town gallery. Retain protected
   coastlines and effect settings; the sparse Northwall fixture is not a
   populated-town stress test.
2. Natural actions/eruption replays and low-angle/zoom/settings checks above
   cover the exercised gameplay paths, not all possible progression states.
   Populated Northwall remains a coverage limitation, not a reason to retain
   the rejected plateau renderer.
   Debug warps preserve some Aitos HUD/dialog state, so they validate town
   geometry/entry, not story progression or natural travel.
   The actions and eruption replays above cover useful subsets.
3. Default-path wiring is complete for both build systems. The plateau
   compositor and its dedicated comparison branch were deleted at the user's
   request. Historical screenshots and benchmark binaries/logs preserve the
   comparison evidence. Flat SIM remains available; no test switch can select
   the old plateau.
4. Repeated all-effects tests passed on the Deck and Mac, starting the Deck
   first and running the Mac concurrently. The Deck gained about 22% overview
   and 20% close-up host throughput at real 1280×800 output; Mac total render
   CPU was effectively unchanged, with cheaper overview composition. Detailed
   methods, caveats and cleanup verification are in the performance report.
   No release or commit is implied by this pass.
