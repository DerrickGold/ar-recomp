# Church interior prototype

This opt-in view places the camera above the altar at the Master's chair,
looking out through the cathedral entrance into the town. The deep nave has
dark, warm stone with no visible tile grid, native Palace columns beside the people,
and soft blue daylight bouncing in from the door. A feathered warm pool from
an implied high window behind the camera lights the people. Soft warm god rays
descend from that window; a weaker blue shaft enters through the door. Slowly drifting
dust catches the light, with a restrained glow around the altar and a gentle
vignette. These effects are composed before the native dialogue and HUD. The
people remain the original animated 2D sprites. The altar is a 3D reconstruction
of the native art: cream stone, four rounded corner feet, a beveled body,
a recessed front panel, a gently indented top, and rounded bolsters at either
end. It uses an ivory/ochre palette and smooth normals. Its lit surfaces are reduced
to a ten-color warm stone ramp with subtle, stable grain, giving the carved
shape restrained pixel-like variation. The altar,
columns and architecture share diffuse light from the warm high window and
cool doorway, with a low bounced fill in the shadows. The altar's actual mesh
casts soft shadows onto itself and the floor; round column proxies cast and
receive shadows while the visible columns remain native 2D art. Contact shading
anchors the feet and column bases. Column lighting varies across their curved
sides and height, instead of brightening an entire cutout. Its geometry
uses the same camera and depth buffer as the room. The 9.2-by-3.1-unit footprint
is authored in level local coordinates and only translated into place; the
sprite's isometric top silhouette is not used as a physical depth measurement.
The end caps and corner feet retain their round cross-sections as depth changes.
The room's level floor uses the same terrain-height anchor as the cathedral
model in SIM. Floor contacts are exactly at that elevation, without separate
floor/feet offsets; exterior terrain retains its natural height relative to it.
The Palace column shaft and rounded capital are composed into tall 2D columns with rounded feet; no
square pedestal geometry remains. Their original umber, ochre and cream palette
sets the interior stone tones. All cutouts are sorted back to front against
the architecture and altar depth buffer. The archway is open, with no attached
door panels. Recessed stone reveals and a five-slab threshold give the opening
physical thickness. Native dialogue, offering icons, and selection controls continue
to work.

## Try it

Build the game using the normal `play` preset. Under **Town 3D → Scene**, enable
**Church interior 3D**. It defaults to Off and requires **Simulation town 3D**.
The setting is saved normally and its label/help is translated into the four
interface languages. For an isolated developer launch, use:

```sh
AR_SIM3D_CHURCH=1 AR_SIM3D=1 ./build-release/ActRaiserRecomp /path/to/ar.sfc
```

Enter an enhanced SIM town and select **Listen** or **Take an Offering**.
Use a separate save/settings directory when testing alongside another checkout.
Turn **Church interior 3D** off to use the ordinary church. Authentic town mode
also keeps the ordinary church. The environment variable overrides the saved
choice for a developer run.

The local prototype worktree includes an ignored, ready-to-run
`build-release/church-demo/launch.command`. It uses a copied fixture save and
isolated settings, automatically enters Fillmore's Listen scene, then hands
control back to the keyboard/controller. Launching it resets only its demo save.
The same directory has `launch-aitos.command` and `launch-marahna.command`, each
with its own copied fixture save/settings and natural world-map travel replay.
Aitos demonstrates the nearby mountain face; Marahna demonstrates the ocean
below its raised temple.

For perspective review, use `build-release/church-demo/launch-reference.command`,
or add `AR_CHURCH_ALTAR_REFERENCE=1` to either the game or architecture-preview
command. This substitutes a plain 9.2-by-3.1-by-2.5-unit rectangular block and
faint world-space floor guides. Its entire bottom face lies exactly on the floor.
Only the guide-line decals have a tiny depth bias to prevent flicker; the floor
and block do not. The regular launcher retains the carved altar and dark floor.
Both modes use the same lower camera, world coordinates, lighting and depth pass.
Changing the reference flag also rebuilds the cached shadow mesh.

For architecture review without a ROM or character assets:

```sh
cmake --preset tests-release
cmake --build --preset tests-release --target actraiser_church_preview
./build-tests-release/actraiser_church_preview
# Optional single-frame BMP capture:
./build-tests-release/actraiser_church_preview /tmp/church.bmp
# GPU cache, particle motion, fade and resize regression checks:
ctest --test-dir build-tests-release --output-on-failure -R actraiser_church_scene_gpu
```

