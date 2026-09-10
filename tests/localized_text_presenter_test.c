/* Real shaping, fitting, treatments and presenter; synthetic artwork and a
 * texture sink keep this test independent of ROMs, windows and GPU drivers. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/sdl/text_rasterizer_sdl.h"
#include "render/localized_text_presenter.h"

static int failures, test_size, test_scale, test_treatment, test_example;
#define CHECK(value) do { if (!(value)) { \
  fprintf(stderr, "%s:%d: %s (size=%d scale=%d treatment=%d example=%d)\n", \
      __FILE__, __LINE__, #value, test_size, test_scale, test_treatment, test_example); \
  ++failures; \
} } while (0)

typedef struct TextureSink {
  uintptr_t next;
  unsigned uploads, live;
  bool fail_create, fail_upload;
  bool alive[65536];
} TextureSink;
static bool Create(void *context, const ArRenderTextureDesc *desc, ArRenderTexture *out) {
  TextureSink *sink = context;
  (void)desc;
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
  return !sink->fail_upload && pixels && pitch > 0;
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
  const TextureSink *sink = context;
  CHECK(!texture.value || (texture.value < sizeof(sink->alive) && sink->alive[texture.value]));
  (void)src; (void)dst; (void)state; return true;
}
static bool Geometry(void *context, ArRenderTexture texture,
                     const ArRenderVertex2D *vertices, int count,
                     const int32_t *indices, int index_count,
                     const ArRenderDrawState *state) {
  (void)context; (void)texture; (void)vertices; (void)count;
  (void)indices; (void)index_count; (void)state; return true;
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
  const char *text = texts[example];
  const char *placeholder = strstr(text, example == 2 ? "\u2003" : "\u2007");
  const ArLocalizationInlineObjectKind kinds[] = {
    kArLocalizationInlineObject_SpeedDirection, kArLocalizationInlineObject_SpeedDirection,
    kArLocalizationInlineObject_StatusLife, kArLocalizationInlineObject_StatusPopulation,
    kArLocalizationInlineObject_NameCursor,
    kArLocalizationInlineObject_NameCursor,
  };
  const ArLocalizationTextLayoutKind layouts[] = {
    kArLocalizationTextLayout_MessageSpeed, kArLocalizationTextLayout_MessageSpeed,
    kArLocalizationTextLayout_StatusMaster, kArLocalizationTextLayout_StatusCities,
    kArLocalizationTextLayout_Flow,
    kArLocalizationTextLayout_Flow,
  };
  const ArTextCellRegion regions[] = {
    {18, 12, 10, 4}, {18, 12, 10, 4}, {10, 6, 12, 17},
    {3, 6, 26, 20}, {3, 7, 27, 16}, {3, 7, 27, 16},
  };
  ArLocalizationFrame frame;
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(&frame, "fr", "test", AR_TEST_FONT_PATH, 1, &settings));
  SetArt(&frame.artwork[kArLocalizationArtwork_SpeedDirection], 16);
  SetArt(&frame.artwork[kArLocalizationArtwork_Life], 8);
  SetArt(&frame.artwork[kArLocalizationArtwork_Population], 16);
  memcpy(frame.name_cursor_argb, frame.artwork[kArLocalizationArtwork_Life].argb,
         sizeof(frame.name_cursor_argb));
  frame.name_cursor_valid = true;
  const ArLocalizationInlineObjectSnapshot object = {
    kinds[example], (uint32_t)(placeholder - text + 3),
  };
  CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(
      &frame, 1, (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
      regions[example], text, strlen(text), 100, 100, 1,
      kArTextDirection_LeftToRight, example >= 3 ? 8 : 7, layouts[example],
      NULL, 0, &object, 1));
  const HudPresentationChunk chunk = {
    .inspector_kind = kInspectorPresentation_HudBg,
    .screen_source = {0, 0, 256, 224}, .texture_source = {0, 0, 256, 224},
    .output_destination = {0, 0, 256 * scale, 224 * scale},
  };
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
  const char *labels[] = {"ESTUARY", "Région de l'Été", "Upper\nValley"};
  for (size_t label = 0; label < sizeof(labels) / sizeof(labels[0]); ++label) {
    for (int rtl = 0; rtl <= 1; ++rtl) {
      ArLocalizationFrame frame;
      ArLocalizationFrame_Reset(&frame);
      CHECK(ArLocalizationFrame_SetFont(
          &frame, "fr", "test", AR_TEST_FONT_PATH, 1, &settings));
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

static int ExerciseReport(ArRenderDevice *device, ArEnhancedTextSettings settings,
                           int scale, int example) {
  const bool score = example == 1;
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
  CHECK(ArLocalizationFrame_SetFont(&frame, "fr", "test", AR_TEST_FONT_PATH, 1, &settings));
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
  CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(
      &frame, 7, (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
      (ArTextCellRegion){3, 6, 26, 20}, text, strlen(text), 100, 100, 1,
      kArTextDirection_LeftToRight, 8,
      score ? kArLocalizationTextLayout_StatusScore : kArLocalizationTextLayout_StatusCities,
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
  TextureSink *sink = device->context;
  char error[kArTextRasterErrorCapacity];
  ArTextPresentationFont font = {
      .struct_size = sizeof(font),
      .abi_version = AR_TEXT_PRESENTATION_ABI_VERSION,
      .stack_id = "test",
      .primary_path = AR_TEST_FONT_PATH,
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
  const char *bad_paths[] = {"no-such-localization-font.ttf", __FILE__};
  for (size_t i = 0; i < sizeof(bad_paths) / sizeof(bad_paths[0]); ++i) {
    candidate.primary_path = bad_paths[i];
    CHECK(!ArLocalizedTextPresenter_PrepareFont(device, &candidate, error,
                                                sizeof(error)));
    CHECK(error[0] && sink->live == 1 && sink->uploads == warm_uploads);
    CHECK(ArLocalizedTextPresenter_PrepareFont(device, &font, error,
                                               sizeof(error)));
    CHECK(sink->uploads ==
          warm_uploads); /* Failed selection retained the cache. */
  }
  candidate.primary_path = AR_TEST_FONT_PATH;
  const char *fallbacks[] = {"no-such-fallback.ttf"};
  candidate.fallback_paths = fallbacks;
  candidate.fallback_count = 1;
  CHECK(!ArLocalizedTextPresenter_PrepareFont(device, &candidate, error,
                                              sizeof(error)));
  CHECK(error[0] && sink->live == 1 && sink->uploads == warm_uploads);
  CHECK(ArLocalizedTextPresenter_PrepareFont(device, &font, error,
                                             sizeof(error)));
  fallbacks[0] = AR_TEST_FONT_PATH;
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
                                    candidate.primary_path, candidate.revision,
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
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0, 256,
                                   224, &chunk, 1, &prepared);
  CHECK(prepared.text_count == 1 && sink->uploads == staged_uploads);
  CHECK(ArLocalizedTextPresenter_Draw(device, &prepared));
  ArLocalizedTextPresenter_Reset(device);
  CHECK(sink->live == 0);
}

