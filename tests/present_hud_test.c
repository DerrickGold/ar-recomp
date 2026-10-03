#include "render/present_hud.h"

#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>
#include "app/session_fatal.h"
#include "present/present.h"
#include "render/localized_text_presenter.h"
#include "sim/menu/present_sim_menu.h"

static ArRenderDevice s_device;
static FrameSlot slot;
static struct {
  ArRenderTexture target;
  int created, destroyed, native_draws, composite_draws, flattened, restores;
  int fatals, ready, text_resets;
  bool fail_create, fail_bind, fail_viewport, fail_restore, fail_clear, fail_flatten, fail_text;
  int source_created, source_destroyed, source_attempts, fail_source_number;
  bool source_alive[32], fail_update;
  int updates[2], pitches[2];
  ArRenderRectI upload[2];
  uint32_t first_pixel[2];
  float brightness;
} backend;
static bool menu_active;

bool PresentSimMenu_Active(const FrameSlot *frame) { (void)frame; return menu_active; }
void SessionFatal_Request(const char *format, ...) { assert(format); backend.fatals++; }
void ArTextPresentation_MarkReady(uint64_t ticket) { assert(ticket == 42); backend.ready++; }
void ArTextPresentation_ReportPage(uint64_t ticket, uint32_t start, uint32_t end) {
  (void)ticket;
  (void)start;
  (void)end;
}
void ArLocalizedTextPresenter_Reset(ArRenderDevice *device) {
  assert(device == &s_device);
  backend.text_resets++;
}
void ArLocalizedTextPresenter_Prepare(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    bool valid, uint16_t base, unsigned map_width, unsigned map_height,
    uint16_t hscroll, uint16_t vscroll, unsigned width, unsigned height,
    const HudPresentationChunk *chunks, size_t count, ArLocalizedPreparedFrame *out) {
  assert(device == &s_device && frame == &slot.localization && chunks && count);
  assert(width == 256 && height == 224);
  (void)valid;
  (void)base;
  (void)map_width;
  (void)map_height;
  (void)hscroll;
  (void)vscroll;
  *out = (ArLocalizedPreparedFrame){.ready_dialogue_ticket = 42};
}
bool ArLocalizedTextPresenter_DrawWithBrightness(
    ArRenderDevice *device, const ArLocalizedPreparedFrame *prepared, float brightness) {
  assert(device == &s_device && prepared->ready_dialogue_ticket == 42);
  assert(backend.target.value);
  backend.brightness = brightness;
  return !backend.fail_text;
}

