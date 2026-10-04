#include "host/host_ppu_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "action/action_obj_apron.h"
#include "host/host_frame_surfaces.h"
#include "host/host_display.h"
#include "present/display_geometry.h"
#include "render/present_hud.h"
#include "replacements/hd_replacement_host.h"
#include "snesrecomp/game_runtime.h"

enum { kArgbBytesPerPixel = 4 };

static bool s_authentic_capture_enabled;
static bool s_authentic_surface_bound;
static uint64_t s_authentic_frame_serial;
static uint64_t s_authentic_next_frame_serial;

bool HostPpuOutputControl_Begin(HostPpuOutputControl *control) {
  if (!control || !RtlGameRunner()) return false;
  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  if (!api || api->struct_size < SNES_RUNNER_API_PPU_OUTPUT_CONTROL_SIZE ||
      (api->capabilities & SR_RUNNER_CAP_PPU_OUTPUT_CONTROL) == 0u)
    return false;
  SrGenerationSnapshot generation = {
      .struct_size = sizeof(generation),
  };
  SrRunnerHandle *runner = RtlGameRunner();
  if (api->query_generations(runner, &generation) != SR_RESULT_OK)
    return false;
  control->api = api;
  control->runner = runner;
  control->lifetime_generation = generation.lifetime_generation;
  return true;
}

SrResult HostPpuOutputControl_Bind(
    const HostPpuOutputControl *control, SrPpuOutputKind kind,
    uint32_t source, uint32_t band, uint32_t scale, uint8_t *pixels,
    uint64_t pixel_byte_size, uint64_t pitch_bytes, uint32_t height_pixels,
    uint32_t flags) {
  if (!control) return SR_RESULT_UNAVAILABLE;
  const SrPpuOutputBindingRequest request = {
      .struct_size = sizeof(request),
      .flags = flags,
      .lifetime_generation = control->lifetime_generation,
      .kind = kind,
      .source = source,
      .band = band,
      .scale = scale,
      .pixels = pixels,
      .pixel_byte_size = pixel_byte_size,
      .pitch_bytes = pitch_bytes,
      .height_pixels = height_pixels,
  };
  return control->api->bind_ppu_output_surface(
      control->runner, &request);
}

static SrResult HostPpuOutputControl_SetHorizontalMargin(
    const HostPpuOutputControl *control, SrPpuHorizontalMarginMode mode,
    uint32_t budget_pixels) {
  if (!control) return SR_RESULT_UNAVAILABLE;
  const SrPpuHorizontalMarginRequest request = {
      .struct_size = sizeof(request),
      .lifetime_generation = control->lifetime_generation,
      .mode = mode,
      .budget_pixels = budget_pixels,
  };
  return control->api->configure_ppu_horizontal_margin(
      control->runner, &request);
}

void HostPpuOutput_SetAuthenticEnabled(bool enabled) {
  if (s_authentic_capture_enabled == enabled &&
      s_authentic_surface_bound == enabled)
    return;
  s_authentic_capture_enabled = enabled;
  s_authentic_surface_bound = false;
  s_authentic_frame_serial = 0;
  HostPpuOutputControl output;
  if (!HostPpuOutputControl_Begin(&output)) return;
  const size_t pitch = (size_t)g_snes_width * kArgbBytesPerPixel;
  const SrResult result = HostPpuOutputControl_Bind(
      &output, SR_PPU_OUTPUT_AUTHENTIC, 0u, 0u, 0u,
      enabled ? g_authentic_pixels : NULL,
      enabled ? sizeof(g_authentic_pixels) : 0u,
      enabled ? pitch : 0u,
      enabled ? kHostDisplayFramebufferHeight : 0u, 0u);
  if (result != SR_RESULT_OK) {
    s_authentic_capture_enabled = false;
    (void)HostPpuOutputControl_Bind(
        &output, SR_PPU_OUTPUT_AUTHENTIC, 0u, 0u, 0u,
        NULL, 0u, 0u, 0u, 0u);
    fprintf(stderr,
            "[compare] authentic surface rejected for width %d\n",
            g_snes_width);
  } else {
    s_authentic_surface_bound = enabled;
  }
}

bool HostPpuOutput_AuthenticEnabled(void) {
  return s_authentic_capture_enabled;
}

void HostPpuOutput_AuthenticFrameCompleted(bool frame_valid) {
  if (!frame_valid) {
    s_authentic_frame_serial = 0;
    return;
  }
  if (!s_authentic_capture_enabled || !s_authentic_surface_bound)
    return;
  s_authentic_next_frame_serial++;
  if (!s_authentic_next_frame_serial) s_authentic_next_frame_serial++;
  s_authentic_frame_serial = s_authentic_next_frame_serial;
}

uint64_t HostPpuOutput_AuthenticFrameSerial(void) {
  return s_authentic_frame_serial;
}

