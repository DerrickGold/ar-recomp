# Runner SDK API reference {#api_reference}

This reference is the lookup companion to
[`GAME_ENHANCEMENT_INTEGRATION.md`](GAME_ENHANCEMENT_INTEGRATION.md). That
guide explains how to recover game semantics and structure an enhancement;
this document explains how to call the public C contracts safely.

All supported headers are under `include/snesrecomp`. Nothing under `src` is
an SDK contract.

## Which surface to use

| Consumer | Primary header | Contract |
| --- | --- | --- |
| Frontend/bootstrap | `snesrecomp/game/bootstrap.h` | Register the linked game, create the runner, and shut it down |
| Linked game hot path | `snesrecomp/game_runtime.h` | Small synchronous helpers for frame timing, PPU policy, and instrumentation |
| Enhancement, tool, or host layer | `snesrecomp/runner.h`, or a narrow `snesrecomp/runner/*.h` domain header | Versioned, capability-gated access through `SnesRunnerApi` |
| Generated C | `snesrecomp/game/*.h` | 65816 register, dispatch, tracing, and generated-support ABI |
| Host conveniences | `snesrecomp/host/launcher.h` | Portable ROM loading and launch-path helpers; presentation and diagnostics use the runner ABI or application-owned code |
| Runner replay artifacts | `snesrecomp/runner/replay.h` | Stateful canonical host-frame input streams over caller-owned transport |
| Stateless utilities | `snesrecomp/support/*.h` | File loading, CRC32, packing, and other transport-independent helpers |

Use the linked-game helpers only inside the executable linked to this runtime.
External tools and optional enhancement layers should use `SnesRunnerApi` so
they can validate ABI version, table extent, and capabilities.

## Linking contract

Link the game to the CMake target `snesrecomp::runtime`. A source checkout can
create that target from `runner.cmake`; an installed SDK provides it through
`find_package(snesrecomp-runtime CONFIG REQUIRED)`. In both forms the game sees
only `include/snesrecomp`. It must not compile runner sources, add `runtime/src`
to its include path, or name individual runner object files.

Prebuilt archives are target-specific. Select a library matching the target
operating system, CPU architecture, object format, and build options. The
archive intentionally retains unresolved generated-game callbacks until the
final executable link; the game module supplies those symbols.

`snesrecomp/runner.h` is the stable umbrella. Consumers that need a smaller
surface may include `runner/base.h`, `ppu.h`, `events.h`, `audio.h`,
`mutation.h`, `determinism.h`, `input.h`, `replay.h`, or `api.h`; each domain header is independently compile-tested
in C11 and the umbrella is compile-tested in C++17.

## Acquiring and checking the API

`sr_runner_get_api(SR_RUNNER_ABI_VERSION)` returns a process-lifetime table or
`NULL`. Before calling an operation, check both its capability bit and the byte
extent that reaches the operation. A non-null function pointer alone is not a
version check.

```c
static const SnesRunnerApi *GetRunnerApi(uint64_t capability,
                                         uint32_t minimum_size) {
    const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
    if (api == NULL || api->abi_version != SR_RUNNER_ABI_VERSION ||
        api->struct_size < minimum_size ||
        (api->capabilities & capability) == 0u) {
        return NULL;
    }
    return api;
}
```

Every input/output structure with `struct_size` must be initialized to its
named `*_V2_SIZE` macro. Zero-initialize the complete value first so reserved
fields remain zero.

## Result codes

| Result | Meaning | Normal response |
| --- | --- | --- |
| `SR_RESULT_OK` | Operation completed | Consume the output |
| `SR_RESULT_INVALID_ARGUMENT` | Bad size, range, flags, pointer, or combination | Fix the request; no partial mutation is performed |
| `SR_RESULT_UNSUPPORTED` | Capability, version, memory region, or operation is not implemented | Disable that optional feature or use the portable fallback |
| `SR_RESULT_UNAVAILABLE` | Valid service, but its required resource/producer/sample does not exist yet | Retry only after the required lifecycle step; do not reinterpret it as an empty sample |
| `SR_RESULT_STALE_VIEW` | Supplied lifetime generation no longer matches | Re-query generations/snapshot and rebuild the request |
| `SR_RESULT_PENDING` | Queued mutation has not reached its safe point | Query it later |
| `SR_RESULT_BUSY` | A frame resource is already claimed or registration is unsafe now | Respect current ownership or retry in the next lifecycle/frame |

## Ownership and lifetime

| Value kind | Lifetime | May be retained? |
| --- | --- | --- |
| `SnesRunnerApi` and its function pointers | Process lifetime | Yes |
| `SrRunnerHandle` from `runner_changed`/`RtlGameRunner` | Until the matching `runner_changed(NULL)` | Yes, but never dereference it |
| Copied snapshot structures | Caller-owned copy | Yes; their generation describes when they were captured |
| `SrBorrowedSpan`, `SrBorrowedU16Span`, surface snapshots | Until validation fails, normally the next tick/reset/load/mutation | Descriptor yes; pointed-to memory only while valid |
| Event/audio callback payload pointers | Callback only | No; copy the bounded data needed later |
| PPU frame transaction context and emulated-memory pointers | Callback only | No |
| Bound output surface storage | Host-owned | The owner retains its storage; keep it alive until unbound/shutdown |
| Virtual tilemap callbacks and `user_data` | Game-owned, retained by runner | Keep alive until replacement/reset/shutdown |
| Frame-policy bands and most request arrays | Copied or consumed synchronously | Caller may release after the call returns |

