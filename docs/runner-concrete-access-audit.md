# Runner concrete-access audit

This audit defines the cutover-critical portion of the runner ABI work.  A
concrete runtime layout may be used inside `runtime-next`; application code in
`src/` must depend on the versioned ABI or an explicitly versioned host service
instead.  Generated CPU state is a separate game/compiler contract and is not
part of this component-layout fence.

The migration starts with 27 application files including `snes/snes.h`,
`snes/ppu.h`, `snes/apu.h`, `snes/dsp.h`, `snes/spc.h`, `snes/dma.h`, or
`snes/cart.h`.  `tools/check_runner_boundary.cmake` records the exact set.  It
fails for both a new violation and a stale exception, so the count can only
move deliberately toward zero.

The first read-only slice removes `dev/oracle_trace.c` and
`sim/sim_phase0_trace.c`, leaving 25 temporary exceptions. The runner ABI is
now version 2; this project is its only consumer, so the expanded PPU snapshot
is the single supported layout rather than a version-1/version-2 compatibility
branch. All public descriptor extent names are likewise rebaselined to v2;
the runner rejects every requested ABI version other than 2.

The first type-boundary slice publishes the fixed-width `SrPpuObjPart` value
descriptor and removes `action/action_obj_apron.h` and
`sim/sim_render_metadata.h`, leaving 23 temporary exceptions. Concrete PPU
code aliases the same descriptor, so the handoff remains a direct value copy
without translation or allocation.

The second type-boundary slice moves `diorama/diorama_planes.h` onto the ABI
overlay IDs, leaving 22 temporary exceptions and no concrete component import
caused only by an application header's public value types.

The developer-tools slice replaces its retained `Ppu *` with coherent v2 PPU
state/frame snapshots and generation-matched borrowed OAM spans. This removes
`dev/dev_tools.c` and `dev/host_dev_tools.c`, leaving 20 temporary exceptions.
The capture runs only for an explicit developer action and does not add work to
normal frames.

`frame_slot.c` already consumed the same coherent v2 view; replacing its last
three concrete size constants with ABI surface limits removes its stale header
dependency and leaves 19 temporary exceptions without changing frame work.

The same public surface limits remove stale layout-header imports from
`diorama/diorama_host.c` and `host/host_display.c`, leaving 17 temporary
exceptions. These host paths still rebind through the existing adapter; their
surface ownership will move behind the opaque v2 control service separately.

The scene-inspector slice publishes the fixed Mode-7 register state and a
generation-checked background-coordinate resolver in ABI v2. The resolver
uses the renderer's own mosaic, extent, vertical-clip, BG3-widen, and fill
policy path; application diagnostics no longer duplicate that logic or retain
a concrete `Ppu *`. VRAM and OAM remain zero-copy borrowed views. This removes
`dev/scene_inspector.c` and leaves 16 temporary exceptions. The coordinate
service runs only for an explicit inspection action. Expanding the PPU state
snapshot adds one 16-byte matrix copy to existing snapshot queries; it adds no
new query to normal frames.

The first host-control slice adds generation-checked, synchronous PPU output
binding and horizontal-margin operations. Requests retain host-owned buffers
zero-copy but prove their alignment, pitch, height, and byte capacity before
the runner stores a pointer. `hd_replacement_host.c` now binds the main,
authentic-comparison, BG/OBJ overlay, priority-band, and Mode-7 surfaces without
importing `Ppu`; this leaves 15 temporary exceptions. Explicit ABI margin
changes expire surface views. The game's existing per-frame margin setters do
not gain generation-counter writes after an adjacent native-SIMD benchmark
showed that seemingly small change cost roughly 6% in wide action scenes.

The first frame-critical mutation slice adds generation-checked, synchronous
overlay-capture and Mode-7-override claims. Each claim is atomic at the
emulation-thread safe point: the first owner for a source wins and later
callers receive `SR_RESULT_BUSY` instead of observing and then overwriting
concrete PPU fields. `hd_replacements.c` now takes at most one coherent PPU
snapshot per enabled frame with loaded replacement art, submits fixed-width
requests, and retains the existing host-owned Mode-7 pixels zero-copy for the
frame. Its tests use a fake public ABI rather than PPU stubs. This removes
`hd_replacements.c` and leaves 14 temporary exceptions.

The diorama renderer's remaining PPU include supplied only the maximum backing
surface dimensions. Replacing those private names with the existing public ABI
limits removes `diorama/diorama.c` and leaves 13 temporary exceptions. The
numeric constants and generated instructions are unchanged; no runner query or
per-frame service was added.

The CPU-state control slice publishes the SNES arithmetic-unit operand/result
latches as a fixed-width snapshot plus a generation-checked synchronous
restore. The opt-in world-map HLE comparison oracle uses this service to roll
back its diagnostic transaction without retaining a concrete `Snes *`.
Production world-map composition never invokes either operation. This removes
`sim/sim_world_map_build.c` and leaves 12 temporary exceptions.

The host-bootstrap slice removes the last concrete component access from
`main.c`, leaving 11 temporary exceptions. Presentation storage and texture
allocation use public ABI surface limits; the settings palette borrows CGRAM;
and the enhanced town canvas receives one coherent PPU snapshot plus
generation-matched, zero-copy VRAM/CGRAM spans. A pure capture-availability
predicate keeps those three ABI calls out of every frame that cannot build the
canvas. Screenshot diagnostics now report the margins latched with the rendered
pixels rather than later live PPU state. Loaded-ROM transforms are consolidated
inside the already-fenced ActRaiser adapter, and the obsolete direct startup
margin write is gone because the preceding ABI surface rebind already performs
that configuration.

