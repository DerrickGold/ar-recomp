#include "render/localized_text_artwork.h"

#include <math.h>
#include <string.h>

#include "render/localized_text_layout.h"

/* Decoded artwork keeps its uploaded texture and measured ink until the frame
 * supplies different pixels, so a static HUD never re-uploads. */
static struct {
  ArRenderTexture name_cursor;
  uint32_t name_cursor_argb[kArLocalizationFrameNameCursorPixels];
  ArRenderRectI name_cursor_ink;
  ArRenderTexture artwork_textures[kArLocalizationArtwork_Count];
  ArLocalizationArtwork artwork[kArLocalizationArtwork_Count];
  ArRenderRectI artwork_ink[kArLocalizationArtwork_Count];
} s_artwork;

static bool PrepareNameCursor(ArRenderDevice *device,
                              const ArLocalizationFrame *frame,
                              ArRenderTexture *texture) {
  if (!frame->name_cursor_valid) return false;
  if (!ArRenderTexture_IsValid(s_artwork.name_cursor) ||
      memcmp(s_artwork.name_cursor_argb, frame->name_cursor_argb,
             sizeof(s_artwork.name_cursor_argb))) {
    if (!ArRenderTexture_IsValid(s_artwork.name_cursor)) {
      const ArRenderTextureDesc desc = {
          .width = kArLocalizationFrameNameCursorExtent,
          .height = kArLocalizationFrameNameCursorExtent,
          .format = kArRenderPixelFormat_Argb8888,
          .usage = kArRenderTextureUsage_Static,
          .filter = kArRenderFilter_Nearest,
          .blend = kArRenderBlendMode_Alpha,
      };
      if (!ArRenderDevice_CreateTexture(device, &desc,
                                        &s_artwork.name_cursor))
        return false;
    }
    if (!ArRenderDevice_UpdateTexture(
            device, s_artwork.name_cursor, NULL, frame->name_cursor_argb,
            kArLocalizationFrameNameCursorExtent * (int)sizeof(uint32_t))) {
      ArRenderDevice_DestroyTexture(device, s_artwork.name_cursor);
      s_artwork.name_cursor = ArRenderTexture_Invalid();
      return false;
    }
    memcpy(s_artwork.name_cursor_argb, frame->name_cursor_argb,
           sizeof(s_artwork.name_cursor_argb));
    const ArTextBitmap bitmap = {
        .pixels = frame->name_cursor_argb,
        .width = kArLocalizationFrameNameCursorExtent,
        .height = kArLocalizationFrameNameCursorExtent,
        .pitch_bytes = kArLocalizationFrameNameCursorExtent * (int)sizeof(uint32_t),
        .format = kArRenderPixelFormat_Argb8888,
    };
    s_artwork.name_cursor_ink = ArTextBitmap_InkBounds(&bitmap,
        (ArRenderRectI){0, 0, bitmap.width, bitmap.height});
  }
  *texture = s_artwork.name_cursor;
  return true;
}

enum {
  /* Enough for one 8x8 tile at kArtworkSmoothScale, which is every icon the
   * game hands us today (the artwork contract caps a source at 16x8). The
   * scale is generous rather than matched to the destination, which the
   * artwork module does not know: a texture larger than the key is filtered
   * down cleanly, one merely equal to it is not. */
  kArtworkSmoothScale = 8,
  kArtworkSmoothMaximumPixels =
      kArLocalizationArtworkPixels * kArtworkSmoothScale * kArtworkSmoothScale,
};

static uint32_t s_smooth_pixels[kArtworkSmoothMaximumPixels];

static float SquaredDistanceToSegment(float px, float py, float ax, float ay,
                                      float bx, float by) {
  const float dx = bx - ax, dy = by - ay;
  const float length = dx * dx + dy * dy;
  float t = length > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / length : 0.0f;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  const float ex = px - (ax + t * dx), ey = py - (ay + t * dy);
  return ex * ex + ey * ey;
}

/* Smooth enlargement of a captured native icon.
 *
 * The game's icons are 8x8 tiles, so beside HD text they are visibly coarse.
 * Each opaque source pixel is treated as a round dot at its own position and
 * adjacent dots are joined, which keeps the shape's exact topology -- the same
 * strokes, holes and joins -- while giving it resolution-independent edges.
 * Colour is taken from the nearest opaque source pixel, so the game's palette
 * and the shading bands it puts on a letter's top and bottom rows survive.
 *
 * Nothing here is authored from the ROM: the shape arrives at runtime in the
 * captured artwork, which is why this enlarges rather than replaces. */
