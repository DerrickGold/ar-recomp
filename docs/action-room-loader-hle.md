# Action-room standalone loader and editor parity

Last verified: 2026-08-22, USA ROM.

This document is the implementation contract for a self-contained action-room
background loader. Its primary consumer is the action layer editor. The editor
authors `diorama-layers.ini`; the game remains a read-only consumer of that
configuration and does not become a second authoring path.

## Decision

We have enough ROM knowledge to load and render an arbitrary action room
without a save state and without running gameplay. The loader can be a pure,
deterministic function of:

```text
ROM + room + camera + render phase + diorama-layers.ini
```

The earlier stateful concerns are resolved:

- all 49 action rooms and their cumulative graphics scripts decode;
- the 28-byte video profiles provide the native screen masks, colour math,
  tilemap sizes, Mode-1 state, parallax, animation, and page-cycle fields;
- action character animation is a 4 KiB VRAM snapshot followed by fixed-size
  phase uploads, not an unknown frame composer;
- continuation rooms reconstruct the same source window as their capture owner,
  so an offline room build is self-contained;
- `0402` and `0403` have an exact four-page/five-frame BG2 cycle;
- the complete persistent-raster census reduces to ten deterministic presets;
- video-profile byte `+27` (`$F2`) has no registered/reachable consumer and is
  not a rendering input.

Save states and a side-loaded game process remain useful as differential
oracles. They are no longer loader dependencies.

## Implementation status

The stable two-background scene milestone is implemented:

- `src/action/action_room_scene.c` is the pure immutable ROM loader. It owns
  cumulative asset-script replay, command-3 profile resolution, exact common
  priority/attribute merge, finite tile lookup and expansion, character-phase
  reconstruction, the `0402`/`0403` page sequence, and room raster identity.
- `ActionRoomScene_BuildFrameState` resolves native camera/parallax, character
  and page phases, profile registers, and every visible line of R1-R10 scroll
  or mosaic state. `ActionRoomScene_RenderNativeFrame` is the stable 256x224
  Mode-1 BG1/BG2 compositor: tile lookup, flips, transparency, priority,
  TM/TS, stable screen masks, mosaic, brightness, and supported colour math.
- The action editor exporter links that module and emits schema
  `actraiser-action-bg-v4`, including the normalized waveform, the final 8 KiB
  character-decompression workspace, and a C-rendered golden frame for every
  room. It no longer owns a private ROM decoder.
- The editor exposes a dedicated **Native frame** reference with camera and
  frame controls. Its JavaScript raster/compositor verifies itself against the
  room's C golden hash when opened. **Diorama 3D** consumes the same exact
  camera-local BG captures before routing pixels into authored virtual bands;
  the full-room map remains the painting surface.
- The game registers the same immutable ROM bytes at boot. The room scene's
  cumulative-ROM BG1/BG2 page maps and metatile definitions are now the default
  production finite-world source; `AR_ACTION_ROOM_SCENE_HLE=0` selects the
  exact staged-WRAM source control.
  The bootstrap still stages the resident VRAM ring, gameplay data, actors and
  callbacks. Action CHR/CGRAM, maps/metatiles and video profiles now use
  guarded CPU-facing HLEs; the non-presentation stages stay native. The ring
  remains the authentic-pixel
  safety oracle, and a scene load or dimension failure falls back to the
  ordinary live-WRAM source. The default promotion follows the exact 12-target
  matrix and natural Fillmore acceptance route described below.
- Setting
  `AR_ACTION_ROOM_SCENE_COMPARE=1` compares each newly published live
  `ActionBgWorld` against every tile produced by `ActionRoomScene`. During
  scanout it also compares the immutable frame's scroll, mosaic, TM/TS,
  TMW/TSW, colour-math, mode and BGSC fields with the live PPU immediately
  before each visible line, preserving HDMA timing in the oracle. It reports
  the first mismatch and never changes gameplay, provider eligibility, or PPU
  state. `AR_ACTION_ROOM_SCENE_COMPARE_VERBOSE=1` logs accepted raster holds
  and raises mismatch reporting from the first failure to at most 128 reports;
  it has no effect unless the main comparison gate is enabled.
- `ActRaiserActionBg_StageRoomSceneLayer` now materializes the exact native
  command-4/5 output image: BG1/BG2 pixel dimensions, only the active
  page-major map bytes at `$7E:8000/$C000`, and the byte-swapped 2 KiB
  metatile table at `$7E:2100/$2900`. It deliberately preserves every byte
  outside those ranges. `AR_ACTION_ROOM_STAGE_COMPARE=1` is an independent,
  read-only live oracle for that image, so staging acceptance does not also
  opt into the broader transient frame/raster comparator.
- The action-only `$02:B363` command-5 and `$02:B3EB` command-4 handlers now
  execute `ActRaiser_LoadActionMetatiles` and `ActRaiser_LoadActionMap` by
  default. They consume the live VM operands, advance its `$A2`/Y cursor,
  preserve the native CPU/status/RTS and decompressor-scratch contract, and
  stage the same collision/ring-visible WRAM ranges. `AR_ACTION_ROOM_LOAD_HLE=0`
  is the exact native control.
  This is a guarded HLE rather than an unconditional replacement: the new
  `hle_func_if` generator seam retains the decoded ROM body for title/SIM raw
  loads, BG3 selectors, wrong M/X modes, and any command shape outside the
  audited 49-room action domain. Gameplay, audio, and OBJ commands remain in
  the native `$02:B1F7` VM.
