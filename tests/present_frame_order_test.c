/* The terminal frame order is correctness, not decoration: the CRT resolve
 * paints outside the game image black, so host UI must run afterward. Compile
 * the small orchestrator against stage stubs to keep that order testable on a
 * headless machine with no renderer or GPU. */
#include "present.h"
#include "present_internal.h"
#include "crt_post.h"
#include "render_comparison.h"
#include "session_fatal.h"
#include "settings.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

SDL_Renderer *g_renderer = (SDL_Renderer *)(uintptr_t)1;
ArRenderDevice g_render_device;
Settings g_settings;

static int s_failures;
static int s_stage;
static const FrameSlot *s_expected_slot;
static float s_expected_alpha;
static double s_expected_presentation_fps;
static RenderComparisonView s_expected_view;
static SDL_Rect s_expected_host_viewport;
static uint8_t s_expected_transition_alpha;
static int s_expected_stages;
static uint64_t s_authentic_uploaded_serial;
static const SDL_Rect kFallback = { 160, 0, 960, 720 };
static const SDL_Rect kResolved = { 161, 1, 958, 718 };

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

SDL_Rect ComputePresentationViewportWithOutput(
    SDL_Renderer *renderer, bool ignore_aspect_ratio,
    int pixel_aspect, int visible_width, int snes_height,
    SDL_Point *output_size) {
  CHECK(s_stage++ == 0);
  CHECK(renderer == g_renderer);
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
  *output_size = (SDL_Point){1280, 720};
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
}

bool PresentAuthenticScene(const FrameSlot *slot, SDL_Rect viewport) {
  CHECK(s_stage++ == 2);
  CHECK(s_expected_view == kRenderComparison_Authentic);
  CHECK(slot == s_expected_slot);
  CHECK(SDL_RectsEqual(&viewport, &kFallback));
  return true;
}

bool PresentAuthenticPictureInPicture(const FrameSlot *slot,
                                      SDL_Rect priority_viewport) {
  CHECK(s_stage++ == 3);
  CHECK(s_expected_view == kRenderComparison_SideBySide);
  CHECK(slot == s_expected_slot);
  CHECK(SDL_RectsEqual(&priority_viewport, &kFallback));
  return true;
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
      s_expected_view == kRenderComparison_SideBySide ? 4 : 3;
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

void PresentHostUi(const FrameSlot *slot, SDL_Rect viewport,
                   SDL_Point output_size,
                   double presentation_fps) {
  CHECK(s_stage++ == s_expected_stages - 1);
  CHECK(slot == s_expected_slot);
  /* The UI must receive End's authoritative rectangle, not the fallback that
   * was calculated before SDL resolved its per-target logical presentation. */
  CHECK(SDL_RectsEqual(&viewport, &s_expected_host_viewport));
  CHECK(output_size.x == 1280 && output_size.y == 720);
  CHECK(presentation_fps == s_expected_presentation_fps);
}

static void RunCase(FrameSlot *slot) {
  s_stage = 0;
  SDL_Rect image = PresentFrame(
      slot, s_expected_alpha, s_expected_presentation_fps);
  CHECK(s_stage == s_expected_stages);
  CHECK(SDL_RectsEqual(&image, &s_expected_host_viewport));
}

int main(void) {
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
  RunCase(&slot);

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

  /* A hold makes enhanced rendering the priority path and adds authentic PiP
   * inside the game composite before CRT resolve and all host UI. */
  RenderComparison_OnPress(2200);
  RenderComparison_Tick(2620, true, true);
  RenderComparison_Tick(3520, true, true);
  s_expected_view = kRenderComparison_SideBySide;
  s_expected_host_viewport = kResolved;
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
  slot.authentic_frame_serial = 8;
  s_stage = 0;
  (void)PresentFrame(&slot, s_expected_alpha, s_expected_presentation_fps);
  CHECK(s_stage == 0);
  CHECK(SessionFatal_Requested());

  if (s_failures) {
    fprintf(stderr, "present_frame_order_test: %d failure(s)\n", s_failures);
    return 1;
  }
  puts("present_frame_order_test: PASS");
  return 0;
}