static bool BuildSmoothArtwork(const ArLocalizationArtwork *art, int *out_width,
                               int *out_height) {
  const int w = art->width, h = art->height;
  if (w <= 0 || h <= 0 || w * h > kArLocalizationArtworkPixels) return false;
  const int sw = w * kArtworkSmoothScale, sh = h * kArtworkSmoothScale;
  /* One source pixel across, so a single-pixel stroke stays a single stroke. */
  const float radius = 0.62f;
  const float edge = 0.5f / (float)kArtworkSmoothScale;
  bool any = false;
  for (int y = 0; y < sh; ++y) {
    for (int x = 0; x < sw; ++x) {
      /* Output pixel centre, expressed in source-pixel coordinates. */
      const float px = ((float)x + 0.5f) / (float)kArtworkSmoothScale;
      const float py = ((float)y + 0.5f) / (float)kArtworkSmoothScale;
      float nearest = 1e9f;
      uint32_t colour = 0;
      float colour_distance = 1e9f;
      for (int sy = 0; sy < h; ++sy) {
        for (int sx = 0; sx < w; ++sx) {
          const uint32_t source = art->argb[sy * w + sx];
          if (!(source >> 24)) continue;
          const float ax = (float)sx + 0.5f, ay = (float)sy + 0.5f;
          float d = SquaredDistanceToSegment(px, py, ax, ay, ax, ay);
          if (d < colour_distance) { colour_distance = d; colour = source; }
          /* Join to the neighbours that follow, so each pair is walked once. */
          static const int kNeighbours[4][2] = {{1,0},{0,1},{1,1},{1,-1}};
          for (unsigned n = 0; n < 4; ++n) {
            const int nx = sx + kNeighbours[n][0], ny = sy + kNeighbours[n][1];
            if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
            if (!(art->argb[ny * w + nx] >> 24)) continue;
            const float d2 = SquaredDistanceToSegment(
                px, py, ax, ay, (float)nx + 0.5f, (float)ny + 0.5f);
            if (d2 < d) d = d2;
          }
          if (d < nearest) nearest = d;
        }
      }
      const float distance = sqrtf(nearest);
      float coverage = (radius + edge - distance) / (2.0f * edge);
      if (coverage < 0.0f) coverage = 0.0f;
      if (coverage > 1.0f) coverage = 1.0f;
      const uint32_t alpha =
          (uint32_t)((float)(colour >> 24) * coverage + 0.5f);
      s_smooth_pixels[y * sw + x] = alpha ? ((alpha << 24) | (colour & 0xffffffu)) : 0u;
      any = any || alpha != 0u;
    }
  }
  *out_width = sw;
  *out_height = sh;
  return any;
}

