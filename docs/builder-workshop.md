# The builder workshop

Choose a shared `.o2r` language pack in **Play → Manage Mods** or drop it onto
the Builder. The Languages filter also includes imported
legacy `.arlang` packs and translations delivered to the mods folder. See
[installation and external authoring](language-packs.md)
for archive/folder layouts, updates, command-line tools and AI-ready references.
The installed-package checklist manages directly copied archives too.

Open `ActRaiserRecompBuilder.app` on macOS, `ActRaiserRecompBuilder.AppImage`
on Linux, or `ActRaiserRecompBuilder.exe` on Windows to enter the Workshop.
The generic Linux builder's `run-build` launcher opens the same interface in
your browser.

**Check for updates** in **Builder Settings** opens the project's GitHub Releases page
in your system browser. Download a newer Builder there, then rebuild your game
to apply its changes. **GitHub repository** opens the project source and README.

On the desktop Builder's first launch, **Choose game folder** lets you review
the exact playable output directory before any game files are initialized.
The default is a portable `ActRaiserRecomp/` folder beside the Builder. Existing
installations can be updated after explicit confirmation; saves, settings,
language packs and custom assets are preserved, and the folder is never cleared.
For an old `utils/` installation, choose a new game folder and use **Import
previous installation** to bring its data forward.

