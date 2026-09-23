#include "present_sim_menu.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "present_internal.h"
#include "settings_overlay_artwork.h"
#include "session_fatal.h"
#include "render/localized_text_presenter.h"
#include "render/localized_text_layout.h"
#include "render/text_cell_composite.h"

extern ArRenderDevice g_render_device;
extern ArRenderTexture g_hud_bg_texture;
extern ArRenderTexture g_sim3d_layer_textures[kSim3DPlane_Count];

static ArRenderTexture s_icons;
static uint32_t s_revision;

/* Presentation-only retention: a shorter page must not pull the dialogue
 * frame and title downward. No reveal/page/acknowledgement state lives here. */
static struct {
  uint64_t generation, font_revision;
  uint32_t dialogue_generation;
  ArRenderRectI view;
  ArEnhancedTextSettings settings;
  float dialogue_extra;
} s_layout;

static void BeginLayout(const FrameSlot *slot,ArRenderRectI view) {
  const SimMenuModel *m=&slot->sim_menu.model;
  if (s_layout.generation==m->generation &&
      s_layout.dialogue_generation==m->dialogue_generation &&
      s_layout.font_revision==slot->localization.font_revision &&
      !memcmp(&s_layout.settings,&slot->localization.settings,sizeof(s_layout.settings)) &&
      !memcmp(&s_layout.view,&view,sizeof(view))) return;
  memset(&s_layout,0,sizeof(s_layout));
  s_layout.generation=m->generation;
  s_layout.dialogue_generation=m->dialogue_generation;
  s_layout.font_revision=slot->localization.font_revision;
  s_layout.settings=slot->localization.settings;
  s_layout.view=view;
}

bool PresentSimMenu_Active(const FrameSlot *slot) {
  return slot && slot->sim_menu.valid &&
      slot->sim_menu.model.phase != kSimMenu_Closed &&
      slot->sim_menu.model.phase != kSimMenu_Native;
}

void PresentSimMenu_Reset(void) {
  ArRenderDevice_DestroyTexture(&g_render_device,s_icons);
  s_icons=ArRenderTexture_Invalid(); s_revision=0;
  memset(&s_layout,0,sizeof(s_layout));
}

static ArRenderRectF Rect(ArRenderRectI view, float x,float y,float w,float h) {
  const float scale=view.h/224.0f;
  return (ArRenderRectF){view.x+(view.w-256*scale)*0.5f+x*scale,
                         view.y+y*scale,w*scale,h*scale};
}

static ArRenderRectI MenuViewport(ArRenderRectI view,SimMenuPhase phase,
                                  unsigned percent) {
  /* Scale the complete menu uniformly, including text and captured dialogue.
   * Dock panels stay below the unscaled HUD; modal windows stay centered. */
  const float scale=fmaxf(0.5f,fminf(1.0f,percent/100.0f));
  const int width=(int)roundf(view.w*scale), height=(int)roundf(view.h*scale);
  const bool dock=phase==kSimMenu_Browse || phase==kSimMenu_Inventory ||
      phase==kSimMenu_Opening;
  const int top=dock?(int)roundf((view.h-height)*27.0f/224):(view.h-height)/2;
  return (ArRenderRectI){view.x+(view.w-width)/2,view.y+top,width,height};
}

static void Texture(ArRenderTexture t, ArRenderRectF source, ArRenderRectF dest) {
  ArRenderDevice_DrawTexture(&g_render_device,t,&source,&dest);
}

static void FrameAt(ArRenderRectF bounds,float border_x,float border_y) {
  const ArRenderTexture frame=SettingsOverlayArtwork_Get()->dialog_frame;
  /* The same 8px native tiles on all four sides, reduced to a 3px profile.
   * No host borders or differently scaled left/right edges. */
  const float x=bounds.x,y=bounds.y,w=bounds.w,h=bounds.h;
  const float xs[]={x,x+border_x,x+w-border_x};
  const float ys[]={y,y+border_y,y+h-border_y};
  const float ws[]={border_x,w-2*border_x,border_x};
  const float hs[]={border_y,h-2*border_y,border_y};
  for (int row=0;row<3;++row) for(int col=0;col<3;++col) {
    if(row==1 && col==0) {
      /* Mirror the right edge's native profile: the source left bevel has a
       * wider bright strip which otherwise still looks heavier after scaling. */
      const ArRenderRectF r={xs[col],ys[row],ws[col],hs[row]};
      const ArRenderColorF white={1,1,1,1};
      const ArRenderVertex2D v[]={
        {{r.x,r.y},white,{1,1.0f/3}},{{r.x+r.w,r.y},white,{2.0f/3,1.0f/3}},
        {{r.x,r.y+r.h},white,{1,2.0f/3}},{{r.x+r.w,r.y+r.h},white,{2.0f/3,2.0f/3}}};
      const int32_t indices[]={0,1,2,2,1,3};
      ArRenderDevice_DrawGeometry(&g_render_device,frame,v,4,indices,6);
      continue;
    }
    Texture(frame,(ArRenderRectF){col*8,row*8,8,8},
            (ArRenderRectF){xs[col],ys[row],ws[col],hs[row]});
  }
}

