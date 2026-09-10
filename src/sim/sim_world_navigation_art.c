#include "sim_world_navigation_art.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "sim_background_mountains.h"
#include "sim_town_ground_art.h"
#include "sim_town_terrain.h"

static float Smoothstep(float value) {
  if (value <= 0.0f) return 0.0f;
  if (value >= 1.0f) return 1.0f;
  return value * value * (3.0f - 2.0f * value);
}

float SimWorldNavigationArt_TownWeight(int source_x, int source_y) {
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    int origin_x = 0, origin_y = 0;
    if (!SimWorldMap_OriginForTown(town, &origin_x, &origin_y)) continue;
    const int x0 = origin_x * kSimWorldMapTilePixels;
    const int y0 = origin_y * kSimWorldMapTilePixels;
    const int x1 = x0 + kSimTownCells * kSimWorldMapTilePixels;
    const int y1 = y0 + kSimTownCells * kSimWorldMapTilePixels;
    if (source_x < x0 || source_x >= x1 ||
        source_y < y0 || source_y >= y1)
      continue;
    int distance = source_x - x0;
    if (source_y - y0 < distance) distance = source_y - y0;
    if (x1 - 1 - source_x < distance) distance = x1 - 1 - source_x;
    if (y1 - 1 - source_y < distance) distance = y1 - 1 - source_y;
    return Smoothstep(
        (float)distance / (float)kSimWorldNavigationTownFeatherPixels);
  }
  return 1.0f;
}

static uint32_t BlendPixel(uint32_t baseline, uint32_t developed,
                           int source_x, int source_y) {
  if (baseline == developed) return developed;
  /* The pristine map is a material reference, never authoritative game
   * state. In particular Bloodpool's cleansed western lake touches the town
   * feather strip: blending its old red water back into live blue water
   * resurrects pollution. Only blend nearby shades of compatible materials;
   * changed water, reclaimed desert and structures keep their live colours. */
  for (int shift = 0; shift <= 24; shift += 8) {
    const int a = (int)((baseline >> shift) & 0xFFu);
    const int b = (int)((developed >> shift) & 0xFFu);
    if (abs(a - b) > 24) return developed;
  }
  /* Material changes never blend, so they also need no six-town feather
   * lookup. Resolve the spatial weight only for compatible, unequal shades. */
  const float weight = SimWorldNavigationArt_TownWeight(source_x, source_y);
  if (weight >= 1.0f) return developed;
  const unsigned fixed = (unsigned)(weight * 256.0f + 0.5f);
  const unsigned inverse = 256u - fixed;
  uint32_t out = 0;
  for (int shift = 0; shift <= 24; shift += 8) {
    const unsigned a = (baseline >> shift) & 0xFFu;
    const unsigned b = (developed >> shift) & 0xFFu;
    out |= ((a * inverse + b * fixed + 128u) >> 8) << shift;
  }
  return out;
}

static void BlendSourceRow(uint32_t *out, int x, int y, int width,
                           const uint32_t *developed, int developed_pitch,
                           const uint32_t *baseline, int baseline_pitch) {
  y = y < 0 ? 0 : y >= kSimWorldMapPixels ? kSimWorldMapPixels - 1 : y;
  const uint32_t *a = baseline + (size_t)y * baseline_pitch;
  const uint32_t *b = developed + (size_t)y * developed_pitch;
  for (int i = 0; i < width + 2; i++) {
    int sx = x + i - 1;
    sx = sx < 0 ? 0 : sx >= kSimWorldMapPixels ? kSimWorldMapPixels - 1 : sx;
    out[i] = BlendPixel(a[sx], b[sx], sx, y);
  }
}

static void RebuildGroundRectangle(uint32_t *out, int pitch,
                                   int source_x, int source_y, int width, int height,
                                   const uint32_t *developed, int developed_pitch,
                                   const uint32_t *baseline, int baseline_pitch) {
  /* Scale2x needs only three source rows and a one-pixel halo. Keeping them
   * in a bounded rolling window replaces the full bake's 4 MiB scratch
   * allocation and avoids rebuilding the same halo for adjacent dirty cells.
   * This is the original pixel rule, not a different filter or resolution. */
  uint32_t rows[3][kSimWorldMapPixels + 2];
  uint32_t *north = rows[0], *row = rows[1], *south = rows[2];
  BlendSourceRow(north, source_x, source_y - 1, width,
      developed, developed_pitch, baseline, baseline_pitch);
  BlendSourceRow(row, source_x, source_y, width,
      developed, developed_pitch, baseline, baseline_pitch);
  for (int y = 0; y < height; y++) {
    BlendSourceRow(south, source_x, source_y + y + 1, width,
        developed, developed_pitch, baseline, baseline_pitch);
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
    uint32_t *reuse = north; north = row; row = south; south = reuse;
  }
}

