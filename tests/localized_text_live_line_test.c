/* A page with a live line (a name typed above its keyboard) is drawn as the
 * unchanged page plus the line alone, so typing rebuilds only the line. That
 * split is only acceptable if the screen cannot tell: this composites both
 * ways in software -- real shaping, fitting, effects and pixelation, a texture
 * store standing in for the GPU -- and requires identical pixels. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser/actraiser_localization_style.h"
#include "host/font_resources.h"
#include "platform/sdl/text_rasterizer_sdl.h"
#include "render/localized_text_presenter.h"

static int failures;
static const char *test_case = "";
#define CHECK(value) do { if (!(value)) { \
  fprintf(stderr, "%s:%d: %s (%s)\n", __FILE__, __LINE__, #value, test_case); \
  ++failures; \
} } while (0)

enum { kCanvasWidth = 1280, kCanvasHeight = 1120, kTextureCapacity = 4096 };

typedef struct StoredTexture {
  int width, height;
  bool rgba;
  uint32_t *pixels;
} StoredTexture;

/* Straight-alpha "over" into a float canvas, exactly as a blended quad drawn
 * 1:1 at integer positions composites. Only textured draws matter here; the
 * presenter's inline objects are compared as prepared data instead. */
typedef struct Compositor {
  StoredTexture textures[kTextureCapacity];
  /* Textures ever created; slots are reused once destroyed. */
  uintptr_t created;
  float *canvas;
  int left, top, right, bottom;
  unsigned unsupported;
} Compositor;

static bool Create(void *context, const ArRenderTextureDesc *desc,
                   ArRenderTexture *out) {
  Compositor *compositor = context;
  uintptr_t id = 1;
  while (id < kTextureCapacity && compositor->textures[id].pixels) ++id;
  if (id >= kTextureCapacity || desc->width <= 0 || desc->height <= 0)
    return false;
  ++compositor->created;
  compositor->textures[id] = (StoredTexture){
      desc->width, desc->height, desc->format == kArRenderPixelFormat_Rgba8888,
      calloc((size_t)desc->width * (size_t)desc->height, sizeof(uint32_t))};
  if (!compositor->textures[id].pixels) return false;
  *out = (ArRenderTexture){id};
  return true;
}

static void Destroy(void *context, ArRenderTexture texture) {
  Compositor *compositor = context;
  if (texture.value && texture.value < kTextureCapacity) {
    free(compositor->textures[texture.value].pixels);
    compositor->textures[texture.value] = (StoredTexture){0};
  }
}

static bool Upload(void *context, ArRenderTexture texture,
                   const ArRenderRectI *rect, const void *pixels, int pitch) {
  Compositor *compositor = context;
  if (!texture.value || texture.value >= kTextureCapacity || rect || !pixels)
    return false;
  StoredTexture *stored = &compositor->textures[texture.value];
  for (int y = 0; y < stored->height; ++y)
    memcpy(stored->pixels + (size_t)y * (size_t)stored->width,
           (const uint8_t *)pixels + (size_t)y * (size_t)pitch,
           (size_t)stored->width * sizeof(uint32_t));
  return true;
}

static void Composite(Compositor *compositor, ArRenderTexture texture,
                      int source_x, int source_y, int width, int height,
                      int target_x, int target_y) {
  const StoredTexture *stored = &compositor->textures[texture.value];
  if (!stored->pixels || !stored->rgba || source_x < 0 || source_y < 0 ||
      source_x + width > stored->width || source_y + height > stored->height ||
      target_x < 0 || target_y < 0 || target_x + width > kCanvasWidth ||
      target_y + height > kCanvasHeight) {
    ++compositor->unsupported;
    return;
  }
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      const uint32_t texel = stored->pixels[(size_t)(source_y + y) *
          (size_t)stored->width + (size_t)(source_x + x)];
      const float alpha = (float)(texel & 255u) / 255.0f;
      if (alpha <= 0.0f) continue;
      float *pixel = compositor->canvas +
          ((size_t)(target_y + y) * kCanvasWidth + (size_t)(target_x + x)) * 4u;
      for (unsigned c = 0; c < 3; ++c) {
        const float value = (float)((texel >> (24u - 8u * c)) & 255u) / 255.0f;
        pixel[c] = value * alpha + pixel[c] * (1.0f - alpha);
      }
      pixel[3] = alpha + pixel[3] * (1.0f - alpha);
    }
  if (target_x < compositor->left) compositor->left = target_x;
  if (target_y < compositor->top) compositor->top = target_y;
  if (target_x + width > compositor->right) compositor->right = target_x + width;
  if (target_y + height > compositor->bottom) compositor->bottom = target_y + height;
}

