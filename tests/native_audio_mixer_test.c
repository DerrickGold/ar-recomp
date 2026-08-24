#include "native_audio_mixer.h"

#include "settings.h"
#include "snes/apu.h"
#include "snes/dsp.h"

#include <stdio.h>
#include <string.h>

Settings g_settings;

void (*g_apu_spc_dsp_write_hook)(Apu *, uint8_t, uint8_t);
void (*g_rtl_apu_state_loaded_hook)(Apu *);

static int s_lock_depth;
static int s_music_gain = -1;
static int s_sfx_gain = -1;
static int s_replacement_music_gain = -1;
static int s_voice_bus[8];

void RtlApuLock(void) { s_lock_depth++; }
void RtlApuUnlock(void) { s_lock_depth--; }
void dsp_setBusGains(int music_percent, int sfx_percent) {
  s_music_gain = music_percent;
  s_sfx_gain = sfx_percent;
}
void MusicReplacements_SetMusicVolumePercent(int volume_percent) {
  s_replacement_music_gain = volume_percent;
}
void dsp_setVoiceBus(Dsp *dsp, int ch, DspVoiceBus bus) {
  (void)dsp;
  if (ch >= 0 && ch < 8) s_voice_bus[ch] = bus;
}
DspVoiceBus dsp_getVoiceBus(const Dsp *dsp, int ch) {
  (void)dsp;
  return ch >= 0 && ch < 8
      ? (DspVoiceBus)s_voice_bus[ch]
      : kDspVoiceBus_Unclassified;
}

static int s_failures;
#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

static void TestClassifier(void) {
  int voice = -1;
  NativeAudioVoiceClass voice_class = kNativeAudioVoice_Unclassified;

  CHECK(NativeAudioMixer_ClassifyDspWrite(
      0x64, 0x0c, 0x40, 0x00, &voice, &voice_class));
  CHECK(voice == 6 && voice_class == kNativeAudioVoice_Music);

  /* Live trace evidence: effect X=$10 is authoritative even at writes where
   * the ownership byte is temporarily clear. */
  CHECK(NativeAudioMixer_ClassifyDspWrite(
      0x64, 0x10, 0x40, 0x00, &voice, &voice_class));
  CHECK(voice == 6 && voice_class == kNativeAudioVoice_Sfx);

  CHECK(NativeAudioMixer_ClassifyDspWrite(
      0x75, 0x12, 0x80, 0x00, &voice, &voice_class));
  CHECK(voice == 7 && voice_class == kNativeAudioVoice_Sfx);

  /* Shared helpers repurpose X; the ownership byte remains the fallback. */
  CHECK(NativeAudioMixer_ClassifyDspWrite(
      0x64, 0x64, 0x40, 0x40, &voice, &voice_class));
  CHECK(voice == 6 && voice_class == kNativeAudioVoice_Sfx);
  CHECK(!NativeAudioMixer_ClassifyDspWrite(
      0x64, 0x64, 0x40, 0x00, &voice, &voice_class));

  CHECK(!NativeAudioMixer_ClassifyDspWrite(
      0x64, 0x0c, 0x80, 0x00, &voice, &voice_class));
  CHECK(!NativeAudioMixer_ClassifyDspWrite(
      0x4c, 0x08, 0x10, 0x00, &voice, &voice_class)); /* global KON */
  CHECK(!NativeAudioMixer_ClassifyDspWrite(
      0x68, 0x0c, 0x40, 0x00, &voice, &voice_class)); /* read-only ENVX */
}

static void TestInstallAndRestore(void) {
  memset(s_voice_bus, 0, sizeof(s_voice_bus));
  g_settings.audio_music_volume = 65;
  g_settings.audio_sfx_volume = 35;
  NativeAudioMixer_Install();

  CHECK(s_lock_depth == 0);
  CHECK(s_music_gain == 65 && s_sfx_gain == 35);
  CHECK(s_replacement_music_gain == 65);
  CHECK(g_apu_spc_dsp_write_hook != NULL);
  CHECK(g_rtl_apu_state_loaded_hook != NULL);

  Apu apu;
  Spc spc;
  memset(&apu, 0, sizeof(apu));
  memset(&spc, 0, sizeof(spc));
  apu.dsp = (Dsp *)(uintptr_t)1;
  apu.spc = &spc;
  spc.x = 0x12;
  apu.ram[0x47] = 0x80;
  apu.ram[0x1a] = 0x00;
  g_apu_spc_dsp_write_hook(&apu, 0x74, 0x09);
  CHECK(s_voice_bus[7] == kDspVoiceBus_Sfx);

  memset(s_voice_bus, 0, sizeof(s_voice_bus));
  apu.ram[0x1a] = 0x40;
  g_rtl_apu_state_loaded_hook(&apu);
  for (int voice = 0; voice < 6; voice++)
    CHECK(s_voice_bus[voice] == kDspVoiceBus_Music);
  CHECK(s_voice_bus[6] == kDspVoiceBus_Sfx);
  CHECK(s_voice_bus[7] == kDspVoiceBus_Music);
}

int main(void) {
  TestClassifier();
  TestInstallAndRestore();
  if (s_failures) {
    fprintf(stderr, "native_audio_mixer_test: %d failure(s)\n", s_failures);
    return 1;
  }
  puts("native_audio_mixer_test: PASS");
  return 0;
}
