#include "dma.h"
#include "saveload.h"

#include <stddef.h>
#include <stdlib.h>

extern uint8_t snes_read(Snes *snes, uint32_t address);
extern void snes_write(Snes *snes, uint32_t address, uint8_t value);
extern uint8_t snes_readBBus(Snes *snes, uint8_t address);
extern void snes_writeBBus(Snes *snes, uint8_t address, uint8_t value);

DmaStartTraceHook *g_dma_start_trace_hook;

static const uint8_t k_bbus_offsets[8][4] = {
    {0, 0, 0, 0},
    {0, 1, 0, 1},
    {0, 0, 0, 0},
    {0, 0, 1, 1},
    {0, 1, 2, 3},
    {0, 1, 0, 1},
    {0, 0, 0, 0},
    {0, 0, 1, 1},
};

Dma *dma_init(Snes *snes) {
    Dma *dma = (Dma *)malloc(sizeof(*dma));
    if (dma != NULL) {
        dma->snes = snes;
        dma_reset(dma);
    }
    return dma;
}

void dma_free(Dma *dma) {
    free(dma);
}

void dma_reset(Dma *dma) {
    if (dma == NULL) {
        return;
    }
    for (unsigned index = 0; index < kDmaChannelCount; ++index) {
        DmaChannel *channel = &dma->channel[index];
        channel->bAdr = 0xffu;
        channel->aAdr = 0xffffu;
        channel->aBank = 0xffu;
        channel->size = 0xffffu;
        channel->indBank = 0xffu;
        channel->tableAdr = 0xffffu;
        channel->repCount = 0xffu;
        channel->unusedByte = 0xffu;
        channel->dmaActive = false;
        channel->hdmaActive = false;
        channel->mode = 7u;
        channel->fixed = true;
        channel->decrement = true;
        channel->indirect = true;
        channel->fromB = true;
        channel->unusedBit = true;
        channel->doTransfer = false;
        channel->terminated = false;
        channel->offIndex = 0u;
    }
    dma->dmaTimer = 0u;
    dma->dmaBusy = false;
}

void dma_saveload(Dma *dma, SaveLoadInfo *info) {
    if (dma != NULL && info != NULL && info->func != NULL) {
        info->func(info, &dma->channel,
                   sizeof(*dma) - offsetof(Dma, channel));
    }
}

static DmaChannel *selected_channel(Dma *dma, uint16_t address) {
    return &dma->channel[(address >> 4) & 7u];
}

uint8_t dma_read(Dma *dma, uint16_t address) {
    if (dma == NULL) {
        return 0u;
    }
    const DmaChannel *channel = selected_channel(dma, address);
    switch (address & 0x0fu) {
        case 0x0u:
            return (uint8_t)(channel->mode |
                             (channel->fixed ? 0x08u : 0u) |
                             (channel->decrement ? 0x10u : 0u) |
                             (channel->unusedBit ? 0x20u : 0u) |
                             (channel->indirect ? 0x40u : 0u) |
                             (channel->fromB ? 0x80u : 0u));
        case 0x1u: return channel->bAdr;
        case 0x2u: return (uint8_t)channel->aAdr;
        case 0x3u: return (uint8_t)(channel->aAdr >> 8);
        case 0x4u: return channel->aBank;
        case 0x5u: return (uint8_t)channel->size;
        case 0x6u: return (uint8_t)(channel->size >> 8);
        case 0x7u: return channel->indBank;
        case 0x8u: return (uint8_t)channel->tableAdr;
        case 0x9u: return (uint8_t)(channel->tableAdr >> 8);
        case 0xau: return channel->repCount;
        case 0xbu:
        case 0xfu: return channel->unusedByte;
        default: return 0u;
    }
}

