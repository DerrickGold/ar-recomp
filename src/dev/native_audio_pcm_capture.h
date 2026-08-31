#ifndef ACTRAISER_NATIVE_AUDIO_PCM_CAPTURE_H
#define ACTRAISER_NATIVE_AUDIO_PCM_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct NativeAudioPcmCapture {
  int16_t *samples;
  uint64_t first_frame;
  uint64_t end_frame;
  uint32_t sample_rate;
  uint32_t frame_capacity;
} NativeAudioPcmCapture;

bool NativeAudioPcmCapture_Init(NativeAudioPcmCapture *capture,
                                uint32_t frame_capacity);
void NativeAudioPcmCapture_Destroy(NativeAudioPcmCapture *capture);

/* Retain one contiguous S16 stereo segment. A frame-offset gap or sample-rate
 * change starts a new segment so the WAV never invents missing time. */
bool NativeAudioPcmCapture_Append(NativeAudioPcmCapture *capture,
                                  uint64_t frame_offset,
                                  const int16_t *samples,
                                  uint32_t frame_count,
                                  uint32_t sample_rate,
                                  uint32_t channel_count);

bool NativeAudioPcmCapture_WriteWav(const NativeAudioPcmCapture *capture,
                                    const char *path);

#endif /* ACTRAISER_NATIVE_AUDIO_PCM_CAPTURE_H */
