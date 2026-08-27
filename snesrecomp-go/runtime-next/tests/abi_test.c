#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "runner_next.h"
#include "runner_next_internal.h"
#include "snes/dma.h"
#include "snes/ppu.h"
#include "snes/saveload.h"
#include "snes/snes.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { TEST_WRAM_SIZE = 128 * 1024 };

typedef struct SpcPlayer SpcPlayer;
SpcPlayer *g_spc_player;

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static void leave_loaded_state_unchanged(SaveLoadInfo *info, void *data,
                                         size_t size) {
    (void)info;
    (void)data;
    (void)size;
}

static uint8_t s_cpu_component;
static uint8_t s_overlay_pixel;
static uint32_t s_mode7_pixel;
static uint32_t s_main_surface[384u * 253u];
static uint32_t s_authentic_surface[384u * 253u];
static uint32_t s_overlay_surface[384u * 232u];
static uint32_t s_overlay_band_surface[384u * 232u];
static uint32_t s_mode7_surface[768u * 506u];

typedef struct TestObserver {
    unsigned count;
    SrRunnerHandle *runner;
    SrRunnerEvent event;
    const int16_t *expected_audio_samples;
    uint32_t expected_audio_frame_count;
    int16_t expected_audio_first;
    int16_t expected_audio_last;
    int audio_payload_valid;
} TestObserver;

static SrResult query_test_cpu_state(
        Snes *snes, SrCpuStateSnapshot *out_state) {
    (void)snes;
    out_state->flags = SR_CPU_STATE_M_FLAG | SR_CPU_STATE_EMULATION |
                       SR_CPU_STATE_HOST_RETURN_VALID |
                       SR_CPU_STATE_EXECUTION_PC_VALID;
    out_state->frame_counter = 42u;
    out_state->execution_pc24 = 0x123456u;
    out_state->a = 0x1234u;
    out_state->x = 0x5678u;
    out_state->y = 0x9abcu;
    out_state->s = 0x01efu;
    out_state->d = 0x0020u;
    out_state->db = 0x7eu;
    out_state->pb = 0x03u;
    out_state->p = 0xa5u;
    return SR_RESULT_OK;
}

static SrResult query_test_execution_state(
        Snes *snes, SrExecutionSnapshot *out_state) {
    (void)snes;
    out_state->flags = SR_EXECUTION_CURRENT_BLOCK_VALID |
                       SR_EXECUTION_CURRENT_FUNCTION_VALID;
    out_state->block_serial = 91u;
    out_state->current_block_pc24 = 0x345678u;
    out_state->current_function = "test_current";
    out_state->stack_depth = 2u;
    out_state->stack[0].function_name = "outer";
    out_state->stack[0].entry_stack = 0x01ffu;
    out_state->stack[0].host_return_valid = 1u;
    out_state->stack[1].function_name = "inner";
    out_state->stack[1].entry_stack = 0x01fbu;
    out_state->history_count = 2u;
    out_state->history[0].pc24 = 0x123400u;
    out_state->history[0].cpu_flags = SR_CPU_STATE_M_FLAG;
    out_state->history[0].register_x = 0x4567u;
    out_state->history[0].stack_pointer = 0x01f0u;
    out_state->history[1].pc24 = 0x123456u;
    out_state->history[1].cpu_flags = SR_CPU_STATE_X_FLAG;
    out_state->history[1].register_x = 0x89abu;
    out_state->history[1].stack_pointer = 0x01eau;
    return SR_RESULT_OK;
}

static void observe_test_event(void *user_data, SrRunnerHandle *runner,
                               const SrRunnerEvent *event) {
    TestObserver *observer = (TestObserver *)user_data;
    ++observer->count;
    observer->runner = runner;
    observer->event = *event;
    if (event->type == SR_EVENT_AUDIO_PRODUCED) {
        observer->audio_payload_valid =
            event->audio_samples == observer->expected_audio_samples &&
            event->audio_frame_count == observer->expected_audio_frame_count &&
            event->audio_samples != NULL &&
            event->audio_samples[0] == observer->expected_audio_first &&
            event->audio_samples[event->audio_frame_count *
                                 event->audio_channel_count - 1u] ==
                observer->expected_audio_last;
        observer->event.audio_samples = NULL;
    }
}

static void mix_test_audio(int16 *buffer, int frames) {
    int sample;
    for (sample = 0; sample < frames * 2; ++sample)
        buffer[sample] = (int16)(0x1200 + sample);
}

static int check(int condition, const char *message) {
    if (condition) return 0;
    fprintf(stderr, "runtime-next ABI failed: %s\n", message);
    return 1;
}

static void set_solid_4bpp_tile(Ppu *ppu, unsigned tile, unsigned color) {
    unsigned row;
    for (row = 0u; row < 8u; ++row) {
        uint16_t low = 0u;
        uint16_t high = 0u;
        if ((color & 1u) != 0u) low |= UINT16_C(0x00ff);
        if ((color & 2u) != 0u) low |= UINT16_C(0xff00);
        if ((color & 4u) != 0u) high |= UINT16_C(0x00ff);
        if ((color & 8u) != 0u) high |= UINT16_C(0xff00);
        ppu->vram[tile * 16u + row] = low;
        ppu->vram[tile * 16u + row + 8u] = high;
    }
}

static int check_generation(const SnesRunnerApi *api, SrRunnerHandle *runner,
                            uint64_t lifetime, uint64_t tick, uint64_t reset,
                            uint64_t load, uint64_t mutation) {
    SrGenerationSnapshot generation = {sizeof(generation), 0u, 0u, 0u,
                                       0u, 0u, 0u};
    int failed = 0;
    failed |= check(api->query_generations(runner, &generation) == SR_RESULT_OK,
                    "generation query failed");
    failed |= check(generation.struct_size == sizeof(generation),
                    "generation size changed");
    failed |= check(generation.reserved == 0u, "generation reserved is nonzero");
    failed |= check(generation.lifetime_generation == lifetime,
                    "lifetime generation mismatch");
    failed |= check(generation.tick_generation == tick,
                    "tick generation mismatch");
    failed |= check(generation.reset_generation == reset,
                    "reset generation mismatch");
    failed |= check(generation.load_generation == load,
                    "load generation mismatch");
    failed |= check(generation.mutation_generation == mutation,
                    "mutation generation mismatch");
    return failed;
}

