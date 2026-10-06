# Next release — draft

Unreleased changes since **v0494** (September 16, 2026).
The next version number and release date are not assigned yet.
Reviewed through **cd0380a8** (October 5, 2026), covering all 187 commits
after the tag, including merged work. Related changes are grouped below.

**A major milestone for this release: the full USA campaign has been
successfully played from start to finish.** This completed playthrough used
native graphics and covered all six towns, all 12 acts, Death Heim and the ending.

**Extensive action-stage presentation work remains in 3D Diorama mode.** Every
level has been play-tested and issues have been recorded. Work includes manually
editing tile maps to fill the extra widescreen space, correcting background
policies, and fixing skybox rendering. Adding unique environmental effects is
also an ongoing per-level effort, with treatments currently implemented for
Fillmore Acts 1/2 and Bloodpool Acts 1/2.

This update adds customizable regional gameplay, ten independent save slots,
an optional modern town menu, 3D church interiors, a completion ocean scene,
and editable scenery effects with combined editor projects.
It expands widescreen and Diorama authoring, moves more rendering work onto the
GPU, improves frame pacing, and adds styled translation templates. The Builder
also starts and builds faster, with a clearer update workflow.

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
- Customize magic controls and costs, starting lives and spell inventory,
  score-earned lives, retry scoring, room timers, pickups and Action Mode
  availability, including European Action Mode behavior.
- Choose regional enemy stats, hitboxes, movement, attack timing and boss
  strategies independently of their artwork. Supported encounters include
  the Centaur, Minotaur, Wizard, Antlion, Pharaoh, Aitos dragon and skulls,
  Marahna plant/head and Viper, Northwall bosses, and Tanzra.
- Select regional terrain, enemy and item placements, hazards, Aitos mosaic
  patterns, scene music routing and Death Heim arrival rules. Room-dependent
  changes take effect at the appropriate entry boundary.
- Customize town construction costs and timing, development and recovery,
  fishing, earthquake behavior, house-loss accounting, population and level
  goals, lair reserves and rewards, SIM combat, and story prerequisites.
  Persisted lair history prevents switching rules from replaying rewards;
  older saves receive an acknowledgement when their missing history matters.
- Fix Japanese placements on statue-materialization entries, check room actor
  capacity before changing state, and correct regional hook and return
  behavior around stage completion and town actions.
- Support regional title art, Death Heim graphics, European item graphics,
  town symbols, pyramid details, actor art and music sequences from donor
  packages, with fallback when a selected media package is unavailable.

See [Regional settings](regional-settings.md) and [Regional media](regional-media.md).

## Save slots and campaign tools

- Keep **ten independent campaigns**, each with its regional settings,
  randomizer recipe and enhanced player name, through the **Saves** menu.