void dma_write(Dma *dma, uint16_t address, uint8_t value) {
    if (dma == NULL) {
        return;
    }
    DmaChannel *channel = selected_channel(dma, address);
    switch (address & 0x0fu) {
        case 0x0u:
            channel->mode = (uint8_t)(value & 7u);
            channel->fixed = (value & 0x08u) != 0u;
            channel->decrement = (value & 0x10u) != 0u;
            channel->unusedBit = (value & 0x20u) != 0u;
            channel->indirect = (value & 0x40u) != 0u;
            channel->fromB = (value & 0x80u) != 0u;
            break;
        case 0x1u: channel->bAdr = value; break;
        case 0x2u: channel->aAdr = (uint16_t)((channel->aAdr & 0xff00u) | value); break;
        case 0x3u: channel->aAdr = (uint16_t)((channel->aAdr & 0x00ffu) |
                                              ((uint16_t)value << 8)); break;
        case 0x4u: channel->aBank = value; break;
        case 0x5u: channel->size = (uint16_t)((channel->size & 0xff00u) | value); break;
        case 0x6u: channel->size = (uint16_t)((channel->size & 0x00ffu) |
                                              ((uint16_t)value << 8)); break;
        case 0x7u: channel->indBank = value; break;
        case 0x8u: channel->tableAdr = (uint16_t)((channel->tableAdr & 0xff00u) | value); break;
        case 0x9u: channel->tableAdr = (uint16_t)((channel->tableAdr & 0x00ffu) |
                                                  ((uint16_t)value << 8)); break;
        case 0xau: channel->repCount = value; break;
        case 0xbu:
        case 0xfu: channel->unusedByte = value; break;
        default: break;
    }
}

static void transfer_byte(Dma *dma, DmaChannel *channel) {
    const uint8_t bbus = (uint8_t)(channel->bAdr +
        k_bbus_offsets[channel->mode][channel->offIndex]);
    const uint32_t abus = ((uint32_t)channel->aBank << 16) | channel->aAdr;
    if (channel->fromB) {
        snes_write(dma->snes, abus, snes_readBBus(dma->snes, bbus));
    } else {
        snes_writeBBus(dma->snes, bbus, snes_read(dma->snes, abus));
    }

    channel->offIndex = (uint8_t)((channel->offIndex + 1u) & 3u);
    if (!channel->fixed) {
        channel->aAdr = (uint16_t)(channel->aAdr +
            (channel->decrement ? UINT16_MAX : 1u));
    }
    --channel->size;
    if (channel->size == 0u) {
        channel->offIndex = 0u;
        channel->dmaActive = false;
        dma->dmaTimer += 8u;
    }
}

void dma_doDma(Dma *dma) {
    if (dma == NULL || !dma->dmaBusy) {
        return;
    }
    if (dma->dmaTimer != 0u) {
        dma->dmaTimer -= 2u;
        return;
    }

    for (unsigned index = 0; index < kDmaChannelCount; ++index) {
        if (dma->channel[index].dmaActive) {
            transfer_byte(dma, &dma->channel[index]);
            dma->dmaTimer += 6u;
            return;
        }
    }
    dma->dmaBusy = false;
}

bool dma_cycle(Dma *dma) {
    if (dma == NULL || !dma->dmaBusy) {
        return false;
    }
    dma_doDma(dma);
    return true;
}

void dma_startDma(Dma *dma, uint8_t channels, bool hdma) {
    if (dma == NULL) {
        return;
    }
    for (unsigned index = 0; index < kDmaChannelCount; ++index) {
        const bool selected = (channels & (uint8_t)(1u << index)) != 0u;
        if (hdma) {
            dma->channel[index].hdmaActive = selected;
        } else {
            dma->channel[index].dmaActive = selected;
            if (selected && g_dma_start_trace_hook != NULL) {
                g_dma_start_trace_hook(index, &dma->channel[index]);
            }
        }
    }
    if (!hdma) {
        dma->dmaBusy = channels != 0u;
        if (dma->dmaBusy) {
            dma->dmaTimer += 16u;
        }
    }
}
