/* Original, redistributable SPC program. Exercise the real SPC, DSP and timers
 * together; the scalar path is intentionally retained as the reference. */
#include "snes/apu.h"
#include "snes/spc.h"
#include "snes/saveload.h"
#include "support/audio_audit_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint64_t g_apu_timer0_total_ticks;
static int failures;

typedef struct State {
    SaveLoadInfo info;
    size_t offset;
    uint8_t bytes[524288];
} State;

static void check(bool ok, const char *message) {
    if (!ok) { fprintf(stderr, "APU batch: %s\n", message); ++failures; }
}

static void transfer(SaveLoadInfo *info, void *data, size_t count) {
    State *state = (State *)info;
    if (count > sizeof(state->bytes) - state->offset) {
        info->failed = true;
        return;
    }
    if (info->saving) memcpy(state->bytes + state->offset, data, count);
    else memcpy(data, state->bytes + state->offset, count);
    state->offset += count;
}

static void save(Apu *apu, State *state) {
    state->offset = 0;
    state->info = (SaveLoadInfo){.func = transfer, .portable = true, .saving = true};
    apu_saveload(apu, &state->info);
    check(!state->info.failed, "snapshot fits");
}

static void observe_opcode(Spc *spc, uint16_t pc) {
    if (pc != 0x400) return;
    /* Persist hook-visible clocks and timers, catching reordering even when
     * the eventual PCM happens to match. Also exercise an ARAM mutation. */
    Apu *apu = spc->apu;
    for (unsigned i = 0; i < 8; ++i)
        apu->ram[0x40 + i] = (uint8_t)(apu->cycleClock >> (i * 8));
    apu->ram[0x48] = apu->timer[2].cycles;
    apu->ram[0x49] = apu->dspSlot;
    apu->ram[0x304] ^= (uint8_t)apu->sampleClock;
}

static int charge_opcode(Spc *spc, uint16_t pc, int cycles) {
    (void)spc;
    return pc == 0x400 || pc == 0x402 ? 0 : cycles;
}

static Apu *fixture(bool extended) {
    static const uint8_t program[] = {
        0xe4, 0xfd, 0xc4, 0x20, /* MOV A,$FD; MOV $20,A (read/clear timer) */
        0xe4, 0xfe, 0xc4, 0x21, 0xe4, 0xff, 0xc4, 0x22,
        0xe4, 0xf4, 0xc4, 0xf5, /* input and output ports */
        0xbc, 0x8f, 0x02, 0xf2, 0xc4, 0xf3, /* pitch write */
        0xe4, 0xf3, 0xc4, 0x23, /* DSP read */
        0xe5, 0x00, 0x80, 0xc5, 0x02, 0x80, /* echo RAM read/write */
        0xac, 0x04, 0x03, /* INC !$0304 (BRR payload) */
        0x5f, 0x00, 0x04 /* JMP !$0400 */
    };
    Apu *apu = apu_init();
    if (!apu) return NULL;
    apu_reset(apu);
    apu->diagnosticCountersEnabled = false;
    apu->auditWritesEnabled = true;
    apu->romReadable = false;
    apu->spc->pc = 0x400;
    apu->cycles = UINT32_MAX - 64u;
    apu->cycleClock = UINT64_C(0x100000000);
    memcpy(apu->ram + 0x400, program, sizeof(program));
    apu->ram[0x201] = apu->ram[0x203] = 3; /* source 0 at $0300 */
    apu->ram[0x300] = 0xc3; /* looping BRR */
    for (unsigned i = 1; i < 9; ++i) apu->ram[0x300 + i] = (uint8_t)(i * 17);
    Dsp *dsp = apu->dsp;
    dsp_write(dsp, 0x5d, 2);
    dsp_write(dsp, 0x0c, 0x7f); dsp_write(dsp, 0x1c, 0x7f);
    dsp_write(dsp, 0x2c, 0x30); dsp_write(dsp, 0x3c, 0x30);
    dsp_write(dsp, 0x6c, 0); dsp_write(dsp, 0x6d, 0x80);
    dsp_write(dsp, 0x7d, 1); dsp_write(dsp, 0x0f, 0x7f);
    for (unsigned voice = 0; voice < (extended ? 40u : 8u); ++voice) {
        if (voice >= 8 && (voice & 7) < 6) continue;
        static const uint8_t regs[] = {0x40, 0x38, 0x20, 0x10, 0, 0, 0, 0x70};
        for (unsigned reg = 0; reg < sizeof(regs); ++reg) {
            uint8_t adr = (uint8_t)((voice & 7) * 16 + reg);
            if (voice < 8) dsp_write(dsp, adr, regs[reg]);
            else dsp_writeVirtualVoiceRegister(dsp, (int)voice, adr, regs[reg]);
        }
        if (voice >= 8) {
            dsp_writeVirtualVoiceControl(dsp, (int)voice, 0x4c, true);
            dsp_writeVirtualVoiceControl(dsp, (int)voice, 0x4d, true);
        }
    }
    dsp_write(dsp, 0x4c, 0xff); dsp_write(dsp, 0x4d, 0xff);
    for (unsigned t = 0; t < 3; ++t) {
        apu_cpuWrite(apu, (uint16_t)(0xfa + t), (uint8_t)t);
    }
    apu_cpuWrite(apu, 0xf1, 7);
    for (unsigned t = 0; t < 3; ++t) apu->timer[t].divider = 0xfe;
    for (unsigned i = 0; i < 64; ++i)
        apu_schedulePortWrite(apu, (uint8_t)(i & 3), (uint8_t)(i * 13), i * 3);
    return apu;
}