static void Frame(ArRenderRectI view,float x,float y,float w,float h) {
  const float scale=view.h/224.0f;
  FrameAt(Rect(view,x,y,w,h),3*scale,3*scale);
}

static void Icon(ArRenderRectI view,unsigned id,bool selected,float x,float y) {
  if (id>=kSimMenuArtCount) return;
  Texture(s_icons,(ArRenderRectF){selected?0:16,id*16,16,16},Rect(view,x,y,16,16));
}

static void DockBackground(ArRenderRectI view,float x,float width) {
  /* Feather the charcoal into the town instead of enclosing it in a frame.
   * The darkest band sits below the icons, with only a short lower fade. */
  const float xs[]={x,x+10,x+width-10,x+width};
  const float ys[]={27,34,58,80,86};
  const float shades[]={0.16f,0.12f,0.04f,0,0};
  const float alphas[]={0,0.44f,0.68f,0.94f,0};
  ArRenderVertex2D vertices[20];
  int32_t indices[72];
  unsigned n=0;
  for (unsigned row=0;row<5;++row) for(unsigned col=0;col<4;++col) {
    const ArRenderRectF p=Rect(view,xs[col],ys[row],0,0);
    vertices[row*4+col]=(ArRenderVertex2D){{p.x,p.y},
      {shades[row],shades[row],shades[row],col==0 || col==3?0:alphas[row]},
      {0,0}};
    if (row<4 && col<3) {
      const int32_t i=row*4+col;
      indices[n++]=i; indices[n++]=i+1; indices[n++]=i+4;
      indices[n++]=i+4; indices[n++]=i+1; indices[n++]=i+5;
    }
  }
  const ArRenderDrawState state={.flags=kArRenderDrawState_Blend,
                                 .blend=kArRenderBlendMode_Alpha};
  ArRenderDevice_DrawGeometryWithState(&g_render_device,ArRenderTexture_Invalid(),
      vertices,20,indices,n,&state);
}

static void DockTitle(const FrameSlot *slot,ArRenderRectI view,unsigned category) {
  /* One fixed title has room for the original glyph proportions. The native
   * caption no longer gets squeezed independently into six narrow columns. */
  const ArRenderRectF r=Rect(view,32,28,192,18);
  ArLocalizedPreparedFrame prepared={0};
  if (ArLocalizedTextPresenter_PrepareScreenText(&g_render_device,
      &slot->sim_menu.label_frame,610+category,
      (ArRenderRectI){r.x,r.y,r.w,r.h},&prepared)) {
    ArLocalizedTextPresenter_DrawWithBrightness(&g_render_device,&prepared,
                                                (slot->inidisp&15)/15.0f);
    return;
  }
  const SettingsOverlayArtwork *art=SettingsOverlayArtwork_Get();
  const char *text=slot->sim_menu.labels[category];
  const size_t length=strlen(text);
  const float x=floorf(128-(length*7+1)*0.5f);
  for (size_t i=0;i<length;++i) {
    const unsigned ch=(unsigned char)text[i];
    if (!art->glyph_defined[ch]) continue;
    Texture(art->fonts[kText_Normal],(ArRenderRectF){(ch&15)*8,(ch>>4)*8,8,8},
            Rect(view,x+i*7,33,8,8));
  }
}

/* Fitting is only for labels and numeric fields, never dialogue prose. */
static void LabelGlyphs(ArRenderRectI view,const char *text,float x,float y,float width) {
  const SettingsOverlayArtwork *art=SettingsOverlayArtwork_Get();
  const size_t n=strlen(text);
  const float advance=n*7>width?width/n:7;
  for(unsigned i=0;i<n;++i) {
    const unsigned ch=(unsigned char)text[i];
    if (!art->glyph_defined[ch]) continue;
    Texture(art->fonts[kText_Normal],(ArRenderRectF){(ch&15)*8,(ch>>4)*8,8,8},
            Rect(view,x+i*advance,y,advance+1,8));
  }
}

static void Label(const FrameSlot *slot, ArRenderRectI view, unsigned surface,
                   const char *text,float x,float y,float width) {
  /* Extra ink room around the same row center prevents descenders/shadows
   * from choosing a smaller font than neighbouring labels. */
  const ArRenderRectF r=Rect(view,x,y-4,width,18);
  ArLocalizedPreparedFrame prepared={0};
  if (ArLocalizedTextPresenter_PrepareScreenText(&g_render_device,
      &slot->sim_menu.label_frame,surface,(ArRenderRectI){r.x,r.y,r.w,r.h},&prepared)) {
    ArLocalizedTextPresenter_DrawWithBrightness(&g_render_device,&prepared,
                                                (slot->inidisp & 15)/15.0f);
  } else LabelGlyphs(view,text,x,y,width);
}

