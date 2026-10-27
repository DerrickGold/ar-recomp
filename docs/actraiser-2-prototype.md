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
not display or play its ENIX boot sequence. The shared globe uses AR1's original
Advent rotation, zoom, fade and music, then the level's native entrance runs.
Returning uses AR1's incoming palace fade and Sky Palace music, at the saved
position. Direct entry requests the same shared palettes and graphics
as normal world-to-level travel, including START's gold effect/text palette.
Normal mapped gameplay controls then work inside the level.
**D** changes the shared native/diorama preference. **System → Return to world**
returns to the same palace view; **System → Retry level** restarts the entrance.
Keys and controller buttons use the shared input mapper: R, M, Tab and Back
are no longer intercepted by level-only shortcuts.
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

AR2 remains a development preview. The normal Builder instructions describe
AR1 and its regional ROMs; missing AR2 imports are hidden from the Workshop,
and the character setting is hidden without valid character assets. Explicit
ROM selection and the developer `game-import` command still support prototype
work. Existing imports remain visible for inspection and repair. An AR1-only
source build requires no AR2 ROM, import pack, generated code or native module.

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
  ABI 5 carries a shared view request with horizontal and vertical budgets;
  frame metadata identifies the native rows and visible camera origin within
  the expanded capture.
- `src/sim/world_nav/world_navigation_travel.c` keeps each continent's palace
  coordinates around the native movement controller. Its value snapshot travels
  through the normal frame capture; the renderer never reads live travel state.
  The existing world projection rotates the planet before applying shared
  camera inspection, with a short eased turn and input suppression in transit.
- `src/host/imported_world_travel.c` enters a level after the AR1 producer has paused,
  retains the live session, and updates from the main application loop using
  its event pump, window and render device. The library stays loaded until
  session teardown; there is no child process or nested game/event loop.
  `actraiser_world_travel.c` borrows the existing AR1 coroutine at its suspended
  yield. The original `$02:847C` owns the descent; `$02:98B7` uploads Advent
  and Sky Palace through the normal authentic/enhanced music routing. Return
  reuses `$00:82D1`'s incoming fade. This replaces the separate 90-tick transform
  and invented reverse spin. The borrowed calls retain native stack ownership;
  CPU and work RAM are restored before the original world continuation resumes.
  `imported_game_session.c` owns a worker for module loading, initialization
  and hidden startup frames while the main thread animates the descent. The
  same worker retains the coroutine for gameplay and snapshots; fibers never
  move between threads. Graphics, playback and window events stay on the host.
  Native animation timing stays fixed. The shared action-loader audio hold
  follows on black; a cold import may extend that black endpoint.
  Cancellation releases the runtime on its owner before session teardown.
  Return checks the saved CPU and work RAM checkpoint before resuming AR1.
  AR1 audio runs during travel and pauses during imported gameplay. Each AR2 entrance
  currently starts fresh; it does not alter AR1's saves or campaign progress.
- `src/sim/world_nav/present_world_nav_imports.c` adds the atlas to the normal
  globe at the opposite longitude from AR1. Terrain and the native palace
  share the same projection and world lighting controls. Record identities remain distinct even
  when two destinations share coordinates.
  Imported map art uses AR1's shared Scale2x preparation (2048-square output)
  and linear sampling in both globe and flat presentation. The globe chart is
  enlarged 15% about its centre; terrain, camera focus and the palace marker
  share that transform, including eased continent turns. Native movement,
  destination bounds and flat-map coordinates retain their original scale.
  Deep ocean is transparent only on the globe, with its RGB retained beneath
  zero alpha to avoid black fringes when filtering shorelines.
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
horizontal capture, including its shift at finite edges. Finite-edge capture
keeps the full apron populated; a separate camera origin frames the displayed
width and keeps the player visible at the entrance. Diorama's shared vertical-extension setting
adds up to 128 total rows and reallocates unused edge coverage to the opposite
side. Flat Auto uses its drawable-derived vertical budget. Terrain and actor
culling follow these rows; exact signed OBJ coordinates prevent sprites from
wrapping vertically or horizontally. Native camera triggers, proximity checks,
blink timing and the 128-slot OAM limit
remain in effect. Diorama includes actors in its larger capture apron so the dynamic camera cannot
reveal scenery with missing actors. Native presentation uses its display margin.
Actors revealed by that extra view retain their native offscreen flag after
the expanded draw list is built. Their visibility-wait and rearm scripts use
the original 256-by-224 gameplay view throughout play, including the orb and
stone entrance. Camera- and player-range encounter scripts also retain their
native coordinates. Death retries that skip the orb follow their immediate
control handoff without an artificial delay.

