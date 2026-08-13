#include "diorama_rom_backdrop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"

static int failures;

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);           \
      failures++;                                                           \
    }                                                                       \
  } while (0)

static void PutBits(uint8_t *bytes, size_t *bit, unsigned value,
                    unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    const unsigned shift = count - 1 - i;
    if ((value >> shift) & 1u)
      bytes[*bit >> 3] |= (uint8_t)(1u << (7 - (*bit & 7)));
    (*bit)++;
  }
}

static size_t StartStream(uint8_t *bytes, size_t output_size) {
  memset(bytes, 0, 32);
  bytes[0] = (uint8_t)output_size;
  bytes[1] = (uint8_t)(output_size >> 8);
  return 16;
}

static size_t StreamBytes(size_t bit) { return (bit + 7) / 8; }

static void TestLiterals(void) {
  uint8_t packed[32], output[3] = {0};
  size_t bit = StartStream(packed, sizeof(output));
  for (unsigned value = 'A'; value <= 'C'; value++) {
    PutBits(packed, &bit, 1, 1);
    PutBits(packed, &bit, value, 8);
  }
  CHECK(DioramaRomBackdrop_DecompressAsset(
      packed, StreamBytes(bit), output, sizeof(output)));
  CHECK(!memcmp(output, "ABC", sizeof(output)));
  CHECK(!DioramaRomBackdrop_DecompressAsset(
      packed, StreamBytes(bit), output, sizeof(output) + 1));
}

static void TestOverlappingDictionaryCopyAndTruncation(void) {
  uint8_t packed[32], output[6] = {0};
  size_t bit = StartStream(packed, sizeof(output));
  PutBits(packed, &bit, 1, 1);       /* literal A at dictionary $EF */
  PutBits(packed, &bit, 'A', 8);
  PutBits(packed, &bit, 0, 1);       /* copy from $EF, length 3+2 */
  PutBits(packed, &bit, 0xEF, 8);
  PutBits(packed, &bit, 3, 4);
  const size_t size = StreamBytes(bit);
  CHECK(DioramaRomBackdrop_DecompressAsset(
      packed, size, output, sizeof(output)));
  CHECK(!memcmp(output, "AAAAAA", sizeof(output)));
  CHECK(!DioramaRomBackdrop_DecompressAsset(
      packed, size - 1, output, sizeof(output)));
}

static size_t WriteLiteralAsset(uint8_t *dst, const uint8_t *source,
                                size_t output_size) {
  const size_t packed_size = 2 + (output_size * 9 + 7) / 8;
  memset(dst, 0, packed_size);
  dst[0] = (uint8_t)output_size;
  dst[1] = (uint8_t)(output_size >> 8);
  size_t bit = 16;
  for (size_t i = 0; i < output_size; i++) {
    PutBits(dst, &bit, 1, 1);
    PutBits(dst, &bit, source[i], 8);
  }
  return StreamBytes(bit);
}

static void Put24(uint8_t *dst, size_t value) {
  dst[0] = (uint8_t)value;
  dst[1] = (uint8_t)(value >> 8);
  dst[2] = (uint8_t)(value >> 16);
}

