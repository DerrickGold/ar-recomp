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
4. Use **Manage installed packs** to toggle packs on or off without deleting them.
5. Restart the game and select the enabled package in the system overlay's
   Localization settings.

Private `.arproject` backups and compatible legacy ZIPs remain under **Author
backup or legacy archive** in the import workflow. Unpacked authoring folders
are available under **Advanced: unpacked language folder**.

### Manual installation in a legacy bundle

For older bundles with a `utils/` directory and `run-game` scripts:

1. Close the game.
2. Copy the `.arlang` file into `utils/game-assets/languages/packs/` in your
   downloaded game folder. Create `packs/` if needed. **Do not unzip it.**
3. Start the game with `run-game.command`, `run-game.sh` or `run-game.bat`.
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

The game prepares archives through the retained
`utils/tools/actraiser-builder` utility;
it never opens the editor. Keep that executable when moving/slimming a bundle.
The workshop's build-tool cleanup already retains it. A source CMake build
with Go available builds the helper beside the game executable. Other ports
can provide their own archive adapter; unpacked folders need no helper.

## Where files belong

| Artifact | Purpose and location |
| --- | --- |
| `.arlang` | Reviewed publication; put directly in `utils/game-assets/languages/packs/`. |
| Unpacked pack | `utils/game-assets/languages/packs/<exact-package-id>/pack.ini` plus its declared files. |
| Native US source | Generated locally by the game build under `utils/game-assets/languages/native-us/`; retain it for fallback. |
| `.arproject` / `projects/` | Private workshop backups, source templates and progress; not an installed game translation. |
| `.arlang-cache/` | Disposable prepared archive snapshots inside `packs/`; never publish or edit them. |
| `.arlang-state/` | Archive enable/disable preferences inside `packs/`, keyed by package ID. |

In a source checkout the game working directory is normally the repository root:
use `game-assets/languages/packs/` without the `utils/` prefix. In a distribution
the launcher sets `utils/` as the working directory; placing assets beside the
game executable instead is incorrect.

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
archives as well as folders. Alternatively, from the distribution root:

```sh
utils/tools/actraiser-builder language disable --root utils --installed my-pack.arlang
utils/tools/actraiser-builder language enable --root utils --installed my-pack.arlang
utils/tools/actraiser-builder language uninstall --root utils --installed my-pack.arlang
```

Windows uses `utils\tools\actraiser-builder.exe`; use the same arguments.
`--installed`
names the file or folder **as it appears in `packs/`**, not its display name.
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

Examples live under `utils/examples/` in a distribution and under `examples/`
in a source checkout.

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

The following commands run from the distribution root. In a checkout build
`installer/build/actraiser-builder`, or use
`go -C installer run ./cmd/actraiser-builder` from the repository root with
absolute input/output paths. A built executable resolves ordinary relative
paths from your shell's current directory; `go -C installer run` starts in the
`installer/` module instead. Options documented as relative to `--root` still
resolve from the selected game assets/fallback directory.

```sh
# Grammar, paths, aliases, anchors, placeholders and presentation contracts.
utils/tools/actraiser-builder language validate --pack /path/to/my-pack

# Also check the actual game font stack, including omitted native-US messages.
utils/tools/actraiser-builder language validate --pack /path/to/my-pack \
  --root utils --game ./ActRaiserRecomp --sample 'Élise'

# Publish all supplied messages after explicitly reviewing them.
utils/tools/actraiser-builder language package --pack /path/to/my-pack \
  --root utils --game ./ActRaiserRecomp --out my-translation.arlang \
  --all-messages --confirm-rights

# Optional validated copy into this installation (no publishing consent needed).
utils/tools/actraiser-builder language install --root utils --pack my-translation.arlang

# Export a fresh ROM-free machine-readable reference for external tools.
utils/tools/actraiser-builder language reference --out authoring-reference.json
```

On Windows use `--game .\ActRaiserRecomp.exe`. A game build and its locally
generated Native US source are required for font checks/publication, not for
plain semantic validation. JSON validation reports identify the package and
semantic counts, plus missing glyph locations when `--game` is supplied.
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
automatically game-pack fallbacks. Coverage uses the same backend as play; still visually check names,
complex scripts, long labels, line breaks and scrolling before publishing.

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

Check the `utils/` path, filename extension and hidden-file status, disabled state,
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
