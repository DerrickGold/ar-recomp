#include "dsp.h"

#include "snesrecomp/host/audio_trace.h"
#include "simd.h"
#include "dsp_shadow.h"
#include "saveload.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if SR_SIMD_NEON64
#include <arm_neon.h>
#elif SR_SIMD_SSE2
#include <emmintrin.h>
#endif

/* These optional services are supplied by the complete runner. Focused tests
 * provide inert definitions, keeping the device core independent of a host. */
enum {
    kEnvelopeAttack = 0,
    kEnvelopeDecay = 1,
    kEnvelopeSustain = 2,
    kEnvelopeRelease = 4
};

static const uint16_t kRatePeriods[32] = {
    0, 2048, 1536, 1280, 1024, 768, 640, 512,
    384, 320, 256, 192, 160, 128, 96, 80,
    64, 48, 40, 32, 24, 20, 16, 12,
    10, 8, 6, 5, 4, 3, 2, 1
};

static bool s_extended_voices_enabled;
static int s_music_gain_percent = 100;
static int s_sfx_gain_percent = 100;
static bool s_music_muted;

int g_dsp_voice_mute_srcn_min = -1;
void (*g_dsp_voice_kon_hook)(int, uint8_t, uint16_t, int, int, uint16_t);

static int clamp16(int value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return value;
}

static int clamp_percent(int value) {
    if (value < 0) return 0;
    if (value > 100) return 100;
    return value;
}

static uint16_t read_u16(const uint8_t *ram, uint16_t address) {
    return (uint16_t)(ram[address] |
                      ((uint16_t)ram[(uint16_t)(address + 1u)] << 8));
}

static int16_t read_s16(const uint8_t *ram, uint16_t address) {
    return (int16_t)read_u16(ram, address);
}

static void write_s16(uint8_t *ram, uint16_t address, int value) {
    const uint16_t result = (uint16_t)(int16_t)clamp16(value);
    ram[address] = (uint8_t)result;
    ram[(uint16_t)(address + 1u)] = (uint8_t)(result >> 8);
}

void dsp_setExtendedVoicesEnabled(bool enabled) {
    s_extended_voices_enabled = enabled;
}

bool dsp_extendedVoicesEnabled(void) {
    return s_extended_voices_enabled;
}

int dsp_activeVoiceCount(void) {
    return s_extended_voices_enabled ? kDspMaximumVoiceCount
                                     : kDspHardwareVoiceCount;
}

void dsp_setVoiceBus(Dsp *dsp, int channel, DspVoiceBus bus) {
    if (dsp == NULL || channel < 0 || channel >= kDspMaximumVoiceCount) return;
    if (bus < kDspVoiceBus_Unclassified || bus > kDspVoiceBus_Sfx) {
        bus = kDspVoiceBus_Unclassified;
    }
    dsp->voiceBus[channel] = (uint8_t)bus;
}

DspVoiceBus dsp_getVoiceBus(const Dsp *dsp, int channel) {
    if (dsp == NULL || channel < 0 || channel >= kDspMaximumVoiceCount) {
        return kDspVoiceBus_Unclassified;
    }
    return (DspVoiceBus)dsp->voiceBus[channel];
}

void dsp_setBusGains(int music_percent, int sfx_percent) {
    s_music_gain_percent = clamp_percent(music_percent);
    s_sfx_gain_percent = clamp_percent(sfx_percent);
}

void dsp_getBusGains(int *music_percent, int *sfx_percent) {
    if (music_percent != NULL) *music_percent = s_music_gain_percent;
    if (sfx_percent != NULL) *sfx_percent = s_sfx_gain_percent;
}

void dsp_setMusicBusMuted(bool muted) {
    s_music_muted = muted;
}

static bool voice_is_muted(const Dsp *dsp, int channel) {
    const DspVoiceBus bus = dsp_getVoiceBus(dsp, channel);
    if (s_music_muted && bus == kDspVoiceBus_Music) return true;
    return bus == kDspVoiceBus_Unclassified &&
           g_dsp_voice_mute_srcn_min >= 0 &&
           dsp->channel[channel].srcn >= g_dsp_voice_mute_srcn_min;
}

