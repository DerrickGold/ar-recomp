/* Real shaping, fitting, treatments and presenter; synthetic artwork and a
 * texture sink keep this test independent of ROMs, windows and GPU drivers. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/sdl/text_rasterizer_sdl.h"
#include "host/font_resources.h"
#include "actraiser/actraiser_localization_grid.h"
#include "actraiser/actraiser_localization_world_navigation.h"
#include "render/localized_text_presenter.h"

static int failures, test_size, test_scale, test_treatment, test_example;
static ArHostFontResources s_font_store;
static ArFontResourceId s_test_font;
/* Named so a failure says which screen it was drawing, not just an index. */
static const char *test_case = "";
#define CHECK(value) do { if (!(value)) { \
  fprintf(stderr, "%s:%d: %s (%s: size=%d scale=%d treatment=%d example=%d)\n", \
      __FILE__, __LINE__, #value, test_case, test_size, test_scale, \
      test_treatment, test_example); \
  ++failures; \
} } while (0)

enum { kRecordedDraws = 512 };
typedef struct TextureSink {
  uintptr_t next;
  unsigned uploads, live, fail_upload_at;
  ArRenderColorF last_tint;
  unsigned draws;
  unsigned geometry_draws;
  double geometry_area;
  const ArLocalizedPreparedText *verify_text;
  const int *verify_shifts;
  ArRenderTextureDesc last_descriptor;
  ArRenderRectF draw_source[kRecordedDraws];
  bool fail_create, fail_upload;
  bool alive[65536];
} TextureSink;
static bool Create(void *context, const ArRenderTextureDesc *desc, ArRenderTexture *out) {
  TextureSink *sink = context;
  if (desc) sink->last_descriptor = *desc;
  if (sink->fail_create || sink->next + 1 >= sizeof(sink->alive)) return false;
  *out = (ArRenderTexture){++sink->next};
  sink->alive[out->value] = true;
  ++sink->live;
  return true;
}
static void Destroy(void *context, ArRenderTexture texture) {
  TextureSink *sink = context;
  CHECK(texture.value < sizeof(sink->alive) && sink->alive[texture.value]);
  sink->alive[texture.value] = false;
  CHECK(sink->live > 0);
  --sink->live;
}
static bool Upload(void *context, ArRenderTexture texture, const ArRenderRectI *rect,
                   const void *pixels, int pitch) {
  TextureSink *sink = context;
  (void)texture; (void)rect;
  ++sink->uploads;
  return !sink->fail_upload && sink->uploads != sink->fail_upload_at && pixels && pitch > 0;
}
static bool Target(void *context, ArRenderTexture texture) {
  (void)context; (void)texture; return true;
}
static bool NoArgs(void *context) { (void)context; return true; }
static bool Size(void *context, int *w, int *h) {
  (void)context; *w = 1536; *h = 1344; return true;
}
static bool Rect(void *context, const ArRenderRectI *rect) {
  (void)context; (void)rect; return true;
}
static bool Clear(void *context, ArRenderColorF color) {
  (void)context; (void)color; return true;
}
static bool Draw(void *context, ArRenderTexture texture, const ArRenderRectF *src,
                 const ArRenderRectF *dst, const ArRenderDrawState *state) {
  TextureSink *sink = context;
  CHECK(!texture.value || (texture.value < sizeof(sink->alive) && sink->alive[texture.value]));
  sink->last_tint = state && (state->flags & kArRenderDrawState_Tint)
      ? state->tint : (ArRenderColorF){1, 1, 1, 1};
  if (sink->draws < kRecordedDraws)
    sink->draw_source[sink->draws] =
        src ? *src : (ArRenderRectF){0, 0, 0, 0};
  ++sink->draws;
  (void)dst; return true;
}
static bool Geometry(void *context, ArRenderTexture texture,
                     const ArRenderVertex2D *vertices, int count,
                     const int32_t *indices, int index_count,
                     const ArRenderDrawState *state) {
  TextureSink *sink = context;
  if (texture.value) {
    CHECK(texture.value < sizeof(sink->alive) && sink->alive[texture.value]);
    CHECK(count % 4 == 0 && index_count == count / 4 * 6);
    for (int i = 0; i < count; i += 4) {
      if (sink->verify_text) {
        const ArLocalizedPreparedText *text = sink->verify_text;
        const int x = (int)lroundf(vertices[i].tex_coord.x * text->surface.width);
        const int y = (int)lroundf(vertices[i].tex_coord.y * text->surface.height);
        bool found = false;
        for (size_t p = 0; p < text->surface.reveal_piece_count; ++p) {
          const ArTextRevealPiece *piece = &text->surface.reveal_pieces[p];
          if (piece->source.x != x || piece->source.y != y) continue;
          CHECK(vertices[i].position.x == text->destination.x + x +
              sink->verify_shifts[piece->cluster_index]);
          CHECK(vertices[i].position.y == text->destination.y + y);
          found = true;
          break;
        }
        CHECK(found);
      }
      sink->geometry_area +=
          (vertices[i + 2].position.x - vertices[i].position.x) *
          (vertices[i + 2].position.y - vertices[i].position.y);
      for (int j = 0; j < 4; ++j) {
        CHECK(vertices[i + j].tex_coord.x >= 0 && vertices[i + j].tex_coord.x <= 1);
        CHECK(vertices[i + j].tex_coord.y >= 0 && vertices[i + j].tex_coord.y <= 1);
      }
    }
    for (int i = 0; i < index_count; ++i)
      CHECK(indices[i] >= 0 && indices[i] < count);
    sink->last_tint = vertices[0].color;
    ++sink->geometry_draws;
  }
  (void)state;
  return true;
}
static const char *Error(void *context) { (void)context; return "texture sink"; }
static const ArRenderBackendOps sink_ops = {
  .struct_size = sizeof(sink_ops), .create_texture = Create,
  .destroy_texture = Destroy, .update_texture = Upload,
  .set_render_target = Target, .use_output_coordinates = NoArgs,
  .get_output_size = Size, .set_viewport = Rect, .set_clip_rect = Rect,
  .clear = Clear, .draw_texture = Draw, .draw_geometry = Geometry,
  .present = NoArgs, .last_error = Error,
};

static void SetArt(ArLocalizationArtwork *art, int width) {
  *art = (ArLocalizationArtwork){.width = width, .height = 8, .valid = true};
  /* Deliberately asymmetric transparent padding, unrelated to a ROM tile. */
  for (int y = 1; y < 4; ++y)
    for (int x = 0; x < width; ++x)
      art->argb[y * width + x] = UINT32_C(0xffffffff);
}

static ArRenderRectI Ink(const ArLocalizedPreparedText *text) {
  ArRenderRectI ink = text->surface.ink_bounds;
  ink.x += text->destination.x;
  ink.y += text->destination.y;
  return ink;
}

static void Exercise(ArRenderDevice *device, ArEnhancedTextSettings settings,
                     int scale, int example) {
  const char *texts[] = {
    "0|1|2|3|4|5|6|7|8|9\n\nFast|\u2007|Slow",
    "0|1|2|3|4|5|6|7|8|9\n\nVif|\u2007|Léger",
    "\n\u2003x9",
    "\n\n\nArea|\u2007|State|Lv|Objets",
    "Name\n\n  \u2007Ég  I", /* Selected key, not the blank placeholder. */
    "Name\n\n   Ég \u2007I", /* Same row, selected narrow unaccented key. */
  };
  static const char *const names[] = {
    "message speed scale", "message speed scale (fr)", "master status",
    "cities report", "name entry (accented key)", "name entry (narrow key)",
  };
  test_case = names[example];
  const char *text = texts[example];
  const char *placeholder = strstr(text, example == 2 ? "\u2003" : "\u2007");
  const ArLocalizationInlineObjectKind kinds[] = {
    kArLocalizationInlineObject_SpeedDirection, kArLocalizationInlineObject_SpeedDirection,
    kArLocalizationInlineObject_StatusLife, kArLocalizationInlineObject_StatusPopulation,
    kArLocalizationInlineObject_NameCursor,
    kArLocalizationInlineObject_NameCursor,
  };
  const ActRaiserLocalizationMenu menus[] = {
    kActRaiserLocalizationMenu_MessageSpeed, kActRaiserLocalizationMenu_MessageSpeed,
    kActRaiserLocalizationMenu_StatusMaster, kActRaiserLocalizationMenu_StatusCities,
    kActRaiserLocalizationMenu_None,
    kActRaiserLocalizationMenu_None,
  };
  const ArTextCellRegion regions[] = {
    {18, 12, 10, 4}, {18, 12, 10, 4}, {10, 6, 12, 17},
    {3, 6, 26, 20}, {3, 7, 27, 16}, {3, 7, 27, 16},
  };
  ArLocalizationFrame frame;
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(&frame, "fr", "test", s_test_font, 1, &settings));
  SetArt(&frame.artwork[kArLocalizationArtwork_SpeedDirection], 16);
  SetArt(&frame.artwork[kArLocalizationArtwork_Life], 8);
  SetArt(&frame.artwork[kArLocalizationArtwork_Population], 16);
  memcpy(frame.name_cursor_argb, frame.artwork[kArLocalizationArtwork_Life].argb,
         sizeof(frame.name_cursor_argb));
  frame.name_cursor_valid = true;
  const ArLocalizationInlineObjectSnapshot object = {
    kinds[example], (uint32_t)(placeholder - text + 3),
  };
  if (menus[example] != kActRaiserLocalizationMenu_None) {
    ArLocalizationTextGrid grid;
    CHECK(ActRaiserLocalizationGrid_Build(menus[example], regions[example],
                                          &grid));
    CHECK(ArLocalizationFrame_AddTextWithGrid(
        &frame, 1, (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
        regions[example], text, strlen(text), 100, 100, 1,
        kArTextDirection_LeftToRight, example >= 3 ? 8 : 7, &grid, NULL,
        NULL, 0, &object, 1));
  } else {
    CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(
        &frame, 1, (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
        regions[example], text, strlen(text), 100, 100, 1,
        kArTextDirection_LeftToRight, example >= 3 ? 8 : 7,
        kArLocalizationTextLayout_Flow, NULL, 0, &object, 1));
    /* A keyboard states the blank between its keys; the selector is sized to
     * the room that leaves, not to a fraction of the line. */
    CHECK(ArLocalizationFrame_SetKeySeparator(&frame, " ", 1));
  }
  const HudPresentationChunk chunk = {
    .inspector_kind = kInspectorPresentation_HudBg,
    .screen_source = {0, 0, 256, 224}, .texture_source = {0, 0, 256, 224},
    .output_destination = {0, 0, 256 * scale, 224 * scale},
  };
  frame.snapshots[0].shadow_enabled = true;
  frame.snapshots[0].slant_ascii_numerals = true;
  ArLocalizedPreparedFrame prepared;
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                  256, 224, &chunk, 1, &prepared);
  CHECK(prepared.mask_count > 0 && prepared.text_count > 0);
  CHECK(prepared.inline_object_count == 1);
  if (prepared.inline_object_count != 1 || !prepared.text_count) return;
  int top = INT32_MAX, bottom = 0;
  if (example >= 4) {
    const ArTextSurface *surface = &prepared.texts[0].surface;
    int line = -1;
    for (size_t i = 0; i < surface->reveal_cluster_count; ++i) {
      if (surface->reveal_clusters[i].end_utf8_byte == object.end_utf8_byte)
        line = surface->reveal_clusters[i].line_index;
    }
    for (size_t i = 0; i < surface->reveal_cluster_count; ++i) {
      const ArRenderRectI ink = surface->cluster_ink_bounds[i];
      if (surface->reveal_clusters[i].line_index != line || !ink.h) continue;
      const int y = prepared.texts[0].destination.y + ink.y;
      if (y < top) top = y;
      if (y + ink.h > bottom) bottom = y + ink.h;
    }
  } else {
    for (size_t i = example <= 1 ? 10 : 0; i < prepared.text_count; ++i) {
      const ArRenderRectI ink = Ink(&prepared.texts[i]);
      if (!ink.h) continue;
      if (ink.y < top) top = ink.y;
      if (ink.y + ink.h > bottom) bottom = ink.y + ink.h;
    }
  }
  CHECK(top < bottom);
  if (example == 3) {
    const int baseline = prepared.texts[0].destination.y + prepared.texts[0].surface.ascent;
    for (size_t i = 1; i < prepared.text_count; ++i) {
      if (prepared.texts[i].surface.ink_bounds.h)
        CHECK(prepared.texts[i].destination.y + prepared.texts[i].surface.ascent == baseline);
    }
  }
  const ArRenderRectI placed = prepared.inline_objects[0].destination;
  if (example >= 4) {
    /* The selector fits the gutter the game declared, not the line height. */
    CHECK(placed.h > 0 && placed.h < prepared.texts[0].surface.line_advance);
  }
  const double art_center = placed.y + (1.0 + 3.0 / 2.0) * placed.h / 8.0;
  CHECK(fabs(art_center - (top + bottom) / 2.0) <= 0.75);
  const ArTextCellRegion region = regions[example];
  CHECK(placed.x >= region.column * 8 * scale);
  CHECK(placed.y >= region.row * 8 * scale);
  CHECK(placed.x + placed.w <= (region.column + region.columns) * 8 * scale);
  CHECK(placed.y + placed.h <= (region.row + region.rows) * 8 * scale);
  if (example <= 1) {
    const ArRenderRectI left = Ink(&prepared.texts[10]);
    const ArRenderRectI right = Ink(&prepared.texts[12]);
    CHECK(abs((placed.x - left.x - left.w) - (right.x - placed.x - placed.w)) <= 1);
  }
  /* Static settings/menu content never cause repeated font work/uploads. */
  const unsigned uploads = ((TextureSink *)device->context)->uploads;
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                  256, 224, &chunk, 1, &prepared);
  CHECK(((TextureSink *)device->context)->uploads == uploads);
}

