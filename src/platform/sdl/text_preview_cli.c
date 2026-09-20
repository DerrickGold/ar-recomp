#include "platform/sdl/text_preview_cli.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser/actraiser_dialogue_window.h"
#include "actraiser/actraiser_localization_credits.h"
#include "actraiser/actraiser_localization_fixed_text.h"
#include "actraiser/actraiser_localization_grid.h"
#include "actraiser/actraiser_localization_hud.h"
#include "actraiser/actraiser_localization_name_entry.h"
#include "actraiser/actraiser_localization_routes.h"
#include "actraiser/actraiser_localization_text_normalize.h"
#include "actraiser/actraiser_localization_text_style.h"
#include "actraiser/actraiser_localization_world_navigation.h"
#include "host/font_resources.h"
#include "localization/language_contract.h"
#include "localization/unicode_grapheme.h"
#include "platform/sdl/render_sdl_internal.h"
#include "platform/sdl/text_rasterizer_sdl.h"
#include "render/localized_text_presenter.h"
#include "sim/sim_world_navigation_scene.h"

enum {
  kFramesPerSheet = 32,
  kMaximumFrames = 2048,
  kScale = 3,
  kMaximumFontFiles = 81
};

typedef struct PreviewInput {
  uint32_t page, delay;
  ArEnhancedTextSettings settings;
  ArDialogueValue values[kArDialogueMaximumValues];
  uint32_t count;
  uint32_t inks[kActRaiserTextInk_Count];
} PreviewInput;

typedef struct PreviewFont {
  ArFontResourceId resource;
  char reference[kArLanguageFontPathCapacity];
} PreviewFont;

typedef struct Preview {
  PreviewInput input;
  ArLanguagePack pack, fallback;
  ArDialogueSession session;
  ActRaiserDialogueWindow window;
  uint16_t first_page, clear_control_count;
  ArHostFontResources resources;
  PreviewFont fonts[kMaximumFontFiles];
  size_t font_count;
  ArLocalizationFrame frame;
  ArLocalizedPreparedFrame prepared;
  ArTextCellRegion region;
  uint8_t native_font_pixels;
  ArLanguagePresentationContract presentation;
  ArTextFontRole roles[kArLanguageMaximumFontRoles];
  ArFontResourceId primary, fallbacks[kArLanguageMaximumFallbackFonts];
  SDL_Surface *canvas, *sheet;
  SDL_Renderer *renderer;
  ArSdlRenderBackend backend;
  ArRenderDevice device;
  FILE *report;
  const char *output;
  unsigned frames, sheets, ticks;
  int width, height;
  bool exact_bounds, credits, hud, world_label, artwork_placeholders;
  ActRaiserLocalizationHudPresentation hud_geometry;
  char error[512];
} Preview;

/* Little-endian scalars and length-prefixed UTF-8. The Go host owns JSON and
 * path validation; the worker still bounds every value before allocating. */
static bool ReadU32(uint32_t *value) {
  unsigned char b[4];
  if (fread(b, 1, 4, stdin) != 4)
    return false;
  *value =
      b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
  return true;
}

static bool ReadString(char *text, size_t capacity) {
  uint32_t bytes;
  if (!ReadU32(&bytes) || bytes >= capacity ||
      fread(text, 1, bytes, stdin) != bytes || memchr(text, 0, bytes))
    return false;
  text[bytes] = 0;
  return true;
}

