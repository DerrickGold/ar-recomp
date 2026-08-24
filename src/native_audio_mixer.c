#include "native_audio_mixer.h"

#include <stdio.h>
#include <stdlib.h>

#include "common_rtl.h"
#include "host/host_audio.h"
#include "music_replacements.h"
#include "settings.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/spc.h"

enum {
  kActRaiserCurrentTrackMaskAddress = 0x47,
  kActRaiserEffectOwnershipMaskAddress = 0x1a,
  kDspVoiceCount = 8,
  kDspPerVoiceWritableRegisterCount = 8,
};

static void (*s_previous_dsp_write_hook)(Apu *, uint8_t, uint8_t);
static void (*s_previous_state_loaded_hook)(Apu *);
static bool s_bus_log;

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

static void ClassifyDspWrite(Apu *apu, uint8_t addr, uint8_t value) {
  if (s_previous_dsp_write_hook)
    s_previous_dsp_write_hook(apu, addr, value);
  (void)value;
  if (!apu || !apu->dsp) return;

  int voice = -1;
  NativeAudioVoiceClass voice_class = kNativeAudioVoice_Unclassified;
  if (!NativeAudioMixer_ClassifyDspWrite(
          addr, apu->spc ? apu->spc->x : 0xff,
          apu->ram[kActRaiserCurrentTrackMaskAddress],
          apu->ram[kActRaiserEffectOwnershipMaskAddress],
          &voice, &voice_class))
    return;

  const DspVoiceBus bus = voice_class == kNativeAudioVoice_Sfx
      ? kDspVoiceBus_Sfx : kDspVoiceBus_Music;
  if (s_bus_log && dsp_getVoiceBus(apu->dsp, voice) != bus) {
    fprintf(stderr,
            "[audio-bus] voice=%d -> %s x=%02x mask=%02x owner=%02x "
            "dsp=%02x\n",
            voice, bus == kDspVoiceBus_Sfx ? "SFX" : "MUSIC",
            apu->spc ? apu->spc->x : 0xff,
            apu->ram[kActRaiserCurrentTrackMaskAddress],
            apu->ram[kActRaiserEffectOwnershipMaskAddress], addr);
  }
  dsp_setVoiceBus(apu->dsp, voice, bus);
}

static void RestoreVoiceClasses(Apu *apu) {
  if (s_previous_state_loaded_hook)
    s_previous_state_loaded_hook(apu);
  if (!apu || !apu->dsp) return;

  /* Voices 0-5 are always song tracks. Voices 6/7 are effects exactly while
   * their driver ownership bits are set; subsequent per-track DSP writes keep
   * the labels precise as playback advances after the load. */
  const uint8_t ownership = apu->ram[kActRaiserEffectOwnershipMaskAddress];
  for (int voice = 0; voice < kDspVoiceCount; voice++) {
    const uint8_t mask = (uint8_t)(1u << voice);
    dsp_setVoiceBus(apu->dsp, voice,
                    (voice >= 6 && (ownership & mask))
                        ? kDspVoiceBus_Sfx
                        : kDspVoiceBus_Music);
  }
}

void NativeAudioMixer_ApplySettings(void) {
  RtlApuLock();
  dsp_setBusGains(g_settings.audio_music_volume,
                  g_settings.audio_sfx_volume);
  MusicReplacements_SetMusicVolumePercent(
      g_settings.audio_music_volume);
  RtlApuUnlock();
  if (s_bus_log)
    fprintf(stderr, "[audio-bus] gains music=%d%% sfx=%d%%\n",
            g_settings.audio_music_volume, g_settings.audio_sfx_volume);
}

void NativeAudioMixer_Install(void) {
  const char *log = getenv("AR_AUDIO_BUSLOG");
  s_bus_log = log && log[0] && log[0] != '0';
  s_previous_dsp_write_hook = g_apu_spc_dsp_write_hook;
  g_apu_spc_dsp_write_hook = ClassifyDspWrite;
  s_previous_state_loaded_hook = g_rtl_apu_state_loaded_hook;
  g_rtl_apu_state_loaded_hook = RestoreVoiceClasses;
  NativeAudioMixer_ApplySettings();
}
