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

## Required seams

### Type leakage

These headers import a concrete component only to publish a PPU-owned enum or
structure in an application interface:

- `action/action_obj_apron.h`
- `diorama/diorama_planes.h`
- `sim/sim_render_metadata.h`

Move the small value types into application-owned or public fixed-width
descriptors.  This is build-boundary cleanup and has no per-frame cost.

### Read-only inspection

Developer tools and capture code read registers or memories without owning the
component:

- `dev/dev_tools.c`, `dev/host_dev_tools.c`, `dev/oracle_trace.c`,
  `dev/scene_inspector.c`, and `dev/native_audio_trace_runtime.c`
- `frame_slot.c`, `sim/sim_phase0_trace.c`, and the read portions of `main.c`
  and `hd_replacements.c`

Use coherent fixed-width queries and generation-checked borrowed spans.  The
dirty-range/scatter-gather work belongs here only where it removes an existing
bulk copy; it is not a mandate to invent unused infrastructure.

### Host surface and presentation control

These modules bind host-owned output surfaces or configure presentation-time
capture:

- `hd_replacement_host.c`, `host/host_display.c`, and
  `diorama/diorama_host.c`
- host-control portions of `main.c` and `frame_slot.c`

Expose opaque, capability-gated surface/control operations.  Pixel storage
stays host-owned and zero-copy.

### Frame-critical enhancement mutation

These paths alter SNES state or derived renderer state synchronously while a
game frame is being produced:

- `actraiser/actraiser_rtl.c`, `actraiser/actraiser_action_bg.c`,
  `actraiser/actraiser_widescreen_bg.c`, and
  `actraiser/actraiser_widescreen_sprites.c`
- `diorama/diorama.c`, `sim/sim3d.c`, `sim/sim_render_atlas.c`, and
  `sim/sim_world_map_build.c`
- mutation portions of `hd_replacements.c`

Do not replace these with asynchronous commands or repeated full snapshots.
They need synchronous game-adapter services at the emulation-thread safe point,
using fixed-width requests and caller-owned buffers.  This preserves timing and
lets the runner optimize the operation internally.

### APU, SPC, and DSP integration

These modules currently cross the audio-thread ownership boundary directly:

- `actraiser/actraiser_spc_player.c` and `actraiser/actraiser_spc_upload.c`
- `native_audio_extension.c`, `native_audio_mixer.c`, and
  `dev/native_audio_trace_runtime.c`

Only the operations these consumers use are cutover-critical.  A general audio
inspection SDK, asynchronous pinned snapshots, and speculative DSP controls are
deferred.

## Deferred work

Caller-buffer save-state serialization, queued external state loading,
asynchronous pinned snapshots, generic language bindings, and broad external
SDK fixtures are useful follow-ups but do not block concrete-layout isolation.
The current pass includes one optimization effort: dirty ranges or
scatter/gather descriptors where measurement identifies a real redundant bulk
copy in a migrated consumer.