static void ExerciseSingleLine(ArRenderDevice *device,
                               ArEnhancedTextSettings settings, int scale) {
  test_case = "single line label";
  const char *labels[] = {"ESTUARY", "Région de l'Été", "Upper Valley"};
  for (size_t label = 0; label < sizeof(labels) / sizeof(labels[0]); ++label) {
    for (int rtl = 0; rtl <= 1; ++rtl) {
      ArLocalizationFrame frame;
      ArLocalizationFrame_Reset(&frame);
      CHECK(ArLocalizationFrame_SetFont(
          &frame, "fr", "test", s_test_font, 1, &settings));
      CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(
          &frame, 4, (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
          (ArTextCellRegion){6, 1, 12, 1}, labels[label], strlen(labels[label]),
          100, 100, 1, rtl ? kArTextDirection_RightToLeft : kArTextDirection_LeftToRight,
          7, kArLocalizationTextLayout_SingleLineLabel, NULL, 0, NULL, 0));
      const HudPresentationChunk chunk = {
        .inspector_kind = kInspectorPresentation_HudBg,
        .screen_source = {0, 0, 168, 32}, .texture_source = {0, 0, 168, 32},
        .output_destination = {11, 13, 168 * scale, 32 * scale},
      };
      ArLocalizedPreparedFrame prepared;
      ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                      256, 224, &chunk, 1, &prepared);
      CHECK(prepared.mask_count == 1 && prepared.text_count == 1);
      if (prepared.mask_count != 1 || prepared.text_count != 1) continue;
      /* Native mode label, health row and magic icon never belong to this
       * replacement, regardless of size, sampling, accent or direction. */
      /* The BG fetch phase places tile row 1 at visible scanline 7. */
      CHECK(prepared.masks[0].x == 48 && prepared.masks[0].y == 7);
      CHECK(prepared.masks[0].w == 96 && prepared.masks[0].h == 8);
      const ArRenderRectI dst = prepared.texts[0].destination;
      CHECK(rtl ? dst.x + dst.w == 11 + 144 * scale : dst.x == 11 + 48 * scale);
      CHECK(dst.w <= 96 * scale && dst.h <= 8 * scale);
      CHECK(dst.y >= 13 + 7 * scale && dst.y + dst.h <= 13 + 15 * scale);
      CHECK(abs((dst.y - 13 - 7 * scale) - (13 + 15 * scale - dst.y - dst.h)) <= 1);
      const unsigned uploads = ((TextureSink *)device->context)->uploads;
      ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                      256, 224, &chunk, 1, &prepared);
      CHECK(((TextureSink *)device->context)->uploads == uploads);
    }
  }
}

static void ExerciseLabelBreaks(ArRenderDevice *device) {
  test_case = "authored breaks survive label placement";
  const ArLocalizationTextLayoutKind layouts[] = {
      kArLocalizationTextLayout_SingleLineLabel,
      kArLocalizationTextLayout_CenteredLabel,
      kArLocalizationTextLayout_RightAlignedLabel,
      kArLocalizationTextLayout_LeftAlignedLabel,
      kArLocalizationTextLayout_FramedLabel,
      kArLocalizationTextLayout_CenteredBlock};
  const char text[] = "HHHHH\n\nH";
  const HudPresentationChunk chunk = {
      .inspector_kind = kInspectorPresentation_HudBg,
      .screen_source = {0, 0, 256, 224}, .texture_source = {0, 0, 256, 224},
      .output_destination = {0, 0, 768, 672}};
  for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); ++i)
  for (unsigned styled = 0; styled < 2; ++styled)
  for (unsigned screen = 0; screen < 2; ++screen) {
    if (screen && (layouts[i] == kArLocalizationTextLayout_FramedLabel ||
                   layouts[i] == kArLocalizationTextLayout_CenteredBlock))
      continue;
    test_example = (int)i;
    ArLocalizationFrame frame;
    ArLocalizationFrame_Reset(&frame);
    CHECK(ArLocalizationFrame_SetFont(&frame, "en", "test", s_test_font, 1,
                                      &frame.settings));
    if (screen)
      CHECK(ArLocalizationFrame_AddScreenText(&frame, 4, 24, 48, 208, 48,
          text, sizeof(text) - 1, 8, 8, 1, kArTextDirection_LeftToRight, 7,
          layouts[i]));
    else
      CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(&frame, 4,
          (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
          (ArTextCellRegion){3, 6, 26, 6}, text, sizeof(text) - 1, 8, 8, 1,
          kArTextDirection_LeftToRight, 7, layouts[i], NULL, 0, NULL, 0));
    if (styled) {
      const ArTextRunAppearance base = {.font_role = "body",
          .scale_basis = 10000, .band_rgb = 0xffffff, .body_rgb = 0xffffff};
      const ArTextAppearanceSpan span = {.start = 7, .end = 8,
          .appearance = {.font_role = "body", .scale_basis = 8000,
                        .band_rgb = 0xff8800, .body_rgb = 0xff8800,
                        .italic = true}};
      CHECK(ArLocalizationFrame_SetTextAppearance(&frame, &base, &span, 1));
    }
    SetArt(&frame.artwork[kArLocalizationArtwork_LabelFrameLeft], 8);
    SetArt(&frame.artwork[kArLocalizationArtwork_LabelFrameRight], 7);
    ArLocalizedPreparedFrame prepared;
    if (screen)
      CHECK(ArLocalizedTextPresenter_PrepareScreenText(device, &frame, 4,
          (ArRenderRectI){72, 144, 624, 144}, &prepared));
    else
      ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                      256, 224, &chunk, 1, &prepared);
    CHECK(prepared.text_count == 1);
    if (prepared.text_count != 1) continue;
    const ArTextSurface *surface = &prepared.texts[0].surface;
    if (styled) CHECK(surface->line_count == 3);
    CHECK(surface->reveal_cluster_count > 0);
    if (!surface->reveal_cluster_count) continue;
    const ArTextRevealCluster *first = &surface->reveal_clusters[0];
    const ArTextRevealCluster *last =
        &surface->reveal_clusters[surface->reveal_cluster_count - 1];
    CHECK(first->line_index == 0 && last->line_index == 2);
    CHECK(last->end_utf8_byte == sizeof(text) - 1 && last->y > first->y);
    if (layouts[i] != kArLocalizationTextLayout_SingleLineLabel &&
        layouts[i] != kArLocalizationTextLayout_LeftAlignedLabel)
      CHECK(last->x > first->x);
  }
  /* A tiny field must fail fitting instead of flattening forty authored rows
   * into a line that could fit horizontally. Keep its native pixels intact. */
  ArLocalizationFrame frame;
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(&frame, "en", "test", s_test_font, 1,
                                    &frame.settings));
  char crowded[80];
  for (size_t i = 0; i < sizeof(crowded) - 1; ++i)
    crowded[i] = i % 2 ? '\n' : 'H';
  crowded[sizeof(crowded) - 1] = 0;
  CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(&frame, 4,
      (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
      (ArTextCellRegion){3, 6, 26, 1}, crowded, sizeof(crowded) - 1, 79, 79, 1,
      kArTextDirection_LeftToRight, 7, kArLocalizationTextLayout_SingleLineLabel,
      NULL, 0, NULL, 0));
  ArLocalizedPreparedFrame prepared;
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                  256, 224, &chunk, 1, &prepared);
  CHECK(!prepared.text_count && !prepared.mask_count);
  ArLocalizedTextPresenter_Reset(device);
}

static void ExerciseScreenText(ArRenderDevice *device) {
  test_case = "authentic screen-space label";
  ArLocalizationFrame frame;
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(
      &frame, "fr", "test", s_test_font, 1, &frame.settings));
  const char text[] = "Région de l'Été";
  CHECK(ArLocalizationFrame_AddScreenText(
      &frame, kActRaiserLocalizationWorldNavigationSurface, 156, 25, 76, 8,
      text, sizeof(text) - 1, 15, 15, 3,
      kArTextDirection_LeftToRight, 7,
      kArLocalizationTextLayout_SingleLineLabel));
  ArLocalizedPreparedFrame prepared;
  const ArRenderRectI bounds = {100, 50, 304, 32};
  CHECK(ArLocalizedTextPresenter_PrepareScreenText(
      device, &frame, kActRaiserLocalizationWorldNavigationSurface,
      bounds, &prepared));
  CHECK(prepared.text_count == 1 && !prepared.mask_count);
  if (prepared.text_count == 1) {
    const ArRenderRectI dst = prepared.texts[0].destination;
    CHECK(dst.x == bounds.x && dst.w <= bounds.w && dst.h <= bounds.h);
    CHECK(dst.y >= bounds.y && dst.y + dst.h <= bounds.y + bounds.h);
    const unsigned uploads = ((TextureSink *)device->context)->uploads;
    CHECK(ArLocalizedTextPresenter_PrepareScreenText(
        device, &frame, kActRaiserLocalizationWorldNavigationSurface,
        bounds, &prepared));
    CHECK(((TextureSink *)device->context)->uploads == uploads);
    CHECK(ArLocalizedTextPresenter_DrawWithBrightness(
        device, &prepared, 0.5f));
    CHECK(fabsf(((TextureSink *)device->context)->last_tint.r - 0.5f) < .001f);
  }

  ArLocalizationTextLanguage rtl = {
      .locale = "fr", .direction = kArTextDirection_RightToLeft};
  CHECK(ArLocalizationFrame_SetTextLanguage(&frame, &rtl));
  CHECK(ArLocalizedTextPresenter_PrepareScreenText(
      device, &frame, kActRaiserLocalizationWorldNavigationSurface,
      bounds, &prepared));
  CHECK(prepared.text_count == 1);
  if (prepared.text_count == 1)
    CHECK(prepared.texts[0].destination.x +
              prepared.texts[0].destination.w == bounds.x + bounds.w);

  ArLocalizationFrame blank;
  ArLocalizationFrame_Reset(&blank);
  CHECK(ArLocalizationFrame_SetFont(
      &blank, "fr", "test", s_test_font, 1, &blank.settings));
  CHECK(ArLocalizationFrame_AddScreenText(
      &blank, kActRaiserLocalizationWorldNavigationSurface,
      156, 25, 76, 8, "", 0, 0, 0, 4,
      kArTextDirection_LeftToRight, 7,
      kArLocalizationTextLayout_SingleLineLabel));
  CHECK(ArLocalizedTextPresenter_PrepareScreenText(
      device, &blank, kActRaiserLocalizationWorldNavigationSurface,
      bounds, &prepared));
  CHECK(!prepared.text_count);
  CHECK(!ArLocalizedTextPresenter_PrepareScreenText(
      device, &blank, 99, bounds, &prepared));
}