## Prototype limits

- The camera is fixed at the chair, 14 units above the floor with a
  0.40-radian (about 23-degree) downward pitch. Lowering the eye as well as
  its aim reduces the visible top of the altar while retaining the town and
  daytime sky through the entrance. The 3D altar adapts the native artwork's
  shape to that perspective. The original room fade remains in use.
  An animated flight from the town camera into the building is not implemented.
- The doorway shows the last complete town geometry and terrain captured before
  entry. Town simulation stays frozen during the conversation, while a few nearby
  trees and native water highlights have presentation-only animation. The doorway is set deeper into
  the nave to keep the view small. Exterior models use Ultra detail
  geometry. Styling, lighting direction and shading mode are retained from the
  actual SIM frame, using the same biome material ramps and model proportions.
  Ground and roads use the complete native SIM atlas with perspective-correct
  sampling. A private copy receives soft cast shadows from the Ultra model
  silhouettes, projected along the captured SIM sun direction onto the same
  terrain. Overlapping casters share one mask, so their darkness does not stack.
  The source atlas is never changed. Mountains now use the same native Ultra
  source geometry, atlas, tile remapping and camera-facing relief recipe as SIM.
  The source emitter accepts owned inputs so the subsequent church transition
  cannot replace mountain art with church tiles. The developed world-map atlas
  extends the land and coast beyond the town boundary; the ocean continues at
  SIM's registered sea datum below the cathedral floor, using its blue palette.
  The town border and outside mesh share registered floor heights. These distant
  surroundings use a local planar projection of SIM geography; they do not
  reproduce the globe's curvature. Mountain eruptions stay frozen during dialogue.
  Ocean highlights reuse the four native water frames at their original cadence;
  an owned copy prevents the church from changing the live world map.
  The bake is cached by snapshot, landscape,
  sun and model style. The underlying sky keeps its daytime blue gradient.
  A doorway-only exposure lift, distant defocus and restrained bloom simulate
  looking into bright daylight from the dark nave. Focus stays on the altar
  and native people while the town retains soft colors and silhouettes.
  The arched aperture is copied into quarter-resolution targets. A Gaussian
  blur supplies most of the exterior image. A cached 32-by-48 depth proxy weights
  it more strongly at greater distances, retaining a larger sharp component for
  nearby silhouettes. This adds no blur passes or GPU readbacks. Two weaker glow
  radii spill light around the opening. Only the exterior
  feeds this glow, and native UI is composited afterward. This is an artistic
  exposure/glare approximation, not a full HDR or adaptive-eye simulation.
  Bloom follows the captured room fade and recreates its targets on resize.
- Palace column artwork is decoded once from the user's ROM at session startup.
  The bounded native scene asset declaration supplies the tile and palette
  addresses, including regional palette relocation. No Palace visit is required
  and no extracted ROM pixels are distributed with the code. The decoded column
  pixels match byte-for-byte across the local USA, Japan, Europe, German and
  French ROMs. This verifies the artwork, not every regional gameplay flow.
- The high window is centered above the camera. Visible rays, drifting dust
  and the diffuse spotlight share its origin and spread; the widened pool
  reaches the column bases while leaving the upper shafts in darkness.
- Lighting uses two cached 1024-square shadow maps built from the fixed altar
  mesh and round column profiles. Filtered visibility softens cast shadows,
  with tighter edges close to their receivers. These are static interior
  shadows; animated people receive this environment shading and have soft
  contact shadows, but do not cast animated silhouette shadows across other
  objects. Bounced fill and contact occlusion are approximations, not global
  illumination. The ray volumes and
  glow remain projected layers, with light visibility attenuating the rays.
  The old floor-glow overlay was removed so it cannot paint over objects or
  fill in their shadows. Dust uses captured presentation time and never
  changes gameplay randomness.
- Native people use the original two actor cells as pixel-art billboards; no
  new character models or art are introduced. A cached 4-by-8 lighting grid
  per actor samples the same warm window, cool doorway fill and environment
  shadows as the room. A gentle wrapped response preserves the sprites' painted
  highlights and palette relationships. The original source pixels and native
  UI are untouched. Captured OBJ pixels already include the native fade, so the
  lighting tint does not apply that fade a second time.
