# ActRaiser 2 scene and task formats

These findings describe the USA cartridge with internal title `ActRaiser-2
USA`, map mode `$31`, and checksum `$EFCC`. They are a partial description;
the complete command set and all level behaviors have not been validated.

## Scene asset stream

The asset stream begins at CPU address `$86:8000` (file offset `$068000`).
Its parser is at `$80:C7B3`; the scene search is at `$80:CE0C`. The live
source pointer is at WRAM `$52`, and the selected scene is at `$051A`.

Records contain a two-byte header, variable-length commands, and a zero
command terminator. The examined search compares the second header byte
against the scene number. The first header byte's broader purpose has not
been established. Industen's captured entrance selects scene `$14`; its
asset record begins at file offset `$0681E1` with header `$00,$14`.

The framing is:

```text
record = header_byte, scene_number, command*, $00
command = opcode, operands[operand_count(highest_set_bit(opcode))]
```

This describes asset setup, not the complete playable level. Actor creation,
movement, collision and event execution live elsewhere.

The highest set bit selects a command family. The native skip routine gives
these operand sizes, excluding the command byte:

| Highest set bit | Operand bytes |
|---|---:|
| 7 | 6 |
| 6 | 6 |
| 5 | 7 |
| 4 | 4 |
| 3 | 1 |
| 2 | 3 |
| 1 | 5 |
| 0 | 6 |

This is the same command framing used by AR1's asset stream at `$05:8000`.
The families cover graphics, palettes, metatile definitions, maps and scene
setup. Matching framing does not prove that every flag, resource reference,
destination, or inherited-state rule has the same meaning.

Resource references must use AR2's pointer conversion at `$80:CE78`, including
its alternate bank-window path at `$80:CEA8`. Treating the three operand bytes
as a physical file offset, or applying AR1's LoROM conversion unchanged, is
not sufficient. Scene profiles are selected through `$81:CEB6`.

## Background maps

The horizontal-strip builder at `$80:C4E2` computes a metatile address from
`row * width + column + map_base`. The tile expansion at `$80:C562` reads
one byte per metatile and selects an eight-byte definition containing four
8-by-8 tile words. This gives 16-by-16 metatiles.

For the captured Industen entrance, BG1's map starts at `$7E:8000`, with
144 by 48 metatiles (2304 by 768 pixels); definitions start at `$7E:2000`.
The strip builder's width, map-base and definition-base fields are at
`$055C`, `$0564` and `$056C`, indexed by layer. Full-room decoding still
requires comparison against native streaming across camera positions.
At the captured entrance camera, row-major expansion matches all 896 visible
tile cells on each of BG1 and BG2 against the resident VRAM maps.

AR1's action maps use page-major storage. AR2's observed row-major addressing
therefore needs its own input adapter, even though both can produce the same
bounded world-tile lookup interface.

## World atlas and destinations

The world initializer at `$82:AB17` loads a complete 1024-by-1024 Mode 7
atlas. Both its 128-by-128 byte tilemap and its 256 chunky 8-by-8 tiles
use the shared Quintet LZSS asset encoding, with a 16-bit decompressed-size
header of 16,384 bytes:

| Asset | CPU address | File offset |
|---|---|---|
| Initial world tilemap | `$C4:1AA0` | `$041AA0` |
| Progression variant tilemap | `$CA:7830` | `$0A7830` |
| Mode 7 tile graphics | `$94:DE55` | `$14DE55` |

The initial tilemap and tile graphics match all 16,384 bytes of their
respective low/high VRAM planes in a native world-map capture. The alternate
map matches its native WRAM buffer. `$82:CB30` applies progression-dependent
changes; baking the initial atlas does not implement those changes.

