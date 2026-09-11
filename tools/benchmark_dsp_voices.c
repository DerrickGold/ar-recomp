#include "snes/dsp.h"
#include "snes/dsp_shadow.h"
#include "snes/saveload.h"
#include "snesrecomp/game/audio_timing.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Focused DSP benchmark: keep diagnostics and the optional shadow renderer
 * inert so the result measures the canonical voice/echo mixer only. */
int snes_frame_counter;
DspShadow *dsp_shadow_create(void) { return NULL; }
void dsp_shadow_free(DspShadow *shadow) { (void)shadow; }
void dsp_shadow_process(DspShadow *shadow, Dsp *dsp, int canon_l, int canon_r,
                        int *out_l, int *out_r) {
  (void)shadow;
  (void)dsp;
  *out_l = canon_l;
  *out_r = canon_r;
}
void audio_trace_on_sample(int16_t l, int16_t r, int dropped,
                           uint32_t ring_fill) {
  (void)l; (void)r; (void)dropped; (void)ring_fill;
}
void audio_trace_on_reg_write(uint8_t addr, uint8_t val) {
  (void)addr; (void)val;
}
void audio_trace_on_consume(uint64_t read_idx, uint32_t count,
                            uint32_t avail_after) {
  (void)read_idx; (void)count; (void)avail_after;
}

static void ConfigureVoice(Dsp *dsp, int voice, bool noise, bool echo) {
  const int bank_voice = voice & 7;
  const uint8_t bit = (uint8_t)(1u << bank_voice);
  const uint8_t base = (uint8_t)(bank_voice * 0x10);
  const uint8_t values[8] = {
      64u, (uint8_t)-48, 0u, 0x10u, 0u, 0u, 0u, 0x7fu,
  };
  for (uint8_t reg = 0; reg < 8u; ++reg) {
    if (voice < kDspHardwareVoiceCount)
      dsp_write(dsp, (uint8_t)(base + reg), values[reg]);
    else
      dsp_writeVirtualVoiceRegister(
          dsp, voice, (uint8_t)(base + reg), values[reg]);
  }
  if (voice < kDspHardwareVoiceCount) {
    dsp_writeHardwareVoiceMask(dsp, 0x3du, noise ? bit : 0, bit);
    dsp_writeHardwareVoiceMask(dsp, 0x4du, echo ? bit : 0, bit);
    dsp_writeHardwareVoiceMask(dsp, 0x4cu, bit, bit);
  } else {
    dsp_writeVirtualVoiceControl(dsp, voice, 0x3du, noise);
    dsp_writeVirtualVoiceControl(dsp, voice, 0x4du, echo);
    dsp_writeVirtualVoiceControl(dsp, voice, 0x4cu, true);
  }
  dsp_setVoiceBus(dsp, voice,
                  voice < 8 ? kDspVoiceBus_Music : kDspVoiceBus_Sfx);
}

static void RunCase(const char *name, bool extended, uint64_t voices,
                    bool noise, bool echo, int sample_count) {
  uint8_t *ram = (uint8_t *)calloc(0x10000, 1);
  if (!ram) {
    fprintf(stderr, "benchmark_dsp_voices: allocation failed\n");
    exit(1);
  }
  Dsp *dsp = dsp_init(ram);
  if (!dsp) {
    fprintf(stderr, "benchmark_dsp_voices: allocation failed\n");
    free(ram);
    exit(1);
  }
  dsp_reset(dsp);
  /* A looping, nonzero BRR stream with filter history, away from the directory
   * and echo RAM. Sparse cases reproduce the game's fixed destination lanes. */
  ram[0x2001] = ram[0x2003] = 0x30;
  ram[0x3000] = 0x9b; /* range 9, filter 2, end + loop */
  for (int i = 1; i < 9; ++i) ram[0x3000 + i] = (uint8_t)(i * 37);
  dsp_write(dsp, 0x5du, 0x20u);
  dsp_write(dsp, 0x0cu, 0x7fu);
  dsp_write(dsp, 0x1cu, 0x7fu);
  dsp_write(dsp, 0x6cu, echo ? 0u : 0x20u);
  dsp_write(dsp, 0x2cu, 0x30u);
  dsp_write(dsp, 0x3cu, 0x30u);
  dsp_write(dsp, 0x0du, 0x20u);
  dsp_write(dsp, 0x0fu, 0x7fu);
  dsp_write(dsp, 0x6du, 0x60u);
  dsp_write(dsp, 0x7du, 1u);
  dsp_setExtendedVoicesEnabled(extended);
  dsp_setBusGains(100, 100);
  for (int voice = 0; voice < kDspMaximumVoiceCount; voice++)
    if (voices & (UINT64_C(1) << voice)) ConfigureVoice(dsp, voice, noise, echo);
  for (int i = 0; i < 10000; i++) {
    dsp_cycle(dsp);
    dsp->sampleRead = dsp->sampleWrite;
  }

  const clock_t begin = clock();
  for (int i = 0; i < sample_count; i++) {
    dsp_cycle(dsp);
    /* Model a draining consumer, not the full-ring/drop fast path. */
    dsp->sampleRead = dsp->sampleWrite;
  }
  const clock_t elapsed = clock() - begin;
  const double seconds = (double)elapsed / CLOCKS_PER_SEC;
  const double ns_per_sample = seconds * 1000000000.0 / sample_count;
  uint32_t checksum = 2166136261u;
  for (unsigned i = 0; i < DSP_SAMPLE_RING * 2u; ++i)
    checksum = (checksum ^ (uint16_t)dsp->sampleBuffer[i]) * 16777619u;
  printf("%-29s %8.2f ns/sample  %6.3f%% core at 32 kHz  pcm=%08x\n",
         name, ns_per_sample,
         ns_per_sample * RTL_AUDIO_NATIVE_RATE / 10000000.0, checksum);
  dsp_free(dsp);
  free(ram);
}

