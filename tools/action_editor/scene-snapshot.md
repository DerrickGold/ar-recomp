# Original-background comparison snapshot v1

This is the first comparison fixture for the
[shared-renderer editor plan](../../docs/action-effects-editor-plan.md).
`ActionSceneSnapshot_Encode/Decode` is the native format authority. The editor
encodes the same format and the WASM host validates it through that C decoder.
The snapshot contains asset bytes, never a ROM path, native struct dump, pointer,
GPU object, process address, `FrameSlot`, gameplay memory or executable script.

## Scope and provenance

The assets come from `ActionRoomScene_Load`'s cumulative native room script.
Regional BG1 terrain uses `ActionRoomTerrain_Project`; the projected bytes and
profile ID are both retained. Backgrounds are the **original** artwork, not the
editor's in-memory tile/pixel/depth edits. An explicit camera, frame and optional
native phase/raster inputs make replay independent of wall-clock timing.

Version 1 represents a stable two-background Mode 1 room. It deliberately does
not record environmental decorations, native actors/BG3, transient object
windows, fade state, edited geometry, extended rows, Diorama projection, lighting,
blur, skybox or final display filters. It is not an enhanced native gameplay
capture. Later snapshot capabilities must name and validate these inputs rather
than pretending that the original-background packet is a full scene.

Fixtures embed local game artwork. Keep generated fixtures and images in ignored
`build/` or `runs/` directories, not source control. Record the source revision,
ROM identity, terrain, room, camera, clock and capture scope alongside evidence
when comparing builds; the v1 packet itself contains no revision/ROM hash.

## Wire format

All header words are unsigned little-endian 32-bit integers. Signed fields use
two's-complement representation. Byte arrays follow without padding. The header
is 112 bytes (28 words); packets are bounded to 80 KiB. Unknown versions, flags,
reserved values, inconsistent dimensions/lengths and invalid checksum are errors.

| Word | Meaning |
| --- | --- |
| 0–3 | Magic `ARSC`, version `1`, total bytes, terrain profile `0..2` |
| 4–5 | Action group and room, validated by the game's room-domain helper |
| 6 | Presence bits: character banks 0/1, extra characters, palette, video profile, waveform, mosaic window, raster workspace, entry camera |
| 7–9 | Video profile index, raster preset `0..10`, entry camera X |
| 10–13 | Signed camera X/Y and raster camera X, unsigned game frame |
| 14–15 | Animation phase (`-1` automatic or `0..255`), page phase (`-1` automatic or `0..3`) |
| 16 | Raster camera present bit 0, BGSC override mask bits 1–2, entry-frame bit 3 |
| 17 | BGSC overrides: BG1 in low byte, BG2 in next byte |
| 18–21 | BG1 page width, page height, map byte count, metatile/map presence bits |
| 22–25 | Same fields for BG2 |
| 26–27 | FNV-1a byte checksum, reserved zero |

The checksum includes all packet bytes with word 26 treated as four zero bytes.
Map bytes equal width × height × 256, at most 16,384. The limit is **total pages**,
not a fixed 16 × 4 shape; tall rooms must work. An absent map has zero dimensions
and byte count. Native storage bounds are checked before copying any payload.
Coordinates preserve `-32768..65535`; the current native baseline renderer only
accepts nonnegative display camera coordinates. A structurally valid packet can
therefore still be rejected as unsupported by the renderer.

Payload order:

1. Characters (16,384), extra characters (8,192), palette (256).
2. BG1 metatiles (2,048) and variable map bytes.
3. BG2 metatiles (2,048) and variable map bytes.
4. Video profile (28), raster waveform (256), mosaic window (256), raster
   workspace (8,192).

Fixed-size blocks remain present when their presence flag is false. Decoding
validates the complete packet before changing destination state. The WASM host
also builds and renders a candidate before replacing its active scene. Import
failure leaves the previous scene available. No allocation occurs during load
or render, memory cannot grow, and the module has no host imports.

## Validation and rollout gates

- C tests cover maximum payload, signed values, complete byte round trips,
  truncation, unsupported versions/flags, inconsistent dimensions, checksum
  damage and failure without destination mutation. Run them with sanitizers.
- The ROM-free WASM test consumes a C-encoded synthetic scene and compares its
  frame hash with native replay. Reset, repeated load, missing required assets,
  invalid data and failed module initialization are explicit cases.
- The optional local-ROM gate covers all 49 rooms × three terrain profiles at
  eight camera/time/phase combinations each. At frame 37 it also checks the
  exporter's independently loaded native golden. Other cases include room-edge
  cameras, frames 0/1/511/65535/65536/UINT32_MAX, raster-camera history, entry-frame
  state, and explicit animation/page/BGSC overrides.
- Native ARGB hashes and hashes recomputed from WASM's exported RGBA bytes must
  agree exactly. This integer rasterizer needs no GPU tolerance. A hash check is
  a regression gate, not a substitute for enhanced-scene visual review.
- Browser UI, actual `file://` loading and enhanced rendering have separate
  acceptance checks. Node executes the real WASM and checks its output, but does
  not prove browser canvas, WebGL, Metal, Vulkan or D3D12 behavior.

The next contract extension needs owned edited surfaces/masks, resolved effect
sources, frame/settings inputs and dynamic source clocks. Replay it natively
before adding the corresponding WebGL backend. Do not serialize a raw `FrameSlot`
or introduce desktop-global stubs to make the linker succeed.
