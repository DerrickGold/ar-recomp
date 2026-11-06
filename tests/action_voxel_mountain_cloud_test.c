/* Foreground cloud artwork shares a physical face across native priority
 * bands. Optional room exports verify the semantic family in every region;
 * the ordinary test needs no ROM and checks the low-band split contract. */
#include "support/test_assert.h"
#include "action/action_voxel_internal.h"
#include "action/action_scene_snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint64_t kCloudKeys[] = {
  UINT64_C(0x2aea48375a15bd07), UINT64_C(0x815f2e81389f8fe8),
  UINT64_C(0xa0386d40f5e979a1), UINT64_C(0xc12938c78acd55c6),
  UINT64_C(0xce280f834f4658a8), UINT64_C(0xe63716e38b1ed90b),
};
static const uint64_t kMixedKeys[] = {
  UINT64_C(0x65e761d1da43126d), UINT64_C(0x7616376a189c2dcd),
  UINT64_C(0x85b46143938db23a), UINT64_C(0xd2323e327c173266),
  UINT64_C(0xd8bfabbb42410938), UINT64_C(0xfc11e71402edc8ec),
};
enum { kCloudOnly = 1, kCloudAndRock = 2, kInputPitch = 19, kOutputPitch = 21 };

static unsigned Ink(const ActionRoomScene *scene, unsigned word, unsigned x, unsigned y) {
  if (word & 0x4000) x = 7 - x;
  if (word & 0x8000) y = 7 - y;
  const unsigned character = word & 1023, bit = 7 - x;
  assert(character < sizeof(scene->characters) / 32);
  const uint8_t *p = scene->characters + character * 32;
  return ((p[y * 2] >> bit) & 1u) | (((p[y * 2 + 1] >> bit) & 1u) << 1) |
      (((p[y * 2 + 16] >> bit) & 1u) << 2) | (((p[y * 2 + 17] >> bit) & 1u) << 3);
}

/* Match reviewed artwork, never the ROM's metatile or character numbering.
 * Priority/collision/palette belong to the source identity, not the expected
 * output classification. All four displayed flips have the same contract. */
static unsigned Contract(const ActionRoomScene *scene, const uint16_t words[4],
                         unsigned collision, unsigned *key_index) {
  for (unsigned flip = 0; flip < 4; ++flip) {
    uint64_t key = UINT64_C(14695981039346656037);
#define HASH_BYTE(value) (key = (key ^ (uint8_t)(value)) * UINT64_C(1099511628211))
    for (unsigned q = 0; q < 4; ++q) {
      const unsigned from = q ^ flip, word = words[from];
      HASH_BYTE((word >> 10) & 7); HASH_BYTE((collision >> from) & 1);
      HASH_BYTE(!!(word & 0x2000));
      for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x)
        HASH_BYTE(Ink(scene, word, flip & 1 ? 7 - x : x, flip & 2 ? 7 - y : y));
    }
#undef HASH_BYTE
    for (unsigned i = 0; i < sizeof(kCloudKeys) / sizeof(kCloudKeys[0]); ++i)
      if (key == kCloudKeys[i]) { if (key_index) *key_index = i; return kCloudOnly; }
    for (unsigned i = 0; i < sizeof(kMixedKeys) / sizeof(kMixedKeys[0]); ++i)
      if (key == kMixedKeys[i]) { if (key_index) *key_index = i + 6; return kCloudAndRock; }
  }
  return 0;
}

static uint32_t NativeColor(const ActionRoomScene *scene, unsigned word, unsigned ink) {
  if (!ink) return 0;
  const unsigned index = ((word >> 10) & 7) * 16 + ink;
  const unsigned color = scene->palette[index * 2] | scene->palette[index * 2 + 1] << 8;
  uint32_t result = 0xff000000u;
  for (unsigned channel = 0; channel < 3; ++channel) {
    const unsigned value = (color >> (channel * 5)) & 31;
    result |= ((value << 3) | (value >> 2)) << (16 - channel * 8);
  }
  return result;
}

