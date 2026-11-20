# Install, create and share language packs

Language packs are data, not plugins or executable code. You do not need the
language editor to install one, write a translation, or publish your work.
This guide is the short workflow; [the format reference](language-pack-format.md)
defines the complete script syntax and game constraints.

## Install a shared `.o2r` language pack

1. Open **Manage Mods** and choose the `.o2r`, or drop it onto the Builder.
2. Use the **Languages** filter to preview, enable or uninstall packs.
3. Restart the game and choose the enabled pack under **Localization → Text source**.

The same picker converts legacy `.arlang` publications to `.o2r`.
Installing v1 content prepares a v2 game copy.

Private `.arproject` backups and compatible legacy ZIPs remain under **Author
backup or legacy archive** in **Workshop → Languages → Open a project or backup**. Unpacked authoring folders
are available under **Advanced: unpacked language folder**.

### Manual installation

Use the game's writable data directory, not the inside of an application bundle:

1. Close the game.
2. Copy the `.o2r` archive into `game-assets/mods/` or a registered external mods
   folder. For a legacy `.arlang`, use `game-assets/languages/packs/`.
3. Start the game normally. Older `utils/` bundles still use their `run-game`
   script.
4. Open the system overlay → Localization → Game text and select its package name.

The filename can contain spaces and does not have to match the package name,
ID or locale. Avoid filenames starting with a dot: hidden entries are ignored.
Discovery is at startup, not live file watching. Already discovered languages
can be switched during play. External packs automatically use enhanced fonts;
Native US still offers original rendering or enhanced rendering of US text.

Multiple translations of the same locale appear separately by name. The
language ID in `pack.ini` identifies the translation across releases. For one
ID, the highest declared `.o2r` release takes priority. If the latest releases
have different content and the same version, or an enabled legacy copy has
different content, choose which copy to use in the Languages library.

The game prepares archives through its bundled `actraiser-builder` helper;
it never opens the editor. Native game apps carry the helper inside the app;
folder outputs retain it under `tools/` (`utils/tools/` in older bundles).
Keep the helper when moving or slimming a folder installation. The Workshop's
build-tool cleanup already retains it. A source CMake build
with Go available builds the helper beside the game executable. Other ports
can provide their own archive adapter; unpacked folders need no helper.

## Apply inline styling in Workshop

The message editor's **Inline styling** toolbar supports **Italic**, **Upright**,
font roles, named styles, colors, and sizes from 25% to 400%. Select text or a
complete placeholder, then apply a style. With no selection, the toolbar inserts
an empty tag pair and places the cursor inside it. Normal Undo reverses the edit.
Font choices include unsaved roles from the Fonts tab; named styles come from
the pack's `@define-style` declarations.

Selections across lines or table cells style each text portion separately,
leaving commands and separators intact. Existing tags stay in place; the new
style is applied inside them. Native references are read-only, aliases must
first be expanded, and inline terms inherit their caller's style. A v1 project
needs **Create v2 copy** before using these controls. Use **Validate & preview**
to review mixed-font wrapping and size changes before saving.

## Upgrade a v1 pack

Installing a v1 pack through the Builder automatically upgrades the installed
copy to v2, keeping its package ID. **Import & install** and **Review before
installing** explain this before installation; the completion screen confirms
the upgrade. The command-line `language install` also upgrades the installed
copy and reports it. Original archives and saved projects stay unchanged.

To edit or publish the converted templates, open the existing project in
Languages and choose **Create v2 copy** with a new package ID. The builder
writes inferred historical layout, font, numeral and color rules into readable
templates. It preserves wording, control anchors,
progress and notes. Review the new copy in side-by-side playback, then install
it. Disable the old installed pack when switching to its replacement.

For an unpacked v1 pack, the same conversion is available from the helper:

```sh
"$BUILDER_CLI" language upgrade --pack /path/to/v1-pack \
  --new-id my.translation-v2 --out my.translation-v2.arproject
```