- The action-only `$02:B28E` command-7 and `$02:B330` command-6 handlers now
  execute `ActRaiser_LoadActionCharacters` and `ActRaiser_LoadActionPalette`
  by default. The character path covers the audited compressed 8 KiB banks
  and 4 KiB dialog-font upload, including the native `$7E:6000` workspace,
  LZSS scratch, VRAM ports and CPU contract. The palette path copies the
  audited 128-byte slices through the native CGRAM ports. Both consume the
  live VM cursor, and `AR_ACTION_ROOM_GFX_HLE=0` is their exact native control.
  Their read-only `hle_func_if` guards require the action domain, DB/M/X mode,
  known destinations and sizes; command 7 also verifies the compressed asset
  header before redirecting. Title/SIM raw shapes and any unknown operands
  retain the generated handler body.
- The action-only `$02:B4E8` command-3 handler now executes
  `ActRaiser_ApplyActionVideoConfig` by default. It reads the live profile
  operand and the same 28-byte `$02:893E` record, then applies the native
  BG mode/screens, windows, colour math, common priorities, parallax ratios,
  fade/page/character-animation state, timer and renderer-nonoperative `$F2`.
  Its read-only guard requires D/DB=0, native M1X0 action entry, and one of the
  43 profile IDs present in the 49-room stock script census. Unsupported modes,
  non-action calls, unknown operands, and `AR_ACTION_ROOM_VIDEO_HLE=0` execute
  the generated native body. Commands 2/1/0 and the enclosing VM stay native.
- The live comparator and production provider own separate `ActionBgWorld`
  caches. This is required when the room-scene source and both compare gates
  are enabled together: the observer runs after provider binding but
  before scanout, so sharing a cache would replace the immutable publication
  with live WRAM, decode both sources every frame, and flood successful
  full-world reports. A ROM-free regression mutates WRAM after binding and
  proves that the retained provider snapshot remains unchanged and that stable
  redraws publish neither cache again.
- ROM-free tests pin inheritance, profile priority, tile lookup, phase/page
  resolution, every raster preset, native compositor priority/colour-math
  cases, full-world and frame-register match/mismatch behavior, and failure on
  dimensional drift. The optional stock-ROM census loads and renders all 49
  rooms, while retaining the pinned 30 animated rooms, two page-cycle rooms,
  17 raster-bearing rooms, and five forced-BG2-priority rooms.

The live acceptance set is now byte-exact for all R1-R10 raster families. The
original six-preset matrix compares 7,049,476 pre-scanline PPU registers with
zero mismatches; natural routes add R1, R4, R7, R8, and R10 coverage through
the otherwise unreachable boss transitions. R2 pins an authentic one-frame
hit-stop hold. The complete Death Heim route additionally pins 1,351 frames
that retain the last R8 table while the ending script continues. Each hold is
accepted only when every live raster value exactly matches a previously
accepted immutable table.
BG3 HUD, real OBJ streams, fades, and gameplay-object-driven window timelines
remain outside this standalone background contract by design.

A natural Fillmore route in `runs/20260822-134834/` covered 6,227 action frames
and four room-scene loads with zero load failure, provider fallback, tile
mismatch, finite-edge failure, or raster-register mismatch. Its combined
diagnostic gates compared 396,087,296 full-world tiles before exposing the
shared-cache performance defect described above. Because every immutable/live
comparison was exact, the route remains parity evidence; the isolated-cache
regression closes the provider-ownership and performance defect without
requiring another complete playthrough.

Post-fix bounded acceptance run `runs/20260822-140544/` enabled the immutable
provider and both comparators for 1,193 frames. It recorded four total world
publications, exactly two full-world comparisons (one per BG, 81,920 tiles),
and two success lines; 2,370 provider/observer layer-frames and 1,337,865
raster registers remained exact with zero fallback. This is the intended
steady-state cost instead of republishing and re-reporting every layer on every
frame.

The production handoff remains intentionally incremental. The renderer obtains
complete finite BG tile words from the immutable room scene, while the two
background asset commands now stage collision/ring-visible WRAM through their
CPU-facing HLEs and its graphics commands stage CHR/CGRAM through PPU-port
HLEs. Command 3 now applies the video profile through its own guarded HLE. The
enclosing `$02:B1F7` VM and commands 2/1/0 stay native, so this does not absorb
level objects, actor initialization, audio, or gameplay. Every redirected
routine retains its exact native handler body whenever a read-only guard
rejects an invocation.

## Engineering audit (2026-08-22)

The action-room HLE, finite-world provider, virtual-band path, and editor
exporter were audited together rather than as independent features.

Code ownership and naming:

- The command-7/6, command-5/4, and command-3 HLEs now share one internal set
  of explicitly named direct-page, long-indexed, and native-stack bus
  primitives. The helpers still call `cpu_read*`/`cpu_write*`; they do not
  bypass the emulator's observable CPU, PPU-port, or debug contracts.
- Production raster identifiers describe their behavior (`Bg2MosaicWave`,
  `Bg2ParallaxPerspective`, `DualBgOpposedWaves`, and so on). Their explicit
  values 1-10 preserve the R1-R10 research labels and exporter schema.
- The standalone editor exporter is strict C11 with warnings enabled. It no
  longer hides warnings behind `-w`, and the shared runtime's MSVC warning
  pragmas are guarded so Clang/GCC builds remain warning-clean.

Rendering portability:

- Virtual worlds supply only 16-bit SNES tilemap words and optional band IDs
  through fixed-width CPU callbacks. The ordinary CPU PPU remains responsible
  for CHR fetch, palette, priority, windows, mosaic, and color math.
- Callback pointers, compiled classification caches, and overlay metadata are
  host-owned frame state outside the serialized PPU register/VRAM/CGRAM
  ranges. Savestates therefore contain no process pointers or backend data.