static float LabelWidth(const FrameSlot *slot,ArRenderRectI view,unsigned surface,
                         const char *fallback,float available,float height) {
  const ArRenderRectF r=Rect(view,0,0,available,height);
  ArLocalizedPreparedFrame prepared={0};
  if (ArLocalizedTextPresenter_PrepareScreenText(&g_render_device,
      &slot->sim_menu.label_frame,surface,(ArRenderRectI){r.x,r.y,r.w,r.h},&prepared) &&
      prepared.text_count==1)
    return ceilf(prepared.texts[0].destination.w/(view.h/224.0f))+1;
  /* Only the ROM's ASCII fallback uses the native fixed glyph advance. */
  return fminf(available,strlen(fallback)*7+1);
}

void PresentSimMenu_DrawNativeHelp(const FrameSlot *slot, ArRenderRectI view) {
  if (!PresentSimMenu_Active(slot) || !slot->sim_menu.help.active) return;
  /* Neutral Help has no retail script to rasterize translated glyphs. Mirror
   * its already-captured page into the original dialogue footprint in PiP;
   * the native frame and continuation arrow retain their original placement. */
  const ArRenderRectI body = {view.x + view.w * 40 / 256,
      view.y + view.h * 156 / 224, view.w * 176 / 256, view.h * 56 / 224};
  ArLocalizedPreparedFrame prepared = {0};
  if (!ArLocalizedTextPresenter_PrepareScreenText(&g_render_device,
      &slot->localization, 700, body, &prepared)) return;
  const ArRenderRectF background = {body.x, view.y+view.h*154/224.0f,
      body.w, view.h*50/224.0f};
  ArRenderDevice_DrawSolidRect(&g_render_device, &background,
      (ArRenderColorF){0, 0, 0, 1}, kArRenderBlendMode_Opaque);
  ArLocalizedTextPresenter_DrawWithBrightness(&g_render_device, &prepared,
                                              (slot->inidisp & 15) / 15.0f);
}

/* Relocate the captured game text, including native continuation/pointer
 * tiles and localized cell claims, without creating another dialogue clock. */
static HudPresentationChunk PrepareTextInRect(const FrameSlot *slot,
    ArRenderRectI source, ArRenderRectF dest,
    ArLocalizedPreparedFrame *prepared) {
  HudPresentationChunk chunk={
    .texture=g_hud_bg_texture,
    /* Cell claims only project through chunks identified as native BG3. */
    .inspector_kind=kInspectorPresentation_HudBg,
    .texture_source={slot->ws_extra+source.x,source.y,source.w,source.h},
    .screen_source=source,
    .output_destination={dest.x,dest.y,dest.w,dest.h}};
  ArLocalizedTextPresenter_Prepare(&g_render_device,&slot->localization,
    slot->bg3_state_valid,slot->bg3_tilemap_base_words,
    slot->bg3_tilemap_width_tiles,slot->bg3_tilemap_height_tiles,
    slot->bg3_hscroll,slot->bg3_vscroll,256,224,&chunk,1,prepared);
  return chunk;
}

static HudPresentationChunk PrepareNativeLabels(const FrameSlot *slot,
    ArRenderRectI view, ArRenderRectI source, float x, float y,
    ArLocalizedPreparedFrame *prepared) {
  return PrepareTextInRect(slot,source,
      Rect(view,x,y,source.w,source.h),prepared);
}

static void DrawNativeText(const FrameSlot *slot, HudPresentationChunk chunk,
                           const ArLocalizedPreparedFrame *prepared) {
  HudPresentationChunk pieces[kArTextCellMaximumChunkPieces];
  const size_t count=ArTextCellComposite_SubtractMasks(&chunk,prepared->masks,
    prepared->mask_count,pieces,kArTextCellMaximumChunkPieces);
  if(count!=SIZE_MAX) for(size_t p=0;p<count;++p) {
    const ArRenderRectI a=pieces[p].texture_source,b=pieces[p].output_destination;
    Texture(pieces[p].texture,(ArRenderRectF){a.x,a.y,a.w,a.h},
            (ArRenderRectF){b.x,b.y,b.w,b.h});
  }
  if (count!=SIZE_MAX && ArLocalizedTextPresenter_DrawWithBrightness(
          &g_render_device,prepared,(slot->inidisp&15)/15.0f))
    ArTextPresentation_MarkReady(prepared->ready_dialogue_ticket);
}

static void NativeLabels(const FrameSlot *slot, ArRenderRectI view,
                         ArRenderRectI source, float x, float y) {
  ArLocalizedPreparedFrame prepared={0};
  const HudPresentationChunk chunk=PrepareNativeLabels(slot,view,source,x,y,&prepared);
  DrawNativeText(slot,chunk,&prepared);
}

/* Measure the same captured pixels/claims that will be drawn. Tile occupancy
 * is not an ink bound: BG3 scroll, font effects and translated wrapping all
 * change the visible extent. Keep the full native source for claim projection,
 * then trim only the presentation around the resulting ink. */