static bool ReadInput(PreviewInput *input) {
  char magic[8];
  if (fread(magic, 1, 8, stdin) != 8 || memcmp(magic, "ARTPREV1", 8))
    return false;
  ArEnhancedTextSettings_Defaults(&input->settings);
  uint32_t size, sampling, pixelation, pixel_size;
  if (!ReadU32(&input->page) || input->page >= 64 || !ReadU32(&input->delay) ||
      input->delay > 9 || !ReadU32(&size) || !ReadU32(&sampling) ||
      !ReadU32(&pixelation) || !ReadU32(&pixel_size) ||
      !ReadU32(&input->count) || input->count > kArDialogueMaximumValues)
    return false;
  input->settings.size_percent = (int)size;
  input->settings.sampling = sampling;
  input->settings.pixelation = pixelation;
  input->settings.pixelation_size = (int)pixel_size;
  if (!ArEnhancedTextSettings_IsValid(&input->settings))
    return false;
  for (uint32_t i = 0; i < input->count; ++i) {
    ArDialogueValue *value = &input->values[i];
    char text[kArDialogueValueTextCapacity];
    if (!ReadString(value->name, sizeof(value->name)) ||
        !ReadString(text, sizeof(text)))
      return false;
    value->kind = ArLanguageContract_PlaceholderKind(value->name);
    if (!value->kind)
      return false;
    for (uint32_t j = 0; j < i; ++j)
      if (!strcmp(value->name, input->values[j].name))
        return false;
    if (value->kind == kArLanguagePlaceholder_Number) {
      char *end;
      errno = 0;
      value->number = strtoll(text, &end, 10);
      if (errno || end == text || *end)
        return false;
    } else {
      memcpy(value->text, text, strlen(text) + 1);
    }
  }
  for (size_t i = 1; i < kActRaiserTextInk_Count; ++i)
    if (!ReadU32(&input->inks[i]) || input->inks[i] > 0xffffff)
      return false;
  return fgetc(stdin) == EOF;
}

static void JsonString(FILE *out, const char *value) {
  fputc('"', out);
  for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
    if (*p == '"' || *p == '\\') {
      fputc('\\', out);
      fputc(*p, out);
    } else if (*p < 32)
      fprintf(out, "\\u%04x", *p);
    else
      fputc(*p, out);
  }
  fputc('"', out);
}

static bool Resolve(void *context, const char *name,
                    ArLanguagePlaceholderKind kind, ArDialogueValue *value,
                    char *error, size_t capacity) {
  PreviewInput *input = context;
  for (uint32_t i = 0; i < input->count; ++i) {
    if (strcmp(name, input->values[i].name))
      continue;
    if (kind != input->values[i].kind)
      return false;
    *value = input->values[i];
    return true;
  }
  snprintf(error, capacity, "Supply a preview scenario value for %s", name);
  return false;
}

static ArFontResourceId RegisterFont(Preview *preview, const char *manifest,
                                     const char *builtin,
                                     const char *reference) {
  if (preview->font_count == kMaximumFontFiles)
    return 0;
  char path[4096];
  if (!strcmp(reference, "builtin:actraiser-sans"))
    snprintf(path, sizeof(path), "%s", builtin);
  else if (!ArLanguagePack_ResolveMemberPath(manifest, reference, path,
                                             sizeof(path)))
    return 0;
  ArFontResourceId id = ArHostFontResources_RegisterFile(
      &preview->resources, path, preview->error, sizeof(preview->error));
  if (!id)
    return 0;
  PreviewFont *font = &preview->fonts[preview->font_count++];
  font->resource = id;
  snprintf(font->reference, sizeof(font->reference), "%s", reference);
  return id;
}

static bool LoadFonts(Preview *p, const char *manifest, const char *builtin) {
  const ArLanguagePackMetadata *m = ArLanguagePack_GetMetadata(&p->pack);
  p->primary = RegisterFont(p, manifest, builtin, m->primary_font);
  if (!p->primary)
    return false;
  for (size_t i = 0; i < m->fallback_font_count; ++i) {
    p->fallbacks[i] = RegisterFont(p, manifest, builtin, m->fallback_fonts[i]);
    if (!p->fallbacks[i])
      return false;
  }
  for (size_t i = 0; i < m->font_role_count; ++i) {
    const ArLanguageFontRole *source = &m->font_roles[i];
    ArTextFontRole *role = &p->roles[i];
    snprintf(role->name, sizeof(role->name), "%s", source->name);
    role->primary = RegisterFont(p, manifest, builtin, source->primary_font);
    if (!role->primary)
      return false;
    role->fallback_count = source->fallback_font_count;
    for (size_t j = 0; j < source->fallback_font_count; ++j) {
      role->fallbacks[j] =
          RegisterFont(p, manifest, builtin, source->fallback_fonts[j]);
      if (!role->fallbacks[j])
        return false;
    }
  }
  return true;
}

