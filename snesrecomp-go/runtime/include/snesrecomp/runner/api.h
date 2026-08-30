/**
 * @file api.h
 * @brief Versioned runner API discovery and function table.
 * @ingroup sr_runner_table
 */
#pragma once

#include "snesrecomp/runner/audio.h"
#include "snesrecomp/runner/determinism.h"
#include "snesrecomp/runner/events.h"
#include "snesrecomp/runner/mutation.h"
#include "snesrecomp/runner/ppu.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup sr_runner_table
 *  @{
 */

/**
 * @brief Process-lifetime dispatch table for the public runner ABI.
 *
 * Validate `abi_version`, `struct_size`, and the matching capability bit
 * before calling a member. Table fields are additive: a function pointer that
 * lies beyond `struct_size` must not be read. Unless its request says
 * otherwise, an operation is synchronous on the emulation thread.
 *
 * @see sr_runner_get_api
 * @see api_reference
 */
typedef struct SnesRunnerApi {
    /** ABI implemented by this table; currently SR_RUNNER_ABI_VERSION. */
    uint32_t abi_version;
    /** Readable byte extent of this table. */
    uint32_t struct_size;
    /** Bitwise OR of the available `SR_RUNNER_CAP_*` services. */
    uint64_t capabilities;
    /** Resolve an opaque identity for a runner-owned hardware component. */
    SrResult (*get_component)(SrRunnerHandle *runner,
                              SrComponentKind component,
                              const SrComponentHandle **out_component);
    /** Copy the runner's current lifetime and component generations. */
    SrResult (*query_generations)(SrRunnerHandle *runner,
                                  SrGenerationSnapshot *out_generations);
    /** Borrow an immutable byte-addressed memory region. */
    SrResult (*borrow_memory)(SrRunnerHandle *runner, SrMemoryRegion region,
                              SrBorrowedSpan *out_span);
    /** Test whether a prior byte borrow still describes live storage. */
    uint32_t (*borrow_is_valid)(SrRunnerHandle *runner,
                                const SrBorrowedSpan *span);
    /** Copy the recompiled 65816 register and execution state. */
    SrResult (*query_cpu_state)(SrRunnerHandle *runner,
                                SrCpuStateSnapshot *out_state);
    /** Copy coherent controls at the instant of the call. This is not the
     * per-scanline state used to composite an already completed frame. */
    SrResult (*query_ppu_state)(SrRunnerHandle *runner,
                                SrPpuStateSnapshot *out_state);
    /** Borrow host-native 16-bit VRAM, CGRAM, or OAM elements. */
    SrResult (*borrow_u16_memory)(SrRunnerHandle *runner,
                                  SrMemoryRegion region,
                                  SrBorrowedU16Span *out_span);
    /** Test whether a prior 16-bit borrow still describes live storage. */
    uint32_t (*borrow_u16_is_valid)(SrRunnerHandle *runner,
                                    const SrBorrowedU16Span *span);
    /** Copy frame-derived capture and presentation state. */
    SrResult (*query_ppu_frame_state)(SrRunnerHandle *runner,
                                      SrPpuFrameSnapshot *out_state);
    /** Rasterize a contiguous OAM object range into caller storage. */
    SrResult (*rasterize_ppu_obj_range)(
        SrRunnerHandle *runner, const SrPpuObjRasterRequest *request,
        SrPpuObjRasterResult *out_result);
    /** Resolve visible object pixels without publishing a surface. */
    SrResult (*resolve_ppu_obj_range)(
        SrRunnerHandle *runner, const SrPpuObjResolveRequest *request,
        SrPpuObjResolveResult *out_result);
    /** Rasterize explicitly selected object parts into caller storage. */
    SrResult (*rasterize_ppu_obj_parts)(
        SrRunnerHandle *runner,
        const SrPpuObjPartsRasterRequest *request,
        SrPpuObjRasterResult *out_result);
    /** Borrow the runner's current PPU output surfaces. */
    SrResult (*query_ppu_surfaces)(SrRunnerHandle *runner,
                                   SrPpuSurfaceSnapshot *out_surfaces);
    /** Test whether a borrowed surface snapshot remains valid. */
    uint32_t (*ppu_surface_snapshot_is_valid)(
        SrRunnerHandle *runner, const SrPpuSurfaceSnapshot *surfaces);
    /** Copy generated-code call-stack and recent-block diagnostics. */
    SrResult (*query_execution_state)(SrRunnerHandle *runner,
                                      SrExecutionSnapshot *out_state);
    /** Install a synchronous filtered runner-event observer. */
    SrResult (*subscribe_events)(SrRunnerHandle *runner,
                                 const SrEventSubscription *subscription,
                                 uint64_t *out_subscription_id);
    /** Remove a runner-event observer while event producers are stopped. */
    SrResult (*unsubscribe_events)(SrRunnerHandle *runner,
                                   uint64_t subscription_id);
    /** Copy and queue a bounded mutation from a host thread. */
    SrResult (*queue_mutation)(SrRunnerHandle *runner,
                               const SrMutationCommand *command,
                               uint64_t *out_command_id);
    /** Copy, and optionally consume, a queued mutation's status. */
    SrResult (*query_mutation)(SrRunnerHandle *runner,
                               uint64_t command_id, uint32_t flags,
                               SrMutationStatus *out_status);
    /** Resolve the source texel sampled by a displayed BG coordinate. */
    SrResult (*resolve_ppu_background_coordinate)(
        SrRunnerHandle *runner,
        const SrPpuBackgroundCoordinateRequest *request,
        SrPpuBackgroundCoordinateResult *out_result);
    /** Bind or unbind host-owned PPU output storage. */
    SrResult (*bind_ppu_output_surface)(
        SrRunnerHandle *runner, const SrPpuOutputBindingRequest *request);
    /** Configure persistent left and right presentation margins. */
    SrResult (*configure_ppu_horizontal_margin)(
        SrRunnerHandle *runner,
        const SrPpuHorizontalMarginRequest *request);
    /** Claim a frame-scoped separated-layer overlay capture. */
    SrResult (*claim_ppu_overlay_capture)(
        SrRunnerHandle *runner,
        const SrPpuOverlayCaptureRequest *request);
    /** Claim a frame-scoped game-provided Mode 7 surface. */
    SrResult (*claim_ppu_mode7_override)(
        SrRunnerHandle *runner,
        const SrPpuMode7OverrideRequest *request);
    /** Copy the 65816 multiply/divide hardware state. */
    SrResult (*query_cpu_math_state)(SrRunnerHandle *runner,
                                     SrCpuMathState *out_state);
    /** Restore multiply/divide state at a synchronous safe point. */
    SrResult (*restore_cpu_math_state)(SrRunnerHandle *runner,
                                       const SrCpuMathState *state);
    /** Install a coherent APU/SPC trace observer. */
    SrResult (*subscribe_audio_trace)(
        SrRunnerHandle *runner,
        const SrAudioTraceSubscription *subscription,
        uint64_t *out_subscription_id);
    /** Remove an audio trace observer while audio producers are stopped. */
    SrResult (*unsubscribe_audio_trace)(SrRunnerHandle *runner,
                                        uint64_t subscription_id);
    /** Atomically compare and update the SPC PC under the APU lock. */
    SrResult (*compare_exchange_spc_pc)(
        SrRunnerHandle *runner, const SrSpcPcControlRequest *request,
        SrSpcPcControlResult *out_result);
    /** Configure the original DSP music and sound-effect bus gains. */
    SrResult (*configure_audio_mix)(SrRunnerHandle *runner,
                                    const SrAudioMixControl *control);
    /** Visit one coherent callback-lifetime PPU frame transaction. */
    SrResult (*visit_ppu_frame_transaction)(
        SrRunnerHandle *runner,
        const SrPpuFrameTransactionRequest *request);
    /** Atomically replace frame overlay captures after validation. */
    SrResult (*compare_exchange_ppu_overlay_captures)(
        SrRunnerHandle *runner,
        const SrPpuOverlayCaptureExchangeRequest *request);
    /** Atomically compare and patch sorted sparse VRAM words. */
    SrResult (*compare_exchange_ppu_vram_words)(
        SrRunnerHandle *runner,
        const SrPpuVramPatchRequest *request);
    /** Publish game-owned unwrapped object positions for the current frame. */
    SrResult (*update_ppu_obj_metadata)(
        SrRunnerHandle *runner,
        const SrPpuObjMetadataRequest *request);
    /** Copy coherent DMA and HDMA channel state. */
    SrResult (*query_dma_state)(SrRunnerHandle *runner,
                                SrDmaStateSnapshot *out_state);
    /** Publish finite background extents and margin fill policy. */
    SrResult (*update_ppu_layer_extents)(
        SrRunnerHandle *runner,
        const SrPpuLayerExtentRequest *request);
    /** Replace the current frame's game-owned virtual tilemap providers. */
    SrResult (*replace_ppu_virtual_tilemaps)(
        SrRunnerHandle *runner,
        const SrPpuVirtualTilemapRequest *request);
    /** Publish authentic camera coordinates used by widescreen mapping. */
    SrResult (*update_ppu_authentic_camera)(
        SrRunnerHandle *runner,
        const SrPpuAuthenticCameraRequest *request);
    /** Run runner-owned scanlines, timing, HDMA, and margin scanout. */
    SrResult (*run_ppu_scanout)(
        SrRunnerHandle *runner,
        const SrPpuScanoutRequest *request,
        SrPpuScanoutResult *out_result);
    /** Apply timing-latch transitions without selecting an adapter schedule. */
    SrResult (*control_game_timing)(
        SrRunnerHandle *runner,
        const SrGameTimingRequest *request,
        SrGameTimingResult *out_result);
    /** Copy the current packed and auto-joypad controller state. */
    SrResult (*query_input_state)(
        SrRunnerHandle *runner, SrInputStateSnapshot *out_state);
    /** Apply an all-or-nothing frame presentation policy transaction. */
    SrResult (*apply_ppu_frame_policy)(
        SrRunnerHandle *runner, const SrPpuFramePolicyRequest *request);
    /** Clear derived frame state while preserving persistent host bindings. */
    SrResult (*reset_ppu_frame_state)(
        SrRunnerHandle *runner, const SrPpuFrameResetRequest *request);
    /** Configure frame-scoped object capture into caller-owned storage. */
    SrResult (*configure_ppu_obj_capture)(
        SrRunnerHandle *runner, const SrPpuObjCaptureRequest *request);
    /** Copy ARAM, visible DSP registers, and scalar APU state under one APU
     * lock acquisition. Returns busy from audio-production and audio-trace
     * callbacks. */
    SrResult (*query_apu_state)(
        SrRunnerHandle *runner, const SrApuStateQuery *query,
        SrApuStateSnapshot *out_state);
    /** Produce the canonical current runner-hardware semantic digest. */
    SrResult (*query_semantic_digest)(
        SrRunnerHandle *runner, const SrSemanticDigestRequest *request,
        SrSemanticDigestResult *out_result);
} SnesRunnerApi;

#define SNES_RUNNER_API_V2_BASE_SIZE                                           \
    ((uint32_t)(offsetof(SnesRunnerApi, borrow_is_valid) +                 \
                sizeof(((SnesRunnerApi *)0)->borrow_is_valid)))

#define SNES_RUNNER_API_CPU_STATE_SIZE                                    \
    ((uint32_t)(offsetof(SnesRunnerApi, query_cpu_state) +                 \
                sizeof(((SnesRunnerApi *)0)->query_cpu_state)))

#define SNES_RUNNER_API_PPU_STATE_SIZE                                    \
    ((uint32_t)(offsetof(SnesRunnerApi, borrow_u16_is_valid) +             \
                sizeof(((SnesRunnerApi *)0)->borrow_u16_is_valid)))

#define SNES_RUNNER_API_PPU_FRAME_STATE_SIZE                              \
    ((uint32_t)(offsetof(SnesRunnerApi, query_ppu_frame_state) +           \
                sizeof(((SnesRunnerApi *)0)->query_ppu_frame_state)))

#define SNES_RUNNER_API_PPU_OBJ_RASTER_SIZE                               \
    ((uint32_t)(offsetof(SnesRunnerApi, rasterize_ppu_obj_range) +         \
                sizeof(((SnesRunnerApi *)0)->rasterize_ppu_obj_range)))

#define SNES_RUNNER_API_PPU_OBJ_RESOLVE_SIZE                              \
    ((uint32_t)(offsetof(SnesRunnerApi, resolve_ppu_obj_range) +           \
                sizeof(((SnesRunnerApi *)0)->resolve_ppu_obj_range)))

#define SNES_RUNNER_API_PPU_OBJ_PARTS_SIZE                                \
    ((uint32_t)(offsetof(SnesRunnerApi, rasterize_ppu_obj_parts) +         \
                sizeof(((SnesRunnerApi *)0)->rasterize_ppu_obj_parts)))

#define SNES_RUNNER_API_PPU_SURFACE_SIZE                                  \
    ((uint32_t)(offsetof(SnesRunnerApi, ppu_surface_snapshot_is_valid) +   \
                sizeof(((SnesRunnerApi *)0)                               \
                           ->ppu_surface_snapshot_is_valid)))

#define SNES_RUNNER_API_EXECUTION_STATE_SIZE                              \
    ((uint32_t)(offsetof(SnesRunnerApi, query_execution_state) +           \
                sizeof(((SnesRunnerApi *)0)->query_execution_state)))

#define SNES_RUNNER_API_EVENT_OBSERVER_SIZE                               \
    ((uint32_t)(offsetof(SnesRunnerApi, unsubscribe_events) +              \
                sizeof(((SnesRunnerApi *)0)->unsubscribe_events)))

#define SNES_RUNNER_API_SAFE_POINT_MUTATION_SIZE                          \
    ((uint32_t)(offsetof(SnesRunnerApi, query_mutation) +                  \
                sizeof(((SnesRunnerApi *)0)->query_mutation)))

#define SNES_RUNNER_API_PPU_BACKGROUND_COORDINATE_SIZE                    \
    ((uint32_t)(offsetof(SnesRunnerApi, resolve_ppu_background_coordinate) + \
                sizeof(((SnesRunnerApi *)0)                               \
                           ->resolve_ppu_background_coordinate)))

#define SNES_RUNNER_API_PPU_OUTPUT_CONTROL_SIZE                           \
    ((uint32_t)(offsetof(SnesRunnerApi, configure_ppu_horizontal_margin) + \
                sizeof(((SnesRunnerApi *)0)                               \
                           ->configure_ppu_horizontal_margin)))

#define SNES_RUNNER_API_PPU_CAPTURE_CONTROL_SIZE                          \
    ((uint32_t)(offsetof(SnesRunnerApi, claim_ppu_mode7_override) +        \
                sizeof(((SnesRunnerApi *)0)->claim_ppu_mode7_override)))

#define SNES_RUNNER_API_CPU_MATH_STATE_SIZE                              \
    ((uint32_t)(offsetof(SnesRunnerApi, restore_cpu_math_state) +         \
                sizeof(((SnesRunnerApi *)0)->restore_cpu_math_state)))

#define SNES_RUNNER_API_AUDIO_TRACE_OBSERVER_SIZE                        \
    ((uint32_t)(offsetof(SnesRunnerApi, unsubscribe_audio_trace) +        \
                sizeof(((SnesRunnerApi *)0)->unsubscribe_audio_trace)))

#define SNES_RUNNER_API_SPC_CONTROL_SIZE                                 \
    ((uint32_t)(offsetof(SnesRunnerApi, compare_exchange_spc_pc) +        \
                sizeof(((SnesRunnerApi *)0)->compare_exchange_spc_pc)))

#define SNES_RUNNER_API_AUDIO_MIX_CONTROL_SIZE                           \
    ((uint32_t)(offsetof(SnesRunnerApi, configure_audio_mix) +            \
                sizeof(((SnesRunnerApi *)0)->configure_audio_mix)))

#define SNES_RUNNER_API_PPU_FRAME_TRANSACTION_SIZE                       \
    ((uint32_t)(offsetof(SnesRunnerApi,                                  \
                         compare_exchange_ppu_overlay_captures) +         \
                sizeof(((SnesRunnerApi *)0)                              \
                           ->compare_exchange_ppu_overlay_captures)))

#define SNES_RUNNER_API_PPU_VRAM_PATCH_SIZE                              \
    ((uint32_t)(offsetof(SnesRunnerApi, compare_exchange_ppu_vram_words) + \
                sizeof(((SnesRunnerApi *)0)                              \
                           ->compare_exchange_ppu_vram_words)))

#define SNES_RUNNER_API_PPU_OBJ_METADATA_SIZE                            \
    ((uint32_t)(offsetof(SnesRunnerApi, update_ppu_obj_metadata) +         \
                sizeof(((SnesRunnerApi *)0)->update_ppu_obj_metadata)))

#define SNES_RUNNER_API_DMA_STATE_SIZE                                   \
    ((uint32_t)(offsetof(SnesRunnerApi, query_dma_state) +                \
                sizeof(((SnesRunnerApi *)0)->query_dma_state)))

#define SNES_RUNNER_API_PPU_BACKGROUND_POLICY_SIZE                       \
    ((uint32_t)(offsetof(SnesRunnerApi, update_ppu_authentic_camera) +    \
                sizeof(((SnesRunnerApi *)0)->update_ppu_authentic_camera)))

#define SNES_RUNNER_API_PPU_SCANOUT_SIZE                                 \
    ((uint32_t)(offsetof(SnesRunnerApi, run_ppu_scanout) +                \
                sizeof(((SnesRunnerApi *)0)->run_ppu_scanout)))

#define SNES_RUNNER_API_GAME_TIMING_CONTROL_SIZE                         \
    ((uint32_t)(offsetof(SnesRunnerApi, control_game_timing) +            \
                sizeof(((SnesRunnerApi *)0)->control_game_timing)))

#define SNES_RUNNER_API_INPUT_STATE_SIZE                                \
    ((uint32_t)(offsetof(SnesRunnerApi, query_input_state) +             \
                sizeof(((SnesRunnerApi *)0)->query_input_state)))

#define SNES_RUNNER_API_PPU_FRAME_POLICY_SIZE                           \
    ((uint32_t)(offsetof(SnesRunnerApi, apply_ppu_frame_policy) +        \
                sizeof(((SnesRunnerApi *)0)->apply_ppu_frame_policy)))

#define SNES_RUNNER_API_PPU_FRAME_RESET_SIZE                            \
    ((uint32_t)(offsetof(SnesRunnerApi, reset_ppu_frame_state) +          \
                sizeof(((SnesRunnerApi *)0)->reset_ppu_frame_state)))

#define SNES_RUNNER_API_PPU_OBJ_CAPTURE_SIZE                            \
    ((uint32_t)(offsetof(SnesRunnerApi, configure_ppu_obj_capture) +      \
                sizeof(((SnesRunnerApi *)0)->configure_ppu_obj_capture)))

#define SNES_RUNNER_API_APU_STATE_SNAPSHOT_SIZE                         \
    ((uint32_t)(offsetof(SnesRunnerApi, query_apu_state) +               \
                sizeof(((SnesRunnerApi *)0)->query_apu_state)))

#define SNES_RUNNER_API_SEMANTIC_DIGEST_SIZE                            \
    ((uint32_t)(offsetof(SnesRunnerApi, query_semantic_digest) +         \
                sizeof(((SnesRunnerApi *)0)->query_semantic_digest)))

/** @brief Stable description of the linked runner implementation. */
typedef struct SrRunnerDescriptor {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t capabilities;
    const char *implementation;
} SrRunnerDescriptor;

/**
 * @brief Describes the linked runner implementation.
 * @return Runner-owned process-lifetime descriptor.
 */
const SrRunnerDescriptor *sr_runner_descriptor(void);

/**
 * @brief Acquires the versioned public runner dispatch table.
 * @param[in] requested_abi_version ABI the caller was compiled against.
 * @return Runner-owned process-lifetime table, or `NULL` when unsupported.
 * @note Check the table extent and matching capability before reading or
 * calling an additive member.
 */
const SnesRunnerApi *sr_runner_get_api(uint32_t requested_abi_version);

/** @} */

#ifdef __cplusplus
}
#endif
