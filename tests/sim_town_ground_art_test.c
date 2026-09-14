#include "sim/sim_town_ground_art.h"
#include "sim/sim_town_canvas.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "byte_order.h"

static int failures;
#define CHECK(e) do { if (!(e)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); failures++; \
} } while (0)

/* Independent oracle: feed the existing live-town renderer the native
 * loader's exact VRAM, palette and byte-swapped metatile definitions. Check
 * every pixel of every metatile in all four development/palette variants. */
static void CheckAgainstTownCanvas(const uint8_t *rom, size_t size) {
  uint8_t *wram = calloc(0x20000, 1);
  uint16_t *vram = calloc(0x8000, sizeof(*vram));
  uint16_t cgram[256] = {0};
  CHECK(wram && vram);
  if (!wram || !vram) { free(wram); free(vram); return; }
  CHECK(SimTownGroundArt_Init(rom, size));
  for (unsigned i = 0; i < 2048; i += 2) {
    wram[0x2100 + i] = rom[0xC881A + i + 1];
    wram[0x2100 + i + 1] = rom[0xC881A + i];
  }
  for (unsigned variant = 0; variant < 4; variant++) {
    const uint8_t town = variant & 2 ? 6 : 1;
    const uint8_t tier = variant & 1 ? 2 : 1;
    for (unsigned i = 0; i < 0x2000; i++)
      vram[i] = ByteOrder_ReadLe16(
          rom + 0x60000 + (variant & 1) * 0x4000 + i * 2);
    for (unsigned i = 0; i < 128; i++)
      cgram[i] = ByteOrder_ReadLe16(
          rom + (variant & 2 ? 0xE3D93 : 0xE3B93) + i * 2);
    SimTownCanvas_Reset();
    const uint32_t backdrop = 0xFF3F17B5u;
    /* Isolate source index $21 with the independent town renderer. Expanded
     * colours are not an identity: other indices may have the same RGB. */
    uint16_t original_cgram[256];
    memcpy(original_cgram, cgram, sizeof(cgram));
    memset(cgram, 0, sizeof(cgram));
    cgram[0x21] = 31;
    SimTownCanvas_Render(town, wram, vram, cgram, 15, backdrop);
    for (unsigned tile = 0; tile < 256; tile++) {
      uint32_t expected[256];
      uint8_t mask[256];
      CHECK(SimTownCanvas_RenderTerrainMetatile(wram, (uint8_t)tile, expected));
      CHECK(SimTownGroundArt_ColorIndexMask(town, tier, (uint8_t)tile, 0x21, mask));
      for (unsigned p = 0; p < 256; p++) CHECK(mask[p] == (expected[p] == 0xFFFF0000u));
      CHECK(SimTownGroundArt_ColorIndexMask(town, tier, (uint8_t)tile, 0, mask));
      for (unsigned p = 0; p < 256; p++) CHECK(mask[p] == 0);
    }
    /* Independent palette-identity oracle across ALL native DMA phases.
     * A land/transparent texel in any phase protects the complete metatile. */
    bool open_water[256];
    uint8_t water_pixels[256][256];
    memset(water_pixels,1,sizeof(water_pixels));
    for (unsigned tile=0;tile<256;++tile) open_water[tile]=true;
    memset(cgram,0,sizeof(cgram)); cgram[0x17]=cgram[0x18]=31;
    for (unsigned phase=0;phase<kSimTownGroundAnimationFrames;++phase) {
      if (SimTownGroundArt_AnimationAvailable())
        for (unsigned word=0;word<0x80;++word)
          vram[word]=ByteOrder_ReadLe16(rom+0x60000+(variant&1)*0x4000+phase*0x100+word*2);
      SimTownCanvas_Render(town,wram,vram,cgram,15,backdrop);
      for (unsigned tile=0;tile<256;++tile) {
        uint32_t expected[256];
        CHECK(SimTownCanvas_RenderTerrainMetatile(wram,tile,expected));
        for (unsigned p=0;p<256;++p) {
          open_water[tile] &= expected[p]==0xffff0000u;
          water_pixels[tile][p] &= expected[p]==0xffff0000u;
        }
      }
    }
    for (unsigned tile=0;tile<256;++tile) {
      uint8_t mask[256];
      CHECK(SimTownGroundArt_OpenWaterMask(town,tier,tile,mask));
      CHECK(!memcmp(mask,water_pixels[tile],sizeof(mask)));
      CHECK(SimTownGroundArt_IsOpenWater(town,tier,tile)==open_water[tile]);
      CHECK(SimTownGroundArt_IsOpenWater(town,tier,tile)==open_water[tile]); /* cached */
      CHECK(SimTownGroundArt_OpenWaterMask(town,tier,tile,mask));
      CHECK(!memcmp(mask,water_pixels[tile],sizeof(mask))); /* cached */
    }
    memcpy(cgram, original_cgram, sizeof(cgram));
    for (uint8_t phase = 0; phase < kSimTownGroundAnimationFrames; phase++) {
      /* Independent native DMA oracle: snapshot the loader's original bytes,
       * select a $100-byte slice, and upload it to VRAM word zero. Never copy
       * from a previously animated VRAM window (which corrupts later phases). */
      if (SimTownGroundArt_AnimationAvailable())
        for (unsigned word = 0; word < 0x80; word++)
          vram[word] = ByteOrder_ReadLe16(rom + 0x60000 +
              (variant & 1) * 0x4000 + phase * 0x100 + word * 2);
      SimTownCanvas_Render(town, wram, vram, cgram, 15, backdrop);
      for (unsigned tile = 0; tile < 256; tile++) {
        uint32_t expected[256];
        CHECK(SimTownCanvas_RenderTerrainMetatile(wram, (uint8_t)tile, expected));
        const uint32_t *actual = SimTownGroundArt_AnimatedMetatile(town, tier, (uint8_t)tile, phase);
        CHECK(actual);
        if (!actual) continue;
        for (unsigned p = 0; p < 256; p++)
          CHECK(expected[p] == (actual[p] ? actual[p] : backdrop));
        CHECK(SimTownGroundArt_AnimatedMetatile(town, tier, (uint8_t)tile, phase) == actual);
        bool animated = false;
        for (unsigned q = 0; q < 4; q++) {
          const uint8_t *definition = rom + 0xC881A + tile * 8 + q * 2;
          animated |= (((unsigned)definition[0] << 8 | definition[1]) & 511) < 8;
        }
        if (!phase || !animated)
          CHECK(SimTownGroundArt_Metatile(town, tier, (uint8_t)tile) == actual);
        if (town == 1)
          for (uint8_t other = 2; other <= 5; other++)
            CHECK(SimTownGroundArt_AnimatedMetatile(other, tier, (uint8_t)tile, phase) == actual);
      }
    }
  }
  SimTownCanvas_Reset();
  SimTownGroundArt_Shutdown();
  CHECK(!SimTownGroundArt_Available());
  CHECK(!SimTownGroundArt_Metatile(1, 1, 8));
  CHECK(!SimTownGroundArt_IsOpenWater(1,1,0x25));
  CHECK(!SimTownGroundArt_IsOpenWater(0,1,0x25));
  CHECK(!SimTownGroundArt_IsOpenWater(7,1,0x25));
  uint8_t mask[256], original[256];
  memset(mask,0x5a,sizeof(mask)); memcpy(original,mask,sizeof(mask));
  CHECK(!SimTownGroundArt_OpenWaterMask(1,1,0x25,mask));
  CHECK(!memcmp(mask,original,sizeof(mask)));
  free(wram);
  free(vram);
}

