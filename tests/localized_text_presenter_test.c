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

typedef struct TextureSink { uintptr_t next; unsigned uploads, live; } TextureSink;
static bool Create(void *context, const ArRenderTextureDesc *desc, ArRenderTexture *out) {
  TextureSink *sink = context;
  (void)desc;
  *out = (ArRenderTexture){++sink->next};
  ++sink->live;
  return true;
}
static void Destroy(void *context, ArRenderTexture texture) {
  TextureSink *sink = context;
  (void)texture;
  CHECK(sink->live > 0);
  --sink->live;
}
static bool Upload(void *context, ArRenderTexture texture, const ArRenderRectI *rect,
                   const void *pixels, int pitch) {
  TextureSink *sink = context;
  (void)texture; (void)rect;
  ++sink->uploads;
  return pixels && pitch > 0;
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
  (void)context; (void)texture; (void)src; (void)dst; (void)state; return true;
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

int main(void) {
  TextureSink sink = {0};
  ArRenderDevice device;
  CHECK(ArRenderDevice_Init(&device, &sink_ops, &sink,
      (ArRenderCapabilities){.maximum_texture_width = 4096, .maximum_texture_height = 4096}));
  ArTextBackend backend;
  ArSdlTextBackend_Init(&backend);
  ArLocalizedTextPresenter_SetBackend(&backend);
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
          }
        }
      }
    }
  }
  ArLocalizedTextPresenter_Reset(&device);
  CHECK(sink.live == 0);
  return failures ? 1 : 0;
}
