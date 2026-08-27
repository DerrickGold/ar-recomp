#include "ar_trace.h"
#include "cpu_state.h"
#include "runner_next_internal.h"
#include "snes/snes.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef AR_TRACE_TEST_PATH
#define AR_TRACE_TEST_PATH "runtime-next-ar-trace.jsonl"
#endif

unsigned char g_ram[0x20000];
const char *g_last_recomp_func;
uint32_t g_ar_blk_ring[1024];
unsigned g_ar_blk_idx;
CpuState g_cpu;
static int failures;

static void check(bool condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "runtime-next AR trace failed: %s\n", message);
    ++failures;
}

int main(void) {
    Snes runner = {0};
    SrRunnerEvent dma_event = {0};
    const int channels = AR_TR_FUNC | AR_TR_REG | AR_TR_DMA | AR_TR_WRAM |
                         AR_TR_STACK | AR_TR_FRAME | AR_TR_PPUMEM |
                         AR_TR_DISPMISS;
    ar_trace_bind_runner(&runner, 1);
    check(ar_trace_open_file(AR_TRACE_TEST_PATH, channels, 7, 7) == 1,
          "open explicit trace file");
    snes_frame_counter = 6;
    check(!ar_trace_active(), "lower frame bound");
    snes_frame_counter = 7;
    g_ram[0x88] = 0x34u;
    g_ram[0x89] = 0x12u;
    g_cpu.S = 0x01f0u;
    g_cpu.DB = 0x7eu;
    g_cpu.PB = 0x02u;
    g_cpu.m_flag = 1u;
    g_cpu.x_flag = 0u;
    g_last_recomp_func = "fn\"escaped";
    g_ar_blk_idx = 1u;
    g_ar_blk_ring[0] = 0x123456u;
    check(ar_trace_active() && ar_trace_ch(AR_TR_DMA) &&
          !ar_trace_ch(AR_TR_VRAM), "active channel mask");
    runner.abiFrameCounter = 7u;
    check((g_sr_runner_event_mask &
           (SR_EVENT_MASK_MEMORY_WRITE | SR_EVENT_MASK_REGISTER_ACCESS |
            SR_EVENT_MASK_DMA | SR_EVENT_MASK_FRAME |
            SR_EVENT_MASK_INTERRUPT | SR_EVENT_MASK_ERROR)) ==
              (SR_EVENT_MASK_MEMORY_WRITE | SR_EVENT_MASK_REGISTER_ACCESS |
               SR_EVENT_MASK_DMA | SR_EVENT_MASK_FRAME |
               SR_EVENT_MASK_INTERRUPT | SR_EVENT_MASK_ERROR),
          "trace observer subscriptions");
    ar_trace_func(0x029abcu, "entry", 1, 0, 1, 0);
    sr_runner_emit_register_access(&runner, true, 0x2100u, 0x8fu, 1u);
    dma_event.type = SR_EVENT_DMA_BEGIN;
    dma_event.address = 0x7e1234u;
    dma_event.dma_a_address24 = 0x7e1234u;
    dma_event.dma_transfer_bytes = 256u;
    dma_event.dma_channel = 2u;
    dma_event.dma_mode = 1u;
    dma_event.dma_b_address = 0x18u;
    sr_runner_emit_event(&runner, SR_EVENT_MASK_DMA, &dma_event);
    sr_runner_emit_memory_write(
        &runner, SR_MEMORY_WRAM, 0x01f0u, 0x0011u, 0x0022u, 1u);
    sr_runner_emit_memory_write(
        &runner, SR_MEMORY_CGRAM, 3u, 0u, 0x7fffu, 2u);
    sr_runner_emit_frame_boundary(
        &runner, SR_EVENT_FRAME_BEGIN | SR_EVENT_FRAME_VBLANK, "vblank");
    sr_runner_emit_interrupt(
        &runner, SR_INTERRUPT_NMI, SR_EVENT_INTERRUPT_ENTER, 0x123456u,
        0xffeau, SR_INTERRUPT_SCANLINE_UNKNOWN, "nmi");
    sr_runner_emit_error(
        &runner, SR_RUNNER_ERROR_DISPATCH_MISS, SR_EVENT_ERROR_RECOVERABLE,
        0x345678u, 0x123456u, "dispatch-miss");
    ar_trace_close();
    check(g_sr_runner_event_mask == 0u, "trace observer cleanup");

    FILE *file = fopen(AR_TRACE_TEST_PATH, "rb");
    char contents[8192];
    const size_t count = file == NULL ? 0u :
        fread(contents, 1u, sizeof(contents) - 1u, file);
    if (file != NULL) fclose(file);
    contents[count] = '\0';
    check(strstr(contents, "\"gf\":4660") != NULL &&
          strstr(contents, "\"blk\":\"123456\"") != NULL &&
          strstr(contents, "fn\\\"escaped") != NULL,
          "prefix state and JSON escaping");
    check(strstr(contents, "\"ch\":\"func\"") != NULL &&
          strstr(contents, "\"ch\":\"reg\"") != NULL &&
          strstr(contents, "\"ch\":\"dma\"") != NULL &&
          strstr(contents, "\"ch\":\"stack\"") != NULL &&
          strstr(contents, "\"ch\":\"wram\"") != NULL &&
          strstr(contents, "\"ch\":\"ppumem\"") != NULL &&
          strstr(contents, "\"ch\":\"frame\"") != NULL &&
          strstr(contents, "\"ch\":\"dispmiss\"") != NULL,
          "selected channels emit correlated JSONL records");
    check(remove(AR_TRACE_TEST_PATH) == 0, "remove trace fixture");
    if (failures != 0) {
        fprintf(stderr, "runtime-next AR trace: %d failure(s)\n", failures);
        return 1;
    }
    puts("runtime-next AR trace: PASS");
    return 0;
}
