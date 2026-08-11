# Sim-3D water surface: shader feasibility investigation

Status: **preliminary — evidence gathered and a look chosen from a live
preview; nothing implemented in the engine.** Companion to
[rendering-engine.md](rendering-engine.md) §7 (tile animation),
[effects-hook-investigation.md](effects-hook-investigation.md) (the same
"identify the authentic state first, then choose a style" method), and
[../specs/ar-recomp-sim-rendering-plan.md](../specs/ar-recomp-sim-rendering-plan.md).

The question: can the sim-3D town view give its water depth and transparency,
and what does that cost? The answer is yes, and the part that looked hardest —
knowing which pixels are water — turns out to be exactly derivable from state
the game already publishes.

The style question was settled separately, by building each candidate and
looking at it: see §4. The short version is a visible ocean bed at a fixed
depth under a mostly-transparent glassy surface — deliberately stylised rather
than simulated, with no motion of its own.

## 1. The blocker that isn't: identifying water

A shader over the ground plane needs a mask, because the ground is one texture
containing grass, forest, rock and water alike. Three candidate sources were
considered; the third is exact and needs no hardcoded tile tables.

### 1a. The town's animated CHR page is the water art

`docs/rendering-engine.md` §7 records that in a sim town (`$18=0`, `$19` in
1..6) `$02:BAF5` captures four VRAM pages into `$7F:B800/$B900/$BA00/$BB00`
and later ticks re-upload one captured page to VRAM `$0000` — the town's BG1
character base. A page is 256 bytes; at 4bpp that is **exactly eight tiles,
`$000`-`$007`**.

Measured, not assumed. Headless capture of the D0-fillmore-actions replay,
snapshotting VRAM every two game frames across the animation cadence:

```
$18=00 $19=01   $D7=B900 $DC=00 $E0=22 $DE=07 $DF=03
1000->1002 changed tiles: $000 $001 $002 $003 $004 $005 $006 $007  (8)
1002->1004 changed tiles: (0)
1004->1006 changed tiles: (0)
1006->1008 changed tiles: (0)
1008->1010 changed tiles: $000 $001 $002 $003 $004 $005 $006 $007  (8)
```

Nothing else in the 1024-tile character block ever changes. Rendering those
eight slots across the four phases shows what they are: one deep-water tile
with drifting wave dashes, and seven river/waterfall/stream variants. The
animated page **is** the water art, and it is the only animated art in a town.

Reproduce:

```bash
AR_HEADLESS=1 AR_INPUT_REPLAY=saves/sim-actions.rec AR_QUIT_FRAMES=1100 AR_SETTINGS_PATH=tests/fixtures/sim3d/sim-actions-settings.ini AR_VRAMDUMP_GF=1000,1002,1004,1006,1008,1010 ./build-release/ActRaiserRecomp ar.sfc --config config.ini
```

(seed the run's SRAM from `tests/fixtures/sim3d/sim-actions-seed.srm.b64` via
`AR_SAVE_NATIVE_PATH`; Fillmore is entered around host frame 819.)

### 1b. Per-tile masking is not enough — the rivers are static tiles

Fillmore's tilemap places only `$000` and `$001` from the animated page, 288
cells each. Marking just those cells catches the sea and the lake and **misses
the river**, which is drawn from ordinary static tiles in the `$052`-`$077`
range. A tile-granular mask is therefore the wrong granularity.

### 1c. Per-pixel colour classification is exact

The animated tiles use exactly three CGRAM colours in palette bank 1
(`$6529`, `$6DA5`, `$7248`). Marking every canvas pixel whose resolved colour
is one of those three produces a mask covering the sea, the lake, the winding
river, the shoreline fringe and the four small ponds — 44,329 pixels, 16.9% of
the 512x512 canvas — with no visible false positives anywhere on the map.

So the rule, with nothing hardcoded:

1. find the animated character slots (the tiles whose CHR changes on the
   animation cadence — `sim_town_canvas.c` already `memcmp`s the whole
   character block every frame, so this is a per-tile refinement of a
   comparison it is doing anyway);
2. collect the CGRAM colours those slots resolve to, in the palette banks the
   tilemap actually draws them with;
3. mark every canvas pixel whose colour is in that set.

Step 3 runs inside the existing per-pixel loop in `SimTownCanvas_Render`
(`src/sim/sim_town_canvas.c:96-118`), which only re-runs when the tilemap,
character data, palette or brightness actually changed. A still town costs
nothing.

