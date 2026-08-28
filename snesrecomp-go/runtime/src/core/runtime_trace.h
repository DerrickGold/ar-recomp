#ifndef SNESRECOMP_RUNTIME_TRACE_H
#define SNESRECOMP_RUNTIME_TRACE_H

#include <stdint.h>

typedef struct Snes Snes;

#define SR_TRACE_CHANNEL_FUNC 0x01
#define SR_TRACE_CHANNEL_VRAM 0x02
#define SR_TRACE_CHANNEL_VMADD 0x04
#define SR_TRACE_CHANNEL_REG 0x08
#define SR_TRACE_CHANNEL_DMA 0x10
#define SR_TRACE_CHANNEL_MX 0x20
#define SR_TRACE_CHANNEL_CALL 0x40
#define SR_TRACE_CHANNEL_DISPMISS 0x80
#define SR_TRACE_CHANNEL_GARBAGE 0x100
#define SR_TRACE_CHANNEL_WRAM 0x200
#define SR_TRACE_CHANNEL_STACK 0x400
#define SR_TRACE_CHANNEL_HWREAD 0x800
#define SR_TRACE_CHANNEL_PPUMEM 0x1000
#define SR_TRACE_CHANNEL_FRAME 0x2000
#define SR_TRACE_CHANNEL_DISPATCH 0x4000

int sr_trace_active(void);
int sr_trace_channel_enabled(int channel_bit);
void sr_trace_func(uint32_t pc24, const char *name, int m, int x,
                   int expected_m, int expected_x);
void sr_trace_call(uint32_t pc24, const char *name, int m, int x,
                   int expected_m, int expected_x);
void sr_trace_vram(uint16_t address, uint16_t value, const char *path);
void sr_trace_vmadd(uint16_t address, const char *source);
void sr_trace_reg(uint16_t address, uint8_t value);
void sr_trace_dma(int channel, uint8_t b_address, uint8_t a_bank,
                  uint16_t a_address, uint32_t size, int from_b_bus);
void sr_trace_dispmiss(uint32_t from_pc, uint32_t to_pc);
void sr_trace_garbage(uint32_t pc24, const char *name, int m, int x);
void sr_trace_wram(uint32_t offset, uint16_t old_value, uint16_t value,
                   int width);
void sr_trace_hwread(uint16_t address, uint8_t value);
void sr_trace_ppumem(const char *memory, uint16_t address, uint16_t value);
void sr_trace_frame(const char *event);
void sr_trace_flush(const char *reason);

/* Deterministic host configuration. The normal game initializes lazily from
 * SNESRECOMP_TRACE_FILE; this entry is useful to embedders and tests that
 * avoid process
 * environment mutation. Negative window bounds mean unbounded. */
int sr_trace_open_file(const char *path, int channel_mask,
                       long host_frame_low, long host_frame_high);
void sr_trace_close(void);
void sr_trace_bind_runner(Snes *runner, int enabled);

#endif
