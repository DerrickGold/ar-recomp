# Aitos waterfall extension seam — investigation handover

Status: unresolved as of 2026-08-13. The latest reproduction identifies a
host-side eligibility dropout that explains complete extension disappearance,
but a smaller seam that expands and contracts while the extension is present
still needs instrumentation.

## Executive summary

The Aitos `$04/$02-$03` Diorama waterfall has two related visual failures:

1. A horizontal seam between captured BG2 and its synthetic folded waterfall
   continuation changes size during vertical camera motion/jump response.
2. At sufficient jump height, the continuation disappears completely and the
   finite BG2 cutoff/skybox band is exposed. The current failing frame is
   `runs/20260813-130402/snapshots/snap_00_gf4688.ppm`.

The complete disappearance is now explained. It is not the SNES PPU culling a
mesh; the SNES does not know about this presentation-owned geometry. Our host
capture uses a camera-bounded BG1 splash-platform scan as both:

- the identity for local platform splash particles, and
- the scene-wide permission to publish the BG2 waterfall veil, mist, scoped
  `waterfall` layer section, and folded continuation.

At gf4688 the full decoded BG map still contains every splash structure, but
none is inside the scan window. That suppresses the waterfall decoration;
`FrameSlot_Capture` consequently publishes the ordinary room section, and
`Diorama_Composite` skips the continuation. This is a host semantic-culling
bug driven by the native camera position, not missing map data.

This finding does **not** yet explain every smaller expanding/contracting seam.
The section gate is Boolean, so it explains the terminal dropout and may cause
boundary flicker, but not necessarily a continuously changing gap while the
section remains published. Instrument section eligibility and projected join
coordinates before changing the geometry again.

## Current working state

- Repository: `/Users/derrick/Documents/Programming/ActRaiserRecomp`
- Branch: `main`
- HEAD: `f286ea8 feat: refine Aitos effects and ROM skyboxes`
- The waterfall investigation is an **uncommitted dirty tree** on top of that
  commit. Do not reset or discard it. It also contains other completed Aitos,
  action-effect, Diorama, and sim changes.
- Latest failing executable:
  `build-release/ActRaiserRecomp`, built 2026-08-13 13:03:58.
- Latest failing run: `runs/20260813-130402`, started 13:04:02, so it includes
  the latest 352-row vertical-interpolation denominator fix.

The current saved settings are relevant:

```ini
diorama_mode = On
diorama_camera_mode = Dynamic Cam
diorama_skybox = Plane + skybox
diorama_vertical_extend = 32
gpu_shaders_enabled = On
gpu_fx_rim = On
gpu_fx_dof = On
gpu_fx_edgeaa = On
gpu_fx_shadow = Off
gpu_interp_enabled = On
```

The canonical action BG policy still stores Aitos BG2 as fixed 24px top/bottom
in `src/action/action_bg_plan.c` and
`kActionBgAitosWaterfallBottomExtensionPixels = 24` in
`src/action/action_bg_plan.h`. A separate live BG-tuner experiment raised the
lower extent to the full 64px and the moving seam remained. Therefore 24px
cropping is ruled out as a sufficient root cause; the source policy has not
been permanently changed to 64.

## Reproduction and evidence

### Primary reproduction

1. Run the current release build in 16:10 square-PAR, Diorama mode.
2. Enter Aitos `$18=$04`, `$19=$02`, waterfall section.
3. Keep Dynamic Cam, scroll interpolation, DoF, and edge AA enabled to match
   `settings.ini`.
4. Move/jump vertically and watch the BG2/continuation boundary near the bottom
   of the tilted waterfall plane.
5. At sufficient height, pause and capture a snapshot. The reference is:
   `runs/20260813-130402/snapshots/snap_00_gf4688.ppm`.

The frame shows captured waterfall art ending on a hard horizontal line, with
the skybox/background colour visible beneath it. The folded continuation and
its concealing atmosphere are absent.

### Exact gf4688 state

`snap_00_gf4688.ppu.json` and the matching WRAM snapshot report:

```text
map                         $04/$02
BG1 camera                  (648, 88)
BG2 camera                  (648, 88)
BG1 decoded world           1792 x 768 px
BG map page                 $8000
captured vertical margins   top=32, bottom=32
capture height              32 + 224 + 32 = 288 rows
```

Replaying the production `ActionBgMapView` addressing and the current
`SceneBgScanBounds_InitWindow(..., margin_x=128, margin_y=64)` calculation
produces:

```text
camera-local scan: x=[512,1040), y=[16,384)
first exact splash structures in the relevant X region:
  (896,480), (832,496), (1008,512), (752,544), ...
exact structures inside current scan: 0
```

The complete map still has 21 valid two-to-eight-cell splash structures. The
nearest qualifying row starts at Y=480, 96px beyond the scan's exclusive Y=384
edge. This directly rules out the game unloading or erasing the authored BG
structures at this point.

### The host-side dropout chain

The current dependency chain is:

```text
CaptureAitosWater
  -> scan BG1 only around the current camera
  -> require at least one exact splash-platform structure
  -> append kActionEffect_AitosWaterfall and mist
  -> FrameSlot_Capture finds the waterfall decoration
  -> publish kDioramaLayerSection_AitosWaterfall
  -> Diorama_Composite admits the BG2/BG2Hi folded continuation
```

If the first scan finds no platform, `CaptureAitosWater` returns before
publishing the broad veil and mist. `FrameSlot_Capture` defaults to
`kDioramaLayerSection_Room`, and this predicate in `Diorama_Composite` fails:

```c
map_group == kActRaiserMapGroup_Aitos &&
map_number >= 2 && map_number <= 3 &&
layer_section == kDioramaLayerSection_AitosWaterfall
```

Relevant code:

- `src/action/action_effects.c`: `CaptureAitosWater`, especially the
  `margin_x=128, margin_y=64` scan and `if (!splash_count) return`.
- `src/frame_slot.c`: derives `diorama_layer_section` from the transient
  decoration list.
- `src/diorama/diorama.c`: `aitos_waterfall_extension` predicate and folded
  continuation construction.

The existing regression covers a positive local structure at camera
`(728,488)` and a negative cave camera at `(120,32)`. It does not cover a frame
that is still visually in the waterfall shaft but has temporarily moved all
platform signatures outside the small scan window. gf4688 is that missing
case.

## Current continuation architecture

The uncommitted continuation implementation does the following:

- Repeats the authentic 224-row interval, not policy-extension rows, through
  `DioramaVerticalRepeatPlan_Build`.
- Builds a subdivided continuation with `BuildFoldedOverflowMesh`.
- Begins at the authentic bottom and stays coplanar beneath the remaining host
  capture rows.
- Adds another 16px (two native 8px tile rows) of coplanar underlap before
  bending toward `kShoeboxZFront`.
- Uses the same current-frame MVP, layer U range, bounded V interpolation
  shift, shade, and source texture as the host BG2 plane.
- Appends extension primitives and host BG2 primitives into one
  `SDL_RenderGeometry` submission, extension first and host second.
- Publishes the same folded surface through `DioramaProjection` so BG2-local
  veil/mist geometry can follow it.
- Applies the host layer's DoF/edge shader to the combined geometry. The shader
  has an attached-lower-bound uniform intended to prevent the host's bottom
  feather/blur taps from sampling skybox or transparent padding.

BG2 and BG2Hi each receive continuation geometry when their respective plane
is drawable. Only BG2-low publishes the public projection used by atmosphere.
Remember that the per-plane draw loop still has another eligibility gate:
`DioramaLayerIsDrawable` must see a visible texture and current pixels before
it reaches the continuation code.

## What has been tried

| Attempt | Current result / conclusion |
|---|---|
| Add bottom foam and mist, then thicken it into multiple cloud banks | Improved concealment only when published. It cannot mask gf4688 because the same camera-local gate suppresses the waterfall, mist, section token, and continuation together. |
| Add a repeated BG2 continuation below the finite capture | Makes the waterfall reach lower in ordinary frames, but introduced/exposed the moving join. |
| Source the continuation from the immutable authentic 224 rows rather than live BG extent rows | Prevents BG Extents edits from moving or disabling the repeat source. Seam remains. |
| Raise live BG2 lower extent from fixed 24px to full 64px | Seam remains. Rules out the 24px policy cutoff alone. |
| Move the continuation closer and tuck it beneath captured BG2 | Reduced gross gaps but did not eliminate the moving seam. |
| Increase coplanar overlap to 16px / two tile rows | Seam remains. Insufficient overlap alone is not the root cause. |
| Remove/adjust feathering at the attached lower edge | Seam remains; some blends made the line more visible. |
| Clamp DoF taps to the last drawable BG2 texel and suppress host fragments beyond the handoff | Shader artifacts may be reduced, but the reported seam remains. Shader path has not yet been cleanly A/B'd off in the latest reproduction. |
| Give host and continuation the same current-frame MVP and bounded UV shift | Implemented. Seam remains. |
| Append extension and host geometry into one render submission | Implemented. Seam remains, ruling out a delay between two CPU draw calls as the complete explanation. It does not prove two independently triangulated surfaces cannot rasterize differently. |
| Normalize vertical scroll interpolation by the allocated 352-row texture rather than the authentic 224-row viewport | This is a valid unit-contract correction: 224 caused a 352/224 UV overshoot. The post-fix `runs/20260813-130402` still fails, so it was not the waterfall seam's root cause. |
| Bake the extension “into” the BG pass | Host and extension now share one ordered geometry submission. A true 2D texture bake cannot represent the portion that folds forward in Z without first changing the geometry design. |