- Virtual bands become the same ARGB CPU overlay surfaces already consumed by
  the diorama's SDL abstraction. No Metal object, shader branch, byte order,
  or row-pitch assumption was added. The audit build compiles and links the
  full game natively on macOS and as `x86_64-windows-gnu`; the committed
  Metal/MSL, Vulkan/SPIR-V, and D3D12/DXIL shader families are unchanged by
  this CPU-side feature.

Runtime cost:

- Authored cell rules used to be scanned for every virtual tile sample. A
  room can contain 512 spans and scanout performs tens of thousands of
  provider samples per frame, making that representation a latent quadratic
  cost as authoring expands. Binding now compiles the authentic-priority,
  metatile, and ordered-cell rules into one byte per finite-world 8x8 tile.
  Scanout classification is O(1); the cache rebuilds only when the world
  serial or a classification-field hash changes, including same-address live
  editor edits. Geometry-only virtual layers skip classification entirely.
  Diagnostics report cache builds and hits.
- Immutable room-scene maps and metatile tables have stable addresses by
  contract. Repeated publication now takes a pointer/key identity fast path
  instead of comparing the full map plus 2 KiB definitions every frame. Live
  WRAM publications retain byte comparison because they can mutate in place.
- The native-ring preflight still compares the authentic 33x28 tile viewport
  before binding. That bounded work is intentional: it detects streamer lag
  and gameplay-time tile patches and is the safety condition that permits the
  finite provider to own authentic pixels. CPU/PPU staging loops run only at
  room transitions and likewise remain bus-accurate rather than introducing a
  backend-specific bulk-write shortcut.

Verification for this audit: the strict exporter loaded all 49 rooms with no
failure; all 79 configured tests passed under the headless driver (the two
GPU-device tests skipped as designed); the changed world, virtual-layer, and
room compositor targets passed ASan/UBSan; and the hermetic Windows x86-64
build compiled and linked all 247 translation units.

## Scope and parity boundary

The baseline contract is the complete native **background scene**:

- BG1 and BG2 character data, palettes, metatiles, maps, dimensions, scroll,
  tile priority, flip, transparency, main/subscreen membership, colour math,
  mosaic, and persistent HDMA/raster behaviour;
- selectable animation, BG2-page, and raster phases;
- the same virtual-plane classification, Z, painter order, alpha, camera, and
  projection that the game consumes from `diorama-layers.ini`.

Gameplay object simulation is deliberately outside the loader. Consequently,
an arbitrary-room preview does not need enemies, collisions, player state,
stage transitions, audio, or a valid save. BG3 HUD and OBJ can be optional
display overlays; they must not be confused with the authoritative BG1/BG2
authoring result.

Transient object-driven effects are a separate optional timeline. Boss iris
wipes and other windows that exist only while a gameplay object advances are
not required for a stable room preview. If the editor later exposes those
moments, they should be named preview phases rather than hidden gameplay
simulation.

## Authoritative load order

The standalone loader should perform these steps in order:

1. Validate the room against the 49-room action registry.
2. Replay the room's cumulative asset-script graphics commands from
   `$05:8000` (commands 7 through 4): two 8 KiB BG character banks, the extra
   character bank, BG palette, both metatile tables, and both page maps. Retain
   the final 8 KiB character-decompression workspace because some raster
   builders deliberately inherit bytes that the upload left there.
3. Read the room's command-3 video-profile index and apply its 28-byte record
   from `$02:893E`.
4. Merge the action tile-word mask `$ECFF` and the profile's common attributes
   while expanding metatiles into complete, finite BG1/BG2 worlds.
5. If character animation is enabled, copy the selected 4 KiB source window
   from the reconstructed character image and apply the requested phase.
6. Resolve the requested BG2 page-cycle phase, if present.
7. Resolve BG scroll from the explicit camera and the profile's native
   parallax ratios.
8. Build the room's persistent raster preset for the requested game-frame
   phase and apply it per scanline.
9. Render through the native PPU rules: main/subscreen winners, priority,
   windows, mosaic, and colour math.
10. Split the resulting source BGs into authored virtual bands and render the
    same Diorama geometry/order/alpha model used by the game.

The ROM's action entry executes the same major authorities in this order:
`$02:B1F7` asset VM, `$00:92CB` level data, setup/rebuild routines,
`$02:BAF5` character snapshot, then normal NMI scanout. The HLE omits gameplay
records but preserves all background authorities.

## Video-profile fields that the editor must export

`$02:B4E8` indexes a 28-byte record at `$02:893E + 28*profile`.

| Bytes | Native result | Standalone use |
|---|---|---|
| `+0..+3` | `TM/TMW`, `TS/TSW`, `CGWSEL`, `CGADSUB` | main/subscreen membership, window gating, colour math |
| `+4` bits 0..2 | common `$2000` tile priority for BG1/BG2/BG3 | OR into every expanded tile word for that BG |
| `+4` bit 3 | `$8F=$1000` | no action-room profile currently sets it |
| `+5` | BG1SC/BG2SC size bits; bases remain `$60/$70` | native tilemap topology and page selection |
| `+6` | `BGMODE` | native Mode-1 state for action rooms |
| `+7..+12` | six nibble-split ratios | native H/V parallax |
| `+13..+18` | fade/brightness state | stable preview initializes the settled value |
| `+19..+22` | BG2SC page-cycle counters | `0402`/`0403` four-page animation |
| `+23..+24` | character target/stride/count/cadence | character animation |
| `+25..+26` | action timer | non-rendering; omit from standalone scene |
| `+27` | direct-page `$F2` | write-only in registered code; omit from renderer |

