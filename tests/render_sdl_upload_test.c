/* Real GPU ordering/readback tests for upload reuse and split presentation. */
#include "platform/sdl/render_sdl_internal.h"
#include "support/test_assert.h"
#include "support/test_sdl_environment.h"
#include <stdio.h>
#include <string.h>

enum { W = 257, H = 9, Pitch = W + 7 };
static Uint32 Color(unsigned frame, int x, int y) {
  return 0xff000000u | ((frame * 37 + x * 13) & 255) << 16 |
      ((y * 29 + frame) & 255) << 8 | ((x + y + frame * 17) & 255);
}
static void CheckUploads(ArRenderDevice *device, ArRenderPixelFormat format) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  const SDL_PixelFormat pixel_format = format == kArRenderPixelFormat_Argb8888
      ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888;
  const SDL_PixelFormatDetails *details = SDL_GetPixelFormatDetails(pixel_format);
  ArRenderTexture source;
  const ArRenderTextureDesc desc = {.width=W, .height=H, .format=format,
    .usage=kArRenderTextureUsage_Streaming, .filter=kArRenderFilter_Nearest,
    .blend=kArRenderBlendMode_Opaque};
  assert(ArRenderDevice_CreateTexture(device, &desc, &source));
  SDL_Texture *target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
      SDL_TEXTUREACCESS_TARGET, W*2, H);
  assert(target && SDL_SetRenderTarget(renderer, target));
  Uint32 pixels[H][Pitch];
  for (unsigned frame=0; frame<64; ++frame) {
    for (int y=0; y<H; ++y) for (int x=0; x<Pitch; ++x) {
      const Uint32 c=Color(frame,x,y);
      pixels[y][x]=SDL_MapRGBA(details,NULL,(c>>16)&255,(c>>8)&255,c&255,255);
    }
    assert(ArRenderDevice_UpdateTexture(device,source,NULL,pixels,sizeof(pixels[0])));
    const SDL_FRect left={0,0,W,H}, right={W,0,W,H};
    SDL_Texture *native=ArSdlRenderBackend_UnwrapTexture(source);
    assert(SDL_RenderTexture(renderer,native,NULL,&left));
    /* An odd-width, offset patch must preserve all untouched texels. It is
     * queued after the left draw and before the right draw, without readback
     * or an explicit wait to serialize the staging buffer for the backend. */
    const ArRenderRectI patch={3,2,13,4};
    for (int y=0; y<patch.h; ++y) for (int x=0; x<patch.w; ++x)
      pixels[y][x]=SDL_MapRGBA(details,NULL,241,17,(Uint8)frame,255);
    assert(ArRenderDevice_UpdateTexture(device,source,&patch,pixels,sizeof(pixels[0])));
    assert(SDL_RenderTexture(renderer,native,NULL,&right));
    SDL_Surface *raw=SDL_RenderReadPixels(renderer,NULL);
    assert(raw);
    SDL_Surface *read=SDL_ConvertSurface(raw,SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(raw);assert(read && read->w==W*2 && read->h==H);
    for (int y=0; y<H; ++y) for (int x=0; x<W*2; ++x) {
      Uint32 actual;memcpy(&actual,(Uint8 *)read->pixels+y*read->pitch+x*4,4);
      const bool patched=x>=W+patch.x && x<W+patch.x+patch.w && y>=patch.y && y<patch.y+patch.h;
      const Uint32 expected=patched ? 0xfff11100u|frame : Color(frame,x%W,y);
      assert(actual==expected);
    }
    SDL_DestroySurface(read);
  }
  /* Deletion while the last update is in flight must release staging safely. */
  assert(ArRenderDevice_UpdateTexture(device,source,NULL,pixels,sizeof(pixels[0])));
  ArRenderDevice_DestroyTexture(device,source);
  assert(ArRenderDevice_SetRenderTarget(device,ArRenderTexture_Invalid()));
  SDL_DestroyTexture(target);
}

