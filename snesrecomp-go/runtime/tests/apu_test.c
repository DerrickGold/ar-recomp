#include "snes/apu.h"
#include "snes/saveload.h"
#include "snes/spc.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t g_apu_timer0_total_ticks;
int snes_frame_counter;
extern uint64_t g_spc_write_counts[0x100];
extern uint64_t g_spc_pc_histogram[0x10000];
extern int g_spc_pc_max_seen;
extern uint64_t g_spc_outport_value_counts[4 * 256];
static int failures;
static unsigned opcode_runs;
static unsigned dsp_observer_calls;
static unsigned save_calls;
static unsigned extra_save_calls;

typedef struct MemoryState {
    SaveLoadInfo info;
    uint8_t bytes[131072];
    size_t offset;
    bool loading;
} MemoryState;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "runtime APU contract failed: %s\n", message);
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
void dsp_clock(Dsp *dsp) {
    dsp->firBufferIndex = (uint8_t)((dsp->firBufferIndex + 1u) & 31u);
    if (dsp->firBufferIndex == 0u) ++dsp->sampleWrite;
}
uint8_t dsp_read(Dsp *dsp, uint8_t address) { return dsp->ram[address]; }
void dsp_write(Dsp *dsp, uint8_t address, uint8_t value) { dsp->ram[address] = value; }
void dsp_saveload(Dsp *dsp, SaveLoadInfo *info) { (void)dsp; (void)info; }

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

static void transfer_state(SaveLoadInfo *info, void *data, size_t size) {
    MemoryState *state = (MemoryState *)info;
    if (state->offset + size > sizeof(state->bytes)) {
        info->failed = true;
        return;
    }
    if (state->loading) memcpy(data, state->bytes + state->offset, size);
    else memcpy(state->bytes + state->offset, data, size);
    state->offset += size;
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
    apu_schedulePortWrite(apu, 2u, 0x33u, 1u);
    apu_cycle(apu);
    check(apu->inPorts[2] == 0u && apu->sampleClock == 0u,
          "future write waits while the first sample is in flight");
    for (unsigned index = 1; index < 32u; ++index) apu_cycle(apu);
    check(apu->sampleClock == 1u && apu->inPorts[2] == 0u,
          "DSP publishes a sample after exactly 32 slots");
    apu_cycle(apu);
    check(apu->inPorts[2] == 0x33u,
          "queued write applies on produced-sample clock");
    check(((Dsp *)apu->dsp)->sampleWrite == 1u && opcode_runs != 0u &&
              apu_cycle_count(apu) == 33u,
          "DSP, SPC, and semantic APU clock advance together");

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

static void test_diagnostic_counter_gate(Apu *apu) {
    memset(g_spc_write_counts, 0, sizeof(uint64_t) * 0x100u);
    memset(g_spc_pc_histogram, 0, sizeof(uint64_t) * 0x10000u);
    memset(g_spc_outport_value_counts, 0, sizeof(uint64_t) * 4u * 256u);
    g_spc_pc_max_seen = 0;
    apu_reset(apu);
    apu->diagnosticCountersEnabled = false;
    apu->spc->pc = 0x1234u;
    apu->cpuCyclesLeft = 0u;
    apu_cycle(apu);
    apu_cpuWrite(apu, 0x20u, 0x55u);
    apu_cpuWrite(apu, 0xf4u, 0x66u);
    check(g_spc_pc_histogram[0x1234u] == 0u &&
              g_spc_write_counts[0x20u] == 0u &&
              g_spc_outport_value_counts[0x66u] == 0u,
          "SPC diagnostic counters remain inert by default");

    apu->diagnosticCountersEnabled = true;
    apu->spc->pc = 0x2345u;
    apu->cpuCyclesLeft = 0u;
    apu_cycle(apu);
    apu_cpuWrite(apu, 0x20u, 0x77u);
    apu_cpuWrite(apu, 0xf4u, 0x88u);
    check(g_spc_pc_histogram[0x2345u] == 1u &&
              g_spc_pc_max_seen == 0x2345 &&
              g_spc_write_counts[0x20u] == 1u &&
              g_spc_outport_value_counts[0x88u] == 1u,
          "explicit SPC diagnostics retain the legacy counters");
    apu->diagnosticCountersEnabled = false;
}

static void test_aram_write_coverage(Apu *apu) {
    apu_reset(apu);
    apu->auditWritesEnabled = true;
    apu_cpuWrite(apu, 0x2345u, 0xa5u);
    check((apu->ramWritten[0x2345u >> 3] &
           (uint8_t)(1u << (0x2345u & 7u))) != 0u &&
          (apu->ramWritten[0x2344u >> 3] &
           (uint8_t)(1u << (0x2344u & 7u))) == 0u,
          "SPC writes publish byte-precise ARAM coverage");
    apu_reset(apu);
    check(apu->ramWritten[0x2345u >> 3] == 0u,
          "ARAM write coverage resets with APU state");
    apu->auditWritesEnabled = false;
}

static void test_saveload(Apu *apu) {
    SaveLoadInfo info = {capture_save};
    g_apu_extra_saveload_hook = capture_extra;
    apu_saveload(apu, &info);
    check(save_calls == 12u && extra_save_calls == 1u,
          "APU core and scheduler save spans plus extension hook");
}

static void test_portable_timeline_saveload(Apu *apu) {
    MemoryState state = {0};
    g_apu_extra_saveload_hook = NULL;
    apu_reset(apu);
    apu->cycleClock = UINT64_C(0x1122334455667788);
    apu->timelineTargetCycles = UINT64_C(0x2233445566778899);
    apu->sampleClock = UINT64_C(0x33445566778899aa);
    apu_schedulePortWrite(apu, 3u, 0x5au,
                          UINT64_C(0x445566778899aabb));
    state.info.func = transfer_state;
    state.info.saving = true;
    state.info.portable = true;
    apu_saveload(apu, &state.info);
    check(!state.info.failed, "portable APU timeline save");

    apu_reset(apu);
    state.offset = 0u;
    state.loading = true;
    state.info.saving = false;
    apu_saveload(apu, &state.info);
    check(!state.info.failed &&
              apu_cycle_count(apu) == UINT64_C(0x1122334455667788) &&
              apu->timelineTargetCycles == UINT64_C(0x2233445566778899) &&
              apu->sampleClock == UINT64_C(0x33445566778899aa) &&
              apu->portQTail - apu->portQHead == 1u &&
              apu->portQueue[apu->portQHead &
                  (APU_PORT_QUEUE_LEN - 1u)].target_sample ==
                      UINT64_C(0x445566778899aabb),
          "portable APU timeline and scheduled writes restore exactly");
}

int main(void) {
    Apu *apu = apu_init();
    check(apu != NULL, "APU allocation");
    if (apu == NULL) return 1;
    test_reset_and_io(apu);
    test_dsp_hooks(apu);
    test_queue_and_cycles(apu);
    test_timers(apu);
    test_diagnostic_counter_gate(apu);
    test_aram_write_coverage(apu);
    test_saveload(apu);
    test_portable_timeline_saveload(apu);
    apu_free(apu);
    return failures == 0 ? 0 : 1;
}