static bool Integral(float value, int *out) {
  *out = (int)lroundf(value);
  return fabsf(value - (float)*out) < 0.001f;
}

static bool DrawTexture(void *context, ArRenderTexture texture,
                        const ArRenderRectF *src, const ArRenderRectF *dst,
                        const ArRenderDrawState *state) {
  Compositor *compositor = context;
  int sx, sy, sw, sh, dx, dy, dw, dh;
  const bool tinted = state && (state->flags & kArRenderDrawState_Tint) &&
      (state->tint.r != 1.0f || state->tint.g != 1.0f ||
       state->tint.b != 1.0f || state->tint.a != 1.0f);
  if (!texture.value || texture.value >= kTextureCapacity || !src || !dst ||
      tinted || !Integral(src->x, &sx) || !Integral(src->y, &sy) ||
      !Integral(src->w, &sw) || !Integral(src->h, &sh) ||
      !Integral(dst->x, &dx) || !Integral(dst->y, &dy) ||
      !Integral(dst->w, &dw) || !Integral(dst->h, &dh) || sw != dw ||
      sh != dh) {
    /* Object artwork is enlarged; only 1:1 text draws are composited. */
    if (texture.value && src && dst && sw == dw && sh == dh) ++compositor->unsupported;
    return true;
  }
  Composite(compositor, texture, sx, sy, sw, sh, dx, dy);
  return true;
}

static bool Geometry(void *context, ArRenderTexture texture,
                     const ArRenderVertex2D *vertices, int count,
                     const int32_t *indices, int index_count,
                     const ArRenderDrawState *state) {
  Compositor *compositor = context;
  (void)indices;
  (void)index_count;
  (void)state;
  if (!texture.value) return true;
  const StoredTexture *stored = &compositor->textures[texture.value];
  for (int i = 0; i + 3 < count; i += 4) {
    int sx, sy, sr, sb, dx, dy, dr, db;
    if (!Integral(vertices[i].tex_coord.x * (float)stored->width, &sx) ||
        !Integral(vertices[i].tex_coord.y * (float)stored->height, &sy) ||
        !Integral(vertices[i + 2].tex_coord.x * (float)stored->width, &sr) ||
        !Integral(vertices[i + 2].tex_coord.y * (float)stored->height, &sb) ||
        !Integral(vertices[i].position.x, &dx) ||
        !Integral(vertices[i].position.y, &dy) ||
        !Integral(vertices[i + 2].position.x, &dr) ||
        !Integral(vertices[i + 2].position.y, &db) ||
        sr - sx != dr - dx || sb - sy != db - dy) {
      ++compositor->unsupported;
      continue;
    }
    Composite(compositor, texture, sx, sy, sr - sx, sb - sy, dx, dy);
  }
  return true;
}

static bool Target(void *context, ArRenderTexture texture) {
  (void)context; (void)texture; return true;
}
static bool NoArgs(void *context) { (void)context; return true; }
static bool Size(void *context, int *w, int *h) {
  (void)context; *w = kCanvasWidth; *h = kCanvasHeight; return true;
}
static bool Rect(void *context, const ArRenderRectI *rect) {
  (void)context; (void)rect; return true;
}
static bool Clear(void *context, ArRenderColorF color) {
  (void)context; (void)color; return true;
}
static const char *Error(void *context) { (void)context; return "compositor"; }
static const ArRenderBackendOps kCompositorOps = {
  .struct_size = sizeof(kCompositorOps), .create_texture = Create,
  .destroy_texture = Destroy, .update_texture = Upload,
  .set_render_target = Target, .use_output_coordinates = NoArgs,
  .get_output_size = Size, .set_viewport = Rect, .set_clip_rect = Rect,
  .clear = Clear, .draw_texture = DrawTexture, .draw_geometry = Geometry,
  .present = NoArgs, .last_error = Error,
};