bool ArLocalizedTextArtwork_PrepareTexture(ArRenderDevice *device,
                            const ArLocalizationFrame *frame,
                            ArLocalizationArtworkKind kind,
                            ArRenderTexture *texture) {
  if ((unsigned)kind >= kArLocalizationArtwork_Count) return false;
  const ArLocalizationArtwork *art = &frame->artwork[kind];
  if (!art->valid || !art->width || !art->height ||
      art->width * art->height > kArLocalizationArtworkPixels)
    return false;
  ArRenderTexture *cached = &s_artwork.artwork_textures[kind];
  ArLocalizationArtwork *previous = &s_artwork.artwork[kind];
  if (ArRenderTexture_IsValid(*cached) && previous->width == art->width &&
      previous->height == art->height &&
      !memcmp(previous->argb, art->argb, sizeof(art->argb))) {
    *texture = *cached;
    return true;
  }
  /* The keyboard's action keys sit among HD text, where an 8x8 tile reads as
   * a smudge. Enlarge those; the report icons keep their native pixels, which
   * is how they have always been reviewed. */
  const bool smooth = kind == kArLocalizationArtwork_NameFinish ||
      kind == kArLocalizationArtwork_NameBackspace;
  const uint32_t *pixels = art->argb;
  int width = art->width, height = art->height;
  if (smooth && !BuildSmoothArtwork(art, &width, &height)) {
    width = art->width;
    height = art->height;
  } else if (smooth) {
    pixels = s_smooth_pixels;
  }
  if (previous->width != art->width || previous->height != art->height) {
    ArRenderDevice_DestroyTexture(device, *cached);
    *cached = ArRenderTexture_Invalid();
  }
  if (!ArRenderTexture_IsValid(*cached)) {
    const ArRenderTextureDesc desc = {
        .width = width, .height = height,
        .format = kArRenderPixelFormat_Argb8888,
        .usage = kArRenderTextureUsage_Static,
        /* Enlarged art is already antialiased; sampling it sharply would put
         * the stair steps back. */
        .filter = pixels == art->argb ? kArRenderFilter_Nearest
                                      : kArRenderFilter_Linear,
        .blend = kArRenderBlendMode_Alpha,
    };
    if (!ArRenderDevice_CreateTexture(device, &desc, cached)) return false;
  }
  if (!ArRenderDevice_UpdateTexture(device, *cached, NULL, pixels,
                                    width * (int)sizeof(uint32_t))) {
    ArRenderDevice_DestroyTexture(device, *cached);
    *cached = ArRenderTexture_Invalid();
    return false;
  }
  *previous = *art;
  const ArTextBitmap bitmap = {
      .pixels = art->argb, .width = art->width, .height = art->height,
      .pitch_bytes = art->width * (int)sizeof(uint32_t),
      .format = kArRenderPixelFormat_Argb8888,
  };
  s_artwork.artwork_ink[kind] = ArTextBitmap_InkBounds(&bitmap,
      (ArRenderRectI){0, 0, art->width, art->height});
  *texture = *cached;
  return true;
}

/* Whether the `separator_bytes` bytes ending at `end` are the key separator. */
static bool SeparatorEndsAt(const char *utf8, size_t utf8_bytes, size_t end,
                            const char *separator, size_t separator_bytes) {
  return end <= utf8_bytes && end >= separator_bytes &&
      !memcmp(utf8 + end - separator_bytes, separator, separator_bytes);
}

/* A selector sits in the blank the game reserves between keys. Measure that
 * shaped room rather than assuming a blank is a fraction of the line height: a
 * full line-height arrow can otherwise cover the previous key.
 *
 * A gutter is a run of two or more adjacent clusters of the surface's declared
 * key separator -- the renderer is told what that is, so a keyboard spaced
 * with an ideographic blank works without changing this code, and a lone blank
 * inside ordinary text is not a gutter. The narrowest gutter sizes the
 * selector, so it stays the same as the selection moves and as fallback fonts
 * change individual advances. */
static int NameCursorExtent(const ArTextSurface *surface,
                            const ArLocalizationTextSnapshot *snapshot,
                            const char *utf8, size_t utf8_bytes,
                            int maximum_extent) {
  const size_t bytes = snapshot->key_separator_bytes;
  const char *separator = snapshot->key_separator;
  if (!bytes) return maximum_extent;
  for (size_t index = 0; index < surface->reveal_cluster_count; ++index) {
    const ArTextRevealCluster *first = &surface->reveal_clusters[index];
    const size_t end = first->end_utf8_byte;
    /* A gutter starts where one separator is followed by another. */
    if (!SeparatorEndsAt(utf8, utf8_bytes, end, separator, bytes) ||
        end + bytes > utf8_bytes ||
        memcmp(utf8 + end, separator, bytes) != 0)
      continue;
    int right = first->x + first->width;
    size_t last_end = end;
    while (index + 1u < surface->reveal_cluster_count) {
      const ArTextRevealCluster *next = &surface->reveal_clusters[index + 1u];
      if (next->end_utf8_byte != last_end + bytes ||
          next->line_index != first->line_index ||
          !SeparatorEndsAt(utf8, utf8_bytes, next->end_utf8_byte, separator,
                           bytes))
        break;
      if (next->x + next->width > right) right = next->x + next->width;
      last_end = next->end_utf8_byte;
      ++index;
    }
    const int width = right - first->x;
    if (width > 0 && width < maximum_extent) maximum_extent = width;
  }
  return maximum_extent;
}