static void TestGenericRoomScriptAndInheritance(void) {
  enum {
    kRomSize = 0x30000,
    kScript = 0x28000,
    kChr0 = 0x1000,
    kChr1 = 0x4000,
    kMeta1 = 0x7000,
    kMeta2 = 0x8000,
    kMap1 = 0x9000,
    kMap2 = 0x9400,
    kPalette = 0x9800,
    kPalette2 = 0x9900,
    kPaletteUpper = 0x9A00,
  };
  uint8_t *rom = calloc(1, kRomSize);
  uint8_t *zeros = calloc(1, 0x2000);
  uint8_t *chars = calloc(1, 0x2000);
  uint8_t *meta1 = calloc(1, 0x0800);
  uint32_t *pixels = calloc(
      kDioramaRomBackdropPixels * kDioramaRomBackdropPixels,
      sizeof(*pixels));
  CHECK(rom && zeros && chars && meta1 && pixels);
  if (!rom || !zeros || !chars || !meta1 || !pixels) goto done;

  /* Tile zero emits colour 1. BG1's metatile selects palette group 4, proving
   * that the generic path follows the upper four BG palettes as well as the
   * originally measured Aitos BG2 palettes 0..3. */
  for (unsigned row = 0; row < 8; row++) chars[row * 2] = 0xFF;
  for (unsigned quadrant = 0; quadrant < 4; quadrant++)
    meta1[quadrant * 2] = 0x10;      /* byte-swapped SNES word $1000 */
  WriteLiteralAsset(rom + kChr0, chars, 0x2000);
  WriteLiteralAsset(rom + kChr1, zeros, 0x2000);
  WriteLiteralAsset(rom + kMeta1, meta1, 0x0800);
  WriteLiteralAsset(rom + kMeta2, zeros, 0x0800);
  rom[kMap1] = rom[kMap1 + 1] = 1;
  rom[kMap2] = rom[kMap2 + 1] = 1;
  WriteLiteralAsset(rom + kMap1 + 2, zeros, 0x0100);
  WriteLiteralAsset(rom + kMap2 + 2, zeros, 0x0100);
  rom[kPalette + 2] = 0x1F;         /* BGR15 red at palette 0 colour 1 */
  rom[kPalette2 + 2] = 0xE0;        /* BGR15 green at palette 0 colour 1 */
  rom[kPalette2 + 3] = 0x03;
  rom[kPaletteUpper + 2] = 0x00;    /* BGR15 blue at palette 4 colour 1 */
  rom[kPaletteUpper + 3] = 0x7C;

  size_t at = kScript;
  rom[at++] = 'S'; rom[at++] = 'Y'; rom[at++] = 0;
  rom[at++] = 0x04; rom[at++] = 0x01;
#define COMMAND(byte, count) rom[at++] = (byte); size_t ops = at; at += (count)
  { COMMAND(0x40, 6); rom[ops] = 0; rom[ops + 1] = 0x40;
    rom[ops + 2] = 0; Put24(rom + ops + 3, kPalette); }
  { COMMAND(0x40, 6); rom[ops] = 0; rom[ops + 1] = 0x40;
    rom[ops + 2] = 0x40; Put24(rom + ops + 3, kPaletteUpper); }
  { COMMAND(0x20, 7); rom[ops + 3] = 1; Put24(rom + ops + 4, kMeta1); }
  { COMMAND(0x20, 7); rom[ops + 3] = 2; Put24(rom + ops + 4, kMeta2); }
  { COMMAND(0x80, 6); rom[ops] = 0; rom[ops + 1] = 0x10;
    rom[ops + 2] = 0; Put24(rom + ops + 3, kChr0); }
  { COMMAND(0x80, 6); rom[ops] = 0; rom[ops + 1] = 0x10;
    rom[ops + 2] = 0x10; Put24(rom + ops + 3, kChr1); }
  { COMMAND(0x10, 4); rom[ops] = 1; Put24(rom + ops + 1, kMap1); }
  { COMMAND(0x10, 4); rom[ops] = 2; Put24(rom + ops + 1, kMap2); }
  rom[at++] = 0;
  /* Room 2 deliberately has no graphics commands. The stock game inherits
   * them within the act, and an arbitrary ROM-background selection must do the
   * same rather than depending on room visit order. */
  rom[at++] = 0x04; rom[at++] = 0x02; rom[at++] = 0;
  /* Room 3 changes only the inherited palette. This pins arbitrary-source
   * switching: the output must follow the complete selected room identity,
   * not a texture/pixel cache left behind by the previous source. */
  rom[at++] = 0x04; rom[at++] = 0x03;
  { COMMAND(0x40, 6); rom[ops] = 0; rom[ops + 1] = 0x40;
    rom[ops + 2] = 0; Put24(rom + ops + 3, kPalette2); }
  rom[at++] = 0;
#undef COMMAND

  const size_t pixel_count =
      kDioramaRomBackdropPixels * kDioramaRomBackdropPixels;
  CHECK(DioramaRomBackdrop_LoadActionBg(
      rom, kRomSize, 0x04, 0x01, 2, pixels, pixel_count));
  CHECK(pixels[0] == 0xFFFF0000u);
  CHECK(pixels[pixel_count - 1] == 0xFFFF0000u);
  memset(pixels, 0, pixel_count * sizeof(*pixels));
  CHECK(DioramaRomBackdrop_LoadActionBg(
      rom, kRomSize, 0x04, 0x02, 1, pixels, pixel_count));
  CHECK(pixels[12345] == 0xFF0000FFu);
  CHECK(DioramaRomBackdrop_LoadActionBg(
      rom, kRomSize, 0x04, 0x03, 2, pixels, pixel_count));
  CHECK(pixels[12345] == 0xFF00FF00u);
  CHECK(!DioramaRomBackdrop_LoadActionBg(
      rom, kRomSize, 0x04, 0x04, 1, pixels, pixel_count));
  CHECK(!DioramaRomBackdrop_LoadActionBg(
      rom, kRomSize, 0x04, 0x01, 3, pixels, pixel_count));

done:
  free(pixels);
  free(meta1);
  free(chars);
  free(zeros);
  free(rom);
}

static void TestStockRomCatalogue(const char *path) {
  FILE *file = fopen(path, "rb");
  CHECK(file != NULL);
  if (!file) return;
  CHECK(fseek(file, 0, SEEK_END) == 0);
  const long length = ftell(file);
  CHECK(length > 0);
  rewind(file);
  uint8_t *rom = length > 0 ? malloc((size_t)length) : NULL;
  CHECK(rom != NULL);
  if (!rom) { fclose(file); return; }
  CHECK(fread(rom, 1, (size_t)length, file) == (size_t)length);
  fclose(file);

  const size_t pixel_count =
      kDioramaRomBackdropPixels * kDioramaRomBackdropPixels;
  uint32_t *pixels = malloc(pixel_count * sizeof(*pixels));
  CHECK(pixels != NULL);
  if (!pixels) { free(rom); return; }
  int decoded = 0;
  for (uint8_t group = 1; group <= 7; group++) {
    for (uint8_t map = 1; map <= ActRaiser_ActionMapLast(group); map++) {
      for (uint8_t bg = 1; bg <= 2; bg++) {
        if (!DioramaRomBackdrop_LoadActionBg(
                rom, (size_t)length, group, map, bg, pixels,
                pixel_count)) {
          printf("FAIL stock ROM backdrop %02X/%02X BG%u\n",
                 group, map, bg);
          failures++;
        } else {
          decoded++;
        }
      }
    }
  }
  CHECK(decoded == (4 + 8 + 6 + 7 + 8 + 8 + 8) * 2);
  free(pixels);
  free(rom);
}

int main(int argc, char **argv) {
  TestLiterals();
  TestOverlappingDictionaryCopyAndTruncation();
  TestGenericRoomScriptAndInheritance();
  /* Optional local census against a legally supplied stock ROM. CTest invokes
   * this binary without one, keeping the suite hermetic and distributable. */
  if (argc > 1) TestStockRomCatalogue(argv[1]);
  if (failures) {
    printf("diorama rom backdrop: %d failure(s)\n", failures);
    return 1;
  }
  printf("diorama rom backdrop: all checks passed\n");
  return 0;
}