static int ExerciseReport(ArRenderDevice *device, ArEnhancedTextSettings settings,
                           int scale, int example) {
  const bool score = example == 1;
  test_case = score ? "score report" : "cities report";
  char pressure[2048] = "\n\n\nWWWWWWW|WWWW|WWWW|WWWW|WWWW\n\n\n\n";
  if (example >= 2) {
    for (unsigned row = 0; row < 6; ++row) {
      const size_t used = strlen(pressure);
      snprintf(pressure + used, sizeof(pressure) - used,
          "%sWWWWWWW%u|WWWW%u|WWWW%u|WWWW%u|WWWW%u",
          row ? "\n\n" : "", row, row + 6, row + 12, row + 18, row + 24);
    }
  }
  const char *text = example >= 2 ? pressure : score
      ? "\n\n\nZone|Épreuve 1|Épreuve 2\n\n\n\n"
        "Westridge|009120|012300\n\nÎle Verte|120|002345"
      : "\n\n\nZone|\u2007|État|NV|OBJET\n\n\n\n"
        "Westridge|922|Max|3|0\n\nÎle Verte|002|Lent|1|4";
  ArLocalizationFrame frame;
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(&frame, "fr", "test", s_test_font, 1, &settings));
  /* A preceding HUD label must survive every cold fitting probe and eviction. */
  CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(
      &frame, 4, (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
      (ArTextCellRegion){6, 1, 12, 1}, "HUD", 3, 3, 3, 1,
      kArTextDirection_LeftToRight, 7, kArLocalizationTextLayout_SingleLineLabel,
      NULL, 0, NULL, 0));
  SetArt(&frame.artwork[kArLocalizationArtwork_Population], 16);
  const ArLocalizationInlineObjectSnapshot object = {
      kArLocalizationInlineObject_StatusPopulation,
      example ? 0 : (uint32_t)(strstr(text, "\u2007") - text + 3),
  };
  const ArTextCellRegion report_region = {3, 6, 26, 20};
  ArLocalizationTextGrid report_grid;
  CHECK(ActRaiserLocalizationGrid_Build(
      score ? kActRaiserLocalizationMenu_StatusScore
            : kActRaiserLocalizationMenu_StatusCities,
      report_region, &report_grid));
  CHECK(ArLocalizationFrame_AddTextWithGrid(
      &frame, 7, (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
      report_region, text, strlen(text), 100, 100, 1,
      kArTextDirection_LeftToRight, 8, &report_grid, NULL,
      NULL, 0, example ? NULL : &object, example ? 0 : 1));
  const HudPresentationChunk chunk = {
    .inspector_kind = kInspectorPresentation_HudBg,
    .screen_source = {0, 0, 256, 224}, .texture_source = {0, 0, 256, 224},
    .output_destination = {0, 0, (example == 3 ? 128 : 256) * scale, 224 * scale},
  };
  ArLocalizedPreparedFrame prepared;
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                  256, 224, &chunk, 1, &prepared);
  const unsigned columns = score ? 3 : 5;
  if (example == 3) {
    CHECK(prepared.mask_count == 1 && prepared.text_count == 1);
    CHECK(ArLocalizedTextPresenter_Draw(device, &prepared));
    const unsigned uploads = ((TextureSink *)device->context)->uploads;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                    256, 224, &chunk, 1, &prepared);
    CHECK(prepared.mask_count == 1 && prepared.text_count == 1);
    CHECK(((TextureSink *)device->context)->uploads == uploads);
    return 0;
  }
  const unsigned rows = example == 2 ? 7 : 3;
  CHECK(prepared.mask_count == 2 && prepared.text_count == 1 + rows * columns);
  if (prepared.text_count != 1 + rows * columns) return 0;
  CHECK(prepared.inline_object_count == (example ? 0 : 1));
  for (unsigned row = 0; row < rows; ++row) {
    for (unsigned col = 0; col < columns; ++col) {
      const ArLocalizedPreparedText *current = &prepared.texts[1 + row * columns + col];
      const ArRenderRectI dst = current->destination;
      CHECK(current->surface.ascent == prepared.texts[1].surface.ascent);
      CHECK(dst.x >= 24 * scale && dst.x + dst.w <= 232 * scale);
      if (col) {
        const ArRenderRectI previous = prepared.texts[row * columns + col].destination;
        CHECK(previous.x + previous.w + 2 * scale <= dst.x);
      }
      if (row == 2) {
        const ArRenderRectI previous_row = prepared.texts[1 + columns + col].destination;
        if (col == 0 || (!score && col == 2)) CHECK(dst.x == previous_row.x);
        else CHECK(dst.x + dst.w == previous_row.x + previous_row.w);
      }
    }
  }
  if (!example && settings.size_percent == 140 && scale == 4 &&
      settings.pixelation == kArEnhancedTextPixelation_Mosaic && settings.pixelation_size == 2) {
    CHECK(prepared.texts[5].destination.w > 24 * scale);
    CHECK(prepared.texts[5].destination.x < 208 * scale);
  }
  CHECK(ArLocalizedTextPresenter_Draw(device, &prepared));
  const unsigned uploads = ((TextureSink *)device->context)->uploads;
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                  256, 224, &chunk, 1, &prepared);
  CHECK(((TextureSink *)device->context)->uploads == uploads);
  CHECK(ArLocalizedTextPresenter_Draw(device, &prepared));
  return prepared.texts[1].surface.ascent;
}

static void ExercisePreflight(ArRenderDevice *device,
                              const ArTextBackend *backend) {
  test_case = "font preflight";
  TextureSink *sink = device->context;
  char error[kArTextRasterErrorCapacity];
  ArTextPresentationFont font = {
      .struct_size = sizeof(font),
      .abi_version = AR_TEXT_PRESENTATION_ABI_VERSION,
      .stack_id = "test",
      .primary = s_test_font,
      .revision = 1,
  };
  CHECK(
      !ArLocalizedTextPresenter_PrepareFont(NULL, &font, error, sizeof(error)));
  CHECK(error[0] && !sink->live);
  ArLocalizedTextPresenter_SetBackend(NULL);
  CHECK(!ArLocalizedTextPresenter_PrepareFont(device, &font, error,
                                              sizeof(error)));
  CHECK(error[0] && !sink->live);
  ArLocalizedTextPresenter_SetBackend(backend);
  ++font.abi_version;
  CHECK(!ArLocalizedTextPresenter_PrepareFont(device, &font, error,
                                              sizeof(error)));
  --font.abi_version;
  CHECK(ArLocalizedTextPresenter_PrepareFont(device, &font, error,
                                             sizeof(error)));
  CHECK(!error[0] && sink->live == 1);
  const unsigned warm_uploads = sink->uploads;
  CHECK(ArLocalizedTextPresenter_PrepareFont(device, &font, error,
                                             sizeof(error)));
  CHECK(sink->uploads == warm_uploads);

  ArTextPresentationFont candidate = font;
  candidate.revision = 2;
  const ArFontResourceId bad_fonts[] = {UINT64_MAX,
      ArHostFontResources_RegisterFile(&s_font_store, __FILE__, error, sizeof(error))};
  CHECK(bad_fonts[1]); /* Valid resource, invalid font bytes. */
  for (size_t i = 0; i < sizeof(bad_fonts) / sizeof(bad_fonts[0]); ++i) {
    candidate.primary = bad_fonts[i];
    CHECK(!ArLocalizedTextPresenter_PrepareFont(device, &candidate, error,
                                                sizeof(error)));
    CHECK(error[0] && sink->live == 1 && sink->uploads == warm_uploads);
    CHECK(ArLocalizedTextPresenter_PrepareFont(device, &font, error,
                                               sizeof(error)));
    CHECK(sink->uploads ==
          warm_uploads); /* Failed selection retained the cache. */
  }
  candidate.primary = ArHostFontResources_RegisterFile(&s_font_store, AR_TEST_FONT_PATH,
                                                       error, sizeof(error));
  CHECK(candidate.primary && candidate.primary != s_test_font);
  ArFontResourceId fallbacks[] = {UINT64_MAX};
  candidate.fallbacks = fallbacks;
  candidate.fallback_count = 1;
  CHECK(!ArLocalizedTextPresenter_PrepareFont(device, &candidate, error,
                                              sizeof(error)));
  CHECK(error[0] && sink->live == 1 && sink->uploads == warm_uploads);
  CHECK(ArLocalizedTextPresenter_PrepareFont(device, &font, error,
                                             sizeof(error)));
  fallbacks[0] = s_test_font;
  for (int failure = 0; failure < 2; ++failure) {
    sink->fail_create = failure == 0;
    sink->fail_upload = failure == 1;
    CHECK(!ArLocalizedTextPresenter_PrepareFont(device, &candidate, error,
                                                sizeof(error)));
    CHECK(error[0] && sink->live == 1);
    const unsigned uploads = sink->uploads;
    CHECK(ArLocalizedTextPresenter_PrepareFont(device, &font, error,
                                               sizeof(error)));
    CHECK(sink->uploads == uploads);
  }
  sink->fail_create = sink->fail_upload = false;
  CHECK(ArLocalizedTextPresenter_PrepareFont(device, &candidate, error,
                                             sizeof(error)));
  CHECK(sink->live == 1); /* Retry succeeds; previous resources retired once. */
  ArLocalizedTextPresenter_SetBackend(NULL);
  CHECK(!ArLocalizedTextPresenter_PrepareFont(device, &candidate, error,
                                              sizeof(error)));
  ArLocalizedTextPresenter_SetBackend(backend);
  const unsigned uploads = sink->uploads;
  CHECK(ArLocalizedTextPresenter_PrepareFont(device, &candidate, error,
                                             sizeof(error)));
  CHECK(sink->uploads == uploads + 1 && sink->live == 1);
  /* Frames carry the same ordered stack: no second open/upload at capture. */
  ArLocalizationFrame frame;
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(&frame, "en", candidate.stack_id,
                                    candidate.primary, candidate.revision,
                                    &frame.settings));
  CHECK(ArLocalizationFrame_SetFallbackFonts(&frame, fallbacks, 1));
  CHECK(ArLocalizationFrame_AddText(
      &frame, 1, (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
      (ArTextCellRegion){5, 19, 24, 6}, "Visible", 7, 7, 7, 1,
      kArTextDirection_LeftToRight, 8, NULL, 0));
  const HudPresentationChunk chunk = {
      .inspector_kind = kInspectorPresentation_HudBg,
      .screen_source = {0, 0, 256, 224},
      .texture_source = {0, 0, 256, 224},
      .output_destination = {0, 0, 1024, 896},
  };
  ArLocalizedPreparedFrame prepared;
  const unsigned pre_frame_uploads = sink->uploads;
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0, 256,
                                   224, &chunk, 1, &prepared);
  CHECK(prepared.text_count == 1 && sink->uploads == pre_frame_uploads + 1);
  CHECK(ArLocalizedTextPresenter_Draw(device, &prepared));
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0, 256,
                                   224, &chunk, 1, &prepared);
  CHECK(prepared.text_count == 1 && sink->uploads == pre_frame_uploads + 1);
  ++candidate.revision;
  CHECK(ArLocalizedTextPresenter_PrepareFont(device, &candidate, error,
                                             sizeof(error)));
  CHECK(sink->live == 3); /* Active probe/text plus one staged candidate. */
  CHECK(ArLocalizedTextPresenter_Draw(device, &prepared));
  const unsigned staged_uploads = sink->uploads;
  /* Semantic selection rejection keeps using the old frame/font. */
  ArLocalizedTextPresenter_DiscardPreparedFont(device);
  CHECK(sink->live == 2); /* Rejected candidate no longer consumes budget. */
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0, 256,
                                   224, &chunk, 1, &prepared);
  CHECK(prepared.text_count == 1 && sink->uploads == staged_uploads);
  CHECK(ArLocalizedTextPresenter_Draw(device, &prepared));
  /* Immutable old frames contain borrowed IDs. While its backend is pinned,
   * registration retirement has no effect, even on a new raster request. */
  ArHostFontResources_Retire(&s_font_store, candidate.primary);
  CHECK(!ArHostFontResources_Destroy(&s_font_store));
  ++frame.snapshots[0].source_revision;
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0, 256,
                                   224, &chunk, 1, &prepared);
  CHECK(prepared.text_count == 1);
  CHECK(ArLocalizedTextPresenter_Draw(device, &prepared));
  ArLocalizedTextPresenter_Reset(device);
  CHECK(sink->live == 0);
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0, 256,
                                   224, &chunk, 1, &prepared);
  CHECK(!prepared.text_count && !prepared.mask_count && sink->live == 0);
}

