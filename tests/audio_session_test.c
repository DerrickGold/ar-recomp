#include "audio/audio_session.h"
#include "audio/audio_presentation_policy.h"
#include "audio/native_audio_extension.h"
#include "audio/native_audio_mixer.h"
#include "app/settings.h"
#include "app/session_fatal.h"
#include "actraiser/actraiser_rtl.h"
#include "host/host_audio.h"
#include "replacements/music_replacements.h"
#include "snesrecomp/game/types.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Settings g_settings;
static char s_order[64];
static size_t s_count;
static bool s_pause_ok = true;
static unsigned s_failures;

static void Record(char step) {
  assert(s_count + 1 < sizeof(s_order));
  s_order[s_count++] = step;
  s_order[s_count] = 0;
}
static void Expect(const char *steps) {
  assert(!strcmp(s_order, steps));
  s_count = 0;
  s_order[0] = 0;
}
int MusicReplacements_Load(const char *path) { Record('L'); return 0; }
void MusicReplacements_InstallHooks(void) { Record('M'); }
void NativeAudioExtension_Install(void) { Record('E'); }
void NativeAudioMixer_Install(void) { Record('X'); }
void AudioPresentationPolicy_Reset(void) { Record('R'); }
int Settings_AudioFrequencyHz(void) { return 48000; }
bool HostAudio_Init(int frequency, int samples, int volume, bool enabled) {
  assert(frequency == 48000 && samples == 512 && volume == 75 && enabled);
  Record('O');
  return true;
}
bool HostAudio_SetHostPaused(bool paused) {
  Record(paused ? 'H' : 'h');
  return s_pause_ok;
}
void MusicReplacements_SetHostPaused(bool paused) { Record(paused ? 'P' : 'p'); }
void HostAudio_Shutdown(void) { Record('S'); }
void MusicReplacements_Shutdown(void) { Record('D'); }
int HostAudio_TakeRejectedChunkCount(void) { Record('C'); return 0; }
void ActRaiser_SpcUploaderCompleteTick(void) { Record('U'); }
void MusicReplacements_FrameTick(void) { Record('T'); }
void HostAudio_SetMasterVolumePercent(int percent) { Record('V'); }
bool HostAudio_SetEnabled(bool enabled) { Record('A'); return true; }
void NativeAudioMixer_ApplySettings(void) { Record('N'); }
void MusicReplacements_ApplySetting(void) { Record('B'); }
void SessionFatal_RequestKind(SessionFailureKind kind, const char *format, ...) {
  assert(kind == kSessionFailure_AudioDevice);
  ++s_failures;
}
void NORETURN Die(const char *message) { abort(); }

int main(void) {
  g_settings.audio_samples = 512;
  g_settings.audio_master_volume = 75;
  g_settings.audio_enabled = true;
  AudioSession_Install();
  Expect("LMEXR");
  AudioSession_StartOutput();
  Expect("O");
  AudioSession_SetPaused(true);
  Expect("HP");
  AudioSession_SetPaused(true);
  Expect("");
  AudioSession_SetPaused(false);
  Expect("ph");
  AudioSession_AfterTicks();
  Expect("CUT");

  SettingDesc changed = {.field = &g_settings.audio_master_volume};
  AudioSession_ApplySetting(&changed);
  Expect("V");
  changed.field = &g_settings.audio_music_volume;
  AudioSession_ApplySetting(&changed);
  Expect("N");
  changed.field = &g_settings.music_replacements;
  AudioSession_ApplySetting(&changed);
  Expect("B");
  changed.field = &g_settings.window_scale;
  AudioSession_ApplySetting(&changed);
  Expect("");

  s_pause_ok = false;
  AudioSession_SetPaused(true);
  Expect("HP");
  assert(s_failures == 1);
  AudioSession_Shutdown();
  Expect("SD");
  puts("audio session: ordered transport edges, live settings and teardown passed");
  return 0;
}
