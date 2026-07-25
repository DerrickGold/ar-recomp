#ifndef HOST_AUDIO_H
#define HOST_AUDIO_H

#include <stdbool.h>

/* Initialize host-audio synchronization and open the SDL stream when enabled.
 * The requested frequency and buffer size are restart-class settings and
 * remain fixed until HostAudio_Shutdown. Device-open failure is non-fatal and
 * is reported to stderr; false means synchronization could not be created. */
bool HostAudio_Init(int requested_frequency_hz, int requested_buffer_frames,
                    int master_volume_percent, bool enabled);

void HostAudio_Shutdown(void);
void HostAudio_SetEnabled(bool enabled);
void HostAudio_SetMasterVolumePercent(int master_volume_percent);

/* Atomically drain the number of callback chunks rejected by SDL since the
 * previous call. Reporting stays on the main thread to avoid audio underruns. */
int HostAudio_TakeRejectedChunkCount(void);

/* Runtime APU synchronization seam used by the recompiled game and audio-side
 * replacement systems. These remain no-ops until HostAudio_Init succeeds. */
void RtlApuLock(void);
void RtlApuUnlock(void);

#endif /* HOST_AUDIO_H */