static void ExerciseFontRolePreflight(ArRenderDevice *device) {
  test_case = "font role preflight";
  TextureSink *sink = device->context;
  char error[kArTextRasterErrorCapacity];
  ArTextFontRole role = {.name = "hud", .primary = s_test_font};
  ArTextPresentationFont font = {.struct_size = sizeof(font),
                                 .abi_version =
                                     AR_TEXT_PRESENTATION_ABI_VERSION,
                                 .stack_id = "roles",
                                 .primary = s_test_font,
                                 .revision = 1,
                                 .roles = &role,
                                 .role_count = 1};
  CHECK(ArLocalizedTextPresenter_PrepareFont(device, &font, error,
                                             sizeof(error)));
  CHECK(sink->live ==
        2); /* Both named stacks actually rasterized and uploaded. */
  const unsigned warm = sink->uploads;
  CHECK(ArLocalizedTextPresenter_PrepareFont(device, &font, error,
                                             sizeof(error)));
  CHECK(sink->uploads == warm);
  const ArFontResourceId bad = ArHostFontResources_RegisterFile(
      &s_font_store, __FILE__, error, sizeof(error));
  CHECK(bad);
  role.primary = bad;
  CHECK(!ArLocalizedTextPresenter_PrepareFont(device, &font, error,
                                              sizeof(error)));
  CHECK(sink->live ==
        2); /* The rejected role cannot replace the ready selection. */
  role.primary = s_test_font;
  const unsigned rejected = sink->uploads;
  CHECK(ArLocalizedTextPresenter_PrepareFont(device, &font, error,
                                             sizeof(error)));
  CHECK(sink->uploads == rejected);
  ArLocalizedTextPresenter_DiscardPreparedFont(device);
  CHECK(sink->live == 0);
}

static void ExerciseDialogueFailure(ArRenderDevice *device) {
  test_case = "dialogue failure";
  ArLocalizedTextPresenter_Reset(device);
  TextureSink *sink = device->context;
  const ArTextCellDestination destination = {3, kArTextCellScreen_Composited,
                                             0};
  const ArTextCellRegion region = {5, 19, 24, 6};
  const HudPresentationChunk chunk = {
      .inspector_kind = kInspectorPresentation_HudBg,
      .screen_source = {0, 0, 256, 224},
      .texture_source = {0, 0, 256, 224},
      .output_destination = {0, 0, 1024, 896},
  };
  char tall[6001];
  for (size_t i = 0; i < sizeof(tall) - 1; ++i)
    tall[i] = i % 2 ? '\n' : 'W';
  tall[sizeof(tall) - 1] = 0;
  const char *texts[] = {tall, "Healthy", "Upload failure", ""};
  ArLocalizationFrame frame;
  ArLocalizedPreparedFrame prepared;
  for (size_t example = 0; example < 4; ++example) {
    ArLocalizationFrame_Reset(&frame);
    CHECK(ArLocalizationFrame_SetFont(&frame, "en", "test", s_test_font,
                                      1, &frame.settings));
    const size_t bytes = strlen(texts[example]);
    CHECK(ArLocalizationFrame_AddDialogueWindow(
        &frame, 1, destination, region, texts[example], bytes, (uint32_t)bytes,
        (uint32_t)bytes, example + 1, kArTextDirection_LeftToRight, 8));
    frame.dialogue_ticket = example + 1;
    frame.dialogue_surface_id = 1;
    sink->fail_upload = example == 2;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0, 256,
                                     224, &chunk, 1, &prepared);
    if (example == 0 || example == 2) {
      CHECK(!prepared.mask_count && !prepared.text_count &&
            !prepared.ready_dialogue_ticket);
    } else {
      CHECK(prepared.mask_count == 1 &&
            prepared.ready_dialogue_ticket == frame.dialogue_ticket);
    }
  }
  sink->fail_upload = false;
  SetArt(&frame.artwork[kArLocalizationArtwork_Continue], 8);
  CHECK(ArLocalizationFrame_AddIndicator(
      &frame, 1, kArLocalizationIndicator_DialogueContinue,
      (ArTextCellRegion){17, 24, 1, 1}));
  sink->fail_create = true;
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0, 256,
                                   224, &chunk, 1, &prepared);
  CHECK(!prepared.mask_count && !prepared.ready_dialogue_ticket &&
        !prepared.indicator_count);
  sink->fail_create = false;
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0, 256,
                                   224, &chunk, 1, &prepared);
  CHECK(prepared.mask_count == 1 &&
        prepared.ready_dialogue_ticket == frame.dialogue_ticket);
  CHECK(prepared.indicator_count == 1);
  ArLocalizedTextPresenter_Reset(device);
  CHECK(!sink->live);
}

static void ExerciseEmpty(ArRenderDevice *device, const ArTextBackend *backend) {
  test_case = "empty replacement";
  ArLocalizedTextPresenter_Reset(device);
  ArLocalizedTextPresenter_SetBackend(NULL);
  TextureSink *sink = device->context;
  const unsigned uploads = sink->uploads;
  ArLocalizationFrame frame;
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(&frame, "en", "test", s_test_font, 1,
                                     &frame.settings));
  const ArTextCellDestination destination = {3, kArTextCellScreen_Composited, 0};
  const ArTextCellRegion region = {5, 19, 24, 6};
  const ArTextCellRegion preserve = {5, 21, 24, 1};
  const HudPresentationChunk chunk = {
      .inspector_kind = kInspectorPresentation_HudBg,
      .screen_source = {0, 0, 256, 224}, .texture_source = {0, 0, 256, 224},
      .output_destination = {0, 0, 1024, 896},
  };
  ArLocalizedPreparedFrame prepared;
  /* Empty flow, tables, cursor menus and single-line labels never ask the
   * text backend for an artificial blank texture, even without a backend. */
  /* A grid with no game behind it: the renderer needs nothing but this
   * description to claim the cells. */
  const ArLocalizationTextGrid invented = {
      .rule_count = 1, .row_height = 2,
      .rules = {{.first_line = 0, .last_line = 5, .field_count = 1,
                 .cell_count = 1,
                 .cells = {{0, 24, kArTextHorizontalAlignment_Leading, true,
                            false, false}}}}};
  for (int layout = kArLocalizationTextLayout_Flow;
       layout <= kArLocalizationTextLayout_SingleLineLabel; ++layout) {
    frame.snapshot_count = frame.text_bytes = frame.cells.count = 0;
    frame.grid_count = 0;
    if (layout == kArLocalizationTextLayout_Grid) {
      CHECK(ArLocalizationFrame_AddTextWithGrid(
          &frame, 1, destination, region, "", 0, 0, 0, 1,
          kArTextDirection_LeftToRight, 8, &invented, NULL, &preserve, 1, NULL, 0));
    } else {
      CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(
          &frame, 1, destination, region, "", 0, 0, 0, 1,
          kArTextDirection_LeftToRight, 8, (ArLocalizationTextLayoutKind)layout,
          &preserve, 1, NULL, 0));
    }
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                    256, 224, &chunk, 1, &prepared);
    CHECK(prepared.mask_count == 2 && prepared.text_count == 0);
    int area = 0;
    for (size_t index = 0; index < prepared.mask_count; ++index)
      area += prepared.masks[index].w * prepared.masks[index].h;
    CHECK(area == 24 * 8 * 5 * 8); /* Preserve the native divider row. */
    CHECK(sink->uploads == uploads && sink->live == 0);
    CHECK(ArLocalizedTextPresenter_Draw(device, &prepared));
  }
  /* A blank page still exposes its authentic continuation indicator. */
  SetArt(&frame.artwork[kArLocalizationArtwork_Continue], 8);
  CHECK(ArLocalizationFrame_AddIndicator(
      &frame, 1, kArLocalizationIndicator_DialogueContinue,
      (ArTextCellRegion){17, 24, 1, 1}));
  for (int repeat = 0; repeat < 2; ++repeat) {
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                    256, 224, &chunk, 1, &prepared);
    CHECK(prepared.indicator_count == 1 && prepared.text_count == 0);
    CHECK(sink->uploads == uploads + 1 && sink->live == 1);
    CHECK(ArLocalizedTextPresenter_Draw(device, &prepared));
  }
  CHECK(ArLocalizationFrame_AddText(
      &frame, 2, destination, (ArTextCellRegion){5, 4, 8, 2}, "Visible", 7, 7, 7,
      1, kArTextDirection_LeftToRight, 8, NULL, 0));
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                  256, 224, &chunk, 1, &prepared);
  CHECK(prepared.mask_count == 2 && prepared.text_count == 0);
  ArLocalizedTextPresenter_SetBackend(backend);
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                  256, 224, &chunk, 1, &prepared);
  CHECK(prepared.mask_count == 3 && prepared.text_count == 1);
  CHECK(prepared.indicator_count == 1);
  CHECK(ArLocalizedTextPresenter_Draw(device, &prepared));
  ArLocalizedTextPresenter_Reset(device);
  CHECK(sink->live == 0);
}