static bool PadNameSample(Preview *p) {
  for (uint32_t i = 0; i < p->input.count; ++i) {
    ArDialogueValue *value = &p->input.values[i];
    if (strcmp(value->name, "master_name"))
      continue;
    size_t bytes = strlen(value->text), offset = 0, next;
    unsigned count = 0;
    while (offset < bytes) {
      if (!ArUnicodeGrapheme_Next(value->text, bytes, offset, NULL, &next) ||
          next <= offset)
        return false;
      offset = next;
      ++count;
    }
    if (count > kActRaiserLocalizationNameLength ||
        bytes >= kActRaiserLocalizationUnicodeNameCapacity) {
      snprintf(
          p->error, sizeof(p->error),
          "The name-entry sample must fit eight graphemes and 256 UTF-8 bytes");
      return false;
    }
    while (count++ < kActRaiserLocalizationNameLength) {
      memcpy(value->text + bytes, "\xE2\x80\x87", 3);
      bytes += 3;
    }
    value->text[bytes] = 0;
  }
  return true;
}

static bool Begin(Preview *p, const char *manifest, const char *fallback,
                  const char *builtin, const char *id) {
  if (!strcmp(id, "name_entry.prompt_and_alphabet") && !PadNameSample(p))
    return false;
  ArLanguagePackIo io;
  ArLanguagePackFileIo_Init(&io);
  ArLanguagePackError error = {0};
  if (!ArLanguagePack_Load(&p->pack, &io, manifest, &error) ||
      !ArLanguagePack_Load(&p->fallback, &io, fallback, &error))
    goto failed;
  const ArLanguagePackMetadata *m = ArLanguagePack_GetMetadata(&p->pack);
  if (m->format_version != 2 ||
      ArLanguagePack_GetMetadata(&p->fallback)->format_version != 2) {
    snprintf(
        error.message, sizeof(error.message),
        "Upgrade this project and its source reference to v2 before playback");
    goto failed;
  }
  const ArLanguagePack *effective =
      ArLanguagePack_FindMessage(&p->pack, id) ? &p->pack : &p->fallback;
  if (!ArLanguageContract_ValidateMessage(effective, id, &error))
    goto failed;
  ArDialogueSource source = {
      .effective_pack = effective,
      .term_fallback_pack = &p->fallback,
      .message = ArLanguagePack_FindMessage(effective, id),
      .resolved_source = kArDialogueResolvedSource_SelectedPack,
      .presentation = kArDialoguePresentation_Enhanced};
  ArDialogueContract contract = {0};
  contract.value_count = ArLanguageContract_AllowedPlaceholderCount(id);
  if (contract.value_count > kArDialogueMaximumValues)
    goto failed;
  for (uint32_t i = 0; i < contract.value_count; ++i) {
    const char *name = ArLanguageContract_AllowedPlaceholder(id, i);
    snprintf(contract.values[i].name, sizeof(contract.values[i].name), "%s",
             name);
    contract.values[i].kind = ArLanguageContract_PlaceholderKind(name);
  }
  contract.control_count = ArLanguageContract_RequiredAnchorCount(
      id, ArLanguagePack_GetMetadata(effective)->source_profile);
  ArDialogueValueResolver resolver = {
      .struct_size = sizeof(resolver),
      .abi_version = AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION,
      .context = &p->input,
      .resolve = Resolve};
  if (!ArDialogueSession_BeginSource(&p->session, &source, &contract, id,
                                     &resolver,
                                     kArLocalizationFrameTextCapacity, &error))
    goto failed;
  if (!LoadFonts(p, manifest, builtin) ||
      !ArLanguageContract_Presentation(id, &p->presentation))
    return false;
  return true;
failed:
  snprintf(p->error, sizeof(p->error), "%s",
           error.message[0] ? error.message : "message unavailable");
  return false;
}

static ArLocalizationTextLayoutKind Layout(const Preview *p) {
  if (p->presentation.shape == kArLanguagePresentation_Flow)
    return kArLocalizationTextLayout_DialogueWindow;
  const char *layout = p->presentation.layout;
  if (!strcmp(layout, "centered_label"))
    return kArLocalizationTextLayout_CenteredLabel;
  if (!strcmp(layout, "single_line_label"))
    return kArLocalizationTextLayout_SingleLineLabel;
  if (!strcmp(layout, "centered_block"))
    return kArLocalizationTextLayout_CenteredBlock;
  if (!strcmp(layout, "right_aligned_label"))
    return kArLocalizationTextLayout_RightAlignedLabel;
  if (!strcmp(layout, "left_aligned_label"))
    return kArLocalizationTextLayout_LeftAlignedLabel;
  if (!strcmp(layout, "framed_label"))
    return kArLocalizationTextLayout_FramedLabel;
  return kArLocalizationTextLayout_Flow;
}

