#ifndef SNESRECOMP_DSP_SHADOW_H
#define SNESRECOMP_DSP_SHADOW_H

#include "audio_shadow.h"

typedef struct Dsp Dsp;

typedef struct DspShadow {
    ShadowVerifier verifier;
    int enabled;
} DspShadow;

DspShadow *dsp_shadow_create(void);
void dsp_shadow_free(DspShadow *shadow);
void dsp_shadow_process(DspShadow *shadow, Dsp *dsp, int canonical_left,
                        int canonical_right, int *output_left,
                        int *output_right);

#endif
