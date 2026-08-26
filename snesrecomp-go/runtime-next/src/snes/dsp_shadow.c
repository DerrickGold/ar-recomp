#include "dsp_shadow.h"

#include "dsp.h"

#include <stdio.h>
#include <stdlib.h>

static int clamp16(int value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return value;
}

static float cubic(float before, float current, float next, float after,
                   float fraction) {
    const float a = 2.0f * current;
    const float b = next - before;
    const float c = 2.0f * before - 5.0f * current + 4.0f * next - after;
    const float d = 3.0f * (current - next) + after - before;
    return 0.5f * (a + fraction * (b + fraction * (c + fraction * d)));
}

DspShadow *dsp_shadow_create(void) {
    DspShadow *shadow = (DspShadow *)calloc(1u, sizeof(*shadow));
    if (shadow == NULL) return NULL;
    shadow_verifier_init(&shadow->verifier);
    const char *setting = getenv("SNESRECOMP_AUDIO_SHADOW");
    shadow->enabled = setting != NULL &&
                      !(setting[0] == '0' && setting[1] == '\0');
    if (shadow->enabled) {
        fprintf(stderr, "[audio] S-DSP cubic shadow armed; canonical output "
                        "remains active until verification passes\n");
    }
    return shadow;
}

void dsp_shadow_free(DspShadow *shadow) {
    free(shadow);
}

void dsp_shadow_process(DspShadow *shadow, Dsp *dsp, int canonical_left,
                        int canonical_right, int *output_left,
                        int *output_right) {
    if (output_left == NULL || output_right == NULL) return;
    if (shadow == NULL || !shadow->enabled || dsp == NULL) {
        *output_left = canonical_left;
        *output_right = canonical_right;
        return;
    }

    float left = 0.0f;
    float right = 0.0f;
    for (int channel = 0; channel < kDspHardwareVoiceCount; ++channel) {
        DspChannel *voice = &dsp->channel[channel];
        float sample;
        if (voice->useNoise) {
            sample = dsp->noiseSample;
        } else {
            const unsigned index = voice->pitchCounter >> 12;
            const unsigned previous = index == 0u ? 0u : index - 1u;
            const unsigned next = index < 15u ? index + 1u : index;
            const unsigned after = index < 14u ? index + 2u : next;
            const float fraction = (float)(voice->pitchCounter & 0x0fffu) /
                                   4096.0f;
            sample = cubic(voice->decodeBuffer[previous + 3u],
                           voice->decodeBuffer[index + 3u],
                           voice->decodeBuffer[next + 3u],
                           voice->decodeBuffer[after + 3u], fraction);
        }
        const float enveloped = sample * ((float)voice->gain / 2048.0f);
        left += enveloped * ((float)voice->volumeL / 64.0f);
        right += enveloped * ((float)voice->volumeR / 64.0f);
    }
    const float gain = shadow_verifier_gain(&shadow->verifier);
    left *= (float)dsp->masterVolumeL / 128.0f * gain;
    right *= (float)dsp->masterVolumeR / 128.0f * gain;

    (void)shadow_verifier_judge(&shadow->verifier,
                                (float)canonical_left / 32768.0f,
                                (float)canonical_right / 32768.0f,
                                left / 32768.0f, right / 32768.0f);
    if (shadow->verifier.reverted[0] != '\0') {
        fprintf(stderr, "[audio] S-DSP cubic shadow reverted: %s\n",
                shadow->verifier.reverted);
        shadow->verifier.reverted[0] = '\0';
    }
    if (shadow_verifier_proven(&shadow->verifier)) {
        *output_left = clamp16((int)left);
        *output_right = clamp16((int)right);
    } else {
        *output_left = canonical_left;
        *output_right = canonical_right;
    }
}
