#if defined(AR_HAS_SDL3_TTF) && AR_HAS_SDL3_TTF
#include "platform/sdl/styled_run_sdl.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* This engine consumes the documented glyph/cluster representation, without
 * adding a second shaping library or depending on SDL's private font state. */
static bool SDLCALL Capture(void *unused, TTF_Text *text) {
  (void)unused;
  text->internal->engine_text = text;
  return true;
}
static void SDLCALL Release(void *unused, TTF_Text *text) {
  (void)unused;
  text->internal->engine_text = NULL;
}
static TTF_TextEngine s_engine = {.version = sizeof(TTF_TextEngine),
                                  .CreateText = Capture,
                                  .DestroyText = Release};

typedef struct ContextShape {
  ArSdlFontSet *fonts;
  TTF_Text *text;
  int width, height;
} ContextShape;

const ArTextRunAppearance *
ArSdlTextAppearance_At(const ArTextRasterRequest *request, size_t offset) {
  offset += request->appearance_source_offset;
  size_t low = 0, high = request->appearance_span_count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    const ArTextAppearanceSpan *span = &request->appearance_spans[middle];
    if (span->end <= offset)
      low = middle + 1;
    else if (span->start > offset)
      high = middle;
    else
      return &span->appearance;
  }
  return request->appearance;
}

static bool SameGeometry(const ArTextRunAppearance *a,
                         const ArTextRunAppearance *b) {
  return a->scale_basis == b->scale_basis && a->italic == b->italic &&
         !strcmp(a->font_role, b->font_role);
}

void ArSdlStyledRun_Destroy(ArSdlStyledRun *run) {
  free(run->clusters);
  free(run->glyphs);
  memset(run, 0, sizeof(*run));
}

static ContextShape *Shape(ContextShape *shapes, size_t *count,
                           ArSdlTextFonts *fonts,
                           const ArTextRunAppearance *appearance,
                           int base_pixels, const char *text, size_t bytes,
                           const ArTextRasterRequest *request, bool rtl,
                           Uint32 script, ArTextRasterFailure *failure) {
  const int pixels =
      (int)(((int64_t)base_pixels * appearance->scale_basis + 5000) / 10000);
  if (pixels < 1 || pixels > 4096) {
    *failure = kArTextRasterFailure_Deterministic;
    SDL_SetError("template scale produces an unsupported effective font size");
    return NULL;
  }
  char error[256] = {0};
  ArSdlFontSet *set =
      ArSdlTextFonts_Acquire(fonts, appearance->font_role, pixels,
                             appearance->italic, failure, error, sizeof(error));
  if (!set) {
    SDL_SetError("%s", error);
    return NULL;
  }
  *failure = kArTextRasterFailure_Retryable;
  for (size_t i = 0; i < *count; ++i)
    if (shapes[i].fonts == set)
      return &shapes[i];
  if (*count == kArSdlMaximumFontVariants) {
    *failure = kArTextRasterFailure_Deterministic;
    SDL_SetError("text run exceeds the font variant limit");
    return NULL;
  }
  ContextShape *shape = &shapes[(*count)++];
  shape->fonts = set;
  char language[128] = {0};
  if (request->language_bcp47_bytes >= sizeof(language))
    return NULL;
  if (request->language_bcp47_bytes)
    memcpy(language, request->language_bcp47, request->language_bcp47_bytes);
  for (size_t i = 0; i <= set->role->fallback_count; ++i)
    if (!TTF_SetFontLanguage(i ? set->fallbacks[i - 1] : set->primary,
                             language[0] ? language : NULL))
      return NULL;
  shape->text = TTF_CreateText(&s_engine, set->primary, text, bytes);
  if (!shape->text ||
      !TTF_SetTextDirection(shape->text,
                            rtl ? TTF_DIRECTION_RTL : TTF_DIRECTION_LTR) ||
      !TTF_SetTextScript(shape->text, script) ||
      !TTF_SetTextWrapWhitespaceVisible(shape->text, true) ||
      !TTF_GetTextSize(shape->text, &shape->width, &shape->height) ||
      !TTF_UpdateText(shape->text))
    return NULL;
  return shape;
}

/* Substring rectangles describe ink, not an independent advance. Derive the
 * advance from neighboring shaped origins, retaining the final bearing. */