The current ROM-backdrop exporter hardcodes BG1 `$1000` and BG2 `$0100`
character-bank attributes. That is necessary but not sufficient. Video-profile
byte `+4` also forces the common tile priority bit. Every action profile forces
BG3 priority; these rooms additionally force BG2 priority and will be wrong if
the exporter omits it:

```text
0102  0103  0205  0602  0604
```

This is a P0 editor-parity correction. It changes native classification
fallbacks as well as the flat composite.

## Character animation: closed contract

`$02:BAF5` is the missing operation. When animation is enabled it reads exactly
`$1000` bytes from VRAM word address `$DA` (`$0000` or `$1000`) into
`$7F:B800-$BFFF`. It is a snapshot of the already-loaded character sheet; it
does not synthesize frames.

The normal tick at `$02:BC56` computes:

```text
phase  = $E0 & $DF
source = $7F:B800 + phase * $E1
size   = $E1
target = VRAM word $DA
```

and `$02:AF30` performs that upload. In the profile:

- `+23 bit 7` chooses target `$1000` instead of `$0000`;
- `+23 bits 4..6`, shifted left seven, are the frame stride in bytes;
- `+23 low nibble - 1` is the phase mask;
- `(+24 & $7F) - 1` is the tick mask, so the observed inputs `2/4/8`
  update every 2/4/8 game frames.

If raw `+24` has bit 7, `$02:BAF5` clears that bit and deliberately skips the
snapshot. This is a runtime continuation optimization: the natural prior room
already populated `$7F:B800`. It is not an offline dependency. The cumulative
ROM asset reconstruction produces a source window byte-identical to the
canonical capture for every continuation family:

| Canonical capture | Rooms with the same target window | Target byte offset |
|---|---|---:|
| `0102` | `0103` | `$2000` |
| `0202` | `0203`, `0205` | `$0000` |
| `0301` | `0302` | `$0000` |
| `0303` | `0304`-`0306`, `0704` | `$0000` |
| `0404` | `0405`, `0406` | `$0000` |
| `0501` | `0502`, `0503` | `$2000` |
| `0504` | `0505`-`0507`, `0508`, `0706` | `$0000` |
| `0602` | `0603`, `0604` | `$2000` |
| `0605` | `0606`, `0607` | `$0000` |

`0508`/`0706` start a new runtime capture but share the same reconstructed
source bytes as the `0504` family. The important implementation rule is
simpler than the historical route: after reconstructing any animated room,
snapshot that room's own target window and select a phase.

Runtime evidence includes 1,024 16-bit `$02:BAF5` readback writes covering the
whole destination on `0202`, plus a separate `0102` run that pins VMADD
`$1000`. Static disassembly pins the size and both addresses.

## BG2 page-cycle animation

Only `0402` and `0403` use it. Their BG2 map is an authored 2x2 grid of four
256x256 pages. `$02:BC27/$02:BC34` advances `$C7` through
`$04,$08,$0C,$00` and `$02:AEAE` writes BG2SC `$74,$78,$7C,$70`.
Each page remains active for five game frames.

The editor should expose a four-position page selector and an optional play
clock. An explicit phase is the authoring authority; exact first-visible-frame
alignment is only needed by an automated native-start comparison.

## Persistent raster presets

A 49-room direct-load census identified every persistent background callback.
Rooms absent from this table have no persistent HDMA preset. All scroll targets
are SNES register addresses; `$02:96D4` is the ROM's 256-byte waveform table.

| ID | Rooms | Builder | Target/effect | Deterministic contract |
|---|---|---|---|---|
| R1 | `0104` | `$02:92D8` | ch2 BG2HOFS `$210F` | 111 two-line samples, waveform phase `frame/2`, plus frame scroll |
| R2 | `0201` | `$02:9204` | ch2 BG2HOFS `$210F` | 127 static lines then 96 one-line fixed-point perspective samples from `2*(frame+cameraX)` |
| R3 | `0202`,`0203` | `$02:931A` | ch2 BG2VOFS `$2110` | 127 static lines then a 32-line descending vertical ripple |
| R4 | `0405` | `$02:9382` | ch2 MOSAIC `$2106` | 112 two-line samples; low-byte `frame/4` phase, with the inherited 16-bit source index selecting the ROM window after the waveform |
| R5 | `0401` | `$02:93DF` | ch2 BG2HOFS `$210F` | nine bands with counts `63,16,8,8,16,8,16,40,80`; upper motion uses frame/camera fractions |
| R6 | `0601`,`0605` | `$02:945E` | ch2 BG2HOFS `$210F` | six upper bands plus 32 one-line perspective samples; depends on frame and camera X |
| R7 | `0608` | `$02:94E9` | ch2 BG2HOFS `$210F` | 111 two-line waveform samples with an increasing phase step |
| R8 | `0701` | `$02:9549` | ch2 BG2HOFS `$210F` | eleven fixed bands with opposing frame-speed fractions |
| R9 | `0702`-`0707` | `$02:9665` | ch2 BG2HOFS `$210F` | 79 lines at 0, 64 at `cameraX/4`, 96 at `cameraX/2` |
| R10 | `0708` | `$02:95E9` | ch2 BG2HOFS + ch3 BG1HOFS | 111 two-line dual-wave samples: `wave+frame` and `-wave+$40` |

`0402` and `0403` do **not** use the `0401` raster preset; they use the BG2
page cycle above. `0405` is the only other Aitos room with a persistent raster
callback, and it is a mosaic effect rather than scroll.

