#ifndef AR_AUDIO_SESSION_H
#define AR_AUDIO_SESSION_H
/* AudioSession: the running game's native audio, replacement music and output
 * device. Owns startup, host-pause edges, live volume/settings changes and
 * shutdown ordering. Pair Begin/End once per game session. Begin resets pause
 * and diagnostic state; start output only after replay/observers have bound
 * the live runner; end before reading audio reports or destroying the runner. */

#include <stdbool.h>

struct SettingDesc;

void AudioSession_Begin(void);
void AudioSession_StartOutput(void);
void AudioSession_AfterTicks(void);
void AudioSession_SetPaused(bool paused);
void AudioSession_ApplySetting(const struct SettingDesc *desc);
void AudioSession_End(void);

#endif /* AR_AUDIO_SESSION_H */