static void ExerciseHudScaling(ArRenderDevice *device) {
  test_case = "hud scaling";
  const ArRenderRectI viewports[] = {
      {11, 13, 640, 480},   {11, 13, 1280, 720},  {11, 13, 1280, 800},
      {11, 13, 1920, 1080}, {11, 13, 3440, 1440},
  };
  const int percentages[] = {0, 25, 100, 225, 400};
  const int sizes[] = {80, 140};
  for (size_t v = 0; v < sizeof(viewports) / sizeof(viewports[0]); ++v)
    for (int density = 1; density <= 2; ++density)
      for (int par = 0; par <= 1; ++par)
        for (size_t h = 0; h < sizeof(percentages) / sizeof(percentages[0]);
             ++h)
          for (size_t f = 0; f < sizeof(sizes) / sizeof(sizes[0]); ++f) {
            test_size = sizes[f];
            test_scale = percentages[h] * density;
            test_treatment = par;
            test_example = (int)v;
            HudProjectionInputs inputs = {
                .hud_bg_texture = {1},
                .hud_obj_texture = {2},
                .hud_scale_percent = percentages[h] * density,
                .crt_pixel_aspect = par != 0,
                .snes_width = 512,
                .snes_height = 224,
                .visible_width = 352,
                .authentic_width = 256,
                .hud_split_height = 32,
                .hud_player_row_y = 32,
                .hud_left_only_y = 32,
                .hud_left_end = 168,
                .hud_right_start = 168,
                .hud_body_y1 = 224,
                .obj_icon_valid = true,
                .obj_icon_x = 148,
                .obj_icon_y = 11,
            };
            HudPresentationChunk chunks[kHudPresentationChunkCapacity];
            const int count = ArHudLayout_BuildPresentationChunks(
                viewports[v], &inputs, chunks);
            CHECK(count == 4);
            ArEnhancedTextSettings settings;
            ArEnhancedTextSettings_Defaults(&settings);
            settings.size_percent = sizes[f];
            ArLocalizationFrame frame;
            ArLocalizationFrame_Reset(&frame);
            CHECK(ArLocalizationFrame_SetFont(&frame, "fr", "test",
                                              s_test_font, 1, &settings));
            const char *label = "Région de l'Été";
            CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(
                &frame, 4,
                (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
                (ArTextCellRegion){6, 1, 12, 1}, label, strlen(label), 100, 100,
                1, kArTextDirection_LeftToRight, 7,
                kArLocalizationTextLayout_SingleLineLabel, NULL, 0, NULL, 0));
            ArLocalizedPreparedFrame prepared;
            ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0,
                                             0, 256, 224, chunks, count,
                                             &prepared);
            CHECK(prepared.text_count == 1 && prepared.mask_count == 1);
            if (prepared.text_count != 1 || prepared.mask_count != 1)
              continue;
            const ArRenderRectI dst = prepared.texts[0].destination;
            const ArRenderRectI left = chunks[0].output_destination;
            CHECK(abs(dst.x - (left.x + 48 * left.w / 168)) <= 1);
            CHECK(dst.x + dst.w <= left.x + (144 * left.w + 167) / 168);
            CHECK(dst.y >= left.y + 7 * left.h / 32);
            CHECK(dst.y + dst.h <= left.y + (15 * left.h + 31) / 32);
            CHECK(dst.x + dst.w <= chunks[3].output_destination.x);
            CHECK(prepared.masks[0].y == 7 && prepared.masks[0].h == 8);
            CHECK(inputs.hud_scale_percent == percentages[h] * density);
            const unsigned uploads = ((TextureSink *)device->context)->uploads;
            ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0,
                                             0, 256, 224, chunks, count,
                                             &prepared);
            CHECK(((TextureSink *)device->context)->uploads == uploads);
          }
  ArLocalizedTextPresenter_Reset(device);
}

static void ExerciseActionHudBands(ArRenderDevice *device) {
  test_case = "action hud bands";
  const int scales[] = {0, 25, 100, 400};
  for (unsigned rtl = 0; rtl < 2; ++rtl)
  for (unsigned wide = 0; wide < 2; ++wide) for (unsigned scale = 0; scale < 4; ++scale) {
    HudProjectionInputs inputs = {
      .hud_bg_texture = {1}, .hud_scale_percent = scales[scale],
      .snes_width = 512, .snes_height = 224, .visible_width = wide ? 352 : 256,
      .authentic_width = 256, .hud_split_height = 40, .hud_player_row_y = 20,
      .hud_left_only_y = 28, .hud_left_end = 88, .hud_right_start = 168, .hud_body_y1 = 224,
    };
    HudPresentationChunk chunks[kHudPresentationChunkCapacity];
    const int count = ArHudLayout_BuildPresentationChunks((ArRenderRectI){0, 0, 1280, 896}, &inputs, chunks);
    ArLocalizationFrame frame;
    ArLocalizationFrame_Reset(&frame);
    CHECK(ArLocalizationFrame_SetFont(&frame, "fr", "test", s_test_font, 1, &frame.settings));
    for (unsigned row = 2; row <= 3; ++row) {
      CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(&frame, row,
          (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
          (ArTextCellRegion){0, row, 6, 1}, "JOUEUR", 6, 6, 6, 1,
          rtl ? kArTextDirection_RightToLeft : kArTextDirection_LeftToRight,
          7, kArLocalizationTextLayout_RightAlignedLabel, NULL, 0, NULL, 0));
      frame.snapshots[frame.snapshot_count - 1].left_inset_pixels = 5;
      frame.snapshots[frame.snapshot_count - 1].right_inset_pixels = 4;
      frame.snapshots[frame.snapshot_count - 1].top_inset_pixels = 1;
    }
    ArLocalizedPreparedFrame prepared;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 252,
        256, 224, chunks, count, &prepared);
    CHECK(prepared.text_count == 2 && prepared.mask_count == 2);
    CHECK(prepared.masks[0].y == 19 && prepared.masks[1].y == 27);
    for (int i = 0; i < count; ++i) {
      if (chunks[i].screen_source.y != 20 || chunks[i].screen_source.x) continue;
      ArRenderRectI field = {0};
      CHECK(ArTextCellComposite_ProjectToOutput(&chunks[i],
          (ArRenderRectI){0, 20, 48, 1}, &field) || field.h == 0);
      const int edge = field.x + field.w - (4 * field.w + 24) / 48;
      for (unsigned label = 0; label < prepared.text_count; ++label)
        CHECK(prepared.texts[label].destination.x + prepared.texts[label].destination.w == edge);
    }
    const unsigned uploads = ((TextureSink *)device->context)->uploads;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 252,
        256, 224, chunks, count, &prepared);
    CHECK(((TextureSink *)device->context)->uploads == uploads);
    /* A disconnected/independently shifted band must not acquire text. */
    CHECK(ArLocalizedTextPresenter_DrawWithBrightness(device, &prepared, 0.5f));
    CHECK(((TextureSink *)device->context)->last_tint.r == 0.5f);
    CHECK(ArLocalizedTextPresenter_DrawWithBrightness(device, &prepared, 0));
    CHECK(((TextureSink *)device->context)->last_tint.r == 0);
    CHECK(ArLocalizedTextPresenter_Draw(device, &prepared));
    CHECK(((TextureSink *)device->context)->last_tint.r == 1);
    CHECK(((TextureSink *)device->context)->uploads == uploads);
    for (int i = 0; i < count; ++i)
      if (chunks[i].screen_source.y == 20) chunks[i].output_destination.x += 30;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 252,
        256, 224, chunks, count, &prepared);
    if (scales[scale] != 25) CHECK(!prepared.text_count && !prepared.mask_count);
  }
  ArLocalizedTextPresenter_Reset(device);
}

static void ExerciseActionHudGaps(ArRenderDevice *device) {
  test_case = "action hud gaps";
  const char *labels[][3] = {
    {"TIME", "SCORE", "PLAYER"}, {"TEMPS", "SCORE", "JOUEUR"},
    {"CHRONOMÈTRE", "PUNKTE", "SPIELER"},
  };
  for (unsigned language = 0; language < 3; ++language)
  for (unsigned rtl = 0; rtl < 2; ++rtl)
  for (int size = 80; size <= 140; size += 30)
  for (int scale = 1; scale <= 4; scale *= 2) {
    ArLocalizationFrame frame;
    ArLocalizationFrame_Reset(&frame);
    frame.settings.size_percent = size;
    CHECK(ArLocalizationFrame_SetFont(&frame, "fr", "test", s_test_font, 1, &frame.settings));
    const struct {
      ArTextCellRegion cells;
      const char *text;
      unsigned left, right;
      bool trailing;
    } fields[] = {
      {{11, 1, 4, 1}, labels[language][0], 0, 6, true},
      {{15, 1, 3, 1}, "292", 1, 1, false},
      {{8, 1, 2, 1}, "03", 0, 1, false},
      {{21, 1, 5, 1}, labels[language][1], 0, 0, false},
      {{26, 1, 5, 1}, language ? "99999" : "    0", 0, 1, true},
      {{0, 2, 6, 1}, labels[language][2], 5, 4, true},
    };
    for (unsigned i = 0; i < 6; ++i) {
      CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(&frame, i + 1,
          (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
          fields[i].cells, fields[i].text, strlen(fields[i].text), 100, 100, 1,
          rtl && (i == 0 || i == 3 || i == 5) ? kArTextDirection_RightToLeft :
          kArTextDirection_LeftToRight, 7, fields[i].trailing ?
          kArLocalizationTextLayout_RightAlignedLabel : kArLocalizationTextLayout_LeftAlignedLabel,
          NULL, 0, NULL, 0));
      ArLocalizationTextSnapshot *snapshot = &frame.snapshots[i];
      snapshot->left_inset_pixels = fields[i].left;
      snapshot->right_inset_pixels = fields[i].right;
      snapshot->top_inset_pixels = 1;
      snapshot->italic = i == 1 || i == 2 || i == 4;
      snapshot->native_font_pixels = snapshot->italic ? 8 : 7;
      snapshot->top_inset_pixels = snapshot->italic ? 0 : 1;
      snapshot->shadow_enabled = true;
    }
    const HudPresentationChunk chunk = {
      .inspector_kind = kInspectorPresentation_HudBg,
      .screen_source = {0, 0, 256, 224}, .texture_source = {0, 0, 256, 224},
      .output_destination = {0, 0, 256 * scale, 224 * scale},
    };
    ArLocalizedPreparedFrame prepared;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 252,
        256, 224, &chunk, 1, &prepared);
    CHECK(prepared.text_count == 6 && prepared.mask_count == 6);
    if (prepared.text_count != 6) continue;
    const ArRenderRectI time = prepared.texts[0].destination;
    const ArRenderRectI timer = prepared.texts[1].destination;
    CHECK(timer.x - time.x - time.w == 7 * scale);
    CHECK(prepared.texts[2].destination.x == 64 * scale); /* beside native x */
    const ArRenderRectI score_label = prepared.texts[3].destination;
    CHECK(score_label.x == 168 * scale); /* aligned with the native scrolls */
    const ArRenderRectI score = prepared.texts[4].destination;
    CHECK(score.x + score.w == 247 * scale); /* stable for one or five digits */
    const ArRenderRectI player = prepared.texts[5].destination;
    CHECK(48 * scale - player.x - player.w == 4 * scale);
    CHECK(prepared.masks[0].x == 88 && prepared.masks[0].w == 32);
    CHECK(prepared.masks[5].x == 0 && prepared.masks[5].w == 48);
    const unsigned uploads = ((TextureSink *)device->context)->uploads;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 252,
        256, 224, &chunk, 1, &prepared);
    CHECK(((TextureSink *)device->context)->uploads == uploads);
    /* Invalid gutters fail closed, without masking away the native label. */
    frame.snapshots[0].right_inset_pixels = 32;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 252,
        256, 224, &chunk, 1, &prepared);
    CHECK(prepared.text_count == 5 && prepared.mask_count == 5);
  }
  ArLocalizedTextPresenter_Reset(device);
}

