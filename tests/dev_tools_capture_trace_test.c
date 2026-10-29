/* Exercise the actual PNG/legacy PPM capture functions. Only their producer/presenter/
 * readback dependencies are stubs; no GPU claim is made. */
#include "dev/dev_tools.h"
#include "host/host_display.h"
#include "present/presentation_frame_generation.h"
#include "render/image_write.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

static int captured, uploaded, presented, readbacks, released;
static bool fatal, fatal_upload, fatal_present;
static float expected_phase;
static const FrameSlot *drawn_slot;
static const ArRenderRectI actual_viewport = {1, 0, 2, 2};
static uint8_t rgb[24];

void Diorama_CaptureCameraPresentationState(DioramaCameraPresentationState *state) {
  *state = (DioramaCameraPresentationState){.zoom_offset = 1.25f};
}

void Sim3DCamera_CapturePresentationState(Sim3DCameraPresentationState *state) {
  *state = (Sim3DCameraPresentationState){.mode = 1, .orbit_yaw = .25f, .orbit_pitch = -.5f};
}

void FrameSlot_Capture(FrameSlot *slot, const SimFrameData *sim) {
  assert(!sim);
  ++captured;
  *slot = (FrameSlot){.snes_width = 256, .snes_height = 224, .visible_width = 256,
      .sim = {.view = kSimView_Enhanced}};
}
void PresentUpload(const FrameSlot *slot) {
  assert(slot->diorama_camera.controls.zoom_offset == 1.25f);
  assert(slot->sim_camera.mode == 1 && slot->sim_camera.orbit_yaw == .25f &&
      slot->sim_camera.orbit_pitch == -.5f);
  ++uploaded;
  drawn_slot = slot;
  if (fatal_upload) fatal = true;
}
ArRenderRectI PresentFrame(const FrameSlot *slot, float alpha, double fps) {
  assert(slot == drawn_slot && alpha == expected_phase && fps == 60);
  ++presented;
  if (fatal_present) fatal = true; /* Renderer latches fatal, still returns a rect. */
  return actual_viewport;
}
double HostDisplay_FramesPerSecond(void) { return 60; }
int Settings_VisibleWidth(void) { return 2; }
int Settings_VisibleX0(void) { return 0; }
bool SessionFatal_Requested(void) { return fatal; }

static void Release(void *owner) {
  assert(owner == rgb);
  ++released;
}
static bool Readback(void *context, DevToolsRgb24Capture *out) {
  assert(context == rgb && presented == 1);
  ++readbacks;
  *out = (DevToolsRgb24Capture){
    .pixels = rgb, .width = 4, .height = 2, .pitch_bytes = 12,
    .owner = rgb, .release = Release,
  };
  return true;
}

static void Reset(void) {
  captured = uploaded = presented = readbacks = released = 0;
  fatal = fatal_upload = fatal_present = false;
  expected_phase = kPresentationFrameGenerationPhaseNone;
}

