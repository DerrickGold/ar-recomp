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

typedef struct UploadFlushProbe { unsigned calls; bool fail; } UploadFlushProbe;
static bool UploadFlush(void *userdata) {
  UploadFlushProbe *probe = userdata;
  ++probe->calls;
  return !probe->fail;
}

static Uint32 UploadColor(unsigned frame, int x, int y) {
  return (Color(frame,x,y)&0x00ffffffu) | ((x+y+(int)frame)%3 ? 0x7f000000u : 0);
}

static void FillUpload(Uint32 pixels[H][Pitch], const SDL_PixelFormatDetails *details,
    unsigned frame) {
  for (int y=0; y<H; ++y) for (int x=0; x<Pitch; ++x) {
    const Uint32 c=UploadColor(frame,x,y);
    pixels[y][x]=SDL_MapRGBA(details,NULL,(c>>16)&255,(c>>8)&255,c&255,c>>24);
  }
}

static SDL_Surface *ReadUploadTarget(SDL_Renderer *renderer) {
  SDL_Surface *raw=SDL_RenderReadPixels(renderer,NULL);
  assert(raw);
  SDL_Surface *read=SDL_ConvertSurface(raw,SDL_PIXELFORMAT_ARGB8888);
  SDL_DestroySurface(raw);assert(read);
  return read;
}

static void CheckUploadBatches(ArRenderDevice *device, ArRenderPixelFormat format) {
  SDL_Renderer *renderer=ArSdlRenderBackend_Renderer(device);
  ArSdlRenderBackend *backend=device->context;
  const SDL_PixelFormatDetails *details=SDL_GetPixelFormatDetails(
      format==kArRenderPixelFormat_Argb8888 ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888);
  const ArRenderTextureDesc desc={.width=W,.height=H,.format=format,
    .usage=kArRenderTextureUsage_Streaming,.filter=kArRenderFilter_Nearest,.blend=kArRenderBlendMode_Opaque};
  ArRenderTexture sources[2];
  for (unsigned i=0; i<2; ++i) assert(ArRenderDevice_CreateTexture(device,&desc,&sources[i]));
  SDL_Texture *target=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_TARGET,W*4,H);
  assert(target && SDL_SetRenderTarget(renderer,target));
  Uint32 pixels[H][Pitch],expected[2][H][W];
  for (unsigned frame=0; frame<8; ++frame) {
    SDL_Surface *reference=NULL;
    /* Scalar native uploads, one native transaction, then optional-backend
     * fallback. No wait/readback intervenes between the uploads and draws. */
    for (unsigned mode=0; mode<3; ++mode) {
      backend->reuse_texture_uploads=mode!=2;
      for (unsigned i=0; i<2; ++i) {
        FillUpload(pixels,details,frame+i);
        assert(ArRenderDevice_UpdateTexture(device,sources[i],NULL,pixels,sizeof(pixels[0])));
      }
      for (unsigned i=0; i<2; ++i) {
        const SDL_FRect destination={(float)(i*W),0,W,H};
        assert(SDL_RenderTexture(renderer,ArSdlRenderBackend_UnwrapTexture(sources[i]),NULL,&destination));
      }
      UploadFlushProbe probe={0};
      assert(ArSdlRenderBackend_SetExternalFlush(device,UploadFlush,&probe));
      if (mode) assert(ArRenderDevice_BeginTextureUploads(device));
      for (unsigned i=0; i<2; ++i) {
        FillUpload(pixels,details,frame+i+2);
        assert(ArRenderDevice_UpdateTexture(device,sources[i],NULL,pixels,sizeof(pixels[0])));
        for (int y=0; y<H; ++y) for (int x=0; x<W; ++x) expected[i][y][x]=UploadColor(frame+i+2,x,y);
        /* Reuse and immediately overwrite caller memory; staging must retain
         * every queued payload, including overlapping writes to one texture. */
        memset(pixels,0xcd,sizeof(pixels));
        const ArRenderRectI patches[]={{3,2,13,4},{9,3,7,2}};
        for (unsigned p=0; p<2; ++p) {
          const ArRenderRectI *r=&patches[p];
          const Uint32 c=0xff000000u|((61+i*79)<<16)|((23+p*101)<<8)|(frame+11);
          const Uint32 mapped=SDL_MapRGBA(details,NULL,(c>>16)&255,(c>>8)&255,c&255,c>>24);
          for (int y=0; y<r->h; ++y) for (int x=0; x<r->w; ++x) {
            pixels[y][x]=mapped;expected[i][y+r->y][x+r->x]=c;
          }
          assert(ArRenderDevice_UpdateTexture(device,sources[i],r,pixels,sizeof(pixels[0])));
          memset(pixels,0x97,sizeof(pixels));
        }
      }
      if (mode) assert(ArRenderDevice_EndTextureUploads(device,true));
      assert(probe.calls==(mode==1 ? 1u : 6u));
      assert(!backend->upload_open && !backend->upload_commands && !backend->upload_copy);
      assert(ArSdlRenderBackend_SetExternalFlush(device,NULL,&probe));
      for (unsigned i=0; i<2; ++i) {
        const SDL_FRect destination={(float)((i+2)*W),0,W,H};
        assert(SDL_RenderTexture(renderer,ArSdlRenderBackend_UnwrapTexture(sources[i]),NULL,&destination));
      }
      SDL_Surface *actual=ReadUploadTarget(renderer);
      assert(actual->w==W*4 && actual->h==H);
      for (int y=0; y<H; ++y) for (int x=0; x<W*4; ++x) {
        Uint32 pixel;memcpy(&pixel,(Uint8 *)actual->pixels+y*actual->pitch+x*4,4);
        const unsigned panel=(unsigned)x/W;
        assert(pixel==(panel<2 ? UploadColor(frame+panel,x%W,y) : expected[panel-2][y][x%W]));
      }
      if (!mode) reference=actual;
      else {
        for (int y=0; y<H; ++y)
          assert(!memcmp((Uint8 *)reference->pixels+y*reference->pitch,
                         (Uint8 *)actual->pixels+y*actual->pitch,W*4*4));
        SDL_DestroySurface(actual);
      }
    }
    SDL_DestroySurface(reference);
  }
  backend->reuse_texture_uploads=true;
  for (unsigned i=0; i<2; ++i) ArRenderDevice_DestroyTexture(device,sources[i]);
  assert(ArRenderDevice_SetRenderTarget(device,ArRenderTexture_Invalid()));
  SDL_DestroyTexture(target);
}

