#ifndef AR_NATIVE_AUDIO_MIXER_H
#define AR_NATIVE_AUDIO_MIXER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum NativeAudioVoiceClass {
  kNativeAudioVoice_Unclassified = 0,
  kNativeAudioVoice_Music,
  kNativeAudioVoice_Sfx,
} NativeAudioVoiceClass;

/* Resolve one ActRaiser SPC -> DSP register write into physical-voice
 * provenance. The driver exposes the currently serviced logical track as a
 * one-bit mask at ARAM $47, while SPC X identifies song tracks $00-$0E versus
 * effect tracks $10/$12. Ownership bits at $1A are a fallback for shared
 * register helpers where X has already been repurposed. Returns false for
 * global DSP registers or states that do not identify exactly one voice. */
bool NativeAudioMixer_ClassifyDspWrite(
    uint8_t dsp_addr, uint8_t logical_track, uint8_t track_mask,
    uint8_t ownership_mask,
    int *voice, NativeAudioVoiceClass *voice_class);

/* Install the normal-play provenance observer and apply configured bus gains.
 * Safe before SnesInit: the gain values are presentation globals and the
 * observer begins classifying once the SPC starts writing registers. */
void NativeAudioMixer_Install(void);

/* Live settings callback for native voices and replacement OGG music. */
void NativeAudioMixer_ApplySettings(void);

#endif
