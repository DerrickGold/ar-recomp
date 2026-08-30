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

int main(void) {
    dsp_setExtendedVoicesEnabled(false);
    dsp_setMusicBusMuted(false);
    dsp_setBusGains(100, 100);
    test_reference_primitives();
    test_registers_keying_and_startup();
    test_buses_and_parallel_virtual_bank();
    test_state_ring_and_resampling();
    if (failures != 0) {
        fprintf(stderr, "runtime DSP: %d failure(s)\n", failures);
        return 1;
    }
    puts("runtime DSP: PASS");
    return 0;
}