static void ExerciseDialogueFailure(ArRenderDevice *device) {
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
    CHECK(ArLocalizationFrame_SetFont(&frame, "en", "test", AR_TEST_FONT_PATH,
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
  ArLocalizedTextPresenter_Reset(device);
  ArLocalizedTextPresenter_SetBackend(NULL);
  TextureSink *sink = device->context;
  const unsigned uploads = sink->uploads;
  ArLocalizationFrame frame;
  ArLocalizationFrame_Reset(&frame);
  CHECK(ArLocalizationFrame_SetFont(&frame, "en", "test", AR_TEST_FONT_PATH, 1,
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
  for (int layout = kArLocalizationTextLayout_Flow;
       layout <= kArLocalizationTextLayout_SingleLineLabel; ++layout) {
    frame.snapshot_count = frame.text_bytes = frame.cells.count = 0;
    CHECK(ArLocalizationFrame_AddTextWithObjectsAndLayout(
        &frame, 1, destination, region, "", 0, 0, 0, 1,
        kArTextDirection_LeftToRight, 8, (ArLocalizationTextLayoutKind)layout,
        &preserve, 1, NULL, 0));
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
                                              AR_TEST_FONT_PATH, 1, &settings));
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

int main(void) {
  TextureSink sink = {0};
  ArRenderDevice device;
  CHECK(ArRenderDevice_Init(&device, &sink_ops, &sink,
      (ArRenderCapabilities){.maximum_texture_width = 4096, .maximum_texture_height = 4096}));
  ArTextBackend backend;
  ArSdlTextBackend_Init(&backend);
  ArLocalizedTextPresenter_SetBackend(&backend);
  ExercisePreflight(&device, &backend);
  ExerciseDialogueFailure(&device);
  ExerciseEmpty(&device, &backend);
  ExerciseHudScaling(&device);
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
  return failures ? 1 : 0;
}