- Soft contact shadows sit on the projected floor beneath the visible native
  silhouettes. Their position accounts for transparent padding around the feet.
  Bounds update with animation; absent people have no shadow. The foreground
  room layer occludes contacts behind the altar. These shadows are inexpensive
  feathered ellipses, not new character models or per-frame shadow maps.
- If no matching town snapshot or native foreground capture is available,
  presentation falls back to the original church.

The implementation only changes presentation. The original game still owns
all dialogue, input and item transfers. BG2 and the complete OBJ plane are
captured observationally. The shared HUD owns the complete status band;
the selected icon is handed off during scanout so it appears exactly once in
the enhanced view, including native-room fallback. The independent authentic
comparison scanout is untouched.

Church panel frames and offering controls now use the shared HUD layout and
localized-text compositor. The status bar anchors to the sides and body UI stays
centered at its intended scale instead of stretching with a wide viewport.
Picture-in-picture uses the native 4:3 scanout and stays in the upper-left
space below the status bar during church scenes, clear of the dialogue and
offering controls. Other scenes retain their usual lower-right comparison.

GPU resource resets retain the owned town/landscape data so an open church can
rebuild. Session changes, reset/load generations, disabled settings and travel
out of town retire that snapshot. A state loaded directly inside church (or
turning the option on there without a prior town capture) uses the original
room until the next visit from town. Recoverable allocation/draw failures also
use the native room for that visit, retrying after the next complete town frame.
Failure to restore the renderer's target remains a core renderer error.

## Room caching and performance

The fixed interior is pre-rendered into two transparent image layers, split at
native actor depth. Far architecture and columns sit behind the people; the
near columns and altar sit in front. This preserves their occlusion while the
people and native dialogue keep updating normally. The people draw with cached
light tints; their contact shadows are a small live overlay before the people.
**Dust motes remain live:**
all 96 particles drift every frame, after the cached room is composited. Rays
and glow are also separate presentation layers.

Room images rebuild on scene/light/style changes or viewport resize; fades tint
the existing images. Geometry and lighting survive a resize. The two RGBA images
cost about 7.8 MiB at 1280 by 800. Source geometry outside the doorway is culled
before upload. Up to eight visible trees use three retained crown poses; water
highlights are bounded to 192 quads. Both use deterministic presentation time,
without gameplay randomness or per-frame geometry/texture rebuilds. The water
effect submits a small dynamic vertex batch. Scene reset releases the retained
meshes and textures.

The Steam Deck target is 90 fps, or 11.11 ms per frame. Local measurements on
Apple M2 / Metal, at 1194 by 896, used the same isolated Fillmore Listen replay:

| Measurement | Before this pass | With details and room caching |
| --- | --- | --- |
| Steady presentation CPU time | 12.8–13.6 ms | 0.52–0.59 ms |
| Steady frame interval | about 14.2–15.0 ms | about 8.33 ms (120 fps) |
| Retained/dynamic depth vertices reported per present | about 229,000 | about 4,520 |
| Depth geometry upload per present | 5.98 MiB | 0.026 MiB |
| First church frame, including cache build | about 77 ms | about 25 ms |

The steady replay recorded no renderer fallbacks or failed draws. These are
host frame/CPU measurements, not GPU timings; this backend reports GPU time as
unavailable. Actual Steam Deck validation remains necessary, including cold
entry and resize costs. The first-frame bake is not inside the steady 11.11 ms
budget on this host. Local logs are saved under the ignored demo directory as
`performance-before.log` and `performance-after.log`.

The ROM-free GPU check compares cached/cold/reset images, verifies moving dust
without a room rebake, checks fade/time reuse and resize invalidation, and
covers an empty town. A synthetic sprite case also checks lit colors, visible
contact shadows, an absent visitor, a single native fade, and actor changes
without room/geometry rebuilds. Lighting tests verify that window occlusion
reduces the actor tint and that doorway fill stays cool. Live native replays
additionally verify actor/column occlusion in Fillmore, Aitos and Marahna.

After adding sprite lighting and contacts, the Fillmore replay still measured
about 120 fps with roughly 0.57 ms presentation CPU time on the same host.
Lighting samples are cached at scene build, with no per-frame shadow-map queries
or new render targets for the people. Fresh examples are saved as
`church-people-lighting.png` and `church-aitos-people-lighting.png` in the demo
folder; `performance-people-lighting.log` records the local replay.

## Landscape validation

