#include "audio_shadow.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { kMaximumStrikes = 3 };
static const float kEnvelopeResponse = 0.0075f;
static const float kMinimumLevel = 0.002f;
static const float kMinimumCorrelation = 0.5f;

void shadow_verifier_init(ShadowVerifier *verifier) {
    if (verifier == NULL) return;
    memset(verifier, 0, sizeof(*verifier));
    verifier->gain = 1.0f;
    verifier->required_passes = 1u;
}

static float mean_channel(const float samples[SHADOW_WINDOW][2], int side) {
    double total = 0.0;
    for (unsigned index = 0; index < SHADOW_WINDOW; ++index) {
        total += samples[index][side];
    }
    return (float)(total / SHADOW_WINDOW);
}

static bool finish_window(ShadowSelfCheck *check, float *correlation,
                          float *ratio) {
    const float canonical_left = mean_channel(check->canonical, 0);
    const float canonical_right = mean_channel(check->canonical, 1);
    const float candidate_left = mean_channel(check->candidate, 0);
    const float candidate_right = mean_channel(check->candidate, 1);
    const float canonical_level = (canonical_left + canonical_right) * 0.5f;
    const float candidate_level = (candidate_left + candidate_right) * 0.5f;
    check->count = 0u;
    if (canonical_level < kMinimumLevel) return false;
    *ratio = candidate_level / canonical_level;

    double variance = 0.0;
    for (unsigned index = 0; index < SHADOW_WINDOW; ++index) {
        const double left = check->canonical[index][0] - canonical_left;
        const double right = check->canonical[index][1] - canonical_right;
        variance += left * left + right * right;
    }
    if (variance / SHADOW_WINDOW <= 1e-10) {
        *correlation = 1.0f;
        return true;
    }

    const unsigned overlap = SHADOW_WINDOW - SHADOW_MAX_LAG;
    bool found = false;
    float best = -1.0f;
    for (unsigned lag = 0; lag <= SHADOW_MAX_LAG; lag += 2u) {
        double channel_sum = 0.0;
        unsigned channels = 0u;
        for (int side = 0; side < 2; ++side) {
            double mean_a = 0.0;
            double mean_b = 0.0;
            for (unsigned index = 0; index < overlap; ++index) {
                mean_a += check->canonical[SHADOW_MAX_LAG + index][side];
                mean_b += check->candidate[SHADOW_MAX_LAG - lag + index][side];
            }
            mean_a /= overlap;
            mean_b /= overlap;
            double covariance = 0.0;
            double variance_a = 0.0;
            double variance_b = 0.0;
            for (unsigned index = 0; index < overlap; ++index) {
                const double a =
                    check->canonical[SHADOW_MAX_LAG + index][side] - mean_a;
                const double b =
                    check->candidate[SHADOW_MAX_LAG - lag + index][side] - mean_b;
                covariance += a * b;
                variance_a += a * a;
                variance_b += b * b;
            }
            if (variance_a > 1e-12 && variance_b > 1e-12) {
                channel_sum += covariance / sqrt(variance_a * variance_b);
                ++channels;
            }
        }
        if (channels != 0u) {
            const float value = (float)(channel_sum / channels);
            if (!found || value > best) best = value;
            found = true;
        }
    }
    *correlation = found ? best : 0.0f;
    return true;
}

static bool push_sample(ShadowSelfCheck *check, float canonical_left,
                        float canonical_right, float candidate_left,
                        float candidate_right, float *correlation,
                        float *ratio) {
    const float canonical[2] = {fabsf(canonical_left), fabsf(canonical_right)};
    const float candidate[2] = {fabsf(candidate_left), fabsf(candidate_right)};
    for (int side = 0; side < 2; ++side) {
        check->canonical_envelope[side] += kEnvelopeResponse *
            (canonical[side] - check->canonical_envelope[side]);
        check->candidate_envelope[side] += kEnvelopeResponse *
            (candidate[side] - check->candidate_envelope[side]);
    }
    if (++check->phase < SHADOW_DECIM) return false;
    check->phase = 0u;
    for (int side = 0; side < 2; ++side) {
        check->canonical[check->count][side] = check->canonical_envelope[side];
        check->candidate[check->count][side] = check->candidate_envelope[side];
    }
    if (++check->count < SHADOW_WINDOW) return false;
    return finish_window(check, correlation, ratio);
}

static void pause_verifier(ShadowVerifier *verifier, float correlation,
                           float ratio) {
    verifier->proven = false;
    verifier->consecutive_passes = 0u;
    verifier->check.strikes = 0u;
    verifier->required_passes = verifier->required_passes < 8u
        ? verifier->required_passes * 2u : 16u;
    ++verifier->pauses;
    snprintf(verifier->reverted, sizeof(verifier->reverted),
             "shadow/canonical correlation %.2f, level ratio %.2f",
             (double)correlation, (double)ratio);
}

static bool try_calibrate(ShadowVerifier *verifier, float correlation,
                          float ratio) {
    if (verifier->calibrated || correlation < 0.7f ||
        (ratio >= 0.85f && ratio <= 1.15f) || ratio < 0.2f || ratio > 5.0f ||
        verifier->ratio_count >= 6u) {
        return false;
    }
    verifier->ratio_history[verifier->ratio_count % 3u] = ratio;
    ++verifier->ratio_count;
    if (verifier->ratio_count < 3u) return true;
    const float average = (verifier->ratio_history[0] +
                           verifier->ratio_history[1] +
                           verifier->ratio_history[2]) / 3.0f;
    for (unsigned index = 0; index < 3u; ++index) {
        if (fabsf(verifier->ratio_history[index] / average - 1.0f) >= 0.1f) {
            return true;
        }
    }
    verifier->gain /= average;
    if (verifier->gain < 0.25f) verifier->gain = 0.25f;
    if (verifier->gain > 4.0f) verifier->gain = 4.0f;
    verifier->calibrated = true;
    verifier->check.strikes = 0u;
    return true;
}

ShadowJudgement shadow_verifier_judge(ShadowVerifier *verifier,
                                      float canonical_left,
                                      float canonical_right,
                                      float candidate_left,
                                      float candidate_right) {
    if (verifier == NULL) return SHADOW_JUDGE_NONE;
    float correlation = 0.0f;
    float ratio = 0.0f;
    if (!push_sample(&verifier->check, canonical_left, canonical_right,
                     candidate_left, candidate_right, &correlation, &ratio)) {
        return SHADOW_JUDGE_NONE;
    }
    verifier->last_correlation = correlation;
    verifier->last_ratio = ratio;
    if (try_calibrate(verifier, correlation, ratio)) return SHADOW_JUDGE_NONE;

    const bool level_matches = ratio >= 0.55f && ratio <= 1.6f;
    const bool passes = correlation >= kMinimumCorrelation && level_matches;
    if (verifier->proven) {
        if (passes) {
            if (verifier->check.strikes != 0u) --verifier->check.strikes;
        } else if (++verifier->check.strikes >= kMaximumStrikes) {
            pause_verifier(verifier, correlation, ratio);
        }
    } else if (passes) {
        if (++verifier->consecutive_passes >= verifier->required_passes) {
            verifier->proven = true;
            verifier->check.strikes = 0u;
        }
    } else {
        verifier->consecutive_passes = 0u;
    }
    verifier->failed_structurally =
        !passes && correlation < kMinimumCorrelation && level_matches;
    return passes ? SHADOW_JUDGE_PASS : SHADOW_JUDGE_FAIL;
}