**Verification still owed:** the colour set was derived in Fillmore only. The
other five towns need the same check — in particular that no non-water art in
Bloodpool/Kasandora/Aitos/Marahna/Northwall shares a water colour. This is a
cheap headless sweep once a replay reaches each town.

### 1d. The world-map underlay is already solved

The out-of-bounds ground extension is the Mode-7 world map, and its water is
already identified by name: tiles `$00` and `$AA`, replaced by the four ROM
frames at `$0A:B000` (`src/sim/sim_world_map.c:24-25`,
`SimWorldMap_SetWaterAnimationSource`). That underlay is mostly ocean and is a
large fraction of what the camera sees at the town's edges, so it wants the
same effect — and can produce its mask for free during `SimWorldMap_Bake`.

## 2. Where water reaches the screen

Sim-3D draws ground from three sources, and all three carry water. Treating
only one of them looks worse than treating none, because the seams between
them are visible.

| Source | Drawn by | Space | Water identified how |
|---|---|---|---|
| Live BG1 capture (`Bg1Low`/`Bg1High`) | `DrawSimGroundPlane`, `src/present_sim3d.c:138` | captured screen | town-space mask, offset by `camera_x/y` |
| Town canvas, 512² | `DrawSimTownCanvas` → `DrawSimGroundExtension`, `src/present_sim3d.c:2444` | town | §1c, built in `SimTownCanvas_Render` |
| World-map underlay, 1024² + blurred mip | `DrawSimWorldUnderlay`, `src/present_sim3d.c:2376` | world map | §1d, built in `SimWorldMap_Bake` |

All three are `SDL_RenderGeometry` calls over a projected mesh, so one
fragment shader covers all three. Everything spatial — the bed, the sheen
patches, the grain — must be computed in **town / world space**, not UV space,
or it will visibly shear across the canvas-to-underlay seam and swim when the
camera scrolls. That means each draw supplies a UV→world affine transform as a
uniform; the values already exist (`slot->sim.camera_x/y`,
`underlay_screen_x0`, and the `texture_x_at_zero` / `span` arguments
`DrawSimGroundExtension` already takes).

## 3. Shader hook mechanics

The project already has the whole pipeline; this adds one shader to it, not a
new subsystem.

- Author `src/shaders/water.frag.glsl`, regenerate with
  `tools/build_shaders.py water`, commit the generated `water_frag.h`. Needs
  `glslc` + `spirv-cross` on the dev machine only — see
  [BUILD_TOOLING.md](BUILD_TOOLING.md) "GPU shaders".
- Binding convention is fixed by SDL: fragment `set 2` = sampled textures,
  `set 3` = uniform buffers; vertex `location 0` = COLOR0, `location 1` =
  TEXCOORD0. `src/shaders/rim.frag.glsl` is the working reference.
- Create with `GpuShaderBlob_CreateFragment(device, &kWaterBlobs, "water", 2, 1)`
  — **two** samplers: binding 0 is the draw's own ground texture (SDL binds it),
  binding 1 is the water field, supplied through
  `SDL_GPURenderStateCreateInfo::sampler_bindings`.
- Binding 1 needs an `SDL_GPUTexture`, not an `SDL_Texture`. Fetch it with
  `SDL_GetPointerProperty(SDL_GetTextureProperties(tex), SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, NULL)`
  and pair it with an `SDL_GPUSampler` created on the device. This is the one
  piece of the pipeline the codebase has not used before — everything else is
  copied from `diorama.c`'s blur/rim/DOF states.
- Per draw: `SDL_SetGPURenderStateFragmentUniforms(state, 0, &u, sizeof u)`,
  `SDL_SetGPURenderState(renderer, state)`, the existing `SDL_RenderGeometry`,
  then `SDL_SetGPURenderState(renderer, NULL)`.

### The water field texture

Rather than a bare mask, make it one ARGB8888 texture per ground source and
use all four channels — same lifetime and upload path as the colour image it
accompanies, so it costs one extra streaming texture and no extra CPU passes:

| Channel | Content | Feeds |
|---|---|---|
| R | water coverage 0/255 | everything |
| G | distance to shore, clamped to 32px (8px suffices — see §4) | the pane edge |
| B | reserved (flow direction / river-vs-sea class) | directional ripple |
| A | 255 | — |

Sampled with `SDL_SCALEMODE_LINEAR`, R comes back as soft 0..1 coverage at
shorelines, which is what the foam and edge terms want. Do not hard-threshold
it or the edges alias back to 8-pixel stairsteps.