- Native game replays visually checked Fillmore, Aitos and Marahna, including
  Aitos's close mountain face and Marahna's ocean through the open arch.
- Church capture, landscape ownership/registration, exterior shadows, interior
  lighting, detail/focus helpers, cached-room GPU rendering, render device/state,
  depth pass, source manifests and renderer boundary checks pass (17 checks).
- The separate private-header gate reports existing SDL private-header includes
  in `tests/diorama_bg_gpu_test.c` and
  `tools/action_editor/check_native_projection.c`. Both were reproduced using
  the unmodified HEAD files and gate rules; the new church preview is allowed
  alongside the existing standalone renderer tools.
- The captured Aitos GPU integration run passes, including byte-for-byte native
  mountain source parity after resetting the live SIM buffers, model/mountain
  rendering, crater effects, and camera/cache checks.
- The broader ROM-free world-navigation weather suite fails its existing cloud
  cap image-difference assertion at `present_world_nav_gpu_test.c:888` on this
  Metal host. The same failure was reproduced with the unmodified HEAD test and
  mountain renderer compiled separately; it is unrelated to this prototype.


## Integration release gates

Implemented in the isolated church worktree:

- Persistent, opt-in Town 3D setting; shared HUD and enhanced-text placement.
- ROM-backed column art, owned town snapshots, reset/load invalidation and
  recoverable rendering fallback.
- Settings/controller navigation, HUD backdrop placement, native sprite
  ownership, ROM decoding, snapshot lifetime and GPU-failure regression tests.

Still required before merging for release:

- Actual Steam Deck measurements at 1280×800, including cold entry, resize,
  repeated visits and worst-case towns. The prior local 25 ms cold bake still
  needs optimization; steady M2 results do not establish the 90 fps Deck target.
- Windows/Linux backend and packaging checks, plus device-loss/fullscreen QA.
- Complete all six towns, regional audience/offering variants, inventory-full
  paths, extended text/localization and input/accessibility coverage.
- Resolve or explicitly account for existing repository quality failures before
  the release gate is green. The native church's enhanced-text body can be
  missing with the option Off; an isolated replay reproduces this in the control
  binary too and must be addressed before final release signoff.

No merge to main has been performed.


### Integration validation (2026-10-03)

- Game `play` build succeeds. Optimized C/Python suite: **355/358 pass**.
  Remaining failures are the previously reproduced private-header and
  world-navigation GPU baseline issues, plus existing style debt. Comparing
  changed C/H files against HEAD reports no added style violations.
- Settings catalog tests and focused HUD/comparison/frame-order/church checks
  pass. The localization scheduling test now cleans up its own checkpoint
  companion between synthetic saves, avoiding stale fixture failures.
- Church Off produces byte-identical captured pixels and final WRAM/SRAM to
  the control binary in the isolated Listen replay. The complete offering
  replay produces identical final WRAM/SRAM with the option On and Off.
- Fresh 16:10 Listen (enhanced text) and 16:9 Offerings (native text) captures
  show the independent native inset, a single icon per view, and unobstructed
  main dialogue/controls. Leaving church restores the normal inset placement;
  the complete PiP offering replay also matches control WRAM/SRAM.
  Enabling the option mid-conversation also verifies
  the native fallback retains one anchored hourglass.
- Shader consistency check explicitly skipped because DXC is unavailable.
  No Steam Deck performance claim or platform packaging signoff is implied.

Ignored local evidence in `build-release/church-demo/` includes
`church-pip-listen.png`, `church-pip-offering.png`,
`church-native-fallback.png`, and the integration test logs.


## Selected-magic icon ownership audit (2026-10-03)

Scope: selected magic/hourglass handling in flat action/SIM, enhanced SIM,
Sky Palace, diorama, church/fallback, and authentic comparison. The findings below describe the pre-consolidation code. The shared
handoff described at the end of this document now replaces those paths.

### Findings

The game already has one semantic identity, `kActRaiserSprite_HudIcon`, in
`actraiser_sprite_ownership.c`. SIM and Palace identify the selected-magic
producer at record `$083E`; action records the extent emitted by its native
HUD producer. Other menu records can emit the same spell art and intentionally
remain separate. Consolidation must retain producer ownership and variable
OAM counts, rather than infer identity from tile IDs, colors, or a fixed four
sprites. `ActRaiserHud` is a separate BG3 text-owner tracker, not a competing
implementation of this OBJ identity.