The table storage is part of the contract. R1-R6 and R8 use `$7E:6000`, R7
uses `$7E:6800`, R9 uses `$7E:7000`, and R10 uses `$6000` for BG2 plus `$6800`
for BG1. The compressed-CHR loader always stages through
`$7E:6000-$7FFF`. R1, R2, R3, R7, R9's first band, and R10 overwrite only one
of the two Mode-2 scroll bytes in selected entries, so the untouched byte is
the corresponding byte from the last decompression. This seemingly stale data
is visible PPU input and is exported explicitly in schema v4. R4 is Mode 0;
R5, R6, and R8 write complete 16-bit scroll samples.

R4 has a separate original-ROM quirk. `$02:9382` reads only the low byte of
`$88`, shifts it twice, stores that byte to DP `$00`, and then reloads the
scratch pair as a 16-bit X index. Settled action mode leaves DP `$01 = $01`, so
the source is `$02:97D4+phase`: the 256-byte ROM window immediately following
the nominal `$02:96D4` waveform, including adjacent code bytes. The editor
descriptor exports that window separately. On natural entry to `0405`, the
first callback still carries the preceding room's scratch high byte and emits
a flat `$02` MOSAIC table; the first visible R4 scanout retains that table.
`ActionRoomSceneFrameRequest.raster_entry_frame` names this one-frame state,
while ordinary editor previews use the settled deterministic pattern.

Visible presentation frame N normally scans the table built during action tick
N-1, including that tick's camera X. The callback belongs to the action-update
path rather than scanout, so hit-stop or an ending fade can retain the whole
last-built table for one or many displayed frames. A standalone preview
advances deterministically once per requested frame. The game-side oracle
keeps the last fully accepted raster table and accepts a hold only when every
live raster value matches it; it reports these separately as `raster-holds`
rather than hiding them as approximate tolerance.

Natural entry also pins bootstrap inputs that a stable room preview does not
need. Northwall `0605` builds its first visible R6 table with camera X `$01C0`
before settling at `$0188`. Death Heim rematches `0702`-`0707` use entry
cameras `$0010,$0000,$0000,$00D0,$0000,$0090`. Their provisional profile
BG2SC values settle to one-page `$70`; `0708` likewise changes `$72 -> $70`
after its first setup frame. After the final boss, `$00:F5F0-$F619` switches
the hub to BG1SC/BG2SC `$64/$74` and leaves the last R8 HDMA table frozen
through the ending fade. These transient values are modeled by entry metadata
and a script-owned page override, while the editor continues to show the
stable authoring state by default.

The builder should generate register values for each scanline into a small
immutable raster state. The PPU renderer then consumes that state. Replaying
the CPU routines per preview frame would add state without improving fidelity.

## Standalone scene interface

A useful shared interface is:

```c
typedef struct ActionRoomSceneRequest {
  uint8_t group, map;
  int camera_x, camera_y;
  uint32_t game_frame;
  int animation_phase; /* -1 derives from game_frame */
  int page_phase;      /* -1 derives from game_frame */
  bool include_bg3;
  bool include_obj_reference;
} ActionRoomSceneRequest;
```

The result should own immutable reconstructed assets plus resolved per-frame
state:

```text
BG worlds + dimensions + tile words
CHR/CGRAM
video profile and PPU registers
native scrolls
character-animation phase
BG2 page phase
per-scanline raster registers
resolved diorama plane/band configuration
```

The editor should control camera, animation phase, page phase, and raster time
directly. “Play” merely advances `game_frame`; pausing must leave a completely
reproducible scene.

## Implementation priority

### P0 — one exact scene authority (implemented)

1. Extend the existing ROM exporter/decoder to emit the profile index and all
   rendering-relevant video fields, including the common priority bits.
2. Add the character-animation source/phase and four-page cycle to the exported
   room descriptor.
3. Add the room-to-raster-preset table and pure preset builders.
4. Render BG1/BG2 with exact TM/TS, priority, mosaic, and colour math instead
   of the editor's current simplified five-step stack.
5. Feed that same resolved scene into the flat and Diorama previews. Virtual
   classification changes planes; it must not create a second native decoder.

The implementation uses the deferred-WebAssembly path: pure C is authoritative
for tests/export, and the self-contained JavaScript port consumes a normalized
descriptor plus per-room C golden frame so drift is checked when a room opens.

### P1 — authoring is editor-owned

1. **Implemented:** load and round-trip the complete `diorama-layers.ini` while
   owning the four ordinary BG and two virtual BG records in base action-room
   sections.
2. **Implemented:** virtual-band Z/order/alpha and metatile/cell/rectangle
   rules, plus ordinary BG Z/order/alpha and Flat/Rake/Bow/Thickness/Stack/
   Voxel strategy controls.
3. **Implemented:** every classification or geometry change updates Diorama
   immediately through the same mesh, skirt, stack-direction, falloff and solid
   voxel formulas as the runtime; native flat rendering remains intentionally
   configuration-neutral.
4. **Implemented:** export produces one merged INI and the game only
   parses/renders it. OBJ, BG3, backdrop and scoped effect records remain
   preserved because they are outside action-background tile authoring.

### P2 — differential acceptance (stable set implemented)

For each representative state, compare the standalone result with a native
room snapshot at the same room, camera, and phases:

- expanded BG1/BG2 tile words;
- CHR and CGRAM hashes;
- video-profile PPU registers;
- per-scanline raster register hashes;
- isolated BG1 low/high and BG2 low/high surfaces;
- main, subscreen, and final composite hashes.

Minimum representatives are all ten raster presets, every distinct animation
shape/target, both page-cycle rooms, the five forced-BG2-priority rooms, and a
Marahna colour-math room. Then run all 49 rooms at a stable camera and at every
available map edge.

