/* ppu_render_pipeline_test.c — end-to-end SDL3 present validation with the REAL PPU.
 *
 * Stronger companion to render_pipeline_test.c: instead of a synthetic pixel
 * buffer, this instantiates the actual SNES PPU, points it at a framebuffer via
 * the same PpuBeginDrawing/ppu_runLine path main.c uses, sets a known backdrop
 * color, renders real scanlines, then pushes that framebuffer through the exact
 * SDL3 present + read-back pipeline. It proves the pixels the emulator actually
 * produces (RGB with alpha byte = 0) are VISIBLE after presenting — i.e. the
 * texture blend mode is correct and the screen is not black.
 *
 * Runs headless under the dummy video driver with the software renderer.
 */
#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "snes/ppu.h"

/* main.c owns this global; the PPU line renderer reads it to pick new/old path. */
bool g_new_ppu = false;

/* Trace/debug hooks the PPU references but that only fire under instrumentation
 * (SNESRECOMP_TRACE / debug server). Stub them so this standalone harness links
 * without pulling in the whole runtime. None run in a normal render. */
int ar_trace_active(void) { return 0; }
void ar_trace_ppumem(uint16_t a, uint8_t v) { (void)a; (void)v; }
void ar_trace_reg(uint16_t a, uint8_t v) { (void)a; (void)v; }
void ar_trace_vmadd(uint16_t a) { (void)a; }
void ar_trace_vram(uint16_t a, uint16_t v) { (void)a; (void)v; }
void ar_vramraw(uint16_t a, uint8_t v, int p) { (void)a; (void)v; (void)p; }
int ar_vramwatch(uint16_t a, uint8_t v) { (void)a; (void)v; return 0; }
void CpuDispatchLogWriteFile(const char *path) { (void)path; }
unsigned g_ar_blk_idx;
uint32_t g_ar_blk_ring[256];
const char *g_last_recomp_func;
uint8_t g_ram[0x20000];
int snes_frame_counter;

static int s_failures;
#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

enum { kW = 256, kH = 224 };

/* Build a BGR555 SNES color word (what CGRAM stores): 5 bits each, R low. */
static uint16_t bgr555(int r5, int g5, int b5) {
  return (uint16_t)((r5 & 0x1f) | ((g5 & 0x1f) << 5) | ((b5 & 0x1f) << 10));
}

/* Expand a 5-bit channel to 8-bit the way the PPU does ((v<<3)|(v>>2)). */
static int expand5(int v5) { return ((v5 & 0x1f) << 3) | ((v5 & 0x1f) >> 2); }

int main(void) {
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  CHECK(SDL_Init(SDL_INIT_VIDEO));

  /* ---- Real PPU: draw a solid backdrop, full brightness, force-blank off ---- */
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) { SDL_Quit(); return 1; }
  ppu_reset(ppu);

  /* Framebuffer the PPU renders into (ARGB8888, 4 bytes/px), same shape as
   * main.c's g_pixels. The PPU writes RGB and leaves the alpha byte 0. */
  static uint8_t fb[kW * kH * 4];
  memset(fb, 0, sizeof(fb));
  PpuBeginDrawing(ppu, fb, kW * 4, 0);

  /* Backdrop = CGRAM entry 0. Pick a distinctive color: R=25,G=10,B=3 (5-bit).
   * With no BG/OBJ layers enabled, every pixel resolves to this backdrop. */
  const int R5 = 25, G5 = 10, B5 = 3;
  ppu->cgram[0] = bgr555(R5, G5, B5);
  ppu->inidisp = 0x0f;   /* brightness 15, force-blank (bit 7) OFF */
  ppu->bgmode = 0;       /* mode 0; no layers enabled -> pure backdrop */

  for (int line = 0; line <= kH; line++) ppu_runLine(ppu, line);

  /* Sanity: the PPU wrote RGB with alpha byte 0 (BGRA byte order in memory:
   * [0]=B [1]=G [2]=R [3]=A). Confirm a mid-screen pixel matches the backdrop
   * and its alpha byte is indeed 0 (the condition that black-screens under a
   * BLEND texture). */
  const uint8_t *mid = fb + ((size_t)(kH / 2) * kW + (kW / 2)) * 4;
  int exp_r = expand5(R5), exp_g = expand5(G5), exp_b = expand5(B5);
  fprintf(stderr, "[ppu-test] backdrop pixel BGRA = %d,%d,%d,%d (expected RGB %d,%d,%d, A 0)\n",
          mid[0], mid[1], mid[2], mid[3], exp_r, exp_g, exp_b);
  CHECK(mid[2] == exp_r && mid[1] == exp_g && mid[0] == exp_b);
  CHECK(mid[3] == 0);   /* the alpha-0 framebuffer that triggered the bug */

  /* ---- Present through the exact SDL3 path main.c uses ---- */
  SDL_Window *window =
      SDL_CreateWindow("ppu-pipeline-test", kW * 3, kH * 3, SDL_WINDOW_HIDDEN);
  CHECK(window != NULL);
  SDL_Renderer *renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
  CHECK(renderer != NULL);

  if (renderer) {
    SDL_Texture *texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kW, kH);
    CHECK(texture != NULL);
    /* The fix under test: force NONE so the alpha-0 framebuffer blits opaque.
     * (SDL3 defaults to BLEND, which would present this as a black screen.) */
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

    CHECK(SDL_UpdateTexture(texture, NULL, fb, kW * 4));
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    CHECK(SDL_RenderClear(renderer));
    CHECK(SDL_RenderTexture(renderer, texture, NULL, NULL));
    CHECK(SDL_RenderPresent(renderer));

    SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL);
    SDL_Surface *argb =
        raw ? SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888) : NULL;
    if (raw) SDL_DestroySurface(raw);
    CHECK(argb != NULL);

    if (argb) {
      /* Center of the presented output must be the backdrop RGB, not black. */
      const uint8_t *row = (const uint8_t *)argb->pixels +
                           (size_t)(argb->h / 2) * argb->pitch;
      const uint8_t *cp = row + (size_t)(argb->w / 2) * 4;  /* B,G,R,A */
      int got_b = cp[0], got_g = cp[1], got_r = cp[2];
      long nonblack = 0;
      for (int y = 0; y < argb->h; y++) {
        const uint8_t *r = (const uint8_t *)argb->pixels + (size_t)y * argb->pitch;
        for (int x = 0; x < argb->w; x++)
          if (r[x * 4] || r[x * 4 + 1] || r[x * 4 + 2]) nonblack++;
      }
      fprintf(stderr, "[ppu-test] presented center RGB = %d,%d,%d; nonblack=%ld/%ld\n",
              got_r, got_g, got_b, nonblack, (long)argb->w * argb->h);
      CHECK(got_r == exp_r && got_g == exp_g && got_b == exp_b);
      CHECK(nonblack == (long)argb->w * argb->h);  /* whole frame visible */
      SDL_DestroySurface(argb);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
  }

  SDL_DestroyWindow(window);
  ppu_free(ppu);
  SDL_Quit();

  if (s_failures) {
    fprintf(stderr, "ppu render pipeline tests: %d failure(s)\n", s_failures);
    return 1;
  }
  fprintf(stderr, "ppu render pipeline tests: pass\n");
  return 0;
}
