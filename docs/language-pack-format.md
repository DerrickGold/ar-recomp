# Language pack authoring format

For direct `.arlang` installation and authoring with an editor or AI, start with
[Install, create and share language packs](language-packs.md). The accompanying
[machine-readable route reference](language-authoring-reference.json) supplies
the US contract for every message, including anchors and allowed placeholders.

ActRaiser language packs are UTF-8 directories designed for ordinary text
editors and the builder's Languages workspace. A pack contains no ROM
addresses, dictionary tokens, or font-tile numbers. Version 1 deliberately
starts fresh; the unreleased prototype format is not supported.

The game applies packs to simulation-mode and Sky Palace menus/dialogue/HUD, action
HUD labels and counters, action stage cards and pause/stage messages, and the
US title-screen options. Ending-tour dialogue uses the ordinary dialogue
integration; credit pages support editable text while logo/copyright artwork remains native,
as do regional-only menu flows. The dormant native sound-test composer also
supports enhanced text when invoked by debugging tools; this does not enable
a new sound-test menu in normal play. The semantic catalog
covers the whole game so later integrations can use the same packs.

In Western source packs, `dialogue.ending.slot_00` through `slot_07` hold the
ending messages. Their live wrapper routes are emitted as aliases to those
editable bodies, so editing an ending message updates its linked uses. The
Japanese reference exposes the equivalent messages through
`dialogue.event.wrapper_00.call_00.source_00` through `source_06`, the Sky Palace
introduction `dialogue.event.wrapper_05.call_03.source_00`, and the empty-temple
message `dialogue.event.wrapper_05.call_06.source_00`. An author can deliberately
replace an alias with an independent body to vary a particular use. Keep each
message's locked control anchors: they preserve the native delays and exits.

Credits appear under **End Credits**. `credits.page_00`–`credits.page_14`
are staff pages; `credits.the_end`, `credits.best_player` and `credits.game_over`
are the final cards. Each accepts up to six explicit lines (`@line`) on one
page; `@page` is not supported because native code owns the page sequence and
timing. Lines are centered and fitted independently on a fixed vertical pitch;
the heading's first grapheme retains the native gold accent. `@empty` clears
the entire text page. Missing entries fall back to Native US, and a rendering
failure retains the whole native page, never a partially erased staff list.
Changing language while a credit page is visible retains its native timing.
The build/extraction step adds missing credits to older Native US baselines
without replacing existing edits or progress. Community packs are not modified.
Regional references preserve their original contributor lists, line breaks and
spelling; they are not silently corrected. Japan's extra `credits.special_mode`
is dormant reference text, not an additional page enabled in the US game.
US copyright pages 15/16 and JP page 15 remain artwork-only, available in the
graphics archive rather than as editable translations.

`sound_test.menu.labels` is a fixed five-line grid (including the blank lines)
with live `{sound_music_id:02}` and `{sound_effect_id:02}` values. Separate each
counter from its label with ` | ` to keep both counters in the same fixed
column regardless of label width. Older single-field rows remain supported.
Its native
origin and two-row line cells are game-owned; labels fit within ten native
columns. It has no separate selector sprite. Closing the native modal releases
the enhanced surface, including when its translation was unavailable.

Action labels use `action.hud.act_label`, `time_label`, `score_label`,
`player_label` and `enemy_label` (each with the `action.hud.` prefix).
They are optional additions to the original complete-pack contract. New source
extractions include them; an older community pack falls back to Native US for
missing labels. Lives, timer and score remain game-owned numbers, not editable
script literals. Enhanced counters retain their slanted appearance and labels
use the game's palette rather than the dialogue palette.

Simulation/Sky Palace HUD labels use `sim_sky.hud.context_label`,
`sim_sky.hud.angel_label`, and `sim_sky.hud.sp_label`. These are also optional
additions with Native US fallback in older community packs. Population and
current/maximum SP remain game-owned values. Western reference extractions
include all three labels; the Japanese reference currently includes only the
angel label. Its different CRT/AREA context layout is not interchangeable with
the US field. A translation targeting the US game can author all three US
fields regardless of the language of its reference ROM.

Title `title.save_choice.labels` takes two nonempty lines: Continue and New
game, in that order. Blank padding rows are ignored; labels stay beside the
native selector. `title.start_prompt` and `title.selector.professional` supply
the other US options. These are fitted fields, as are action cards/messages;
extra authored pages are not an extension of a fixed menu and are rejected —
see "What each route displays".
Translating a regional reference's different mode/difficulty menu does not
automatically replace the US title choices—edit the US semantic routes.

## Author backups and sharing archives

The builder uses `.arproject` for private, resumable author backups and
`.arlang` for publishable translations. Both are bounded ZIP archives with a
root `package.json` identifying the format, version and archive kind, and the
same human-editable `pack.ini` and script files described below.

Backups retain progress, comments, notes and the local source template. Do not
publish ROM-derived backups. A publication contains reviewed translations
(Done, optionally WIP), declared font dependencies and explicit `notices/`
license files. It omits private notes, progress and unchanged source-template
messages; omitted routes use the game's native fallback. Publication requires
the author's explicit confirmation of redistribution rights. This is not a
legal determination or a substitute for reviewing your content and licenses.

Import validates paths, sizes, manifest dependencies and game semantic contracts
before accepting a project. Project saves reject stale revisions and conflicting
IDs. Runtime installation stages a complete immutable version before replacing
its manifest; existing versions are retained for recovery. A directory import
reads only declared scripts/fonts and the defined progress, author metadata and
notice files, not unrelated files in that directory.