After rebuilding, the game's first launch automatically imports older saves
in that data folder into Slot 1 and retains the originals. No manual save
conversion is required; see [Upgrading older saves](manual.md#upgrading-older-saves).

The Builder remembers your selection. **Change game folder…** in **Setup**
changes it for the next launch only: finish or save your work, then close and
reopen the Builder. Changing folders does not move or copy the previous game.
The generic Linux browser launcher keeps its existing output/command-line flow.

After opening a game folder, the Builder checks whether the game matches the
release **bundled with this Builder**. This is an offline comparison, not an
online check for newer releases. Play confirms a matching build. **Rebuild
recommended** appears when the release inputs differ, or when an older install
has no verifiable build record. When the previously saved ROM is readable,
**Rebuild now** applies the bundled changes in one click. Otherwise **Review
rebuild** opens Setup so you can select your ROM again. Saves, settings,
language packs and custom assets are preserved.

You can still play the existing game. Pressing any Play button on a stale or
unverifiable build explains that this Builder's changes will not apply until
you rebuild; choose **Play existing build**, **Rebuild now** (or **Review rebuild**
if the ROM is missing), or **Cancel**. Matching builds launch without that prompt.

The Builder keeps the ROM selected for a build as `user-rom.sfc` in the game
data folder. Setup shows this saved path, and **Rebuild game** reuses it
without opening a file picker. Building with a different core ROM replaces that
copy. The original file-picker location is not needed, and moving the entire game folder
keeps one-click rebuild working. Existing `game.sfc` installs are also recognized,
including the older generic bundle layout. Missing, unreadable or invalid ROMs
produce an error; missing copies require selecting the ROM again.

In **Setup**, select a ROM checklist entry to open your system file picker.
Dropping `.sfc` or `.smc` files onto the Builder fills the matching entries
based on their contents. In Big Screen, the checklist opens the
controller-friendly file browser at your last successful ROM folder, or
at Downloads on first use when it is available. The mod picker remembers a
separate folder. Use **Computer** to switch drives and **Up one folder** to
move to the parent. **Cancel** or controller Back closes the picker from any
folder. When a build starts, the Builder opens a
dedicated view with its status, current phase and live log.
You can visit other sections while the build runs. Return to **Build status**
to check the current stage or review the log. After a failed build, **Save build report**
writes the diagnostic and log to the game folder’s `logs/` directory in either view. After a successful build,
**Play ActRaiser** launches the game directly from Big Screen’s build status.

Every Builder build starts with empty temporary directories for generated code,
object files and Zig compiler caches, even when rebuilding the same release.
Those per-attempt files are removed after success or failure. The compiler SDK,
bundled runtime libraries, installed game and player data are not cache-cleanup
targets. Older caches and interrupted attempts are not reused or automatically
deleted; if cleanup fails, the build log identifies the leftover directory.
This applies to desktop Builders and the generic Linux Builder, not ordinary
developer CMake builds. Clean rebuilds may take longer than incremental builds.

Successful builds write `.actraiser-build.json` in the game output folder. Keep
it with the game when moving the folder. It records the bundled input identity,
Builder version and executable checksum, not personal data or absolute paths.
Failed/cancelled builds do not update it. Missing, invalid or mismatched records
conservatively recommend rebuilding rather than claiming the game is current.
Version labels alone are not trusted: two builds labelled `dev` or `dirty` can
contain different code. Developer/older generic Builders without a release
identity report that comparison is unavailable; use a current release Builder.

The globe beside build status opens **Interface language** on every desktop page.
In Big Screen, select the globe beside **ActRaiser** on Play. The choices are
**English**, **Français**, **Deutsch**, and **日本語**. This sets the Builder language
independently of game text and of the locale you are translating.
European English uses English. The preference is saved in
`game-assets/workshop-settings.json`, so it survives reopening the builder on a
different local browser port. Switching updates only interface labels; it does
not reload your project, discard unsaved text/file selections, install a pack,
or change the game's selected language. Navigation, Play, Setup, Workshop
and Help are translated, along with language-package management, creation,
cloning, import, installation and export workflows. Message-editing controls,
reference tools, layout guidance, previews, credits/notices and font tools also
use the selected interface language. Location groups, introductory-message
titles and route descriptions are translated too; searching accepts these
navigation captions in all four interface languages. Original script excerpts, technical build logs,
manifest identifiers and raw diagnostic details retain their original text.
An installed manual is shown as supplied and is not translated, and native
browser controls (PDF reader, audio player and file chooser) use the browser's
own language settings. The workshop's native package-folder prompt uses your
workshop interface language, while its standard buttons follow the operating system.

Script commands and inserted value names stay the same in every interface
language. For example, the value picker inserts `{master_name}`, not a translated
identifier. Switching languages also preserves expanded tree branches, search
results, reference text, font order, coverage results and unsaved credit notices.

## Supported ROM checksums

A first build requires **ActRaiser (USA), revision 0**. The other five ROMs are optional sources of extra content. All six supported cartridge releases are revision 0. Open **ROM checksums** on the Builder’s ROM checklist to see the accepted SHA-256 values in desktop or Big Screen mode.

The sizes and checksums below cover the cartridge data. The Builder accepts `.sfc` and `.smc` files with or without a 512-byte copier header and omits that header before verifying SHA-256. When checking a dump yourself, exclude the header to reproduce these values.

| Cartridge | Bytes without copier header | CRC32 | SHA-256 |
|---|---:|---|---|
| ActRaiser (USA) | 1,048,576 | `EAC3358D` | `b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0` |
| ActRaiser (Japan) | 1,048,576 | `BEE9B30C` | `3655833fd0fb4985c3dbbf28141f65564f7a228ee92d81a12982ef7cb952a51b` |
| ActRaiser (Europe, English) | 1,048,576 | `09097B2B` | `146a68436fa9dbe728ddc7355821384765325e356cb8b9b193a4f22333ed52a0` |
| ActRaiser (Germany) | 1,048,576 | `29A78AF9` | `01923db83e0e8b19d476483649d956e3e24cfc918ca04a6aa04faa29ba8e4c41` |
| ActRaiser (France) | 1,048,576 | `A27E2664` | `6cc2cadfcb4fba4c1abb2a1d06b49b840bec65d75acaa0ac8831576442e7e96a` |
| ActRaiser 2 (USA) | 1,572,864 | `4901F718` | `71bdd448a30b88725864e55594ebb67a118b1f197a3f9e5dd39dbf23399efa15` |

## Import game content

Optional content is not required to build or play ActRaiser.
**Setup → ROM checklist** accepts optional ROM sources and shows which
features each one supplies. Select a source and choose **Import selected extras**
on an existing build. On the first build, the Builder imports selected extras
along with the US game. Use **Import a private regional media archive** in
Setup for an existing `.armedia` file. Imports are stored with your local game and travel
with packaged builds and installation transfers.

### ActRaiser 2 Master

1. Select **ActRaiser 2 (USA)** in the ROM checklist.
2. Choose an unmodified ROM (`.sfc` or `.smc`, with or without a copier
   header), then press **Import selected extras** or build the game.
3. Restart the game, or build it first if you have not already done so.
4. Open **System → Game → ActRaiser 2 Master** in the game.
   Turn it on or off during a level to switch characters.

The imported Master includes gliding, diving and sword moves while playing
ActRaiser's stages. The setting starts off and only appears when valid character
assets are installed. A current game build needs only a restart after importing.
The character import saves `game-assets/imports/ar2-master.armaster`; it does not
replace your ActRaiser ROM. Keep this pack with your game when moving it.

Importing also adds **ActRaiser 2 · above the clouds** to **Scenery** in the
Workshop. **ActRaiser 2 · Fillmore patrol** appears when the Workshop
also has the original ActRaiser scenery, extracted from your US build ROM.
These scenes use your imported character graphics and animations. They appear
without reopening the Workshop and remain available without the AR2 source ROM.
Choose a scene yourself; importing preserves your current selection. The existing
**A little atmosphere** controls and reduced-motion preference apply to both.

### Adding content importers

The catalog is designed to accept additional content sets. Developers register each importer in
`installer/internal/gameimports`, with its own validation, extraction,
installation and status functions and translated instructions. The import catalog supplies the Builder and headless import tools. Imported content belongs
under `game-assets/imports/`, which game packaging carries automatically.

## Find the right task

- **Play** opens an existing game, shows release freshness and offers a rebuild
  when the bundled release changes.
  If the mod scan fails at launch, retry it or use the last working mod setup.
  A game with no prepared setup can use its legacy replacements.
- **Manage Mods** opens with all installed packs grouped by type, with
  **Title artwork** first. Use the **Enabled**
  checkbox to turn a pack on or off. **Preview** opens a dialog for music
  tracks, title artwork, language samples or manual pages, including disabled
  packs. When enabled packs offer the same asset, each affected mod shows a
  conflict flag. Choose **Conflicting mods** in the filter to see them together.
  Open either pack’s **Preview** and choose the source for the shared asset.
  This clears that conflict from every affected row. If a pack was already
  supplying the asset, it keeps playing while you decide. You can revisit
  the choice in either preview. In a multi-track pack, **Exclude this asset**
  skips just that track or regional variant. **Include this asset** restores it.
  For example, a soundtrack can supply the title theme while a standalone
  theme stays installed and shows **Inactive**. The standalone theme can
  take over if you disable the soundtrack. Re-enabling the soundtrack
  restores its priority and remembers its individual track settings.
  A filtered view shows **Clear filter**. Multi-file installs report each
  file and open **All mods** when the batch spans multiple mod types.
  On desktop, choose **Languages** to search installed packs by name, locale,
  ID, or archive filename and see how many matches are enabled or disabled.
  Competing language releases show their filenames in both views. If launcher
  folders supply the same filename, the choices show their paths.
  **Needs attention** lists archives whose type could not be read.
  In Big Screen, choose a type above the tables to filter the same screen.
  Up and Down move between mods. Left and Right move between actions for
  the selected mod. **Game music** in the Music packs filter opens the
  original soundtrack browser alongside active O2R replacements and legacy
  manifest tracks. Music previews in both layouts have play, pause, and
  ten-second seek buttons. Desktop also has a volume slider.
  **Uninstall** moves a local archive to
  `game-assets/mods/.uninstalled/` after confirmation. Bundled and launcher
  packs can be disabled from the same list.
  The desktop **Preview** dialog offers **Save .o2r copy** for local archives,
  including ones created by legacy conversion. Language previews show the
  release's author, version, license and text coverage, including how many entries use
  the Native US fallback.
- **Setup** shows the ROM checklist, game folder, and build controls. Use
  **View build log** to review the last build. Optional cleanup is under
  **Storage & build tools**.
- **Workshop** opens saved projects and authoring tools on desktop.
  **Languages** includes translation creation, cloning, reference extraction
  and sharing. **Music** includes original-ROM music extraction, replacement
  previews, per-region variants and music pack export. The track picker marks
  songs with custom music and identifies songs with regional replacements only. **Save** stores
  edits in Workshop. **Save draft and continue** saves pending changes before
  packaging. **Discard all changes…** lists the affected tracks and regions
  before clearing unsaved music edits. In the pack dialog, **Close** keeps the
  pack details, file and track selections for this Builder session. **Discard
  draft** clears the current pack draft. **Create title artwork pack** validates
  a 2212 × 760 PNG with at least 1% visible coverage.
  **Create manual pack** packages your PDF for sharing. **Install in game** installs
  the pack and opens its type in **Manage Mods**. **Export .o2r…** saves a
  shareable archive. The Builder generates an ID from the pack's contents and
  metadata. Music replacements use Ogg Vorbis. Convert
  MP3s to Vorbis-encoded `.ogg` files before selecting them.
- To revise a published music bundle, open **Workshop → Music → Import .o2r…**.
  Import restores the Ogg files to their track and region slots, including loop
  points and gain. Matching assignments are replaced in the saved draft. Other
  Workshop tracks stay available. Add or replace songs, then choose **Install
  or export…**. The pack details are filled in, and the imported tracks and newly
  filled slots are selected. You can change the selection and version before export.
  **Finish editing** clears the pack details and keeps your saved music available for the next pack.
  **Create title artwork pack** and **Create manual pack** offer the same import
  action for their PNG or PDF. For a translation, use **Languages → Import for
  editing**. Original archives and installed mods are only affected by a separate
  export or install action. Supported README, license and icon files carry forward. Revised music, artwork and manual packs receive a new
  content-derived ID and can coexist with an earlier installed revision.
- **Builder Settings** contains scenery, help and project
  links. Enable **Close Builder when the game launches** to close the standalone
  Builder after a successful launch. The game keeps running. Unsaved Workshop
  edits and pending mod writes keep the Builder open. Recomp Launcher’s tracked
  sessions wait for the game to exit.

The sidebar supports arrow-key navigation, Home and End. Browser Back and
Forward return to sections without unloading the editor. Unsaved message text
and file selections stay in place when changing sections, but are not durable
browser drafts. Save before closing.

When an action fails, follow the recovery guidance above **Error details**.
**Copy error details** includes the action, error code and original diagnostic.
Keep unsaved edits open while resolving the problem. An imported project can
remain saved in Workshop even if installation fails; the error identifies that
partial success. If the connection drops during installation, check the installed
package before retrying because Workshop may not have received the result.

For missing characters, open **Fonts → Check coverage**, add a suitable fallback
font and save before retrying. An unexpected font-check response instead indicates
a problem communicating with the game: keep the pack and include the report and
game version when requesting support. Rebuild with the current Workshop if the
game was built with an older version.

Music search accepts translated track names, the original English names and
manifest IDs such as `song-01`. Switching the interface language preserves the
selected track, search text, pending files, per-level choices and audio playback.
Custom variant names, filenames and manifest conditions are displayed unchanged.

The interface language does not change your translation's locale, package ID,
name, credits, private notes or script. It also preserves import replacement
choices, publication permissions, the WIP option and pending font uploads.
Only install/export actions apply those choices; switching the interface
language never installs, publishes or enables a pack.

The Builder extracts the US text as a protected local `.o2r` under
`game-assets/mods/.bundled/`. The game reads its unpacked copy at
`game-assets/languages/native-us/pack.ini`. This US baseline is required for
enhanced text. A build prepares it before compiler discovery, and an existing
baseline can be packaged without extracting the ROM again.

European English, German, French and Japanese ROMs create optional language
mods under **Manage Mods → Languages**. They can be selected in-game when
enabled. Messages that use different controls or layouts fall back to US text.
The complete regional reference remains available in Workshop for comparison.
Keep ROM-derived archives local.

## Install and share manuals

In **Manage Mods**, choose a manual `.o2r` or PDF, or drop it onto the
Builder. Compatible PDFs are packaged as `.o2r` on import. Workshop checks
PDF compatibility as soon as you select the file for a manual pack.
Manual packs also work with the generic `mods` CLI and registered launcher
folders. Enable the packs you want to keep available and choose one when they
compete for the in-game manual. **Preview** pages through a pack without
changing the in-game selection. **Open manual** under **Manage Mods → Manual
packs** opens the active PDF. Big Screen supports page previews and activation
controls under **Manage Mods → Manual packs**.

Other PDFs remain readable as a loose fallback at `game-assets/manual.pdf`.
**Manage Mods → Manual packs** can open or uninstall the fallback. Uninstall
moves it into the legacy customization backup folder.

Two readers, two requirements:

- **In the workshop**, any valid PDF works. It opens in your browser's own PDF
  viewer, with its page navigation, zoom, search and printing.
- **In the game**, the reader needs a *scan album*: one baseline JPEG per page,
  every page within 5% of the book’s most common size, with the page images
  making up at least 80% of the file. It never parses PDF structure — it carves
  the page images directly — so a text or vector PDF yields no pages.

  The size rule is a tolerance rather than exact equality, because a real
  flatbed pass drifts: a genuine 48-page scan may come out mostly 1009×1767
  with a third of its sheets a pixel wider and one crooked page at 1014×1770.
  That is one book. An embedded figure or letterhead logo is off by tens of
  percent, which is what the rule still rejects.

Use **Create manual pack** to publish a compatible scan album as a manual `.o2r`.
The Builder checks in-game compatibility when exporting or installing packs.
Previously imported loose PDFs can still be read from **Manage Mods → Manual packs**.

To convert a text PDF, export or rasterise it to one baseline JPEG per page at a
single page size, then recombine those images into a PDF.

## Move legacy customizations to Manage Mods

Open **Manage Mods → Review legacy customizations…** to see eligible music
assignments, regional splits, title artwork, and your imported manual. Choose
**Later** to keep using the current setup, or **Convert customizations** to
move compatible entries into mod packs. Existing mod selections keep priority.
Resolve any open conflict in **Manage Mods** before converting a replacement
for that target. The review explains which custom rules and files stay on the
legacy path. If Setup asks for a rebuild, finish it before converting.

Preview and select the included English or Japanese HD titles in **Manage
Mods → Title artwork**. Enabling either included design switches off the
other. A fresh install starts with both off and uses the original ROM title.
On upgrade, the Builder backs up the manifest and moves an exact legacy
Builder-owned title selection to its included pack when the title slot has
no competing choice. Custom manifest artwork remains available as a fallback.
An archive already using one of the included IDs keeps its place in the catalog.

To restore the previous setup, close the game, disable the converted packs from
the migration, and restore the reported manifest backup as
`game-assets/manifest.ini`. If you converted a loose manual, copy its backup
from `game-assets/legacy-mod-backup/` to `game-assets/manual.pdf`. The original
music and artwork files remain available at their original paths.

## Save and test language projects

Install a shared language pack in **Manage Mods** or drop it onto the Builder.
Legacy `.arlang` publications are converted to `.o2r` during installation.
In **Workshop → Languages**, open a saved project or import an editable copy
of a translation. Author backups (`.arproject`), compatible legacy ZIPs, and
unpacked language folders are available under **Import for editing**.
Publish releases through **Export .o2r…** with a version such as `1.0.0`.
In the desktop app, choose the archive destination in the system Save dialog.

Opening a saved project gives you an editable workshop copy. The editor's
sticky toolbar has **Languages** and **Save**, with a visible
saved/unsaved indicator. **Save** (or Cmd+S / Ctrl+S) saves pending
message text, Not started / WIP / Done status, package details, notes, public
notices and font dependencies together. Validation failure leaves all of those edits unsaved so you
can correct them. Saving also refreshes game copies previously installed from
this Workshop project, preserving whether the pack is enabled or disabled. Restart the game
to load the updated text. If the refresh fails, the editor keeps your saved
progress and explains why the installed copy could not be updated.
Drafts survive switching between **Messages**, **Details & credits**, and **Fonts**.
Opening a different message prompts only when the current message has unsaved
text or status changes. Package details, credits, and font edits carry over.
For the first installation, use **Install or export… → Install in game**, then
**Install for this game**. Subsequent saves update it automatically. Local
installation keeps every supplied message, including unfinished translations.
It does not ask you to confirm redistribution rights or filter someone else's
work. Those choices belong to **Export .o2r…**, where publishers choose
whether to include WIP translations. Restart the game and select the package
name in the overlay's Localization settings with Enhanced text rendering.

Use **Fonts** to choose a primary TTF/OTF, add ordered fallback fonts, or restore
the bundled font. File selections are copied into the project by **Save**;
reordering retained fonts does not duplicate their data. Keep redistribution
license notices in **Details & credits**. **Check coverage** uses the built game's
own font backend and reports missing characters with message locations; an
optional sample checks a player name or other live value. Install and export
require this check to succeed. Private backups remain available without it.
Complete a game build first; scalar coverage does not prove shaping or layout.

The library is sorted by package name, with locale and stable ID to distinguish
editions. **Installed in game**, **Workshop only**, and **Read-only reference**
badges make their roles explicit. An installed package can exist without an
editable project; import its archive to edit a copy. Workshop shows up to four
workshop projects; Languages contains the complete managed package library.

While editing, choose a **Reference language** or **Extract reference ROM…**.
This stays in the same project and preserves your unsaved work and cursor.
Source text and your translation appear side by side on wide screens, stacked
on smaller screens. The source heading identifies the actual reference used,
including US fallback when a regional reference has no corresponding message.
The builder's CMake-backed and hermetic builds prepare the native US source
automatically; an existing source is validated and reused, not overwritten.

## Enable, disable or uninstall a translation

Languages opens with a checklist of **Installed packs**. Check a pack to make it
available in the game's language selector; uncheck it to disable discovery without
deleting its files or translation progress. Changes save immediately and take
effect after restarting the game. The game still uses one selected language at
a time, and original US text is always available. Updating a disabled package
keeps it disabled.

Disabling renames only `pack.ini` to `disabled-pack.ini` in the installed folder.
Re-enabling validates the declared files and restores that manifest. If a pack
changed since the checklist loaded, refresh before trying again.

For removal from the managed list, choose **Uninstall…** on the
package, and confirm its name and ID. Restart the game to refresh its catalog.
Your editable project and progress are left untouched. Original US text is part
of the game, not an uninstallable package.

Uninstall removes the active discovery manifest, not the translation's data.
The manifest is retained as `uninstalled-*.ini` in that installed package's
folder, alongside its version files and fonts. The confirmation reports the
exact recovery path. This intentionally does not reclaim disk space or break
files a running game may still be using. To use the pack again, review and
install your saved project, or import and install the original shared archive.

See [Language packs](language-pack-format.md) for editing, references,
import/export, credits and installation details.

## A little atmosphere

The workshop can play a quiet background scene behind every menu, with a
brighter preview on Play. Choose **Fillmore · the Master’s patrol** for the
Master jump-slashing birds and dispatching approaching ground enemies, or
**Sky Palace · angel patrol** for a flight through the clouds. With a clean,
headerless US ROM supplied to the builder, sprites, palettes, animation poses,
and Fillmore's forest/ground artwork are extracted locally. Movement paths,
scenery repetition and encounters are decorative, not live gameplay. Ordinary
enemies take one hit; a centaur occasionally enters for a short three-hit
mini-boss battle, then the patrol resumes. There is no sound, screen shake,
flashing background, or input to manage; this is a little show behind the tools.
The atmosphere never captures clicks or changes a game, save, or translation.

The tiny local cache lives in `game-assets/workshop/` (inside `utils/` in a
packaged installation). It is reused on later launches, even without the ROM.
Scenery is prepared independently of build completion, including when opening
directly into another menu. Before extraction, or if the ROM/cache cannot be
used, an illustrated placeholder remains available. An updated artwork
collection may need the US ROM once to refresh its cache. A cache-write failure
still allows artwork for the current session.
These ROM-derived images are never included in distributed builder packages.

Choose **Follow system**, **Animated scene**, **Still scene**, or **Scene off**
in **Builder Settings**. A system reduced-motion preference always keeps the scene
still, including when Animated is selected. Switching menus continues the same
scene; hiding the browser tab pauses it until you return. **Scene off** hides
both the background and Play preview. Scenery and motion choices are remembered
for this browser session only; they do not alter game settings.

## Application icons

The Builder uses an original 16-bit-style floating-temple icon; the game uses
an original sword. Neither contains ROM-derived graphics. The same artwork is
embedded in Windows executable resources, macOS app bundles, and Linux
AppImages/desktop icons. AppImage packaging explicitly includes and validates
the required `.DirIcon`, including when packaging on macOS.

Masters, platform exports, generation prompts, and the maintainer-only export
command are documented in [Application icons](../installer/internal/appicons/README.md).
Player builds use the embedded exports and need no extra image tools.