BG2 follows the displayed foreground camera at the level's authored parallax
ratio (`$05A2`), including the widened view's stops at either edge. The larger
diorama capture apron does not add background motion. Scaling absolute camera
positions preserves the native rounding phase, avoiding one-pixel jitter while
the foreground is held still. Native timed background motion remains intact.
`actraiser_ar2_native_scroll` checks Industen and Benefic in native, wide,
diorama and resized views, compares native gameplay traces, and checks actual
Industen background pixels remain fixed until the foreground starts moving.

The native IPL/block and BRR-catalog audio uploaders acknowledge every byte
through the SPC ports. The module identifies those routines through the
versioned audio policy so the runtime does not add its normal 128-sample command
dwell to each transferred byte. The original senders, acknowledgements, data
and queued-port ordering remain active; ordinary music/SFX commands keep their
existing pacing. Industen's boss sample upload previously spent 136,269,504 APU
cycles waiting on that extra dwell. It now takes 1,095,776 cycles; one local
replay fell from about 3.3 seconds to 32 ms with identical native encounter
timing. The activation regression bounds emulated work rather than wall time
and requires audio during the encounter. Upload completion still reanchors
the audio timeline before the next native frame.

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
with restored AR1 CPU/RAM state and native Advent/Sky Palace uploads. Packaging tests move an application
and its optional module, delete the build libraries, and execute the result.
World-travel tests cover input masking, expanded selection bounds and overlaps,
co-located records, interrupted turns, transition composition and restoration
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
It also checks capture-only resizes while frozen: CPU, WRAM and PPU snapshots
remain identical and no audio is produced. The native entrance sweep repeats
these refreshes before and during native pause, checking an exact pixel round
trip and unchanged CPU, WRAM and APU time in every supported research entrance.

