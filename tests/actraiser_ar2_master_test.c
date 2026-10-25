#include "actraiser/actraiser_ar2_master_assets.h"
#include "support/test_assert.h"
#include "support/ar2_master_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Everything here is synthetic: no ActRaiser 2 data is needed or committed. */

static void PutWord(Ar2MasterAssets *assets, uint16_t address, uint16_t value) {
  const size_t at = (size_t)(address - kAr2MasterMotionBase);
  assets->motion[at] = (uint8_t)value;
  assets->motion[at + 1] = (uint8_t)(value >> 8);
}

static void TestMotionPrograms(void) {
  static Ar2MasterAssets assets;
  memset(&assets, 0, sizeof(assets));
  /* Motion 1: x program at $6100, no y program. The program pointer names
   * the word before its first (frames - 1, delta) pair; a negative count is
   * a jump to another pair. */
  PutWord(&assets, 0x6004, 0x6100);
  PutWord(&assets, 0x6006, 0x0000);
  PutWord(&assets, 0x6102, 2);
  PutWord(&assets, 0x6104, 3);
  PutWord(&assets, 0x6106, 0);
  PutWord(&assets, 0x6108, 0xFFFF);
  PutWord(&assets, 0x610A, 0x8000);
  PutWord(&assets, 0x610C, 0x6102);

  uint16_t x, y;
  Ar2MasterAssets_MotionPrograms(&assets, 1, &x, &y);
  assert(x == 0x6100 && y == 0);
  Ar2MasterAssets_MotionPrograms(&assets, kAr2MasterMotionCount, &x, &y);
  assert(x == 0 && y == 0);

  Ar2MotionAxis axis;
  Ar2MotionAxis_Start(&axis, 0x6100);
  static const int16_t kExpected[] = {3, 3, 3, -1, 3, 3, 3, -1, 3};
  for (size_t i = 0; i < sizeof(kExpected) / sizeof(kExpected[0]); i++)
    assert(Ar2MotionAxis_Step(&assets, &axis) == kExpected[i]);

  Ar2MotionAxis none;
  Ar2MotionAxis_Start(&none, 0);
  assert(Ar2MotionAxis_Step(&assets, &none) == 0);

  /* A jump onto another jump ends the program instead of spinning. */
  PutWord(&assets, 0x6200, 0);
  PutWord(&assets, 0x6202, 0x8000);
  PutWord(&assets, 0x6204, 0x6202);
  Ar2MotionAxis bad;
  Ar2MotionAxis_Start(&bad, 0x6200);
  assert(Ar2MotionAxis_Step(&assets, &bad) == 0 && bad.pointer == 0);

  assert(Ar2MasterAssets_MotionWord(&assets, 0x5FFF) == 0);
  assert(Ar2MasterAssets_MotionWord(&assets,
                                    (uint16_t)(kAr2MasterMotionBase + kAr2MasterMotionBytes)) == 0);
}

static void PoolAdd(uint8_t pool[32], unsigned tile) {
  pool[tile >> 3] |= (uint8_t)(1u << (tile & 7));
}

static bool PoolHas(const uint8_t pool[32], unsigned tile) {
  return pool[tile >> 3] >> (tile & 7) & 1;
}

static Ar2MasterPart Part(unsigned source, bool large, uint16_t flips) {
  return (Ar2MasterPart){.attributes = (uint16_t)(flips | source), .large = large};
}

static bool HasCopy(const Ar2MasterPacking *packing, unsigned tile, unsigned source) {
  for (unsigned i = 0; i < packing->copy_count; i++)
    if (packing->copies[i].tile == tile && packing->copies[i].source == source) return true;
  return false;
}

