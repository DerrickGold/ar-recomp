/* Temporary diagnostic stub: the upstream debug_server.c (TCP debug server)
 * has bitrotted against the current cpu_trace.c (references removed symbols
 * g_oamblk_*, s_oamblk, g_rtst_*, OAMBLK_TRACE_SIZE, ...). For local
 * stack-drift hunts we only need SNESRECOMP_TRACE=1 to compile in the
 * cpu_trace.c tripwires; the TCP server itself is unused. This file provides
 * no-op definitions for the handful of debug_server entry points that
 * non-debug_server translation units reference, so the trace build links
 * without compiling debug_server.c. NOT part of the normal (perf) build. */
#include <stdint.h>
#include "cpu_state.h"

volatile uint64_t g_block_counter = 0;

void dbg_oam_block_trace(CpuState *cpu, uint32 pc24) { (void)cpu; (void)pc24; }
void dbg_rts_trace(CpuState *cpu, uint32 src_pc, uint16 entry_s,
                   uint16 ret_s, uint32 popped_pc, uint8 hrv) {
  (void)cpu; (void)src_pc; (void)entry_s; (void)ret_s; (void)popped_pc; (void)hrv;
}

void debug_server_record_frame(int frame) { (void)frame; }
void debug_server_on_reg_write(uint16_t adr, uint8_t val) { (void)adr; (void)val; }
void debug_server_on_vram_write(uint32_t byte_addr, uint8_t value) { (void)byte_addr; (void)value; }
void debug_server_profile_push(const char *name) { (void)name; }
void debug_server_profile_latch(int frame_num) { (void)frame_num; }
void debug_server_arm_default_reg_trace(void) { }
void debug_server_arm_default_dma_tripwire(void) { }
void debug_server_arm_default_wram_trace(void) { }
void debug_server_on_oam_render(void) { }
void debug_server_on_oam_write(int is_high, uint16_t index, uint16_t value) { (void)is_high; (void)index; (void)value; }
void debug_server_on_oracle_vram_write(uint32_t byte_addr, uint8_t value) { (void)byte_addr; (void)value; }
