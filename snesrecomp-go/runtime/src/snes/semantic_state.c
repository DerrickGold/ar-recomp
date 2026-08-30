#include "semantic_state.h"

#include "apu.h"
#include "cart.h"
#include "dma.h"
#include "dsp_accuracy_bridge.h"
#include "ppu.h"
#include "snes.h"
#include "snesrecomp/game/runtime_constants.h"

#include <string.h>

void snes_semantic_write_f64(SnesSemanticWriter *writer, double value) {
    uint64_t bits = 0u;
    _Static_assert(sizeof(bits) == sizeof(value),
                   "semantic f64 requires binary64 storage");
    memcpy(&bits, &value, sizeof(bits));
    snes_semantic_write_u64(writer, bits);
}

static void write_u16_array(SnesSemanticWriter *writer,
                            const uint16_t *values, size_t count) {
    size_t index;
    for (index = 0u; index < count; ++index)
        snes_semantic_write_u16(writer, values[index]);
}

static void write_i16_array(SnesSemanticWriter *writer,
                            const int16_t *values, size_t count) {
    size_t index;
    for (index = 0u; index < count; ++index)
        snes_semantic_write_i16(writer, values[index]);
}

static void write_ppu_v2(const Ppu *ppu, SnesSemanticWriter *writer) {
    unsigned index;
    snes_semantic_write_bytes(writer, "ppu2", 4u);
    snes_semantic_write_u8(writer, ppu->inidisp);
    snes_semantic_write_u8(writer, ppu->obsel);
    snes_semantic_write_u8(writer, ppu->oamaddl);
    snes_semantic_write_u8(writer, ppu->oamaddh);
    snes_semantic_write_u8(writer, ppu->bgmode);
    snes_semantic_write_u8(writer, ppu->mosaic);
    snes_semantic_write_bytes(writer, ppu->bgXsc, sizeof(ppu->bgXsc));
    snes_semantic_write_u16(writer, ppu->bgTileAdr);
    snes_semantic_write_u8(writer, ppu->m7sel);
    snes_semantic_write_u8(writer, ppu->setini);
    write_u16_array(writer, ppu->hScroll, 4u);
    write_u16_array(writer, ppu->vScroll, 4u);
    write_i16_array(writer, ppu->m7matrix, 8u);
    snes_semantic_write_u16(writer, ppu->fixedColor);
    snes_semantic_write_u32(writer, ppu->windowsel);
    snes_semantic_write_u8(writer, ppu->window1left);
    snes_semantic_write_u8(writer, ppu->window1right);
    snes_semantic_write_u8(writer, ppu->window2left);
    snes_semantic_write_u8(writer, ppu->window2right);
    snes_semantic_write_u16(writer, ppu->wbgobjlog);
    snes_semantic_write_bytes(
        writer, ppu->screenEnabled, sizeof(ppu->screenEnabled));
    snes_semantic_write_bytes(
        writer, ppu->screenWindowed, sizeof(ppu->screenWindowed));
    snes_semantic_write_u8(writer, ppu->cgadsub);
    snes_semantic_write_u8(writer, ppu->cgwsel);
    snes_semantic_write_u16(writer, ppu->vramPointer);
    snes_semantic_write_bool(writer, ppu->vramIncrementOnHigh);
    snes_semantic_write_u8(writer, ppu->vramRemapMode);
    snes_semantic_write_u8(writer, ppu->vramIncrement);
    snes_semantic_write_u16(writer, ppu->vramReadBuffer);
    snes_semantic_write_u8(writer, ppu->cgramPointer);
    snes_semantic_write_bool(writer, ppu->cgramSecondWrite);
    snes_semantic_write_u8(writer, ppu->cgramBuffer);
    snes_semantic_write_u8(writer, ppu->oamAdr);
    snes_semantic_write_bool(writer, ppu->oamInHigh);
    snes_semantic_write_bool(writer, ppu->oamSecondWrite);
    snes_semantic_write_u8(writer, ppu->oamBuffer);
    snes_semantic_write_bool(writer, ppu->timeOver);
    snes_semantic_write_bool(writer, ppu->rangeOver);
    snes_semantic_write_u8(writer, ppu->scrollPrev);
    snes_semantic_write_u8(writer, ppu->scrollPrev2);
    snes_semantic_write_u8(writer, ppu->mosaicStartLine);
    snes_semantic_write_u8(writer, ppu->m7prev);
    snes_semantic_write_i32(writer, ppu->m7startX);
    snes_semantic_write_i32(writer, ppu->m7startY);
    snes_semantic_write_bool(writer, ppu->evenFrame);
    snes_semantic_write_bool(writer, ppu->frameOverscan);
    snes_semantic_write_bool(writer, ppu->frameInterlace);
    snes_semantic_write_u16(writer, ppu->hCount);
    snes_semantic_write_u16(writer, ppu->vCount);
    snes_semantic_write_bool(writer, ppu->hCountSecond);
    snes_semantic_write_bool(writer, ppu->vCountSecond);
    snes_semantic_write_bool(writer, ppu->countersLatched);
    write_u16_array(writer, ppu->cgram, kPpuCgramEntries);
    write_u16_array(writer, ppu->oam, kPpuOamWords);
    snes_semantic_write_bytes(writer, ppu->highOam, sizeof(ppu->highOam));
    for (index = 0u; index < sizeof(ppu->vram) / sizeof(ppu->vram[0]);
         ++index)
        snes_semantic_write_u16(writer, ppu->vram[index]);
}