static ArRenderRectI QuestionInk(const FrameSlot *slot,
    const HudPresentationChunk *chunk, const ArLocalizedPreparedFrame *prepared) {
  ArRenderRectI ink={0};
  const SrPpuSurfaceView *surface=&slot->sim3d_output_surfaces.hud_bg;
  if (!surface->data || !(surface->flags & SR_PPU_SURFACE_BOUND))
    surface=&slot->ppu_surfaces.overlays[SR_PPU_OVERLAY_BG3][0];
  HudPresentationChunk pieces[kArTextCellMaximumChunkPieces];
  const size_t count=ArTextCellComposite_SubtractMasks(chunk,prepared->masks,
      prepared->mask_count,pieces,kArTextCellMaximumChunkPieces);
  if (count!=SIZE_MAX && surface->data &&
      (surface->flags & SR_PPU_SURFACE_BOUND) &&
      surface->pixel_format==SR_PPU_PIXEL_FORMAT_ARGB8888_U32 &&
      surface->pitch_bytes>=surface->width_pixels*sizeof(uint32_t) &&
      surface->byte_size>=surface->pitch_bytes*surface->height_pixels) {
    for (size_t p=0;p<count;++p) {
      const ArRenderRectI src=pieces[p].texture_source, dst=pieces[p].output_destination;
      if (src.x<0 || src.y<0 || src.x+src.w>(int)surface->width_pixels ||
          src.y+src.h>(int)surface->height_pixels) continue;
      for (int y=0;y<src.h;++y) {
        const uint32_t *row=(const uint32_t *)(surface->data+
            (src.y+y)*surface->pitch_bytes)+src.x;
        for (int x=0;x<src.w;++x) {
          /* Native black paper is opaque; include colored ink plus its one
           * pixel black shadow, rather than measuring the whole paper. */
          if (!(row[x]&0xff000000u) || !(row[x]&0x00ffffffu)) continue;
          const int left=dst.x+x*dst.w/src.w, top=dst.y+y*dst.h/src.h;
          const int right=dst.x+(x+2)*dst.w/src.w, bottom=dst.y+(y+2)*dst.h/src.h;
          ink=ArLocalizedTextLayout_UnionInk(ink,
              (ArRenderRectI){left,top,right-left,bottom-top});
        }
      }
    }
  }
  for (unsigned i=0;i<prepared->text_count;++i) {
    const ArLocalizedPreparedText *text=&prepared->texts[i];
    ArRenderRectI bounds=text->surface.ink_bounds;
    bounds.x+=text->destination.x; bounds.y+=text->destination.y;
    ArRenderRectI source=text->surface.ink_bounds;
    if (text->viewport.w>0 && text->viewport.h>0 &&
        !ArLocalizedTextLayout_Clip(text->viewport,&source,&bounds)) continue;
    ink=ArLocalizedTextLayout_UnionInk(ink,bounds);
  }
  for (unsigned i=0;i<prepared->indicator_count;++i)
    ink=ArLocalizedTextLayout_UnionInk(ink,prepared->indicators[i].destination);
  for (unsigned i=0;i<prepared->inline_object_count;++i)
    ink=ArLocalizedTextLayout_UnionInk(ink,prepared->inline_objects[i].destination);
  for (unsigned i=0;i<prepared->decoration_count;++i)
    ink=ArLocalizedTextLayout_UnionInk(ink,prepared->decorations[i].destination);
  return ink;
}

static void RestoreCompletePage(ArLocalizedPreparedFrame *prepared) {
  /* A modern window must show its whole current page, including the salutation.
   * The native dialogue viewport may have scrolled its first lines out while
   * revealing larger glyphs. Both questions and follow-up dialogue fit their
   * windows around that ink. Undo only this presentation scroll, leaving
   * reveal, page boundaries and acknowledgement timing intact. */
  if (prepared->text_count==1) {
    ArLocalizedPreparedText *text=&prepared->texts[0];
    if (text->viewport.h>0) {
      const int dy=text->viewport.y-text->destination.y;
      text->destination.y+=dy;
      text->viewport=(ArRenderRectI){0};
      for (unsigned i=0;i<prepared->indicator_count;++i)
        prepared->indicators[i].destination.y+=dy;
      for (unsigned i=0;i<prepared->inline_object_count;++i)
        prepared->inline_objects[i].destination.y+=dy;
      for (unsigned i=0;i<prepared->decoration_count;++i)
        prepared->decorations[i].destination.y+=dy;
    }
  }
}

static ArRenderRectF DialogueRect(const FrameSlot *slot, ArRenderRectI view,
                                   float x,float y,float w,float h) {
  /* Same scene-coordinate projection as the ordinary HUD dialogue body. */
  const float sx=(float)view.w/slot->visible_width;
  const float sy=(float)view.h/slot->snes_height;
  return (ArRenderRectF){view.x+(view.w-256*sx)*0.5f+x*sx,
                         view.y+y*sy,w*sx,h*sy};
}