/* The ink every key on one row put on the surface. Individual keys differ --
 * accents, descenders, a blank placeholder -- so anything sized or centred
 * against the row uses the whole row and stays put as the selection moves. */
static ArRenderRectI RowInk(const ArTextSurface *surface, int line_index) {
  ArRenderRectI ink = {0};
  if (!surface->cluster_ink_bounds) return ink;
  for (size_t i = 0; i < surface->reveal_cluster_count; ++i) {
    if (surface->reveal_clusters[i].line_index == line_index)
      ink = ArLocalizedTextLayout_UnionInk(ink, surface->cluster_ink_bounds[i]);
  }
  return ink;
}

static bool CursorSelectsArtwork(
    const ArLocalizationFrame *frame, const ArLocalizationTextSnapshot *snapshot,
    const ArLocalizationInlineObjectSnapshot *cursor) {
  if (snapshot->inline_object_offset > frame->inline_object_count ||
      snapshot->inline_object_count >
          frame->inline_object_count - snapshot->inline_object_offset)
    return false;
  for (uint8_t i = 0; i < snapshot->inline_object_count; ++i) {
    const ArLocalizationInlineObjectSnapshot *key =
        &frame->inline_objects[snapshot->inline_object_offset + i];
    if (key->end_utf8_byte == cursor->end_utf8_byte &&
        (key->kind == kArLocalizationInlineObject_NameBackspace ||
         key->kind == kArLocalizationInlineObject_NameFinish))
      return true;
  }
  return false;
}