void HostPpuOutput_Rebind(void) {
  HostPpuOutputControl output;
  if (!HostPpuOutputControl_Begin(&output)) return;

  /* The old pixels describe the old surface geometry until a complete pass
   * reaches the new binding. */
  s_authentic_frame_serial = 0;

  const size_t pitch = (size_t)g_snes_width * kArgbBytesPerPixel;
  /* The main framebuffer is bound APRON-WIDE: it doubles as the diorama's
   * backdrop plane, and every other diorama plane is apron-wide, so a narrow
   * backdrop would composite offset from the layers by the apron. The
   * compositor centres the scanline span in it (PpuSurfaceApron), so screen
   * x = 0 lands at column apron + ws_extra. Readers of g_pixels therefore
   * offset by kPpuObjApron columns -- see present.c's flat upload. */
  const size_t frame_pitch =
      ActionApron_SurfacePitch(g_snes_width, SR_PPU_OBJ_APRON);
  /* Keep the general renderer available as a deterministic A/B oracle for
   * optimized scanout.  This is intentionally a process-start diagnostic,
   * not a player setting: switching algorithms mid-frame would invalidate
   * comparison captures. */
  const uint32_t render_flags = getenv("AR_PPU_REFERENCE")
      ? SR_PPU_OUTPUT_REFERENCE_PIXEL_RENDERER : 0u;
  (void)HostPpuOutputControl_Bind(
      &output, SR_PPU_OUTPUT_MAIN, 0u, 0u, 0u, g_pixels,
      sizeof(g_pixels), frame_pitch, kHostDisplayFramebufferHeight,
      render_flags);
  /* Geometry may be contracting from a wider prior bind. Clear first so a
   * validation failure cannot leave the old stride attached to new pixels. */
  (void)HostPpuOutputControl_Bind(
      &output, SR_PPU_OUTPUT_AUTHENTIC, 0u, 0u, 0u,
      NULL, 0u, 0u, 0u, 0u);
  s_authentic_surface_bound = false;
  (void)HostPpuOutputControl_Bind(
      &output, SR_PPU_OUTPUT_CLEAR_OVERLAY_SOURCES, 0u, 0u, 0u,
      NULL, 0u, 0u, 0u, 0u);
  const bool hud_background_ready = ArRenderTexture_IsValid(PresentHud_BackgroundTexture());
  const bool hud_object_ready = ArRenderTexture_IsValid(PresentHud_ObjectTexture());
  (void)HostPpuOutputControl_Bind(
      &output, SR_PPU_OUTPUT_OVERLAY, SR_PPU_OVERLAY_BG3, 0u, 0u,
      hud_background_ready ? g_hud_bg_pixels : NULL,
      hud_background_ready ? sizeof(g_hud_bg_pixels) : 0u,
      hud_background_ready ? pitch : 0u,
      hud_background_ready ? kHostDisplayFramebufferHeight : 0u, 0u);
  (void)HostPpuOutputControl_Bind(
      &output, SR_PPU_OUTPUT_OVERLAY, SR_PPU_OVERLAY_OBJ, 0u, 0u,
      hud_object_ready ? g_hud_obj_pixels : NULL,
      hud_object_ready ? sizeof(g_hud_obj_pixels) : 0u,
      hud_object_ready ? pitch : 0u,
      hud_object_ready ? kHostDisplayFramebufferHeight : 0u, 0u);
  /* Observe native church art and dialogue frames. Bind
   * before scanout; frame-policy transactions only claim captures. */
  (void)HostPpuOutputControl_Bind(&output, SR_PPU_OUTPUT_OVERLAY, SR_PPU_OVERLAY_BG2, 0u, 0u,
                                  g_action_bg2_mask_pixels, sizeof(g_action_bg2_mask_pixels), pitch,
                                  kHostDisplayFramebufferHeight, 0u);
  HdReplacementHost_RebindSurfaces(&output);
  if (g_ws_active)
    (void)HostPpuOutputControl_SetHorizontalMargin(
        &output, SR_PPU_HORIZONTAL_MARGIN_CENTERED, (uint32_t)g_ws_extra);
  else
    (void)HostPpuOutputControl_SetHorizontalMargin(
        &output, SR_PPU_HORIZONTAL_MARGIN_AVAILABLE, 0u);
  if (s_authentic_capture_enabled) {
    const SrResult result = HostPpuOutputControl_Bind(
        &output, SR_PPU_OUTPUT_AUTHENTIC, 0u, 0u, 0u,
        g_authentic_pixels, sizeof(g_authentic_pixels), pitch,
        kHostDisplayFramebufferHeight, 0u);
    if (result != SR_RESULT_OK) {
      s_authentic_capture_enabled = false;
      fprintf(stderr,
              "[compare] authentic surface rejected after rebind for width %d\n",
              g_snes_width);
    } else {
      s_authentic_surface_bound = true;
    }
  }
}

void HostPpuOutput_Reset(void) {
  s_authentic_capture_enabled = false;
  s_authentic_surface_bound = false;
  s_authentic_frame_serial = 0;
  s_authentic_next_frame_serial = 0;
}
