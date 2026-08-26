#include "dma.h"
#include "../runner_next_internal.h"
#include "saveload.h"
#include "snes.h"

#include <stddef.h>
#include <stdlib.h>

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
    if (dma == NULL || info == NULL || info->func == NULL) return;
    if (!info->portable) {
        info->func(info, &dma->channel,
                   sizeof(*dma) - offsetof(Dma, channel));
        return;
    }
    for (unsigned index = 0; index < kDmaChannelCount; ++index) {
        DmaChannel *channel = &dma->channel[index];
        saveload_u8(info, &channel->bAdr);
        saveload_u16(info, &channel->aAdr);
        saveload_u8(info, &channel->aBank);
        saveload_u16(info, &channel->size);
        saveload_u8(info, &channel->indBank);
        saveload_u16(info, &channel->tableAdr);
        saveload_u8(info, &channel->repCount);
        saveload_u8(info, &channel->unusedByte);
        saveload_bool(info, &channel->dmaActive);
        saveload_bool(info, &channel->hdmaActive);
        saveload_u8(info, &channel->mode);
        saveload_bool(info, &channel->fixed);
        saveload_bool(info, &channel->decrement);
        saveload_bool(info, &channel->indirect);
        saveload_bool(info, &channel->fromB);
        saveload_bool(info, &channel->unusedBit);
        saveload_bool(info, &channel->doTransfer);
        saveload_bool(info, &channel->terminated);
        saveload_u8(info, &channel->offIndex);
    }
    saveload_u32(info, &dma->dmaTimer);
    saveload_bool(info, &dma->dmaBusy);
}

static DmaChannel *selected_channel(Dma *dma, uint16_t address) {
    return &dma->channel[(address >> 4) & 7u];
}

uint8_t dma_read(Dma *dma, uint16_t address) {
    uint8_t value = 0u;
    if (dma == NULL) return value;
    const DmaChannel *channel = selected_channel(dma, address);
    switch (address & 0x0fu) {
        case 0x0u:
            value = (uint8_t)(channel->mode |
                              (channel->fixed ? 0x08u : 0u) |
                              (channel->decrement ? 0x10u : 0u) |
                              (channel->unusedBit ? 0x20u : 0u) |
                              (channel->indirect ? 0x40u : 0u) |
                              (channel->fromB ? 0x80u : 0u));
            break;
        case 0x1u: value = channel->bAdr; break;
        case 0x2u: value = (uint8_t)channel->aAdr; break;
        case 0x3u: value = (uint8_t)(channel->aAdr >> 8); break;
        case 0x4u: value = channel->aBank; break;
        case 0x5u: value = (uint8_t)channel->size; break;
        case 0x6u: value = (uint8_t)(channel->size >> 8); break;
        case 0x7u: value = channel->indBank; break;
        case 0x8u: value = (uint8_t)channel->tableAdr; break;
        case 0x9u: value = (uint8_t)(channel->tableAdr >> 8); break;
        case 0xau: value = channel->repCount; break;
        case 0xbu:
        case 0xfu: value = channel->unusedByte; break;
        default: break;
    }
    if (sr_runner_event_enabled(SR_EVENT_MASK_REGISTER_ACCESS)) {
        sr_runner_emit_register_access(
            dma->snes, false, address, value, 1u);
    }
    return value;
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
    if (sr_runner_event_enabled(SR_EVENT_MASK_REGISTER_ACCESS)) {
        sr_runner_emit_register_access(
            dma->snes, true, address, value, 1u);
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

void dma_run_to_idle(Dma *dma) {
    if (dma == NULL || !dma->dmaBusy) {
        return;
    }

    /*
     * General DMA is currently synchronous: the CPU waits for the complete
     * transfer, and dma_cycle() does not advance any other emulated hardware.
     * Preserve dma_cycle() as the timer-stepped reference path, but avoid its
     * host-only countdown and repeated channel scan for synchronous callers.
     */
    for (unsigned index = 0; index < kDmaChannelCount && dma->dmaBusy; ++index) {
        DmaChannel *channel = &dma->channel[index];
        while (dma->dmaBusy && channel->dmaActive) {
            transfer_byte(dma, channel);
        }
    }

    /* The stepped path consumes all startup, byte, and channel timers before
     * reporting idle. No other runtime component observes those timer ticks. */
    dma->dmaTimer = 0u;
    dma->dmaBusy = false;
}

static void emit_dma_begin(Dma *dma, unsigned index, bool hdma) {
    const DmaChannel *channel;
    SrRunnerEvent event = {0};
    if (!sr_runner_event_enabled(SR_EVENT_MASK_DMA)) return;
    channel = &dma->channel[index];
    event.type = SR_EVENT_DMA_BEGIN;
    event.frame_counter = dma->snes != NULL
        ? dma->snes->abiFrameCounter : 0u;
    event.flags = (hdma ? SR_EVENT_DMA_HDMA : 0u) |
                  (channel->fromB ? SR_EVENT_DMA_FROM_B_BUS : 0u) |
                  (channel->fixed ? SR_EVENT_DMA_FIXED_A_BUS : 0u) |
                  (channel->decrement
                       ? SR_EVENT_DMA_DECREMENT_A_BUS : 0u) |
                  (channel->indirect ? SR_EVENT_DMA_INDIRECT : 0u);
    event.address = ((uint32_t)channel->aBank << 16) | channel->aAdr;
    event.dma_a_address24 = event.address;
    event.dma_transfer_bytes = hdma ? 0u
        : (channel->size == 0u ? UINT32_C(0x10000) : channel->size);
    event.dma_table_address = hdma ? channel->aAdr : 0u;
    event.dma_channel = (uint8_t)index;
    event.dma_mode = channel->mode;
    event.dma_b_address = channel->bAdr;
    event.dma_indirect_bank = channel->indBank;
    event.label = hdma ? "hdma" : "dma";
    sr_runner_emit_event(dma->snes, SR_EVENT_MASK_DMA, &event);
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
        if (selected) emit_dma_begin(dma, index, hdma);
    }
    if (!hdma) {
        dma->dmaBusy = channels != 0u;
        if (dma->dmaBusy) {
            dma->dmaTimer += 16u;
        }
    }
}