static unsigned Collision(const ActionRoomScene *scene, unsigned id) {
  unsigned collision = 0;
  for (unsigned q = 0; q < 4; ++q)
    collision |= !!(scene->bg[0].metatiles[id * 8 + q * 2] & 2) << q;
  return collision;
}

static void Renumber(ActionRoomScene *scene) {
  uint8_t definitions[sizeof(scene->bg[0].metatiles)], characters[sizeof(scene->characters)];
  memcpy(characters, scene->characters, sizeof(characters));
  for (unsigned bank = 0; bank < 2; ++bank) for (unsigned i = 0; i < 256; ++i)
    memcpy(scene->characters + (bank * 256 + ((i + 37) & 255)) * 32,
           characters + (bank * 256 + i) * 32, 32);
  for (unsigned bg = 0; bg < 2; ++bg) {
    memcpy(definitions, scene->bg[bg].metatiles, sizeof(definitions));
    for (unsigned i = 0; i < 256; ++i) {
      const unsigned id = (i + 73) & 255;
      memcpy(scene->bg[bg].metatiles + id * 8, definitions + i * 8, 8);
      for (unsigned q = 0; q < 4; ++q)
        scene->bg[bg].metatiles[id * 8 + q * 2 + 1] =
            (definitions[i * 8 + q * 2 + 1] + 37) & 255;
    }
    for (size_t i = 0; i < scene->bg[bg].map_size; ++i)
      scene->bg[bg].map[i] = (scene->bg[bg].map[i] + 73) & 255;
  }
}

static void SplitContract(void) {
  ActionVoxelGeometry *g = ActionVoxel_Create(); assert(g);
  g->ready = true; g->profile = kActionVoxelProfile_AitosMountains;
  g->width = 16; g->height = 16; g->front = calloc(256, 1); assert(g->front);
  uint32_t low[256], rear[256], front[256];
  for (unsigned i = 0; i < 256; ++i) {
    const bool high = (i & 8) != 0;
    g->front[i] = kVoxelFoliage | (high ? kVoxelHighPriority : 0);
    low[i] = high ? 0 : 0xff123400u + i;
  }
  const ActionRoomSceneFrameState frame = {.brightness = 15};
  assert(ActionVoxel_Split(g, 0, 0, 16, 16, 16, &frame, low, 16, rear, front));
  for (unsigned i = 0; i < 256; ++i) assert(!rear[i] && front[i] == low[i]);
  ActionVoxel_Release(g);
}

typedef struct CloudCounts {
  unsigned cells, mixed, low, high, rock, keys[12];
  int first_x[2], first_y[2];
} CloudCounts;

