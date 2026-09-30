#include "sim/world_nav/sim_world_navigation_art.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "sim/mountains/sim_background_mountains.h"
#include "sim/town/sim_town_ground_art.h"
#include "sim/town/sim_town_terrain.h"

_Static_assert(sizeof(SimWorldNavigationArtAnimation) <= 320 * 1024,
    "prepared animation work must stay within its 320 KiB budget");

static void CopySourceRow(uint32_t *out, int x, int y, int width,
                          const uint32_t *developed, int developed_pitch) {
  y = y < 0 ? 0 : y >= kSimWorldMapPixels ? kSimWorldMapPixels - 1 : y;
  const uint32_t *row = developed + (size_t)y * developed_pitch;
  for (int i = 0; i < width + 2; i++) {
    int sx = x + i - 1;
    sx = sx < 0 ? 0 : sx >= kSimWorldMapPixels ? kSimWorldMapPixels - 1 : sx;
    out[i] = row[sx];
  }
}

static void RebuildGroundRectangle(uint32_t *out, int pitch,
                                   int source_x, int source_y, int width, int height,
                                   const uint32_t *developed, int developed_pitch) {
  /* Scale2x needs only three source rows and a one-pixel halo. Keeping them
   * in a bounded rolling window replaces the full bake's 4 MiB scratch
   * allocation and avoids rebuilding the same halo for adjacent dirty cells.
   * This is the original pixel rule, not a different filter or resolution. */
  uint32_t rows[3][kSimWorldMapPixels + 2];
  uint32_t *north = rows[0], *row = rows[1], *south = rows[2];
  CopySourceRow(north, source_x, source_y - 1, width,
      developed, developed_pitch);
  CopySourceRow(row, source_x, source_y, width,
      developed, developed_pitch);
  for (int y = 0; y < height; y++) {
    CopySourceRow(south, source_x, source_y + y + 1, width,
        developed, developed_pitch);
    uint32_t *north_out = out + (size_t)y * 2 * pitch;
    uint32_t *south_out = north_out + pitch;
    for (int x = 0; x < width; x++) {
      const uint32_t above = north[x + 1], left = row[x], centre = row[x + 1];
      const uint32_t right = row[x + 2], below = south[x + 1];
      north_out[x * 2] =
          left == above && left != below && above != right ? left : centre;
      north_out[x * 2 + 1] =
          above == right && above != left && right != below ? right : centre;
      south_out[x * 2] =
          left == below && left != above && below != right ? left : centre;
      south_out[x * 2 + 1] =
          below == right && left != below && above != right ? right : centre;
    }
    uint32_t *reuse = north;
    north = row;
    row = south;
    south = reuse;
  }
}

bool SimWorldNavigationArt_Build(
    uint32_t *out_pixels, int out_pitch_pixels,
    const uint32_t *developed_pixels, int developed_pitch_pixels) {
  if (!out_pixels || out_pitch_pixels < kSimWorldNavigationArtPixels ||
      !developed_pixels || developed_pitch_pixels < kSimWorldMapPixels)
    return false;
  RebuildGroundRectangle(out_pixels, out_pitch_pixels, 0, 0,
      kSimWorldMapPixels, kSimWorldMapPixels,
      developed_pixels, developed_pitch_pixels);
  return true;
}

static bool GroundTile(const SimWorldNavigationTownGround *ground, uint8_t town,
                       int x, int y, bool detailed, bool models, bool cliffs, uint8_t *tile) {
  if (ground->native_rows[town - 1][y] & (UINT32_C(1) << x)) return false;
  *tile = ground->terrain[town - 1][y * kSimTownCells + x];
  if (SimBackgroundMountains_TileFlags(town, *tile)) return false;
  if (ground->object_rows[town - 1][y] & (UINT32_C(1) << x)) {
    if (!models) return false;
    /* The locked ancient tree stands on snow, not a developed grass plot. */
    if (*tile != 0xFF)
      *tile = *tile == 0xE1 ? 0x41 : *tile == 0xE2 ? 0x3A : 0x08;
  } else if (!detailed || (!cliffs && SimTownTerrain_IsFaceCell(town, x, y)) ||
             (*tile >= 0xE0 && *tile <= 0xEF)) {
    return false;
  }
  return true;
}