The current live set covers every family: R1 (`0104`), R2 (`0201`), R3
(`0202`), R4 (`0405`), R5 (`0401`), R6 (`0601`/`0605`), R7 (`0608`), R8
(`0701`), R9 (`0702`-`0707`), and R10 (`0708`). Natural transition recordings
cover the rooms for which a raw warp is not a valid initialization fixture.
The exact runs are listed under Evidence below.

## Remaining knowledge gaps

None of these blocks a stable arbitrary-room editor preview:

1. **Transient object-driven windows/effects.** Map boss iris/wipe timelines
   only if the editor needs named previews of those moments. Stable room state
   is already deterministic.
2. **Additional named transient phases.** R4's flat entry table, Northwall's
   R6 entry camera, every Death Heim rematch entry camera, provisional BG2SC,
   and the ending page/table hold are pinned. Authoring already has explicit
   animation/page controls; more named transient previews are only needed if
   they become useful editor presets.
3. **BG3/OBJ presentation completeness.** Background authoring does not need
   gameplay sprites. A “literal game frame” toggle would need the common HUD
   and a defined static actor fixture, not a room loader change.
4. **Transient boss margin/window policy.** This is a widescreen presentation
   audit, not missing room construction.

The remaining work is therefore bounded engineering plus acceptance evidence,
not open-ended ROM archaeology.

### Natural-entry fixture capture

`AR_REPLAY_LIVE_AFTER_END=1` remains the fixture-acquisition path for future
transients: it replays a deterministic prefix, then returns control to live
input on the first later game frame. It takes precedence over
`AR_REPLAY_NOSTOP`, which intentionally holds the final recorded input instead.
When `AR_INPUT_RECORD` is also set, the recorder writes both the replayed prefix
and live continuation, producing a self-contained natural-entry fixture from
power-on:

```sh
AR_INPUT_REPLAY=saves/act1-boss2.bin \
AR_REPLAY_LIVE_AFTER_END=1 \
AR_INPUT_RECORD=saves/fillmore-r1-natural.rec \
AR_MOONJUMP=1 \
AR_NO_KNOCKBACK=1 \
AR_RANGED_SWORD=1 \
AR_ACTION_ROOM_SCENE_COMPARE=1 \
./build-release/ActRaiserRecomp ar.sfc --config config.ini
```