static int voice_bus_gain(const Dsp *dsp, int channel) {
    switch (dsp_getVoiceBus(dsp, channel)) {
        case kDspVoiceBus_Music: return s_music_gain_percent;
        case kDspVoiceBus_Sfx: return s_sfx_gain_percent;
        default: return 100;
    }
}

static int scale_for_bus(const Dsp *dsp, int channel, int sample) {
    const int gain = voice_bus_gain(dsp, channel);
    return gain == 100 ? sample : sample * gain / 100;
}

static bool virtual_voice_is_sleeping(const Dsp *dsp, int channel) {
    const DspChannel *voice = &dsp->channel[channel];
    return channel >= kDspHardwareVoiceCount && voice->gain == 0u &&
           voice->adsrState == kEnvelopeRelease && !voice->keyOn &&
           !voice->keyOff && !dsp->reset;
}

Dsp *dsp_init(uint8_t *ram) {
    if (ram == NULL) return NULL;
    Dsp *dsp = (Dsp *)calloc(1u, sizeof(*dsp));
    if (dsp == NULL) return NULL;
    dsp->apu_ram = ram;
    dsp->shadow = dsp_shadow_create();
    return dsp;
}

void dsp_free(Dsp *dsp) {
    if (dsp == NULL) return;
    dsp_shadow_free((DspShadow *)dsp->shadow);
    free(dsp);
}

void dsp_reset(Dsp *dsp) {
    if (dsp == NULL) return;
    uint8_t *apu_ram = dsp->apu_ram;
    void *shadow = dsp->shadow;
    memset(dsp, 0, sizeof(*dsp));
    dsp->apu_ram = apu_ram;
    dsp->shadow = shadow;
    dsp->ram[0x7c] = 0xffu;
    dsp->mute = true;
    dsp->reset = true;
    dsp->noiseSample = -0x4000;
    dsp->echoDelay = 1u;
    dsp->echoRemain = 1u;
}

void dsp_saveload(Dsp *dsp, SaveLoadInfo *info) {
    if (dsp == NULL || info == NULL || info->func == NULL) return;
    if (!info->portable) {
        info->func(info, &dsp->ram, sizeof(*dsp) - offsetof(Dsp, ram));
        return;
    }
    saveload_bytes(info, dsp->ram, sizeof(dsp->ram));
    for (unsigned index = 0; index < kDspMaximumVoiceCount; ++index) {
        DspChannel *voice = &dsp->channel[index];
        saveload_u16(info, &voice->pitch);
        saveload_u16(info, &voice->pitchCounter);
        saveload_bool(info, &voice->pitchModulation);
        saveload_i16_array(info, voice->decodeBuffer,
                           sizeof(voice->decodeBuffer) /
                               sizeof(voice->decodeBuffer[0]));
        saveload_u8(info, &voice->srcn);
        saveload_u16(info, &voice->decodeOffset);
        saveload_u8(info, &voice->previousFlags);
        saveload_i16(info, &voice->old);
        saveload_i16(info, &voice->older);
        saveload_bool(info, &voice->useNoise);
        saveload_u16_array(info, voice->adsrRates,
                           sizeof(voice->adsrRates) /
                               sizeof(voice->adsrRates[0]));
        saveload_u16(info, &voice->rateCounter);
        saveload_u8(info, &voice->adsrState);
        saveload_u16(info, &voice->sustainLevel);
        saveload_bool(info, &voice->useGain);
        saveload_u8(info, &voice->gainMode);
        saveload_bool(info, &voice->directGain);
        saveload_u16(info, &voice->gainValue);
        saveload_u16(info, &voice->gain);
        saveload_bool(info, &voice->keyOn);
        saveload_bool(info, &voice->keyOff);
        saveload_i16(info, &voice->sampleOut);
        saveload_i8(info, &voice->volumeL);
        saveload_i8(info, &voice->volumeR);
        saveload_bool(info, &voice->echoEnable);
    }
    saveload_u16(info, &dsp->dirPage);
    saveload_bool(info, &dsp->evenCycle);
    saveload_bool(info, &dsp->mute);
    saveload_bool(info, &dsp->reset);
    saveload_i8(info, &dsp->masterVolumeL);
    saveload_i8(info, &dsp->masterVolumeR);
    saveload_i16(info, &dsp->noiseSample);
    saveload_u16(info, &dsp->noiseRate);
    saveload_u16(info, &dsp->noiseCounter);
    saveload_bool(info, &dsp->echoWrites);
    saveload_i8(info, &dsp->echoVolumeL);
    saveload_i8(info, &dsp->echoVolumeR);
    saveload_i8(info, &dsp->feedbackVolume);
    saveload_u16(info, &dsp->echoBufferAdr);
    saveload_u16(info, &dsp->echoDelay);
    saveload_u16(info, &dsp->echoRemain);
    saveload_u16(info, &dsp->echoBufferIndex);
    saveload_u8(info, &dsp->firBufferIndex);
    saveload_bytes(info, dsp->firValues, sizeof(dsp->firValues));
    saveload_i16_array(info, dsp->firBufferL,
                       sizeof(dsp->firBufferL) / sizeof(dsp->firBufferL[0]));
    saveload_i16_array(info, dsp->firBufferR,
                       sizeof(dsp->firBufferR) / sizeof(dsp->firBufferR[0]));
    saveload_i16_array(info, dsp->sampleBuffer,
                       sizeof(dsp->sampleBuffer) /
                           sizeof(dsp->sampleBuffer[0]));
    saveload_u32(info, &dsp->sampleWrite);
    saveload_u32(info, &dsp->sampleRead);
}