static bool PublishStyle(Preview *p, const ActRaiserTextStylePlan *styles) {
  ActRaiserTextPalette palette = {0};
  for (size_t i = 1; i < kActRaiserTextInk_Count; ++i) {
    palette.rgb[i] = p->input.inks[i];
    palette.available |= (uint16_t)(1u << i);
  }
  return ActRaiserTextStyle_Publish(styles, 0, &palette, &p->frame);
}

static bool PublishAppearance(Preview *p, const ArDialoguePageSnapshot *page,
                              const ArTextBidiSpans *bidi,
                              const ActRaiserTextStylePlan *styles) {
  ArLocalizationTextLanguage language = {
      .direction = page->direction == kArLanguageDirection_RightToLeft
                       ? kArTextDirection_RightToLeft
                   : page->direction == kArLanguageDirection_LeftToRight
                       ? kArTextDirection_LeftToRight
                       : kArTextDirection_Auto};
  snprintf(language.locale, sizeof(language.locale), "%s", page->locale);
  return ArLocalizationFrame_SetTextLanguage(&p->frame, &language) &&
         ArLocalizationFrame_SetTextBidiSpans(&p->frame, bidi) &&
         PublishStyle(p, styles);
}

static bool BuildDialogueFrame(Preview *p, const ArDialoguePageSnapshot *page) {
  ActRaiserDialogueWindow *w = &p->window;
  if (!ActRaiserDialogueWindow_Build(w, &p->session, page, p->first_page,
                                     p->clear_control_count))
    return false;
  ArTextDirection direction =
      page->direction == kArLanguageDirection_RightToLeft
          ? kArTextDirection_RightToLeft
      : page->direction == kArLanguageDirection_LeftToRight
          ? kArTextDirection_LeftToRight
          : kArTextDirection_Auto;
  return ArLocalizationFrame_AddStructuredDialogueWindow(
             &p->frame, 1,
             (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
             p->region, w->text, w->bytes,
             ActRaiserDialogueWindow_RevealedBytes(w,
                                                   page->revealed_utf8_bytes),
             w->clusters, page->source_revision, direction,
             p->native_font_pixels, w->structural_boundaries) &&
         PublishAppearance(p, page, &w->bidi, &w->styles);
}

static void ReserveArtwork(Preview *p, ArLocalizationArtworkKind kind,
                           uint8_t width) {
  ArLocalizationArtwork *art = &p->frame.artwork[kind];
  *art = (ArLocalizationArtwork){.valid = true, .width = width, .height = 8};
  for (unsigned y = 0; y < 8; ++y)
    for (unsigned x = 0; x < width; ++x)
      if (!x || x + 1 == width || !y || y == 7)
        art->argb[y * width + x] = 0xff8296ae;
  p->artwork_placeholders = true;
}

static void PrepareArtwork(Preview *p, const ActRaiserResolvedText *text) {
  for (uint8_t i = 0; i < text->inline_object_count; ++i) {
    switch (text->inline_objects[i].kind) {
    case kArLocalizationInlineObject_StatusLife:
      ReserveArtwork(p, kArLocalizationArtwork_Life, 8);
      break;
    case kArLocalizationInlineObject_StatusPopulation:
      ReserveArtwork(p, kArLocalizationArtwork_Population, 8);
      break;
    case kArLocalizationInlineObject_SpeedDirection:
      ReserveArtwork(p, kArLocalizationArtwork_SpeedDirection, 8);
      break;
    default:
      break; /* Name action keys have the presenter's own drawn fallback. */
    }
  }
  if (p->hud &&
      p->hud_geometry.layout == kArLocalizationTextLayout_FramedLabel) {
    ReserveArtwork(p, kArLocalizationArtwork_LabelFrameLeft, 8);
    ReserveArtwork(p, kArLocalizationArtwork_LabelFrameRight, 7);
  }
}

static bool BuildFrame(Preview *p, const ArDialoguePageSnapshot *page) {
  ArLocalizationFrame *frame = &p->frame;
  ArLocalizationFrame_Reset(frame);
  const ArLanguagePackMetadata *m = ArLanguagePack_GetMetadata(&p->pack);
  if (!ArLocalizationFrame_SetFont(frame, m->locale, "workshop-preview",
                                   p->primary, 1, &p->input.settings) ||
      !ArLocalizationFrame_SetFallbackFonts(frame, p->fallbacks,
                                            m->fallback_font_count) ||
      !ArLocalizationFrame_SetFontRoles(frame, p->roles, m->font_role_count))
    return false;
  if (p->presentation.shape == kArLanguagePresentation_Flow)
    return BuildDialogueFrame(p, page);
  ActRaiserResolvedText text;
  if (!ActRaiserLocalizationFixedText_FromPage(page, &text, p->error,
                                               sizeof(p->error)))
    return false;
  PrepareArtwork(p, &text);
  const ArTextCellDestination destination = {3, kArTextCellScreen_Composited,
                                             0};
  if (p->world_label)
    return ActRaiserLocalizationWorldNavigation_AddText(frame, &text) &&
           PublishAppearance(p, page, &text.bidi, &text.styles);
  if (p->credits) {
    uint32_t legacy_accent;
    return ActRaiserLocalizationCredits_PrepareText(&text, &legacy_accent) &&
           ActRaiserLocalizationCredits_AddText(frame, destination, &text) &&
           PublishStyle(p, &text.styles);
  }
  const ActRaiserLocalizationMenu menu =
      (ActRaiserLocalizationMenu)ArLanguageRowShape_ForRoute(page->message_id);
  bool added;
  if (menu != kActRaiserLocalizationMenu_None) {
    ArLocalizationTextGrid grid;
    if (!ActRaiserLocalizationGrid_Build(menu, p->region, &grid))
      return false;
    added = ArLocalizationFrame_AddTextWithGrid(
        frame, 1, destination, p->region, text.utf8, text.utf8_bytes,
        text.cluster_count, text.cluster_count, text.source_revision,
        text.language.direction, p->native_font_pixels, &grid,
        text.structural_boundaries, NULL, 0, text.inline_objects,
        text.inline_object_count);
  } else {
    added = ArLocalizationFrame_AddTextWithObjectsAndLayout(
        frame, 1, destination, p->region, text.utf8, text.utf8_bytes,
        text.cluster_count, text.cluster_count, text.source_revision,
        text.language.direction, p->native_font_pixels,
        p->hud ? p->hud_geometry.layout : Layout(p), NULL, 0,
        text.inline_objects, text.inline_object_count);
  }
  if (!added)
    return false;
  if (p->hud) {
    ArLocalizationTextSnapshot *snapshot =
        &frame->snapshots[frame->snapshot_count - 1];
    snapshot->top_inset_pixels = p->hud_geometry.top_inset;
    snapshot->left_inset_pixels = p->hud_geometry.left_inset;
    snapshot->right_inset_pixels = p->hud_geometry.right_inset;
  }
  if (!strncmp(page->message_id, "city.", 5) &&
      Layout(p) == kArLocalizationTextLayout_SingleLineLabel)
    frame->snapshots[frame->snapshot_count - 1].top_inset_pixels = 1;
  if (p->presentation.shape == kArLanguagePresentation_Keyboard &&
      (!ArLocalizationFrame_SetKeySeparator(frame, " ", 1) ||
       !ArLocalizationFrame_SetKeyGrid(
           frame, kActRaiserLocalizationNameEntryColumns,
           kActRaiserLocalizationNameEntryRows,
           kActRaiserLocalizationNameEntryKeyCellColumns) ||
       !ArLocalizationFrame_SetLiveLine(frame, text.live_field.utf8_offset,
                                        text.live_field.utf8_bytes,
                                        text.live_field.cells)))
    return false;
  return PublishAppearance(p, page, &text.bidi, &text.styles);
}

static bool SaveSheet(Preview *p) {
  if (!p->sheet)
    return true;
  char path[4096];
  if (snprintf(path, sizeof(path), "%s/sheet-%u.png", p->output, p->sheets) >=
          (int)sizeof(path) ||
      !SDL_SavePNG(p->sheet, path))
    return false;
  SDL_DestroySurface(p->sheet);
  p->sheet = NULL;
  ++p->sheets;
  return true;
}

static bool RenderFrame(Preview *p, const char *kind, const char *control) {
  if (p->frames >= kMaximumFrames) {
    snprintf(p->error, sizeof(p->error),
             "Page exceeds the playback limit of %d reveal frames; split it "
             "with @page",
             kMaximumFrames);
    return false;
  }
  ArDialoguePageSnapshot page;
  if (!ArDialogueSession_GetPage(&p->session, &page) || !BuildFrame(p, &page))
    return false;
  const HudPresentationChunk chunk = {
      .inspector_kind = kInspectorPresentation_HudBg,
      .screen_source = {0, 0, 256, 224},
      .texture_source = {0, 0, 256, 224},
      .output_destination = {-(int)p->region.column * 8 * kScale,
                             -(int)p->region.row * 8 * kScale, 256 * kScale,
                             224 * kScale}};
  if (p->world_label) {
    if (!ArLocalizedTextPresenter_PrepareScreenText(
            &p->device, &p->frame, kActRaiserLocalizationWorldNavigationSurface,
            (ArRenderRectI){0, 0, p->width, p->height}, &p->prepared))
      return false;
  } else {
    ArLocalizedTextPresenter_Prepare(&p->device, &p->frame, true, 0, 32, 32, 0,
                                     0, 256, 224, &chunk, 1, &p->prepared);
  }
  if (p->frame.text_bytes && !p->prepared.text_count) {
    snprintf(p->error, sizeof(p->error),
             "Text could not be fitted to the game's %u × %u cell region; "
             "inspect font, size, field styles and inline artwork",
             p->region.columns, p->region.rows);
    return false;
  }
  SDL_SetRenderDrawColor(p->renderer, 18, 24, 40, 255);
  if (!SDL_RenderClear(p->renderer) ||
      !ArLocalizedTextPresenter_Draw(&p->device, &p->prepared) ||
      !SDL_RenderPresent(p->renderer))
    return false;
  if (!p->sheet)
    p->sheet = SDL_CreateSurface(p->width, p->height * kFramesPerSheet,
                                 SDL_PIXELFORMAT_RGBA32);
  SDL_Rect destination = {0, (int)(p->frames % kFramesPerSheet) * p->height,
                          p->width, p->height};
  if (!p->sheet || !SDL_BlitSurface(p->canvas, NULL, p->sheet, &destination))
    return false;
  if (p->frames)
    fputc(',', p->report);
  fprintf(p->report,
          "{\"tick\":%u,\"sheet\":%u,\"index\":%u,\"revealed\":%zu,\"kind\":",
          p->ticks, p->sheets, p->frames % kFramesPerSheet,
          page.revealed_utf8_bytes);
  JsonString(p->report, kind);
  if (control) {
    fputs(",\"control\":", p->report);
    JsonString(p->report, control);
  }
  fputc('}', p->report);
  ++p->frames;
  return p->frames % kFramesPerSheet || SaveSheet(p);
}

static bool CompleteControl(Preview *p, const ArDialogueToken *token) {
  if (!ArDialogueSession_CompleteControl(&p->session, token->control_ordinal))
    return false;
  if (ActRaiserDialogueWindow_ControlClears(token->control_id)) {
    p->first_page = (uint16_t)p->session.state.authored_page_index;
    p->clear_control_count = (uint16_t)(token->control_ordinal + 1u);
  }
  return true;
}

static bool SkipToPage(Preview *p) {
  ArLanguagePackError error = {0};
  for (unsigned guard = 0; guard < 262144; ++guard) {
    if (p->session.state.authored_page_index == p->input.page)
      return true;
    if (p->session.state.terminal)
      break;
    if (p->session.state.awaiting_page_advance) {
      if (!ArDialogueSession_AdvancePage(&p->session))
        break;
      if (!p->input.delay) {
        p->first_page = (uint16_t)p->session.state.authored_page_index;
        p->clear_control_count = 0;
      }
      continue;
    }
    if (p->session.state.awaiting_input)
      ArDialogueSession_ResumeInput(&p->session);
    if (p->session.state.wait_frames_remaining)
      ArDialogueSession_TickWait(&p->session,
                                 p->session.state.wait_frames_remaining);
    ArDialogueToken token;
    if (!ArDialogueSession_Next(&p->session, &token, &error))
      break;
    if (token.kind == kArDialogueToken_Control && !CompleteControl(p, &token))
      break;
  }
  snprintf(p->error, sizeof(p->error), "Requested page is unavailable: %s",
           error.message);
  return false;
}

static void Diagnostics(Preview *p) {
  FILE *out = p->report;
  fputs("],\"fonts\":[", out);
  bool comma = false;
  for (size_t t = 0; t < p->prepared.text_count; ++t) {
    const ArTextSurface *surface = &p->prepared.texts[t].surface;
    for (size_t i = 0; i < surface->font_use_count; ++i) {
      const ArTextFontUse *use = &surface->font_uses[i];
      const char *reference = "unknown";
      for (size_t f = 0; f < p->font_count; ++f)
        if (p->fonts[f].resource == use->resource)
          reference = p->fonts[f].reference;
      if (comma)
        fputc(',', out);
      comma = true;
      fprintf(out,
              "{\"start\":%u,\"end\":%u,\"pixels\":%u,\"missing\":%s,"
              "\"reference\":",
              use->start, use->end, use->font_pixels,
              use->missing ? "true" : "false");
      JsonString(out, reference);
      fputc('}', out);
    }
  }
  fprintf(out,
          "],\"duration\":%u,\"sheetCount\":%u,\"artworkPlaceholders\":%s}",
          p->ticks, p->sheets, p->artwork_placeholders ? "true" : "false");
}

static bool Playback(Preview *p) {
  ArDialoguePageSnapshot page;
  if (!SkipToPage(p) || !ArDialogueSession_GetPage(&p->session, &page))
    return false;
  char path[4096];
  snprintf(path, sizeof(path), "%s/report.json", p->output);
  p->report = fopen(path, "wb");
  if (!p->report)
    return false;
  fprintf(p->report,
          "{\"version\":1,\"width\":%d,\"height\":%d,\"page\":%u,\"pages\":%u,"
          "\"nativeBounds\":%s,\"source\":",
          p->width, p->height, page.page_index, page.page_count,
          p->exact_bounds ? "true" : "false");
  JsonString(p->report, page.source_path);
  fputs(",\"messageID\":", p->report);
  JsonString(p->report, page.message_id);
  fputs(",\"resolvedID\":", p->report);
  JsonString(p->report, page.resolved_message_id);
  fprintf(p->report, ",\"line\":%u,\"layout\":", page.source_line);
  JsonString(p->report, p->presentation.layout);
  fputs(",\"frames\":[", p->report);
  if (!RenderFrame(p, "start", NULL))
    return false;
  if (p->presentation.shape != kArLanguagePresentation_Flow) {
    if (!SaveSheet(p))
      return false;
    Diagnostics(p);
    return true;
  }
  ArLanguagePackError error = {0};
  for (unsigned guard = 0; guard < 16384; ++guard) {
    if (p->session.state.wait_frames_remaining) {
      unsigned wait = p->session.state.wait_frames_remaining;
      p->ticks += wait;
      ArDialogueSession_TickWait(&p->session, wait);
    }
    ArDialogueToken token;
    if (!ArDialogueSession_Next(&p->session, &token, &error)) {
      snprintf(p->error, sizeof(p->error), "%s", error.message);
      return false;
    }
    switch (token.kind) {
    case kArDialogueToken_Grapheme:
      if (!RenderFrame(p, "reveal", NULL))
        return false;
      if (token.first_scalar != ' ' && token.first_scalar != '\n' &&
          token.first_scalar != '\t' && token.first_scalar != '\r')
        p->ticks += p->input.delay;
      break;
    case kArDialogueToken_WaitStarted:
      if (!RenderFrame(p, "wait", NULL))
        return false;
      break;
    case kArDialogueToken_Control:
      if (!RenderFrame(p, "control", token.control_id) ||
          !CompleteControl(p, &token))
        return false;
      break;
    case kArDialogueToken_PageComplete:
    case kArDialogueToken_End:
      if (!RenderFrame(p, token.kind == kArDialogueToken_End ? "end" : "page",
                       NULL) ||
          !SaveSheet(p))
        return false;
      Diagnostics(p);
      return true;
    default:
      if (p->session.state.awaiting_input) {
        if (!RenderFrame(p, "input", NULL))
          return false;
        ArDialogueSession_ResumeInput(&p->session);
      }
      break;
    }
  }
  snprintf(p->error, sizeof(p->error),
           "Playback did not reach a page boundary");
  return false;
}

int ArSdlTextPreview_Run(int argc, char **argv) {
  if (argc != 6) {
    fprintf(stderr, "usage: text-preview PACK FALLBACK BUILTIN_FONT MESSAGE_ID "
                    "OUTPUT_DIRECTORY < scenario.bin\n");
    return 2;
  }
  Preview *p = calloc(1, sizeof(*p));
  if (!p)
    return 2;
  ArLanguagePack_Init(&p->pack);
  ArLanguagePack_Init(&p->fallback);
  ArDialogueSession_Init(&p->session);
  p->output = argv[5];
  bool ok =
      ReadInput(&p->input) && Begin(p, argv[1], argv[2], argv[3], argv[4]);
  if (ok) {
    p->region = (ArTextCellRegion){5, 19, 23, 7};
    p->native_font_pixels = 7;
    p->exact_bounds = ActRaiserLocalizationRoute_TextBounds(
        argv[4], &p->region, &p->native_font_pixels);
    p->credits = !strncmp(argv[4], "credits.", 8);
    p->hud = ActRaiserLocalizationHud_Presentation(argv[4], &p->hud_geometry);
    if (p->credits) {
      p->region = kActRaiserCreditsRegion;
      p->native_font_pixels = kActRaiserCreditsFontPixels;
      p->exact_bounds = true;
    } else if (p->hud) {
      p->region = p->hud_geometry.region;
      p->native_font_pixels = p->hud_geometry.font_pixels;
      p->exact_bounds = true;
    }
    p->world_label = !strcmp(argv[4], "world_map.location_label");
    if (p->world_label)
      p->exact_bounds = true;
    p->width = (p->world_label ? kSimWorldNavigationLabelWidth
                               : p->region.columns * 8) *
               kScale;
    p->height =
        (p->world_label ? kSimWorldNavigationLabelHeight : p->region.rows * 8) *
        kScale;
    p->canvas = SDL_CreateSurface(p->width, p->height, SDL_PIXELFORMAT_RGBA32);
    p->renderer = p->canvas ? SDL_CreateSoftwareRenderer(p->canvas) : NULL;
    ok = p->renderer &&
         ArSdlRenderBackend_Bind(&p->device, &p->backend, p->renderer);
  }
  if (ok) {
    ArTextBackend backend;
    ArSdlTextBackend_Init(&backend);
    ArLocalizedTextPresenter_SetBackend(&backend);
    ArFontResources provider = ArHostFontResources_Provider(&p->resources);
    ArLocalizedTextPresenter_SetFontResources(&provider);
    ok = Playback(p);
    char poster[4096];
    if (ok && snprintf(poster, sizeof(poster), "%s/poster.png", p->output) <
                  (int)sizeof(poster))
      ok = SDL_SavePNG(p->canvas, poster);
  }
  if (p->report && fclose(p->report))
    ok = false;
  if (!ok)
    fprintf(stderr, "text preview: %s\n",
            p->error[0]         ? p->error
            : SDL_GetError()[0] ? SDL_GetError()
                                : "invalid request or unsupported layout");
  ArLocalizedTextPresenter_Reset(&p->device);
  ArLocalizedTextPresenter_SetFontResources(NULL);
  ArLocalizedTextPresenter_SetBackend(NULL);
  SDL_DestroySurface(p->sheet);
  SDL_DestroyRenderer(p->renderer);
  SDL_DestroySurface(p->canvas);
  ArDialogueSession_Destroy(&p->session);
  ArLanguagePack_Destroy(&p->pack);
  ArLanguagePack_Destroy(&p->fallback);
  if (!ArHostFontResources_Destroy(&p->resources))
    ok = false;
  free(p);
  return ok ? 0 : 2;
}
