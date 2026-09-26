#ifndef AR_CONFIG_H
#define AR_CONFIG_H

/* ParseConfigFile reads the legacy config.ini and stages application values
 * into the Settings registry (Settings_StageConfigValue / *Environment). It
 * keeps no state of its own: runtime video/audio code reads g_settings, so the
 * old g_config mirror struct was removed — nothing outside this file ever read
 * it. See docs/manual.md for the parsed-but-inert compatibility keys. */
void ParseConfigFile(const char *path);

#endif  /* AR_CONFIG_H */
