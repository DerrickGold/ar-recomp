#include "sim_world_navigation_scene.h"

#include <math.h>
#include <string.h>

#include "sim_world_map.h"

enum {
  kAuthenticScreenCentreX = 128,
  kAuthenticScreenCentreY = 112,
  kWorldLocationCount = 7,
};

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
   * 8.8 A/B/C/D matrix. Invert that 2x2 transform once on the game thread;
   * the render thread then receives a direct source-to-screen affine map.
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

  const double inverse_scale = 256.0 / (double)determinant;
  const double m00 = (double)d * inverse_scale;
  const double m01 = (double)-b * inverse_scale;
  const double m10 = (double)-c * inverse_scale;
  const double m11 = (double)a * inverse_scale;
  const double tx = (double)kAuthenticScreenCentreX -
      m00 * navigation->focus_x - m01 * navigation->focus_y;
  const double ty = (double)kAuthenticScreenCentreY -
      m10 * navigation->focus_x - m11 * navigation->focus_y;
  const double affine[6] = {m00, m01, tx, m10, m11, ty};
  for (int i = 0; i < 6; i++) {
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
  if (navigation->active_location >= 1 &&
      navigation->active_location <= kWorldLocationCount) {
    const WorldLocationRegion *region =
        &kWorldLocationRegions[navigation->active_location - 1];
    out->active_region_valid = true;
    out->active_region_x = region->x;
    out->active_region_y = region->y;
    out->active_region_width = 256;
    out->active_region_height = 256;
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
      ((float)zoom_current - (float)kSimWorldNavigationZoomNear) * 0.25f;
  if (camera_altitude < 0.0f) camera_altitude = 0.0f;

  /* A 32px crossing band keeps the scripted zoom/rotation event continuous:
   * the cloud bodies do not pop on the single frame that crosses the deck. */
  const float half_band = 16.0f;
  float t = (camera_altitude - ((float)cloud_altitude_px - half_band)) /
      (half_band * 2.0f);
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  return t * t * (3.0f - 2.0f * t);
}

uint8_t SimWorldNavigationScene_MasterFadeAlpha(uint8_t brightness) {
  if (brightness > 15) brightness = 15;
  /* 255 / 15 is exactly 17, so every hardware brightness step maps to an
   * exact 8-bit blend step with no rounding drift at either endpoint. */
  return (uint8_t)((15 - brightness) * 17);
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

static bool OamSlotHidden(const uint16_t oam[256], int slot) {
  return (oam[slot * 2] >> 8) == 0xE0;
}

static bool PalaceSignatureAt(const uint16_t oam[256], int first) {
  if (first < 1 || first > 128 - 9) return false;
  unsigned occupied = 0;
  for (int i = 0; i < 9; i++) {
    const uint16_t position = oam[(first + i) * 2];
    const uint16_t attributes = oam[(first + i) * 2 + 1];
    const int x = position & 0xFF;
    const int y = position >> 8;
    if (x < 104 || x > 136 || (x - 104) % 16 ||
        y < 81 || y > 113 || (y - 81) % 16 ||
        (attributes >> 8) != 0x32)
      return false;
    const unsigned cell =
        (unsigned)((y - 81) / 16 * 3 + (x - 104) / 16);
    if (occupied & (1u << cell)) return false;
    occupied |= 1u << cell;
  }
  /* The ROM changes tile numbers and traversal order between Palace animation
   * frames. The invariant is the complete fixed-centre 3x3 grid, one slot per
   * cell, all with the same palette/priority attributes. */
  return occupied == 0x1FFu;
}

bool SimWorldNavigationScene_ClassifyOam(
    const uint16_t oam[256],
    SimWorldNavigationComposition *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  if (!oam) return false;

  bool all_hidden = true;
  for (int slot = 0; slot < 128; slot++) {
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
  for (int slot = 1; slot <= 128 - 9; slot++) {
    if (PalaceSignatureAt(oam, slot)) {
      palace_first = slot;
      break;
    }
  }
  if (palace_first < 1) return false;

  /* The location label/frame is packed immediately before the Palace. Hidden
   * holes would make one range raster include stale off-screen entries, so an
   * unexpected layout is a fallback rather than an inferred composition. */
  for (int slot = 0; slot < palace_first; slot++) {
    const uint16_t attributes = oam[slot * 2 + 1];
    if (OamSlotHidden(oam, slot) || ((attributes >> 12) & 3) != 3)
      return false;
  }
  for (int slot = palace_first + 9; slot < 128; slot++)
    if (!OamSlotHidden(oam, slot)) return false;

  out->valid = true;
  out->ui.visible = true;
  out->ui.oam_first = 0;
  out->ui.oam_count = (uint8_t)palace_first;
  out->palace.visible = true;
  out->palace.oam_first = (uint8_t)palace_first;
  out->palace.oam_count = 9;
  return true;
}
