#include "audio/audio_session.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "actraiser/actraiser_rtl.h"
#include "app/session_fatal.h"
#include "app/settings.h"
#include "audio/audio_presentation_policy.h"
#include "audio/native_audio_extension.h"
#include "audio/native_audio_mixer.h"
#include "host/host_audio.h"
#include "replacements/music_replacements.h"
#include "snesrecomp/game/types.h"

static bool s_pause_initialized, s_applied_pause;
static int s_rejected_chunks;

void AudioSession_Begin(void) {
  s_pause_initialized = s_applied_pause = false;
  s_rejected_chunks = 0;
  /* Music uses the manifest's [music:] sections and works headless too. */
  const char *music_manifest = getenv("AR_MUSIC_MANIFEST");
  if (!music_manifest || !music_manifest[0])
    music_manifest = "game-assets/manifest.ini";
  MusicReplacements_Load(music_manifest);
  MusicReplacements_InstallHooks();
  NativeAudioExtension_Install();
  NativeAudioMixer_Install();
  AudioPresentationPolicy_Reset();
}

void AudioSession_StartOutput(void) {
  if (!HostAudio_Init(Settings_AudioFrequencyHz(), g_settings.audio_samples,
                      g_settings.audio_master_volume,
                      g_settings.audio_enabled)) {
    Die("The selected audio output could not be opened. Check the system "
        "output device, then restart the game. You can also change the "
        "audio buffer or sample-rate setting before launching again.");
  }
}

void AudioSession_AfterTicks(void) {
  /* Surface audio-chunk drops the callback counted (R12). Reported here, off
   * the audio thread, and coalesced so a sustained problem cannot spam. */
  {
    int dropped = HostAudio_TakeRejectedChunkCount();
    if (dropped) {
      s_rejected_chunks += dropped;
      fprintf(stderr, "[audio] %d chunk(s) rejected by SDL_PutAudioStreamData "
                      "(%d total this session) — audio glitched\n",
              dropped, s_rejected_chunks);
    }
  }

  /* Complete the SPC engine's resident uploader once it enters the $CC-wait,
   * for the case where the CPU's HLEd $9A56 ran before the engine got there
   * (takes its own APU lock). */
  ActRaiser_SpcUploaderCompleteTick();

  /* Music replacement live policy (setting toggled off mid-song) takes its
   * own APU lock. Neither operation runs under a session-wide lock. */
  MusicReplacements_FrameTick();
}

/* One application-level host-pause edge owns both transport layers. The order
 * matters: stop the device before latching the OGG decoder, then release the
 * decoder before resuming the device, so no callback can advance only one
 * source across the edge. */
void AudioSession_SetPaused(bool paused) {
  if (s_pause_initialized && s_applied_pause == paused) return;
  s_pause_initialized = true;
  s_applied_pause = paused;
  bool success = true;
  if (paused) success = HostAudio_SetHostPaused(true);
  MusicReplacements_SetHostPaused(paused);
  if (!paused) success = HostAudio_SetHostPaused(false);
  if (!success) {
    SessionFatal_RequestKind(kSessionFailure_AudioDevice,
        "audio stream rejected by device: %s",
        SDL_GetError());
  }
}

void AudioSession_End(void) {
  HostAudio_Shutdown();
  MusicReplacements_Shutdown();
}

void AudioSession_ApplySetting(const SettingDesc *desc) {
  if (desc->field == &g_settings.audio_master_volume)
    HostAudio_SetMasterVolumePercent(g_settings.audio_master_volume);
  if (desc->field == &g_settings.audio_music_volume ||
      desc->field == &g_settings.audio_sfx_volume)
    NativeAudioMixer_ApplySettings();
  if (desc->field == &g_settings.audio_enabled)
    (void)HostAudio_SetEnabled(g_settings.audio_enabled);
  if (desc->field == &g_settings.music_replacements)
    MusicReplacements_ApplySetting();
}