/* Every menu prose surface comes through this dialogue path: questions,
 * Describe, selector instructions, errors and follow-up acknowledgements.
 * The caller chooses only its placement and surrounding artwork. The native
 * scheduler or read-only Help session owns text, reveal, waits and pages. */
static HudPresentationChunk PrepareMenuDialogue(const FrameSlot *slot,
    ArRenderRectF destination,ArLocalizedPreparedFrame *prepared) {
  const HudPresentationChunk chunk=PrepareTextInRect(slot,
      (ArRenderRectI){32,144,192,72},destination,prepared);
  if (slot->sim_menu.help.active) {
    /* Account for captured BG3 scroll and the font's shade above its body.
     * The dialogue claim includes six text rows and the continuation footer. */
    const float sx=destination.w/192, sy=destination.h/72;
    const ArRenderRectF body={destination.x+8*sx,destination.y+12*sy,
                             176*sx,56*sy};
    if (ArLocalizedTextPresenter_PrepareScreenText(&g_render_device,
        &slot->localization,700,
        (ArRenderRectI){body.x,body.y,body.w,body.h},prepared)) {
      /* The main dialogue may grow upward. Its continuation tile is drawn
       * separately at the original bottom position, below the complete text. */
      prepared->masks[prepared->mask_count++]=(ArRenderRectI){40,154,176,62};
    }
  }
  RestoreCompletePage(prepared);
  return chunk;
}

static float Dialogue(const FrameSlot *slot, ArRenderRectI view) {
  /* Reuse only the actual game's bottom window. Menu scale and item metadata
   * do not change its footprint. BG2 supplies the native frame/paper; BG3 and
   * localized claims supply text, effects and the continuation marker. */
  ArLocalizedPreparedFrame prepared={0};
  HudPresentationChunk chunk=PrepareMenuDialogue(slot,
      DialogueRect(slot,view,32,0,192,72),&prepared);
  const bool enhanced_help=slot->sim_menu.help.active && prepared.mask_count;
  const ArRenderRectI ink=QuestionInk(slot,&chunk,&prepared);
  const float scale=(float)view.h/slot->snes_height;
  const float ink_bottom=ceilf(
      (ink.y+ink.h-chunk.output_destination.y)/scale);
  float extra=fmaxf(0,ink_bottom-(enhanced_help?56:
      slot->sim_menu.help.active?68:63));
  if (slot->sim_menu.model.phase==kSimMenu_Describe) {
    extra=fmaxf(extra,s_layout.dialogue_extra);
    s_layout.dialogue_extra=extra;
  }
  /* Keep the original corners/edge thickness when a larger localized page
   * needs extra height. Only the native middle rows are extended upward. */
  /* BG2's captured town box occupies y=151..214 (including the native
   * one-scanline scroll), not the wider BG3 text/claim region y=144..215.
   * Cropping the latter would also restore the command panel's bottom edge. */
  const int source_y[]={151,159,207}, source_h[]={8,48,8};
  const float dest_y[]={151-extra,159-extra,207}, dest_h[]={8,48+extra,8};
  for (int p=0;p<2;++p) {
    const int plane=p?kSim3DPlane_Bg2High:kSim3DPlane_Bg2Low;
    if (!(slot->sim.separated_plane_mask & (1u<<plane))) continue;
    for (int row=0;row<3;++row)
      Texture(g_sim3d_layer_textures[plane],
          (ArRenderRectF){slot->ws_extra+24,source_y[row],208,source_h[row]},
          DialogueRect(slot,view,24,dest_y[row],208,dest_h[row]));
  }
  chunk=PrepareMenuDialogue(slot,
      DialogueRect(slot,view,32,144-extra,192,72),&prepared);
  DrawNativeText(slot,chunk,&prepared);
  if (enhanced_help)
    Texture(g_hud_bg_texture,(ArRenderRectF){slot->ws_extra+208,204,8,8},
            DialogueRect(slot,view,208,204,8,8));
  return 151-extra;
}

