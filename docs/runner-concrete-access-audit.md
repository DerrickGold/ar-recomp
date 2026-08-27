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

The SIM separated-capture slice removes `sim/sim3d.c` and leaves 4 temporary
exceptions. A synchronous frame transaction supplies one coherent PPU state
and frame snapshot, generation-matched zero-copy CGRAM/OAM borrows, and bounded
writable views of the host-owned overlay surfaces. Fixed colour and raw
transparent-fill policy are now public fixed-width values. SIM replaces all
selected capture policies through one atomic compare/exchange, binds its
caller-owned plane storage through the existing output service, and restores
the exact prior geometry, flags, transparent-fill, and OAM-range values after
scanout. The town-HUD handoff uses the public OBJ raster service and retains
only its own host buffers beyond the callback. This removes repeated concrete
field reads without adding an emulated-memory copy.

The Sky Palace VRAM slice removes `actraiser/actraiser_widescreen_bg.c` and
leaves 3 temporary exceptions. ABI v2 now accepts a bounded sparse list of
VRAM word compare/exchanges: it validates every expected value and address
before applying any replacement, rejects duplicate addresses, and can validate
a caller-declared sorted list with a single adjacent comparison. Sky Palace
builds that sorted list only for changed margin words and restores only values
that still contain its replacement, so a later producer is never overwritten.
This replaces the previous 4,096-word backup and two 8 KiB whole-tilemap copies
per active frame with a synchronous, game-agnostic transaction.

The widescreen OBJ-metadata slice removes
`actraiser/actraiser_widescreen_sprites.c` and leaves 2 temporary exceptions.
ABI v2 now publishes the resolved small/large OBJ dimensions in the coherent
PPU snapshot and accepts a bounded batch of exact OBJ positions plus the
camera-relative bit. The runner validates the complete batch before clearing
or publishing anything, rejects duplicate slots, and treats the values as
derived renderer metadata rather than emulated state. Action mode clears once,
collects every admitted OAM position in caller-owned storage, and commits once
after the scan. SIM mode reuses one coherent snapshot for the metadata build
and commits at most one batch per source record. No emulated memory, surface,
or intermediate raster is copied.

The action-background slice removes `actraiser/actraiser_action_bg.c` and
leaves 1 temporary exception. ABI v2 now publishes a fixed-width eight-channel
DMA snapshot and the authentic-surface binding flag, accepts bounded batches of
default or scanline-band layer extents, atomically replaces the two supported
virtual background providers, and publishes the authentic BG1/BG2 per-line
camera plus object offset. Provider callbacks retain caller-owned world
descriptors but return tile spans zero-copy; the adapter borrows VRAM only for
its existing preflight and never copies a tilemap or frame. Every request is
validated in full before clearing or replacing live renderer policy.

The synchronous scanout slice keeps the exception count at 1 because
`actraiser/actraiser_rtl.c` still contains the remaining game/runner adapter,
but removes its most timing-sensitive concrete loop. The runner now owns native
line execution, all eight HDMA channels, hold-first/hold-last vertical margins,
and vertical IRQ scheduling; the game supplies only its recompiled CPU IRQ
handler. Optional diagnostic callbacks receive fixed-width per-line PPU/HDMA
state and callback-lifetime, zero-copy surface views, so normal frames make one
scanout ABI call without a per-line cross-boundary callback. The former
`SimpleHdma` compatibility type and helpers are gone from common RTL.

The RDNMI pacing slice also keeps the exception count at 1, but removes
concrete `Snes *` layout from the game-specific `$4210` read callback. The SNES
register model now publishes only a fixed-width, callback-lifetime flag word
covering forced-NMI pacing, interrupt context, and NMI availability. ActRaiser
can retain its verified coroutine-yield policy without observing or mutating
runner-owned state. The hot path still performs exactly one indirect callback:
there is no adapter bridge, allocation, snapshot, or copied component state.

The game-frame lifecycle slice keeps the exception count at 1 while removing
the adapter's direct reads and writes of `forceNmi`, `nmiAvail`, `nmiEnabled`,
and `inNmi`. A public, capability-gated timing transaction gives external
layers validated begin/complete operations plus distinct persistent-state and
new-transition flags. The linked game uses the same semantics through a narrow
direct singleton adapter, avoiding API-table lookup and repeated descriptor
validation on its once-per-frame hot path. Cancellation always clears forced
pacing, and only a newly entered, hardware-enabled NMI dispatches the game
handler; stale persistent `inNmi` state cannot be mistaken for a transition.

The PPU display-control slice also keeps the exception count at 1, but removes
five scattered reads of concrete `inidisp`, `bgmode`, and main/sub-screen
fields. External layers retain the coherent public PPU snapshot. The linked
game's APU-halt pacing, force-blank animation gate, unbound-background fallback,
and developer logs use one packed fixed-width accessor at synchronous safe
points, avoiding a full snapshot or descriptor validation in the two hot
consumers. A missing runner maps to the same safe all-zero display state those
policies previously used.

The input-state slice keeps the exception count at 1 while removing the last
two direct controller-latch reads. ABI v2 now publishes a coherent copied
snapshot containing both the runner's documented packed-button order and the
SNES `$4218-$421B` auto-joypad order, with lifetime and frame identity. The
frame logger and moon-jump policy query it only when their diagnostic or cheat
is enabled, so normal frames gain no query or copy.

All 92 application tests pass, including the three host-GPU tests. The runner
ABI test covers multiword patch/restore, stale views, all-or-nothing
contention, duplicate and unsorted addresses, unknown flags, and reserved
fields. C++17 header and x86-64 macOS cross-target syntax checks pass. The
targeted wide replay executes the transaction from gf276 and at gf420 without
a failed restore. Seven adjacent replay pairs retain identical artifacts;
suite regressions are +0.30% portable and +0.43% native-SIMD. The focused
active-Sky workload measures +2.15% portable and +0.79% native-SIMD, within the
phase gate.

