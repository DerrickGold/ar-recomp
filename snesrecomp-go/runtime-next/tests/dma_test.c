#include "snes/dma.h"
#include "snes/saveload.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum AccessKind {
    kReadABus = 1,
    kWriteABus,
    kReadBBus,
    kWriteBBus,
};

typedef struct AccessEvent {
    uint32_t address;
    uint8_t value;
    uint8_t kind;
} AccessEvent;

enum { kAccessEventCapacity = 512 };

struct Snes {
    uint8_t abus[0x10000];
    uint8_t bbus[0x100];
    AccessEvent access[kAccessEventCapacity];
    size_t access_count;
    uint64_t access_hash;
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

static void check_case(int condition, const char *case_name,
                       const char *detail) {
    if (!condition) {
        fprintf(stderr, "runtime-next DMA contract failed: %s: %s\n",
                case_name, detail);
        ++failures;
    }
}

static void record_access(Snes *snes, uint8_t kind, uint32_t address,
                          uint8_t value) {
    if (snes->access_count < kAccessEventCapacity) {
        AccessEvent *event = &snes->access[snes->access_count];
        event->kind = kind;
        event->address = address;
        event->value = value;
    }
    ++snes->access_count;

    uint64_t hash = snes->access_hash ^ kind;
    hash *= UINT64_C(1099511628211);
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        hash ^= (uint8_t)(address >> shift);
        hash *= UINT64_C(1099511628211);
    }
    snes->access_hash = (hash ^ value) * UINT64_C(1099511628211);
}

uint8_t snes_read(Snes *snes, uint32_t address) {
    const uint8_t value = snes->abus[(uint16_t)address];
    record_access(snes, kReadABus, address, value);
    return value;
}

void snes_write(Snes *snes, uint32_t address, uint8_t value) {
    record_access(snes, kWriteABus, address, value);
    snes->abus[(uint16_t)address] = value;
}

uint8_t snes_readBBus(Snes *snes, uint8_t address) {
    const uint8_t value = snes->bbus[address];
    record_access(snes, kReadBBus, address, value);
    return value;
}

void snes_writeBBus(Snes *snes, uint8_t address, uint8_t value) {
    record_access(snes, kWriteBBus, address, value);
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
    while (dma_cycle(dma) && guard++ < 400000u) {
    }
    check(guard < 400000u, "DMA reaches idle");
}

static void seed_snes(Snes *snes) {
    memset(snes, 0, sizeof(*snes));
    for (unsigned index = 0u; index < sizeof(snes->abus); ++index) {
        snes->abus[index] = (uint8_t)(index * 37u + 11u);
    }
    for (unsigned index = 0u; index < sizeof(snes->bbus); ++index) {
        snes->bbus[index] = (uint8_t)(index * 13u + 7u);
    }
}

static void configure_channel(Dma *dma, unsigned index, uint8_t control,
                              uint8_t bbus, uint16_t abus, uint8_t bank,
                              uint16_t size) {
    const uint16_t base = (uint16_t)(0x4300u + index * 0x10u);
    dma_write(dma, base, control);
    dma_write(dma, (uint16_t)(base + 1u), bbus);
    dma_write(dma, (uint16_t)(base + 2u), (uint8_t)abus);
    dma_write(dma, (uint16_t)(base + 3u), (uint8_t)(abus >> 8));
    dma_write(dma, (uint16_t)(base + 4u), bank);
    dma_write(dma, (uint16_t)(base + 5u), (uint8_t)size);
    dma_write(dma, (uint16_t)(base + 6u), (uint8_t)(size >> 8));
}

static int channels_equal(const DmaChannel *left, const DmaChannel *right) {
    return left->bAdr == right->bAdr &&
           left->aAdr == right->aAdr &&
           left->aBank == right->aBank &&
           left->size == right->size &&
           left->indBank == right->indBank &&
           left->tableAdr == right->tableAdr &&
           left->repCount == right->repCount &&
           left->unusedByte == right->unusedByte &&
           left->dmaActive == right->dmaActive &&
           left->hdmaActive == right->hdmaActive &&
           left->mode == right->mode &&
           left->fixed == right->fixed &&
           left->decrement == right->decrement &&
           left->indirect == right->indirect &&
           left->fromB == right->fromB &&
           left->unusedBit == right->unusedBit &&
           left->doTransfer == right->doTransfer &&
           left->terminated == right->terminated &&
           left->offIndex == right->offIndex;
}

static void check_equivalent(const char *case_name, const Dma *reference,
                             const Dma *fast, const Snes *reference_snes,
                             const Snes *fast_snes) {
    int channel_state_equal = 1;
    for (unsigned index = 0u; index < kDmaChannelCount; ++index) {
        channel_state_equal &= channels_equal(&reference->channel[index],
                                              &fast->channel[index]);
    }
    check_case(channel_state_equal, case_name, "channel state matches stepped path");
    check_case(reference->dmaBusy == fast->dmaBusy &&
               reference->dmaTimer == fast->dmaTimer,
               case_name, "busy and timer state match stepped path");
    check_case(memcmp(reference_snes->abus, fast_snes->abus,
                      sizeof(reference_snes->abus)) == 0,
               case_name, "A-bus side effects match stepped path");
    check_case(memcmp(reference_snes->bbus, fast_snes->bbus,
                      sizeof(reference_snes->bbus)) == 0,
               case_name, "B-bus side effects match stepped path");
    check_case(reference_snes->access_count == fast_snes->access_count &&
               reference_snes->access_hash == fast_snes->access_hash,
               case_name, "access ordering matches stepped path");

    const size_t captured = reference_snes->access_count < kAccessEventCapacity
        ? reference_snes->access_count : kAccessEventCapacity;
    check_case(memcmp(reference_snes->access, fast_snes->access,
                      captured * sizeof(reference_snes->access[0])) == 0,
               case_name, "captured access trace matches stepped path");
}

