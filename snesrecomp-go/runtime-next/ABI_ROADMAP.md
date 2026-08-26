# Runner ABI roadmap

`runtime-next` currently preserves the generated-code ABI and the legacy-shaped
global surface needed for side-by-side rollout. Those globals and concrete
`Ppu`, `Snes`, CPU, APU, and DSP layouts are compatibility details, not the
long-term extension API.

After the legacy runner is retired, expose a versioned portable C ABI centered
on a `SnesRunnerApi` table. The table starts with an ABI version and structure
size, advertises capability bits, and operates on opaque runner, CPU, PPU, APU,
DSP, cartridge, and host handles. Public values use fixed-width integers and
descriptors; they never expose compiler layout, SDL objects, native file
handles, or platform graphics/audio types.

## Read access

- Stable queries cover CPU registers and execution location; WRAM, SRAM,
  VRAM, CGRAM, OAM, PPU registers; APU RAM, ports, timers, and DSP voices;
  cartridge mapping; controller state; frame/audio clocks; and runner status.
- Synchronous callers can borrow immutable spans valid until the next runner
  tick, reset, load, or mutation. Generation counters and dirty ranges let
  tools update only changed regions.
- A frame snapshot descriptor groups component generations, timing, render
  surfaces, audio spans, and optional trace cursors without copying whole
  component memories. Asynchronous consumers explicitly pin or clone only the
  resources they retain.
- Callers provide output buffers for serialization, audio, traces, and bulk
  queries. Scatter/gather descriptors allow discontiguous data without an
  intermediate contiguous frame copy.

## Observation and controlled mutation

- Capability-gated observers report frame boundaries, register and memory
  writes, DMA, audio production, interrupts, generated-function/block events,
  dispatch misses, and errors. Subscriptions select address ranges and event
  classes before execution so disabled observation is cheap.
- Mutations are separate from read views. They are validated, queued for a
  documented safe point, and return an explicit result. Debugger writes,
  save-state loads, input injection, and enhancement commands cannot race a
  running component.
- The runner owns its emulation thread. Borrowed data is thread-confined;
  pinned snapshots and host callbacks state their ownership and lifetime.

## Layering

The ABI describes SNES mechanisms only. ActRaiser addresses, scene meanings,
music identifiers, enhancement policy, and manifest rules remain in a game
adapter. This lets layered enhancements inspect the same generic component
state and events without teaching the runner about a particular title.

## Migration order

1. Add ABI layout, capability, lifetime, and generation-counter tests while
   retaining the existing globals as an internal compatibility adapter.
2. Move read-only inspection, diagnostics, and snapshot consumers to ABI v1.
3. Move trace/event consumers to filtered observers and measure disabled-hook
   overhead.
4. Add safe-point mutation commands and versioned save-state serialization.
5. Remove external concrete-structure access only after all game adapters use
   the versioned boundary.

Cross-platform CI should compile the ABI from C, C++, Go/cgo, and Rust bindgen
fixtures; poison expired borrowed views in tests; verify serialized versions;
and count bytes cloned per frame so a low-copy regression fails visibly.