## What is ruled out, and what is not

### Ruled out as sufficient root causes

- **The canonical 24px BG2 lower crop by itself.** Full-64 live tuning still
  showed the seam.
- **Too little overlap by itself.** A 16px coplanar underlap still showed it.
- **A separate CPU draw-call timing delay.** Host and extension are currently
  submitted together.
- **The 224-vs-352 interpolation denominator as the seam root.** It was wrong
  and is now fixed, but gf4688 is from the fixed build and still fails.
- **The full BG map or splash signatures being absent at gf4688.** They are
  present in the snapshot WRAM and decode correctly.
- **Native SNES hardware directly culling the synthetic continuation.** The
  continuation is host-only. The complete dropout is caused by host semantic
  eligibility derived from a native-camera-bounded scan.

### Not ruled out for the residual moving seam

- The `waterfall` section token switching near a scan boundary on adjacent
  frames. This must be logged over a jump, not inferred from one snapshot.
- Dynamic Cam changing pitch/distance during jump and landing. Host and
  continuation share an MVP, but their separate triangulations/fold formulas
  may respond differently at the join.
- The DoF/edge shader, especially filtering and the
  `lower_content_v_max` ownership boundary.
- Scroll interpolation itself. The latest settings have it enabled; it needs a
  controlled off/on comparison after section eligibility is forced stable.
- BG2-low/BG2-high priority-split eligibility changing independently.
- Changes to live top/bottom vertical margins or valid-span endpoints over
  adjacent frames. gf4688 itself has stable 32/32 margins, but no time-series
  trace exists yet.
- Raster coverage between non-shared host/extension vertices. One draw call is
  not the same as one topologically shared mesh edge.

## Recommended next investigation

### 1. Fix or temporarily stabilize scene eligibility first

Do not tune overlap again until the continuation is guaranteed to remain
published throughout a waterfall jump.

The durable design should separate:

- camera-local splash structures, which should continue using bounded capture
  and be emitted only when relevant, from
- waterfall-section identity, which must not disappear merely because all
  platforms moved outside a 64px vertical lookaround.

Candidate approaches, in preferred investigation order:

1. Add a dedicated, immutable `Aitos waterfall section` capture field instead
   of inferring the section from the decoration list. Establish it with an
   exact scene signature and retain it across short camera-local signature
   absences. Reset on map transition and on a positive identity for the cave.
2. Measure whether a larger **vertical-only** identity window is sufficient
   while the existing narrow X window still rejects the shared `$04/$02` cave.
   gf4688 requires reaching structure Y=480 from camera Y=88; the current scan
   ends at 384. Avoid choosing a margin from this single frame without checking
   the complete room traversal.
3. Identify a stable live BG2 tile/CHR/register signature for the waterfall
   section that remains valid when no BG1 platform is local. The decoded BG2
   map alone was previously considered ambiguous with the cave, so validate
   the complete live tuple rather than matching a visual number.

Do not replace this with a broad `$04/$02` map gate: that map also contains the
preceding cave, which is why the exact signature existed in the first place.

Add a regression using gf4688's measured state or an equivalent synthetic
fixture:

- map `$04/$02`
- world `1792x768`, page `$8000`
- camera `(648,88)`
- exact structures beginning at `(896,480)`
- waterfall section remains eligible by the chosen stable contract
- local platform spray may still be empty
- the cave negative case at `(120,32)` remains rejected

### 2. Add one state-change diagnostic across the complete chain

Suggested opt-in diagnostic: `AR_AITOS_WATERFALL_LOG=1`. Log only on a changed
tuple to keep a jump readable:

