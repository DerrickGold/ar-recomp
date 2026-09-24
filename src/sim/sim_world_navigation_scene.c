#include "sim_world_navigation_scene.h"

#include <math.h>
#include <string.h>

#include "constants.h"
#include "actraiser_game.h"
#include "sim_world_map.h"

enum {
  kWorldLocationCount = 7,
  kWorldLocationFirst = 1,
  kWorldLocationRegionPixels = kActRaiserAuthenticWidth,
  kMode7MatrixFixedPointUnit = 256,
  kHardwareMaximumBrightness = 15,
  kAlphaPerBrightnessStep = UINT8_MAX / kHardwareMaximumBrightness,
};

static const float kCameraAltitudePerZoomUnit = 0.25f;
static const float kCloudCrossingBandPixels = 32.0f;

typedef struct WorldLocationRegion {
  uint16_t x, y;
} WorldLocationRegion;

/* ROM $01:B73C, converted to the top-left of each 256x256 source-pixel
 * selection region. Keep this table pinned to the original label selector:
 * it is the authority for which land the game says is current. */
static const WorldLocationRegion kWorldLocationRegions[kWorldLocationCount] = {
  {640, 384},  /* Fillmore */
  {384, 384},  /* Bloodpool */
  {128, 512},  /* Kasandora */
  {128, 256},  /* Aitos */
  {512, 768},  /* Marahna */
  {256,   0},  /* Northwall */
  {640,   0},  /* Death Heim */
};

bool SimWorldNavigationScene_Build(
    SimWorldNavigationScene *out,
    const SimWorldNavigationFrame *navigation,
    uint32_t developed_texture_serial) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  if (!navigation || !developed_texture_serial) return false;

  /* Mode 7 maps a screen delta to a source-texture delta through the signed
   * 8.8 A/B/C/D matrix. Invert that 2x2 transform during capture so
   * presentation receives a direct source-to-screen affine map.
   *
   * Keep the determinant in 64 bits. The inputs are signed 16-bit and the
   * cross products can span nearly the entire signed 32-bit range before
   * subtraction. */
  const int64_t a = navigation->matrix[0];
  const int64_t b = navigation->matrix[1];
  const int64_t c = navigation->matrix[2];
  const int64_t d = navigation->matrix[3];
  const int64_t determinant = a * d - b * c;
  if (!determinant) return false;

  const double inverse_scale =
      (double)kMode7MatrixFixedPointUnit / (double)determinant;
  const double m00 = (double)d * inverse_scale;
  const double m01 = (double)-b * inverse_scale;
  const double m10 = (double)-c * inverse_scale;
  const double m11 = (double)a * inverse_scale;
  const double tx = (double)(kActRaiserAuthenticWidth / 2) -
      m00 * navigation->focus_x - m01 * navigation->focus_y;
  const double ty = (double)(kActRaiserAuthenticHeight / 2) -
      m10 * navigation->focus_x - m11 * navigation->focus_y;
  const double affine[kSimWorldNavigationAffineComponentCount] = {
    m00, m01, tx, m10, m11, ty,
  };
  for (int i = 0; i < kSimWorldNavigationAffineComponentCount; i++) {
    if (!isfinite(affine[i])) return false;
    out->source_to_screen[i] = (float)affine[i];
  }

  out->texture_serial = developed_texture_serial;
  out->texture_width = kSimWorldMapPixels;
  out->texture_height = kSimWorldMapPixels;
  out->tile_width = kSimWorldMapTiles;
  out->tile_height = kSimWorldMapTiles;
  out->ground[0] = (SimWorldNavigationGroundVertex){0, 0, 0.0f, 0.0f};
  out->ground[1] = (SimWorldNavigationGroundVertex){
      kSimWorldMapTiles, 0, 1.0f, 0.0f};
  out->ground[2] = (SimWorldNavigationGroundVertex){
      kSimWorldMapTiles, kSimWorldMapTiles, 1.0f, 1.0f};
  out->ground[3] = (SimWorldNavigationGroundVertex){
      0, kSimWorldMapTiles, 0.0f, 1.0f};
  out->active_location = navigation->active_location;
  if (navigation->active_location >= kWorldLocationFirst &&
      navigation->active_location <
          kWorldLocationFirst + kWorldLocationCount) {
    const WorldLocationRegion *region =
        &kWorldLocationRegions[
            navigation->active_location - kWorldLocationFirst];
    out->active_region_valid = true;
    out->active_region_x = region->x;
    out->active_region_y = region->y;
    out->active_region_width = kWorldLocationRegionPixels;
    out->active_region_height = kWorldLocationRegionPixels;
  }
  out->valid = true;
  return true;
}

