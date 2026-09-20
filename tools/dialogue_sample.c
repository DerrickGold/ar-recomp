/* ROM-free executable documentation for the text/dialogue core.
 *
 * ar_dialogue_sample --offscreen OUTPUT.png BODY_FONT ALTERNATE_FONT
 * ar_dialogue_sample --interactive BODY_FONT ALTERNATE_FONT
 *
 * Interactive: Tab chooses a panel; Space advances it; Return advances both;
 * F5 saves the chosen session and F9 restores it. Escape exits. The host
 * supplies one reveal/wait tick every 40 ms, independent of the renderer. */
#include "host/font_resources.h"
#include "localization/dialogue_session.h"
#include "platform/sdl/text_rasterizer_sdl.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  kPanelWidth = 450,
  kPanelHeight = 170,
  kMargin = 20,
  kCanvasWidth = 960,
  kCanvasHeight = 230
};

static const char kScript[] =
    "@define-style prose band=#DBEAFE body=#FFFFFF shadow=#172038\n"
    "@define-style warm band=#FFAB42 body=#FFE5A3 shadow=#241A10 "
    "shape=keyline\n"
    ":: sample.source\n@style prose\n"
    "Welcome, {name}.\n@line\n"
    "You found <i>{count:02}</i> <span font=\"display\" style=\"warm\" "
    "scale=\"120%\">stars</span>.\n"
    "@wait 8\n@line\n<span scale=\"80%\">Look up and make a wish.</span>\n"
    "@anchor sample.arrived\n@page\nThe next page keeps its own reveal "
    "clock.\n@end\n"
    ":: sample.translation\n@style prose\n"
    "Bienvenue, {name}.\n@line\n"
    "Vous avez trouvé <i>{count:02}</i> <span font=\"display\" style=\"warm\" "
    "scale=\"120%\">étoiles</span>.\n"
    "@wait 8\n@line\n<span scale=\"80%\">Levez les yeux et faites un "
    "vœu.</span>\n"
    "@anchor sample.arrived\n@page\nUne nouvelle page a son propre "
    "rythme.\n@end\n";

typedef struct Panel {
  ArDialogueSession session;
  ArDialogueSource source;
  ArDialogueStableState saved;
  ArTextBitmap bitmap;
  uint32_t prepared_page;
  unsigned delivered_controls;
  int count;
  bool has_saved;
} Panel;

static const ArDialogueContract kContract = {
    .values = {{"name", kArLanguagePlaceholder_LocalizedText},
               {"count", kArLanguagePlaceholder_Number}},
    .value_count = 2,
    .control_count = 1};

static bool Resolve(void *context, const char *name,
                    ArLanguagePlaceholderKind kind, ArDialogueValue *value,
                    char *error, size_t capacity) {
  (void)error;
  (void)capacity;
  value->kind = kind;
  if (!strcmp(name, "name") && kind == kArLanguagePlaceholder_LocalizedText) {
    snprintf(value->text, sizeof(value->text), "Amélie");
    return true;
  }
  if (strcmp(name, "count") || kind != kArLanguagePlaceholder_Number)
    return false;
  value->number = ((Panel *)context)->count;
  return true;
}

static const ArTextTreatment *FindTreatment(void *context, const char *name) {
  const ArDialoguePageSnapshot *page = context;
  for (size_t i = 0; i < page->treatment_count; ++i)
    if (!strcmp(page->treatments[i].definition.name, name))
      return &page->treatments[i].definition;
  return NULL;
}