static void write_dma_v2(const Dma *dma, SnesSemanticWriter *writer) {
    unsigned index;
    snes_semantic_write_bytes(writer, "dma2", 4u);
    for (index = 0u; index < kDmaChannelCount; ++index) {
        const DmaChannel *channel = &dma->channel[index];
        snes_semantic_write_u8(writer, channel->bAdr);
        snes_semantic_write_u16(writer, channel->aAdr);
        snes_semantic_write_u8(writer, channel->aBank);
        snes_semantic_write_u16(writer, channel->size);
        snes_semantic_write_u8(writer, channel->indBank);
        snes_semantic_write_u16(writer, channel->tableAdr);
        snes_semantic_write_u8(writer, channel->repCount);
        snes_semantic_write_u8(writer, channel->unusedByte);
        snes_semantic_write_bool(writer, channel->dmaActive);
        snes_semantic_write_bool(writer, channel->hdmaActive);
        snes_semantic_write_u8(writer, channel->mode);
        snes_semantic_write_bool(writer, channel->fixed);
        snes_semantic_write_bool(writer, channel->decrement);
        snes_semantic_write_bool(writer, channel->indirect);
        snes_semantic_write_bool(writer, channel->fromB);
        snes_semantic_write_bool(writer, channel->unusedBit);
        snes_semantic_write_bool(writer, channel->doTransfer);
        snes_semantic_write_bool(writer, channel->terminated);
        snes_semantic_write_u8(writer, channel->offIndex);
    }
    snes_semantic_write_u32(writer, dma->dmaTimer);
    snes_semantic_write_bool(writer, dma->dmaBusy);
}

static void write_spc_v2(const Spc *spc, SnesSemanticWriter *writer) {
    snes_semantic_write_u8(writer, spc->a);
    snes_semantic_write_u8(writer, spc->x);
    snes_semantic_write_u8(writer, spc->y);
    snes_semantic_write_u8(writer, spc->sp);
    snes_semantic_write_u16(writer, spc->pc);
    snes_semantic_write_bool(writer, spc->c);
    snes_semantic_write_bool(writer, spc->z);
    snes_semantic_write_bool(writer, spc->v);
    snes_semantic_write_bool(writer, spc->n);
    snes_semantic_write_bool(writer, spc->i);
    snes_semantic_write_bool(writer, spc->h);
    snes_semantic_write_bool(writer, spc->p);
    snes_semantic_write_bool(writer, spc->b);
    snes_semantic_write_bool(writer, spc->stopped);
}

static void write_dsp_v2(const Dsp *dsp, SnesSemanticWriter *writer) {
    unsigned index;
    sr_dsp_accuracy_write_semantic_v2(
        (const SrDspAccuracy *)dsp->accuracy, writer);
    for (index = 0u; index < kDspMaximumVoiceCount; ++index) {
        const DspChannel *voice = &dsp->channel[index];
        snes_semantic_write_u16(writer, voice->pitch);
        snes_semantic_write_bool(writer, voice->pitchModulation);
        snes_semantic_write_u8(writer, voice->srcn);
        snes_semantic_write_bool(writer, voice->useNoise);
        snes_semantic_write_bool(writer, voice->useGain);
        snes_semantic_write_bool(writer, voice->directGain);
        snes_semantic_write_u16(writer, voice->gainValue);
        snes_semantic_write_u8(writer, (uint8_t)voice->volumeL);
        snes_semantic_write_u8(writer, (uint8_t)voice->volumeR);
        snes_semantic_write_bool(writer, voice->echoEnable);
    }
}

