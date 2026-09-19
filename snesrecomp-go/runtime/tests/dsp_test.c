#include "snes/dsp.h"
#include "snes/dsp_accuracy_bridge.h"
#include "snes/saveload.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct DspShadow DspShadow;
static int failures;
static unsigned trace_writes;
static unsigned trace_samples;
static unsigned trace_drops;
static unsigned trace_consumes;

DspShadow *dsp_shadow_create(void) { return NULL; }
void dsp_shadow_free(DspShadow *shadow) { (void)shadow; }
void dsp_shadow_process(DspShadow *shadow, Dsp *dsp, int canonical_left,
                        int canonical_right, int *output_left,
                        int *output_right) {
    (void)shadow;
    (void)dsp;
    *output_left = canonical_left;
    *output_right = canonical_right;
}
void audio_trace_on_sample(int16_t left, int16_t right, int dropped,
                           uint32_t ring_fill) {
    (void)left;
    (void)right;
    (void)ring_fill;
    ++trace_samples;
    trace_drops += dropped != 0;
}
void audio_trace_on_reg_write(uint8_t address, uint8_t value) {
    (void)address;
    (void)value;
    ++trace_writes;
}
void audio_trace_on_consume(uint64_t read_index, uint32_t count,
                            uint32_t available_after) {
    (void)read_index;
    (void)count;
    (void)available_after;
    ++trace_consumes;
}

static void check(bool condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "runtime DSP failed: %s\n", message);
    ++failures;
}

static Dsp *new_dsp(uint8_t *ram) {
    Dsp *dsp;
    memset(ram, 0, 0x10000u);
    dsp = dsp_init(ram);
    check(dsp != NULL, "allocation");
    if (dsp == NULL) return NULL;
    dsp_reset(dsp);
    dsp_write(dsp, 0x0cu, 0x7fu);
    dsp_write(dsp, 0x1cu, 0x7fu);
    dsp_write(dsp, 0x6cu, 0x20u);
    return dsp;
}

static void install_looping_brr(uint8_t *ram, uint8_t source, uint16_t start) {
    static const uint8_t payload[8] = {
        0x01, 0x23, 0x45, 0x67, 0x01, 0x23, 0x45, 0x67
    };
    const uint16_t entry = (uint16_t)(0x0200u + source * 4u);
    ram[entry] = (uint8_t)start;
    ram[(uint16_t)(entry + 1u)] = (uint8_t)(start >> 8);
    ram[(uint16_t)(entry + 2u)] = (uint8_t)start;
    ram[(uint16_t)(entry + 3u)] = (uint8_t)(start >> 8);
    ram[start] = 0xc3u;
    memcpy(ram + start + 1u, payload, sizeof(payload));
}

static void write_voice(Dsp *dsp, int channel, uint8_t reg, uint8_t value) {
    if (channel < kDspHardwareVoiceCount)
        dsp_write(dsp, (uint8_t)(channel * 0x10 + reg), value);
    else
        dsp_writeVirtualVoiceRegister(
            dsp, channel, (uint8_t)((channel & 7) * 0x10 + reg), value);
}

static void control_voice(Dsp *dsp, int channel, uint8_t reg, bool enabled) {
    if (channel < kDspHardwareVoiceCount) {
        const uint8_t bit = (uint8_t)(1u << channel);
        dsp_writeHardwareVoiceMask(dsp, reg, enabled ? bit : 0u, bit);
    } else {
        dsp_writeVirtualVoiceControl(dsp, channel, reg, enabled);
    }
}

static void configure_voice(Dsp *dsp, int channel, DspVoiceBus bus,
                            bool echo) {
    write_voice(dsp, channel, 0u, 83u);
    write_voice(dsp, channel, 1u, (uint8_t)-37);
    write_voice(dsp, channel, 2u, 0x00u);
    write_voice(dsp, channel, 3u, 0x10u);
    write_voice(dsp, channel, 4u, 2u);
    write_voice(dsp, channel, 5u, 0u);
    write_voice(dsp, channel, 6u, 0u);
    write_voice(dsp, channel, 7u, 0x7fu);
    control_voice(dsp, channel, 0x4du, echo);
    control_voice(dsp, channel, 0x4cu, true);
    dsp_setVoiceBus(dsp, channel, bus);
}

static void run_samples(Dsp *dsp, unsigned count) {
    while (count-- != 0u) dsp_cycle(dsp);
}

