#ifndef HOST_DISPLAY_H
#define HOST_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

typedef enum HostDisplayPresentMode {
  kHostDisplayPresent_None,
  kHostDisplayPresent_GameTick,
  /* A visual-regression run is still headless for input, timing, and save
   * policy, but owns a hidden real GPU window. Submit every emulated tick
   * without the host throttle so automation exercises the compositor as fast
   * as the platform's swapchain permits. */
  kHostDisplayPresent_HeadlessVideo,
  kHostDisplayPresent_Paused,
  kHostDisplayPresent_Menu,
} HostDisplayPresentMode;

/* Resolve whether an emulated tick is discarded, presented interactively, or
 * sent through the host-unpaced hidden compositor. Keeping this policy explicit
 * prevents AR_HEADLESS_VIDEO from allocating a renderer that the frame loop
 * then silently bypasses. */
static inline HostDisplayPresentMode HostDisplay_EmulatedFramePresentMode(
    bool headless, bool headless_video) {
  if (!headless) return kHostDisplayPresent_GameTick;
  return headless_video ? kHostDisplayPresent_HeadlessVideo
                        : kHostDisplayPresent_None;
}

/* Row capacity of every host-side ARGB frame surface. 240 covered the authentic
 * 224 lines plus the 239-line overscan mode; it now also has to cover the
 * diorama's vertical margin bands, so it tracks the PPU's own render-target
 * height (224 + 2*kPpuExtraTopBottom = 352). Authentic scanline 0 sits at row
 * PpuVerticalOrigin(ppu) -- NOT row 0 -- whenever a top margin is live, exactly
 * as texture column 0 means screen x = -ws_extra on the horizontal axis. */
enum { kHostDisplayFramebufferHeight = 352 };

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
void HostDisplay_DisableVsync(void);
bool HostDisplay_WindowPointToOutput(int window_x, int window_y,
                                    int *output_x, int *output_y);

uint64_t HostDisplay_CatchupCapNs(int maximum_catchup_frames);

void HostDisplay_InvalidatePresentHistory(void);
bool HostDisplay_SubmitFrame(HostDisplayPresentMode mode, float alpha);
/* Recompose the retained frame between emulation ticks for visual
 * interpolation, or continuously when Refresh rate is explicitly Uncapped so
 * the FPS counter can measure renderer throughput. */
bool HostDisplay_TryRepresentFrame(float alpha,
                                   bool diorama_frame_active,
                                   bool interpolation_enabled,
                                   bool redraw_pending);
/* Rolling completed SDL_RenderPresent calls per second. */
double HostDisplay_FramesPerSecond(void);

/* Enforce the render-loop invariant that every iteration presents or yields,
 * while recording any produced frame that somehow did neither. */
void HostDisplay_YieldIfNoPresent(bool presented,
                                  bool window_hidden,
                                  bool produced_frame);

#endif /* HOST_DISPLAY_H */
