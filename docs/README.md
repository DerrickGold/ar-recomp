# ActRaiser documentation

Guides for playing ActRaiser Recompiled, creating replacement content, and
working with the original game's data. For downloads and build instructions,
start with the [project README](https://github.com/DerrickGold/ar-recomp#quick-start).

## Playing and customizing

- [Manual](manual.md) — controls, settings, saves, and troubleshooting.
- [Regional differences](regional-differences.md) — a plain-English comparison
  of the releases, their mechanics, and what is still being investigated.
- [Unused content and oddities](unused-content.md) — debug facilities,
  developer-text reports, unexplained artwork and skipped code; separate
  from features removed or changed between regions.
- [Modern SIM menu](manual.md#modern-sim-menu) — enable the compact town menu,
  adjust its scale, and use the angel's Describe hint.
- [Builder and Workshop](builder-workshop.md) — building the game and replacing
  artwork, music, and text.
- [Desktop packages and user data](desktop-packaging.md) — portable installs,
  save locations, and packaging a local build.
- [Language packs](language-packs.md) — authoring, validating, installing, and
  sharing translations.
- [Language pack format](language-pack-format.md) — UTF-8 scripts, placeholders,
  and page/control rules.
- [Performance overlay](performance-overlay.md) — reading measurements and
  reporting a slow scene.

## Modding references

The game-data references describe the US ROM unless stated otherwise. Entries
marked uncertain should not be treated as verified offsets or behavior.

- [RAM map](ram-map.md) and [ROM map](rom-map.md) — known state and data regions.
- [Regional differences: technical evidence](regional-differences-technical.md)
  — ROM tables, code boundaries, measurements, and limits of the evidence
  behind the reader-facing article.
- [Symbol map](research-symbol-map.md) — ROM addresses and their known purposes.
- [Dialogue and menu text](dialogue-system.md) — text encoding, source identities,
  and replacement boundaries.
- [Simulation command menu](sim-menu-reference.md) — actions, original icons,
  offering inventories, and description/confirmation boundaries.
- [Reusable dialogue host](text-template-core.md) — templates, mixed typography,
  shared playback mechanics and the standalone sample.
- [Save format](save-format.md) — SRAM layout, checksum, and editable fields.
- [Native audio channels](snes-native-audio-channels.md) — original SPC driver
  voices, requests, and effect sequences.
- [Simulation objects](sim-object-catalog.md) — object records and visual identities.
- [Action scene editor](https://github.com/DerrickGold/ar-recomp/blob/main/tools/action_editor/README.md) and
  [diorama depth shapes](diorama-depth-shapes.md) — authoring enhanced room layouts.

For other game ports and native extensions, see the
[recompiler guide](https://github.com/DerrickGold/ar-recomp/tree/main/snesrecomp-go) and
[runner SDK](../snesrecomp-go/runtime/docs/README.md).

Internal architecture notes, investigations, plans, and validation reports belong
in the ignored `development/` tree, not in this public documentation set.
