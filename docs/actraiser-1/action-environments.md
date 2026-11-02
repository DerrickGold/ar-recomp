# ActRaiser 1 action environments

These enhancements use AR1 rooms and regional terrain. The shared
[action scene editor](https://github.com/DerrickGold/ar-recomp/blob/main/tools/action_editor/README.md)
provides the authoring controls; [diorama depth shapes](../diorama-depth-shapes.md)
describes room geometry.

## Aitos Act 1

Rooms `04:01–03` use the **Environmental effects** setting. Their definitions
are editable in `action-effects.ini` for US, Japanese, and European terrain.

| Room | Environment |
| --- | --- |
| 04:01 | Distant lightning and layered clouds behind the cliffs; lava bubbles, warm light, and localized heat shimmer. |
| 04:02 | Waterfall impact spray, mist, and light across the crossing. |
| 04:03 | Waterfall plumes, low mist, and sunlight around the boss arena. |

Select BG2 to edit distant weather anchors; airborne particles and lava heat
regions use BG1. **Distant storm lightning** exposes strike area, timing, tint,
intensity, pattern seed, bends, and forks. Scenery, player, and enemy lighting
receivers are independent. Distant Aitos lightning leaves those receivers off
by default so it does not light fighters in front of the cliffs.

## Aitos Act 2

Rooms `04:04–07` use the **Environmental effects** setting and shared editable
recipes for all three regional terrain profiles. Lava is the main light source
in the first three rooms: a continuous warm wash rises from each lake, with
soft bank edges, scenery shadows, slower embers and local smoke. Scenery,
player and enemy light receivers remain independently selectable. Local heat
regions distort scenery while actors and the HUD stay sharp.

Lower foreground and background ambience separates the warm lava-facing edges
from unlit rock. Broad light gradients add bounce around the crossings, and
warmer smoke makes the heated air visible above the lakes. BG2 ambience uses
the shared background-light material with all ray components disabled.

Lava surfaces carry slow cooling crust with hot seams, occasional bubble domes
and short splashes whose droplets fall back inside the banks. A brief local
flash at each burst warms nearby stone and foreground smoke. Small pits use
fewer cells and bubble sites. Sparse rising ash adds larger nearby flakes and
finer background particles; all motion follows the shared visual clock.

The statue descent retains its native timed flames. The final room has no
lava; its original background statue flames and warm floor fill frame Flaming
Wheel's orange fire and cyan attacks.
Its added torch sparks and motes are composited into the BG2 source before
filtering, so they inherit the statues' DoF in plane, skybox-only and combined
views. Torch illumination still uses the separate scenery/actor receiver pass.

The prominent repeating bubble mounds are original animated BG1 artwork,
separate from the small procedural splashes. In Diorama, each mound now plays
the original four frames on an independent schedule anchored to its world cell.
The default uses eight ticks per frame, variable 48–144-tick quiet intervals,
and a 75% burst chance per cycle. Skipped cycles create longer pauses and change
which sites are active. Scrolling, pausing and reverse seeking reproduce the
same pattern. Only metatile `02` receives character overrides; the continuous
lava lip (`01`) and stepping-platform artwork (`03/04`) keep their native art.
The original animation bytes remain immutable and gameplay/VRAM are untouched.

Lava illumination now moves slowly across the surrounding rock. Each native
bubble mound also gives a short warm flash and a few small sparks during its
own spurt/drop frames. Erased or replaced mound artwork produces no reaction.
Small rock vents emit irregular puffs with warm contacts; background vents share
the wall’s DoF. Sparse groups of ceiling grit stop at an authored stone or lava
contact, producing a soft dust puff or a tiny splash. These are visual effects;
terrain and gameplay remain unchanged.

Select a **lava-lake** marker and **Edit complete lava / waterfall definition**
to edit **Upward light** strength, reach, bank feather, shadow strength and
colour. Setting its strength to zero restores radial spill lighting. Older
saved complete definitions retain their previous lighting. Smoke, heat and
exposure are ordinary placed effects; Save project includes them in
`action-effects.ini` without altering scenery edits.

**Moving lava light** (`up-motion`) sets strength, wavelength and cycle frames.
**Native bubble reaction** (`native-reaction`) sets flash strength/radius and
spark count/rise. Reactions require native bubble staggering. Previously saved
complete fields keep both additions disabled until explicitly enabled.

The Add effect menu has **Rock vent** and **Falling grit** presets. A vent’s
bottom centre is its rock contact; use BG2 fixed point + Background placement
for a distant vent. A grit region runs from its ceiling attachment to the first
surface below. Choose its stone/lava impact and keep the path clear of platforms.
Active fraction and Cycle frames control the pauses. All placements are saved
in the project’s effects file, with separate records for US, JP and EU terrain.

The complete lake field also exposes `molten-surface`, `molten-bursts`,
`molten-color` and `burst-light` (strength and 8–96 pixel radius). Set seam
strength and bubble sites to zero to disable surface activity, or set burst
light strength to zero to keep bubbles without flashes. Older saved fields
import with these additions disabled. Ash regions are ordinary particle-area
effects with independently editable near/far placement, drift, size and colour.

The **Native bubbles** controls (`native-bubbles`) set staggering on/off, burst
chance, minimum/maximum quiet intervals, animation frame duration and pattern
seed. They apply to rooms `04:04–06` in all three terrain profiles. Turning
staggering off restores the synchronized native animation; zero burst chance
keeps the mounds quiet. Older complete fields import with staggering disabled.
The game and editor use the same capture code, and **Environmental effects**
also switches the native animation treatment off.

Lava rendering shares one ember-motion helper for pits and lakes, with separate
surface-height rules. Native character animation and reaction capture share the
same timing configuration. A shared burst description supplies mesh lighting
and batched mist samples; each bank evaluates its nearby flashes once. The
upward-light mesh reuses corner illumination while keeping its original quad
order, clipping and shadow sources. These calculations use local scratch space
and preserve deterministic seeking without adding simulation state or GPU passes.

## Marahna Act 2

Rooms `05:04–08` also use **Environmental effects** and editable recipes for
US, Japanese, and European terrain.

| Room | Environment |
| --- | --- |
| 05:04 | Cool temple fill, warm shrine torches, window light, dust, and low wisps. |
| 05:05 | Lattice-window light, torches, a dim descent, and mist below the spikes. |
| 05:06–07 | Shared light positions for the arch gallery and sanctuary, with wet ledges and drips. |
| 05:08 | Warm light around the gold relief, cooler window light, and low arena mist. |

These regions use BG1/world coordinates. Window, torch, and ambient lighting
can affect scenery, the player, and enemies. Rooms `05:06–07` share a map, so
their authored light positions remain consistent across the room transition.

## Death Heim final battle

For room `07:08`, enable **Action 3D → Scene → Diorama 3D** and **Environmental
effects**, the same master used by other action rooms. The completion-scene
setting is independent.
The enhancement adds glow, colour grading, fighter illumination, and sparse
drifting dust to the original nebula and starfield artwork.

In the editor's **Shared renderer**, enable **Environmental effects**.
Use **Settings → Native nebula glow** to edit the material. The preview switch
does not erase authored values. Project and level INI exports preserve
the material in `diorama-layers.ini`:

```ini
[layers:07:08]
native-bloom = strength:0.85 radius:13 threshold:0.24 saturation:1.08 contrast:1.05 light:1.3 receivers:6 dust:0.3 dust-scale:1 dust-speed:1 dust-blue:467ccf dust-rose:bc438b
```

Radius is in native image pixels. Glow strength and fighter illumination are
independent; receiver bits are player `2` and enemies `4` (`6` for both).
Shift-click resets individual controls; resetting the material restores defaults.

Dust density `0` disables the added clouds; speed `0` freezes them. Spread,
drift, and the blue/rose colours are independently editable. Pausing freezes
their gameplay clock, and editor seeking reproduces the same time. Save,
reset, undo, and INI reload preserve these controls; older material entries
inherit the default dust values.
