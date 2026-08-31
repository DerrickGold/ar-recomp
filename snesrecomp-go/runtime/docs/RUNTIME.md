# Shared runtime {#runtime_architecture}

`runtime/` is the portable execution environment for code emitted by
`snesrecomp-go`. Generated functions and every public entry point retain a C11
ABI. The private attributed S-DSP accuracy unit is C++20 and is hidden behind a
C bridge. Go is needed for regeneration, not at game runtime.

The historical comparison runner was retired after parity validation. There is
one runtime manifest and no source, include, or build fallback to that retired
implementation.

## Boundary

The shared layer provides:

- `CpuState`, generated call/return/dispatch support, LoROM and WRAM access;
- PPU, APU, SPC700, DSP, DMA, cartridge, SRAM, and joypad primitives;
- frame/audio pacing helpers, save-state primitives, checksums, and keybinds;
- unresolved-control-flow traps, runtime-selectable observation, and optional
  deep trace rings;
- MSU-1 register/data/audio support;
- host-overlay extraction hooks in the PPU;
- optional shadow audio and display color-LUT modules; and
- an installable `snesrecomp::runtime` static-library target, with source-build
  manifest and configuration support in `runtime/runner.cmake`.

The per-game project provides the executable frontend, verified ROM loading,
configuration/settings, a versioned `RtlGameModule`, reset/frame/interrupt
policy, HLE bodies, and all ROM-address-specific behavior. See the broader
recompiler project for generated-project and build-tool integration.

## Game module contract

Register exactly one immutable `RtlGameModule` before `SnesInit`. The module
has its own ABI version, byte size, and capabilities; optional nested tables
must agree with their capability bits. Registration validates the complete
descriptor once and returns `SR_RESULT_OK`, `SR_RESULT_UNSUPPORTED`,
`SR_RESULT_INVALID_ARGUMENT`, or `SR_RESULT_BUSY`.

- `RtlGameIdentity` owns stable IDs, display text, and save naming.
- `RtlGameLifecycleApi` receives an opaque runner on bind/revoke and an
  initialization context. Mutable ROM bytes are available only during the
  initialization callback, after loading and before reset.
- `RtlGameExecutionApi` owns the required frame callback and optional PPU
  draw, `$4210`, and verified dispatch-recovery policy.
- `RtlGameStateProviderApi` publishes generated CPU/execution snapshots using
  module-owned context. The runner installs and revokes these providers.
- `RtlGameAudioApi` groups privileged SPC-upload, voice-routing, and extended
  audio safe-point callbacks behind separate audio capability bits.

The runner caches the validated nested tables, so the structure adds no
allocation or repeated descriptor traversal on frame hot paths. Callbacks
isolate game policy from the SNES core; shared runtime code must not add
hard-coded ROM addresses or game-prefixed external symbols.

## MSU-1

The runtime implements the `$2000-$2007` MSU-1 contract, `.msu` data
channel, and 44.1 kHz signed-16 stereo PCM mixing. It is inert unless a game
ROM drives the registers and a pack is explicitly configured.

Set `SNESRECOMP_MSU1` to either a pack base prefix or directory. Track `N`
resolves as `<prefix>-N.pcm`; data resolves as `<prefix>.msu`. A PCM file
uses the usual eight-byte header (`MSU1` plus a little-endian loop frame),
followed by stereo PCM. Missing or invalid tracks set the error bit so a ROM
driver can fall back to SPC audio.

## Host-overlay extraction

The PPU can export already-rendered BG/OBJ rectangles into transparent ARGB
surfaces without mutating emulated memory or save-state data. The runtime owns
tile decode, windows, mosaic, palette, brightness, and source isolation; the
game layer chooses verified capture regions and the frontend composes or
replaces them.

`SnesRunnerApi.bind_ppu_output_surface` installs persistent host-owned output
resources. `claim_ppu_overlay_capture`, `claim_ppu_mode7_override`, and
`configure_ppu_obj_capture` describe frame-scoped ownership and must be cleared
or re-established each frame. Composition uses a coherent
`visit_ppu_frame_transaction` callback rather than a private PPU pointer.

