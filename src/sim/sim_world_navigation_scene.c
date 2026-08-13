#include "sim_world_navigation_scene.h"

#include <math.h>
#include <string.h>

#include "constants.h"
#include "sim_world_map.h"

enum {
  kWorldLocationCount = 7,
  kWorldLocationFirst = 1,
  kWorldLocationRegionPixels = kActRaiserAuthenticWidth,
  kMode7MatrixFixedPointUnit = 256,
  kHardwareMaximumBrightness = 15,
  kAlphaPerBrightnessStep = UINT8_MAX / kHardwareMaximumBrightness,
  kOamWordsPerSlot = 2,
  kOamSlotCount =
      kSimWorldNavigationOamWords / kOamWordsPerSlot,
  kOamHiddenY = 0xE0,
  /* Slot zero begins the packed location UI, so a valid Palace must leave at
   * least that one-slot prefix in front of its fixed grid. */
  kPalaceFirstSlotMinimum = 1,
  kPalaceGridColumns = 3,
  kPalaceGridRows = 3,
  kPalaceOamCount = kPalaceGridColumns * kPalaceGridRows,
  kPalaceOriginX = 104,
  kPalaceOriginY = 81,
  kPalaceCellPixels = 16,
  kPalaceAttributesHigh = 0x32,
  kPalaceOccupiedMask = (1u << kPalaceOamCount) - 1,
  kUiPriorityShift = 12,
  kUiPriorityMask = 3,
  kUiRequiredPriority = 3,
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

static bool OamSlotHidden(const uint16_t oam[kSimWorldNavigationOamWords], int slot) {
  return (oam[slot * kOamWordsPerSlot] >> 8) == kOamHiddenY;
}

static bool PalaceSignatureAt(const uint16_t oam[kSimWorldNavigationOamWords], int first) {
  if (first < kPalaceFirstSlotMinimum ||
      first > kOamSlotCount - kPalaceOamCount)
    return false;
  unsigned occupied = 0;
  for (int i = 0; i < kPalaceOamCount; i++) {
    const int word = (first + i) * kOamWordsPerSlot;
    const uint16_t position = oam[word];
    const uint16_t attributes = oam[word + 1];
    const int x = position & UINT8_MAX;
    const int y = position >> 8;
    if (x < kPalaceOriginX ||
        x > kPalaceOriginX +
                (kPalaceGridColumns - 1) * kPalaceCellPixels ||
        (x - kPalaceOriginX) % kPalaceCellPixels ||
        y < kPalaceOriginY ||
        y > kPalaceOriginY +
                (kPalaceGridRows - 1) * kPalaceCellPixels ||
        (y - kPalaceOriginY) % kPalaceCellPixels ||
        (attributes >> 8) != kPalaceAttributesHigh)
      return false;
    const unsigned cell =
        (unsigned)((y - kPalaceOriginY) / kPalaceCellPixels *
                       kPalaceGridColumns +
                   (x - kPalaceOriginX) / kPalaceCellPixels);
    if (occupied & (1u << cell)) return false;
    occupied |= 1u << cell;
  }
  /* The ROM changes tile numbers and traversal order between Palace animation
   * frames. The invariant is the complete fixed-centre 3x3 grid, one slot per
   * cell, all with the same palette/priority attributes. */
  return occupied == kPalaceOccupiedMask;
}

bool SimWorldNavigationScene_ClassifyOam(
    const uint16_t oam[kSimWorldNavigationOamWords],
    SimWorldNavigationComposition *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  if (!oam) return false;

  bool all_hidden = true;
  for (int slot = 0; slot < kOamSlotCount; slot++) {
    if (!OamSlotHidden(oam, slot)) {
      all_hidden = false;
      break;
    }
  }
  if (all_hidden) {
    out->valid = true;
    out->empty_animation = true;
    return true;
  }

  int palace_first = -1;
  for (int slot = kPalaceFirstSlotMinimum;
       slot <= kOamSlotCount - kPalaceOamCount; slot++) {
    if (PalaceSignatureAt(oam, slot)) {
      palace_first = slot;
      break;
    }
  }
  if (palace_first < kPalaceFirstSlotMinimum) return false;

  /* The location label/frame is packed immediately before the Palace. Hidden
   * holes would make one range raster include stale off-screen entries, so an
   * unexpected layout is a fallback rather than an inferred composition. */
  for (int slot = 0; slot < palace_first; slot++) {
    const uint16_t attributes =
        oam[slot * kOamWordsPerSlot + 1];
    if (OamSlotHidden(oam, slot) ||
        ((attributes >> kUiPriorityShift) & kUiPriorityMask) !=
            kUiRequiredPriority)
      return false;
  }
  for (int slot = palace_first + kPalaceOamCount;
       slot < kOamSlotCount; slot++)
    if (!OamSlotHidden(oam, slot)) return false;

  out->valid = true;
  out->ui.visible = true;
  out->ui.oam_first = 0;
  out->ui.oam_count = (uint8_t)palace_first;
  out->palace.visible = true;
  out->palace.oam_first = (uint8_t)palace_first;
  out->palace.oam_count = kPalaceOamCount;
  return true;
}