static bool Create(void *context, const ArRenderTextureDesc *desc, ArRenderTexture *out) {
  (void)context;
  if (desc->usage == kArRenderTextureUsage_Streaming) {
    assert(desc->width == SR_PPU_SURFACE_MAX_WIDTH && desc->height == 224);
    assert(desc->format == kArRenderPixelFormat_Argb8888);
    assert(desc->filter == kArRenderFilter_Nearest && desc->blend == kArRenderBlendMode_Alpha);
    if (++backend.source_attempts == backend.fail_source_number) return false;
    const int index = ++backend.source_created;
    assert(index < 32);
    backend.source_alive[index] = true;
    *out = (ArRenderTexture){100u + (uintptr_t)index};
    return true;
  }
  assert(desc->usage == kArRenderTextureUsage_Target && desc->width > 0 && desc->height > 0);
  if (backend.fail_create) return false;
  *out = (ArRenderTexture){(uintptr_t)++backend.created};
  return true;
}
static void Destroy(void *context, ArRenderTexture texture) {
  (void)context;
  if (texture.value > 100) {
    const uintptr_t index = texture.value - 100;
    assert(index < 32 && backend.source_alive[index]);
    backend.source_alive[index] = false;
    backend.source_destroyed++;
    return;
  }
  assert(texture.value && texture.value <= (uintptr_t)backend.created);
  backend.destroyed++;
}
static bool Update(void *context, ArRenderTexture texture, const ArRenderRectI *rect,
                   const void *pixels, int pitch) {
  (void)context;
  assert(rect && pixels && pitch > 0);
  const int index = texture.value == PresentHud_BackgroundTexture().value ? 0 : 1;
  if (index) assert(texture.value == PresentHud_ObjectTexture().value);
  backend.updates[index]++;
  backend.upload[index] = *rect;
  backend.pitches[index] = pitch;
  memcpy(&backend.first_pixel[index], pixels, sizeof(uint32_t));
  return !backend.fail_update;
}
static bool Target(void *context, ArRenderTexture target) {
  (void)context;
  if (backend.fail_bind) return false;
  backend.target = target;
  return true;
}
static bool Coordinates(void *context) { (void)context; return true; }
static bool Viewport(void *context, const ArRenderRectI *rect) {
  (void)context;
  (void)rect;
  return !backend.fail_viewport;
}
static bool Clip(void *context, const ArRenderRectI *rect) {
  (void)context;
  (void)rect;
  return true;
}
static bool Capture(void *context, ArRenderTargetState *out) {
  (void)context;
  *out = (ArRenderTargetState){.target = backend.target, .valid = true};
  return true;
}
static bool Restore(void *context, const ArRenderTargetState *state) {
  (void)context;
  backend.restores++;
  if (backend.fail_restore) return false;
  backend.target = state->target;
  return true;
}
static bool Clear(void *context, ArRenderColorF color) {
  (void)context;
  assert(color.a == 0 && color.r == 0 && color.g == 0 && color.b == 0);
  return !backend.fail_clear;
}
static bool Draw(void *context, ArRenderTexture texture, const ArRenderRectF *source,
                 const ArRenderRectF *dest, const ArRenderDrawState *state) {
  (void)context;
  assert(dest && dest->w > 0 && dest->h > 0);
  if (texture.value < 100) {
    assert(!backend.target.value && !source);
    assert(state && state->blend == kArRenderBlendMode_AlphaPremultiplied);
    backend.flattened++;
    return !backend.fail_flatten;
  }
  assert(source && source->w > 0 && source->h > 0);
  if (backend.target.value) backend.composite_draws++;
  else backend.native_draws++;
  return true;
}
static const char *Error(void *context) { (void)context; return "injected target failure"; }
static const ArRenderBackendOps ops = {
  .struct_size = sizeof(ArRenderBackendOps),
  .create_texture = Create, .destroy_texture = Destroy, .update_texture = Update,
  .set_render_target = Target, .use_output_coordinates = Coordinates,
  .set_viewport = Viewport, .set_clip_rect = Clip,
  .capture_render_target_state = Capture, .restore_render_target_state = Restore,
  .clear = Clear, .draw_texture = Draw, .last_error = Error,
};
static const ArRenderRectI viewport = {100, 50, 960, 840};

static void ResetCase(void) {
  PresentHud_Reset(&s_device);
  PresentHud_DestroySources(&s_device);
  memset(&backend, 0, sizeof(backend));
  memset(&slot, 0, sizeof(slot));
  menu_active = false;
  slot.snes_width = 512;
  slot.snes_height = 224;
  slot.visible_width = 256;
  slot.visible_x0 = 128;
  slot.hud_split_height = 32;
  slot.hud_left_end = 64;
  slot.hud_right_start = 192;
  slot.hud_player_row_y = 8;
  slot.hud_left_only_y = 16;
  slot.extra_left_right = 128;
  slot.overlay_captures[kFrameSlotOverlay_Bg3].y1 = 64;
  slot.inidisp = 15;
  assert(PresentHud_CreateSources(&s_device, slot.snes_height));
}

/* A promoted spell icon may have only 32 rows even while the diorama scene
 * owns a full-height OBJ capture. Use distinct pixels to verify source priority
 * as well as extent; a successful upload of the wrong surface is still a bug. */
static uint32_t bg_pixels[512 * 224], obj_pixels[512 * 224];
static uint32_t sim_bg_pixels[512 * 224], sim_obj_pixels[512 * 224];
static uint32_t icon_pixels[512 * 32];

