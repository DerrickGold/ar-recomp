#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Config g_config = {
  .new_renderer = true,
  .window_scale = 3,
  .enable_audio = true,
  .output_method = kOutputMethod_SDL,
  .audio_freq = 44100,
  .audio_channels = 2,
  .audio_samples = 2048,
};

void ParseConfigFile(const char *path) {
  if (!path) return;
  FILE *f = fopen(path, "r");
  if (!f) return;

  char line[256];
  while (fgets(line, sizeof(line), f)) {
    char key[64], val[64];
    if (sscanf(line, " %63[^= ] = %63s", key, val) != 2) continue;
    if (strcmp(key, "WindowScale") == 0) g_config.window_scale = (uint8)atoi(val);
    else if (strcmp(key, "Fullscreen") == 0) g_config.fullscreen = (uint8)atoi(val);
    else if (strcmp(key, "NewRenderer") == 0) g_config.new_renderer = atoi(val) != 0;
    else if (strcmp(key, "NoSpriteLimits") == 0) g_config.no_sprite_limits = atoi(val) != 0;
    else if (strcmp(key, "AudioFreq") == 0) g_config.audio_freq = (uint16)atoi(val);
    else if (strcmp(key, "AudioChannels") == 0) g_config.audio_channels = (uint8)atoi(val);
    else if (strcmp(key, "AudioSamples") == 0) g_config.audio_samples = (uint16)atoi(val);
    else if (strcmp(key, "EnableAudio") == 0) g_config.enable_audio = atoi(val) != 0;
    else if (strcmp(key, "LinearFiltering") == 0) g_config.linear_filtering = atoi(val) != 0;
  }
  fclose(f);
}