bool SimWorldNavigationScene_BuildSkyPalace(
    SimWorldNavigationScene *out, uint16_t focus_x, uint16_t focus_y,
    uint16_t active_location, uint32_t developed_texture_serial) {
  if (!out) return false;
  *out = (SimWorldNavigationScene){0};
  if (focus_x >= kSimWorldMapPixels || focus_y >= kSimWorldMapPixels) return false;
  /* An authored unit chart, not a replacement for the captured travel state.
   * Presentation selects the Palace camera independently of this affine. */
  const SimWorldNavigationFrame chart = {
    .focus_x = focus_x, .focus_y = focus_y,
    .matrix = {kMode7MatrixFixedPointUnit, 0, 0, kMode7MatrixFixedPointUnit},
    .active_location = active_location,
  };
  return SimWorldNavigationScene_Build(out, &chart, developed_texture_serial);
}

bool SimWorldNavigationScene_ProjectSource(
    const SimWorldNavigationScene *scene,
    float source_x, float source_y,
    float *screen_x, float *screen_y) {
  if (!scene || !scene->valid || !screen_x || !screen_y) return false;
  const float x = scene->source_to_screen[0] * source_x +
      scene->source_to_screen[1] * source_y +
      scene->source_to_screen[2];
  const float y = scene->source_to_screen[3] * source_x +
      scene->source_to_screen[4] * source_y +
      scene->source_to_screen[5];
  if (!isfinite(x) || !isfinite(y)) return false;
  *screen_x = x;
  *screen_y = y;
  return true;
}

float SimWorldNavigationScene_CloudVisibility(
    uint16_t zoom_current, uint16_t cloud_altitude_px) {
  if (!cloud_altitude_px) return 1.0f;

  /* The three authentic steady states are near=$0206, middle=$040A and
   * far=$0562. Treat one quarter of their zoom delta as camera altitude in
   * the same original-pixel vocabulary as the existing cloud setting. */
  float camera_altitude =
      ((float)zoom_current - (float)kSimWorldNavigationZoomNear) *
      kCameraAltitudePerZoomUnit;
  if (camera_altitude < 0.0f) camera_altitude = 0.0f;

  /* A 32px crossing band keeps the scripted zoom/rotation event continuous:
   * the cloud bodies do not pop on the single frame that crosses the deck. */
  const float half_band = kCloudCrossingBandPixels * 0.5f;
  float t = (camera_altitude - ((float)cloud_altitude_px - half_band)) /
      (half_band * 2.0f);
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  return t * t * (3.0f - 2.0f * t);
}

float SimWorldNavigationScene_AdventScale(float flat_scale, float maximum_scale) {
  if (!isfinite(flat_scale) || flat_scale <= 0 ||
      !isfinite(maximum_scale) || maximum_scale <= 0) return 0;
  const float start = maximum_scale * .5f;
  if (flat_scale <= start) return flat_scale;
  /* $02:849B subtracts four zoom units each native tick; $02:84A2 keeps
   * that motion running through the fifteen brightness steps. Flat scale
   * is reciprocal zoom. An exponential clamp exhausts the approach early;
   * this reciprocal continuation keeps finite motion through the last
   * visible tick. At start, both scale and d(scale)/d(flat_scale) match.
   * Divide first to avoid squaring large scales. No retained clock/state. */
  return maximum_scale - start * (start / flat_scale);
}

