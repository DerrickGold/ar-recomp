# Death Heim final-battle native-art enhancement

Validated 2026-10-29. Room `07:08` uses its original blue/magenta nebula,
starfield, raster-wave animation and layer geometry. The optional material adds
coloured highlight bloom, restrained colour grading, projected fighter
illumination and sparse drifting blue/magenta cloud wisps. It does not load the
previous generated cosmic backdrop or floor.

## Controls

In game, enable **Action 3D → Scene → Diorama 3D** and **Death Heim final
battle**. The latter remains opt-in, persisted as `death_heim_final_battle`.
The completion-scene setting is independent.

In the action editor, select room `07:08`, open **Shared renderer**, and check
**Native nebula glow (07:08)**. **Settings → Native nebula glow** controls the
material. Save project and level INI exports include it in `diorama-layers.ini`:

```ini
[layers:07:08]
native-bloom = strength:0.85 radius:13 threshold:0.24 saturation:1.08 contrast:1.05 light:1.3 receivers:6 dust:0.3 dust-scale:1 dust-speed:1 dust-blue:467ccf dust-rose:bc438b
```

Radius is measured in native image pixels. Glow strength and fighter illumination
are independent; receiver bits are player `2` and enemies `4`. Shift-click resets
individual values, and resetting the material restores engine defaults. The
preview checkbox does not change the game's preference.

Space dust has independent density, spread, drift and two colour controls in the
same modal. Density `0` restores the bloom-only image; drift `0` holds the cloud
mesh still. Legacy saved material lines inherit the new dust defaults. Save,
reset, undo and INI reload cover these controls too.

## Nebula dust

Six sparse veils reuse `CloudMaterial_Wisps`, `CloudMesh_Patch` and
`CloudMotion_Orbit`. Different phases and slow differential shear produce moving
space dust while preserving dark gaps and the original bright nebula ribbons.
The veils follow the native background projection and remain behind fighters.
Their time comes from the paused gameplay scene clock, with integer harmonics
that remain continuous at its 16-bit wrap. Master fades also fade the dust.

All variants occupy one immutable 576×96 ARGB atlas (216 KiB) baked during boot
preparation. A single 210-vertex geometry draw joins the existing native-art target
before bloom extraction; there is no extra render target, filter pass, shader or
readback. Rendering reuses the existing colour grade and radiance map. The
resident skybox path now publishes its captured-frame fitted bands for CPU
details, using the same already-computed fit as its GPU drawing path.

Native and WASM tests cover one retained cloud batch, pause, backward seeking,
clock wrap, zero speed, independent disable, invalid configuration rejection,
legacy loading, round trips, fades and hidden resident-band slots. Game/editor
builds passed. The sanitized native/WASM command comparison passed all 134 cases
(3,371,891 values; maximum floating-point difference 0.000144), including the
new cloud draw and master fade. Metal, Deck Vulkan and Windows DX12/Vulkan captures differ by at
most 4/255 per channel; mean differences are below 0.25/255. DX12 and Vulkan on
Windows are identical. Dust disabled is pixel-identical to the previous
bloom-only Metal capture. No late shader compilations occurred in the timed runs.

Three alternating Deck pairs at 1280×800 used 240 warmup and 240 measured frames,
with animated native waves, identical settings except dust density, and display
wake/power checks before each run. Medians across runs:

| Material | Whole harness mean | Whole harness p99 |
| --- | ---: | ---: |
| Native bloom, dust off | 7.975 ms | 8.784 ms |
| Native bloom + dust | 8.386 ms | 9.239 ms |

The paired mean increases were 0.259, 0.411 and 0.259 ms. These include shared-room
preparation, submission and GPU completion, and do not certify full-game 90 Hz
presentation. Windows measurements here are compatibility checks only.

Evidence: `runs/death-heim-dust-20261029/`, including native platform captures,
timing/provenance JSON, regression logs and a 16-second 640×360 comparison MP4
(about 1.6 MB). The clip shows eight seconds each of bloom only and bloom plus
dust, using the player stand-in. Intermediate PNG frames are removed after
encoding; representative stills remain.

## Earlier bloom calibration and render cost

The wider halo softens bright filament cores without blurring the source art.
Saturation and contrast stay close to native values, preserving dark space.
The existing radiance map provides matching blue/magenta illumination on actors.

Fixed an intensity bug: strengths above one previously reached normalized texture
tint validation and silently fell back to ordinary rendering. Gain now travels
through the existing shader uniform block and additive glow draw. The editor's
full `0–3` strength range works without adding a pass, texture or sampler.

The original background geometry is submitted once into an output-resolution
target. Extraction/horizontal blur and vertical blur use targets bounded to
640×360, followed by grading and glow composition. Fighter lighting reuses the
radiance target; matching receiver maps can be shared. Authored scenery lighting
already has a retained-map guard and is reused after bloom preparation.
There are no effect texture uploads or CPU readbacks during animation. The shader
is included in boot preparation for Metal, Vulkan and DirectX 12; gain is a
uniform, not a new pipeline variant.

## Validation

- Game and editor/WASM rebuilt successfully.
- Native GPU material tests passed on Mac Metal, Steam Deck Vulkan, Windows DX12
  and Windows Vulkan, including gain `0`, `1`, `2` and `3`.
- Shared-WASM regression tests cover source geometry, pause/seek, aspect/orbit,
  background modes, resource reuse, receiver controls, disable restoration and
  normalized draw tints throughout the intensity range.
- Editor tests cover defaults, reset, undo and INI/project persistence.
- Shader source/generated-artifact checks passed. No late shader builds were
  reported after warmup in the captured native runs.
- The final disabled Metal image is pixel-identical to the pre-polish native
  baseline. Enabled captures were reviewed at 16:9, 16:10 and a wide 2.4:1 view.
- Matching 16:9 captures on Deck Vulkan and Windows DX12/Vulkan differ from
  Metal by at most 4/255 per channel (mean below 0.08/255). The Windows DX12 and
  Vulkan captures match each other.

Steam Deck measurements used the native shared renderer at 1280×800, three
alternating on/off pairs, 240 warmup frames followed by 240 measured frames per
run. Each run advances the native wave animation. The display was woken and
brightness/power checked before every run. Results are medians across runs:

| Material | Whole harness mean | Whole harness p99 |
| --- | ---: | ---: |
| Original | 7.925 ms | 8.352 ms |
| Enhanced | 8.475 ms | 9.297 ms |

The mean increase is about 0.55 ms. These are offscreen preparation, submission
and GPU-completion timings, not pure GPU durations or full-game presentation
measurements; they do not certify 90 Hz gameplay. Windows results establish
DX12/Vulkan compatibility only, given its unattended display state.

Evidence is under `runs/death-heim-native-20261029/`: platform captures, material
logs, Deck traces and provenance, build/test logs, and a 12-second 640×360
before/after MP4 (about 0.94 MB). The video uses the shared renderer with the
player stand-in, not a recorded boss fight. Bulk intermediate PNG frames are
discarded after encoding; representative stills and timing traces remain.