## Current runtime controls

The system overlay's **Localization** section separates **Game text** from
**Enhanced font** settings. Native rendering always uses the unchanged U.S.
text and font. Selecting any external pack requires enhanced rendering, even
an alternative English pack; selecting Native US restores the native/enhanced
choice. Enhanced rendering uses the chosen text source and supports
80–140% font size, Crisp/Smooth sampling, and None/Low resolution/Mosaic
pixelation with strengths 2, 4, 6, or 8. Defaults are 140%, Crisp, and Mosaic 2.
Choose Pixelation = None for the full-resolution font without a pixelation
effect. Existing saved font preferences are retained when defaults change.
Pixelation strength is an upper bound: small windows and tightly fitted text
automatically use finer blocks to avoid reducing characters to unreadable dots.
These settings can be changed during play and are saved with other settings.

The **Interface** tab selects English, French, German or Japanese for the system
menu, independently of game text and the builder's interface preference.
Setting labels, descriptions, choices and controller-binding captions are
translated, as are the manual reader's page counters and control hints (not
the scanned manual artwork). Saved setting keys/values, package names, player names and
platform-provided keycap/device names retain their original identity.

The system interface owns a separate font stack, not your selected pack's
fonts. Non-ASCII package names use cached, shaped Noto Sans with bundled Japanese,
Arabic and Hebrew fallbacks at output resolution; ordinary English interface lettering keeps the native
bitmap appearance. Ports without the enhanced backend retain the safe bitmap
fallback. This does not make a game pack's missing glyphs available: declare
the dependencies needed by its actual game text in the pack's Fonts tab.

The Go builder prepares the local Native US source first in both its CMake-backed
and hermetic build flows, preserving existing source messages. Older native
baselines gain missing graphical HUD labels and, when a ROM is available,
credit text through an atomic update;
previous source files and translation progress remain intact. Community packs
are never automatically rewritten. Its runtime path
is `game-assets/languages/native-us/pack.ini`. The workshop's Languages section
and Home shortcuts require this complete, valid source: start a build with your
US ROM and they unlock automatically after extraction, before the rest of the
build finishes. A later build failure does not relock the editor. A pre-existing
valid source is recognized on opening the workshop, without requiring the ROM.
Incomplete or invalid sources remain locked with an explanation; saved projects
are not removed. Regional references can then be extracted within Languages.

Both build flows and the GUI use the same Go extraction and authoring library.
The browser sends edits to that library; it has no separate ROM decoder or pack
parser. Game-side C compatibility is tested directly against Go output. Python
is optional development tooling, not a builder, editor, or runtime dependency.

Enhanced-text builds require SDL3_ttf as well as SDL3. Supported standalone
bundles carry the font-rendering SDK and preserve its license notices under
`utils/licenses/SDL3_ttf`, even when build tools are removed. Generic Linux
installers also include matching pinned headers and libraries; only source
checkouts use system development packages. The builder checks for missing
headers/link libraries and known wrong-architecture binaries before compiling.

Open **Languages** in the builder sidebar. **Add a language pack** accepts a
shared `.arlang` file directly above the installed library. You can also drop
one `.arlang` at a time into the Workshop from another sidebar section; this
opens Languages and a review, not an automatic installation. Build the game
first to make language tools available. **Install a language pack** and
**Create a translation** offer the guided workflows; **Open a project or
backup**, **Clone a language pack**, and **Extract a ROM reference** are below
them. Choose a workflow to see only its relevant steps. The package library
combines editable projects and installed packs, is sorted by package name, and
can be searched by name, locale or ID. Filter to installed packages or workshop
projects that are not installed. Home also
offers shortcuts to up to four saved projects (name order, not recency).
Opening a project or saving edits changes the workshop project only;
it does **not** install or update the game's copy. **Import & install** explicitly
does both. Cloning creates a new stable
package ID, retaining the original contributor credits, progress and content.
Switching sidebar sections preserves unsaved message text and asset selections;
closing the workshop or changing projects asks before discarding unsaved work.
This is not autosave: save messages and details before closing the Workshop.

The default tree follows playthrough order: Title, Sky Palace, Fillmore,
Bloodpool, Kasandora, Aitos, Marahna, Northwall, Death Heim, End Credits.
Each location expands into categories such as story dialogue, menus, miracles,
reports and save prompts. **[shared]** links in multiple towns refer to one
message and one progress status, not separate translations. Overall progress
counts each semantic ID once; location totals include their shared links.
Unlocated dormant/regional data stays reachable under Sky Palace → Additional
regional & table references, explicitly marked as reference-only. Technical
message-ID navigation remains available from **Organize by**.

The dialogue after Continue is under **Sky Palace → Introduction, name entry &
Continue → Resume saved game — welcome back after Continue**. Search checks
translation text, friendly labels, technical IDs and
your local source reference, even for messages absent from the current pack.
The extracted native US reference is selected automatically when available.
Mark messages Not started, WIP or Done as you work. The sticky **Save progress**
button (also Cmd+S / Ctrl+S) saves pending message text/status, metadata, private
notes and public notices in one validated project save. Invalid input leaves
the entire pending change unsaved, with an error beside the Save action.
The **Messages** and **Details & credits** tabs keep the working area compact
without discarding pending edits. **Languages** returns to the package library and warns before
discarding unsaved work.
Route-specific value pickers insert supported placeholders; the shared Go
validator checks edits and their native control anchors before saving. Native
sources are read-only: create a translation from US, and use another extracted
release as a reference. **Reference language** and **Extract reference ROM…**
remain available inside the editor; changing or extracting a reference does
not discard drafts or switch away from the current project. Source and edited
scripts are side by side on wide screens, stacked on narrower screens. The
source heading identifies the reference actually used, including native US
fallback when needed. Identical extracted references can be reused without
overwriting projects. The tree includes cross-release/future-phase routes,
so its total is larger than the number of messages native to one ROM.

