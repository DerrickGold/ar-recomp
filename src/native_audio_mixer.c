#include "native_audio_mixer.h"

#include <stdio.h>
#include <stdlib.h>

#include "music_replacements.h"
#include "runner_next.h"
#include "settings.h"

enum {
  kActRaiserCurrentTrackMaskAddress = 0x47,
  kActRaiserEffectOwnershipMaskAddress = 0x1a,
  kDspVoiceCount = 8,
  kDspPerVoiceWritableRegisterCount = 8,
};

static bool s_bus_log;
static SrRunnerHandle *s_runner;

bool NativeAudioMixer_ClassifyDspWrite(
    uint8_t dsp_addr, uint8_t logical_track, uint8_t track_mask,
    uint8_t ownership_mask,
    int *voice, NativeAudioVoiceClass *voice_class) {
  const int register_index = dsp_addr & 0x0f;
  const int physical_voice = dsp_addr >> 4;
  if (dsp_addr >= 0x80 || physical_voice >= kDspVoiceCount ||
      register_index >= kDspPerVoiceWritableRegisterCount)
    return false;

  const uint8_t expected_mask = (uint8_t)(1u << physical_voice);
  if (track_mask != expected_mask)
    return false;

  if (voice) *voice = physical_voice;
  if (voice_class) {
    /* The sequence interpreter keeps logical track X live through its ordinary
     * per-voice DSP writes. Prefer it: captured effect writes from X=$10 can
     * legitimately see $1A clear at this exact instruction. Some shared DSP
     * helpers repurpose X for a register address, so those fall back to the
     * ownership bit proven by the driver's suppression paths. */
    if ((logical_track == 0x10 && physical_voice == 6) ||
        (logical_track == 0x12 && physical_voice == 7)) {
      *voice_class = kNativeAudioVoice_Sfx;
    } else if (logical_track <= 0x0e && !(logical_track & 1) &&
               (logical_track >> 1) == physical_voice) {
      *voice_class = kNativeAudioVoice_Music;
    } else if (ownership_mask & expected_mask) {
      *voice_class = kNativeAudioVoice_Sfx;
    } else {
      /* X has been repurposed and ownership is momentarily clear. The write
       * contains no new provenance: preserve the voice's last proven label.
       * This is observed directly in the effect helper (X=$64 after X=$10). */
      return false;
    }
  }
  return true;
}

void NativeAudioMixer_RouteDspWrite(
    const RtlAudioDspWriteContext *context,
    RtlAudioDspWriteRouting *routing) {
  int voice = -1;
  NativeAudioVoiceClass voice_class = kNativeAudioVoice_Unclassified;
  uint8_t bus;
  int output_voice;
  if (routing == NULL ||
      routing->struct_size < RTL_AUDIO_DSP_WRITE_ROUTING_V2_SIZE)
    return;
  routing->update_count = 0u;
  if (context == NULL ||
      context->struct_size < RTL_AUDIO_DSP_WRITE_CONTEXT_V2_SIZE ||
      context->flags != 0u || context->apu_ram == NULL ||
      context->voice_bus == NULL ||
      context->apu_ram_byte_size <= kActRaiserCurrentTrackMaskAddress ||
      context->apu_ram_byte_size <= kActRaiserEffectOwnershipMaskAddress ||
      context->voice_bus_count == 0u ||
      context->voice_bus_count > RTL_AUDIO_ADAPTER_VOICE_MAX ||
      context->extended_voices_enabled > 1u ||
      (context->extended_voices_enabled == 0u &&
       context->voice_bus_count != kDspVoiceCount) ||
      (context->extended_voices_enabled != 0u &&
       context->voice_bus_count != RTL_AUDIO_ADAPTER_VOICE_MAX))
    return;
  if (!NativeAudioMixer_ClassifyDspWrite(
          context->dsp_address, context->spc_x,
          context->apu_ram[kActRaiserCurrentTrackMaskAddress],
          context->apu_ram[kActRaiserEffectOwnershipMaskAddress],
          &voice, &voice_class))
    return;

  bus = voice_class == kNativeAudioVoice_Sfx
      ? RTL_AUDIO_VOICE_BUS_SFX : RTL_AUDIO_VOICE_BUS_MUSIC;
  output_voice =
      bus == RTL_AUDIO_VOICE_BUS_SFX &&
              context->extended_voices_enabled != 0u && voice >= 6
          ? voice + 2
          : voice;
  if (output_voice < 0 ||
      (uint32_t)output_voice >= context->voice_bus_count)
    return;
  if (s_bus_log && context->voice_bus[output_voice] != bus) {
    fprintf(stderr,
            "[audio-bus] voice=%d -> %s x=%02x mask=%02x owner=%02x "
            "dsp=%02x%s\n",
            output_voice,
            bus == RTL_AUDIO_VOICE_BUS_SFX ? "SFX" : "MUSIC",
            context->spc_x,
            context->apu_ram[kActRaiserCurrentTrackMaskAddress],
            context->apu_ram[kActRaiserEffectOwnershipMaskAddress],
            context->dsp_address,
            output_voice != voice ? " virtual" : "");
  }
  routing->update[0].voice = (uint8_t)output_voice;
  routing->update[0].bus = bus;
  routing->update_count = 1u;
  if (output_voice != voice) {
    routing->update[1].voice = (uint8_t)voice;
    routing->update[1].bus = RTL_AUDIO_VOICE_BUS_MUSIC;
    routing->update_count = 2u;
  }
}

