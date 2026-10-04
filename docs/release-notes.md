# Next release — draft

Unreleased changes since **v0494** (September 16, 2026).
The next version number and release date are not assigned yet.

This update adds customizable regional gameplay, ten independent save slots,
an optional modern town menu, and new scenery effects. It also improves the
Builder's update workflow and startup time, and makes game builds faster.

Open the in-game settings with **Esc**, **F1** or **L3** on a controller.
Menu paths below start there unless they are marked **Workshop**.

## Regional gameplay and media

- Choose US, Japanese or European gameplay presets, or mix individual rules
  for combat, enemy behavior, stage layouts, magic, lives, scoring and town
  development under **Regions**. **Regions → Presets → Apply gameplay preset…**
  applies a whole preset; the other tabs let you customize individual rules.
- Combine European Beginner, Normal or Expert difficulty adjustments with
  your preferred regional gameplay rules using
  **Regions → Action → Difficulty adjustments**.
- Select regional artwork and music independently from gameplay and language.
  Choose them under **Regions → Art & music**. Extract supported media from
  your own donor ROMs in **Workshop → Assets → Regional media**.
- Save regional choices separately for each campaign. Settings explain when
  a change takes effect and whether it affects an existing town.
- Review population-rule changes at the Sky Palace before redeveloping towns.
  Confirming a conversion creates a recovery copy of the campaign.

See [Regional settings](regional-settings.md) and [Regional media](regional-media.md).

## Save slots and campaign tools

- Keep **ten independent campaigns**, each with its regional settings,
  randomizer recipe and enhanced player name, through the **Saves** menu.
- Existing saves are imported into Slot 1, with the originals retained.
- Set up regional rules for a new campaign before starting an empty slot.
- Switch campaigns or restart the game without closing and reopening its window.
- Export and import a complete campaign as a single `.arsave` file, including
  its regional settings and enhanced player name. Use **Export campaign** or
  **Import save** under **Saves → Advanced → Actions**.
- Choose save import and export locations through file pickers.
- Save your edits and restart in the same window with
  **Saves → Advanced → Actions → Apply and restart**, then choose **Continue**
  at the title screen to load the changes.