static void test_reference_primitives(void) {
    const uint8_t ramp[9] = {
        0xc0u, 0x01u, 0x23u, 0x45u, 0x67u,
        0x01u, 0x23u, 0x45u, 0x67u
    };
    int16_t decoded[16] = {0};
    int16_t window[4] = {0x0800, 0, 0, 0};
    sr_dsp_accuracy_decode_brr(ramp, 0, 0, decoded);
    for (int index = 0; index < 16; ++index)
        check(decoded[index] == (index & 7) * 2048,
              "BRR shift/filter-0 ramp matches hardware arithmetic");
    check(sr_dsp_accuracy_gauss(window, 0x00u) == 0x000,
          "Gaussian newest corner coefficient");
    check(sr_dsp_accuracy_gauss(window, 0xffu) == 0x172,
          "Gaussian newest far coefficient");
    window[0] = window[1] = window[2] = window[3] = -0x4000;
    check(sr_dsp_accuracy_gauss(window, 0x00u) == 0x3ff8,
          "Gaussian second-add overflow glitch");
}

static void test_registers_keying_and_startup(void) {
    uint8_t ram[0x10000];
    Dsp *dsp = new_dsp(ram);
    if (dsp == NULL) return;
    install_looping_brr(ram, 2u, 0x0300u);
    dsp_write(dsp, 0x5du, 0x02u);
    configure_voice(dsp, 0, kDspVoiceBus_Unclassified, false);
    run_samples(dsp, 4u);
    check(dsp->sampleBuffer[3u * 2u] == 0,
          "hardware key-on startup remains silent");
    run_samples(dsp, 8u);
    check(dsp->channel[0].gain == 0x7f0u &&
              dsp->channel[0].sampleOut != 0,
          "direct gain and BRR become live after startup");
    check(dsp->sampleBuffer[10u * 2u] != 0 &&
              dsp->sampleBuffer[10u * 2u] !=
                  dsp->sampleBuffer[10u * 2u + 1u],
          "accurate voice is mixed into asymmetric stereo");
    check(dsp_read(dsp, 0x08u) == 0x7fu,
          "ENVX publishes through the slot pipeline");

    dsp->ram[0x7cu] = 0xffu;
    dsp_write(dsp, 0x7cu, 0x55u);
    check(dsp_read(dsp, 0x7cu) == 0u,
          "any ENDX write acknowledges all visible flags");
    check(trace_writes >= 12u && trace_samples >= 12u,
          "register and sample trace seams fire");
    dsp_free(dsp);
}

static void test_buses_and_parallel_virtual_bank(void) {
    uint8_t music_ram[0x10000], sfx_ram[0x10000], virtual_ram[0x10000];
    Dsp *music = new_dsp(music_ram);
    Dsp *sfx = new_dsp(sfx_ram);
    Dsp *virtual_dsp = new_dsp(virtual_ram);
    if (music == NULL || sfx == NULL || virtual_dsp == NULL) goto done;
    install_looping_brr(music_ram, 2u, 0x0300u);
    install_looping_brr(sfx_ram, 2u, 0x0300u);
    install_looping_brr(virtual_ram, 2u, 0x0300u);
    dsp_write(music, 0x5du, 2u);
    dsp_write(sfx, 0x5du, 2u);
    dsp_write(virtual_dsp, 0x5du, 2u);
    configure_voice(music, 0, kDspVoiceBus_Music, false);
    configure_voice(sfx, 0, kDspVoiceBus_Sfx, false);
    configure_voice(virtual_dsp, 8, kDspVoiceBus_Sfx, false);
    control_voice(virtual_dsp, 8, 0x4cu, true);

    dsp_setBusGains(0, 100);
    dsp_setExtendedVoicesEnabled(true);
    run_samples(music, 12u);
    run_samples(sfx, 12u);
    run_samples(virtual_dsp, 12u);
    check(music->sampleBuffer[10u * 2u] == 0 &&
              sfx->sampleBuffer[10u * 2u] != 0,
          "music and SFX buses have independent gains");
    check(virtual_dsp->sampleBuffer[10u * 2u] ==
              sfx->sampleBuffer[10u * 2u] &&
              virtual_dsp->sampleBuffer[10u * 2u + 1u] ==
              sfx->sampleBuffer[10u * 2u + 1u],
          "virtual lane is PCM-identical to a corresponding hardware lane");
    check(dsp_activeVoiceCount() == 40,
          "four parallel virtual banks expose 32 additional voices");
done:
    dsp_free(music);
    dsp_free(sfx);
    dsp_free(virtual_dsp);
    dsp_setBusGains(100, 100);
    dsp_setExtendedVoicesEnabled(false);
}

typedef struct MemoryState {
    SaveLoadInfo info;
    uint8_t bytes[131072];
    size_t offset;
    bool loading;
} MemoryState;