Keep the boot SRAM and gameplay-affecting settings with every new fixture;
controller input alone is not deterministic without them. The comparator
remains read-only, and replay sessions protect normal SRAM and settings after
control returns to the player. Canonical fixture hashes, run identifiers,
comparison counts, and results for Fillmore, Aitos, Northwall, and Death Heim
live in the [action-background evidence census](bg-hle-census.md#guarded-command-3-video-profile-handoff--2026-08-22).

## Complete room matrix

Profiles are shown in hexadecimal. Animation notation is
`N|R target/stride x count/p cadence`, where `N` is a native fresh snapshot and
`R` is a native continuation/reuse. Both are self-contained in the offline HLE.
Targets are VRAM word addresses and strides are bytes.

| Room | Profile | BG1 pages | BG2 pages | Tile animation | BG2 page cycle | Raster | `$F2` |
|---|---:|---:|---:|---|---|---|---:|
| `0101` | `03` | 16x3 | 9x2 | `—` | — | — | 0 |
| `0102` | `04` | 8x5 | 8x5 | `N 1000/100x4/p8` | — | — | 0 |
| `0103` | `05` | 4x7 | 1x2 | `R 1000/100x4/p8` | — | — | 0 |
| `0104` | `06` | 2x1 | 1x1 | `—` | — | R1 | 0 |
| `0201` | `07` | 16x2 | 1x1 | `—` | — | R2 | 0 |
| `0202` | `09` | 3x2 | 1x1 | `N 0000/080x4/p2` | — | R3 | 0 |
| `0203` | `0A` | 4x4 | 1x1 | `R 0000/080x4/p2` | — | R3 | 0 |
| `0204` | `0B` | 2x2 | 1x1 | `—` | — | — | 0 |
| `0205` | `0C` | 7x4 | 7x4 | `R 0000/080x4/p2` | — | — | 0 |
| `0206` | `0D` | 3x1 | 1x1 | `—` | — | — | 0 |
| `0207` | `0E` | 4x4 | 1x1 | `—` | — | — | 0 |
| `0208` | `0F` | 1x1 | 1x1 | `—` | — | — | 0 |
| `0301` | `10` | 16x3 | 2x2 | `N 0000/200x2/p8` | — | — | 1 |
| `0302` | `11` | 12x3 | 2x2 | `R 0000/200x2/p8` | — | — | 1 |
| `0303` | `12` | 8x2 | 4x2 | `N 0000/080x4/p4` | — | — | 1 |
| `0304` | `13` | 6x3 | 4x2 | `R 0000/080x4/p4` | — | — | 1 |
| `0305` | `14` | 4x6 | 4x2 | `R 0000/080x4/p4` | — | — | 1 |
| `0306` | `15` | 2x1 | 4x2 | `R 0000/080x4/p4` | — | — | 1 |
| `0401` | `16` | 16x4 | 1x1 | `—` | — | R5 | 0 |
| `0402` | `17` | 7x3 | 2x2 | `—` | 4 pages / 5f | — | 0 |
| `0403` | `18` | 2x1 | 2x2 | `—` | 4 pages / 5f | — | 0 |
| `0404` | `19` | 5x4 | 4x4 | `N 0000/100x4/p8` | — | — | 0 |
| `0405` | `1A` | 2x2 | 1x1 | `R 0000/100x4/p8` | — | R4 | 0 |
| `0406` | `1B` | 6x4 | 4x4 | `R 0000/100x4/p8` | — | — | 0 |
| `0407` | `1C` | 2x1 | 2x1 | `—` | — | — | 0 |
| `0501` | `1D` | 8x2 | 2x2 | `N 1000/200x4/p8` | — | — | 0 |
| `0502` | `1E` | 6x2 | 2x2 | `R 1000/200x4/p8` | — | — | 0 |
| `0503` | `1F` | 2x1 | 2x1 | `R 1000/200x4/p8` | — | — | 0 |
| `0504` | `20` | 4x4 | 4x2 | `N 0000/080x4/p4` | — | — | 0 |
| `0505` | `21` | 3x2 | 2x2 | `R 0000/080x4/p4` | — | — | 0 |
| `0506` | `22` | 9x7 | 5x4 | `R 0000/080x4/p4` | — | — | 0 |
| `0507` | `23` | 9x7 | 5x4 | `R 0000/080x4/p4` | — | — | 0 |
| `0508` | `24` | 2x2 | 2x1 | `N 0000/080x4/p4` | — | — | 0 |
| `0601` | `25` | 10x4 | 1x1 | `—` | — | R6 | 1 |
| `0602` | `26` | 7x3 | 7x3 | `N 1000/100x4/p8` | — | — | 1 |
| `0603` | `27` | 6x6 | 7x3 | `R 1000/100x4/p8` | — | — | 1 |
| `0604` | `28` | 2x2 | 2x2 | `R 1000/100x4/p8` | — | — | 1 |
| `0605` | `29` | 3x3 | 1x1 | `N 0000/080x4/p8` | — | R6 | 0 |
| `0606` | `2A` | 4x8 | 2x3 | `R 0000/080x4/p8` | — | — | 0 |
| `0607` | `2B` | 4x6 | 2x2 | `R 0000/080x4/p8` | — | — | 0 |
| `0608` | `2C` | 2x2 | 1x1 | `—` | — | R7 | 0 |
| `0701` | `2D` | 2x1 | 2x1 | `—` | — | R8 | 0 |
| `0702` | `06` | 2x1 | 1x1 | `—` | — | R9 | 0 |
| `0703` | `0F` | 1x1 | 1x1 | `—` | — | R9 | 0 |
| `0704` | `15` | 2x1 | 1x1 | `R 0000/080x4/p4` | — | R9 | 1 |
| `0705` | `1C` | 2x1 | 1x1 | `—` | — | R9 | 0 |
| `0706` | `24` | 2x2 | 1x1 | `N 0000/080x4/p4` | — | R9 | 0 |
| `0707` | `2C` | 2x2 | 1x1 | `—` | — | R9 | 0 |
| `0708` | `2E` | 1x1 | 1x1 | `—` | — | R10 | 0 |

## Evidence and reproducibility

- ROM static disassembly: `$02:B4E8`, `$02:BAF5`, `$02:BC27-$BC56`,
  `$02:9204-$96D3`, and the callbacks that call those builders.
- Exporter audit: 49 rooms, 0 failures, 177 pooled blobs, 887 KiB raw.
- Runtime character traces: `0202` target `$0000`; `0102` target `$1000`;
  both capture exactly 4 KiB into `$7F:B800-$BFFF`.
- Runtime page-cycle trace: `0402` BG2SC `$74,$78,$7C,$70`, five game frames
  per phase.
- Runtime raster census: all 49 direct room loads completed; the persistent
  callbacks match R1-R10 above. `0401`, `0601`, `0605`, `0702`-`0708` were
  additionally inspected at function/table level.
- Exact stable-room scanline oracle, 2026-08-22: `0201` R2
  `runs/20260822-130419` (996,907 registers, one exact retained-table frame),
  `0202` R3 `runs/20260822-130420`, `0401` R5
  `runs/20260822-130422`, `0601` R6 `runs/20260822-130424`, and `0702` R9
  `runs/20260822-130426` (1,337,865 registers each); all five report zero
  register mismatch.
- Natural R1 acceptance, 2026-08-22: manual capture
  `runs/20260822-170359/` and deterministic replay
  `runs/20260822-170748/` traverse `0102 -> 0103 -> 0104`, end after the boss
  defeat, and match final WRAM/SRAM/dispatch/state byte-for-byte. The 621 R1
  frames compare 701,109 registers with zero mismatch. Default-HLE capture
  `runs/20260822-170853/` and native presentation-bootstrap/source control
  `runs/20260822-170914/` are byte-identical at boss frame 7000 for the
  framebuffer, WRAM, VRAM, CGRAM, OAM/high-OAM and PPU registers, plus all
  four final-state artifacts. The native-source arm also matches the complete
  2,048-tile `0104` BG1 world.
- Natural R4 acceptance, 2026-08-22: manual capture
  `runs/20260822-171954/` and deterministic replay
  `runs/20260822-173608/` traverse `0404 -> 0405 -> 0406 -> 0407` and match
  final WRAM/SRAM/dispatch/state byte-for-byte. Across the route, 3,353 frames
  compare 3,785,537 registers with zero mismatch; the 222 `0405` frames
  contribute 250,638 exact R4 registers. The fixture pins moon jump, no
  knockback, and ranged sword in both capture and replay. It exposed and now
  pins R4's low-byte-only phase, inherited source-page index, and flat first
  visible table. Default-HLE run `runs/20260822-174151/` and native
  presentation-bootstrap/source control `runs/20260822-174203/` are
  byte-identical at R4 frame 2700 for the framebuffer, WRAM, VRAM, CGRAM,
  OAM/high-OAM and PPU registers, plus all four final-state artifacts. The
  control also matches all seven published room layers / 86,016 tiles.
- Natural Northwall acceptance, 2026-08-22: manual capture
  `runs/20260822-174442/` and replay `runs/20260822-180657/` traverse
  `0605 -> 0606 -> 0607 -> 0608`. All 3,592 action frames / 4,055,368
  registers are exact, including the first-visible `0605` R6 camera and R7;
  final state/WRAM/SRAM/dispatch are byte-identical. Default-HLE run
  `runs/20260822-180838/` and native presentation-loader/source control
  `runs/20260822-180845/` match all 11 framebuffer, PPU-snapshot, and
  final-state artifacts byte-for-byte. The control also matches six published
  layers / 80,896 tiles.
- Natural Death Heim acceptance, 2026-08-22: manual capture
  `runs/20260822-174637/` and deterministic replay
  `runs/20260822-180704/` cover every rematch, R8, R9, R10, the final boss,
  and ending handoff. All 14,637 action frames / 16,525,173 registers are
  exact, including six rematch bootstrap cameras, stable one-page BG2SC,
  post-boss `$64/$74`, and 1,351 frames retaining the final R8 table. Default
  HLE `runs/20260822-180852/` and native presentation-loader/source control
  `runs/20260822-180913/` match all visual, PPU, and emulated-state artifacts
  byte-for-byte at R8, R10, and the ending fade. The raw diagnostic dispatch
  log differs only in its monotonic ordinal/total, as expected when the native
  control executes 94 fewer HLE dispatch records.
- First production-source A/B, 2026-08-22: live-WRAM manifest
  `runs/bg-hle-matrix-20260822-133427.json` versus immutable-room manifest
  `runs/bg-hle-matrix-20260822-133346.json`. All 12 ordinary-entry targets
  sourced 19,522 bound layer-frames with zero room-scene fallback; native-ring
  preflight compared 18,216,295 tiles with zero mismatch/outside, and all 204
  framebuffer, WRAM/SRAM, dispatch, final-state and PPU-snapshot artifacts are
  byte-exact.
- Promoted-default matrix `runs/room-scene-default-matrix-20260822.json`
  repeats all 12 targets with the environment gate absent: 19,522 provider and
  room-scene layer-frames, zero source fallback, and 18,216,295 native-ring
  preflight tiles with zero mismatch/outside. The focused default/control pair
  `runs/room-scene-default-20260822.json` and
  `runs/room-scene-live-control-20260822.json` is byte-identical at both
  framebuffer and captured WRAM/VRAM/CGRAM/OAM artifacts.
- Exact command-4/5 staging matrix
  `runs/action-room-stage-matrix-20260822-v3.json`: all 12 ordinary-entry
  targets pass; all 24 loaded layers compare 166,496 dimension/map/metatile bytes
  with zero mismatch. The same runs retain 19,315,975 exact native-ring words,
  zero finite exits, and zero immutable-provider fallback. The staging oracle
  runs before PPU layer-enable/provider checks, so dormant BG2 assets in
  `0201`, `0303`, `0401`, `0504`, `0601`, and `0605` are covered too.
- Guarded CPU-loader matrix
  `runs/action-room-load-hle-matrix-20260822-v2.json`: all 12 ordinary-entry
  targets execute 24 command-5 and 24 command-4 HLE invocations, staging
  166,400 asset bytes. The independent room-stage oracle compares those assets
  plus 96 dimension bytes (166,496 total) with zero mismatch. All framebuffer,
  map, definition, dimension, provider, and native-ring hashes are identical to
  the pre-redirect native manifest
  `runs/action-room-stage-matrix-20260822-v3.json`. ROM-free CPU tests also pin
  LoROM pointer conversion, compression scratch, byte swapping, script cursor,
  registers/status, stack/RTS, both destinations, explicit-off behavior, and
  guarded fallback shapes.
- Guarded graphics-loader matrices
  `runs/action-room-gfx-native-control-matrix-20260822-v1.json` and
  `runs/action-room-gfx-hle-matrix-20260822-v1.json`: the same release binary
  runs all 12 ordinary entries with command 7/6 native and redirected. The HLE
  arm executes 39 character commands and 36 palette commands, staging 320,000
  bytes. The separate 49-room script census covers all 89 character and 87
  palette invocations, with five/three admitted operand shapes and no bad
  compressed-size header. All 204 framebuffer, WRAM/SRAM, VRAM, CGRAM, OAM, PPU-snapshot,
  dispatch and final-state artifacts are byte-exact. ROM-free CPU tests also
  pin the 8 KiB character banks, 4 KiB font variant, palette offsets, PPU-port
  order, shared decompression workspace/scratch, script cursor, registers,
  status, stack/RTS, explicit-off behavior and rejected shapes.
- Guarded video-profile matrices
  `runs/action-room-video-native-control-matrix-20260822-v1.json` and
  `runs/action-room-video-hle-matrix-20260822-v1.json`: the same release binary
  runs all 12 ordinary entries with command 3 native and redirected. The HLE
  arm executes 12 profile applications (336 record bytes). All 204 complete
  framebuffer, WRAM/SRAM, VRAM, CGRAM, OAM, PPU-snapshot, dispatch and
  final-state artifacts are byte-exact. The 49-room static census contains 49
  command-3 invocations and 43 profile IDs (`$03-$2E` excluding `$08`). The
  ROM-free CPU test pins every PPU/direct-page field, both priority paths, the
  `$C4/$C1` transform, script cursor, registers/status, stack/RTS, explicit-off
  behavior and guarded fallbacks.
- Static consumer census: all generated `$02:B4E8` variants write `$F2`; no
  registered/recompiled function reads it.