static void apply_voice_register(Dsp *dsp, int channel, uint8_t reg,
                                 uint8_t value) {
    if (dsp == NULL || channel < 0 || channel >= kDspMaximumVoiceCount ||
        reg > 7u) {
        return;
    }
    DspChannel *voice = &dsp->channel[channel];
    switch (reg) {
        case 0: voice->volumeL = (int8_t)value; break;
        case 1: voice->volumeR = (int8_t)value; break;
        case 2:
            voice->pitch = (uint16_t)((voice->pitch & 0x3f00u) | value);
            break;
        case 3:
            voice->pitch = (uint16_t)(((uint16_t)value << 8 |
                                       (voice->pitch & 0xffu)) & 0x3fffu);
            break;
        case 4: voice->srcn = value; break;
        case 5:
            voice->adsrRates[0] = kRatePeriods[(value & 0x0fu) * 2u + 1u];
            voice->adsrRates[1] = kRatePeriods[((value >> 4) & 7u) * 2u + 16u];
            voice->useGain = (value & 0x80u) == 0u;
            break;
        case 6:
            voice->adsrRates[2] = kRatePeriods[value & 0x1fu];
            voice->sustainLevel = (uint16_t)(((value >> 5) + 1u) * 0x100u);
            break;
        case 7:
            voice->directGain = (value & 0x80u) == 0u;
            if (voice->directGain) {
                voice->gainValue = (uint16_t)((value & 0x7fu) << 4);
            } else {
                voice->gainMode = (uint8_t)((value >> 5) & 3u);
                voice->adsrRates[3] = kRatePeriods[value & 0x1fu];
            }
            break;
        default: break;
    }
}

static void apply_hardware_mask(Dsp *dsp, uint8_t address, uint8_t value,
                                uint8_t update_mask) {
    if (dsp == NULL) return;
    for (int channel = 0; channel < kDspHardwareVoiceCount; ++channel) {
        const uint8_t bit = (uint8_t)(1u << channel);
        if ((update_mask & bit) == 0u) continue;
        const bool enabled = (value & bit) != 0u;
        DspChannel *voice = &dsp->channel[channel];
        switch (address) {
            case 0x2d: voice->pitchModulation = enabled; break;
            case 0x3d: voice->useNoise = enabled; break;
            case 0x4c: voice->keyOn = enabled; break;
            case 0x5c: voice->keyOff = enabled; break;
            case 0x4d: voice->echoEnable = enabled; break;
            default: return;
        }
    }
    dsp->ram[address] = (uint8_t)((dsp->ram[address] & ~update_mask) |
                                  (value & update_mask));
}