bool ArLocalizedTextArtwork_PrepareInlineObject(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    const ArLocalizationTextSnapshot *snapshot,
    const ArLocalizationInlineObjectSnapshot *object, const ArTextSurface *surface,
    const char *utf8, size_t utf8_bytes,
    const ArTextRevealCluster *cluster, ArRenderRectI text_destination,
    int key_cell_extent, ArLocalizedPreparedInlineObject *prepared) {
  if (!object || !surface || !cluster || !prepared) return false;
  const ArLocalizationInlineObjectKind kind = object->kind;
  if (kind == kArLocalizationInlineObject_NameFieldUnderline) {
    *prepared = (ArLocalizedPreparedInlineObject){.kind = kind};
    return ArLocalizedTextLayout_NameUnderline(
        surface, cluster, text_destination, &prepared->destination);
  }
  const bool name_cursor =
      kind == kArLocalizationInlineObject_NameCursor;
  /* The finish and backspace keys are whole keys, not marks beside one: the
   * game fills their glyph cell exactly as it fills a letter's. Their
   * placeholder characters carry almost no advance, so measuring the cluster
   * would draw them at a fraction of the size the original had. */
  const bool key_artwork =
      kind == kArLocalizationInlineObject_NameFinish ||
      kind == kArLocalizationInlineObject_NameBackspace;
  int extent = cluster->width;
  if (key_artwork && key_cell_extent > 0) {
    /* The cell is the room the key owns, and at the default text size it is
     * what the original filled. It is a ceiling rather than the answer: the
     * cell follows the window, while the letters follow the text-size setting,
     * so a reader who shrinks the text would otherwise be left with two
     * oversized keys among small ones. */
    const int row_ink_height = RowInk(surface, cluster->line_index).h;
    extent = key_cell_extent;
    if (row_ink_height > 0 && row_ink_height < extent) extent = row_ink_height;
  } else {
    if (surface->line_advance > 0 && extent > surface->line_advance)
      extent = surface->line_advance;
    if (extent > cluster->height) extent = cluster->height;
  }
  if (extent <= 0) return false;

  ArRenderTexture texture = ArRenderTexture_Invalid();
  int object_height = extent;
  if (name_cursor) {
    object_height = cluster->height;
    if (surface->line_advance > 0 && object_height > surface->line_advance)
      object_height = surface->line_advance;
    /* A stated key cell is the room the game itself gave the selector, so it
     * replaces the gutter measurement rather than capping it. */
    object_height = key_cell_extent > 0
        ? key_cell_extent
        : NameCursorExtent(surface, snapshot, utf8, utf8_bytes, object_height);
    if (object_height <= 0) return false;
    if (!PrepareNameCursor(device, frame, &texture)) return false;
  } else if (kind == kArLocalizationInlineObject_StatusLife ||
             kind == kArLocalizationInlineObject_StatusPopulation ||
             kind == kArLocalizationInlineObject_SpeedDirection) {
    const ArLocalizationArtworkKind art_kind =
        kind == kArLocalizationInlineObject_StatusLife ? kArLocalizationArtwork_Life :
        kind == kArLocalizationInlineObject_StatusPopulation ? kArLocalizationArtwork_Population :
        kArLocalizationArtwork_SpeedDirection;
    if (!ArLocalizedTextArtwork_PrepareTexture(device, frame, art_kind, &texture)) return false;
  } else if (kind == kArLocalizationInlineObject_NameFinish ||
             kind == kArLocalizationInlineObject_NameBackspace) {
    /* Prefer the keyboard's own glyphs. They are only captured while the
     * keyboard is on screen, so a failure here is ordinary: the drawing side
     * falls back to its own shapes rather than leaving the key blank. */
    const ArLocalizationArtworkKind art_kind =
        kind == kArLocalizationInlineObject_NameFinish
            ? kArLocalizationArtwork_NameFinish
            : kArLocalizationArtwork_NameBackspace;
    (void)ArLocalizedTextArtwork_PrepareTexture(device, frame, art_kind,
                                                &texture);
  }
  *prepared = (ArLocalizedPreparedInlineObject){
      .kind = kind,
      .destination = {text_destination.x + cluster->x +
           (name_cursor ? -object_height : (cluster->width - extent) / 2),
       text_destination.y + cluster->y +
           (cluster->height - object_height) / 2,
       name_cursor ? object_height : extent, object_height},
      .texture = texture,
  };
  if ((name_cursor || kind == kArLocalizationInlineObject_NameBackspace ||
       kind == kArLocalizationInlineObject_NameFinish ||
       kind == kArLocalizationInlineObject_SelectionPointer) &&
      surface->cluster_ink_bounds) {
    ArRenderRectI ink = {0};
    if (name_cursor && !CursorSelectsArtwork(frame, snapshot, object)) {
      /* Center on the selected key's visible ink. A row union lets a
       * neighbour's descender or accent pull the arrow away from that key,
       * especially between the upper- and lowercase rows. These bounds are
       * already cached, including the current pixelation and shading. */
      for (size_t i = 0; i < surface->reveal_cluster_count; ++i) {
        if (surface->reveal_clusters[i].end_utf8_byte == cluster->end_utf8_byte) {
          ink = surface->cluster_ink_bounds[i];
          break;
        }
      }
    }
    /* Blank/inline-art keys have no text ink: use the same row center as the
     * backspace and finish artwork drawn in their place. */
    if (ink.h <= 0) ink = RowInk(surface, cluster->line_index);
    if (ink.h > 0) {
      ink.y += text_destination.y;
      return ArLocalizedTextLayout_CenterInkVertically(
          ink, name_cursor ? s_artwork.name_cursor_ink
                           : (ArRenderRectI){0, 0, extent, object_height},
          name_cursor ? kArLocalizationFrameNameCursorExtent : object_height,
          &prepared->destination);
    }
  }
  return true;
}

ArRenderRectI ArLocalizedTextArtwork_Ink(ArLocalizationArtworkKind kind) {
  return (unsigned)kind < kArLocalizationArtwork_Count
      ? s_artwork.artwork_ink[kind] : (ArRenderRectI){0};
}

void ArLocalizedTextArtwork_Reset(ArRenderDevice *device) {
  for (unsigned i = 0; i < kArLocalizationArtwork_Count; ++i) {
    ArRenderDevice_DestroyTexture(device, s_artwork.artwork_textures[i]);
    s_artwork.artwork_textures[i] = ArRenderTexture_Invalid();
    memset(&s_artwork.artwork[i], 0, sizeof(s_artwork.artwork[i]));
    s_artwork.artwork_ink[i] = (ArRenderRectI){0};
  }
  ArRenderDevice_DestroyTexture(device, s_artwork.name_cursor);
  s_artwork.name_cursor = ArRenderTexture_Invalid();
  s_artwork.name_cursor_ink = (ArRenderRectI){0};
  memset(s_artwork.name_cursor_argb, 0, sizeof(s_artwork.name_cursor_argb));
}