The duplication is in moving that owned sprite into presentation:

| Path | Previous capture/removal mechanism |
| --- | --- |
| Flat action/SIM and Palace | `ActRaiser_WidescreenHudObjPromote` claims the OBJ overlay with RemoveFromGame and publishes first/count/rows. |
| Enhanced SIM | `PrepareHudHandoff` independently rasterizes the range before scanout; `RestoreTownHudPolicy` reconstructs the HUD and erases its footprint from the scene planes and base framebuffer. |
| Diorama | `ActRaiser_DioramaHudObjPrepare/Finish` uses live range capture, then removes/restores the footprint in captured OBJ planes, or uses the special full-add relocation path. |
| Church | Reclaims full observational OBJ capture after promotion; skips top-band sprites in its panel atlas and crops the native fallback's status band. |
| Presentation | `PresentHud_Upload` selects among SIM HUD, promoted HUD, and generic OBJ surfaces; `BuildProjectionInputs` separately derives icon coordinates from OAM. |

1. **Capture ownership can be overwritten after promotion.** In church the
   ordering is explicitly full capture → HUD-only RemoveFromGame capture →
   full observational capture again. The last step keeps the icon in native
   scene pixels, while the latched promotion metadata still authorizes the
   anchored HUD copy. The current band crop prevents that duplicate, but it
   leaves church responsible for knowing the same ownership rule.
2. **One conceptual result is published as several independent pieces.**
   `hud_icon_first/count/rows`, `hud_obj_surface`, SIM output surfaces and the
   current generic overlay descriptor can describe different captures. Existing
   comments document prior stride, row-origin, count and height mismatches.
   The presenter still contains a source-selection cascade, and promotion
   metadata does not itself certify that later scene extraction completed.
3. **SIM and diorama capture the same class at different times.** SIM uses a
   pre-scanout raster; diorama captures visible icon pixels during scanout and
   performs a separate best-effort underlay repair. Their handling of palette
   changes, brightness and sprites behind the icon is therefore different.
   This is a consolidation risk, not a newly reproduced visual regression.
4. **The existing relocation API is deliberately narrow.**
   `runner_ppu.inc` rejects `SR_PPU_OBJ_CAPTURE_RELOCATED` unless the active
   capture has `MarkFullAddSubscreen`. The public API describes removing a
   subset from that full-add plane; it is not general native-frame removal.
   Reusing it unchanged for ordinary church/SIM extraction is invalid.
5. **Tests cover components, but not the complete ownership invariant.**
   Sprite ownership, HUD source/extent selection, layout, SIM handoff, church
   fallback and comparison transitions have tests. A successful upload or a
   correct anchored draw alone does not prove the old scene copy is absent.

### Recommended consolidation

Keep the existing semantic classifier and shared `PresentHud` layout. Add one
capture-to-presentation owner that publishes an immutable per-frame icon result:
its generation, OAM range, pixel surface, source rectangle, and completed scene
removal state. The selected scene adapter must finish extraction before this
result becomes available to the HUD. Failure should choose a coherent native
frame or another explicitly complete presentation, never combine a removed
scene with a missing icon, or an unremoved scene with an anchored copy.

Migrate in stages:

1. Bundle icon metadata and surface selection into that single result, with
   behavioral parity tests. Scene adapters initially preserve their existing
   color-math and occlusion mechanisms.
2. Consolidate capture and exclusion during native scanout. Extend the runner
   with an explicit generic exclusion contract if required; preserve the
   independent authentic output and the original sprite evaluation/underlay.
   Do not extend the meaning of the existing full-add flag implicitly.
3. Migrate flat, SIM, diorama and church adapters, then delete the redundant
   post-capture erase/rebuild code and church status-band workaround when the
   shared exclusion path has demonstrated parity.

Acceptance should count one selected icon in each enhanced view and one in
its independent native comparison. Cover every selected spell/hourglass,
1-slot and 4-slot art, legitimate menu copies, 4:3/16:10/16:9, SIM↔Palace↔church
and action/death transitions, fades, resize/load, renderer fallback, and
scene sprites behind the icon. Check native pixels and WRAM/SRAM parity.
Keep the work bounded to the icon's footprint and avoid additional full-frame
readbacks or texture allocations; measure it against the Deck frame budget.

