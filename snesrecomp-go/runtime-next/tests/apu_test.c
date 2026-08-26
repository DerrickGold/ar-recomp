#include "snes/apu.h"
#include "snes/saveload.h"
#include "snes/spc.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t g_apu_timer0_total_ticks;
static int failures;
static unsigned opcode_runs;
static unsigned apply_calls;
static unsigned dsp_observer_calls;
static unsigned save_calls;
static unsigned extra_save_calls;
static uint8_t applied_value;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "runtime-next APU contract failed: %s\n", message);
        ++failures;
    }
}

Spc *spc_init(Apu *apu) {
    Spc *spc = (Spc *)calloc(1u, sizeof(*spc));
    if (spc != NULL) spc->apu = apu;
    return spc;
}
void spc_free(Spc *spc) { free(spc); }
void spc_reset(Spc *spc) {
    memset(&spc->a, 0, sizeof(*spc) - offsetof(Spc, a));
    spc->pc = (uint16_t)(apu_cpuRead(spc->apu, 0xfffeu) |
                         ((uint16_t)apu_cpuRead(spc->apu, 0xffffu) << 8));
}
int spc_runOpcode(Spc *spc) { ++opcode_runs; ++spc->pc; return 2; }
void spc_saveload(Spc *spc, SaveLoadInfo *info) { (void)spc; (void)info; }

Dsp *dsp_init(uint8_t *ram) { (void)ram; return (Dsp *)calloc(1u, sizeof(Dsp)); }
void dsp_free(Dsp *dsp) { free(dsp); }
void dsp_reset(Dsp *dsp) { memset(dsp, 0, sizeof(*dsp)); }
void dsp_cycle(Dsp *dsp) { ++dsp->sampleWrite; }
uint8_t dsp_read(Dsp *dsp, uint8_t address) { return dsp->ram[address]; }
void dsp_write(Dsp *dsp, uint8_t address, uint8_t value) { dsp->ram[address] = value; }
void dsp_saveload(Dsp *dsp, SaveLoadInfo *info) { (void)dsp; (void)info; }

static void on_apply(Apu *apu, uint8_t port, uint8_t value) {
    (void)apu; (void)port;
    ++apply_calls;
    applied_value = value;
}
static void on_dsp(Apu *apu, uint8_t address, uint8_t value) {
    (void)apu; (void)address; (void)value;
    ++dsp_observer_calls;
}
static bool filter_dsp(Apu *apu, uint8_t address, uint8_t *value) {
    (void)apu; (void)address;
    *value ^= 0xffu;
    return true;
}
static void capture_save(SaveLoadInfo *info, void *data, size_t size) {
    (void)info; (void)data; (void)size;
    ++save_calls;
}
static void capture_extra(Apu *apu, SaveLoadInfo *info) {
    (void)apu; (void)info;
    ++extra_save_calls;
}

static void test_reset_and_io(Apu *apu) {
    apu_reset(apu);
    check(apu->romReadable && apu->spc->pc == 0xffc0u,
          "reset selects independently assembled bootstrap");
    check(apu_cpuRead(apu, 0xffc0u) == 0xe8u &&
          apu_cpuRead(apu, 0xfffeu) == 0xc0u, "bootstrap bytes and vector");
    apu_cpuWrite(apu, 0xf2u, 0x22u);
    apu_cpuWrite(apu, 0xf3u, 0x5au);
    check(apu_cpuRead(apu, 0xf3u) == 0x5au, "DSP address/data ports");
    apu_cpuWrite(apu, 0xf4u, 0x81u);
    check(apu->outPorts[0] == 0x81u, "SPC output port write");
    apu->inPorts[1] = 0x42u;
    check(apu_cpuRead(apu, 0xf5u) == 0x42u, "SPC input port read");
}

static void test_dsp_hooks(Apu *apu) {
    g_apu_spc_dsp_write_hook = on_dsp;
    g_apu_spc_dsp_write_filter_hook = filter_dsp;
    apu_cpuWrite(apu, 0xf2u, 0x12u);
    apu_cpuWrite(apu, 0xf3u, 0x0fu);
    check(dsp_observer_calls == 1u && apu_cpuRead(apu, 0xf3u) == 0xf0u,
          "DSP observer sees original and filter transforms write");
    g_apu_spc_dsp_write_hook = NULL;
    g_apu_spc_dsp_write_filter_hook = NULL;
}

static void test_queue_and_cycles(Apu *apu) {
    apu_reset(apu);
    g_apu_port_apply_trace_hook = on_apply;
    apu_schedulePortWrite(apu, 2u, 0x33u, 1u);
    apu_cycle(apu);
    check(apu->inPorts[2] == 0u && apu->sampleClock == 1u,
          "future write waits through first sample boundary");
    for (unsigned index = 0; index < 32u; ++index) apu_cycle(apu);
    check(apu->inPorts[2] == 0x33u && apply_calls == 1u && applied_value == 0x33u,
          "queued write applies on produced-sample clock");
    check(((Dsp *)apu->dsp)->sampleWrite == 2u && opcode_runs != 0u,
          "DSP and SPC advance on their clocks");

    apu_clearPortQueue(apu);
    for (unsigned index = 0; index <= APU_PORT_QUEUE_LEN; ++index) {
        apu_schedulePortWrite(apu, 0u, (uint8_t)index, 10000u + index);
    }
    check(apu->portQTail - apu->portQHead == APU_PORT_QUEUE_LEN &&
          apu->inPorts[0] == 0u, "queue overflow applies oldest in order");
}

static void test_timers(Apu *apu) {
    apu_reset(apu);
    apu_cpuWrite(apu, 0xfau, 1u);
    apu_cpuWrite(apu, 0xf1u, 1u);
    apu_cycle(apu);
    check(apu->timer[0].counter == 1u && g_apu_timer0_total_ticks == 1u,
          "timer divider tick");
    check(apu_cpuRead(apu, 0xfdu) == 1u && apu_cpuRead(apu, 0xfdu) == 0u,
          "timer read clears counter");
    apu->inPorts[0] = 1u; apu->inPorts[1] = 2u;
    apu_cpuWrite(apu, 0xf1u, 0x10u);
    check(apu->inPorts[0] == 0u && apu->inPorts[1] == 0u,
          "control port clears input pair");
}

static void test_saveload(Apu *apu) {
    SaveLoadInfo info = {capture_save};
    g_apu_extra_saveload_hook = capture_extra;
    apu_saveload(apu, &info);
    check(save_calls == 10u && extra_save_calls == 1u,
          "APU core and scheduler save spans plus extension hook");
}

int main(void) {
    Apu *apu = apu_init();
    check(apu != NULL, "APU allocation");
    if (apu == NULL) return 1;
    test_reset_and_io(apu);
    test_dsp_hooks(apu);
    test_queue_and_cycles(apu);
    test_timers(apu);
    test_saveload(apu);
    apu_free(apu);
    return failures == 0 ? 0 : 1;
}