static ArHostFontResources s_font_store;
static ArFontResourceId s_font;

enum { kPageCapacity = 1024 };

/* The page the runtime composes for the USA keyboard: prompt, the name padded
 * to eight graphemes with figure spaces, the emptied underline row, and five
 * rows of keys with four-space gutters and two artwork keys. */
typedef struct KeyboardPage {
  char utf8[kPageCapacity];
  size_t bytes;
  size_t name_start, name_end;
  ArLocalizationInlineObjectSnapshot objects[kArLocalizationFrameInlineObjectCapacity];
  uint8_t object_count;
} KeyboardPage;

static void Append(KeyboardPage *page, const char *text) {
  const size_t length = strlen(text);
  if (page->bytes + length < sizeof(page->utf8)) {
    memcpy(page->utf8 + page->bytes, text, length);
    page->bytes += length;
    page->utf8[page->bytes] = 0;
  }
}

static void AddObject(KeyboardPage *page, ArLocalizationInlineObjectKind kind) {
  page->objects[page->object_count++] =
      (ArLocalizationInlineObjectSnapshot){kind, (uint32_t)page->bytes};
}

/* `name` is a list of graphemes separated by '|'. */
static void BuildPage(KeyboardPage *page, const char *name, unsigned selected) {
  *page = (KeyboardPage){0};
  Append(page, "Please enter Master's name.\n");
  page->name_start = page->bytes;
  unsigned graphemes = 0;
  for (const char *cursor = name; *cursor;) {
    const char *end = strchr(cursor, '|');
    const size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
    char grapheme[16] = {0};
    memcpy(grapheme, cursor, length);
    Append(page, grapheme);
    AddObject(page, kArLocalizationInlineObject_NameFieldUnderline);
    ++graphemes;
    cursor += length + (end ? 1u : 0u);
  }
  for (; graphemes < 8; ++graphemes) {
    Append(page, "\xE2\x80\x87");
    AddObject(page, kArLocalizationInlineObject_NameFieldUnderline);
  }
  page->name_end = page->bytes;
  Append(page, "\n\n");
  static const char *const rows[5][13] = {
    {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M"},
    {"N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"},
    {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m"},
    {"n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"},
    {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "\xEF\xBF\xBC",
     "\xEF\xBF\xBC"},
  };
  for (unsigned row = 0; row < 5; ++row) {
    if (row) Append(page, "\n");
    for (unsigned column = 0; column < 13; ++column) {
      if (column) Append(page, "    ");
      Append(page, rows[row][column]);
      if (row == 4 && column == 11)
        AddObject(page, kArLocalizationInlineObject_NameBackspace);
      if (row == 4 && column == 12)
        AddObject(page, kArLocalizationInlineObject_NameFinish);
      if (row * 13 + column == selected)
        AddObject(page, kArLocalizationInlineObject_NameCursor);
    }
  }
}

/* `live` marks the name row as an ordinary live line; `cells` additionally
 * makes it an entry field of that many cells. */
static bool BuildFrame(ArLocalizationFrame *frame, const KeyboardPage *page,
                       const ArEnhancedTextSettings *settings, bool live,
                       uint8_t cells, ArTextDirection direction) {
  ArLocalizationFrame_Reset(frame);
  if (!ArLocalizationFrame_SetFont(frame, "en", "test", s_font, 1, settings))
    return false;
  /* The selector's native tile, which the game adapter would capture. */
  frame->name_cursor_valid = true;
  for (int y = 2; y < 6; ++y)
    for (int x = 1; x <= y; ++x)
      frame->name_cursor_argb[y * kArLocalizationFrameNameCursorExtent + x] =
          UINT32_C(0xffffffff);
  uint32_t clusters = 0;
  for (size_t i = 0; i < page->bytes; ++i)
    clusters += ((uint8_t)page->utf8[i] & 0xc0u) != 0x80u;
  if (!ArLocalizationFrame_AddTextWithObjectsAndLayout(
          frame, 5, (ArTextCellDestination){3, kArTextCellScreen_Composited, 0},
          (ArTextCellRegion){3, 7, 27, 16}, page->utf8, page->bytes, clusters,
          clusters, 7, direction, 8, kArLocalizationTextLayout_Flow, NULL, 0,
          page->objects, page->object_count))
    return false;
  ArTextBidiSpans spans = {.count = 1};
  spans.spans[0] = (ArTextBidiSpan){(uint32_t)page->name_start,
                                    (uint32_t)page->name_end,
                                    kArTextDirection_Auto};
  const uint16_t palette[4] = {0, 0, 0x7f33, 0x7fff};
  ActRaiserLocalizationStyle_Ordinary(&frame->snapshots[0], palette);
  return ArLocalizationFrame_SetTextBidiSpans(frame, &spans) &&
      ArLocalizationFrame_SetKeySeparator(frame, " ", 1) &&
      ArLocalizationFrame_SetKeyGrid(frame, 13, 5, 2) &&
      (!live || ArLocalizationFrame_SetLiveLine(
                    frame, page->name_start, page->name_end - page->name_start,
                    cells));
}

static void ClearRows(float *canvas, int left, int top, int right,
                      int bottom) {
  if (right <= left) return;
  for (int y = top; y < bottom; ++y)
    memset(canvas + ((size_t)y * kCanvasWidth + (size_t)left) * 4u, 0,
           (size_t)(right - left) * 4u * sizeof(float));
}

/* Draws `prepared` onto a clear `canvas` and reports what it covered. */
static void DrawOnto(ArRenderDevice *device, Compositor *compositor,
                     float *canvas, const ArLocalizedPreparedFrame *prepared,
                     int bounds[4]) {
  compositor->canvas = canvas;
  compositor->left = compositor->top = INT32_MAX;
  compositor->right = compositor->bottom = INT32_MIN;
  compositor->unsupported = 0;
  CHECK(ArLocalizedTextPresenter_Draw(device, prepared));
  CHECK(compositor->unsupported == 0);
  bounds[0] = compositor->left;
  bounds[1] = compositor->top;
  bounds[2] = compositor->right;
  bounds[3] = compositor->bottom;
}

static bool SameRows(const float *a, const float *b, const int bounds[4],
                     unsigned *differing_rows) {
  unsigned rows = 0;
  if (bounds[2] > bounds[0])
    for (int y = bounds[1]; y < bounds[3]; ++y)
      rows += memcmp(a + ((size_t)y * kCanvasWidth + (size_t)bounds[0]) * 4u,
                     b + ((size_t)y * kCanvasWidth + (size_t)bounds[0]) * 4u,
                     (size_t)(bounds[2] - bounds[0]) * 4u * sizeof(float)) != 0;
  if (differing_rows) *differing_rows = rows;
  return rows == 0;
}

static void Union(int into[4], const int other[4]) {
  if (other[2] <= other[0]) return;
  if (into[2] <= into[0]) {
    memcpy(into, other, 4 * sizeof(int));
    return;
  }
  if (other[0] < into[0]) into[0] = other[0];
  if (other[1] < into[1]) into[1] = other[1];
  if (other[2] > into[2]) into[2] = other[2];
  if (other[3] > into[3]) into[3] = other[3];
}

static bool SameObjects(const ArLocalizedPreparedFrame *a,
                        const ArLocalizedPreparedFrame *b) {
  if (a->inline_object_count != b->inline_object_count) return false;
  for (uint8_t i = 0; i < a->inline_object_count; ++i) {
    const ArLocalizedPreparedInlineObject *x = &a->inline_objects[i];
    const ArLocalizedPreparedInlineObject *y = &b->inline_objects[i];
    if (x->kind != y->kind || memcmp(&x->destination, &y->destination,
                                     sizeof(x->destination)) ||
        ArRenderTexture_IsValid(x->texture) != ArRenderTexture_IsValid(y->texture))
      return false;
  }
  return a->mask_count == b->mask_count &&
      !memcmp(a->masks, b->masks, a->mask_count * sizeof(a->masks[0]));
}

typedef struct Totals {
  unsigned compared, split, whole, sensitive;
} Totals;

static float *s_whole_canvas, *s_split_canvas;

static void Compare(ArRenderDevice *device, Compositor *compositor,
                    const ArEnhancedTextSettings *settings, int scale,
                    const char *name, unsigned selected,
                    ArTextDirection direction, bool expect_split,
                    Totals *totals) {
  static KeyboardPage page;
  static ArLocalizationFrame frame;
  static ArLocalizedPreparedFrame whole, split;
  BuildPage(&page, name, selected);
  const HudPresentationChunk chunk = {
    .inspector_kind = kInspectorPresentation_HudBg,
    .screen_source = {0, 0, 256, 224}, .texture_source = {0, 0, 256, 224},
    .output_destination = {7, 5, 256 * scale, 224 * scale},
  };
  int covered[4] = {0}, drawn[4];
  /* Each prepared frame is drawn before the next is prepared: preparing
   * releases the previous frame's hold on its cached surfaces. */
  CHECK(BuildFrame(&frame, &page, settings, false, 0, direction));
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                   256, 224, &chunk, 1, &whole);
  CHECK(whole.text_count == 1);
  DrawOnto(device, compositor, s_whole_canvas, &whole, drawn);
  Union(covered, drawn);
  CHECK(BuildFrame(&frame, &page, settings, true, 0, direction));
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                   256, 224, &chunk, 1, &split);
  CHECK(split.text_count == (expect_split ? 2 : 1));
  totals->split += split.text_count == 2;
  totals->whole += split.text_count == 1;
  CHECK(SameObjects(&whole, &split));
  DrawOnto(device, compositor, s_split_canvas, &split, drawn);
  Union(covered, drawn);
  unsigned rows = 0;
  if (!SameRows(s_whole_canvas, s_split_canvas, covered, &rows))
    fprintf(stderr, "  name=\"%s\" scale=%d size=%d pixelation=%d/%d: "
                    "%u rows differ\n", name, scale, settings->size_percent,
            (int)settings->pixelation, settings->pixelation_size, rows);
  CHECK(rows == 0);
  ++totals->compared;

  /* Positive control: the comparison sees the line slip by one pixel. */
  if (split.text_count == 2 && totals->sensitive < 8) {
    ArLocalizedPreparedFrame *moved = malloc(sizeof(*moved));
    if (moved) {
      *moved = split;
      moved->texts[1].destination.x += 1;
      ClearRows(s_split_canvas, covered[0], covered[1], covered[2], covered[3]);
      DrawOnto(device, compositor, s_split_canvas, moved, drawn);
      Union(covered, drawn);
      totals->sensitive += !SameRows(s_whole_canvas, s_split_canvas, covered, NULL);
      free(moved);
    }
  }
  ClearRows(s_whole_canvas, covered[0], covered[1], covered[2], covered[3]);
  ClearRows(s_split_canvas, covered[0], covered[1], covered[2], covered[3]);
}

