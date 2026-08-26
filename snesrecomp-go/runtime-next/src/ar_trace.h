#ifndef RUNTIME_NEXT_AR_TRACE_H
#define RUNTIME_NEXT_AR_TRACE_H

#include <stdint.h>

#define AR_TR_FUNC 0x01
#define AR_TR_VRAM 0x02
#define AR_TR_VMADD 0x04
#define AR_TR_REG 0x08
#define AR_TR_DMA 0x10
#define AR_TR_MX 0x20
#define AR_TR_CALL 0x40
#define AR_TR_DISPMISS 0x80
#define AR_TR_GARBAGE 0x100
#define AR_TR_WRAM 0x200
#define AR_TR_STACK 0x400
#define AR_TR_HWREAD 0x800
#define AR_TR_PPUMEM 0x1000
#define AR_TR_FRAME 0x2000

int ar_trace_active(void);
int ar_trace_ch(int channel_bit);
void ar_trace_func(uint32_t pc24, const char *name, int m, int x,
                   int expected_m, int expected_x);
void ar_trace_call(uint32_t pc24, const char *name, int m, int x,
                   int expected_m, int expected_x);
void ar_trace_vram(uint16_t address, uint16_t value, const char *path);
void ar_trace_vmadd(uint16_t address, const char *source);
void ar_trace_reg(uint16_t address, uint8_t value);
void ar_trace_dma(int channel, uint8_t b_address, uint8_t a_bank,
                  uint16_t a_address, uint32_t size, int from_b_bus);
void ar_trace_dispmiss(uint32_t from_pc, uint32_t to_pc);
void ar_trace_garbage(uint32_t pc24, const char *name, int m, int x);
void ar_trace_wram(uint32_t offset, uint16_t old_value, uint16_t value,
                   int width);
void ar_trace_hwread(uint16_t address, uint8_t value);
void ar_trace_ppumem(const char *memory, uint16_t address, uint16_t value);
void ar_trace_frame(const char *event);
void ar_trace_flush(const char *reason);

/* Deterministic host configuration. The normal game initializes lazily from
 * AR_TRACE; this entry is useful to embedders and tests that avoid process
 * environment mutation. Negative window bounds mean unbounded. */
int ar_trace_open_file(const char *path, int channel_mask,
                       long host_frame_low, long host_frame_high);
void ar_trace_close(void);

#endif