void NativeAudioMixer_RouteStateLoaded(
    const RtlAudioStateLoadedContext *context,
    RtlAudioStateLoadedRouting *routing) {
  uint8_t ownership;
  uint32_t voice;
  if (routing == NULL ||
      routing->struct_size < RTL_AUDIO_STATE_LOADED_ROUTING_V2_SIZE)
    return;
  routing->voice_bus_count = 0u;
  if (context == NULL ||
      context->struct_size < RTL_AUDIO_STATE_LOADED_CONTEXT_V2_SIZE ||
      context->flags != 0u || context->apu_ram == NULL ||
      context->apu_ram_byte_size <= kActRaiserEffectOwnershipMaskAddress ||
      context->voice_bus_count < kDspVoiceCount ||
      context->voice_bus_count > RTL_AUDIO_ADAPTER_VOICE_MAX ||
      context->extended_voices_enabled > 1u ||
      context->reserved8[0] != 0u || context->reserved8[1] != 0u ||
      context->reserved8[2] != 0u ||
      (context->extended_voices_enabled == 0u &&
       context->voice_bus_count != kDspVoiceCount) ||
      (context->extended_voices_enabled != 0u &&
       context->voice_bus_count != RTL_AUDIO_ADAPTER_VOICE_MAX))
    return;
  /* Voices 0-5 are always song tracks. Voices 6/7 are effects exactly while
   * their driver ownership bits are set; subsequent per-track DSP writes keep
   * the labels precise as playback advances after the load. */
  ownership = context->apu_ram[kActRaiserEffectOwnershipMaskAddress];
  for (voice = 0u; voice < kDspVoiceCount; ++voice) {
    const uint8_t mask = (uint8_t)(1u << voice);
    routing->voice_bus[voice] =
        (context->extended_voices_enabled == 0u && voice >= 6u &&
         (ownership & mask) != 0u)
            ? RTL_AUDIO_VOICE_BUS_SFX
            : RTL_AUDIO_VOICE_BUS_MUSIC;
  }
  if (context->extended_voices_enabled != 0u) {
    for (voice = kDspVoiceCount; voice < context->voice_bus_count; ++voice)
      routing->voice_bus[voice] = RTL_AUDIO_VOICE_BUS_SFX;
  }
  routing->voice_bus_count = context->voice_bus_count;
}

void NativeAudioMixer_BindRunner(SrRunnerHandle *runner) {
  s_runner = runner;
  if (runner != NULL) NativeAudioMixer_ApplySettings();
}

void NativeAudioMixer_ApplySettings(void) {
  const SnesRunnerApi *api;
  SrAudioMixControl control = {
      .struct_size = SR_AUDIO_MIX_CONTROL_V2_SIZE,
      .music_gain_percent = (uint32_t)g_settings.audio_music_volume,
      .sfx_gain_percent = (uint32_t)g_settings.audio_sfx_volume,
  };
  if (s_runner != NULL) {
    api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
    if (api != NULL &&
        api->struct_size >= SNES_RUNNER_API_AUDIO_MIX_CONTROL_SIZE &&
        (api->capabilities & SR_RUNNER_CAP_AUDIO_MIX_CONTROL) != 0u &&
        api->configure_audio_mix != NULL)
      (void)api->configure_audio_mix(s_runner, &control);
  }
  MusicReplacements_SetMusicVolumePercent(
      g_settings.audio_music_volume);
  if (s_bus_log)
    fprintf(stderr, "[audio-bus] gains music=%d%% sfx=%d%%\n",
            g_settings.audio_music_volume, g_settings.audio_sfx_volume);
}

void NativeAudioMixer_Install(void) {
  const char *log = getenv("AR_AUDIO_BUSLOG");
  s_bus_log = log && log[0] && log[0] != '0';
  NativeAudioMixer_ApplySettings();
}
