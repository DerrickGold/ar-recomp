#include <stdint.h>

typedef struct CpuState CpuState;

volatile uint64_t g_block_counter;

void dbg_oam_block_trace(CpuState *cpu, uint32_t pc24) {
    (void)cpu;
    (void)pc24;
}

void dbg_rts_trace(CpuState *cpu, uint32_t source_pc, uint16_t entry_stack,
                   uint16_t return_stack, uint32_t popped_pc, uint8_t hrv) {
    (void)cpu;
    (void)source_pc;
    (void)entry_stack;
    (void)return_stack;
    (void)popped_pc;
    (void)hrv;
}

void debug_server_record_frame(int frame) { (void)frame; }
void debug_server_on_reg_write(uint16_t address, uint8_t value) {
    (void)address;
    (void)value;
}
void debug_server_on_vram_write(uint32_t address, uint8_t value) {
    (void)address;
    (void)value;
}
void debug_server_profile_push(const char *name) { (void)name; }
void debug_server_profile_latch(int frame) { (void)frame; }
void debug_server_arm_default_reg_trace(void) {}
void debug_server_arm_default_dma_tripwire(void) {}
void debug_server_arm_default_wram_trace(void) {}
void debug_server_on_oam_render(void) {}
void debug_server_on_oam_write(int high_table, uint16_t index, uint16_t value) {
    (void)high_table;
    (void)index;
    (void)value;
}
void debug_server_on_oracle_vram_write(uint32_t address, uint8_t value) {
    (void)address;
    (void)value;
}