Borrowed memory is immutable through the SDK. Do not cast away `const`. Use a
validated safe-point mutation or a purpose-built compare/exchange operation
when state must change.

For observational APIs, current state and retrospective samples are different
contracts. A current-state query may return `OK` with every value zero when
zero is the real initialized hardware state. A retrospective query returns
`UNAVAILABLE` until its producer has actually completed; it must not fabricate
an all-zero “sample.” Validity flags, rather than digest contents or numeric
values, distinguish present data from absent data.

## Capability matrix

The “minimum extent” is the smallest `api->struct_size` that permits the named
operation. When one capability exposes multiple entries, check the extent for
the specific entry you call.

| Capability | Minimum API extent | Operations | Execution/lifetime notes |
| --- | --- | --- | --- |
| `SR_RUNNER_CAP_COMPONENT_HANDLES` | `SNES_RUNNER_API_V2_BASE_SIZE` | `get_component` | Opaque identity only; never cast the result |
| `SR_RUNNER_CAP_GENERATION_COUNTERS` | `SNES_RUNNER_API_V2_BASE_SIZE` | `query_generations` | Copied, synchronous |
| `SR_RUNNER_CAP_BORROWED_BYTE_SPANS` | `SNES_RUNNER_API_V2_BASE_SIZE` | `borrow_memory`, `borrow_is_valid` | Emulation-thread, thread-confined borrow |
| `SR_RUNNER_CAP_CPU_STATE` | `SNES_RUNNER_API_CPU_STATE_SIZE` | `query_cpu_state` | Copied snapshot |
| `SR_RUNNER_CAP_PPU_STATE` | `SNES_RUNNER_API_PPU_STATE_SIZE` | `query_ppu_state` | Instantaneous call-time controls; not an already-composited frame timeline |
| `SR_RUNNER_CAP_BORROWED_U16_SPANS` | `SNES_RUNNER_API_PPU_STATE_SIZE` | `borrow_u16_memory`, `borrow_u16_is_valid` | Host-native VRAM/CGRAM/OAM words |
| `SR_RUNNER_CAP_PPU_FRAME_STATE` | `SNES_RUNNER_API_PPU_FRAME_STATE_SIZE` | `query_ppu_frame_state` | Copied frame-derived policy/capture state |
| `SR_RUNNER_CAP_PPU_OBJ_RASTER` | `SNES_RUNNER_API_PPU_OBJ_RASTER_SIZE`, `...OBJ_RESOLVE_SIZE`, or `...OBJ_PARTS_SIZE` | `rasterize_ppu_obj_range`, `resolve_ppu_obj_range`, `rasterize_ppu_obj_parts` | Caller-owned output buffers; emulation-thread; resolve priority filters mixed ranges and returns `OK` with zero parts when no part matches |
| `SR_RUNNER_CAP_PPU_SURFACE_VIEWS` | `SNES_RUNNER_API_PPU_SURFACE_SIZE` | `query_ppu_surfaces`, `ppu_surface_snapshot_is_valid` | Borrowed host-surface views |
| `SR_RUNNER_CAP_EXECUTION_STATE` | `SNES_RUNNER_API_EXECUTION_STATE_SIZE` | `query_execution_state` | Requires linked-game state provider |
| `SR_RUNNER_CAP_EVENT_OBSERVERS` | `SNES_RUNNER_API_EVENT_OBSERVER_SIZE` | `subscribe_events`, `unsubscribe_events` | Install/remove while execution is stopped; callbacks are synchronous |
| `SR_RUNNER_CAP_SAFE_POINT_MUTATIONS` | `SNES_RUNNER_API_SAFE_POINT_MUTATION_SIZE` | `queue_mutation`, `query_mutation` | Queue copies the command; applied before a host frame observes state |
| `SR_RUNNER_CAP_PPU_BACKGROUND_COORDINATE` | `SNES_RUNNER_API_PPU_BACKGROUND_COORDINATE_SIZE` | `resolve_ppu_background_coordinate` | Synchronous derived lookup |
| `SR_RUNNER_CAP_PPU_OUTPUT_CONTROL` | `SNES_RUNNER_API_PPU_OUTPUT_CONTROL_SIZE` | `bind_ppu_output_surface`, `configure_ppu_horizontal_margin` | Persistent host binding/configuration |
| `SR_RUNNER_CAP_PPU_CAPTURE_CONTROL` | `SNES_RUNNER_API_PPU_CAPTURE_CONTROL_SIZE` | `claim_ppu_overlay_capture`, `claim_ppu_mode7_override` | Frame-scoped claim; conflicts return `BUSY` |
| `SR_RUNNER_CAP_CPU_MATH_STATE` | `SNES_RUNNER_API_CPU_MATH_STATE_SIZE` | `query_cpu_math_state`, `restore_cpu_math_state` | Restore only at an emulation-thread safe point |
| `SR_RUNNER_CAP_AUDIO_TRACE_OBSERVERS` | `SNES_RUNNER_API_AUDIO_TRACE_OBSERVER_SIZE` | `subscribe_audio_trace`, `unsubscribe_audio_trace` | Callback may run under APU lock; never call mutating audio services from it |
| `SR_RUNNER_CAP_SPC_CONTROL` | `SNES_RUNNER_API_SPC_CONTROL_SIZE` | `compare_exchange_spc_pc` | Atomic APU-locked compare/exchange; not from audio callback |
| `SR_RUNNER_CAP_AUDIO_MIX_CONTROL` | `SNES_RUNNER_API_AUDIO_MIX_CONTROL_SIZE` | `configure_audio_mix` | Synchronous native bus gain, mute, and unclassified fallback policy |
| `SR_RUNNER_CAP_PPU_FRAME_TRANSACTIONS` | `SNES_RUNNER_API_PPU_FRAME_TRANSACTION_SIZE` | `visit_ppu_frame_transaction`, `compare_exchange_ppu_overlay_captures` | Coherent callback-lifetime frame state and atomic capture replacement |
| `SR_RUNNER_CAP_PPU_VRAM_PATCH` | `SNES_RUNNER_API_PPU_VRAM_PATCH_SIZE` | `compare_exchange_ppu_vram_words` | Atomic sorted sparse patch; no partial write |
| `SR_RUNNER_CAP_PPU_OBJ_METADATA` | `SNES_RUNNER_API_PPU_OBJ_METADATA_SIZE` | `update_ppu_obj_metadata` | Synchronous game-owned unwrapped positions |
| `SR_RUNNER_CAP_DMA_STATE` | `SNES_RUNNER_API_DMA_STATE_SIZE` | `query_dma_state` | Copied coherent DMA/HDMA snapshot |
| `SR_RUNNER_CAP_PPU_BACKGROUND_POLICY` | `SNES_RUNNER_API_PPU_BACKGROUND_POLICY_SIZE` | `update_ppu_layer_extents`, `replace_ppu_virtual_tilemaps`, `update_ppu_authentic_camera` | Frame/game-owned providers; observe each request's copy/retain rules |
| `SR_RUNNER_CAP_PPU_SCANOUT` | `SNES_RUNNER_API_PPU_SCANOUT_SIZE` | `run_ppu_scanout` | Runner owns scanlines, `$420C`-armed HDMA, IRQ timing, and margin hold; request suppression can only narrow HDMA |
| `SR_RUNNER_CAP_GAME_TIMING_CONTROL` | `SNES_RUNNER_API_GAME_TIMING_CONTROL_SIZE` | `control_game_timing` | Timing-latch transitions only; the game adapter owns body/NMI/scanout ordering |
| `SR_RUNNER_CAP_INPUT_STATE` | `SNES_RUNNER_API_INPUT_STATE_SIZE` | `query_input_state` | Copied controller state |
| `SR_RUNNER_CAP_PPU_FRAME_POLICY` | `SNES_RUNNER_API_PPU_FRAME_POLICY_SIZE` | `apply_ppu_frame_policy` | BEGIN clears prior frame providers; FINALIZE preserves newly published ones |
| `SR_RUNNER_CAP_PPU_FRAME_RESET` | `SNES_RUNNER_API_PPU_FRAME_RESET_SIZE` | `reset_ppu_frame_state` | Begin-frame clear of derived capture/override state; persistent surfaces remain bound |
| `SR_RUNNER_CAP_PPU_OBJ_CAPTURE` | `SNES_RUNNER_API_PPU_OBJ_CAPTURE_SIZE` | `configure_ppu_obj_capture` | Frame-scoped; caller surface must survive scanout |
| `SR_RUNNER_CAP_APU_STATE_SNAPSHOT` | `SNES_RUNNER_API_APU_STATE_SNAPSHOT_SIZE` | `query_apu_state` | Copies coherent ARAM, visible DSP registers, and scalar timing under the APU lock; returns `BUSY` from audio/trace callbacks |
| `SR_RUNNER_CAP_SEMANTIC_DIGEST` | `SNES_RUNNER_API_SEMANTIC_DIGEST_SIZE` | `query_semantic_digest` | Canonical current runner-hardware state; the game CPU provider runs before the APU lock, and the operation returns `BUSY` from audio/trace callbacks |