static int ClusterAdvance(const ContextShape *shape,
                          const TTF_SubString *cluster, size_t bytes,
                          bool rtl) {
  TTF_SubString adjacent;
  int advance;
  if (rtl) {
    advance = cluster->offset > 0 &&
                      TTF_GetTextSubString(shape->text, cluster->offset - 1,
                                           &adjacent)
                  ? adjacent.rect.x - cluster->rect.x
                  : shape->width - cluster->rect.x;
  } else {
    const size_t end = (size_t)cluster->offset + cluster->length;
    advance =
        end < bytes && TTF_GetTextSubString(shape->text, (int)end, &adjacent)
            ? adjacent.rect.x - cluster->rect.x
            : shape->width - cluster->rect.x;
  }
  return advance > 0 ? advance : 0;
}

static size_t ClusterAt(const ArSdlStyledRun *run, size_t offset) {
  size_t low = 0, high = run->cluster_count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (run->clusters[middle].end <= offset)
      low = middle + 1;
    else
      high = middle;
  }
  return low;
}

static bool CaptureGlyphs(ArSdlStyledRun *run, ContextShape *shapes,
                          size_t count, ContextShape *const *cluster_shapes) {
  size_t capacity = 0;
  for (size_t s = 0; s < count; ++s) {
    const ContextShape *shape = &shapes[s];
    const TTF_TextData *data = shape->text->internal;
    for (int i = 0; i < data->num_ops; ++i) {
      const TTF_DrawOperation *op = &data->ops[i];
      if (op->cmd != TTF_DRAW_COMMAND_COPY || op->copy.text_offset < 0)
        continue;
      const size_t c = ClusterAt(run, (size_t)op->copy.text_offset);
      if (c == run->cluster_count || cluster_shapes[c] != shape)
        continue;
      if (run->glyph_count == capacity) {
        const size_t next = capacity ? capacity * 2 : 32;
        if (next > 65536)
          return SDL_SetError("styled run exceeds the glyph limit");
        ArSdlStyledGlyph *glyphs = realloc(run->glyphs, next * sizeof(*glyphs));
        if (!glyphs)
          return SDL_SetError("out of memory retaining styled glyphs");
        run->glyphs = glyphs;
        capacity = next;
      }
      ArSdlStyledGlyph *glyph = &run->glyphs[run->glyph_count++];
      glyph->copy = op->copy;
      glyph->cluster = c;
      unsigned member = 0;
      if (op->copy.glyph_font != shape->fonts->primary) {
        for (member = 1; member <= shape->fonts->role->fallback_count; ++member)
          if (op->copy.glyph_font == shape->fonts->fallbacks[member - 1])
            break;
        if (member > shape->fonts->role->fallback_count)
          return SDL_SetError("shaper returned an unregistered fallback font");
      }
      run->clusters[c].font_mask |= 1u << member;
      if (!op->copy.glyph_index)
        run->clusters[c].missing_font_mask |= 1u << member;
      glyph->copy.dst.x -= run->clusters[c].rect.x;
      glyph->copy.dst.y -= TTF_GetFontAscent(shape->fonts->primary);
      if (-glyph->copy.dst.y > run->ascent)
        run->ascent = -glyph->copy.dst.y;
      if (glyph->copy.dst.y + glyph->copy.dst.h > run->descent)
        run->descent = glyph->copy.dst.y + glyph->copy.dst.h;
    }
  }
  return true;
}

