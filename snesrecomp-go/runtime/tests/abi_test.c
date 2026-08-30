#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/generated_support.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/runner.h"
#include "runner_internal.h"
#include "runner_game_module_internal.h"
#include "runner_state_internal.h"
#include "snesrecomp/game/cpu.h"
#include "snes/dma.h"
#include "snes/dsp.h"
#include "snes/ppu.h"
#include "snes/saveload.h"
#include "snes/snes.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { TEST_WRAM_SIZE = 128 * 1024 };

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static void leave_loaded_state_unchanged(SaveLoadInfo *info, void *data,
                                         size_t size) {
    (void)info;
    (void)data;
    (void)size;
}

static uint8_t s_cpu_component;

/* The contract archive deliberately leaves the generated game's dispatch
 * table unresolved. This ABI-only fixture exercises no generated blocks but
 * supplies the required linked-game seam. */
const DispatchEntry g_dispatch_table[] = {{0}};
const unsigned g_dispatch_table_count = 0u;
static uint8_t s_overlay_pixel;
static uint32_t s_mode7_pixel;
static uint32_t s_main_surface[384u * 253u];
static uint32_t s_authentic_surface[384u * 253u];
static uint32_t s_overlay_surface[384u * 232u];
static uint32_t s_overlay_band_surface[384u * 232u];
static uint32_t s_mode7_surface[768u * 506u];
static uint16_t s_virtual_span_entries[4];
static uint8_t s_virtual_context;
static uint8_t s_apu_snapshot_ram[SR_APU_RAM_BYTE_COUNT];
static uint8_t s_dsp_snapshot_registers[SR_DSP_REGISTER_BYTE_COUNT];
static int16_t s_discard_audio[
    RTL_APU_TIMELINE_FRAMES_PER_TICK * 2u];

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

typedef struct TestPhaseObserver {
    unsigned count;
    SrRunnerEvent events[16];
} TestPhaseObserver;

typedef struct TestAudioTraceObserver {
    unsigned count;
    SrRunnerHandle *runner;
    SrAudioTraceEvent events[6];
    uint8_t driver_bytes[6][3];
} TestAudioTraceObserver;

typedef struct TestAudioSnapshotReentryObserver {
    const SnesRunnerApi *api;
    SrApuStateQuery *query;
    SrApuStateSnapshot *state;
    SrResult result;
    unsigned count;
} TestAudioSnapshotReentryObserver;

typedef struct TestPpuFrameTransactionObserver {
    unsigned count;
    int valid;
    SrRunnerHandle *runner;
} TestPpuFrameTransactionObserver;

typedef struct TestPpuScanoutObserver {
    Snes *snes;
    unsigned before_count;
    unsigned after_count;
    unsigned irq_count;
    uint32_t irq_line;
    int valid;
    int saw_hdma_before;
    int saw_hdma_after;
    int saw_indirect_before;
    int saw_indirect_after;
} TestPpuScanoutObserver;

typedef struct TestChainedIrqObserver {
    Snes *snes;
    unsigned count;
    uint32_t lines[4];
} TestChainedIrqObserver;

static uint32_t test_virtual_tile_lookup(
        void *user_data, int32_t tile_x, int32_t tile_y,
        uint16_t *entry) {
    if (user_data != &s_virtual_context || entry == NULL) return 0u;
    if (tile_x == -1) return SR_PPU_VIRTUAL_TILE_FALLBACK_AUTHENTIC;
    if (tile_x == -2) return UINT32_MAX;
    *entry = (uint16_t)((tile_x & 0xff) | ((tile_y & 0xff) << 8));
    return SR_PPU_VIRTUAL_TILE_FOUND;
}

static uint32_t test_virtual_tile_span_lookup(
        void *user_data, int32_t tile_x, int32_t tile_y,
        int32_t tile_step, uint32_t capacity, const uint16_t **entries,
        int64_t *word_stride) {
    uint32_t count = capacity < 4u ? capacity : 4u;
    uint32_t index;
    if (user_data != &s_virtual_context || entries == NULL ||
        word_stride == NULL || tile_step == 0)
        return 0u;
    for (index = 0u; index < count; ++index)
        s_virtual_span_entries[index] = (uint16_t)(
            ((tile_x + (int32_t)index * tile_step) & 0xff) |
            ((tile_y & 0xff) << 8));
    *entries = s_virtual_span_entries;
    *word_stride = 1;
    return count;
}

static uint32_t test_virtual_tile_band_lookup(
        void *user_data, int32_t tile_x, int32_t tile_y,
        uint16_t entry, uint8_t *band) {
    (void)tile_x;
    (void)tile_y;
    if (user_data != &s_virtual_context || band == NULL) return 0u;
    *band = (uint8_t)(entry & 3u);
    return 1u;
}