Open **Pack actions** for **Download private backup**, or **Export for sharing** →
**Export language pack** for publication. These prepare an explicit download
link. **Install a language pack** defaults to **Import new pack**, with an
`.arlang` file picker and drag-and-drop; no extraction is necessary. **Install
already imported** remains available for saved projects. **Author backup or
legacy archive** accepts `.arproject` and compatible `.zip` files. **Advanced:
unpacked language folder** retains the native folder chooser and manual path.
Selecting or dropping a source does not change the
workshop or game until confirmed. Imports support a distinct side-by-side ID or
explicit replacement of the project with the incoming ID, never an unrelated
open project. **Import & install**, or **Pack actions → Install in game** →
**Install for this game**, installs all supplied translations under
`game-assets/languages/packs/<package-id>/pack.ini`. Restart the game to refresh
the installed catalog, then select the package by name in the system overlay.
Local installation includes every supplied message regardless of review status;
it neither asks for redistribution rights nor re-filters a publisher's content.
Publication remains separate: the publisher confirms rights and chooses whether
to include WIP, while Not started and unchanged source templates are omitted.
Multiple packs with the same locale or name remain distinct by their stable ID;
selection survives package-name changes and catalog reordering. Selecting an
external pack forces enhanced rendering; only Native US offers the original
text/font mode.

After installation, **Manage installed packs** returns to the library. Its
checkboxes enable or disable packs without deleting translations or authoring
progress. Updating a disabled pack keeps it disabled. Restart the game after
changing the installed catalog, then select an enabled pack in the game.

Players can also copy a published `.arlang` directly into that `packs/` directory.
The game prepares it on startup through the same Go archive reader, with no GUI.
The archive filename is independent of its ID; unpacked directory names must
exactly match their manifest ID. Conflicting enabled IDs are unavailable until
resolved, including folder/archive duplicates. The complete manual workflow,
CLI commands and cache locations are in the [quickstart](language-packs.md).

The library defaults to an installed-package checklist. Enable/disable choices
save immediately; restart the game to apply them. Unchecking a package keeps
its files and project but hides it from the runtime selector by renaming
`pack.ini` to `disabled-pack.ini`. Checking it validates its declared dependencies
and restores `pack.ini`. Only one of these manifests may exist at a time.
Updating a disabled pack preserves its disabled state. Original US text remains
available independently; the game uses one selected language, not a merged stack.

For archives the checklist records disabled IDs in `.arlang-state/` instead of
modifying the archive. Archive uninstall moves the file to `.uninstalled/` for
recovery. Both directories are inside `packs/`; the game ignores them during
ordinary discovery. Prepared archive snapshots in `.arlang-cache/` are retained
until explicitly discarded with the game closed.

The library's **Uninstall…** action removes an enabled or disabled package from discovery
after confirmation; restart the game afterward. It never deletes the editable
project or the native US source. Installed data and a renamed `uninstalled-*.ini`
manifest are retained in the package folder for recovery, so uninstall does not
reclaim disk space. Review and install again to restore the pack. Concurrent
replacement or a changed manifest requires a library refresh before removal.

Paths are relative to the builder's explicit project root, which is `utils/` in
a distribution—not the executable directory. Resumable projects live under
`game-assets/languages/projects/`. Extracted retail scripts and regional ROMs
are local-only, not bundled. The optional development `Configured pack` source
still accepts `AR_LOCALIZATION_PACK`; it does not replace installed discovery.

The editor's script preview shows logical pages and controls, not exact in-game
wrapping, font size, mosaic, scroll timing or keyboard geometry. It does not
certify shaping or final layout. The separate Fonts tab checks scalar coverage
through the game backend; test appearance in game before publishing. Directory
imports and archives retain declared fonts; new translations use the bundled
font. A missing or incompatible selected pack retains the prior working source.

Install/publication review also reports text coverage by dialogue, menus,
keyboard, shared terms, HUD and credits. Expand a group for missing IDs and
separate author progress counts. **Supplied** includes aliases and intentional
`@empty` entries; it does not certify translation or review quality. Known
unchanged source templates are counted separately. Publication coverage uses
the actual filtered export, retaining the author's WIP status in the report.
Changing publication options invalidates that report until checked again.

`actraiser-builder language validate --pack <path>` provides the same inventory in
`text_coverage`, independently of optional `font_coverage`. `required` describes
contract completeness; `liveOptional` identifies the 26 audited live US HUD/
credits extras. `surfaces[].missing` lists native-fallback IDs. Groups include
shared/reference contract entries and are not an execution/screen census.
The reference's `us_runtime_usage` distinguishes `contract`, `live_optional`,
`dormant` and `regional_reference`; it does not change runtime dispatch or pack
syntax. The sound-test menu and special-mode credits are listed as dormant US
entries, not ordinary gameplay. This inventory contains IDs/counts, not ROM text.

