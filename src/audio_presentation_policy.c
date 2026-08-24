#include "audio_presentation_policy.h"

#include "music_replacements.h"

static bool s_authentic;

void AudioPresentationPolicy_Reset(void) {
  s_authentic = false;
  MusicReplacements_SetSessionBypassed(false);
}

void AudioPresentationPolicy_SetAuthentic(bool authentic) {
  if (s_authentic == authentic) return;
  s_authentic = authentic;
  MusicReplacements_SetSessionBypassed(authentic);
}

bool AudioPresentationPolicy_IsAuthentic(void) {
  return s_authentic;
}

bool AudioPresentationPolicy_ShouldEmitDialogBlip(
    bool enhanced_setting_enabled) {
  return s_authentic || enhanced_setting_enabled;
}