static SrPpuSurfaceView Surface(uint32_t *pixels, unsigned rows) {
  return (SrPpuSurfaceView){
    .data = (uint8_t *)pixels, .byte_size = 512u * rows * sizeof(uint32_t),
    .pitch_bytes = 512u * sizeof(uint32_t), .width_pixels = 512, .height_pixels = rows,
    .pixel_format = SR_PPU_PIXEL_FORMAT_ARGB8888_U32, .flags = SR_PPU_SURFACE_BOUND,
  };
}

static uint64_t UploadFrame(void) {
  const PresentHudUploadResult result = PresentHud_Upload(&s_device, &slot);
  return result.background_bytes + result.object_bytes;
}

static void SourceSelectionAndExtents(void) {
  ResetCase();
  bg_pixels[0] = 0xff010101;
  obj_pixels[0] = 0xff020202;
  sim_bg_pixels[0] = 0xff030303;
  sim_obj_pixels[0] = 0xff040404;
  icon_pixels[0] = 0xff050505;
  slot.ppu_surfaces.overlays[SR_PPU_OVERLAY_BG3][0] = Surface(bg_pixels, 224);
  slot.ppu_surfaces.overlays[SR_PPU_OVERLAY_OBJ][0] = Surface(obj_pixels, 224);
  slot.overlay_captures[kFrameSlotOverlay_Obj].y1 = 224;
  slot.hud_obj_surface = Surface(icon_pixels, 32);
  slot.hud_icon_rows = 32;
  slot.diorama_active = true;
  const PresentHudUploadResult uploaded = PresentHud_Upload(&s_device, &slot);
  assert(uploaded.background_bytes == 512u * 64 * 4);
  assert(uploaded.object_bytes == 512u * 32 * 4);
  assert(backend.first_pixel[0] == bg_pixels[0] && backend.first_pixel[1] == icon_pixels[0]);
  assert(backend.upload[0].w == 512 && backend.upload[0].h == 64);
  assert(backend.upload[1].h == 32 && backend.pitches[1] == 512 * 4);
  assert(UploadFrame() == 0); /* Retained contents skip transfer. */
  assert(backend.updates[0] == 1 && backend.updates[1] == 1);

  slot.sim3d_output_surfaces.hud_bg = Surface(sim_bg_pixels, 224);
  slot.sim3d_output_surfaces.hud_obj = Surface(sim_obj_pixels, 224);
  assert(UploadFrame() > 0);
  assert(backend.first_pixel[0] == sim_bg_pixels[0]);
  assert(backend.first_pixel[1] == sim_obj_pixels[0] && backend.upload[1].h == 224);
  slot.sim3d_output_surfaces.hud_bg = (SrPpuSurfaceView){0};
  slot.sim3d_output_surfaces.hud_obj = (SrPpuSurfaceView){0};
  slot.hud_obj_surface = (SrPpuSurfaceView){0};
  assert(UploadFrame() > 0);
  assert(backend.first_pixel[0] == bg_pixels[0] && backend.first_pixel[1] == obj_pixels[0]);

  /* Detached BG3 body text still uploads without a status-bar split. */
  slot.hud_split_height = 0;
  bg_pixels[0]++;
  assert(UploadFrame() == 0);
  slot.overlay_captures[kFrameSlotOverlay_Bg3].flags = kFrameSlotOverlayFlag_RemoveFromGame;
  assert(UploadFrame() == 4);
  assert(backend.first_pixel[0] == bg_pixels[0]);
}

