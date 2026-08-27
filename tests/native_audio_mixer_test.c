#include "native_audio_mixer.h"

#include "runner_next.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>

Settings g_settings;

static int s_configure_calls;
static int s_music_gain = -1;
static int s_sfx_gain = -1;
static int s_replacement_music_gain = -1;

static SrResult ConfigureAudioMix(
    SrRunnerHandle *runner, const SrAudioMixControl *control) {
  if (runner == NULL || control == NULL ||
      control->struct_size < SR_AUDIO_MIX_CONTROL_V2_SIZE)
    return SR_RESULT_INVALID_ARGUMENT;
  ++s_configure_calls;
  s_music_gain = (int)control->music_gain_percent;
  s_sfx_gain = (int)control->sfx_gain_percent;
  return SR_RESULT_OK;
}

const SnesRunnerApi *sr_runner_get_api(uint32_t requested_version) {
  static const SnesRunnerApi api = {
      .abi_version = SR_RUNNER_ABI_VERSION,
      .struct_size = sizeof(SnesRunnerApi),
      .capabilities = SR_RUNNER_CAP_AUDIO_MIX_CONTROL,
      .configure_audio_mix = ConfigureAudioMix,
  };
  return requested_version == SR_RUNNER_ABI_VERSION ? &api : NULL;
}

void MusicReplacements_SetMusicVolumePercent(int volume_percent) {
  s_replacement_music_gain = volume_percent;
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

static void TestRouting(void) {
  uint8_t aram[SR_APU_RAM_BYTE_COUNT] = {0};
  uint8_t voice_bus[RTL_AUDIO_ADAPTER_VOICE_MAX] = {0};
  RtlAudioDspWriteContext context = {
      .struct_size = RTL_AUDIO_DSP_WRITE_CONTEXT_V2_SIZE,
      .apu_ram = aram,
      .voice_bus = voice_bus,
      .apu_ram_byte_size = sizeof(aram),
      .voice_bus_count = 8u,
      .spc_x = 0x12u,
      .dsp_address = 0x74u,
  };
  RtlAudioDspWriteRouting routing = {
      .struct_size = RTL_AUDIO_DSP_WRITE_ROUTING_V2_SIZE,
  };
  RtlAudioStateLoadedContext loaded = {
      .struct_size = RTL_AUDIO_STATE_LOADED_CONTEXT_V2_SIZE,
      .apu_ram = aram,
      .apu_ram_byte_size = sizeof(aram),
      .voice_bus_count = 8u,
  };
  RtlAudioStateLoadedRouting loaded_routing = {
      .struct_size = RTL_AUDIO_STATE_LOADED_ROUTING_V2_SIZE,
  };

  aram[0x47u] = 0x80u;
  NativeAudioMixer_RouteDspWrite(&context, &routing);
  CHECK(routing.update_count == 1u && routing.update[0].voice == 7u &&
        routing.update[0].bus == RTL_AUDIO_VOICE_BUS_SFX);

  context.extended_voices_enabled = 1u;
  context.voice_bus_count = RTL_AUDIO_ADAPTER_VOICE_MAX;
  memset(&routing, 0, sizeof(routing));
  routing.struct_size = RTL_AUDIO_DSP_WRITE_ROUTING_V2_SIZE;
  NativeAudioMixer_RouteDspWrite(&context, &routing);
  CHECK(routing.update_count == 2u && routing.update[0].voice == 9u &&
        routing.update[0].bus == RTL_AUDIO_VOICE_BUS_SFX &&
        routing.update[1].voice == 7u &&
        routing.update[1].bus == RTL_AUDIO_VOICE_BUS_MUSIC);

  aram[0x1au] = 0x40u;
  NativeAudioMixer_RouteStateLoaded(&loaded, &loaded_routing);
  CHECK(loaded_routing.voice_bus_count == 8u);
  for (int voice = 0; voice < 6; ++voice)
    CHECK(loaded_routing.voice_bus[voice] == RTL_AUDIO_VOICE_BUS_MUSIC);
  CHECK(loaded_routing.voice_bus[6] == RTL_AUDIO_VOICE_BUS_SFX);
  CHECK(loaded_routing.voice_bus[7] == RTL_AUDIO_VOICE_BUS_MUSIC);

  loaded.extended_voices_enabled = 1u;
  loaded.voice_bus_count = RTL_AUDIO_ADAPTER_VOICE_MAX;
  memset(&loaded_routing, 0, sizeof(loaded_routing));
  loaded_routing.struct_size = RTL_AUDIO_STATE_LOADED_ROUTING_V2_SIZE;
  NativeAudioMixer_RouteStateLoaded(&loaded, &loaded_routing);
  CHECK(loaded_routing.voice_bus_count == RTL_AUDIO_ADAPTER_VOICE_MAX);
  for (int voice = 0; voice < 8; ++voice)
    CHECK(loaded_routing.voice_bus[voice] == RTL_AUDIO_VOICE_BUS_MUSIC);
  for (int voice = 8; voice < (int)RTL_AUDIO_ADAPTER_VOICE_MAX; ++voice)
    CHECK(loaded_routing.voice_bus[voice] == RTL_AUDIO_VOICE_BUS_SFX);

  context.struct_size = sizeof(context.struct_size);
  routing.update_count = 2u;
  NativeAudioMixer_RouteDspWrite(&context, &routing);
  CHECK(routing.update_count == 0u);
}

static void TestInstallAndGains(void) {
  SrRunnerHandle *runner = (SrRunnerHandle *)(uintptr_t)1u;
  g_settings.audio_music_volume = 65;
  g_settings.audio_sfx_volume = 35;
  NativeAudioMixer_Install();
  CHECK(s_configure_calls == 0);
  CHECK(s_replacement_music_gain == 65);

  NativeAudioMixer_BindRunner(runner);
  CHECK(s_configure_calls == 1);
  CHECK(s_music_gain == 65 && s_sfx_gain == 35);
  CHECK(s_replacement_music_gain == 65);
  NativeAudioMixer_BindRunner(NULL);
}

int main(void) {
  TestClassifier();
  TestRouting();
  TestInstallAndGains();
  if (s_failures) {
    fprintf(stderr, "native_audio_mixer_test: %d failure(s)\n", s_failures);
    return 1;
  }
  puts("native_audio_mixer_test: PASS");
  return 0;
}
