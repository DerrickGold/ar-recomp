# Shared runtime

`runtime/` is the C execution environment for code emitted by
`snesrecomp-go`. It remains C because generated functions are C and link
directly against its ABI. Go is needed for regeneration, not at game runtime.

## Boundary

The shared layer provides:

- `CpuState`, generated call/return/dispatch support, LoROM and WRAM access;
- PPU, APU, SPC700, DSP, DMA, cartridge, SRAM, and joypad primitives;
- frame/audio pacing helpers, save-state primitives, checksums, and keybinds;
- unresolved-control-flow traps and optional trace rings;
- MSU-1 register/data/audio support;
- host-overlay extraction hooks in the PPU;
- optional shadow audio and display color-LUT modules; and
- CMake source/include lists in `runtime/runner.cmake`.

The per-game project provides the executable frontend, verified ROM loading,
configuration/settings, `RtlGameInfo`, reset/frame/interrupt policy, HLE
bodies, and all ROM-address-specific behavior. See `PROJECT_INTEGRATION.md`.

## `RtlGameInfo`

Register exactly one static `RtlGameInfo` before `SnesInit`:

- `title`: stable identifier used by diagnostics/default save names;
- `initialize`: optional reset callback;
- `run_frame`: required frame callback used by the shared loop;
- `draw_ppu_frame`: optional drawing callback;
- `read_rdnmi`: optional per-game `$4210` override, returning `-1` to delegate;
- `recover_dispatch_miss`: optional policy gate for verified computed-dispatch
  recovery sites; and
- `save_name_prefix`: optional `saves/<prefix>N.sav` prefix.

The callbacks isolate game policy from the SNES core. Shared runtime code must
not add hard-coded ROM addresses or game-prefixed external symbols.

## MSU-1

The runtime implements the `$2000-$2007` MSU-1 contract, `.msu` data channel,
and 44.1 kHz signed-16 stereo PCM mixing. It is inert unless a game ROM drives
the registers and a pack is explicitly configured.

Set `SNESRECOMP_MSU1` to either a pack base prefix or directory. Track `N`
resolves as `<prefix>-N.pcm`; data resolves as `<prefix>.msu`. A PCM file uses
the usual eight-byte header (`MSU1` plus a little-endian loop frame), followed
by stereo PCM. Missing/invalid tracks set the error bit so a ROM driver can
fall back to SPC audio.

Vanilla ROMs without an MSU-1 driver do nothing. Recompiling a patched ROM is a
new binary input and requires its own cfg coverage, checksum pin, and complete
validation.

## Host-overlay extraction

The PPU can export already-rendered BG/OBJ rectangles into transparent ARGB
surfaces without mutating emulated memory or savestate data. The runtime owns
tile decode, windows, mosaic, palette, brightness, and source isolation; the
game layer decides what rectangle/OAM range is valid and the frontend decides
how to compose or replace it.

Bindings (`PpuBindOverlaySurface`) are persistent host resources. Capture
descriptors (`PpuSetOverlayCapture`, `PpuSetOverlayOamRange`) are per-frame
policy and must be cleared/re-established each frame. Passing no binding is a
deterministic no-op suitable for headless/oracle runs.

Mode-7 canvas-space override is available through
`PpuBindMode7OverlaySurface` and `PpuSetMode7Override`. Substitution follows
the live matrix and scanline state; the game project remains responsible for
choosing a verified canvas rectangle.

## Optional enhancement modules

The runtime includes an opt-in S-DSP shadow renderer and display color LUT.
Their governing rule, inherited from the source project, is that the authentic
path remains authoritative: an enhancement may substitute only after
differential proof and must fall back loudly if it diverges. Both are off by
default.

## Trace and debug status

`SNESRECOMP_ENABLE_TRACE=ON` compiles the local CPU/dispatch/watch rings and
tripwire instrumentation. The copied historical TCP debug server has drifted
from the current trace structures, so `runner.cmake` links
`debug_server_stub.c`; TCP query commands described in old source documents are
not currently available through this standalone build.

Generated code and the runtime still expose valuable local checks, including
entry/call/exit M/X assertions, stack-balance checks, garbage-variant traps,
dispatch-miss logging, and unresolved-stub traps. Projects should document the
environment switches they expose rather than assuming the old Python
project's branch-specific debug guides apply unchanged.

Several diagnostic symbols and environment variables retain the historical
`ar_`/`AR_` prefix because they are part of the output/runtime compatibility
ABI. The prefix does not make the recompiler depend on ActRaiser, but some
opt-in probes inside the copied trace implementation were created for specific
SMW, MMX, or ActRaiser investigations. Treat those probes as historical until
a consuming project verifies and documents them for its own ROM.

## Known portability constraints

- The maintained integration path is CMake plus the source list in
  `runner.cmake`.
- SDL2 is currently part of that list through `keybinds.c`.
- Optional launcher GUI sources/assets exist under `runtime/src/launcher/` but
  are not in the default runner source list and require their own RmlUi/front-
  end integration.
- `debug_server.c` is retained for provenance and future repair, but is not
  built by the default or trace source lists.
- Platform claims should be based on each consuming project's CI/build matrix;
  the ActRaiser host project currently validates macOS most thoroughly.

## Provenance

The runtime was copied from the historical Python project's `runner/` tree.
See `../ATTRIBUTION.md` and `runtime/README.md` before redistribution.