void dsp_writeVirtualVoiceRegister(Dsp *dsp, int channel,
                                   uint8_t source_address, uint8_t value) {
    if (channel < kDspHardwareVoiceCount || channel >= kDspMaximumVoiceCount) {
        return;
    }
    audio_trace_on_reg_write(source_address, value);
    apply_voice_register(dsp, channel, (uint8_t)(source_address & 0x0fu), value);
}

void dsp_writeVirtualVoiceControl(Dsp *dsp, int channel,
                                  uint8_t global_address, bool enabled) {
    if (dsp == NULL || channel < kDspHardwareVoiceCount ||
        channel >= kDspMaximumVoiceCount) {
        return;
    }
    DspChannel *voice = &dsp->channel[channel];
    switch (global_address) {
        case 0x2d: voice->pitchModulation = enabled; break;
        case 0x3d: voice->useNoise = enabled; break;
        case 0x4c: voice->keyOn = enabled; break;
        case 0x5c: voice->keyOff = enabled; break;
        case 0x4d: voice->echoEnable = enabled; break;
        default: break;
    }
}

void dsp_writeHardwareVoiceMask(Dsp *dsp, uint8_t address, uint8_t value,
                                uint8_t update_mask) {
    if (address != 0x2du && address != 0x3du && address != 0x4cu &&
        address != 0x5cu && address != 0x4du) {
        return;
    }
    audio_trace_on_reg_write(address, value);
    apply_hardware_mask(dsp, address, value, update_mask);
}

uint8_t dsp_read(Dsp *dsp, uint8_t address) {
    return dsp == NULL ? 0u : dsp->ram[address & 0x7fu];
}

void dsp_write(Dsp *dsp, uint8_t address, uint8_t value) {
    if (dsp == NULL) return;
    address &= 0x7fu;
    audio_trace_on_reg_write(address, value);
    const int channel = address >> 4;
    if (channel < kDspHardwareVoiceCount && (address & 0x0fu) <= 7u) {
        apply_voice_register(dsp, channel, (uint8_t)(address & 0x0fu), value);
        dsp->ram[address] = value;
        return;
    }
    switch (address) {
        case 0x0c: dsp->masterVolumeL = (int8_t)value; break;
        case 0x1c: dsp->masterVolumeR = (int8_t)value; break;
        case 0x2c: dsp->echoVolumeL = (int8_t)value; break;
        case 0x3c: dsp->echoVolumeR = (int8_t)value; break;
        case 0x0d: dsp->feedbackVolume = (int8_t)value; break;
        case 0x2d:
        case 0x3d:
        case 0x4c:
        case 0x4d:
        case 0x5c:
            apply_hardware_mask(dsp, address, value, 0xffu);
            return;
        case 0x5d: dsp->dirPage = (uint16_t)value << 8; break;
        case 0x6c:
            dsp->reset = (value & 0x80u) != 0u;
            dsp->mute = (value & 0x40u) != 0u;
            dsp->echoWrites = (value & 0x20u) == 0u;
            dsp->noiseRate = kRatePeriods[value & 0x1fu];
            break;
        case 0x6d: dsp->echoBufferAdr = (uint16_t)value << 8; break;
        case 0x7c: value = 0u; break;
        case 0x7d:
            dsp->echoDelay = (uint16_t)((value & 0x0fu) * 512u);
            if (dsp->echoDelay == 0u) dsp->echoDelay = 1u;
            break;
        case 0x0f: case 0x1f: case 0x2f: case 0x3f:
        case 0x4f: case 0x5f: case 0x6f: case 0x7f:
            dsp->firValues[channel] = (int8_t)value;
            break;
        default: break;
    }
    dsp->ram[address] = value;
}

