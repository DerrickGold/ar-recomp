/* main_thread_boot_test.c — Phase 0 insurance: prove the SDL present PATTERN
 * works entirely on the SDL_Init thread.
 *
 * Phase 0 of the cleanup moves ALL SDL rendering back onto the main thread by
 * never spawning the present thread. This harness is the cheap, ROM-free proof
 * that the create-window -> create-renderer -> clear -> present -> readpixels
 * sequence is valid when performed on ONE thread (the SDL_Init thread) — i.e.
 * that nothing in that sequence structurally requires a second thread.
 *
 * It CANNOT link main.c's real SubmitFrameToPresent (static + ROM-welded via
 * funcs.h), so it validates the pattern, not that specific function.
 *
 * Runs headless under the dummy video driver with the software renderer, like
 * the sibling render-pipeline tests.
 */
#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static int s_failures;
#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s (%s)\n", \
            __FILE__, __LINE__, #expr, SDL_GetError()); \
    s_failures++; \
  } \
} while (0)

enum { kW = 256, kH = 224, kWinScale = 3 };

int main(void) {
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  CHECK(SDL_Init(SDL_INIT_VIDEO));

  /* Pin the thread SDL was initialized on. Everything below must run on it —
   * that is the whole Phase 0 invariant (no present thread). */
  const SDL_ThreadID init_tid = SDL_GetCurrentThreadID();

  SDL_Window *window = SDL_CreateWindow(
      "main-thread-boot-test", kW * kWinScale, kH * kWinScale,
      SDL_WINDOW_HIDDEN);
  CHECK(window != NULL);
  SDL_Renderer *renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
  CHECK(renderer != NULL);
  if (!renderer) { if (window) SDL_DestroyWindow(window); SDL_Quit(); return 1; }

  /* Drive several frames synchronously on the init thread — clear to a known
   * color, present, read the pixels back, assert they survive. This is the
   * exact ordering the synchronous SubmitFrameToPresent path uses:
   * (composite ->) RenderPresent, all on the calling thread. */
  const struct { Uint8 r, g, b; } frames[] = {
      { 0x3A, 0x7B, 0xEF }, { 0x10, 0xC0, 0x20 }, { 0xFF, 0x00, 0x88 },
  };
  for (int f = 0; f < (int)(sizeof(frames) / sizeof(frames[0])); f++) {
    /* Re-assert we never migrated off the init thread across frames. */
    CHECK(SDL_GetCurrentThreadID() == init_tid);

    CHECK(SDL_SetRenderDrawColor(renderer, frames[f].r, frames[f].g,
                                 frames[f].b, 0xFF));
    CHECK(SDL_RenderClear(renderer));
    CHECK(SDL_RenderPresent(renderer));

    SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL);
    CHECK(raw != NULL);
    SDL_Surface *argb =
        raw ? SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888) : NULL;
    if (raw) SDL_DestroySurface(raw);
    CHECK(argb != NULL);
    if (argb) {
      const uint8_t *row = (const uint8_t *)argb->pixels +
                           (size_t)(argb->h / 2) * argb->pitch;
      const uint8_t *cp = row + (size_t)(argb->w / 2) * 4;  /* B,G,R,A */
      CHECK(cp[2] == frames[f].r && cp[1] == frames[f].g && cp[0] == frames[f].b);
      SDL_DestroySurface(argb);
    }
  }

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  if (s_failures) {
    fprintf(stderr, "main-thread boot test: %d failure(s)\n", s_failures);
    return 1;
  }
  fprintf(stderr, "main-thread boot test: pass\n");
  return 0;
}