Translation and reference panes use their own content language, independently
of the workshop interface language. Raw script lines resolve direction one line
at a time so ASCII directives such as `@anchor` remain readable alongside RTL
prose. The logical preview uses the translation's `direction` setting and keeps
control IDs/value placeholders isolated left-to-right. These are display rules:
the editor does not insert hidden bidi characters or reverse saved text. Browser
caret behavior and the game's mixed-script appearance still need RTL-specific
qualification; the logical preview is not proof of in-game bidi correctness.

Authorship is stored in `author` and `license`. Preserve original contributor
credits when adapting a pack, add your contributions, and use public
`notices/CREDITS.txt` for longer attribution. Those fields/notices survive
publication, import, editing and installation. Private working notes are kept
only in backups. Metadata records attribution, not verified authorship or a
cryptographic revision history.

Before enabling enhanced text, the game tests opening the primary and ordered
fallback fonts and uploading a rasterized sample. Missing/corrupt font files or
an unavailable renderer reject the switch without adding dialogue pages or waits;
the prior selection stays active. Declared fallback fonts are used by the live
renderer. This readiness test does not verify every character in a translation:
include appropriately licensed fonts covering your language, run the coverage
check below, and preview its text.

If a dialogue later cannot be rendered, its remaining text and interaction fall
back to native for that invocation. Added-only waits/prompts retire; native
confirmations and game events remain player/game-controlled. Your pack selection
is retained, and the next message can use enhanced text again.

Scoped dialogue supports adding/removing `@page` boundaries and optional
`@wait` pauses. The game confirms authored pages before passing native control
barriers; removed pages do not leave redundant continuation prompts. Keep all
locked anchors in order, and place added content **before** a final `yield.*`
anchor, which hands input to the native menu. Content after it is rejected as
unreachable. Live source changes preserve the current semantic progress: shorter
messages clamp to a valid page, while additional pages remain reachable before
unexecuted native control barriers. Switching text rendering during a prompt
does not answer a native choice or confirm a native continuation. Already
completed terminal/menu-yield states remain complete. Changing font styling
does not restart typing. Installing a new pack requires a restart to refresh
the catalog; switching between already discovered packs does not.
Debug save-state restoration
is unsupported and is not a language-pack compatibility requirement. This does
not change normal battery saves or the intended live language-switching behavior.
`@empty` does work for scoped dialogue and ordinary fixed-menu text: it hides
the wording while keeping native choices, waits, boxes, and other artwork.
It does not skip a game event or automatically answer a prompt. Name-entry
keyboards still require their valid interactive grid and cannot be blanked.

## Checking font coverage

Open an editable project in **Languages → Fonts**. Choose a primary font and
up to eight ordered fallbacks; use **Move up**, **Move down**, and **Remove** to
adjust the stack. Choose TTF/OTF files (up to 64 MiB each) or reuse a dependency
already in the project. **Save progress** copies new files into the project in
the same transaction as pending message, metadata and notice edits. Removing
a dependency does not delete its original file or its notices. Native reference
projects are read-only: create a translation first.

**Check coverage with game font backend** checks the saved project, plus native
US routes omitted from it. With pending changes the button becomes **Save & check
coverage**. A report lists missing code points with script/message/line locations
and unresolved live placeholders. Use the optional sample for a player name.
The report identifies the content and native-fallback revisions and SHA-256
digests of the exact primary/fallback bytes checked. It shows at most 256 missing
code points and four locations each, while retaining the total missing count.

Install and export run the same check before writing their candidate pack.
Export checks its filtered publication, not unfinished text excluded from it.
A missing/corrupt font, missing glyph, unavailable native source or unavailable
game backend blocks installation/export without changing the prior installation.
Saving work and downloading private backups do not require coverage to pass.
Complete a game build first; the installed builder invokes that trusted game
executable, never an executable supplied by a pack. No Python is required.

The game also warns on its standard error when a font stack has no glyph for a
character encountered during play, naming the stack and the code point. Add a
suitable dependency in the Fonts tab or in the source pack's `pack.ini`.

Place an appropriately licensed TTF/OTF file inside the source pack directory
(for example, `fonts/MyFallback.ttf`), then add
`fallback = fonts/MyFallback.ttf` under `[fonts]` in `pack.ini`. Fallback rows
are ordered and may repeat for different font files. Include any required
license notices when sharing. Reimport the updated source directory through
Languages, explicitly replacing the same-ID workshop project/installation when
prompted; save or back up existing workshop edits first. Restart the game after
installing. Do not edit files inside an installed immutable version directory.

The shipped headless host protocol is `ActRaiserRecomp --font-coverage-v1 FONT
[FALLBACK ...]`. It reads one hexadecimal Unicode scalar per stdin line and
writes `HEX<TAB>0/1` per input line in order. Exit 0 means the query completed
(inspect the 0/1 results); exit 2 means invalid input or an unavailable backend.
It opens no ROM, window, settings or save file. The builder supplies immutable
private font snapshots and bounds input, output and run time. This adapter uses
the same SDL3_ttf primary/fallback implementation as live text rendering.

The checker below is a **development tool** and is not part of a released
build. From a source checkout configured with SDL3_ttf enhanced-text support,
build it and run it against your pack directory:

```sh
cmake --build build --target actraiser_font_coverage
python3 tools/check_language_fonts.py --pack path/to/my-language-pack \
  --sample 'Élise' --out font-coverage.json
```