Audit validation: all eight focused ownership, HUD, layout, PPU-pipeline,
church, frame-order and comparison checks pass. Log:
`build-release/church-demo/church-hud-audit-tests.log`. The consolidation adds shared-renderer exactly-once coverage described below.


## Shared HUD icon handoff (2026-10-03)

Selected magic/hourglass ownership now has one completed `HudIconFrame`:
runner lifetime, frame serial, semantic OAM range, native source rectangle,
pixel surface, and completed scene-removal state. It is prepared after scene
capture policies and published only after a successful scanout of that lifetime.
The frame queue copies its surface together with the record; retained frames
keep the projection metadata. Presentation and the inspector no longer derive
icon position from live OAM or choose among competing OBJ surfaces.

The runner's explicit `SR_PPU_OBJ_CAPTURE_HANDOFF` facet combines a live RANGE
capture with exclusion of those slots inside the same rectangle from enhanced
main/sub composition and scene OBJ exports. It preserves original sprite
eligibility and resolves the real underlay during scanout. Native comparison
and independent WINNERS capture remain untouched. Full-add RELOCATED retains
its existing API meaning; HUD transfer no longer uses it.

Removed the SIM preraster/mask rebuild, diorama erase/underlay repair, church
top-band OBJ filter and native fallback crop, and HUD source-selection cascade.
Church still requests its full actor/menu scene range; that setup is unrelated
to which selected icon moves into the HUD. Genuine menu copies remain in their
own range and can use identical artwork. Unknown/invalid ownership or a rejected
capture produces no detached icon; failed scanout publishes no transfer.

The host keeps a fixed buffer limited to the maximum HUD height. Each frame's
GPU upload is at most the icon's 16x16 footprint (1 KiB), and unchanged pixels
skip upload. The renderer adds bounded scanline scratch, with no full-frame
readback or per-frame texture creation. Steam Deck hardware performance remains
a separate qualification gate.

Validation includes one- and four-slot icons, identical menu artwork, underlying
sprites, plain and extracted/full-add/winner-mask scene policies, live palette
changes, clear/reset, invalid/stale ABI requests, fast/reference render parity,
frame-queue ownership and sole-source presentation. A church listening replay
with enhanced text and native PiP matches the prior working composite exactly,
including both people. The initial integration replay exposed a missing actor
range setup, which was restored before this patch was applied to the worktree.

Before merging main, all nine standard GPU replay scenarios passed with 99
byte-identical captures and matching final WRAM. The full Debug and optimized
game suites each passed 355 of 358 tests; the three pre-existing failures were
private-header boundaries, the style baseline, and the world-navigation Metal
GPU test. Go tests and vet passed. The tooling gate lacked Ruff and shader
regeneration explicitly skipped without DXC.

### Integration with current main

The streamed town queue owns the same icon record and pixels as action frames.
The SIM atlas renderer also excludes that completed semantic range: atlas
billboards bypass the PPU scene planes, so leaving its old generic-capture check
in place produced a second hourglass after returning from church. Its regression
coverage includes broad and empty scene captures, one- and four-slot icons,
retained metadata without borrowed pixels, and independent menu/world objects.

Final validation on the integrated build:

- All 12 focused optimized checks and all 11 corresponding Debug checks pass,
  including the real PPU, queue, HUD, church GPU/cache and atlas ownership paths.
- The two standard town replay scenarios retain 26 byte-identical captures and
  identical final WRAM against the pre-consolidation game.
- Listen retains both native people and matches the prior working composite
  exactly. Offerings, native fallback, and return-to-town PiP captures show one
  selected icon per view. The full offering round trip matches control WRAM/SRAM.
- A normal windowed replay exercises the live frame pipeline before entering
  church and after returning to town (520 streamed presents), with identical
  final WRAM/SRAM to the synchronous replay. This is functional acceptance,
  not a Steam Deck performance measurement.
- Main integration's full Debug suite passed 350/359 and the optimized suite
  passed 347/359. Six Debug and eight optimized action-recipe hash failures
  reproduce in a clean archive of main at `546ba473`. The three earlier
  private-header/style/world-navigation failures remain. An additional optimized
  native-worker GPU failure passed when rerun separately. No baselines were
  changed; modified authored files introduce no style-count increases.

Local integration evidence is retained under `build-release/church-demo/`,
including `church-final-listen.png`, `church-final-town.png`,
`church-final-stream-town.png`, and the merged/focused/baseline test logs.
