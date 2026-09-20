/* One orchestration point for every rendered frame. Keeping this separate from
 * the large mode compositors makes the scene -> resolve -> host-UI order both
 * structural and independently testable with stubbed stages. */
#include "present.h"

#include "constants.h"
#include "crt_post.h"
#include "present_internal.h"
#include "present_sim_menu.h"
#include "render_comparison.h"
#include "session_fatal.h"
#include "settings.h"
#include "performance_metrics.h"

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
  const PerformanceScope performance = PerformanceMetrics_Begin(kPerformance_PostProcess);
  const bool result = CrtPost_Begin(&g_render_device, &config);
  PerformanceMetrics_End(performance);
  return result;
}

static ArRenderRectI EndCrtPost(int scan_columns, int scan_lines,
                                ArRenderRectI image) {
  const PerformanceScope performance = PerformanceMetrics_Begin(kPerformance_PostProcess);
  const ArRenderRectI result = CrtPost_End(
      &g_render_device, scan_columns, scan_lines, image);
  PerformanceMetrics_End(performance);
  return result;
}

static bool AuthenticFrameSynchronized(const FrameSlot *slot) {
  return slot->authentic_frame_serial != 0 &&
      PresentAuthenticUploadedFrameSerial() == slot->authentic_frame_serial;
}

static void RequestComparisonDrawFailure(const char *stage) {
  const char *render_error = ArRenderDevice_LastError(&g_render_device);
  SessionFatal_Request(
      "Authentic comparison failed while drawing its %s (%s). Restart the "
      "game; if this repeats, update your graphics driver or select a "
      "different renderer.",
      stage, render_error && render_error[0]
          ? render_error : "renderer rejected the draw");
}

static ArRenderRectI DrawFrame(const FrameSlot *slot, float alpha,
                           double presentation_fps) {
  ArRenderRectI image = {0};
  if (!slot || !ArRenderDevice_IsReady(&g_render_device)) return image;

  ArRenderExtentI output_size = {0};
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
        &g_render_device, false, kPixelAspect_Crt43,
        kFrameSlotAuthenticWidth, kFrameSlotAuthenticHeight, &output_size);
    (void)BeginCrtPost();
    if (SessionFatal_Requested()) return image;
    if (!PresentAuthenticScene(slot, image)) {
      RequestComparisonDrawFailure("native view");
      (void)EndCrtPost(
          kFrameSlotAuthenticWidth, kFrameSlotAuthenticHeight, image);
      return image;
    }
    PresentSimMenu_Draw(slot, image);
    image = EndCrtPost(
        kFrameSlotAuthenticWidth, kFrameSlotAuthenticHeight, image);
  } else {
    image = ComputePresentationViewportWithOutput(
        &g_render_device, slot->ignore_aspect_ratio,
        slot->pixel_aspect, slot->visible_width, slot->snes_height,
        &output_size);
    (void)BeginCrtPost();
    if (SessionFatal_Requested()) return image;
    PresentCompositeScene(slot, alpha);
    PresentSimMenu_Draw(slot, image);
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
  const PerformanceScope ui = PerformanceMetrics_Begin(kPerformance_HostUi);
  PresentHostUi(slot, image, output_size, presentation_fps);
  PerformanceMetrics_End(ui);
  return image;
}

ArRenderRectI PresentFrame(const FrameSlot *slot, float alpha,
                           double presentation_fps) {
  /* Navigation and neutral Help replace the preceding native dialogue just
   * as a handoff hides an answered question. Native cleanup can retain its
   * old ticket; only phases that actually present that dialogue must report
   * to its scheduler. Hardware blanking/fades are also intentional. */
  const bool menu_hides_dialogue = slot && slot->sim_menu.valid &&
      (slot->sim_menu.model.phase == kSimMenu_Handoff ||
       slot->sim_menu.model.phase == kSimMenu_Opening ||
       slot->sim_menu.model.phase == kSimMenu_Browse ||
       slot->sim_menu.model.phase == kSimMenu_Inventory ||
       (slot->sim_menu.model.phase == kSimMenu_Describe &&
        slot->sim_menu.help.active));
  const bool visible =
      slot && !(slot->inidisp & 0x80) && (slot->inidisp & 0x0f) &&
      !menu_hides_dialogue;
  ArTextPresentation_BeginFrame(visible ? slot->localization.dialogue_ticket
                                        : 0);
  const ArRenderRectI image = DrawFrame(slot, alpha, presentation_fps);
  ArTextPresentation_EndFrame();
  return image;
}