static void CheckUploadCancellation(ArRenderDevice *device) {
  SDL_Renderer *renderer=ArSdlRenderBackend_Renderer(device);
  ArSdlRenderBackend *backend=device->context;
  const SDL_PixelFormatDetails *details=SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_ARGB8888);
  const ArRenderTextureDesc desc={.width=W,.height=H,.format=kArRenderPixelFormat_Argb8888,
    .usage=kArRenderTextureUsage_Streaming,.filter=kArRenderFilter_Nearest,.blend=kArRenderBlendMode_Opaque};
  ArRenderTexture source;
  assert(ArRenderDevice_CreateTexture(device,&desc,&source));
  SDL_Texture *target=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_TARGET,W,H);
  assert(target && SDL_SetRenderTarget(renderer,target));
  Uint32 pixels[H][Pitch];
  UploadFlushProbe empty={0};
  assert(ArSdlRenderBackend_SetExternalFlush(device,UploadFlush,&empty));
  assert(ArRenderDevice_BeginTextureUploads(device));
  assert(ArRenderDevice_EndTextureUploads(device,true));
  assert(empty.calls==0 && !backend->upload_commands && !backend->upload_copy);
  assert(ArSdlRenderBackend_SetExternalFlush(device,NULL,&empty));

  for (unsigned failure=0; failure<10; ++failure) {
    FillUpload(pixels,details,failure);
    assert(ArRenderDevice_UpdateTexture(device,source,NULL,pixels,sizeof(pixels[0])));
    UploadFlushProbe probe={.fail=failure==2};
    assert(ArSdlRenderBackend_SetExternalFlush(device,UploadFlush,&probe));
    assert(ArRenderDevice_BeginTextureUploads(device));
    FillUpload(pixels,details,failure+71);
    const bool updated=ArRenderDevice_UpdateTexture(device,source,NULL,pixels,sizeof(pixels[0]));
    assert(updated==(failure!=2));
    const ArRenderRectI bad={W-1,0,2,1},viewport={0,0,W,H};
    ArRenderTexture forbidden;
    switch(failure) {
      case 0: break; /* Explicit cancel after a valid deferred update. */
      case 1: assert(!ArRenderDevice_UpdateTexture(device,source,&bad,pixels,sizeof(pixels[0])));break;
      case 2: assert(probe.calls==1);break; /* Prior owner could not drain. */
      case 3: assert(!ArRenderDevice_DrawTexture(device,source,NULL,NULL));break;
      case 4: assert(!ArRenderDevice_SetRenderTarget(device,ArRenderTexture_Invalid()));break;
      case 5: assert(!ArRenderDevice_BeginTextureUploads(device));break;
      case 6: assert(!ArRenderDevice_UpdateTexture(device,source,NULL,pixels,0));break;
      case 7: ArRenderDevice_DestroyTexture(device,source);break; /* Rejected, still owned. */
      case 8: assert(!ArRenderDevice_CreateTexture(device,&desc,&forbidden));break;
      case 9: assert(!ArRenderDevice_SetViewport(device,&viewport));break;
    }
    if (failure==0 || failure==6) assert(ArRenderDevice_EndTextureUploads(device,false));
    else assert(!ArRenderDevice_EndTextureUploads(device,true));
    assert(!backend->upload_open && !backend->upload_failed && !backend->upload_commands && !backend->upload_copy);
    const bool detached=ArSdlRenderBackend_SetExternalFlush(device,NULL,&probe);
    assert(detached==(failure!=2));
    assert(SDL_GetRenderTarget(renderer)==target);
    assert(SDL_RenderTexture(renderer,ArSdlRenderBackend_UnwrapTexture(source),NULL,NULL));
    SDL_Surface *read=ReadUploadTarget(renderer);
    for (int y=0; y<H; ++y) for (int x=0; x<W; ++x) {
      Uint32 actual;memcpy(&actual,(Uint8 *)read->pixels+y*read->pitch+x*4,4);
      assert(actual==UploadColor(failure,x,y));
    }
    SDL_DestroySurface(read);
    /* The same backend and staging buffers must recover on the next scope. */
    assert(ArRenderDevice_BeginTextureUploads(device));
    assert(ArRenderDevice_UpdateTexture(device,source,NULL,pixels,sizeof(pixels[0])));
    assert(ArRenderDevice_EndTextureUploads(device,true));
    assert(SDL_RenderTexture(renderer,ArSdlRenderBackend_UnwrapTexture(source),NULL,NULL));
    read=ReadUploadTarget(renderer);
    for (int y=0; y<H; ++y) for (int x=0; x<W; ++x) {
      Uint32 actual;memcpy(&actual,(Uint8 *)read->pixels+y*read->pitch+x*4,4);
      assert(actual==UploadColor(failure+71,x,y));
    }
    SDL_DestroySurface(read);
  }
  assert(!ArRenderDevice_EndTextureUploads(device,true));
  ArRenderDevice_DestroyTexture(device,source);
  assert(ArRenderDevice_SetRenderTarget(device,ArRenderTexture_Invalid()));
  SDL_DestroyTexture(target);
  puts("upload batches: 16 scalar/batched/fallback pixel-exact triples; 10 cancellation/recovery cases");
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
  CheckUploadBatches(&device,kArRenderPixelFormat_Argb8888);
  CheckUploadBatches(&device,kArRenderPixelFormat_Abgr8888);
  CheckUploadCancellation(&device);
  CheckPreparedPresentation(&device,window);
  ArSdlRenderBackend_Destroy(&device);SDL_DestroyWindow(window);SDL_Quit();
  puts("render_sdl_upload_test: PASS");return 0;
}
