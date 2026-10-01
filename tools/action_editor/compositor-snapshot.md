# Captured compositor snapshot v1

This `.ardi` packet is separate from the original-background `.arscene` fixture.
Its native authority is `DioramaSnapshot_Encode/Decode` in
`src/diorama/diorama_snapshot.c`. It records the inputs to the production
`Diorama_Composite`, including the actual captured BG/OBJ priority surfaces and
extra rows/columns. The browser does not generate approximate replacement planes.

## Scope

Included: current plane pixels, coverage masks, painter order and resolved depth
shapes, bounds/framing/camera, per-plane offsets, BG2 row spans and periodic or
captured skybox pixels, transparent fills, additive-plane policy, BG1 exposure
and the four compositor shader settings. Tilted BG3 is included when captured.

Excluded: environmental and actor-effect callbacks/inputs, simulation, a flat
HUD, heat/final CRT passes and live map edits. The native capture hook rejects
frame generation rather than combine raw endpoints with generated offsets.
Named ROM replacement sources are rejected rather than substituted. These need
new explicitly captured inputs before comparison can claim their parity.

The native hook is opt-in. It allocates no pixel storage during ordinary play.
It copies and zero-pads the owned images synchronously during upload, applying
the same apron policy as the production uploader. Drawing only associates the
matching timestamp's metadata; it never reads released producer pixels.
`FrameSlot`, CPU state, structs, pointers, paths and GPU handles are not serialized.

## Format

The header is 4,096 bytes. Its first four little-endian uint32 words are:

1. Magic `0x49445241` (ASCII `ARDI`).
2. Version `1`.
3. Exact total byte length.
4. FNV-1a checksum over the complete packet, treating bytes 12–15 as zero.

The ordered, typed field definition is
`src/diorama/diorama_snapshot_fields.inc`. Each scalar is one explicit 32-bit
word: integers use little-endian two's complement; floats use IEEE binary32.
Booleans accept only zero/one before assignment. Coverage masks are two words,
low first. The rest of the header is reserved zero bytes. Format changes must
bump the version; the field list is not a C memory-layout ABI.

Payload images follow in plane-ID order for the presence mask, then the optional
skybox. Each pixel is four canonical bytes **R,G,B,A**, top row first, without
row padding. Plane allocations are 640×352. The skybox is either 256×256 periodic
art or a zero-padded 640×352 captured allocation; its valid width remains metadata.
No mipmaps or GPU representations appear in the packet. Native upload converts
RGBA to host-endian ARGB words. The WASM adapter converts those words for WebGL.

The maximum packet is 12,619,776 bytes. Unknown versions, nonfinite/range-invalid
fields, duplicate plane entries, named source IDs, overlapping row spans, invalid
extents/flags, nonzero reserved bytes, bad checksums and inconsistent payloads are
rejected before destination state changes. Inputs remain bounded even if the
checksum was recomputed over malformed metadata.

Decoded images borrow the immutable packet; `DioramaSnapshot_Bind` repairs the
internal metadata pointers after a structure move. Browser loading owns a copy,
uploads the candidate first, then replaces the active scene. Failed imports keep
the previous scene available. The single WASM instance has fixed 64 MiB memory.
The compositor retains bounded scratch targets and streamed geometry buffers are
reused. Changing output size does not fabricate additional captured scenery. Browser
output is capped to 2048 pixels per axis; a larger recorded output is fitted
within that limit while preserving its aspect ratio.

## Evidence and portability

Keep generated packets, source artwork and screenshots in ignored `runs/` or
`build/`; do not commit ROM-derived fixtures. Record source revision, settings,
room/terrain and capture route alongside each comparison. The packet identifies
room/section but does not contain a source revision or ROM hash.

Native Metal versus WebGL2 image checks currently cover Fillmore 01/01,
Bloodpool 02/01 and Aitos 04/02, plus changed Bloodpool framing/aspect/skybox policy.
The GLSL ES variants are generated from production GLSL, with explicit std140
uniforms and a sampling shim for GL render-target orientation. The builder checks
the uniform interface against the semantic C-to-browser parameter bridge.

These checks do not establish full enhanced-scene parity or validate hardware
we have not run. Native Vulkan/Steam Deck and Windows/D3D12, all-room captures,
effect inputs, named replacements and live edit resolution remain pending.
