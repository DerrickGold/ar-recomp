#ifndef AR_AUDIO_PRESENTATION_POLICY_H
#define AR_AUDIO_PRESENTATION_POLICY_H

#include <stdbool.h>

/* Session-only authority for presentation modes that temporarily request the
 * ROM's audio behavior. Persistent output/device settings remain outside this
 * policy and continue to apply in every mode. */
void AudioPresentationPolicy_Reset(void);
void AudioPresentationPolicy_SetAuthentic(bool authentic);
bool AudioPresentationPolicy_IsAuthentic(void);

/* Enhanced mode respects the player's dialogue-blip preference. Authentic
 * mode restores the ROM event without exposing render state to the CPU hook. */
bool AudioPresentationPolicy_ShouldEmitDialogBlip(
    bool enhanced_setting_enabled);

#endif /* AR_AUDIO_PRESENTATION_POLICY_H */
