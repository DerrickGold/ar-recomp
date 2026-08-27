# Game enhancement integration {#game_enhancement_integration}

This guide is for a project that already runs recompiled SNES code and now
wants game-aware widescreen rendering, separated graphics layers, enhanced
audio, or replacement assets. The runner provides hardware mechanisms. The
game project provides meaning: world coordinates, finite map bounds, HUD
ownership, track identities, and the decompiled producers that create those
resources.

The key rule is simple: enhance the producer, not the final framebuffer. A
256-pixel SNES image does not contain the tiles that should exist beyond its
edges, and a stream of DSP writes does not contain a stable human-facing track
name. Recover that information from the game code and publish it through the
runner contracts.

## Public header boundary

All supported integration headers are under `runtime/include/snesrecomp`:

| Header | Intended consumer |
| --- | --- |
| `snesrecomp/runner.h` | Frontends, tools, and enhancement code using the versioned, capability-gated runner ABI |
| `snesrecomp/game.h` | The linked game's immutable identity, lifecycle, execution, state-provider, and audio module tables |
| `snesrecomp/game_runtime.h` | Low-overhead synchronous helpers for the linked game frame loop and instrumentation |
| `snesrecomp/game_audio.h` | Callback-lifetime DSP/SPC routing and extension contexts |
| `snesrecomp/spc_upload.h` | Generic SPC upload parsing and the bounded upload transaction |
| `snesrecomp/game/cpu.h` | `CpuState` and the generated 65816 function ABI |
| `snesrecomp/game/runtime.h` | Generated-code memory, register, frame, and audio entry points |
| `snesrecomp/game/bootstrap.h` | Frontend runner lifecycle and `RtlGameModule` registration |
| `snesrecomp/game/generated_support.h` | Watchdog, recompiled-call-stack, block-history, and tail-call support emitted C depends on |
| `snesrecomp/game/snes_regs.h` | Stable SNES MMIO addresses used by decompiled/HLE game code |
| `snesrecomp/game/apu_sync.h` | Generated-code APU synchronization constants and entry points |
| `snesrecomp/game/runtime_constants.h` | Fixed capacities shared by generated runtime structures |
| `snesrecomp/game/trace.h` | Generated-code trace hooks and no-trace inline forms |
| `snesrecomp/game/types.h` | Fixed-width generated-code support types |
| `snesrecomp/host/*.h` | Optional launcher, presentation, frame-dump, and audio-trace host contracts |
| `snesrecomp/support/*.h` | Stateless file and checksum helpers |

Do not include anything from `runtime/src`, `runtime/src/snes`, or an
`*_internal.h` file. Those headers describe implementation state and can change
without notice. Legacy short-header forwarders have been removed; current
recompiler output includes the namespaced headers directly.

Frontend code normally needs `game/bootstrap.h`; generated banks and HLE glue
need `game/generated_support.h`. Keeping them separate prevents ordinary host
integration from accidentally depending on watchdog rings or the recompiled
call-stack implementation.

For a source build, `runner.cmake` exports:

- `SNESRECOMP_RUNNER_SOURCES` for one dedicated static-library target;
- `SNESRECOMP_RUNNER_PUBLIC_INCLUDE_DIRS` for authored game/frontend code;
- `SNESRECOMP_RUNNER_PRIVATE_INCLUDE_DIRS` for compiling that library only; and
- `snesrecomp_configure_runtime_target()` to apply the supported public/private
  include boundary and portable build options.

Compile runner sources into a static library and link the game to it. Do not
add runner sources or private includes to the game target. An installed or
vended SDK exposes the same boundary as `snesrecomp::runtime` and does not
require `runtime/src` at all.

For capability-to-function mapping, table extents, result codes, threading,
and pointer lifetime rules, use [`API_REFERENCE.md`](API_REFERENCE.md).

## Base game registration

Register one immutable `RtlGameModule` before `SnesInit`. Identity and
execution are required. Add lifecycle, state-provider, and audio tables only
when their capability bits are present. Every table and string must remain
alive until shutdown.

```c
#include "snesrecomp/game/bootstrap.h"

static const RtlGameIdentity kIdentity = {
  .struct_size = RTL_GAME_IDENTITY_V1_SIZE,
  .game_id = "my_game",
  .display_name = "My Game",
  .save_name_prefix = "save",
};

static const RtlGameExecutionApi kExecution = {
  .struct_size = RTL_GAME_EXECUTION_API_V2_SIZE,
  .run_frame = RunOneFrameOfGame,
  .draw_ppu_frame = DrawEnhancedFrame,
};

static const RtlGameModule kGame = {
  .abi_version = RTL_GAME_MODULE_ABI_VERSION,
  .struct_size = RTL_GAME_MODULE_V2_SIZE,
  .capabilities = RTL_GAME_MODULE_CAP_IDENTITY |
                  RTL_GAME_MODULE_CAP_EXECUTION,
  .identity = &kIdentity,
  .execution = &kExecution,
};

bool RegisterMyGame(void) {
  return RtlRegisterGame(&kGame) == SR_RESULT_OK;
}
```

