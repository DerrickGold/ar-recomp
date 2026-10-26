# ActRaiser 2 in the main application

The optional desktop prototype adds AR2's continent and two playable entrances
to the main ActRaiser Recomp application. Industen and Benefic run native AR2
movement, enemies, collision and audio, with native or diorama presentation.
This is preliminary support, not a complete second campaign.

## Build and play

1. In the Workshop's main ROM selector, select ActRaiser (USA) and ActRaiser 2
   (USA) together. You can also include the Japanese, European English, French
   and German AR1 ROMs: up to six files in one build. Names and order do not
   matter; supported hashes identify each release, including copier headers.
2. Build normally. The builder imports the AR2 Master, continent and native
   level data, generates the optional module, and packages it beside the main
   game. Regional AR1 ROMs add graphics/music packs and Workshop text reference
   projects. Regional text references retain their existing reference-only
   contract; they do not become US-runtime translations automatically.
3. Reach the world map and press **C** or the controller's **Back** button to
   turn the same globe toward AR2. The existing palace, movement, zoom and
   rendering settings remain active. With World navigation 3D off, this
   switches the native flat map instead. C returns to the last palace position
   on the other continent; there is no separate continent browser.
4. Fly the palace over a destination with the normal arrows/D-pad, then use
   the mapped **B**, **Y**, or **Start** button to enter. **A/X** retain the
   native map zoom controls. Industen and Benefic are enabled; other places
   display an unavailable label until their native paths are supported.
   Selection areas are 64 by 64 map pixels around the native destinations;
   overlapping areas select the closest destination to the palace.

The cartridge initializes offscreen: entering/restarting an AR2 level does
not display or play its ENIX boot sequence. The shared globe zooms and spins into the destination, then the level's
native entrance and fade run. Returning reverses the globe approach and restores
the palace at its previous position. Direct entry requests the same shared palettes and graphics
as normal world-to-level travel, including START's gold effect/text palette.
Normal mapped gameplay controls then work inside the level.
**D/Tab** changes the shared native/diorama preference, **M** or controller
**Back** returns to the same palace view, and **R** restarts the entrance.
**Escape/F1** opens the common settings menu and pauses the level. Display,
action-camera, layer visibility, effects, HUD scale, CRT and world preferences
come from the same settings used by AR1. Right-drag, mouse wheel, middle-click
reset and mapped analog camera controls also use the shared camera owner.
AR1-specific campaign, save and editor controls remain unavailable during an
AR2 visit. Return to AR1 to use those adapters.

**F2/C** in an AR2 level uses the shared snapshot action. It writes
`snapshots/ar2_*.{wram,vram,cgram,oam,highoam}.bin`, `.ppu.json`, `.cpu.json`
and a composed `.ppm` screenshot under the current run directory (or `saves`
when run directories are disabled). It works during native pause and the host
menu; captures requested during loading wait for a composed level frame.

A later build can reuse its saved AR1 USA ROM while adding newly selected
regional or AR2 files. Existing edited language projects are preserved.
The generic content-import tools also remain available; separate import steps
are unnecessary when using the main ROM selector.

Source builds use `cmake/ImportedGames.cmake` when
`game-assets/imports/ar2-world.argame` exists. Reconfigure CMake after first
importing it. `AR2_CONTENT_PACK` can point at another locally prepared pack.
The CMake development path needs Python and Go; the packaged Workshop uses
its bundled `snesbuild`, compiler and private runtime archive.

## Ownership and extension points

- `installer/internal/romset` prepares the unordered selection before writing
  imports. Each content owner validates and installs its own bounded format.
  Identical repeated imports are harmless. A filesystem failure can leave
  completed individual imports; publication is atomic per content item.
- `ar2-world.argame` contains a versioned header and the complete normalized
  1.5 MiB USA ROM payload, checked against its supported SHA-256. Native scripts,
  scene loaders and audio still address original ROM data. This local game
  pack and generated native code belong to the user's build, not the public
  Workshop distribution. It differs from the Master-only sprite pack.
