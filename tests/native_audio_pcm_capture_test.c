#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "byte_order.h"
#include "dev/native_audio_pcm_capture.h"

static int s_failures;

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expression); \
    s_failures++; \
  } \
} while (0)

int main(void) {
  NativeAudioPcmCapture capture;
  CHECK(!NativeAudioPcmCapture_Init(NULL, 4u));
  CHECK(!NativeAudioPcmCapture_Init(&capture, 0u));
  CHECK(NativeAudioPcmCapture_Init(&capture, 4u));

  const int16_t first[] = {1, 2, 3, 4, 5, 6};
  const int16_t second[] = {7, 8, 9, 10, 11, 12};
  CHECK(!NativeAudioPcmCapture_Append(
      &capture, 10u, first, 3u, 32000u, 1u));
  CHECK(!NativeAudioPcmCapture_Append(
      &capture, 10u, first, 3u, UINT32_MAX, 2u));
  CHECK(NativeAudioPcmCapture_Append(
      &capture, 10u, first, 3u, 32000u, 2u));
  CHECK(NativeAudioPcmCapture_Append(
      &capture, 13u, second, 3u, 32000u, 2u));
  CHECK(capture.first_frame == 12u);
  CHECK(capture.end_frame == 16u);
  CHECK(capture.sample_rate == 32000u);

  char path[] = "/tmp/actraiser-native-pcm-XXXXXX";
  int descriptor = mkstemp(path);
  CHECK(descriptor >= 0);
  if (descriptor >= 0) close(descriptor);
  CHECK(NativeAudioPcmCapture_WriteWav(&capture, path));

  uint8_t wav[60] = {0};
  FILE *file = fopen(path, "rb");
  CHECK(file != NULL);
  if (file) {
    CHECK(fread(wav, 1, sizeof(wav), file) == sizeof(wav));
    CHECK(fgetc(file) == EOF);
    CHECK(fclose(file) == 0);
  }
  CHECK(memcmp(wav, "RIFF", 4) == 0);
  CHECK(memcmp(wav + 8, "WAVEfmt ", 8) == 0);
  CHECK(ByteOrder_ReadLe32(wav + 24) == 32000u);
  CHECK(ByteOrder_ReadLe16(wav + 22) == 2u);
  CHECK(ByteOrder_ReadLe32(wav + 40) == 16u);
  const int16_t expected[] = {5, 6, 7, 8, 9, 10, 11, 12};
  for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]);
       index++) {
    CHECK((int16_t)ByteOrder_ReadLe16(wav + 44 + index * 2u) ==
          expected[index]);
  }

  /* Gaps and rate changes begin a new segment instead of synthesizing time. */
  const int16_t discontinuous[] = {21, 22, 23, 24};
  CHECK(NativeAudioPcmCapture_Append(
      &capture, 100u, discontinuous, 2u, 32000u, 2u));
  CHECK(capture.first_frame == 100u && capture.end_frame == 102u);
  CHECK(NativeAudioPcmCapture_Append(
      &capture, 102u, discontinuous, 1u, 44100u, 2u));
  CHECK(capture.first_frame == 102u && capture.end_frame == 103u);
  CHECK(capture.sample_rate == 44100u);

  CHECK(unlink(path) == 0);
  NativeAudioPcmCapture_Destroy(&capture);
  CHECK(capture.samples == NULL && capture.frame_capacity == 0u);
  if (s_failures) return 1;
  puts("native audio PCM capture tests: pass");
  return 0;
}