See the manual's [Save slots](manual.md#save-slots) and
[Save editor](manual.md#save-editor).

## Experimental randomizer

- Save the randomizer's seed and options with each campaign. Continue restores
  the same setup without rolling it again.
- Independently randomize regional action and town rules for a new campaign.
  Enable **Regional action rules** and/or **Regional town rules** under
  **Randomizer → Seed**.

To access these options, enable **System → Tools → Show debug settings**, then
open **Randomizer** at the title screen. See [Seeded campaigns](randomizer.md)
for setup and current limitations.

## Town menus and everyday play

- Choose a compact town command menu that remembers selections and lets you
  read about a miracle or offering before using it. Set
  **Town 3D → Scene → SIM menu** to **Modern**; it also works with 3D towns
  turned off. **Original** remains the default.
- Adjust the modern town menu's size with
  **Town 3D → Scene → SIM menu scale (%)**.
- Skip repetitive SIM action explanations with an optional quality-of-life
  setting for the original town menu. Enable
  **System → Game → Quality of life → Native menu quick use** (off by default).
  It skips optional miracle explanations and selection instructions while
  keeping confirmations and outcomes. Use **Describe menu item** to read the
  full explanation when needed.
- Continue with the Sky Palace positioned over your last visited town using
  **System → Game → Quality of life → Remember last town**. This option is
  on by default and remembers the location separately for each save.

See [Modern SIM menu](manual.md#modern-sim-menu) and
[Native menu quick use](manual.md#native-sim-menu-quick-use).

## Visual improvements

- Turn scenery lighting and particles on or off independently of spell effects
  with **Video → Effects → Environmental effects** (on by default). This
  controls the Fillmore and Bloodpool additions below in flat and Diorama views.
- Add canopy light, falling leaves and drifting motes to Fillmore's forest.
- Add water accents, mist, drips, dust and lighting to Fillmore's caves,
  temple and tower.
- Add moonlight, red-water reflections and low mist to Bloodpool's marsh.
- Add window light, torch glow and interior atmosphere to Bloodpool's castle.
- Improve the Centaur's lightning effects, controlled by
  **Video → Effects → Action spell lighting** and **Action spell particles**.
- Improve Northwall boss magic and water impacts, using the same
  **Action spell lighting** and **Action spell particles** options under
  **Video → Effects**.
- Refine regional town buildings, construction stages, landmarks, rocks and
  vegetation, with improved tree shadows. The 3D models are selected through
  **Town 3D → Scene → Voxel town quality**.
- Restore the Sun miracle's warm tint and add light rays over the selected area.
  Control the added rays with **Town 3D → Scene → Effect lighting**, and their
  drifting motes with **Effect particles** in the same tab.
- Display locked-town terrain without prematurely revealing its cathedral.
- Choose Japanese HD title artwork under
  **Workshop → Assets → Title artwork → Artwork style**, with
  **Use the included HD title** enabled.
- Show HD title artwork throughout the animated title intro.

## Performance and fixes

- Add **Screen ratio → Auto** for action stages. It adapts extra rows or
  columns to the window, including paused resizing, while retaining the
  selected pixel aspect. Thanks to Simon W. Jackson for the contribution.
- Extend Auto to world navigation, simulation towns and the Sky Palace.
  Enhanced 3D worlds and towns can also expand vertically; native flat paths
  retain their scanout height. Keep menus and HUD graphics at their pixel aspect.
- Remove HUD-shaped shadows from scene lighting when the HUD is relocated.
- Improve town-rendering performance.
- Speed up initial loading of the globe's terrain.
- Improve action-rendering performance when showing extra rows above and
  below the original view with **Action 3D → Scene → Vertical extend**.
- Fix sprites clipping early at the edges of widescreen Action 3D views.
- Fix repeated or displaced terrain at extended viewport seams when GPU
  background capture and the original-view comparison use different cameras.
- Restore held camera input and Dynamic Cam's automatic return and edge
  framing during streamed action rendering.
- Reduce CPU use for game audio.
- Keep original music muted when switching between replacement music tracks.
- Fix macOS startup keyboard focus when entering fullscreen.

## Languages and Workshop

Choose an installed translation under **Localization → Game text → Text source**.
Create and edit language packs in **Workshop → Languages**.

- Language packs can mix fonts, colors, sizes and emphasis within a message
  using the new **v2 text format**, with authored line and paragraph breaks.
- Add side-by-side playback of the original text and your translation.
- Add a styling toolbar to the translation editor.
- Saving Workshop edits updates the installed language pack.
- Make name entry more responsive when using
  **Localization → Game text → Text rendering → Enhanced**.
- Fix translated town pause text.

See [Language pack authoring](language-pack-format.md) for the v2 format and
[Workshop playback](language-pack-format.md#workshop-playback-and-tracing).

## Builder

- Open the desktop Builder faster on repeat launches across Windows, macOS
  and Linux. Unchanged bundled tools no longer need a full check every time;
  the first launch still performs full verification.
- Show when your installed game needs rebuilding to receive the changes
  bundled with the Builder you are using.
- Use **Rebuild now** to rebuild with your saved ROM without selecting it again.
- Speed up game builds, with less compilation work running at once on machines
  with limited memory.
- Fix compiler launch failures when a Windows portable installation is inside
  a deeply nested folder.
- Add application icons for the Builder and generated game.
- Show recovery instructions when Windows Application Control blocks a build
  tool. Download READMEs also include Windows and macOS startup guidance.

## Upgrading from v0494

- Back up the entire `saves/` directory with the game closed. Rebuild into the
  existing game folder with the new Builder to retain saves, settings, language
  packs and custom assets. Downloading a Builder alone does not update the
  generated game. See [Updating an installation](../README.md#4-update-or-import-an-existing-installation).
- Use the game's **Progress Log** before switching save slots; switching does
  not capture unsaved gameplay.
- Use a complete `.arsave` export when moving an enhanced campaign between
  slots or installations. Plain SRAM remains useful for emulator interchange
  but does not carry the complete enhanced campaign.
- Upgrade v1 language packs through the Builder into separate v2 projects.
  The game offers upgrade guidance or native text for that session; it does
  not rewrite community packs automatically.
- Windows tools remain unsigned. If Windows blocks a bundled tool, see
  [Windows Application Control](desktop-packaging.md#windows-application-control).
- macOS may warn that it cannot verify the Builder or one of its tools. See
  [macOS Gatekeeper](desktop-packaging.md#macos-gatekeeper) for opening a trusted download.

## Still being tested

- Play-testing of combined regional rules is ongoing.
- A complete randomized campaign has not yet been play-tested.
- Northwall SIM events and Diorama routes still need play-testing.
- macOS Intel and generic Linux still need representative launch testing.