int main(void) {
    static uint8_t wram[TEST_WRAM_SIZE];
    const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
    const SrComponentHandle *component = NULL;
    SrGenerationSnapshot too_small = {sizeof(uint32_t), 0u, 0u, 0u,
                                      0u, 0u, 0u};
    SrBorrowedSpan wram_span = {sizeof(wram_span), 0u, NULL, 0u, 0u};
    SrBorrowedSpan apu_span = {sizeof(apu_span), 0u, NULL, 0u, 0u};
    SrBorrowedSpan high_oam_span = {
        sizeof(high_oam_span), 0u, NULL, 0u, 0u};
    SrBorrowedSpan unsupported = {sizeof(unsupported), 0u, NULL, 0u, 0u};
    SrBorrowedSpan too_small_span = {sizeof(uint32_t), 0u, NULL, 0u, 0u};
    SrCpuStateSnapshot cpu_state = {sizeof(cpu_state), 0u, 0u, 0u, 0u,
                                    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    SrCpuStateSnapshot small_cpu_state = {sizeof(uint32_t), 0u, 0u, 0u, 0u,
                                          0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
                                          0u};
    SrExecutionSnapshot execution_state = {sizeof(execution_state), 0u};
    SrExecutionSnapshot small_execution_state = {sizeof(uint32_t), 0u};
    TestObserver block_observer = {0};
    TestObserver dispatch_observer = {0};
    TestObserver memory_observer = {0};
    TestObserver ppu_memory_observer = {0};
    TestObserver register_observer = {0};
    TestObserver dma_observer = {0};
    TestObserver frame_observer = {0};
    TestObserver interrupt_error_observer = {0};
    SrEventSubscription subscription = {
        .struct_size = sizeof(subscription),
        .flags = SR_EVENT_FILTER_PC_RANGE,
        .event_mask = SR_EVENT_MASK_EXECUTION_BLOCK |
                      SR_EVENT_MASK_DYNAMIC_DISPATCH,
        .pc_first = 0x100000u,
        .pc_last = 0x10ffffu,
        .callback = observe_test_event,
        .user_data = &block_observer,
    };
    SrEventSubscription dispatch_subscription = {
        .struct_size = sizeof(dispatch_subscription),
        .event_mask = SR_EVENT_MASK_DYNAMIC_DISPATCH,
        .callback = observe_test_event,
        .user_data = &dispatch_observer,
    };
    SrEventSubscription memory_subscription = {
        .struct_size = sizeof(memory_subscription),
        .flags = SR_EVENT_FILTER_ADDRESS_RANGE |
                 SR_EVENT_FILTER_MEMORY_REGION,
        .event_mask = SR_EVENT_MASK_MEMORY_WRITE,
        .memory_region = SR_MEMORY_WRAM,
        .address_first = 0x0100u,
        .address_last = 0x010fu,
        .callback = observe_test_event,
        .user_data = &memory_observer,
    };
    SrEventSubscription register_subscription = {
        .struct_size = sizeof(register_subscription),
        .flags = SR_EVENT_FILTER_ADDRESS_RANGE,
        .event_mask = SR_EVENT_MASK_REGISTER_ACCESS,
        .address_first = 0x2100u,
        .address_last = 0x213fu,
        .callback = observe_test_event,
        .user_data = &register_observer,
    };
    SrEventSubscription ppu_memory_subscription = {
        .struct_size = sizeof(ppu_memory_subscription),
        .flags = SR_EVENT_FILTER_ADDRESS_RANGE |
                 SR_EVENT_FILTER_MEMORY_REGION,
        .event_mask = SR_EVENT_MASK_MEMORY_WRITE,
        .memory_region = SR_MEMORY_VRAM,
        .address_first = 6u,
        .address_last = 6u,
        .callback = observe_test_event,
        .user_data = &ppu_memory_observer,
    };
    SrEventSubscription dma_subscription = {
        .struct_size = sizeof(dma_subscription),
        .flags = SR_EVENT_FILTER_ADDRESS_RANGE,
        .event_mask = SR_EVENT_MASK_DMA,
        .address_first = 0x7e1234u,
        .address_last = 0x7e1234u,
        .callback = observe_test_event,
        .user_data = &dma_observer,
    };
    SrEventSubscription frame_subscription = {
        .struct_size = sizeof(frame_subscription),
        .event_mask = SR_EVENT_MASK_FRAME | SR_EVENT_MASK_AUDIO,
        .callback = observe_test_event,
        .user_data = &frame_observer,
    };
    SrEventSubscription interrupt_error_subscription = {
        .struct_size = sizeof(interrupt_error_subscription),
        .flags = SR_EVENT_FILTER_PC_RANGE,
        .event_mask = SR_EVENT_MASK_INTERRUPT | SR_EVENT_MASK_ERROR,
        .pc_first = 0x123400u,
        .pc_last = 0x1234ffu,
        .callback = observe_test_event,
        .user_data = &interrupt_error_observer,
    };
    SrRunnerEvent runner_event = {0u};
    int16_t audio_samples[8] = {0};
    SrMutationCommand memory_mutation = {
        .struct_size = sizeof(memory_mutation),
        .type = SR_MUTATION_WRITE_MEMORY,
        .memory_region = SR_MEMORY_WRAM,
        .address = 0x0106u,
        .byte_count = 3u,
        .bytes = {0xa1u, 0xb2u, 0xc3u},
    };
    SrMutationCommand input_mutation = {
        .struct_size = sizeof(input_mutation),
        .type = SR_MUTATION_SET_INPUT,
        .input_value = 0x0001u,
        .input_mask = 0x0fffu,
    };
    SrMutationCommand vram_mutation = {
        .struct_size = sizeof(vram_mutation),
        .type = SR_MUTATION_WRITE_MEMORY,
        .memory_region = SR_MEMORY_VRAM,
        .address = 0x4001u,
        .byte_count = 3u,
        .bytes = {0x11u, 0x22u, 0x33u},
    };
    SrMutationStatus mutation_status = {sizeof(mutation_status), 0u};
    uint64_t subscription_id = 0u;
    uint64_t dispatch_subscription_id = 0u;
    uint64_t memory_subscription_id = 0u;
    uint64_t ppu_memory_subscription_id = 0u;
    uint64_t register_subscription_id = 0u;
    uint64_t dma_subscription_id = 0u;
    uint64_t frame_subscription_id = 0u;
    uint64_t interrupt_error_subscription_id = 0u;
    uint64_t memory_mutation_id = 0u;
    uint64_t input_mutation_id = 0u;
    uint64_t vram_mutation_id = 0u;
    SrBorrowedU16Span vram_span = {
        sizeof(vram_span), 0u, NULL, 0u, 0u};
    SrBorrowedU16Span cgram_span = {
        sizeof(cgram_span), 0u, NULL, 0u, 0u};
    SrBorrowedU16Span oam_span = {
        sizeof(oam_span), 0u, NULL, 0u, 0u};
    SrBorrowedU16Span small_u16_span = {
        sizeof(uint32_t), 0u, NULL, 0u, 0u};
    SrPpuStateSnapshot ppu_state = {sizeof(ppu_state), 0u};
    SrPpuStateSnapshot small_ppu_state = {sizeof(uint32_t), 0u};
    SrPpuFrameSnapshot ppu_frame = {sizeof(ppu_frame), 0u};
    SrPpuFrameSnapshot small_ppu_frame = {sizeof(uint32_t), 0u};
    SrPpuBackgroundCoordinateRequest coordinate_request = {
        .struct_size = sizeof(coordinate_request),
        .layer = 0u,
        .screen_x = -1,
        .screen_y = 9,
    };
    SrPpuBackgroundCoordinateResult coordinate_result = {
        sizeof(coordinate_result), 0u};
    SrPpuBackgroundCoordinateResult small_coordinate_result = {
        sizeof(uint32_t), 0u};
    SrPpuSurfaceSnapshot ppu_surfaces = {sizeof(ppu_surfaces), 0u};
    SrPpuSurfaceSnapshot rebound_surfaces = {sizeof(rebound_surfaces), 0u};
    SrPpuSurfaceSnapshot small_ppu_surfaces = {sizeof(uint32_t), 0u};
    SrPpuOutputBindingRequest output_binding = {
        .struct_size = sizeof(output_binding),
    };
    SrPpuHorizontalMarginRequest margin_request = {
        .struct_size = sizeof(margin_request),
    };
    SrPpuOverlayCaptureRequest capture_request = {
        .struct_size = sizeof(capture_request),
    };
    SrPpuMode7OverrideRequest mode7_override_request = {
        .struct_size = sizeof(mode7_override_request),
    };
    SrCpuMathState math_state = {
        .struct_size = sizeof(math_state),
    };
    SrCpuMathState small_math_state = {
        .struct_size = sizeof(uint32_t),
    };
    uint32_t obj_pixels[8u * 8u];
    SrPpuObjRasterRequest obj_request = {
        .struct_size = sizeof(obj_request),
        .lifetime_generation = 0u,
        .first_sprite = 0u,
        .sprite_count = 1u,
        .priority = 2u,
        .pixel_format = SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32,
        .pixels = obj_pixels,
        .pixel_byte_size = sizeof(obj_pixels),
        .pitch_bytes = 8u * sizeof(uint32_t),
    };
    SrPpuObjRasterResult obj_result = {sizeof(obj_result), 0u};
    SrPpuObjRasterResult small_obj_result = {sizeof(uint32_t), 0u};
    SaveLoadInfo load = {leave_loaded_state_unchanged, false, false, false};
    Snes *snes;
    SrRunnerHandle *runner;
    int failed = 0;

    failed |= check(offsetof(SnesRunnerApi, abi_version) == 0u,
                    "API version is not the first field");
    failed |= check(offsetof(SnesRunnerApi, struct_size) == sizeof(uint32_t),
                    "API size is not the second field");
    failed |= check(offsetof(SnesRunnerApi, capabilities) == 2u * sizeof(uint32_t),
                    "API capability layout mismatch");
    failed |= check(offsetof(SrBorrowedSpan, struct_size) == 0u,
                    "span size is not the first field");
    failed |= check(offsetof(SrGenerationSnapshot, struct_size) == 0u,
                    "generation size is not the first field");
    failed |= check(sizeof(SrPpuObjPart) == 8u &&
                        offsetof(SrPpuObjPart, tile_attr) == 4u &&
                        offsetof(SrPpuObjPart, reserved) == 7u,
                    "PPU OBJ value descriptor layout mismatch");
    failed |= check(SR_BORROWED_SPAN_V2_SIZE <= sizeof(SrBorrowedSpan),
                    "span v2 size exceeds structure");
    failed |= check(SR_GENERATION_SNAPSHOT_V2_SIZE <=
                        sizeof(SrGenerationSnapshot),
                    "generation v2 size exceeds structure");
    failed |= check(SNES_RUNNER_API_V2_BASE_SIZE <= sizeof(SnesRunnerApi),
                    "API v2 base size exceeds structure");
    failed |= check(SR_CPU_STATE_SNAPSHOT_V2_SIZE <=
                        sizeof(SrCpuStateSnapshot),
                    "CPU snapshot v2 size exceeds structure");
    failed |= check(SNES_RUNNER_API_CPU_STATE_SIZE <= sizeof(SnesRunnerApi),
                    "CPU API extent exceeds structure");
    failed |= check(SR_BORROWED_U16_SPAN_V2_SIZE <=
                        sizeof(SrBorrowedU16Span),
                    "u16 span v2 size exceeds structure");
    failed |= check(SR_PPU_STATE_SNAPSHOT_V2_SIZE <=
                            sizeof(SrPpuStateSnapshot),
                    "PPU snapshot v2 extent mismatch");
    failed |= check(SNES_RUNNER_API_PPU_STATE_SIZE <= sizeof(SnesRunnerApi),
                    "PPU API extent exceeds structure");
    failed |= check(SR_PPU_FRAME_SNAPSHOT_V2_SIZE <=
                        sizeof(SrPpuFrameSnapshot),
                    "PPU frame snapshot v2 size exceeds structure");
    failed |= check(SNES_RUNNER_API_PPU_FRAME_STATE_SIZE <=
                        sizeof(SnesRunnerApi),
                    "PPU frame API extent exceeds structure");
    failed |= check(SR_PPU_BACKGROUND_COORDINATE_REQUEST_V2_SIZE <=
                            sizeof(SrPpuBackgroundCoordinateRequest) &&
                        SR_PPU_BACKGROUND_COORDINATE_RESULT_V2_SIZE <=
                            sizeof(SrPpuBackgroundCoordinateResult) &&
                        SNES_RUNNER_API_PPU_BACKGROUND_COORDINATE_SIZE <=
                            sizeof(SnesRunnerApi),
                    "PPU background coordinate extent exceeds structure");
    failed |= check(SR_PPU_OBJ_RASTER_REQUEST_V2_SIZE <=
                        sizeof(SrPpuObjRasterRequest) &&
                        SR_PPU_OBJ_RASTER_RESULT_V2_SIZE <=
                            sizeof(SrPpuObjRasterResult),
                    "PPU OBJ raster extent exceeds structure");
    failed |= check(SNES_RUNNER_API_PPU_OBJ_RASTER_SIZE <=
                        sizeof(SnesRunnerApi),
                    "PPU OBJ raster API extent exceeds structure");
    failed |= check(SR_PPU_SURFACE_SNAPSHOT_V2_SIZE <=
                        sizeof(SrPpuSurfaceSnapshot),
                    "PPU surface snapshot extent exceeds structure");
    failed |= check(SNES_RUNNER_API_PPU_SURFACE_SIZE <=
                        sizeof(SnesRunnerApi),
                    "PPU surface API extent exceeds structure");
    failed |= check(SR_PPU_OUTPUT_BINDING_REQUEST_V2_SIZE <=
                            sizeof(SrPpuOutputBindingRequest) &&
                        SR_PPU_HORIZONTAL_MARGIN_REQUEST_V2_SIZE <=
                            sizeof(SrPpuHorizontalMarginRequest) &&
                        SNES_RUNNER_API_PPU_OUTPUT_CONTROL_SIZE <=
                            sizeof(SnesRunnerApi),
                    "PPU output control extent exceeds structure");
    failed |= check(SR_PPU_OVERLAY_CAPTURE_REQUEST_V2_SIZE <=
                            sizeof(SrPpuOverlayCaptureRequest) &&
                        SR_PPU_MODE7_OVERRIDE_REQUEST_V2_SIZE <=
                            sizeof(SrPpuMode7OverrideRequest) &&
                        SNES_RUNNER_API_PPU_CAPTURE_CONTROL_SIZE <=
                            sizeof(SnesRunnerApi),
                    "PPU capture control extent exceeds structure");
    failed |= check(SR_CPU_MATH_STATE_V2_SIZE <= sizeof(SrCpuMathState) &&
                        SNES_RUNNER_API_CPU_MATH_STATE_SIZE <=
                            sizeof(SnesRunnerApi),
                    "CPU math state extent exceeds structure");
    failed |= check(SR_EXECUTION_SNAPSHOT_V2_SIZE <=
                        sizeof(SrExecutionSnapshot),
                    "execution snapshot extent exceeds structure");
    failed |= check(SR_RUNNER_EVENT_V2_SIZE <= sizeof(SrRunnerEvent) &&
                        SR_EVENT_SUBSCRIPTION_V2_SIZE <=
                            sizeof(SrEventSubscription),
                    "event observer structure extent exceeds structure");
    failed |= check(SNES_RUNNER_API_EVENT_OBSERVER_SIZE <=
                        sizeof(SnesRunnerApi),
                    "event observer API extent exceeds structure");
    failed |= check(SR_MUTATION_COMMAND_V2_SIZE <=
                        sizeof(SrMutationCommand) &&
                        SR_MUTATION_STATUS_V2_SIZE <=
                            sizeof(SrMutationStatus),
                    "safe-point mutation extent exceeds structure");
    failed |= check(SNES_RUNNER_API_SAFE_POINT_MUTATION_SIZE <=
                        sizeof(SnesRunnerApi),
                    "safe-point mutation API extent exceeds structure");
    failed |= check(sizeof(((SrBorrowedSpan *)0)->byte_size) == sizeof(uint64_t),
                    "span size is not fixed width");
    failed |= check(sr_runner_get_api(SR_RUNNER_ABI_VERSION + 1u) == NULL,
                    "unsupported API version accepted");
    failed |= check(sr_runner_get_api(1u) == NULL,
                    "retired ABI v1 accepted");
    failed |= check(api != NULL, "API is null");
    if (api == NULL) return 1;
    failed |= check(api->abi_version == SR_RUNNER_ABI_VERSION,
                    "API version mismatch");
    failed |= check(api->struct_size == sizeof(*api), "API size mismatch");
    failed |= check((api->capabilities & SR_RUNNER_CAP_COMPONENT_HANDLES) != 0u,
                    "component capability missing");
    failed |= check((api->capabilities & SR_RUNNER_CAP_GENERATION_COUNTERS) != 0u,
                    "generation capability missing");
    failed |= check((api->capabilities & SR_RUNNER_CAP_BORROWED_BYTE_SPANS) != 0u,
                    "borrow capability missing");
    failed |= check((api->capabilities & SR_RUNNER_CAP_CPU_STATE) != 0u,
                    "CPU state capability missing");
    failed |= check((api->capabilities & SR_RUNNER_CAP_CPU_MATH_STATE) != 0u,
                    "CPU math state capability missing");
    failed |= check((api->capabilities &
                         (SR_RUNNER_CAP_PPU_STATE |
                          SR_RUNNER_CAP_BORROWED_U16_SPANS |
                          SR_RUNNER_CAP_PPU_FRAME_STATE |
                          SR_RUNNER_CAP_PPU_OBJ_RASTER |
                          SR_RUNNER_CAP_PPU_SURFACE_VIEWS |
                          SR_RUNNER_CAP_PPU_BACKGROUND_COORDINATE |
                          SR_RUNNER_CAP_PPU_OUTPUT_CONTROL |
                          SR_RUNNER_CAP_PPU_CAPTURE_CONTROL)) ==
                        (SR_RUNNER_CAP_PPU_STATE |
                         SR_RUNNER_CAP_BORROWED_U16_SPANS |
                         SR_RUNNER_CAP_PPU_FRAME_STATE |
                         SR_RUNNER_CAP_PPU_OBJ_RASTER |
                         SR_RUNNER_CAP_PPU_SURFACE_VIEWS |
                         SR_RUNNER_CAP_PPU_BACKGROUND_COORDINATE |
                         SR_RUNNER_CAP_PPU_OUTPUT_CONTROL |
                         SR_RUNNER_CAP_PPU_CAPTURE_CONTROL),
                    "PPU capabilities missing");
    failed |= check((api->capabilities &
                         (SR_RUNNER_CAP_EXECUTION_STATE |
                          SR_RUNNER_CAP_EVENT_OBSERVERS |
                          SR_RUNNER_CAP_SAFE_POINT_MUTATIONS)) ==
                        (SR_RUNNER_CAP_EXECUTION_STATE |
                         SR_RUNNER_CAP_EVENT_OBSERVERS |
                         SR_RUNNER_CAP_SAFE_POINT_MUTATIONS),
                    "observer capabilities missing");

    snes = snes_init(wram);
    failed |= check(snes != NULL, "runner allocation failed");
    if (snes == NULL) return 1;
    runner = sr_runner_handle(snes);

    snes->ppu->inidisp = 0x8du;
    snes->ppu->obsel = 0x63u;
    snes->ppu->bgmode = 0x19u;
    snes->ppu->mosaic = 0x31u;
    snes->ppu->bgXsc[0] = 0x63u;
    snes->ppu->bgXsc[1] = 0x54u;
    snes->ppu->bgXsc[2] = 0x45u;
    snes->ppu->bgXsc[3] = 0x36u;
    snes->ppu->bgTileAdr = 0x0005u;
    snes->ppu->m7sel = 0xc3u;
    for (unsigned matrix = 0u; matrix < 8u; ++matrix)
        snes->ppu->m7matrix[matrix] = (int16_t)(0x1100u + matrix);
    snes->ppu->windowsel = 0x91u;
    snes->ppu->wbgobjlog = 0x82u;
    snes->ppu->cgwsel = 0x73u;
    snes->ppu->cgadsub = 0x64u;
    snes->ppu->hScroll[0] = 0x1234u;
    snes->ppu->vScroll[0] = 0x2345u;
    snes->ppu->screenEnabled[0] = 0x17u;
    snes->ppu->screenEnabled[1] = 0x03u;
    snes->ppu->screenWindowed[0] = 0x02u;
    snes->ppu->screenWindowed[1] = 0x04u;
    snes->ppu->extraLeftCur = 12u;
    snes->ppu->extraRightCur = 13u;
    snes->ppu->extraTopCur = 14u;
    snes->ppu->extraBottomCur = 15u;
    snes->ppu->vram[7] = 0x4567u;
    snes->ppu->cgram[8] = 0x1234u;
    snes->ppu->oam[9] = 0x89abu;
    snes->ppu->highOam[10] = 0xcdu;
    snes->ppu->wsHudSplitHeight = 32u;
    snes->ppu->wsHudLeftEnd = 64u;
    snes->ppu->wsHudRightStart = 192u;
    snes->ppu->wsHudPlayerRowY = 8u;
    snes->ppu->wsHudLeftOnlyY = 16u;
    snes->ppu->extraLeftRight = 48u;
    PpuSetWidescreenLayerMirror(snes->ppu, 0x01u);
    PpuSetWidescreenLayerRepeatBand(snes->ppu, 1u, 20u, 21u);
    snes->ppu->overlayCaptures[0].x0 = -12;
    snes->ppu->overlayCaptures[0].x1 = 268;
    snes->ppu->overlayCaptures[0].y0 = -4;
    snes->ppu->overlayCaptures[0].y1 = 228;
    snes->ppu->overlayCaptures[0].flags =
        kPpuOverlayFlag_MarkFullAddSubscreen |
        kPpuOverlayFlag_MarkOwningScreenWinner;
    snes->ppu->overlayCaptures[0].transparentFillMode =
        kPpuOverlayTransparentFill_Cgram;
    snes->ppu->overlayCaptures[0].transparentFillCgram = 8u;
    snes->ppu->overlayCaptures[0].transparentFillConfigured = 1u;
    snes->ppu->overlayCaptures[0].oamFirst = 7u;
    snes->ppu->overlayCaptures[0].oamCount = 9u;
    snes->ppu->overlayRenderBuffer[0] = &s_overlay_pixel;
    snes->ppu->overlayRenderBands[0][0] = &s_overlay_pixel;
    snes->ppu->overlayRenderContentMask[0] = 3u;
    snes->ppu->overlayRenderContentMask[1] = 1u;
    snes->ppu->m7Override.rgba = &s_mode7_pixel;
    failed |= check(api->query_cpu_state(runner, &cpu_state) ==
                        SR_RESULT_UNAVAILABLE,
                    "CPU state without a provider was available");
    failed |= check(api->query_execution_state(runner, &execution_state) ==
                        SR_RESULT_UNAVAILABLE,
                    "execution state without a provider was available");
    failed |= check(api->rasterize_ppu_obj_range(
                        runner, &obj_request, &obj_result) ==
                        SR_RESULT_UNAVAILABLE,
                    "PPU OBJ raster without a provider was available");
    cpu_state.struct_size = sizeof(cpu_state);
    sr_runner_set_cpu_state_provider(
        snes, query_test_cpu_state, &s_cpu_component);
    sr_runner_set_execution_state_provider(
        snes, query_test_execution_state);
    sr_runner_bind_ppu_services(snes, true);

    failed |= check_generation(api, runner, 0u, 0u, 0u, 0u, 0u);
    failed |= check(api->query_generations(runner, &too_small) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized generation output accepted");
    failed |= check(api->get_component(runner, SR_COMPONENT_PPU, &component) ==
                        SR_RESULT_OK && component != NULL,
                    "PPU component unavailable");
    failed |= check(api->get_component(runner, SR_COMPONENT_CPU, &component) ==
                        SR_RESULT_OK && component != NULL,
                    "CPU component unavailable");
    failed |= check(api->get_component(runner, UINT32_MAX, &component) ==
                        SR_RESULT_UNSUPPORTED && component == NULL,
                    "unknown component accepted");

    failed |= check(api->borrow_memory(runner, SR_MEMORY_WRAM, &wram_span) ==
                        SR_RESULT_OK,
                    "WRAM borrow failed");
    failed |= check(wram_span.data == wram && wram_span.byte_size == sizeof(wram),
                    "WRAM span mismatch");
    failed |= check(api->borrow_is_valid(runner, &wram_span),
                    "new WRAM span is invalid");
    failed |= check(api->borrow_memory(runner, SR_MEMORY_APU_RAM, &apu_span) ==
                        SR_RESULT_UNSUPPORTED && apu_span.data == NULL,
                    "asynchronous APU RAM borrow accepted");
    failed |= check(api->borrow_memory(runner, SR_MEMORY_HIGH_OAM,
                                       &high_oam_span) == SR_RESULT_OK &&
                        high_oam_span.byte_size ==
                            SR_PPU_HIGH_OAM_BYTE_COUNT &&
                        high_oam_span.data[10] == 0xcdu,
                    "high OAM byte borrow mismatch");
    failed |= check(api->borrow_memory(runner, UINT32_MAX, &unsupported) ==
                        SR_RESULT_UNSUPPORTED && unsupported.data == NULL,
                    "unknown memory accepted");
    failed |= check(api->borrow_memory(runner, SR_MEMORY_WRAM,
                                       &too_small_span) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized span output accepted");

    failed |= check(api->borrow_u16_memory(runner, SR_MEMORY_VRAM,
                                           &vram_span) == SR_RESULT_OK &&
                        vram_span.element_count == SR_PPU_VRAM_WORD_COUNT &&
                        vram_span.data[7] == 0x4567u,
                    "VRAM u16 borrow mismatch");
    failed |= check(api->borrow_u16_memory(runner, SR_MEMORY_CGRAM,
                                           &cgram_span) == SR_RESULT_OK &&
                        cgram_span.element_count == SR_PPU_CGRAM_WORD_COUNT &&
                        cgram_span.data[8] == 0x1234u,
                    "CGRAM u16 borrow mismatch");
    failed |= check(api->borrow_u16_memory(runner, SR_MEMORY_OAM,
                                           &oam_span) == SR_RESULT_OK &&
                        oam_span.element_count == SR_PPU_OAM_WORD_COUNT &&
                        oam_span.data[9] == 0x89abu,
                    "OAM u16 borrow mismatch");
    failed |= check(api->borrow_u16_is_valid(runner, &vram_span),
                    "new VRAM span is invalid");
    failed |= check(api->borrow_u16_memory(runner, SR_MEMORY_WRAM,
                                           &small_u16_span) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized u16 span output accepted");

    failed |= check(api->query_ppu_state(runner, &ppu_state) == SR_RESULT_OK,
                    "PPU state query failed");
    failed |= check(ppu_state.display_control == 0x8du &&
                        ppu_state.object_select == 0x63u &&
                        ppu_state.bg_mode_control == 0x19u &&
                        ppu_state.mosaic_control == 0x31u &&
                        ppu_state.bg_mode == 1u &&
                        ppu_state.brightness == 13u,
                    "PPU register snapshot mismatch");
    failed |= check((ppu_state.flags &
                         (SR_PPU_STATE_FORCED_BLANK |
                          SR_PPU_STATE_BG3_PRIORITY)) ==
                        (SR_PPU_STATE_FORCED_BLANK |
                         SR_PPU_STATE_BG3_PRIORITY),
                    "PPU flag snapshot mismatch");
    failed |= check(ppu_state.main_screen == 0x17u &&
                        ppu_state.sub_screen == 0x03u &&
                        ppu_state.main_windowed == 0x02u &&
                        ppu_state.sub_windowed == 0x04u,
                    "PPU screen snapshot mismatch");
    failed |= check(ppu_state.margin_left == 12u &&
                        ppu_state.margin_right == 13u &&
                        ppu_state.margin_top == 14u &&
                        ppu_state.margin_bottom == 15u,
                    "PPU margin snapshot mismatch");
    failed |= check(ppu_state.backgrounds[0].h_scroll == 0x1234u &&
                        ppu_state.backgrounds[0].v_scroll == 0x2345u &&
                        ppu_state.backgrounds[0].tilemap_base_word == 0x6000u &&
                        ppu_state.backgrounds[0].tile_base_word == 0x5000u &&
                        ppu_state.backgrounds[0].tilemap_width_tiles == 64u &&
                        ppu_state.backgrounds[0].tilemap_height_tiles == 64u &&
                        ppu_state.backgrounds[0].tile_size_pixels == 16u &&
                        ppu_state.backgrounds[0].bits_per_pixel == 4u,
                    "PPU background snapshot mismatch");
    failed |= check(ppu_state.struct_size == SR_PPU_STATE_SNAPSHOT_V2_SIZE &&
                        ppu_state.window_select == 0x91u &&
                        ppu_state.window_logic == 0x82u &&
                        ppu_state.color_math_control == 0x73u &&
                        ppu_state.color_math_designation == 0x64u &&
                        ppu_state.background_tilemap_control[0] == 0x63u &&
                        ppu_state.background_tilemap_control[1] == 0x54u &&
                        ppu_state.background_tilemap_control[2] == 0x45u &&
                        ppu_state.background_tilemap_control[3] == 0x36u &&
                        ppu_state.background_tile_base_control == 0x0005u &&
                        ppu_state.mode7_select == 0xc3u &&
                        ppu_state.mode7_matrix[0] == 0x1100 &&
                        ppu_state.mode7_matrix[7] == 0x1107,
                    "PPU v2 raw-control snapshot mismatch");
    failed |= check(api->query_ppu_state(runner, &small_ppu_state) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU snapshot accepted");

    failed |= check(api->query_ppu_frame_state(runner, &ppu_frame) ==
                        SR_RESULT_OK,
                    "PPU frame query failed");
    failed |= check(ppu_frame.display_control == 0x8du &&
                        ppu_frame.bg_mode == 1u &&
                        ppu_frame.hud_split_height == 32u &&
                        ppu_frame.hud_left_end == 64u &&
                        ppu_frame.hud_right_start == 192u &&
                        ppu_frame.hud_player_row_y == 8u &&
                        ppu_frame.hud_left_only_y == 16u &&
                        ppu_frame.margin_budget == 48u &&
                        ppu_frame.mode7_override_active == 1u &&
                        ppu_frame.overlay_count == SR_PPU_OVERLAY_SOURCE_COUNT,
                    "PPU frame scalar snapshot mismatch");
    failed |= check(ppu_frame.overlays[0].x0 == -12 &&
                        ppu_frame.overlays[0].x1 == 268 &&
                        ppu_frame.overlays[0].y0 == -4 &&
                        ppu_frame.overlays[0].y1 == 228 &&
                        ppu_frame.overlays[0].flags ==
                            (SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN |
                             SR_PPU_OVERLAY_MARK_OWNING_SCREEN_WINNER) &&
                        ppu_frame.overlays[0].content_band_mask == 3u &&
                        ppu_frame.overlays[0].transparent_fill_argb ==
                            UINT32_C(0xff8f791c) &&
                        ppu_frame.overlays[0].transparent_fill_configured == 1u &&
                        ppu_frame.overlays[0].oam_first == 7u &&
                        ppu_frame.overlays[0].oam_count == 9u,
                    "PPU overlay snapshot mismatch");
    failed |= check(ppu_frame.overlays[1].content_band_mask == 0u,
                    "unbound overlay surface reported content");
    failed |= check(api->query_ppu_frame_state(runner, &small_ppu_frame) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU frame snapshot accepted");

    coordinate_request.lifetime_generation =
        ppu_state.lifetime_generation;
    failed |= check(api->resolve_ppu_background_coordinate(
                        runner, &coordinate_request, &coordinate_result) ==
                            SR_RESULT_OK &&
                        coordinate_result.struct_size ==
                            SR_PPU_BACKGROUND_COORDINATE_RESULT_V2_SIZE &&
                        coordinate_result.flags ==
                            (SR_PPU_BACKGROUND_COORDINATE_MAPPED |
                             SR_PPU_BACKGROUND_COORDINATE_MOSAIC) &&
                        coordinate_result.source_x == 4 &&
                        coordinate_result.sample_y == 8 &&
                        coordinate_result.fill ==
                            SR_PPU_BACKGROUND_FILL_MIRROR &&
                        coordinate_result.motion ==
                            SR_PPU_BACKGROUND_MOTION_FILL_RELATIVE,
                    "PPU mirrored mosaic coordinate mismatch");
    coordinate_request.layer = 1u;
    coordinate_request.screen_x = 260;
    coordinate_request.screen_y = 20;
    coordinate_result.struct_size = sizeof(coordinate_result);
    failed |= check(api->resolve_ppu_background_coordinate(
                        runner, &coordinate_request, &coordinate_result) ==
                            SR_RESULT_OK &&
                        coordinate_result.flags ==
                            (SR_PPU_BACKGROUND_COORDINATE_MAPPED |
                             SR_PPU_BACKGROUND_COORDINATE_BAND_OVERRIDE) &&
                        coordinate_result.source_x == 4 &&
                        coordinate_result.sample_y == 21 &&
                        coordinate_result.fill ==
                            SR_PPU_BACKGROUND_FILL_REPEAT,
                    "PPU repeat-band coordinate mismatch");
    failed |= check(api->resolve_ppu_background_coordinate(
                        runner, &coordinate_request,
                        &small_coordinate_result) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU coordinate result accepted");
    coordinate_request.lifetime_generation++;
    coordinate_result.struct_size = sizeof(coordinate_result);
    failed |= check(api->resolve_ppu_background_coordinate(
                        runner, &coordinate_request, &coordinate_result) ==
                            SR_RESULT_STALE_VIEW,
                    "stale PPU coordinate request accepted");
    coordinate_request.lifetime_generation--;
    coordinate_request.screen_x =
        (int32_t)(SR_PPU_NATIVE_WIDTH + SR_PPU_HORIZONTAL_MARGIN_MAX);
    coordinate_result.struct_size = sizeof(coordinate_result);
    failed |= check(api->resolve_ppu_background_coordinate(
                        runner, &coordinate_request, &coordinate_result) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "out-of-range PPU coordinate accepted");

    output_binding = (SrPpuOutputBindingRequest) {
        .struct_size = sizeof(output_binding),
        .flags = SR_PPU_OUTPUT_REFERENCE_PIXEL_RENDERER,
        .kind = SR_PPU_OUTPUT_MAIN,
        .pixels = (uint8_t *)s_main_surface,
        .pixel_byte_size = sizeof(s_main_surface),
        .pitch_bytes = 384u * sizeof(uint32_t),
        .height_pixels = 253u,
    };
    failed |= check(api->bind_ppu_output_surface(
                        runner, &output_binding) == SR_RESULT_OK &&
                        snes->ppu->renderFlags ==
                            kPpuRenderFlags_ReferencePixelRenderer,
                    "main output surface bind failed");
    output_binding = (SrPpuOutputBindingRequest) {
        .struct_size = sizeof(output_binding),
        .kind = SR_PPU_OUTPUT_AUTHENTIC,
        .pixels = (uint8_t *)s_authentic_surface,
        .pixel_byte_size = sizeof(s_authentic_surface),
        .pitch_bytes = 384u * sizeof(uint32_t),
        .height_pixels = 253u,
    };
    failed |= check(api->bind_ppu_output_surface(
                        runner, &output_binding) == SR_RESULT_OK,
                    "authentic output surface bind failed");
    output_binding = (SrPpuOutputBindingRequest) {
        .struct_size = sizeof(output_binding),
        .kind = SR_PPU_OUTPUT_OVERLAY,
        .source = SR_PPU_OVERLAY_BG1,
        .pixels = (uint8_t *)s_overlay_surface,
        .pixel_byte_size = sizeof(s_overlay_surface),
        .pitch_bytes = 384u * sizeof(uint32_t),
        .height_pixels = 232u,
    };
    failed |= check(api->bind_ppu_output_surface(
                        runner, &output_binding) == SR_RESULT_OK,
                    "overlay output surface bind failed");
    output_binding = (SrPpuOutputBindingRequest) {
        .struct_size = sizeof(output_binding),
        .kind = SR_PPU_OUTPUT_OVERLAY_PRIORITY,
        .source = SR_PPU_OVERLAY_BG1,
        .band = 1u,
        .pixels = (uint8_t *)s_overlay_band_surface,
        .pixel_byte_size = sizeof(s_overlay_band_surface),
        .pitch_bytes = 384u * sizeof(uint32_t),
        .height_pixels = 232u,
    };
    failed |= check(api->bind_ppu_output_surface(
                        runner, &output_binding) == SR_RESULT_OK,
                    "overlay priority output surface bind failed");
    snes->ppu->overlayRenderContentMask[0] = 3u;
    output_binding = (SrPpuOutputBindingRequest) {
        .struct_size = sizeof(output_binding),
        .kind = SR_PPU_OUTPUT_MODE7,
        .scale = 2u,
        .pixels = (uint8_t *)s_mode7_surface,
        .pixel_byte_size = sizeof(s_mode7_surface),
        .pitch_bytes = 768u * sizeof(uint32_t),
        .height_pixels = 506u,
    };
    failed |= check(api->bind_ppu_output_surface(
                        runner, &output_binding) == SR_RESULT_OK,
                    "Mode-7 output surface bind failed");
    failed |= check(api->query_ppu_surfaces(runner, &ppu_surfaces) ==
                        SR_RESULT_OK,
                    "PPU surface query failed");
    failed |= check(ppu_surfaces.overlay_count ==
                            SR_PPU_OVERLAY_SOURCE_COUNT &&
                        ppu_surfaces.band_count == SR_PPU_SURFACE_BAND_COUNT &&
                        ppu_surfaces.binding_generation != 0u,
                    "PPU surface snapshot header mismatch");
    failed |= check(ppu_surfaces.main.data ==
                            (const uint8_t *)s_main_surface &&
                        ppu_surfaces.main.pitch_bytes ==
                            384u * sizeof(uint32_t) &&
                        ppu_surfaces.main.width_pixels == 384u &&
                        ppu_surfaces.main.height_pixels == 253u &&
                        ppu_surfaces.main.origin_x == 64 &&
                        ppu_surfaces.main.origin_y == 14 &&
                        ppu_surfaces.main.scale == 1u &&
                        ppu_surfaces.main.flags ==
                            (SR_PPU_SURFACE_BOUND |
                             SR_PPU_SURFACE_HAS_CONTENT),
                    "main PPU surface mismatch");
    failed |= check(ppu_surfaces.authentic.data ==
                            (const uint8_t *)s_authentic_surface &&
                        ppu_surfaces.authentic.byte_size ==
                            384u * 253u * sizeof(uint32_t),
                    "authentic PPU surface mismatch");
    failed |= check(ppu_surfaces.overlays[0][0].data ==
                            (const uint8_t *)s_overlay_surface &&
                        ppu_surfaces.overlays[0][0].height_pixels == 232u &&
                        ppu_surfaces.overlays[0][0].origin_x == 64 &&
                        ppu_surfaces.overlays[0][0].origin_y == 4 &&
                        (ppu_surfaces.overlays[0][0].flags &
                         SR_PPU_SURFACE_HAS_CONTENT) != 0u &&
                        ppu_surfaces.overlays[0][1].data ==
                            (const uint8_t *)s_overlay_band_surface &&
                        (ppu_surfaces.overlays[0][1].flags &
                         SR_PPU_SURFACE_HAS_CONTENT) != 0u &&
                        ppu_surfaces.overlays[1][0].data == NULL,
                    "overlay PPU surface mismatch");
    /* Presentation may restore a short HUD capture after a full separated
     * plane was rendered. The bound buffer capacity, not that later policy,
     * remains the readable surface extent. */
    memset(&snes->ppu->overlayCaptures[0], 0,
           sizeof(snes->ppu->overlayCaptures[0]));
    ppu_surfaces.struct_size = sizeof(ppu_surfaces);
    failed |= check(api->query_ppu_surfaces(runner, &ppu_surfaces) ==
                            SR_RESULT_OK &&
                        ppu_surfaces.overlays[0][0].height_pixels == 232u &&
                        ppu_surfaces.overlays[0][1].height_pixels == 232u,
                    "restored capture truncated bound overlay capacity");
    failed |= check(ppu_surfaces.mode7.data ==
                            (const uint8_t *)s_mode7_surface &&
                        ppu_surfaces.mode7.width_pixels == 768u &&
                        ppu_surfaces.mode7.height_pixels == 506u &&
                        ppu_surfaces.mode7.origin_x == 128 &&
                        ppu_surfaces.mode7.origin_y == 28 &&
                        ppu_surfaces.mode7.scale == 2u &&
                        (ppu_surfaces.mode7.flags &
                         SR_PPU_SURFACE_HAS_CONTENT) != 0u,
                    "Mode-7 PPU surface mismatch");
    failed |= check(api->ppu_surface_snapshot_is_valid(
                        runner, &ppu_surfaces),
                    "new PPU surface snapshot is invalid");
    failed |= check(api->query_ppu_surfaces(runner, &small_ppu_surfaces) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU surface snapshot accepted");
    output_binding = (SrPpuOutputBindingRequest) {
        .struct_size = sizeof(output_binding),
        .kind = SR_PPU_OUTPUT_OVERLAY,
        .source = SR_PPU_OVERLAY_BG1,
        .pixels = (uint8_t *)s_overlay_surface,
        .pixel_byte_size = sizeof(s_overlay_surface),
        .pitch_bytes = 384u * sizeof(uint32_t),
        .height_pixels = 232u,
    };
    failed |= check(api->bind_ppu_output_surface(
                        runner, &output_binding) == SR_RESULT_OK,
                    "overlay output surface rebind failed");
    failed |= check(!api->ppu_surface_snapshot_is_valid(
                        runner, &ppu_surfaces),
                    "surface rebind did not expire PPU surface snapshot");
    failed |= check(api->query_ppu_surfaces(runner, &rebound_surfaces) ==
                        SR_RESULT_OK &&
                        api->ppu_surface_snapshot_is_valid(
                            runner, &rebound_surfaces),
                    "PPU surface snapshot requery failed");
    output_binding.struct_size = sizeof(uint32_t);
    failed |= check(api->bind_ppu_output_surface(
                        runner, &output_binding) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU output binding accepted");
    output_binding.struct_size = sizeof(output_binding);
    output_binding.lifetime_generation++;
    failed |= check(api->bind_ppu_output_surface(
                        runner, &output_binding) == SR_RESULT_STALE_VIEW,
                    "stale PPU output binding accepted");
    output_binding.lifetime_generation--;
    output_binding.pixel_byte_size--;
    failed |= check(api->bind_ppu_output_surface(
                        runner, &output_binding) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU output capacity accepted");
    output_binding.pixel_byte_size++;

    margin_request.mode = SR_PPU_HORIZONTAL_MARGIN_CENTERED;
    margin_request.budget_pixels = 64u;
    failed |= check(api->configure_ppu_horizontal_margin(
                        runner, &margin_request) == SR_RESULT_OK &&
                        snes->ppu->extraLeftRight == 64u &&
                        snes->ppu->extraLeftCur == 0u &&
                        snes->ppu->extraRightCur == 0u,
                    "centered PPU margin configuration failed");
    failed |= check(!api->ppu_surface_snapshot_is_valid(
                        runner, &rebound_surfaces),
                    "PPU margin change did not expire surface snapshot");
    rebound_surfaces.struct_size = sizeof(rebound_surfaces);
    failed |= check(api->query_ppu_surfaces(runner, &rebound_surfaces) ==
                        SR_RESULT_OK,
                    "PPU surface query after margin change failed");
    failed |= check(api->configure_ppu_horizontal_margin(
                        runner, &margin_request) == SR_RESULT_OK &&
                        api->ppu_surface_snapshot_is_valid(
                            runner, &rebound_surfaces),
                    "unchanged PPU margin expired surface snapshot");
    margin_request.budget_pixels = SR_PPU_HORIZONTAL_MARGIN_MAX + 1u;
    failed |= check(api->configure_ppu_horizontal_margin(
                        runner, &margin_request) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "out-of-range PPU margin accepted");
    margin_request.budget_pixels = 48u;
    margin_request.mode = SR_PPU_HORIZONTAL_MARGIN_AVAILABLE;
    failed |= check(api->configure_ppu_horizontal_margin(
                        runner, &margin_request) == SR_RESULT_OK &&
                        snes->ppu->extraLeftRight == 48u &&
                        snes->ppu->extraLeftCur == 48u &&
                        snes->ppu->extraRightCur == 48u,
                    "available PPU margin configuration failed");

    output_binding = (SrPpuOutputBindingRequest) {
        .struct_size = sizeof(output_binding),
        .kind = SR_PPU_OUTPUT_CLEAR_OVERLAY_SOURCES,
    };
    failed |= check(api->bind_ppu_output_surface(
                        runner, &output_binding) == SR_RESULT_OK &&
                        snes->ppu->overlayRenderBuffer[0] == NULL &&
                        snes->ppu->overlayRenderBands[0][0] == NULL,
                    "PPU overlay clear failed");

    capture_request = (SrPpuOverlayCaptureRequest) {
        .struct_size = sizeof(capture_request),
        .flags = SR_PPU_OVERLAY_REMOVE_FROM_GAME |
                 SR_PPU_OVERLAY_MARK_MAIN_SCREEN_WINNER,
        .source = SR_PPU_OVERLAY_BG2,
        .x = -8,
        .y = 4,
        .width = 120,
        .height = 32,
    };
    failed |= check(api->claim_ppu_overlay_capture(
                        runner, &capture_request) == SR_RESULT_OK &&
                        snes->ppu->overlayCaptures[SR_PPU_OVERLAY_BG2].x0 ==
                            -8 &&
                        snes->ppu->overlayCaptures[SR_PPU_OVERLAY_BG2].x1 ==
                            112 &&
                        snes->ppu->overlayCaptures[SR_PPU_OVERLAY_BG2].flags ==
                            (SR_PPU_OVERLAY_REMOVE_FROM_GAME |
                             SR_PPU_OVERLAY_MARK_MAIN_SCREEN_WINNER),
                    "PPU overlay capture claim failed");
    failed |= check(api->claim_ppu_overlay_capture(
                        runner, &capture_request) == SR_RESULT_BUSY,
                    "busy PPU overlay capture claim accepted");
    capture_request.lifetime_generation++;
    failed |= check(api->claim_ppu_overlay_capture(
                        runner, &capture_request) == SR_RESULT_STALE_VIEW,
                    "stale PPU overlay capture claim accepted");
    capture_request.lifetime_generation--;
    capture_request.flags = UINT32_MAX;
    failed |= check(api->claim_ppu_overlay_capture(
                        runner, &capture_request) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "invalid PPU overlay capture flags accepted");

    mode7_override_request = (SrPpuMode7OverrideRequest) {
        .struct_size = sizeof(mode7_override_request),
        .pixels = s_mode7_surface,
        .pixel_byte_size = 4u * sizeof(uint32_t),
        .width_pixels = 2u,
        .height_pixels = 2u,
        .canvas_x0 = 10,
        .canvas_y0 = 20,
        .canvas_x1 = 30,
        .canvas_y1 = 40,
        .wrap = 1u,
    };
    failed |= check(api->claim_ppu_mode7_override(
                        runner, &mode7_override_request) == SR_RESULT_OK &&
                        snes->ppu->m7Override.rgba == s_mode7_surface &&
                        snes->ppu->m7Override.width == 2 &&
                        snes->ppu->m7Override.height == 2 &&
                        snes->ppu->m7Override.canvasX0 == 10 &&
                        snes->ppu->m7Override.canvasY1 == 40 &&
                        snes->ppu->m7Override.wrap == 1u,
                    "PPU Mode-7 override claim failed");
    failed |= check(api->claim_ppu_mode7_override(
                        runner, &mode7_override_request) == SR_RESULT_BUSY,
                    "busy PPU Mode-7 override claim accepted");
    snes->ppu->m7Override.rgba = NULL;
    mode7_override_request.pixel_byte_size--;
    failed |= check(api->claim_ppu_mode7_override(
                        runner, &mode7_override_request) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU Mode-7 image accepted");

    snes->ppu->inidisp = 0x0fu;
    snes->ppu->obsel = 0u;
    snes->ppu->cgram[0x81] = 0x001fu;
    set_solid_4bpp_tile(snes->ppu, 0u, 1u);
    snes->ppu->oam[0] = 10u | (20u << 8);
    snes->ppu->oam[1] = 2u << 12;
    memset(obj_pixels, 0x5a, sizeof(obj_pixels));
    failed |= check(api->rasterize_ppu_obj_range(
                        runner, &obj_request, &obj_result) == SR_RESULT_OK,
                    "PPU OBJ raster failed");
    failed |= check(obj_result.lifetime_generation == 0u &&
                        obj_result.x0 == 10 && obj_result.y0 == 20 &&
                        obj_result.x1 == 18 && obj_result.y1 == 28 &&
                        obj_result.width == 8u && obj_result.height == 8u,
                    "PPU OBJ raster bounds mismatch");
    failed |= check(obj_pixels[0] == UINT32_C(0xffff0000) &&
                        obj_pixels[63] == UINT32_C(0xffff0000),
                    "PPU OBJ raster pixels mismatch");
    failed |= check(api->rasterize_ppu_obj_range(
                        runner, &obj_request, &small_obj_result) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU OBJ raster result accepted");
    obj_request.pixel_byte_size = sizeof(uint32_t);
    obj_result.struct_size = sizeof(obj_result);
    failed |= check(api->rasterize_ppu_obj_range(
                        runner, &obj_request, &obj_result) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU OBJ raster buffer accepted");
    obj_request.pixel_byte_size = sizeof(obj_pixels);
    obj_request.pixel_format = UINT32_MAX;
    obj_result.struct_size = sizeof(obj_result);
    failed |= check(api->rasterize_ppu_obj_range(
                        runner, &obj_request, &obj_result) ==
                        SR_RESULT_UNSUPPORTED,
                    "unknown PPU OBJ pixel format accepted");
    obj_request.pixel_format = SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32;

    failed |= check(api->query_cpu_state(runner, &cpu_state) == SR_RESULT_OK,
                    "CPU state query failed");
    failed |= check(cpu_state.a == 0x1234u && cpu_state.x == 0x5678u &&
                        cpu_state.y == 0x9abcu && cpu_state.s == 0x01efu &&
                        cpu_state.d == 0x0020u,
                    "CPU 16-bit register snapshot mismatch");
    failed |= check(cpu_state.db == 0x7eu && cpu_state.pb == 0x03u &&
                        cpu_state.p == 0xa5u,
                    "CPU 8-bit register snapshot mismatch");
    failed |= check(cpu_state.lifetime_generation == 0u &&
                        cpu_state.frame_counter == 42u &&
                        cpu_state.execution_pc24 == 0x123456u,
                    "CPU execution metadata mismatch");
    failed |= check((cpu_state.flags &
                         (SR_CPU_STATE_M_FLAG | SR_CPU_STATE_EMULATION |
                          SR_CPU_STATE_HOST_RETURN_VALID |
                          SR_CPU_STATE_EXECUTION_PC_VALID)) ==
                        (SR_CPU_STATE_M_FLAG | SR_CPU_STATE_EMULATION |
                         SR_CPU_STATE_HOST_RETURN_VALID |
                         SR_CPU_STATE_EXECUTION_PC_VALID) &&
                        (cpu_state.flags & SR_CPU_STATE_X_FLAG) == 0u,
                    "CPU flag snapshot mismatch");
    failed |= check(api->query_cpu_state(runner, &small_cpu_state) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized CPU snapshot accepted");

    failed |= check(api->query_execution_state(runner, &execution_state) ==
                        SR_RESULT_OK,
                    "execution state query failed");
    failed |= check(execution_state.lifetime_generation == 0u &&
                        execution_state.block_serial == 91u &&
                        execution_state.current_block_pc24 == 0x345678u &&
                        execution_state.stack_depth == 2u &&
                        execution_state.history_count == 2u &&
                        strcmp(execution_state.current_function,
                               "test_current") == 0,
                    "execution state metadata mismatch");
    failed |= check(strcmp(execution_state.stack[0].function_name,
                           "outer") == 0 &&
                        execution_state.stack[0].entry_stack == 0x01ffu &&
                        execution_state.stack[0].host_return_valid == 1u &&
                        strcmp(execution_state.stack[1].function_name,
                               "inner") == 0 &&
                        execution_state.stack[1].entry_stack == 0x01fbu,
                    "execution stack mismatch");
    failed |= check(execution_state.history[0].pc24 == 0x123400u &&
                        execution_state.history[0].cpu_flags ==
                            SR_CPU_STATE_M_FLAG &&
                        execution_state.history[0].register_x == 0x4567u &&
                        execution_state.history[0].stack_pointer == 0x01f0u &&
                        execution_state.history[1].pc24 == 0x123456u &&
                        execution_state.history[1].cpu_flags ==
                            SR_CPU_STATE_X_FLAG,
                    "execution block history mismatch");
    failed |= check(api->query_execution_state(
                        runner, &small_execution_state) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized execution snapshot accepted");

    {
        SrEventSubscription invalid = subscription;
        uint64_t invalid_id = 7u;
        invalid.struct_size = sizeof(uint32_t);
        failed |= check(api->subscribe_events(runner, &invalid,
                                               &invalid_id) ==
                            SR_RESULT_INVALID_ARGUMENT &&
                            invalid_id == 0u,
                        "undersized event subscription accepted");
        invalid = subscription;
        invalid.event_mask = SR_EVENT_MASK_RECOMP_FUNCTION;
        failed |= check(api->subscribe_events(runner, &invalid,
                                               &invalid_id) ==
                            SR_RESULT_UNSUPPORTED,
                        "unsupported event class accepted");
        invalid = frame_subscription;
        invalid.flags = SR_EVENT_FILTER_PC_RANGE;
        invalid.pc_first = 0u;
        invalid.pc_last = 0xffffffu;
        failed |= check(api->subscribe_events(runner, &invalid,
                                               &invalid_id) ==
                            SR_RESULT_UNSUPPORTED,
                        "PC filter accepted for frame events");
        invalid = subscription;
        invalid.flags = SR_EVENT_FILTER_ADDRESS_RANGE;
        failed |= check(api->subscribe_events(runner, &invalid,
                                               &invalid_id) ==
                            SR_RESULT_UNSUPPORTED,
                        "address filter accepted for block events");
        invalid = memory_subscription;
        invalid.address_first = 0x0200u;
        invalid.address_last = 0x0100u;
        failed |= check(api->subscribe_events(runner, &invalid,
                                               &invalid_id) ==
                            SR_RESULT_INVALID_ARGUMENT,
                        "inverted address filter accepted");
        invalid = register_subscription;
        invalid.flags |= SR_EVENT_FILTER_MEMORY_REGION;
        failed |= check(api->subscribe_events(runner, &invalid,
                                               &invalid_id) ==
                            SR_RESULT_UNSUPPORTED,
                        "memory-region filter accepted for registers");
        invalid = subscription;
        invalid.pc_first = 0x110000u;
        invalid.pc_last = 0x100000u;
        failed |= check(api->subscribe_events(runner, &invalid,
                                               &invalid_id) ==
                            SR_RESULT_INVALID_ARGUMENT,
                        "inverted PC filter accepted");
    }
    {
        SrMutationCommand invalid = memory_mutation;
        uint64_t invalid_id = 7u;
        invalid.struct_size = sizeof(uint32_t);
        failed |= check(api->queue_mutation(runner, &invalid, &invalid_id) ==
                            SR_RESULT_INVALID_ARGUMENT &&
                            invalid_id == 0u,
                        "undersized mutation command accepted");
        invalid = memory_mutation;
        invalid.memory_region = SR_MEMORY_ROM;
        failed |= check(api->queue_mutation(runner, &invalid, &invalid_id) ==
                            SR_RESULT_UNSUPPORTED,
                        "ROM mutation accepted");
        invalid = memory_mutation;
        invalid.address = TEST_WRAM_SIZE - 1u;
        invalid.byte_count = 2u;
        failed |= check(api->queue_mutation(runner, &invalid, &invalid_id) ==
                            SR_RESULT_INVALID_ARGUMENT,
                        "out-of-range memory mutation accepted");
        invalid = memory_mutation;
        invalid.type = UINT32_MAX;
        failed |= check(api->queue_mutation(runner, &invalid, &invalid_id) ==
                            SR_RESULT_UNSUPPORTED,
                        "unknown mutation type accepted");
        invalid = input_mutation;
        invalid.input_mask = UINT32_C(0x01000000);
        failed |= check(api->queue_mutation(runner, &invalid, &invalid_id) ==
                            SR_RESULT_INVALID_ARGUMENT,
                        "out-of-range input mutation accepted");
        failed |= check(api->query_mutation(
                            runner, 0u, 0u, &mutation_status) ==
                            SR_RESULT_INVALID_ARGUMENT,
                        "zero mutation id accepted");
    }
    failed |= check(api->subscribe_events(runner, &subscription,
                                           &subscription_id) ==
                        SR_RESULT_OK && subscription_id != 0u,
                    "filtered observer subscription failed");
    failed |= check(api->subscribe_events(runner, &dispatch_subscription,
                                           &dispatch_subscription_id) ==
                        SR_RESULT_OK && dispatch_subscription_id != 0u &&
                        dispatch_subscription_id != subscription_id,
                    "second observer subscription failed");
    failed |= check(api->subscribe_events(runner, &memory_subscription,
                                           &memory_subscription_id) ==
                        SR_RESULT_OK && memory_subscription_id != 0u,
                    "memory observer subscription failed");
    failed |= check(api->subscribe_events(runner, &register_subscription,
                                           &register_subscription_id) ==
                        SR_RESULT_OK && register_subscription_id != 0u,
                    "register observer subscription failed");
    failed |= check(api->subscribe_events(runner, &ppu_memory_subscription,
                                           &ppu_memory_subscription_id) ==
                        SR_RESULT_OK && ppu_memory_subscription_id != 0u,
                    "PPU memory observer subscription failed");
    failed |= check(api->subscribe_events(runner, &dma_subscription,
                                           &dma_subscription_id) ==
                        SR_RESULT_OK && dma_subscription_id != 0u,
                    "DMA observer subscription failed");
    failed |= check(api->subscribe_events(runner, &frame_subscription,
                                           &frame_subscription_id) ==
                        SR_RESULT_OK && frame_subscription_id != 0u,
                    "frame observer subscription failed");
    failed |= check(api->subscribe_events(
                        runner, &interrupt_error_subscription,
                        &interrupt_error_subscription_id) == SR_RESULT_OK &&
                        interrupt_error_subscription_id != 0u,
                    "interrupt/error observer subscription failed");
    failed |= check(g_sr_runner_event_mask ==
                        (SR_EVENT_MASK_EXECUTION_BLOCK |
                         SR_EVENT_MASK_DYNAMIC_DISPATCH |
                         SR_EVENT_MASK_MEMORY_WRITE |
                         SR_EVENT_MASK_REGISTER_ACCESS |
                         SR_EVENT_MASK_DMA |
                         SR_EVENT_MASK_AUDIO |
                         SR_EVENT_MASK_FRAME |
                         SR_EVENT_MASK_INTERRUPT |
                         SR_EVENT_MASK_ERROR),
                    "observer union mask mismatch");

    snes->abiFrameCounter = 9u;
    snes_write(snes, 0x000105u, 0x4cu);
    snes_write(snes, 0x000205u, 0x5du);
    failed |= check(memory_observer.count == 1u &&
                        memory_observer.event.type == SR_EVENT_MEMORY_WRITE &&
                        memory_observer.event.frame_counter == 9u &&
                        memory_observer.event.memory_region == SR_MEMORY_WRAM &&
                        memory_observer.event.address == 0x0105u &&
                        memory_observer.event.previous_value == 0u &&
                        memory_observer.event.value == 0x4cu &&
                        memory_observer.event.width_bytes == 1u,
                    "filtered WRAM write event mismatch");

    snes->ppu->vramPointer = 3u;
    snes->ppu->vram[3] = 0x1200u;
    ppu_write(snes->ppu, 0x18u, 0x34u);
    failed |= check(ppu_memory_observer.count == 1u &&
                        ppu_memory_observer.event.type ==
                            SR_EVENT_MEMORY_WRITE &&
                        ppu_memory_observer.event.memory_region ==
                            SR_MEMORY_VRAM &&
                        ppu_memory_observer.event.address == 6u &&
                        ppu_memory_observer.event.previous_value == 0u &&
                        ppu_memory_observer.event.value == 0x34u &&
                        ppu_memory_observer.event.width_bytes == 1u,
                    "filtered VRAM write event mismatch");

    snes_write(snes, 0x002100u, 0x8fu);
    snes_write(snes, 0x004200u, 0x00u);
    failed |= check(register_observer.count == 1u &&
                        register_observer.event.type ==
                            SR_EVENT_REGISTER_WRITE &&
                        register_observer.event.frame_counter == 9u &&
                        register_observer.event.address == 0x2100u &&
                        register_observer.event.value == 0x8fu &&
                        register_observer.event.width_bytes == 1u,
                    "filtered register event mismatch");

    dma_write(snes->dma, 0x4320u, 0xddu);
    dma_write(snes->dma, 0x4321u, 0x18u);
    dma_write(snes->dma, 0x4322u, 0x34u);
    dma_write(snes->dma, 0x4323u, 0x12u);
    dma_write(snes->dma, 0x4324u, 0x7eu);
    dma_write(snes->dma, 0x4325u, 0x00u);
    dma_write(snes->dma, 0x4326u, 0x00u);
    dma_write(snes->dma, 0x4327u, 0x7fu);
    dma_startDma(snes->dma, 0x04u, false);
    failed |= check(dma_observer.count == 1u &&
                        dma_observer.event.type == SR_EVENT_DMA_BEGIN &&
                        dma_observer.event.frame_counter == 9u &&
                        dma_observer.event.flags ==
                            (SR_EVENT_DMA_FROM_B_BUS |
                             SR_EVENT_DMA_FIXED_A_BUS |
                             SR_EVENT_DMA_DECREMENT_A_BUS |
                             SR_EVENT_DMA_INDIRECT) &&
                        dma_observer.event.address == 0x7e1234u &&
                        dma_observer.event.dma_a_address24 == 0x7e1234u &&
                        dma_observer.event.dma_transfer_bytes == 0x10000u &&
                        dma_observer.event.dma_table_address == 0u &&
                        dma_observer.event.dma_channel == 2u &&
                        dma_observer.event.dma_mode == 5u &&
                        dma_observer.event.dma_b_address == 0x18u &&
                        dma_observer.event.dma_indirect_bank == 0x7fu &&
                        strcmp(dma_observer.event.label, "dma") == 0,
                    "general DMA begin event mismatch");

    dma_reset(snes->dma);
    dma_write(snes->dma, 0x4320u, 0x45u);
    dma_write(snes->dma, 0x4321u, 0x19u);
    dma_write(snes->dma, 0x4322u, 0x34u);
    dma_write(snes->dma, 0x4323u, 0x12u);
    dma_write(snes->dma, 0x4324u, 0x7eu);
    dma_write(snes->dma, 0x4327u, 0x7fu);
    dma_startDma(snes->dma, 0x04u, true);
    failed |= check(dma_observer.count == 2u &&
                        dma_observer.event.type == SR_EVENT_DMA_BEGIN &&
                        dma_observer.event.flags ==
                            (SR_EVENT_DMA_HDMA | SR_EVENT_DMA_INDIRECT) &&
                        dma_observer.event.dma_transfer_bytes == 0u &&
                        dma_observer.event.dma_table_address == 0x1234u &&
                        dma_observer.event.dma_b_address == 0x19u &&
                        strcmp(dma_observer.event.label, "hdma") == 0,
                    "HDMA begin event mismatch");
    dma_reset(snes->dma);

    sr_runner_emit_frame_boundary(
        snes, SR_EVENT_FRAME_BEGIN | SR_EVENT_FRAME_VBLANK, "vblank");
    failed |= check(frame_observer.count == 1u &&
                        frame_observer.event.type ==
                            SR_EVENT_FRAME_BOUNDARY &&
                        frame_observer.event.frame_counter == 9u &&
                        frame_observer.event.flags ==
                            (SR_EVENT_FRAME_BEGIN |
                             SR_EVENT_FRAME_VBLANK) &&
                        strcmp(frame_observer.event.label, "vblank") == 0,
                    "frame boundary event mismatch");

    g_snes = snes;
    g_rtl_music_mix_hook = mix_test_audio;
    RtlSetAudioOutputRate(44100);
    frame_observer.expected_audio_samples = audio_samples;
    frame_observer.expected_audio_frame_count = 4u;
    frame_observer.expected_audio_first = 0x1200;
    frame_observer.expected_audio_last = 0x1207;
    RtlRenderAudio(audio_samples, 4, 2);
    failed |= check(frame_observer.count == 2u &&
                        frame_observer.event.type ==
                            SR_EVENT_AUDIO_PRODUCED &&
                        frame_observer.event.frame_counter == 0u &&
                        frame_observer.event.flags ==
                            (SR_EVENT_AUDIO_FINAL_MIX |
                             SR_EVENT_AUDIO_TRANSIENT_SAMPLES) &&
                        frame_observer.event.audio_frame_offset == 0u &&
                        frame_observer.event.audio_frame_count == 4u &&
                        frame_observer.event.audio_sample_rate == 44100u &&
                        frame_observer.event.audio_channel_count == 2u &&
                        frame_observer.event.audio_sample_format ==
                            SR_AUDIO_SAMPLE_FORMAT_S16_NATIVE &&
                        frame_observer.audio_payload_valid &&
                        strcmp(frame_observer.event.label, "final-mix") == 0,
                    "final mixed audio event mismatch");
    frame_observer.expected_audio_frame_count = 2u;
    frame_observer.expected_audio_last = 0x1203;
    RtlRenderAudio(audio_samples, 2, 2);
    failed |= check(frame_observer.count == 3u &&
                        frame_observer.event.audio_frame_offset == 4u &&
                        frame_observer.event.audio_frame_count == 2u &&
                        frame_observer.audio_payload_valid &&
                        snes->abiAudioFrameCounter == 6u,
                    "audio output clock mismatch");
    g_rtl_music_mix_hook = NULL;
    g_snes = NULL;

    sr_runner_emit_interrupt(
        snes, SR_INTERRUPT_NMI, SR_EVENT_INTERRUPT_ENTER, 0x123456u,
        0xffeau, SR_INTERRUPT_SCANLINE_UNKNOWN, "nmi");
    failed |= check(interrupt_error_observer.count == 1u &&
                        interrupt_error_observer.event.type ==
                            SR_EVENT_INTERRUPT &&
                        interrupt_error_observer.event.frame_counter == 9u &&
                        interrupt_error_observer.event.flags ==
                            SR_EVENT_INTERRUPT_ENTER &&
                        interrupt_error_observer.event.pc24 == 0x123456u &&
                        interrupt_error_observer.event.interrupt_kind ==
                            SR_INTERRUPT_NMI &&
                        interrupt_error_observer.event.interrupt_vector ==
                            0xffeau &&
                        interrupt_error_observer.event.interrupt_scanline ==
                            SR_INTERRUPT_SCANLINE_UNKNOWN &&
                        strcmp(interrupt_error_observer.event.label, "nmi") ==
                            0,
                    "interrupt event mismatch");

    sr_runner_emit_error(
        snes, SR_RUNNER_ERROR_DISPATCH_MISS, SR_EVENT_ERROR_RECOVERABLE,
        0x123478u, 0x101000u, "dispatch-miss");
    failed |= check(interrupt_error_observer.count == 2u &&
                        interrupt_error_observer.event.type ==
                            SR_EVENT_ERROR &&
                        interrupt_error_observer.event.flags ==
                            SR_EVENT_ERROR_RECOVERABLE &&
                        interrupt_error_observer.event.pc24 == 0x123478u &&
                        interrupt_error_observer.event.source_pc24 ==
                            0x101000u &&
                        interrupt_error_observer.event.address == 0x123478u &&
                        interrupt_error_observer.event.error_code ==
                            SR_RUNNER_ERROR_DISPATCH_MISS &&
                        strcmp(interrupt_error_observer.event.label,
                               "dispatch-miss") == 0,
                    "error event mismatch");
    sr_runner_emit_error(
        snes, SR_RUNNER_ERROR_UNREACHABLE, 0u, 0x200000u, 0u,
        "filtered-error");
    failed |= check(interrupt_error_observer.count == 2u,
                    "interrupt/error PC filter mismatch");

    runner_event.type = SR_EVENT_EXECUTION_BLOCK;
    runner_event.frame_counter = 7u;
    runner_event.cpu_flags = SR_CPU_STATE_M_FLAG;
    runner_event.pc24 = 0x100123u;
    runner_event.register_x = 0x4567u;
    runner_event.stack_pointer = 0x01f0u;
    runner_event.label = "block";
    sr_runner_emit_event(snes, SR_EVENT_MASK_EXECUTION_BLOCK,
                         &runner_event);
    failed |= check(block_observer.count == 1u &&
                        dispatch_observer.count == 0u &&
                        block_observer.runner == runner &&
                        block_observer.event.struct_size ==
                            SR_RUNNER_EVENT_V2_SIZE &&
                        block_observer.event.serial != 0u &&
                        block_observer.event.frame_counter == 7u &&
                        block_observer.event.pc24 == 0x100123u &&
                        block_observer.event.register_x == 0x4567u &&
                        block_observer.event.stack_pointer == 0x01f0u,
                    "filtered block event mismatch");

    runner_event.type = SR_EVENT_DYNAMIC_DISPATCH;
    runner_event.flags = SR_EVENT_DISPATCH_FOUND |
                         SR_EVENT_DISPATCH_MIRRORED;
    runner_event.pc24 = 0x200123u;
    runner_event.source_pc24 = 0x200100u;
    runner_event.label = "dispatch";
    sr_runner_emit_event(snes, SR_EVENT_MASK_DYNAMIC_DISPATCH,
                         &runner_event);
    failed |= check(block_observer.count == 1u &&
                        dispatch_observer.count == 1u &&
                        dispatch_observer.event.serial >
                            block_observer.event.serial &&
                        dispatch_observer.event.type ==
                            SR_EVENT_DYNAMIC_DISPATCH &&
                        dispatch_observer.event.flags ==
                            (SR_EVENT_DISPATCH_FOUND |
                             SR_EVENT_DISPATCH_MIRRORED) &&
                        dispatch_observer.event.source_pc24 == 0x200100u,
                    "dispatch event/filter mismatch");
    failed |= check(api->unsubscribe_events(runner, subscription_id) ==
                        SR_RESULT_OK &&
                        g_sr_runner_event_mask ==
                            (SR_EVENT_MASK_DYNAMIC_DISPATCH |
                             SR_EVENT_MASK_MEMORY_WRITE |
                             SR_EVENT_MASK_REGISTER_ACCESS |
                             SR_EVENT_MASK_DMA |
                             SR_EVENT_MASK_AUDIO |
                             SR_EVENT_MASK_FRAME |
                             SR_EVENT_MASK_INTERRUPT |
                             SR_EVENT_MASK_ERROR),
                    "first observer unsubscribe failed");
    failed |= check(api->unsubscribe_events(
                        runner, dispatch_subscription_id) == SR_RESULT_OK &&
                        api->unsubscribe_events(
                            runner, memory_subscription_id) == SR_RESULT_OK &&
                        api->unsubscribe_events(
                            runner, ppu_memory_subscription_id) ==
                            SR_RESULT_OK &&
                        api->unsubscribe_events(
                            runner, register_subscription_id) == SR_RESULT_OK &&
                        api->unsubscribe_events(
                            runner, dma_subscription_id) == SR_RESULT_OK &&
                        api->unsubscribe_events(
                            runner, frame_subscription_id) == SR_RESULT_OK &&
                        api->unsubscribe_events(
                            runner, interrupt_error_subscription_id) ==
                            SR_RESULT_OK &&
                        g_sr_runner_event_mask == 0u,
                    "remaining observers unsubscribe failed");
    failed |= check(api->unsubscribe_events(runner, subscription_id) ==
                        SR_RESULT_UNAVAILABLE,
                    "duplicate observer unsubscribe accepted");

    subscription.flags = 0u;
    failed |= check(api->subscribe_events(runner, &subscription,
                                           &subscription_id) ==
                        SR_RESULT_OK,
                    "observer resubscribe failed");
    sr_runner_clear_event_subscriptions(snes);
    failed |= check(g_sr_runner_event_mask == 0u &&
                        api->unsubscribe_events(runner, subscription_id) ==
                            SR_RESULT_UNAVAILABLE,
                    "runner observer cleanup failed");

    sr_runner_note_tick(snes);
    failed |= check_generation(api, runner, 1u, 1u, 0u, 0u, 0u);
    failed |= check(!api->borrow_is_valid(runner, &wram_span),
                    "tick did not expire WRAM span");
    failed |= check(!api->borrow_u16_is_valid(runner, &vram_span),
                    "tick did not expire VRAM span");
    failed |= check(!api->ppu_surface_snapshot_is_valid(
                        runner, &rebound_surfaces),
                    "tick did not expire PPU surface snapshot");
    obj_result.struct_size = sizeof(obj_result);
    failed |= check(api->rasterize_ppu_obj_range(
                        runner, &obj_request, &obj_result) ==
                        SR_RESULT_STALE_VIEW,
                    "stale PPU OBJ raster request accepted");

    wram_span.struct_size = sizeof(wram_span);
    failed |= check(api->borrow_memory(runner, SR_MEMORY_WRAM, &wram_span) ==
                        SR_RESULT_OK && api->borrow_is_valid(runner, &wram_span),
                    "WRAM reborrow failed");
    snes_reset(snes, false);
    failed |= check_generation(api, runner, 2u, 1u, 1u, 0u, 0u);
    failed |= check(snes->abiAudioFrameCounter == 0u,
                    "reset did not restart audio output clock");
    failed |= check(!api->borrow_is_valid(runner, &wram_span),
                    "reset did not expire WRAM span");

    snes_saveload(snes, &load);
    failed |= check_generation(api, runner, 3u, 1u, 1u, 1u, 0u);
    sr_runner_note_mutation(snes);
    failed |= check_generation(api, runner, 4u, 1u, 1u, 1u, 1u);

    memory_observer = (TestObserver){0};
    failed |= check(api->subscribe_events(runner, &memory_subscription,
                                           &memory_subscription_id) ==
                        SR_RESULT_OK,
                    "mutation observer subscription failed");
    failed |= check(api->queue_mutation(runner, &memory_mutation,
                                         &memory_mutation_id) ==
                        SR_RESULT_OK && memory_mutation_id != 0u &&
                        wram[0x0106u] == 0u,
                    "memory mutation queue failed");
    failed |= check(api->queue_mutation(runner, &input_mutation,
                                         &input_mutation_id) ==
                        SR_RESULT_OK && input_mutation_id != 0u &&
                        input_mutation_id != memory_mutation_id,
                    "input mutation queue failed");
    failed |= check(api->queue_mutation(runner, &vram_mutation,
                                         &vram_mutation_id) ==
                        SR_RESULT_OK && vram_mutation_id != 0u &&
                        vram_mutation_id != input_mutation_id &&
                        snes->ppu->vram[0x2000u] == 0u &&
                        snes->ppu->vram[0x2001u] == 0u,
                    "VRAM mutation queue failed");
    failed |= check(api->query_mutation(
                        runner, memory_mutation_id, 0u,
                        &mutation_status) == SR_RESULT_OK &&
                        mutation_status.struct_size ==
                            SR_MUTATION_STATUS_V2_SIZE &&
                        mutation_status.state == SR_MUTATION_STATE_QUEUED &&
                        mutation_status.result == SR_RESULT_PENDING &&
                        mutation_status.command_id == memory_mutation_id &&
                        mutation_status.applied_frame_counter == 0u,
                    "queued mutation status mismatch");

    g_snes = snes;
    snes_frame_counter = 17;
    RtlRunFrame(0u);
    g_snes = NULL;
    failed |= check(wram[0x0106u] == 0xa1u &&
                        wram[0x0107u] == 0xb2u &&
                        wram[0x0108u] == 0xc3u &&
                        snes->ppu->vram[0x2000u] == 0x1100u &&
                        snes->ppu->vram[0x2001u] == 0x3322u &&
                        snes->input1_currentState == 0x0001u &&
                        snes->input2_currentState == 0u,
                    "safe-point mutation application mismatch");
    failed |= check(memory_observer.count == 3u &&
                        memory_observer.event.type ==
                            SR_EVENT_MEMORY_WRITE &&
                        memory_observer.event.frame_counter == 17u &&
                        memory_observer.event.memory_region == SR_MEMORY_WRAM &&
                        memory_observer.event.address == 0x0108u &&
                        memory_observer.event.previous_value == 0u &&
                        memory_observer.event.value == 0xc3u,
                    "safe-point memory observation mismatch");
    failed |= check_generation(api, runner, 8u, 2u, 1u, 1u, 4u);
    failed |= check(api->query_mutation(
                        runner, memory_mutation_id,
                        SR_MUTATION_QUERY_CONSUME,
                        &mutation_status) == SR_RESULT_OK &&
                        mutation_status.state == SR_MUTATION_STATE_APPLIED &&
                        mutation_status.result == SR_RESULT_OK &&
                        mutation_status.applied_frame_counter == 17u,
                    "applied memory mutation status mismatch");
    failed |= check(api->query_mutation(
                        runner, memory_mutation_id, 0u,
                        &mutation_status) == SR_RESULT_UNAVAILABLE,
                    "consumed mutation status remained available");
    failed |= check(api->query_mutation(
                        runner, input_mutation_id,
                        SR_MUTATION_QUERY_CONSUME,
                        &mutation_status) == SR_RESULT_OK &&
                        mutation_status.state == SR_MUTATION_STATE_APPLIED &&
                        mutation_status.result == SR_RESULT_OK &&
                        mutation_status.applied_frame_counter == 17u,
                    "applied input mutation status mismatch");
    failed |= check(api->query_mutation(
                        runner, vram_mutation_id,
                        SR_MUTATION_QUERY_CONSUME,
                        &mutation_status) == SR_RESULT_OK &&
                        mutation_status.state == SR_MUTATION_STATE_APPLIED &&
                        mutation_status.result == SR_RESULT_OK &&
                        mutation_status.applied_frame_counter == 17u,
                    "applied VRAM mutation status mismatch");
    failed |= check(api->unsubscribe_events(
                        runner, memory_subscription_id) == SR_RESULT_OK &&
                        g_sr_runner_event_mask == 0u,
                    "mutation observer cleanup failed");

    snes->multiplyA = 0x12u;
    snes->multiplyResult = 0x3456u;
    snes->divideA = 0x789au;
    snes->divideResult = 0xbcdeu;
    failed |= check(api->query_cpu_math_state(runner, &math_state) ==
                            SR_RESULT_OK &&
                        math_state.struct_size == SR_CPU_MATH_STATE_V2_SIZE &&
                        math_state.lifetime_generation ==
                            snes->abiLifetimeGeneration &&
                        math_state.multiply_operand == 0x12u &&
                        math_state.multiply_or_remainder_result == 0x3456u &&
                        math_state.divide_dividend == 0x789au &&
                        math_state.divide_quotient == 0xbcdeu,
                    "CPU math state query mismatch");
    failed |= check(api->query_cpu_math_state(runner, &small_math_state) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized CPU math state output accepted");
    snes->multiplyA = 1u;
    snes->multiplyResult = 2u;
    snes->divideA = 3u;
    snes->divideResult = 4u;
    failed |= check(api->restore_cpu_math_state(runner, &math_state) ==
                            SR_RESULT_OK &&
                        snes->multiplyA == 0x12u &&
                        snes->multiplyResult == 0x3456u &&
                        snes->divideA == 0x789au &&
                        snes->divideResult == 0xbcdeu,
                    "CPU math state restore mismatch");
    failed |= check_generation(api, runner, 9u, 2u, 1u, 1u, 5u);
    failed |= check(api->restore_cpu_math_state(runner, &math_state) ==
                        SR_RESULT_STALE_VIEW,
                    "stale CPU math state restore accepted");

    sr_runner_set_cpu_state_provider(snes, NULL, NULL);
    sr_runner_set_execution_state_provider(snes, NULL);
    sr_runner_bind_ppu_services(snes, false);
    snes_free(snes);
    memset(wram, 0, sizeof(wram));
    return failed;
}