static void DescriptionTitle(const FrameSlot *slot, ArRenderRectI view,
                              float dialogue_top) {
  const SimMenuFrame *f=&slot->sim_menu;
  const SimMenuModel *m=&f->model;
  const unsigned id=m->return_phase==kSimMenu_Inventory?20+m->items[m->item_slot]:
      m->submenu?5+SimMenuModel_Action(m):m->category;
  const unsigned label_id=m->return_phase==kSimMenu_Inventory?601+m->item_slot:
      m->submenu?601+m->row[m->category]:600;
  const float sx=(float)view.w/slot->visible_width;
  const float sy=(float)view.h/slot->snes_height;
  const float y=dialogue_top-26; /* Separate plaque, two pixels above the frame. */
  const ArRenderRectF label=DialogueRect(slot,view,0,y+3,176,18);
  ArLocalizedPreparedFrame prepared={0};
  const bool localized=ArLocalizedTextPresenter_PrepareScreenText(&g_render_device,
      &f->label_frame,label_id,
      (ArRenderRectI){label.x,label.y,label.w,label.h},&prepared) &&
      prepared.text_count==1;
  bool rtl=false;
  float text_width=fminf(176,strlen(f->labels[id])*7+1);
  if (localized) {
    const ArLocalizationScreenTextRecord *record=
        ArLocalizationFrame_FindScreenText(&f->label_frame,label_id);
    const ArTextDirection authored=
        f->label_frame.snapshots[record->snapshot_slot].language.direction;
    const ArTextDirection direction=authored==kArTextDirection_Auto?
        prepared.texts[0].surface.paragraph_direction:authored;
    rtl=direction==kArTextDirection_RightToLeft;
    text_width=ceilf(prepared.texts[0].destination.w/sx);
  }
  const float width=text_width+32, x=rtl?232-width:24;
  FrameAt(DialogueRect(slot,view,x,y,width,24),3*sx,3*sy);
  const float icon_x=rtl?x+width-22:x+6;
  const float text_x=rtl?x+6:x+26;
  Texture(s_icons,(ArRenderRectF){0,id*16,16,16},
          DialogueRect(slot,view,icon_x,y+4,16,16));
  if (localized) {
    /* Keep the measured surface pinned: moving the label changes neither its
     * shaping nor its font size, including mixed-script and auto-direction text. */
    ArLocalizedPreparedText *text=&prepared.texts[0];
    const ArRenderRectF dest=DialogueRect(slot,view,text_x,y+3,text_width,18);
    text->destination.x=rtl?(int)(dest.x+dest.w)-text->destination.w:(int)dest.x;
    ArLocalizedTextPresenter_DrawWithBrightness(&g_render_device,&prepared,
                                                (slot->inidisp&15)/15.0f);
  } else {
    const SettingsOverlayArtwork *art=SettingsOverlayArtwork_Get();
    const char *text=f->labels[id];
    for (size_t i=0;text[i];++i) {
      const unsigned ch=(unsigned char)text[i];
      if (art->glyph_defined[ch])
        Texture(art->fonts[kText_Normal],
            (ArRenderRectF){(ch&15)*8,(ch>>4)*8,8,8},
            DialogueRect(slot,view,text_x+i*7,y+8,8,8));
    }
  }
}

static bool MenuLabelRightToLeft(const FrameSlot *slot, ArRenderRectI view,
                                 unsigned surface) {
  const ArLocalizationFrame *labels=&slot->sim_menu.label_frame;
  const ArLocalizationScreenTextRecord *record=
      ArLocalizationFrame_FindScreenText(labels,surface);
  if (!record) return false;
  const ArTextDirection direction=labels->snapshots[record->snapshot_slot].language.direction;
  if (direction!=kArTextDirection_Auto)
    return direction==kArTextDirection_RightToLeft;
  const ArRenderRectF r=Rect(view,0,0,132,18);
  ArLocalizedPreparedFrame prepared={0};
  return ArLocalizedTextPresenter_PrepareScreenText(&g_render_device,labels,surface,
      (ArRenderRectI){r.x,r.y,r.w,r.h},&prepared) && prepared.text_count==1 &&
      prepared.texts[0].surface.paragraph_direction==kArTextDirection_RightToLeft;
}

static void Browse(const FrameSlot *slot,ArRenderRectI view,
                   const SimMenuModel *m) {
  const SimMenuFrame *f=&slot->sim_menu;
  const float dock_width=fminf(360,view.w*224.0f/view.h-8);
  const float dock_x=(256-dock_width)*0.5f;
  const float pitch=(dock_width-16)/6;
  DockBackground(view,dock_x,dock_width);
  DockTitle(slot,view,m->category);
  for (unsigned i=0;i<6;++i) {
    const float center=dock_x+8+pitch*(i+0.5f);
    Texture(s_icons,(ArRenderRectF){i==m->category?0:16,i*16,16,16},
            Rect(view,center-16,47,32,32));
  }
  if (!m->submenu && m->phase!=kSimMenu_Inventory) return;
  const unsigned count=m->phase==kSimMenu_Inventory?m->item_count:
      kSimMenuCategoryActionCounts[m->category];
  const bool miracle=m->phase!=kSimMenu_Inventory && m->category==2;
  /* Reserve a full label column before the separate SP column, so long
   * miracle names do not need a smaller font than their neighbouring rows. */
  const float content_width=miracle?198:166;
  /* Keep the name/SP columns intact. A separate trailing column associates
   * the ROM angel and physical binding with only the selected entry. Retain
   * that space behind Describe so opening Help cannot move the submenu. */
  const unsigned selected_row=m->phase==kSimMenu_Inventory?m->item_slot:m->row[m->category];
  const bool rtl=MenuLabelRightToLeft(slot,view,601+selected_row);
  const float binding_width=fminf(56,strlen(f->describe_binding)*7);
  const float hint_width=f->describe_binding[0]?24+binding_width:0;
  const float panel_width=content_width+hint_width;
  float x=dock_x+8+pitch*(m->category+0.5f)-16;
  x=fminf(x,dock_x+dock_width-8-panel_width);
  /* A long remapped binding may make the panel wider than the dock itself. */
  const float viewport_left=(256-view.w*224.0f/view.h)*0.5f;
  x=fmaxf(x,viewport_left+4);
  const float content_x=x+(rtl?hint_width:0);
  const unsigned row_height=count?fminf(20,floorf(128.0f/count)):20;
  Frame(view,x,84,panel_width,8+count*row_height);
  for(unsigned row=0;row<count;++row) {
    unsigned id=0; bool selected=false;
    if(m->phase==kSimMenu_Inventory) {
      id=20+m->items[row]; selected=row==m->item_slot;
    } else {
      for(unsigned action=0;action<15;++action)
        if(kSimMenuActions[action].category==m->category &&
           kSimMenuActions[action].row==row) id=6+action;
      selected=row==m->row[m->category];
    }
    Icon(view,id,selected,content_x+6,88+row*row_height);
    Label(slot,view,601+row,f->labels[id],content_x+27,92+row*row_height,132);
    if(miracle) {
      char cost[8]; snprintf(cost,sizeof(cost),"%u",m->miracle_sp[id-10]);
      LabelGlyphs(view,cost,content_x+content_width-32,92+row*row_height,25);
    }
    if (selected && hint_width &&
        (f->model.phase==kSimMenu_Browse || f->model.phase==kSimMenu_Inventory)) {
      const float hint_x=rtl?x+6:x+content_width+2;
      Texture(s_icons,(ArRenderRectF){0,kSimMenuArtDescribeAngel*16,16,16},
          Rect(view,hint_x+(rtl?binding_width+4:0),90+row*row_height,12,12));
      LabelGlyphs(view,f->describe_binding,hint_x+(rtl?0:16),
                   92+row*row_height,binding_width);
    }
  }
}