static void test_fast_path_modes(void) {
    for (unsigned direction = 0u; direction < 2u; ++direction) {
        for (unsigned mode = 0u; mode < 8u; ++mode) {
            Snes reference_snes;
            Snes fast_snes;
            seed_snes(&reference_snes);
            fast_snes = reference_snes;
            Dma *reference = dma_init(&reference_snes);
            Dma *fast = dma_init(&fast_snes);
            check(reference != NULL && fast != NULL, "differential DMA allocation");
            if (reference == NULL || fast == NULL) {
                dma_free(reference);
                dma_free(fast);
                return;
            }

            uint8_t control = (uint8_t)(mode | (direction != 0u ? 0x80u : 0u));
            if (mode % 3u == 1u) control |= 0x10u;
            if (mode % 3u == 2u) control |= 0x08u;
            configure_channel(reference, 0u, control, 0x20u, 0x1004u, 0x7eu, 9u);
            configure_channel(fast, 0u, control, 0x20u, 0x1004u, 0x7eu, 9u);
            reference->dmaTimer = 10u;
            fast->dmaTimer = 10u;
            dma_startDma(reference, 1u, false);
            dma_startDma(fast, 1u, false);
            run_to_idle(reference);
            dma_run_to_idle(fast);

            char case_name[64];
            snprintf(case_name, sizeof(case_name), "mode %u %s",
                     mode, direction != 0u ? "B-to-A" : "A-to-B");
            check_equivalent(case_name, reference, fast,
                             &reference_snes, &fast_snes);
            dma_free(reference);
            dma_free(fast);
        }
    }
}

static void test_fast_path_priority_and_wrap(void) {
    Snes reference_snes;
    Snes fast_snes;
    seed_snes(&reference_snes);
    fast_snes = reference_snes;
    Dma *reference = dma_init(&reference_snes);
    Dma *fast = dma_init(&fast_snes);
    check(reference != NULL && fast != NULL, "priority DMA allocation");
    if (reference == NULL || fast == NULL) {
        dma_free(reference);
        dma_free(fast);
        return;
    }

    configure_channel(reference, 0u, 0x00u, 0x10u, 0xfffeu, 0x7eu, 4u);
    configure_channel(fast, 0u, 0x00u, 0x10u, 0xfffeu, 0x7eu, 4u);
    configure_channel(reference, 3u, 0x88u, 0x20u, 0x2000u, 0x7fu, 1u);
    configure_channel(fast, 3u, 0x88u, 0x20u, 0x2000u, 0x7fu, 1u);
    configure_channel(reference, 7u, 0x01u, 0x30u, 0x3000u, 0x7eu, 2u);
    configure_channel(fast, 7u, 0x01u, 0x30u, 0x3000u, 0x7eu, 2u);
    dma_startDma(reference, 0x89u, false);
    dma_startDma(fast, 0x89u, false);
    run_to_idle(reference);
    dma_run_to_idle(fast);
    check_equivalent("priority and address wrap", reference, fast,
                     &reference_snes, &fast_snes);

    check_case(reference_snes.access_count == 14u,
               "priority and address wrap", "expected access count");
    check_case(reference_snes.access[0].kind == kReadABus &&
               reference_snes.access[0].address == 0x7efffeu &&
               reference_snes.access[6].kind == kReadABus &&
               reference_snes.access[6].address == 0x7e0001u &&
               reference_snes.access[8].kind == kReadBBus &&
               reference_snes.access[8].address == 0x20u &&
               reference_snes.access[10].kind == kReadABus &&
               reference_snes.access[10].address == 0x7e3000u,
               "priority and address wrap",
               "channels are ascending and 16-bit A-bus address wraps");
    dma_free(reference);
    dma_free(fast);
}

static void test_fast_path_zero_size(void) {
    Snes reference_snes;
    Snes fast_snes;
    seed_snes(&reference_snes);
    fast_snes = reference_snes;
    Dma *reference = dma_init(&reference_snes);
    Dma *fast = dma_init(&fast_snes);
    check(reference != NULL && fast != NULL, "zero-size DMA allocation");
    if (reference == NULL || fast == NULL) {
        dma_free(reference);
        dma_free(fast);
        return;
    }

    configure_channel(reference, 2u, 0x08u, 0x40u, 0x1234u, 0x7eu, 0u);
    configure_channel(fast, 2u, 0x08u, 0x40u, 0x1234u, 0x7eu, 0u);
    dma_startDma(reference, 0x04u, false);
    dma_startDma(fast, 0x04u, false);
    run_to_idle(reference);
    dma_run_to_idle(fast);
    check_equivalent("zero size means 65536 bytes", reference, fast,
                     &reference_snes, &fast_snes);
    check_case(reference_snes.access_count == 2u * 65536u,
               "zero size means 65536 bytes", "full transfer length retained");
    dma_free(reference);
    dma_free(fast);
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
    trace_count = 0u;
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
    g_dma_start_trace_hook = NULL;
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
    test_fast_path_modes();
    test_fast_path_priority_and_wrap();
    test_fast_path_zero_size();
    return failures == 0 ? 0 : 1;
}