static void transfer_state(SaveLoadInfo *info, void *data, size_t size) {
    MemoryState *state = (MemoryState *)info;
    check(state->offset + size <= sizeof(state->bytes), "save span fits");
    if (state->offset + size > sizeof(state->bytes)) {
        info->failed = true;
        return;
    }
    if (state->loading) memcpy(data, state->bytes + state->offset, size);
    else memcpy(state->bytes + state->offset, data, size);
    state->offset += size;
}

static void test_state_ring_and_resampling(void) {
    uint8_t ram[0x10000];
    int16_t expected[16 * 2], actual[16 * 2];
    Dsp *dsp = new_dsp(ram);
    MemoryState state;
    uint32_t saved_write;
    if (dsp == NULL) return;
    install_looping_brr(ram, 2u, 0x0300u);
    dsp_write(dsp, 0x5du, 2u);
    dsp_setExtendedVoicesEnabled(true);
    configure_voice(dsp, 8, kDspVoiceBus_Sfx, false);
    run_samples(dsp, 20u);

    memset(&state, 0, sizeof(state));
    state.info.func = transfer_state;
    state.info.saving = true;
    state.info.portable = true;
    saved_write = dsp->sampleWrite;
    dsp_saveload(dsp, &state.info);
    check(!state.info.failed && state.offset > sizeof(dsp->sampleBuffer),
          "snapshot includes all five accurate DSP banks");
    run_samples(dsp, 16u);
    memcpy(expected, dsp->sampleBuffer +
           (saved_write & (DSP_SAMPLE_RING - 1u)) * 2u, sizeof(expected));

    state.offset = 0u;
    state.loading = true;
    state.info.saving = false;
    dsp_saveload(dsp, &state.info);
    check(!state.info.failed && dsp->sampleWrite == saved_write,
          "accurate DSP state and ring cursors restore");
    run_samples(dsp, 16u);
    memcpy(actual, dsp->sampleBuffer +
           (saved_write & (DSP_SAMPLE_RING - 1u)) * 2u, sizeof(actual));
    check(memcmp(actual, expected, sizeof(actual)) == 0,
          "restored virtual bank continues PCM deterministically");

    dsp->sampleRead = 0u;
    dsp->sampleWrite = DSP_SAMPLE_RING;
    {
        const unsigned drops_before = trace_drops;
        dsp_cycle(dsp);
        check(dsp->sampleWrite == DSP_SAMPLE_RING &&
                  trace_drops == drops_before + 1u,
              "full ring drops without overwriting unread samples");
    }
    dsp->sampleRead = dsp->sampleWrite = 0u;
    dsp->sampleBuffer[0] = 0;
    dsp->sampleBuffer[1] = 100;
    dsp->sampleBuffer[2] = 1000;
    dsp->sampleBuffer[3] = 1100;
    dsp->sampleWrite = 2u;
    {
        double phase = 0.5;
        int16_t output[2] = {0, 0};
        dsp_getSamplesResampled(dsp, output, 1, 1.0, &phase);
        check(output[0] == 500 && output[1] == 600 &&
                  dsp->sampleRead == 1u && phase == 0.5 &&
                  trace_consumes > 0u,
              "continuous resampler preserves fractional phase");
    }
    dsp_free(dsp);
    dsp_setExtendedVoicesEnabled(false);
}

static void exercise_slot_writes(Dsp *dsp) {
    for (unsigned cycle = 0; cycle < 32u * 24u; ++cycle) {
        const int voice = (cycle & 1) ? 15 : 6;
        if (cycle % 17 == 0) write_voice(dsp, voice, 2, (uint8_t)cycle);
        if (cycle % 29 == 0) write_voice(dsp, voice, 5, (uint8_t)cycle);
        if (cycle % 31 == 0) control_voice(dsp, voice, 0x4c, true);
        if (cycle % 37 == 0) dsp_write(dsp, 0x7c, 0xff);
        if (cycle % 41 == 0) dsp_write(dsp, 0x0f, (uint8_t)cycle);
        dsp_clock(dsp);
    }
}

