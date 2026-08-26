#include "snes/dma.h"
#include "snes/saveload.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct Snes {
    uint8_t abus[0x10000];
    uint8_t bbus[0x100];
};

static int failures;
static size_t save_size;
static void *save_address;
static unsigned trace_count;
static unsigned traced_channel;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "runtime-next DMA contract failed: %s\n", message);
        ++failures;
    }
}

uint8_t snes_read(Snes *snes, uint32_t address) {
    return snes->abus[(uint16_t)address];
}

void snes_write(Snes *snes, uint32_t address, uint8_t value) {
    snes->abus[(uint16_t)address] = value;
}

uint8_t snes_readBBus(Snes *snes, uint8_t address) {
    return snes->bbus[address];
}

void snes_writeBBus(Snes *snes, uint8_t address, uint8_t value) {
    snes->bbus[address] = value;
}

static void capture_save(SaveLoadInfo *info, void *data, size_t size) {
    (void)info;
    save_address = data;
    save_size = size;
}

static void capture_start(unsigned channel, const DmaChannel *state) {
    ++trace_count;
    traced_channel = channel;
    check(state->dmaActive, "trace observes active channel");
}

static void run_to_idle(Dma *dma) {
    unsigned guard = 0u;
    while (dma_cycle(dma) && guard++ < 100000u) {
    }
    check(guard < 100000u, "DMA reaches idle");
}

static void test_reset_and_registers(Dma *dma) {
    check(dma->channel[0].bAdr == 0xffu && dma->channel[7].aAdr == 0xffffu,
          "reset fills hardware-visible registers");
    check(dma_read(dma, 0x4300u) == 0xffu, "reset control byte");

    static const uint8_t values[12] = {
        0xd5u, 0x18u, 0x34u, 0x12u, 0x7eu, 0x78u,
        0x56u, 0x7fu, 0xbcu, 0x9au, 0x81u, 0x42u,
    };
    for (uint16_t offset = 0u; offset < 12u; ++offset) {
        dma_write(dma, (uint16_t)(0x4330u + offset), values[offset]);
        check(dma_read(dma, (uint16_t)(0x4330u + offset)) == values[offset],
              "DMA register round trip");
    }
    dma_write(dma, 0x433fu, 0x99u);
    check(dma_read(dma, 0x433bu) == 0x99u, "unused register mirror");
    check(dma_read(dma, 0x433cu) == 0u, "open-bus-compatible unused read");
}

static void test_a_to_b(Dma *dma, Snes *snes) {
    dma_reset(dma);
    for (unsigned index = 0; index < 4u; ++index) {
        snes->abus[0x1000u + index] = (uint8_t)(0x20u + index);
    }
    dma_write(dma, 0x4300u, 0x04u);
    dma_write(dma, 0x4301u, 0x10u);
    dma_write(dma, 0x4302u, 0x00u);
    dma_write(dma, 0x4303u, 0x10u);
    dma_write(dma, 0x4304u, 0x7eu);
    dma_write(dma, 0x4305u, 4u);
    dma_write(dma, 0x4306u, 0u);
    g_dma_start_trace_hook = capture_start;
    dma_startDma(dma, 1u, false);
    run_to_idle(dma);
    check(snes->bbus[0x10u] == 0x20u && snes->bbus[0x11u] == 0x21u &&
          snes->bbus[0x12u] == 0x22u && snes->bbus[0x13u] == 0x23u,
          "mode-4 A-bus to B-bus sequence");
    check(dma->channel[0].aAdr == 0x1004u && !dma->channel[0].dmaActive,
          "source increments and channel completes");
    check(trace_count == 1u && traced_channel == 0u, "start trace hook");
}

static void test_b_to_a_fixed(Dma *dma, Snes *snes) {
    dma_reset(dma);
    snes->bbus[0x40u] = 0xacu;
    dma_write(dma, 0x4310u, 0x88u);
    dma_write(dma, 0x4311u, 0x40u);
    dma_write(dma, 0x4312u, 0x34u);
    dma_write(dma, 0x4313u, 0x12u);
    dma_write(dma, 0x4314u, 0x7eu);
    dma_write(dma, 0x4315u, 1u);
    dma_write(dma, 0x4316u, 0u);
    dma_startDma(dma, 2u, false);
    run_to_idle(dma);
    check(snes->abus[0x1234u] == 0xacu, "B-bus to A-bus transfer");
    check(dma->channel[1].aAdr == 0x1234u, "fixed source address");

    dma_startDma(dma, 0x84u, true);
    check(dma->channel[2].hdmaActive && dma->channel[7].hdmaActive &&
          !dma->dmaBusy, "HDMA selection does not start general DMA");
}

static void test_saveload(Dma *dma) {
    SaveLoadInfo info = {capture_save};
    dma_saveload(dma, &info);
    check(save_address == &dma->channel, "save span begins at channels");
    check(save_size == sizeof(*dma) - offsetof(Dma, channel),
          "save span includes DMA timing state");
}

int main(void) {
    Snes snes;
    memset(&snes, 0, sizeof(snes));
    Dma *dma = dma_init(&snes);
    check(dma != NULL, "DMA allocation");
    if (dma == NULL) return 1;
    test_reset_and_registers(dma);
    test_a_to_b(dma, &snes);
    test_b_to_a_fixed(dma, &snes);
    test_saveload(dma);
    dma_free(dma);
    return failures == 0 ? 0 : 1;
}
