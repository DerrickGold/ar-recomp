#ifndef HOST_DISPLAY_H
#define HOST_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

typedef enum HostDisplayPresentMode {
  kHostDisplayPresent_GameTick,
  kHostDisplayPresent_Paused,
  kHostDisplayPresent_Menu,
} HostDisplayPresentMode;

enum { kHostDisplayFramebufferHeight = 240 };

/* 262 scanlines * 1364 master-clock dots / 21.477272 MHz = 60.0988 Hz. */
extern const uint64_t kHostDisplayEmulationFrameIntervalNs;

void HostDisplay_SetWidescreenRuntimeAllowed(bool allowed);
void HostDisplay_ResolveVideoGeometry(bool apply_runtime_changes);
void HostDisplay_CalculateWindowSize(int scale, int *width, int *height);
void HostDisplay_RecomputeLogicalPresentation(void);
void HostDisplay_ApplyWindowScale(void);

void HostDisplay_ApplyWindowMode(void);
void HostDisplay_UpdateProperties(void);
void HostDisplay_PollProperties(void);
void HostDisplay_ApplyRefreshVsync(void);

uint64_t HostDisplay_CatchupCapNs(int maximum_catchup_frames);

void HostDisplay_InvalidatePresentHistory(void);
bool HostDisplay_SubmitFrame(HostDisplayPresentMode mode, float alpha);
bool HostDisplay_TryRepresentFrame(float alpha,
                                   bool diorama_frame_active,
                                   bool interpolation_enabled,
                                   bool redraw_pending);

/* Enforce the render-loop invariant that every iteration presents or yields,
 * while recording any produced frame that somehow did neither. */
void HostDisplay_YieldIfNoPresent(bool presented,
                                  bool window_hidden,
                                  bool produced_frame);

#endif /* HOST_DISPLAY_H */
