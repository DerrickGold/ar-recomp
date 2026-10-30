# v0805 — draft

Changes since **v0804**.

This update adds atmosphere to Marahna and Aitos, enhances Death Heim's original
final-battle artwork in diorama mode, and reduces rendering and editor overhead.
It also brings a bonus from our ongoing ActRaiser 2 work: its winged Master is
now available to import and play in ActRaiser's stages.

## ActRaiser 2 and the winged Master

Work on ActRaiser 2 support is underway, including its action stages and world
navigation. Full-game support remains in early development.

As a bonus, use ActRaiser 2's Master in ActRaiser's stages, with gliding, diving
and sword moves in both classic and diorama modes:

1. In **Workshop → Sights & sounds → Game imports**, choose
   **ActRaiser 2 / Master character**.
2. Select your own unmodified **ActRaiser 2 (USA)** ROM (`.sfc` or `.smc`) and
   press **Extract & install**.
3. Restart your updated game and open **System → Game**. Enable
   **ActRaiser 2 Master** under **Quality-of-life improvements**. The setting
   starts off; you can switch characters during the same level.

Importing also adds **ActRaiser 2 · above the clouds** to the Workshop's
**Scenery** choices. **ActRaiser 2 · Fillmore patrol** becomes available when
the Workshop also has the original ActRaiser scenery from your US build ROM.
The scenes respect your atmosphere and reduced-motion preferences.
See the [Workshop import guide](https://github.com/DerrickGold/ar-recomp/blob/main/docs/builder-workshop.md#actraiser-2-master).

## Presentation and environments

**Classic** means the flat action renderer, including widescreen. **Both modes**
means classic and diorama. Environmental effects are optional in either mode;
the background-material enhancements below require diorama.

- **Both modes, with Environmental effects:** Add Marahna Act 1 swamp atmosphere
  with canopy light, rolling mist and wakes, hanging vines, fireflies, jade wisps,
  moss and dew.
- **Both modes, with Environmental effects:** Add Marahna Act 2 temple atmosphere
  with warm shrine torches, cool window light, dust, low wisps, wet ledges and
  drips. Lighting stays aligned across the connected gallery and sanctuary.
- **Both modes, with Environmental effects:** Refine Aitos Act 1 lava bubbles,
  molten detail, warm light, waterfall spray and mist. Environmental recipes
  support US, Japanese and European terrain layouts.
- **Diorama, with Environmental effects:** Refine Aitos's distant storm and
  layered cloud lighting, with editable cloud openings, light shafts and
  background brightness.
- **Diorama, with Environmental effects:** Enhance Death Heim's original
  final-battle nebula with coloured glow, fighter illumination and sparse
  blue/magenta drifting dust. Preserve the original artwork, fight, sprites and
  timing; pause freezes the added motion. Enable **Action 3D → Scene → Diorama
  3D** and **Environmental effects**. The normal effects switch controls this
  enhancement in the game and editor; the completion-scene option is independent.

Room-by-room diorama presentation work remains ongoing, including widening tile
maps, refining background policies and skybox rendering, and adding unique
environmental effects to more levels.

## Action editor

- Make the new Marahna and Aitos effects editable with the shared game/editor
  rendering code. Save environmental recipes and material settings in level
  INI files and project archives.
- Add Death Heim nebula controls for glow, fighter lighting, dust density,
  spread, speed and colours, with reset, undo and saved material settings.
  Preview them with the regular **Environmental effects** switch, initialized
  from the game's setting.
- Increase the total environmental-recipe capacity from 256 to 1,024 entries.
  Project archives now accept `action-effects.ini` files up to 512 KiB, previously
  128 KiB, retaining per-room geometry limits.
- Reuse unchanged tile frames in native and browser previews while effects and
  receiver lighting continue animating.
- Improve warmed-room benchmarking and diagnostic captures, including compressed
  lossless PNG output.
- Add classic widescreen scenery baselines to the room benchmark for comparison
  with diorama output.

## Rendering performance

- Reduce repeated mist-lighting calculations and effect-resource preparation.
- **Diorama:** Reduce sprite-capture clearing work and repeated lighting work
  for transparent sprite pixels.

Performance improvements depend on the device, graphics backend and scene.

## Documentation and updating

- Group public game references under `docs/actraiser-1/`, with shared application,
  Builder and authoring guides at the top level. Update packaged documentation
  and links for the new layout.
- Keep internal research, benchmark journals and migration handovers out of
  release packages while retaining player controls and authoring guides.

For desktop installations, rebuild with the updated Builder. See
[Updating an installation](https://github.com/DerrickGold/ar-recomp#4-update-or-import-an-existing-installation).
If you are moving from another installation, use **Import previous installation…**
to bring its data into the selected game folder. Older saves in that data folder
are automatically adopted into Slot 1 on first launch, with their originals
preserved; no manual conversion is needed. See
[Upgrading older saves](https://github.com/DerrickGold/ar-recomp/blob/main/docs/manual.md#upgrading-older-saves).