## Memory-region operation matrix

`SrMemoryRegion` identifies storage across several APIs; it does not promise
that every region is borrowable through every accessor.

| Region | `borrow_memory` | `borrow_u16_memory` | Coherent copied access |
| --- | --- | --- | --- |
| `SR_MEMORY_WRAM` | Supported | Unsupported | Not required |
| `SR_MEMORY_SRAM` | Supported | Unsupported | Not required |
| `SR_MEMORY_ROM` | Supported | Unsupported | Not required |
| `SR_MEMORY_APU_RAM` | Unsupported | Unsupported | `query_apu_state` |
| `SR_MEMORY_DSP_REGISTERS` | Unsupported | Unsupported | `query_apu_state` |
| `SR_MEMORY_VRAM` | Unsupported | Supported | PPU frame transaction |
| `SR_MEMORY_CGRAM` | Unsupported | Supported | PPU frame transaction |
| `SR_MEMORY_OAM` | Unsupported | Supported | PPU frame transaction |
| `SR_MEMORY_HIGH_OAM` | Supported | Unsupported | PPU frame transaction |

## Common call sequences

### Read CPU or memory state

1. Query generations if several reads must describe the same lifetime.
2. Query the copied CPU snapshot.
3. Borrow only the memory region needed.
4. Finish synchronously or copy the small subset retained later.
5. Call `borrow_is_valid` before reusing a previous borrow.

