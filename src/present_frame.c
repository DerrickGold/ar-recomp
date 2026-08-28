/* One orchestration point for every rendered frame. Keeping this separate from
 * the large mode compositors makes the scene -> resolve -> host-UI order both
 * structural and independently testable with stubbed stages. */
#include "present.h"

#include "constants.h"
#include "crt_post.h"
#include "present_internal.h"
#include "render_comparison.h"
#include "session_fatal.h"
#include "settings.h"

extern SDL_Renderer *g_renderer;
extern ArRenderDevice g_render_device;

static CrtPostConfig CurrentCrtConfig(void) {
  const float scale = (float)kPercentScale;
  return (CrtPostConfig){
    .enabled = g_settings.crt_enabled,
    .curvature = (float)g_settings.crt_curvature_x100 / scale,
    .scanline_depth = (float)g_settings.crt_scanline_x100 / scale,
    .mask_strength = (float)g_settings.crt_mask_x100 / scale,
    .aberration = (float)g_settings.crt_aberration_x100 / scale,
    .bandwidth = (float)g_settings.crt_bandwidth_x100 / scale,
    .vignette = (float)g_settings.crt_vignette_x100 / scale,
    .brightness = (float)g_settings.crt_brightness_x100 / scale,
  };
}

static bool BeginCrtPost(void) {
  const CrtPostConfig config = CurrentCrtConfig();
  return CrtPost_Begin(&g_render_device, &config);
}

static SDL_Rect EndCrtPost(int scan_columns, int scan_lines,
                           SDL_Rect image) {
  ArRenderRectI resolved = CrtPost_End(
      &g_render_device, scan_columns, scan_lines,
      (ArRenderRectI){image.x, image.y, image.w, image.h});
  return (SDL_Rect){resolved.x, resolved.y, resolved.w, resolved.h};
}

static bool AuthenticFrameSynchronized(const FrameSlot *slot) {
  return slot->authentic_frame_serial != 0 &&
      PresentAuthenticUploadedFrameSerial() == slot->authentic_frame_serial;
}

static void RequestComparisonDrawFailure(const char *stage) {
  const char *sdl_error = SDL_GetError();
  SessionFatal_Request(
      "Authentic comparison failed while drawing its %s (%s). Restart the "
      "game; if this repeats, update your graphics driver or select a "
      "different SDL renderer.",
      stage, sdl_error[0] ? sdl_error : "renderer rejected the draw");
}

SDL_Rect PresentFrame(const FrameSlot *slot, float alpha,
                      double presentation_fps) {
  SDL_Rect image = {0};
  if (!slot || !g_renderer) return image;

  SDL_Point output_size = {0};
  const RenderComparisonView view = RenderComparison_PresentView();
  if (view != kRenderComparison_Enhanced &&
      !AuthenticFrameSynchronized(slot)) {
    SessionFatal_Request(
        "Authentic comparison tried to present a frame that was not current "
        "for this geometry. Restart the game; if this repeats, report the "
        "current room and graphics settings.");
    return image;
  }
  if (view == kRenderComparison_Authentic) {
    /* Authentic comparison fixes both geometry choices to the native signal:
     * 256x224 content with the SNES 4:3 pixel aspect. User window size and
     * independent output treatments and non-visual host UI remain intact. */
    image = ComputePresentationViewportWithOutput(
        g_renderer, false, kPixelAspect_Crt43,
        kFrameSlotAuthenticWidth, kFrameSlotAuthenticHeight, &output_size);
    (void)BeginCrtPost();
    if (SessionFatal_Requested()) return image;
    if (!PresentAuthenticScene(slot, image)) {
      RequestComparisonDrawFailure("native view");
      (void)EndCrtPost(
          kFrameSlotAuthenticWidth, kFrameSlotAuthenticHeight, image);
      return image;
    }
    image = EndCrtPost(
        kFrameSlotAuthenticWidth, kFrameSlotAuthenticHeight, image);
  } else {
    image = ComputePresentationViewportWithOutput(
        g_renderer, slot->ignore_aspect_ratio,
        slot->pixel_aspect, slot->visible_width, slot->snes_height,
        &output_size);
    (void)BeginCrtPost();
    if (SessionFatal_Requested()) return image;
    PresentCompositeScene(slot, alpha);
    if (SessionFatal_Requested()) {
      (void)EndCrtPost(
          slot->visible_width, slot->snes_height, image);
      return image;
    }
    if (view == kRenderComparison_SideBySide &&
        !PresentAuthenticPictureInPicture(slot, image)) {
      RequestComparisonDrawFailure("picture-in-picture view");
      (void)EndCrtPost(
          slot->visible_width, slot->snes_height, image);
      return image;
    }
    image = EndCrtPost(
        slot->visible_width, slot->snes_height, image);
  }
  if (SessionFatal_Requested()) return image;
  const uint8_t fade = RenderComparison_TransitionFadeAlpha();
  if (fade && !PresentComparisonTransitionOverlay(
                  fade, RenderComparison_ViewName(
                            RenderComparison_TransitionTargetView()))) {
    RequestComparisonDrawFailure("transition overlay");
    return image;
  }
  PresentHostUi(slot, image, output_size, presentation_fps);
  return image;
}