static void TestPackFrame(void) {
  uint8_t pool[32] = {0};
  for (unsigned tile = 0x80; tile < 0xA0; tile++)
    if ((tile & 15) != 15) PoolAdd(pool, tile);

  Ar2MasterFrame frame = {.tile_bytes = 64 * 32, .part_count = 5};
  frame.parts[0] = Part(0x00, true, 0);
  frame.parts[1] = Part(0x02, true, 0x4000);
  frame.parts[2] = Part(0x20, false, 0);
  frame.parts[3] = Part(0x01, false, 0x8000); /* inside part 0's block */
  frame.parts[4] = Part(0x00, true, 0xC000);  /* same block as part 0 */
  Ar2MasterPacking packing;
  assert(Ar2MasterAssets_PackFrame(&frame, pool, &packing));
  assert(packing.part_tiles[0] == 0x80);
  assert(packing.part_tiles[1] == 0x82);
  assert(packing.part_tiles[2] == 0x84);
  assert(packing.part_tiles[3] == 0x81);
  assert(packing.part_tiles[4] == 0x80);
  assert(packing.copy_count == 9);
  assert(HasCopy(&packing, 0x80, 0x00) && HasCopy(&packing, 0x81, 0x01));
  assert(HasCopy(&packing, 0x90, 0x10) && HasCopy(&packing, 0x91, 0x11));
  assert(HasCopy(&packing, 0x82, 0x02) && HasCopy(&packing, 0x93, 0x13));
  assert(HasCopy(&packing, 0x84, 0x20));
  for (unsigned i = 0; i < packing.copy_count; i++)
    assert(PoolHas(pool, packing.copies[i].tile));

  /* A 16x16 OBJ wraps within its row at column 15, so a block may never
   * start there even when the wrapped tiles are free. */
  uint8_t edge[32] = {0};
  PoolAdd(edge, 0x8F);
  PoolAdd(edge, 0x90);
  PoolAdd(edge, 0x9F);
  PoolAdd(edge, 0xA0);
  Ar2MasterFrame large = {.tile_bytes = 64 * 32, .part_count = 1};
  large.parts[0] = Part(0x00, true, 0);
  assert(!Ar2MasterAssets_PackFrame(&large, edge, &packing));

  /* Every small part still finds a single tile. */
  Ar2MasterFrame smalls = {.tile_bytes = 64 * 32, .part_count = 3};
  smalls.parts[0] = Part(0x05, false, 0);
  smalls.parts[1] = Part(0x06, false, 0);
  smalls.parts[2] = Part(0x05, false, 0);
  assert(Ar2MasterAssets_PackFrame(&smalls, edge, &packing));
  assert(packing.copy_count == 2 && packing.part_tiles[0] == packing.part_tiles[2]);

  /* Parts may not read past the frame's own streamed block. */
  Ar2MasterFrame short_block = {.tile_bytes = 16 * 32, .part_count = 1};
  short_block.parts[0] = Part(0x00, true, 0);
  assert(!Ar2MasterAssets_PackFrame(&short_block, pool, &packing));
  short_block.parts[0] = Part(0x10, false, 0);
  assert(!Ar2MasterAssets_PackFrame(&short_block, pool, &packing));

  /* Running out of blocks fails rather than overlapping. */
  uint8_t one_block[32] = {0};
  PoolAdd(one_block, 0x80);
  PoolAdd(one_block, 0x81);
  PoolAdd(one_block, 0x90);
  PoolAdd(one_block, 0x91);
  Ar2MasterFrame two = {.tile_bytes = 64 * 32, .part_count = 2};
  two.parts[0] = Part(0x00, true, 0);
  two.parts[1] = Part(0x02, true, 0);
  assert(!Ar2MasterAssets_PackFrame(&two, one_block, &packing));
}

static uint16_t Rgb(unsigned r, unsigned g, unsigned b) { return (uint16_t)(r | g << 5 | b << 10); }

static unsigned Channel(uint16_t color, unsigned channel) { return (color >> (5 * channel)) & 31; }

