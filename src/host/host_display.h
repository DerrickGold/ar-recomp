#ifndef AR_HOST_DISPLAY_H
#define AR_HOST_DISPLAY_H
/* HostDisplay: the display side of the host frame loop. Decides whether each
 * emulated tick is dropped, presented or sent to the headless compositor,
 * sizes the host frame surfaces, and presents at the display's cadence.
 * Phase: host (main thread). */

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

/* The active frame geometry, resolved by HostDisplay_ResolveVideoGeometry:
 * the rendered width (the authentic 256 columns plus both widescreen margins)
 * and the authentic height (vertical margin rows are counted separately, in
 * g_ws_extra_top/bottom). Every frame surface and texture uses only this
 * leading extent. */
extern int g_snes_width, g_snes_height;
/* The pixel-aspect mode in force (a PixelAspect value), latched from
 * g_settings.pixel_aspect when the geometry is resolved. */
extern int g_active_pixel_aspect;

/* Mean NTSC field period, including the alternate field's four-cycle short line. */
extern const uint64_t kHostDisplayEmulationFrameIntervalNs;

void HostDisplay_SetWidescreenRuntimeAllowed(bool allowed);
void HostDisplay_ResolveVideoGeometry(bool apply_runtime_changes);
void HostDisplay_CalculateWindowSize(int scale, int *width, int *height);
void HostDisplay_RecomputeLogicalPresentation(void);
void HostDisplay_ApplyWindowScale(void);

void HostDisplay_ApplyWindowMode(void);
void HostDisplay_UpdateProperties(void);
void HostDisplay_WindowDisplayChanged(void);
void HostDisplay_WindowDisplayScaleChanged(void);
void HostDisplay_DisplayModeChanged(uint32_t display_id);
void HostDisplay_DisplayRemoved(uint32_t display_id);
void HostDisplay_ApplyRefreshVsync(void);
void HostDisplay_DisableVsync(void);
/* A visibility/focus transition may restore blocking renderer VSync after the
 * completion-rate guard selected software pacing while the window was not
 * being presented normally. Re-evaluate it from a clean observation window. */
void HostDisplay_ResetVsyncPacing(void);
bool HostDisplay_WindowPointToOutput(int window_x, int window_y,
                                    int *output_x, int *output_y);

uint64_t HostDisplay_CatchupCapNs(uint64_t emulation_frame_interval_ns,
                                  int maximum_catchup_frames);

void HostDisplay_InvalidatePresentHistory(void);
struct SimFrameData;
bool HostDisplay_SubmitFrame(HostDisplayPresentMode mode, float alpha,
                             const struct SimFrameData *annotated_sim);
struct FrameSlot;
bool HostDisplay_StageOwnedFrame(const struct FrameSlot *frame);
bool HostDisplay_CanPresentDuringProduction(void);
/* Software-paced producer: test deadlines before drawing rather
 * than sleeping inside present, so the loop can service a completed producer.
 * VSync retains its normal blocking policy; this never changes a setting. */
void HostDisplay_SetProducerPacing(bool enabled);
/* Recompose the retained frame between emulation ticks at the selected host
 * cadence. Visual interpolation is optional: with it disabled, the exact
 * retained tick is presented while host-owned camera/effect time can advance. */
bool HostDisplay_TryRepresentFrame(float alpha,
                                   bool diorama_frame_active,
                                   bool interpolation_enabled,
                                   bool redraw_pending);
/* Opt-in presenter-thread diagnostics. CPU wall times, not GPU timestamps or
 * physical display times. Cleared for each TryRepresentFrame attempt. swap_ns
 * includes any scheduled submit wait; submit_start_ns separates backend work. */
typedef struct HostDisplayPresentTrace {
  uint64_t deadline_ns, draw_start_ns, draw_ns, swap_ns, vector_wait_ns;
  uint64_t draw_cpu_ns;
  uint64_t submit_deadline_ns, submit_start_ns, submit_wait_ns;
  uint64_t backend_flush_ns, backend_acquire_ns, backend_submit_ns;
} HostDisplayPresentTrace;
void HostDisplay_EnablePresentTrace(bool enabled);
HostDisplayPresentTrace HostDisplay_LastPresentTrace(void);
/* Absolute next presentation sample deadline for the independent producer.
 * Ordinarily this is the draw deadline; the early-draw experiment retains it
 * as the output deadline. Zero means that clock is not active. */
uint64_t HostDisplay_NextPresentationDeadline(void);
/* Diagnostic early preparation samples the intended output time, preserving
 * interpolation phase when drawing ahead of that time. Otherwise returns now. */
uint64_t HostDisplay_PresentationSampleTime(uint64_t now_ns);
/* Native content timestamp, not a CPU deadline or measured scanout time.
 * Vsync advances it by the precise refresh period per successful present;
 * ordinary queue recovery may leave it slightly before now. */
uint64_t HostDisplay_NativeFrameSampleTime(uint64_t now_ns);
/* 0 immediate, 1 renderer refresh, 2 software deadline, 3 VSync probe. */
int HostDisplay_PacingSource(void);
/* Rolling completed backend presents per second. */
double HostDisplay_FramesPerSecond(void);

/* Enforce the render-loop invariant that every iteration presents or yields,
 * while recording any produced frame that somehow did neither. */
void HostDisplay_YieldIfNoPresent(bool presented,
                                  bool window_hidden,
                                  bool produced_frame);

/* Apply live settings here, beside the subsystem they configure. */
struct SettingDesc;
void HostDisplay_ApplySetting(const struct SettingDesc *desc);

#endif /* AR_HOST_DISPLAY_H */
