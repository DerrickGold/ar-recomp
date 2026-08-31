#include "host_audio.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "snesrecomp/game/runtime.h"

enum {
  kAudioChannelCount = 2,
  kAudioBytesPerSample = sizeof(int16_t),
  kAudioBytesPerFrame = kAudioChannelCount * kAudioBytesPerSample,
  kAudioCallbackChunkFrameCapacity = 2048,
  kDefaultConfiguredBufferFrames = 2048,
  kMinimumReportedFrequencyHz = 8000,
  kMaximumReportedFrequencyHz = 384000,
  kFullVolumePercent = 100,
  kBufferFramesHintCapacity = 16,
};

static SDL_Mutex *s_audio_mutex;
static SDL_AtomicInt s_output_enabled;
static SDL_AtomicInt s_master_volume_percent;
static SDL_AtomicInt s_rejected_chunk_count;
static SDL_AudioStream *s_audio_stream;
static SDL_ThreadID s_game_thread_id;
static int s_requested_frequency_hz;
static int s_requested_buffer_frames;
static bool s_audio_open;
static bool s_host_paused;

static int ClampVolumePercent(int volume_percent) {
  if (volume_percent < 0) return 0;
  if (volume_percent > kFullVolumePercent) return kFullVolumePercent;
  return volume_percent;
}

void RtlApuLock(void) {
  if (!s_audio_mutex) return;
  if (RtlApuProfileIsEnabled()) {
    if (SDL_TryLockMutex(s_audio_mutex)) return;
    const uint64_t wait_start_ns = SDL_GetTicksNS();
    SDL_LockMutex(s_audio_mutex);
    const uint64_t wait_duration_ns =
        SDL_GetTicksNS() - wait_start_ns;
    if (s_game_thread_id != 0 &&
        SDL_GetCurrentThreadID() == s_game_thread_id) {
      RtlApuProfileRecordHostWait(wait_duration_ns, true);
    } else {
      RtlApuProfileRecordHostWait(wait_duration_ns, false);
    }
    return;
  }
  SDL_LockMutex(s_audio_mutex);
}

void RtlApuUnlock(void) {
  if (s_audio_mutex) SDL_UnlockMutex(s_audio_mutex);
}

/* SDL asks for bytes in the stream's input format: signed 16-bit stereo. */
static void SDLCALL AudioCallback(void *userdata, SDL_AudioStream *stream,
                                  int additional_bytes, int total_bytes) {
  (void)userdata;
  (void)total_bytes;
  if (additional_bytes <= 0 || !s_audio_mutex) return;

  uint8_t audio_chunk[
      kAudioCallbackChunkFrameCapacity * kAudioBytesPerFrame];

  const int volume_percent =
      SDL_GetAtomicInt(&s_master_volume_percent);
  const bool output_enabled = SDL_GetAtomicInt(&s_output_enabled) != 0;
  int remaining_bytes = additional_bytes;
  while (remaining_bytes > 0) {
    const int chunk_bytes =
        remaining_bytes < (int)sizeof(audio_chunk)
            ? remaining_bytes
            : (int)sizeof(audio_chunk);
    RtlRenderAudio((int16_t *)audio_chunk,
                   chunk_bytes / kAudioBytesPerFrame,
                   kAudioChannelCount);
    /* "Enable audio" is an output mute, not a transport pause. Keep rendering
     * while muted so authentic SPC, replacement OGG, and MSU cursors all
     * advance together; silence only the completed mix sent to SDL. */
    if (!output_enabled) {
      memset(audio_chunk, 0, (size_t)chunk_bytes);
    } else if (volume_percent != kFullVolumePercent) {
      int16_t *samples = (int16_t *)audio_chunk;
      const int sample_count = chunk_bytes / kAudioBytesPerSample;
      for (int sample_index = 0; sample_index < sample_count; sample_index++) {
        samples[sample_index] =
            (int16_t)(((int32_t)samples[sample_index] * volume_percent) /
                      kFullVolumePercent);
      }
    }
    if (!SDL_PutAudioStreamData(stream, audio_chunk, chunk_bytes))
      SDL_AddAtomicInt(&s_rejected_chunk_count, 1);
    remaining_bytes -= chunk_bytes;
  }
}