static void decode_brr(Dsp *dsp, int channel) {
    DspChannel *voice = &dsp->channel[channel];
    voice->decodeBuffer[0] = voice->decodeBuffer[16];
    voice->decodeBuffer[1] = voice->decodeBuffer[17];
    voice->decodeBuffer[2] = voice->decodeBuffer[18];

    if ((voice->previousFlags & 1u) != 0u) {
        const uint16_t pointer = (uint16_t)(dsp->dirPage + 4u * voice->srcn);
        dsp->ram[0x7c] |= channel < kDspHardwareVoiceCount
                              ? (uint8_t)(1u << channel) : 0u;
        if ((voice->previousFlags & 2u) != 0u) {
            voice->decodeOffset = read_u16(dsp->apu_ram,
                                           (uint16_t)(pointer + 2u));
        } else {
            voice->adsrState = kEnvelopeRelease;
            voice->gain = 0u;
        }
    }

    const uint8_t header = dsp->apu_ram[voice->decodeOffset++];
    const unsigned shift = header >> 4;
    const unsigned filter = (header >> 2) & 3u;
    voice->previousFlags = header & 3u;
    int older = voice->older;
    int old = voice->old;
    for (unsigned index = 0; index < 16u; ++index) {
        const uint8_t packed = dsp->apu_ram[
            (uint16_t)(voice->decodeOffset + index / 2u)];
        int nibble = (index & 1u) != 0u ? packed & 0x0f : packed >> 4;
        if (nibble >= 8) nibble -= 16;
        int sample;
        if (shift <= 12u) {
            sample = nibble * (1 << shift);
        } else {
            sample = nibble < 0 ? -2048 : 0;
        }
        switch (filter) {
            case 1: sample += old * 15 / 16; break;
            case 2: sample += old * 61 / 32 - older * 15 / 16; break;
            case 3: sample += old * 115 / 64 - older * 13 / 16; break;
            default: break;
        }
        sample = clamp16(sample) & ~1;
        voice->decodeBuffer[index + 3u] = (int16_t)sample;
        older = old;
        old = sample;
    }
    voice->decodeOffset = (uint16_t)(voice->decodeOffset + 8u);
    voice->older = (int16_t)older;
    voice->old = (int16_t)old;
}

static void key_on(Dsp *dsp, int channel) {
    DspChannel *voice = &dsp->channel[channel];
    const uint16_t pointer = (uint16_t)(dsp->dirPage + 4u * voice->srcn);
    voice->decodeOffset = read_u16(dsp->apu_ram, pointer);
    const uint16_t brr_address = voice->decodeOffset;
    voice->previousFlags = 0u;
    voice->pitchCounter = 0u;
    voice->old = 0;
    voice->older = 0;
    memset(voice->decodeBuffer, 0, sizeof(voice->decodeBuffer));
    voice->gain = 0u;
    voice->rateCounter = 0u;
    voice->adsrState = kEnvelopeAttack;
    voice->keyOn = false;
    if (channel < kDspHardwareVoiceCount) {
        dsp->ram[0x7c] &= (uint8_t)~(1u << channel);
    }
    decode_brr(dsp, channel);
    if (g_dsp_voice_kon_hook != NULL) {
        g_dsp_voice_kon_hook(channel, voice->srcn, brr_address,
                             voice->volumeL, voice->volumeR, voice->pitch);
    }
}

static void update_envelope(DspChannel *voice) {
    if (voice->adsrState == kEnvelopeRelease) {
        voice->gain = voice->gain > 8u ? (uint16_t)(voice->gain - 8u) : 0u;
        return;
    }

    const bool live_gain = voice->useGain;
    const bool direct = live_gain && voice->directGain;
    if (direct) {
        voice->gain = voice->gainValue;
        return;
    }

    unsigned stage = voice->adsrState;
    if (stage > kEnvelopeSustain) stage = kEnvelopeSustain;
    const uint16_t period = live_gain ? voice->adsrRates[3]
                                      : voice->adsrRates[stage];
    if (period == 0u || ++voice->rateCounter < period) return;
    voice->rateCounter = 0u;

    int gain = voice->gain;
    if (live_gain) {
        switch (voice->gainMode) {
            case 0: gain -= 32; break;
            case 1: gain -= ((gain - 1) >> 8) + 1; break;
            case 2: gain += 32; break;
            case 3: gain += gain < 0x600 ? 32 : 8; break;
            default: break;
        }
    } else if (stage == kEnvelopeAttack) {
        gain += period == 1u ? 1024 : 32;
    } else {
        gain -= ((gain - 1) >> 8) + 1;
    }
    if (gain < 0) gain = 0;
    if (gain > 0x7ff) gain = 0x7ff;
    voice->gain = (uint16_t)gain;

    if (voice->adsrState == kEnvelopeAttack && gain >= 0x7e0) {
        voice->adsrState = kEnvelopeDecay;
    } else if (voice->adsrState == kEnvelopeDecay &&
               gain <= voice->sustainLevel) {
        voice->adsrState = kEnvelopeSustain;
    }
}

