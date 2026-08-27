/* Differential audio verifier.
 *
 * Derived from the engine-neutral shadow verifier shared by the recomp
 * ecosystem, copyright Jrickey, licensed MIT OR Apache-2.0. This copy is
 * distributed under runtime's MIT license. */
#ifndef SNESRECOMP_AUDIO_SHADOW_H
#define SNESRECOMP_AUDIO_SHADOW_H

#include <stdbool.h>
#include <stdint.h>

typedef enum ShadowJudgement {
    SHADOW_JUDGE_NONE = 0,
    SHADOW_JUDGE_PASS,
    SHADOW_JUDGE_FAIL
} ShadowJudgement;

enum {
    SHADOW_DECIM = 64,
    SHADOW_WINDOW = 1024,
    SHADOW_MAX_LAG = 56
};

typedef struct ShadowSelfCheck {
    float canonical_envelope[2];
    float candidate_envelope[2];
    float canonical[SHADOW_WINDOW][2];
    float candidate[SHADOW_WINDOW][2];
    uint32_t count;
    uint32_t phase;
    uint32_t strikes;
} ShadowSelfCheck;

typedef struct ShadowVerifier {
    ShadowSelfCheck check;
    float gain;
    bool calibrated;
    float ratio_history[3];
    uint32_t ratio_count;
    bool proven;
    uint32_t required_passes;
    uint32_t consecutive_passes;
    uint64_t pauses;
    char reverted[160];
    float last_correlation;
    float last_ratio;
    bool failed_structurally;
} ShadowVerifier;

void shadow_verifier_init(ShadowVerifier *verifier);
ShadowJudgement shadow_verifier_judge(ShadowVerifier *verifier,
                                      float canonical_left,
                                      float canonical_right,
                                      float candidate_left,
                                      float candidate_right);

static inline float shadow_verifier_gain(const ShadowVerifier *verifier) {
    return verifier->gain;
}

static inline bool shadow_verifier_proven(const ShadowVerifier *verifier) {
    return verifier->proven;
}

#endif