static int NorthwallRingIndex(const SimWorldNavigationTownGround *ground,
                             uint8_t town, int x, int y) {
  if (town != 6 || !(ground->native_rows[5][y] & (UINT32_C(1) << x))) return -1;
  const uint8_t tile = ground->terrain[5][y * kSimTownCells + x];
  if (tile != 0xC0 && tile != 0xC1 && tile != 0xC8 && tile != 0xC9) return -1;
  return (tile & 1) | ((tile & 8) >> 2);
}

static size_t RingSourcePixel(int x, int y) {
  /* One native snow tile supplies the halo around the complete 2x2 ring. */
  const unsigned tile = x < 0 || y < 0 || x >= 16 || y >= 16 ? 0 :
      1 + (y / 8) * 2 + x / 8;
  return tile * 64 + ((y + 8) & 7) * 8 + ((x + 8) & 7);
}

static bool PrepareNorthwallRing(const SimWorldNavigationTownGround *ground,
                                uint32_t out[4][kSimTownCellPixels * kSimTownCellPixels]) {
  bool needed = false;
  if (ground && (ground->enabled_town_mask & 32))
    for (int y = 0; y < kSimTownCells; y++)
      needed |= ground->native_rows[5][y] != 0;
  if (!needed) return true;
  uint32_t pixels[5 * 64], snow[2];
  uint8_t indices[5 * 64];
  static const uint8_t tiles[] = {0x02, 0xA4, 0xA5, 0xB4, 0xB5};
  if (!SimTownGroundArt_PaletteColor(6, 0x1E, &snow[0]) ||
      !SimTownGroundArt_PaletteColor(6, 0x1F, &snow[1])) return false;
  for (unsigned tile = 0; tile < 5; tile++)
    if (!SimWorldMap_CopyTileArt(tiles[tile], pixels + tile * 64, indices + tile * 64))
      return false;
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++) {
      const size_t centre = RingSourcePixel(x, y), above = RingSourcePixel(x, y - 1);
      const size_t left = RingSourcePixel(x - 1, y), right = RingSourcePixel(x + 1, y);
      const size_t below = RingSourcePixel(x, y + 1);
      const uint32_t a = pixels[above], l = pixels[left], r = pixels[right], b = pixels[below];
      /* Select exactly the native Scale2x samples before remapping colours.
       * Only authored snow identities change; ring ink and texture stay intact. */
      const size_t samples[] = {
        l == a && l != b && a != r ? left : centre,
        a == r && a != l && r != b ? right : centre,
        l == b && l != a && b != r ? left : centre,
        b == r && l != b && a != r ? right : centre,
      };
      const unsigned tile = (y / 8) * 2 + x / 8;
      for (unsigned p = 0; p < 4; p++) {
        const size_t sample = samples[p];
        const unsigned at = (y % 8 * 2 + (p >> 1)) * 16 + x % 8 * 2 + (p & 1);
        out[tile][at] = indices[sample] == 0x0E ? snow[0] :
            indices[sample] == 0x0F ? snow[1] : pixels[sample];
      }
    }
  return true;
}

static void GroundFeather(unsigned feather[kSimTownCells * kSimTownCellPixels]) {
  const int pixels = kSimTownCells * kSimTownCellPixels;
  const float width = kSimWorldNavigationTownFeatherPixels * kSimWorldNavigationArtScale;
  for (int i = 0; i < pixels; i++) {
    const int edge = i < pixels - 1 - i ? i : pixels - 1 - i;
    const float t = edge < width ? edge / width : 1;
    feather[i] = (unsigned)(256 * t * t * (3 - 2 * t) + .5f);
  }
}

static uint32_t BlendGround(uint32_t world, uint32_t town, unsigned weight) {
  if (weight >= 256) return town;
  if (!weight || world == town) return world;
  uint32_t out = UINT32_C(0xFF000000);
  for (int shift = 0; shift < 24; shift += 8)
    out |= (((((world >> shift) & 255u) * (256u - weight) +
              ((town >> shift) & 255u) * weight + 128u) >> 8) << shift);
  return out;
}

static void OverlayGroundCell(uint32_t *out, int pitch, const uint32_t *pixels,
                             int cx, int cy, const unsigned *feather, bool model_owned) {
  for (int y = 0; y < kSimTownCellPixels; y++)
    for (int x = 0; x < kSimTownCellPixels; x++) {
      const uint32_t color = pixels[y * kSimTownCellPixels + x];
      if (!(color >> 24)) continue;
      const unsigned fx = feather[cx * kSimTownCellPixels + x];
      const unsigned fy = feather[cy * kSimTownCellPixels + y];
      /* Replacement geometry owns the entire source footprint. Blending
       * its old overview glyph back in would resurrect native tree bundles. */
      out[y * pitch + x] = BlendGround(out[y * pitch + x], color,
          model_owned ? 256 : fx < fy ? fx : fy);
    }
}

