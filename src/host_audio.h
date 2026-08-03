#ifndef HOST_AUDIO_H
#define HOST_AUDIO_H

#include <stdbool.h>

/* Initialize host-audio synchronization and open the SDL stream. `enabled`
 * controls whether the completed mix is audible; rendering continues silently
 * while disabled so authentic and host-streamed audio timelines stay aligned.
 * The requested frequency and buffer size are restart-class settings and
 * remain fixed until HostAudio_Shutdown. Device-open failure is non-fatal and
 * is reported to stderr; false means synchronization could not be created. */
bool HostAudio_Init(int requested_frequency_hz, int requested_buffer_frames,
                    int master_volume_percent, bool enabled);

void HostAudio_Shutdown(void);
/* Mute/unmute the final mixed output without pausing any playback cursor. */
void HostAudio_SetEnabled(bool enabled);
/* Host-owned pause (P, inspector pause, or settings overlay). This gates the
 * SDL stream device itself, so authentic SPC music, SFX, replacement music,
 * and their playback cursors all stop together. It is independent of the
 * user's output-mute setting; rendering resumes when the host pause clears,
 * either audibly or silently according to that setting. */
void HostAudio_SetHostPaused(bool paused);
void HostAudio_SetMasterVolumePercent(int master_volume_percent);

/* Atomically drain the number of callback chunks rejected by SDL since the
 * previous call. Reporting stays on the main thread to avoid audio underruns. */
int HostAudio_TakeRejectedChunkCount(void);

/* Runtime APU synchronization seam used by the recompiled game and audio-side
 * replacement systems. These remain no-ops until HostAudio_Init succeeds. */
void RtlApuLock(void);
void RtlApuUnlock(void);

#endif /* HOST_AUDIO_H */
