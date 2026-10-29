# ActRaiser Recompiled documentation

Player guides, content-authoring contracts, and maintained game-data references.
For downloads and build instructions, start with the
[project README](https://github.com/DerrickGold/ar-recomp#quick-start).
The [release notes](release-notes.md) describe the next release's changes.

## Playing and creating content

- [Manual](manual.md) — controls, settings, saves, and troubleshooting.
- [Builder and Workshop](builder-workshop.md) — building the game and replacing
  artwork, music, and text.
- [Desktop packages and user data](desktop-packaging.md) — portable installs,
  save locations, and packaging a local build.
- [Android preview builds](android.md) — source builds, installation, touch
  controls, and data locations.
- [Language packs](language-packs.md) and [pack format](language-pack-format.md)
  — translation authoring, validation, installation, and sharing.
- [Text templates and standalone sample](text-template-core.md) — typography,
  playback controls, and a reusable host example.
- [Performance overlay](performance-overlay.md) — interpreting measurements
  and reporting a slow scene.
- [Action scene editor](https://github.com/DerrickGold/ar-recomp/blob/main/tools/action_editor/README.md)
  and [diorama depth shapes](diorama-depth-shapes.md) — authoring enhanced rooms.

These application and tool guides remain at the top level. Native gameplay,
memory layouts, scripts, and save formats are grouped by game.

## ActRaiser 1

The [ActRaiser 1 index](actraiser-1/README.md) contains regional settings and
comparisons, action environments, the randomizer, and native modding references.
Start with its [RAM map](actraiser-1/ram-map.md),
[ROM map](actraiser-1/rom-map.md), or [save format](actraiser-1/save-format.md)
when inspecting game data. References describe the USA release unless another
release is explicitly named; uncertain entries retain their evidence limits.

For other game ports and native extensions, see the
[recompiler guide](https://github.com/DerrickGold/ar-recomp/tree/main/snesrecomp-go)
and [runner SDK](../snesrecomp-go/runtime/docs/README.md).

## Documentation placement

Keep player/modder guides, published file formats, reusable-tool usage and
durable reverse-engineering findings here. Give each game's native references
their own directory and state the release and address space. Never reuse one
game's RAM addresses, script meanings, or campaign rules as another's reference.

Implementation plans, prototype integration notes, probing recipes, handovers,
benchmark runs and validation journals belong in the ignored `development/`
tree. Its `docs/actraiser-1/` and `docs/actraiser-2/` directories hold separate
game research; `docs/shared/` holds host/bundle work and shared rendering research.
Keep useful controls and authoring instructions in the public guide when moving
a mixed document's research history. Public links and release packages must not
depend on those local records.
