#include "native_audio_pcm_capture.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "byte_order.h"

enum {
  kPcmChannelCount = 2,
  kPcmWavHeaderBytes = 44,
};

static void ResetSegment(NativeAudioPcmCapture *capture,
                         uint64_t frame_offset, uint32_t sample_rate) {
  capture->first_frame = frame_offset;
  capture->end_frame = frame_offset;
  capture->sample_rate = sample_rate;
}

bool NativeAudioPcmCapture_Init(NativeAudioPcmCapture *capture,
                                uint32_t frame_capacity) {
  if (!capture || frame_capacity == 0u) return false;
  const size_t sample_count =
      (size_t)frame_capacity * (size_t)kPcmChannelCount;
  if (sample_count / kPcmChannelCount != frame_capacity ||
      sample_count > SIZE_MAX / sizeof(int16_t))
    return false;
  memset(capture, 0, sizeof(*capture));
  capture->samples = (int16_t *)malloc(
      sample_count * sizeof(*capture->samples));
  if (!capture->samples) return false;
  capture->frame_capacity = frame_capacity;
  return true;
}

void NativeAudioPcmCapture_Destroy(NativeAudioPcmCapture *capture) {
  if (!capture) return;
  free(capture->samples);
  memset(capture, 0, sizeof(*capture));
}

bool NativeAudioPcmCapture_Append(NativeAudioPcmCapture *capture,
                                  uint64_t frame_offset,
                                  const int16_t *samples,
                                  uint32_t frame_count,
                                  uint32_t sample_rate,
                                  uint32_t channel_count) {
  if (!capture || !capture->samples || capture->frame_capacity == 0u ||
      !samples || frame_count == 0u || sample_rate == 0u ||
      channel_count != kPcmChannelCount ||
      sample_rate > UINT32_MAX /
          (kPcmChannelCount * sizeof(int16_t)) ||
      frame_count > UINT64_MAX - frame_offset)
    return false;

  if (capture->end_frame != frame_offset ||
      (capture->sample_rate != 0u && capture->sample_rate != sample_rate))
    ResetSegment(capture, frame_offset, sample_rate);
  if (capture->sample_rate == 0u) capture->sample_rate = sample_rate;

  for (uint32_t frame = 0; frame < frame_count; frame++) {
    const uint64_t absolute = frame_offset + frame;
    const size_t destination =
        (size_t)(absolute % capture->frame_capacity) * kPcmChannelCount;
    const size_t source = (size_t)frame * kPcmChannelCount;
    capture->samples[destination] = samples[source];
    capture->samples[destination + 1u] = samples[source + 1u];
  }
  capture->end_frame = frame_offset + frame_count;
  if (capture->end_frame - capture->first_frame > capture->frame_capacity)
    capture->first_frame = capture->end_frame - capture->frame_capacity;
  return true;
}

bool NativeAudioPcmCapture_WriteWav(const NativeAudioPcmCapture *capture,
                                    const char *path) {
  if (!capture || !capture->samples || !path || !path[0] ||
      capture->sample_rate == 0u ||
      capture->end_frame <= capture->first_frame)
    return false;

  const uint64_t frame_count = capture->end_frame - capture->first_frame;
  if (frame_count > (UINT32_MAX - (kPcmWavHeaderBytes - 8u)) /
                        (kPcmChannelCount * sizeof(int16_t)))
    return false;
  const uint32_t data_bytes =
      (uint32_t)(frame_count * kPcmChannelCount * sizeof(int16_t));
  uint8_t header[kPcmWavHeaderBytes] = {0};
  memcpy(header, "RIFF", 4);
  ByteOrder_WriteLe32(header + 4, data_bytes + kPcmWavHeaderBytes - 8u);
  memcpy(header + 8, "WAVEfmt ", 8);
  ByteOrder_WriteLe32(header + 16, 16u);
  ByteOrder_WriteLe16(header + 20, 1u);
  ByteOrder_WriteLe16(header + 22, kPcmChannelCount);
  ByteOrder_WriteLe32(header + 24, capture->sample_rate);
  ByteOrder_WriteLe32(
      header + 28,
      capture->sample_rate * kPcmChannelCount * sizeof(int16_t));
  ByteOrder_WriteLe16(header + 32, kPcmChannelCount * sizeof(int16_t));
  ByteOrder_WriteLe16(header + 34, 16u);
  memcpy(header + 36, "data", 4);
  ByteOrder_WriteLe32(header + 40, data_bytes);

  FILE *file = fopen(path, "wb");
  bool success = file &&
      fwrite(header, 1, sizeof(header), file) == sizeof(header);
  const uint64_t first_index =
      capture->first_frame % capture->frame_capacity;
  const uint64_t first_count = frame_count <
          (uint64_t)capture->frame_capacity - first_index
      ? frame_count : (uint64_t)capture->frame_capacity - first_index;
  if (success) {
    success = fwrite(
        capture->samples + first_index * kPcmChannelCount,
        kPcmChannelCount * sizeof(int16_t), (size_t)first_count, file) ==
        first_count;
  }
  if (success && first_count < frame_count) {
    success = fwrite(
        capture->samples, kPcmChannelCount * sizeof(int16_t),
        (size_t)(frame_count - first_count), file) ==
        frame_count - first_count;
  }
  if (file && fclose(file) != 0) success = false;
  return success;
}
