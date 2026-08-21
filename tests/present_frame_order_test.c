/* The terminal frame order is correctness, not decoration: the CRT resolve
 * paints outside the game image black, so host UI must run afterward. Compile
 * the small orchestrator against stage stubs to keep that order testable on a
 * headless machine with no renderer or GPU. */
#include "present.h"
#include "present_internal.h"
#include "crt_post.h"

#include <stdint.h>
#include <stdio.h>

SDL_Renderer *g_renderer = (SDL_Renderer *)(uintptr_t)1;

static int s_failures;
static int s_stage;
static const FrameSlot *s_expected_slot;
static const DioramaScrollSnapshot *s_expected_previous;
static const ActionObjInterpolationFrame *s_expected_previous_action_obj;
static float s_expected_alpha;
static double s_expected_presentation_fps;
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
  CHECK(pixel_aspect == 7);
  CHECK(visible_width == 256);
  CHECK(snes_height == 224);
  CHECK(output_size != NULL);
  *output_size = (SDL_Point){1280, 720};
  return kFallback;
}

bool CrtPost_Begin(SDL_Renderer *renderer) {
  CHECK(s_stage++ == 1);
  CHECK(renderer == g_renderer);
  return true;
}

void PresentCompositeScene(const FrameSlot *slot,
                           const DioramaScrollSnapshot *prev_scroll,
                           const ActionObjInterpolationFrame *prev_action_obj,
                           float alpha) {
  CHECK(s_stage++ == 2);
  CHECK(slot == s_expected_slot);
  CHECK(prev_scroll == s_expected_previous);
  CHECK(prev_action_obj == s_expected_previous_action_obj);
  CHECK(alpha == s_expected_alpha);
}

SDL_Rect CrtPost_End(SDL_Renderer *renderer,
                     int scan_columns, int scan_lines, SDL_Rect image) {
  CHECK(s_stage++ == 3);
  CHECK(renderer == g_renderer);
  CHECK(scan_columns == 256);
  CHECK(scan_lines == 224);
  CHECK(SDL_RectsEqual(&image, &kFallback));
  return kResolved;
}

void PresentHostUi(const FrameSlot *slot, SDL_Rect viewport,
                   SDL_Point output_size,
                   double presentation_fps) {
  CHECK(s_stage++ == 4);
  CHECK(slot == s_expected_slot);
  /* The UI must receive End's authoritative rectangle, not the fallback that
   * was calculated before SDL resolved its per-target logical presentation. */
  CHECK(SDL_RectsEqual(&viewport, &kResolved));
  CHECK(output_size.x == 1280 && output_size.y == 720);
  CHECK(presentation_fps == s_expected_presentation_fps);
}

int main(void) {
  FrameSlot slot = {0};
  slot.ignore_aspect_ratio = false;
  slot.pixel_aspect = 7;
  slot.visible_width = 256;
  slot.snes_height = 224;
  DioramaScrollSnapshot previous = {0};
  ActionObjInterpolationFrame previous_action_obj = {0};
  s_expected_slot = &slot;
  s_expected_previous = &previous;
  s_expected_previous_action_obj = &previous_action_obj;
  s_expected_alpha = 0.375f;
  s_expected_presentation_fps = 144.25;

  SDL_Rect resolved = PresentFrame(
      &slot, &previous, &previous_action_obj, s_expected_alpha,
      s_expected_presentation_fps);
  CHECK(s_stage == 5);
  CHECK(SDL_RectsEqual(&resolved, &kResolved));

  if (s_failures) {
    fprintf(stderr, "present_frame_order_test: %d failure(s)\n", s_failures);
    return 1;
  }
  puts("present_frame_order_test: PASS");
  return 0;
}