static void ExerciseLabelFrame(ArRenderDevice *device) {
  test_case = "framed label";
  const char *labels[] = {"ACT", "Étape", ""};
  for (unsigned label = 0; label < 3; ++label)
  for (int size = 80; size <= 140; size += 60)
  for (unsigned rtl = 0; rtl < 2; ++rtl)
  for (int scale = 1; scale <= 4; scale *= 2) {
    ArLocalizationFrame frame;
    ArLocalizationFrame_Reset(&frame);
    frame.settings.size_percent = size;
    CHECK(ArLocalizationFrame_SetFont(&frame, "fr", "test", s_test_font, 1, &frame.settings));
    const size_t bytes = strlen(labels[label]);
    CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(&frame, 1,
        (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
        (ArTextCellRegion){0, 1, 6, 1}, labels[label], bytes, bytes, bytes, 1,
        rtl ? kArTextDirection_RightToLeft : kArTextDirection_LeftToRight,
        7, kArLocalizationTextLayout_FramedLabel, NULL, 0, NULL, 0));
    frame.snapshots[0].left_inset_pixels = 5;
    frame.snapshots[0].right_inset_pixels = 4;
    frame.snapshots[0].top_inset_pixels = 1;
    SetArt(&frame.artwork[kArLocalizationArtwork_LabelFrameLeft], 8);
    SetArt(&frame.artwork[kArLocalizationArtwork_LabelFrameRight], 7);
    const HudPresentationChunk chunk = {
      .inspector_kind = kInspectorPresentation_HudBg,
      .screen_source = {0, 0, 256, 224}, .texture_source = {0, 0, 256, 224},
      .output_destination = {0, 0, 256 * scale, 224 * scale},
    };
    ArLocalizedPreparedFrame prepared;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 252,
        256, 224, &chunk, 1, &prepared);
    CHECK(prepared.decoration_count == 2 && prepared.mask_count == 1);
    CHECK(prepared.text_count == (bytes ? 1 : 0));
    CHECK(prepared.masks[0].x == 0 && prepared.masks[0].w == 48);
    if (prepared.decoration_count != 2) continue;
    const ArRenderRectI left = prepared.decorations[0].destination;
    const ArRenderRectI right = prepared.decorations[1].destination;
    CHECK(left.w == 8 * scale && right.w == 7 * scale);
    CHECK(left.y == 11 * scale && right.y == 11 * scale);
    CHECK(left.h == 8 * scale && right.h == 8 * scale);
    CHECK(left.x >= 5 * scale && right.x + right.w <= 44 * scale);
    CHECK(abs((left.x - 5 * scale) - (44 * scale - right.x - right.w)) <= 1);
    if (bytes && prepared.text_count) {
      const ArRenderRectI text = prepared.texts[0].destination;
      CHECK(left.x + left.w == text.x && right.x == text.x + text.w);
    } else CHECK(left.x + left.w == right.x);
    CHECK(ArLocalizedTextPresenter_DrawWithBrightness(device, &prepared, 0.5f));
    CHECK(((TextureSink *)device->context)->last_tint.r == 0.5f);
    const unsigned uploads = ((TextureSink *)device->context)->uploads;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 252,
        256, 224, &chunk, 1, &prepared);
    CHECK(((TextureSink *)device->context)->uploads == uploads);
    /* Missing native artwork must preserve the complete original panel. */
    frame.artwork[kArLocalizationArtwork_LabelFrameRight].valid = false;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 252,
        256, 224, &chunk, 1, &prepared);
    CHECK(!prepared.text_count && !prepared.decoration_count && !prepared.mask_count);
  }
  ArLocalizedTextPresenter_Reset(device);
}

/* The keyboard's action keys are the game's own tiny letter pairs ("Bs",
 * "Ed"). When that art is captured the object must draw it; the shapes the
 * draw side falls back to are a last resort for a keyboard composed without
 * its VRAM, not a second rendering of the same key. */
static void ExerciseKeyboardActionKeys(ArRenderDevice *device) {
  test_case = "keyboard action keys";
  for (int captured = 0; captured <= 1; ++captured) {
    ArEnhancedTextSettings settings;
    ArEnhancedTextSettings_Defaults(&settings);
    ArLocalizationFrame frame;
    ArLocalizationFrame_Reset(&frame);
    CHECK(ArLocalizationFrame_SetFont(&frame, "en", "test", s_test_font,
                                      1, &settings));
    if (captured) SetArt(&frame.artwork[kArLocalizationArtwork_NameBackspace], 8);
    static const char kRow[] = "0 1 2 3 . x";
    const ArLocalizationInlineObjectSnapshot object = {
      kArLocalizationInlineObject_NameBackspace, (uint32_t)strlen(kRow),
    };
    CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(
        &frame, 1, (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
        (ArTextCellRegion){3, 7, 27, 16}, kRow, strlen(kRow),
        (uint32_t)strlen(kRow), (uint32_t)strlen(kRow), 1,
        kArTextDirection_LeftToRight, 8, kArLocalizationTextLayout_Flow,
        NULL, 0, &object, 1));
    const HudPresentationChunk chunk = {
      .inspector_kind = kInspectorPresentation_HudBg,
      .screen_source = {0, 0, 256, 224}, .texture_source = {0, 0, 256, 224},
      .output_destination = {0, 0, 1024, 896},
    };
    ArLocalizedPreparedFrame prepared;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                     256, 224, &chunk, 1, &prepared);
    CHECK(prepared.inline_object_count == 1);
    if (prepared.inline_object_count == 1) {
      /* Captured art becomes a texture; without it the object still occupies
       * its key so the fallback has somewhere to draw. */
      CHECK(ArRenderTexture_IsValid(prepared.inline_objects[0].texture) ==
            (captured != 0));
      CHECK(prepared.inline_objects[0].destination.w > 0);
      if (captured) {
        /* An 8x8 tile beside HD text is enlarged from its own shape, and
         * sampled smoothly -- drawing enlarged art sharply would put the
         * stair steps straight back. */
        TextureSink *sink = device->context;
        CHECK(sink->last_descriptor.width > 8 &&
              sink->last_descriptor.height > 8);
        CHECK(sink->last_descriptor.width % 8 == 0 &&
              sink->last_descriptor.width == sink->last_descriptor.height);
        CHECK(sink->last_descriptor.filter == kArRenderFilter_Linear);
      }
    }
    CHECK(ArLocalizedTextPresenter_Draw(device, &prepared));
  }
  ArLocalizedTextPresenter_Reset(device);
}

/* Partial reveal must cost draw calls per run, not per visible glyph, and the
 * pixels it submits must still be exactly the revealed clusters. */
static void ExerciseRevealBatching(ArRenderDevice *device) {
  test_case = "reveal batching";
  TextureSink *sink = device->context;
  ArEnhancedTextSettings settings;
  ArEnhancedTextSettings_Defaults(&settings);
  ArLocalizationFrame frame;
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(&frame, "en", "test", s_test_font, 1,
                                    &settings));
  static const char kLine[] =
      "The Master listens to every town that still remembers him.";
  const uint32_t clusters = (uint32_t)strlen(kLine);
  const uint32_t revealed = clusters / 2u;
  CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(
      &frame, 1, (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
      (ArTextCellRegion){2, 18, 28, 5}, kLine, strlen(kLine), revealed,
      clusters, 1, kArTextDirection_LeftToRight, 8,
      kArLocalizationTextLayout_Flow, NULL, 0, NULL, 0));
  const HudPresentationChunk chunk = {
    .inspector_kind = kInspectorPresentation_HudBg,
    .screen_source = {0, 0, 256, 224}, .texture_source = {0, 0, 256, 224},
    .output_destination = {0, 0, 256, 224},
  };
  ArLocalizedPreparedFrame prepared;
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0, 256,
                                   224, &chunk, 1, &prepared);
  CHECK(prepared.text_count == 1);
  if (prepared.text_count != 1) return;
  const ArLocalizedPreparedText *text = &prepared.texts[0];
  size_t visible = text->revealed_cluster_count;
  if (visible > text->surface.reveal_cluster_count)
    visible = text->surface.reveal_cluster_count;
  CHECK(visible > 8 && visible < text->surface.reveal_cluster_count);

  long expected_area = 0;
  CHECK(text->surface.reveal_piece_count > 0);
  for (size_t i = 0; i < text->surface.reveal_piece_count; ++i) {
    const ArTextRevealPiece *piece = &text->surface.reveal_pieces[i];
    if (piece->cluster_index < visible)
      expected_area += (long)piece->source.w * piece->source.h;
  }
  sink->draws = 0;
  sink->geometry_draws = 0;
  sink->geometry_area = 0;
  CHECK(ArLocalizedTextPresenter_Draw(device, &prepared));
  CHECK(sink->draws == 0 && sink->geometry_draws > 0);
  CHECK(sink->geometry_draws * 2u <= visible);
  CHECK(sink->geometry_area == expected_area);

  /* Keyboard-style shifts apply to the owner's effects too. Even with every
   * key visible, do not switch to an unshifted whole-bitmap draw. */
  for (size_t i = 0; i < text->surface.reveal_cluster_count; ++i)
    prepared.cluster_shifts[i] = (int)i * 3;
  prepared.cluster_shift_count = text->surface.reveal_cluster_count;
  prepared.texts[0].cluster_shift_offset = 0;
  prepared.texts[0].viewport = (ArRenderRectI){0};
  prepared.texts[0].revealed_cluster_count = (uint32_t)text->surface.reveal_cluster_count;
  sink->verify_text = &prepared.texts[0];
  sink->verify_shifts = prepared.cluster_shifts;
  sink->geometry_area = 0;
  expected_area = 0;
  for (size_t i = 0; i < text->surface.reveal_piece_count; ++i)
    expected_area += (long)text->surface.reveal_pieces[i].source.w * text->surface.reveal_pieces[i].source.h;
  CHECK(ArLocalizedTextPresenter_Draw(device, &prepared));
  CHECK(sink->draws == 0 && sink->geometry_area == expected_area);
  sink->verify_text = NULL;
  sink->verify_shifts = NULL;

  /* A completed reveal is still a single draw of the whole surface. */
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(&frame, "en", "test", s_test_font, 1,
                                    &settings));
  CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(
      &frame, 1, (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
      (ArTextCellRegion){2, 18, 28, 5}, kLine, strlen(kLine), clusters,
      clusters, 1, kArTextDirection_LeftToRight, 8,
      kArLocalizationTextLayout_Flow, NULL, 0, NULL, 0));
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0, 256,
                                   224, &chunk, 1, &prepared);
  sink->draws = 0;
  sink->geometry_draws = 0;
  CHECK(ArLocalizedTextPresenter_Draw(device, &prepared));
  CHECK(sink->draws == 1 && sink->geometry_draws == 0);
}