void PresentSimMenu_Draw(const FrameSlot *slot, ArRenderRectI view) {
  if (!PresentSimMenu_Active(slot) || (slot->inidisp & 0x80)) return;
  BeginLayout(slot,view);
  const SimMenuFrame *f=&slot->sim_menu;
  const SimMenuModel *m=&f->model;
  if (m->phase==kSimMenu_Handoff) return;
  if ((m->phase==kSimMenu_Dialogue &&
       (!m->dialogue_has_selector || SimMenuModel_Action(m)==15)) ||
      m->phase==kSimMenu_MessageSpeed) {
    ArRenderDevice_SetClipRect(&g_render_device,NULL);
    Dialogue(slot,view);
    if (m->phase==kSimMenu_MessageSpeed) {
      /* The native script owns prompt reveal, selection and sample text.
       * Only its scale gets a separate, menu-scaled window; prose retains
       * the ordinary bottom dialogue footprint throughout the operation. */
      view=MenuViewport(view,m->phase,f->scale_percent);
      Frame(view,72,88,112,48);
      /* Include the arrow row and the complete text claim after BG3 scroll.
       * The retail -4px scroll places its bottom at y=132, outside the old
       * fixed y=88..128 crop; that silently rejected all enhanced labels. */
      ArRenderRectI source[kArTextCellMaximumProjectedRegions];
      if (slot->bg3_state_valid && ArTextCellComposite_ProjectRegion(
          (ArTextCellRegion){17,11,12,5}, slot->bg3_tilemap_width_tiles,
          slot->bg3_tilemap_height_tiles, slot->bg3_hscroll, slot->bg3_vscroll,
          256,224,source)==1)
        NativeLabels(slot,view,source[0],80,92);
    }
    return;
  }
  if (!ArRenderTexture_IsValid(s_icons)) {
    const ArRenderTextureDesc desc={32,kSimMenuArtHeight,kArRenderPixelFormat_Argb8888,
      kArRenderTextureUsage_Streaming,kArRenderFilter_Nearest,kArRenderBlendMode_Alpha};
    if (!ArRenderDevice_CreateTexture(&g_render_device,&desc,&s_icons)) {
      SessionFatal_Request("Could not allocate the SIM menu artwork texture.");
      return;
    }
  }
  if (s_revision!=f->art_revision) {
    if (!ArRenderDevice_UpdateTexture(&g_render_device,s_icons,NULL,f->argb,32*4)) {
      SessionFatal_Request("Could not upload the SIM menu artwork texture.");
      return;
    }
    s_revision=f->art_revision;
  }
  ArRenderDevice_SetClipRect(&g_render_device,NULL);
  if (m->phase==kSimMenu_Describe) {
    SimMenuModel origin=*m;
    origin.phase=m->return_phase;
    Browse(slot,MenuViewport(view,origin.phase,f->scale_percent),&origin);
    const float dialogue_top=Dialogue(slot,view);
    DescriptionTitle(slot,view,dialogue_top);
    return;
  }
  const ArRenderRectI full_view=view;
  view=MenuViewport(view,m->phase,f->scale_percent);
  if (m->phase==kSimMenu_Confirm ||
      (m->phase==kSimMenu_Dialogue && m->dialogue_has_selector &&
       SimMenuModel_Action(m)!=15)) {
    const unsigned action=SimMenuModel_Action(m);
    ArLocalizedPreparedFrame prepared={0};
    HudPresentationChunk chunk=PrepareMenuDialogue(slot,
        Rect(view,32,144,192,72),&prepared);
    const ArRenderRectI ink=QuestionInk(slot,&chunk,&prepared);
    const float scale=view.h/224.0f;
    const bool native=prepared.text_count==0;
    const float body_width=native && f->prompt_width?f->prompt_width:
        ink.w>0?ceilf(ink.w/scale):192;
    const float body_height=native && f->prompt_height?f->prompt_height:
        ink.h>0?ceilf(ink.h/scale):72;
    /* Reuse the original fixed Yes/No labels as well as the original icons.
     * Their complete source includes both rows and any localized cell claims;
     * measurement discards the unused native paper around them. */
    ArLocalizedPreparedFrame choices={0};
    const ArRenderRectI choice_source={144,72,112,72};
    const HudPresentationChunk choice_chunk=PrepareNativeLabels(slot,view,
        choice_source,144,72,&choices);
    const ArRenderRectI choice_ink=QuestionInk(slot,&choice_chunk,&choices);
    const float label_width=fmaxf(LabelWidth(slot,view,609,"Yes",48,40),
        fmaxf(24,choice_ink.w>0?ceilf(choice_ink.w/scale):0));
    const float choice_width=20+label_width;
    const bool miracle=action>=5 && action<=9;
    const float heading_room=fmaxf(1,full_view.w/scale-8-28-choice_width-22-
                                     (miracle?44:0));
    const float heading_width=22+LabelWidth(slot,view,601+m->row[m->category],
        f->labels[5+action],heading_room,18)+(miracle?44:0);
    const float text_width=fmaxf(body_width,heading_width);
    const float content_height=fmaxf(40,body_height+24);
    const float width=text_width+12+choice_width+16, height=content_height+16;
    const float x=floorf((256-width)*0.5f), y=floorf((224-height)*0.5f);
    const float text_y=y+8+floorf((content_height-body_height-24)*0.5f);
    Frame(view,x,y,width,height);
    Icon(view,5+action,true,x+8,text_y);
    Label(slot,view,601+m->row[m->category],f->labels[5+action],x+30,text_y+4,
          text_width-22-(miracle?44:0));
    if (miracle) {
      char label[16];
      snprintf(label,sizeof(label),"%u SP",m->miracle_sp[action-5]);
      LabelGlyphs(view,label,x+8+text_width-36,text_y+4,36);
    }
    /* Reprepare at the fitted position after header text has used the shared
     * text cache. The source stays full-size, including for long translations. */
    const float dx=native && f->prompt_width?8:
        ink.w>0?(ink.x-chunk.output_destination.x)/scale:0;
    const float dy=native && f->prompt_height?12:
        ink.h>0?(ink.y-chunk.output_destination.y)/scale:0;
    const float body_y=text_y+24;
    const float choice_x=x+width-8-choice_width;
    const float choice_y=y+8+floorf((content_height-40)*0.5f);
    chunk=PrepareMenuDialogue(slot,Rect(view,x+8-dx,body_y-dy,192,72),&prepared);
    /* The native paper can extend past the ink. Clip it to the question's
     * allocated row, so it cannot overwrite the border or the Yes/No icons. */
    const ArRenderRectF body=Rect(view,x+6,body_y-2,body_width+4,body_height+4);
    const ArRenderRectI clip={body.x,body.y,body.w,body.h};
    ArRenderDevice_SetClipRect(&g_render_device,&clip);
    DrawNativeText(slot,chunk,&prepared);
    ArRenderDevice_SetClipRect(&g_render_device,NULL);
    /* Reserve the final choice column while the question reveals. Its native
     * labels/pointer become visible only when the selector owns input. */
    if (m->phase!=kSimMenu_Confirm) return;
    const float choice_dx=choice_ink.w>0?
        (choice_ink.x-choice_chunk.output_destination.x)/scale:40;
    const float choice_dy=choice_ink.h>0?
        (choice_ink.y-choice_chunk.output_destination.y)/scale:19;
    const ArRenderRectF choice_bounds=Rect(view,choice_x+18,choice_y,
                                          label_width+4,40);
    const ArRenderRectI choice_clip={choice_bounds.x,choice_bounds.y,
                                    choice_bounds.w,choice_bounds.h};
    ArRenderDevice_SetClipRect(&g_render_device,&choice_clip);
    NativeLabels(slot,view,choice_source,choice_x+20-choice_dx,
                 choice_y+4-choice_dy);
    ArRenderDevice_SetClipRect(&g_render_device,NULL);
    Icon(view,41,m->yes,choice_x,choice_y);
    Icon(view,42,!m->yes,choice_x,choice_y+24);
    return;
  }
  Browse(slot,view,m);
}