SimWorldNavigationAtmosphereHeights SimWorldNavigationScene_AtmosphereHeights(
    float maximum_terrain_tiles, uint16_t cloud_altitude_px) {
  if (!isfinite(maximum_terrain_tiles) || maximum_terrain_tiles < 0.0f)
    maximum_terrain_tiles = 0.0f;
  /* Preserve the town cloud-altitude dial's world-navigation scale, but
   * measure its clearance above the tallest peak rather than a fixed plain.
   * A small minimum also keeps the zero-altitude setting off the terrain. */
  const float cloud_clearance = fmaxf(
      0.35f, (float)cloud_altitude_px / (float)(kSimTownCellPixels * 8));
  const float cloud_tiles = maximum_terrain_tiles + cloud_clearance;
  return (SimWorldNavigationAtmosphereHeights){
    .cloud_tiles = cloud_tiles,
    .outer_tiles = cloud_tiles + 0.75f,
  };
}

float SimWorldNavigationScene_AtmosphereOpacity(float height_fraction) {
  if (!isfinite(height_fraction) || height_fraction >= 1) return 0;
  const float height = fmaxf(0, height_fraction);
  const float remaining = 1 - height;
  /* A denser inner halo and a zero-slope outer tail, not a uniformly tinted
   * glass shell. This inexpensive artistic profile is not a scattering LUT. */
  return .32f * expf(-1.5f * height) * remaining * remaining;
}

float SimWorldNavigationScene_CloudLimbOpacity(float facing) {
  if (!isfinite(facing) || facing <= 0) return 0;
  const float t = fminf(1, facing / .5f);
  return t * t * (3 - 2 * t);
}

uint8_t SimWorldNavigationScene_MasterFadeAlpha(uint8_t brightness) {
  if (brightness > kHardwareMaximumBrightness)
    brightness = kHardwareMaximumBrightness;
  /* 255 / 15 is exactly 17, so every hardware brightness step maps to an
   * exact 8-bit blend step with no rounding drift at either endpoint. */
  return (uint8_t)((kHardwareMaximumBrightness - brightness) *
                   kAlphaPerBrightnessStep);
}

float SimWorldNavigationScene_LocationHaze(
    const SimWorldNavigationScene *scene,
    float source_x, float source_y, float lead) {
  if (!scene) return 0.0f;
  if (!scene->active_region_valid) return 1.0f;
  if (lead <= 0.0f) return 0.0f;
  const float x0 = scene->active_region_x;
  const float y0 = scene->active_region_y;
  const float x1 = x0 + scene->active_region_width;
  const float y1 = y0 + scene->active_region_height;
  float dx = source_x < x0 ? x0 - source_x
      : source_x > x1 ? source_x - x1 : 0.0f;
  float dy = source_y < y0 ? y0 - source_y
      : source_y > y1 ? source_y - y1 : 0.0f;
  /* Euclidean distance rounds the four corners instead of revealing the
   * selector table as a literal axis-aligned box. */
  float t = sqrtf(dx * dx + dy * dy) / lead;
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  return t * t * (3.0f - 2.0f * t);
}

bool SimWorldNavigationScene_FromOwnership(
    const ActRaiserSpriteOwnership *ownership,
    SimWorldNavigationComposition *out) {
  if (!out) return false;
  *out = (SimWorldNavigationComposition){0};
  if (!ownership || !ownership->valid || ownership->unowned_emitted ||
      ownership->group != kActRaiserMapGroup_NonAction ||
      ownership->map != kActRaiserNonActionMap_WorldMap)
    return false;
  SimWorldNavigationComposition result = {.valid = true,
      .label_location = ownership->location};
  SimWorldNavigationCompositionLayer *layers[] = {
    &result.label, &result.plaque, &result.palace,
  };
  const ActRaiserSpriteRole roles[] = {
    kActRaiserSprite_WorldLabel, kActRaiserSprite_WorldPlaque,
    kActRaiserSprite_WorldPalace,
  };
  bool any = false;
  for (unsigned i = 0; i < 3; ++i) {
    layers[i]->visible = ActRaiserSpriteOwnership_Range(ownership, roles[i],
        &layers[i]->oam_first, &layers[i]->oam_count);
    bool emitted = false;
    for (unsigned slot = 0; slot < kActRaiserSpriteSlots; ++slot)
      emitted |= ownership->slots[slot] == roles[i];
    if (emitted && !layers[i]->visible) return false;
    any |= emitted;
  }
  if (any && (!result.palace.visible || !result.plaque.visible)) return false;
  result.empty_animation = !any;
  *out = result;
  return true;
}