static bool OpenAudioStream(void) {
  if (s_audio_open) return true;

  SDL_AudioSpec requested_spec = {0};
  int device_native_frequency_hz = 0;
  SDL_AudioSpec native_spec = {0};
  if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                               &native_spec, NULL) &&
      native_spec.freq >= kMinimumReportedFrequencyHz &&
      native_spec.freq <= kMaximumReportedFrequencyHz) {
    device_native_frequency_hz = native_spec.freq;
  }

  /* Prefer the physical device rate when the user left the setting on Auto.
   * That keeps SDL's conversion chain to one hop. */
  requested_spec.freq =
      s_requested_frequency_hz > 0
          ? s_requested_frequency_hz
          : device_native_frequency_hz > 0
                ? device_native_frequency_hz
                : kHostAudioMinimumOutputFrequencyHz;
  if (requested_spec.freq < kHostAudioMinimumOutputFrequencyHz) {
    fprintf(stderr,
            "[audio] requested %d Hz is below SDL's %d device minimum; "
            "using %d (the setting cannot lower it)\n",
            requested_spec.freq, kHostAudioMinimumOutputFrequencyHz,
            kHostAudioMinimumOutputFrequencyHz);
    requested_spec.freq = kHostAudioMinimumOutputFrequencyHz;
  }
  requested_spec.format = SDL_AUDIO_S16;
  requested_spec.channels = kAudioChannelCount;

  if (s_requested_buffer_frames > 0) {
    int device_buffer_frames = s_requested_buffer_frames;
    /* The default represents roughly 46 ms at 44.1 kHz. Scale only that
     * default to retain its latency meaning at higher device rates; an
     * explicitly selected frame count remains exact. */
    if (device_buffer_frames == kDefaultConfiguredBufferFrames &&
        device_native_frequency_hz > 0) {
      device_buffer_frames =
          (int)((int64_t)kDefaultConfiguredBufferFrames *
                device_native_frequency_hz /
                    kHostAudioMinimumOutputFrequencyHz);
    }
    char buffer_frames_hint[kBufferFramesHintCapacity];
    snprintf(buffer_frames_hint, sizeof(buffer_frames_hint), "%d",
             device_buffer_frames);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, buffer_frames_hint);
  }

  s_audio_stream = SDL_OpenAudioDeviceStream(
      SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &requested_spec, AudioCallback, NULL);
  if (!s_audio_stream) {
    fprintf(stderr, "SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
    return false;
  }

  /* The callback produces the stream input rate. SDL owns any subsequent
   * conversion to the physical device's native rate. */
  RtlSetAudioOutputRate(requested_spec.freq);

  SDL_AudioSpec device_spec = requested_spec;
  int actual_device_buffer_frames = 0;
  if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(s_audio_stream),
                                &device_spec,
                                &actual_device_buffer_frames)) {
    fprintf(stderr,
            "[audio] SDL_GetAudioDeviceFormat failed: %s "
            "(device rate/buffer diagnostic unavailable)\n",
            SDL_GetError());
  }
  fprintf(stderr,
          "[audio] stream input %d Hz, device %d Hz %d-frame buffer "
          "(requested %d frames)\n",
          requested_spec.freq, device_spec.freq, actual_device_buffer_frames,
          s_requested_buffer_frames);
  s_audio_open = true;
  return true;
}

bool HostAudio_Init(int requested_frequency_hz, int requested_buffer_frames,
                    int master_volume_percent, bool enabled) {
  s_requested_frequency_hz = requested_frequency_hz;
  s_requested_buffer_frames = requested_buffer_frames;
  SDL_SetAtomicInt(&s_output_enabled, 0);
  s_host_paused = false;
  HostAudio_SetMasterVolumePercent(master_volume_percent);

  s_audio_mutex = SDL_CreateMutex();
  if (!s_audio_mutex) {
    fprintf(stderr, "[audio] SDL_CreateMutex failed: %s\n", SDL_GetError());
    return false;
  }
  s_game_thread_id = SDL_GetCurrentThreadID();
  return HostAudio_SetEnabled(enabled);
}

static bool ApplyHostPauseState(void) {
  if (!s_audio_mutex) return false;
  if (!s_host_paused) {
    if (!OpenAudioStream()) return false;
    if (!SDL_ResumeAudioStreamDevice(s_audio_stream)) {
      fprintf(stderr, "[audio] resume failed: %s\n", SDL_GetError());
      return false;
    }
  } else if (s_audio_open &&
             !SDL_PauseAudioStreamDevice(s_audio_stream)) {
    fprintf(stderr, "[audio] pause failed: %s\n", SDL_GetError());
    return false;
  }
  return true;
}

bool HostAudio_SetEnabled(bool enabled) {
  SDL_SetAtomicInt(&s_output_enabled, enabled ? 1 : 0);
  /* Init and a prior open failure reach here without a stream. Once open, a
   * mute toggle is purely atomic and need not issue a redundant device resume. */
  return s_audio_open || ApplyHostPauseState();
}

bool HostAudio_SetHostPaused(bool paused) {
  if (s_host_paused == paused) return true;
  s_host_paused = paused;
  return ApplyHostPauseState();
}

void HostAudio_SetMasterVolumePercent(int master_volume_percent) {
  SDL_SetAtomicInt(&s_master_volume_percent,
                   ClampVolumePercent(master_volume_percent));
}

int HostAudio_TakeRejectedChunkCount(void) {
  return SDL_SetAtomicInt(&s_rejected_chunk_count, 0);
}

void HostAudio_Shutdown(void) {
  if (s_audio_stream) SDL_DestroyAudioStream(s_audio_stream);
  if (s_audio_mutex) SDL_DestroyMutex(s_audio_mutex);
  s_audio_stream = NULL;
  s_audio_mutex = NULL;
  s_game_thread_id = 0;
  s_audio_open = false;
  SDL_SetAtomicInt(&s_output_enabled, 0);
  s_host_paused = false;
}
