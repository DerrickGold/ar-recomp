/* One orchestration point for every rendered frame. Keeping this separate from
 * the large mode compositors makes the scene -> resolve -> host-UI order both
 * structural and independently testable with stubbed stages. */
#include "present.h"

#include "crt_post.h"
#include "present_internal.h"

extern SDL_Renderer *g_renderer;

SDL_Rect PresentFrame(const FrameSlot *slot, float alpha,
                      double presentation_fps) {
  SDL_Rect image = {0};
  if (!slot || !g_renderer) return image;

  SDL_Point output_size = {0};
  image = ComputePresentationViewportWithOutput(
      g_renderer, slot->ignore_aspect_ratio,
      slot->pixel_aspect, slot->visible_width, slot->snes_height,
      &output_size);
  CrtPost_Begin(g_renderer);
  PresentCompositeScene(slot, alpha);
  image = CrtPost_End(
      g_renderer, slot->visible_width, slot->snes_height, image);
  PresentHostUi(slot, image, output_size, presentation_fps);
  return image;
}