Use the lifecycle `runner_changed` callback to bind long-lived enhancement
objects to the opaque `SrRunnerHandle`. Treat the mutable ROM pointer in the
initialization callback as callback-scoped. Never retain it.

## What must be decompiled for widescreen

Start with a correct authentic renderer. Widescreen should be a semantic layer
above it, not a replacement for SNES timing, HDMA, windows, color math, or
priority rules.

For each scene family, locate and decompile these producers:

1. **Camera producer.** Find the game variables and routines that compute the
   world-space camera, scroll registers, shake, scripted locks, and room
   transitions. Do not infer the camera only from the final PPU register: many
   games add row effects or wrap a smaller hardware value around a larger world
   coordinate.
2. **Background tile producer.** Find the routine that converts a world or room
   coordinate into a tilemap entry and the code that streams columns/rows into
   VRAM. Recover the source map dimensions, metatile expansion, palette and
   priority bits, empty/out-of-bounds policy, and any per-room indirection.
3. **Extent producer.** Identify finite room edges, wraparound maps, clamps,
   split-screen bands, and rows that deliberately move differently. These
   decisions belong to the game, not the generic PPU.
4. **Object producer.** Find where game entities become OAM entries. Preserve
   an unwrapped or camera-relative X position before the SNES truncates it to
   nine bits. Identify HUD-owned OAM ranges separately from world objects.
5. **Raster-effect producer.** Identify IRQ/HDMA tables that change scroll,
   mode, windows, mosaic, or color math within a frame. Publish row bands or
   let runner-owned scanout execute HDMA; do not assume the whole screen shares
   the register values seen at VBlank.

This is the part an integrator cannot skip. Repeating the visible 32x32 or
64x64 hardware tilemap may be acceptable for a deliberately repeating scene,
but it cannot reconstruct a finite world that the game has already scrolled
out of VRAM. Stretching edge pixels or copying the last tile produces the
smearing and false geometry this API is designed to avoid.

## Widescreen frame flow

Perform enhancement work on the emulation thread at the draw safe point:

1. Call `reset_ppu_frame_state` to clear transient captures, OBJ relocation,
   and Mode-7 override state without unbinding persistent host surfaces.
2. Build an `SrPpuFramePolicy` and apply BEGIN. This atomically establishes the
   horizontal/vertical budget, layer clamp/mirror/repeat/normal-scroll masks,
   row bands, vertical clips, capture padding, and HUD split geometry. The
   linked hot path may use `RtlGameApplyPpuFramePolicy`; other consumers use
   `SnesRunnerApi.apply_ppu_frame_policy` with the current lifetime generation.
3. Publish game-owned virtual tile providers with
   `replace_ppu_virtual_tilemaps` and finite bounds with
   `update_ppu_layer_extents`.
4. If exact fallback policy depends on which providers were accepted, apply
   the same budget again with `SR_PPU_FRAME_POLICY_FINALIZE`. FINALIZE preserves
   the providers and extents installed after BEGIN.
5. Publish camera-relative object metadata with `update_ppu_obj_metadata` and,
   when comparison rendering needs it, authentic per-row camera values with
   `update_ppu_authentic_camera`.
6. Claim separated BG/OBJ or Mode-7 captures. Bind persistent host-owned output
   buffers with `bind_ppu_output_surface`.
7. Run composition inside `visit_ppu_frame_transaction`. Its snapshots,
   VRAM/CGRAM/OAM borrows, and writable surfaces are coherent and valid only
   for the callback. Use bounded services such as OBJ resolve/raster and
   compare/exchange rather than casting a component handle.
8. Call `run_ppu_scanout`. The runner owns scanline progression, HDMA effects,
   margin hold behavior, and vertical IRQ scheduling. The game callback owns
   only the recompiled CPU's IRQ handler.

Every request must check the required capability bit, API extent, return value,
and generation. A borrowed pointer is not a cache. Persistent game-side source
maps and provider `user_data` remain game-owned and must outlive the frame in
which they are published.

## Implementing a virtual tile provider

`SrPpuVirtualTilemapBinding.lookup` is the correctness path. Given signed world
tile coordinates, return the exact SNES tilemap word that the original producer
would have placed in VRAM, or report that the coordinate has no tile. Keep
metatile decoding and room-specific semantics in the game project.

