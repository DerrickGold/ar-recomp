/* The terminal frame order is correctness, not decoration: the CRT resolve
 * paints outside the game image black, so host UI must run afterward. Compile
 * the small orchestrator against stage stubs to keep that order testable on a
 * headless machine with no renderer or GPU. */
#include "present/present.h"
#include "present/present_internal.h"
#include "sim/menu/present_sim_menu.h"
#include "render/crt_post.h"
#include "present/render_comparison.h"
#include "app/session_fatal.h"
#include "app/settings.h"

#include <stdio.h>
#include <string.h>

ArRenderDevice g_render_device;
Settings g_settings;

static int s_failures;
static int s_stage;
static const FrameSlot *s_expected_slot;
static float s_expected_alpha;
static double s_expected_presentation_fps;
static RenderComparisonView s_expected_view;
static ArRenderRectI s_expected_host_viewport;
static uint8_t s_expected_transition_alpha;
static int s_expected_stages;
static uint64_t s_authentic_uploaded_serial;
static bool s_dialogue_ready;
static bool s_fail_scene;
static int s_menu_draws;
static const ArRenderRectI kFallback = {160, 0, 960, 720};
static const ArRenderRectI kResolved = {161, 1, 958, 718};

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

static bool RectsEqual(ArRenderRectI left, ArRenderRectI right) {
  return left.x == right.x && left.y == right.y &&
      left.w == right.w && left.h == right.h;
}

bool ArRenderDevice_IsReady(const ArRenderDevice *device) {
  return device == &g_render_device;
}

const char *ArRenderDevice_LastError(const ArRenderDevice *device) {
  CHECK(device == &g_render_device);
  return "test renderer error";
}

ArRenderRectI ComputePresentationViewportWithOutput(
    ArRenderDevice *device, bool ignore_aspect_ratio,
    int pixel_aspect, int visible_width, int snes_height,
    ArRenderExtentI *output_size) {
  CHECK(s_stage++ == 0);
  CHECK(device == &g_render_device);
  CHECK(!ignore_aspect_ratio);
  if (s_expected_view == kRenderComparison_Authentic) {
    CHECK(pixel_aspect == kPixelAspect_Crt43);
    CHECK(visible_width == kFrameSlotAuthenticWidth);
    CHECK(snes_height == kFrameSlotAuthenticHeight);
  } else {
    CHECK(pixel_aspect == 7);
    CHECK(visible_width == 256);
    CHECK(snes_height == 224);
  }
  CHECK(output_size != NULL);
  *output_size = (ArRenderExtentI){1280, 720};
  return kFallback;
}

bool CrtPost_Begin(ArRenderDevice *device, const CrtPostConfig *config) {
  CHECK(s_stage++ == 1);
  CHECK(device == &g_render_device);
  CHECK(config != NULL);
  CHECK(config->enabled);
  CHECK(config->curvature == 0.25f);
  CHECK(config->scanline_depth == 0.5f);
  CHECK(config->mask_strength == 0.75f);
  CHECK(config->brightness == 1.25f);
  return true;
}

uint64_t PresentAuthenticUploadedFrameSerial(void) {
  return s_authentic_uploaded_serial;
}

void PresentCompositeScene(const FrameSlot *slot, float alpha) {
  CHECK(s_stage++ == 2);
  CHECK(slot == s_expected_slot);
  CHECK(alpha == s_expected_alpha);
  if (s_fail_scene) SessionFatal_Request("injected scene target failure");
  if (s_dialogue_ready)
    ArTextPresentation_MarkReady(slot->localization.dialogue_ticket);
}

bool PresentAuthenticScene(const FrameSlot *slot, ArRenderRectI viewport) {
  CHECK(s_stage++ == 2);
  CHECK(s_expected_view == kRenderComparison_Authentic);
  CHECK(slot == s_expected_slot);
  CHECK(RectsEqual(viewport, kFallback));
  return true;
}