## 4. What the effect should actually be

The house style here is a lit diorama of 16-bit art, not a photoreal ocean.

**The direction, arrived at over three rounds of live preview: a visible ocean
bed at one fixed depth, a thin blue body over it, and the ROM's water tile
contributing nothing but its bright pixels as glints on the surface.**
Deliberately *un*realistic — no wave simulation, no flow, no moving highlight.
The only thing that moves anywhere in the effect is the authentic 7.5 fps CHR
cycle.

Three earlier passes are worth recording because each was rejected for a
concrete reason:

- **Animated specular shimmer** fought the ROM's 4-frame cycle rather than
  adding to it — two unrelated motions on one surface.
- **Depth-graded water** (absorption ramping with shore distance) still read
  as a simulated body of water. Flattening it to one depth is what makes it
  read as a built diorama instead.
- **The tile as an opaque pane** (`mix(art, bed, sheerness)`) was wrong in
  kind: it made the ROM art *replace* the water colour rather than sit over a
  floor, so raising sheerness dissolved the water instead of revealing a bed.

The model:

1. **A bed you are meant to see.** Mixed from bank 1's own sand, rock and weed
   entries and — the part that makes it work — **quantised to whole town
   pixels** in a few flat steps. Smooth procedural noise sitting under 8×8
   pixel art reads as a photograph slid in behind the sprites; snapping it to
   the same grid makes it belong to the map. **This is invented content**: the
   ROM's water tiles are opaque and there is no floor art anywhere in the game.
   It is the largest single thing the effect adds, and worth taking
   deliberately rather than by drift.
2. **One fixed depth.** Uniform darkening, no shore ramp.
3. **The body as absorption, not a lerp.** Multiply the bed by a per-channel
   absorption with red falling off first, *then* mix toward a scatter colour.
   Lerping the bed toward a mid blue instead desaturates everything and the
   pool comes out a pale grey haze — this was measured, not assumed. Note the
   ground plane itself must stay **opaque**: it is the backing that stops the
   world-map underlay showing through (see the comment at
   `src/present_sim3d.c:2444`). All of the transparency is internal to the
   shader.
4. **Only the tile's bright pixels return, as glints.** Threshold the ROM art
   on luminance and let the light dashes through as reflections on the glass.
   That is the entire surface treatment, and it is what keeps the authentic
   cadence as the thing that twinkles without simulating anything.
5. **Flat sky in the glass.** The ground quad's normal is constant (+Z), so a
   true fresnel term is constant too. Flat is the correct answer here, not a
   shortcut.
6. **One broad sheen band raking across the sheet**, running along the light
   azimuth from `SimShadowLight()` (`src/present_sim3d.c:766`) so it agrees
   with the shadow direction instead of implying a second sun; elevation sets
   brightness only, never position. Repeating rather than a single lobe, so
   the open ocean beyond the town catches it too. No time term.
7. **The pane's edge.** A clean static lip ~3 px from the waterline. Not foam
   — it is the edge of the sheet, and it does not move.

Dropped along with the realism: shoreline foam as an animated band, the live
ripple, and refractive wobble. Still deferred: **billboard reflections** (a
second mirrored, mask-clipped object pass — real cost and real risk to the
sort order).

### 4a. Why it still looked wrong, and what fixed it

With all of the above in place the result was *correct* and still did not look
good. The reason was one root cause with three faces: **a shader emits
thousands of continuous colour values into a scene assembled from a handful of
palette entries on an 8-pixel grid.** Everything below is about putting the
output back on the source's own terms, and each is cheap.