- `src/actraiser2/module` owns a private runtime and coroutine. Its shared
  library exports only `ArImportedGame_GetApi`; CPU, RAM, PPU, APU and generated
  functions remain private. The ABI lends frame planes and PCM to the host
  until the next synchronous step. It does not expose runtime globals or SDL
  objects. Known module filenames come from the application, never the pack.
- `src/sim/world_nav/world_navigation_travel.c` keeps each continent's palace
  coordinates around the native movement controller. Its value snapshot travels
  through the normal frame capture; the renderer never reads live travel state.
  The existing world projection rotates the planet before applying shared
  camera inspection, with a short eased turn and input suppression in transit.
- `src/host/imported_world_travel.c` enters a level after the AR1 producer has paused,
  retains the live session, and updates from the main application loop using
  its event pump, window and render device. The library stays loaded until
  session teardown; there is no child process or nested game/event loop.
  Globe descent/ascent changes captured presentation metadata, not AR1 RAM.
  `imported_game_session.c` owns a worker for module loading, initialization
  and hidden startup frames while the main thread animates the descent. The
  same worker retains the coroutine for gameplay and snapshots; fibers never
  move between threads. Graphics, playback and window events stay on the host.
  The normal 90-tick descent may wait on black at its end, matching AR1.
  Cancellation releases the runtime on its owner before session teardown.
  Return checks CPU state, work RAM and native frame count before resuming
  AR1. Audio is paused and resumed at the same position. Each AR2 entrance
  currently starts fresh; it does not alter AR1's saves or campaign progress.
- `src/sim/world_nav/present_world_nav_imports.c` adds the atlas to the normal
  globe at the opposite longitude from AR1. Terrain and the native palace
  share the same projection and world lighting controls. Record identities remain distinct even
  when two destinations share coordinates.
- `src/actraiser2/ar2_presentation.c` adapts native surfaces directly to the
  production compositor; it no longer uses a capture/replay fixture or local
  camera defaults. Native priority/depth ownership stays game-specific. Common
  camera controls, render options and CRT policy are captured each draw. Native
  ticks/audio advance independently of the shared host refresh limiter.
- `Ar2VideoFrame.hud` is separate from the world planes. Native BG3 HUD tiles
  are removed from scenery, while Benefic's BG3 mist remains a world effect.
  Ownership is applied before each scanline. The HUD map's bottom four tile
  rows are the cartridge's frame border and stay out of the scaled HUD.
  `screen_objects` separately captures the START/PAUSE actor's OAM range and
  removes those sprites from scene composition. Those prompts stay centered
  horizontally regardless of the world-camera offset, depth or HUD scale.
  This prepares for interchangeable HUD presentations; selecting an AR1 HUD
  for AR2 is not implemented yet.

AR1 and AR2 retain separate native gameplay and script adapters. Shared code
handles presentation, world-tile access, packaging and the native module
build. See [scene and task formats](actraiser-2-data-formats.md) for why asset
stream similarities do not establish gameplay compatibility.

## Current limits and validation

The prototype respects the configured screen/pixel aspect and fits the HUD
independently. Auto uses the current drawable size, and scenery capture adds
up to 128 columns on each side. At finite room edges the capture shifts inward
using the complete decoded tilemap, so the widened view is populated immediately
instead of revealing empty columns as the native camera moves. Sprites use the
same shift; the gameplay camera and HUD remain independent. Small rooms and
explicit free-camera zoom/orbit can still expose scene borders. In the checked rooms, the actor
visibility window and both sprite-piece emitters now follow the actual
horizontal capture, including its shift at finite edges. Exact signed OBJ coordinates prevent sprites in
the added columns from wrapping across the screen. Native vertical bounds,
camera triggers, proximity checks, blink timing and the 128-slot OAM limit
remain in effect. Diorama includes actors in its larger capture apron so the dynamic camera cannot
reveal scenery with missing actors. Native presentation uses its display margin.
Actors revealed by that extra view remain dormant throughout the orb and stone
entrance until the native player controller enables input. Rendering stays wide;
the actor's native visibility-wait script resumes afterward. Death retries that
skip the orb follow their immediate control handoff without an artificial delay.

