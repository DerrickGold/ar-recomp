#ifndef SNESRECOMP_APU_H
#define SNESRECOMP_APU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp.h"
#include "spc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Apu Apu;
typedef struct SaveLoadInfo SaveLoadInfo;
typedef struct Snes Snes;

typedef struct Timer {
    uint8_t cycles;
    uint8_t divider;
    uint8_t target;
    uint8_t counter;
    bool enabled;
} Timer;

#define APU_PORT_QUEUE_LEN 128u
#define APU_PORT_MIN_DWELL 128u
#define APU_RAM_WRITE_BITMAP_BYTES (0x10000u / 8u)

typedef struct ApuPortWrite {
    uint64_t target_sample;
    uint8_t port;
    uint8_t val;
} ApuPortWrite;

struct Apu {
    Spc *spc;
    Dsp *dsp;
    /* Host diagnostic switch; derived from the environment and deliberately
     * outside the serialized ram..pad compatibility span. */
    bool diagnosticCountersEnabled;
    bool auditWritesEnabled;
    uint8_t ramWritten[APU_RAM_WRITE_BITMAP_BYTES];
    uint8_t ram[0x10000];
    bool romReadable;
    uint8_t dspAdr;
    uint32_t cycles;
    uint8_t inPorts[6];
    uint8_t outPorts[4];
    Timer timer[3];
    uint8_t cpuCyclesLeft;
    uint8_t dspSlot;
    uint8_t pad[6];
    ApuPortWrite portQueue[APU_PORT_QUEUE_LEN];
    uint32_t portQHead;
    uint32_t portQTail;
    uint64_t sampleClock;
    /* Monotonic semantic clocks. cycleClock counts individual S-DSP slots;
     * timelineTargetCycles is the runner-owned game-tick target. */
    uint64_t cycleClock;
    uint64_t timelineTargetCycles;
    uint64_t portClock;
    uint64_t portClockNs;
    uint64_t portLastTarget[4];
    uint8_t portLastVal[4];
    uint8_t portLastValid[4];
};

Apu *apu_init(void);
void apu_free(Apu *apu);
void apu_reset(Apu *apu);
void apu_cycle(Apu *apu);
uint8_t apu_cpuRead(Apu *apu, uint16_t address);
void apu_cpuWrite(Apu *apu, uint16_t address, uint8_t value);
void apu_saveload(Apu *apu, SaveLoadInfo *info);
void apu_schedulePortWrite(Apu *apu, uint8_t port, uint8_t value,
                           uint64_t target_sample);
void apu_clearPortQueue(Apu *apu);
void apu_markRamWritten(Apu *apu, uint16_t address, size_t count);

extern void (*g_apu_spc_port_write_trace_hook)(Apu *, uint8_t, uint8_t);
extern void (*g_apu_spc_dsp_write_hook)(Apu *, uint8_t, uint8_t);
extern bool (*g_apu_spc_dsp_write_filter_hook)(Apu *, uint8_t, uint8_t *);
extern void (*g_apu_extra_saveload_hook)(Apu *, SaveLoadInfo *);
uint64_t snes_apu_cycle_count(void);
uint64_t apu_cycle_count(const Apu *apu);

#ifdef __cplusplus
}
#endif

#endif
