/*
 * End-to-end conformance test for the linked-game contract.
 *
 * `examples/minimal_game` documents the contract but intentionally does not
 * link or run, so it cannot catch a project that satisfies the compiler and
 * still cannot work. Every assertion below corresponds to a real mistake made
 * while bringing up a second game against this runner, where the only feedback
 * was a black screen, a dead controller, or a linker error naming an
 * unfamiliar symbol.
 *
 * The ROM is synthesized here, so the test needs no game data.
 */

#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/required_symbols.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game_runtime.h"
#include "snesrecomp/runner.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int check(int condition, const char *message) {
    if (condition) return 0;
    fprintf(stderr, "conformance failed: %s\n", message);
    return 1;
}

/* ---- the generated half, stubbed ------------------------------------- */
/* A host that links the runner without recompiled output still has to satisfy
 * the dispatch table; an empty one is valid. */
const DispatchEntry g_dispatch_table[1] = {{0u, {NULL, NULL, NULL, NULL}}};
const unsigned g_dispatch_table_count = 0u;

/* ---- the game half --------------------------------------------------- */
/* The only two APU symbols the game owes the runner. Deliberately not
 * tests/apu_sync_stub.c: that also defines RtlApuWrite and
 * rtl_accumulate_apu_catchup, which the runner itself implements, so linking
 * it against the full runtime is a duplicate-symbol error. */
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static unsigned g_frames_run;
static unsigned g_draws;
static unsigned g_scanouts;
static const SnesRunnerApi *g_runner_api;
static SrRunnerHandle *g_runner;
static SrResult g_scanout_result = SR_RESULT_UNAVAILABLE;
static uint8_t g_scanout_display_control;

static void ConformanceIrq(void *user_data, uint32_t line) {
    (void)user_data;
    (void)line;
}

static void ConformanceRunFrame(void) { ++g_frames_run; }
static void ConformanceDrawFrame(void) {
    SrGenerationSnapshot generations = {
        .struct_size = SR_GENERATION_SNAPSHOT_V2_SIZE,
    };
    SrPpuScanoutRequest request = {
        .struct_size = SR_PPU_SCANOUT_REQUEST_V2_SIZE,
        .irq_callback = ConformanceIrq,
    };
    SrPpuScanoutResult result = {
        .struct_size = SR_PPU_SCANOUT_RESULT_V2_SIZE,
    };
    ++g_draws;
    if (g_runner_api != NULL && g_runner != NULL) {
        if (g_runner_api->query_generations(g_runner, &generations) !=
            SR_RESULT_OK)
            return;
        /* RtlRunFrame invalidates borrowed views and advances the lifetime
         * generation, so frame-scoped requests must use a fresh snapshot. */
        request.lifetime_generation = generations.lifetime_generation;
        g_scanout_result =
            g_runner_api->run_ppu_scanout(g_runner, &request, &result);
        if (g_scanout_result == SR_RESULT_OK) {
            ++g_scanouts;
            g_scanout_display_control = result.final_state.display_control;
        }
    }
}

static const RtlGameIdentity kIdentity = {
    .struct_size = RTL_GAME_IDENTITY_V1_SIZE,
    .game_id = "conformance",
    .display_name = "Conformance",
    .save_name_prefix = "conformance",
};

static const RtlGameExecutionApi kExecution = {
    .struct_size = RTL_GAME_EXECUTION_API_V2_SIZE,
    .run_frame = ConformanceRunFrame,
    .draw_ppu_frame = ConformanceDrawFrame,
};

static const RtlGameModule kModule = {
    .abi_version = RTL_GAME_MODULE_ABI_VERSION,
    .struct_size = RTL_GAME_MODULE_V2_SIZE,
    .capabilities = RTL_GAME_MODULE_CAP_IDENTITY | RTL_GAME_MODULE_CAP_EXECUTION,
    .identity = &kIdentity,
    .execution = &kExecution,
};

/* ---- a synthetic 32 KiB LoROM ---------------------------------------- */
enum { kRomBytes = 32768, kHeaderBase = 0x7FB0 };
static uint8_t g_rom_image[kRomBytes];

static void BuildRom(void) {
    uint8_t *header = g_rom_image + kHeaderBase;
    unsigned sum = 0;
    unsigned index;

    memset(g_rom_image, 0x00, sizeof g_rom_image);
    memset(header + 0x10, ' ', 21);
    memcpy(header + 0x10, "CONFORMANCE", 11);
    header[0x25] = 0x20; /* LoROM, slow */
    header[0x26] = 0x00; /* no coprocessor */
    header[0x27] = 0x05; /* 32 KiB */
    header[0x28] = 0x00; /* no cartridge RAM */
    header[0x29] = 0x01; /* USA */
    header[0x2A] = 0x33;
    header[0x2B] = 0x00;
    /* Reset vector; the test never executes it, but analysis reads it. */
    header[0x4C] = 0x00;
    header[0x4D] = 0x80;

    header[0x2C] = header[0x2D] = header[0x2E] = header[0x2F] = 0x00;
    for (index = 0; index < kRomBytes; ++index) sum += g_rom_image[index];
    sum &= 0xFFFFu;
    header[0x2E] = (uint8_t)(sum & 0xFFu);
    header[0x2F] = (uint8_t)(sum >> 8);
    header[0x2C] = (uint8_t)(~sum & 0xFFu);
    header[0x2D] = (uint8_t)((~sum >> 8) & 0xFFu);
}