The world map enables direct color through `$2130`, so the tile's pixel byte
is a color value rather than a CGRAM index. Its `BBGGGRRR` bits expand to
5-bit channels `RRR00`, `GGG00`, `BB000`; Mode 7 contributes no tile-palette
bits. This agrees with the direct-color conversion in
[Snes9x's graphics implementation](https://github.com/snes9xgit/snes9x/blob/master/gfx.cpp).
CHR tile `$00` consists entirely of `$80`, the map's deep ocean identity.
The adapter retains native indices so presentation can join that ocean to
another continent without classifying terrain by its displayed RGB color.

The destination table at `$81:8000` (file `$018000`) contains 22 records of
eight little-endian 16-bit words, followed by a zero first word:

```text
x_min, x_max, y_min, y_max,
flags_and_scene, progress_list_pointer, unlock_gate, completion_gate
```

The native hit test at `$82:B1D7` uses half-open coordinate bounds. Selection
retains the one-based record identity in `$0971`, table byte offset in
`$0975`, and marker coordinates `(x_min + 8, y_min + 8)` in `$0977/$0979`.
The low byte of `flags_and_scene` identifies the entry scene. Fourteen
records are action entries; eight select the town/world scene `$0A`.
Some records occupy the same location in different progression states.
Their identities and gates must remain distinct when building a catalog.

The action-entry scenes are `$14,$16,$18,$1A,$1E,$21,$28,$2B,$2F,$3A,$3D,
$41,$49,$51`. This is an entry-point catalog, not a count of all internal
rooms or proof that their enemies, transitions and bosses execute correctly.
The loader still needs game-specific startup state and native task coverage.
The atlas adapter deliberately leaves unlock policy and execution support to
its caller; a prototype can expose the geography while enabling only checked
entry points.

For a shared globe, the two games can keep their own map coordinates and
destination records. Place each map's stereographic chart on a fixed globe
orientation, with AR2 rotated 180 degrees in longitude from AR1. Apply that
same placement to terrain, markers and picking. Camera rotation then changes
the view without moving either continent or changing native stage coordinates.
This presentation layer does not require combining the two games' task scripts
or treating AR2 destinations as AR1 simulation towns.

## Executable task scripts

Actor/event tasks also contain ordinary 65816 instructions. `COP` instructions
invoke the native handler at `$80:82C3`, which dispatches through `$80:82DB`.
Arguments can follow the COP signature inline; the handler advances or rewrites
the interrupted return address. Those bytes are data, not a guaranteed
continuation at `PC + 2`.

Examples verified in the boot task path:

- `COP $9C` reaches `$80:8D08`, allocates a task through `$80:94E9`, and
  consumes a three-byte code pointer for the new task.
- `COP $C8` reaches `$80:910B`, stores the current task's next code pointer,
  updates the interrupt return PC, and returns through `RTI`.
- Some handlers consume the interrupt frame and use `RTL` to return to the
  task scheduler instead of immediately continuing the interrupted script.

The scheduler at `$80:95C9` resumes tasks through stored native code pointers;
the observed pointer fields are `$7F:0004,X` and `$7F:0006,X`. Task execution
therefore depends on native state, register widths, stack behavior and update
ordering, beyond the scene's asset records.

For a concrete example, the task fragment at `$82:8101` begins with:

```text
02 9E 1C 93 82 28 00 E0 FF
```

This is `COP $9E`, a three-byte task pointer `$82:931C`, and two signed
16-bit offsets, `+40` and `-32`. Its handler at `$80:8D55` allocates a task,
installs that code pointer, and offsets its position; the horizontal offset
also follows facing. The next byte after these nine bytes is an ordinary
65816 `LDA` instruction. A task decoder must understand both kinds of data.

## Visibility and actor lifetime

The native visibility pass at `$80:9765` walks the list headed by WRAM
`$087F`, following actor word `+$2C`. It compares the actor's bounds
(`+$10/+$14` horizontally, `+$12/+$16` vertically) with render camera
`$0540/$0544`. The horizontal window is 256 pixels and the vertical window
is 224 pixels. Actor flag `+$04 & $8000` records the offscreen result.
Visible actors enter the depth-sorted list consumed by `$80:9ECB`.

This flag affects gameplay as well as rendering. `COP $1E` at `$80:87CC`
can wait for an offscreen actor to become visible. The visibility pass can
remove actors bearing flag `$0002` through `COP $A2`. Industen's enemy reset
task at `$82:D6A6` restores its spawn coordinates, waits, and only rearms
when the spawn becomes offscreen again. Enlarging just the PPU canvas misses
these lifecycle rules; disabling cleanup entirely would change them too.
The removal call at `$80:9843` saves the draw-list cursor with `PHY`; its
`$80:9845` continuation must execute `PLY` before visiting the next actor.

Individual sprite pieces have another horizontal test at `$80:9F72`;
queued effect sprites use `$80:9FDB`. Both accept a biased X coordinate
in `[0,272)` and subtract 16 before packing OAM. Their vertical tests remain
separate. Widened rendering needs exact signed positions alongside packed
OAM: its nine-bit X cannot distinguish every left-margin position from a
right-margin position. The prototype supplies this as renderer metadata,
preserving native OAM packing and its 128-slot capacity.

At a finite horizontal level edge, the presentation capture moves inward so
its full width is backed by decoded tiles. BG1 and signed OBJ metadata receive
the same pixel offset. BG2 uses the displayed foreground's shift scaled by
its native `$05A2` parallax ratio, with separate compensation for the capture
apron. Native `$0538/$0540` camera values stay unchanged. The draw list uses
that shifted window too, including
Diorama's capture apron and extra vertical rows. After each actor's draw-list
decision, the adapter restores `+$04 & $8000` from the original 256-by-224
bounds before native iteration advances at `$80:976E`. Thus rendering an
actor in the extra view does not make it gameplay-visible. Native blinking
still bypasses that bounds test; actors removed by the pass are not rewritten.
The expanded cull still controls render lifetime for actors bearing `$0002`.

`COP $1E` and other consumers of the offscreen flag retain the original
activation and rearm behavior throughout play, including the orb and stone
entrance. `$095B` only covers the orb phase; `$0951` enables the player
controller after the stone phase. Neither flag is needed for an extra-view
activation exception: the unchanged native wait handler uses native visibility
before and after control is enabled. Its retry timer, task continuation and
out-of-world bypass remain intact. Benefic's `$82:F590` actor can be drawn
at the entrance, but wakes only when native gameplay visibility allows it.
Its COP branches and stored callbacks remain explicit generation roots.

## Progression and retry state

Room progression is executable task logic, not a uniform scene increment.
For example, Industen's `$82:E577` task starts a camera-lock and actor-cleanup
sequence. Later phases wait for the player to reach the far end and then
the upper exit; `$82:E627` requests scene `$15` when player Y is below 56.
Benefic's `$82:F33B` first checks player X >= 808 and Y < 224, then changes
camera bounds, queues graphics and music work, and starts further tasks.
Those operations can happen while the current scene ID remains `$16`.
Widescreen margins must not move these native trigger coordinates.

`COP $00/$01` implements a script subroutine call/return: the handler keeps
the continuation in the actor's bank-7F fields. The shared routine at
`$82:9B41` initializes the boss health display. Availability of the scene's
asset record alone does not cover these calls or the boss's behavior tasks.

On death, `$82:8C2C` decrements the BCD lives word at `$091B`, restores
health from `$091F` to `$091D`, restores checkpoint fields, and queues the
current scene through `$0518`. `$0921` is the magic-use counter, not lives.
With fewer than two lives, the native task instead requests the world/town
scene `$0A` without decrementing the counter. The prototype intercepts that
request at the native loader entry `$80:B88C`, before the cartridge world UI
runs, and reports a terminal return-to-world event to the host. The imported
globe then owns navigation again. This does not save campaign progress or
interpret boss completion flags.

## Compatibility boundary

Asset command framing and resolved graphics are useful candidates for shared
code. Converting geometry or asset declarations does not preserve a game's
collision rules, movement, enemy logic, event timing or progression by itself.
An AR1-to-AR2 task conversion would need to reproduce those semantics, not
merely translate command numbers. A common scene representation can retain
game-specific behavior references while each game executes its own logic.

Keep per-game asset interpretation and task execution behind adapters. Share
the bounded command reader where its framing is identical, and share resolved
graphics, finite world-tile access, camera/view calculations and explicit HUD
surfaces. AR1's current scene structure contains AR1-specific video profiles,
raster presets and asset inheritance; it is not yet a neutral scene model.

If a common Workshop authoring format becomes useful, derive it from the
validated overlap and retain game-specific gameplay profiles and behavior
references. Neither game's raw cartridge format needs to become the common
format. Wider views and interchangeable HUD styles do not require translating
AR1's native gameplay into AR2 tasks.