bool SimWorldNavigationArt_Build(
    uint32_t *out_pixels, int out_pitch_pixels,
    const uint32_t *developed_pixels, int developed_pitch_pixels,
    const uint32_t *baseline_pixels, int baseline_pitch_pixels) {
  if (!out_pixels || out_pitch_pixels < kSimWorldNavigationArtPixels ||
      !developed_pixels || developed_pitch_pixels < kSimWorldMapPixels ||
      !baseline_pixels || baseline_pitch_pixels < kSimWorldMapPixels)
    return false;
  RebuildGroundRectangle(out_pixels, out_pitch_pixels, 0, 0,
      kSimWorldMapPixels, kSimWorldMapPixels,
      developed_pixels, developed_pitch_pixels, baseline_pixels, baseline_pitch_pixels);
  return true;
}

static uint32_t BlendGround(uint32_t a, uint32_t b, unsigned weight) {
  if (weight >= 256) return b;
  if (!weight || a == b) return a;
  uint32_t out = UINT32_C(0xFF000000);
  for (int shift = 0; shift < 24; shift += 8)
    out |= (((((a >> shift) & 255u) * (256u - weight) +
              ((b >> shift) & 255u) * weight + 128u) >> 8) << shift);
  return out;
}

static bool GroundTile(const SimWorldNavigationTownGround *ground, uint8_t town,
                       int x, int y, bool models, bool cliffs, uint8_t *tile) {
  *tile = ground->terrain[town - 1][y * kSimTownCells + x];
  if (SimBackgroundMountains_TileFlags(town, *tile) ||
      (!cliffs && SimTownTerrain_IsFaceCell(town, x, y))) return false;
  if (ground->object_rows[town - 1][y] & (UINT32_C(1) << x)) {
    if (!models) return false;
    *tile = *tile == 0xE1 ? 0x41 : *tile == 0xE2 ? 0x3A : 0x08;
  } else if (*tile >= 0xE0 && *tile <= 0xEF) {
    return false;
  }
  return true;
}

static void GroundFeather(unsigned feather[kSimTownCells * kSimTownCellPixels]) {
  const int pixels = kSimTownCells * kSimTownCellPixels;
  for (int i = 0; i < pixels; i++) {
    const int edge = i < pixels - 1 - i ? i : pixels - 1 - i;
    feather[i] = (unsigned)(256.0f * Smoothstep(
        (float)edge / (kSimWorldNavigationTownFeatherPixels *
                       kSimWorldNavigationArtScale)) + 0.5f);
  }
}

static void OverlayGroundCell(uint32_t *out, int pitch, const uint32_t *pixels,
                              int cx, int cy, const unsigned *feather) {
  for (int y = 0; y < kSimTownCellPixels; y++)
    for (int x = 0; x < kSimTownCellPixels; x++) {
      const uint32_t color = pixels[y * kSimTownCellPixels + x];
      if (!(color >> 24)) continue;
      const unsigned fx = feather[cx * kSimTownCellPixels + x];
      const unsigned fy = feather[cy * kSimTownCellPixels + y];
      out[y * pitch + x] = BlendGround(out[y * pitch + x], color, fx < fy ? fx : fy);
    }
}

bool SimWorldNavigationArt_OverlayTownGround(
    uint32_t *out_pixels, int out_pitch_pixels,
    const SimWorldNavigationTownGround *ground, bool models_enabled, bool cliff_geometry,
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
  enum { kTownPixels = kSimTownCells * kSimTownCellPixels };
  unsigned feather[kTownPixels];
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
        if (!GroundTile(ground, town, cx, cy, models_enabled, cliff_geometry, &tile)) continue;
        const uint32_t *pixels = SimTownGroundArt_AnimatedMetatile(
            town, ground->development_tier[town - 1], tile, animation_phase);
        uint32_t *out = out_pixels + (size_t)(origin_y + cy) * kSimTownCellPixels *
            out_pitch_pixels + (origin_x + cx) * kSimTownCellPixels;
        OverlayGroundCell(out, out_pitch_pixels, pixels, cx, cy, feather);
      }
  }
  return true;
}