static void TestSyntheticSources(void) {
  const size_t size = 0x100000;
  uint8_t *rom = calloc(size, 1);
  CHECK(rom);
  if (!rom) return;
  rom[0x1098D] = 0x24;
  rom[0x1098E] = 8;
  for (size_t i = 0x60000; i < 0x68000; i++)
    rom[i] = (uint8_t)((i * 37 + i / 19) ^ (i >> 7));
  for (size_t i = 0xE3B93; i < 0xE3E93; i++)
    rom[i] = (uint8_t)(i * 17 + i / 11);
  for (unsigned tile = 0; tile < 256; tile++)
    for (unsigned q = 0; q < 4; q++) {
      const uint16_t entry = (uint16_t)(
          ((tile * 7 + q) & 511) | 0x0200 | ((tile & 7) << 10) | (q << 14));
      rom[0xC881A + tile * 8 + q * 2] = (uint8_t)(entry >> 8);
      rom[0xC881B + tile * 8 + q * 2] = (uint8_t)entry;
    }
  CheckAgainstTownCanvas(rom, size);
  CHECK(SimTownGroundArt_Init(rom, size));
  CHECK(SimTownGroundArt_AnimationAvailable());
  for (unsigned frame = 0; frame <= UINT16_MAX; frame++)
    CHECK(SimTownGroundArt_AnimationPhase((uint16_t)frame) == (((uint16_t)(frame - 1) >> 3) & 3));
  CHECK(!SimTownGroundArt_AnimatedMetatile(1, 1, 8, 4));
  CHECK(!SimTownGroundArt_AnimatedMetatile(1, 1, 8, 255));
  CHECK(!SimTownGroundArt_AnimatedMetatile(0, 1, 8, 1));
  CHECK(!SimTownGroundArt_Metatile(0, 1, 8));
  CHECK(!SimTownGroundArt_Metatile(7, 1, 8));
  uint8_t mask[256], untouched[256];
  memset(mask, 0x5A, sizeof(mask));
  memcpy(untouched, mask, sizeof(mask));
  CHECK(!SimTownGroundArt_OpenWaterMask(0,1,8,mask));
  CHECK(!SimTownGroundArt_OpenWaterMask(7,1,8,mask));
  CHECK(!SimTownGroundArt_OpenWaterMask(1,1,8,NULL));
  CHECK(!memcmp(mask,untouched,sizeof(mask)));
  CHECK(!SimTownGroundArt_ColorIndexMask(0, 1, 8, 0x21, mask));
  CHECK(!SimTownGroundArt_ColorIndexMask(7, 1, 8, 0x21, mask));
  CHECK(!SimTownGroundArt_ColorIndexMask(4, 1, 8, 128, mask));
  CHECK(!SimTownGroundArt_ColorIndexMask(4, 1, 8, 0x21, NULL));
  CHECK(!memcmp(mask, untouched, sizeof(mask)));
  uint32_t copy[4][4][256];
  for (unsigned variant = 0; variant < 4; variant++) {
    for (uint8_t phase = 0; phase < 4; phase++) {
      const uint32_t *pixels = SimTownGroundArt_AnimatedMetatile(
          variant & 2 ? 6 : 1, variant & 1 ? 2 : 1, 0, phase);
      CHECK(pixels);
      if (!pixels) { free(rom); return; }
      memcpy(copy[variant][phase], pixels, sizeof(copy[variant][phase]));
    }
    CHECK(memcmp(copy[variant][0], copy[variant][1], sizeof(copy[variant][0])));
    CHECK(!memcmp(copy[variant][0], SimTownGroundArt_Metatile(
        variant & 2 ? 6 : 1, variant & 1 ? 2 : 1, 0), sizeof(copy[variant][0])));
  }
  /* Reinitialization drops every decoded atlas. Overwrite the original ROM
   * before the first decode in this new lifetime, not after a cache hit. */
  CHECK(SimTownGroundArt_Init(rom, size));
  memset(rom, 0, size);
  /* Lazy decoding must use owned source bytes, never a borrowed ROM lifetime. */
  for (unsigned variant = 0; variant < 4; variant++) {
    for (uint8_t phase = 0; phase < 4; phase++) {
      const uint32_t *pixels = SimTownGroundArt_AnimatedMetatile(
          variant & 2 ? 6 : 2, variant & 1 ? 2 : 1, 0, phase);
      CHECK(pixels);
      if (pixels) CHECK(!memcmp(pixels, copy[variant][phase], sizeof(copy[variant][phase])));
    }
  }
  /* A different or absent animation profile keeps the static provider usable. */
  CHECK(SimTownGroundArt_Init(rom, size));
  CHECK(!SimTownGroundArt_AnimationAvailable());
  CHECK(SimTownGroundArt_AnimationPhase(20) == 0);
  CHECK(SimTownGroundArt_AnimatedMetatile(1, 1, 0, 3) == SimTownGroundArt_Metatile(1, 1, 0));
  CHECK(!SimTownGroundArt_Init(rom, 0xE3E92));
  CHECK(!SimTownGroundArt_Available());
  CHECK(!SimTownGroundArt_Init(NULL, size));
  free(rom);
}

int main(int argc, char **argv) {
  TestSyntheticSources();
  if (argc == 2) {
    FILE *file = fopen(argv[1], "rb");
    CHECK(file);
    if (file) {
      fseek(file, 0, SEEK_END);
      const long size = ftell(file);
      rewind(file);
      uint8_t *rom = size >= 0xE3E93 ? malloc((size_t)size) : NULL;
      CHECK(rom);
      if (rom) {
        CHECK(fread(rom, 1, (size_t)size, file) == (size_t)size);
        CheckAgainstTownCanvas(rom, (size_t)size);
        free(rom);
      }
      fclose(file);
    }
  }
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
