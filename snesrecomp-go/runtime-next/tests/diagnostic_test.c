#include "diagnostic.h"
#include "common_cpu_infra.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int snes_frame_counter;
uint8 g_ram[kSnesWramSize];
const char *g_last_recomp_func = "diagnostic-test";
const char *g_recomp_stack[64];
int g_recomp_stack_top;
uint32 g_ar_blk_ring[kRuntimeBlockTraceRingCapacity];
uint32 g_ar_blk_aux[kRuntimeBlockTraceRingCapacity];
uint16 g_ar_blk_s[kRuntimeBlockTraceRingCapacity];
unsigned g_ar_blk_idx;

int ar_trace_active(void) { return 0; }
void ar_trace_garbage(uint32 pc24, const char *name, int m, int x) {
    (void)pc24; (void)name; (void)m; (void)x;
}
void RecompStackDump(void) {}

static int failures;

static void check(int condition, const char *message) {
    if (condition) return;
    ++failures;
    fprintf(stderr, "runtime-next diagnostic failed: %s\n", message);
}

static void test_histograms(void) {
    unsigned index;
    ar_diagnostic_reset();
    for (index = 0u; index < 65u; ++index) {
        ar_mxhist_record(0x018000u, 1, 1);
    }
    ar_mxhist_record(0x018000u, 0, 1);
    check(ar_mxhist_count(0x018000u, 1, 1) == 65u,
          "dominant M/X histogram count");
    check(ar_mxhist_count(0x018000u, 0, 1) == 1u,
          "minority M/X histogram count");
    check(ar_mxhist_count(0x028000u, 0, 0) == 0u,
          "missing M/X histogram entry");
}

static void test_block_history(void) {
    uint32 pc[4];
    uint32 aux[4];
    uint16 stack[4];
    unsigned index;
    for (index = 0u; index < kRuntimeBlockTraceRingCapacity + 2u; ++index) {
        unsigned slot = index & kRuntimeBlockTraceRingMask;
        g_ar_blk_ring[slot] = 0x100000u + index;
        g_ar_blk_aux[slot] = 0x200000u + index;
        g_ar_blk_s[slot] = (uint16)index;
    }
    g_ar_blk_idx = kRuntimeBlockTraceRingCapacity + 2u;
    check(ar_block_history3(pc, aux, stack, 4) == 4,
          "three-channel block history count");
    check(pc[0] == 0x1003feu && pc[3] == 0x100401u &&
          aux[0] == 0x2003feu && stack[3] == 0x0401u,
          "three-channel block history wrap and order");
    memset(pc, 0, sizeof(pc));
    check(ar_block_history2(pc, aux, 2) == 2 &&
          pc[0] == 0x100400u && pc[1] == 0x100401u,
          "two-channel block history window");
}

static void test_xtrace_and_warnings(void) {
    CpuState cpu;
    unsigned index;
    memset(&cpu, 0, sizeof(cpu));
    ar_diagnostic_reset();
    for (index = 0u; index < 600u; ++index) {
        ar_xtrace_record(index, index + 1u, index & 1u, 1, index / 2u);
    }
    check(ar_xtrace_count() == 512u, "X-transition ring capacity");
    check(ar_dispatch_oob_warn(&cpu, 0x018123u, 9u) ==
              RECOMP_RETURN_NORMAL,
          "dispatch OOB remains a soft diagnostic");
    ar_garbage_variant_trap(&cpu, "bad-variant", 0x018123u);
}

static void test_stack_trace_window(void) {
#ifdef _WIN32
    _putenv_s("AR_STRACE", "1");
    _putenv_s("AR_STRACE_LO", "03B210");
    _putenv_s("AR_STRACE_HI", "03B230");
#else
    setenv("AR_STRACE", "1", 1);
    setenv("AR_STRACE_LO", "03B210", 1);
    setenv("AR_STRACE_HI", "03B230", 1);
#endif
    ar_diagnostic_reset();
    g_ar_blk_idx = 1u;
    g_ar_blk_ring[0] = 0x03b220u;
    check(ar_strace_active(), "stack trace activates inside configured PC window");
    ar_strace_block(0x03b220u, 0x01f0u, 1, 0);
    ar_strace_op("push", 0x01f0u, 0x42u, 0x01efu);
    g_ar_blk_ring[0] = 0x03b240u;
    check(!ar_strace_active(), "stack trace rejects PCs outside configured window");
#ifdef _WIN32
    _putenv_s("AR_STRACE", "");
    _putenv_s("AR_STRACE_LO", "");
    _putenv_s("AR_STRACE_HI", "");
#else
    unsetenv("AR_STRACE");
    unsetenv("AR_STRACE_LO");
    unsetenv("AR_STRACE_HI");
#endif
    ar_diagnostic_reset();
}

int main(void) {
    test_histograms();
    test_block_history();
    test_xtrace_and_warnings();
    test_stack_trace_window();
    check(!ar_strace_active(), "stack trace disabled by default");
    check(ar_vramwatch(0x1234u, 0x56u) == 0,
          "VRAM watcher disabled by default");
    if (failures != 0) return 1;
    puts("runtime-next diagnostic: PASS");
    return 0;
}
