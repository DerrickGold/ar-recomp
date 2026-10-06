#include "sim/menu/sim_menu_localization.h"
#include "actraiser/actraiser_localization_runtime.h"
#include "actraiser/actraiser_localization_style.h"
#include "actraiser/actraiser_localization_text_style.h"
#include "localization/unicode_grapheme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ActRaiserTextStylePlan s_menu_help_style;
static ArTextBidiSpans s_menu_help_bidi;

static void MenuLabelSingleLine(ActRaiserResolvedText *text) {
  /* Modern labels have one wide row. Keep style/bidi byte offsets intact
   * while discarding line breaks authored for the original narrow boxes. */
  for (size_t byte=0;byte<text->utf8_bytes;++byte)
    if (text->utf8[byte]=='\n' || text->utf8[byte]=='\r') text->utf8[byte]=' ';
}

static void CaptureMenuDockLabels(
    ArLocalizationFrame *dock, const ArLocalizationFrame *source,
    const uint16_t *cgram, size_t cgram_count) {
  if (!dock) return;
  ArLocalizationFrame_Reset(dock);
  if (!source || !source->font_revision ||
      !ArLocalizationFrame_SetFont(dock, source->locale, source->font_stack_id,
          source->primary_font, source->font_revision, &source->settings)) return;
  ArLocalizationFrame_SetFallbackFonts(dock, source->fallback_fonts,
                                       source->fallback_font_count);
  ArLocalizationFrame_SetFontRoles(dock, source->font_roles,
                                   source->font_role_count);
  static const char *const categories[6]={
    "sim.menu.choice.movement", "sim.menu.choice.direct_people",
    "sim.menu.choice.miracles", "sim.menu.choice.offerings",
    "sim.menu.choice.status", "sim.menu.choice.other"};
  ActRaiserTextPalette palette;
  ActRaiserTextPalette_Capture(&palette,cgram,cgram_count);
  for (unsigned i=0;i<6;++i) {
    ActRaiserResolvedText text={0};
    char error[256]={0};
    if (!ActRaiserLocalizationRuntime_ResolveText(
        categories[i],&text,error,sizeof(error))) continue;
    MenuLabelSingleLine(&text);
    if (!ArLocalizationFrame_AddScreenText(dock,610+i,0,0,192,18,
          text.utf8,text.utf8_bytes,text.cluster_count,text.cluster_count,
          text.source_revision,text.language.direction,8,
          kArLocalizationTextLayout_CenteredLabel)) continue;
    ArLocalizationFrame_SetTextLanguage(dock,&text.language);
    ArLocalizationFrame_SetTextBidiSpans(dock,&text.bidi);
    ActRaiserTextStyle_Publish(&text.styles,0,&palette,dock);
  }
}

static void AppendMenuLabels(
    ArLocalizationFrame *frame, const SimMenuModel *menu,
    const uint16_t *cgram, size_t cgram_count) {
  if (!frame || !menu || !frame->font_revision ||
      menu->phase == kSimMenu_Native || menu->phase == kSimMenu_Closed) return;
  const bool confirmation=menu->phase==kSimMenu_Confirm ||
      (menu->phase==kSimMenu_Dialogue && menu->dialogue_has_selector &&
       SimMenuModel_Action(menu)!=15);
  SimMenuModel origin=*menu;
  if(origin.phase==kSimMenu_Describe || origin.phase==kSimMenu_Dialogue)
    origin.phase=origin.return_phase;
  /* Opening already draws Browse while native input waits for release.
   * Publish its complete labels before that first visible frame, too. */
  if(origin.phase==kSimMenu_Opening || origin.phase==kSimMenu_Confirm ||
     origin.phase==kSimMenu_MessageSpeed) origin.phase=kSimMenu_Browse;
  menu=&origin;
  static const char *const categories[6]={
    "sim.menu.choice.movement", "sim.menu.choice.direct_people",
    "sim.menu.choice.miracles", "sim.menu.choice.offerings",
    "sim.menu.choice.status", "sim.menu.choice.other"};
  for (unsigned index=0;index<9;++index) {
    const char *id=NULL;
    char item[64];
    if (!index) id=categories[menu->category];
    else if (menu->phase == kSimMenu_Inventory && index<=menu->item_count) {
      snprintf(item,sizeof(item),"sim.menu.possession.slot_%02u",menu->items[index-1]-1);
      id=item;
    } else if (menu->phase == kSimMenu_Browse && menu->submenu) {
      for (unsigned a=0;a<15;++a)
        if (kSimMenuActions[a].category==menu->category &&
            kSimMenuActions[a].row==index-1) id=kSimMenuActions[a].semantic_id;
    }
    if (!id) continue;
    ActRaiserResolvedText text={0};
    char error[256]={0};
    if (!ActRaiserLocalizationRuntime_ResolveText(id,&text,error,sizeof(error))) continue;
    MenuLabelSingleLine(&text);
    /* Keep the existing 10px requested font, with enough vertical room for
     * its shadow and descenders at the largest enhanced-text setting. */
    if (!ArLocalizationFrame_AddScreenText(frame,600+index,0,0,132,18,
          text.utf8,text.utf8_bytes,text.cluster_count,text.cluster_count,
          text.source_revision,text.language.direction,10,
          kArLocalizationTextLayout_SingleLineLabel)) continue;
    ArLocalizationFrame_SetTextLanguage(frame,&text.language);
    ArLocalizationFrame_SetTextBidiSpans(frame,&text.bidi);
    ActRaiserTextPalette palette;
    ActRaiserTextPalette_Capture(&palette,cgram,cgram_count);
    ActRaiserTextStyle_Publish(&text.styles,0,&palette,frame);
  }
  if (confirmation) {
    /* Reserve translated choice ink before the native selector composes it.
     * Same source and 6x5 native region/font as system.choice.yes_no. This
     * record is measured only; the native selector still draws its labels. */
    ActRaiserResolvedText text={0};
    char error[256]={0};
    if (ActRaiserLocalizationRuntime_ResolveText(
        "system.choice.yes_no",&text,error,sizeof(error)) &&
        ArLocalizationFrame_AddScreenText(frame,609,0,0,48,40,text.utf8,
          text.utf8_bytes,text.cluster_count,text.cluster_count,text.source_revision,
          text.language.direction,7,kArLocalizationTextLayout_SingleLineLabel)) {
      ArLocalizationFrame_SetTextLanguage(frame,&text.language);
      ArLocalizationFrame_SetTextBidiSpans(frame,&text.bidi);
      ActRaiserTextPalette palette;
      ActRaiserTextPalette_Capture(&palette,cgram,cgram_count);
      ActRaiserTextStyle_Publish(&text.styles,0,&palette,frame);
    }
  }
}