static int interpolate_voice(const DspChannel *voice) {
    const unsigned index = voice->pitchCounter >> 12;
    const unsigned next_index = index < 15u ? index + 1u : index;
    const int current = voice->decodeBuffer[index + 3u];
    const int next = voice->decodeBuffer[next_index + 3u];
    const int fraction = voice->pitchCounter & 0x0fffu;
    return current + (next - current) * fraction / 0x1000;
}

static void cycle_voice(Dsp *dsp, int channel) {
    DspChannel *voice = &dsp->channel[channel];
    if (dsp->evenCycle) {
        if (voice->keyOff) {
            voice->adsrState = kEnvelopeRelease;
        } else if (voice->keyOn) {
            key_on(dsp, channel);
        }
    }
    if (dsp->reset) {
        voice->adsrState = kEnvelopeRelease;
        voice->gain = 0u;
    }

    uint32_t pitch = voice->pitch;
    if (channel > 0 && voice->pitchModulation) {
        int predecessor = channel - 1;
        if (channel == kDspHardwareVoiceCount) predecessor = 5;
        int adjusted = (int)pitch +
                       (dsp->channel[predecessor].sampleOut * (int)pitch >> 15);
        if (adjusted < 0) adjusted = 0;
        if (adjusted > 0x3fff) adjusted = 0x3fff;
        pitch = (uint32_t)adjusted;
    }

    int sample = voice->useNoise ? dsp->noiseSample : interpolate_voice(voice);
    update_envelope(voice);
    sample = sample * voice->gain / 0x800;
    voice->sampleOut = (int16_t)clamp16(sample);
    if (channel < kDspHardwareVoiceCount) {
        dsp->ram[(channel << 4) | 8] = (uint8_t)(voice->gain >> 4);
        dsp->ram[(channel << 4) | 9] =
            (uint8_t)(int8_t)(voice->sampleOut >> 8);
    }

    const uint32_t position = voice->pitchCounter + pitch;
    if (position >= 0x10000u) decode_brr(dsp, channel);
    voice->pitchCounter = (uint16_t)position;
}

static void update_noise(Dsp *dsp) {
    if (dsp->noiseRate == 0u || ++dsp->noiseCounter < dsp->noiseRate) return;
    dsp->noiseCounter = 0u;
    const unsigned feedback = ((uint16_t)dsp->noiseSample ^
                               ((uint16_t)dsp->noiseSample >> 1)) & 1u;
    uint16_t noise = (uint16_t)(((uint16_t)dsp->noiseSample >> 1) |
                                (feedback << 14));
    if ((noise & 0x4000u) != 0u) noise |= 0x8000u;
    dsp->noiseSample = (int16_t)noise;
}

static void handle_echo(Dsp *dsp, int echo_input_left, int echo_input_right,
                        int *output_left, int *output_right) {
    const uint16_t address = (uint16_t)(dsp->echoBufferAdr +
                                        dsp->echoBufferIndex * 4u);
    dsp->firBufferL[dsp->firBufferIndex] = read_s16(dsp->apu_ram, address);
    dsp->firBufferR[dsp->firBufferIndex] =
        read_s16(dsp->apu_ram, (uint16_t)(address + 2u));

    int filtered_left = 0;
    int filtered_right = 0;
    for (unsigned tap = 0; tap < 8u; ++tap) {
        const unsigned position = (dsp->firBufferIndex - tap) & 7u;
        filtered_left += dsp->firBufferL[position] * dsp->firValues[tap] / 128;
        filtered_right += dsp->firBufferR[position] * dsp->firValues[tap] / 128;
    }
    filtered_left = clamp16(filtered_left);
    filtered_right = clamp16(filtered_right);
    *output_left = clamp16(*output_left +
                           filtered_left * dsp->echoVolumeL / 128);
    *output_right = clamp16(*output_right +
                            filtered_right * dsp->echoVolumeR / 128);

    if (dsp->echoWrites) {
        write_s16(dsp->apu_ram, address,
                  echo_input_left + filtered_left * dsp->feedbackVolume / 128);
        write_s16(dsp->apu_ram, (uint16_t)(address + 2u),
                  echo_input_right + filtered_right * dsp->feedbackVolume / 128);
    }
    dsp->firBufferIndex = (uint8_t)((dsp->firBufferIndex + 1u) & 7u);
    if (++dsp->echoBufferIndex >= dsp->echoDelay) dsp->echoBufferIndex = 0u;
    dsp->echoRemain = (uint16_t)(dsp->echoDelay - dsp->echoBufferIndex);
}

