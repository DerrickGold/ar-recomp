#include "config.h"
#include "settings.h"
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
  .aspect_par_43 = true,
};

void ParseConfigFile(const char *path) {
  if (!path) return;
  FILE *f = fopen(path, "r");
  if (!f) return;

  char line[256];
  char section[64] = {0};
  while (fgets(line, sizeof(line), f)) {
    char parsed_section[64];
    if (sscanf(line, " [%63[^]]]", parsed_section) == 1) {
      memcpy(section, parsed_section, strlen(parsed_section) + 1);
      continue;
    }
    char key[64], val[64];
    if (sscanf(line, " %63[^= ] = %63s", key, val) != 2) continue;
    bool application_section = !section[0] || !strcmp(section, "General") ||
        !strcmp(section, "Graphics") || !strcmp(section, "Sound");
    if (application_section && strcmp(key, "WindowScale") == 0) {
      g_config.window_scale = (uint8)atoi(val);
      Settings_StageConfigValue("window_scale", val);
    } else if (application_section && strcmp(key, "Fullscreen") == 0) {
      g_config.fullscreen = (uint8)atoi(val);
      Settings_StageConfigValue("fullscreen", val);
    } else if (application_section && strcmp(key, "NewRenderer") == 0) {
      g_config.new_renderer = atoi(val) != 0;
      Settings_StageConfigValue("new_renderer", val);
    } else if (application_section && strcmp(key, "NoSpriteLimits") == 0) {
      g_config.no_sprite_limits = atoi(val) != 0;
    } else if (application_section && strcmp(key, "AudioFreq") == 0) {
      g_config.audio_freq = (uint16)atoi(val);
      Settings_StageConfigValue("audio_frequency", val);
    } else if (application_section && strcmp(key, "AudioChannels") == 0) {
      g_config.audio_channels = (uint8)atoi(val);
    } else if (application_section && strcmp(key, "AudioSamples") == 0) {
      g_config.audio_samples = (uint16)atoi(val);
      Settings_StageConfigValue("audio_samples", val);
    } else if (application_section && strcmp(key, "EnableAudio") == 0) {
      g_config.enable_audio = atoi(val) != 0;
      Settings_StageConfigValue("audio_enabled", val);
    } else if (application_section && strcmp(key, "LinearFiltering") == 0) {
      g_config.linear_filtering = atoi(val) != 0;
    } else if (application_section && strcmp(key, "IgnoreAspectRatio") == 0) {
      g_config.ignore_aspect_ratio = atoi(val) != 0;
      Settings_StageConfigValue("ignore_aspect_ratio", val);
    } else if (application_section && strcmp(key, "ExtendedAspectRatio") == 0) {
      /* "16:9", "16:10", ... — anything unparsable (e.g. "off") disables. */
      unsigned ax = 0, ay = 0;
      g_config.extend_aspect_x = g_config.extend_aspect_y = 0;
      if (sscanf(val, "%u:%u", &ax, &ay) == 2 && ax && ay && ax < 256 && ay < 256) {
        g_config.extend_aspect_x = (uint8)ax;
        g_config.extend_aspect_y = (uint8)ay;
      }
      Settings_StageConfigValue("extended_aspect", val);
    } else if (application_section && strcmp(key, "AspectPAR") == 0) {
      g_config.aspect_par_43 = strcmp(val, "square") != 0;
      Settings_StageConfigValue("pixel_aspect", val);
    }
    /* Registry-backed AR_* values are staged at the config tier so a later
     * settings.ini can override them. Unknown debug AR_* / SNESREF_* keys still
     * use the environment bridge consumed by legacy diagnostics. overwrite=0
     * keeps an explicit command-line environment value authoritative. e.g.:
     *   AR_INF_HP = 1 / AR_NO_KNOCKBACK = 1 / AR_WARP = 0201 / AR_GARBAGE_STACK = 1 */
    else if (strncmp(key, "AR_", 3) == 0 || strncmp(key, "SNESREF_", 8) == 0) {
      if (Settings_StageConfigEnvironment(key, val))
        continue;
#ifndef _WIN32
      setenv(key, val, 0);
#else
      if (!getenv(key)) { char b[160];
        snprintf(b, sizeof b, "%s=%s", key, val); _putenv(b); }
#endif
    }
  }
  fclose(f);
}
