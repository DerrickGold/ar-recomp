#include "support/test_assert.h"
#include "actraiser2/ar2_world_atlas.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void Word(uint8_t *p, unsigned value) {
  p[0] = value;
  p[1] = value >> 8;
}

/* Synthetic literal-only assets; no cartridge bytes or artwork in this test. */
static void LiteralAsset(uint8_t *out, const uint8_t *source) {
  Word(out, 16384);
  memset(out + 2, 0, 16384 * 9 / 8);
  for (size_t i = 0; i < 16384; ++i) {
    const unsigned token = 0x100u | source[i];
    for (unsigned j = 0; j < 9; ++j) {
      const size_t bit = i * 9 + j;
      if (token & (1u << (8 - j))) out[2 + bit / 8] |= 1u << (7 - bit % 8);
    }
  }
}

int main(void) {
  uint8_t *rom = calloc(1, 0x180000);
  Ar2WorldAtlas *atlas = malloc(sizeof(*atlas));
  assert(rom && atlas);
  memcpy(rom + 0xFFC0, "ActRaiser-2 USA", 14);
  rom[0xFFD5] = 0x31;
  Word(rom + 0xFFDE, 0xEFCC);
  uint8_t asset[16384] = {0};
  asset[0] = 1; /* Top-left cell selects the second CHR tile. */
  LiteralAsset(rom + 0x041AA0, asset);
  memset(asset, 0, sizeof(asset));
  asset[64] = 0x01;
  asset[65] = 0x08;
  asset[72] = 0x40;
  asset[73] = 0xFF;
  LiteralAsset(rom + 0x14DE55, asset);
  for (unsigned i = 0; i < kAr2WorldDestinationCapacity; ++i) {
    uint8_t *record = rom + 0x018000 + i * 16;
    Word(record, 200);
    Word(record + 2, 216);
    Word(record + 4, 300);
    Word(record + 6, 316);
    Word(record + 8, 0x600 | i);
    Word(record + 10, 0x8162);
    Word(record + 12, 0x0201);
    Word(record + 14, 0x0401);
  }
  for (unsigned i = 0; i < 14; ++i) memcpy(rom + 0x181B5 + i * 21, " TOWER OF SOUL", 14);
  memcpy(rom + 0x181B5 + 12 * 21, "TOWER OF SOULS", 14);
  memcpy(rom + 0x181B5 + 5 * 21, " DEMONxS CAVE ", 14);
  for (unsigned i = 0; i < 8; ++i) memcpy(rom + 0x183DB + i * 30, "    TOWN      ", 14);

  assert(Ar2WorldAtlas_Decode(atlas, rom, 0x180000) && atlas->available);
  assert(atlas->destination_count == 22);
  assert(atlas->pixels[0] == 0xFF210000u);
  assert(atlas->pixels[1] == 0xFF002100u);
  assert(atlas->pixels[1024] == 0xFF000042u);
  assert(atlas->pixels[1025] == 0xFFE7E7C6u);
  assert(atlas->indices[1025] == 0xFF);
  assert(atlas->pixels[8] == 0xFF000000u); /* Next metatile, not CHR row 2. */
  assert(!strcmp(atlas->destinations[18].label, "TOWER OF SOULS"));
  assert(!strcmp(atlas->destinations[6].label, "DEMON'S CAVE"));
  assert(!strcmp(atlas->destinations[0].label, "TOWN"));
  /* Co-located records keep separate identities, stage and progression data. */
  assert(atlas->destinations[4].x_min == atlas->destinations[20].x_min);
  assert(atlas->destinations[4].record_index == 4);
  assert(atlas->destinations[20].record_index == 20);
  assert(atlas->destinations[20].flags_and_scene == 0x614);
  assert(atlas->destinations[20].unlock_gate == 0x0201);
  assert(atlas->destinations[20].completion_gate == 0x0401);

  assert(!Ar2WorldAtlas_Decode(NULL, rom, 0x180000));
  assert(!Ar2WorldAtlas_Decode(atlas, NULL, 0x180000) && !atlas->available);
  assert(!Ar2WorldAtlas_Decode(atlas, rom, 0x17FFFF) && !atlas->available);
  rom[0xFFD5] = 0x20;
  assert(!Ar2WorldAtlas_Decode(atlas, rom, 0x180000) && !atlas->available);
  rom[0xFFD5] = 0x31;
  Word(rom + 0x18002, 1025);
  assert(!Ar2WorldAtlas_Decode(atlas, rom, 0x180000) && !atlas->available);
  Word(rom + 0x18002, 216);
  Word(rom + 0x18160, 1); /* Missing destination-table terminator. */
  assert(!Ar2WorldAtlas_Decode(atlas, rom, 0x180000) && !atlas->available);
  Word(rom + 0x18160, 0);
  Word(rom + 0x14DE55, 65535);
  assert(!Ar2WorldAtlas_Decode(atlas, rom, 0x180000) && !atlas->available);
  assert(!atlas->destination_count);
  free(rom);
  free(atlas);
  puts("AR2 world atlas: passed");
  return 0;
}
