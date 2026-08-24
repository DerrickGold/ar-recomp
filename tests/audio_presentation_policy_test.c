#include "audio_presentation_policy.h"

#include <stdbool.h>
#include <stdio.h>

static int s_failures;
static int s_music_updates;
static bool s_music_authentic;

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

void MusicReplacements_SetSessionBypassed(bool authentic) {
  s_music_updates++;
  s_music_authentic = authentic;
}

int main(void) {
  AudioPresentationPolicy_Reset();
  CHECK(!AudioPresentationPolicy_IsAuthentic());
  CHECK(!AudioPresentationPolicy_ShouldEmitDialogBlip(false));
  CHECK(AudioPresentationPolicy_ShouldEmitDialogBlip(true));
  CHECK(!s_music_authentic);

  const int reset_updates = s_music_updates;
  AudioPresentationPolicy_SetAuthentic(true);
  CHECK(AudioPresentationPolicy_IsAuthentic());
  CHECK(AudioPresentationPolicy_ShouldEmitDialogBlip(false));
  CHECK(s_music_authentic);
  CHECK(s_music_updates == reset_updates + 1);

  AudioPresentationPolicy_SetAuthentic(true);
  CHECK(s_music_updates == reset_updates + 1);
  AudioPresentationPolicy_SetAuthentic(false);
  CHECK(!AudioPresentationPolicy_IsAuthentic());
  CHECK(!s_music_authentic);
  CHECK(s_music_updates == reset_updates + 2);

  if (s_failures) return 1;
  puts("audio_presentation_policy_test: PASS");
  return 0;
}