static void test_every_slot_continuation(void) {
    uint8_t ram[0x10000], saved_ram[0x10000], expected_ram[0x10000];
    MemoryState *saved = calloc(1, sizeof(*saved));
    MemoryState *expected = calloc(1, sizeof(*expected));
    MemoryState *actual = calloc(1, sizeof(*actual));
    check(saved && expected && actual, "mid-slot state allocation");
    if (!saved || !expected || !actual) goto done;
    dsp_setExtendedVoicesEnabled(true);
    for (unsigned slot = 0; slot < 32; ++slot) {
        Dsp *dsp = new_dsp(ram);
        if (!dsp) break;
        install_looping_brr(ram, 2, 0x300);
        dsp_write(dsp, 0x5d, 2);
        dsp_write(dsp, 0x6c, 0); /* shared echo writes enabled */
        dsp_write(dsp, 0x6d, 0x80);
        dsp_write(dsp, 0x7d, 1);
        dsp_write(dsp, 0x0f, 0x40);
        configure_voice(dsp, 6, kDspVoiceBus_Music, true);
        configure_voice(dsp, 15, kDspVoiceBus_Sfx, true);
        exercise_slot_writes(dsp);
        for (unsigned i = 0; i < slot; ++i) dsp_clock(dsp);
        saved->offset = 0; saved->loading = false;
        saved->info = (SaveLoadInfo){.func=transfer_state, .portable=true, .saving=true};
        dsp_saveload(dsp, &saved->info);
        memcpy(saved_ram, ram, sizeof(ram));
        exercise_slot_writes(dsp);
        memcpy(expected_ram, ram, sizeof(ram));
        expected->offset = 0; expected->loading = false;
        expected->info = saved->info;
        dsp_saveload(dsp, &expected->info);
        saved->offset = 0; saved->loading = true; saved->info.saving = false;
        dsp_saveload(dsp, &saved->info);
        memcpy(ram, saved_ram, sizeof(ram));
        exercise_slot_writes(dsp);
        actual->offset = 0; actual->loading = false;
        actual->info = expected->info;
        dsp_saveload(dsp, &actual->info);
        check(!saved->info.failed && !actual->info.failed &&
                  expected->offset == actual->offset &&
                  memcmp(expected->bytes, actual->bytes, actual->offset) == 0 &&
                  memcmp(expected_ram, ram, sizeof(ram)) == 0,
              "every slot restores CPU-write races, BRR latches, shared echo and PCM");
        dsp_free(dsp);
    }
    dsp_setExtendedVoicesEnabled(false);
done:
    free(saved); free(expected); free(actual);
}

static void test_effect_bank_slot_and_echo_parity(void) {
    dsp_setExtendedVoicesEnabled(true);
    for (int bank = 1; bank < 5; ++bank) {
        for (int lane = 6; lane <= 7; ++lane) {
            uint8_t native_ram[0x10000], extra_ram[0x10000];
            Dsp *native = new_dsp(native_ram), *extra = new_dsp(extra_ram);
            if (!native || !extra) { dsp_free(native); dsp_free(extra); continue; }
            const int voice = bank * 8 + lane;
            install_looping_brr(native_ram, 2, 0x300);
            install_looping_brr(extra_ram, 2, 0x300);
            for (int i = 0; i < 2; ++i) {
                Dsp *dsp = i ? extra : native;
                dsp_write(dsp, 0x5d, 2); dsp_write(dsp, 0x6c, 0);
                dsp_write(dsp, 0x6d, 0x80); dsp_write(dsp, 0x7d, 1);
                dsp_write(dsp, 0x0f, 0x40); dsp_write(dsp, 0x0d, 0x20);
                dsp_write(dsp, 0x2c, 0x30); dsp_write(dsp, 0x3c, 0x30);
            }
            for (unsigned i = 0; i < 173; ++i) {
                dsp_clock(native); dsp_clock(extra);
            }
            configure_voice(native, lane, kDspVoiceBus_Sfx, true);
            configure_voice(extra, voice, kDspVoiceBus_Sfx, true);
            for (unsigned cycle = 0; cycle < 8192; ++cycle) {
                if (cycle % 197 == 0) {
                    write_voice(native, lane, 2, (uint8_t)cycle);
                    write_voice(extra, voice, 2, (uint8_t)cycle);
                }
                if (cycle == 400 || cycle == 3400) {
                    control_voice(native, lane, 0x4c, false);
                    control_voice(extra, voice, 0x4c, false);
                    control_voice(native, lane, 0x5c, true);
                    control_voice(extra, voice, 0x5c, true);
                }
                if (cycle == 2700 || cycle == 5800) {
                    control_voice(native, lane, 0x5c, false);
                    control_voice(extra, voice, 0x5c, false);
                    control_voice(native, lane, 0x4c, true);
                    control_voice(extra, voice, 0x4c, true);
                }
                dsp_clock(native); dsp_clock(extra);
            }
            check(native->sampleWrite == extra->sampleWrite &&
                      memcmp(native->sampleBuffer, extra->sampleBuffer,
                             native->sampleWrite * 2u * sizeof(int16_t)) == 0 &&
                      memcmp(native_ram, extra_ram, sizeof(native_ram)) == 0,
                  "effect banks retain native slot, dormant re-key and shared echo parity");
            dsp_free(native); dsp_free(extra);
        }
    }
    dsp_setExtendedVoicesEnabled(false);
}

