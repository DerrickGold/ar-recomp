#include "snesrecomp/host/audio_trace.h"
#include "snes/dsp.h"
#include "snes/dsp_shadow.h"
#include "snes/saveload.h"

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

static void ConfigureNoiseVoice(Dsp *dsp, int voice) {
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
    dsp_writeHardwareVoiceMask(dsp, 0x3du, bit, bit);
    dsp_writeHardwareVoiceMask(dsp, 0x4cu, bit, bit);
  } else {
    dsp_writeVirtualVoiceControl(dsp, voice, 0x3du, true);
    dsp_writeVirtualVoiceControl(dsp, voice, 0x4cu, true);
  }
  dsp_setVoiceBus(dsp, voice,
                  voice < 8 ? kDspVoiceBus_Music : kDspVoiceBus_Sfx);
}

static void RunCase(const char *name, bool extended, int active_voices,
  int sample_count) {
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
  dsp_write(dsp, 0x0cu, 0x7fu);
  dsp_write(dsp, 0x1cu, 0x7fu);
  dsp_write(dsp, 0x6cu, 0x20u);
  dsp_setExtendedVoicesEnabled(extended);
  dsp_setBusGains(100, 100);
  for (int voice = 0; voice < active_voices; voice++)
    ConfigureNoiseVoice(dsp, voice);
  for (int i = 0; i < 10000; i++)
    dsp_cycle(dsp);

  const clock_t begin = clock();
  for (int i = 0; i < sample_count; i++)
    dsp_cycle(dsp);
  const clock_t elapsed = clock() - begin;
  const double seconds = (double)elapsed / CLOCKS_PER_SEC;
  const double ns_per_sample = seconds * 1000000000.0 / sample_count;
  printf("%-25s %8.2f ns/sample  %6.3f%% of one core at 32.04 kHz\n",
         name, ns_per_sample, ns_per_sample * 32040.0 / 10000000.0);
  dsp_free(dsp);
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
  RunCase("authentic, 8 active", false, 8, sample_count);
  RunCase("extended, 8 active", true, 8, sample_count);
  RunCase("extended, 40 active", true, 40, sample_count);
  return 0;
}
