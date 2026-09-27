#include "sim/menu/present_sim_menu.h"
#include "settings_overlay/settings_overlay_artwork.h"
#include "render/localized_text_presenter.h"
#include "sim/sim3d/sim3d_textures.h"
#include "host/host_video.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

ArRenderDevice g_render_device;
static FrameSlot slot;
static SettingsOverlayArtwork artwork;
static unsigned borders, prose, acknowledgements;
static int pixel_scale;
static bool enhanced, help;

const SettingsOverlayArtwork *SettingsOverlayArtwork_Get(void) { return &artwork; }
ArRenderTexture PresentHud_BackgroundTexture(void) { return (ArRenderTexture){2}; }
ArRenderTexture Sim3DTextures_Layer(int plane) { return (ArRenderTexture){10u+plane}; }
void SessionFatal_Request(const char *format, ...) { (void)format; assert(false); }
void ArTextPresentation_MarkReady(uint64_t ticket) {
  assert(ticket==42);
  acknowledgements++;
}
void ArTextPresentation_ReportPage(uint64_t ticket, uint32_t start, uint32_t end) {
  (void)ticket;
  (void)start;
  (void)end;
}
const ArLocalizationScreenTextRecord *ArLocalizationFrame_FindScreenText(
    const ArLocalizationFrame *frame, uint32_t surface) {
  (void)frame;
  (void)surface;
  return NULL;
}
static void PrepareScrolledPage(ArRenderRectI bounds, ArLocalizedPreparedFrame *out) {
  *out=(ArLocalizedPreparedFrame){.ready_dialogue_ticket=42};
  if (!enhanced) return;
  out->text_count=1;
  out->texts[0].viewport=bounds;
  out->texts[0].destination=(ArRenderRectI){bounds.x,bounds.y-100*pixel_scale,
      bounds.w,150*pixel_scale};
  out->texts[0].surface.ink_bounds=(ArRenderRectI){0,0,bounds.w,150*pixel_scale};
  out->mask_count=1;
  out->masks[0]=(ArRenderRectI){32,144,192,72};
}
void ArLocalizedTextPresenter_Prepare(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    bool valid, uint16_t base, unsigned map_w, unsigned map_h,
    uint16_t scroll_x, uint16_t scroll_y, unsigned width, unsigned height,
    const HudPresentationChunk *chunks, size_t count, ArLocalizedPreparedFrame *out) {
  assert(device==&g_render_device && frame==&slot.localization && count==1);
  assert(width==256 && height==224);
  (void)valid;
  (void)base;
  (void)map_w;
  (void)map_h;
  (void)scroll_x;
  (void)scroll_y;
  PrepareScrolledPage(chunks[0].output_destination,out);
}
bool ArLocalizedTextPresenter_PrepareScreenText(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    uint32_t surface, ArRenderRectI bounds, ArLocalizedPreparedFrame *out) {
  (void)device;
  (void)frame;
  if (surface!=700 || !help) return false;
  PrepareScrolledPage(bounds,out);
  return enhanced;
}
bool ArLocalizedTextPresenter_DrawWithBrightness(
    ArRenderDevice *device, const ArLocalizedPreparedFrame *out, float brightness) {
  assert(device==&g_render_device && brightness==1);
  if (enhanced) {
    assert(out->text_count==1);
    assert(out->texts[0].viewport.h==(help?56:72)*pixel_scale);
    assert(out->texts[0].destination.y==out->texts[0].viewport.y-100*pixel_scale);
  }
  prose++;
  return true;
}
static bool Create(void *context, const ArRenderTextureDesc *desc, ArRenderTexture *out) {
  (void)context;
  (void)desc;
  *out=(ArRenderTexture){1};
  return true;
}
static bool Update(void *context, ArRenderTexture texture, const ArRenderRectI *rect,
                   const void *pixels, int pitch) {
  (void)context;
  (void)texture;
  (void)rect;
  (void)pixels;
  (void)pitch;
  return true;
}
static bool Draw(void *context, ArRenderTexture texture, const ArRenderRectF *source,
                 const ArRenderRectF *dest, const ArRenderDrawState *state) {
  (void)context;
  (void)state;
  if (texture.value==10+kSim3DPlane_Bg2Low) {
    assert(source && dest);
    assert(source->x==24 && source->w==208);
    assert(dest->x==24*pixel_scale && dest->w==208*pixel_scale);
    assert(dest->y==source->y*pixel_scale && dest->h==source->h*pixel_scale);
    borders++;
  }
  return true;
}
static bool Clip(void *context, const ArRenderRectI *rect) {
  (void)context;
  (void)rect;
  return true;
}
static bool Geometry(void *context, ArRenderTexture texture,
    const ArRenderVertex2D *vertices, int vertex_count,
    const int32_t *indices, int index_count, const ArRenderDrawState *state) {
  (void)context;
  (void)texture;
  (void)vertices;
  (void)vertex_count;
  (void)indices;
  (void)index_count;
  (void)state;
  return true;
}
int main(void) {
  static const ArRenderBackendOps ops={.struct_size=sizeof(ArRenderBackendOps),
      .create_texture=Create,.update_texture=Update,.draw_texture=Draw,
      .set_clip_rect=Clip,.draw_geometry=Geometry};
  g_render_device=(ArRenderDevice){.ops=&ops,.context=&slot,
      .capabilities={.flags=kArRenderCapability_StreamingTextures}};
  for (pixel_scale=1;pixel_scale<=3;++pixel_scale)
    for (unsigned mode=0;mode<3;++mode) {
      memset(&slot,0,sizeof(slot));
      enhanced=mode!=0;
      help=mode==2;
      borders=prose=acknowledgements=0;
      slot.snes_width=slot.visible_width=256;
      slot.snes_height=224;
      slot.inidisp=15;
      slot.sim_menu.valid=true;
      slot.sim_menu.scale_percent=50;
      slot.sim_menu.help.active=help;
      SimMenuModel_Open(&slot.sim_menu.model,1,0xf337);
      slot.sim_menu.model.return_phase=kSimMenu_Browse;
      slot.sim_menu.model.phase=kSimMenu_Describe;
      slot.sim.separated_plane_mask=1u<<kSim3DPlane_Bg2Low;
      PresentSimMenu_Draw(&slot,(ArRenderRectI){0,0,256*pixel_scale,224*pixel_scale});
      assert(borders==3 && prose==1 && acknowledgements==1);
    }
  puts("SIM descriptions retain native box size, clipped scroll and acknowledgement");
  return 0;
}
