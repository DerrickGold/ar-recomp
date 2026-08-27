#include "snesrecomp/host/audio_trace.h"
#include "snes/dsp.h"
#include "snes/dsp_shadow.h"
#include "snes/saveload.h"

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

static void TestExtendedVoiceIsMixedAndIndependentlyControlled(void) {
  uint8_t live_ram[0x10000], muted_ram[0x10000];
  dsp_setExtendedVoicesEnabled(true);
  CHECK(dsp_activeVoiceCount() == kDspMaximumVoiceCount);
  Dsp *live = NewDsp(live_ram);
  Dsp *muted = NewDsp(muted_ram);
  ConfigureVoice(live, 8, kDspVoiceBus_Sfx, true);
  ConfigureVoice(muted, 8, kDspVoiceBus_Sfx, true);
  live->echoWrites = muted->echoWrites = true;

  dsp_setBusGains(100, 100);
  dsp_cycle(live);
  CHECK(live->sampleBuffer[0] != 0);
  CHECK(live_ram[0] != 0 || live_ram[1] != 0 ||
        live_ram[2] != 0 || live_ram[3] != 0);

  dsp_setBusGains(100, 0);
  dsp_cycle(muted);
  CHECK(muted->sampleBuffer[0] == 0);
  CHECK(muted_ram[0] == 0 && muted_ram[1] == 0 &&
        muted_ram[2] == 0 && muted_ram[3] == 0);

  /* A split effect mask updates the unowned native bits only. Voice 6 keeps
   * its song KON latch while virtual voice 8 receives the effect KON. */
  live->channel[6].keyOn = true;
  dsp_writeVirtualVoiceControl(live, 8, 0x4c, true);
  dsp_writeHardwareVoiceMask(live, 0x4c, 0x40, 0xbf);
  CHECK(live->channel[6].keyOn);
  CHECK(live->channel[8].keyOn);

  dsp_free(live);
  dsp_free(muted);
  dsp_setExtendedVoicesEnabled(false);
  CHECK(dsp_activeVoiceCount() == 8);
}

static void TestReleasedVirtualVoiceSleepsAndWakesOnKeyOn(void) {
  uint8_t ram[0x10000];
  dsp_setExtendedVoicesEnabled(true);
  Dsp *dsp = NewDsp(ram);
  ConfigureVoice(dsp, 8, kDspVoiceBus_Sfx, false);
  DspChannel *voice = &dsp->channel[8];
  voice->adsrState = 4;
  voice->gain = 0;
  voice->pitch = 0x1000;
  voice->pitchCounter = 0x0123;

  dsp_cycle(dsp);
  CHECK(voice->pitchCounter == 0x0123);
  CHECK(voice->sampleOut == 0);

  voice->keyOn = true;
  dsp->evenCycle = true;
  dsp_cycle(dsp);
  CHECK(!voice->keyOn);
  CHECK(voice->pitchCounter != 0x0123);
  CHECK(voice->gain == voice->gainValue);
  CHECK(voice->sampleOut != 0);

  dsp_free(dsp);
  dsp_setExtendedVoicesEnabled(false);
}

static void ConfigureParityVoice(Dsp *dsp, uint8_t *ram, int voice) {
  /* One looping BRR block with an asymmetric stereo pan. The echo tail is
   * shared/global while the effect itself has EON clear, matching the static
   * audit of all 38 ActRaiser effect sequences. */
  dsp->dirPage = 0x0200;
  ram[0x0200] = 0x00;
  ram[0x0201] = 0x03;
  ram[0x0202] = 0x00;
  ram[0x0203] = 0x03;
  ram[0x0300] = 0x03; /* loop + end, shift 0, filter 0 */
  const uint8_t brr[8] = {
    0x71, 0xe2, 0x63, 0xd4, 0x55, 0xc6, 0x37, 0xa8,
  };
  memcpy(ram + 0x0301, brr, sizeof(brr));

  DspChannel *channel = &dsp->channel[voice];
  channel->srcn = 0;
  channel->pitch = 0x1000;
  channel->useGain = true;
  channel->directGain = true;
  channel->gainValue = 0x7f0;
  channel->volumeL = 83;
  channel->volumeR = -37;
  channel->echoEnable = false;
  channel->keyOn = true;
  dsp_setVoiceBus(dsp, voice, kDspVoiceBus_Sfx);

  dsp->echoWrites = true;
  dsp->echoVolumeL = 32;
  dsp->echoVolumeR = -24;
  dsp->feedbackVolume = 20;
  dsp->echoBufferAdr = 0x4000;
  dsp->echoDelay = 32;
  dsp->echoRemain = 32;
  dsp->firValues[7] = 64;
  ram[0x4000] = 0x00;
  ram[0x4001] = 0x20;
  ram[0x4002] = 0x00;
  ram[0x4003] = 0xf0;
}

