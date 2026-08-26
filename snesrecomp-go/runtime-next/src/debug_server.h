#ifndef DEBUG_SERVER_H
#define DEBUG_SERVER_H

#include "types.h"

#ifndef SNESRECOMP_TRACE
#define SNESRECOMP_TRACE 0
#endif

#ifndef SNESRECOMP_REVERSE_DEBUG
#define SNESRECOMP_REVERSE_DEBUG 1
#endif

#if SNESRECOMP_TRACE
int debug_server_init(int port);
void debug_server_poll(void);
void debug_server_shutdown(void);
void debug_server_start_paused(void);
void debug_server_wait_if_paused(void);
int debug_server_consume_loadstate(void);
uint32_t debug_server_get_controller_inputs(void);
uint32_t debug_server_get_controller_active_mask(void);
void debug_server_record_frame(int frame);
void debug_server_set_ram(uint8_t *ram, uint32_t ram_size);
void debug_server_on_reg_write(uint16_t address, uint8_t value);
void debug_server_on_vram_write(uint32_t byte_address, uint8_t value);
void debug_server_on_oracle_vram_write(uint32_t byte_address, uint8_t value);
void debug_server_on_oam_write(int high_table, uint16_t index, uint16_t value);
void debug_server_on_oam_render(void);
void debug_server_profile_push(const char *name);
void debug_server_profile_latch(int frame_number);
#else
static inline int debug_server_init(int port) { (void)port; return 0; }
static inline void debug_server_poll(void) {}
static inline void debug_server_shutdown(void) {}
static inline void debug_server_start_paused(void) {}
static inline void debug_server_wait_if_paused(void) {}
static inline int debug_server_consume_loadstate(void) { return -1; }
static inline uint32_t debug_server_get_controller_inputs(void) { return 0u; }
static inline uint32_t debug_server_get_controller_active_mask(void) { return 0u; }
static inline void debug_server_record_frame(int frame) { (void)frame; }
static inline void debug_server_set_ram(uint8_t *ram, uint32_t size) {
    (void)ram;
    (void)size;
}
static inline void debug_server_on_reg_write(uint16_t address, uint8_t value) {
    (void)address;
    (void)value;
}
static inline void debug_server_on_vram_write(uint32_t address, uint8_t value) {
    (void)address;
    (void)value;
}
static inline void debug_server_on_oracle_vram_write(uint32_t address,
                                                      uint8_t value) {
    (void)address;
    (void)value;
}
static inline void debug_server_on_oam_write(int high_table, uint16_t index,
                                             uint16_t value) {
    (void)high_table;
    (void)index;
    (void)value;
}
static inline void debug_server_on_oam_render(void) {}
static inline void debug_server_profile_push(const char *name) { (void)name; }
static inline void debug_server_profile_latch(int frame) { (void)frame; }
#endif

#if SNESRECOMP_REVERSE_DEBUG
extern uint8_t g_ram[];
void debug_on_wram_write_byte(uint32_t address, uint8_t old_value,
                              uint8_t new_value);
void debug_on_wram_write_word(uint32_t address, uint16_t old_value,
                              uint16_t new_value);
void debug_on_block_enter(uint32_t pc, uint32_t a, uint32_t x, uint32_t y);

static inline void rdb_store8(uint32_t address, uint8_t value) {
    uint8_t old_value = g_ram[address];
    g_ram[address] = value;
    debug_on_wram_write_byte(address, old_value, value);
}

static inline void rdb_store16(uint32_t address, uint16_t value) {
    uint16_t old_value = (uint16_t)g_ram[address] |
                         ((uint16_t)g_ram[address + 1u] << 8);
    g_ram[address] = (uint8_t)value;
    g_ram[address + 1u] = (uint8_t)(value >> 8);
    debug_on_wram_write_word(address, old_value, value);
}

#define RDB_STORE8(address, value) rdb_store8((uint32_t)(address), (uint8_t)(value))
#define RDB_STORE16(address, value) rdb_store16((uint32_t)(address), (uint16_t)(value))
#define RDB_REG_UNKNOWN 0xFFFFFFFFu
#define RDB_BLOCK_HOOK(pc, a, x, y) \
    debug_on_block_enter((uint32_t)(pc), (uint32_t)(a), (uint32_t)(x), \
                         (uint32_t)(y))
#define RDB_LOAD8(address) (g_ram[(address)])
#define RDB_LOAD16(address) \
    ((uint16_t)g_ram[(address)] | ((uint16_t)g_ram[(address) + 1u] << 8))
#define RDB_INSN_HOOK(pc, mnemonic, a, x, y, b, mx) ((void)0)
#endif

#endif
