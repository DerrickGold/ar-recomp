#ifndef HOST_DISPLAY_H
#define HOST_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

typedef enum HostDisplayPresentMode {
  kHostDisplayPresent_GameTick,
  kHostDisplayPresent_Paused,
  kHostDisplayPresent_Menu,
} HostDisplayPresentMode;

/* Row capacity of every host-side ARGB frame surface. 240 covered the authentic
 * 224 lines plus the 239-line overscan mode; it now also has to cover the
 * diorama's vertical margin bands, so it tracks the PPU's own render-target
 * height (224 + 2*kPpuExtraTopBottom = 288). Authentic scanline 0 sits at row
 * PpuVerticalOrigin(ppu) -- NOT row 0 -- whenever a top margin is live, exactly
 * as texture column 0 means screen x = -ws_extra on the horizontal axis. */
enum { kHostDisplayFramebufferHeight = 288 };

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
bool HostDisplay_WindowPointToOutput(int window_x, int window_y,
                                    int *output_x, int *output_y);

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