## Trace and debug status

Runner event subscriptions and audio-trace subscriptions remain available in
ordinary optimized builds and are inactive until a host subscribes. The
project-owned JSONL/watch recorder is a separate diagnostic consumer: build it
with `SNESRECOMP_ENABLE_TRACE_RECORDER=ON`, then select its runtime mode through
`SNESRECOMP_TRACE_FILE`, `SNESRECOMP_TRACE_WATCH_FILE`, and
`SNESRECOMP_TRACE_CHANNELS`. Play and packaged builds should leave the option
off so the recorder, formatting code, and generated entry/call probes are
compiled out.

`SNESRECOMP_ENABLE_TRACE=ON` additionally compiles deep generated-CPU
instrumentation: per-opcode/boundary paths, local execution rings, and the
generated trace link stub. It implies the runtime recorder. Use that build only
when public event observers and the runtime JSONL recorder cannot answer the
question. Entry/call/exit M/X assertions,
stack-balance checks, garbage-variant traps, dispatch-miss diagnostics, and
unresolved-stub traps remain actionable in ordinary builds. The retired TCP
debug server was intentionally not carried into the independent runner.

Generated/runtime diagnostic symbols use the `sr_` namespace. Generic process
controls use descriptive `SNESRECOMP_*` names such as
`SNESRECOMP_TRACE_FILE`, `SNESRECOMP_MX_HISTORY`, and
`SNESRECOMP_WATCHDOG`; `AR_*` is reserved for ActRaiser-owned policy above the
runner.

## Portability contract

- Public/core ABI code is portable C11 with fixed-width types; the private
  attributed DSP device is C++20 behind a C bridge.
- Release CMake targets enable IPO when the selected compiler reports support;
  `SNESRECOMP_ENABLE_IPO=OFF` provides the non-IPO compatibility path.
- OS, SDL, graphics, and audio-library types stay behind host adapters.
- Hot read paths do not allocate.
- Native, forced-portable, standalone subsystem, root-project, and Zig
  cross-target builds provide the validation gates.
- Optional SIMD is selected by compiler target macros and always retains the
  portable implementation.

## Determinism contract

For one linked runner build and host target, identical ROM bytes, initial
serialized semantic state, game module, emulation-affecting configuration,
host-frame input sequence, adapter schedule, and APU production schedule
produce the same schema-versioned semantic digest at equivalent safe points.
The digest-owned schema fixes integer byte order and is independent from the
save-state traversal. It excludes host pointers, wall-clock time, presentation
allocation padding, DSP PCM delivery, voice-bus mixing policy, diagnostics,
and game-authored native/HLE extension save data. If such an extension owns
additional emulation-affecting state, its adapter must checkpoint that state
through a separate game-owned canonical digest.

Presentation determinism additionally requires the same frame policy,
published enhancement resources, scanout schedule, and renderer selection.
The digest is returned by the scanout transaction that produced it and covers
the completed logical canvas, including active margins, but not unused surface
storage. Fast/reference renderer parity remains a validation
gate; it is not implied merely because both implementations share a digest
schema.

The guarantee does not cross digest schema versions, runner behavior changes,
different game-generated code, nondeterministic game/host callbacks, live input
or mutation races, or external enhancement resources that differ. Canonical
encoding makes digests comparable across machines, but cross-architecture and
cross-optimization execution equivalence remains a conformance result, not an
ABI guarantee. Unconstrained audio callback demand can advance the emulated APU
ahead of its game-tick timeline; deterministic tests must use headless timeline
pacing or reproduce the same audio production schedule. Record build
provenance in the surrounding test report while the ABI is still evolving—the
digest is deliberately not a source-revision identifier.

## Known SDK gap

Gameplay save/load remains available to the linked game, but a generic
frontend or external tool cannot yet request a versioned caller-owned save
image or queue a load through the public safe-point boundary. This is the
remaining concrete ABI integration gap. The human-facing operation and
lifetime reference is `API_REFERENCE.md`.
