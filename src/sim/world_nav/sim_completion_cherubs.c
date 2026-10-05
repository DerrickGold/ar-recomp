#include "sim/world_nav/sim_completion_cherubs.h"

#include <string.h>

static uint32_t s_pixels[kSimCompletionCherubAtlasWidth * kSimCompletionCherubAtlasHeight];
static uint32_t s_revision;

bool SimCompletionCherubs_Init(const uint8_t *rom, size_t bytes) {
  memset(s_pixels, 0, sizeof(s_pixels));
  s_revision = 0;
  /* Town asset scripts load raw $0D:8000 into the first OBJ character page.
   * $01:A627/$A63C/$A651 are the SIM angel's three frontal wing poses. */
  const size_t characters = 0x068000;
  static const unsigned poses[kSimCompletionCherubFrames] = {0xA627, 0xA63C, 0xA651};
  if (!rom || bytes < characters + 512 * 32) return false;
  uint32_t pixels[kSimCompletionCherubAtlasWidth * kSimCompletionCherubAtlasHeight] = {0};
  for (unsigned frame = 0; frame < kSimCompletionCherubFrames; frame++) {
    const unsigned pose = poses[frame];
    if (rom[pose] != 4) return false;
    unsigned coverage = 0;
    for (unsigned part = 0; part < 4; part++) {
      const uint8_t *p = rom + pose + 1 + part * 5;
      const unsigned attribute = p[3] | (unsigned)p[4] << 8;
      /* Four 8x8 parts; the moving wings extend two rows below the first
       * pose's 16px footprint. Preserve their native animation offsets. */
      if (p[0] != 0 || p[1] > kSimCompletionCherubWidth - 8 ||
          p[2] > kSimCompletionCherubHeight - 8)
        return false;
      const uint8_t *tile = rom + characters + (attribute & 511) * 32;
      for (unsigned y = 0; y < 8; y++)
        for (unsigned x = 0; x < 8; x++) {
          const unsigned sy = attribute & 0x8000 ? 7 - y : y;
          const unsigned bit = attribute & 0x4000 ? x : 7 - x;
          const unsigned ink =
              ((tile[sy * 2] | tile[sy * 2 + 1] | tile[sy * 2 + 16] | tile[sy * 2 + 17]) >> bit) &
              1;
          if (!ink) continue;
          const unsigned at = (p[2] + y) * kSimCompletionCherubAtlasWidth +
                              frame * kSimCompletionCherubWidth + p[1] + x;
          if (!pixels[at]) coverage++;
          pixels[at] = UINT32_C(0xffffffff);
        }
    }
    if (!coverage) return false;
  }
  uint32_t hash = UINT32_C(2166136261);
  for (unsigned i = 0; i < sizeof(pixels) / sizeof(pixels[0]); i++)
    hash = (hash ^ pixels[i]) * UINT32_C(16777619);
  memcpy(s_pixels, pixels, sizeof(pixels));
  s_revision = hash ? hash : 1;
  return true;
}

bool SimCompletionCherubs_Copy(uint32_t *pixels, uint32_t *revision) {
  if (!s_revision || !pixels || !revision) return false;
  memcpy(pixels, s_pixels, sizeof(s_pixels));
  *revision = s_revision;
  return true;
}
