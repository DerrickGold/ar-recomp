#include "snes/audio_shadow.h"

#include <stdbool.h>
#include <stdio.h>

static int failures;

static void check(bool condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "runtime-next audio shadow failed: %s\n", message);
    ++failures;
}

static ShadowJudgement feed_window(ShadowVerifier *verifier, bool matching,
                                   bool silent) {
    ShadowJudgement result = SHADOW_JUDGE_NONE;
    for (unsigned index = 0; index < SHADOW_DECIM * SHADOW_WINDOW; ++index) {
        const float ramp = (float)(index % 997u) / 997.0f;
        const float canonical = silent ? 0.0f : 0.05f + 0.45f * ramp;
        const float candidate = matching ? canonical : (silent ? 0.0f : 0.25f);
        const ShadowJudgement current = shadow_verifier_judge(
            verifier, canonical, canonical * 0.75f,
            candidate, candidate * 0.75f);
        if (current != SHADOW_JUDGE_NONE) result = current;
    }
    return result;
}

static void test_prove_and_revert(void) {
    ShadowVerifier verifier;
    shadow_verifier_init(&verifier);
    check(verifier.gain == 1.0f && verifier.required_passes == 1u &&
          !verifier.proven, "initial state");
    check(feed_window(&verifier, true, false) == SHADOW_JUDGE_PASS &&
          verifier.proven, "matching stream proves candidate");
    check(feed_window(&verifier, false, false) == SHADOW_JUDGE_FAIL,
          "different envelope fails correlation");
    (void)feed_window(&verifier, false, false);
    (void)feed_window(&verifier, false, false);
    check(!verifier.proven && verifier.pauses == 1u &&
          verifier.required_passes == 2u && verifier.reverted[0] != '\0',
          "three failures revert and increase probation");
}

static void test_silence_has_no_verdict(void) {
    ShadowVerifier verifier;
    shadow_verifier_init(&verifier);
    check(feed_window(&verifier, true, true) == SHADOW_JUDGE_NONE &&
          !verifier.proven, "silent canonical stream cannot prove a candidate");
}

static void test_constant_gain_calibration(void) {
    ShadowVerifier verifier;
    shadow_verifier_init(&verifier);
    for (unsigned window = 0; window < 3u; ++window) {
        for (unsigned index = 0; index < SHADOW_DECIM * SHADOW_WINDOW; ++index) {
            const float signal = 0.05f + 0.4f * (float)(index % 887u) / 887.0f;
            (void)shadow_verifier_judge(&verifier, signal, signal,
                                        signal * 2.0f, signal * 2.0f);
        }
    }
    check(verifier.calibrated && verifier.gain > 0.45f && verifier.gain < 0.55f,
          "stable constant level difference calibrates output gain");
}

int main(void) {
    test_prove_and_revert();
    test_silence_has_no_verdict();
    test_constant_gain_calibration();
    if (failures != 0) {
        fprintf(stderr, "runtime-next audio shadow: %d failure(s)\n", failures);
        return 1;
    }
    puts("runtime-next audio shadow: PASS");
    return 0;
}
