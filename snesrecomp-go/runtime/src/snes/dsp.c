#include "dsp.h"

#include "apu.h"
#include "dsp_accuracy_bridge.h"
#include "runner_internal.h"
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

bool g_dsp_extended_voices_enabled;
static int s_music_gain_percent = 100;
static int s_sfx_gain_percent = 100;
static bool s_music_muted;
static int s_unclassified_music_source_min = -1;

static int clamp_percent(int value) {
    if (value < 0) return 0;
    if (value > 100) return 100;
    return value;
}

static uint16_t read_u16(const uint8_t *ram, uint16_t address) {
    return (uint16_t)(ram[address] |
                      ((uint16_t)ram[(uint16_t)(address + 1u)] << 8));
}

void dsp_setExtendedVoicesEnabled(bool enabled) {
    g_dsp_extended_voices_enabled = enabled;
}

bool dsp_extendedVoicesEnabled(void) {
    return g_dsp_extended_voices_enabled;
}

int dsp_activeVoiceCount(void) {
    return g_dsp_extended_voices_enabled ? kDspMaximumVoiceCount
                                         : kDspHardwareVoiceCount;
}

void dsp_setVoiceBus(Dsp *dsp, int channel, DspVoiceBus bus) {
    if (dsp == NULL || channel < 0 || channel >= kDspMaximumVoiceCount) return;
    if (bus < kDspVoiceBus_Unclassified || bus > kDspVoiceBus_Sfx)
        bus = kDspVoiceBus_Unclassified;
    dsp->voiceBus[channel] = (uint8_t)bus;
}

