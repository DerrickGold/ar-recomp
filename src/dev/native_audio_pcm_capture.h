#ifndef AR_NATIVE_AUDIO_PCM_CAPTURE_H
#define AR_NATIVE_AUDIO_PCM_CAPTURE_H
/* NativeAudioPcmCapture: collects the native audio output as contiguous S16
 * stereo segments (a gap or rate change starts a new one) and writes them out
 * as WAV files for audio tracing.
 * Phase: developer tools.
 * Tests: tests/native_audio_pcm_capture_test.c */

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

#endif /* AR_NATIVE_AUDIO_PCM_CAPTURE_H */
