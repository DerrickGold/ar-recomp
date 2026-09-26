#include "replacements/present_hd_replacements.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "present/present.h"

static FrameSlot s_slot;
static struct {
  int draws, uploads, pitch;
  bool has_source, tinted, fail_upload;
  ArRenderTexture texture;
  ArRenderRectF source, destination;
  ArRenderRectI upload;
  ArRenderColorF tint;
  const void *pixels;
} s_backend;

static bool Draw(void *context, ArRenderTexture texture, const ArRenderRectF *source,
                 const ArRenderRectF *destination, const ArRenderDrawState *state) {
  assert(context == &s_backend && destination);
  ++s_backend.draws;
  s_backend.texture = texture;
  s_backend.has_source = source != NULL;
  if (source) s_backend.source = *source;
  s_backend.destination = *destination;
  s_backend.tinted = state && (state->flags & kArRenderDrawState_Tint);
  if (s_backend.tinted) s_backend.tint = state->tint;
  return true;
}
static bool Upload(void *context, ArRenderTexture texture, const ArRenderRectI *rect,
                   const void *pixels, int pitch) {
  assert(context == &s_backend && rect);
  ++s_backend.uploads;
  s_backend.texture = texture;
  s_backend.upload = *rect;
  s_backend.pixels = pixels;
  s_backend.pitch = pitch;
  return !s_backend.fail_upload;
}
static const ArRenderBackendOps kOps = {
  .struct_size = sizeof(ArRenderBackendOps), .draw_texture = Draw, .update_texture = Upload,
};
static ArRenderDevice s_device = {.ops = &kOps, .context = &s_backend};

static void TestScreenReplacement(void) {
  s_slot = (FrameSlot){.snes_width = 320, .snes_height = 224,
      .visible_x0 = 24, .visible_width = 272, .inidisp = 7, .hd_entry_count = 1};
  s_slot.hd_entries[0] = (FrameSlotHdEntry){.active = true, .source = kFrameSlotOverlay_Bg1,
      .brightness_mod = true, .image_inset_left = 3, .texture = {42}};
  FrameSlotOverlayCapture *capture = &s_slot.overlay_captures[kFrameSlotOverlay_Bg1];
  *capture = (FrameSlotOverlayCapture){.x0 = 5, .y0 = 10, .x1 = 50, .y1 = 30,
      .flags = kFrameSlotOverlayFlag_RemoveFromGame};
  const ArRenderRectI viewport = {24, 13, 544, 448};
  PresentHdReplacements_DrawScreen(&s_device, &s_slot, viewport);
  assert(s_backend.draws == 1 && s_backend.texture.value == 42 && !s_backend.has_source);
  /* The three-pixel native erasure gutter must not shift replacement artwork. */
  assert(s_backend.destination.x == 56 && s_backend.destination.y == 33);
  assert(s_backend.destination.w == 84 && s_backend.destination.h == 40);
  assert(s_backend.tinted && s_backend.tint.r == 119 / 255.0f);
  assert(s_backend.tint.g == s_backend.tint.r && s_backend.tint.b == s_backend.tint.r);
  assert(s_backend.tint.a == 1);
  s_slot.hd_entries[0].brightness_mod = false;
  PresentHdReplacements_DrawScreen(&s_device, &s_slot, viewport);
  assert(s_backend.draws == 2 && s_backend.tint.r == 1);

  s_slot.inidisp |= 0x80;
  PresentHdReplacements_DrawScreen(&s_device, &s_slot, viewport);
  s_slot.inidisp = 15;
  capture->flags = 0;
  PresentHdReplacements_DrawScreen(&s_device, &s_slot, viewport);
  capture->flags = kFrameSlotOverlayFlag_RemoveFromGame;
  capture->x1 = capture->x0;
  PresentHdReplacements_DrawScreen(&s_device, &s_slot, viewport);
  capture->x1 = 50;
  s_slot.hd_entries[0].active = false;
  PresentHdReplacements_DrawScreen(&s_device, &s_slot, viewport);
  s_slot.hd_entries[0].active = true;
  s_slot.hd_entries[0].texture = ArRenderTexture_Invalid();
  PresentHdReplacements_DrawScreen(&s_device, &s_slot, viewport);
  assert(s_backend.draws == 2);
}

static void TestMode7Replacement(void) {
  const ArRenderTexture texture = {99};
  const ArRenderRectI viewport = {24, 13, 544, 448};
  s_slot.m7_active = true;
  enum { kWidth = 1280, kHeight = 896, kPitch = kWidth * sizeof(uint32_t) };
  uint8_t *pixels = calloc(kHeight, kPitch);
  assert(pixels);
  SrPpuSurfaceView *surface = &s_slot.ppu_surfaces.mode7;
  *surface = (SrPpuSurfaceView){.data = pixels, .byte_size = kHeight * kPitch,
      .pitch_bytes = kPitch, .width_pixels = kWidth, .height_pixels = kHeight,
      .flags = SR_PPU_SURFACE_BOUND, .pixel_format = SR_PPU_PIXEL_FORMAT_ARGB8888_U32};
  assert(PresentHdReplacements_UploadMode7(&s_device, texture, &s_slot) == 1088 * 896 * 4);
  assert(s_backend.uploads == 1 && s_backend.texture.value == 99);
  assert(s_backend.upload.x == 96 && s_backend.upload.y == 0);
  assert(s_backend.upload.w == 1088 && s_backend.upload.h == 896);
  assert(s_backend.pixels == pixels + 96 * 4 && s_backend.pitch == kPitch);
  PresentHdReplacements_DrawMode7(&s_device, texture, &s_slot, viewport);
  assert(s_backend.draws == 3 && s_backend.texture.value == 99);
  assert(s_backend.has_source && !s_backend.tinted);
  assert(s_backend.source.x == 96 && s_backend.source.y == 0);
  assert(s_backend.source.w == 1088 && s_backend.source.h == 896);
  assert(s_backend.destination.x == 24 && s_backend.destination.y == 13);
  assert(s_backend.destination.w == 544 && s_backend.destination.h == 448);

  s_backend.fail_upload = true;
  assert(!PresentHdReplacements_UploadMode7(&s_device, texture, &s_slot));
  s_backend.fail_upload = false;
  assert(s_backend.uploads == 2); /* Failed transfers report no successful bytes. */
  --surface->byte_size;
  assert(!PresentHdReplacements_UploadMode7(&s_device, texture, &s_slot));
  ++surface->byte_size;
  surface->flags = 0;
  assert(!PresentHdReplacements_UploadMode7(&s_device, texture, &s_slot));
  surface->flags = SR_PPU_SURFACE_BOUND;
  ++s_slot.snes_height;
  assert(!PresentHdReplacements_UploadMode7(&s_device, texture, &s_slot));
  --s_slot.snes_height;
  s_slot.m7_active = false;
  assert(!PresentHdReplacements_UploadMode7(&s_device, texture, &s_slot));
  PresentHdReplacements_DrawMode7(&s_device, texture, &s_slot, viewport);
  s_slot.m7_active = true;
  assert(!PresentHdReplacements_UploadMode7(&s_device, ArRenderTexture_Invalid(), &s_slot));
  PresentHdReplacements_DrawMode7(&s_device, ArRenderTexture_Invalid(), &s_slot, viewport);
  assert(s_backend.uploads == 2 && s_backend.draws == 3);
  free(pixels);
}

int main(void) {
  TestScreenReplacement();
  TestMode7Replacement();
  puts("HD replacement presentation: all passed");
  return 0;
}