void SimMenuLocalization_CaptureLabels(
    ArLocalizationFrame *labels, const ArLocalizationFrame *source,
    const SimMenuModel *menu,
    const uint16_t *cgram, size_t cgram_count) {
  /* Native inventory/HUD/dialogue claims can fill their entire snapshot
   * budget. Copy only font configuration; keep every modern label separate. */
  CaptureMenuDockLabels(labels,source,cgram,cgram_count);
  AppendMenuLabels(labels,menu,cgram,cgram_count);
}

bool SimMenuLocalization_PrepareHelp(
    const ArDialoguePageSnapshot *source, SimMenuHelpPage *help) {
  memset(&s_menu_help_style, 0, sizeof(s_menu_help_style));
  memset(&s_menu_help_bidi, 0, sizeof(s_menu_help_bidi));
  if (!source || !help || help->source_end > source->utf8_bytes) return false;
  ArDialoguePager_Begin(&help->pager, help->enhanced, 0);
  const size_t bytes = help->source_end - help->source_start;
  uint16_t *offsets = calloc(bytes + 1, sizeof(*offsets));
  if (!offsets) return false;
  /* Help retains the authored source slice exactly. Native-only soft wraps
   * never enter this text, so style and bidi offsets remain byte-for-byte. */
  for (size_t at = 0; at <= bytes; ++at) offsets[at]=(uint16_t)at;
  char error[256];
  bool valid = ActRaiserTextStyle_AppendPage(&s_menu_help_style, source,
      help->source_start, bytes, offsets, help->text, help->bytes, 0,
      error, sizeof(error));
  for (size_t i = 0; valid && i < source->bidi_span_count; ++i) {
    ArTextBidiSpan span = source->bidi_spans[i];
    if (span.end <= help->source_start || span.start >= help->source_end) continue;
    const size_t a = span.start > help->source_start ? span.start - help->source_start : 0;
    const size_t b = span.end < help->source_end ? span.end - help->source_start : bytes;
    span.start = offsets[a];
    span.end = offsets[b];
    if (span.start < span.end) {
      if (s_menu_help_bidi.count >= kArTextMaximumBidiSpans) valid = false;
      else s_menu_help_bidi.spans[s_menu_help_bidi.count++] = span;
    }
  }
  free(offsets);
  return valid;
}

void SimMenuLocalization_AppendHelp(
    ArLocalizationFrame *frame, const SimMenuHelpPage *help,
    const uint16_t *cgram, size_t cgram_count) {
  if(!frame || !help || !help->active || !frame->font_revision ||
      !cgram || cgram_count<4) return;
  const size_t revealed=help->revealed_bytes;
  size_t at = 0, next;
  uint32_t total = 0, visible = 0;
  while(at<help->bytes && ArUnicodeGrapheme_Next(help->text,help->bytes,at,NULL,&next)) {
    ++total;
    if (next <= revealed) ++visible;
    at = next;
  }
  if(!ArLocalizationFrame_AddScreenText(frame,700,40,156,176,56,
    help->text,help->bytes,visible,total,
    /* Zero is an invalid snapshot revision; page zero at byte zero is the
     * most common Help page and must be published too. */
    (((uint64_t)help->authored_page<<32) | help->source_start)+1,
    help->direction,7,kArLocalizationTextLayout_DialogueWindow)) return;
  ArLocalizationTextLanguage language={.direction=help->direction};
  snprintf(language.locale,sizeof(language.locale),"%s",help->locale);
  ArLocalizationFrame_SetTextLanguage(frame,&language);
  ArLocalizationTextSnapshot *text=&frame->snapshots[frame->snapshot_count-1];
  text->revealed_utf8_bytes=(uint32_t)revealed;
  text->style_id=kArTextStyle_RetailPaletteBands;
  text->shadow_enabled = true;
  text->shadow_shape = kArTextShadow_Diagonal;
  text->shadow_rgb=ActRaiserLocalizationStyle_Rgb(cgram[1]);
  text->band_rgb=ActRaiserLocalizationStyle_Rgb(cgram[2]);
  text->body_rgb=ActRaiserLocalizationStyle_Rgb(cgram[3]);
  ActRaiserTextPalette palette;
  ActRaiserTextPalette_Capture(&palette, cgram, cgram_count);
  ActRaiserTextStyle_Publish(&s_menu_help_style, 0, &palette, frame);
  ArLocalizationFrame_SetTextBidiSpans(frame, &s_menu_help_bidi);
  frame->dialogue_ticket = help->pager.ticket;
  frame->dialogue_surface_id = 700;
  frame->dialogue_paged = help->pager.ticket != 0;
  frame->dialogue_page_start = help->pager.start;
}

