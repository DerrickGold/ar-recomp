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
| Host conveniences | `snesrecomp/host/*.h` | ROM/path launch helpers, frame/audio tracing, and presentation support |
| Stateless utilities | `snesrecomp/support/*.h` | File loading and CRC32 |

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
`mutation.h`, or `api.h`; each domain header is independently compile-tested
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
| `SR_RESULT_UNAVAILABLE` | Valid service, but no active runner/resource/provider exists | Retry only after the required lifecycle step |
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
| `SR_RUNNER_CAP_PPU_STATE` | `SNES_RUNNER_API_PPU_STATE_SIZE` | `query_ppu_state` | Copied coherent PPU controls |
| `SR_RUNNER_CAP_BORROWED_U16_SPANS` | `SNES_RUNNER_API_PPU_STATE_SIZE` | `borrow_u16_memory`, `borrow_u16_is_valid` | Host-native VRAM/CGRAM/OAM words |
| `SR_RUNNER_CAP_PPU_FRAME_STATE` | `SNES_RUNNER_API_PPU_FRAME_STATE_SIZE` | `query_ppu_frame_state` | Copied frame-derived policy/capture state |
| `SR_RUNNER_CAP_PPU_OBJ_RASTER` | `SNES_RUNNER_API_PPU_OBJ_RASTER_SIZE`, `...OBJ_RESOLVE_SIZE`, or `...OBJ_PARTS_SIZE` | `rasterize_ppu_obj_range`, `resolve_ppu_obj_range`, `rasterize_ppu_obj_parts` | Caller-owned output buffers; emulation-thread |
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
| `SR_RUNNER_CAP_AUDIO_MIX_CONTROL` | `SNES_RUNNER_API_AUDIO_MIX_CONTROL_SIZE` | `configure_audio_mix` | Synchronous native music/SFX gain policy |
| `SR_RUNNER_CAP_PPU_FRAME_TRANSACTIONS` | `SNES_RUNNER_API_PPU_FRAME_TRANSACTION_SIZE` | `visit_ppu_frame_transaction`, `compare_exchange_ppu_overlay_captures` | Coherent callback-lifetime frame state and atomic capture replacement |
| `SR_RUNNER_CAP_PPU_VRAM_PATCH` | `SNES_RUNNER_API_PPU_VRAM_PATCH_SIZE` | `compare_exchange_ppu_vram_words` | Atomic sorted sparse patch; no partial write |
| `SR_RUNNER_CAP_PPU_OBJ_METADATA` | `SNES_RUNNER_API_PPU_OBJ_METADATA_SIZE` | `update_ppu_obj_metadata` | Synchronous game-owned unwrapped positions |
| `SR_RUNNER_CAP_DMA_STATE` | `SNES_RUNNER_API_DMA_STATE_SIZE` | `query_dma_state` | Copied coherent DMA/HDMA snapshot |
| `SR_RUNNER_CAP_PPU_BACKGROUND_POLICY` | `SNES_RUNNER_API_PPU_BACKGROUND_POLICY_SIZE` | `update_ppu_layer_extents`, `replace_ppu_virtual_tilemaps`, `update_ppu_authentic_camera` | Frame/game-owned providers; observe each request's copy/retain rules |
| `SR_RUNNER_CAP_PPU_SCANOUT` | `SNES_RUNNER_API_PPU_SCANOUT_SIZE` | `run_ppu_scanout` | Runner owns scanlines, `$420C`-armed HDMA, IRQ timing, and margin hold; request suppression can only narrow HDMA |
| `SR_RUNNER_CAP_GAME_TIMING_CONTROL` | `SNES_RUNNER_API_GAME_TIMING_CONTROL_SIZE` | `control_game_timing` | Synchronous recompiled frame-slice pacing |
| `SR_RUNNER_CAP_INPUT_STATE` | `SNES_RUNNER_API_INPUT_STATE_SIZE` | `query_input_state` | Copied controller state |
| `SR_RUNNER_CAP_PPU_FRAME_POLICY` | `SNES_RUNNER_API_PPU_FRAME_POLICY_SIZE` | `apply_ppu_frame_policy` | BEGIN clears prior frame providers; FINALIZE preserves newly published ones |
| `SR_RUNNER_CAP_PPU_FRAME_RESET` | `SNES_RUNNER_API_PPU_FRAME_RESET_SIZE` | `reset_ppu_frame_state` | Begin-frame clear of derived capture/override state; persistent surfaces remain bound |
| `SR_RUNNER_CAP_PPU_OBJ_CAPTURE` | `SNES_RUNNER_API_PPU_OBJ_CAPTURE_SIZE` | `configure_ppu_obj_capture` | Frame-scoped; caller surface must survive scanout |

## Common call sequences

### Read CPU or memory state

1. Query generations if several reads must describe the same lifetime.
2. Query the copied CPU snapshot.
3. Borrow only the memory region needed.
4. Finish synchronously or copy the small subset retained later.
5. Call `borrow_is_valid` before reusing a previous borrow.

### Publish widescreen state

1. `reset_ppu_frame_state`.
2. Apply frame policy BEGIN.
3. Publish virtual tilemaps, finite extents, authentic cameras, and OBJ metadata.
4. Apply FINALIZE only when fallback policy depends on accepted providers.
5. Claim captures/configure OBJ capture.
6. Compose through `visit_ppu_frame_transaction`.
7. `run_ppu_scanout`. Leave `hdma_suppress_mask` zero for normal hardware
   behavior. Set bits only when an enhancement intentionally suppresses
   channels already armed through `$420C`; the request cannot arm channels.

### Observe and replace audio

1. Recover semantic track/SFX events in the game adapter.
2. Use audio-trace observers only for coherent hardware observation.
3. Route linked-game voices through `RtlGameAudioApi` safe points.
4. Apply original music/SFX bus gains through `configure_audio_mix`.
5. Keep file decoding, replacement streams, track names, and manifests in the
   game/frontend layer.

## Threading and callbacks

Unless a contract explicitly says otherwise, runner operations are synchronous
and belong on the emulation thread. `queue_mutation` is the cross-host-thread
entry point. Event callbacks execute on the producing thread; audio callbacks
may execute while the APU lock is held. Do not call back into a service that
could acquire the same lock, advance the runner, or mutate the subscribed list.

## Compatibility policy

The SDK is currently consumed by this project and can move as one repository.
There are no legacy short-header or `ar_` symbol aliases. Generated C and game
code must be regenerated/migrated with the runtime. Additive ABI fields still
use version, `struct_size`, and capability checks so tools remain explicit
about what they consume.