/* The ink a surface's shaped letters cover, without their shading. */
static void LetterInk(const ArTextSurface *surface, int *left, int *right) {
  *left = surface->reveal_clusters[0].x;
  *right = *left + surface->reveal_clusters[0].width;
  for (size_t i = 1; i < surface->reveal_cluster_count; ++i) {
    const ArTextRevealCluster *cluster = &surface->reveal_clusters[i];
    if (cluster->x < *left) *left = cluster->x;
    if (cluster->x + cluster->width > *right) *right = cluster->x + cluster->width;
  }
}

static unsigned CountGraphemes(const char *name) {
  if (!*name) return 0;
  unsigned count = 1;
  for (const char *cursor = name; *cursor; ++cursor) count += *cursor == '|';
  return count;
}

typedef struct FieldTotals {
  unsigned fields, cells;
} FieldTotals;

/* An entry field puts each letter in its own fixed cell: shaped alone so it
 * never joins, centred in the cell, on the page's pixel grid, sharing one
 * baseline, and reused from the cache when the letter is typed again. */
static void CheckField(ArRenderDevice *device, Compositor *compositor,
                       const ArEnhancedTextSettings *settings, int scale,
                       FieldTotals *totals) {
  static KeyboardPage page;
  static ArLocalizationFrame frame;
  static ArLocalizedPreparedFrame prepared, whole;
  const HudPresentationChunk chunk = {
    .inspector_kind = kInspectorPresentation_HudBg,
    .screen_source = {0, 0, 256, 224}, .texture_source = {0, 0, 256, 224},
    .output_destination = {7, 5, 256 * scale, 224 * scale},
  };
  /* "fi" and "fl" form ligatures when shaped together; "AV" and "To" kern. */
  static const char *const names[] = {
    "", "f|i|f|l", "A|V|T|o", "W|M|W|M|W|M|W|M", "i|l|i|l|i|l|i|l",
    "g|y|p|q|1|2|3|4", "\xC3\x89|l|i|s|e", "B|A|A|B", "A", "W|A|W|A",
  };
  ArRenderTexture page_texture = ArRenderTexture_Invalid();
  ArRenderRectI page_destination = {0};
  ArRenderRectI first_a = {0};
  uintptr_t first_a_texture = 0;
  int underline_y = 0;
  for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); ++n) {
    BuildPage(&page, names[n], 7);
    if (n == 0) {
      /* Where the whole page puts the field's underlines. */
      CHECK(BuildFrame(&frame, &page, settings, false, 0,
                       kArTextDirection_LeftToRight));
      ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                       256, 224, &chunk, 1, &whole);
      CHECK(whole.text_count == 1);
      for (uint8_t i = 0; i < whole.inline_object_count; ++i)
        if (whole.inline_objects[i].kind ==
            kArLocalizationInlineObject_NameFieldUnderline)
          underline_y = whole.inline_objects[i].destination.y;
    }
    const uintptr_t textures_before = compositor->created;
    CHECK(BuildFrame(&frame, &page, settings, true, 8,
                     kArTextDirection_LeftToRight));
    ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                     256, 224, &chunk, 1, &prepared);
    const unsigned inked = CountGraphemes(names[n]);
    CHECK(prepared.text_count == 1u + inked);
    if (prepared.text_count != 1u + inked) continue;
    ++totals->fields;
    /* The page around the field is one cached surface, whatever is typed. */
    if (n == 0) {
      page_texture = prepared.texts[0].surface.texture;
      page_destination = prepared.texts[0].destination;
    }
    CHECK(prepared.texts[0].surface.texture.value == page_texture.value);
    CHECK(!memcmp(&prepared.texts[0].destination, &page_destination,
                  sizeof(page_destination)));

    int underline_x[8];
    int underline_w = -1;
    unsigned underlines = 0;
    for (uint8_t i = 0; i < prepared.inline_object_count; ++i) {
      const ArLocalizedPreparedInlineObject *object = &prepared.inline_objects[i];
      if (object->kind != kArLocalizationInlineObject_NameFieldUnderline) continue;
      if (underlines < 8) underline_x[underlines] = object->destination.x;
      CHECK(underline_w < 0 || object->destination.w == underline_w);
      underline_w = object->destination.w;
      CHECK(object->destination.y == underline_y);
      ++underlines;
    }
    CHECK(underlines == 8);
    if (underlines != 8) continue;
    const int pitch = underline_x[1] - underline_x[0];
    CHECK(pitch > 0);
    for (unsigned i = 1; i < 8; ++i)
      CHECK(underline_x[i] - underline_x[i - 1] == pitch);
    const int field_x = underline_x[0] - pitch / 10;
    const int quantum = prepared.texts[0].surface.mosaic_block > 1
        ? prepared.texts[0].surface.mosaic_block
        : prepared.texts[0].surface.raster_scale;
    CHECK(pitch % quantum == 0);
    CHECK(pitch >= prepared.texts[0].surface.raster_font_pixels *
                       prepared.texts[0].surface.raster_scale);

    int line_top = 0;
    int previous_ink_right = INT32_MIN;
    for (unsigned cell = 0; cell < inked; ++cell) {
      const ArLocalizedPreparedText *text = &prepared.texts[1 + cell];
      totals->cells++;
      /* Shaped alone: one letter, never a ligature of two. */
      CHECK(text->surface.reveal_cluster_count == 1);
      int left, right;
      LetterInk(&text->surface, &left, &right);
      const int centre2 = 2 * text->destination.x + left + right;
      const int cell_centre2 = 2 * (field_x + (int)cell * pitch) + pitch;
      CHECK(abs(centre2 - cell_centre2) <= quantum + 2);
      CHECK((text->destination.x - page_destination.x) % quantum == 0);
      CHECK((text->destination.y - page_destination.y) % quantum == 0);
      const int top = text->destination.y + text->surface.reveal_clusters[0].y;
      if (!cell) line_top = top;
      CHECK(abs(top - line_top) < quantum);
      /* Neighbours never overlap, shading included. */
      const ArRenderRectI ink = text->surface.ink_bounds;
      CHECK(text->destination.x + ink.x >= previous_ink_right);
      previous_ink_right = text->destination.x + ink.x + ink.w;
      if (!strncmp(names[n], "A", 1) && cell == 0) {
        if (!first_a_texture) {
          first_a = text->destination;
          first_a_texture = text->surface.texture.value;
        }
        /* The same letter in the same cell does not move or re-rasterize
         * whatever follows it. */
        CHECK(!memcmp(&text->destination, &first_a, sizeof(first_a)));
        CHECK(text->surface.texture.value == first_a_texture);
      }
    }
    /* Every letter of the last name was typed before: nothing new to raster. */
    if (n == sizeof(names) / sizeof(names[0]) - 1)
      CHECK(compositor->created == textures_before);
    int drawn[4];
    DrawOnto(device, compositor, s_whole_canvas, &prepared, drawn);
    ClearRows(s_whole_canvas, drawn[0], drawn[1], drawn[2], drawn[3]);
  }

  /* A right-to-left page has no leading edge the field could start from. */
  BuildPage(&page, "A|b", 3);
  CHECK(BuildFrame(&frame, &page, settings, true, 8,
                   kArTextDirection_RightToLeft));
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                   256, 224, &chunk, 1, &prepared);
  CHECK(prepared.text_count == 1);
  /* A line whose graphemes do not fill the stated cells is drawn as text. */
  BuildPage(&page, "A|b", 3);
  CHECK(BuildFrame(&frame, &page, settings, true, 7,
                   kArTextDirection_LeftToRight));
  ArLocalizedTextPresenter_Prepare(device, &frame, true, 0, 32, 32, 0, 0,
                                   256, 224, &chunk, 1, &prepared);
  CHECK(prepared.text_count == 1);
}