### Record and verify an input replay

`snesrecomp/runner/replay.h` defines a versioned little-endian artifact,
not filesystem policy. Supply exact-read/exact-write callbacks for a file,
memory buffer, network object, or test fixture.

1. At the chosen safe point, record the ROM SHA-256 and an initial semantic
   digest in `SrInputReplayHeader` when available.
2. Append exactly one `SrInputReplayFrame` per host tick. Ordinals must be
   contiguous; record the effective two-controller packed input exposed by
   `query_input_state`, not raw frontend events.
3. Optionally append one digest checkpoint for the frame just written.
4. Finish the writer. A reader reports `UNAVAILABLE` only after validating the
   footer and physical end-of-stream; transport EOF before the footer or bytes
   after it are malformed artifacts.
5. During replay, feed the recorded packed buttons into the matching host tick
   and disable live input mutations. Treat ordinal or checkpoint mismatch as a
   desynchronization, not as a reason to silently resynchronize.

The container deliberately uses host-frame ordinals. A title's WRAM “game
frame” counter may stall, skip, or update at a different phase and belongs in
title-owned diagnostics rather than the generic stream format.

### Compare deterministic state and presentation

Call `query_semantic_digest` at an emulation-thread safe point. Schema 3 is an
explicit digest-owned traversal, not a save-state traversal. It hashes the
recompiled 65816 snapshot, current packed and serial device input, and runner-owned
APU/SPC/DSP/DMA/PPU/SRAM/WRAM state. It excludes pointers, ABI lifetime
counters, host clocks, DSP PCM delivery, voice-bus mixing policy, diagnostics,
and game-authored HLE/native extension save data. A game that has additional
emulation-affecting native state owns a separate canonical digest for it.

Set `SR_PPU_SCANOUT_CAPTURE_PRESENTATION_DIGEST` on the scanout request for a
frame that needs a checkpoint. That same `run_ppu_scanout` call returns the
digest in `SrPpuScanoutResult.presentation` and sets
`SR_PPU_SCANOUT_PRESENTATION_DIGEST_VALID`. There is no cached “last frame”
query. Later reuse of the caller-owned surface cannot change the copied result;
scanouts without the flag pay no hashing cost and leave the validity bit clear.
Schema 1 hashes the scanout frame ordinal, XRGB8888 format, logical canvas
geometry/margins, and row-major logical pixels encoded little-endian.
Surface pitch padding, centered apron, unused capacity rows, and other bound
surfaces are excluded. Semantic state and presentation have intentionally
separate lifecycles: pair the semantic query and scanout result explicitly in
the replay checkpoint assembled by the adapter.

### Integrate a recompiled frame loop

Generated direct JSR/JSL calls now bracket their hardware return frame with a
stack-local `CpuReturnScope`. An adjusted native RTS/RTL may resume that exact
call when its actual PC/bank, frame kind, and entry stack match, and its final
stack remains below the caller's own return frame. The caller then retains
the native post-return S instead of applying its legacy stack-neutrality
restore. Ordinary equal-stack returns keep their existing fast path. HLEs
which return normally without a native adjusted return retain their existing
stack contract; no return ownership is inferred from an HLE name.

A stronger, narrowly generated contract handles native **return-word
relocation** even when the caller has already consumed its own entry frame.
The emitter recognizes a single-block word pull, stack transfer using a
different register, unchanged word push, and RTS (`PLY/TCS/PHY`, `PLX/TCS/PHX`,
or `PLA/TXS/PHA`). A runtime witness checks that the pull actually read the
current immediate call's original two-byte frame and exact continuation.
Up to four intervening direct/absolute STZ stores require live WRAM and
nonaliasing guards; hardware or mapper-dependent writes invalidate the
witness. No extra guest memory reads, frame pushes, or stack adjustments are
introduced. This is not a whole-function M/X or cleanup-size summary.