static void TestTranslateFade(void) {
  uint16_t stone[16], final_colors[16], ar2[16], out[16], step[16];
  for (unsigned i = 0; i < 16; i++) {
    stone[i] = Rgb(i * 2, i * 2, i * 2);
    final_colors[i] = Rgb(i * 2, 31 - i * 2, i);
    ar2[i] = Rgb(31 - i, i, 16);
  }

  /* The final step is the AR2 palette itself. */
  Ar2MasterAssets_TranslateFade(ar2, stone, final_colors, final_colors, out);
  assert(!memcmp(out, ar2, sizeof(out)));

  /* The stone step uses only the native stone colours. */
  Ar2MasterAssets_TranslateFade(ar2, stone, final_colors, stone, out);
  assert(out[0] == ar2[0]);
  for (unsigned i = 1; i < 16; i++) {
    bool found = false;
    for (unsigned j = 1; j < 16 && !found; j++)
      found = out[i] == stone[j];
    assert(found);
  }
  uint16_t stone_out[16];
  memcpy(stone_out, out, sizeof(stone_out));

  /* Halfway along the native line is halfway between those two. */
  for (unsigned i = 0; i < 16; i++) {
    uint16_t color = 0;
    for (unsigned c = 0; c < 3; c++)
      color |= (uint16_t)(((Channel(stone[i], c) + Channel(final_colors[i], c)) / 2) << (5 * c));
    step[i] = color;
  }
  Ar2MasterAssets_TranslateFade(ar2, stone, final_colors, step, out);
  for (unsigned i = 1; i < 16; i++)
    for (unsigned c = 0; c < 3; c++) {
      const int mid = ((int)Channel(stone_out[i], c) + (int)Channel(ar2[i], c)) / 2;
      assert(abs((int)Channel(out[i], c) - mid) <= 1);
    }

  /* A flash that is not on the line is copied: all white stays all white. */
  for (unsigned i = 0; i < 16; i++)
    step[i] = 0x7FFF;
  Ar2MasterAssets_TranslateFade(ar2, stone, final_colors, step, out);
  assert(out[0] == ar2[0]);
  for (unsigned i = 1; i < 16; i++)
    assert(out[i] == 0x7FFF);
}

static void TestIdentifyRom(void) {
  enum { kSize = 0x180000, kHeader = 512 };
  uint8_t *image = calloc(kSize + kHeader, 1);
  assert(image);
  const uint8_t *payload = NULL;
  size_t payload_size = 0;
  char error[160] = "";

  assert(!Ar2MasterAssets_IdentifyRom(image, 1024, &payload, &payload_size, error, sizeof(error)));
  assert(strstr(error, "12 Mbit"));
  assert(!Ar2MasterAssets_IdentifyRom(image, kSize, &payload, &payload_size, error, sizeof(error)));
  assert(strstr(error, "title"));
  /* A copier header is skipped before the title is read. */
  assert(!Ar2MasterAssets_IdentifyRom(image, kSize + kHeader, &payload, &payload_size, error,
                                      sizeof(error)));
  assert(strstr(error, "title"));

  /* Right title and mirrored checksum, but not the supported dump. The
   * checksum and its complement always add $1FE to the sum themselves. */
  static const char kTitle[] = "ActRaiser-2 USA";
  memcpy(image + 0xFFC0, kTitle, sizeof(kTitle) - 1);
  uint32_t sum = 0x1FE;
  for (size_t i = 0; i < sizeof(kTitle) - 1; i++)
    sum += (uint8_t)kTitle[i];
  const uint16_t checksum = (uint16_t)sum;
  image[0xFFDC] = (uint8_t)~checksum;
  image[0xFFDD] = (uint8_t)(~checksum >> 8);
  image[0xFFDE] = (uint8_t)checksum;
  image[0xFFDF] = (uint8_t)(checksum >> 8);
  assert(!Ar2MasterAssets_IdentifyRom(image, kSize, &payload, &payload_size, error, sizeof(error)));
  assert(strstr(error, "SHA-256"));
  image[0x150000] = 1; /* the mirrored half counts twice */
  assert(!Ar2MasterAssets_IdentifyRom(image, kSize, &payload, &payload_size, error, sizeof(error)));
  assert(strstr(error, "checksum"));

  static Ar2MasterAssets assets;
  assert(!Ar2MasterAssets_Parse(image, kSize, &assets, error, sizeof(error)));
  free(image);
}