static SrResult query_test_cpu_state(
        void *user_data, SrCpuStateSnapshot *out_state) {
    if (user_data != &s_cpu_component) return SR_RESULT_INVALID_ARGUMENT;
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
        void *user_data, SrExecutionSnapshot *out_state) {
    if (user_data != &s_cpu_component) return SR_RESULT_INVALID_ARGUMENT;
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

static void observe_test_phase_event(
        void *user_data, SrRunnerHandle *runner,
        const SrRunnerEvent *event) {
    TestPhaseObserver *observer = (TestPhaseObserver *)user_data;
    (void)runner;
    if (observer == NULL || event == NULL || observer->count >= 16u) return;
    observer->events[observer->count++] = *event;
}

static void observe_test_audio_trace(void *user_data,
                                     SrRunnerHandle *runner,
                                     const SrAudioTraceEvent *event) {
    TestAudioTraceObserver *observer = (TestAudioTraceObserver *)user_data;
    unsigned index = observer->count++;
    if (index >= 6u) return;
    observer->runner = runner;
    observer->events[index] = *event;
    if (event->apu_ram != NULL &&
        event->apu_ram_byte_size >= SR_APU_RAM_BYTE_COUNT) {
        observer->driver_bytes[index][0] = event->apu_ram[0x1a];
        observer->driver_bytes[index][1] = event->apu_ram[0x35];
        observer->driver_bytes[index][2] = event->apu_ram[0x47];
    }
    observer->events[index].apu_ram = NULL;
}

static void observe_test_audio_snapshot_reentry(
        void *user_data, SrRunnerHandle *runner,
        const SrAudioTraceEvent *event) {
    TestAudioSnapshotReentryObserver *observer =
        (TestAudioSnapshotReentryObserver *)user_data;
    (void)event;
    ++observer->count;
    observer->result = observer->api->query_apu_state(
        runner, observer->query, observer->state);
}

static SrResult observe_test_ppu_frame_transaction(
        void *user_data, SrRunnerHandle *runner,
        const SrPpuFrameTransactionContext *context) {
    TestPpuFrameTransactionObserver *observer =
        (TestPpuFrameTransactionObserver *)user_data;
    ++observer->count;
    observer->runner = runner;
    observer->valid = context != NULL &&
        context->struct_size == SR_PPU_FRAME_TRANSACTION_CONTEXT_V2_SIZE &&
        context->state.struct_size == SR_PPU_STATE_SNAPSHOT_V2_SIZE &&
        context->frame.struct_size == SR_PPU_FRAME_SNAPSHOT_V2_SIZE &&
        context->state.fixed_color == UINT16_C(0x3456) &&
        context->frame.overlays[0].transparent_fill_mode ==
            SR_PPU_TRANSPARENT_FILL_CGRAM &&
        context->frame.overlays[0].transparent_fill_cgram == 8u &&
        context->vram.data != NULL &&
        context->vram.element_count == SR_PPU_VRAM_WORD_COUNT &&
        context->vram.data[7] == UINT16_C(0x4567) &&
        context->cgram.data != NULL &&
        context->cgram.element_count == SR_PPU_CGRAM_WORD_COUNT &&
        context->oam.data != NULL &&
        context->oam.element_count == SR_PPU_OAM_WORD_COUNT &&
        context->high_oam.data != NULL &&
        context->high_oam.byte_size == SR_PPU_HIGH_OAM_BYTE_COUNT &&
        context->main.data == (uint8_t *)s_main_surface &&
        context->main.pitch_bytes == 384u * sizeof(uint32_t) &&
        context->main.height_pixels == 253u &&
        context->authentic.data == (uint8_t *)s_authentic_surface &&
        context->authentic.pitch_bytes == 384u * sizeof(uint32_t) &&
        context->authentic.height_pixels == 253u &&
        context->overlays[0].data == (uint8_t *)s_overlay_surface &&
        context->overlays[0].pitch_bytes == 384u * sizeof(uint32_t) &&
        context->overlays[0].height_pixels == 232u;
    return SR_RESULT_OK;
}

static void observe_test_ppu_scanout_line(
        void *user_data, const SrPpuScanoutLineContext *context) {
    TestPpuScanoutObserver *observer =
        (TestPpuScanoutObserver *)user_data;
    if (observer == NULL || context == NULL) return;
    observer->valid &=
        context->struct_size == SR_PPU_SCANOUT_LINE_CONTEXT_V2_SIZE &&
        context->lifetime_generation ==
            observer->snes->abiLifetimeGeneration &&
        context->line <= SR_PPU_NATIVE_HEIGHT &&
        context->channel_count == SR_DMA_CHANNEL_COUNT &&
        context->state.struct_size == SR_PPU_STATE_SNAPSHOT_V2_SIZE &&
        context->main_surface.data == (const uint8_t *)s_main_surface &&
        context->authentic_surface.data ==
            (const uint8_t *)s_authentic_surface;
    if ((context->flags & SR_PPU_SCANOUT_LINE_BEFORE) != 0u) {
        ++observer->before_count;
        if (context->line == 0u &&
            (context->channels[2].flags &
             SR_PPU_SCANOUT_HDMA_ACTIVE) != 0u &&
            context->channels[2].mode == 0u &&
            context->channels[2].b_address == 0u)
            observer->saw_hdma_before = 1;
        if (context->line == 0u &&
            context->channels[3].flags ==
                (SR_PPU_SCANOUT_HDMA_ACTIVE |
                 SR_PPU_SCANOUT_HDMA_INDIRECT) &&
            context->channels[3].indirect_bank == 0x7eu)
            observer->saw_indirect_before = 1;
    } else if ((context->flags &
                SR_PPU_SCANOUT_LINE_AFTER_HDMA) != 0u) {
        ++observer->after_count;
        if (context->line == 0u &&
            context->state.display_control == 0x8fu &&
            context->channels[2].repeat_count == 0u)
            observer->saw_hdma_after = 1;
        if (context->line == 0u &&
            context->state.bg_mode_control == 0x09u &&
            context->channels[3].repeat_count == 0u)
            observer->saw_indirect_after = 1;
    } else {
        observer->valid = 0;
    }
}

static void observe_test_ppu_scanout_irq(
        void *user_data, uint32_t line) {
    TestPpuScanoutObserver *observer =
        (TestPpuScanoutObserver *)user_data;
    if (observer == NULL || observer->snes == NULL) return;
    ++observer->irq_count;
    observer->irq_line = line;
    observer->valid &= observer->snes->inIrq;
    observer->snes->inIrq = false;
    observer->snes->vIrqEnabled = false;
}

static void observe_test_chained_ppu_irq(
        void *user_data, uint32_t line) {
    static const uint16_t kNextTimers[] = {7u, 12u, 20u};
    TestChainedIrqObserver *observer =
        (TestChainedIrqObserver *)user_data;
    if (observer == NULL || observer->snes == NULL ||
        observer->count >= 4u)
        return;
    observer->lines[observer->count++] = line;
    observer->snes->inIrq = false;
    if (observer->count < 4u) {
        observer->snes->vTimer = kNextTimers[observer->count - 1u];
        observer->snes->vIrqEnabled = true;
    } else {
        observer->snes->vIrqEnabled = false;
    }
}

static void mix_test_audio(int16 *buffer, int frames) {
    int sample;
    for (sample = 0; sample < frames * 2; ++sample)
        buffer[sample] = (int16)(0x1200 + sample);
}

static const RtlGameAudioApi s_mix_test_audio_api = {
    .struct_size = RTL_GAME_AUDIO_API_V2_SIZE,
    .capabilities = RTL_GAME_AUDIO_CAP_PRESENTATION,
    .mix_output = mix_test_audio,
};

static int check(int condition, const char *message) {
    if (condition) return 0;
    fprintf(stderr, "runtime ABI failed: %s\n", message);
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

static int test_public_vertical_margin_scanout(
        const SnesRunnerApi *api, SrRunnerHandle *runner, Snes *snes) {
    enum {
        kPitchPixels = 384,
        kCapacityRows = 253,
        kTop = 8,
        kBottom = 8,
        kRenderedRows = SR_PPU_NATIVE_HEIGHT + kTop + kBottom,
    };
    const uint32_t sentinel = UINT32_C(0x005aa55a);
    SrGenerationSnapshot generation = {
        .struct_size = sizeof(generation),
    };
    SrPpuFrameResetRequest reset = {
        .struct_size = sizeof(reset),
    };
    SrPpuFramePolicyRequest policy = {
        .struct_size = sizeof(policy),
        .policy = {
            .struct_size = sizeof(policy.policy),
            .horizontal_mode = SR_PPU_HORIZONTAL_MARGIN_AVAILABLE,
            .margin_top_pixels = kTop,
            .margin_bottom_pixels = kBottom,
        },
    };
    SrPpuOutputBindingRequest binding = {
        .struct_size = sizeof(binding),
        .kind = SR_PPU_OUTPUT_MAIN,
        .pixels = (uint8_t *)s_main_surface,
        .pixel_byte_size = sizeof(s_main_surface),
        .pitch_bytes = kPitchPixels * sizeof(uint32_t),
        .height_pixels = kCapacityRows,
    };
    SrPpuScanoutRequest scanout = {
        .struct_size = sizeof(scanout),
        .irq_callback = observe_test_ppu_scanout_irq,
    };
    SrPpuScanoutResult result = {
        .struct_size = sizeof(result),
    };
    SrPpuSurfaceSnapshot surfaces = {
        .struct_size = sizeof(surfaces),
    };
    uint32_t first_top;
    uint32_t first_native;
    uint32_t first_bottom;
    int failed = 0;
    unsigned row;
    unsigned column;

    ppu_reset(snes->ppu);
    dma_reset(snes->dma);
    snes_writeReg(snes, 0x420cu, 0u);
    snes->vIrqEnabled = false;
    snes->inIrq = false;
    snes->ppu->inidisp = 0x0fu;
    snes->ppu->bgmode = 1u;
    snes->ppu->screenEnabled[0] = 1u;
    snes->ppu->screenEnabled[1] = 0u;
    snes->ppu->bgTileAdr = 0u;
    snes->ppu->bgXsc[0] = 0x20u;
    snes->ppu->hScroll[0] = 0u;
    snes->ppu->cgram[0x11] = 0x001fu; /* red */
    snes->ppu->cgram[0x21] = 0x7c00u; /* blue */
    snes->ppu->cgram[0x31] = 0x03e0u; /* green */
    snes->ppu->cgram[0x41] = 0x03ffu; /* yellow */
    snes->ppu->cgram[0x51] = 0x7c1fu; /* magenta */
    for (unsigned tile = 1u; tile <= 5u; ++tile)
        set_solid_4bpp_tile(snes->ppu, tile, 1u);
    for (row = 0u; row < 32u; ++row) {
        unsigned tile = 1u;
        unsigned palette = 1u;
        if (row == 1u) tile = palette = 2u;
        else if (row == 2u) tile = palette = 3u;
        else if (row == 29u) tile = palette = 4u;
        else if (row == 30u) tile = palette = 5u;
        for (column = 0u; column < 32u; ++column)
            snes->ppu->vram[0x2000u + row * 32u + column] =
                (uint16_t)(tile | (palette << 10));
    }

    failed |= check(api->query_generations(runner, &generation) == SR_RESULT_OK,
                    "vertical-margin generation query failed");
    reset.lifetime_generation = generation.lifetime_generation;
    policy.lifetime_generation = generation.lifetime_generation;
    binding.lifetime_generation = generation.lifetime_generation;
    scanout.lifetime_generation = generation.lifetime_generation;
    failed |= check(api->reset_ppu_frame_state(runner, &reset) == SR_RESULT_OK,
                    "vertical-margin frame reset failed");
    failed |= check(api->bind_ppu_output_surface(runner, &binding) == SR_RESULT_OK,
                    "vertical-margin main surface bind failed");
    failed |= check(api->apply_ppu_frame_policy(runner, &policy) == SR_RESULT_OK,
                    "vertical-margin frame policy failed");
    surfaces.struct_size = sizeof(surfaces);
    failed |= check(api->query_ppu_surfaces(runner, &surfaces) == SR_RESULT_OK &&
                        surfaces.main.origin_x == 64 &&
                        surfaces.main.origin_y == kTop &&
                        surfaces.main.height_pixels == kRenderedRows,
                    "vertical-margin public surface geometry mismatch");

    for (row = 0u; row < kPitchPixels * kCapacityRows; ++row)
        s_main_surface[row] = sentinel;
    snes->ppu->vScroll[0] = 8u;
    result.struct_size = sizeof(result);
    failed |= check(api->run_ppu_scanout(runner, &scanout, &result) == SR_RESULT_OK,
                    "vertical-margin first public scanout failed");
    for (row = 0u; row < kRenderedRows; ++row) {
        for (column = 0u; column < kPitchPixels; ++column) {
            if (s_main_surface[row * kPitchPixels + column] == sentinel) {
                failed |= check(0, "vertical-margin scanout left a sentinel pixel");
                row = kRenderedRows;
                break;
            }
        }
    }
    for (row = kRenderedRows; row < kCapacityRows; ++row) {
        for (column = 0u; column < kPitchPixels; ++column)
            failed |= check(
                s_main_surface[row * kPitchPixels + column] == sentinel,
                "vertical-margin scanout exceeded rendered height");
    }
    first_top = s_main_surface[64u];
    first_native = s_main_surface[kTop * kPitchPixels + 64u];
    first_bottom =
        s_main_surface[(kTop + SR_PPU_NATIVE_HEIGHT) * kPitchPixels + 64u];
    failed |= check(first_top == UINT32_C(0x00ff0000) &&
                        first_native == UINT32_C(0x000000ff) &&
                        first_bottom == UINT32_C(0x00ffff00),
                    "vertical-margin first-frame content mismatch");

    for (row = 0u; row < kPitchPixels * kCapacityRows; ++row)
        s_main_surface[row] = sentinel;
    failed |= check(api->reset_ppu_frame_state(runner, &reset) == SR_RESULT_OK &&
                        api->apply_ppu_frame_policy(runner, &policy) == SR_RESULT_OK,
                    "vertical-margin moving-camera policy failed");
    snes->ppu->vScroll[0] = 16u;
    result.struct_size = sizeof(result);
    failed |= check(api->run_ppu_scanout(runner, &scanout, &result) == SR_RESULT_OK,
                    "vertical-margin moving-camera scanout failed");
    failed |= check(s_main_surface[64u] == UINT32_C(0x000000ff) &&
                        s_main_surface[kTop * kPitchPixels + 64u] ==
                            UINT32_C(0x0000ff00) &&
                        s_main_surface[(kTop + SR_PPU_NATIVE_HEIGHT) *
                                           kPitchPixels + 64u] ==
                            UINT32_C(0x00ff00ff) &&
                        s_main_surface[64u] != first_top &&
                        s_main_surface[kTop * kPitchPixels + 64u] != first_native &&
                        s_main_surface[(kTop + SR_PPU_NATIVE_HEIGHT) *
                                           kPitchPixels + 64u] != first_bottom,
                    "vertical-margin rows did not follow the moving camera");
    return failed;
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
    uint64_t audio_trace_subscription_id = 0u;
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
    SrPpuFramePolicyBand frame_policy_bands[2] = {0};
    SrPpuFramePolicyRequest frame_policy_request = {
        .struct_size = sizeof(frame_policy_request),
        .policy = {
            .struct_size = sizeof(SrPpuFramePolicy),
            .horizontal_mode = SR_PPU_HORIZONTAL_MARGIN_AVAILABLE,
            .bands = frame_policy_bands,
            .band_count = 2u,
        },
    };
    SrPpuOverlayCaptureRequest capture_request = {
        .struct_size = sizeof(capture_request),
    };
    SrPpuOverlayCaptureExchangeRequest capture_exchange = {
        .struct_size = sizeof(capture_exchange),
    };
    SrPpuVramWordPatch vram_patches[2] = {0};
    SrPpuVramPatchRequest vram_patch_request = {
        .struct_size = sizeof(vram_patch_request),
        .patches = vram_patches,
        .patch_count = 2u,
    };
    SrPpuObjPositionUpdate obj_position_updates[2] = {0};
    SrPpuObjMetadataRequest obj_metadata_request = {
        .struct_size = sizeof(obj_metadata_request),
        .updates = obj_position_updates,
        .update_count = 2u,
    };
    SrPpuObjMetadataRequest small_obj_metadata_request = {
        .struct_size = sizeof(uint32_t),
        .flags = SR_PPU_OBJ_METADATA_CLEAR_POSITIONS,
    };
    SrDmaStateSnapshot dma_state = {
        .struct_size = sizeof(dma_state),
    };
    SrDmaStateSnapshot small_dma_state = {
        .struct_size = sizeof(uint32_t),
    };
    SrPpuLayerExtentUpdate layer_extent_updates[2] = {0};
    SrPpuLayerExtentRequest layer_extent_request = {
        .struct_size = sizeof(layer_extent_request),
        .updates = layer_extent_updates,
        .update_count = 2u,
    };
    SrPpuVirtualTilemapRequest virtual_tilemap_request = {
        .struct_size = sizeof(virtual_tilemap_request),
    };
    uint16_t authentic_bg1[SR_PPU_NATIVE_HEIGHT] = {0};
    uint16_t authentic_bg2[SR_PPU_NATIVE_HEIGHT] = {0};
    SrPpuAuthenticCameraRequest authentic_camera_request = {
        .struct_size = sizeof(authentic_camera_request),
    };
    TestPpuScanoutObserver scanout_observer = {
        .valid = 1,
    };
    SrPpuScanoutRequest scanout_request = {
        .struct_size = sizeof(scanout_request),
        .hdma_suppress_mask = 0u,
        .line_callback = observe_test_ppu_scanout_line,
        .irq_callback = observe_test_ppu_scanout_irq,
        .user_data = &scanout_observer,
    };
    SrPpuScanoutRequest small_scanout_request = {
        .struct_size = sizeof(uint32_t),
        .irq_callback = observe_test_ppu_scanout_irq,
    };
    SrPpuScanoutResult scanout_result = {
        .struct_size = sizeof(scanout_result),
    };
    SrPpuScanoutResult small_scanout_result = {
        .struct_size = sizeof(uint32_t),
    };
    SrGenerationSnapshot ppu_generation = {
        .struct_size = sizeof(ppu_generation),
    };
    TestPpuFrameTransactionObserver ppu_transaction_observer = {0};
    SrPpuFrameTransactionRequest ppu_transaction_request = {
        .struct_size = sizeof(ppu_transaction_request),
        .callback = observe_test_ppu_frame_transaction,
        .user_data = &ppu_transaction_observer,
    };
    SrPpuFrameTransactionRequest small_ppu_transaction_request = {
        .struct_size = sizeof(uint32_t),
        .callback = observe_test_ppu_frame_transaction,
        .user_data = &ppu_transaction_observer,
    };
    SrPpuFrameResetRequest ppu_frame_reset_request = {
        .struct_size = sizeof(ppu_frame_reset_request),
    };
    SrPpuFrameResetRequest small_ppu_frame_reset_request = {
        .struct_size = sizeof(uint32_t),
    };
    SrPpuObjCaptureRequest obj_capture_request = {
        .struct_size = sizeof(obj_capture_request),
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
    TestAudioTraceObserver audio_trace_observer = {0};
    SrAudioTraceSubscription audio_trace_subscription = {
        .struct_size = sizeof(audio_trace_subscription),
        .callback = observe_test_audio_trace,
        .user_data = &audio_trace_observer,
        .event_mask = SR_AUDIO_TRACE_MASK_ALL,
    };
    SrAudioTraceSubscription small_audio_trace_subscription = {
        .struct_size = sizeof(uint32_t),
        .callback = observe_test_audio_trace,
        .user_data = &audio_trace_observer,
    };
    SrAudioTraceSubscription filtered_audio_trace_subscription = {
        .struct_size = sizeof(filtered_audio_trace_subscription),
        .callback = observe_test_audio_trace,
        .user_data = &audio_trace_observer,
        .event_mask = SR_AUDIO_TRACE_MASK_DSP_WRITE,
    };
    SrAudioTraceSubscription zero_mask_audio_trace_subscription = {
        .struct_size = sizeof(zero_mask_audio_trace_subscription),
        .callback = observe_test_audio_trace,
        .user_data = &audio_trace_observer,
    };
    SrAudioTraceSubscription key_on_audio_trace_subscription = {
        .struct_size = sizeof(key_on_audio_trace_subscription),
        .callback = observe_test_audio_trace,
        .user_data = &audio_trace_observer,
        .event_mask = SR_AUDIO_TRACE_MASK_DSP_KEY_ON,
    };
    SrApuStateQuery apu_state_query = {
        .struct_size = sizeof(apu_state_query),
        .apu_ram = s_apu_snapshot_ram,
        .apu_ram_capacity = sizeof(s_apu_snapshot_ram),
        .dsp_registers = s_dsp_snapshot_registers,
        .dsp_register_capacity = sizeof(s_dsp_snapshot_registers),
    };
    SrApuStateQuery small_apu_state_query = {
        .struct_size = sizeof(uint32_t),
    };
    SrApuStateSnapshot apu_state = {
        .struct_size = sizeof(apu_state),
    };
    SrApuStateSnapshot small_apu_state = {
        .struct_size = sizeof(uint32_t),
    };
    RtlApuProfile apu_profile = {
        .struct_size = RTL_APU_PROFILE_V2_SIZE,
    };
    TestAudioSnapshotReentryObserver snapshot_reentry_observer = {
        .api = api,
        .query = &apu_state_query,
        .state = &apu_state,
        .result = SR_RESULT_OK,
    };
    SrAudioTraceSubscription snapshot_reentry_subscription = {
        .struct_size = sizeof(snapshot_reentry_subscription),
        .callback = observe_test_audio_snapshot_reentry,
        .user_data = &snapshot_reentry_observer,
        .event_mask = SR_AUDIO_TRACE_MASK_DSP_WRITE,
    };
    SrSpcPcControlRequest spc_control_request = {
        .struct_size = sizeof(spc_control_request),
        .expected_pc_low = 0x0f0eu,
        .expected_pc_high = 0x0f18u,
        .replacement_pc = 0x0f48u,
        .expected_aram_address = 0x0f48u,
        .expected_aram_count = 5u,
        .expected_aram = {0xcdu, 0x31u, 0xd8u, 0xf1u, 0x6fu},
    };
    SrSpcPcControlRequest small_spc_control_request = {
        .struct_size = sizeof(uint32_t),
    };
    SrSpcPcControlResult spc_control_result = {
        .struct_size = sizeof(spc_control_result),
    };
    SrSpcPcControlResult small_spc_control_result = {
        .struct_size = sizeof(uint32_t),
    };
    SrAudioMixControl audio_mix_control = {
        .struct_size = sizeof(audio_mix_control),
        .music_gain_percent = 65u,
        .sfx_gain_percent = 35u,
    };
    SrAudioMixControl small_audio_mix_control = {
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
    SrPpuObjPart obj_parts[1];
    SrPpuObjResolveRequest obj_resolve_request = {
        .struct_size = sizeof(obj_resolve_request),
        .first_sprite = 0u,
        .sprite_count = 1u,
        .priority = 2u,
        .part_capacity = 1u,
        .parts = obj_parts,
    };
    SrPpuObjResolveResult obj_resolve_result = {
        .struct_size = sizeof(obj_resolve_result),
    };
    SrPpuObjResolveResult small_obj_resolve_result = {
        .struct_size = sizeof(uint32_t),
    };
    SrPpuObjPartsRasterRequest obj_parts_request = {
        .struct_size = sizeof(obj_parts_request),
        .parts = obj_parts,
        .part_count = 1u,
        .pixel_format = SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32,
        .pixels = obj_pixels,
        .pixel_byte_size = sizeof(obj_pixels),
        .pitch_bytes = 8u * sizeof(uint32_t),
    };
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
    failed |= check(SR_PPU_OBJ_RESOLVE_REQUEST_V2_SIZE <=
                            sizeof(SrPpuObjResolveRequest) &&
                        SR_PPU_OBJ_RESOLVE_RESULT_V2_SIZE <=
                            sizeof(SrPpuObjResolveResult) &&
                        SR_PPU_OBJ_PARTS_RASTER_REQUEST_V2_SIZE <=
                            sizeof(SrPpuObjPartsRasterRequest) &&
                        SNES_RUNNER_API_PPU_OBJ_PARTS_SIZE <=
                            sizeof(SnesRunnerApi),
                    "PPU OBJ parts API extent exceeds structure");
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
    failed |= check(SR_PPU_OVERLAY_CAPTURE_EXCHANGE_REQUEST_V2_SIZE <=
                            sizeof(SrPpuOverlayCaptureExchangeRequest) &&
                        SR_PPU_FRAME_TRANSACTION_CONTEXT_V2_SIZE <=
                            sizeof(SrPpuFrameTransactionContext) &&
                        SR_PPU_FRAME_TRANSACTION_REQUEST_V2_SIZE <=
                            sizeof(SrPpuFrameTransactionRequest) &&
                        SNES_RUNNER_API_PPU_FRAME_TRANSACTION_SIZE <=
                            sizeof(SnesRunnerApi),
                    "PPU frame transaction extent exceeds structure");
    failed |= check(sizeof(SrPpuVramWordPatch) == 8u &&
                        SR_PPU_VRAM_PATCH_REQUEST_V2_SIZE <=
                            sizeof(SrPpuVramPatchRequest) &&
                        SNES_RUNNER_API_PPU_VRAM_PATCH_SIZE <=
                            sizeof(SnesRunnerApi),
                    "PPU VRAM patch extent exceeds structure");
    failed |= check(sizeof(SrPpuObjPositionUpdate) == 8u &&
                        SR_PPU_OBJ_METADATA_REQUEST_V2_SIZE <=
                            sizeof(SrPpuObjMetadataRequest) &&
                        SNES_RUNNER_API_PPU_OBJ_METADATA_SIZE <=
                            sizeof(SnesRunnerApi),
                    "PPU OBJ metadata extent exceeds structure");
    failed |= check(SR_DMA_STATE_SNAPSHOT_V2_SIZE <=
                            sizeof(SrDmaStateSnapshot) &&
                        SNES_RUNNER_API_DMA_STATE_SIZE <=
                            sizeof(SnesRunnerApi),
                    "DMA state extent exceeds structure");
    failed |= check(SR_PPU_LAYER_EXTENT_REQUEST_V2_SIZE <=
                            sizeof(SrPpuLayerExtentRequest) &&
                        SR_PPU_VIRTUAL_TILEMAP_REQUEST_V2_SIZE <=
                            sizeof(SrPpuVirtualTilemapRequest) &&
                        SR_PPU_AUTHENTIC_CAMERA_REQUEST_V2_SIZE <=
                            sizeof(SrPpuAuthenticCameraRequest) &&
                        SNES_RUNNER_API_PPU_BACKGROUND_POLICY_SIZE <=
                            sizeof(SnesRunnerApi),
                    "PPU background policy extent exceeds structure");
    failed |= check(SR_PPU_SCANOUT_LINE_CONTEXT_V2_SIZE <=
                            sizeof(SrPpuScanoutLineContext) &&
                        SR_PPU_SCANOUT_REQUEST_V2_SIZE <=
                            sizeof(SrPpuScanoutRequest) &&
                        SR_PPU_SCANOUT_RESULT_V2_SIZE <=
                            sizeof(SrPpuScanoutResult) &&
                        SNES_RUNNER_API_PPU_SCANOUT_SIZE <=
                            sizeof(SnesRunnerApi),
                    "PPU scanout extent exceeds structure");
    failed |= check(SR_GAME_TIMING_REQUEST_V2_SIZE <=
                            sizeof(SrGameTimingRequest) &&
                        SR_GAME_TIMING_RESULT_V2_SIZE <=
                            sizeof(SrGameTimingResult) &&
                        SNES_RUNNER_API_GAME_TIMING_CONTROL_SIZE <=
                            sizeof(SnesRunnerApi),
                    "game timing control extent exceeds structure");
    failed |= check(SR_INPUT_STATE_SNAPSHOT_V2_SIZE <=
                            sizeof(SrInputStateSnapshot) &&
                        SNES_RUNNER_API_INPUT_STATE_SIZE <=
                            sizeof(SnesRunnerApi),
                    "input state extent exceeds structure");
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
    failed |= check(SR_AUDIO_TRACE_EVENT_V2_SIZE <=
                            sizeof(SrAudioTraceEvent) &&
                        SR_AUDIO_TRACE_SUBSCRIPTION_V2_SIZE <=
                            sizeof(SrAudioTraceSubscription) &&
                        SNES_RUNNER_API_AUDIO_TRACE_OBSERVER_SIZE <=
                            sizeof(SnesRunnerApi),
                    "audio trace observer extent exceeds structure");
    failed |= check(SR_SPC_PC_CONTROL_REQUEST_V2_SIZE <=
                            sizeof(SrSpcPcControlRequest) &&
                        SR_SPC_PC_CONTROL_RESULT_V2_SIZE <=
                            sizeof(SrSpcPcControlResult) &&
                        SNES_RUNNER_API_SPC_CONTROL_SIZE <=
                            sizeof(SnesRunnerApi),
                    "SPC control extent exceeds structure");
    failed |= check(SR_AUDIO_MIX_CONTROL_V2_SIZE <=
                            sizeof(SrAudioMixControl) &&
                        SNES_RUNNER_API_AUDIO_MIX_CONTROL_SIZE <=
                            sizeof(SnesRunnerApi),
                    "audio mix control extent exceeds structure");
    failed |= check(SR_APU_STATE_QUERY_V2_SIZE <=
                            sizeof(SrApuStateQuery) &&
                        SR_APU_STATE_SNAPSHOT_V2_SIZE <=
                            sizeof(SrApuStateSnapshot) &&
                        SNES_RUNNER_API_APU_STATE_SNAPSHOT_SIZE <=
                            sizeof(SnesRunnerApi),
                    "APU state snapshot extent exceeds structure");
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
                     SR_RUNNER_CAP_AUDIO_TRACE_OBSERVERS) != 0u,
                    "audio trace observer capability missing");
    failed |= check((api->capabilities & SR_RUNNER_CAP_SPC_CONTROL) != 0u,
                    "SPC control capability missing");
    failed |= check((api->capabilities &
                     SR_RUNNER_CAP_AUDIO_MIX_CONTROL) != 0u,
                    "audio mix control capability missing");
    failed |= check((api->capabilities &
                     SR_RUNNER_CAP_APU_STATE_SNAPSHOT) != 0u,
                    "APU state snapshot capability missing");
    failed |= check((api->capabilities &
                         (SR_RUNNER_CAP_PPU_STATE |
                          SR_RUNNER_CAP_BORROWED_U16_SPANS |
                          SR_RUNNER_CAP_PPU_FRAME_STATE |
                          SR_RUNNER_CAP_PPU_OBJ_RASTER |
                          SR_RUNNER_CAP_PPU_SURFACE_VIEWS |
                          SR_RUNNER_CAP_PPU_BACKGROUND_COORDINATE |
                          SR_RUNNER_CAP_PPU_OUTPUT_CONTROL |
                          SR_RUNNER_CAP_PPU_CAPTURE_CONTROL |
                          SR_RUNNER_CAP_PPU_FRAME_TRANSACTIONS |
                          SR_RUNNER_CAP_PPU_VRAM_PATCH |
                          SR_RUNNER_CAP_PPU_OBJ_METADATA |
                          SR_RUNNER_CAP_PPU_BACKGROUND_POLICY |
                          SR_RUNNER_CAP_PPU_SCANOUT)) ==
                        (SR_RUNNER_CAP_PPU_STATE |
                         SR_RUNNER_CAP_BORROWED_U16_SPANS |
                         SR_RUNNER_CAP_PPU_FRAME_STATE |
                         SR_RUNNER_CAP_PPU_OBJ_RASTER |
                         SR_RUNNER_CAP_PPU_SURFACE_VIEWS |
                         SR_RUNNER_CAP_PPU_BACKGROUND_COORDINATE |
                         SR_RUNNER_CAP_PPU_OUTPUT_CONTROL |
                         SR_RUNNER_CAP_PPU_CAPTURE_CONTROL |
                         SR_RUNNER_CAP_PPU_FRAME_TRANSACTIONS |
                         SR_RUNNER_CAP_PPU_VRAM_PATCH |
                         SR_RUNNER_CAP_PPU_OBJ_METADATA |
                         SR_RUNNER_CAP_PPU_BACKGROUND_POLICY |
                         SR_RUNNER_CAP_PPU_SCANOUT),
                    "PPU capabilities missing");
    failed |= check((api->capabilities & SR_RUNNER_CAP_DMA_STATE) != 0u,
                    "DMA state capability missing");
    failed |= check((api->capabilities &
                     SR_RUNNER_CAP_GAME_TIMING_CONTROL) != 0u,
                    "game timing control capability missing");
    failed |= check((api->capabilities & SR_RUNNER_CAP_INPUT_STATE) != 0u,
                    "input state capability missing");
    failed |= check((api->capabilities &
                     SR_RUNNER_CAP_PPU_FRAME_POLICY) != 0u,
                    "PPU frame-policy capability missing");
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

    {
        SrInputStateSnapshot input = {
            .struct_size = sizeof(input),
        };
        SrInputStateSnapshot small_input = {
            .struct_size = sizeof(uint32_t),
        };
        snes->input1_currentState = 0x0123u;
        snes->input2_currentState = 0x0a55u;
        failed |= check(api->query_input_state(runner, &small_input) ==
                            SR_RESULT_INVALID_ARGUMENT,
                        "undersized input snapshot accepted");
        failed |= check(api->query_input_state(runner, &input) ==
                                SR_RESULT_OK &&
                            input.struct_size ==
                                SR_INPUT_STATE_SNAPSHOT_V2_SIZE &&
                            input.flags == 0u && input.reserved == 0u &&
                            input.lifetime_generation ==
                                snes->abiLifetimeGeneration &&
                            input.frame_counter == snes->abiFrameCounter &&
                            input.packed_buttons[0] == 0x0123u &&
                            input.packed_buttons[1] == 0x0a55u &&
                            input.auto_joypad[0] == 0xc480u &&
                            input.auto_joypad[1] == 0xaa50u,
                        "input snapshot mismatch");
    }

    {
        SrGameTimingRequest timing_request = {
            .struct_size = sizeof(timing_request),
            .operation = SR_GAME_TIMING_BEGIN_FRAME_SLICE,
        };
        SrGameTimingResult timing_result = {
            .struct_size = sizeof(timing_result),
        };
        SrGameTimingResult small_timing_result = {
            .struct_size = sizeof(uint32_t),
        };
        snes->forceNmi = false;
        snes->nmiAvail = false;
        snes->inNmi = false;
        snes->nmiEnabled = false;
        snes->hPos = 37u;
        snes->vPos = 17u;
        snes->inVblank = false;
        failed |= check(api->control_game_timing(
                            runner, &timing_request,
                            &small_timing_result) ==
                                SR_RESULT_INVALID_ARGUMENT &&
                            !snes->forceNmi && !snes->nmiAvail,
                        "undersized timing result mutated state");
        timing_request.flags = UINT32_C(0x80000000);
        failed |= check(api->control_game_timing(
                            runner, &timing_request, &timing_result) ==
                                SR_RESULT_INVALID_ARGUMENT &&
                            !snes->forceNmi && !snes->nmiAvail,
                        "unknown timing flag mutated state");
        timing_request.flags = SR_GAME_TIMING_DISPATCH_NMI_IF_ENABLED;
        failed |= check(api->control_game_timing(
                            runner, &timing_request, &timing_result) ==
                                SR_RESULT_INVALID_ARGUMENT &&
                            !snes->forceNmi && !snes->nmiAvail,
                        "begin accepted completion-only timing flag");
        timing_request.flags = 0u;
        timing_request.reserved = 1u;
        failed |= check(api->control_game_timing(
                            runner, &timing_request, &timing_result) ==
                                SR_RESULT_INVALID_ARGUMENT &&
                            !snes->forceNmi && !snes->nmiAvail,
                        "reserved timing field mutated state");
        timing_request.reserved = 0u;
        timing_request.operation = 0u;
        failed |= check(api->control_game_timing(
                            runner, &timing_request, &timing_result) ==
                                SR_RESULT_INVALID_ARGUMENT &&
                            !snes->forceNmi && !snes->nmiAvail,
                        "unknown timing operation mutated state");
        timing_request.operation = SR_GAME_TIMING_BEGIN_FRAME_SLICE;
        failed |= check(api->control_game_timing(
                            runner, &timing_request, &timing_result) ==
                                SR_RESULT_OK &&
                            snes->forceNmi && snes->nmiAvail &&
                            snes->hPos == 0u && snes->inVblank &&
                            timing_result.struct_size ==
                                SR_GAME_TIMING_RESULT_V2_SIZE &&
                            timing_result.state_flags ==
                                (SR_GAME_TIMING_STATE_FORCE_NMI |
                                 SR_GAME_TIMING_STATE_NMI_AVAILABLE) &&
                            timing_result.transition_flags == 0u,
                        "frame-slice begin timing/beam state mismatch");
        timing_request.operation = SR_GAME_TIMING_COMPLETE_FRAME_SLICE;
        failed |= check(api->control_game_timing(
                            runner, &timing_request, &timing_result) ==
                                SR_RESULT_OK &&
                            !snes->forceNmi && snes->nmiAvail &&
                            !snes->inNmi &&
                            timing_result.state_flags ==
                                SR_GAME_TIMING_STATE_NMI_AVAILABLE &&
                            timing_result.transition_flags == 0u,
                        "frame-slice cancellation timing state mismatch");
        snes->inNmi = true;
        timing_request.flags = SR_GAME_TIMING_DISPATCH_NMI_IF_ENABLED;
        failed |= check(api->control_game_timing(
                            runner, &timing_request, &timing_result) ==
                                SR_RESULT_OK &&
                            snes->inNmi &&
                            (timing_result.state_flags &
                             SR_GAME_TIMING_STATE_IN_NMI) != 0u &&
                            timing_result.transition_flags == 0u,
                        "disabled NMI gate reported a new interrupt");
        snes->nmiEnabled = true;
        failed |= check(api->control_game_timing(
                            runner, &timing_request, &timing_result) ==
                                SR_RESULT_OK &&
                            snes->inNmi &&
                            timing_result.state_flags ==
                                (SR_GAME_TIMING_STATE_NMI_AVAILABLE |
                                 SR_GAME_TIMING_STATE_NMI_ENABLED |
                                 SR_GAME_TIMING_STATE_IN_NMI) &&
                            timing_result.transition_flags ==
                                SR_GAME_TIMING_TRANSITION_NMI_ENTERED,
                        "enabled NMI gate did not enter interrupt state");

        {
            TestPhaseObserver phase_observer = {0};
            SrEventSubscription phase_subscription = {
                .struct_size = sizeof(phase_subscription),
                .event_mask =
                    SR_EVENT_MASK_FRAME | SR_EVENT_MASK_INTERRUPT,
                .callback = observe_test_phase_event,
                .user_data = &phase_observer,
            };
            uint64_t phase_subscription_id = 0u;
            snes->inNmi = false;
            snes->nmiEnabled = true;
            timing_request.operation = SR_GAME_TIMING_BEGIN_FRAME_SLICE;
            timing_request.flags = 0u;
            failed |= check(api->subscribe_events(
                                runner, &phase_subscription,
                                &phase_subscription_id) == SR_RESULT_OK,
                            "timing phase observer subscription failed");
            failed |= check(api->control_game_timing(
                                runner, &timing_request, &timing_result) ==
                                SR_RESULT_OK,
                            "observed frame-slice begin failed");
            timing_request.operation =
                SR_GAME_TIMING_COMPLETE_FRAME_SLICE;
            timing_request.flags =
                SR_GAME_TIMING_DISPATCH_NMI_IF_ENABLED;
            failed |= check(api->control_game_timing(
                                runner, &timing_request, &timing_result) ==
                                SR_RESULT_OK,
                            "observed frame-slice completion failed");
            failed |= check(
                phase_observer.count == 3u &&
                    phase_observer.events[0].type ==
                        SR_EVENT_FRAME_BOUNDARY &&
                    phase_observer.events[0].flags ==
                        (SR_EVENT_FRAME_BEGIN | SR_EVENT_FRAME_VBLANK |
                         SR_EVENT_FRAME_GAME_SLICE) &&
                    strcmp(phase_observer.events[0].label,
                           "game-slice-begin") == 0 &&
                    phase_observer.events[1].type ==
                        SR_EVENT_FRAME_BOUNDARY &&
                    phase_observer.events[1].flags ==
                        (SR_EVENT_FRAME_END |
                         SR_EVENT_FRAME_GAME_SLICE) &&
                    strcmp(phase_observer.events[1].label,
                           "game-slice-end") == 0 &&
                    phase_observer.events[2].type ==
                        SR_EVENT_INTERRUPT &&
                    phase_observer.events[2].interrupt_kind ==
                        SR_INTERRUPT_NMI &&
                    phase_observer.events[2].flags ==
                        (SR_EVENT_INTERRUPT_ENTER |
                         SR_EVENT_INTERRUPT_TRANSITION) &&
                    strcmp(phase_observer.events[2].label,
                           "nmi-transition") == 0,
                "timing phase event sequence mismatch");
            failed |= check(api->unsubscribe_events(
                                runner, phase_subscription_id) ==
                                SR_RESULT_OK,
                            "timing phase observer unsubscribe failed");
        }
        snes->inNmi = false;
        snes->nmiEnabled = false;
    }

    dsp_setExtendedVoicesEnabled(false);
    snes->apu->ram[0x1234u] = 0x5au;
    dsp_write(snes->apu->dsp, 0x5du, 0x6eu);
    dsp_write(snes->apu->dsp, 0x6cu, 0x40u);
    snes->apu->dsp->sampleRead = 11u;
    snes->apu->dsp->sampleWrite = 17u;
    snes->apu->sampleClock = 29u;
    snes->apu->cycles = 35u;
    snes->apu->cycleClock = 35u;
    snes->apu->dspSlot = 3u;
    snes->apu->dspAdr = 0x5du;
    snes->apu->romReadable = true;
    snes->apu->inPorts[0] = 0x10u;
    snes->apu->inPorts[4] = 0x14u;
    snes->apu->inPorts[5] = 0x15u;
    snes->apu->outPorts[3] = 0x23u;
    snes->apu->timer[1].enabled = true;
    snes->apu->timer[1].target = 0x31u;
    snes->apu->timer[1].counter = 0x07u;
    snes->apu->spc->pc = 0x3456u;
    snes->apu->spc->a = 0x16u;
    snes->apu->spc->x = 0x27u;
    snes->apu->spc->y = 0x38u;
    snes->apu->spc->sp = 0x49u;
    snes->apu->spc->c = true;
    snes->apu->spc->z = true;
    apu_clearPortQueue(snes->apu);
    apu_schedulePortWrite(snes->apu, 2u, 0x7fu, 100u);
    failed |= check(api->query_apu_state(
                        runner, &small_apu_state_query, &apu_state) ==
                            SR_RESULT_INVALID_ARGUMENT &&
                        api->query_apu_state(
                            runner, &apu_state_query, &small_apu_state) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "undersized APU state query accepted");
    failed |= check(api->query_apu_state(
                        runner, &apu_state_query, &apu_state) ==
                            SR_RESULT_OK &&
                        s_apu_snapshot_ram[0x1234u] == 0x5au &&
                        s_dsp_snapshot_registers[0x5du] == 0x6eu &&
                        apu_state.apu_ram_bytes_written ==
                            SR_APU_RAM_BYTE_COUNT &&
                        apu_state.dsp_register_bytes_written ==
                            SR_DSP_REGISTER_BYTE_COUNT &&
                        apu_state.apu_cycles == 35u &&
                        apu_state.dsp_frames_completed == 29u &&
                        apu_state.pcm_read_cursor == 11u &&
                        apu_state.pcm_write_cursor == 17u &&
                        apu_state.pcm_ring_fill_frames == 6u &&
                        apu_state.scheduled_port_write_count == 1u &&
                        apu_state.spc_pc == 0x3456u &&
                        apu_state.spc_psw == 0x03u &&
                        apu_state.dsp_slot == 3u &&
                        apu_state.current_dsp_address == 0x5du &&
                        apu_state.cpu_to_apu_ports[0] == 0x10u &&
                        apu_state.apu_aux_ports[0] == 0x14u &&
                        apu_state.apu_aux_ports[1] == 0x15u &&
                        apu_state.apu_to_cpu_ports[3] == 0x23u &&
                        apu_state.timer_enabled_mask == 0x02u &&
                        apu_state.timer_targets[1] == 0x31u &&
                        apu_state.timer_outputs[1] == 0x07u &&
                        apu_state.hardware_voice_count == 8u &&
                        apu_state.extended_voice_count == 0u &&
                        (apu_state.flags &
                         (SR_APU_STATE_DSP_MUTED |
                          SR_APU_STATE_BOOT_ROM_VISIBLE)) ==
                            (SR_APU_STATE_DSP_MUTED |
                             SR_APU_STATE_BOOT_ROM_VISIBLE),
                    "coherent APU state snapshot mismatch");
    sr_runner_audio_production_begin();
    failed |= check(api->query_apu_state(
                        runner, &apu_state_query, &apu_state) ==
                            SR_RESULT_BUSY,
                    "APU state query allowed during audio production");
    sr_runner_audio_production_end();

    RtlApuProfileReset();
    snes->apuCatchupCycles = 5.0;
    snes_catchupApu(snes);
    for (unsigned index = 0u; index < 3u; ++index)
        apu_cycle(snes->apu);
    sr_runner_record_apu_profile_cycles(
        SR_APU_PROFILE_CYCLE_AUDIO_DEMAND, 3u, 0u);
    for (unsigned index = 0u; index < 4u; ++index)
        apu_cycle(snes->apu);
    sr_runner_record_apu_profile_cycles(
        SR_APU_PROFILE_CYCLE_UPLOAD_CONTROL, 4u, 0u);
    for (unsigned index = 0u; index < 2u; ++index)
        apu_cycle(snes->apu);
    sr_runner_record_apu_profile_cycles(
        SR_APU_PROFILE_CYCLE_TIMELINE, 2u, 0u);
    RtlApuProfileRead(&apu_profile);
    failed |= check(apu_profile.flags == 0u &&
                        apu_profile.apu_cycles_total == 14u &&
                        apu_profile.apu_cycles_port_sync == 5u &&
                        apu_profile.apu_cycles_audio_demand == 3u &&
                        apu_profile.apu_cycles_upload_control == 4u &&
                        apu_profile.apu_cycles_timeline == 2u &&
                        apu_profile.apu_cycles_unattributed == 0u &&
                        apu_profile.port_sync_calls == 1u,
                    "APU profile attribution mismatch");

    g_snes = snes;
    apu_reset(snes->apu);
    RtlApuProfileReset();
    RtlAdvanceApuTimeline();
    RtlApuProfileRead(&apu_profile);
    failed |= check(
        apu_cycle_count(snes->apu) == RTL_APU_TIMELINE_CYCLES_PER_TICK &&
            snes->apu->timelineTargetCycles ==
                RTL_APU_TIMELINE_CYCLES_PER_TICK &&
            snes->apu->sampleClock == RTL_APU_TIMELINE_FRAMES_PER_TICK &&
            apu_profile.apu_cycles_total ==
                RTL_APU_TIMELINE_CYCLES_PER_TICK &&
            apu_profile.apu_cycles_timeline ==
                RTL_APU_TIMELINE_CYCLES_PER_TICK &&
            apu_profile.apu_cycles_unattributed == 0u,
        "headless APU timeline tick mismatch");

    apu_reset(snes->apu);
    RtlApuProfileReset();
    for (uint32_t index = 0u;
         index < RTL_APU_TIMELINE_CYCLES_PER_TICK + 96u; ++index)
        apu_cycle(snes->apu);
    sr_runner_record_apu_profile_cycles(
        SR_APU_PROFILE_CYCLE_AUDIO_DEMAND,
        RTL_APU_TIMELINE_CYCLES_PER_TICK + 96u, 0u);
    RtlAdvanceApuTimeline();
    failed |= check(
        apu_cycle_count(snes->apu) ==
                RTL_APU_TIMELINE_CYCLES_PER_TICK + 96u &&
            snes->apu->timelineTargetCycles ==
                RTL_APU_TIMELINE_CYCLES_PER_TICK,
        "timeline double-advanced an APU already ahead from audio demand");
    RtlAdvanceApuTimeline();
    RtlApuProfileRead(&apu_profile);
    failed |= check(
        apu_cycle_count(snes->apu) ==
                2u * RTL_APU_TIMELINE_CYCLES_PER_TICK &&
            snes->apu->sampleClock ==
                2u * RTL_APU_TIMELINE_FRAMES_PER_TICK &&
            apu_profile.apu_cycles_audio_demand ==
                RTL_APU_TIMELINE_CYCLES_PER_TICK + 96u &&
            apu_profile.apu_cycles_timeline ==
                RTL_APU_TIMELINE_CYCLES_PER_TICK - 96u &&
            apu_profile.apu_cycles_unattributed == 0u,
        "timeline did not deterministically rejoin an ahead audio consumer");

    apu_reset(snes->apu);
    RtlSetAudioOutputRate(32040);
    RtlApuProfileReset();
    RtlRenderAudio(s_discard_audio, RTL_APU_TIMELINE_FRAMES_PER_TICK, 2);
    RtlAdvanceApuTimeline();
    failed |= check(
        apu_cycle_count(snes->apu) ==
                RTL_APU_TIMELINE_CYCLES_PER_TICK + 64u &&
            snes->apu->timelineTargetCycles ==
                RTL_APU_TIMELINE_CYCLES_PER_TICK &&
            snes->apu->sampleClock ==
                RTL_APU_TIMELINE_FRAMES_PER_TICK + 2u,
        "discard-pumped audio was double-advanced by the timeline");
    RtlAdvanceApuTimeline();
    RtlApuProfileRead(&apu_profile);
    failed |= check(
        apu_cycle_count(snes->apu) ==
                2u * RTL_APU_TIMELINE_CYCLES_PER_TICK &&
            snes->apu->sampleClock ==
                2u * RTL_APU_TIMELINE_FRAMES_PER_TICK &&
            apu_profile.apu_cycles_audio_demand ==
                RTL_APU_TIMELINE_CYCLES_PER_TICK + 64u &&
            apu_profile.apu_cycles_timeline ==
                RTL_APU_TIMELINE_CYCLES_PER_TICK - 64u &&
            apu_profile.apu_cycles_unattributed == 0u,
        "discard-pumped audio did not deterministically rejoin the timeline");
    snes->abiAudioFrameCounter = 0u;

    apu_reset(snes->apu);
    apu_cycle(snes->apu);
    RtlApuProfileReset();
    apu_reset(snes->apu);
    RtlApuProfileRead(&apu_profile);
    failed |= check(
        apu_profile.apu_cycles_total == 0u &&
            (apu_profile.flags & RTL_APU_PROFILE_INCONSISTENT) != 0u,
        "APU profile clock rollback wrapped its cycle delta");

    apu_reset(snes->apu);
    g_apu_pace_cycles_estimate = 0u;
    g_apu_last_sync_cycles = 0u;
    RtlApuWrite(0x2140u, 0x11u);
    RtlApuWrite(0x2140u, 0x22u);
    RtlApuWrite(0x2140u, 0x22u);
    failed |= check(
        snes->apu->portQTail - snes->apu->portQHead == 3u &&
            snes->apu->portQueue[0].target_sample == 0u &&
            snes->apu->portQueue[1].target_sample == APU_PORT_MIN_DWELL &&
            snes->apu->portQueue[2].target_sample == APU_PORT_MIN_DWELL &&
            snes->apu->portClockNs == 0u,
        "CPU-to-APU port scheduling depends on non-emulated time");
    RtlApuProfileReset();
    g_snes = NULL;

    snes->apu->ram[0x1a] = 0xa1u;
    snes->apu->ram[0x35] = 0xb2u;
    snes->apu->ram[0x47] = 0xc3u;
    snes->apu->inPorts[3] = 0xd4u;
    snes->apu->outPorts[2] = 0xe5u;
    snes->apu->spc->a = 0x16u;
    snes->apu->spc->x = 0x27u;
    snes->apu->spc->y = 0x38u;
    snes->apu->spc->sp = 0x49u;
    failed |= check(api->subscribe_audio_trace(
                        runner, &small_audio_trace_subscription,
                        &audio_trace_subscription_id) ==
                            SR_RESULT_INVALID_ARGUMENT &&
                        audio_trace_subscription_id == 0u,
                    "undersized audio trace subscription accepted");
    failed |= check(api->subscribe_audio_trace(
                        runner, &audio_trace_subscription,
                        &audio_trace_subscription_id) == SR_RESULT_OK &&
                        audio_trace_subscription_id != 0u,
                    "audio trace subscription failed");
    failed |= check(sr_runner_audio_trace_enabled(SR_AUDIO_TRACE_MASK_ALL),
                    "audio trace observation did not become active");
    snes->apu->spc->pc = 0x3456u;
    snes->apu->spc->stopped = false;
    snes->apu->ram[0x3456u] = 0x00u;
    (void)spc_runOpcode(snes->apu->spc);
    apu_cpuWrite(snes->apu, 0xf2u, 0x5du);
    apu_cpuWrite(snes->apu, 0xf3u, 0x6eu);
    apu_clearPortQueue(snes->apu);
    snes->apu->cycles = 0u;
    snes->apu->dspSlot = 0u;
    snes->apu->cpuCyclesLeft = 1u;
    apu_schedulePortWrite(snes->apu, 2u, 0x7fu, 0u);
    apu_cycle(snes->apu);
    snes->apu->inPorts[3] = 0x80u;
    (void)apu_cpuRead(snes->apu, 0xf7u);
    failed |= check(audio_trace_observer.count == 4u &&
                        audio_trace_observer.runner == runner,
                    "audio trace callback count mismatch");
    failed |= check(audio_trace_observer.events[0].type ==
                            SR_AUDIO_TRACE_SPC_OPCODE &&
                        audio_trace_observer.events[0].spc_pc == 0x3456u &&
                        audio_trace_observer.events[0].spc_a == 0x16u &&
                        audio_trace_observer.events[0].spc_x == 0x27u &&
                        audio_trace_observer.events[0].spc_y == 0x38u &&
                        audio_trace_observer.events[0].spc_sp == 0x49u &&
                        audio_trace_observer.events[0].apu_input_ports[3] ==
                            0xd4u &&
                        audio_trace_observer.events[0].apu_output_ports[2] ==
                            0xe5u &&
                        audio_trace_observer.driver_bytes[0][0] == 0xa1u &&
                        audio_trace_observer.driver_bytes[0][1] == 0xb2u &&
                        audio_trace_observer.driver_bytes[0][2] == 0xc3u,
                    "SPC opcode trace payload mismatch");
    failed |= check(audio_trace_observer.events[1].type ==
                            SR_AUDIO_TRACE_DSP_WRITE &&
                        audio_trace_observer.events[1].struct_size ==
                            SR_AUDIO_TRACE_EVENT_V2_SIZE &&
                        audio_trace_observer.events[1].spc_pc == 0x3457u &&
                        audio_trace_observer.events[1].spc_instruction_pc ==
                            0x3456u &&
                        audio_trace_observer.events[1].dsp_address == 0x5du &&
                        audio_trace_observer.events[1].value == 0x6eu,
                    "DSP trace payload mismatch");
    failed |= check(audio_trace_observer.events[2].type ==
                            SR_AUDIO_TRACE_APU_PORT_APPLY &&
                        audio_trace_observer.events[2].port == 2u &&
                        audio_trace_observer.events[2].value == 0x7fu &&
                        audio_trace_observer.events[3].type ==
                            SR_AUDIO_TRACE_SPC_PORT_READ &&
                        audio_trace_observer.events[3].port == 3u &&
                        audio_trace_observer.events[3].value == 0x80u,
                    "APU port trace payload mismatch");
    sr_runner_emit_audio_trace(
        snes->apu, SR_AUDIO_TRACE_CPU_PORT_WRITE, 0u, 1u, 0u, 0x42u,
        1234u, 0u, 91u, "GameAudioWrite");
    sr_runner_emit_audio_trace(
        snes->apu, SR_AUDIO_TRACE_SPC_UPLOAD, 0u, 0u, 0u, 0u,
        1235u, 0x1a94b8u, 92u, "GameAudioUpload");
    failed |= check(audio_trace_observer.count == 6u &&
                        audio_trace_observer.events[4].type ==
                            SR_AUDIO_TRACE_CPU_PORT_WRITE &&
                        audio_trace_observer.events[4].port == 1u &&
                        audio_trace_observer.events[4].value == 0x42u &&
                        audio_trace_observer.events[4].frame_counter == 91u &&
                        strcmp(audio_trace_observer.events[4].function_name,
                               "GameAudioWrite") == 0 &&
                        audio_trace_observer.events[5].type ==
                            SR_AUDIO_TRACE_SPC_UPLOAD &&
                        audio_trace_observer.events[5].source_address ==
                            0x1a94b8u &&
                        audio_trace_observer.events[5].frame_counter == 92u &&
                        strcmp(audio_trace_observer.events[5].function_name,
                               "GameAudioUpload") == 0,
                    "game-side audio trace payload mismatch");
    failed |= check(api->unsubscribe_audio_trace(
                        runner, audio_trace_subscription_id) ==
                            SR_RESULT_OK &&
                        !sr_runner_audio_trace_enabled(
                            SR_AUDIO_TRACE_MASK_ALL),
                    "audio trace unsubscribe remained active");
    audio_trace_subscription_id = 0u;

    failed |= check(api->subscribe_audio_trace(
                        runner, &zero_mask_audio_trace_subscription,
                        &audio_trace_subscription_id) ==
                            SR_RESULT_INVALID_ARGUMENT &&
                        audio_trace_subscription_id == 0u,
                    "zero audio trace mask accepted");
    memset(&audio_trace_observer, 0, sizeof(audio_trace_observer));
    failed |= check(api->subscribe_audio_trace(
                        runner, &filtered_audio_trace_subscription,
                        &audio_trace_subscription_id) == SR_RESULT_OK &&
                        sr_runner_audio_trace_enabled(
                            SR_AUDIO_TRACE_MASK_DSP_WRITE) &&
                        !sr_runner_audio_trace_enabled(
                            SR_AUDIO_TRACE_MASK_SPC_OPCODE),
                    "filtered audio trace mask was not installed");
    snes->apu->spc->pc = 0x3456u;
    snes->apu->ram[0x3456u] = 0x00u;
    (void)spc_runOpcode(snes->apu->spc);
    apu_cpuWrite(snes->apu, 0xf2u, 0x2cu);
    apu_cpuWrite(snes->apu, 0xf3u, 0x55u);
    failed |= check(audio_trace_observer.count == 1u &&
                        audio_trace_observer.events[0].type ==
                            SR_AUDIO_TRACE_DSP_WRITE,
                    "filtered tracing dispatched an unrequested event");
    failed |= check(api->unsubscribe_audio_trace(
                        runner, audio_trace_subscription_id) ==
                            SR_RESULT_OK,
                    "filtered audio trace unsubscribe failed");
    audio_trace_subscription_id = 0u;

    memset(&audio_trace_observer, 0, sizeof(audio_trace_observer));
    failed |= check(api->subscribe_audio_trace(
                        runner, &key_on_audio_trace_subscription,
                        &audio_trace_subscription_id) == SR_RESULT_OK &&
                        sr_runner_audio_trace_enabled(
                            SR_AUDIO_TRACE_MASK_DSP_KEY_ON) &&
                        !sr_runner_audio_trace_enabled(
                            SR_AUDIO_TRACE_MASK_DSP_WRITE),
                    "DSP key-on trace mask was not installed");
    dsp_write(snes->apu->dsp, 0x5du, 0x02u);
    dsp_write(snes->apu->dsp, 0x00u, 0x81u);
    dsp_write(snes->apu->dsp, 0x01u, 0x7eu);
    dsp_write(snes->apu->dsp, 0x02u, 0x34u);
    dsp_write(snes->apu->dsp, 0x03u, 0x12u);
    dsp_write(snes->apu->dsp, 0x04u, 0x0bu);
    snes->apu->ram[0x022cu] = 0x78u;
    snes->apu->ram[0x022du] = 0x56u;
    snes->apu->cycleClock = 4321u;
    dsp_write(snes->apu->dsp, 0x4cu, 0x01u);
    dsp_writeVirtualVoiceRegister(snes->apu->dsp, 8, 0x00u, 0x80u);
    dsp_writeVirtualVoiceRegister(snes->apu->dsp, 8, 0x01u, 0x7fu);
    dsp_writeVirtualVoiceRegister(snes->apu->dsp, 8, 0x02u, 0xcdu);
    dsp_writeVirtualVoiceRegister(snes->apu->dsp, 8, 0x03u, 0x2au);
    dsp_writeVirtualVoiceRegister(snes->apu->dsp, 8, 0x04u, 0x02u);
    snes->apu->ram[0x0208u] = 0x00u;
    snes->apu->ram[0x0209u] = 0x03u;
    snes->apu->cycleClock = 4322u;
    dsp_writeVirtualVoiceControl(snes->apu->dsp, 8, 0x4cu, true);
    failed |= check(
        RtlApuCycleCount() == 4322u &&
            audio_trace_observer.count == 2u &&
            audio_trace_observer.events[0].type ==
                SR_AUDIO_TRACE_DSP_KEY_ON &&
            audio_trace_observer.events[0].cycle_count == 4321u &&
            audio_trace_observer.events[0].voice_index == 0u &&
            audio_trace_observer.events[0].voice_source_number == 0x0bu &&
            audio_trace_observer.events[0].voice_brr_address == 0x5678u &&
            audio_trace_observer.events[0].voice_volume_left == -127 &&
            audio_trace_observer.events[0].voice_volume_right == 126 &&
            audio_trace_observer.events[0].voice_pitch == 0x1234u &&
            audio_trace_observer.events[1].cycle_count == 4322u &&
            audio_trace_observer.events[1].voice_index == 8u &&
            audio_trace_observer.events[1].voice_source_number == 0x02u &&
            audio_trace_observer.events[1].voice_brr_address == 0x0300u &&
            audio_trace_observer.events[1].voice_pitch == 0x2acdu,
        "DSP key-on trace payload mismatch");
    failed |= check(api->unsubscribe_audio_trace(
                        runner, audio_trace_subscription_id) ==
                            SR_RESULT_OK,
                    "DSP key-on trace unsubscribe failed");
    audio_trace_subscription_id = 0u;

    failed |= check(api->subscribe_audio_trace(
                        runner, &snapshot_reentry_subscription,
                        &audio_trace_subscription_id) == SR_RESULT_OK,
                    "snapshot re-entry observer subscription failed");
    apu_cpuWrite(snes->apu, 0xf2u, 0x3cu);
    apu_cpuWrite(snes->apu, 0xf3u, 0x44u);
    failed |= check(snapshot_reentry_observer.count == 1u &&
                        snapshot_reentry_observer.result == SR_RESULT_BUSY,
                    "APU snapshot allowed from an audio trace callback");
    failed |= check(api->unsubscribe_audio_trace(
                        runner, audio_trace_subscription_id) ==
                            SR_RESULT_OK,
                    "snapshot re-entry observer unsubscribe failed");
    audio_trace_subscription_id = 0u;

    snes->ppu->inidisp = 0x8du;
    snes->ppu->obsel = 0x63u;
    snes->ppu->oamaddl = 0x24u;
    snes->ppu->oamaddh = 0x81u;
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
    snes->ppu->window1left = 0x12u;
    snes->ppu->window1right = 0x34u;
    snes->ppu->window2left = 0x56u;
    snes->ppu->window2right = 0x78u;
    snes->ppu->wbgobjlog = 0x82u;
    snes->ppu->cgwsel = 0x73u;
    snes->ppu->cgadsub = 0x64u;
    snes->ppu->setini = 0x4du;
    snes->ppu->fixedColor = 0x3456u;
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
    snes->ppu->wsLayerClamp = 0x01u;
    snes->ppu->wsLayerNormalScroll = 0x08u;
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
    failed |= check(api->resolve_ppu_obj_range(
                        runner, &obj_resolve_request,
                        &obj_resolve_result) == SR_RESULT_UNAVAILABLE,
                    "PPU OBJ resolve without a provider was available");
    failed |= check(api->rasterize_ppu_obj_parts(
                        runner, &obj_parts_request, &obj_result) ==
                        SR_RESULT_UNAVAILABLE,
                    "PPU OBJ parts raster without a provider was available");
    cpu_state.struct_size = sizeof(cpu_state);
    sr_runner_set_cpu_state_provider(
        snes, query_test_cpu_state, &s_cpu_component, &s_cpu_component);
    sr_runner_set_execution_state_provider(
        snes, query_test_execution_state, &s_cpu_component);
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
                        ppu_state.mode7_matrix[7] == 0x1107 &&
                        ppu_state.fixed_color == 0x3456u &&
                        ppu_state.reserved16 == 0u &&
                        ppu_state.window1_left == 0x12u &&
                        ppu_state.window1_right == 0x34u &&
                        ppu_state.window2_left == 0x56u &&
                        ppu_state.window2_right == 0x78u &&
                        ppu_state.setini_control == 0x4du &&
                        ppu_state.layer_clamp_mask == 0x01u &&
                        ppu_state.layer_mirror_mask == 0x01u &&
                        ppu_state.layer_repeat_mask == 0x00u &&
                        ppu_state.layer_normal_scroll_mask == 0x08u &&
                        ppu_state.oam_address_low == 0x24u &&
                        ppu_state.oam_address_high == 0x81u &&
                        ppu_state.object_priority_rotation == 1u &&
                        ppu_state.reserved8_3 == 0u &&
                        ppu_state.object_small_size_pixels == 16u &&
                        ppu_state.object_large_size_pixels == 32u &&
                        ppu_state.reserved16_2 == 0u,
                    "PPU v2 raw-control snapshot mismatch");
    failed |= check(api->query_ppu_state(runner, &small_ppu_state) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU snapshot accepted");

    failed |= check(api->query_ppu_frame_state(runner, &ppu_frame) ==
                        SR_RESULT_OK,
                    "PPU frame query failed");
    failed |= check(ppu_frame.display_control == 0x8du &&
                        ppu_frame.flags ==
                            SR_PPU_FRAME_HUD_POLICY_CONFIGURED &&
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
                        ppu_frame.overlays[0].transparent_fill_mode ==
                            SR_PPU_TRANSPARENT_FILL_CGRAM &&
                        ppu_frame.overlays[0].transparent_fill_cgram == 8u &&
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
    output_binding.height_pixels = 252u;
    output_binding.pixel_byte_size =
        output_binding.pitch_bytes * output_binding.height_pixels;
    failed |= check(api->bind_ppu_output_surface(
                        runner, &output_binding) ==
                            SR_RESULT_INVALID_ARGUMENT &&
                        snes->ppu->renderHeight == 253u,
                    "short main output capacity replaced a valid binding");
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
    failed |= check(api->visit_ppu_frame_transaction(
                        runner, &small_ppu_transaction_request) ==
                            SR_RESULT_INVALID_ARGUMENT &&
                        ppu_transaction_observer.count == 0u,
                    "undersized PPU frame transaction accepted");
    failed |= check(api->visit_ppu_frame_transaction(
                        runner, &ppu_transaction_request) == SR_RESULT_OK &&
                        ppu_transaction_observer.count == 1u &&
                        ppu_transaction_observer.runner == runner &&
                        ppu_transaction_observer.valid,
                    "coherent PPU frame transaction view mismatch");
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
    ppu_frame_reset_request.lifetime_generation =
        ppu_surfaces.lifetime_generation;
    small_ppu_frame_reset_request.lifetime_generation =
        ppu_surfaces.lifetime_generation;
    failed |= check(api->reset_ppu_frame_state(
                        runner, &small_ppu_frame_reset_request) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU frame reset accepted");
    failed |= check(api->reset_ppu_frame_state(
                        runner, &ppu_frame_reset_request) == SR_RESULT_OK &&
                        snes->ppu->overlayCaptures[0].x1 == 0 &&
                        snes->ppu->m7Override.rgba == NULL,
                    "PPU frame reset did not clear transient captures");
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
    margin_request.budget_pixels = 65u;
    failed |= check(api->configure_ppu_horizontal_margin(
                        runner, &margin_request) ==
                            SR_RESULT_INVALID_ARGUMENT &&
                        snes->ppu->extraLeftRight == 64u &&
                        snes->ppu->extraLeftCur == 0u,
                    "PPU margin exceeded bound surface width");
    margin_request.budget_pixels = 48u;
    margin_request.mode = SR_PPU_HORIZONTAL_MARGIN_AVAILABLE;
    failed |= check(api->configure_ppu_horizontal_margin(
                        runner, &margin_request) == SR_RESULT_OK &&
                        snes->ppu->extraLeftRight == 48u &&
                        snes->ppu->extraLeftCur == 48u &&
                        snes->ppu->extraRightCur == 48u,
                    "available PPU margin configuration failed");
    rebound_surfaces.struct_size = sizeof(rebound_surfaces);
    failed |= check(api->query_ppu_surfaces(
                        runner, &rebound_surfaces) == SR_RESULT_OK &&
                        api->ppu_surface_snapshot_is_valid(
                            runner, &rebound_surfaces),
                    "PPU surface snapshot before frame policy failed");

    frame_policy_request.lifetime_generation =
        snes->abiLifetimeGeneration;
    frame_policy_request.policy.flags =
        SR_PPU_FRAME_POLICY_PAD_CAPTURED_TO_BUDGET;
    frame_policy_request.policy.margin_budget_pixels = 48u;
    frame_policy_request.policy.margin_left_pixels = 31u;
    frame_policy_request.policy.margin_right_pixels = 37u;
    frame_policy_request.policy.margin_top_pixels = 8u;
    frame_policy_request.policy.margin_bottom_pixels = 9u;
    frame_policy_request.policy.layer_clamp_mask = 0x01u;
    frame_policy_request.policy.layer_mirror_mask = 0x02u;
    frame_policy_request.policy.layer_repeat_mask = 0x04u;
    frame_policy_request.policy.layer_normal_scroll_mask = 0x08u;
    frame_policy_request.policy.vertical_clip_layer_mask = 0x05u;
    frame_policy_request.policy.vertical_clip_top_rows[0] = 3u;
    frame_policy_request.policy.vertical_clip_bottom_rows[0] = 4u;
    frame_policy_request.policy.vertical_clip_top_rows[2] = 5u;
    frame_policy_request.policy.vertical_clip_bottom_rows[2] = 6u;
    frame_policy_request.policy.hud_split_height = 32u;
    frame_policy_request.policy.hud_left_end_x = 64u;
    frame_policy_request.policy.hud_right_start_x = 192u;
    frame_policy_request.policy.hud_player_row_y = 8u;
    frame_policy_request.policy.hud_left_only_y = 16u;
    frame_policy_bands[0] = (SrPpuFramePolicyBand) {
        .layer = 1u,
        .y0 = 20u,
        .y1 = 30u,
        .fill = SR_PPU_BACKGROUND_FILL_REPEAT,
        .motion = SR_PPU_BACKGROUND_MOTION_FILL_RELATIVE,
    };
    frame_policy_bands[1] = (SrPpuFramePolicyBand) {
        .layer = 0u,
        .y0 = 40u,
        .y1 = 44u,
        .fill = SR_PPU_BACKGROUND_FILL_MIRROR,
        .motion = SR_PPU_BACKGROUND_MOTION_NORMAL_SCROLL,
    };
    frame_policy_request.policy.margin_top_pixels = 64u;
    frame_policy_request.policy.margin_bottom_pixels = 64u;
    failed |= check(api->apply_ppu_frame_policy(
                        runner, &frame_policy_request) ==
                            SR_RESULT_INVALID_ARGUMENT &&
                        snes->ppu->extraTopCur == 14u &&
                        snes->ppu->extraBottomCur == 15u,
                    "PPU frame policy exceeded bound surface height");
    frame_policy_request.policy.margin_top_pixels = 8u;
    frame_policy_request.policy.margin_bottom_pixels = 9u;
    failed |= check(api->apply_ppu_frame_policy(
                        runner, &frame_policy_request) == SR_RESULT_OK &&
                        snes->ppu->extraLeftRight == 48u &&
                        snes->ppu->extraLeftCur == 31u &&
                        snes->ppu->extraRightCur == 37u &&
                        snes->ppu->extraTopCur == 8u &&
                        snes->ppu->extraBottomCur == 9u &&
                        snes->ppu->wsLayerClamp == 0x01u &&
                        snes->ppu->wsLayerMirror == 0x02u &&
                        snes->ppu->wsLayerRepeat == 0x04u &&
                        snes->ppu->wsLayerNormalScroll == 0x08u &&
                        snes->ppu->wsPadCapturedToBudget == 1u &&
                        snes->ppu->verticalMarginLayerClip == 0x05u &&
                        snes->ppu->verticalMarginTopRows[2] == 5u &&
                        snes->ppu->verticalMarginBottomRows[2] == 6u &&
                        snes->ppu->wsHudSplitHeight == 32u &&
                        snes->ppu->wsHudLeftEnd == 64u &&
                        snes->ppu->wsHudRightStart == 192u &&
                        snes->ppu->wsBandFill[1][29] ==
                            kPpuWidescreenBandFill_Repeat &&
                        snes->ppu->wsBandFill[0][40] ==
                            kPpuWidescreenBandFill_Mirror &&
                        snes->ppu->wsBandMotion[0][40] ==
                            kPpuWidescreenMotion_NormalScroll,
                    "atomic PPU frame policy mismatch");
    failed |= check(!api->ppu_surface_snapshot_is_valid(
                        runner, &rebound_surfaces),
                    "frame-policy geometry retained a stale surface snapshot");
    snes->ppu->virtualTilemap[0].context = &s_virtual_context;
    snes->ppu->wsLayerExtentLeftDefault[0] = 77u;
    frame_policy_request.policy.flags |= SR_PPU_FRAME_POLICY_FINALIZE;
    frame_policy_request.policy.margin_left_pixels = 30u;
    frame_policy_request.policy.margin_right_pixels = 36u;
    frame_policy_bands[0].fill = SR_PPU_BACKGROUND_FILL_CLAMP;
    failed |= check(api->apply_ppu_frame_policy(
                        runner, &frame_policy_request) == SR_RESULT_OK &&
                        snes->ppu->extraLeftCur == 30u &&
                        snes->ppu->extraRightCur == 36u &&
                        snes->ppu->virtualTilemap[0].context ==
                            &s_virtual_context &&
                        snes->ppu->wsLayerExtentLeftDefault[0] == 77u &&
                        snes->ppu->wsBandFill[1][20] ==
                            kPpuWidescreenBandFill_Clamp,
                    "PPU frame-policy finalize did not preserve providers");
    frame_policy_request.policy.margin_budget_pixels = 47u;
    failed |= check(api->apply_ppu_frame_policy(
                        runner, &frame_policy_request) == SR_RESULT_BUSY &&
                        snes->ppu->extraLeftCur == 30u &&
                        snes->ppu->virtualTilemap[0].context ==
                            &s_virtual_context,
                    "mismatched PPU frame-policy finalize mutated state");
    frame_policy_request.policy.margin_budget_pixels = 20u;
    frame_policy_bands[1].y1 = SR_PPU_NATIVE_HEIGHT + 1u;
    failed |= check(api->apply_ppu_frame_policy(
                        runner, &frame_policy_request) ==
                            SR_RESULT_INVALID_ARGUMENT &&
                        snes->ppu->extraLeftRight == 48u &&
                        snes->ppu->wsLayerMirror == 0x02u,
                    "invalid PPU frame policy partially applied");
    frame_policy_request.policy.margin_budget_pixels = 48u;
    frame_policy_bands[1].y1 = 44u;
    ++frame_policy_request.lifetime_generation;
    failed |= check(api->apply_ppu_frame_policy(
                        runner, &frame_policy_request) ==
                            SR_RESULT_STALE_VIEW &&
                        snes->ppu->extraLeftCur == 30u,
                    "stale PPU frame policy applied");
    --frame_policy_request.lifetime_generation;

    frame_policy_request.policy = (SrPpuFramePolicy) {
        .struct_size = sizeof(frame_policy_request.policy),
        .horizontal_mode = SR_PPU_HORIZONTAL_MARGIN_CENTERED,
        .margin_budget_pixels = 24u,
    };
    failed |= check(api->apply_ppu_frame_policy(
                        runner, &frame_policy_request) == SR_RESULT_OK &&
                        snes->ppu->extraLeftRight == 24u &&
                        snes->ppu->extraLeftCur == 0u &&
                        snes->ppu->extraRightCur == 0u,
                    "centered PPU frame-policy begin failed");
    snes->ppu->virtualTilemap[0].context = &s_virtual_context;
    frame_policy_request.policy.flags = SR_PPU_FRAME_POLICY_FINALIZE;
    failed |= check(api->apply_ppu_frame_policy(
                        runner, &frame_policy_request) == SR_RESULT_OK &&
                        snes->ppu->extraLeftRight == 24u &&
                        snes->ppu->extraLeftCur == 0u &&
                        snes->ppu->extraRightCur == 0u &&
                        snes->ppu->virtualTilemap[0].context ==
                            &s_virtual_context,
                    "centered PPU frame-policy finalize changed geometry");
    failed |= check(api->apply_ppu_frame_policy(
                        runner, &frame_policy_request) == SR_RESULT_BUSY &&
                        snes->ppu->extraLeftRight == 24u &&
                        snes->ppu->virtualTilemap[0].context ==
                            &s_virtual_context,
                    "consumed PPU frame-policy finalize was reused");

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

    {
        const SrPpuOverlayCaptureState original = {
            .x0 = -8,
            .x1 = 112,
            .y0 = 4,
            .y1 = 36,
            .flags = SR_PPU_OVERLAY_REMOVE_FROM_GAME |
                     SR_PPU_OVERLAY_MARK_MAIN_SCREEN_WINNER,
            .transparent_fill_configured = 1u,
            .transparent_fill_mode = SR_PPU_TRANSPARENT_FILL_CGRAM,
            .transparent_fill_cgram = 0x36u,
        };
        const SrPpuOverlayCaptureState temporary = {
            .x0 = -4,
            .x1 = 124,
            .y0 = 0,
            .y1 = 48,
            .flags = SR_PPU_OVERLAY_MARK_BG_HALF_ADD,
            .transparent_fill_configured = 1u,
            .transparent_fill_mode = SR_PPU_TRANSPARENT_FILL_BLACK,
        };
        failed |= check(PpuSetOverlayTransparentFill(
                            snes->ppu, kPpuOverlaySource_Bg2,
                            kPpuOverlayTransparentFill_Cgram, 0x36u),
                        "PPU capture exchange fill fixture failed");
        capture_exchange = (SrPpuOverlayCaptureExchangeRequest) {
            .struct_size = sizeof(capture_exchange),
            .source_mask = 1u << SR_PPU_OVERLAY_BG2,
        };
        capture_exchange.expected[SR_PPU_OVERLAY_BG2] = original;
        capture_exchange.replacement[SR_PPU_OVERLAY_BG2] = temporary;
        failed |= check(api->compare_exchange_ppu_overlay_captures(
                            runner, &capture_exchange) == SR_RESULT_OK &&
                            snes->ppu->overlayCaptures[
                                SR_PPU_OVERLAY_BG2].x0 == -4 &&
                            snes->ppu->overlayCaptures[
                                SR_PPU_OVERLAY_BG2].x1 == 124 &&
                            snes->ppu->overlayCaptures[
                                SR_PPU_OVERLAY_BG2].flags ==
                                SR_PPU_OVERLAY_MARK_BG_HALF_ADD &&
                            snes->ppu->overlayCaptures[
                                SR_PPU_OVERLAY_BG2]
                                    .transparentFillConfigured == 1u &&
                            snes->ppu->overlayCaptures[
                                SR_PPU_OVERLAY_BG2].transparentFillMode ==
                                kPpuOverlayTransparentFill_Black,
                        "PPU capture compare/exchange failed");
        failed |= check(api->compare_exchange_ppu_overlay_captures(
                            runner, &capture_exchange) == SR_RESULT_BUSY &&
                            snes->ppu->overlayCaptures[
                                SR_PPU_OVERLAY_BG2].x0 == -4,
                        "stale PPU capture expectation was accepted");
        capture_exchange.expected[SR_PPU_OVERLAY_BG2] = temporary;
        capture_exchange.replacement[SR_PPU_OVERLAY_BG2] = original;
        failed |= check(api->compare_exchange_ppu_overlay_captures(
                        runner, &capture_exchange) == SR_RESULT_OK &&
                            snes->ppu->overlayCaptures[
                                SR_PPU_OVERLAY_BG2].x0 == -8 &&
                            snes->ppu->overlayCaptures[
                                SR_PPU_OVERLAY_BG2].transparentFillMode ==
                                kPpuOverlayTransparentFill_Cgram &&
                            snes->ppu->overlayCaptures[
                                SR_PPU_OVERLAY_BG2].transparentFillCgram ==
                                0x36u,
                        "PPU capture policy restore failed");
        capture_exchange.lifetime_generation++;
        failed |= check(api->compare_exchange_ppu_overlay_captures(
                            runner, &capture_exchange) ==
                                SR_RESULT_STALE_VIEW,
                        "stale PPU capture exchange accepted");
        capture_exchange.lifetime_generation--;
        capture_exchange.replacement[
            SR_PPU_OVERLAY_BG2].reserved8[0] = 1u;
        failed |= check(api->compare_exchange_ppu_overlay_captures(
                            runner, &capture_exchange) ==
                                SR_RESULT_INVALID_ARGUMENT,
                        "invalid PPU capture policy accepted");
    }

    failed |= check(PpuSetOverlayCapture(
                        snes->ppu, kPpuOverlaySource_Obj, 0, 0, 256, 32,
                        kPpuOverlayFlag_MarkFullAddSubscreen) &&
                        PpuSetOverlayOamRange(snes->ppu, 2u, 4u),
                    "PPU OBJ capture fixture failed");
    obj_capture_request = (SrPpuObjCaptureRequest) {
        .struct_size = sizeof(obj_capture_request),
        .flags = SR_PPU_OBJ_CAPTURE_RANGE |
                 SR_PPU_OBJ_CAPTURE_RELOCATED,
        .lifetime_generation = ppu_state.lifetime_generation,
        .range_first = 2u,
        .range_count = 2u,
        .range_x = 4,
        .range_y = 4,
        .range_width = 16u,
        .range_height = 16u,
        .range_pixels = (uint8_t *)s_overlay_surface,
        .range_pixel_byte_size = sizeof(s_overlay_surface),
        .range_pitch_bytes = 384u * sizeof(uint32_t),
        .relocated_first = 2u,
        .relocated_count = 2u,
    };
    failed |= check(api->configure_ppu_obj_capture(
                        runner, &obj_capture_request) == SR_RESULT_OK &&
                        snes->ppu->objRangeCapture.first == 2u &&
                        snes->ppu->objRangeCapture.count == 2u &&
                        snes->ppu->objRangeCapture.pixels ==
                            (uint8_t *)s_overlay_surface &&
                        snes->ppu->overlayObjRelocatedFirst == 2u &&
                        snes->ppu->overlayObjRelocatedCount == 2u,
                    "PPU OBJ capture configuration failed");
    obj_capture_request.range_pixel_byte_size = 1u;
    failed |= check(api->configure_ppu_obj_capture(
                        runner, &obj_capture_request) ==
                            SR_RESULT_INVALID_ARGUMENT &&
                        snes->ppu->objRangeCapture.count == 2u &&
                        snes->ppu->overlayObjRelocatedCount == 2u,
                    "invalid PPU OBJ capture partially applied");
    obj_capture_request.range_pixel_byte_size = sizeof(s_overlay_surface);

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

    failed |= check(api->resolve_ppu_obj_range(
                        runner, &obj_resolve_request,
                        &obj_resolve_result) == SR_RESULT_OK,
                    "PPU OBJ resolve failed");
    failed |= check(obj_resolve_result.lifetime_generation == 0u &&
                        obj_resolve_result.part_count == 1u &&
                        obj_resolve_result.x0 == 10 &&
                        obj_resolve_result.y0 == 20 &&
                        obj_resolve_result.x1 == 18 &&
                        obj_resolve_result.y1 == 28 &&
                        obj_parts[0].x == 10 && obj_parts[0].y == 20 &&
                        obj_parts[0].size == 8u &&
                        obj_parts[0].reserved == 0u,
                    "PPU OBJ resolved parts mismatch");
    failed |= check(api->resolve_ppu_obj_range(
                        runner, &obj_resolve_request,
                        &small_obj_resolve_result) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU OBJ resolve result accepted");
    obj_resolve_request.part_capacity = 0u;
    obj_resolve_result.struct_size = sizeof(obj_resolve_result);
    failed |= check(api->resolve_ppu_obj_range(
                        runner, &obj_resolve_request,
                        &obj_resolve_result) == SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU OBJ part capacity accepted");
    obj_resolve_request.part_capacity = 1u;
    obj_resolve_result.struct_size = sizeof(obj_resolve_result);
    failed |= check(api->resolve_ppu_obj_range(
                        runner, &obj_resolve_request,
                        &obj_resolve_result) == SR_RESULT_OK,
                    "PPU OBJ resolve did not recover after rejection");

    obj_parts_request.x0 = obj_resolve_result.x0;
    obj_parts_request.y0 = obj_resolve_result.y0;
    obj_parts_request.x1 = obj_resolve_result.x1;
    obj_parts_request.y1 = obj_resolve_result.y1;
    memset(obj_pixels, 0x5a, sizeof(obj_pixels));
    obj_result.struct_size = sizeof(obj_result);
    failed |= check(api->rasterize_ppu_obj_parts(
                        runner, &obj_parts_request, &obj_result) ==
                        SR_RESULT_OK,
                    "PPU OBJ parts raster failed");
    failed |= check(obj_result.x0 == 10 && obj_result.y0 == 20 &&
                        obj_result.width == 8u && obj_result.height == 8u &&
                        obj_pixels[0] == UINT32_C(0xffff0000) &&
                        obj_pixels[63] == UINT32_C(0xffff0000),
                    "PPU OBJ parts raster result mismatch");
    obj_parts[0].reserved = 1u;
    obj_result.struct_size = sizeof(obj_result);
    failed |= check(api->rasterize_ppu_obj_parts(
                        runner, &obj_parts_request, &obj_result) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "invalid PPU OBJ part accepted");
    obj_parts[0].reserved = 0u;

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
    g_rtl_game_audio = &s_mix_test_audio_api;
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
    g_rtl_game_audio = NULL;
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
    obj_resolve_result.struct_size = sizeof(obj_resolve_result);
    failed |= check(api->resolve_ppu_obj_range(
                        runner, &obj_resolve_request,
                        &obj_resolve_result) == SR_RESULT_STALE_VIEW,
                    "stale PPU OBJ resolve request accepted");
    obj_result.struct_size = sizeof(obj_result);
    failed |= check(api->rasterize_ppu_obj_parts(
                        runner, &obj_parts_request, &obj_result) ==
                        SR_RESULT_STALE_VIEW,
                    "stale PPU OBJ parts raster request accepted");

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

    memcpy(snes->apu->ram + 0x0f48u,
           spc_control_request.expected_aram,
           spc_control_request.expected_aram_count);
    snes->apu->spc->pc = 0x0f10u;
    failed |= check(api->compare_exchange_spc_pc(
                        runner, &small_spc_control_request,
                        &spc_control_result) == SR_RESULT_INVALID_ARGUMENT,
                    "undersized SPC control request accepted");
    failed |= check(api->compare_exchange_spc_pc(
                        runner, &spc_control_request,
                        &small_spc_control_result) == SR_RESULT_INVALID_ARGUMENT,
                    "undersized SPC control result accepted");
    spc_control_request.expected_aram_count =
        SR_SPC_PC_EXPECTED_ARAM_MAX + 1u;
    failed |= check(api->compare_exchange_spc_pc(
                        runner, &spc_control_request,
                        &spc_control_result) == SR_RESULT_INVALID_ARGUMENT,
                    "oversized SPC control signature accepted");
    spc_control_request.expected_aram_count = 5u;
    spc_control_request.expected_aram_address = 0xfffeu;
    failed |= check(api->compare_exchange_spc_pc(
                        runner, &spc_control_request,
                        &spc_control_result) == SR_RESULT_INVALID_ARGUMENT,
                    "out-of-range SPC control signature accepted");
    spc_control_request.expected_aram_address = 0x0f48u;
    spc_control_request.expected_aram[0] ^= 0xffu;
    failed |= check(api->compare_exchange_spc_pc(
                        runner, &spc_control_request,
                        &spc_control_result) == SR_RESULT_OK &&
                        spc_control_result.flags == 0u &&
                        spc_control_result.observed_pc == 0x0f10u &&
                        spc_control_result.current_pc == 0x0f10u &&
                        snes->apu->spc->pc == 0x0f10u,
                    "SPC control changed state after an ARAM mismatch");
    failed |= check_generation(api, runner, 9u, 2u, 1u, 1u, 5u);
    spc_control_request.expected_aram[0] ^= 0xffu;
    failed |= check(api->compare_exchange_spc_pc(
                        runner, &spc_control_request,
                        &spc_control_result) == SR_RESULT_OK &&
                        spc_control_result.flags ==
                            (SR_SPC_PC_CONTROL_MATCHED |
                             SR_SPC_PC_CONTROL_WRITTEN) &&
                        spc_control_result.observed_pc == 0x0f10u &&
                        spc_control_result.current_pc == 0x0f48u &&
                        snes->apu->spc->pc == 0x0f48u,
                    "SPC control compare-and-set mismatch");
    failed |= check_generation(api, runner, 10u, 2u, 1u, 1u, 6u);

    failed |= check(api->configure_audio_mix(
                        runner, &small_audio_mix_control) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "undersized audio mix control accepted");
    audio_mix_control.music_gain_percent = 101u;
    failed |= check(api->configure_audio_mix(runner, &audio_mix_control) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "out-of-range audio mix gain accepted");
    audio_mix_control.music_gain_percent = 65u;
    audio_mix_control.unclassified_music_source_min = 12u;
    failed |= check(api->configure_audio_mix(runner, &audio_mix_control) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "unrequested audio source partition accepted");
    audio_mix_control.flags = SR_AUDIO_MIX_MUTE_MUSIC |
                              SR_AUDIO_MIX_PARTITION_UNCLASSIFIED_BY_SOURCE;
    failed |= check(api->configure_audio_mix(runner, &audio_mix_control) ==
                        SR_RESULT_OK,
                    "audio mix control failed");
    {
        int music_gain = -1;
        int sfx_gain = -1;
        dsp_getBusGains(&music_gain, &sfx_gain);
        dsp_setVoiceBus(snes->apu->dsp, 0, kDspVoiceBus_Music);
        dsp_setVoiceBus(snes->apu->dsp, 1, kDspVoiceBus_Unclassified);
        snes->apu->dsp->channel[1].srcn = 12u;
        dsp_refreshMixControls(snes->apu->dsp);
        failed |= check(music_gain == 65 && sfx_gain == 35 &&
                            snes->apu->dsp->voiceMuted[0] == 1u &&
                            snes->apu->dsp->voiceMuted[1] == 1u,
                        "audio mix mute/source-partition mismatch");
    }
    audio_mix_control.flags = 0u;
    audio_mix_control.unclassified_music_source_min = 0u;
    failed |= check(api->configure_audio_mix(runner, &audio_mix_control) ==
                        SR_RESULT_OK,
                    "audio mix policy reset failed");
    failed |= check_generation(api, runner, 10u, 2u, 1u, 1u, 6u);

    snes->ppu->vram[0x0123u] = 0x1111u;
    snes->ppu->vram[0x0456u] = 0x2222u;
    vram_patches[0] = (SrPpuVramWordPatch) {
        .word_address = 0x0123u,
        .expected = 0x1111u,
        .replacement = 0xaaaau,
    };
    vram_patches[1] = (SrPpuVramWordPatch) {
        .word_address = 0x0456u,
        .expected = 0x2222u,
        .replacement = 0xbbbbu,
    };
    failed |= check(api->query_generations(runner, &ppu_generation) ==
                        SR_RESULT_OK,
                    "PPU VRAM patch generation query failed");
    vram_patch_request.lifetime_generation =
        ppu_generation.lifetime_generation;
    failed |= check(api->compare_exchange_ppu_vram_words(
                        runner, &vram_patch_request) == SR_RESULT_OK &&
                        snes->ppu->vram[0x0123u] == 0xaaaau &&
                        snes->ppu->vram[0x0456u] == 0xbbbbu,
                    "atomic PPU VRAM patch failed");
    failed |= check(api->compare_exchange_ppu_vram_words(
                        runner, &vram_patch_request) ==
                            SR_RESULT_STALE_VIEW,
                    "stale PPU VRAM patch accepted");

    failed |= check(api->query_generations(runner, &ppu_generation) ==
                        SR_RESULT_OK,
                    "PPU VRAM restore generation query failed");
    vram_patch_request.lifetime_generation =
        ppu_generation.lifetime_generation;
    vram_patches[0].expected = 0xaaaau;
    vram_patches[0].replacement = 0x1111u;
    vram_patches[1].expected = 0xbbbbu;
    vram_patches[1].replacement = 0x2222u;
    failed |= check(api->compare_exchange_ppu_vram_words(
                        runner, &vram_patch_request) == SR_RESULT_OK &&
                        snes->ppu->vram[0x0123u] == 0x1111u &&
                        snes->ppu->vram[0x0456u] == 0x2222u,
                    "atomic PPU VRAM restore failed");

    failed |= check(api->query_generations(runner, &ppu_generation) ==
                        SR_RESULT_OK,
                    "sorted PPU VRAM patch generation query failed");
    vram_patch_request.flags = SR_PPU_VRAM_PATCH_ADDRESSES_SORTED;
    vram_patch_request.lifetime_generation =
        ppu_generation.lifetime_generation;
    vram_patches[0].expected = vram_patches[0].replacement = 0x1111u;
    vram_patches[1].expected = vram_patches[1].replacement = 0x2222u;
    failed |= check(api->compare_exchange_ppu_vram_words(
                        runner, &vram_patch_request) == SR_RESULT_OK,
                    "sorted PPU VRAM no-op patch failed");
    {
        SrPpuVramWordPatch temporary = vram_patches[0];
        vram_patches[0] = vram_patches[1];
        vram_patches[1] = temporary;
    }
    failed |= check(api->compare_exchange_ppu_vram_words(
                        runner, &vram_patch_request) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "unsorted PPU VRAM patch accepted as sorted");
    {
        SrPpuVramWordPatch temporary = vram_patches[0];
        vram_patches[0] = vram_patches[1];
        vram_patches[1] = temporary;
    }
    vram_patch_request.flags = 2u;
    failed |= check(api->compare_exchange_ppu_vram_words(
                        runner, &vram_patch_request) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "unknown PPU VRAM patch flag accepted");
    vram_patch_request.flags = 0u;

    failed |= check(api->query_generations(runner, &ppu_generation) ==
                        SR_RESULT_OK,
                    "PPU VRAM contention generation query failed");
    vram_patch_request.lifetime_generation =
        ppu_generation.lifetime_generation;
    vram_patches[0].expected = 0x1111u;
    vram_patches[0].replacement = 0x3333u;
    vram_patches[1].expected = 0xffffu;
    vram_patches[1].replacement = 0x4444u;
    failed |= check(api->compare_exchange_ppu_vram_words(
                        runner, &vram_patch_request) == SR_RESULT_BUSY &&
                        snes->ppu->vram[0x0123u] == 0x1111u &&
                        snes->ppu->vram[0x0456u] == 0x2222u,
                    "contended PPU VRAM patch was partially applied");
    vram_patches[1] = vram_patches[0];
    failed |= check(api->compare_exchange_ppu_vram_words(
                        runner, &vram_patch_request) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "duplicate PPU VRAM patch address accepted");
    vram_patches[1].word_address = 0x0456u;
    vram_patches[1].reserved = 1u;
    failed |= check(api->compare_exchange_ppu_vram_words(
                        runner, &vram_patch_request) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "reserved PPU VRAM patch field accepted");

    PpuSetObjExactPosition(snes->ppu, 9u, 90, 91);
    PpuSetObjCameraRelative(snes->ppu, 9u, true);
    snes->ppu->objScanlineMasksValid = true;
    obj_position_updates[0] = (SrPpuObjPositionUpdate) {
        .x = -40,
        .y = 230,
        .slot = 3u,
        .flags = SR_PPU_OBJ_POSITION_CAMERA_RELATIVE,
    };
    obj_position_updates[1] = (SrPpuObjPositionUpdate) {
        .x = 300,
        .y = -18,
        .slot = 7u,
    };
    failed |= check(api->query_generations(runner, &ppu_generation) ==
                        SR_RESULT_OK,
                    "PPU OBJ metadata generation query failed");
    obj_metadata_request.flags =
        SR_PPU_OBJ_METADATA_CLEAR_POSITIONS |
        SR_PPU_OBJ_METADATA_CLEAR_CAMERA_RELATIVE;
    obj_metadata_request.lifetime_generation =
        ppu_generation.lifetime_generation;
    failed |= check(api->update_ppu_obj_metadata(
                        runner, &obj_metadata_request) == SR_RESULT_OK &&
                        snes->ppu->objPosValid[3] == 1u &&
                        snes->ppu->objPosX[3] == -40 &&
                        snes->ppu->objPosY[3] == 230 &&
                        snes->ppu->objCameraRelative[3] &&
                        snes->ppu->objPosValid[7] == 1u &&
                        snes->ppu->objPosX[7] == 300 &&
                        snes->ppu->objPosY[7] == -18 &&
                        !snes->ppu->objCameraRelative[7] &&
                        snes->ppu->objPosValid[9] == 0u &&
                        !snes->ppu->objCameraRelative[9] &&
                        !snes->ppu->objScanlineMasksValid,
                    "batched PPU OBJ metadata update mismatch");
    failed |= check_generation(api, runner, 12u, 2u, 1u, 1u, 8u);

    obj_metadata_request.flags = 0u;
    obj_metadata_request.update_count = 1u;
    obj_position_updates[0] = (SrPpuObjPositionUpdate) {
        .x = 41,
        .y = 42,
        .slot = 7u,
    };
    failed |= check(api->update_ppu_obj_metadata(
                        runner, &obj_metadata_request) == SR_RESULT_OK &&
                        snes->ppu->objPosValid[3] == 1u &&
                        snes->ppu->objPosX[7] == 41 &&
                        snes->ppu->objPosY[7] == 42 &&
                        !snes->ppu->objCameraRelative[7],
                    "incremental PPU OBJ metadata update mismatch");

    obj_metadata_request.lifetime_generation++;
    failed |= check(api->update_ppu_obj_metadata(
                        runner, &obj_metadata_request) ==
                            SR_RESULT_STALE_VIEW &&
                        snes->ppu->objPosX[7] == 41,
                    "stale PPU OBJ metadata update accepted");
    obj_metadata_request.lifetime_generation--;

    obj_metadata_request.flags =
        SR_PPU_OBJ_METADATA_CLEAR_POSITIONS |
        SR_PPU_OBJ_METADATA_CLEAR_CAMERA_RELATIVE;
    obj_metadata_request.update_count = 2u;
    obj_position_updates[0].slot = 3u;
    obj_position_updates[1] = obj_position_updates[0];
    obj_position_updates[1].x = 99;
    failed |= check(api->update_ppu_obj_metadata(
                        runner, &obj_metadata_request) ==
                            SR_RESULT_INVALID_ARGUMENT &&
                        snes->ppu->objPosValid[3] == 1u &&
                        snes->ppu->objPosValid[7] == 1u,
                    "invalid PPU OBJ batch cleared existing metadata");
    obj_position_updates[1].slot = 128u;
    failed |= check(api->update_ppu_obj_metadata(
                        runner, &obj_metadata_request) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "out-of-range PPU OBJ slot accepted");
    obj_position_updates[1].slot = 8u;
    obj_position_updates[1].flags = 2u;
    failed |= check(api->update_ppu_obj_metadata(
                        runner, &obj_metadata_request) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "unknown PPU OBJ position flag accepted");
    obj_position_updates[1].flags = 0u;
    obj_position_updates[1].reserved = 1u;
    failed |= check(api->update_ppu_obj_metadata(
                        runner, &obj_metadata_request) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "reserved PPU OBJ position field accepted");
    obj_position_updates[1].reserved = 0u;
    obj_metadata_request.flags = 4u;
    failed |= check(api->update_ppu_obj_metadata(
                        runner, &obj_metadata_request) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "unknown PPU OBJ metadata request flag accepted");
    obj_metadata_request.flags = SR_PPU_OBJ_METADATA_CLEAR_POSITIONS;
    obj_metadata_request.updates = NULL;
    obj_metadata_request.update_count = 0u;
    failed |= check(api->update_ppu_obj_metadata(
                        runner, &obj_metadata_request) == SR_RESULT_OK &&
                        snes->ppu->objPosValid[3] == 0u &&
                        snes->ppu->objPosValid[7] == 0u,
                    "clear-only PPU OBJ metadata request failed");
    failed |= check(api->update_ppu_obj_metadata(
                        runner, &small_obj_metadata_request) ==
                            SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU OBJ metadata request accepted");

    snes->dma->dmaBusy = true;
    snes->dma->dmaTimer = 1234u;
    snes->dma->channel[2].hdmaActive = true;
    snes->dma->channel[2].indirect = true;
    snes->dma->channel[2].mode = 2u;
    snes->dma->channel[2].aAdr = 0x6800u;
    snes->dma->channel[2].aBank = 0x7eu;
    snes->dma->channel[2].bAdr = 0x0fu;
    snes->dma->channel[2].tableAdr = 0x6812u;
    snes->dma->channel[2].repCount = 0x82u;
    failed |= check(api->query_dma_state(runner, &dma_state) ==
                        SR_RESULT_OK &&
                        dma_state.struct_size ==
                            SR_DMA_STATE_SNAPSHOT_V2_SIZE &&
                        dma_state.lifetime_generation ==
                            snes->abiLifetimeGeneration &&
                        dma_state.flags == SR_DMA_STATE_BUSY &&
                        dma_state.timer == 1234u &&
                        dma_state.channel_count == SR_DMA_CHANNEL_COUNT &&
                        (dma_state.channels[2].flags &
                         (SR_DMA_CHANNEL_HDMA_ACTIVE |
                          SR_DMA_CHANNEL_INDIRECT)) ==
                            (SR_DMA_CHANNEL_HDMA_ACTIVE |
                             SR_DMA_CHANNEL_INDIRECT) &&
                        dma_state.channels[2].mode == 2u &&
                        dma_state.channels[2].a_address == 0x6800u &&
                        dma_state.channels[2].a_bank == 0x7eu &&
                        dma_state.channels[2].b_address == 0x0fu &&
                        dma_state.channels[2].table_address == 0x6812u &&
                        dma_state.channels[2].repeat_count == 0x82u,
                    "DMA state snapshot mismatch");
    failed |= check(api->query_dma_state(runner, &small_dma_state) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized DMA state output accepted");

    failed |= check(api->query_generations(runner, &ppu_generation) ==
                        SR_RESULT_OK,
                    "background-policy generation query failed");
    layer_extent_request.lifetime_generation =
        ppu_generation.lifetime_generation;
    layer_extent_updates[0] = (SrPpuLayerExtentUpdate) {
        .kind = SR_PPU_LAYER_EXTENT_DEFAULT,
        .layer = 1u,
        .left = 40u,
        .right = 52u,
        .top = 7u,
        .bottom = 9u,
    };
    layer_extent_updates[1] = (SrPpuLayerExtentUpdate) {
        .kind = SR_PPU_LAYER_EXTENT_HORIZONTAL_BAND,
        .layer = 1u,
        .y0 = 100u,
        .y1 = 120u,
        .left = SR_PPU_LAYER_EXTENT_AVAILABLE,
        .right = 3u,
    };
    failed |= check(api->update_ppu_layer_extents(
                        runner, &layer_extent_request) == SR_RESULT_OK &&
                        snes->ppu->wsLayerExtentLeftDefault[1] == 40u &&
                        snes->ppu->wsLayerExtentRightDefault[1] == 52u &&
                        snes->ppu->wsLayerExtentTop[1] == 7u &&
                        snes->ppu->wsLayerExtentBottom[1] == 9u &&
                        snes->ppu->wsLayerExtentLeft[1][99] == 40u &&
                        snes->ppu->wsLayerExtentLeft[1][100] == 0xffffu &&
                        snes->ppu->wsLayerExtentRight[1][119] == 3u &&
                        snes->ppu->wsLayerExtentRight[1][120] == 52u,
                    "PPU layer-extent batch mismatch");
    layer_extent_updates[0].left = 11u;
    layer_extent_updates[1].y1 = SR_PPU_NATIVE_HEIGHT + 1u;
    failed |= check(api->update_ppu_layer_extents(
                        runner, &layer_extent_request) ==
                            SR_RESULT_INVALID_ARGUMENT &&
                        snes->ppu->wsLayerExtentLeftDefault[1] == 40u,
                    "invalid PPU layer-extent batch partially applied");
    layer_extent_updates[1].y1 = 120u;
    layer_extent_request.lifetime_generation++;
    failed |= check(api->update_ppu_layer_extents(
                        runner, &layer_extent_request) ==
                            SR_RESULT_STALE_VIEW,
                    "stale PPU layer-extent batch accepted");
    layer_extent_request.lifetime_generation--;

    virtual_tilemap_request.lifetime_generation =
        ppu_generation.lifetime_generation;
    virtual_tilemap_request.layer_mask = 1u;
    virtual_tilemap_request.bindings[0] =
        (SrPpuVirtualTilemapBinding) {
            .lookup = test_virtual_tile_lookup,
            .lookup_span = test_virtual_tile_span_lookup,
            .band_lookup = test_virtual_tile_band_lookup,
            .user_data = &s_virtual_context,
            .camera_x = -20,
            .camera_y = 30,
            .hscroll_anchor = 0x3ffu,
            .vscroll_anchor = 0x123u,
            .flags = SR_PPU_VIRTUAL_TILEMAP_INCLUDE_AUTHENTIC,
        };
    failed |= check(api->replace_ppu_virtual_tilemaps(
                        runner, &virtual_tilemap_request) == SR_RESULT_OK &&
                        snes->ppu->virtualTilemap[0].lookup != NULL &&
                        snes->ppu->virtualTilemap[0].lookup_span != NULL &&
                        snes->ppu->virtualTilemap[0].band_lookup != NULL &&
                        snes->ppu->virtualTilemap[0].camera_x == -20 &&
                        snes->ppu->virtualTilemap[0].camera_y == 30 &&
                        snes->ppu->virtualTilemap[0].hscroll_anchor ==
                            0x3ffu &&
                        snes->ppu->virtualTilemap[1].lookup == NULL,
                    "PPU virtual-tilemap replacement mismatch");
    {
        uint16_t entry = 0u;
        uint8_t band = 0xffu;
        const uint16_t *entries = NULL;
        ptrdiff_t stride = 0;
        const PpuVirtualTilemapBinding *binding =
            &snes->ppu->virtualTilemap[0];
        failed |= check(binding->lookup(
                            binding->context, 5, 7, &entry) &&
                            entry == 0x0705u &&
                            binding->band_lookup(
                                binding->context, 5, 7, entry, &band) &&
                            band == 1u &&
                            binding->lookup_span(
                                binding->context, 8, 9, 1, 3u,
                                &entries, &stride) == 3u &&
                            entries == s_virtual_span_entries &&
                            stride == 1 && entries[2] == 0x090au,
                        "PPU virtual-tilemap callback bridge mismatch");
        failed |= check(binding->lookup(
                            binding->context, -1, 7, &entry) ==
                                kPpuVirtualTilemapLookup_FallbackAuthentic &&
                            binding->lookup(
                                binding->context, -2, 7, &entry) ==
                                kPpuVirtualTilemapLookup_Transparent,
                        "PPU virtual-tilemap lookup result bridge mismatch");
    }
    virtual_tilemap_request.layer_mask = 3u;
    failed |= check(api->replace_ppu_virtual_tilemaps(
                        runner, &virtual_tilemap_request) ==
                            SR_RESULT_INVALID_ARGUMENT &&
                        snes->ppu->virtualTilemap[0].lookup != NULL,
                    "invalid PPU virtual-tilemap replacement cleared prior binding");
    virtual_tilemap_request.layer_mask = 0u;
    failed |= check(api->replace_ppu_virtual_tilemaps(
                        runner, &virtual_tilemap_request) == SR_RESULT_OK &&
                        snes->ppu->virtualTilemap[0].lookup == NULL,
                    "PPU virtual-tilemap clear failed");

    for (uint32_t row = 0u; row < SR_PPU_NATIVE_HEIGHT; ++row) {
        authentic_bg1[row] = (uint16_t)(0x100u + row);
        authentic_bg2[row] = (uint16_t)(0x200u + row);
    }
    authentic_camera_request.flags = SR_PPU_AUTHENTIC_CAMERA_CLEAR;
    authentic_camera_request.lifetime_generation =
        ppu_generation.lifetime_generation;
    authentic_camera_request.layer_mask = SR_PPU_AUTHENTIC_CAMERA_ALL;
    authentic_camera_request.row_count = SR_PPU_NATIVE_HEIGHT;
    authentic_camera_request.bg1_hscroll = authentic_bg1;
    authentic_camera_request.bg2_hscroll = authentic_bg2;
    authentic_camera_request.object_offset_x = -77;
    failed |= check(api->update_ppu_authentic_camera(
                        runner, &authentic_camera_request) == SR_RESULT_OK &&
                        snes->ppu->authenticHScrollMask ==
                            SR_PPU_AUTHENTIC_CAMERA_ALL &&
                        snes->ppu->authenticHScroll[0][0] == 0x100u &&
                        snes->ppu->authenticHScroll[0][223] == 0x1dfu &&
                        snes->ppu->authenticHScroll[1][17] == 0x211u &&
                        snes->ppu->authenticObjOffsetX == -77,
                    "PPU authentic-camera replacement mismatch");
    authentic_camera_request.row_count--;
    failed |= check(api->update_ppu_authentic_camera(
                        runner, &authentic_camera_request) ==
                            SR_RESULT_INVALID_ARGUMENT &&
                        snes->ppu->authenticHScrollMask ==
                            SR_PPU_AUTHENTIC_CAMERA_ALL,
                    "invalid PPU authentic-camera request cleared prior state");
    authentic_camera_request = (SrPpuAuthenticCameraRequest) {
        .struct_size = sizeof(authentic_camera_request),
        .flags = SR_PPU_AUTHENTIC_CAMERA_CLEAR,
        .lifetime_generation = ppu_generation.lifetime_generation,
    };
    failed |= check(api->update_ppu_authentic_camera(
                        runner, &authentic_camera_request) == SR_RESULT_OK &&
                        snes->ppu->authenticHScrollMask == 0u &&
                        snes->ppu->authenticObjOffsetX == 0,
                    "PPU authentic-camera clear failed");

    failed |= test_public_vertical_margin_scanout(api, runner, snes);

    /* The synchronous scanout service owns the generic PPU/HDMA schedule.
     * Use one-line direct and indirect tables to update INIDISP and BGMODE,
     * then verify both callback phases and the recompiled CPU's IRQ seam. */
    PpuSetExtraSpace(snes->ppu, 0u);
    PpuSetExtraVerticalSpace(snes->ppu, 0, 0);
    snes->ppu->inidisp = 0x80u;
    dma_reset(snes->dma);
    wram[0x0100u] = 1u;
    wram[0x0101u] = 0x8fu;
    wram[0x0102u] = 0u;
    snes->dma->channel[2].aBank = 0x7eu;
    snes->dma->channel[2].aAdr = 0x0100u;
    snes->dma->channel[2].bAdr = 0u;
    snes->dma->channel[2].mode = 0u;
    snes->dma->channel[2].indirect = false;
    wram[0x0110u] = 1u;
    wram[0x0111u] = 0x20u;
    wram[0x0112u] = 0x01u;
    wram[0x0113u] = 0u;
    wram[0x0120u] = 0x09u;
    snes->dma->channel[3].aBank = 0x7eu;
    snes->dma->channel[3].aAdr = 0x0110u;
    snes->dma->channel[3].bAdr = 5u;
    snes->dma->channel[3].mode = 0u;
    snes->dma->channel[3].indirect = true;
    snes->dma->channel[3].indBank = 0x7eu;
    snes_writeReg(snes, 0x420cu, 0x0cu);
    snes->vIrqEnabled = true;
    snes->vTimer = 3u;
    scanout_observer.snes = snes;
    scanout_request.lifetime_generation = snes->abiLifetimeGeneration;
    {
        const bool even_frame = snes->ppu->evenFrame;
        failed |= check(api->run_ppu_scanout(
                            runner, &small_scanout_request,
                            &scanout_result) == SR_RESULT_INVALID_ARGUMENT &&
                            snes->ppu->evenFrame == even_frame,
                        "undersized PPU scanout request mutated state");
        failed |= check(api->run_ppu_scanout(
                            runner, &scanout_request,
                            &small_scanout_result) ==
                                SR_RESULT_INVALID_ARGUMENT &&
                            snes->ppu->evenFrame == even_frame,
                        "undersized PPU scanout result mutated state");
        ++scanout_request.lifetime_generation;
        failed |= check(api->run_ppu_scanout(
                            runner, &scanout_request,
                            &scanout_result) == SR_RESULT_STALE_VIEW &&
                            snes->ppu->evenFrame == even_frame,
                        "stale PPU scanout request mutated state");
        --scanout_request.lifetime_generation;
        scanout_request.flags = 1u;
        failed |= check(api->run_ppu_scanout(
                            runner, &scanout_request,
                            &scanout_result) == SR_RESULT_INVALID_ARGUMENT &&
                            snes->ppu->evenFrame == even_frame,
                        "unknown PPU scanout flag mutated state");
        scanout_request.flags = 0u;
    }
    scanout_result.struct_size = sizeof(scanout_result);
    failed |= check(api->run_ppu_scanout(
                        runner, &scanout_request, &scanout_result) ==
                            SR_RESULT_OK,
                    "PPU scanout failed");
    failed |= check(scanout_observer.valid &&
                        scanout_observer.before_count ==
                            SR_PPU_NATIVE_HEIGHT + 1u &&
                        scanout_observer.after_count ==
                            SR_PPU_NATIVE_HEIGHT + 1u &&
                        scanout_observer.irq_count == 1u &&
                        scanout_observer.irq_line == 4u &&
                        scanout_observer.saw_hdma_before &&
                        scanout_observer.saw_hdma_after &&
                        scanout_observer.saw_indirect_before &&
                        scanout_observer.saw_indirect_after,
                    "PPU scanout callback schedule mismatch");
    failed |= check(scanout_result.struct_size ==
                            SR_PPU_SCANOUT_RESULT_V2_SIZE &&
                        scanout_result.lifetime_generation ==
                            snes->abiLifetimeGeneration &&
                        (scanout_result.flags &
                         SR_PPU_SCANOUT_AUTHENTIC_SURFACE_READY) != 0u &&
                        scanout_result.final_state.struct_size ==
                            SR_PPU_STATE_SNAPSHOT_V2_SIZE &&
                        scanout_result.final_state.display_control == 0x8fu &&
                        scanout_result.final_state.bg_mode_control == 0x09u &&
                        (scanout_result.final_state.flags &
                         SR_PPU_STATE_FORCED_BLANK) != 0u &&
                        !snes->vIrqEnabled && !snes->inIrq &&
                        snes->dma->channel[2].hdmaActive &&
                        snes->dma->channel[3].hdmaActive,
                    "PPU scanout result mismatch");
    failed |= check_generation(api, runner, 12u, 2u, 1u, 1u, 8u);

    /* A raster handler may rewrite $4209 and re-arm the next V-IRQ in the
     * same scanout. Re-reading the timer after every callback is therefore a
     * hardware scheduling requirement, not a redundant reload. */
    {
        TestChainedIrqObserver chained_irq = {
            .snes = snes,
        };
        TestPhaseObserver phase_observer = {0};
        SrEventSubscription phase_subscription = {
            .struct_size = sizeof(phase_subscription),
            .event_mask = SR_EVENT_MASK_FRAME | SR_EVENT_MASK_INTERRUPT,
            .callback = observe_test_phase_event,
            .user_data = &phase_observer,
        };
        uint64_t phase_subscription_id = 0u;
        scanout_request.line_callback = NULL;
        scanout_request.irq_callback = observe_test_chained_ppu_irq;
        scanout_request.user_data = &chained_irq;
        dma_reset(snes->dma);
        snes_writeReg(snes, 0x420cu, 0u);
        snes->vIrqEnabled = true;
        snes->vTimer = 3u;
        scanout_result.struct_size = sizeof(scanout_result);
        failed |= check(api->subscribe_events(
                            runner, &phase_subscription,
                            &phase_subscription_id) == SR_RESULT_OK,
                        "scanout phase observer subscription failed");
        failed |= check(api->run_ppu_scanout(
                            runner, &scanout_request, &scanout_result) ==
                                SR_RESULT_OK &&
                            chained_irq.count == 4u &&
                            chained_irq.lines[0] == 4u &&
                            chained_irq.lines[1] == 8u &&
                            chained_irq.lines[2] == 13u &&
                            chained_irq.lines[3] == 21u &&
                            !snes->vIrqEnabled && !snes->inIrq,
                        "chained raster IRQ timer reload mismatch");
        failed |= check(
            phase_observer.count == 10u &&
                phase_observer.events[0].type ==
                    SR_EVENT_FRAME_BOUNDARY &&
                phase_observer.events[0].flags ==
                    (SR_EVENT_FRAME_BEGIN | SR_EVENT_FRAME_SCANOUT) &&
                strcmp(phase_observer.events[0].label,
                       "scanout-begin") == 0 &&
                phase_observer.events[1].type == SR_EVENT_INTERRUPT &&
                phase_observer.events[1].interrupt_kind ==
                    SR_INTERRUPT_IRQ &&
                phase_observer.events[1].interrupt_scanline == 4 &&
                phase_observer.events[1].flags ==
                    (SR_EVENT_INTERRUPT_ENTER |
                     SR_EVENT_INTERRUPT_CALLBACK) &&
                phase_observer.events[8].type == SR_EVENT_INTERRUPT &&
                phase_observer.events[8].interrupt_scanline == 21 &&
                phase_observer.events[8].flags ==
                    (SR_EVENT_INTERRUPT_EXIT |
                     SR_EVENT_INTERRUPT_CALLBACK) &&
                phase_observer.events[9].type ==
                    SR_EVENT_FRAME_BOUNDARY &&
                phase_observer.events[9].flags ==
                    (SR_EVENT_FRAME_END | SR_EVENT_FRAME_VBLANK |
                     SR_EVENT_FRAME_SCANOUT) &&
                strcmp(phase_observer.events[9].label,
                       "scanout-end") == 0,
            "scanout phase event sequence mismatch");
        failed |= check(api->unsubscribe_events(
                            runner, phase_subscription_id) == SR_RESULT_OK,
                        "scanout phase observer unsubscribe failed");
    }

    /* Scanout policy can suppress hardware-armed channels, but cannot mutate
     * $420C state or arm a channel the game left disabled. */
    scanout_request.line_callback = NULL;
    scanout_request.irq_callback = observe_test_ppu_scanout_irq;
    scanout_request.user_data = NULL;
    scanout_request.hdma_suppress_mask = 0x04u;
    snes_writeReg(snes, 0x420cu, 0x04u);
    snes->ppu->inidisp = 0x80u;
    scanout_result.struct_size = sizeof(scanout_result);
    failed |= check(api->run_ppu_scanout(
                        runner, &scanout_request, &scanout_result) ==
                            SR_RESULT_OK &&
                        snes->ppu->inidisp == 0x80u &&
                        snes->dma->channel[2].hdmaActive &&
                        !snes->dma->channel[3].hdmaActive,
                    "HDMA suppression changed hardware-owned channel state");
    scanout_request.hdma_suppress_mask = 0u;
    snes_writeReg(snes, 0x420cu, 0u);
    scanout_result.struct_size = sizeof(scanout_result);
    failed |= check(api->run_ppu_scanout(
                        runner, &scanout_request, &scanout_result) ==
                            SR_RESULT_OK &&
                        snes->ppu->inidisp == 0x80u &&
                        !snes->dma->channel[2].hdmaActive,
                    "zero scanout request armed a hardware-disabled channel");

    sr_runner_set_cpu_state_provider(snes, NULL, NULL, NULL);
    sr_runner_set_execution_state_provider(snes, NULL, NULL);
    sr_runner_bind_ppu_services(snes, false);
    snes_free(snes);
    memset(wram, 0, sizeof(wram));
    return failed;
}
