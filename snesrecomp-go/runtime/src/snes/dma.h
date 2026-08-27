#ifndef SNESRECOMP_DMA_H
#define SNESRECOMP_DMA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Snes Snes;
typedef struct SaveLoadInfo SaveLoadInfo;
typedef struct Dma Dma;

typedef struct DmaChannel {
    uint8_t bAdr;
    uint16_t aAdr;
    uint8_t aBank;
    uint16_t size;
    uint8_t indBank;
    uint16_t tableAdr;
    uint8_t repCount;
    uint8_t unusedByte;
    bool dmaActive;
    bool hdmaActive;
    uint8_t mode;
    bool fixed;
    bool decrement;
    bool indirect;
    bool fromB;
    bool unusedBit;
    bool doTransfer;
    bool terminated;
    uint8_t offIndex;
} DmaChannel;

enum { kDmaChannelCount = 8 };

struct Dma {
    Snes *snes;
    DmaChannel channel[kDmaChannelCount];
    uint32_t dmaTimer;
    bool dmaBusy;
};

typedef void DmaStartTraceHook(unsigned channel, const DmaChannel *state);
extern DmaStartTraceHook *g_dma_start_trace_hook;

Dma *dma_init(Snes *snes);
void dma_free(Dma *dma);
void dma_reset(Dma *dma);
uint8_t dma_read(Dma *dma, uint16_t address);
void dma_write(Dma *dma, uint16_t address, uint8_t value);
void dma_doDma(Dma *dma);
/* Retained timer-stepped reference/debug path. */
bool dma_cycle(Dma *dma);
/* Complete the current synchronous general DMA without host-only timer polls. */
void dma_run_to_idle(Dma *dma);
void dma_startDma(Dma *dma, uint8_t channels, bool hdma);
void dma_saveload(Dma *dma, SaveLoadInfo *info);

#ifdef __cplusplus
}
#endif

#endif