```text
gf, map
BG1/BG2 camera x/y
scan bounds and splash count
published waterfall-section token
capture top/bottom and total height
BG2 valid drawable y0/y1
BG2-low/BG2-high drawable flags
layer_v0/layer_v1/layer_v_shift
fold_t, overlap_t, handoff_t, extension_nv/ni
dynamic camera tilt/distance and interpolation alpha
```

At screen centre, also project and log:

- the host point at `fold_t`,
- the extension first row,
- the host final drawable row,
- the extension underlap/handoff row.

Report their screen-space Y/Z deltas. This distinguishes a semantic dropout,
geometric separation, and texture/filter line without relying on screenshots.

### 3. Run a small A/B matrix after eligibility is stable

| Test | What it isolates |
|---|---|
| Force the validated waterfall section on for one diagnostic run | Confirms whether all remaining motion occurs with geometry continuously present. Do not ship the broad force gate. |
| Scroll interpolation Off / On | Separates source-window motion from base geometry/capture motion. |
| Free Cam / Dynamic Cam | Separates game scroll from reactive pitch/zoom changes caused by jumping and landing. |
| DoF Off, Edge AA Off, then both On | Separates raster geometry from shader sampling/feather ownership. |
| Log/draw BG2-low only, then BG2-high only | Detects priority-split eligibility or depth disagreement. |
| Diagnostic solid colours for host, coplanar overlap, and folded portion | Shows whether the visible band is uncovered skybox, filtered host pixels, or the wrong continuation region. |

The highest-value first clean baseline is:

```text
stable/forced validated section
Free Cam
scroll interpolation Off
DoF Off
edge AA Off
```

If the seam still changes size there, investigate shared topology/projected
join coordinates. If it is stable there, re-enable one feature at a time.

### 4. If geometry remains responsible, make the join topologically shared

The current single submission still contains two independently generated
vertex grids. If the diagnostic projected coordinates diverge, build host BG2
and its attached continuation through one mesh builder that shares the exact
handoff vertex row and indices. Do not rely on numerically similar duplicate
vertices plus overlap. Keep the authentic host UV window and continuation
repeat UV window as separate row ranges within that one mesh.

## Tests and validation already green

Before the gf4688 visual rejection, the current dirty tree passed:

- release application build,
- all 45 non-GPU tests,
- GPU shader blob regression (46th release test),
- generated Metal/SPIR-V shader artifact check,
- focused ASan Diorama projection, scroll-math, and skybox-UV tests,
- `git diff --check`.

These prove the existing contracts and memory safety checks, not visual
acceptance. After changing eligibility, rerun at minimum:

```sh
cmake --build build --target ActRaiserRecomp actraiser_action_effects_test \
  actraiser_diorama_scroll_math_test actraiser_diorama_projection_test
ctest --test-dir build --output-on-failure -E '^actraiser_shader_blob$'
tools/build_shaders.py --check
ctest --test-dir build --output-on-failure -R '^actraiser_shader_blob$'
```

Then capture a vertical-motion sequence, not just one still. Acceptance should
include:

- ordinary ascent/descent,
- maximum jump height,
- landing shake,
- pause at the high point,
- cave-to-waterfall transition and backtracking,
- fixed 24px policy and live full-64 diagnostic policy.

## Key files

- `src/action/action_effects.c` — splash signature scan and waterfall/mist
  publication.
- `src/frame_slot.c` — transient decoration-to-layer-section inference.
- `src/action/action_bg_world.c` — shared, bounded full BG map addressing.
- `src/action/action_bg_plan.c` / `.h` — canonical 24px Aitos BG2 policy.
- `src/diorama/diorama.c` — host BG2 mesh, continuation mesh, combined draw,
  shader uniforms, and per-plane eligibility.
- `src/diorama/diorama_depth_shapes.c` / `.h` — authentic repeat and fold math.
- `src/diorama/diorama_projection.c` — maps BG2-local effects over the fold.
- `src/diorama/diorama_scroll_math.c` — present-time UV interpolation.
- `src/diorama/diorama_skybox_uv.c` / `.h` — drawable BG span resolution.
- `src/shaders/dof_edge.frag.glsl` — combined DoF/edge ownership handling.
- `tests/action_effects_test.c` — current positive waterfall and cave-negative
  identities; missing high-jump continuity case.
- `tests/diorama_scroll_math_test.c` — repeat/fold and 352-row interpolation
  regressions.
- `runs/20260813-130402/snapshots/snap_00_gf4688.*` — current primary evidence.