static void ExerciseValueBoundaries(ArRenderDevice *device) {
  test_case = "authored boundaries versus literal value pipes";
  const char text[] = "I|WWWWWWWWWWWWWWWW|M";
  const ArLocalizationTextGrid grid = {
      .rule_count = 1, .row_height = 2, .shared_column_count = 2,
      .rules = {{.first_line = 0, .last_line = 0, .field_count = 2,
          .cell_count = 2, .shared_columns = true,
          .cells = {{0, 13, kArTextHorizontalAlignment_Leading, true, 0, 0},
                    {13, 26, kArTextHorizontalAlignment_Trailing, false, 0, 0}}}}};
  const HudPresentationChunk chunk = {
      .inspector_kind = kInspectorPresentation_HudBg,
      .screen_source = {0, 0, 256, 224}, .texture_source = {0, 0, 256, 224},
      .output_destination = {0, 0, 1024, 896}};
  ArLocalizedTextPresenter_Reset(device);
  for (unsigned variant = 0; variant < 2; ++variant) {
    ArLocalizationFrame frame;
    ArLocalizationFrame_Reset(&frame);
    CHECK(ArLocalizationFrame_SetFont(&frame, "en", "test", s_test_font, 1, &frame.settings));
    uint8_t boundaries[AR_TEXT_BOUNDARY_BYTES(sizeof(text))] = {0};
    ArTextBoundary_Set(boundaries, variant ? sizeof(text) - 3 : 1, true);
    CHECK(ArLocalizationFrame_AddTextWithGrid(
        &frame, 7, (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
        (ArTextCellRegion){3, 6, 26, 2}, text, sizeof(text) - 1, 20, 20, 1,
        kArTextDirection_LeftToRight, 8, &grid, boundaries, NULL, 0, NULL, 0));
    ArLocalizedPreparedFrame warm, cold;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                    256, 224, &chunk, 1, &warm);
    CHECK(warm.text_count == 2 && warm.mask_count == 1);
    ArLocalizedTextPresenter_Reset(device);
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                    256, 224, &chunk, 1, &cold);
    CHECK(cold.text_count == 2 && cold.mask_count == 1);
    for (unsigned i = 0; i < warm.text_count && i < cold.text_count; ++i) {
      CHECK(!memcmp(&warm.texts[i].destination, &cold.texts[i].destination,
                    sizeof(ArRenderRectI)));
      CHECK(warm.texts[i].surface.ascent == cold.texts[i].surface.ascent);
    }
  }
  ArLocalizedTextPresenter_Reset(device);
}

static void ExerciseMaximumSharedColumns(ArRenderDevice *device) {
  test_case = "contract maximum shared columns";
  const char text[] = "a|b|c|d|e|f|g|h|i|j";
  const ArLocalizationTextGrid grid = {
      .rule_count = 1, .row_height = 2, .shared_column_count = 10,
      .shared_template_line = 0,
      .rules = {{.first_line = 0, .last_line = 0, .field_count = 10,
          .cell_count = 10, .shared_columns = true,
          .cells = {{0, 3}, {3, 6}, {6, 9}, {9, 12}, {12, 15},
                    {15, 18}, {18, 21}, {21, 24}, {24, 27}, {27, 32}}}}};
  ArLocalizationFrame frame;
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(
      &frame, "en", "test", s_test_font, 1, &frame.settings));
  CHECK(ArLocalizationFrame_AddTextWithGrid(
      &frame, 77,
      (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
      (ArTextCellRegion){0, 6, 32, 2}, text, sizeof(text) - 1,
      19, 19, 1, kArTextDirection_LeftToRight, 8, &grid, NULL,
      NULL, 0, NULL, 0));
  const HudPresentationChunk chunk = {
      .inspector_kind = kInspectorPresentation_HudBg,
      .screen_source = {0, 0, 256, 224},
      .texture_source = {0, 0, 256, 224},
      .output_destination = {0, 0, 1024, 896}};
  ArLocalizedPreparedFrame prepared;
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                   256, 224, &chunk, 1, &prepared);
  CHECK(prepared.text_count == 10 && prepared.mask_count == 1);
  ArLocalizedTextPresenter_Reset(device);
}

static void ExerciseCenteredPage(ArRenderDevice *device) {
  test_case = "centered page grid, accent and atomic fallback";
  const ArLocalizationTextGrid grid = {
      .rule_count=1,.row_height=4,.crop_rows=1,.center_rows=1,
      .rules={{.first_line=0,.last_line=25,.field_count=1,.cell_count=1,
               .cells={{0,32,kArTextHorizontalAlignment_Center,0,0,0}}}},
  };
  TextureSink *sink=device->context;
  for (int scale=1;scale<=6;scale+=1) for (int size=80;size<=140;size+=30)
  for (unsigned lines=1;lines<=6;++lines) for (int treatment=0;treatment<=2;++treatment) {
    test_size = size;
    test_scale = scale;
    test_treatment = treatment;
    test_example = (int)lines;
    ArLocalizationFrame frame;
    ArLocalizationFrame_Reset(&frame);
    frame.settings.size_percent=size;
    frame.settings.pixelation=treatment;
    frame.settings.pixelation_size=treatment ? 2 : 0;
    CHECK(ArLocalizationFrame_SetFont(&frame,"fr","test",s_test_font,1,&frame.settings));
    char text[1024]={0}; size_t bytes=0;
    const unsigned first=11-2*(lines-1);
    for (unsigned i=0;i<first;++i) text[bytes++]='\n';
    for (unsigned line=0;line<lines;++line) {
      if (line) { memcpy(text+bytes,"\n\n\n\n",4);bytes+=4; }
      bytes+=(size_t)snprintf(text+bytes,sizeof(text)-bytes,"%s %u",
          line ? "Une autre personne" : "- Équipe de traduction -",line);
    }
    CHECK(ArLocalizationFrame_AddTextWithGrid(&frame,300,
        (ArTextCellDestination){3,kArTextCellScreen_Composited,0},
        (ArTextCellRegion){0,1,32,26},text,bytes,100,100,1,kArTextDirection_LeftToRight,
        13,&grid,NULL,NULL,0,NULL,0));
    frame.snapshots[0].style_id=kArTextStyle_RetailPaletteBands;
    frame.snapshots[0].body_rgb=frame.snapshots[0].band_rgb=0xffffff;
    frame.snapshots[0].accent_end_utf8_byte=first+4;
    frame.snapshots[0].accent_rgb=0xff9400;
    const HudPresentationChunk chunk={
      .inspector_kind=kInspectorPresentation_HudBg,
      .screen_source={0,0,256,224},.texture_source={0,0,256,224},
      .output_destination={71,0,256*scale,224*scale},
    };
    ArLocalizedPreparedFrame prepared;
    ArLocalizedTextPresenter_Prepare(device,&frame,true,0,32,32,0,0,256,224,&chunk,1,&prepared);
    CHECK(prepared.text_count==lines && prepared.mask_count==1);
    for (unsigned i=0;i<prepared.text_count;++i) {
      const ArLocalizedPreparedText *text=&prepared.texts[i];
      const ArRenderRectI ink=text->surface.ink_bounds, dest=text->destination;
      const int center=((int)(first+i*4+3)*8-1)*scale;
      CHECK(abs((dest.y+ink.y)*2+ink.h-center*2)<=1);
      CHECK(abs(dest.x*2+dest.w-(71*2+256*scale))<=1);
      CHECK(dest.y+ink.y>=0 && dest.y+ink.y+ink.h<=224*scale);
    }
    const unsigned uploads=sink->uploads;
    for (unsigned warm=0;warm<5;++warm)
      ArLocalizedTextPresenter_Prepare(device,&frame,true,0,32,32,0,0,256,224,&chunk,1,&prepared);
    CHECK(sink->uploads==uploads);
    CHECK(ArLocalizedTextPresenter_DrawWithBrightness(device,&prepared,0.25f));
    CHECK(sink->last_tint.r==0.25f);
    // Any one row failure must leave the full native page, including rows
    // already prepared earlier in this frame. No partly erased credits.
    ArLocalizedTextPresenter_Reset(device);
    sink->fail_upload=true;
    ArLocalizedTextPresenter_Prepare(device,&frame,true,0,32,32,0,0,256,224,&chunk,1,&prepared);
    CHECK(prepared.text_count==0 && prepared.mask_count==0);
    sink->fail_upload=false;
    ArLocalizedTextPresenter_Reset(device);
    if (lines > 1) {
      // Preflight is warm now. Fail the second row after one successful upload.
      sink->fail_upload_at = sink->uploads + 2;
      ArLocalizedTextPresenter_Prepare(device,&frame,true,0,32,32,0,0,256,224,&chunk,1,&prepared);
      CHECK(sink->uploads >= sink->fail_upload_at);
      CHECK(prepared.text_count==0 && prepared.mask_count==0);
      sink->fail_upload_at = 0;
      ArLocalizedTextPresenter_Reset(device);
    }
  }
}

static void ExerciseSourceLanguage(ArRenderDevice *device) {
  test_case = "effective source language in flow, dialogue, labels and grids";
  const ArLocalizationTextLayoutKind layouts[] = {kArLocalizationTextLayout_Flow,
      kArLocalizationTextLayout_DialogueWindow, kArLocalizationTextLayout_SingleLineLabel,
      kArLocalizationTextLayout_Grid};
  const char text[] = "Native fallback 123";
  const ArTextCellDestination destination = {3, kArTextCellScreen_Composited, 0};
  const ArTextCellRegion region = {3, 6, 26, 6};
  const ArLocalizationTextGrid grid = {.rule_count = 1, .row_height = 2,
      .rules = {{.first_line = 0, .last_line = 0, .field_count = 1,
          .cell_count = 1, .cells = {{0, 26, kArTextHorizontalAlignment_Leading, 1}}}}};
  const HudPresentationChunk chunk = {.inspector_kind = kInspectorPresentation_HudBg,
      .screen_source = {0, 0, 256, 224}, .texture_source = {0, 0, 256, 224},
      .output_destination = {37, 19, 512, 448}};
  TextureSink *sink = device->context;
  for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); ++i) {
    ArLocalizationFrame frame;
    ArLocalizationFrame_Reset(&frame);
    CHECK(ArLocalizationFrame_SetFont(&frame, "ar", "test", s_test_font, 2, &frame.settings));
    if (layouts[i] == kArLocalizationTextLayout_Grid)
      CHECK(ArLocalizationFrame_AddTextWithGrid(&frame, 7, destination, region,
          text, sizeof(text)-1, sizeof(text)-1, sizeof(text)-1, 1,
          kArTextDirection_RightToLeft, 8, &grid, NULL, NULL, 0, NULL, 0));
    else
      CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(&frame, 7, destination, region,
          text, sizeof(text)-1, sizeof(text)-1, sizeof(text)-1, 1,
          kArTextDirection_RightToLeft, 8, layouts[i], NULL, 0, NULL, 0));
    ArLocalizationTextLanguage source = {.locale = "en-US", .direction = kArTextDirection_LeftToRight};
    CHECK(ArLocalizationFrame_SetTextLanguage(&frame, &source));
    const ArTextBidiSpans bidi = {.count = 2,
        .spans = {{7,15,kArTextDirection_Auto},{16,19,kArTextDirection_LeftToRight}}};
    CHECK(ArLocalizationFrame_SetTextBidiSpans(&frame,&bidi));
    ArLocalizedPreparedFrame prepared;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                    256, 224, &chunk, 1, &prepared);
    CHECK(prepared.text_count == 1 && prepared.mask_count == 1);
    if (prepared.text_count != 1) continue;
    CHECK(prepared.texts[0].surface.paragraph_direction == kArTextDirection_LeftToRight);
    const uintptr_t texture = prepared.texts[0].surface.texture.value;
    const unsigned uploads = sink->uploads;
    /* A frame default change cannot overwrite a copied source language or
     * invalidate its warm cache entry. Changing the source locale must. */
    strcpy(frame.locale, "ja");
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                    256, 224, &chunk, 1, &prepared);
    CHECK(prepared.text_count == 1 && sink->uploads == uploads);
    if (prepared.text_count == 1) CHECK(prepared.texts[0].surface.texture.value == texture);
    strcpy(source.locale, "de");
    CHECK(ArLocalizationFrame_SetTextLanguage(&frame, &source));
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                    256, 224, &chunk, 1, &prepared);
    CHECK(prepared.text_count == 1 && sink->uploads > uploads);
    if (prepared.text_count == 1) CHECK(prepared.texts[0].surface.texture.value != texture);
    ArLocalizedTextPresenter_Reset(device);
  }
}

