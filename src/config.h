#pragma once
#include "types.h"

enum {
  kOutputMethod_SDL,
  kOutputMethod_SDLSoftware,
  kOutputMethod_OpenGL,
};

typedef struct Config {
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
} Config;

extern Config g_config;

void ParseConfigFile(const char *path);
