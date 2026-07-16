#pragma once
#include "types.h"

enum {
  kOutputMethod_SDL,
  kOutputMethod_SDLSoftware,
  kOutputMethod_OpenGL,
};

typedef struct Config {
  /* Legacy parse mirror only. Wired application values are staged into the
   * Settings registry by config.c; runtime video/audio code reads g_settings. */
  bool new_renderer;
  bool ignore_aspect_ratio;
  uint8 fullscreen;
  uint8 window_scale;
  bool enable_audio;
  bool linear_filtering;
  uint8 output_method;
  uint16 audio_freq;
  uint8 audio_channels;
  uint16 audio_samples;
  bool autosave;
  bool no_sprite_limits;
  bool enable_gamepad[2];
  int gamepad_deadzone;
  bool skip_launcher;
  /* Widescreen: target display aspect as a ratio (e.g. 16:9). 0:0 = off
   * (authentic 4:3). The extra rendered columns per side are derived from
   * this at startup — see main.c. */
  uint8 extend_aspect_x, extend_aspect_y;
  /* true = preserve the CRT 4:3 pixel stretch (7:6 PAR) in the widescreen
   * math and present; false = square pixels (crisper, needs wider margins). */
  bool aspect_par_43;
} Config;

extern Config g_config;

void ParseConfigFile(const char *path);