Once correct, implement `lookup_span`. Scanout commonly asks for adjacent
tiles, so returning a contiguous or fixed-stride run avoids a callback for each
pixel/tile. `band_lookup` is an optional fast path for priority/depth grouping;
it must agree with the scalar result. Never make the scalar callback a slow
framebuffer search—it should index already-decoded game data.

Useful validation for every provider:

- scalar and span answers agree at negative, zero, boundary, and wrapped
  coordinates;
- authentic-width output is unchanged when the provider is enabled;
- left and right margins agree with a camera moved to the same world positions;
- finite rooms stop cleanly while intentional wraparound maps repeat;
- per-row scroll/HDMA scenes are tested above and below every band transition;
  and
- portable builds are benchmarked separately from architecture-specific SIMD.

## Sprites, HUD, and separated layers

OAM alone is insufficient for wide world placement after the game has wrapped
an X coordinate. Publish exact or camera-relative positions while decompiling
the entity-to-OAM producer. Use runner constants for SNES wrap behavior and
keep HUD ranges explicitly screen-relative.

Separated capture is ownership based. A successful overlay claim owns that
source for the frame; a conflicting claim returns `SR_RESULT_BUSY`. Use
compare/exchange when temporarily replacing another known capture policy, and
restore only the value you actually observed. Flags describing color math,
subscreen participation, and removal from the authentic image are part of the
claim—do not repair transparency later by guessing from RGB values.

For 3D or HD composition, bind caller-owned surfaces once and write directly
inside the frame transaction. Avoid copying full framebuffers between game and
runner. Derived OBJ parts and rasters use caller-provided buffers so the runner
can remain allocation-free on the hot path.

## Enhanced audio integration

The baseline SPC700 and S-DSP path requires no game-specific audio code. Add an
`RtlGameAudioApi` only for semantics the hardware stream cannot provide by
itself.

The usual discovery workflow is:

1. Find the main CPU routine that uploads the sound driver and sample banks.
   Implement the bounded SPC upload callbacks at that already-synchronous safe
   point; use `SrSpcUploadContext` rather than accessing an APU/SPC structure.
2. Decompile the game's music/SFX command protocol: ports used, command bytes,
   track IDs, bank changes, fades, and stop/resume behavior. Record stable game
   events before they collapse into generic DSP register writes.
3. Trace the driver code that assigns the eight hardware voices. Use DSP-write
   routing callbacks to classify voices as music, SFX, or unclassified. Restore
   classifications from ARAM in the state-loaded callback.
4. If the project supports extended voices, use the bounded operation callback
   in `RtlAudioExtensionContext`. Do not retain its ARAM pointer or mutate DSP
   internals directly.
5. Keep the semantic track catalogue in the game project: stable ID, display
   name, expected upload/command signature, loop metadata, and replacement
   asset path. The runner should not contain title-specific track names.

Replacement playback should subscribe to those recovered semantic events and
apply music/SFX bus gains through `configure_audio_mix`. Preserve a portable
fallback through the original SPC/DSP path. Preview extraction is a separate
offline tool: it may run the portable audio core and emit WAV files, but the
browser/UI does not need an SPC player and the runtime ABI does not need to
expose live SPC layout.

Validate audio with original-only, replacement-only, and mixed output; rapid
track changes; save/load during a note and fade; pause/turbo; all known upload
variants; and an unknown track ID that falls back safely.

## Common integration mistakes

- **Framebuffer extrapolation instead of producer recovery:** creates repeated,
  smeared, or contextually wrong margin tiles.
- **Using one frame-wide register snapshot for an HDMA scene:** breaks rows
  whose scroll/window/color state changes during scanout.
- **Retaining transaction borrows:** becomes invalid after callback return or a
  generation-changing mutation.
- **Publishing a provider before frame-policy BEGIN:** BEGIN intentionally
  clears previous-frame providers and extents.
- **Wrapping sprite X before publishing metadata:** loses the world position
  needed to place an object in a margin.
- **Treating every DSP voice as music:** suppresses or replaces sound effects.
- **Putting game IDs or ROM addresses in the runner:** makes the core less
  reusable and prevents another game from using the same mechanism.
- **Including private runner headers:** couples the game to concrete layouts and
  bypasses lifetime, ownership, and validation rules.

## Definition of done for a new game

A game integration is ready when authentic rendering and audio still match the
unmodified path; no authored source includes `runtime/src`; all provider
and audio semantics are game-owned; widescreen covers finite, repeating,
HDMA-heavy, Mode-7, HUD, and sprite scenes found in the title; save/load and
replay artifacts remain deterministic; and portable plus native performance
stay within the project's measured gates.
