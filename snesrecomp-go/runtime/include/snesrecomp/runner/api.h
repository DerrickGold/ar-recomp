#pragma once

#include "snesrecomp/runner/audio.h"
#include "snesrecomp/runner/events.h"
#include "snesrecomp/runner/mutation.h"
#include "snesrecomp/runner/ppu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnesRunnerApi {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t capabilities;
    SrResult (*get_component)(SrRunnerHandle *runner,
                              SrComponentKind component,
                              const SrComponentHandle **out_component);
    SrResult (*query_generations)(SrRunnerHandle *runner,
                                  SrGenerationSnapshot *out_generations);
    SrResult (*borrow_memory)(SrRunnerHandle *runner, SrMemoryRegion region,
                              SrBorrowedSpan *out_span);
    uint32_t (*borrow_is_valid)(SrRunnerHandle *runner,
                                const SrBorrowedSpan *span);
    SrResult (*query_cpu_state)(SrRunnerHandle *runner,
                                SrCpuStateSnapshot *out_state);
    SrResult (*query_ppu_state)(SrRunnerHandle *runner,
                                SrPpuStateSnapshot *out_state);
    SrResult (*borrow_u16_memory)(SrRunnerHandle *runner,
                                  SrMemoryRegion region,
                                  SrBorrowedU16Span *out_span);
    uint32_t (*borrow_u16_is_valid)(SrRunnerHandle *runner,
                                    const SrBorrowedU16Span *span);
    SrResult (*query_ppu_frame_state)(SrRunnerHandle *runner,
                                      SrPpuFrameSnapshot *out_state);
    SrResult (*rasterize_ppu_obj_range)(
        SrRunnerHandle *runner, const SrPpuObjRasterRequest *request,
        SrPpuObjRasterResult *out_result);
    SrResult (*resolve_ppu_obj_range)(
        SrRunnerHandle *runner, const SrPpuObjResolveRequest *request,
        SrPpuObjResolveResult *out_result);
    SrResult (*rasterize_ppu_obj_parts)(
        SrRunnerHandle *runner,
        const SrPpuObjPartsRasterRequest *request,
        SrPpuObjRasterResult *out_result);
    SrResult (*query_ppu_surfaces)(SrRunnerHandle *runner,
                                   SrPpuSurfaceSnapshot *out_surfaces);
    uint32_t (*ppu_surface_snapshot_is_valid)(
        SrRunnerHandle *runner, const SrPpuSurfaceSnapshot *surfaces);
    SrResult (*query_execution_state)(SrRunnerHandle *runner,
                                      SrExecutionSnapshot *out_state);
    SrResult (*subscribe_events)(SrRunnerHandle *runner,
                                 const SrEventSubscription *subscription,
                                 uint64_t *out_subscription_id);
    SrResult (*unsubscribe_events)(SrRunnerHandle *runner,
                                   uint64_t subscription_id);
    SrResult (*queue_mutation)(SrRunnerHandle *runner,
                               const SrMutationCommand *command,
                               uint64_t *out_command_id);
    SrResult (*query_mutation)(SrRunnerHandle *runner,
                               uint64_t command_id, uint32_t flags,
                               SrMutationStatus *out_status);
    SrResult (*resolve_ppu_background_coordinate)(
        SrRunnerHandle *runner,
        const SrPpuBackgroundCoordinateRequest *request,
        SrPpuBackgroundCoordinateResult *out_result);
    SrResult (*bind_ppu_output_surface)(
        SrRunnerHandle *runner, const SrPpuOutputBindingRequest *request);
    SrResult (*configure_ppu_horizontal_margin)(
        SrRunnerHandle *runner,
        const SrPpuHorizontalMarginRequest *request);
    SrResult (*claim_ppu_overlay_capture)(
        SrRunnerHandle *runner,
        const SrPpuOverlayCaptureRequest *request);
    SrResult (*claim_ppu_mode7_override)(
        SrRunnerHandle *runner,
        const SrPpuMode7OverrideRequest *request);
    SrResult (*query_cpu_math_state)(SrRunnerHandle *runner,
                                     SrCpuMathState *out_state);
    SrResult (*restore_cpu_math_state)(SrRunnerHandle *runner,
                                       const SrCpuMathState *state);
    SrResult (*subscribe_audio_trace)(
        SrRunnerHandle *runner,
        const SrAudioTraceSubscription *subscription,
        uint64_t *out_subscription_id);
    SrResult (*unsubscribe_audio_trace)(SrRunnerHandle *runner,
                                        uint64_t subscription_id);
    SrResult (*compare_exchange_spc_pc)(
        SrRunnerHandle *runner, const SrSpcPcControlRequest *request,
        SrSpcPcControlResult *out_result);
    SrResult (*configure_audio_mix)(SrRunnerHandle *runner,
                                    const SrAudioMixControl *control);
    SrResult (*visit_ppu_frame_transaction)(
        SrRunnerHandle *runner,
        const SrPpuFrameTransactionRequest *request);
    SrResult (*compare_exchange_ppu_overlay_captures)(
        SrRunnerHandle *runner,
        const SrPpuOverlayCaptureExchangeRequest *request);
    SrResult (*compare_exchange_ppu_vram_words)(
        SrRunnerHandle *runner,
        const SrPpuVramPatchRequest *request);
    SrResult (*update_ppu_obj_metadata)(
        SrRunnerHandle *runner,
        const SrPpuObjMetadataRequest *request);
    SrResult (*query_dma_state)(SrRunnerHandle *runner,
                                SrDmaStateSnapshot *out_state);
    SrResult (*update_ppu_layer_extents)(
        SrRunnerHandle *runner,
        const SrPpuLayerExtentRequest *request);
    SrResult (*replace_ppu_virtual_tilemaps)(
        SrRunnerHandle *runner,
        const SrPpuVirtualTilemapRequest *request);
    SrResult (*update_ppu_authentic_camera)(
        SrRunnerHandle *runner,
        const SrPpuAuthenticCameraRequest *request);
    SrResult (*run_ppu_scanout)(
        SrRunnerHandle *runner,
        const SrPpuScanoutRequest *request,
        SrPpuScanoutResult *out_result);
    SrResult (*control_game_timing)(
        SrRunnerHandle *runner,
        const SrGameTimingRequest *request,
        SrGameTimingResult *out_result);
    SrResult (*query_input_state)(
        SrRunnerHandle *runner, SrInputStateSnapshot *out_state);
    SrResult (*apply_ppu_frame_policy)(
        SrRunnerHandle *runner, const SrPpuFramePolicyRequest *request);
    SrResult (*reset_ppu_frame_state)(
        SrRunnerHandle *runner, const SrPpuFrameResetRequest *request);
    SrResult (*configure_ppu_obj_capture)(
        SrRunnerHandle *runner, const SrPpuObjCaptureRequest *request);
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

typedef struct SrRunnerDescriptor {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t capabilities;
    const char *implementation;
} SrRunnerDescriptor;

/* Describes the linked runner implementation. The returned descriptor and its
 * string remain runner-owned for the process lifetime. */
const SrRunnerDescriptor *sr_runner_descriptor(void);

/* Returns NULL for an unsupported version. The table and all function
 * pointers remain runner-owned for the process lifetime. */
const SnesRunnerApi *sr_runner_get_api(uint32_t requested_abi_version);

#ifdef __cplusplus
}
#endif