bool ArSdlStyledRun_Create(ArSdlStyledRun *run, ArSdlTextFonts *fonts,
                           const char *text, size_t bytes,
                           const size_t *logical_offsets,
                           const ArTextRasterRequest *request, int base_pixels,
                           bool rtl, Uint32 script,
                           ArTextRasterFailure *failure) {
  ContextShape shapes[kArSdlMaximumFontVariants] = {0};
  size_t shape_count = 0;
  bool ok = false;
  ContextShape **cluster_shapes = calloc(bytes, sizeof(*cluster_shapes));
  run->clusters = calloc(bytes, sizeof(*run->clusters));
  if (!run->clusters || !cluster_shapes)
    goto done;
  for (size_t at = 0; at < bytes;) {
    const ArTextRunAppearance *appearance =
        ArSdlTextAppearance_At(request, logical_offsets[at]);
    ContextShape *shape =
        Shape(shapes, &shape_count, fonts, appearance, base_pixels, text, bytes,
              request, rtl, script, failure);
    if (!shape)
      goto done;
    TTF_SubString cluster;
    if (!TTF_GetTextSubString(shape->text, (int)at, &cluster) ||
        cluster.offset != (int)at || cluster.length <= 0 ||
        (size_t)cluster.length > bytes - at) {
      *failure = kArTextRasterFailure_Deterministic;
      SDL_SetError("font boundary at byte %zu divides a shaped cluster",
                   logical_offsets[at]);
      goto done;
    }
    const size_t end = at + cluster.length;
    for (size_t i = at + 1; i < end; ++i)
      if (!SameGeometry(appearance,
                        ArSdlTextAppearance_At(request, logical_offsets[i]))) {
        *failure = kArTextRasterFailure_Deterministic;
        SDL_SetError("font boundary at byte %zu divides a shaped cluster",
                     logical_offsets[i]);
        goto done;
      }
    const int ascent = TTF_GetFontAscent(shape->fonts->primary);
    const int descent = shape->height - ascent;
    if (ascent > run->ascent)
      run->ascent = ascent;
    if (descent > run->descent)
      run->descent = descent;
    const size_t index = run->cluster_count++;
    run->clusters[index] = (ArSdlStyledCluster){
        .start = at,
        .end = end,
        .rect = cluster.rect,
        .advance = ClusterAdvance(shape, &cluster, bytes, rtl),
        .pixels = shape->fonts->pixels,
        .appearance = *appearance,
        .role = shape->fonts->role};
    run->clusters[index].rect.y -= ascent;
    cluster_shapes[index] = shape;
    at = end;
  }
  if (!CaptureGlyphs(run, shapes, shape_count, cluster_shapes))
    goto done;
  const size_t first_visual = rtl ? run->cluster_count - 1 : 0;
  const int leading =
      run->cluster_count && run->clusters[first_visual].rect.x > 0
          ? run->clusters[first_visual].rect.x
          : 0;
  run->width = leading;
  for (size_t visual = 0; visual < run->cluster_count; ++visual) {
    ArSdlStyledCluster *cluster =
        &run->clusters[rtl ? run->cluster_count - 1 - visual : visual];
    cluster->rect.x = run->width;
    cluster->rect.y += run->ascent;
    if (cluster->advance > INT_MAX - run->width)
      goto done;
    run->width += cluster->advance;
  }
  if (run->cluster_count)
    run->clusters[first_visual].advance += leading;
  for (size_t i = 0; i < run->glyph_count; ++i) {
    ArSdlStyledGlyph *glyph = &run->glyphs[i];
    glyph->copy.dst.x += run->clusters[glyph->cluster].rect.x;
    glyph->copy.dst.y += run->ascent;
  }
  int ink_right = run->width;
  for (size_t i = 0; i < run->glyph_count; ++i) {
    const SDL_Rect *ink = &run->glyphs[i].copy.dst;
    if (ink->x > INT_MAX - ink->w)
      goto done;
    if (ink->x + ink->w > ink_right)
      ink_right = ink->x + ink->w;
  }
  if (run->cluster_count) {
    const size_t last_visual = rtl ? 0 : run->cluster_count - 1;
    run->clusters[last_visual].advance += ink_right - run->width;
  }
  run->width = ink_right;
  ok = true;
done:
  free(cluster_shapes);
  for (size_t i = 0; i < shape_count; ++i)
    TTF_DestroyText(shapes[i].text);
  if (!ok)
    ArSdlStyledRun_Destroy(run);
  return ok;
}

bool ArSdlStyledRun_Draw(const ArSdlStyledRun *run, SDL_Surface *surface, int x,
                         int y) {
  for (size_t i = 0; i < run->glyph_count; ++i) {
    const TTF_CopyOperation *op = &run->glyphs[i].copy;
    SDL_Surface *glyph =
        TTF_GetGlyphImageForIndex(op->glyph_font, op->glyph_index, NULL);
    if (!glyph)
      return false;
    SDL_Rect dst = op->dst;
    dst.x += x;
    dst.y += y;
    const bool ok = SDL_BlitSurface(glyph, &op->src, surface, &dst);
    SDL_DestroySurface(glyph);
    if (!ok)
      return false;
  }
  return true;
}
#endif
