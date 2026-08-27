#include "snesrecomp/game/cpu.h"

/*
 * The code generator emits these optional trace calls behind
 * SNESRECOMP_TRACE. A game may replace this source with a real observer; the
 * standalone runtime supplies inert definitions so trace-enabled SDK builds
 * still link without a game-specific diagnostics backend.
 */
void dbg_oam_block_trace(CpuState *cpu, uint32 pc24) {
    (void)cpu;
    (void)pc24;
}

void dbg_rts_trace(CpuState *cpu, uint32 source_pc, uint16 entry_stack,
                   uint16 return_stack, uint32 popped_pc, uint8 hrv) {
    (void)cpu;
    (void)source_pc;
    (void)entry_stack;
    (void)return_stack;
    (void)popped_pc;
    (void)hrv;
}
