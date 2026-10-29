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