static CloudCounts CheckClouds(const ActionRoomScene *scene, const ActionVoxelGeometry *g) {
  CloudCounts counts = {.first_x = {-1, -1}, .first_y = {-1, -1}};
  uint8_t *cloud_pixels = calloc((size_t)g->width, g->height); assert(cloud_pixels);
  const unsigned columns = ActionRoomScene_TileWidth(scene, 1) / 2;
  const unsigned rows = ActionRoomScene_TileHeight(scene, 1) / 2;
  const ActionRoomSceneFrameState frame = {.brightness = 15};
  for (unsigned cy = 0; cy < rows; ++cy) for (unsigned cx = 0; cx < columns; ++cx) {
    uint16_t words[4]; uint8_t id = 0;
    for (unsigned q = 0; q < 4; ++q)
      assert(ActionRoomScene_LookupTile(scene, 1, cx * 2 + (q & 1), cy * 2 + (q >> 1), &words[q], &id));
    const unsigned collision = Collision(scene, id);
    unsigned key_index = 0;
    const unsigned contract = Contract(scene, words, collision, &key_index);
    if (!contract) continue;
    ++counts.cells; ++counts.keys[key_index]; counts.mixed += contract == kCloudAndRock;
    if (counts.first_x[contract - 1] < 0) {
      counts.first_x[contract - 1] = (int)cx; counts.first_y[contract - 1] = (int)cy;
    }
    uint32_t low[kInputPitch * 16], rear[kOutputPitch * 16], front[kOutputPitch * 16];
    uint32_t expected_rear[kOutputPitch * 16], expected_front[kOutputPitch * 16];
    for (unsigned i = 0; i < kInputPitch * 16; ++i) low[i] = 0x5abcde12u;
    for (unsigned i = 0; i < kOutputPitch * 16; ++i)
      rear[i] = front[i] = expected_rear[i] = expected_front[i] = 0xa5bd4321u;
    for (unsigned y = 0; y < 16; ++y) for (unsigned x = 0; x < 16; ++x) {
      const unsigned q = (y / 8) * 2 + x / 8, word = words[q];
      const unsigned ink = Ink(scene, word, x & 7, y & 7);
      const bool high = (word & 0x2000) != 0, solid = (collision & (1u << q)) != 0;
      const bool cloud = contract == kCloudOnly || (112u & (1u << ink));
      const unsigned wx = cx * 16 + x, wy = cy * 16 + y;
      assert((int)wx >= g->origin_x && (int)wy >= g->origin_y);
      const size_t at = (size_t)((int)wy - g->origin_y) * g->width + (int)wx - g->origin_x;
      const unsigned material = g->front[at];
      if (ink) {
        const unsigned expected_kind = cloud ? kVoxelFoliage : high && !solid ? kVoxelForeground : kVoxelSolid;
        if ((material & kVoxelKindMask) != expected_kind)
          fprintf(stderr, "cloud contract key=%u world=%u,%u ink=%u native_high=%u collision=%u kind=%u expected=%u\n",
              key_index, wx, wy, ink, high, solid, material & kVoxelKindMask, expected_kind);
        assert((material & kVoxelKindMask) == expected_kind);
        assert(!!(material & kVoxelHighPriority) == high);
        assert(!!(material & kVoxelColliding) == solid);
        if (cloud) {
          cloud_pixels[at] = 1; counts.high += high; counts.low += !high;
          assert(ActionVoxel_FrontRecess(g, wx / 8, wy / 8) == 0);
        } else ++counts.rock;
      }
      const uint32_t source = high ? 0 : NativeColor(scene, word, ink);
      low[y * kInputPitch + x] = source;
      expected_front[y * kOutputPitch + x] = source;
      /* Noncolliding high rock has no low-band source. Ordinary solid rock
       * retains its rear copy, whereas the face-only cloud has exactly one. */
      expected_rear[y * kOutputPitch + x] = cloud ? 0 : source;
    }
    uint32_t saved[sizeof(low) / sizeof(low[0])]; memcpy(saved, low, sizeof(low));
    assert(ActionVoxel_Split(g, cx * 16, cy * 16, 16, 16, kOutputPitch, &frame,
                            low, kInputPitch, rear, front));
    assert(!memcmp(low, saved, sizeof(low)));
    assert(!memcmp(front, expected_front, sizeof(front)));
    assert(!memcmp(rear, expected_rear, sizeof(rear)));
  }
  assert(counts.cells && counts.mixed && counts.low && counts.high && counts.rock);
  for (unsigned i = 0; i < g->edge_count; ++i) {
    const VoxelEdge *edge = &g->edges[i];
    for (int pixel = 0; pixel < edge->length; ++pixel) {
      const int x = edge->x - g->origin_x + ((edge->side & 1) ? 0 : pixel);
      const int y = edge->y - g->origin_y + ((edge->side & 1) ? pixel : 0);
      assert(x >= 0 && x < g->width && y >= 0 && y < g->height);
      assert(!cloud_pixels[(size_t)y * g->width + x]);
    }
  }
  free(cloud_pixels);
  return counts;
}

