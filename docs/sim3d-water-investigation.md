# Sim-3D water surface: shader feasibility investigation

Status: **preliminary — evidence gathered, nothing implemented.** Companion to
[rendering-engine.md](rendering-engine.md) §7 (tile animation),
[effects-hook-investigation.md](effects-hook-investigation.md) (the same
"identify the authentic state first, then choose a style" method), and
[../specs/ar-recomp-sim-rendering-plan.md](../specs/ar-recomp-sim-rendering-plan.md).

The question: can the sim-3D town view give its water light-based shimmer,
depth and transparency, and what does that cost? The answer is yes, and the
part that looked hardest — knowing which pixels are water — turns out to be
exactly derivable from state the game already publishes.

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
fragment shader covers all three. The wave phase must be computed in **town /
world space**, not UV space, or the animation will visibly shear across the
canvas-to-underlay seam and slide when the camera scrolls. That means each
draw supplies a UV→world affine transform as a uniform; the values already
exist (`slot->sim.camera_x/y`, `underlay_screen_x0`, and the
`texture_x_at_zero` / `span` arguments `DrawSimGroundExtension` already takes).

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
| G | distance to shore, clamped to **32px** (see §4) | bed falloff, absorption, foam |
| B | reserved (flow direction / river-vs-sea class) | directional ripple |
| A | 255 | — |

Sampled with `SDL_SCALEMODE_LINEAR`, R comes back as soft 0..1 coverage at
shorelines, which is what the foam and edge terms want. Do not hard-threshold
it or the edges alias back to 8-pixel stairsteps.

## 4. What the effect should actually be

The house style here is a lit diorama of 16-bit art, not a photoreal ocean.

**The direction, decided after seeing a live preview of the alternative: the
ROM's own tile animation carries the motion, and the shader supplies depth.**
An earlier pass built the effect around animated specular shimmer and a
scrolling wave normal. On the real art it fought the ROM's 4-frame CHR cycle
rather than adding to it — two unrelated motions on one surface. Everything
below therefore has **no time term at all** except one deliberately small
ripple. The authentic 7.5 fps cadence is the movement the eye should read.

1. **A bed under the water.** Shore sand fading to a dark floor with the shore
   distance, plus static grain. **This is invented content** — the ROM's water
   tiles are opaque and there is no seabed art anywhere in the game. Building
   it out of the map's own palette (bank 1's two sand entries, and a floor
   derived by darkening the deep-water colour) is what keeps it from reading
   as imported from a different game. It is a larger departure than lighting
   existing pixels, and worth taking deliberately rather than by drift.
2. **Transparency over that bed.** Absorption driven by shore distance:
   shallow water is nearly clear, deep water hides the bed. Note the ground
   plane itself must stay **opaque** — it is the backing that stops the
   world-map underlay showing through (see the comment at
   `src/present_sim3d.c:2444`). The transparency is between the synthesised
   bed and the water body, entirely inside the shader.
3. **The authored art returns on top as the surface.** Classify each water
   pixel by luminance against the three authored shades: the light crests stay,
   the flat body becomes glass. This is the term that makes the tile animation
   the thing you actually watch, and it costs one dot product. A hard
   three-way colour match would break under the canvas's `LINEAR` filtering; a
   `smoothstep` over luminance does not.
4. **Static sheen.** Fixed glare patches from low-frequency noise, pinned in
   town space and weighted toward the crests the ROM already drew. Placement
   comes from `SimShadowLight()` (`src/present_sim3d.c:766`) so the glare
   agrees with the shadow direction instead of implying a second sun;
   elevation sets brightness only, never position. Glare that ignores the art
   reads as fog sitting on the water rather than light coming off it.
5. **Shoreline foam.** A band inside ~4 px of shore from the distance channel.
   Static; the ROM's crest animation supplies its liveliness.
6. **One small live ripple.** A single specular term on an animated normal,
   default low. Kept only as a dial, not as the effect.

Deferred: **billboard reflections** (a second mirrored, mask-clipped object
pass — real cost and real risk to the sort order) and **refractive wobble**
(offsetting the colour sample by the wave normal; it must be clamped inside
the mask or it drags grass into the water, and it is motion we just decided
not to add).

### Shore distance needs ~32 px, not 8

The first field bake clamped shore distance to 8 town pixels. That is fine for
a foam band and useless for anything else: it leaves a ~4 px shallow fringe and
everything beyond it is flat "deep". Clamping to **32 px** puts about 60% of
Fillmore's water inside the ramp, which is what gives the bed and the
absorption somewhere to happen. Foam then uses the first ~14% of the range.

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
  the far ocean shimmers on a mask that does not match its own pixels.
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
4. Effects 1-4 from §4, behind one settings row with a strength slider.
5. Refraction and reflections only if 1-4 leave something wanted.