`actraiser_ar2_native_progression` uses the already prepared local ROM and
the same private objects as the shipping module. It checks a frozen
256-pixel RAM/pixel trace, ROM-derived START palettes in both entrances and
render modes, widened visibility and OBJ pixels, resizing while
paused (including native pause/resume and centered text), diagnostic files,
HUD/frame-border separation, native retries, last-life returns with fresh re-entry, the next-room
loader and boss activity.
Vertical checks compare native rows pixel-for-pixel while resizing a paused
capture, require real terrain/OBJ pixels in the added rows, keep HUD pixels
inside their native band, and check extra-view enemies remain dormant until
the native gameplay view reaches them.
Projection checks cover Industen's native floor camera at `height - 224`:
its last scanline is a guard row, which must not disable the floor clamp or
hide the available upper rows. The same check covers the raised-platform camera.
The October 26 play snapshots identified missing `$82:EB5D` enemy/linked-part
cleanup and `$82:87AF` player-task continuations. Targeted fixtures now require
actual dispatch through those paths, their sibling `$87BA`, and the cleanup
of all three linked enemy parts. The newly visible upper Benefic actor's
`$82:F75C..F810` continuations and their `$82:F95E..F97A` projectile task are
also generated and exercised. An upward camera fixture checks that the extra
rows follow the player and retain actors above and below the original screen.
A double-jump/glide replay covers `$82:86B4` and the `$82:870D` skid reported
by the later play snapshot, through its animation wait and return to idle.
Only invulnerability is supplied as a fixture to prevent enemy interruption;
movement, collision and task selection run natively from controller input.
The `$809605 -> $828894` failure is the sword attack selected from the second
glide phase. Its entry and animation continuations are now generated. Two
controller replays cover releasing the sword back into glide and landing during
the swing; both must complete the skid and return to idle. The release check
starts watching the glide return only after the attack button is released.
The later `$82975A -> $82975D` snapshot identified missing item-release
continuations. The item selector's animation loops, collection callbacks and
reward-popup cleanup are now generated. A regression places a native container
within sword range and selects the reported item type 3, then attacks and walks
into the released item. It requires actual dispatch through release, collection
and cleanup, checks that the reward is applied once, and checks that no item or
popup task remains. Position, item type and invulnerability are fixtures; task
PCs and callbacks are not written by this test.
Entrance checks cover both phases before player control, a rendered enemy's
sleeping task after control, later native activation, and unchanged timing for
native-width play.
`actraiser_ar2_native_activation` compares Industen's scroll gate and Benefic's
player-position gate in native, wide, diorama and resized views. Explicit
position, matching camera-scroll and invulnerability fixtures place the player
before each gate; ordinary walking/jumping then starts the encounter. The checks
require identical native player/scroll traces, boss-start frames and camera
locks. They also reproduce the `$829690 -> $829692` play failure: selecting
an existing task's cleanup boundary must remove it through native `COP $A2`
and dispatch its final `RTL` successfully.
The `$809843 -> $809845` offscreen-removal return is also generated. Its
native `PLY` restores the draw-list cursor after `COP $A2` removes an actor.
Checks in all four views move an existing actor outside the capture and set
its removal flag, then require dispatch through `$809845`, a correctly linked
actor list, and unchanged draw-list membership for the surviving actors.
The Industen replay continues through both boss attack patterns in all four
views, requiring the third projectile's `$82EE8B -> $82EE8D` continuation and
the other attack's `$82EE1E -> $82EE20` return to the attack loop. Boss tasks
and random state are left to native execution.
The Benefic approach dwells on the upper platform before jumping across its
gate, exercising the `$82:D9C5` and `$82:F985` patrol continuations. A separate
controller replay holds and releases the sword to exercise `$82:8A59` and its
charged projectile task; invulnerability is the only gameplay fixture there.
The developer harness `actraiser_ar2_native_probe` accepts an explicit input/fixture script
and records poses, scene/camera state, lives and sprite pixels in each margin.
`rows` changes the vertical budget; `watch`/`require` verify native dispatch
coverage without adding a probe interface to the shipping module.
`watch-block` counts executed basic blocks, `background` captures BG2 alone,
and `timing` writes per-step wall time and APU cycle counts. Set
`SNESRECOMP_APU_PROFILE=1` to add port synchronization counters and the last
port-writing function; profiling itself increases wall time.
Health, position and pending-scene writes in these tests are controlled
fixtures, not evidence of an unassisted level playthrough. Fixture writes and
RAM pointers are never exported by the shipping module; its snapshot operation
writes diagnostics on the runtime's owner thread through the shared SNES formatter.

### Additional entrance investigation

With the optional content pack installed, configure a separate test build with
`BUILD_TESTING=ON` and build `actraiser_ar2_native_probe`. The probe compiles
the entrance/capture policy with `AR2_NATIVE_RESEARCH`; the shared game module
still accepts only Industen and Benefic. Both use the same generated native
code, scene initialization and runtime. The research policy permits the 14
action entrances in the ROM catalog and Almetha's checked connection to room
`$34`, while map bounds and raster ownership checks still apply. It does not
validate every connected room automatically.

Run an entrance sweep against your own headerless USA ROM:

```sh
python3 tools/ar2_probe_entrances.py /path/to/build/actraiser_ar2_native_probe \
  /path/to/ar2.sfc --output /tmp/ar2-entrances
```

The default sweep runs all entrances in native, widened and diorama capture
modes for 901 frames each. Use `--scene 18`, `--mode native`, `--frames`,
`--jobs` or `--timeout` to narrow or extend it. Each case has a controller
script, CSV state/audio timing, stderr, a machine-readable result, and an
exit snapshot containing WRAM, VRAM, CGRAM, OAM, PPU and CPU state. On a native
failure the probe saves diagnostics before closing the runtime, together with
the last completed image. The script does not overwrite earlier case folders.

`report.json` separates missing native code, timeouts, absent control, missing
separated capture and movement failures. Successful cases must still have a
living, controllable player in the requested level's checked scene path at the
end. Native state traces are compared across completed view modes. The sequence walks, jumps,
swings and presses pause/resume without writing health, position or task state.
This is entrance coverage, not a full playthrough, boss-clear check, visual
approval or campaign progression test. A diorama capture also does not by
itself verify the host's final 3D projection.