static void write_apu_v2(const Apu *apu, SnesSemanticWriter *writer) {
    uint32_t queued;
    unsigned index;
    snes_semantic_write_bytes(writer, "apu2", 4u);
    snes_semantic_write_bytes(writer, apu->ram, sizeof(apu->ram));
    snes_semantic_write_bool(writer, apu->romReadable);
    snes_semantic_write_u8(writer, apu->dspAdr);
    snes_semantic_write_u32(writer, apu->cycles);
    snes_semantic_write_bytes(writer, apu->inPorts, sizeof(apu->inPorts));
    snes_semantic_write_bytes(writer, apu->outPorts, sizeof(apu->outPorts));
    for (index = 0u; index < 3u; ++index) {
        const Timer *timer = &apu->timer[index];
        snes_semantic_write_u8(writer, timer->cycles);
        snes_semantic_write_u8(writer, timer->divider);
        snes_semantic_write_u8(writer, timer->target);
        snes_semantic_write_u8(writer, timer->counter);
        snes_semantic_write_bool(writer, timer->enabled);
    }
    snes_semantic_write_u8(writer, apu->cpuCyclesLeft);
    snes_semantic_write_u8(writer, apu->dspSlot);
    write_dsp_v2(apu->dsp, writer);
    write_spc_v2(apu->spc, writer);
    queued = apu->portQTail - apu->portQHead;
    if (queued > APU_PORT_QUEUE_LEN) {
        writer->failed = true;
        return;
    }
    snes_semantic_write_u32(writer, queued);
    for (index = 0u; index < queued; ++index) {
        const ApuPortWrite *write = &apu->portQueue[
            (apu->portQHead + index) & (APU_PORT_QUEUE_LEN - 1u)];
        snes_semantic_write_u64(writer, write->target_sample);
        snes_semantic_write_u8(writer, write->port);
        snes_semantic_write_u8(writer, write->val);
    }
    snes_semantic_write_u64(writer, apu->sampleClock);
    snes_semantic_write_u64(writer, apu->cycleClock);
    snes_semantic_write_u64(writer, apu->timelineTargetCycles);
    snes_semantic_write_u64(writer, apu->portClock);
    for (index = 0u; index < 4u; ++index)
        snes_semantic_write_u64(writer, apu->portLastTarget[index]);
    snes_semantic_write_bytes(
        writer, apu->portLastVal, sizeof(apu->portLastVal));
    snes_semantic_write_bytes(
        writer, apu->portLastValid, sizeof(apu->portLastValid));
}

static void write_cart_v2(const Cart *cart, SnesSemanticWriter *writer) {
    snes_semantic_write_bytes(writer, "car2", 4u);
    snes_semantic_write_u8(writer, cart->type);
    snes_semantic_write_u32(writer, cart->romSize);
    snes_semantic_write_u32(writer, cart->ramSize);
    snes_semantic_write_bytes(writer, cart->ram, cart->ramSize);
}

static void write_snes_v2(const Snes *snes, SnesSemanticWriter *writer) {
    snes_semantic_write_bytes(writer, "sne2", 4u);
    snes_semantic_write_u16(writer, snes->hPos);
    snes_semantic_write_u16(writer, snes->vPos);
    snes_semantic_write_f64(writer, snes->apuCatchupCycles);
    snes_semantic_write_bool(writer, snes->hIrqEnabled);
    snes_semantic_write_bool(writer, snes->vIrqEnabled);
    snes_semantic_write_bool(writer, snes->nmiEnabled);
    snes_semantic_write_u16(writer, snes->hTimer);
    snes_semantic_write_u16(writer, snes->vTimer);
    snes_semantic_write_bool(writer, snes->inNmi);
    snes_semantic_write_bool(writer, snes->forceNmi);
    snes_semantic_write_bool(writer, snes->nmiAvail);
    snes_semantic_write_bool(writer, snes->inIrq);
    snes_semantic_write_bool(writer, snes->inVblank);
    snes_semantic_write_bool(writer, snes->autoJoyRead);
    snes_semantic_write_u16(writer, snes->autoJoyTimer);
    snes_semantic_write_bool(writer, snes->ppuLatch);
    snes_semantic_write_u8(writer, snes->multiplyA);
    snes_semantic_write_u16(writer, snes->multiplyResult);
    snes_semantic_write_u16(writer, snes->divideA);
    snes_semantic_write_u16(writer, snes->divideResult);
    snes_semantic_write_u32(writer, snes->ramAdr);
    snes_semantic_write_bytes(writer, snes->ram, kSnesWramSize);
}

bool snes_write_semantic_main_state_v2(
        const Snes *snes, SnesSemanticWriter *writer) {
    if (snes == NULL || writer == NULL || writer->write == NULL ||
        snes->dma == NULL || snes->ppu == NULL ||
        snes->cart == NULL || snes->ram == NULL) {
        if (writer != NULL) writer->failed = true;
        return false;
    }
    write_ppu_v2(snes->ppu, writer);
    write_dma_v2(snes->dma, writer);
    write_cart_v2(snes->cart, writer);
    write_snes_v2(snes, writer);
    return !writer->failed;
}

bool snes_write_semantic_apu_state_v2(
        const Snes *snes, SnesSemanticWriter *writer) {
    if (snes == NULL || writer == NULL || writer->write == NULL ||
        snes->apu == NULL || snes->apu->dsp == NULL ||
        snes->apu->spc == NULL) {
        if (writer != NULL) writer->failed = true;
        return false;
    }
    write_apu_v2(snes->apu, writer);
    return !writer->failed;
}
