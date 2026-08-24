#include "audio_trace.h"
#include "snes/dsp.h"
#include "snes/dsp_shadow.h"

#include <stdio.h>
#include <string.h>

/* Runtime diagnostics/enhancement seams are inert in this focused DSP test. */
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

static int s_failures;
#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

static void ConfigureVoice(Dsp *dsp, int voice, DspVoiceBus bus,
                           bool echo) {
  DspChannel *channel = &dsp->channel[voice];
  channel->useNoise = true;
  channel->useGain = true;
  channel->directGain = true;
  channel->gainValue = 0x7f0;
  channel->volumeL = 64;
  channel->volumeR = 64;
  channel->echoEnable = echo;
  dsp_setVoiceBus(dsp, voice, bus);
}

static Dsp *NewDsp(uint8_t *ram) {
  memset(ram, 0, 0x10000);
  Dsp *dsp = dsp_init(ram);
  dsp_reset(dsp);
  dsp->reset = false;
  dsp->mute = false;
  dsp->masterVolumeL = 127;
  dsp->masterVolumeR = 127;
  dsp->noiseSample = 10000;
  return dsp;
}

static void TestUnityIsLabelNeutral(void) {
  uint8_t reference_ram[0x10000], tagged_ram[0x10000];
  Dsp *reference = NewDsp(reference_ram);
  Dsp *tagged = NewDsp(tagged_ram);
  ConfigureVoice(reference, 0, kDspVoiceBus_Unclassified, false);
  ConfigureVoice(reference, 1, kDspVoiceBus_Unclassified, false);
  ConfigureVoice(tagged, 0, kDspVoiceBus_Music, false);
  ConfigureVoice(tagged, 1, kDspVoiceBus_Sfx, false);

  dsp_setMusicBusMuted(false);
  dsp_setBusGains(100, 100);
  dsp_cycle(reference);
  dsp_cycle(tagged);
  CHECK(reference->sampleWrite == 1 && tagged->sampleWrite == 1);
  CHECK(reference->sampleBuffer[0] == tagged->sampleBuffer[0]);
  CHECK(reference->sampleBuffer[1] == tagged->sampleBuffer[1]);
  CHECK(reference->sampleBuffer[0] != 0);

  dsp_free(reference);
  dsp_free(tagged);
}

static void TestIndependentDryGains(void) {
  uint8_t both_ram[0x10000], sfx_ram[0x10000], music_ram[0x10000];
  Dsp *both_muted = NewDsp(both_ram);
  Dsp *sfx_only = NewDsp(sfx_ram);
  Dsp *music_only = NewDsp(music_ram);
  Dsp *mixers[] = { both_muted, sfx_only, music_only };
  for (int i = 0; i < 3; i++) {
    ConfigureVoice(mixers[i], 0, kDspVoiceBus_Music, false);
    ConfigureVoice(mixers[i], 1, kDspVoiceBus_Sfx, false);
  }

  dsp_setBusGains(0, 0);
  dsp_cycle(both_muted);
  CHECK(both_muted->sampleBuffer[0] == 0);

  dsp_setBusGains(0, 100);
  dsp_cycle(sfx_only);
  CHECK(sfx_only->sampleBuffer[0] != 0);

  dsp_setBusGains(100, 0);
  dsp_cycle(music_only);
  CHECK(music_only->sampleBuffer[0] == sfx_only->sampleBuffer[0]);

  for (int i = 0; i < 3; i++) dsp_free(mixers[i]);
}

static void TestEchoSendFollowsBus(void) {
  uint8_t muted_ram[0x10000], live_ram[0x10000];
  Dsp *muted = NewDsp(muted_ram);
  Dsp *live = NewDsp(live_ram);
  ConfigureVoice(muted, 0, kDspVoiceBus_Sfx, true);
  ConfigureVoice(live, 0, kDspVoiceBus_Sfx, true);
  muted->echoWrites = live->echoWrites = true;

  dsp_setBusGains(100, 0);
  dsp_cycle(muted);
  CHECK(muted_ram[0] == 0 && muted_ram[1] == 0 &&
        muted_ram[2] == 0 && muted_ram[3] == 0);

  dsp_setBusGains(100, 100);
  dsp_cycle(live);
  CHECK(live_ram[0] != 0 || live_ram[1] != 0 ||
        live_ram[2] != 0 || live_ram[3] != 0);

  dsp_free(muted);
  dsp_free(live);
}

int main(void) {
  TestUnityIsLabelNeutral();
  TestIndependentDryGains();
  TestEchoSendFollowsBus();
  if (s_failures) {
    fprintf(stderr, "dsp_bus_mix_test: %d failure(s)\n", s_failures);
    return 1;
  }
  puts("dsp_bus_mix_test: PASS");
  return 0;
}
