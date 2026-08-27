#include "snesrecomp/game/trace.h"
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/generated_support.h"

#include <stdio.h>
#include <string.h>

int snes_frame_counter;
CpuState g_cpu;
const char *g_last_recomp_func = "trace-test";
int g_recomp_stack_top;

RecompReturn sr_dispatch_oob_warn(CpuState *cpu, uint32 pc24, uint16 index) {
    (void)cpu; (void)pc24; (void)index;
    return RECOMP_RETURN_SKIP_1;
}

static int failures;
static void check(int condition, const char *message) {
    if (condition) return;
    ++failures;
    fprintf(stderr, "runtime CPU trace failed: %s\n", message);
}

static void test_events(void) {
    SrCpuTraceRecord records[4];
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.A = 0x1234u;
    cpu.X = 0x5678u;
    cpu.S = 0x01f0u;
    cpu.DB = 0x7eu;
    cpu.m_flag = 1u;
    snes_frame_counter = 42;
    check(cpu_trace_init() >= 1024u, "trace allocation");
    cpu_trace_event(&cpu, 0xaa123456u, CPU_TR_BLOCK, 7u, 0x8899u);
    cpu_trace_db_change(&cpu, 0x018000u, 0x01u, 0x7eu,
                        CPU_TR_DB_WRITE);
    cpu_trace_stack_op(&cpu, 0x018001u, CPU_STACK_OP_PHA, 0x01f1u, -1);
    check(sr_cpu_trace_count() == 3u, "trace event count");
    check(sr_cpu_trace_copy(records, 4) == 3, "trace copy count");
    check(records[0].pc24 == 0x123456u && records[0].A == 0x1234u &&
          records[0].X == 0x5678u && records[0].S == 0x01f0u &&
          records[0].DB == 0x7eu && records[0].frame == 42 &&
          records[0].extra0 == 7u && records[0].extra1 == 0x8899u,
          "trace register snapshot");
    check(records[1].event_type == CPU_TR_DB_WRITE &&
          records[1].extra0 == 0x01u && records[1].extra1 == 0x007eu,
          "DB transition event");
    check(records[2].event_type == CPU_TR_STACK_OP &&
          records[2].extra0 == CPU_STACK_OP_PHA,
          "stack operation event");
    sr_cpu_trace_reset();
    check(sr_cpu_trace_count() == 0u, "trace reset");
}

static void test_boundaries_and_faults(void) {
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));
    g_recomp_stack_top = 2;
    g_cpu.S = 0x01e0u;
    check(boundary_audit_init() == 8192u, "boundary capacity");
    boundary_audit_record_entry("function");
    cpu_trace_mark_nlr_exit(BD_EXIT_KIND_SKIP_PROPAGATION);
    boundary_audit_record_exit("function");
    cpu_trace_offrails("rom", 0x018000u);
    cpu_trace_offrails("rom", 0x018123u);
    cpu_trace_offrails("rom", 0x028000u);
    check(cpu_trace_offrails_count() == 2,
          "off-rails grouping by tag and bank");
    check(cpu_trace_unresolved_goto_trap(&cpu, 1u, 2u, "f", "l") ==
              RECOMP_RETURN_NORMAL,
          "unresolved goto soft trap");
    check(cpu_trace_unresolved_stub_trap(&cpu, 3u, "stub") ==
              RECOMP_RETURN_NORMAL,
          "unresolved stub soft trap");
    check(cpu_trace_dispatch_oob(&cpu, 4u, 5u) == RECOMP_RETURN_SKIP_1,
          "dispatch OOB delegation");
}

int main(void) {
    test_events();
    test_boundaries_and_faults();
    if (failures != 0) return 1;
    puts("runtime CPU trace: PASS");
    return 0;
}