The OBJ-metadata ABI test additionally covers clear/update batches, exact
signed coordinates, camera-relative policy, stale generations, scanline-mask
invalidation, incremental updates, clear-only requests, duplicate and invalid
slots, unknown flags, reserved fields, and validation-before-clear. C++17 and
x86-64 checks pass. Seven adjacent replay pairs retain identical terminal
artifacts; the portable suite is 0.38% faster and native-SIMD is 0.37% faster.
The projected-SIM checkpoint reproduces the frozen reference's existing camera
fixture drift but matches all substantive metadata, separated-capture, and
authentic-framebuffer metrics and both render hashes. A wide Aitos frame and 11
Fillmore action-frame captures also match their frozen reference pixels
exactly.

The action-background ABI test covers all eight DMA channels, default and
banded extents, stale generations, validation-before-apply, virtual scalar,
span, and band lookups, invalid replacement preservation, authentic-camera
publication, and clear operations. C11, C++17, ARM64, and x86-64 syntax checks
pass. Seven adjacent replay pairs retain identical terminal artifacts; the
portable suite is 0.39% faster and native-SIMD regresses 0.76%, with every
individual movement at or below 1.17%. Eleven GPU-backed active Fillmore action
frames and the final WRAM, SRAM, dispatch log, and complete state match the
frozen reference byte-for-byte.

The scanout ABI test covers the full 225-line before/after diagnostic contract,
direct and indirect HDMA register writes, vertical IRQ rescheduling and `inIrq`
handoff, zero-copy surface views, final PPU state, stale generations,
undersized descriptors, unknown flags, and validation-before-mutation. All 92
application tests pass, including the three host-GPU tests; the focused ABI and
concrete boundary tests pass; and C11, C++17, ARM64, and x86-64 header syntax
checks pass. Seven adjacent replay pairs retain identical terminal artifacts.
The portable suite changes by +0.05%, while native-SIMD improves by 0.95%.
Eleven active Fillmore action frames plus final WRAM, SRAM, state, and dispatch
artifacts match the frozen pre-scanout reference byte-for-byte.

The RDNMI tests cover the hardware-to-flag mapping, descriptor size, direct
game-hook registration, and fallback delegation contract. The SNES contract
test now uses the real PPU and validates real VRAM DMA instead of linking a
register-file PPU mock across the concrete runner boundary. All 92 application
tests pass, including the three host-GPU tests; the focused SNES, CPU-infra,
and concrete-boundary tests pass; and strict C11/C++17 ARM64 and x86-64 header
checks pass. Seven adjacent replay pairs retain identical WRAM, SRAM, CPU-state,
and dispatch artifacts. The portable suite is unchanged to two decimal places,
while native-SIMD improves by 0.03%.

The game-frame timing tests cover API extent and capability publication,
undersized outputs, unknown flags, validation-before-mutation, begin/cancel,
disabled and enabled NMI gates, stale interrupt state, and the direct linked
adapter. All 92 application tests pass, including the three host-GPU tests;
the focused ABI, SNES, CPU-infra, and concrete-boundary tests pass; and strict
C11/C++17 ARM64 and x86-64 header checks pass. Seven adjacent replay pairs
retain identical WRAM, SRAM, CPU-state, and dispatch artifacts. Portable
regresses 0.24% at suite scale and native-SIMD regresses 0.29%, with no
individual workload above 0.88%. A first implementation that sent the linked
consumer through the fully validated public table was rejected after its
portable action workloads regressed 3.60% and 1.70%; the direct adapter retains
the external ABI while avoiding that unnecessary in-process cost.

The PPU display adapter test covers exact byte packing/unpacking and the safe
no-runner value using the real PPU layout rather than a one-field mock. All 92
application tests pass, including the three host-GPU tests; the focused
CPU-infra and concrete-boundary tests pass; and strict C11/C++17 ARM64 and
x86-64 adapter-header checks pass. Seven adjacent replay pairs retain identical
WRAM, SRAM, CPU-state, and dispatch artifacts. The portable suite improves by
0.10% and native-SIMD regresses 0.20%; no workload regresses by more than 0.88%.

The input-state ABI test covers descriptor extent, capability publication,
undersized output rejection, exact two-controller packed and hardware latch
values, zeroed reserved fields, lifetime identity, and frame identity. All 92
application tests pass, including the three host-GPU tests; the focused ABI
and concrete-boundary tests pass; and strict C11/C++17 ARM64 and x86-64 header
checks pass. Seven adjacent replay pairs retain identical WRAM, SRAM, CPU-state,
and dispatch artifacts. Portable improves by 0.37% at suite scale and
native-SIMD regresses 0.08%; no workload regresses by more than 1.27%.

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

- `actraiser/actraiser_rtl.c`

Do not replace these with asynchronous commands or repeated full snapshots.
They need synchronous game-adapter services at the emulation-thread safe point,
using fixed-width requests and caller-owned buffers.  This preserves timing and
lets the runner optimize the operation internally. The replacement-manifest,
SIM separated-capture, Sky Palace margin mutation, widescreen OBJ metadata, and
action-background paths are now migrated with this model. Native scanout and
game-frame timing are runner-owned, and RDNMI pacing receives only
callback-lifetime flags. The central widescreen policy now uses a bounded
BEGIN/FINALIZE transaction for horizontal/vertical margins, fill and motion
masks, row bands, vertical clipping, capture padding, and HUD geometry. The
remaining exception is capture/composition and developer-diagnostic work in
the core ActRaiser adapter.

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
