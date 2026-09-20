# The builder workshop

Choose or drop a shared `.arlang` in the Workshop to review and install it,
then manage it using the installed-package checklist. Direct archive copies
in the game's language-pack directory are also supported. See
[installation and external authoring](language-packs.md)
for archive/folder layouts, updates, command-line tools and AI-ready references.
The installed-package checklist manages directly copied archives too.

Open `ActRaiserRecompBuilder.app` on macOS, `ActRaiserRecompBuilder.AppImage`
on Linux, or `ActRaiserRecompBuilder.exe` on Windows to enter the Workshop.
The generic Linux builder's `run-build` launcher opens the same interface in
your browser.

On the desktop Builder's first launch, **Choose game folder** lets you review
the exact playable output directory before any game files are initialized.
The default is a portable `ActRaiserRecomp/` folder beside the Builder. Existing
installations can be updated after explicit confirmation; saves, settings,
language packs and custom assets are preserved, and the folder is never cleared.
For an old `utils/` installation, choose a new game folder and use **Import
previous installation** to bring its data forward.

The Builder remembers your selection. **Change game folder…** in the sidebar
changes it for the next launch only: finish or save your work, then close and
reopen the Builder. Changing folders does not move or copy the previous game.
The generic Linux browser launcher keeps its existing output/command-line flow.

**Interface language** in the sidebar selects English, French, German or
Japanese independently of game text and of the locale you are translating.
European English uses English. The preference is saved in
`game-assets/workshop-settings.json`, so it survives reopening the builder on a
different local browser port. Switching updates only interface labels; it does
not reload your project, discard unsaved text/file selections, install a pack,
or change the game's selected language. Navigation, Home, Build & play, Assets
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

## Find the right task

- **Home** — build your game or play an existing installation, open a tool,
  and return to saved language projects.
- **Build & play** — choose your US ROM and build. Progress and percentage remain
  in the top header while you visit other sections; **Run game** appears when
  the game is ready. The detailed log opens on failure. After a
  successful build, optional cleanup lives under **Storage & build tools**.
- **Languages** — see installed packages and workshop projects together,
  install a shared pack, create a translation, resume a project, clone a pack,
  or extract a regional ROM reference. Search by name, locale or ID and filter
  to installed packages or workshop-only projects.
- **Assets** — choose Music or Title artwork. Open the searchable track dropdown, select
  it, and compare the original and replacement in the detail pane. Regional
  music variants remain under their parent track. Save changes explicitly;
  the game uses the new configuration on its next launch.
- **Help & manual** — install your own instruction manual and read workshop
  guidance. See [Install your own manual](#install-your-own-manual) below; the
  manual loads only when you open this section.

The sidebar supports arrow-key navigation, Home and End. Browser Back and
Forward return to sections without unloading the editor. Unsaved message text
and file selections stay in place when changing sections, but are not durable
browser drafts. Save before closing.

Music search accepts translated track names, the original English names and
manifest IDs such as `song-01`. Switching the interface language preserves the
selected track, search text, pending files, per-level choices and audio playback.
Custom variant names, filenames and manifest conditions are displayed unchanged.

The interface language does not change your translation's locale, package ID,
name, credits, private notes or script. It also preserves import replacement
choices, publication permissions, the WIP option and pending font uploads.
Only install/export actions apply those choices; switching the interface
language never installs, publishes or enables a pack.

Languages and its Home shortcuts stay disabled until a complete, valid native
US source exists at `game-assets/languages/native-us/pack.ini`. Start a build
with your US ROM: source extraction runs first, before dependency checks and
regeneration. Languages unlocks automatically as soon as that step finishes;
you can edit while the rest of the build runs, even if a later step fails.
An existing valid source unlocks it when the workshop opens, without needing
the ROM again. Invalid sources are reported rather than silently overwritten;
restore a valid backup, or move the invalid `native-us` folder aside and rebuild.
If a cleaned-up installation lacks the source, restore its generated source
folder or download the build tools again and build with your US ROM. Existing
translation projects are preserved while the section is locked.

## Install your own manual

The workshop ships no instruction manual. Choose a PDF under **Help & manual**
and it is installed to `game-assets/manual.pdf`, which is both where the
workshop reads it and where the game's own reader looks. **Remove manual**
clears it again, and installing a second time replaces the first.

Without one, the workshop shows an empty state and the game simply omits its
Manual section — nothing is broken by not having one.

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

The workshop applies the game's own test when you install a file and tells you
which of the three rules a PDF fails, so a manual that will not open in the game
says so at install time rather than in the game's menu. A PDF that fails the
test is still installed and still readable in the workshop.

To convert a text PDF, export or rasterise it to one baseline JPEG per page at a
single page size, then recombine those images into a PDF.

## Save and test language projects

Use **Languages → Add a language pack** to choose an `.arlang` file, or drop it
into the Workshop from any section. Add one pack at a time; no unzipping is
needed. **Install a language pack** also opens this package-first flow, with
**Install already imported** available for saved projects. Author backups
(`.arproject`) and compatible legacy ZIPs have a secondary import option;
**Advanced: unpacked language folder** retains **Choose folder…** and manual
paths for authors. Selecting or dropping a package opens a preview only.
Choose **Import & install** to save the workshop copy and install it. Matching
package IDs require explicit replacement during import; a distinct ID lets you keep both.

Opening a saved project gives you an editable workshop copy. The editor's
sticky toolbar has **Languages** and **Save progress**, with a visible
saved/unsaved indicator. **Save progress** (or Cmd+S / Ctrl+S) saves pending
message text, Not started / WIP / Done status, package details, notes, public
notices and font dependencies together. Validation failure leaves all of those edits unsaved so you
can correct them. If this package ID is already installed, saving also refreshes
that game copy, preserving whether it is enabled or disabled. Restart the game
to load the updated text. If the refresh fails, the editor keeps your saved
progress and explains why the installed copy could not be updated.
**Messages**, **Details & credits**, and **Fonts** are separate tabs; drafts survive switching.
For the first installation, use **Pack actions → Install in game**, then
**Install for this game**. Subsequent saves update it automatically. Local
installation keeps every supplied message, including unfinished translations.
It does not ask you to confirm redistribution rights or filter someone else's
work. Those choices belong to **Export for sharing**, where publishers choose
whether to include WIP translations. Restart the game and select the package
name in the overlay's Localization settings with Enhanced text rendering.

Use **Fonts** to choose a primary TTF/OTF, add ordered fallback fonts, or restore
the bundled font. File selections are copied into the project by **Save progress**;
reordering retained fonts does not duplicate their data. Keep redistribution
license notices in **Details & credits**. **Check coverage** uses the built game's
own font backend and reports missing characters with message locations; an
optional sample checks a player name or other live value. Install and export
require this check to succeed. Private backups remain available without it.
Complete a game build first; scalar coverage does not prove shaping or layout.

The library is sorted by package name, with locale and stable ID to distinguish
editions. **Installed in game**, **Workshop only**, and **Read-only reference**
badges make their roles explicit. An installed package can exist without an
editable project; import its archive to edit a copy. Home shows up to four
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
brighter preview on Home. Choose **Fillmore · the Master’s patrol** for the
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
in the sidebar. A system reduced-motion preference always keeps the scene
still, including when Animated is selected. Switching menus continues the same
scene; hiding the browser tab pauses it until you return. **Scene off** hides
both the background and Home preview. Scenery and motion choices are remembered
for this browser session only; they do not alter game settings.
