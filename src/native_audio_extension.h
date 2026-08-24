#ifndef AR_NATIVE_AUDIO_EXTENSION_H
#define AR_NATIVE_AUDIO_EXTENSION_H

#include <stdbool.h>
#include <stdint.h>

/* Pure routing helpers for ActRaiser's two effect sequencer tracks. */
bool NativeAudioExtension_RouteVoiceWrite(
    uint8_t dsp_addr, uint8_t logical_track, uint8_t track_mask,
    uint8_t ownership_mask, int *hardware_voice, int *virtual_voice);
uint8_t NativeAudioExtension_RoutedGlobalMask(
    uint8_t logical_track, uint8_t track_mask, uint8_t ownership_mask);
bool NativeAudioExtension_ShouldBypassMusicSuppression(
    uint16_t spc_pc, uint8_t logical_track, uint8_t track_mask,
    uint8_t ownership_mask);

/* Install the restart-class optional bridge. Safe before SnesInit; the DSP
 * core stores enablement globally and initializes its added voices on reset. */
void NativeAudioExtension_Install(void);
bool NativeAudioExtension_IsEnabled(void);

#endif