static void ExerciseRtlDialogue(ArRenderDevice *device) {
  test_case = "RTL dialogue: stable leading edge and cached logical reveal";
  const ArFontResourceId arabic = ArHostFontResources_RegisterFile(
      &s_font_store, AR_TEST_ARABIC_FONT_PATH, NULL, 0);
  CHECK(arabic);
  if (!arabic) return;
  const char text[] = "مرحبا ABC 123\nمرحبا\nمرحبا ABC 123\nمرحبا";
  for (int scale = 2; scale <= 4; scale += 2)
  for (int size = 80; size <= 140; size += 60)
  for (int treatment = 0; treatment <= 2; ++treatment)
  for (int direction = kArTextDirection_Auto;
       direction <= kArTextDirection_RightToLeft; direction += 2) {
    ArLocalizationFrame frame;
    ArLocalizationFrame_Reset(&frame);
    frame.settings.size_percent = size;
    frame.settings.pixelation = treatment;
    frame.settings.pixelation_size = treatment ? 2 : 0;
    CHECK(ArLocalizationFrame_SetFont(&frame, "ar", "test", s_test_font, 2, &frame.settings));
    CHECK(ArLocalizationFrame_SetFallbackFonts(&frame, &arabic, 1));
    CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(&frame, 7,
        (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
        (ArTextCellRegion){3, 6, 26, 6}, text, sizeof(text) - 1, 100, 100, 1,
        (ArTextDirection)direction, 8, kArLocalizationTextLayout_DialogueWindow,
        NULL, 0, NULL, 0));
    const uint32_t value_start = (uint32_t)(strstr(text,"ABC 123")-text);
    const ArTextBidiSpans bidi = {.count = 1,
        .spans = {{value_start,value_start+7,kArTextDirection_Auto}}};
    CHECK(ArLocalizationFrame_SetTextBidiSpans(&frame,&bidi));
    const HudPresentationChunk chunk = {
        .inspector_kind = kInspectorPresentation_HudBg,
        .screen_source = {0, 0, 256, 224}, .texture_source = {0, 0, 256, 224},
        .output_destination = {37, 19, 256 * scale, 224 * scale}};
    ArLocalizedPreparedFrame prepared;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                    256, 224, &chunk, 1, &prepared);
    CHECK(prepared.text_count == 1 && prepared.mask_count == 1);
    if (prepared.text_count != 1) continue;
    const ArTextSurface *surface = &prepared.texts[0].surface;
    int right[16] = {0};
    CHECK(surface->paragraph_direction == kArTextDirection_RightToLeft);
    size_t previous_end = 0;
    for (size_t i = 0; i < surface->reveal_cluster_count; ++i) {
      const ArTextRevealCluster *c = &surface->reveal_clusters[i];
      CHECK(c->end_utf8_byte > previous_end);
      CHECK(c->line_index < 16);
      previous_end = c->end_utf8_byte;
      if (c->line_index < 16 && c->x + c->width > right[c->line_index])
        right[c->line_index] = c->x + c->width;
    }
    CHECK(previous_end == sizeof(text) - 1);
    for (unsigned i = 1; i < 4; ++i)
      CHECK(right[i] > 0 && abs(right[0] - right[i]) <= scale * 2);
    const int origin = prepared.texts[0].destination.x;
    const uintptr_t texture = surface->texture.value;
    const unsigned uploads = ((TextureSink *)device->context)->uploads;
    for (size_t bytes = 2; bytes < sizeof(text); bytes += 2) {
      if (bytes < sizeof(text) - 1 &&
          ((uint8_t)text[bytes] & 0xc0u) == 0x80u)
        continue;
      frame.snapshots[0].revealed_utf8_bytes = bytes;
      ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                      256, 224, &chunk, 1, &prepared);
      CHECK(prepared.text_count == 1);
      if (prepared.text_count != 1) break;
      CHECK(prepared.texts[0].destination.x == origin);
      CHECK(prepared.texts[0].surface.texture.value == texture);
      CHECK(((TextureSink *)device->context)->uploads == uploads);
    }
    /* A cropped auto-direction label follows its resolved direction; explicit
     * physical labels remain fixed beside the game's original selector. */
    frame.snapshots[0].layout = kArLocalizationTextLayout_SingleLineLabel;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                    256, 224, &chunk, 1, &prepared);
    CHECK(prepared.text_count == 1);
    if (prepared.text_count == 1)
      CHECK(prepared.texts[0].destination.x + prepared.texts[0].destination.w == 37 + 29 * 8 * scale);
    frame.snapshots[0].layout = kArLocalizationTextLayout_LeftAlignedLabel;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                    256, 224, &chunk, 1, &prepared);
    CHECK(prepared.text_count == 1);
    if (prepared.text_count == 1) CHECK(prepared.texts[0].destination.x == 37 + 3 * 8 * scale);
    ArLocalizedTextPresenter_Reset(device);
  }
}

static void ExerciseTemplateAppearance(ArRenderDevice *device) {
  test_case = "template appearances through frame publication";
  ArLocalizationFrame frame;
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(&frame, "en", "styled", s_test_font, 1,
                                    &frame.settings));
  const ArTextFontRole role = {.name = "hud", .primary = s_test_font};
  CHECK(ArLocalizationFrame_SetFontRoles(&frame, &role, 1));
  const char text[] = "Small 12\nLarge 34";
  CHECK(ArLocalizationFrame_AddDialogueWindow(
      &frame, 1, (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
      (ArTextCellRegion){2, 4, 28, 12}, text, strlen(text), 0, 17, 1,
      kArTextDirection_LeftToRight, 12));
  const ArTextRunAppearance plain = {.font_role = "body",
                                     .scale_basis = 10000,
                                     .band_rgb = 0xffffff,
                                     .body_rgb = 0xffffff};
  ArTextAppearanceSpan span = {.start = 9,
                               .end = 17,
                               .appearance = {.font_role = "hud",
                                              .scale_basis = 18000,
                                              .band_rgb = 0xff8800,
                                              .body_rgb = 0xffcc00,
                                              .italic = true}};
  CHECK(ArLocalizationFrame_SetTextAppearance(&frame, &plain, &span, 1));
  const HudPresentationChunk chunk = {.inspector_kind =
                                          kInspectorPresentation_HudBg,
                                      .screen_source = {0, 0, 256, 224},
                                      .texture_source = {0, 0, 256, 224},
                                      .output_destination = {0, 0, 768, 672}};
  ArLocalizedPreparedFrame prepared;
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0, 256,
                                   224, &chunk, 1, &prepared);
  CHECK(prepared.text_count == 1 && prepared.mask_count == 1);
  if (prepared.text_count == 1) {
    const ArTextSurface *surface = &prepared.texts[0].surface;
    CHECK(surface->line_count == 2);
    CHECK(surface->font_use_count > 0);
    bool larger_font = false;
    for (size_t i = 0; i < surface->font_use_count; ++i) {
      CHECK(surface->font_uses[i].resource == s_test_font &&
            !surface->font_uses[i].missing);
      larger_font |=
          surface->font_uses[i].start >= 9 &&
          surface->font_uses[i].font_pixels > surface->raster_font_pixels;
    }
    CHECK(larger_font);
    if (surface->line_count == 2)
      CHECK(surface->lines[1].height > surface->lines[0].height);
    const uintptr_t texture = surface->texture.value;
    const unsigned uploads = ((TextureSink *)device->context)->uploads;
    frame.snapshots[0].revealed_utf8_bytes = sizeof(text) - 1;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0, 256,
                                     224, &chunk, 1, &prepared);
    CHECK(prepared.text_count == 1 &&
          prepared.texts[0].surface.texture.value == texture);
    CHECK(((TextureSink *)device->context)->uploads == uploads);
    frame.appearance_spans[0].appearance.band_rgb = 0x00ff00;
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0, 256,
                                     224, &chunk, 1, &prepared);
    CHECK(prepared.text_count == 1 &&
          ((TextureSink *)device->context)->uploads > uploads);
  }
  /* An undeclared role cannot render through a body-font substitution. */
  strcpy(frame.appearance_spans[0].appearance.font_role, "missing");
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0, 256,
                                   224, &chunk, 1, &prepared);
  CHECK(!prepared.text_count && !prepared.mask_count);
  ArLocalizedTextPresenter_Reset(device);
}

int main(void) {
  s_test_font = ArHostFontResources_RegisterFile(&s_font_store, AR_TEST_FONT_PATH, NULL, 0);
  CHECK(s_test_font);
  if (!s_test_font) return 1;
  ArFontResources resources = ArHostFontResources_Provider(&s_font_store);
  ArLocalizedTextPresenter_SetFontResources(&resources);
  TextureSink sink = {0};
  ArRenderDevice device;
  CHECK(ArRenderDevice_Init(&device, &sink_ops, &sink,
      (ArRenderCapabilities){.maximum_texture_width = 4096, .maximum_texture_height = 4096}));
  ArTextBackend backend;
  ArSdlTextBackend_Init(&backend);
  ArLocalizedTextPresenter_SetBackend(&backend);
  ExerciseTemplateAppearance(&device);
  ExerciseSourceLanguage(&device);
  ExerciseFontRolePreflight(&device);
  ExercisePreflight(&device, &backend);
  ExerciseDialogueFailure(&device);
  ExerciseEmpty(&device, &backend);
  ExerciseHudScaling(&device);
  ExerciseActionHudBands(&device);
  ExerciseActionHudGaps(&device);
  ExerciseLabelFrame(&device);
  ExerciseRevealBatching(&device);
  ExerciseKeyboardActionKeys(&device);
  ExerciseValueBoundaries(&device);
  ExerciseMaximumSharedColumns(&device);
  ExerciseCenteredPage(&device);
  ExerciseRtlDialogue(&device);
  ExerciseLabelBreaks(&device);
  ExerciseScreenText(&device);
  const int sizes[] = {80, 110, 140};
  for (int scale = 2; scale <= 6; scale += 2) {
    for (size_t size = 0; size < 3; ++size) {
      for (int sampling = 0; sampling <= 1; ++sampling) {
        for (int treatment = 0; treatment <= 2; ++treatment) {
          ArEnhancedTextSettings settings;
          ArEnhancedTextSettings_Defaults(&settings);
          settings.size_percent = sizes[size];
          settings.sampling = (ArEnhancedTextSampling)sampling;
          settings.pixelation = (ArEnhancedTextPixelation)treatment;
          test_size = sizes[size]; test_scale = scale; test_treatment = treatment;
          for (int block = treatment ? 2 : 0; block <= (treatment ? 8 : 0); block += 2) {
            settings.pixelation_size = block;
            for (int example = 0; example < 6; ++example) {
              test_example = example;
              Exercise(&device, settings, scale, example);
            }
            test_example = 6;
            ExerciseSingleLine(&device, settings, scale);
            test_example = 7;
            ExerciseReport(&device, settings, scale, 0);
            test_example = 8;
            ExerciseReport(&device, settings, scale, 1);
          }
        }
      }
    }
  }
  ArEnhancedTextSettings pressure_settings;
  ArEnhancedTextSettings_Defaults(&pressure_settings);
  test_size = 140; test_scale = 4; test_treatment = 2; test_example = 9;
  const int ordinary_ascent = ExerciseReport(&device, pressure_settings, 4, 0);
  const int crowded_ascent = ExerciseReport(&device, pressure_settings, 4, 2);
  CHECK(crowded_ascent > 0 && crowded_ascent < ordinary_ascent);
  ExerciseReport(&device, pressure_settings, 4, 3);
  ArLocalizedTextPresenter_Reset(&device);
  CHECK(sink.live == 0);
  ArLocalizedTextPresenter_SetFontResources(NULL);
  CHECK(ArHostFontResources_Destroy(&s_font_store));
  return failures ? 1 : 0;
}