The helper is also available with `-DAR_TESTS_ONLY=ON`, so configuring a
ROM-free checkout does not require generated game code.

This uses the same primary/ordered fallback font backend as the game, without
opening a window, loading a ROM, or modifying the pack. `--probe` selects a
helper built elsewhere; `--builtin-font` supplies the bundled primary font's
path for a relocated setup. Repeated `--sample` arguments test possible player
names or other dynamic values.

The JSON report identifies missing Unicode code points by script, message ID,
and source line, and records the tested font paths and SHA-256 hashes. Aliased
text is checked at its literal definition. Comments, command names and placeholder
names are not text; unresolved dynamic placeholders are listed separately.
Exit status is 0 for complete scalar coverage, 1 for missing glyphs, and 2 for
invalid input or an unavailable font/backend. This is separate from pack semantic
validation and can also check reference-only regional extracts.

Scalar coverage gates builder install/export; it is not proof of correct shaping,
ligatures, emoji sequences, or layout. Layout controls, typed inline objects, and Unicode default-ignorable
characters do not need standalone glyphs. Combining accents and ordinary spaces
are checked. Always visually review complex scripts and real dynamic values.

During play, a missing glyph logs a warning without changing dialogue progression.
Warnings are deduplicated and capped at 64 distinct characters per active font
stack, with one suppression notice after that. Coverage results are cached and
checked only when text is rasterized, not on cached menu or reveal frames.
Use the authoring report to find the corresponding source locations.

## Directory layout

```text
my-language-pack/
  pack.ini
  translation-progress.tsv
  text/
    sky-palace.artext
    simulation.artext
  fonts/
    OptionalFallback.ttf
```

`translation-progress.tsv` is editor metadata. The runtime ignores it. Font
files and authored scripts need redistribution terms appropriate for the pack.
Do not distribute scripts, fonts, or graphics extracted from a retail ROM.

## Manifest

`pack.ini` uses a small, strict INI subset:

```ini
[pack]
format = actraiser-language-pack
version = 1
id = example.fr-ca
locale = fr-CA
name = Canadian French
autonym = Français canadien
author = Example Author
license = CC-BY-4.0
direction = auto
target = us-runtime
source_profile = us
fallback = native-us
coverage = partial

[fonts]
primary = builtin:actraiser-sans
fallback = fonts/OptionalFallback.ttf

[scripts]
source = text/sky-palace.artext
source = text/simulation.artext
```

- `id` is the stable package identity. Locale is metadata, so several packages
  can target the same locale. Discovery sorts by package name and then package
  ID, independently of locale or autonym.
  For manual folder installation, use this exact ID as the directory name.
- `locale` is a BCP-47 language tag such as `en-CA`, `fr-FR`, or `ja-JP`.
- `name` is the package name; `autonym` is the language's own display name.
- `direction` is `ltr`, `rtl`, or `auto`. This selects the paragraph base,
  not the order in which you write the script. Always store normal logical
  Unicode text; never reverse Arabic/Hebrew text or numbers manually. The
  enhanced backend resolves mixed-direction runs before shaping. RTL packs
  remain experimental end to end: validate mixed names/numbers, partial-pack
  fallback, editor behavior and package-name display, not just glyph coverage.
  Inserted names/text are automatically isolated using their own first strong
  character; numbers are isolated LTR and localized terms use the term's source
  direction. Do not add direction controls around placeholders to implement
  this: isolation is private to rendering and leaves script/reveal offsets intact.
- Player-selectable packs use `target = us-runtime` and
  `source_profile = us`. A local extract from another official ROM uses
  `target = reference-only`; it cannot accidentally replace the U.S. execution
  baseline.
- `fallback` is fixed to `native-us` in version 1. Missing or rejected messages
  therefore fall back to the locally extracted U.S. enhanced source and then
  to untouched native ROM rendering.
  Enhanced fallback uses the effective message source's locale and direction,
  while retaining the selected pack's font stack. An English fallback message
  does not inherit `rtl` from a partial translation. This applies to dialogue,
  fixed menus/labels, HUD text and credits; native numeric HUD fields retain
  their U.S. formatting and physical placement.
- `coverage = partial` permits per-message fallback. `complete` requires every
  **required** catalog message in the declared source profile, not optional
  routes and not every visible word. The public reference marks these with
  `required_for_complete`. The unchanged US contract has 495 required entries;
  eight additional live HUD labels and eighteen live credits entries remain
  optional. Omitting them requests native fallback even in a complete pack.
- `source` and fallback-font rows may repeat for distinct references. Paths
  must be UTF-8, pack-relative, and at most 511 bytes. Use `/` separators;
  absolute paths, backslashes, empty/`.`/`..` components, control characters,
  and `< > : " | ? *` are rejected. Components must not end in a dot or space,
  or use Windows device names such as `NUL`, `CON`, or `COM1`, even with an
  extension. `builtin:` font identifiers are not filesystem paths.
- Keep referenced filenames distinct without relying on letter case, and do
  not use the same path for different kinds of file (for example, a script and
  a font). The builder's pack reader rejects conflicting paths, symlinks and
  nonregular files. It reads only declared scripts/fonts and optional
  `translation-progress.tsv` from the selected pack root.
  `package.json`, `author-project.json`, and the `notices/` directory are
  reserved for archive metadata and notices, not script/font dependencies.
  Author imports and font edits accept TTF/OTF envelopes only; actual font
  parsing and glyph coverage belong to the game backend.

