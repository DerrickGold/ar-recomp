/* Included by a native test driver that links the production ActionTrunk
 * split and diorama_trunk_guard.c. No renderer or native game is launched. */
#include "diorama/diorama_trunk_guard.h"
#include "action/action_trunk.h"

static void TrunkGuardFixtures(void) {
  const DioramaTrunkGuardBounds world = {0, 0, 1024, 2048};
  DioramaTrunkGuardLayout layout;
  assert(DioramaTrunkGuard_Resolve(208, 1696, 624, 352, world, &layout));
  assert(layout.strips[0].world_y == 1632 && layout.strips[1].world_y == 2048);
  assert(layout.strips[0].atlas_y == 0 && layout.strips[1].atlas_y == 64);
  assert(layout.strips[0].valid.height == 64 && !layout.strips[1].valid.height);
  /* Existing audit w031 needs native row1683: it belongs to this guard,
   * never to a repeated wall or a changed display/camera rectangle. */
  assert(1683 >= layout.strips[0].world_y && 1683 < layout.primary_y);
  assert(DioramaTrunkGuard_Resolve(-57, 145, 624, 352, world, &layout));
  assert(layout.strips[0].valid.x == 57 && layout.strips[0].valid.width == 567);
  assert(layout.strips[1].world_y == 497 && layout.strips[1].valid.height == 64);
  assert(544 < layout.strips[1].world_y + layout.strips[1].valid.height);
  assert(layout.strips[0].world_y + 64 == layout.primary_y);
  assert(layout.strips[1].world_y == layout.primary_y + layout.primary_height);
  assert(DioramaTrunkGuard_Resolve(0, 0, 640, 352, world, &layout));
  assert(!layout.strips[0].valid.width && layout.strips[1].valid.width == 640);
  /* Explicit finite authored extensions remain at their own coordinates. */
  const DioramaTrunkGuardBounds authored = {-32, -16, 1088, 2080};
  assert(DioramaTrunkGuard_Resolve(-64, 0, 640, 352, authored, &layout));
  assert(layout.strips[0].valid.x == 32 && layout.strips[0].valid.y == 48 &&
      layout.strips[0].valid.height == 16);
  assert(!DioramaTrunkGuard_Resolve(0, INT_MIN, 640, 352, world, &layout));
  assert(!DioramaTrunkGuard_Resolve(0, INT_MAX - 352, 640, 352, world, &layout));
  assert(!DioramaTrunkGuard_Resolve(INT_MAX, 0, 640, 352, world, &layout));
  assert(!DioramaTrunkGuard_Resolve(0, 0, 641, 352, world, &layout));
  assert(!DioramaTrunkGuard_Resolve(0, 0, 640, 0, world, &layout));

  ActionTrunkAssets *assets = calloc(1, sizeof(*assets));
  uint32_t *input = calloc(kDioramaTrunkGuardPixels, sizeof(*input));
  uint32_t *output[3], *reference[3];
  const size_t capacities[3] = {kDioramaTrunkGuardPixels,
      kDioramaTrunkGuardPixels, kDioramaTrunkGuardPixels};
  assert(assets && input);
  assets->height = 2048; assets->side_width = 1024;
  for (unsigned band = 0; band < 3; ++band) {
    output[band] = calloc(kDioramaTrunkGuardPixels, sizeof(uint32_t));
    reference[band] = calloc(kDioramaTrunkGuardPixels, sizeof(uint32_t));
    assert(output[band] && reference[band]);
  }
  for (unsigned i = 0; i < kActionTrunkWallWidth * kActionTrunkWallHeight; ++i)
    assets->wall[i] = 0xff204060u + (i & 0x1fu);
  assert(DioramaTrunkGuard_Resolve(208, 512, 624, 352, world, &layout));
  DioramaTrunkGuardSource source = {.layout = layout, .pixels = input,
      .pitch_pixels = 640, .pixel_count = kDioramaTrunkGuardPixels,
      .capture_generation = 17};
  for (unsigned s = 0; s < 2; ++s) {
    const DioramaTrunkGuardStrip *strip = &layout.strips[s];
    for (int y = 0; y < 64; ++y) {
      const int wy = strip->world_y + y;
      for (int x = 0; x < 624; ++x) {
        const int wx = strip->world_x + x;
        const unsigned bit = 1u << (wx & 7);
        const int at = wy * kActionTrunkSideMaskPitch + wx / 8;
        input[(strip->atlas_y + y) * 640 + x] =
            x % 7 ? 0xff000000u | ((unsigned)wy << 8) | (unsigned)(wx & 255) : 0;
        if (wx < 320 || wx >= 704) {
          if (x % 4 == 0) assets->side_masks[kActionTrunkSideSolid][at] |= bit;
          if (x % 4 == 1) assets->side_masks[kActionTrunkSideRear][at] |= bit;
          if (x % 4 == 2) assets->side_masks[kActionTrunkSidePriority][at] |= bit;
        } else {
          if (x % 4 == 0) assets->masks[0][wy * 48 + (wx - 320) / 8] |= bit;
          if (x % 4 == 1) assets->masks[2][wy * 48 + (wx - 320) / 8] |= bit;
        }
      }
    }
  }
  for (unsigned fade = 0; fade <= 15; fade += 15) {
    assert(DioramaTrunkGuard_Split(assets, &source, output, capacities, fade));
    for (unsigned s = 0; s < 2; ++s) {
      const DioramaTrunkGuardStrip *strip = &layout.strips[s];
      const uint32_t *raw[3] = {input + strip->atlas_y * 640, NULL, NULL};
      const size_t pitches[3] = {640, 0, 0};
      uint32_t *part[3];
      for (unsigned b = 0; b < 3; ++b) part[b] = reference[b] + strip->atlas_y * 640;
      ActionTrunk_Split(assets, strip->world_x, strip->world_y, 624, 64, 640,
          raw, pitches, part, fade);
    }
    for (unsigned b = 0; b < 3; ++b)
      assert(!memcmp(output[b], reference[b], kDioramaTrunkGuardPixels * sizeof(uint32_t)));
  }
  /* Input alpha/unknown detail/native high are independent of the new
   * storage layout. Exercise each with direct emitted split witnesses. */
  assert(!output[0][0] && !output[1][0] && !output[2][0]);
  assert(output[1][2] == input[2] && !output[0][2]);
  assert(output[0][3] == input[3] && !output[1][3] && !output[2][3]);
  assert(output[0][4] == input[4] && output[2][4] == input[4]);
  assert(!output[0][5] && output[2][5] == input[5]);
  source.layout.strips[1].atlas_y = 0;
  const uint32_t before = output[1][2];
  assert(!DioramaTrunkGuard_Split(assets, &source, output, capacities, 15));
  assert(output[1][2] == before);
  source.layout = layout;
  uint32_t *alias[3] = {input, output[1], output[2]};
  assert(!DioramaTrunkGuard_Split(assets, &source, alias, capacities, 15));
  alias[0] = output[0]; alias[1] = output[0] + 1;
  assert(!DioramaTrunkGuard_Split(assets, &source, alias, capacities, 15));
  source.pixel_count = 1;
  assert(!DioramaTrunkGuard_Split(assets, &source, output, capacities, 15));
  source.pixel_count = kDioramaTrunkGuardPixels;
  assert(DioramaTrunkGuard_Resolve(208, 1696, 624, 352, world, &source.layout));
  assert(DioramaTrunkGuard_Split(assets, &source, output, capacities, 15));
  for (unsigned b = 0; b < 3; ++b)
    for (int i = 64 * 640; i < kDioramaTrunkGuardPixels; ++i) assert(!output[b][i]);
  /* A valid empty result must clear stale native art, including core Far. */
  assert(DioramaTrunkGuard_Resolve(208, 512, 624, 352,
      (DioramaTrunkGuardBounds){0, 512, 1024, 352}, &source.layout));
  assert(DioramaTrunkGuard_Split(assets, &source, output, capacities, 15));
  for (unsigned b = 0; b < 3; ++b) {
    for (int i = 0; i < kDioramaTrunkGuardPixels; ++i) assert(!output[b][i]);
    free(output[b]); free(reference[b]);
  }
  free(input); free(assets);
  puts("trunk guard layout/split: finite bounds, native priority, source alpha, empty guards and storage validation passed");
}