static void TestPhysicalAndVirtualVoicePcmParity(void) {
  uint8_t authentic_ram[0x10000], extended_ram[0x10000];
  dsp_setExtendedVoicesEnabled(false);
  Dsp *authentic = NewDsp(authentic_ram);
  Dsp *extended = NewDsp(extended_ram);
  ConfigureParityVoice(authentic, authentic_ram, 7);
  ConfigureParityVoice(extended, extended_ram, 8);
  dsp_setBusGains(100, 100);

  for (int i = 0; i < 512; i++)
    dsp_cycle(authentic);
  dsp_setExtendedVoicesEnabled(true);
  for (int i = 0; i < 512; i++)
    dsp_cycle(extended);

  CHECK(authentic->sampleWrite == 512 && extended->sampleWrite == 512);
  CHECK(memcmp(authentic->sampleBuffer, extended->sampleBuffer,
               512 * 2 * sizeof(authentic->sampleBuffer[0])) == 0);
  CHECK(memcmp(&authentic->channel[7], &extended->channel[8],
               sizeof(DspChannel)) == 0);
  CHECK(memcmp(authentic_ram + 0x4000, extended_ram + 0x4000,
               512) == 0);
  CHECK(authentic->sampleBuffer[128 * 2] != 0);
  CHECK(authentic->sampleBuffer[128 * 2] !=
        authentic->sampleBuffer[128 * 2 + 1]);

  dsp_free(authentic);
  dsp_free(extended);
  dsp_setExtendedVoicesEnabled(false);
}

typedef struct MemoryState {
  SaveLoadInfo sli;
  uint8_t bytes[sizeof(Dsp)];
  size_t offset;
  bool loading;
} MemoryState;

static void TransferMemoryState(SaveLoadInfo *sli, void *data, size_t size) {
  MemoryState *state = (MemoryState *)sli;
  CHECK(state->offset + size <= sizeof(state->bytes));
  if (state->offset + size > sizeof(state->bytes)) return;
  if (state->loading)
    memcpy(data, state->bytes + state->offset, size);
  else
    memcpy(state->bytes + state->offset, data, size);
  state->offset += size;
}

static void TestExtendedVoiceStateIsSerialized(void) {
  uint8_t ram[0x10000];
  dsp_setExtendedVoicesEnabled(true);
  Dsp *dsp = NewDsp(ram);
  dsp->channel[8].srcn = 0x0a;
  dsp->channel[8].pitch = 0x2345;
  dsp->channel[8].decodeOffset = 0x4567;
  dsp->channel[8].gain = 0x321;
  dsp->channel[8].keyOn = true;

  MemoryState state;
  memset(&state, 0, sizeof(state));
  state.sli.func = TransferMemoryState;
  dsp_saveload(dsp, &state.sli);
  CHECK(state.offset > 0);

  memset(&dsp->channel[8], 0, sizeof(dsp->channel[8]));
  state.offset = 0;
  state.loading = true;
  dsp_saveload(dsp, &state.sli);
  CHECK(dsp->channel[8].srcn == 0x0a);
  CHECK(dsp->channel[8].pitch == 0x2345);
  CHECK(dsp->channel[8].decodeOffset == 0x4567);
  CHECK(dsp->channel[8].gain == 0x321);
  CHECK(dsp->channel[8].keyOn);

  dsp_free(dsp);
  dsp_setExtendedVoicesEnabled(false);
}

int main(void) {
  dsp_setExtendedVoicesEnabled(false);
  TestUnityIsLabelNeutral();
  TestIndependentDryGains();
  TestEchoSendFollowsBus();
  TestExtendedVoiceIsMixedAndIndependentlyControlled();
  TestReleasedVirtualVoiceSleepsAndWakesOnKeyOn();
  TestPhysicalAndVirtualVoicePcmParity();
  TestExtendedVoiceStateIsSerialized();
  if (s_failures) {
    fprintf(stderr, "dsp_bus_mix_test: %d failure(s)\n", s_failures);
    return 1;
  }
  puts("dsp_bus_mix_test: PASS");
  return 0;
}