When a native dispatch is missing, retain its ROM identity, source/target,
M/X widths and snapshot. Decode the task and the native COP handler before
adding a generated entry. Inline operands are not executable continuations;
fixed-operand handlers such as `$96/$9C` consume three-byte task pointers,
`$80` consumes an animation byte, and `$C8/$8E` have no inline operands.
Never generate a blanket root at every `COP + 2`.

The October 27 coverage pass fixed the original eleven failing entrances and
the follow-on scheduler, actor, animation and COP-return gaps exposed by their
runs. All **14 destinations now complete the 901-frame sequence in all three
capture modes (42 cases)**, with audible PCM, separated layers, movement,
full health before gaining control, and verified native pause/resume. Native
gameplay traces match across modes. No position, health or task writes are
used. The original failed destinations are required to execute by the
`actraiser_ar2_native_entrances` CTest regression; they are not merely present
in the generated registry.

Almetha's normal movement crosses its first doorway at `$84:B10F`, from scene
`$3A` to `$34`. The regression verifies that exact scene path, the destination
room's separated capture and control, and pause/resume after the transition.
The same row-major map adapter and raster ownership checks handle both rooms.

Repeat this coverage with:

```sh
ctest --test-dir /path/to/test-build -R '^actraiser_ar2_native_entrances$' --output-on-failure
```

The source is
`tests/ar2_native_entrance_test.py`, sharing the sweep's controller script and
reporting code. New entrances remain research-only pending longer traversal,
remaining connected rooms, boss fights and completion, and host projection
checks. Later gameplay can still expose native paths outside this short test.

Go import tests can use
`AR_ROM_SET_DIR` pointing to local `ar.sfc`, `ar-jp.sfc`, `ar-eu.sfc`,
`ar-fra.sfc`, `ar-ger.sfc` and `ar2.sfc` fixtures. No ROM data is needed for
ordinary CI tests.

### Maintenance workflow

`src/actraiser2/ar2_scenes.inc` is the shared scene catalog for the globe,
native capture adapter and probe. Each profile names its world entrance and
whether it is production or research-only. Connected rooms retain their own
scene IDs. The ROM's catalog remains authoritative for destination labels.

Recent coverage additions live in `recomp/ar2/coverage.json`: 373 observed or
audited COP edges, with source/target PCs and M/X state. Observations include
the actual scene and a diagnostic-log digest. The marked sections in bank CFGs
are deterministic outputs; handwritten annotations outside them remain authored.
Earlier investigation entries are retained in place until their evidence is
migrated. To add a failure from a retained entrance-sweep report:

```sh
python3 tools/ar2_coverage.py record --rom ar2.sfc --report /path/to/sweep/report.json
python3 tools/ar2_coverage.py sync
python3 tools/ar2_coverage.py check --rom ar2.sfc
cmake --build --preset play
```

`--expand-audited-cop` on `record` follows only the explicitly audited operand
signatures and task pointers. Other handlers require investigation. Keep the
original probe reports and snapshots locally; no ROM bytes enter the manifest.
The coverage check rejects stale CFG output, invalid widths, unknown banks,
wrong ROM identity and COP operands inconsistent with the supplied ROM.

CMake tracks generator Go sources/module files, CFGs, coverage and preparation
scripts, and the optional pack. The preparation stamp hashes these inputs and
checks that generated outputs still exist. Unrelated CMake configuration skips
Go/code generation; generator changes rebuild it automatically. Discovery stays
at configure time because the generated translation-unit list can change.
`ar2_prepare` is the explicit preparation target. Deleting a generated source
also triggers rediscovery; adding/removing the optional pack reconfigures the
module targets. An AR1-only build does not invoke AR2 preparation.

Capture-only refresh is ABI version 6. The module retains the start of the
last scanout and replays its video IRQs inside an owner-thread video checkpoint,
restoring live PPU/DMA, CPU bus state, WRAM and pacing estimates afterward. It
never resumes the game coroutine or renders audio. The checkpoint is an
in-memory video replay facility, not a resumable game save. The host requests
it only when capture geometry changes while stopped.
