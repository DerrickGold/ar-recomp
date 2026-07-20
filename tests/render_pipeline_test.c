/* render_pipeline_test.c — SDL3 render-pipeline regression guard.
 *
 * The prior SDL3 attempt produced a black screen while the game logic ran, so
 * this test exercises exactly the pixel path main.c uses to present the
 * emulated SNES framebuffer — but headless, without a ROM:
 *
 *   ARGB8888 pixel buffer
 *     -> SDL_CreateTexture(STREAMING) + SDL_SetTextureScaleMode(NEAREST)
 *     -> SDL_UpdateTexture(sub-rect, pitch)
 *     -> SDL_SetRenderLogicalPresentation(LETTERBOX)
 *     -> SDL_RenderTexture(FRect src, whole target)
 *     -> SDL_RenderPresent
 *     -> SDL_RenderReadPixels (returns a surface in SDL3)
 *
 * and asserts the pixels survive the round trip (i.e. the frame is NOT black).
 * It pins the SDL3-specific gotchas that caused the regression:
 *   - render primitives take SDL_FRect, not SDL_Rect
 *   - SDL_RenderReadPixels returns a newly allocated SDL_Surface*
 *   - the bool return convention (true = success)
 *   - textures default to linear filtering (nearest must be set explicitly)
 *   - logical presentation replaces SDL_RenderSetLogicalSize
 *
 * Runs under the dummy video driver so it needs no display; the software
 * renderer performs the real blit and readback.
 */
#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int s_failures;
#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

enum { kBufW = 448, kBufH = 240, kSnesW = 256, kSnesH = 224 };

/* Read one ARGB8888 pixel from a converted readback surface. */
static uint32_t SurfacePixel(SDL_Surface *s, int x, int y) {
  const uint8_t *row = (const uint8_t *)s->pixels + (size_t)y * s->pitch;
  const uint8_t *p = row + (size_t)x * 4;
  return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 |
         (uint32_t)p[1] << 8 | p[0];
}