int main(void) {
  s_font = ArHostFontResources_RegisterFile(&s_font_store, AR_TEST_FONT_PATH,
                                            NULL, 0);
  CHECK(s_font);
  if (!s_font) return 1;
  ArFontResources resources = ArHostFontResources_Provider(&s_font_store);
  ArLocalizedTextPresenter_SetFontResources(&resources);
  static Compositor compositor;
  s_whole_canvas = calloc((size_t)kCanvasWidth * kCanvasHeight * 4u, sizeof(float));
  s_split_canvas = calloc((size_t)kCanvasWidth * kCanvasHeight * 4u, sizeof(float));
  CHECK(s_whole_canvas && s_split_canvas);
  if (!s_whole_canvas || !s_split_canvas) return 1;
  ArRenderDevice device;
  CHECK(ArRenderDevice_Init(&device, &kCompositorOps, &compositor,
      (ArRenderCapabilities){.maximum_texture_width = 4096,
                             .maximum_texture_height = 4096}));
  ArTextBackend backend;
  ArSdlTextBackend_Init(&backend);
  ArLocalizedTextPresenter_SetBackend(&backend);

  /* Descenders, capitals, digits (slanted), wide and narrow letters, accents
   * above the cap height and an empty field. */
  static const char *const names[] = {
    "", "A", "J|j", "g|y|p|q|j|Q|g|y", "W|W|W|W|W|W|W|W", "i|l|i|l|i|l|i|l",
    "1|2|3|4|5|6|7|8", "\xC3\x89|l|i|s|e", "\xC3\x85|\xC3\x84|\xC3\x96",
    "M|a|s|t|e|r", "K|y|o|t|o", "T|y|r|0|.|x", "\xC3\xB1|\xC3\xA7|\xC3\xBF",
  };
  const size_t name_count = sizeof(names) / sizeof(names[0]);
  Totals totals = {0};
  const int sizes[] = {80, 110, 140};
  for (int scale = 2; scale <= 4; ++scale) {
    for (size_t size = 0; size < 3; ++size) {
      for (int treatment = 0; treatment <= 2; ++treatment) {
        for (int block = treatment ? 2 : 0; block <= (treatment ? 8 : 0);
             block += 2) {
          ArEnhancedTextSettings settings;
          ArEnhancedTextSettings_Defaults(&settings);
          settings.size_percent = sizes[size];
          settings.pixelation = (ArEnhancedTextPixelation)treatment;
          settings.pixelation_size = block;
          for (size_t name = 0; name < name_count; ++name) {
            char label[96];
            snprintf(label, sizeof(label), "scale=%d size=%d pixelation=%d/%d name=%zu",
                     scale, sizes[size], treatment, block, name);
            test_case = label;
            Compare(&device, &compositor, &settings, scale, names[name],
                    (unsigned)(name * 5u) % 65u, kArTextDirection_LeftToRight,
                    true, &totals);
          }
        }
      }
    }
  }
  FieldTotals field_totals = {0};
  for (int scale = 2; scale <= 4; ++scale) {
    for (size_t size = 0; size < 3; ++size) {
      for (int treatment = 0; treatment <= 2; ++treatment) {
        for (int block = treatment ? 2 : 0; block <= (treatment ? 8 : 0);
             block += 2) {
          ArEnhancedTextSettings settings;
          ArEnhancedTextSettings_Defaults(&settings);
          settings.size_percent = sizes[size];
          settings.pixelation = (ArEnhancedTextPixelation)treatment;
          settings.pixelation_size = block;
          char label[96];
          snprintf(label, sizeof(label), "field scale=%d size=%d pixelation=%d/%d",
                   scale, sizes[size], treatment, block);
          test_case = label;
          CheckField(&device, &compositor, &settings, scale, &field_totals);
        }
      }
    }
  }
  fprintf(stderr, "entry fields=%u inked cells=%u\n", field_totals.fields,
          field_totals.cells);
  CHECK(field_totals.fields == 81u * 10u);

  /* A right-to-left page places its lines from the other edge; it is always
   * drawn whole, and still identically. */
  ArEnhancedTextSettings settings;
  ArEnhancedTextSettings_Defaults(&settings);
  test_case = "right-to-left page";
  Compare(&device, &compositor, &settings, 4, "A|b", 3,
          kArTextDirection_RightToLeft, false, &totals);

  fprintf(stderr, "live-line comparisons=%u split=%u whole=%u sensitive=%u\n",
          totals.compared, totals.split, totals.whole, totals.sensitive);
  CHECK(totals.sensitive == 8);
  ArLocalizedTextPresenter_Reset(&device);
  ArLocalizedTextPresenter_SetFontResources(NULL);
  CHECK(ArHostFontResources_Destroy(&s_font_store));
  free(s_whole_canvas);
  free(s_split_canvas);
  if (failures) {
    fprintf(stderr, "%d live-line check(s) failed\n", failures);
    return 1;
  }
  puts("localized text live-line checks passed");
  return 0;
}