DspVoiceBus dsp_getVoiceBus(const Dsp *dsp, int channel) {
    if (dsp == NULL || channel < 0 || channel >= kDspMaximumVoiceCount)
        return kDspVoiceBus_Unclassified;
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

void dsp_setUnclassifiedMusicSourceMinimum(int source_number) {
    s_unclassified_music_source_min =
        source_number >= 0 && source_number <= UINT8_MAX
            ? source_number : -1;
}

static int voice_bus_gain(const Dsp *dsp, int channel) {
    switch (dsp_getVoiceBus(dsp, channel)) {
        case kDspVoiceBus_Music: return s_music_gain_percent;
        case kDspVoiceBus_Sfx: return s_sfx_gain_percent;
        default: return 100;
    }
}

static bool voice_is_muted(const Dsp *dsp, int channel) {
    const DspVoiceBus bus = dsp_getVoiceBus(dsp, channel);
    if (s_music_muted && bus == kDspVoiceBus_Music) return true;
    return bus == kDspVoiceBus_Unclassified &&
           s_unclassified_music_source_min >= 0 &&
           dsp->channel[channel].srcn >=
               s_unclassified_music_source_min;
}

void dsp_refreshMixControls(Dsp *dsp) {
    int channel;
    dsp->mixControlsUnity = s_music_gain_percent == 100 &&
        s_sfx_gain_percent == 100 && !s_music_muted &&
        s_unclassified_music_source_min < 0;
    if (dsp->mixControlsUnity) return;
    for (channel = 0; channel < kDspMaximumVoiceCount; ++channel) {
        dsp->voiceGainPercent[channel] =
            (uint8_t)voice_bus_gain(dsp, channel);
        dsp->voiceMuted[channel] =
            voice_is_muted(dsp, channel) ? 1u : 0u;
    }
}

static void mirror_voice_register(Dsp *dsp, int channel, uint8_t reg,
                                  uint8_t value) {
    DspChannel *voice;
    if (dsp == NULL || channel < 0 || channel >= kDspMaximumVoiceCount ||
        reg > 7u) return;
    voice = &dsp->channel[channel];
    switch (reg) {
        case 0: voice->volumeL = (int8_t)value; break;
        case 1: voice->volumeR = (int8_t)value; break;
        case 2:
            voice->pitch = (uint16_t)((voice->pitch & 0x3f00u) | value);
            break;
        case 3:
            voice->pitch = (uint16_t)((((uint16_t)value << 8) |
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
            voice->gainValue = voice->directGain
                ? (uint16_t)((value & 0x7fu) << 4) : voice->gainValue;
            voice->gainMode = (uint8_t)((value >> 5) & 3u);
            voice->adsrRates[3] = kRatePeriods[value & 0x1fu];
            break;
        default: break;
    }
}

static void mirror_hardware_mask(Dsp *dsp, uint8_t address, uint8_t value,
                                 uint8_t update_mask) {
    int channel;
    for (channel = 0; channel < kDspHardwareVoiceCount; ++channel) {
        const uint8_t bit = (uint8_t)(1u << channel);
        DspChannel *voice;
        bool enabled;
        if ((update_mask & bit) == 0u) continue;
        voice = &dsp->channel[channel];
        enabled = (value & bit) != 0u;
        switch (address) {
            case 0x2d: voice->pitchModulation = enabled; break;
            case 0x3d: voice->useNoise = enabled; break;
            case 0x4c: voice->keyOn = enabled; break;
            case 0x5c: voice->keyOff = enabled; break;
            case 0x4d: voice->echoEnable = enabled; break;
            default: return;
        }
    }
}

static void sync_accuracy_register_mirrors(Dsp *dsp) {
    if (dsp == NULL || dsp->accuracy == NULL) return;
    sr_dsp_accuracy_copy_registers((const SrDspAccuracy *)dsp->accuracy,
                                   dsp->ram);
    dsp->masterVolumeL = (int8_t)dsp->ram[0x0c];
    dsp->masterVolumeR = (int8_t)dsp->ram[0x1c];
    dsp->echoVolumeL = (int8_t)dsp->ram[0x2c];
    dsp->echoVolumeR = (int8_t)dsp->ram[0x3c];
    dsp->feedbackVolume = (int8_t)dsp->ram[0x0d];
    dsp->dirPage = (uint16_t)dsp->ram[0x5d] << 8;
    dsp->reset = (dsp->ram[0x6c] & 0x80u) != 0u;
    dsp->mute = (dsp->ram[0x6c] & 0x40u) != 0u;
    dsp->echoWrites = (dsp->ram[0x6c] & 0x20u) == 0u;
    dsp->noiseRate = kRatePeriods[dsp->ram[0x6c] & 0x1fu];
    dsp->echoBufferAdr = (uint16_t)dsp->ram[0x6d] << 8;
    dsp->echoDelay = (uint16_t)((dsp->ram[0x7d] & 0x0fu) * 512u);
    if (dsp->echoDelay == 0u) dsp->echoDelay = 1u;
}

static void sync_accuracy_voice_mirrors(Dsp *dsp) {
    int channel;
    if (dsp == NULL || dsp->accuracy == NULL) return;
    for (channel = 0; channel < kDspMaximumVoiceCount; ++channel) {
        SrDspAccuracyVoice source;
        DspChannel *voice = &dsp->channel[channel];
        sr_dsp_accuracy_get_voice((const SrDspAccuracy *)dsp->accuracy,
                                  channel, &source);
        voice->pitchCounter = source.pitch_counter;
        voice->gain = source.envelope;
        voice->sampleOut = source.amplitude;
        voice->decodeOffset = source.brr_address;
        voice->srcn = source.source_number;
        voice->adsrState = source.phase == 3u
            ? kEnvelopeRelease : source.phase;
    }
}

void dsp_syncAccuracyMirrors(Dsp *dsp) {
    sync_accuracy_register_mirrors(dsp);
    sync_accuracy_voice_mirrors(dsp);
}

Dsp *dsp_init(uint8_t *ram) {
    Dsp *dsp;
    if (ram == NULL) return NULL;
    dsp = (Dsp *)calloc(1u, sizeof(*dsp));
    if (dsp == NULL) return NULL;
    dsp->apu_ram = ram;
    dsp->accuracy = sr_dsp_accuracy_create();
    dsp->shadow = dsp_shadow_create();
    if (dsp->accuracy == NULL) {
        dsp_shadow_free((DspShadow *)dsp->shadow);
        free(dsp);
        return NULL;
    }
    dsp_syncAccuracyMirrors(dsp);
    return dsp;
}

void dsp_free(Dsp *dsp) {
    if (dsp == NULL) return;
    sr_dsp_accuracy_destroy((SrDspAccuracy *)dsp->accuracy);
    dsp_shadow_free((DspShadow *)dsp->shadow);
    free(dsp);
}

void dsp_reset(Dsp *dsp) {
    uint8_t *apu_ram;
    Apu *apu;
    void *shadow;
    void *accuracy;
    if (dsp == NULL) return;
    apu_ram = dsp->apu_ram;
    apu = dsp->apu;
    shadow = dsp->shadow;
    accuracy = dsp->accuracy;
    memset(dsp, 0, sizeof(*dsp));
    dsp->apu_ram = apu_ram;
    dsp->apu = apu;
    dsp->shadow = shadow;
    dsp->accuracy = accuracy;
    sr_dsp_accuracy_reset((SrDspAccuracy *)accuracy);
    dsp_syncAccuracyMirrors(dsp);
    dsp_refreshMixControls(dsp);
    dsp->noiseSample = -0x4000;
}

void dsp_saveload(Dsp *dsp, SaveLoadInfo *info) {
    unsigned index;
    if (dsp == NULL || info == NULL || info->func == NULL) return;
    sr_dsp_accuracy_saveload((SrDspAccuracy *)dsp->accuracy, info);
    saveload_bytes(info, dsp->voiceBus, sizeof(dsp->voiceBus));
    for (index = 0; index < kDspMaximumVoiceCount; ++index) {
        DspChannel *voice = &dsp->channel[index];
        saveload_u16(info, &voice->pitch);
        saveload_bool(info, &voice->pitchModulation);
        saveload_u8(info, &voice->srcn);
        saveload_bool(info, &voice->useNoise);
        saveload_bool(info, &voice->useGain);
        saveload_bool(info, &voice->directGain);
        saveload_u16(info, &voice->gainValue);
        saveload_i8(info, &voice->volumeL);
        saveload_i8(info, &voice->volumeR);
        saveload_bool(info, &voice->echoEnable);
    }
    if (!info->semantic) {
        saveload_i16_array(info, dsp->sampleBuffer,
                           sizeof(dsp->sampleBuffer) /
                               sizeof(dsp->sampleBuffer[0]));
        saveload_u32(info, &dsp->sampleWrite);
        saveload_u32(info, &dsp->sampleRead);
    }
    if (!info->saving && !info->failed) {
        dsp_syncAccuracyMirrors(dsp);
        dsp_refreshMixControls(dsp);
    }
}

static void notify_voice_key_on(Dsp *dsp, int channel) {
    uint16_t directory;
    uint16_t start;
    DspChannel *voice;
    if (dsp == NULL || dsp->apu == NULL || channel < 0 ||
        channel >= kDspMaximumVoiceCount ||
        !sr_runner_audio_trace_enabled(SR_AUDIO_TRACE_MASK_DSP_KEY_ON))
        return;
    voice = &dsp->channel[channel];
    directory = (uint16_t)(dsp->dirPage + 4u * voice->srcn);
    start = read_u16(dsp->apu_ram, directory);
    sr_runner_emit_audio_key_on(
        dsp->apu, (uint8_t)channel, voice->srcn, start,
        voice->volumeL, voice->volumeR, voice->pitch);
}

static void notify_key_on(Dsp *dsp, uint8_t bits) {
    int channel;
    if (dsp == NULL || dsp->apu == NULL ||
        !sr_runner_audio_trace_enabled(SR_AUDIO_TRACE_MASK_DSP_KEY_ON))
        return;
    for (channel = 0; channel < kDspHardwareVoiceCount; ++channel) {
        const uint8_t bit = (uint8_t)(1u << channel);
        if ((bits & bit) == 0u) continue;
        notify_voice_key_on(dsp, channel);
    }
}

void dsp_writeVirtualVoiceRegister(Dsp *dsp, int channel,
                                   uint8_t source_address, uint8_t value) {
    if (dsp == NULL || channel < kDspHardwareVoiceCount ||
        channel >= kDspMaximumVoiceCount) return;
    audio_trace_on_reg_write(source_address, value);
    sr_dsp_accuracy_write_virtual_register(
        (SrDspAccuracy *)dsp->accuracy, channel, source_address, value);
    mirror_voice_register(dsp, channel, (uint8_t)(source_address & 0x0fu),
                          value);
}

void dsp_writeVirtualVoiceControl(Dsp *dsp, int channel,
                                  uint8_t global_address, bool enabled) {
    DspChannel *voice;
    if (dsp == NULL || channel < kDspHardwareVoiceCount ||
        channel >= kDspMaximumVoiceCount) return;
    sr_dsp_accuracy_write_virtual_control(
        (SrDspAccuracy *)dsp->accuracy, channel, global_address, enabled);
    voice = &dsp->channel[channel];
    switch (global_address) {
        case 0x2d: voice->pitchModulation = enabled; break;
        case 0x3d: voice->useNoise = enabled; break;
        case 0x4c: voice->keyOn = enabled; break;
        case 0x5c: voice->keyOff = enabled; break;
        case 0x4d: voice->echoEnable = enabled; break;
        default: break;
    }
    if ((global_address & 0x7fu) == 0x4cu && enabled)
        notify_voice_key_on(dsp, channel);
}

void dsp_writeHardwareVoiceMask(Dsp *dsp, uint8_t address, uint8_t value,
                                uint8_t update_mask) {
    if (dsp == NULL || (address != 0x2du && address != 0x3du &&
        address != 0x4cu && address != 0x5cu && address != 0x4du)) return;
    audio_trace_on_reg_write(address, value);
    sr_dsp_accuracy_write_hardware_mask(
        (SrDspAccuracy *)dsp->accuracy, address, value, update_mask);
    mirror_hardware_mask(dsp, address, value, update_mask);
    sync_accuracy_register_mirrors(dsp);
    if (address == 0x4cu) notify_key_on(dsp, (uint8_t)(value & update_mask));
}

uint8_t dsp_read(Dsp *dsp, uint8_t address) {
    if (dsp == NULL) return 0u;
    return sr_dsp_accuracy_read((const SrDspAccuracy *)dsp->accuracy,
                                (uint8_t)(address & 0x7fu));
}

void dsp_copyRegisters(const Dsp *dsp, uint8_t registers[0x80]) {
    if (dsp == NULL || registers == NULL) return;
    sr_dsp_accuracy_copy_registers((const SrDspAccuracy *)dsp->accuracy,
                                   registers);
}

void dsp_write(Dsp *dsp, uint8_t address, uint8_t value) {
    int channel;
    if (dsp == NULL) return;
    address &= 0x7fu;
    audio_trace_on_reg_write(address, value);
    sr_dsp_accuracy_write((SrDspAccuracy *)dsp->accuracy, address, value);
    channel = address >> 4;
    if (channel < kDspHardwareVoiceCount && (address & 0x0fu) <= 7u)
        mirror_voice_register(dsp, channel, (uint8_t)(address & 0x0fu), value);
    switch (address) {
        case 0x2d:
        case 0x3d:
        case 0x4c:
        case 0x4d:
        case 0x5c:
            mirror_hardware_mask(dsp, address, value, 0xffu);
            break;
        default: break;
    }
    sync_accuracy_register_mirrors(dsp);
    if (address == 0x4cu) notify_key_on(dsp, value);
}

uint8_t dsp_currentSlot(const Dsp *dsp) {
    return dsp == NULL ? 0u :
        sr_dsp_accuracy_slot((const SrDspAccuracy *)dsp->accuracy);
}

void dsp_cycle(Dsp *dsp) {
    int slot;
    if (dsp == NULL) return;
    for (slot = 0; slot < 32; ++slot) dsp_clock(dsp);
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