The builder's pack reader limits a manifest to 256 KiB, each script or progress
file to 16 MiB, each local font to 64 MiB, and the combined snapshot to 256 MiB.
These are file-loading safeguards, not a guarantee that a pack fits every
runtime font or layout budget.

Unknown sections, keys, values, commands, and duplicate IDs are errors. A
failed load never partially activates a pack.

## Script basics

An `.artext` message starts with `::` and its generated semantic ID:

```text
:: sky.menu.fight_monsters
Affronter les monstres
@end

:: sky.action_mode.confirm
@anchor reset_text_cursor.00
{master_name}, souhaitez-vous commencer ?
@page
Cette page supplémentaire est validée indépendamment du nombre de pages natif.
@anchor yield.01
@end
```

Ordinary physical lines in one paragraph are joined with wrappable whitespace.
A blank source line starts a paragraph. Source extraction compares each native
dialogue line plus the next complete word with the region's native cell width.
If the word fits (including an exact fit), the exporter keeps the break as
`@preferred-line`; if it overflows, the exporter makes that break wrappable
whitespace. At runtime, a preferred break is kept only when the preceding
segment still fits on one line in the selected proportional font and projected
output box. If that segment has already wrapped, it becomes ordinary word
spacing. This preserves short greetings and similar deliberate-looking lines
without stranding a later short word after an earlier enhanced-font reflow.

This is an inference, not proof of the original author's intent: review the
exported `@preferred-line` commands and promote them to `@line`, or replace them
with ordinary spacing, as appropriate. Unknown dynamic
widths (such as the player name), missing geometry, Japanese text without
reliable space-delimited words, and unprofiled ending layouts preserve their
breaks conservatively. Fixed menu/table rows are always kept. This conversion
only happens during export; loading an edited pack never reinterprets `@line`.
Existing packs must be re-extracted or edited to gain these breaks; back up
translations before replacing generated files.

Native menu records also contain runs of blank cells used to erase previous
tile contents. Extraction omits an otherwise blank inline run, but preserves
its `@line` positions and all spaces adjoining actual text or placeholders.
Those native clearing cells are not paragraphs to translate. An entirely
textless source uses `@empty` with any required native anchors.

The supported commands are:

| Command | Meaning |
| --- | --- |
| `@line` | Intentional hard line break. |
| `@preferred-line` | Flowing dialogue only: retain the native row break if the preceding proportional segment still fits on one line; otherwise treat it as a space. |
| blank line or `@paragraph` | Paragraph break. |
| `@page` | Authored page break. Pages may be added, removed, or reordered. |
| `@wait N` | Optional presentation delay of 1–600 frames. |
| `@anchor ID` | Machine-owned native control position; do not edit it. |
| `@event ID ...` | Allow-listed semantic event. Version 1 initially has no author events enabled. |
| `@empty` | Intentionally show no text. Required anchors remain beside it. |
| `@alias ID` | Reuse another included message with a compatible contract. |
| `@end` | Explicit end; it is added implicitly if omitted. |

Use `@@` at the beginning of a text line to display a literal `@`. Use `\#` or
`\;` for a literal leading comment marker. Double braces (`{{` and `}}`) emit
literal braces.

Physical source lines use LF, CRLF or CR separators. Nonbreaking spaces and
Unicode line/paragraph-separator characters remain text; use `@line`, `@page`
or a blank physical line for explicit authoring boundaries. Quoting a command
argument does not relax its identifier rules; do not escape punctuation inside
quoted anchor IDs.

## Locked anchors and safe extension

The extractor converts native cursor resets, text-state changes, fixed delays,
and yield/continuation points into named locked anchors. Their exact ordered
list is part of the semantic route contract. Validation rejects a pack that
adds, removes, renames, or reorders them.
Do not place an anchor or wait between a base character and its combining
accent, or inside another multi-codepoint character: session compilation
rejects controls that split a Unicode grapheme.

Text and authored pages around those anchors are flexible. A translation can
have more pages than the native message, fewer pages, or be intentionally
empty. The runtime can therefore preserve semantic/control progress while
clamping presentation state during a live language switch, without replaying a
native control or inventing a game event.

`@alias` is only accepted when the referenced message is included in the same
pack, its locked-anchor contract is compatible, and every placeholder it uses
is available to the aliasing route. Alias cycles are rejected.

## Placeholders

Placeholders use generated semantic names rather than memory addresses:

```text
Il reste {lair_count} repaires près de {town_name}.
```

The five-ROM semantic catalog records the union of values genuinely used by
each logical route. This matters when official translations phrase the same
message differently: a value seen only in Japanese may still be available to a
French community translation of that route. Unknown or context-inappropriate
placeholders fail validation.

The current catalog contains localized text/term values, numeric values, and
typed icons. Examples include `{master_name}`, `{town_name}`, `{lair_count}`,
`{master_level}`, per-town report values, action scores, and
`{icon.name_entry.finish}`. The builder presents the route-specific list and
type rather than asking authors to memorize it.

Values are snapshotted when a semantic message begins. A language switch does
not mix counters or names captured at different moments.

Number placeholders may request a minimum digit count: `{master_level:02}`
or `{city_northwall_population:03}`. These display `02` and `002` for a value
of 2. Formats `01` through `09` are supported; larger values are never
truncated. Omit the suffix for unpadded numbers. Extracted scripts retain the
native decimal padding; score fields suppress leading zeroes and use column
alignment instead. Number formatting is not valid on names or icons.