static void test_batched_clock_equivalence(void) {
    static const uint32_t spans[] = {0, 1, 2, 7, 16, 31, 32, 33, 65, 257, 1024};
    uint8_t ram_a[0x10000], ram_b[0x10000];
    MemoryState *a = calloc(1, sizeof(*a)), *b = calloc(1, sizeof(*b));
    check(a && b, "batch test state allocation");
    if (!a || !b) goto done;
    for (unsigned start = 0; start < 32; ++start) {
        Dsp *reference = new_dsp(ram_a), *batched = new_dsp(ram_b);
        if (!reference || !batched) { dsp_free(reference); dsp_free(batched); break; }
        dsp_setExtendedVoicesEnabled(true);
        dsp_setBusGains(100, 100);
        for (unsigned side = 0; side < 2; ++side) {
            Dsp *dsp = side ? batched : reference;
            install_looping_brr(side ? ram_b : ram_a, 2, 0x300);
            dsp_write(dsp, 0x5d, 2); dsp_write(dsp, 0x6c, 0);
            dsp_write(dsp, 0x6d, 0x80); dsp_write(dsp, 0x7d, 1);
            dsp_write(dsp, 0x0f, 0x7f);
            configure_voice(dsp, 6, kDspVoiceBus_Music, true);
            for (int voice = 14; voice < 40; voice += 8) {
                configure_voice(dsp, voice, kDspVoiceBus_Sfx, true);
                configure_voice(dsp, voice + 1, kDspVoiceBus_Sfx, true);
                control_voice(dsp, voice + 1, 0x2d, true);
            }
            for (unsigned cycle = 0; cycle < start; ++cycle) dsp_clock(dsp);
        }
        for (unsigned pass = 0; pass < 4; ++pass) {
            dsp_setExtendedVoicesEnabled(pass != 1);
            dsp_setBusGains(pass & 1 ? 63 : 100, pass & 1 ? 39 : 100);
            for (unsigned i = 0; i < sizeof(spans) / sizeof(spans[0]); ++i) {
                for (unsigned side = 0; side < 2; ++side) {
                    Dsp *dsp = side ? batched : reference;
                    write_voice(dsp, 15, 2, (uint8_t)(i * 31));
                    control_voice(dsp, 15, 0x3d, (i & 1) != 0);
                    control_voice(dsp, 14, 0x5c, i > 5);
                    if (i == 3) control_voice(dsp, 14, 0x4c, true);
                    dsp_write(dsp, 0x7c, 0xff);
                    dsp_write(dsp, 0x0f, (uint8_t)(i * 17));
                    (side ? ram_b : ram_a)[0x304] ^= (uint8_t)i;
                }
                for (uint32_t c = 0; c < spans[i]; ++c) dsp_clock(reference);
                dsp_clockMany(batched, spans[i]);
                a->offset = b->offset = 0;
                a->info = b->info = (SaveLoadInfo){.func = transfer_state, .portable = true, .saving = true};
                dsp_saveload(reference, &a->info); dsp_saveload(batched, &b->info);
                check(!a->info.failed && !b->info.failed && a->offset == b->offset &&
                          memcmp(a->bytes, b->bytes, a->offset) == 0 &&
                          memcmp(ram_a, ram_b, sizeof(ram_a)) == 0 &&
                          memcmp(reference->ram, batched->ram, sizeof(reference->ram)) == 0 &&
                          memcmp(reference->channel, batched->channel, sizeof(reference->channel)) == 0,
                      "batched clocks retain PCM, save state, mirrors and echo RAM at every slot");
            }
        }
        dsp_free(reference); dsp_free(batched);
    }
done:
    free(a); free(b);
    dsp_setExtendedVoicesEnabled(false); dsp_setBusGains(100, 100);
}

int main(void) {
    dsp_setExtendedVoicesEnabled(false);
    dsp_setMusicBusMuted(false);
    dsp_setBusGains(100, 100);
    test_reference_primitives();
    test_registers_keying_and_startup();
    test_buses_and_parallel_virtual_bank();
    test_state_ring_and_resampling();
    test_every_slot_continuation();
    test_effect_bank_slot_and_echo_parity();
    test_batched_clock_equivalence();
    if (failures != 0) {
        fprintf(stderr, "runtime DSP: %d failure(s)\n", failures);
        return 1;
    }
    puts("runtime DSP: PASS");
    return 0;
}
