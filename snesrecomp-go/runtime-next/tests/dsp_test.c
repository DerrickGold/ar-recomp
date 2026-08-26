#include "snes/dsp.h"
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
static unsigned key_on_calls;
static uint16_t keyed_brr;

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
    fprintf(stderr, "runtime-next DSP failed: %s\n", message);
    ++failures;
}

static Dsp *new_dsp(uint8_t *ram) {
    memset(ram, 0, 0x10000u);
    Dsp *dsp = dsp_init(ram);
    check(dsp != NULL, "allocation");
    if (dsp == NULL) return NULL;
    dsp_reset(dsp);
    dsp->reset = false;
    dsp->mute = false;
    dsp->masterVolumeL = 127;
    dsp->masterVolumeR = 127;
    return dsp;
}

static void install_looping_brr(uint8_t *ram, uint8_t source, uint16_t start) {
    const uint16_t entry = (uint16_t)(0x0200u + source * 4u);
    ram[entry] = (uint8_t)start;
    ram[(uint16_t)(entry + 1u)] = (uint8_t)(start >> 8);
    ram[(uint16_t)(entry + 2u)] = (uint8_t)start;
    ram[(uint16_t)(entry + 3u)] = (uint8_t)(start >> 8);
    ram[start] = 0x03u;
    static const uint8_t payload[8] = {
        0x71, 0xe2, 0x63, 0xd4, 0x55, 0xc6, 0x37, 0xa8
    };
    memcpy(ram + start + 1u, payload, sizeof(payload));
}

static void on_key_on(int channel, uint8_t source, uint16_t brr,
                      int left, int right, uint16_t pitch) {
    (void)channel;
    (void)source;
    (void)left;
    (void)right;
    (void)pitch;
    ++key_on_calls;
    keyed_brr = brr;
}

static void configure_voice(Dsp *dsp, int channel, DspVoiceBus bus) {
    DspChannel *voice = &dsp->channel[channel];
    voice->pitch = 0x1000u;
    voice->srcn = 2u;
    voice->volumeL = 96;
    voice->volumeR = -48;
    voice->useGain = true;
    voice->directGain = true;
    voice->gainValue = 0x7f0u;
    voice->keyOn = true;
    dsp_setVoiceBus(dsp, channel, bus);
}

static void test_registers_keying_and_brr(void) {
    uint8_t ram[0x10000];
    Dsp *dsp = new_dsp(ram);
    if (dsp == NULL) return;
    install_looping_brr(ram, 2u, 0x0300u);
    dsp_write(dsp, 0x5du, 0x02u);
    dsp_write(dsp, 0x00u, 96u);
    dsp_write(dsp, 0x01u, (uint8_t)-48);
    dsp_write(dsp, 0x02u, 0x00u);
    dsp_write(dsp, 0x03u, 0x10u);
    dsp_write(dsp, 0x04u, 2u);
    dsp_write(dsp, 0x05u, 0u);
    dsp_write(dsp, 0x07u, 0x7fu);
    g_dsp_voice_kon_hook = on_key_on;
    dsp_write(dsp, 0x4cu, 1u);
    dsp->evenCycle = true;
    dsp_cycle(dsp);
    g_dsp_voice_kon_hook = NULL;

    check(key_on_calls == 1u && keyed_brr == 0x0300u,
          "KON callback reports resolved BRR start");
    check(!dsp->channel[0].keyOn && dsp->channel[0].gain == 0x7f0u,
          "latched KON starts direct-gain envelope");
    check(dsp->channel[0].sampleOut != 0,
          "BRR decoder produces a voice sample");
    check(dsp->sampleWrite == 1u && dsp->sampleBuffer[0] != 0 &&
          dsp->sampleBuffer[0] != dsp->sampleBuffer[1],
          "voice is mixed into asymmetric stereo output");
    check(dsp_read(dsp, 0x08u) == 0x7fu,
          "ENVX reflects current envelope");
    check(trace_writes >= 8u && trace_samples >= 1u,
          "register and sample trace seams fire");
    dsp_free(dsp);
}

static void test_kof_priority_and_live_gain_switch(void) {
    uint8_t ram[0x10000];
    Dsp *dsp = new_dsp(ram);
    if (dsp == NULL) return;
    install_looping_brr(ram, 2u, 0x0300u);
    dsp->dirPage = 0x0200u;
    configure_voice(dsp, 0, kDspVoiceBus_Unclassified);
    dsp->channel[0].keyOff = true;
    dsp->evenCycle = true;
    dsp_cycle(dsp);
    check(dsp->channel[0].keyOn && dsp->channel[0].adsrState == 4u,
          "KOF takes priority without discarding pending KON");
    dsp->channel[0].keyOff = false;
    dsp->evenCycle = true;
    dsp_cycle(dsp);
    check(!dsp->channel[0].keyOn && dsp->channel[0].gain == 0x7f0u,
          "pending KON starts after KOF clears");

    dsp->channel[0].useGain = false;
    dsp->channel[0].adsrState = 2u;
    dsp->channel[0].gain = 0x700u;
    dsp->channel[0].adsrRates[2] = 0u;
    dsp_cycle(dsp);
    check(dsp->channel[0].gain == 0x700u, "zero-rate sustain holds");
    dsp->channel[0].useGain = true;
    dsp->channel[0].directGain = false;
    dsp->channel[0].gainMode = 1u;
    dsp->channel[0].adsrRates[3] = 1u;
    dsp_cycle(dsp);
    check(dsp->channel[0].gain < 0x700u,
          "live ADSR-to-GAIN switch changes an active envelope");
    dsp_free(dsp);
}