The native-audio diagnostic slice replaces four concrete APU/SPC trace-hook
payloads with a capability-gated public observer and leaves 10 temporary
exceptions. Events carry fixed-width SPC registers, port/DSP values, the APU
cycle clock, and a callback-lifetime read-only ARAM view. The game-specific
driver offsets remain in the ActRaiser tracer rather than entering the runner
contract. The old nullable hook check at each existing audio seam became one
unlikely observer-count check, so disabled tracing adds neither a second
branch nor a copy. Subscriptions are cleared with runner teardown.

The standalone ActRaiser `SpcPlayer` compatibility adapter was then removed,
leaving 9 temporary exceptions. Repository-wide symbol analysis showed that
no caller uploaded data to it, rendered its DSP, or observed its ports: boot
allocated a second isolated DSP and reset only cleared that unused instance.
Deleting the adapter avoids inventing a public ABI for dead behavior and does
not alter the live APU, SPC, DSP, upload, replacement-music, or native-mixer
paths.

The live SPC-upload slice moves `actraiser/actraiser_spc_upload.c` behind two
narrow boundaries and leaves 8 temporary exceptions. The runner supplies a
versioned callback-lifetime transaction containing immutable ROM, live ARAM,
parsed upload metadata, and current SPC state while it already owns the APU
lock. The game adapter can extend the upload and request the same bounded
bootstrap execution without learning `Apu` or `Spc` layout. Its later
resident-uploader handshake uses a public atomic compare-and-set service:
an inclusive PC range and bounded ARAM signature must both match before the
runner changes the SPC PC. The common fast path is unchanged; this control is
called only while a game upload completion is pending.

The native mixer slice moves `native_audio_mixer.c` behind a zero-copy,
callback-lifetime routing transaction and leaves 7 temporary exceptions. On
each existing DSP-write seam the runner supplies fixed-width SPC X/address/
value fields plus read-only ARAM and voice-bus spans; the ActRaiser adapter
returns at most two validated bus-label updates. State loads use a separate
cold full-label plan. Live Music/SFX gains now use capability-gated public
audio-mix control, with the runner owning APU locking. The replacement-stream
gain remains host policy. No ARAM, DSP, or audio buffer is copied.

The native extended-audio slice moves `native_audio_extension.c` behind
synchronous game-adapter transactions and leaves 6 temporary exceptions. The
runner supplies fixed-width SPC state and a callback-lifetime, zero-copy ARAM
view, owns validation and application of the small DSP operation set, and
publishes the existing upload safe point while the APU lock is already held.
Extension save/load state crosses a canonical fixed-width transfer service;
the application no longer imports APU, SPC, DSP, SNES, or save-state layouts.
When the extension is disabled the runner installs no opcode, cycle, DSP,
save-state, or upload callback.

The SIM object-atlas slice removes `sim/sim_render_atlas.c` and leaves 5
temporary exceptions. ABI v2 can now resolve a live OAM range once into
caller-owned fixed-width OBJ parts, including rotation, exact-position,
camera-relative, size, and priority policy, then rasterize resolved or
synthetic parts directly into caller-owned cropped storage. The atlas uses its
already captured parts on the normal path and the resolver only as a fallback;
neither path takes a full PPU snapshot or copies an intermediate image.

## Required seams

### Type leakage

Complete. Small value types now live in application-owned or public
fixed-width descriptors. This is build-boundary cleanup and has no per-frame
cost.

### Read-only inspection

Complete for the concrete-layout fence. Developer tools consume coherent
fixed-width snapshots, generation-checked borrows, or synchronous
callback-lifetime views. Dirty-range/scatter-gather support remains warranted
only where it removes an observed bulk copy.

### Host surface and presentation control

Complete for the concrete-layout fence. Host-owned pixel storage stays
zero-copy behind capability-gated output binding, horizontal-margin, capture,
and retained-surface operations.

### Frame-critical enhancement mutation

These paths alter SNES state or derived renderer state synchronously while a
game frame is being produced:

- `actraiser/actraiser_rtl.c`, `actraiser/actraiser_action_bg.c`,
  `actraiser/actraiser_widescreen_bg.c`, and
  `actraiser/actraiser_widescreen_sprites.c`
- `sim/sim3d.c`

Do not replace these with asynchronous commands or repeated full snapshots.
They need synchronous game-adapter services at the emulation-thread safe point,
using fixed-width requests and caller-owned buffers.  This preserves timing and
lets the runner optimize the operation internally. The replacement-manifest
mutation path is now migrated with this model; the listed enhancement modules
remain.

### APU, SPC, and DSP integration

Complete for the concrete-layout fence. Audio diagnostics, SPC upload, native
mixing, and the extended-audio path now use distinct fixed-width observer,
transaction, control, and serialization services. A general audio inspection
SDK, asynchronous pinned snapshots, and speculative DSP controls remain
deferred.

## Deferred work

Caller-buffer save-state serialization, queued external state loading,
asynchronous pinned snapshots, generic language bindings, and broad external
SDK fixtures are useful follow-ups but do not block concrete-layout isolation.
The current pass includes one optimization effort: dirty ranges or
scatter/gather descriptors where measurement identifies a real redundant bulk
copy in a migrated consumer.