void dsp_cycle(Dsp *dsp) {
    if (dsp == NULL) return;
    int dry_left = 0;
    int dry_right = 0;
    int echo_left = 0;
    int echo_right = 0;
    const int voices = dsp_activeVoiceCount();
    for (int channel = 0; channel < voices; ++channel) {
        DspChannel *voice = &dsp->channel[channel];
        if (virtual_voice_is_sleeping(dsp, channel)) {
            voice->sampleOut = 0;
            continue;
        }
        cycle_voice(dsp, channel);
        if (voice_is_muted(dsp, channel)) continue;
        const int left = scale_for_bus(
            dsp, channel, voice->sampleOut * voice->volumeL / 64);
        const int right = scale_for_bus(
            dsp, channel, voice->sampleOut * voice->volumeR / 64);
        dry_left = clamp16(dry_left + left);
        dry_right = clamp16(dry_right + right);
        if (voice->echoEnable) {
            echo_left = clamp16(echo_left + left);
            echo_right = clamp16(echo_right + right);
        }
    }

    int output_left = clamp16(dry_left * dsp->masterVolumeL / 128);
    int output_right = clamp16(dry_right * dsp->masterVolumeR / 128);
    if (dsp->shadow != NULL && !s_extended_voices_enabled &&
        s_music_gain_percent == 100 && s_sfx_gain_percent == 100 &&
        !s_music_muted && g_dsp_voice_mute_srcn_min < 0) {
        dsp_shadow_process((DspShadow *)dsp->shadow, dsp, output_left,
                           output_right, &output_left, &output_right);
    }
    handle_echo(dsp, echo_left, echo_right, &output_left, &output_right);
    if (dsp->mute) output_left = output_right = 0;
    update_noise(dsp);

    const uint32_t fill = dsp->sampleWrite - dsp->sampleRead;
    const int dropped = fill >= DSP_SAMPLE_RING;
    if (!dropped) {
        const uint32_t index = dsp->sampleWrite & (DSP_SAMPLE_RING - 1u);
        dsp->sampleBuffer[index * 2u] = (int16_t)output_left;
        dsp->sampleBuffer[index * 2u + 1u] = (int16_t)output_right;
        ++dsp->sampleWrite;
    }
    audio_trace_on_sample((int16_t)output_left, (int16_t)output_right, dropped,
                          dropped ? fill : fill + 1u);
    dsp->evenCycle = !dsp->evenCycle;
}

void dsp_getSamples(Dsp *dsp, int16_t *samples, int sample_count) {
    if (dsp == NULL || samples == NULL || sample_count <= 0) return;
    const double step = 534.0 / (double)sample_count;
    double position = 0.0;
    const uint32_t base = dsp->sampleRead;
    for (int index = 0; index < sample_count; ++index) {
        const uint32_t source =
            (base + (uint32_t)position) & (DSP_SAMPLE_RING - 1u);
        samples[index * 2] = dsp->sampleBuffer[source * 2u];
        samples[index * 2 + 1] = dsp->sampleBuffer[source * 2u + 1u];
        position += step;
    }
    dsp->sampleRead += 534u;
    audio_trace_on_consume(base, 534u, dsp->sampleWrite - dsp->sampleRead);
}