static void test_buses_echo_and_extended_voices(void) {
    uint8_t music_ram[0x10000], sfx_ram[0x10000], extended_ram[0x10000];
    Dsp *music = new_dsp(music_ram);
    Dsp *sfx = new_dsp(sfx_ram);
    Dsp *extended = new_dsp(extended_ram);
    if (music == NULL || sfx == NULL || extended == NULL) goto done;
    Dsp *all[3] = {music, sfx, extended};
    for (unsigned index = 0; index < 3u; ++index) {
        all[index]->noiseSample = 10000;
        all[index]->echoWrites = true;
    }
    configure_voice(music, 0, kDspVoiceBus_Music);
    configure_voice(sfx, 0, kDspVoiceBus_Sfx);
    music->channel[0].useNoise = true;
    sfx->channel[0].useNoise = true;
    music->channel[0].echoEnable = true;
    sfx->channel[0].echoEnable = true;

    dsp_setBusGains(0, 100);
    dsp_cycle(music);
    dsp_cycle(sfx);
    check(music->sampleBuffer[0] == 0 && sfx->sampleBuffer[0] != 0,
          "music and SFX buses have independent gains");
    check(music_ram[0] == 0 && music_ram[1] == 0 &&
          (sfx_ram[0] != 0 || sfx_ram[1] != 0),
          "bus gain also gates echo sends");

    dsp_setExtendedVoicesEnabled(true);
    configure_voice(extended, 8, kDspVoiceBus_Sfx);
    extended->channel[8].useNoise = true;
    dsp_cycle(extended);
    check(dsp_activeVoiceCount() == 40 && extended->sampleBuffer[0] != 0,
          "enabled virtual voice participates in the mix");
    extended->channel[8].adsrState = 4u;
    extended->channel[8].gain = 0u;
    extended->channel[8].keyOn = false;
    extended->channel[8].pitchCounter = 0x1234u;
    dsp_cycle(extended);
    check(extended->channel[8].pitchCounter == 0x1234u,
          "released virtual voice sleeps until the next KON");
done:
    dsp_free(music);
    dsp_free(sfx);
    dsp_free(extended);
    dsp_setBusGains(100, 100);
    dsp_setExtendedVoicesEnabled(false);
}

typedef struct MemoryState {
    SaveLoadInfo info;
    uint8_t bytes[sizeof(Dsp)];
    size_t offset;
    bool loading;
} MemoryState;

static void transfer_state(SaveLoadInfo *info, void *data, size_t size) {
    MemoryState *state = (MemoryState *)info;
    check(state->offset + size <= sizeof(state->bytes), "save span fits");
    if (state->offset + size > sizeof(state->bytes)) return;
    if (state->loading) memcpy(data, state->bytes + state->offset, size);
    else memcpy(state->bytes + state->offset, data, size);
    state->offset += size;
}

static void test_state_ring_and_resampling(void) {
    uint8_t ram[0x10000];
    Dsp *dsp = new_dsp(ram);
    if (dsp == NULL) return;
    dsp->channel[8].srcn = 0x55u;
    dsp->channel[8].pitch = 0x2345u;
    MemoryState state;
    memset(&state, 0, sizeof(state));
    state.info.func = transfer_state;
    dsp_saveload(dsp, &state.info);
    check(state.offset > sizeof(dsp->ram),
          "save span includes channel and sequencer state");
    memset(&dsp->channel[8], 0, sizeof(dsp->channel[8]));
    state.offset = 0u;
    state.loading = true;
    dsp_saveload(dsp, &state.info);
    check(dsp->channel[8].srcn == 0x55u &&
          dsp->channel[8].pitch == 0x2345u,
          "extended voice survives save/load");

    dsp->sampleRead = 0u;
    dsp->sampleWrite = DSP_SAMPLE_RING;
    const unsigned drops_before = trace_drops;
    dsp_cycle(dsp);
    check(dsp->sampleWrite == DSP_SAMPLE_RING && trace_drops == drops_before + 1u,
          "full ring drops without overwriting unread samples");
    dsp->sampleRead = dsp->sampleWrite = 0u;
    dsp->sampleBuffer[0] = 0;
    dsp->sampleBuffer[1] = 100;
    dsp->sampleBuffer[2] = 1000;
    dsp->sampleBuffer[3] = 1100;
    dsp->sampleWrite = 2u;
    double phase = 0.5;
    int16_t output[2] = {0, 0};
    dsp_getSamplesResampled(dsp, output, 1, 1.0, &phase);
    check(output[0] == 500 && output[1] == 600 && dsp->sampleRead == 1u &&
          phase == 0.5 && trace_consumes > 0u,
          "continuous resampler interpolates and preserves fractional phase");
    dsp_free(dsp);
}

int main(void) {
    dsp_setExtendedVoicesEnabled(false);
    dsp_setMusicBusMuted(false);
    dsp_setBusGains(100, 100);
    test_registers_keying_and_brr();
    test_kof_priority_and_live_gain_switch();
    test_buses_echo_and_extended_voices();
    test_state_ring_and_resampling();
    if (failures != 0) {
        fprintf(stderr, "runtime-next DSP: %d failure(s)\n", failures);
        return 1;
    }
    puts("runtime-next DSP: PASS");
    return 0;
}
