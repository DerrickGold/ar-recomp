#include "sim/menu/present_sim_menu.h"
#include "settings_overlay/settings_overlay_artwork.h"
#include "settings_overlay/settings_overlay_render.h"
#include "render/localized_text_presenter.h"
#include "sim/sim3d/sim3d_textures.h"
#include "host/host_video.h"

#include "support/test_assert.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

ArRenderDevice g_render_device;
static FrameSlot slot;
static SettingsOverlayArtwork artwork;
static unsigned borders, prose, acknowledgements;
static int pixel_scale;
static bool enhanced, help;
static bool progress, frame_seen, clipped;
static unsigned page, page_reports, body_prepares, choice_icons;
static ArRenderRectI body_bounds, clip_bounds;
static ArRenderRectF frame_bounds, choice_bounds[2];
static bool choice_selected[2];

const SettingsOverlayArtwork *SettingsOverlayArtwork_Get(void) { return &artwork; }
static unsigned system_labels, system_icons;
static bool system_icon_selected;
ArUiLocale SettingsOverlay_InterfaceLocale(void) { return kArUiLocale_English; }
bool SettingsOverlay_DrawEnhancedLabelAligned(ArRenderRectI bounds, uint8_t alpha,
                                               const char *text,
                                               ArUiTextAlignment alignment) {
  assert(bounds.w>0 && bounds.h>0 && alpha==255);
  assert(alignment==kArUiTextAlignment_Left);
  assert(strcmp(text,"System Settings")==0);
  system_labels++;
  return true;
}
ArRenderTexture PresentHud_BackgroundTexture(void) { return (ArRenderTexture){2}; }
ArRenderTexture Sim3DTextures_Layer(int plane) { return (ArRenderTexture){10u+plane}; }
void SessionFatal_Request(const char *format, ...) { (void)format; assert(false); }
void ArTextPresentation_MarkReady(uint64_t ticket) {
  if (!ticket) return;
  assert(ticket==42);
  acknowledgements++;
}
void ArTextPresentation_ReportPage(uint64_t ticket, uint32_t start, uint32_t end) {
  if (!ticket) return;
  assert(ticket==42);
  if (progress) assert(start==page*30 && end==page*30+24);
  page_reports++;
}
const ArLocalizationScreenTextRecord *ArLocalizationFrame_FindScreenText(
    const ArLocalizationFrame *frame, uint32_t surface) {
  (void)frame;
  (void)surface;
  return NULL;
}
static void PrepareScrolledPage(ArRenderRectI bounds, ArLocalizedPreparedFrame *out) {
  *out=(ArLocalizedPreparedFrame){.ready_dialogue_ticket=42};
  if (progress) {
    out->dialogue_page_start=page*30;
    out->dialogue_page_end=page*30+24;
  }
  if (!enhanced) return;
  out->text_count=1;
  out->texts[0].viewport=bounds;
  out->texts[0].destination=(ArRenderRectI){bounds.x,bounds.y-100*pixel_scale,
      bounds.w,150*pixel_scale};
  out->texts[0].surface.ink_bounds=(ArRenderRectI){0,0,bounds.w,150*pixel_scale};
  if (progress) out->texts[0].surface.ink_bounds.w=(page%2?176:32)*pixel_scale;
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
  if (progress && chunks[0].screen_source.x==144) {
    *out=(ArLocalizedPreparedFrame){0};
    return;
  }
  if (progress) {
    assert(chunks[0].screen_source.x==32 && chunks[0].screen_source.y==144);
    assert(chunks[0].screen_source.w==200);
    body_bounds=chunks[0].output_destination;
    body_bounds.w=body_bounds.w*192/200;
    body_prepares++;
  }
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
  if (progress && !out->ready_dialogue_ticket) return true;
  if (progress) {
    assert(clipped && memcmp(&body_bounds,&clip_bounds,sizeof(body_bounds))==0);
    assert(body_bounds.w==192*pixel_scale && body_bounds.h==72*pixel_scale);
  }
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
static void IncludeFrame(ArRenderRectF rect) {
  if (!frame_seen) {
    frame_bounds=rect;
    frame_seen=true;
    return;
  }
  const float left=fminf(frame_bounds.x,rect.x), top=fminf(frame_bounds.y,rect.y);
  const float right=fmaxf(frame_bounds.x+frame_bounds.w,rect.x+rect.w);
  const float bottom=fmaxf(frame_bounds.y+frame_bounds.h,rect.y+rect.h);
  frame_bounds=(ArRenderRectF){left,top,right-left,bottom-top};
}
static bool Draw(void *context, ArRenderTexture texture, const ArRenderRectF *source,
                 const ArRenderRectF *dest, const ArRenderDrawState *state) {
  (void)context;
  (void)state;
  if (progress && texture.value==artwork.dialog_frame.value) IncludeFrame(*dest);
  if (progress && texture.value==1 && source &&
      (source->y==41*16 || source->y==42*16)) {
    const unsigned index=source->y==41*16?0:1;
    choice_bounds[index]=*dest;
    choice_selected[index]=source->x==0;
    choice_icons++;
  }
  if (artwork.icons.value && texture.value==artwork.icons.value) {
    assert(source && source->x==kOverlayIcon_System*kIconSize && source->w==kIconSize);
    system_icon_selected=source->y==kIconSize;
    system_icons++;
  }
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
  clipped=rect!=NULL;
  if (rect) clip_bounds=*rect;
  return true;
}
static bool Geometry(void *context, ArRenderTexture texture,
    const ArRenderVertex2D *vertices, int vertex_count,
    const int32_t *indices, int index_count, const ArRenderDrawState *state) {
  (void)context;
  if (progress && texture.value==artwork.dialog_frame.value)
    for (int i=0;i<vertex_count;++i)
      IncludeFrame((ArRenderRectF){vertices[i].position.x,vertices[i].position.y,0,0});
  (void)indices;
  (void)index_count;
  (void)state;
  return true;
}
static void AssertInsideFrame(ArRenderRectF rect) {
  assert(rect.x>frame_bounds.x && rect.y>frame_bounds.y);
  assert(rect.x+rect.w<frame_bounds.x+frame_bounds.w);
  assert(rect.y+rect.h<frame_bounds.y+frame_bounds.h);
}
static void TestProgressLog(void) {
  progress=true;
  help=false;
  artwork.dialog_frame=(ArRenderTexture){3};
  /* Short/long revealed pages, both choices and acknowledgements/errors use
   * the same window, at every menu scale in narrow and wide viewports. */
  for (unsigned percent=50;percent<=100;percent+=25)
    for (unsigned wide=0;wide<2;++wide)
      for (unsigned mode=0;mode<2;++mode) {
        enhanced=mode!=0;
        pixel_scale=!wide && percent==100?3:percent/25;
        ArRenderRectF initial_frame={0};
        ArRenderRectI initial_body={0};
        for (unsigned step=0;step<6;++step) {
          memset(&slot,0,sizeof(slot));
          frame_seen=false;
          borders=prose=acknowledgements=0;
          page_reports=body_prepares=choice_icons=0;
          page=step<2?step:step<4?1:step-2;
          slot.snes_width=slot.visible_width=256;
          slot.snes_height=224;
          slot.inidisp=15;
          slot.sim_menu.valid=true;
          slot.sim_menu.scale_percent=percent;
          slot.sim_menu.prompt_width=32+step*28;
          slot.sim_menu.prompt_height=16+step*8;
          SimMenuModel_Open(&slot.sim_menu.model,1,0xf346);
          slot.sim_menu.model.phase=step==2 || step==3?kSimMenu_Confirm:kSimMenu_Dialogue;
          slot.sim_menu.model.dialogue_has_selector=step<4;
          slot.sim_menu.model.yes=step!=3;
          slot.sim.separated_plane_mask=1u<<kSim3DPlane_Bg2Low;
          const int view_width=wide?1600:984;
          PresentSimMenu_Draw(&slot,(ArRenderRectI){13,17,view_width,896});
          assert(frame_seen && frame_bounds.w==312*pixel_scale);
          assert(frame_bounds.h==104*pixel_scale && !clipped);
          assert(frame_bounds.x>=13 && frame_bounds.x+frame_bounds.w<=13+view_width);
          assert(borders==0 && prose==1 && acknowledgements==1 && page_reports==1);
          assert(body_prepares==1);
          if (!step) {
            initial_frame=frame_bounds;
            initial_body=body_bounds;
          }
          assert(memcmp(&initial_frame,&frame_bounds,sizeof(frame_bounds))==0);
          assert(memcmp(&initial_body,&body_bounds,sizeof(body_bounds))==0);
          AssertInsideFrame((ArRenderRectF){body_bounds.x,body_bounds.y,
                                           body_bounds.w,body_bounds.h});
          const bool confirm=slot.sim_menu.model.phase==kSimMenu_Confirm;
          assert(choice_icons==(confirm?2:0));
          if (confirm) {
            assert(choice_selected[0]==slot.sim_menu.model.yes);
            assert(choice_selected[1]!=slot.sim_menu.model.yes);
            AssertInsideFrame(choice_bounds[0]);
            AssertInsideFrame(choice_bounds[1]);
            assert(choice_bounds[0].x>body_bounds.x+body_bounds.w);
            assert(choice_bounds[0].x==choice_bounds[1].x);
            assert(choice_bounds[0].y+choice_bounds[1].y+choice_bounds[1].h==
                   2*body_bounds.y+body_bounds.h);
          }
        }
      }
}
static void TestSystemRow(void) {
  /* The host System row closes the last category ("Other"), with the
   * overlay's System icon and an interface-catalog label. */
  progress=false;
  artwork.icons=(ArRenderTexture){4};
  for (unsigned row=0;row<3;++row) {
    memset(&slot,0,sizeof(slot));
    system_labels=system_icons=0;
    slot.snes_width=slot.visible_width=256;
    slot.snes_height=224;
    slot.inidisp=15;
    slot.sim_menu.valid=true;
    slot.sim_menu.scale_percent=100;
    SimMenuModel_Open(&slot.sim_menu.model,1,0xf346);
    slot.sim_menu.model.row[kSimMenuSystemCategory]=(uint8_t)row;
    PresentSimMenu_Draw(&slot,(ArRenderRectI){0,0,256,224});
    assert(system_labels==1 && system_icons==1);
    assert(system_icon_selected==(row==kSimMenuSystemRow));
  }
  /* Other categories have no System row. */
  memset(&slot,0,sizeof(slot));
  system_labels=system_icons=0;
  slot.snes_width=slot.visible_width=256;
  slot.snes_height=224;
  slot.inidisp=15;
  slot.sim_menu.valid=true;
  slot.sim_menu.scale_percent=100;
  SimMenuModel_Open(&slot.sim_menu.model,1,0xf342);
  PresentSimMenu_Draw(&slot,(ArRenderRectI){0,0,256,224});
  assert(system_labels==0 && system_icons==0);
  artwork.icons=(ArRenderTexture){0};
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
  TestProgressLog();
  TestSystemRow();
  puts("SIM descriptions and modern Progress Log retain fixed boxes and dialogue paging");
  return 0;
}