static void interpolate_stereo_scalar(const int16_t *first,
                                      const int16_t *second,
                                      double fraction, int16_t *output) {
    for (int side = 0; side < 2; ++side) {
        const int a = first[side];
        const int b = second[side];
        output[side] = (int16_t)(a + (int)((b - a) * fraction));
    }
}

#if SR_SIMD_NEON64
static void interpolate_stereo_simd(const int16_t *first,
                                    const int16_t *second,
                                    double fraction, int16_t *output) {
    /* Each load includes the following stereo frame. The caller keeps the
     * final ring entry on the scalar boundary path, so both loads are within
     * the sample buffer. Only the low two lanes participate. */
    const int32x2_t first32 = vget_low_s32(vmovl_s16(vld1_s16(first)));
    const int32x2_t second32 = vget_low_s32(vmovl_s16(vld1_s16(second)));
    const int64x2_t delta64 = vmovl_s32(vsub_s32(second32, first32));
    const float64x2_t scaled = vmulq_n_f64(vcvtq_f64_s64(delta64), fraction);
    const int32x2_t delta32 = vmovn_s64(vcvtq_s64_f64(scaled));
    const int16x4_t mixed = vmovn_s32(vcombine_s32(
        vadd_s32(first32, delta32), vdup_n_s32(0)));
    output[0] = vget_lane_s16(mixed, 0);
    output[1] = vget_lane_s16(mixed, 1);
}
#elif SR_SIMD_SSE2
static void interpolate_stereo_simd(const int16_t *first,
                                    const int16_t *second,
                                    double fraction, int16_t *output) {
    int32_t first_packed, second_packed, output_packed;
    __m128i first16, second16, first32, second32, delta32, scaled32;
    __m128i zero = _mm_setzero_si128();
    memcpy(&first_packed, first, sizeof(first_packed));
    memcpy(&second_packed, second, sizeof(second_packed));
    first16 = _mm_cvtsi32_si128(first_packed);
    second16 = _mm_cvtsi32_si128(second_packed);
    first32 = _mm_unpacklo_epi16(first16,
        _mm_cmpgt_epi16(zero, first16));
    second32 = _mm_unpacklo_epi16(second16,
        _mm_cmpgt_epi16(zero, second16));
    delta32 = _mm_sub_epi32(second32, first32);
    scaled32 = _mm_cvttpd_epi32(_mm_mul_pd(
        _mm_cvtepi32_pd(delta32), _mm_set1_pd(fraction)));
    output_packed = _mm_cvtsi128_si32(_mm_packs_epi32(
        _mm_add_epi32(first32, scaled32), zero));
    memcpy(output, &output_packed, sizeof(output_packed));
}
#endif

void dsp_getSamplesResampled(Dsp *dsp, int16_t *samples, int sample_count,
                             double native_step, double *phase) {
    if (dsp == NULL || samples == NULL || sample_count <= 0 ||
        native_step <= 0.0 || phase == NULL) {
        return;
    }
    double position = *phase;
    const uint32_t base = dsp->sampleRead;
    for (int index = 0; index < sample_count; ++index) {
        const uint32_t whole = (uint32_t)position;
        const double fraction = position - (double)whole;
        const uint32_t first = (base + whole) & (DSP_SAMPLE_RING - 1u);
        const uint32_t second = (first + 1u) & (DSP_SAMPLE_RING - 1u);
#if SR_SIMD_NEON64 || SR_SIMD_SSE2
        if (first + 1u < DSP_SAMPLE_RING &&
            second + 1u < DSP_SAMPLE_RING) {
            interpolate_stereo_simd(dsp->sampleBuffer + first * 2u,
                                    dsp->sampleBuffer + second * 2u,
                                    fraction, samples + index * 2);
        } else
#endif
        {
            interpolate_stereo_scalar(dsp->sampleBuffer + first * 2u,
                                      dsp->sampleBuffer + second * 2u,
                                      fraction, samples + index * 2);
        }
        position += native_step;
    }
    const uint32_t consumed = (uint32_t)position;
    dsp->sampleRead += consumed;
    *phase = position - (double)consumed;
    audio_trace_on_consume(base, consumed, dsp->sampleWrite - dsp->sampleRead);
}