bool PresentBlankScene(void) {
  CHECK(s_stage++ == 2);
  CHECK(s_expected_view == kRenderComparison_Authentic);
  CHECK((s_expected_slot->inidisp & 0x80) || !(s_expected_slot->inidisp & 15));
  return true;
}

bool PresentAuthenticPictureInPicture(const FrameSlot *slot,
                                      ArRenderRectI priority_viewport) {
  CHECK(s_stage++ == 3);
  CHECK(s_expected_view == kRenderComparison_SideBySide);
  CHECK(slot == s_expected_slot);
  CHECK(RectsEqual(priority_viewport, kFallback));
  return true;
}

void PresentSimMenu_Draw(const FrameSlot *slot, ArRenderRectI viewport) {
  s_menu_draws++;
  CHECK(slot == s_expected_slot);
  CHECK(RectsEqual(viewport, kFallback));
}

bool PresentComparisonTransitionOverlay(uint8_t alpha, const char *label) {
  CHECK(s_stage++ == 4);
  CHECK(alpha == s_expected_transition_alpha);
  CHECK(label != NULL);
  CHECK(strcmp(label, RenderComparison_ViewName(s_expected_view)) == 0);
  return true;
}

ArRenderRectI CrtPost_End(ArRenderDevice *device,
                          int scan_columns, int scan_lines,
                          ArRenderRectI image) {
  const int expected_stage =
      s_expected_view == kRenderComparison_SideBySide &&
      !(s_expected_slot->inidisp & 0x80) && (s_expected_slot->inidisp & 15) ? 4 : 3;
  CHECK(s_stage++ == expected_stage);
  CHECK(device == &g_render_device);
  CHECK(scan_columns == 256);
  CHECK(scan_lines == 224);
  CHECK(image.x == kFallback.x && image.y == kFallback.y &&
        image.w == kFallback.w && image.h == kFallback.h);
  return (ArRenderRectI){
    kResolved.x, kResolved.y, kResolved.w, kResolved.h,
  };
}

void PresentHostUi(const FrameSlot *slot, ArRenderRectI viewport,
                   ArRenderExtentI output_size,
                   double presentation_fps) {
  CHECK(s_stage++ == s_expected_stages - 1);
  CHECK(slot == s_expected_slot);
  /* The UI must receive End's authoritative rectangle, not the fallback that
   * was calculated before SDL resolved its per-target logical presentation. */
  CHECK(RectsEqual(viewport, s_expected_host_viewport));
  CHECK(output_size.width == 1280 && output_size.height == 720);
  CHECK(presentation_fps == s_expected_presentation_fps);
}

static void RunCase(FrameSlot *slot) {
  s_stage = 0;
  ArRenderRectI image = PresentFrame(
      slot, s_expected_alpha, s_expected_presentation_fps);
  CHECK(s_stage == s_expected_stages);
  CHECK(RectsEqual(image, s_expected_host_viewport));
}