- Automatically import older `save.srm`, `save.ini`, or `actraiser.srm` saves
  from the selected data directory into Slot 1 on the game's first upgraded
  launch, with the originals retained. See [Upgrading from v0494](#upgrading-from-v0494).
- Set up regional rules for a new campaign before starting an empty slot.
- Switch campaigns or restart the game without closing and reopening its window.
- Export and import a complete campaign as a single `.arsave` file, including
  its regional settings and enhanced player name. Use **Export campaign** or
  **Import save** under **Saves → Advanced → Actions**.
- Choose save import and export locations through file pickers.
- Save your edits and restart in the same window with
  **Saves → Advanced → Actions → Apply and restart**, then choose **Continue**
  at the title screen to load the changes.
- Keep Advanced tools beside the slot list and clearly identify the active
  campaign they will change. **SNES Y** (default keyboard **A**) opens Advanced
  directly; previewing another slot does not change the edit target.
- Store every slot in its own numbered directory, with separate imports,
  exports and campaign backups. Automatically upgrade older slot layouts and
  preserve their original files and prepared new-game choices.
- Save SRAM, regional metadata and enhanced names as one recoverable snapshot.
  Interrupted writes preserve a complete previous campaign; invalid or missing
  managed metadata is reported for recovery instead of silently loading defaults.
- Persist confirmed regional edits for the active slot even before Continue,
  without advancing gameplay progress. Keep global boot overrides from
  accidentally applying to a different campaign.
- Prevent two game instances from owning the same save collection, and offer
  startup recovery for unavailable saves or interrupted slot switches.

See the manual's [Save slots](manual.md#save-slots) and
[Save editor](manual.md#save-editor).

## Experimental randomizer

- Save the randomizer's seed and options with each campaign. Continue restores
  the same setup without rolling it again.
- Independently randomize regional action and town rules for a new campaign.
  Enable **Regional action rules** and/or **Regional town rules** under
  **Randomizer → Seed**.
- Apply randomized stat scaling over the chosen regional base values and
  preserve the recipe, generator version and campaign identity across saves,
  imports and deterministic replays.
- Prepare a randomized game in an empty save slot without replacing an
  existing campaign; restarting before its first save retains the same setup.

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
- Improve town-menu navigation, confirmation and offering handoffs, message
  paging, timed notices, native menu sounds and the remappable Describe hint.
- Keep the settings overlay accessible on **The End** and **Best Player**
  screens so you can restart or exit after completing the game.
- Fix repeated soft resets from the Sky Palace and retain working audio and
  input when restarting or changing saves in the same window.

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
- Add Bloodpool Act 1 boss-fireball lighting, embers and lingering moonlit
  smoke. Launched shots retain their effects after the boss dies, and frozen
  shots keep their smoke and motion clocks aligned.
- Refine powered sword-beam lighting and particles, including their room
  priority. Native sword and beam damage behavior is unchanged.
- Keep projectile effects attached to the correct native actor through phase
  changes, pauses, source retirement and reused actor slots. This includes
  Marahna Viper lightning becoming a ground charge in both its original fight
  and the Death Heim rematch.
- Preserve scenery effects in Skybox-only rooms and align water accents,
  moon rays, torches and light receivers with their source layers. Refine
  Fillmore cave visibility, mist, wet surfaces and landing dust, and Bloodpool
  water ripples, submerged posts, distant waves and castle window light.
- Refine regional town buildings, construction stages, landmarks, rocks and
  vegetation, with improved tree shadows. The 3D models are selected through
  **Town 3D → Scene → Voxel town quality**.
- Add Aitos's octagonal wooden animal pens, including incomplete construction
  and their world-map presentation.
- Improve regional house silhouettes, straw huts, tents, stilt houses,
  windmills, factories, the Marahna temple, Japanese pyramid, ancient tree,
  palms, shrubs and rocks. Forests form joined clusters with better ground
  contact; canopy-shaped shadows replace oversized tile shadows.
- Preserve castles and ancient trees before town initialization, smooth town
  ground transitions, correct bridge depth and house ground patches, and fix
  speech-bubble placement, selected-town dimming and world sanctuary rings.
- Preserve Northwall snow after Sun miracles and prevent construction from
  freezing animated ground or disturbing cached terrain geometry.
- Restore the Sun miracle's warm tint and add light rays over the selected area.
  Control the added rays with **Town 3D → Scene → Effect lighting**, and their
  drifting motes with **Effect particles** in the same tab.
- Display locked-town terrain without prematurely revealing its cathedral.
- Choose Japanese HD title artwork under
  **Workshop → Assets → Title artwork → Artwork style**, with
  **Use the included HD title** enabled.
- Show HD title artwork throughout the animated title intro.
- Correct native Japanese title-edge coverage without shifting or stretching
  replacement artwork.

- Enable **Town 3D → Scene → Church interior 3D** to meet the people and receive
  offerings inside a lit stone church with a view of the surrounding town.
  It defaults to **Off** and requires **Simulation town 3D**.
- Add a modeled altar, soft shadows, warm window rays, cool doorway light and
  drifting dust while retaining the original people, dialogue, offering icons
  and controls.
- Keep native and enhanced text usable during church audiences, and correct
  selected-magic and offering-icon ownership across scene changes.

## Widescreen and Diorama rooms

- Give Death Heim room 1 separate A (faces) and B (completion) scenery and
  background policies, switching during the original black fade.
- Correct Death Heim's face and eye skybox placement, keep the faces clear
  above the cloud band, and extend the independently scrolling foreground
  water to cover the viewport in **Skybox only** mode.
- Add an optional completion ocean scene with luminous clouds, light shafts,
  waves, sun glints, subtle pixel water, cherubs and feathers. Preserve the
  native platform, hero, text, music timing and ending pace. Its controls live
  under **Action 3D → Scene → Death Heim completion scene** and require
  **Diorama 3D**. The scene applies its own backdrop and enclosure regardless
  of the player's skybox preference, without changing saved settings. Add
  platform reflections, ripples, spray and golden rim light, independently of
  Town/World 3D. A single on/off switch controls the scene; individual effects
  default to on and can be overridden in `settings.ini`.
- Add authored scenery tiles and regional room extensions for wider views.
  Every level has now been play-tested in Diorama mode, with extensive
  presentation corrections still to address across tile maps, background
  policies and skybox rendering.
- Extend Death Heim rooms 1–6 with authored scenery tiles, pixel masks, depth
  layers and background policies across US, Japanese and European terrain.
- Apply editor pixel edits and pasted tiles to native single-page backgrounds,
  including BG2, so authored backdrop changes appear in Diorama captures.
- Fix 16:9 edge capture, finite BG2 skybox bounds, skybox pixel aspect and
  early sprite clipping at the sides of wider Action 3D views.
- Keep more upper and lower room artwork visible within the vertical capture
  budget, preserve parallax at vertical limits, and stop automatic framing at
  projected horizontal room edges. Free Cam remains freely adjustable.
- Default Diorama mode to **Dynamic Cam**. Existing saved camera preferences
  continue to take priority.
- Let temporary Dynamic Cam orbit and zoom return smoothly to the authored
  pose, including held input and edge framing during streamed rendering.
- Fix animated Aitos waterfall skyboxes and their foreground alignment,
  Kasandora rock-priority seams and backdrop tuning, and Marahna Act 2 backdrop
  wrapping and color-plane retention.
- Improve filtering between coplanar priority bands, retain sprite occlusion
  and additive color, and fix original-view comparison transitions, fades,
  black backing and labels.
- Fix Northwall room transitions that could reject the captured frame.
- Remove the 1–5 layer-toggle hotkeys so screenshot shortcuts no longer
  accidentally hide Diorama layers. Use the layer settings to change visibility.

## Action scene and effects editor

- Save scenery, environmental effects and background policies together with
  **Save project** or **Ctrl/Cmd-S**, producing one `action-project.zip` for
  all rooms and regional terrain variants. **Load project** restores both
  `diorama-layers.ini` and `action-effects.ini`; the individual INI tools remain
  available. The **Unsaved changes** badge tracks scenery and effects together,
  including Undo/Redo. Invalid projects are rejected before replacing your work.
- Fix repeated project downloads, including saves of unchanged projects.
  Pending background-policy drafts identify their room/BG and require applying
  or discarding before saving, preserving other rooms' tile edits.
- Collapse the sidebar, toolbars or individual settings sections for more
  preview space. Open the same live settings through **Settings…** or right-click
  menus, with keyboard navigation and preserved policy drafts.
- Author BG1/BG2 edge fill, scroll motion, horizontal and vertical extents,
  and row-band policies through **Background policy**. Apply an undoable edit,
  preview it in **Shared renderer**, or restore the room's defaults. Policies
  save with the scenery, including independent Death Heim A/B scene settings.
- Label Death Heim room 1 B's browser preview as native artwork, with directions
  to the enhanced completion scene's in-game controls.
- Expand the standalone [action editor](https://github.com/DerrickGold/ar-recomp/blob/main/tools/action_editor/README.md) with
  regional terrain selection, rectangular and range selection, tile palettes
  with magnified previews, pixel painting and transparent masks, copy/paste,
  mirroring, repeat placement, bulk deletion, default reset and undo.
- Show projected viewport coverage and missing-edge guides; save per-room
  framing and extend scenery without the previous small fixed tile limit.
  Regional exports preserve tile edits and pixel masks.
- Render tile edits through native background capture so animated tiles,
  raster effects, priorities and room geometry stay aligned with gameplay.
- Add **Shared renderer**, a whole-room WASM/WebGL2 preview using the production
  C PPU and Diorama compositor. Load and explore rooms without capturing a
  running game, seek animation frames in either direction, and preview aspect
  ratios, pixel shapes, coverage, depth and backdrop modes.
- Add an environmental-effects workspace with source markers, family presets,
  light and particle controls, receiver selection, source-member editing,
  actor attachments, copy/paste and repeat placement. Move and resize supported
  fields on the map, disable native defaults, restore them, and preview edits.
- Author forest rays, cave/temple/tower atmosphere, marsh and castle fields,
  waterfalls, fire, lava, mist and combat accents through shared recipes.
  Export effects to `action-effects.ini` alongside room edits in
  `diorama-layers.ini`.
- Validate imported settings and preserve unrelated room data during scoped
  INI round trips. New bundled room content replaces `diorama-layers.ini` after
  preserving the previous file in a numbered `.pre-update-N` backup; identical
  bundled content leaves local edits in place.

The shared preview covers scenery and environmental sources. Actual actor/HUD
artwork, gameplay transitions, final CRT/heat effects and frame generation are
not all represented in that preview; see the editor guide for its current scope.

## Performance and fixes

- Add **Screen ratio → Auto** for action stages. It adapts extra rows or
  columns to the window, including paused resizing, while retaining the
  selected pixel aspect. Thanks to Simon W. Jackson for the contribution.
- Extend Auto to world navigation, simulation towns and the Sky Palace.
  Enhanced 3D worlds and towns can also expand vertically; native flat paths
  retain their scanout height. Keep menus and HUD graphics at their pixel aspect.
- Remove HUD-shaped shadows from scene lighting when the HUD is relocated.
- Enable buffered frame production for action stages and enhanced SIM towns.
  Select native frames against the completed output timeline, holding the last
  image when the next capture is late. Reduce pacing irregularities without
  skipping simulation updates, and keep camera input from repeatedly
  interrupting production.
- Reduce room-transition and pause stalls by retaining frame-generation GPU
  resources while clearing outdated frame history.
- Render supported action backgrounds, motion and effect projection on the
  GPU, retaining scene data across presentations and using a reference fallback
  when needed. Reduce repeated PPU work, tile-packet setup and CPU transfers.
- Reuse unchanged town models and world geometry, share mountain ground
  samples, and avoid full town rebuilds when native windmill tiles animate.
  Construction, insertion and removal update only changed town mesh ranges,
  moving unchanged portions on the GPU to reduce preparation and upload work.
- Reuse model bounds when switching between the Sky Palace and world navigation.
  Retain globe shoreline geometry when inland town changes leave its water
  sources and coastal coverage unchanged.
- Enable cached town-model shadow shapes by default to reduce repeated
  shadow preparation as the view moves.
- Retry GPU model rendering after temporary memory or upload failures, and
  keep retained model and shoreline caches synchronized with their source data.
- Avoid unused or unchanged SIM texture uploads, correct D3D12 atlas-transfer
  alignment, and reuse texture-upload buffers on Windows D3D12 to reduce
  repeated allocations. Improve cache recovery and upload accounting.
- Speed up initial loading of the globe's terrain.
- Improve action-rendering performance when showing extra rows above and
  below the original view with **Action 3D → Scene → Vertical extend**.
- Fix repeated or displaced terrain at extended viewport seams when GPU
  background capture and the original-view comparison use different cameras.
- Reject failed screenshot renders before readback and fix vertical fallback
  rows and capture-buffer leaks. Thanks to Simon W. Jackson for these fixes
  as well as the Auto action-canvas contribution.
- Reduce CPU use for original game audio through bounded APU/DSP batching.
- Preload replacement music and defer settings writes to reduce avoidable
  frame-time stalls while preserving shutdown and recovery behavior.
- Keep original music muted when switching between replacement music tracks.
- Fix macOS startup keyboard focus when entering fullscreen.
- Expand the performance overlay and logs with source-frame holds/skips,
  frame age, producer and presentation timing, and effect-fallback information.
  Add startup focus/input diagnostics for troubleshooting.
- Reduce Windows presentation stalls when performance logging is enabled by
  writing each periodic report in a batch.

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
- Share styled text and paging across town menus, church dialogue, title
  copyright text and credits, with clearer native text ownership and fewer
  fallback-font flashes. Correct cursor alignment and wrapped numeral styling.
- Improve Workshop error messages, uninstall confirmation, Windows file
  replacement and preview-worker paths/protocols.
- Provide updated authoring examples, validation references and a standalone
  text-template sample using the same renderer as the game and Workshop.

See [Language pack authoring](language-pack-format.md) for the v2 format and
[Workshop playback](language-pack-format.md#workshop-playback-and-tracing).

## Builder

- Add sidebar links to check GitHub Releases for updates and visit the project
  repository, opening in the system browser.
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
- Track included source dependencies so cached game builds are correctly
  invalidated after implementation or header changes.

## Recompiler, SDK and development tools

- Reduce generated C compile cost with per-file declarations, shared
  control-flow helpers and smaller compilation units. Schedule larger jobs
  first within the selected worker and memory budget.
- Improve conditional callback discovery, exact-width continuations, native
  return/stack ownership and interception of regional hooks. Correct live CPU
  flag handling at native boundaries and strengthen deterministic CPU/PPU and
  replay comparisons, including an edge-sequence digest.
- Extend public runner interfaces for HD Mode 7 scanout, owned background
  packets, room rendering and native/GPU comparison tools.
- Build the action editor with its embedded WASM previews by default, using
  pinned Emscripten 5.0.7. The generated editor works offline; source builds
  can still select `ACTION_EDITOR_WASM=off` for the JavaScript-only editor.
- Add explicit maximum-quality profiles and verified graphics-backend selection
  to replay comparisons, with broader D3D12, Vulkan and Metal coverage.
- Report measured average presentation FPS and 1% lows in the frame-pacing
  analyzer, including held frames and scene transitions. These rates measure
  completed backend presentations. Document display, session and power-state
  requirements for reproducible live comparisons; see
  [Performance overlay](performance-overlay.md#source-cadence-traces).
- Organize application, host, rendering, regional, save, text and editor code
  by subsystem, with explicit private state and resource lifetimes. Normalize
  authored source formatting and track move/format commits for useful blame.
- Expand native, generated-code, regional-session, save-recovery, editor,
  renderer, shader, packaging and sanitizer regression coverage. Ensure test
  assertions remain enabled in optimized builds.
- Add local source-manifest, ownership, style, Python, JavaScript and shell
  checks. `make check-release` includes the optimized suite;
  `make release-checked` runs those checks before packaging. Ordinary
  `make release` remains packaging-only. Installer development requires Go 1.25+.
- Add `make release-remote <host> [<host> ...]` to split release targets across
  SSH build hosts and optional `localhost` without SSH. Hosts build concurrently
  from the same working-tree snapshot, uploaded once per host. Idle hosts take
  the next target from a shared queue, with each target assigned exactly once.
  It streams progress by host, retains combined and individual host error logs
  with consistent UTC timestamps from the coordinator,
  retrieves verified downloads, reuses caches, and cleans up temporary files
  after failure or handoff. A failed host stops its peers and preserves existing
  local releases. Host configuration stays outside the repository.
- Pin AppImage tools and runtimes to named upstream releases with verified
  checksums, so fresh build hosts do not depend on mutable continuous builds.
  Reuse valid cached downloads and discard failed or invalid transfers.
- Strip physical source and compiler cache paths from runtime archives when
  remote builds use symlinked caches, while retaining their debug symbols.
- Recover pinned Linux SDK packages from Debian's official archive when they
  leave a live mirror, verifying the original locked version, checksum, and size.
- Expand public regional comparisons, unused-content research, RAM/ROM and
  symbol maps, projectile and native-audio references, and reproducible town
  model comparisons. Archive internal plans, benchmarks, migration reports and
  validation journals outside the distributed documentation.

## Upgrading from v0494

- Back up the entire `saves/` directory with the game closed. Rebuild into the
  existing game folder with the new Builder to retain saves, settings, language
  packs and custom assets. Downloading a Builder alone does not update the
  generated game. See [Updating an installation](https://github.com/DerrickGold/ar-recomp#4-update-or-import-an-existing-installation).
- Launch the rebuilt game with that data folder. It automatically adopts
  `saves/save.srm` or `saves/save.ini` into **Slot 1**; if neither exists, it
  adopts `saves/actraiser.srm`. A sole SRAM/INI save retains its format; if
  both exist, the configured backend selects the active one. No manual
  conversion or **Import save** action is needed for this first-launch upgrade.
- The active save and companions move to `saves/slots/01/`; exact originals
  remain in `saves/legacy-layout/`. Choose **Continue** to resume. Existing
  managed slots also upgrade automatically. Conflicting or invalid files
  stop migration and report an error instead of being overwritten.
- If the old data is in another installation, use the desktop Builder's
  **Import previous installation…** first, or move the complete `saves/`
  directory into a fresh destination with the game closed. An older archive
  stores it under `utils/saves/`. Automatic save adoption checks only the
  selected game data directory; it does not search other installs or move
  saves between portable and per-user storage. See
  [Upgrading older saves](manual.md#upgrading-older-saves).
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

## Release status and remaining testing

- **End-to-end completion confirmed:** the full USA campaign has been
  successfully played through with native graphics, including all six towns,
  all 12 acts, Death Heim and the ending.
- Diorama mode has been tested on every level and issues have been recorded.
  Extensive presentation work remains: manually editing tile maps to fill
  widescreen space, correcting background policies, and fixing skybox rendering.
- Unique environmental effects are an ongoing per-level effort. Fillmore
  Acts 1/2 and Bloodpool Acts 1/2 have received their treatments; adding effects
  across the remaining levels is still in progress.
- Regionalization implementation is complete; combined regional rules and
  media still require play-testing.
- Localization and translation authoring are ready. Players can create and
  distribute their own language packs.
- A complete randomized campaign has not yet been play-tested.
- Windows, macOS arm64 and Steam Deck have been tested and boot successfully.
- macOS Intel and generic Linux still need representative launch testing.