Sharing preferences does not infer AR2-specific content: AR1 room layer
manifests, inferred terrain relief/models, authored action effects and motion
interpolation have no AR2 equivalents yet. AR2's imported geography retains
its native artwork, and native gameplay runs at its original tick rate.

Only the Industen (`$14`) and Benefic (`$16`) entrances are exposed on the
globe. Industen's following boss room (`$15`) now has world/HUD separation
and native/diorama capture support. Native death/retry restores health and
decrements the actual lives counter in both entrances. Coverage also includes
the checked boss activity and Benefic's native arena trigger and music handoff.
Losing the last life returns to the imported globe at the native world-loader
boundary. The module reports a terminal return event, and the host closes it;
entering a level again starts a fresh visit.
Boss defeat, stage completion, further rooms, campaign saves and map
progression need further coverage. Unsupported dispatch or a native frame
exceeding its work budget returns an error to the host. Windows fibers and
POSIX contexts provide desktop backends; execution has been validated on
macOS. Android is not supported by this prototype.

Validation includes the mixed six-ROM import (with copier headers and
misleading filenames), repeated module open/close across both entrances,
native movement, native/diorama planes and audible PCM, and an integrated AR1 → AR2 → AR1 trip
with unchanged AR1 CPU/RAM/frame state. Packaging tests move an application
and its optional module, delete the build libraries, and execute the result.
World-travel tests cover input masking, expanded selection bounds and overlaps,
co-located records, interrupted turns, the descent/ascent transform and restoration
of the saved palace coordinates. Capture tests cover both finite edges and small
rooms; local-ROM checks compare the original terrain and require the additional
columns to be populated before camera movement. Integrated
GPU checks navigate the palace to both enabled entrances, hide boot frames,
pause in the common menu, change the shared renderer, and rotate back to AR1.

For local integration automation, `AR_IMPORTED_TRAVEL_AT` accepts comma-separated
host-tick numbers at which to press C through the same travel controller. The
ordinary input replay still drives palace movement and destination entry.
`AR_IMPORTED_REPLAY` is a level-visit script of `present-number command value`
records: `input` (runner joypad bits), `view` (shared diorama switch), `menu`,
`snapshot` (the shared snapshot action), `retry`, and `return`.
`AR_IMPORTED_FRAMES` bounds a visit; `AR_IMPORTED_TRACE`
records only presented level frames, allowing a check that no boot scene leaked.
`AR_IMPORTED_TRANSITION_CAPTURE` takes a path prefix and writes `-descent.bmp`
and `-ascent.bmp` snapshots midway through the shared world transitions.

The ROM-free ABI check is `actraiser_imported_game_module` in CTest when a
module is built. Its executable optionally accepts a local AR2 ROM as a
second argument for the native gameplay checks.
`actraiser_imported_game_session` uses the local ROM to check worker startup,
cancellation while loading, repeated visits, owner-thread stepping and a failed open.

`actraiser_ar2_native_progression` uses the already prepared local ROM and
the same private objects as the shipping module. It checks a frozen
256-pixel RAM/pixel trace, ROM-derived START palettes in both entrances and
render modes, widened visibility and OBJ pixels, resizing while
paused (including native pause/resume and centered text), diagnostic files,
HUD/frame-border separation, native retries, last-life returns with fresh re-entry, the next-room
loader and boss activity.
Entrance checks cover both phases before player control, a visible enemy's
sleeping task and later activation, and unchanged timing for native-width play.
The developer harness `actraiser_ar2_native_probe` accepts an explicit input/fixture script
and records poses, scene/camera state, lives and sprite pixels in each margin.
Health, position and pending-scene writes in these tests are controlled
fixtures, not evidence of an unassisted level playthrough. Fixture writes and
RAM pointers are never exported by the shipping module; its snapshot operation
writes diagnostics on the runtime's owner thread through the shared SNES formatter.

Go import tests can use
`AR_ROM_SET_DIR` pointing to local `ar.sfc`, `ar-jp.sfc`, `ar-eu.sfc`,
`ar-fra.sfc`, `ar-ger.sfc` and `ar2.sfc` fixtures. No ROM data is needed for
ordinary CI tests.