static void SourceLifetimeAndFailures(void) {
  ResetCase();
  slot.ppu_surfaces.overlays[SR_PPU_OVERLAY_BG3][0] = Surface(bg_pixels, 224);
  const uint64_t full = 512u * 64 * 4;
  assert(UploadFrame() == full);
  bg_pixels[5 * 512 + 9]++;
  backend.fail_update = true;
  assert(UploadFrame() == 0);
  assert(backend.upload[0].x == 9 && backend.upload[0].y == 5);
  backend.fail_update = false;
  assert(UploadFrame() == full); /* Failed upload retries in full. */
  const ArRenderTexture background = PresentHud_BackgroundTexture();
  PresentHud_Reset(&s_device);
  assert(!backend.source_destroyed && PresentHud_BackgroundTexture().value == background.value);
  assert(UploadFrame() == full); /* Device reset invalidated mirror. */
  PresentHud_DestroySources(&s_device);
  assert(backend.source_destroyed == 2 && !PresentHud_BackgroundTexture().value);
  assert(!PresentHud_ObjectTexture().value && UploadFrame() == 0);
  assert(PresentHud_CreateSources(&s_device, 224));
  assert(PresentHud_BackgroundTexture().value != background.value);
  assert(UploadFrame() == full);

  /* A bound view with a truncated buffer must never reach the backend. */
  bg_pixels[0]++;
  slot.ppu_surfaces.overlays[SR_PPU_OVERLAY_BG3][0].byte_size--;
  const int updates = backend.updates[0];
  assert(UploadFrame() == 0 && backend.updates[0] == updates);
  PresentHud_DestroySources(&s_device);
  assert(backend.source_created == backend.source_destroyed);
  for (int allocation = 1; allocation <= 2; allocation++) {
    backend.fail_source_number = backend.source_attempts + allocation;
    assert(!PresentHud_CreateSources(&s_device, 224));
    assert(!PresentHud_BackgroundTexture().value && !PresentHud_ObjectTexture().value);
    assert(backend.source_created == backend.source_destroyed);
  }
  backend.fail_source_number = 0;
  assert(PresentHud_CreateSources(&s_device, 224));
  PresentHud_DestroySources(&s_device);
  PresentHud_DestroySources(&s_device);
  assert(backend.source_created == backend.source_destroyed);
}