static bool Prepare(Panel *panel, const ArTextRasterizer *rasterizer,
                    char *error, size_t capacity) {
  ArDialoguePageSnapshot page;
  if (!ArDialogueSession_GetPage(&panel->session, &page))
    return false;
  if (panel->bitmap.pixels && panel->prepared_page == page.page_index)
    return true;
  ArTextAppearanceSpan *spans =
      page.style_span_count ? calloc(page.style_span_count, sizeof(*spans))
                            : NULL;
  if (page.style_span_count && !spans)
    return false;
  ArTextRunAppearance base;
  ArTextTemplateError detail = {0};
  const ArTextAppearanceBindings bindings = {.context = &page,
                                             .find_treatment = FindTreatment};
  bool ok = ArTextTemplate_ResolveAppearance(
      &page.default_style, NULL, page.numerals == 2, &bindings, &base, &detail);
  for (size_t i = 0; ok && i < page.style_span_count; ++i) {
    spans[i].start = page.style_spans[i].start;
    spans[i].end = page.style_spans[i].end;
    ok = ArTextTemplate_ResolveAppearance(
        &page.default_style, &page.style_spans[i].style, page.numerals == 2,
        &bindings, &spans[i].appearance, &detail);
  }
  if (!ok) {
    snprintf(error, capacity, "%s", detail.message);
    free(spans);
    return false;
  }
  const ArTextRasterRequest request = {
      .struct_size = sizeof(request),
      .abi_version = AR_TEXT_RASTER_REQUEST_ABI_VERSION,
      .utf8 = page.utf8,
      .utf8_bytes = page.utf8_bytes,
      .font_stack_id = "sample",
      .font_stack_id_bytes = 6,
      .font_revision = 1,
      .source_revision = page.source_revision,
      .appearance = &base,
      .appearance_spans = spans,
      .appearance_span_count = page.style_span_count,
      .bidi_spans = page.bidi_spans,
      .bidi_span_count = page.bidi_span_count,
      .font_pixels = 24,
      .minimum_font_pixels = 18,
      .maximum_width = kPanelWidth,
      .maximum_height = kPanelHeight,
      .flags = kArTextRasterFlag_WrapWords |
               kArTextRasterFlag_PreserveHardBreaks |
               kArTextRasterFlag_IncludeRevealClusters,
      .direction = kArTextDirection_Auto,
      .filter = kArRenderFilter_Linear,
      .language_bcp47 = page.locale,
      .language_bcp47_bytes = strlen(page.locale)};
  ArTextBitmap next = {0};
  ok = ArTextRasterizer_Rasterize(rasterizer, &request, &next, NULL, error,
                                  capacity);
  free(spans);
  if (!ok)
    return false;
  ArTextRasterizer_ReleaseBitmap(rasterizer, &panel->bitmap);
  panel->bitmap = next;
  panel->prepared_page = page.page_index;
  printf("%s:%u %s: page %u, requested 24px, fitted %dpx, %zu lines\n",
         page.source_path, page.source_line, page.resolved_message_id,
         page.page_index + 1, next.font_pixels, next.line_count);
  return true;
}

static bool Tick(Panel *panel, ArLanguagePackError *error) {
  if (panel->session.state.wait_frames_remaining) {
    ArDialogueSession_TickWait(&panel->session, 1);
    return true;
  }
  ArDialogueToken token;
  if (!ArDialogueSession_Next(&panel->session, &token, error))
    return false;
  if (token.kind == kArDialogueToken_Control) {
    /* This host's event is a counter. The core neither knows nor executes it.
     */
    if (strcmp(token.control_id, "sample.arrived"))
      return false;
    ++panel->delivered_controls;
    return ArDialogueSession_CompleteControl(&panel->session,
                                             token.control_ordinal);
  }
  return true;
}

static void Advance(Panel *panel) {
  if (panel->session.state.awaiting_page_advance)
    ArDialogueSession_AdvancePage(&panel->session);
  else
    ArDialogueSession_ResumeInput(&panel->session);
}

static bool DrawPanel(SDL_Surface *canvas, const Panel *panel, int x, int y) {
  const ArTextBitmap *bitmap = &panel->bitmap;
  ArDialoguePageSnapshot page;
  if (!bitmap->pixels || !ArDialogueSession_GetPage(&panel->session, &page))
    return false;
  SDL_Surface *visible = SDL_CreateSurface(bitmap->width, bitmap->height,
                                           SDL_PIXELFORMAT_RGBA8888);
  if (!visible)
    return false;
  /* Reveal from the immutable complete-page bitmap. Shadow/slanted pixels
   * follow their owning shaped cluster, including ligatures and RTL text. */
  for (int row = 0; row < bitmap->height; ++row)
    for (int col = 0; col < bitmap->width; ++col) {
      const uint32_t owner =
          bitmap->pixel_owners[(size_t)row * bitmap->width + col];
      if (!owner || bitmap->reveal_clusters[owner - 1].end_utf8_byte >
                        page.revealed_utf8_bytes)
        continue;
      memcpy((uint8_t *)visible->pixels + (size_t)row * visible->pitch +
                 (size_t)col * 4,
             (const uint8_t *)bitmap->pixels +
                 (size_t)row * bitmap->pitch_bytes + (size_t)col * 4,
             4);
    }
  SDL_Rect destination = {x, y, visible->w, visible->h};
  const bool ok = SDL_BlitSurface(visible, NULL, canvas, &destination);
  SDL_DestroySurface(visible);
  return ok;
}