static void RunResamplerCase(int sample_count) {
  enum { kOutputFrames = 1024 };
  uint8_t *ram = (uint8_t *)calloc(0x10000, 1);
  int16_t *output =
      (int16_t *)calloc(kOutputFrames * 2u, sizeof(*output));
  if (!ram || !output) {
    fprintf(stderr, "benchmark_dsp_voices: allocation failed\n");
    free(output);
    free(ram);
    exit(1);
  }
  Dsp *dsp = dsp_init(ram);
  if (!dsp) {
    fprintf(stderr, "benchmark_dsp_voices: allocation failed\n");
    free(output);
    free(ram);
    exit(1);
  }
  for (uint32_t frame = 0; frame < DSP_SAMPLE_RING; ++frame) {
    dsp->sampleBuffer[frame * 2u] =
        (int16_t)((frame * 109u + 3001u) & 0xffffu);
    dsp->sampleBuffer[frame * 2u + 1u] =
        (int16_t)((frame * 251u + 1709u) & 0xffffu);
  }
  dsp->sampleWrite = DSP_SAMPLE_RING;
  double phase = 0.375;
  uint32_t checksum = 0;
  int remaining = sample_count;
  const clock_t begin = clock();
  while (remaining > 0) {
    const int frames = remaining < kOutputFrames
        ? remaining : kOutputFrames;
    dsp_getSamplesResampled(dsp, output, frames, RTL_AUDIO_NATIVE_RATE / 48000.0,
                            &phase);
    checksum += (uint16_t)output[(remaining & (kOutputFrames - 1)) * 2];
    remaining -= frames;
  }
  const clock_t elapsed = clock() - begin;
  const double seconds = (double)elapsed / CLOCKS_PER_SEC;
  const double ns_per_frame = seconds * 1000000000.0 / sample_count;
  printf("%-29s %8.2f ns/frame   checksum %u\n",
         "48 kHz resampler", ns_per_frame, checksum);
  dsp_free(dsp);
  free(output);
  free(ram);
}

int main(int argc, char **argv) {
  int sample_count = 5000000;
  if (argc > 1) {
    sample_count = atoi(argv[1]);
    if (sample_count <= 0) {
      fprintf(stderr, "usage: %s [positive-sample-count]\n", argv[0]);
      return 2;
    }
  }
  const uint64_t native = 0xffu;
  uint64_t sparse = native, paired = native;
  for (int bank = 1; bank < 5; ++bank) {
    sparse |= UINT64_C(0x80) << (bank * 8);
    paired |= UINT64_C(0xc0) << (bank * 8);
  }
  RunCase("noise: authentic 8", false, native, true, false, sample_count);
  RunCase("noise: extended idle", true, native, true, false, sample_count);
  RunCase("noise: all 40", true, (UINT64_C(1) << 40) - 1, true, false, sample_count);
  RunCase("BRR: authentic 8", false, native, false, false, sample_count);
  RunCase("BRR: packed 4 effects", true, native | 0xf00u, false, false, sample_count);
  RunCase("BRR: sparse 4 effects", true, sparse, false, false, sample_count);
  RunCase("BRR: paired 4 effects", true, paired, false, false, sample_count);
  RunCase("BRR: sparse + echo", true, sparse, false, true, sample_count);
  RunResamplerCase(sample_count);
  return 0;
}