static void TransparentEdit(const ActionRoomScene *scene, int cx, int cy) {
  assert(cx >= 0 && cy >= 0);
  DioramaRoomOverride terrain = {.used = true, .map_group = 4, .map_number = 1};
  char line[256];
  snprintf(line, sizeof(line), "bg1-pixels = cell:%d,%d transparent:"
      "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF", cx, cy);
  assert(DioramaLayerOrder_ParseLine(&terrain, line, NULL));
  ActionVoxelGeometry *g = ActionVoxel_Create(); assert(g);
  assert(ActionVoxel_Build(g, scene, &terrain, ActionVoxel_SettingsForRoom(4, 1)));
  for (int y = cy * 16; y < cy * 16 + 16; ++y) for (int x = cx * 16; x < cx * 16 + 16; ++x)
    assert(!g->front[(size_t)(y - g->origin_y) * g->width + x - g->origin_x]);
  /* PPU applies the explicit transparent edit before the voxel split. */
  uint32_t low[256] = {0}, rear[256], front[256];
  const ActionRoomSceneFrameState frame = {.brightness = 15};
  assert(ActionVoxel_Split(g, cx * 16, cy * 16, 16, 16, 16, &frame, low, 16, rear, front));
  for (unsigned i = 0; i < 256; ++i) assert(!rear[i] && !front[i]);
  ActionVoxel_Release(g); DioramaLayerOrder_ClearRoom(&terrain);
}

static void Regional(const char *directory) {
  static ActionSceneSnapshot snapshot;
  static ActionRoomScene remapped;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  for (unsigned region = 0; region < 3; ++region) {
    char path[2048]; snprintf(path, sizeof(path), "%s/0401-%u.arscene", directory, region);
    FILE *file = fopen(path, "rb"); assert(file);
    const size_t size = fread(bytes, 1, sizeof(bytes), file); assert(!ferror(file)); assert(!fclose(file));
    assert(ActionSceneSnapshot_Decode(bytes, size, &snapshot));
    const ActionRoomScene *scene = &snapshot.scene;
    assert(scene->group == 4 && scene->map == 1);
    ActionVoxelGeometry *a = ActionVoxel_Create(), *b = ActionVoxel_Create(); assert(a && b);
    assert(ActionVoxel_Build(a, scene, NULL, ActionVoxel_SettingsForRoom(4, 1)));
    assert(ActionVoxel_Active(a, 4, 1));
    const CloudCounts original = CheckClouds(scene, a);
    remapped = *scene; Renumber(&remapped);
    assert(ActionVoxel_Build(b, &remapped, NULL, ActionVoxel_SettingsForRoom(4, 1)));
    const CloudCounts numbered = CheckClouds(&remapped, b);
    assert(!memcmp(&original, &numbered, sizeof(original)));
    assert(a->width == b->width && a->height == b->height && a->origin_x == b->origin_x && a->origin_y == b->origin_y);
    assert(!memcmp(a->front, b->front, (size_t)a->width * a->height));
    assert(a->edge_count == b->edge_count && !memcmp(a->edges, b->edges, a->edge_count * sizeof(*a->edges)));
    for (unsigned type = 0; type < 2; ++type)
      TransparentEdit(scene, original.first_x[type], original.first_y[type]);
    printf("mountain clouds 0401-%u: %u cells (%u mixed), %u low / %u high cloud pixels, %u rock pixels; "
           "exact split, native priority/collision, no cloud extrusion, renumbering and edits passed\n",
           region, original.cells, original.mixed, original.low, original.high, original.rock);
    ActionVoxel_Release(a); ActionVoxel_Release(b);
  }
}

int main(int argc, char **argv) {
  assert(argc == 1 || argc == 2);
  SplitContract();
  if (argc == 2) Regional(argv[1]);
  puts("action_voxel_mountain_cloud_test: PASS");
  return 0;
}