Only that preserved origin permits the wider cleanup. Merely finding the
same PC at an ancestor's stack position does not: the original bounds check
and ancestor-return paths remain unchanged. The runtime does not rebase
caller limits, search ancestors for a convenient PC, or create a function for
an internal continuation. Equal-stack returns remain direct fast paths.
The generated `cpu_capture_return_word`, `cpu_return_word_store_disjoint`, and
`cpu_accept_return_word_relocation` calls form this internal compiler/runtime
contract; adapters should not fabricate witnesses. Regenerate and rebuild
against the matching runtime when adopting it. Long return-word/bank shuttles,
branch-crossing shuttles and other effects remain on their existing paths.

Reset has no incoming hardware return frame and may initialize S. An adapter
which invokes compiled reset should explicitly bracket that root (and its
tail-dispatch driver) rather than treating the initial emulation-mode S as
a native caller-frame boundary:

```c
WatchdogFrameStart();
CpuReturnScope reset_owner;
cpu_reset_scope_begin(&reset_owner, &cpu);
RecompReturn result = compiled_reset(&cpu);
if (result == RECOMP_RETURN_TAILCALL)
    result = cpu_dispatch_pc_from(&cpu, g_tailcall_pc24,
                                  g_tailcall_miss_s, g_tailcall_src24);
cpu_return_scope_end(&reset_owner);
/* Handle result; an unexpected return from the game mainline is an error. */
```

Include `snesrecomp/game/cpu.h` and `snesrecomp/game/generated_support.h`.
This is an execution-entry contract, not a configuration directive or
permission to enter arbitrary routines as reset. The reset boundary is
limited to that root activation, including same-activation tail bodies;
nested interrupt routines retain their own hardware-frame boundary. Ordinary
calls, reset scopes, and paired tails run on the owning execution thread.
These records are host bookkeeping, not additions to portable CPU/save state.
Terminal shutdown clears abandoned records. Watchdog resets outside an active
synchronous checkpoint invalidate records without resurrecting old owners;
they must not be used as normal continuation transfers.

This first adjusted-return contract covers known direct calls and native
bank-zero WRAM stacks without wrap. It does not yet replace the legacy
equal-stack rewritten-return rules or establish ownership for every authored
computed-call construct. Unknown/ancestor returns keep their existing paths;
matching a PC alone does not authorize resuming an arbitrary C caller.

`SR_RUNNER_CAP_EXECUTION_CHECKPOINT` advertises the optional appended
`RtlGameExecutionApi.execution_checkpoint(cpu, pc24)` callback. Opt in with
`RTL_GAME_EXECUTION_API_V3_SIZE`; the V2 extent is unchanged and remains valid.
The runner calls it synchronously before a compiled basic block executes.
No generated-code change or callback is needed for existing games.

This is an **execution adapter**, not an event observer. At a recovered safe
wait block it can service a balanced compiled interrupt, scan out a frame,
submit input, and wait for host pacing, then return to the same active C
frame. Restore the interrupted CPU registers, status widths, stack depth,
program-bank bookkeeping, and host-return context. ISR memory effects remain
live. Never register the active continuation as a new callable routine or
abandon its C activation to advance an ordinary frame. Non-local exit is
reserved for terminal shutdown; resuming or loading an abandoned activation
is unsupported. Nested compiled ISR blocks remain traceable but do not
recursively invoke the checkpoint. Calls run on the owning execution thread.

The hook does not prove a polling loop safe, choose a frame schedule, emulate
an interrupt, or replace game code. Those remain explicit adapter duties.
In particular, an error inside an interrupt is not a successful frame.

`SR_RUNNER_CAP_POLL_WAIT` additionally advertises the appended
`RtlGameExecutionApi.poll_wait(cpu, resume_pc24, read_address24, read_width_bytes)`
callback, enabled with `RTL_GAME_EXECUTION_API_V4_SIZE`. V2 and V3 extents
retain their previous meanings. Regenerate with the matching compiler/runtime
pair to insert this seam; older generated C does not invoke it.

The compiler recognizes a deliberately narrow two-instruction self-loop:
an unindexed direct-page, absolute, or long `LDA`/`LDX`/`LDY`, followed by
`BEQ`/`BNE`/`BMI`/`BPL` back to that same block and M/X state. Only the taken
backedge invokes `cpu_poll_wait`, immediately before the original goto.
The runtime offers it to the adapter only when the entire one- or two-byte
read is in WRAM (including its low-bank mirrors). MMIO, ROM, SRAM, wrapping
reads, indexed accesses, masked tests, and read-modify-write loops are not
offered by this first contract. The original read and flags are retained;
the hook performs no additional bus read. With no callback, behavior is
unchanged and the ordinary compiled loop remains in place.

`resume_pc24` names the load at the loop header, not a new function entry.
The address includes live D/DB and the width comes from the decoded M/X
variant. A callback may synchronously service the adapter's interrupt/frame
policy and return to the same activation, subject to the context-restoration
rules above. Poll and checkpoint callbacks share a reentry guard: nested ISR
blocks remain observable but cannot schedule another callback. Watchdog frame
starts inside either callback preserve active return/stack tracking.