See [command-line setup](#command-line-setup) for `BUILDER_CLI`. Conversion
creates a new output; it does not overwrite the input or an existing output.
Generated Native US packs are upgraded by rebuilding with the current Builder.
If a v1 pack is selected when launching directly, the game shows upgrade
instructions. **Continue with native text** uses original text for that session
and preserves your saved language selection. The game does not convert packs.

## Where files belong

The **game data directory** is the folder containing `config.ini`, `saves/`,
and `game-assets/`:

- **Desktop Builder output:** The game folder selected in the Builder.
- **Per-user macOS app:** `~/Library/Application Support/ActRaiserRecomp/game/`.
- **Per-user Linux AppImage:** `$XDG_DATA_HOME/ActRaiserRecomp/game/`, or
  `~/.local/share/ActRaiserRecomp/game/` when `XDG_DATA_HOME` is unset.
- **Legacy archive:** The download's `utils/` folder.
- **Source build:** Normally the repository root.

An explicit data-directory override takes precedence. Native launchers support
`--print-paths` to report the selected directory without starting the game;
see [desktop packaging](desktop-packaging.md). Launch a new app once to
initialize its data before installing packs manually. The Workshop edits the
game folder selected in the Builder, which may differ from a per-user app's data.

Paths below are relative to that data directory.

| Artifact | Purpose and location |
| --- | --- |
| `.o2r` | Reviewed publication in `game-assets/mods/` or a registered external mods folder. |
| `.arlang` | Legacy publication accepted for import and discovery in `game-assets/languages/packs/`. |
| Unpacked pack | `game-assets/languages/packs/<exact-package-id>/pack.ini` plus its declared files. |
| Native US source | Generated locally by the game build under `game-assets/languages/native-us/`; retain it for fallback. |
| `.arproject` / `projects/` | Private workshop backups, source templates and progress; not an installed game translation. |
| `.arlang-cache/` | Disposable prepared archive snapshots inside `packs/`; never publish or edit them. |
| `.arlang-state/` | Archive enable/disable preferences inside `packs/`, keyed by package ID. |

An unpacked pack needs only `pack.ini`, every declared `.artext` script and any
declared local font files. Preserve author/license notices when copying it.
The directory name must exactly match its manifest ID, including case. No
workshop project, progress file, `package.json`, registration step or `v-*`
subdirectory is required. Builder-installed folders do use immutable `v-*`
subdirectories: copy all referenced files, not just the outer manifest.

## Update, disable or remove

Installing a newer Builder-managed `.o2r` replaces the previous release and
preserves its enabled state. The Builder selects among launcher-managed releases
and the launcher manages the source files. A changed archive gets a new snapshot
so a running game can finish using its current copy.

For installed `.o2r` packages, use **Languages** or the shared `mods` commands:

```sh
"$BUILDER_CLI" mods list --output-dir "$GAME_DATA"
"$BUILDER_CLI" mods disable --output-dir "$GAME_DATA" language-example.fr-ca
"$BUILDER_CLI" mods enable --output-dir "$GAME_DATA" language-example.fr-ca
```

Legacy archives and folders have their own commands:

```sh
"$BUILDER_CLI" language disable --root "$GAME_DATA" --installed my-pack.arlang
"$BUILDER_CLI" language enable --root "$GAME_DATA" --installed my-pack.arlang
"$BUILDER_CLI" language uninstall --root "$GAME_DATA" --installed my-pack.arlang
```

`--installed` names the file or folder **as it appears in `packs/`**, not its display name.
Restart afterward. Uninstalling an archive moves it into `packs/.uninstalled/`
and prints its recovery path. Unpacked installs retain a recovery manifest.
Editable projects and the Native US source are untouched. With the game closed,
you may remove `.arlang-cache/` to reclaim space; needed snapshots regenerate.
Do not remove `.arlang-state/` unless you intend to reset archive availability.

Workshop's **Install in game** creates an unpacked snapshot for testing a
project. **Save** (including Cmd+S / Ctrl+S) automatically updates that
unpacked copy with the same package ID. Disabled
packs stay disabled; uninstalled packs are not reinstalled. All supplied draft
messages are retained. Restart the game to load the new copy. A failed update
leaves your Workshop progress saved and shows an explanation.

To distribute an update, use **Export .o2r…** to create a reviewed `.o2r`
with the same language ID and a higher release version.
Installing someone else's publication does not
ask you to claim redistribution rights or filter its WIP content again.

## Author with a text editor or AI

Use these public resources together:

- [Language pack format](language-pack-format.md): manifest, commands, fonts,
  placeholders, tables, keyboard rules, page limits and fallback behavior.
- [Machine-readable route reference](language-authoring-reference.json): every
  semantic ID, allowed values/types, ordered native anchors and presentation
  constraints for a US-runtime translation. `required_for_complete` distinguishes
  required routes from optional additions. Generated from the shared Go registry.
- [Example source pack](../examples/language-pack/pack.ini) and
  [example publication](../examples/example.fr-ca.o2r): two independently authored
  French example messages, not a full translation or an extracted retail script.
- Your own locally extracted US source script for the messages you are translating.
  Follow `[scripts] source` paths in `native-us/pack.ini`; do not edit that baseline.

The links above are available without a source build. Legacy archives include
them under `utils/docs/` and `utils/examples/`; the macOS Builder carries the
same files under `Contents/Resources/payload/utils/`. Do not assume a generated
game app includes the authoring examples.

Create a separate directory, copy the example layout, and choose your own stable
ID, locale, name, authorship and license. Use `version = 2`, `target = us-runtime`,
`source_profile = us`, `fallback = native-us`, and usually `coverage = partial`.
Omit untranslated messages so the game uses its local US fallback. If you use an
alias, its target must also be included in the same pack. The Builder keeps a
regional ROM's full extraction as a reference and installs a separate local
`.o2r` with messages that fit the US route contracts. Use those US contracts
when authoring a translation.

A useful instruction to give an AI is:

> Produce UTF-8 `.artext` files and `pack.ini` following the supplied format and
> US authoring reference. Translate only the requested message IDs. Preserve
> every required `@anchor` exactly and in order. Never invent IDs, placeholders
> or events. Keep fixed-menu cell/row order and keyboard geometry. Use `@line`
> for intentional breaks; physical newlines alone are wrappable. Added dialogue
> pages must precede a terminal `yield.*` anchor. Omit untranslated messages.
> Preserve contributor credits. Do not include private notes or source templates.

Use Workshop **Preview** to compare the source with an unsaved translation,
including inline fonts, color, relative size, wrapping and reveal timing. Test
long dynamic values and inspect the template/font trace before installing.
See the [v2 syntax and playback guide](language-pack-format.md#v2-templates-and-appearance).

AI output still needs validation and in-game review. Format validation does not
prove translation quality, correct shaping, readable layout or redistribution
permission. Game language packs do not replace the separately shipped overlay
or builder interface catalogs.

## Validate and package without the GUI

### Command-line setup

The command-line tools are still included; no Go installation is needed when
using a packaged helper. The desktop Builder's outer executable opens the GUI
and does not forward `language` commands. Invoke the helper itself:

| Installation | Command-line helper |
| --- | --- |
| macOS game app | `ActRaiserRecomp.app/Contents/MacOS/actraiser-builder` |
| macOS Builder app | `ActRaiserRecompBuilder.app/Contents/Resources/payload/utils/tools/actraiser-builder` |
| Windows game folder | `tools\actraiser-builder.exe` |
| Legacy archive | `utils/tools/actraiser-builder` (add `.exe` on Windows) |
| Extracted Linux game AppImage | `squashfs-root/usr/bin/actraiser-builder` |

For Linux, extract a **game** AppImage into a new temporary directory to access
its helper. This leaves the original app and game data unchanged:

```sh
LANGUAGE_TOOLS_DIR=$(mktemp -d)
cd "$LANGUAGE_TOOLS_DIR"
/absolute/path/to/ActRaiserRecomp.AppImage --appimage-extract
BUILDER_CLI="$LANGUAGE_TOOLS_DIR/squashfs-root/usr/bin/actraiser-builder"
GAME_EXE=/absolute/path/to/ActRaiserRecomp.AppImage
GAME_DATA=/absolute/path/to/your/game-data-folder
```

On macOS, set the paths directly, for example:

```sh
BUILDER_CLI="/absolute/path/to/ActRaiserRecomp.app/Contents/MacOS/actraiser-builder"
GAME_EXE="/absolute/path/to/ActRaiserRecomp.app/Contents/MacOS/ActRaiserRecomp"
GAME_DATA="/absolute/path/to/your/game-data-folder"
```

`GAME_DATA` must contain the built game's fonts and `languages/native-us/`
under `game-assets/`; it is not the Builder's tool cache or the `.app` directory.
`GAME_EXE` is the game binary inside a macOS app, the Linux game AppImage, or
`ActRaiserRecomp.exe` on Windows. For Linux systems without FUSE, set
`export APPIMAGE_EXTRACT_AND_RUN=1` before font checks or packaging.

The examples below use POSIX shell syntax. In PowerShell, set the same three
variables to your Windows paths and use `& $BUILDER_CLI` in place of
`"$BUILDER_CLI"`; the command arguments are the same. Write multi-line commands
on one line instead of using the shell's `\` continuations.

For a source checkout, build the CLI with
`go -C installer build -o build/actraiser-builder ./cmd/actraiser-builder`, or
use `go -C installer run ./cmd/actraiser-builder` with absolute paths.
Ordinary relative paths resolve from your shell's current directory;
`go -C installer run` starts in `installer/` instead. Options documented as
relative to `--root` resolve from the selected game data directory.

### Check and publish

```sh
# Grammar, paths, aliases, anchors, placeholders and presentation contracts.
"$BUILDER_CLI" language validate --pack /path/to/my-pack

# Also check the actual game font stack, including omitted native-US messages.
"$BUILDER_CLI" language validate --pack /path/to/my-pack \
  --root "$GAME_DATA" --game "$GAME_EXE" --sample 'Élise'

# Publish all supplied messages after explicitly reviewing them.
"$BUILDER_CLI" language package --pack /path/to/my-pack \
  --root "$GAME_DATA" --game "$GAME_EXE" --out /path/to/my-translation.o2r --version 1.0.0 \
  --all-messages --confirm-rights

# Optional validated copy into this installation (no publishing consent needed).
"$BUILDER_CLI" mods install --output-dir "$GAME_DATA" /path/to/my-translation.o2r

# Export a fresh ROM-free machine-readable reference for external tools.
"$BUILDER_CLI" language reference --out /path/to/authoring-reference.json
```

A game build and its locally generated Native US source are required for
font checks/publication, not for plain semantic validation. JSON validation
reports identify the package and semantic counts, plus missing glyph locations
when `--game` is supplied.
Commands exit nonzero on failure; output files are not overwritten.

`--all-messages` is an explicit review action, useful for hand-authored directories
with no progress file. Without it, packaging includes Done messages and optionally
WIP with `--include-wip`. Not started messages and unchanged recorded source
templates are excluded. `--confirm-rights` is the publisher's declaration, not a
legal determination. A downloaded pack can be installed without that declaration.
`language package` accepts release versions in `MAJOR.MINOR.PATCH` form.
`language install` can also import a legacy publication.

The bundled primary font covers broad Latin/Greek/Cyrillic text, not all Unicode.
Declare and include appropriately licensed TTF/OTF dependencies for other scripts
and their notices. The interface's Japanese/Arabic/Hebrew fonts are not
automatically game-pack fallbacks. Coverage uses the same backend as play;
still visually check names, complex scripts, long labels, line breaks and
scrolling before publishing.

The validation JSON's `text_coverage` reports supplied and missing IDs by surface,
separately from review status and font coverage. A `complete` pack can still omit
optional live HUD/credits entries; inspect `liveOptional` and the per-surface
missing lists. The builder shows the same inventory during install/export review.

## Archive format

A language `.o2r` is a ZIP containing `manifest.json` with the release version,
`pack.json` with type `actraiser-language`, and `language/pack.ini` with its
declared scripts, fonts, and notices under `language/`. The Builder generates
the launcher ID and preserves the exact language ID inside `pack.ini`.

A legacy `.arlang` keeps `pack.ini` and its dependencies at the ZIP root.
Its `package.json` is:

```json
{"format":"actraiser-language-archive","version":1,"kind":"publication"}
```

See [the header JSON schema](language-archive.schema.json). Scripts/manifests use
the full format reference, not JSON. ZIP Store/Deflate and harmless directory
entries are supported. Reject executable files, symlinks, encrypted entries,
case-colliding or traversal paths, undeclared files, and over-budget expansion.
The archive limit is 256 entries (including directory entries), 256 MiB expanded
and 257 MiB compressed, with tighter per-file limits in the format guide.
Publication archives contain no `author-project.json` or progress file; explicit
`notices/*.txt` / `*.md` carry public credits and licenses. The Go packaging command
builds this container and strips private metadata for you.

## If a pack is missing or will not activate

Check the game data directory, filename extension and hidden-file status, disabled state,
duplicate IDs, and whether you restarted through the launcher. For folders check
the exact folder-ID match. Private backups and regional source archives are not
playable publications. At most 128 enabled packs are selectable; disable extras.
The game logs rejected archives/packs under its normal run logs. An unavailable
helper is reported explicitly; restore it or use an unpacked folder. Missing or
incompatible fonts reject activation while retaining a working text source.
The cache is prepared only at startup, never decompressed on text-reveal frames.
If interrupted preparation leaves a cache lock reported in the log, close the
game and builder before discarding `.arlang-cache/` and retrying. Do not remove
locks while either application is working.