bool SimWorldNavigationArt_OverlayTownGround(
    uint32_t *out_pixels, int out_pitch_pixels,
    const SimWorldNavigationTownGround *ground, bool detailed_ground,
    bool models_enabled, bool cliff_geometry,
    uint8_t animation_phase) {
  if (!out_pixels || out_pitch_pixels < kSimWorldNavigationArtPixels ||
      !ground || !SimTownGroundArt_Available() ||
      animation_phase >= kSimTownGroundAnimationFrames) return false;
  /* Resolve every required atlas before changing any output pixels. */
  for (uint8_t town = 1; town <= kSimTownCount; town++)
    if ((ground->enabled_town_mask & (1u << (town - 1))) &&
        !SimTownGroundArt_AnimatedMetatile(town, ground->development_tier[town - 1], 8,
                                         animation_phase))
      return false;
  uint32_t ring[4][kSimTownCellPixels * kSimTownCellPixels];
  if (detailed_ground && !PrepareNorthwallRing(ground, ring)) return false;
  unsigned feather[kSimTownCells * kSimTownCellPixels];
  GroundFeather(feather);
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    if (!(ground->enabled_town_mask & (1u << (town - 1)))) continue;
    int origin_x, origin_y;
    if (!SimWorldMap_OriginForTown(town, &origin_x, &origin_y)) continue;
    for (int cy = 0; cy < kSimTownCells; cy++)
      for (int cx = 0; cx < kSimTownCells; cx++) {
        uint8_t tile;
        /* Do not flatten perspective-authored slopes onto the relief mesh.
         * Marahna's reused $8D marsh is intentionally not a mountain. */
        const int ring_index = detailed_ground ? NorthwallRingIndex(ground, town, cx, cy) : -1;
        const uint32_t *pixels;
        if (ring_index >= 0) pixels = ring[ring_index];
        else {
          if (!GroundTile(ground, town, cx, cy, detailed_ground,
                          models_enabled, cliff_geometry, &tile)) continue;
          pixels = SimTownGroundArt_AnimatedMetatile(
              town, ground->development_tier[town - 1], tile, animation_phase);
        }
        uint32_t *out = out_pixels + (size_t)(origin_y + cy) * kSimTownCellPixels *
            out_pitch_pixels + (origin_x + cx) * kSimTownCellPixels;
        const bool model_owned = models_enabled &&
            (ground->object_rows[town - 1][cy] & (UINT32_C(1) << cx));
        OverlayGroundCell(out, out_pitch_pixels, pixels, cx, cy, feather, model_owned);
      }
  }
  return true;
}