int main(void) {
  static uint8_t native[16];
  DevToolsContext context = {
    .readback = {.capture_rgb24 = Readback, .context = rgb}, .hud_bg_texture = {1},
    .framebuffer_pixels = native, .framebuffer_pitch = 8, .snes_height = 2,
  };
  memset(rgb, 0x73, sizeof(rgb));

  /* An ordinary composite is read back once and written as a PPM. */
  Reset();
  FILE *file = tmpfile();
  assert(file);
  DevToolsCaptureResult result = DevTools_WriteFramebufferPpm(file, &context, true);
  assert(result.kind == kDevToolsCapture_Composite);
  assert(result.width == 4 && result.height == 2);
  assert(captured == 1 && uploaded == 1 && presented == 1);
  assert(readbacks == 1 && released == 1);
  rewind(file);
  char ppm[64] = {0};
  const size_t bytes = fread(ppm, 1, sizeof(ppm), file);
  assert(bytes == 35 && !memcmp(ppm, "P6\n4 2\n255\n", 11));
  assert(!memcmp(ppm + 11, rgb, 24));
  assert(fclose(file) == 0);

  /* PNG preserves the exact readback RGB without raw-frame disk growth. */
  Reset();file=tmpfile();assert(file);
  result=DevTools_WriteFramebufferPng(file,&context,true);
  assert(result.kind==kDevToolsCapture_Composite&&readbacks==1&&released==1);
  long png_size=ftell(file);assert(png_size>8&&png_size<4096);
  unsigned char png[4096];rewind(file);
  assert(fread(png,1,(size_t)png_size,file)==(size_t)png_size);
  assert(!memcmp(png,"\x89PNG\r\n\x1a\n",8));
  SDL_Surface *decoded=SDL_LoadPNG_IO(SDL_IOFromConstMem(png,(size_t)png_size),true);
  assert(decoded&&decoded->w==4&&decoded->h==2);
  for(int y=0;y<2;++y)for(int x=0;x<4;++x) {
    Uint8 r,g,b,a;assert(SDL_ReadSurfacePixel(decoded,x,y,&r,&g,&b,&a));
    assert(r==0x73&&g==0x73&&b==0x73&&a==255);
  }
  SDL_DestroySurface(decoded);assert(fclose(file)==0);

  /* Solid diagnostic frames must really compress, rather than merely wrapping
   * raw pixels in PNG stored blocks. Preserve nonopaque scene-asset alpha too. */
  static uint8_t rgba[128 * 64 * 4];
  for (unsigned i=0;i<sizeof(rgba);i+=4) {
    rgba[i]=0x12;rgba[i+1]=0x34;rgba[i+2]=0x56;rgba[i+3]=0x78;
  }
  file=tmpfile();assert(file);
  assert(ArImage_WritePng(file,rgba,128,64,128*4,kArImageRgba32));
  png_size=ftell(file);assert(png_size>8&&png_size<(long)sizeof(rgba)/8);
  rewind(file);assert(fread(png,1,(size_t)png_size,file)==(size_t)png_size);
  decoded=SDL_LoadPNG_IO(SDL_IOFromConstMem(png,(size_t)png_size),true);
  assert(decoded&&decoded->w==128&&decoded->h==64);
  Uint8 r,g,b,a;assert(SDL_ReadSurfacePixel(decoded,127,63,&r,&g,&b,&a));
  assert(r==0x12&&g==0x34&&b==0x56&&a==0x78);
  SDL_DestroySurface(decoded);assert(fclose(file)==0);

  /* Native fallback is an opaque screenshot even when the framebuffer's
   * unused alpha byte is zero; host-endian ARGB still yields the correct RGB. */
  Reset();file=tmpfile();assert(file);
  DevToolsContext native_context=context;native_context.readback.capture_rgb24=NULL;
  const uint32_t sample[]={0x00123456,0x00789abc,0x00abcdef,0x00345678};
  memcpy(native,sample,sizeof(sample));
  result=DevTools_WriteFramebufferPng(file,&native_context,false);
  assert(result.kind==kDevToolsCapture_NativeFramebuffer&&!readbacks);
  png_size=ftell(file);assert(png_size>8&&png_size<4096);rewind(file);
  assert(fread(png,1,(size_t)png_size,file)==(size_t)png_size);
  decoded=SDL_LoadPNG_IO(SDL_IOFromConstMem(png,(size_t)png_size),true);
  assert(decoded&&decoded->w==2&&decoded->h==2);
  for(int y=0;y<2;++y)for(int x=0;x<2;++x) {
    Uint8 r,g,b,a;assert(SDL_ReadSurfacePixel(decoded,x,y,&r,&g,&b,&a));
    uint32_t expected=sample[y*2+x];
    assert(r==(expected>>16)&&g==((expected>>8)&255)&&b==(expected&255)&&a==255);
  }
  SDL_DestroySurface(decoded);assert(fclose(file)==0);

  /* Fatal render with a nonempty viewport: failed, no readback, no native
   * fallback (even when allowed), and no PPM bytes written. */
  for (int allow_native = 0; allow_native < 2; ++allow_native) {
    Reset();
    fatal_present = true;
    file = tmpfile();
    assert(file);
    result = DevTools_WriteFramebufferPpm(file, &context, !allow_native);
    assert(result.kind == kDevToolsCapture_Failed && !result.width && !result.height);
    assert(captured == 1 && uploaded == 1 && presented == 1 && fatal);
    assert(!readbacks && !released);
    assert(fflush(file) == 0 && ftell(file) == 0);
    assert(fclose(file) == 0);
  }
  /* A resident/interpolated capture borrows an already uploaded frame. It
   * must obey the same failure contract without capturing or uploading again. */
  const FrameSlot retained = {.snes_width = 256, .snes_height = 224};
  for (int fail = 0; fail < 2; ++fail) {
    Reset();
    drawn_slot = &retained;
    expected_phase = .5f;
    fatal_present = fail;
    file = tmpfile();
    assert(file);
    result = DevTools_WriteFramebufferPpmAtPhase(file, &context, false, .5f, &retained);
    assert(!captured && !uploaded && presented == 1);
    assert(result.kind == (fail ? kDevToolsCapture_Failed : kDevToolsCapture_Composite));
    assert(readbacks == !fail && released == !fail);
    if (fail) assert(fflush(file) == 0 && ftell(file) == 0);
    assert(fclose(file) == 0);
  }
  /* Do not submit a failed upload or start a capture after session failure. */
  for (int already_fatal = 0; already_fatal < 2; ++already_fatal) {
    Reset();
    fatal = already_fatal;
    fatal_upload = !already_fatal;
    file = tmpfile();
    assert(file);
    result = DevTools_WriteFramebufferPpm(file, &context, false);
    assert(result.kind == kDevToolsCapture_Failed && !result.width && !result.height);
    assert(captured == !already_fatal && uploaded == !already_fatal);
    assert(!presented && !readbacks && !released);
    assert(fflush(file) == 0 && ftell(file) == 0);
    assert(fclose(file) == 0);
  }
  puts("dev_tools_capture_trace_test: PASS");
  return 0;
}