static void CheckPreparedPresentation(ArRenderDevice *device, SDL_Window *window) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  const ArRenderTextureDesc desc = {.width=W, .height=H,
    .format=kArRenderPixelFormat_Argb8888, .usage=kArRenderTextureUsage_Streaming,
    .filter=kArRenderFilter_Nearest, .blend=kArRenderBlendMode_Opaque};
  ArRenderTexture source;
  assert(ArRenderDevice_CreateTexture(device, &desc, &source));
  assert(ArRenderDevice_SetRenderTarget(device, ArRenderTexture_Invalid()));
  Uint32 pixels[H][W];
  for (unsigned frame=0; frame<32; ++frame) {
    if (frame & 1) assert(SDL_ShowWindow(window));
    else assert(SDL_HideWindow(window));
    SDL_PumpEvents();
    SDL_Surface *reference = NULL;
    for (unsigned prepared=0; prepared<2; ++prepared) {
      /* Exercise upload reuse/order hazards before submitting. The
       * second upload must not overwrite the texture used by the first draw. */
      for (int y=0; y<H; ++y) for (int x=0; x<W; ++x)
        pixels[y][x]=Color(frame,x,y);
      assert(ArRenderDevice_UpdateTexture(device, source, NULL, pixels, sizeof(pixels[0])));
      const SDL_FRect left={0,0,W,H}, right={W,0,W,H};
      SDL_Texture *native = ArSdlRenderBackend_UnwrapTexture(source);
      assert(SDL_RenderTexture(renderer, native, NULL, &left));
      for (int y=0; y<H; ++y) for (int x=0; x<W; ++x)
        pixels[y][x]=Color(frame+1,x,y);
      assert(ArRenderDevice_UpdateTexture(device, source, NULL, pixels, sizeof(pixels[0])));
      assert(SDL_RenderTexture(renderer, native, NULL, &right));
      const SDL_FRect overlay={17,2,47,4};
      assert(SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND));
      assert(SDL_SetRenderDrawColor(renderer, 231, 91, 15, 93));
      assert(SDL_RenderFillRect(renderer, &overlay));
      /* Both passes use the single ordered present; the retired early
       * submission variant is gone, so this checks repeat determinism. */
      assert(ArRenderDevice_Present(device));
      /* Read only AFTER the terminal submission: a readback before it would
       * accidentally serialize the sequence we are trying to validate. */
      SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL);
      assert(raw);
      SDL_Surface *actual = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888);
      SDL_DestroySurface(raw);
      assert(actual && actual->w == W*2 && actual->h == H);
      Uint32 first;
      memcpy(&first, actual->pixels, sizeof(first));
      assert(first == Color(frame,0,0)); /* Cannot pass by retaining old pixels. */
      if (!prepared) reference = actual;
      else {
        for (int y=0; y<H; ++y)
          assert(!memcmp((Uint8 *)reference->pixels+y*reference->pitch,
                         (Uint8 *)actual->pixels+y*actual->pitch, W*2*4));
        SDL_DestroySurface(actual);
      }
    }
    SDL_DestroySurface(reference);
  }
  ArRenderDevice_DestroyTexture(device, source);
  puts("split presentation: 32 pixel-exact pairs, visible and hidden windows");
}

int main(void) {
  if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
  SDL_Window *window=SDL_CreateWindow("Upload staging test",W*2,H,SDL_WINDOW_HIDDEN);
  assert(window);
  assert(SDL_SetHintWithPriority("AR_SDL_GPU_UPLOAD_REUSE", "1", SDL_HINT_OVERRIDE));
  assert(Test_SDLSetEnv("AR_SDL_GPU_ORDERED", "1", 1) == 0);
  ArRenderDevice device={0};
  if (!ArSdlRenderBackend_CreateForWindow(&device,window,NULL)) {
    fprintf(stderr,"upload test skipped: %s\n",SDL_GetError());
    SDL_DestroyWindow(window);SDL_Quit();return 77;
  }
  assert(((ArSdlRenderBackend *)device.context)->reuse_texture_uploads);
  CheckUploads(&device,kArRenderPixelFormat_Argb8888);
  CheckUploads(&device,kArRenderPixelFormat_Abgr8888);
  CheckPreparedPresentation(&device,window);
  ArSdlRenderBackend_Destroy(&device);SDL_DestroyWindow(window);SDL_Quit();
  puts("render_sdl_upload_test: PASS");return 0;
}
