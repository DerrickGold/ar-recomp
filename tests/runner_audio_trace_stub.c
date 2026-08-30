#include "runner_internal.h"

_Atomic(SrAudioTraceMask) g_sr_runner_audio_trace_mask;

void sr_runner_emit_audio_key_on(Apu *apu, uint8_t voice_index,
                                 uint8_t source_number,
                                 uint16_t brr_address,
                                 int16_t volume_left,
                                 int16_t volume_right,
                                 uint16_t pitch) {
  (void)apu;
  (void)voice_index;
  (void)source_number;
  (void)brr_address;
  (void)volume_left;
  (void)volume_right;
  (void)pitch;
}