Recognition proves a repeated memory test, **not** that NMI owns the flag,
that an interrupt is enabled, or that the loop terminates. The adapter must
choose a suitable schedule and retain a failing watchdog for genuine stalls.
Never clear the polled memory in the host, synthesize ISR results, or treat
each callback as an unconditional successful frame. There is no automatic
hardware-wait lowering, universal cycle-accurate scheduler, or interpreter
fallback here.

`RtlGameExecutionApi.run_frame` is one host tick, not a declaration that the
callback begins at a universal SNES hardware phase. `control_game_timing`
provides two independent timing-latch operations:

- BEGIN positions the modeled beam at VBlank, publishes a fresh `$4210` token,
  and enables forced pacing.
- COMPLETE disables forced pacing and reports whether the live `$4200` gate
  entered NMI.

Neither operation calls the recompiled body or an interrupt handler, and
`run_ppu_scanout` consumes whichever live PPU/DMA state exists when the adapter
invokes it. Consequently, the adapter must recover and own the cyclic schedule.
For example, a wait-token coroutine may resume its body before COMPLETE and
then service the reported NMI, while another game may service the NMI before
running the body that prepares scanout. These are examples, not exhaustive
runner modes.

Event observers make the distinction explicit. HOST_TICK boundaries describe
frontend calls and carry no hardware-phase meaning. GAME_SLICE boundaries
describe BEGIN/COMPLETE; BEGIN also carries VBLANK. SCANOUT boundaries bracket
the synchronous raster transaction. An NMI event qualified TRANSITION reports
the timing-latch transition only. IRQ events qualified CALLBACK bracket the
runner's exact scanline callback; game glue should emit unqualified interrupt
events around actual handler execution.

### Publish widescreen state

1. `reset_ppu_frame_state`.
2. Apply frame policy BEGIN. `SR_PPU_HORIZONTAL_MARGIN_CENTERED` reserves the
   configured wide allocation but keeps a native 256-pixel raster centred in
   it. A wide surface containing only that centred image is the expected mode
   result. Use `SR_PPU_HORIZONTAL_MARGIN_AVAILABLE` when normal PPU scanout
   should rasterize into the side margins.
3. Publish virtual tilemaps, finite extents, authentic cameras, and OBJ metadata.
4. Apply FINALIZE only when fallback policy depends on accepted providers.
5. Claim captures/configure OBJ capture.
6. Compose through `visit_ppu_frame_transaction`.
7. `run_ppu_scanout`. Leave `hdma_suppress_mask` zero for normal hardware
   behavior. Set bits only when an enhancement intentionally suppresses
   channels already armed through `$420C`; the request cannot arm channels.

`margin_top_pixels` and `margin_bottom_pixels` are exact live raster rows; there
is no reserved vertical mode. Scanout renders them above and below the native
224 rows using the PPU state live at those points. Resident VRAM tilemaps work
without another provider. Forced blank, brightness, enabled layers, finite
extents, and any published virtual tilemaps apply normally. The bound main and
authentic surfaces must have room for `224 + top + bottom` rows and for the full
reserved horizontal width. Binding or policy application fails atomically when
that capacity is insufficient.

Virtual scalar providers return one of `SR_PPU_VIRTUAL_TILE_FOUND`,
`SR_PPU_VIRTUAL_TILE_TRANSPARENT`, or
`SR_PPU_VIRTUAL_TILE_FALLBACK_AUTHENTIC`. A provider binding owns every
coordinate for which it is consulted; transparent does not implicitly fall
through to VRAM. Use the explicit fallback result for partial coverage. A span
with null entries is likewise transparent; return a zero-length span at a
mixed-coverage boundary to defer that coordinate to scalar lookup.

### Drive custom composition and high-refresh presentation

The runner has no interpolation clock and does not retain previous presentation
frames. That is intentional: host refresh, window pacing, interpolation
quality, and game-semantic motion belong to the frontend or game renderer. The
runner owns one hardware transaction per emulated tick.

For an image-plane renderer:

1. Bind caller-owned main/authentic and separated overlay surfaces.
2. Publish the frame policy, providers, extents, camera, and OBJ metadata.
3. Run the game-owned preparation and `run_ppu_scanout` once.
4. Copy or upload only the completed caller-owned surfaces needed for a
   previous/current pair. Borrowed surface descriptors must still obey their
   generation rules; ownership of the bound host storage does not transfer.
5. Between emulated ticks, synthesize and present from that immutable pair
   without calling scanout, IRQ, audio, or guest execution again.
6. If a pair is missing or discontinuous, present the exact current endpoint.

Semantic renderers may use `visit_ppu_frame_transaction`, OBJ resolve/raster,
virtual providers, overlays, and Mode-7 replacement as bounded inputs. The
transaction borrows are callback-lifetime only. Entity identity, room formats,
effect reconstruction, previous/current storage, and interpolation eligibility
are not runner services and must not be derived by casting an
`SrComponentHandle` or including private PPU headers.

