/* Capture orchestration is isolated so its failure/readback contract can be
 * tested on every platform without linking the inspector or game runtime. */
#include "dev_tools.h"

#include "app/session_fatal.h"
#include "app/settings.h"
#include "host/host_display.h"
#include "present/presentation_frame_generation.h"

enum { kArgbBytesPerPixel = 4 };

/* Write the live framebuffer to an open PPM, cropped to the active display
 * rectangle. When a host readback provider exists, capture the actual
 * composite so an independently scaled HUD is represented exactly. */
DevToolsCaptureResult DevTools_WriteFramebufferPpm(
    FILE *file, const DevToolsContext *context, bool require_composite) {
  return DevTools_WriteFramebufferPpmAtPhase(
      file, context, require_composite, kPresentationFrameGenerationPhaseNone, NULL);
}

DevToolsCaptureResult DevTools_WriteFramebufferPpmAtPhase(
    FILE *file, const DevToolsContext *context, bool require_composite, float phase,
    const FrameSlot *uploaded_frame) {
  if (!file || !context || SessionFatal_Requested())
    return (DevToolsCaptureResult){0};

  FrameSlot frame_slot;
  bool have_composite = false;
  DevToolsRgb24Capture capture = {0};
  if (context->readback.capture_rgb24 &&
      ArRenderTexture_IsValid(context->hud_bg_texture)) {
    if (!uploaded_frame) {
      FrameSlot_Capture(&frame_slot, NULL);
      Diorama_CaptureCameraPresentationState(&frame_slot.diorama_camera.controls);
      if (frame_slot.sim.view == kSimView_Enhanced ||
          frame_slot.sim.view == kSimView_WorldNavigation) {
        Sim3DCameraPresentationState camera;
        Sim3DCamera_CapturePresentationState(&camera);
        frame_slot.sim_camera.mode = camera.mode;
        frame_slot.sim_camera.orbit_yaw = camera.orbit_yaw;
        frame_slot.sim_camera.orbit_pitch = camera.orbit_pitch;
      }
      PresentUpload(&frame_slot);
      if (SessionFatal_Requested()) return (DevToolsCaptureResult){0};
      uploaded_frame = &frame_slot;
    }
    /* The same scene -> CRT resolve -> host-UI function used by the live
     * window keeps F2 captures visually identical, including an open menu. */
    PresentFrame(uploaded_frame, phase,
                 HostDisplay_FramesPerSecond());
    /* A fatal render can unwind with a nonempty viewport. It is neither a
     * composite nor permission to substitute the native framebuffer. */
    if (SessionFatal_Requested()) return (DevToolsCaptureResult){0};
    have_composite = true;
  }
  if (have_composite && context->readback.capture_rgb24(
          context->readback.context, &capture) &&
      capture.pixels && capture.width > 0 && capture.height > 0 &&
      capture.width <= INT32_MAX / 3 &&
      capture.pitch_bytes >= capture.width * 3) {
    const int output_width = capture.width;
    const int output_height = capture.height;
    fprintf(file, "P6\n%d %d\n255\n", output_width, output_height);
    for (int y = 0; y < output_height; y++) {
      const uint8_t *row =
          capture.pixels + (size_t)y * (size_t)capture.pitch_bytes;
      fwrite(row, 3, (size_t)output_width, file);
    }
    if (capture.release) capture.release(capture.owner);
    return ferror(file) ? (DevToolsCaptureResult){0} :
        (DevToolsCaptureResult){output_width, output_height, kDevToolsCapture_Composite};
  }
  if (capture.release) capture.release(capture.owner);
  if (require_composite) return (DevToolsCaptureResult){0};

  const int visible_x = Settings_VisibleX0();
  const int visible_width = Settings_VisibleWidth();
  if (!context->framebuffer_pixels || context->snes_height <= 0 ||
      visible_x < 0 || visible_width <= 0 ||
      (int64_t)(visible_x + (int64_t)visible_width) * 4 > context->framebuffer_pitch)
    return (DevToolsCaptureResult){0};
  fprintf(file, "P6\n%d %d\n255\n", visible_width, context->snes_height);
  for (int y = 0; y < context->snes_height; y++) {
    const uint8_t *row = context->framebuffer_pixels +
        (size_t)y * (size_t)context->framebuffer_pitch +
        (size_t)visible_x * kArgbBytesPerPixel;
    for (int x = 0; x < visible_width; x++) {
      fputc(row[x * kArgbBytesPerPixel + 2], file);
      fputc(row[x * kArgbBytesPerPixel + 1], file);
      fputc(row[x * kArgbBytesPerPixel + 0], file);
    }
  }
  return ferror(file) ? (DevToolsCaptureResult){0} :
      (DevToolsCaptureResult){visible_width, context->snes_height,
                              kDevToolsCapture_NativeFramebuffer};
}
