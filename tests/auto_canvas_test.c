/* ROM-free geometry and real software-renderer upload/readback. */
#include <SDL3/SDL.h>
#include <limits.h>
#include <stdio.h>
#include "support/test_assert.h"
#include "present/display_geometry.h"
#include "present/present.h"
#include "render/presentation_layout.h"

static void TestGeometry(void) {
  static const struct {
    int width, height;
    bool crt;
    int columns, rows;
  } cases[] = {
    {1080,1080,true,0,37}, {1240,1080,true,0,18},
    {1080,1080,false,0,16}, {1240,1080,false,1,0},
    {1920,1080,true,43,0}, {1920,1080,false,71,0},
    {4,3,true,0,0}, {256,224,false,0,0},
    {10000,100,true,120,0}, {100,10000,true,0,64},
    {INT_MAX,1,false,120,0}, {1,INT_MAX,true,0,64},
  };
  for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
    ActRaiserAutoCanvas canvas = {-1,-1};
    assert(DisplayGeometry_ResolveAutoCanvas(
        cases[i].width,cases[i].height,cases[i].crt,&canvas));
    assert(canvas.extra_columns == cases[i].columns);
    assert(canvas.extra_rows == cases[i].rows);
    assert(!canvas.extra_columns || !canvas.extra_rows);
  }
  ActRaiserAutoCanvas kept = {43,0};
  assert(!DisplayGeometry_ResolveAutoCanvas(0,1080,true,&kept));
  assert(!DisplayGeometry_ResolveAutoCanvas(1240,-1,true,&kept));
  assert(kept.extra_columns == 43 && kept.extra_rows == 0);
  assert(!DisplayGeometry_ResolveAutoCanvas(1,1,true,NULL));
}

static uint32_t Pixel(SDL_Surface *surface, int x, int y) {
  return *(const uint32_t *)((const uint8_t *)surface->pixels +
      (size_t)y * surface->pitch + x * 4);
}

static void TestCapturePlacement(void) {
  /* Requested 256-row square canvas. Top/bottom world bounds shrink capture,
   * not the canvas or the authentic 256x224 band. Source colours include
   * alpha=0, matching the opaque main PPU texture. */
  SDL_Surface *target = SDL_CreateSurface(256,256,SDL_PIXELFORMAT_ARGB8888);
  assert(target);
  SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(target);
  assert(renderer);
  SDL_Texture *texture = SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,
      SDL_TEXTUREACCESS_STREAMING,256,352);
  assert(texture);
  assert(SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_NONE));
  assert(SDL_SetTextureScaleMode(texture,SDL_SCALEMODE_NEAREST));
  static uint32_t pixels[256*352];
  static const int margins[][2] = {{16,16},{0,16},{16,0},{3,7},{0,0}};
  for (unsigned i=0;i<sizeof(margins)/sizeof(margins[0]);++i) {
    FrameSlot frame = {.snes_width=256,.snes_height=224,.visible_width=256,
        .visible_height=256,.visible_top=16,
        .ws_extra_top=margins[i][0],.ws_extra_bottom=margins[i][1]};
    const int rows = FrameSlot_CaptureHeight(&frame);
    for (int y=0;y<rows;++y) {
      const int screen_y=y-frame.ws_extra_top;
      const uint32_t colour=screen_y<0 ? 0x00ff0000u :
          screen_y>=224 ? 0x000000ffu : 0x0000ff00u;
      for (int x=0;x<256;++x) pixels[y*256+x]=colour;
    }
    const SDL_Rect upload={0,0,256,rows};
    assert(SDL_UpdateTexture(texture,&upload,pixels,256*4));
    assert(SDL_SetRenderDrawColor(renderer,0,0,0,255));
    assert(SDL_RenderClear(renderer));
    const ArRenderRectF placed=ArPresentationLayout_CaptureDestination(
        (ArRenderRectI){0,0,256,256},FrameSlot_VisibleHeight(&frame),
        frame.visible_top,rows,frame.ws_extra_top);
    assert(placed.y==16-frame.ws_extra_top && placed.h==rows);
    const SDL_FRect source={0,0,256,rows};
    const SDL_FRect destination={placed.x,placed.y,placed.w,placed.h};
    assert(SDL_RenderTexture(renderer,texture,&source,&destination));
    SDL_Surface *readback=SDL_RenderReadPixels(renderer,NULL);
    assert(readback);
    SDL_Surface *argb=SDL_ConvertSurface(readback,SDL_PIXELFORMAT_ARGB8888);
    assert(argb);
    for (int y=0;y<256;++y) {
      const uint32_t expected=y<16-frame.ws_extra_top ||
          y>=240+frame.ws_extra_bottom ? 0xff000000u :
          y<16 ? 0xffff0000u : y>=240 ? 0xff0000ffu : 0xff00ff00u;
      assert((Pixel(argb,128,y) & 0xffffffu)==(expected & 0xffffffu));
    }
    /* The independent native comparison crop retains all 224 authentic rows. */
    assert(frame.snes_height==224);
    assert((Pixel(argb,128,16) & 0xffffffu)==0x00ff00u);
    assert((Pixel(argb,128,239) & 0xffffffu)==0x00ff00u);
    SDL_DestroySurface(argb);
    SDL_DestroySurface(readback);
  }
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroySurface(target);
}

int main(void) {
  TestGeometry();
  assert(SDL_Init(0));
  TestCapturePlacement();
  SDL_Quit();
  puts("auto_canvas_test: PASS");
  return 0;
}