int main(void) {
  /* Use the real device scopes and layout; only backend I/O and text preparation
   * are substituted so loss of a target can be exercised on every platform. */
  s_device = (ArRenderDevice){.ops = &ops, .context = &backend,
      .capabilities = {.flags = kArRenderCapability_StreamingTextures |
                               kArRenderCapability_RenderTargets |
                               kArRenderCapability_ScopedRenderTargets}};
  SourceSelectionAndExtents();
  SourceLifetimeAndFailures();
  ResetCase();
  HudPresentationChunk chunks[kHudPresentationChunkCapacity];
  assert(PresentHud_BuildChunks(&slot, viewport, chunks) == 7);
  menu_active = true;
  assert(PresentHud_BuildChunks(&slot, viewport, chunks) == 6);
  slot.bg3_state_valid=true;
  slot.bg3_tilemap_width_tiles=slot.bg3_tilemap_height_tiles=32;
  slot.overlay_captures[kFrameSlotOverlay_Bg3].y1=224;
  assert(PresentHud_BuildChunks(&slot, viewport, chunks)==6);
  slot.sim_menu.preserved_bg3_region=(ArTextCellRegion){2,8,28,1};
  assert(PresentHud_BuildChunks(&slot, viewport, chunks)==7);
  assert(chunks[6].screen_source.x==16 && chunks[6].screen_source.y==63);
  assert(chunks[6].screen_source.w==224 && chunks[6].screen_source.h==8);
  assert(chunks[6].output_destination.x + chunks[6].output_destination.w / 2 ==
         viewport.x + viewport.w / 2);
  assert(chunks[6].texture_source.x==144); /* Texture widescreen padding. */
  slot.visible_top=37;
  slot.visible_height=298;
  const ArRenderRectI tall_viewport={100,50,896,894};
  assert(PresentHud_BuildChunks(&slot,tall_viewport,chunks)==7);
  assert(chunks[6].output_destination.x==156 && chunks[6].output_destination.y==350);
  assert(chunks[6].output_destination.w==784 && chunks[6].output_destination.h==24);
  slot.visible_top=slot.visible_height=0;
  slot.bg3_vscroll=252; /* Native -4 scroll still follows its own status row. */
  assert(PresentHud_BuildChunks(&slot, viewport, chunks)==7);
  assert(chunks[6].screen_source.y==67);
  /* Projection follows captured ownership, never an embedded game region. */
  slot.sim_menu.preserved_bg3_region=(ArTextCellRegion){4,10,24,1};
  assert(PresentHud_BuildChunks(&slot, viewport, chunks)==7);
  assert(chunks[6].screen_source.x==32 && chunks[6].screen_source.y==83);
  assert(chunks[6].screen_source.w==192 && chunks[6].screen_source.h==8);
  slot.bg3_state_valid=false;
  slot.bg3_vscroll=0;
  slot.overlay_captures[kFrameSlotOverlay_Bg3].y1=64;
  menu_active = false;
  slot.oam_valid = true;
  slot.hud_icon_first = 8;
  slot.hud_icon_count = 1;
  slot.oam[16] = (8u << 8) | 224u;
  assert(PresentHud_BuildChunks(&slot, viewport, chunks) == 8);
  assert(chunks[7].inspector_kind == kInspectorPresentation_HudObj);
  assert(chunks[7].texture_source.x == 352 && chunks[7].texture_source.y == 8);
  PresentHud_Draw(&s_device, &slot, viewport);
  assert(backend.created == 1 && backend.flattened == 1 && backend.ready == 1);
  assert(!backend.native_draws && backend.composite_draws == 8 && !backend.target.value);
  assert(backend.brightness == 1.0f);
  slot.inidisp = 0x80;
  PresentHud_DrawComposited(&s_device, &slot, viewport);
  assert(backend.created == 1 && backend.brightness == 0.0f);
  PresentHud_DrawComposited(&s_device, &slot, (ArRenderRectI){0, 0, 480, 420});
  assert(backend.created == 2 && backend.destroyed == 1);
  PresentHud_Reset(&s_device);
  assert(backend.destroyed == 2 && backend.text_resets == 1);

  ResetCase();
  slot.overlay_captures[kFrameSlotOverlay_Bg3].y1 = slot.hud_split_height;
  PresentHud_Draw(&s_device, &slot, viewport);
  assert(!backend.created && backend.native_draws == 6 && !backend.ready);
  ResetCase();
  backend.fail_create = true;
  PresentHud_DrawComposited(&s_device, &slot, viewport);
  assert(backend.native_draws == 7 && !backend.fatals && !backend.ready);
  ResetCase();
  backend.fail_bind = true;
  PresentHud_DrawComposited(&s_device, &slot, viewport);
  assert(backend.native_draws == 7 && !backend.fatals && !backend.ready);
  ResetCase();
  backend.fail_clear = true;
  PresentHud_DrawComposited(&s_device, &slot, viewport);
  assert(backend.native_draws == 7 && backend.restores == 1 && !backend.flattened);
  ResetCase();
  backend.fail_text = true;
  PresentHud_DrawComposited(&s_device, &slot, viewport);
  assert(backend.native_draws == 7 && backend.restores == 1 && !backend.flattened);
  assert(!backend.ready);
  ResetCase();
  backend.fail_flatten = true;
  PresentHud_DrawComposited(&s_device, &slot, viewport);
  assert(backend.native_draws == 7 && backend.flattened == 1 && !backend.ready);
  ResetCase();
  backend.fail_restore = true;
  PresentHud_DrawComposited(&s_device, &slot, viewport);
  assert(backend.fatals == 1 && !backend.native_draws && !backend.flattened && !backend.ready);
  ResetCase();
  backend.fail_viewport = backend.fail_restore = true;
  PresentHud_DrawComposited(&s_device, &slot, viewport);
  assert(backend.fatals == 1 && !backend.native_draws && !backend.composite_draws);
  PresentHud_Reset(&s_device);
  PresentHud_DestroySources(&s_device);
  puts("HUD presentation: uploads, source lifetime, captured layout, target lifetime, "
       "native fallback and fatal loss passed");
  return 0;
}
