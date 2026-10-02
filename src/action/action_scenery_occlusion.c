#include "action_environment_scene.h"
#include <string.h>

static unsigned Reverse(unsigned b) {
  b = ((b & 0x55) << 1) | ((b >> 1) & 0x55);
  b = ((b & 0x33) << 2) | ((b >> 2) & 0x33);
  return ((b << 4) | (b >> 4)) & 255;
}
static bool Tile(const ActionEnvironmentScene *s, unsigned bg, int x, int y,
                 ActionEnvironmentTileEdit *out) {
  ActionEnvironmentTileEdit edit;
  const bool edited = s->tile_edit &&
      s->tile_edit(s->tile_edit_context, bg, x / 8, y / 8, &edit);
  /* A resolved replacement already owns its tile word. Empty/other-depth
   * tiles cannot cast here, so neither needs a second metatile lookup. */
  if (edited && (edit.replace || edit.blank || edit.band != 1)) {
    *out = edit;
    return !out->blank && out->band == 1;
  }
  *out = (ActionEnvironmentTileEdit){0};
  uint8_t metatile;
  if (ActionBgMapView_LookupMetatile(&s->maps[bg], x, y, &metatile)) {
    const unsigned q = ((unsigned)y & 8) / 4 + ((unsigned)x & 8) / 8;
    out->entry =
        (ActionEnvironmentScene_Word(s, bg, metatile, q) & s->word_mask[bg]) | s->attributes[bg];
    out->band = (out->entry & 0x2000) ? 2 : 1;
  } else
    out->blank = 1;
  if (edited) {
    out->band = edit.band;
    out->blank = edit.blank;
    memcpy(out->black, edit.black, 8);
    memcpy(out->transparent, edit.transparent, 8);
  }
  return !out->blank && out->band == 1;
}
static unsigned Opacity(const ActionEnvironmentScene *s, unsigned bg,
                        const ActionEnvironmentTileEdit *tile, unsigned row) {
  if (tile->blank || tile->band != 1) return 0;
  const unsigned native_row = (tile->entry & 0x8000) ? 7 - row : row;
  const unsigned at = (s->tile_base[bg] + (tile->entry & 0x3ff) * 16 + native_row) & 32767;
  const unsigned planes = s->vram[at] | s->vram[(at + 8) & 32767];
  unsigned bits = (planes | (planes >> 8)) & 255;
  if (!(tile->entry & 0x4000)) bits = Reverse(bits);
  return (bits | Reverse(tile->black[row])) & ~Reverse(tile->transparent[row]);
}
bool ActionEnvironmentScene_OpacityRow(const ActionEnvironmentScene *s, unsigned bg, int x, int y,
                                       uint8_t *bits) {
  if (!s || !s->vram || bg > 1 || !bits || (x & 7)) return false;
  ActionEnvironmentTileEdit tile;
  Tile(s, bg, x, y, &tile);
  *bits = (uint8_t)Opacity(s, bg, &tile, (unsigned)y & 7);
  return true;
}
static bool CaptureRuns(const ActionEnvironmentScene *s, ActionMoonlightOcclusion *out,
                        const uint8_t *opacity) {
  if (!s || !s->vram || !out) return false;
  /* Match the extended capture plus apron. Negative authored cells are real
   * scenery; tile lookup rejects unpainted padding without a room-wide scan. */
  const int x0 = (s->camera_x[0] - 256) & ~7, x1 = x0 + 768;
  const int y0 = (s->camera_y[0] - 64) & ~7, y1 = y0 + 352;
  if (x0 < -2048 || x1 > 16384 || y0 < -2048 || y1 > 16384) return false;
  uint16_t runs[2][384];
  unsigned previous_count = 0;
  ActionEnvironmentTileEdit tiles[96];
  out->count = out->valid = 0;
  for (int y = y0; y < y1; ++y) {
    if (!opacity && (y == y0 || !(y & 7)))
      for (int x = x0; x < x1; x += 8)
        Tile(s, 0, x, y, &tiles[(x - x0) / 8]);
    uint16_t *previous = runs[(y - y0) & 1], *current = runs[(y - y0 + 1) & 1];
    unsigned count = 0, match = 0;
    int start = -1;
    for (int tile_x = x0; tile_x <= x1; tile_x += 8) {
      const unsigned bits =
          tile_x == x1 ? 0 : opacity ? opacity[(y-y0)*96+(tile_x-x0)/8] :
          Opacity(s, 0, &tiles[(tile_x - x0) / 8], (unsigned)y & 7);
      if (bits == 255) {
        if (start < 0) start = tile_x;
        continue;
      }
      if (!bits && start < 0) continue;
      for (int pixel = 0; pixel < 8; ++pixel) {
        const int x = tile_x + pixel;
        if (bits & (1u << pixel)) {
          if (start < 0) start = x;
          continue;
        }
        if (start < 0) continue;
        while (match < previous_count && out->rectangles[previous[match]].x0 < start)
          ++match;
        unsigned index;
        if (match < previous_count && out->rectangles[previous[match]].x0 == start &&
            out->rectangles[previous[match]].x1 == x) {
          index = previous[match++];
          out->rectangles[index].y1 = (int16_t)(y + 1);
        } else {
          if (out->count == kActionMoonlightMaxOccluders) return false;
          index = out->count++;
          out->rectangles[index] =
              (ActionMoonlightOccluder){(int16_t)start, (int16_t)y, (int16_t)x, (int16_t)(y + 1)};
        }
        if (count == 384) return false;
        current[count++] = (uint16_t)index;
        start = -1;
      }
    }
    previous_count = count;
  }
  out->valid = 1;
  return true;
}

bool ActionEnvironmentScene_CaptureScenery(const ActionEnvironmentScene *s,
                                           ActionMoonlightOcclusion *out) {
  if (!s || !s->vram || !out) return false;
  ActionSceneryCaptureCache *cache = s->scenery_cache;
  if (!cache) return CaptureRuns(s, out, NULL);
  const int x0 = (s->camera_x[0] - 256) & ~7, y0 = (s->camera_y[0] - 64) & ~7;
  if (x0 < -2048 || x0 + 768 > 16384 || y0 < -2048 || y0 + 352 > 16384) return false;
  for (unsigned y = 0; y < kActionSceneryRows; y += 8) {
    for (unsigned x = 0; x < kActionSceneryTileColumns; ++x) {
      ActionEnvironmentTileEdit tile;
      Tile(s, 0, x0 + (int)x*8, y0 + (int)y, &tile);
      for (unsigned row = 0; row < 8; ++row)
        cache->current[y+row][x] = (uint8_t)Opacity(s, 0, &tile, row);
    }
  }
  if (cache->ready && cache->x == x0 && cache->y == y0 &&
      !memcmp(cache->previous, cache->current, sizeof(cache->current))) {
    out->count = cache->result.count;
    out->valid = cache->result.valid;
    memcpy(out->rectangles, cache->result.rectangles, out->count * sizeof(out->rectangles[0]));
    return true;
  }
  cache->ready = false;
  if (!CaptureRuns(s, out, &cache->current[0][0])) return false;
  cache->x = x0; cache->y = y0;
  memcpy(cache->previous, cache->current, sizeof(cache->current));
  cache->result.count = out->count;
  cache->result.valid = out->valid;
  memcpy(cache->result.rectangles, out->rectangles, out->count * sizeof(out->rectangles[0]));
  cache->ready = true;
  return true;
}
