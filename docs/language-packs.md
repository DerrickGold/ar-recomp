# Install, create and share language packs

Language packs are data, not plugins or executable code. You do not need the
language editor to install one, write a translation, or publish your work.
This guide is the short workflow; [the format reference](language-pack-format.md)
defines the complete script syntax and game constraints.

## Install a shared `.arlang` file

The recommended Workshop flow is the same on macOS, Linux and Windows:

1. Build the game, then close it before changing its installed languages.
2. Open **Languages → Add a language pack** and choose the `.arlang` file, or
   drop it into the Workshop. Add one package at a time; **do not unzip it**.
3. Review its language, credits and any conflicts, then choose **Import & install**.
   Replacing a saved project or installed pack requires explicit confirmation.
4. Use the installed-pack checklist in **Languages** to toggle packs on or off
   without deleting them.
5. Restart the game and select the enabled package in the system overlay's
   Localization settings.

Private `.arproject` backups and compatible legacy ZIPs remain under **Author
backup or legacy archive** in the import workflow. Unpacked authoring folders
are available under **Advanced: unpacked language folder**.

### Manual installation

Use the game's writable data directory, not the inside of an application bundle:

1. Close the game.
2. Copy the `.arlang` file into `game-assets/languages/packs/` under that
   directory. Create `packs/` if needed. **Do not unzip it.**
3. Start the game normally. Older `utils/` bundles still use their `run-game`
   script.
4. Open the system overlay → Localization → Game text and select its package name.

The filename can contain spaces and does not have to match the package name,
ID or locale. Avoid filenames starting with a dot: hidden entries are ignored.
Discovery is at startup, not live file watching. Already discovered languages
can be switched during play. External packs automatically use enhanced fonts;
Native US still offers original rendering or enhanced rendering of US text.

Multiple translations of the same language are supported and sorted by package
name. Identity comes from `id` inside the manifest, not the locale. Two enabled
copies of the same ID conflict; neither should be selected until the duplicate
is removed or disabled. This also applies to a folder and archive with the same
ID. Do not keep an older `.arlang` beside its replacement under a different name.

The game prepares archives through its bundled `actraiser-builder` helper;
it never opens the editor. Native game apps carry the helper inside the app;
folder outputs retain it under `tools/` (`utils/tools/` in older bundles).
Keep the helper when moving or slimming a folder installation. The Workshop's
build-tool cleanup already retains it. A source CMake build
with Go available builds the helper beside the game executable. Other ports
can provide their own archive adapter; unpacked folders need no helper.

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
| `.arlang` | Reviewed publication; put directly in `game-assets/languages/packs/`. |
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

Close the game before manually replacing/removing files. Replace an archive in
place with its new version. A changed archive gets a new cache snapshot;
previous snapshots remain intact for running processes. Disabled archive IDs
remain disabled after replacement or filename changes.

The builder's Languages checklist can enable/disable or recoverably uninstall
archives as well as folders. Alternatively, use the helper and data directory
described under [command-line setup](#command-line-setup):

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

Workshop draft installations stay unpacked because they may include unfinished
or unchanged source text. To replace an archive with a workshop draft, uninstall
the archive first. To distribute an update, export a reviewed `.arlang` and
replace the archive instead. Installing someone else's publication does not
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
  [example publication](../examples/example.fr-ca.arlang): two independently authored
  French example messages, not a full translation or an extracted retail script.
- Your own locally extracted US source script for the messages you are translating.
  Follow `[scripts] source` paths in `native-us/pack.ini`; do not edit that baseline.

The links above are available without a source build. Legacy archives include
them under `utils/docs/` and `utils/examples/`; the macOS Builder carries the
same files under `Contents/Resources/payload/utils/`. Do not assume a generated
game app includes the authoring examples.

Create a separate directory, copy the example layout, and choose your own stable
ID, locale, name, authorship and license. Use `target = us-runtime`,
`source_profile = us`, `fallback = native-us`, and usually `coverage = partial`.
Omit untranslated messages so the game uses its local US fallback. If you use an
alias, its target must also be included in the same pack. Regional extracts are
reference-only: use US route contracts even when translating into Japanese.

A useful instruction to give an AI is:

> Produce UTF-8 `.artext` files and `pack.ini` following the supplied format and
> US authoring reference. Translate only the requested message IDs. Preserve
> every required `@anchor` exactly and in order. Never invent IDs, placeholders
> or events. Keep fixed-menu cell/row order and keyboard geometry. Use `@line`
> for intentional breaks; physical newlines alone are wrappable. Added dialogue
> pages must precede a terminal `yield.*` anchor. Omit untranslated messages.
> Preserve contributor credits. Do not include private notes or source templates.

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
  --root "$GAME_DATA" --game "$GAME_EXE" --out /path/to/my-translation.arlang \
  --all-messages --confirm-rights

# Optional validated copy into this installation (no publishing consent needed).
"$BUILDER_CLI" language install --root "$GAME_DATA" --pack /path/to/my-translation.arlang

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
`language install --replace` updates a same-ID archive, retaining disabled state;
it will not silently replace an unpacked folder.

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

`.arlang` is a ZIP with `pack.ini` and dependencies at its root, not inside an
extra enclosing folder. Its `package.json` is:

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
