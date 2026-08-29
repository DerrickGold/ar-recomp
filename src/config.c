#include "config.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
      Settings_StageConfigValue("window_scale", val);
    } else if (application_section && strcmp(key, "Fullscreen") == 0) {
      /* Legacy Fullscreen=1 meant desktop-fullscreen, now window_mode
       * Borderless (1); 0 stays Windowed (0). */
      Settings_StageConfigValue("window_mode", atoi(val) ? "1" : "0");
    } else if (application_section && strcmp(key, "NewRenderer") == 0) {
      /* Removed setting, still parsed so an existing config.ini does not trip
       * over it. The legacy renderer it selected between no longer exists --
       * the PPU has one path -- so there was nothing left to choose. */
    } else if (application_section && strcmp(key, "NoSpriteLimits") == 0) {
      /* Parsed compatibility leftover, no runtime consumer (docs/manual.md). */
    } else if (application_section && strcmp(key, "AudioFreq") == 0) {
      Settings_StageConfigValue("audio_frequency", val);
    } else if (application_section && strcmp(key, "AudioChannels") == 0) {
      /* Parsed compatibility leftover, no runtime consumer (docs/manual.md). */
    } else if (application_section && strcmp(key, "AudioSamples") == 0) {
      Settings_StageConfigValue("audio_samples", val);
    } else if (application_section && strcmp(key, "EnableAudio") == 0) {
      Settings_StageConfigValue("audio_enabled", val);
    } else if (application_section && strcmp(key, "LinearFiltering") == 0) {
      /* Parsed compatibility leftover, no runtime consumer (docs/manual.md). */
    } else if (application_section && strcmp(key, "IgnoreAspectRatio") == 0) {
      Settings_StageConfigValue("ignore_aspect_ratio", val);
    } else if (application_section && strcmp(key, "ExtendedAspectRatio") == 0) {
      /* "16:9", "16:10", ... — anything unparsable (e.g. "off") disables.
       * settings.c owns the parse now; stage the raw value through. */
      Settings_StageConfigValue("extended_aspect", val);
    } else if (application_section && strcmp(key, "AspectPAR") == 0) {
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