/* ---- assertions ------------------------------------------------------ */
enum { kCanvasWidth = SR_PPU_NATIVE_WIDTH, kCanvasHeight = SR_PPU_NATIVE_HEIGHT };
static uint32_t g_canvas[kCanvasWidth * kCanvasHeight];

int main(void) {
    const SnesRunnerApi *api;
    SrRunnerHandle *runner;
    SrGenerationSnapshot generations = {
        .struct_size = SR_GENERATION_SNAPSHOT_V2_SIZE,
    };
    SrPpuOutputBindingRequest binding = {
        .struct_size = SR_PPU_OUTPUT_BINDING_REQUEST_V2_SIZE,
    };
    SrInputStateSnapshot input = {
        .struct_size = SR_INPUT_STATE_SNAPSHOT_V2_SIZE,
    };
    SrDmaStateSnapshot dma = {
        .struct_size = SR_DMA_STATE_SNAPSHOT_V2_SIZE,
    };
    int failed = 0;

    failed |= check(strcmp(sr_result_string(SR_RESULT_OK), "ok") == 0,
                    "sr_result_string(OK)");
    failed |= check(strcmp(sr_result_string(SR_RESULT_INVALID_ARGUMENT),
                           "invalid-argument") == 0,
                    "sr_result_string(INVALID_ARGUMENT)");

    BuildRom();
    failed |= check(RtlRegisterGame(&kModule) == SR_RESULT_OK,
                    "RtlRegisterGame rejected a minimal valid module");
    failed |= check(SnesInit(g_rom_image, (int)sizeof g_rom_image) != NULL,
                    "SnesInit failed on a synthetic LoROM image");

    runner = RtlGameRunner();
    api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
    failed |= check(runner != NULL, "RtlGameRunner returned NULL after SnesInit");
    failed |= check(api != NULL, "sr_runner_get_api returned NULL");
    if (runner == NULL || api == NULL) return 1;

    failed |= check(api->query_generations(runner, &generations) == SR_RESULT_OK,
                    "query_generations failed");
    g_runner_api = api;
    g_runner = runner;

    /* The main output surface rejects a non-zero scale. Only the Mode-7
     * surface is scalable, and passing 1 "because it is unscaled" is the
     * natural wrong guess. */
    binding.lifetime_generation = generations.lifetime_generation;
    binding.kind = SR_PPU_OUTPUT_MAIN;
    binding.scale = 1u;
    binding.pixels = (uint8_t *)g_canvas;
    binding.pitch_bytes = (uint64_t)kCanvasWidth * sizeof(uint32_t);
    binding.height_pixels = kCanvasHeight;
    binding.pixel_byte_size = binding.pitch_bytes * kCanvasHeight;
    failed |= check(
        api->bind_ppu_output_surface(runner, &binding) ==
            SR_RESULT_INVALID_ARGUMENT,
        "SR_PPU_OUTPUT_MAIN accepted a non-zero scale");

    binding.scale = 0u;
    failed |= check(
        api->bind_ppu_output_surface(runner, &binding) == SR_RESULT_OK,
        "SR_PPU_OUTPUT_MAIN rejected a correctly formed binding");

    /*
     * Input packing. RtlRunFrame takes 12 bits per controller and the runner
     * reverses all 16 bits on each register read, so argument bit N surfaces
     * as joypad bit 15-N. Start is argument bit 3 and joypad bit 12.
     */
    {
        const uint32_t start_bit = 1u << 3;
        (void)RtlRunFrame(start_bit);
        failed |= check(api->query_input_state(runner, &input) == SR_RESULT_OK,
                        "query_input_state failed");
        failed |= check(input.packed_buttons[0] == start_bit,
                        "packed_buttons did not echo the host argument");
        failed |= check(input.auto_joypad[0] == (1u << 12),
                        "Start (argument bit 3) did not surface as joypad "
                        "bit 12");
    }

    failed |= check(g_frames_run > 0u, "RtlRunFrame never invoked run_frame");

    /* The zero-initialized scanout request follows $420C. The request does
     * not need to mirror hardware state back into the runner. */
    g_ram[0x0100u] = 1u;
    g_ram[0x0101u] = 0x0fu;
    g_ram[0x0102u] = 0u;
    WriteReg(0x4300u, 0u);
    WriteReg(0x4301u, 0u);
    WriteReg(0x4302u, 0u);
    WriteReg(0x4303u, 1u);
    WriteReg(0x4304u, 0x7eu);
    WriteReg(0x420cu, 1u);
    failed |= check(RtlGameDrawPpuFrame(), "RtlGameDrawPpuFrame reported no "
                                           "draw_ppu_frame callback");
    failed |= check(g_draws > 0u, "draw_ppu_frame was not invoked");
    if (g_scanout_result != SR_RESULT_OK)
        fprintf(stderr, "conformance scanout result: %s\n",
                sr_result_string(g_scanout_result));
    failed |= check(g_scanouts > 0u,
                    "draw_ppu_frame did not drive PPU scanout");
    failed |= check(g_scanout_display_control == 0x0fu,
                    "zero-initialized scanout ignored hardware-armed HDMA");
    failed |= check(api->query_dma_state(runner, &dma) == SR_RESULT_OK &&
                        (dma.channels[0].flags &
                         SR_DMA_CHANNEL_HDMA_ACTIVE) != 0u,
                    "scanout mutated the hardware-owned $420C state");

    SnesShutdown();
    return failed;
}