static bool Draw(SDL_Surface *canvas, Panel *panels, int active, int top) {
  const SDL_PixelFormatDetails *format =
      SDL_GetPixelFormatDetails(canvas->format);
  SDL_Rect background = {0, top, kCanvasWidth, kCanvasHeight};
  SDL_FillSurfaceRect(canvas, &background,
                      SDL_MapRGB(format, NULL, 15, 23, 42));
  for (int i = 0; i < 2; ++i) {
    const int left = kMargin + i * (kPanelWidth + kMargin);
    SDL_Rect box = {left - 8, top + 24, kPanelWidth + 16, kPanelHeight + 16};
    SDL_FillSurfaceRect(canvas, &box, SDL_MapRGB(format, NULL, 29, 42, 62));
    SDL_Rect indicator = {left - 8, top + 24, 4, kPanelHeight + 16};
    if (active == i)
      SDL_FillSurfaceRect(canvas, &indicator,
                          SDL_MapRGB(format, NULL, 255, 190, 80));
    if (!DrawPanel(canvas, &panels[i], left, top + 32))
      return false;
  }
  return true;
}

static bool Restore(Panel *panel, ArLanguagePackError *error) {
  if (!panel->has_saved)
    return true;
  const ArDialogueValueResolver resolver = {
      .struct_size = sizeof(resolver),
      .abi_version = AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION,
      .context = panel,
      .resolve = Resolve};
  return ArDialogueSession_RestoreSource(&panel->session, &panel->source,
                                         &kContract, &panel->saved, &resolver,
                                         error);
}

static bool Offscreen(const char *path, Panel *panels,
                      const ArTextRasterizer *rasterizer,
                      ArLanguagePackError *error) {
  SDL_Surface *sheet = SDL_CreateSurface(kCanvasWidth, kCanvasHeight * 4,
                                         SDL_PIXELFORMAT_RGBA8888);
  if (!sheet)
    return false;
  bool ok = true;
  /* Different clocks prove session independence. Saving/restoring at a partial
   * reveal must leave the other session and both prepared bitmaps untouched. */
  for (unsigned tick = 0; ok && tick < 240; ++tick) {
    ok = Tick(&panels[0], error);
    if (tick % 2 == 0)
      ok = ok && Tick(&panels[1], error);
    if (tick == 12) {
      panels[0].has_saved =
          ArDialogueSession_ExportState(&panels[0].session, &panels[0].saved);
      const uint32_t other = panels[1].session.state.revealed_cluster_count;
      const void *prepared = panels[0].bitmap.pixels;
      ok = ok && Restore(&panels[0], error) &&
           other == panels[1].session.state.revealed_cluster_count &&
           prepared == panels[0].bitmap.pixels;
    }
    if (tick == 150)
      Advance(&panels[0]);
    for (int i = 0; ok && i < 2; ++i)
      ok = Prepare(&panels[i], rasterizer, error->message,
                   sizeof(error->message));
    if (ok && tick % 60 == 59)
      ok = Draw(sheet, panels, 0, (int)(tick / 60) * kCanvasHeight);
  }
  ok = ok && panels[0].delivered_controls == 1 &&
       panels[1].delivered_controls == 1 &&
       panels[0].session.state.authored_page_index == 1 &&
       panels[1].session.state.authored_page_index == 0;
  if (!ok && !error->message[0])
    snprintf(error->message, sizeof(error->message),
             "independent-session proof failed: controls %u/%u, pages %u/%u",
             panels[0].delivered_controls, panels[1].delivered_controls,
             panels[0].session.state.authored_page_index,
             panels[1].session.state.authored_page_index);
  if (ok)
    ok = SDL_SavePNG(sheet, path);
  SDL_DestroySurface(sheet);
  return ok;
}

static bool Interactive(Panel *panels, const ArTextRasterizer *rasterizer,
                        ArLanguagePackError *error) {
  if (!SDL_Init(SDL_INIT_VIDEO))
    return false;
  SDL_Window *window =
      SDL_CreateWindow("Portable dialogue — Tab selects, Space/Return advance, "
                       "F5/F9 save/restore",
                       kCanvasWidth, kCanvasHeight, 0);
  if (!window)
    return false;
  bool running = true, ok = true;
  int active = 0;
  Uint64 next_tick = SDL_GetTicks();
  while (running && ok) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT)
        running = false;
      if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat)
        continue;
      switch (event.key.key) {
      case SDLK_ESCAPE:
        running = false;
        break;
      case SDLK_TAB:
        active = 1 - active;
        break;
      case SDLK_SPACE:
        Advance(&panels[active]);
        break;
      case SDLK_RETURN:
        Advance(&panels[0]);
        Advance(&panels[1]);
        break;
      case SDLK_F5:
        panels[active].has_saved = ArDialogueSession_ExportState(
            &panels[active].session, &panels[active].saved);
        break;
      case SDLK_F9:
        ok = Restore(&panels[active], error);
        break;
      }
    }
    if (SDL_GetTicks() >= next_tick) {
      ok = ok && Tick(&panels[0], error) && Tick(&panels[1], error);
      next_tick = SDL_GetTicks() + 40;
    }
    for (int i = 0; ok && i < 2; ++i)
      ok = Prepare(&panels[i], rasterizer, error->message,
                   sizeof(error->message));
    SDL_Surface *canvas = SDL_GetWindowSurface(window);
    ok = ok && canvas && Draw(canvas, panels, active, 0) &&
         SDL_UpdateWindowSurface(window);
    SDL_Delay(8);
  }
  SDL_DestroyWindow(window);
  return ok;
}

