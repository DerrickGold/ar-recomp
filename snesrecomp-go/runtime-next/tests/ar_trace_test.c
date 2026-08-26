#include "ar_trace.h"
#include "cpu_state.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef AR_TRACE_TEST_PATH
#define AR_TRACE_TEST_PATH "runtime-next-ar-trace.jsonl"
#endif

int snes_frame_counter;
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
    const int channels = AR_TR_FUNC | AR_TR_REG | AR_TR_DMA | AR_TR_WRAM |
                         AR_TR_STACK | AR_TR_FRAME | AR_TR_PPUMEM;
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
    ar_trace_func(0x029abcu, "entry", 1, 0, 1, 0);
    ar_trace_reg(0x2100u, 0x8fu);
    ar_trace_dma(2, 0x18u, 0x7eu, 0x1234u, 256u, 0);
    ar_trace_wram(0x01f0u, 0x0011u, 0x0022u, 1);
    ar_trace_ppumem("cgram", 3u, 0x7fffu);
    ar_trace_frame("nmi");
    ar_trace_close();

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
          strstr(contents, "\"ch\":\"frame\"") != NULL,
          "selected channels emit correlated JSONL records");
    check(remove(AR_TRACE_TEST_PATH) == 0, "remove trace fixture");
    if (failures != 0) {
        fprintf(stderr, "runtime-next AR trace: %d failure(s)\n", failures);
        return 1;
    }
    puts("runtime-next AR trace: PASS");
    return 0;
}