static void TestPack(void) {
  uint8_t data[kAr2MasterFixtureBytes];
  const size_t size = Ar2MasterFixture(data, false);
  static Ar2MasterAssets assets;
  char error[160];
  assert(Ar2MasterAssets_ParsePack(data, size, &assets, error, sizeof(error)));
  assert(assets.frame_count == 1 && assets.frames[0].part_count == 1);
  assert(assets.palette[1] == 0x1234 && assets.data[assets.frames[0].tile_offset] == 0xA5);
  for (size_t length = 0; length < size; length++)
    assert(!Ar2MasterAssets_ParsePack(data, length, &assets, error, sizeof(error)));
  data[size - 1] ^= 1;
  assert(!Ar2MasterAssets_ParsePack(data, size, &assets, error, sizeof(error)));
  Ar2MasterFixture(data, false);
  /* A valid digest does not excuse an animation referencing a missing frame. */
  data[80 + kAr2MasterMotionBytes + 2 + 3] = 1;
  assert(sr_support_sha256(data + 48, size - 48, data + 16));
  assert(!Ar2MasterAssets_ParsePack(data, size, &assets, error, sizeof(error)));
  assert(!assets.frame_count && !assets.data);
}

static uint8_t *ReadFile(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  assert(f && !fseek(f, 0, SEEK_END));
  const long length = ftell(f);
  assert(length > 0 && !fseek(f, 0, SEEK_SET));
  uint8_t *data = malloc((size_t)length);
  assert(data && fread(data, 1, (size_t)length, f) == (size_t)length && !fclose(f));
  *size = (size_t)length;
  return data;
}

/* Optional local-ROM integration: compare the Go import against the original
 * C parser. Neither input is part of the normal ROM-free test suite. */
static void TestExtractedPack(const char *rom_path, const char *pack_path) {
  size_t rom_size, pack_size, payload_size;
  uint8_t *rom = ReadFile(rom_path, &rom_size), *pack = ReadFile(pack_path, &pack_size);
  const uint8_t *payload;
  static Ar2MasterAssets native, extracted;
  char error[160];
  assert(Ar2MasterAssets_IdentifyRom(rom, rom_size, &payload, &payload_size, error, sizeof(error)));
  assert(Ar2MasterAssets_Parse(payload, payload_size, &native, error, sizeof(error)));
  assert(Ar2MasterAssets_ParsePack(pack, pack_size, &extracted, error, sizeof(error)));
  assert(native.frame_count == extracted.frame_count);
  assert(!memcmp(native.anims, extracted.anims, sizeof(native.anims)));
  assert(!memcmp(native.palette, extracted.palette, sizeof(native.palette)));
  assert(!memcmp(native.motion, extracted.motion, sizeof(native.motion)));
  for (unsigned i = 0; i < native.frame_count; i++) {
    Ar2MasterFrame a = native.frames[i], b = extracted.frames[i];
    assert(a.tile_bytes == b.tile_bytes);
    assert(!memcmp(payload + a.tile_offset, pack + b.tile_offset, a.tile_bytes));
    assert(a.left == b.left && a.right == b.right && a.top == b.top && a.bottom == b.bottom);
    assert(a.hurt_half_width == b.hurt_half_width && a.hurt_height == b.hurt_height);
    assert(a.attack_x == b.attack_x && a.attack_y == b.attack_y && a.has_attack == b.has_attack);
    assert(a.attack_width == b.attack_width && a.attack_height == b.attack_height);
    assert(a.part_count == b.part_count);
    for (unsigned j = 0; j < a.part_count; j++) {
      const Ar2MasterPart x = a.parts[j], y = b.parts[j];
      assert(x.x == y.x && x.x_flipped == y.x_flipped && x.y == y.y && x.y_flipped == y.y_flipped);
      assert(x.attributes == y.attributes && x.large == y.large);
    }
  }
  printf("Go/C extraction matches all %u frames\n", (unsigned)native.frame_count);
  free(rom);
  free(pack);
}

int main(int argc, char **argv) {
  TestPack();
  if (argc == 3) TestExtractedPack(argv[1], argv[2]);
  TestMotionPrograms();
  TestPackFrame();
  TestTranslateFade();
  TestIdentifyRom();
  puts("actraiser_ar2_master: ok");
  return 0;
}