## What each route displays

Every semantic route has a declared presentation shape, and both the builder
and the game reject a pack whose content that shape can never display. This is
checked when a pack is validated, not silently truncated at runtime:

| Shape | Where it appears | Enforced |
| --- | --- | --- |
| Dialogue | Simulation, Sky Palace and Temple message windows | Pages, lines and length are yours. |
| Fixed field | Action cards and messages, HUD labels, title options, menu rows, report tables | One page. The composer reads the first page and never advances, so an extra `@page` is content the player cannot reach. |
| Keyboard | `name_entry.prompt_and_alphabet` and its regional variants | Multiple `@page` pages are expected; the game selects them. |
| Inline term | Town names, enemy names, growth states, required master levels | One page and one line. These are substituted into another message, not printed on a line of their own. |

`title.save_choice.labels` additionally shows exactly two choices, so a partly
filled menu (one choice, or three) is rejected. An intentional `@empty` hides
the wording while keeping native controls/artwork. Omit the route instead to
keep fallback lettering.

The five optional `action.hud.*_label` routes are single words on a single
line.

Diagnostics name the message and the source line, so the builder points at the
line to change before an install rather than after.

## Fixed menu and report rows

`|` separates cells on the routes that use a fixed row or report layout: the
status reports, the message-speed scale and the menu rows. On those routes an
authored pipe is always a separator, so a literal `|` cannot be written directly
in a table label. Pipes inserted by placeholders (for example a player's name
or a localized growth-state term) remain ordinary text inside that one cell;
they never create extra columns. Everywhere else, including all dialogue and
keyboard keys, a pipe is ordinary text and is printed as written.

The editor's **Layout constraints** disclosure shows the accepted row shapes
from the same registry used by the game's grid. Unsupported field counts and
misplaced report rows are rejected on save and import, with the logical row
number in the diagnostic. Leading/trailing blank lines are ignored for table
positioning; internal blank rows are significant. Native divider rows remain
artwork. Single-column menu rows cannot gain extra fields.

Keyboard keys use ASCII spaces, not pipes. Keep their alphabet layout separate
from report-cell syntax.

Report totals belong in their own explicit cell, for example
`Resident count | {total_population:04}`. Use `|`, not a run of spaces, to
separate editable fields. Extraction preserves this boundary from the typed
value even when a native dictionary word contributes only one trailing space.

Reports use `|` between cells, so a translated cell can contain multiple words:

```text
Next level | {next_level_population:04}
```

Keep the cell order and `@line` rows of the extracted template: those rows
align labels and values with the game's artwork and selectors. Blank rows
are deliberate. Edit the words inside a cell without adding spaces to align
it. The speed scale has one cell per digit; its selector stays in the native
column. Dialogue paragraphs still use word wrapping, not report columns.

Enhanced Cities and Score reports measure the headings and all data rows to
share horizontal space between columns. Unused space can accommodate a longer
heading without making that word smaller. Gaps tighten before the table's
shared font size is reduced; numeric values remain right-aligned in their
columns. The original box, divider and row positions stay fixed. Report titles,
the Master status artwork and cursor-driven menus retain their separate layout
rules. Do not add alignment padding to translations—the renderer handles it.

The keyboard's two action keys are the game's own art, not translatable text.
They are a pair of tiny letters packed into a single tile -- "Ed" for End and
"Bs" for BackSpace, the second letter subscripted -- and one key cell has no
room for a word in any font. A pack references them as the `icon` placeholders
`{icon.name_entry.finish}` and `{icon.name_entry.backspace}`, which keep their
position in the row; the game supplies the picture.

## Name-entry keyboard pages

`name_entry.prompt_and_alphabet` may contain multiple keyboard pages separated
by `@page`. Each page ends with exactly five logical rows of 13 selectable
grapheme keys. Put the alphabet and digits most players need on page 1, then
place accented letters or other language-specific symbols on later pages.

The enhanced name-entry screen displays `< current/total >` above the keyboard.
Moving left from the first column or right from the last column cycles to the
adjacent page. The selector retains its logical row and moves to the opposite
edge.

The 13 key columns keep the original fixed pitch: every key owns two native
cells, a blank for the selection arrow and then the glyph, and the renderer
centres each key on that grid rather than letting the shaped advance decide.
Rows therefore line up with each other and with the original whatever letters
a pack puts on them -- a row of wide capitals cannot creep toward the window
frame, and a row of narrow ones cannot fall short of it. Keys are still shaped
in one pass, so every glyph on the keyboard shares one size. The builder and
game reject a page whose rows do not contain exactly 13 grapheme keys with
ASCII spaces between them; malformed keys cannot silently become flowed text.

