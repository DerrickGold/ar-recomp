#ifndef AR_AUDIO_SESSION_H
#define AR_AUDIO_SESSION_H
/* AudioSession: the running game's native audio, replacement music and output
 * device. Owns startup, host-pause edges, live volume/settings changes and
 * shutdown ordering. Install before the SFX observer; start output only after
 * replay has bound cold-boot state; shut down before reading audio reports. */

#include <stdbool.h>

struct SettingDesc;

void AudioSession_Install(void);
void AudioSession_StartOutput(void);
void AudioSession_AfterTicks(void);
void AudioSession_SetPaused(bool paused);
void AudioSession_ApplySetting(const struct SettingDesc *desc);
void AudioSession_Shutdown(void);

#endif /* AR_AUDIO_SESSION_H */