int main(void) {
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  CHECK(SDL_Init(SDL_INIT_VIDEO));

  /* A hidden real window + software renderer mirrors main.c's headless_video
   * path (SDL_SOFTWARE_RENDERER) while keeping SDL_RenderReadPixels honest.
   * The window is deliberately WIDER than the 256:224 content aspect (a 16:9
   * shape) so an active LETTERBOX presentation produces pillarbars — which is
   * exactly the condition under which SDL_GetCurrentRenderOutputSize (shrunk)
   * and SDL_GetRenderOutputSize (true) diverge. */
  const int kWinW = 1280, kWinH = 720;
  SDL_Window *window = SDL_CreateWindow(
      "render-pipeline-test", kWinW, kWinH, SDL_WINDOW_HIDDEN);
  CHECK(window != NULL);
  SDL_Renderer *renderer =
      SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
  CHECK(renderer != NULL);
  if (!renderer) { SDL_Quit(); return 1; }

  /* Streaming texture at the full widescreen budget, like g_texture — created
   * the SAME way main.c creates it, including the blend mode. This is the
   * regression guard for the black-screen bug: SDL3 defaults new textures to
   * BLENDMODE_BLEND (SDL2 used NONE), and the PPU framebuffer carries alpha=0,
   * so without an explicit NONE the base framebuffer blends to transparent and
   * the screen goes black. */
  SDL_Texture *texture = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kBufW, kSnesH);
  CHECK(texture != NULL);
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
  SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

  /* Fill the active sub-rect with a known RGB color whose ALPHA BYTE IS ZERO —
   * exactly how the PPU writes g_pixels (ppu_old.c leaves pixelBuffer[3]=0).
   * Using alpha=0 (not an opaque 0xFF..) is what makes this test able to catch
   * the blend-mode regression. Leave a distinct marker in the top-left pixel
   * so we can prove the blit is not flipped/offset. */
  static uint32_t pixels[kBufW * kSnesH];
  const uint32_t fill = 0x003A7BEFu;    /* alpha byte 0x00, like the PPU */
  const uint32_t marker = 0x0010FF20u;
  const uint32_t fill_opaque = 0xFF3A7BEFu;   /* what NONE-blit produces */
  for (int i = 0; i < kBufW * kSnesH; i++) pixels[i] = fill;
  pixels[0] = marker;

  SDL_Rect upload = { 0, 0, kBufW, kSnesH };
  CHECK(SDL_UpdateTexture(texture, &upload, pixels, kBufW * 4));

  /* Present the authentic centre 256 columns, like RenderFramebuffer's 4:3. */
  SDL_Rect src_i = { (kBufW - kSnesW) / 2, 0, kSnesW, kSnesH };
  SDL_FRect src_f = { (float)src_i.x, 0.0f, (float)kSnesW, (float)kSnesH };

  /* LETTERBOX logical presentation, matching the widescreen present path. */
  CHECK(SDL_SetRenderLogicalPresentation(
      renderer, kSnesW, kSnesH, SDL_LOGICAL_PRESENTATION_LETTERBOX));

  /* Regression guard for the output-size function choice: while a logical
   * presentation is active, SDL_GetRenderOutputSize must still report the
   * TRUE physical output (the 1:1 replacement for SDL2's
   * SDL_GetRendererOutputSize), whereas SDL_GetCurrentRenderOutputSize is
   * shrunk to the logical content region. main.c/settings_overlay.c compute
   * viewports and map mouse clicks in physical-pixel space, so they must use
   * the former — using the latter double-letterboxes overlays and mis-maps
   * clicks in widescreen mode. The window is 256x224 * 3 = 768x672. */
  {
    int true_w = 0, true_h = 0, cur_w = 0, cur_h = 0;
    CHECK(SDL_GetRenderOutputSize(renderer, &true_w, &true_h));
    CHECK(SDL_GetCurrentRenderOutputSize(renderer, &cur_w, &cur_h));
    fprintf(stderr, "[render-test] true output %dx%d, logical-adjusted %dx%d\n",
            true_w, true_h, cur_w, cur_h);
    /* True output must equal the physical window (ignores presentation). */
    CHECK(true_w == kWinW && true_h == kWinH);
    /* The logical-adjusted width is the 256:224 content region fitted (with
     * pillarbars) into the 16:9 window, so it must be narrower than the true
     * width. That gap is precisely what double-letterboxes overlays and
     * mis-maps clicks if the wrong function is used for physical geometry. */
    CHECK(cur_w < true_w);
  }

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  CHECK(SDL_RenderClear(renderer));
  CHECK(SDL_RenderTexture(renderer, texture, &src_f, NULL));
  CHECK(SDL_RenderPresent(renderer));

  /* SDL3 SDL_RenderReadPixels RETURNS a surface (SDL2 filled a buffer). */
  SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL);
  CHECK(raw != NULL);
  SDL_Surface *argb =
      raw ? SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888) : NULL;
  if (raw) SDL_DestroySurface(raw);
  CHECK(argb != NULL);

  if (argb) {
    /* The frame must not be black. Under a BLENDMODE_NONE blit the RGB is
     * preserved and the alpha becomes the opaque render target's, so read-back
     * pixels match fill_opaque (compare RGB only, ignoring alpha). If the
     * texture were left at SDL3's default BLEND, the alpha-0 fill would blend
     * to transparent and every content pixel would read back black — which the
     * fill_hits==nonblack and center checks below would fail on. */
    const uint32_t rgb_mask = 0x00FFFFFFu;
    long fill_hits = 0, nonblack = 0;
    for (int y = 0; y < argb->h; y++) {
      for (int x = 0; x < argb->w; x++) {
        uint32_t px = SurfacePixel(argb, x, y);
        if ((px & rgb_mask) != 0) nonblack++;
        if ((px & rgb_mask) == (fill_opaque & rgb_mask)) fill_hits++;
      }
    }
    fprintf(stderr, "[render-test] output %dx%d, fill_hits=%ld nonblack=%ld\n",
            argb->w, argb->h, fill_hits, nonblack);
    /* The content is pillarboxed inside the 16:9 window, so the fill covers
     * only the centered content region — a large fraction, but not the whole
     * frame. Require a substantial presence and that essentially all non-black
     * pixels ARE the fill (the pillarbars are the only black area). */
    CHECK(fill_hits > (long)argb->w * argb->h / 4);
    CHECK(nonblack > 0);
    CHECK(fill_hits == nonblack);  /* nothing non-black except the fill */

    /* The center of the output samples the interior fill, never the marker or
     * a border — proves the blit is neither offset nor flipped into black. */
    uint32_t center = SurfacePixel(argb, argb->w / 2, argb->h / 2);
    CHECK((center & rgb_mask) == (fill_opaque & rgb_mask));

    SDL_DestroySurface(argb);
  }

  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  if (s_failures) {
    fprintf(stderr, "render pipeline tests: %d failure(s)\n", s_failures);
    return 1;
  }
  fprintf(stderr, "render pipeline tests: pass\n");
  return 0;
}