bool SimWorldNavigationArt_UpdateAnimation(
    uint32_t *out_pixels, int out_pitch_pixels,
    const uint32_t *developed_pixels, int developed_pitch_pixels,
    const uint32_t *baseline_pixels, int baseline_pitch_pixels,
    const uint8_t *world_cells,
    const SimWorldNavigationTownGround *ground, bool models_enabled, bool cliff_geometry,
    uint8_t previous_phase, uint8_t animation_phase,
    SimWorldNavigationArtChanges *changes) {
  if (!out_pixels || out_pitch_pixels < kSimWorldNavigationArtPixels ||
      !developed_pixels || developed_pitch_pixels < kSimWorldMapPixels ||
      !baseline_pixels || baseline_pitch_pixels < kSimWorldMapPixels ||
      !changes || (ground && !SimTownGroundArt_Available()) ||
      previous_phase >= kSimTownGroundAnimationFrames ||
      animation_phase >= kSimTownGroundAnimationFrames) return false;
  /* Resolve allocations before touching the retained image. Identical/static
   * variants then cost pointer comparisons, not per-pixel work. */
  for (uint8_t town = 1; ground && town <= kSimTownCount; town++)
    if ((ground->enabled_town_mask & (1u << (town - 1))) &&
        (!SimTownGroundArt_AnimatedMetatile(town, ground->development_tier[town - 1], 8, previous_phase) ||
         !SimTownGroundArt_AnimatedMetatile(town, ground->development_tier[town - 1], 8, animation_phase)))
      return false;
  memset(changes, 0, sizeof(*changes));
  if (world_cells) memcpy(changes->cells, world_cells, sizeof(changes->cells));
  unsigned feather[kSimTownCells * kSimTownCellPixels];
  GroundFeather(feather);
  for (uint8_t town = 1; ground && town <= kSimTownCount; town++) {
    if (!(ground->enabled_town_mask & (1u << (town - 1)))) continue;
    int ox, oy;
    if (!SimWorldMap_OriginForTown(town, &ox, &oy)) continue;
    for (int y = 0; y < kSimTownCells; y++)
      for (int x = 0; x < kSimTownCells; x++) {
        uint8_t tile;
        if (!GroundTile(ground, town, x, y, models_enabled, cliff_geometry, &tile)) continue;
        const uint32_t *before = SimTownGroundArt_AnimatedMetatile(
            town, ground->development_tier[town - 1], tile, previous_phase);
        const uint32_t *after = SimTownGroundArt_AnimatedMetatile(
            town, ground->development_tier[town - 1], tile, animation_phase);
        if (before == after || !memcmp(before, after,
            kSimTownCellPixels * kSimTownCellPixels * sizeof(*after))) continue;
        changes->cells[(oy + y) * kSimWorldMapTiles + ox + x] = 1;
      }
  }
  for (int y = 0; y < kSimWorldMapTiles; y++)
    for (int x = 0; x < kSimWorldMapTiles;) {
      if (!changes->cells[y * kSimWorldMapTiles + x]) { x++; continue; }
      const int first = x;
      while (x < kSimWorldMapTiles && changes->cells[y * kSimWorldMapTiles + x]) x++;
      uint32_t *out = out_pixels + (size_t)y * kSimTownCellPixels * out_pitch_pixels + first * kSimTownCellPixels;
      RebuildGroundRectangle(out, out_pitch_pixels, first * kSimWorldMapTilePixels,
          y * kSimWorldMapTilePixels, (x - first) * kSimWorldMapTilePixels, kSimWorldMapTilePixels,
          developed_pixels, developed_pitch_pixels, baseline_pixels, baseline_pitch_pixels);
    }
  for (uint8_t town = 1; ground && town <= kSimTownCount; town++) {
    if (!(ground->enabled_town_mask & (1u << (town - 1)))) continue;
    int ox, oy;
    if (!SimWorldMap_OriginForTown(town, &ox, &oy)) continue;
    for (int y = 0; y < kSimTownCells; y++)
      for (int x = 0; x < kSimTownCells; x++) {
        if (!changes->cells[(oy + y) * kSimWorldMapTiles + ox + x]) continue;
        uint8_t tile;
        if (!GroundTile(ground, town, x, y, models_enabled, cliff_geometry, &tile)) continue;
        const uint32_t *pixels = SimTownGroundArt_AnimatedMetatile(
            town, ground->development_tier[town - 1], tile, animation_phase);
        uint32_t *out = out_pixels + (size_t)(oy + y) * kSimTownCellPixels * out_pitch_pixels +
            (ox + x) * kSimTownCellPixels;
        OverlayGroundCell(out, out_pitch_pixels, pixels, x, y, feather);
      }
  }
  return true;
}