1. **Posterise with an ordered dither.** Snap the result to a coarse ramp
   (~5 levels per channel at full strength, against BGR555's real 32) using a
   4×4 Bayer matrix **indexed in town pixels, not screen pixels** — locked to
   the art's grid, so it does not crawl when the camera moves. This is the
   single largest improvement available and it is about six lines. The source
   gets all its gradients this way; matching that is most of what "looks like
   the same game" means.
2. **Quantise the bed to 8-pixel cells, not single pixels.** Snapping the
   noise to whole town pixels was not enough — every *other* feature on the map
   is built from 8×8 tiles, so a floor whose shapes ignore that grid reads as
   organic blobs sitting underneath a tiled world. An 8 px cell for the coarse
   layer with a 2 px sub-cell for detail keeps it on the grid without going
   uniformly chunky. Dither the boundary between two bed colours rather than
   hard-stepping it, which is exactly how the source transitions between two
   palette entries.
3. **Parallax the bed against the surface.** Offset the floor sample along the
   view direction, scaled by depth. This is the depth cue that darkening can
   never supply: without it the bed is painted *on* the water rather than
   lying under it, and no amount of tint tuning fixes that.
4. **Give the shoreline a direction.** A uniform bright ring around the whole
   coast reads as a selection outline, because it ignores where the light is.
   Take the gradient of the shore-distance channel to get the direction of
   land, dot it with the light azimuth, and use the sign: land toward the sun
   casts *into* the water, land away from it catches a lip. One term, two
   signs, and the coast acquires a direction.

Not yet tried, in rough order of likely payoff: constraining output to the
town's **actual CGRAM entries** rather than a generic ramp (nearest-colour
against the live palette, which would make it exactly on-model); replacing the
smooth sheen band with hard-edged authored *shapes* in the ROM's own dash
idiom; and giving the bed a small set of authored 8×8 patterns instead of
noise at all.

### Shore distance: 8 px is enough again

The field bake was widened from 8 to 32 town pixels when the bed was
depth-graded, because an 8 px clamp leaves only a ~4 px shallow fringe. With
the depth fixed, the distance channel is only consumed by the ~3 px pane edge,
so **8 px would do**. The 32 px bake is kept because it costs nothing and
preserves the option, but nothing in the current model needs it.

## 5. Constraints and traps

- **Vertex colour must survive.** `DrawSimGroundPlane` encodes the cull haze
  dim and the extent alpha in `SDL_Vertex.color`. The fragment shader must
  multiply through by `v_color` exactly as `rim.frag.glsl` does, or the cull
  fade and the town/underlay handoff both break.
- **Present thread.** Every SDL render call runs on the present thread (see
  the present-thread renderer affinity notes). Shader and sampler creation
  must happen lazily from the draw path, the way `EnsureBlurShader` does — not
  at init on the main thread.
- **Default off, and gated twice.** Like every GPU effect here: the
  `gpu_shaders_enabled` backend switch plus its own row, with a silent fall
  back to the current path when the shader is unavailable. A new effect on by
  default would move every golden in the AR_WS_HEADLESS visual-regression
  harness.
- **Feature-mask slot.** `kSimFeature_All` is `(1u << 14) - 1`
  (`src/sim/sim_render_metadata.h:74`), so bit 14 is free for a
  `kSimFeature_WaterSurface` stage. Adding it means the settings row, the
  `kSim3DStageToggles` table (`src/settings.c:598`) and `kSim3DShippedFeatures`,
  all of which are one-line changes by design.
- **The blurred underlay mip.** `s_sim_underlay_blur_texture` is a separate
  downsampled image. Its field texture must be downsampled the same way, or
  the far ocean is shaded through a mask that does not match its own pixels.
- **The ground quad's normal is +Z, not +Y.** Its geometry is a quad in the XY
  plane tilted about X (`Scene3D_BuildViewProjection`), not a horizontal plane
  under an elevated camera. A Y-up normal makes every view-dependent term fire
  uniformly across the whole surface instead of tracking the art, which reads
  as a flat milky wash. This was a real bug in the preview before it was one in
  the engine.
- **CRT post-process composes fine.** It is a resolve pass over the finished
  frame; this is a per-draw render state. No interaction.
- **Not covered here:** the widescreen margin and vertical-extend streaming
  paths also carry BG1 content. Whether they need the same treatment at the
  screen edges is an open question, not a blocker.

## 6. Suggested order of work

1. Water field for the **world-map underlay** only (§1d). Smallest change,
   uses an already-proven tile set, and the ocean is the largest water area
   on screen. Proves the two-sampler render state end to end.
2. Water field for the **town canvas** (§1c), plus the five-town colour-set
   verification.
3. Same field applied to the **live BG1 planes** via the camera offset, which
   is what closes the seam and makes the visible window match.
4. Terms 1-7 from §4 — the quantised bed, fixed depth, absorption, the tile's
   glints, flat sky, the sheen band, the pane edge — behind one settings row
   with a strength slider.
5. Billboard reflections only if that leaves something wanted. On present
   evidence it does not.

A live preview of the §4 model, running the real shader over the captured
Fillmore canvas and the derived mask, is the artifact published from this
work. It is the right place to settle strengths before any of the above is
written.
