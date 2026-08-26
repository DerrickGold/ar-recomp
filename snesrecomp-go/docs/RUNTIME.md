# Shared runtime

`runtime-next/` is the independently authored portable C execution
environment for code emitted by `snesrecomp-go`. It remains C because
generated functions link directly against its ABI. Go is needed for
regeneration, not at game runtime.

The historical comparison runner was retired after parity validation. There is
one runtime manifest and no source, include, or build fallback to that retired
implementation.

## Boundary

The shared layer provides:

- `CpuState`, generated call/return/dispatch support, LoROM and WRAM access;
- PPU, APU, SPC700, DSP, DMA, cartridge, SRAM, and joypad primitives;
- frame/audio pacing helpers, save-state primitives, checksums, and keybinds;
- unresolved-control-flow traps and optional trace rings;
- MSU-1 register/data/audio support;
- host-overlay extraction hooks in the PPU;
- optional shadow audio and display color-LUT modules; and
- CMake source/include lists in `runtime-next/runner.cmake`.

The per-game project provides the executable frontend, verified ROM loading,
configuration/settings, `RtlGameInfo`, reset/frame/interrupt policy, HLE
bodies, and all ROM-address-specific behavior. See
`PROJECT_INTEGRATION.md`.

## `RtlGameInfo`

Register exactly one static `RtlGameInfo` before `SnesInit`:

- `title`: stable identifier used by diagnostics/default save names;
- `initialize`: optional reset callback;
- `run_frame`: required frame callback used by the shared loop;
- `draw_ppu_frame`: optional drawing callback;
- `read_rdnmi`: optional per-game `$4210` override, returning `-1` to
  delegate;
- `recover_dispatch_miss`: optional policy gate for verified
  computed-dispatch recovery sites; and
- `save_name_prefix`: optional `saves/<prefix>N.sav` prefix.

The callbacks isolate game policy from the SNES core. Shared runtime code must
not add hard-coded ROM addresses or game-prefixed external symbols.

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

Bindings such as `PpuBindOverlaySurface` are persistent host resources.
Capture descriptors are per-frame policy and must be cleared or re-established
each frame. Mode-7 canvas-space substitution is available through
`PpuBindMode7OverlaySurface` and `PpuSetMode7Override`.

## Trace and debug status

`SNESRECOMP_ENABLE_TRACE=ON` compiles local CPU, dispatch, and watchdog
instrumentation. The runtime provides entry/call/exit M/X assertions,
stack-balance checks, garbage-variant traps, dispatch-miss logging, and
unresolved-stub traps. The retired TCP debug server was intentionally not
carried into the independent runner.

Several diagnostic symbols retain historical `ar_` or `AR_` names because
they are part of the generated/runtime compatibility ABI. They do not make the
runner game-specific.

## Portability contract

- Core code is portable C11 with fixed-width public ABI types.
- OS, SDL, graphics, and audio-library types stay behind host adapters.
- Hot read paths do not allocate.
- Native, forced-portable, standalone subsystem, root-project, and Zig
  cross-target builds provide the validation gates.
- Optional SIMD is selected by compiler target macros and always retains the
  portable implementation.

The post-cutover component-access and low-copy ABI plan is tracked in
`../runtime-next/ABI_ROADMAP.md`.