int main(int argc, char **argv) {
  g_settings.crt_enabled = true;
  g_settings.crt_curvature_x100 = 25;
  g_settings.crt_scanline_x100 = 50;
  g_settings.crt_mask_x100 = 75;
  g_settings.crt_aberration_x100 = 10;
  g_settings.crt_bandwidth_x100 = 20;
  g_settings.crt_vignette_x100 = 30;
  g_settings.crt_brightness_x100 = 125;
  FrameSlot slot = {0};
  slot.ignore_aspect_ratio = false;
  slot.pixel_aspect = 7;
  slot.visible_width = 256;
  slot.snes_height = 224;
  s_expected_slot = &slot;
  s_expected_alpha = 0.375f;
  s_expected_presentation_fps = 144.25;

  RenderComparison_Reset();
  s_expected_view = kRenderComparison_Enhanced;
  s_expected_host_viewport = kResolved;
  s_expected_transition_alpha = 0;
  s_expected_stages = 5;
  if (argc == 2 && strcmp(argv[1], "scene-failure") == 0) {
    s_fail_scene = true;
    (void)PresentFrame(&slot, s_expected_alpha, s_expected_presentation_fps);
    CHECK(SessionFatal_Requested());
    CHECK(s_stage == 4); /* Unwind CRT, then stop before menu and host UI. */
    CHECK(s_menu_draws == 0);
    return s_failures ? 1 : 0;
  }
  RunCase(&slot);

  slot.inidisp = 0x0f;
  slot.localization.dialogue_ticket = 10;
  s_dialogue_ready = true;
  RunCase(&slot);
  CHECK(!ArTextPresentation_Failed(10));
  s_dialogue_ready = false;
  RunCase(&slot); /* A visible native fallback must report to the scheduler. */
  CHECK(ArTextPresentation_Failed(10));
  slot.localization.dialogue_ticket = 11;
  slot.inidisp = 0x80;
  RunCase(&slot);
  CHECK(!ArTextPresentation_Failed(11));
  slot.inidisp = 0;
  RunCase(&slot);
  CHECK(!ArTextPresentation_Failed(11));
  slot.inidisp = 0x0f;
  s_dialogue_ready = true;
  RunCase(&slot);
  CHECK(!ArTextPresentation_Failed(11));
  /* An answered question remains in native memory while the modern menu
   * deliberately hides it for the handoff. Do not flag that stale ticket. */
  slot.localization.dialogue_ticket = 12;
  slot.sim_menu.valid = true;
  slot.sim_menu.model.phase = kSimMenu_Handoff;
  s_dialogue_ready = false;
  RunCase(&slot);
  CHECK(!ArTextPresentation_Failed(12));
  slot.sim_menu.model.phase = kSimMenu_Inventory;
  RunCase(&slot);
  CHECK(!ArTextPresentation_Failed(12));
  slot.sim_menu.model.phase = kSimMenu_Describe;
  slot.sim_menu.help.active = true;
  s_dialogue_ready = true; /* Help now publishes its own dialogue ticket. */
  RunCase(&slot);
  CHECK(!ArTextPresentation_Failed(12));
  slot.localization.dialogue_ticket = 14;
  s_dialogue_ready = false;
  RunCase(&slot);
  CHECK(ArTextPresentation_Failed(14));
  slot.localization.dialogue_ticket = 15;
  slot.sim_menu.help.active = false;
  RunCase(&slot); /* Miracle descriptions still require native dialogue. */
  CHECK(ArTextPresentation_Failed(15));
  /* The exception belongs only to an active handoff, never ordinary text. */
  slot.localization.dialogue_ticket = 16;
  slot.sim_menu.model.phase = kSimMenu_Handoff;
  slot.sim_menu.valid = false;
  RunCase(&slot);
  CHECK(ArTextPresentation_Failed(16));
  slot.localization.dialogue_ticket = 17;
  slot.sim_menu.valid = true;
  slot.sim_menu.model.phase = kSimMenu_Dialogue;
  RunCase(&slot);
  CHECK(ArTextPresentation_Failed(17));
  slot.sim_menu.valid = false;
  slot.localization.dialogue_ticket = 0;

  /* Authentic bypasses the enhanced compositor while retaining the player's
   * independent CRT configuration. */
  RenderComparison_OnPress(1000);
  RenderComparison_Tick(1050, false, true);
  RenderComparison_Tick(1950, false, true);
  slot.authentic_frame_serial = 7;
  s_authentic_uploaded_serial = 7;
  s_expected_view = kRenderComparison_Authentic;
  s_expected_host_viewport = kResolved;
  s_expected_stages = 5;
  RunCase(&slot);

  /* A loading screen has no current camera capture. Native black must not
   * read the old room's texture or require its serial. */
  slot.authentic_frame_serial = 0;
  const uint8_t blank_controls[] = {0, 0x80, 0x8f};
  for (unsigned i = 0; i < sizeof(blank_controls); i++) {
    const int menus_before = s_menu_draws;
    slot.inidisp = blank_controls[i];
    RunCase(&slot);
    CHECK(!SessionFatal_Requested() && s_menu_draws == menus_before);
  }
  slot.inidisp = 15;
  slot.authentic_frame_serial = s_authentic_uploaded_serial = 8;
  RunCase(&slot);

  /* A hold makes enhanced rendering the priority path and adds authentic PiP
   * inside the game composite before CRT resolve and all host UI. */
  RenderComparison_OnPress(2200);
  RenderComparison_Tick(2620, true, true);
  RenderComparison_Tick(3520, true, true);
  s_expected_view = kRenderComparison_SideBySide;
  s_expected_host_viewport = kResolved;
  s_expected_stages = 6;
  RunCase(&slot);

  /* PiP stays latched across blank room loading, omits its stale inset, and
   * resumes on the destination's first synchronized visible capture. */
  slot.authentic_frame_serial = 0;
  s_expected_stages = 5;
  for (unsigned i = 0; i < sizeof(blank_controls); i++) {
    const int menus_before = s_menu_draws;
    slot.inidisp = blank_controls[i];
    RunCase(&slot);
    CHECK(!SessionFatal_Requested() && s_menu_draws == menus_before);
    CHECK(RenderComparison_PresentView() == kRenderComparison_SideBySide);
  }
  slot.inidisp = 15;
  slot.authentic_frame_serial = s_authentic_uploaded_serial = 9;
  s_expected_stages = 6;
  RunCase(&slot);

  /* Releasing the hold leaves PiP latched. */
  RenderComparison_Tick(3540, false, true);
  CHECK(RenderComparison_PresentView() == kRenderComparison_SideBySide);

  /* A later short click clears PiP and toggles the underlying authentic base
   * to enhanced. The transition overlay must remain below host UI. */
  RenderComparison_OnPress(3700);
  RenderComparison_Tick(3750, false, true);
  RenderComparison_Tick(4200, false, true);
  s_expected_view = kRenderComparison_Enhanced;
  s_expected_transition_alpha = 255;
  s_expected_stages = 6;
  RunCase(&slot);

  /* A nonzero capture serial is not enough: presentation must reject a frame
   * the upload stage did not synchronize for this exact geometry. Keep this
   * terminal because SessionFatal deliberately latches for process lifetime. */
  RenderComparison_Tick(4650, false, true);
  RenderComparison_OnPress(5000);
  RenderComparison_Tick(5050, false, true);
  RenderComparison_Tick(5950, false, true);
  slot.authentic_frame_serial = 10;
  s_stage = 0;
  (void)PresentFrame(&slot, s_expected_alpha, s_expected_presentation_fps);
  CHECK(s_stage == 0);
  CHECK(SessionFatal_Requested());

  /* Layout feedback belongs to a successfully drawn window and page. Neither
   * a failed draw nor an older re-present may advance the current dialogue. */
  uint32_t page_end=0;
  ArTextPresentation_BeginFrame(20);
  ArTextPresentation_ReportPage(20,0,16);
  CHECK(!ArTextPresentation_PageEnd(20,0,&page_end));
  ArTextPresentation_MarkReady(20);
  ArTextPresentation_EndFrame();
  CHECK(ArTextPresentation_PageEnd(20,0,&page_end) && page_end==16);
  CHECK(!ArTextPresentation_PageEnd(20,16,&page_end));
  ArTextPresentation_BeginFrame(21);
  ArTextPresentation_ReportPage(21,16,32);
  ArTextPresentation_EndFrame();
  CHECK(!ArTextPresentation_PageEnd(21,16,&page_end));
  ArTextPresentation_BeginFrame(22);
  ArTextPresentation_ReportPage(22,16,32);
  ArTextPresentation_MarkReady(22);
  ArTextPresentation_EndFrame();
  ArTextPresentation_BeginFrame(20);
  ArTextPresentation_ReportPage(20,0,8);
  ArTextPresentation_MarkReady(20);
  ArTextPresentation_EndFrame();
  CHECK(ArTextPresentation_PageEnd(22,16,&page_end) && page_end==32);

  if (s_failures) {
    fprintf(stderr, "present_frame_order_test: %d failure(s)\n", s_failures);
    return 1;
  }
  puts("present_frame_order_test: PASS");
  return 0;
}