bool SimWorldNavigationArt_PrepareAnimation(
    SimWorldNavigationArtAnimation *work,
    uint32_t *out_pixels, int out_pitch_pixels,
    const uint32_t *developed_pixels, int developed_pitch_pixels,
    const uint8_t *world_cells,
    const SimWorldNavigationTownGround *ground, bool detailed_ground,
    bool models_enabled, bool cliff_geometry,
    uint8_t previous_phase, uint8_t animation_phase) {
  if (!work) return false;
  work->ready = false;
  if (!out_pixels || out_pitch_pixels < kSimWorldNavigationArtPixels ||
      !developed_pixels || developed_pitch_pixels < kSimWorldMapPixels ||
      (ground && !SimTownGroundArt_Available()) ||
      previous_phase >= kSimTownGroundAnimationFrames ||
      animation_phase >= kSimTownGroundAnimationFrames) return false;
  /* Resolve allocations before touching the retained image. Identical/static
   * variants then cost pointer comparisons, not per-pixel work. */
  for (uint8_t town = 1; ground && town <= kSimTownCount; town++)
    if ((ground->enabled_town_mask & (1u << (town - 1))) &&
        (!SimTownGroundArt_AnimatedMetatile(town, ground->development_tier[town - 1], 8,
                                            previous_phase) ||
         !SimTownGroundArt_AnimatedMetatile(town, ground->development_tier[town - 1], 8,
                                            animation_phase)))
      return false;
  if (detailed_ground && !PrepareNorthwallRing(ground, work->northwall_ring)) return false;
  work->output = out_pixels;
  work->output_pitch = out_pitch_pixels;
  work->developed = developed_pixels;
  work->developed_pitch = developed_pitch_pixels;
  GroundFeather(work->feather);
  SimWorldNavigationArtChanges *changes = &work->changes;
  memset(changes, 0, sizeof(*changes));
  memset(work->overlay, 0, sizeof(work->overlay));
  if (world_cells) memcpy(changes->cells, world_cells, sizeof(changes->cells));
  for (uint8_t town = 1; ground && town <= kSimTownCount; town++) {
    if (!(ground->enabled_town_mask & (1u << (town - 1)))) continue;
    int ox, oy;
    if (!SimWorldMap_OriginForTown(town, &ox, &oy)) continue;
    for (int y = 0; y < kSimTownCells; y++)
      for (int x = 0; x < kSimTownCells; x++) {
        uint8_t tile;
        const int ring_index = detailed_ground ? NorthwallRingIndex(ground, town, x, y) : -1;
        const uint32_t *before, *after;
        if (ring_index >= 0) before = after = work->northwall_ring[ring_index];
        else {
          if (!GroundTile(ground, town, x, y, detailed_ground,
                          models_enabled, cliff_geometry, &tile)) continue;
          before = SimTownGroundArt_AnimatedMetatile(
              town, ground->development_tier[town - 1], tile, previous_phase);
          after = SimTownGroundArt_AnimatedMetatile(
              town, ground->development_tier[town - 1], tile, animation_phase);
        }
        const int at = (oy + y) * kSimWorldMapTiles + ox + x;
        work->overlay[at].pixels = after;
        work->overlay[at].x = (uint8_t)x;
        work->overlay[at].y = (uint8_t)y;
        work->overlay[at].model_owned = models_enabled &&
            (ground->object_rows[town - 1][y] & (UINT32_C(1) << x));
        if (before == after || !memcmp(before, after,
            kSimTownCellPixels * kSimTownCellPixels * sizeof(*after))) continue;
        changes->cells[(oy + y) * kSimWorldMapTiles + ox + x] = 1;
      }
  }
  work->ready = true;
  return true;
}

void SimWorldNavigationArt_RenderAnimationRows(
    const SimWorldNavigationArtAnimation *work, size_t first, size_t end) {
  if (!work || !work->ready || first >= end || end > kSimWorldMapTiles) return;
  for (size_t y = first; y < end; y++) {
    for (int x = 0; x < kSimWorldMapTiles;) {
      if (!work->changes.cells[y * kSimWorldMapTiles + x]) { x++; continue; }
      const int first = x;
      while (x < kSimWorldMapTiles && work->changes.cells[y * kSimWorldMapTiles + x]) x++;
      uint32_t *out =
          work->output + y * kSimTownCellPixels * work->output_pitch + first * kSimTownCellPixels;
      RebuildGroundRectangle(out, work->output_pitch, first * kSimWorldMapTilePixels,
          y * kSimWorldMapTilePixels, (x - first) * kSimWorldMapTilePixels, kSimWorldMapTilePixels,
          work->developed, work->developed_pitch);
    }
    for (int x = 0; x < kSimWorldMapTiles; ++x) {
      const size_t at = y * kSimWorldMapTiles + x;
      if (!work->changes.cells[at] || !work->overlay[at].pixels) continue;
      uint32_t *out =
          work->output + y * kSimTownCellPixels * work->output_pitch + x * kSimTownCellPixels;
      OverlayGroundCell(out, work->output_pitch, work->overlay[at].pixels,
          work->overlay[at].x, work->overlay[at].y, work->feather, work->overlay[at].model_owned);
    }
  }
}

bool SimWorldNavigationArt_UpdateAnimation(
    uint32_t *out_pixels, int out_pitch_pixels,
    const uint32_t *developed_pixels, int developed_pitch_pixels,
    const uint8_t *world_cells,
    const SimWorldNavigationTownGround *ground, bool detailed_ground,
    bool models_enabled, bool cliff_geometry,
    uint8_t previous_phase, uint8_t animation_phase,
    SimWorldNavigationArtChanges *changes) {
  SimWorldNavigationArtAnimation work;
  if (!changes || !SimWorldNavigationArt_PrepareAnimation(&work,
          out_pixels, out_pitch_pixels, developed_pixels, developed_pitch_pixels,
          world_cells, ground, detailed_ground,
          models_enabled, cliff_geometry, previous_phase, animation_phase)) return false;
  SimWorldNavigationArt_RenderAnimationRows(&work, 0, kSimWorldMapTiles);
  *changes = work.changes;
  return true;
}