`SrPpuScanoutLineContext` reports instantaneous line state at the callback's
BEFORE or AFTER_HDMA phase. It does not claim to reconstruct the VRAM/CGRAM/OAM
history of an already completed frame. Leave `line_callback` null outside an
explicit diagnostic. A consumer that keeps its own renderer snapshot or offline
capture owns the copy, serialization format, and performance cost; it is not a
portable emulation save state.

See [Game enhancement integration](GAME_ENHANCEMENT_INTEGRATION.md#custom-composition-and-high-refresh-presentation)
for strategy selection, invalidation, fallback, and conformance guidance.

### Observe and replace audio

1. Recover semantic track/SFX events in the game adapter.
2. Use audio-trace observers only for coherent hardware observation.
3. Route linked-game voices through `RtlGameAudioApi` safe points.
4. Apply original music/SFX bus gains, music replacement muting, and any
   unclassified-source startup fallback through `configure_audio_mix`.
5. Keep file decoding, replacement streams, track names, and manifests in the
   game/frontend layer.

`RTL_GAME_AUDIO_API_V4_SIZE` adds an optional
`extension_spc_opcode_filter(opcode_pc)` preflight. It runs before the runner
constructs a mutable extension context. Returning false skips only the opcode
callback, not execution or `extension_spc_cycle`. The preflight may maintain
game-owned bookkeeping (for example, the current instruction's cycle-charge
latch); it must not access runner state or re-enter the runner. A filter requires
an opcode callback. V2/V3 audio-table extents and null filters retain the
unfiltered behavior. Title-specific instruction addresses belong in the game
adapter, never the runner.

`SrAudioTraceSubscription.event_mask` is required in V2. Use only the event
classes needed by the diagnostic; opcode events are the highest-volume class,
and a zero mask is invalid. Every event carries the live SPC PC, current DSP
slot, and the instruction PC responsible for an in-flight bus operation.
`SR_AUDIO_TRACE_DSP_KEY_ON` additionally carries the continued voice number
(hardware 0..7 or extended 8..39), source number, resolved BRR address, pitch,
and signed left/right volumes. The ARAM pointer is immutable and
callback-lifetime only.

`SrAudioMixControl.flags` can mute the original music bus while a replacement
stream is active. Its optional source-number partition applies only to voices
that remain unclassified; it is a startup fallback, not a replacement for the
game adapter's explicit voice routing. Clearing the flags restores ordinary
bus routing without reaching into DSP state.

For event-independent inspection, allocate 64 KiB of ARAM storage and 128
bytes of DSP-register storage, then call `query_apu_state`. The DSP bytes are
the SNES-visible register image; pending pipeline values are intentionally not
part of the ABI. Do not call the snapshot operation from audio production or
an audio-trace callback.

The native DSP bank follows one 32-slot schedule and exposes voices 0..7.
When a game opts into extended voices, voices 8..39 are four parallel
eight-voice banks running the same BRR decode, Gaussian interpolation,
envelope, noise, key timing, and register-slot rules. Pitch modulation remains
bank-local. Native global registers and noise phase are shared; extended echo
sends enter the one native echo-memory/FIR pass. Each bank applies native-style
master-volume saturation before the five dry outputs are combined. This makes
the extension hardware-shaped and deterministic without pretending it is a
single 40-voice physical S-DSP. Save states serialize all five banks.

An extended bank whose voices have all completed Release is dormant until its
next KON. Waking it copies the native bank's current slot, global counter,
sample index, noise phase, and shared registers before the key event is
consumed. Active banks retain the complete 32-slot schedule. Their local echo
FIR/delay work is deliberately omitted: EON sends join the native bank before
its one physical echo pass, so a second delay line would be redundant and is
not observable through the extended-voice API.

### APU timing ownership and profiling

The private APU batch path coalesces countdown-only cycles between SPC
instructions, timer edges, queued-port application at slot 0, and PCM publication
at slot 31. DSP spans dispatch once on entry, then execute the same ordered slot
bodies, interleaving all active banks before native echo. This is an execution
optimization, not a sample-at-once approximation: register races, ARAM reads,
keying, BRR, envelopes and echo remain slot-accurate. Diagnostic counters or
audio tracing select the original cycle stepper. Production keeps its existing
256-cycle lock chunks and stops at the exact PCM demand; snapshot formats and
public ABI call/borrow lifetimes are unchanged.

Register and voice mirrors retain their existing refresh points. They feed
source-based mix classification, key-on observations, state queries and saved
state; they are not merely a debug display cache. Deferring their refresh is a
separate optimization requiring consumer-by-consumer validation.

The `snesrecomp_runtime_apu_batch` and `snesrecomp_runtime_dsp` tests compare
batched execution with the scalar reference, including partial samples,
extended banks, echo RAM, serialized state and PCM. The APU test executable
also accepts `--benchmark` for an alternating scalar/batched synthetic workload;
its timings are informational, never a CTest pass/fail threshold.

`RtlRunFrame` advances a serialized APU target by the rational NTSC interval
`357366 * 5632 / 118125` APU cycles per game tick (about 60.0988 Hz).
`game/audio_timing.h` defines the shared nominal 1.024 MHz APU / 32 kHz PCM
clock; fractional cycles carry between ticks. Display refresh is independent.
`RtlRenderAudio` may advance the same APU first when
an active consumer needs PCM. Timeline advancement executes only the positive
gap between the target and the actual semantic APU clock, so it never repeats
consumer work. With no consumer, the game tick owns the full gap; headless and
windowed execution therefore use the same target clock. Host wall time and
audio callback block size do not enter emulated port scheduling. The target,
fractional cycle remainder, actual cycle clock, DSP slot, and scheduled writes
are save-state data. Production yields the audio lock at most every 256 APU
cycles and rechecks the clock after reacquiring it. This bounds a producer's
lock-holding work without skipping DSP slots or repeating callback work.

Snapshot version 14 includes the new DSP bus latches and rational phase;
older quick states are rejected, not interpreted under a different layout.
Semantic digest schema 4 includes these execution-affecting fields. The public
runner ABI remains V2; digest schema and snapshot versions are separate.

`RtlAdvanceApuTimeline` is exposed for a custom loop that intentionally bypasses
`RtlRunFrame`; normal integrations must not call both for the same tick.
`RtlApuCycleCount` provides a lock-safe observation of the same semantic clock
for game-side diagnostics. Reset or state load may move that value backwards;
an audio-trace callback must use its event's `cycle_count` instead.

`RtlApuProfileReset` establishes a synchronized measurement baseline and does
not mutate emulated clocks. `RtlApuProfileRead` reports:

| Field | Exact meaning |
| --- | --- |
| `apu_cycles_total` | Semantic APU slots elapsed since the baseline |
| `apu_cycles_audio_demand` | Slots executed to satisfy `RtlRenderAudio` |
| `apu_cycles_port_sync` | Slots executed at CPU/APU port synchronization |
| `apu_cycles_upload_control` | Slots executed by bounded SPC-upload control |
| `apu_cycles_timeline` | Missing target-clock slots executed at game-tick end |
| `apu_cycles_unattributed` | Total minus the four attributed categories |
| `port_sync_calls` / `port_sync_ns` | Count and host cost of port-sync batches only |
| `lock_wait_ns` / `audio_wait_max_ns` | Host synchronization waits, not emulated time |
| `hook_ns` / `upload_ns` | Host cost of game audio hooks and upload processing |
| `scheduled_latency_max` | Largest scheduled CPU-to-APU port latency, in native frames |
| `port_reads` / `port_writes` | CPU-side APU port operations in the interval |

An attribution sum larger than total sets `RTL_APU_PROFILE_INCONSISTENT`
instead of wrapping `apu_cycles_unattributed`.

Host diagnostics use the public runner event and audio-trace subscriptions.
The separate in-process APU-audit recorder is runner-private and is allocated
only for `SNESRECOMP_APU_AUDIT_PREFIX`; it is not a host API. The large SPC
PC/write histograms are likewise pay-for-play through
`SNESRECOMP_SPC_DIAGNOSTICS=1`.

For bring-up, `SNESRECOMP_APU_AUDIT_PREFIX=<path>` enables the recorder and
byte-level ARAM write provenance from APU reset onward. On ordinary runner
teardown it writes `<path>.aram`, `.dsp`, `.written`, and `.audio.jsonl`.
`RtlCaptureApuAudit(path)` provides the same operation at an explicit safe
point. Do not call it from audio production or an audio-trace callback. Analyze
the bundle with `snesbuild apu-audit --prefix <path>`.

The write bitmap covers SPC stores and destinations declared through the
shared HLE image/sample upload helpers. The runner also marks bytes changed by
a game upload-customization callback; a direct callback write that stores the
same value already present in ARAM cannot be distinguished and should not be
used as sole proof that a sample was uploaded. Audio event capture records
the originating recompiled block and function for CPU port writes. When a
different value is applied before the SPC reads the first, diagnostic captures
print the first and base-16 hit milestones immediately and retain the complete
pair census for the Go report. Same-value rewrites have a separate counter and
do not emit the changed-value warning.

## Threading and callbacks

Unless a contract explicitly says otherwise, runner operations are synchronous
and belong on the emulation thread. `queue_mutation` is the cross-host-thread
entry point. Event callbacks execute on the producing thread; audio callbacks
may execute while the APU lock is held. Do not call back into a service that
could acquire the same lock, advance the runner, or mutate the subscribed list.
`RtlRenderAudio` acquires the APU lock around its bounded production and mix
regions; a host audio callback should not hold an outer APU lock around the
call, output gain/mute processing, or submission to the device.

## Compatibility policy

The SDK is currently consumed by this project and can move as one repository.
There are no legacy short-header or `ar_` symbol aliases. Generated C and game
code must be regenerated/migrated with the runtime. Additive ABI fields still
use version, `struct_size`, and capability checks so tools remain explicit
about what they consume.
