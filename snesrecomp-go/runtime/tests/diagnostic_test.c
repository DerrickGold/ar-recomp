#include "diagnostic.h"
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/generated_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int snes_frame_counter;
uint8 g_ram[kSnesWramSize];
const char *g_last_recomp_func = "diagnostic-test";
const char *g_recomp_stack[64];
int g_recomp_stack_top;
uint32 g_sr_block_ring[kRuntimeBlockTraceRingCapacity];
uint32 g_sr_block_aux[kRuntimeBlockTraceRingCapacity];
uint16 g_sr_block_stack[kRuntimeBlockTraceRingCapacity];
unsigned g_sr_block_index;

int sr_trace_active(void) { return 0; }
void sr_trace_garbage(uint32 pc24, const char *name, int m, int x) {
    (void)pc24; (void)name; (void)m; (void)x;
}
void RecompStackDump(void) {}

static int failures;

static void check(int condition, const char *message) {
    if (condition) return;
    ++failures;
    fprintf(stderr, "runtime diagnostic failed: %s\n", message);
}

static void test_histograms(void) {
    unsigned index;
    sr_diagnostic_reset();
    for (index = 0u; index < 65u; ++index) {
        sr_mx_history_record(0x018000u, 1, 1);
    }
    sr_mx_history_record(0x018000u, 0, 1);
    check(sr_mx_history_count(0x018000u, 1, 1) == 65u,
          "dominant M/X histogram count");
    check(sr_mx_history_count(0x018000u, 0, 1) == 1u,
          "minority M/X histogram count");
    check(sr_mx_history_count(0x028000u, 0, 0) == 0u,
          "missing M/X histogram entry");
}

static void test_block_history(void) {
    uint32 pc[4];
    uint32 aux[4];
    uint16 stack[4];
    unsigned index;
    for (index = 0u; index < kRuntimeBlockTraceRingCapacity + 2u; ++index) {
        unsigned slot = index & kRuntimeBlockTraceRingMask;
        g_sr_block_ring[slot] = 0x100000u + index;
        g_sr_block_aux[slot] = 0x200000u + index;
        g_sr_block_stack[slot] = (uint16)index;
    }
    g_sr_block_index = kRuntimeBlockTraceRingCapacity + 2u;
    check(sr_block_history_with_stack(pc, aux, stack, 4) == 4,
          "three-channel block history count");
    check(pc[0] == 0x1003feu && pc[3] == 0x100401u &&
          aux[0] == 0x2003feu && stack[3] == 0x0401u,
          "three-channel block history wrap and order");
    memset(pc, 0, sizeof(pc));
    check(sr_block_history_with_aux(pc, aux, 2) == 2 &&
          pc[0] == 0x100400u && pc[1] == 0x100401u,
          "two-channel block history window");
}

static void test_xtrace_and_warnings(void) {
    CpuState cpu;
    unsigned index;
    memset(&cpu, 0, sizeof(cpu));
    sr_diagnostic_reset();
    for (index = 0u; index < 600u; ++index) {
        sr_x_transition_trace_record(index, index + 1u, index & 1u, 1, index / 2u);
    }
    check(sr_x_transition_trace_count() == 512u, "X-transition ring capacity");
    check(sr_dispatch_oob_warn(&cpu, 0x018123u, 9u) ==
              RECOMP_RETURN_NORMAL,
          "dispatch OOB remains a soft diagnostic");
    check(sr_unresolved_indirect_jump(&cpu, 0x018200u) ==
              RECOMP_RETURN_NORMAL,
          "unresolved indirect jump remains a soft diagnostic");
    check(sr_unresolved_indirect_jump(&cpu, 0x018200u) ==
              RECOMP_RETURN_NORMAL,
          "repeated unresolved indirect jump remains soft");
    check(sr_unresolved_stub_warn(&cpu, 0x011234u, "stub") ==
              RECOMP_RETURN_NORMAL,
          "unresolved stub remains a soft diagnostic");
    check(sr_unresolved_goto_warn(&cpu, 0x018300u, 0x018400u,
                                  "source", "target") ==
              RECOMP_RETURN_NORMAL,
          "unresolved goto remains a soft diagnostic");
    check(sr_diagnostic_trap_warning_count() == 3u,
          "unresolved diagnostics deduplicate sites and targets");
    sr_garbage_variant_trap(&cpu, "bad-variant", 0x018123u);
    sr_diagnostic_reset();
    check(sr_diagnostic_trap_warning_count() == 0u,
          "diagnostic reset clears once-per-site traps");
}

static void test_stack_trace_window(void) {
#ifdef _WIN32
    _putenv_s("SNESRECOMP_STACK_TRACE", "1");
    _putenv_s("SNESRECOMP_STACK_TRACE_LOW", "03B210");
    _putenv_s("SNESRECOMP_STACK_TRACE_HIGH", "03B230");
#else
    setenv("SNESRECOMP_STACK_TRACE", "1", 1);
    setenv("SNESRECOMP_STACK_TRACE_LOW", "03B210", 1);
    setenv("SNESRECOMP_STACK_TRACE_HIGH", "03B230", 1);
#endif
    sr_diagnostic_reset();
    g_sr_block_index = 1u;
    g_sr_block_ring[0] = 0x03b220u;
    check(sr_stack_trace_active(), "stack trace activates inside configured PC window");
    sr_stack_trace_block(0x03b220u, 0x01f0u, 1, 0);
    sr_stack_trace_operation("push", 0x01f0u, 0x42u, 0x01efu);
    g_sr_block_ring[0] = 0x03b240u;
    check(!sr_stack_trace_active(), "stack trace rejects PCs outside configured window");
#ifdef _WIN32
    _putenv_s("SNESRECOMP_STACK_TRACE", "");
    _putenv_s("SNESRECOMP_STACK_TRACE_LOW", "");
    _putenv_s("SNESRECOMP_STACK_TRACE_HIGH", "");
#else
    unsetenv("SNESRECOMP_STACK_TRACE");
    unsetenv("SNESRECOMP_STACK_TRACE_LOW");
    unsetenv("SNESRECOMP_STACK_TRACE_HIGH");
#endif
    sr_diagnostic_reset();
}

int main(void) {
    test_histograms();
    test_block_history();
    test_xtrace_and_warnings();
    test_stack_trace_window();
    check(!sr_stack_trace_active(), "stack trace disabled by default");
    check(sr_vram_watch(0x1234u, 0x56u) == 0,
          "VRAM watcher disabled by default");
    if (failures != 0) return 1;
    puts("runtime diagnostic: PASS");
    return 0;
}