int main(int argc, char **argv) {
  const bool offscreen = argc == 5 && !strcmp(argv[1], "--offscreen");
  if (!offscreen && !(argc == 4 && !strcmp(argv[1], "--interactive"))) {
    fprintf(stderr,
            "usage: %s --offscreen OUTPUT.png BODY_FONT ALTERNATE_FONT\n       "
            "%s --interactive "
            "BODY_FONT ALTERNATE_FONT\n",
            argv[0], argv[0]);
    return 2;
  }
  ArLanguagePack pack;
  ArLanguagePack_Init(&pack);
  ArLanguagePackError error = {0};
  const ArTextDocumentSource script = {"sample.artext", kScript,
                                       sizeof(kScript) - 1};
  const char *roles[] = {"display"};
  const ArTextDocumentConfig document = {.id = "sample",
                                         .locale = "fr",
                                         .format_version = 2,
                                         .sources = &script,
                                         .source_count = 1,
                                         .font_roles = roles,
                                         .font_role_count = 1};
  ArHostFontResources resources = {0};
  ArSdlTextRasterizer adapter = {0};
  Panel panels[2] = {0};
  bool ok = ArLanguagePack_ParseDocument(&pack, &document, &error);
  const ArFontResourceId body =
      ArHostFontResources_RegisterFile(&resources, argv[offscreen ? 3 : 2],
                                       error.message, sizeof(error.message));
  const ArFontResourceId alternate =
      ArHostFontResources_RegisterFile(&resources, argv[offscreen ? 4 : 3],
                                       error.message, sizeof(error.message));
  const ArTextFontRole display = {.name = "display",
                                  .primary = alternate,
                                  .fallbacks = {body},
                                  .fallback_count = 1};
  const ArTextBackendConfig config = {
      .struct_size = sizeof(config),
      .abi_version = AR_TEXT_BACKEND_CONFIG_ABI_VERSION,
      .font_stack_id = "sample",
      .font_revision = 1,
      .resources = ArHostFontResources_Provider(&resources),
      .primary_font = body,
      .fallback_fonts = &alternate,
      .fallback_font_count = 1,
      .roles = &display,
      .role_count = 1,
      .cached_size_capacity = 32};
  ok = ok && body && alternate &&
       ArSdlTextRasterizer_Init(&adapter, &config, error.message,
                                sizeof(error.message));
  const ArTextRasterizer *rasterizer = ArSdlTextRasterizer_Get(&adapter);
  const char *ids[] = {"sample.source", "sample.translation"};
  for (int i = 0; ok && i < 2; ++i) {
    ArDialogueSession_Init(&panels[i].session);
    panels[i].count = i ? 123 : 7;
    panels[i].source = (ArDialogueSource){
        .effective_pack = &pack,
        .message = ArLanguagePack_FindMessage(&pack, ids[i]),
        .resolved_source = kArDialogueResolvedSource_SelectedPack,
        .presentation = kArDialoguePresentation_Enhanced};
    const ArDialogueValueResolver resolver = {
        .struct_size = sizeof(resolver),
        .abi_version = AR_DIALOGUE_VALUE_RESOLVER_ABI_VERSION,
        .context = &panels[i],
        .resolve = Resolve};
    ok = ArDialogueSession_BeginSource(&panels[i].session, &panels[i].source,
                                       &kContract, ids[i], &resolver, 0,
                                       &error) &&
         Prepare(&panels[i], rasterizer, error.message, sizeof(error.message));
  }
  ok = ok && (offscreen ? Offscreen(argv[2], panels, rasterizer, &error)
                        : Interactive(panels, rasterizer, &error));
  for (int i = 0; i < 2; ++i) {
    if (rasterizer)
      ArTextRasterizer_ReleaseBitmap(rasterizer, &panels[i].bitmap);
    ArDialogueSession_Destroy(&panels[i].session);
  }
  ArLanguagePack_Destroy(&pack);
  ArSdlTextRasterizer_Destroy(&adapter);
  ok = ArHostFontResources_Destroy(&resources) && ok;
  if (!ok)
    fprintf(stderr, "dialogue sample: %s; %s\n", error.message, SDL_GetError());
  SDL_Quit();
  return ok ? 0 : 1;
}