static void equivalence(void) {
    static const uint32_t spans[] = {0, 1, 2, 3, 7, 15, 16, 31, 32, 33, 127, 256, 1025, 32769};
    State *a = calloc(1, sizeof(*a)), *b = calloc(1, sizeof(*b));
    check(a && b, "state allocation");
    if (!a || !b) { free(a); free(b); return; }
    g_spc_opcode_patch_hook = observe_opcode;
    g_spc_opcode_cycle_hook = charge_opcode;
    for (unsigned phase = 0; phase < 32; ++phase) {
        dsp_setExtendedVoicesEnabled(true);
        Apu *reference = fixture(true), *batched = fixture(true);
        check(reference && batched, "APU allocation");
        if (!reference || !batched) { apu_free(reference); apu_free(batched); break; }
        for (unsigned c = 0; c < phase; ++c) { apu_cycle(reference); apu_cycle(batched); }
        for (unsigned mode = 0; mode < 5; ++mode) {
            audio_trace_set_enabled(mode == 4);
            reference->diagnosticCountersEnabled = batched->diagnosticCountersEnabled = mode == 2;
            reference->spc->stopped = batched->spc->stopped = mode == 3;
            apu_cpuWrite(reference, 0xf1, mode == 1 ? 0 : 7);
            apu_cpuWrite(batched, 0xf1, mode == 1 ? 0 : 7);
            if (mode == 1) {
                /* Resume a portable snapshot in the middle of a sample. */
                save(reference, a);
                a->offset = 0; a->info.saving = false;
                apu_saveload(batched, &a->info);
                check(!a->info.failed, "mid-slot restore");
                memcpy(batched->ramWritten, reference->ramWritten, sizeof(reference->ramWritten));
            }
            for (unsigned i = 0; i < sizeof(spans) / sizeof(spans[0]); ++i) {
                uint64_t ticks = g_apu_timer0_total_ticks;
                for (uint32_t c = 0; c < spans[i]; ++c) apu_cycle(reference);
                uint64_t expected_ticks = g_apu_timer0_total_ticks - ticks;
                ticks = g_apu_timer0_total_ticks;
                apu_runCycles(batched, spans[i]);
                check(g_apu_timer0_total_ticks - ticks == expected_ticks, "timer tick accounting");
                save(reference, a); save(batched, b);
                check(a->offset == b->offset && !memcmp(a->bytes, b->bytes, a->offset),
                      "scalar/batched state, PCM, ARAM, ports and clocks match");
                check(!memcmp(reference->ramWritten, batched->ramWritten, sizeof(reference->ramWritten)),
                      "write coverage matches");
            }
        }
        apu_free(reference); apu_free(batched);
    }
    free(a); free(b);
    g_spc_opcode_patch_hook = NULL; g_spc_opcode_cycle_hook = NULL;
    audio_trace_set_enabled(0);
    dsp_setExtendedVoicesEnabled(false);
}

/* Optional focused benchmark, not a timing assertion in CTest. Alternate paths
 * in the same binary and check full-state hashes, not just execution time. */
static int benchmark(void) {
    const unsigned samples = 1000000;
    State *state = calloc(1, sizeof(*state));
    if (!state) return 1;
    for (unsigned extended = 0; extended < 2; ++extended) {
        uint32_t expected = 0;
        dsp_setExtendedVoicesEnabled(extended != 0);
        for (unsigned run = 0; run < 6; ++run) {
            bool batched = ((run + run / 2) & 1) != 0;
            Apu *apu = fixture(extended != 0);
            if (!apu) { free(state); return 1; }
            apu->auditWritesEnabled = false;
            uint64_t start = audio_trace_wall_ns();
            for (unsigned s = 0; s < samples / 8; ++s) {
                if (batched) apu_runCycles(apu, 256);
                else for (unsigned c = 0; c < 256; ++c) apu_cycle(apu);
                /* Model a draining callback, not the full-ring/drop path. */
                apu->dsp->sampleRead = apu->dsp->sampleWrite;
            }
            uint64_t elapsed = audio_trace_wall_ns() - start;
            save(apu, state);
            uint32_t hash = 2166136261u;
            for (size_t i = 0; i < state->offset; ++i) hash = (hash ^ state->bytes[i]) * 16777619u;
            if (run == 0) expected = hash;
            check(hash == expected, "benchmark state hashes match");
            printf("%s %s %.2f ns/sample state=%08x\n", extended ? "extended" : "native",
                   batched ? "batched" : "scalar", (double)elapsed / samples, hash);
            apu_free(apu);
        }
    }
    free(state);
    return failures != 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--benchmark")) return benchmark();
    equivalence();
    return failures != 0;
}