Names support up to eight Unicode grapheme clusters. `{master_name}` uses the
accepted Unicode spelling in later enhanced dialogue, independently of which
pack is selected. Native rendering keeps the original keyboard-position
fallback spelling. Unicode is preserved in a separate `.arname` save companion,
not in the ROM-compatible SRAM name field; see
[Unicode name persistence](save-format.md#unicode-player-names-and-emulator-interchange).

Use one Unicode grapheme per position. A precomposed letter such as `é` is one
key; an emoji or a base letter plus combining marks is also one key when it is
a single grapheme. Backspace and finish are typed placeholders rather than font
characters:

```text
:: name_entry.prompt_and_alphabet
Choose a name.
@line
{master_name}
@line
--------
@line
A B C D E F G H I J K L M
@line
N O P Q R S T U V W X Y Z
@line
a b c d e f g h i j k l m
@line
n o p q r s t u v w x y z
@line
0 1 2 3 4 5 6 7 8 9 . {icon.name_entry.backspace} {icon.name_entry.finish}
@page
Choose a name.
@line
{master_name}
@line
--------
@line
À Á Â Ä Ç È É Ê Ë Ì Í Î Ï
@line
Ñ Ò Ó Ô Ö Ù Ú Û Ü Ý Œ Æ ß
@line
à á â ä ç è é ê ë ì í î ï
@line
ñ ò ó ô ö ù ú û ü ý œ æ ÿ
@line
’ - . , ? ! ( ) / : ; {icon.name_entry.backspace} {icon.name_entry.finish}
@end
```

Keep the row and grapheme counts exact on every page. Put `{master_name}` on
its own line immediately above the dash-only underline slot, then the five
key rows. Backspace and finish must occupy the final two positions: native
input still owns those actions. Every page is checked before save/import, not
just the selected page. Each page may contain up to 63 nonblank normalized
lines and 3,072 normalized UTF-8 bytes; runtime name expansion, gutters and the
page indicator have separate reserved space. An entirely empty keyboard route
remains accepted for native fallback. Regional JP kana routes are extracted
references, not substitutes for this US-runtime keyboard consumer.

Keyboard pages are pack-authored and are not limited to the official French
character inventory. Grapheme checks use the same Unicode property version as
the game; neither the builder nor runtime requires Python.

## Progress tracking

The editor stores one tab-separated row per message:

```text
# format=actraiser-language-progress version=1
sky.action_mode.confirm	wip
sky.menu.fight_monsters	done
```

Valid statuses are `not_started`, `wip`, and `done`. They do not change runtime
behavior and can be committed with a community pack to preserve collaborative
translation state.

## Limits

Version 1 rejects rather than truncates content above these limits:

- 64 script files and 8 fallback fonts per pack;
- 16 MiB per script and 64 MiB per pack-relative font;
- 16,384 messages and 4,096 operations per message;
- 64 authored pages and 256 KiB of UTF-8 text per message;
- 600 frames per authored `@wait` and 3,600 authored wait frames per message.

Native locked delays are part of the generated control contract and do not
consume the author-wait budget.

Use `@line`/`@paragraph` for structural breaks in fixed fields, tables and name
entry. `@preferred-line` is rejected outside flowing dialogue. Raw
NEL/U+2028/U+2029 line-separator controls are rejected there so they
cannot bypass native row/choice boundaries. Flowed dialogue and the shared text
backend recognize Unicode separators; U+2028 retains its paragraph's bidi base,
whereas paragraph separators resolve a new base. These controls need no glyph.

The scoped Sky Palace/simulation runtime has a smaller presentation limit:
**16,384 bytes and 256 nonempty value insertions per resolved dialogue**, including
substituted values and one byte separator per page. These are whole-message
budgets even when pages clear the
box. A new over-budget invocation uses native text; switching an active message
to an over-budget candidate keeps the prior selection. Simultaneous menu text
shares the frame's text and 256 insertion-range pools, and a rendered window must also fit the backend's
4,096-pixel height limit. Those later capacity/layout failures use the native
fallback described above rather than silently truncating text or leaving
invisible authored prompts. These are runtime limits, not a claim that every
pack passing the portable format validator will fit every layout/font setting.

## Tooling

The build and Languages UI use the same Go extraction/authoring core. The build
creates the U.S. starting source automatically. A source checkout can also run
the command directly from the repository root by replacing
`actraiser-builder` below with
`go -C installer run ./cmd/actraiser-builder`:

```sh
actraiser-builder localization-extract --rom /path/to/ar.sfc --out native-us.zip
actraiser-builder localization-extract --rom /path/to/ar-fra.sfc --out source-fr.zip
actraiser-builder localization-extract --rom /path/to/ar.sfc \
  --format catalog --out native-us-catalog.json
actraiser-builder localization-graphics --rom /path/to/ar-jp.sfc --out graphics-jp.zip
```

Outputs must not already exist. Source archives are private workshop backups:
opening/importing one is not installation or publication. Regional source packs
remain reference-only; create a translation against the U.S. semantic contract
to use their wording in the U.S. runtime. The tree editor and contextual
placeholder/control pickers use the same validator as these source exports.

The catalogue includes its ROM identity, source-consumer proofs and coverage
report; it is decompilation evidence, not a playable script. A single-ROM
catalogue deliberately does not claim the separate five-release coverage gate.
The graphics archive contains the 256-tile dialogue and credits atlases, raw
2bpp data, codepoint/unknown-slot inventory, source palette slices, all twenty
credits page maps/previews, and a provenance manifest. Credits use a distinct
alphabet, not the dialogue codepoint map. These ROM-derived outputs are for
local reference and must not be included in a community publication.

For adapter development, `--format runtime-routes` and `--format compose-routes`
produce the address-only U.S. dialogue and fixed-menu manifests from fresh Go
discovery. The data-only C-table generators consume those manifests; they do
not extract the ROM or define a second dialogue parser. Composer surface bounds
are shared declarative adapter data, not copied between two implementations.
The former Python ROM extractor and its generation harnesses have been removed.
