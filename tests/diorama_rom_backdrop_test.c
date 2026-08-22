#include "diorama_rom_backdrop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "action/action_room_scene.h"
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
    kVideoProfiles = 0x1093E,
    kProfile = 0x17,
  };
  uint8_t *rom = calloc(1, kRomSize);
  uint8_t *zeros = calloc(1, 0x2000);
  uint8_t *chars0 = calloc(1, 0x2000);
  uint8_t *chars1 = calloc(1, 0x2000);
  uint8_t *meta1 = calloc(1, 0x0800);
  uint8_t *meta2 = calloc(1, 0x0800);
  uint32_t *pixels = calloc(
      kDioramaRomBackdropPixels * kDioramaRomBackdropPixels,
      sizeof(*pixels));
  CHECK(rom && zeros && chars0 && chars1 && meta1 && meta2 && pixels);
  if (!rom || !zeros || !chars0 || !chars1 || !meta1 || !meta2 || !pixels)
    goto done;

  /* `$02:B6D3-$B6F6` masks definition words with $ECFF, then `$02:B4E8`
   * merges attribute byte $10 into BG1 and $01 into BG2. Pin both operations
   * with deliberately misleading definition bits: BG1 raw tile $100 must
   * become tile $000/palette 4, while BG2 raw tile $200 must become tile $100.
   * The two resulting tiles emit different colour indices so using the raw
   * definition, omitting the attribute, or selecting the wrong character bank
   * cannot accidentally produce the expected pixel. */
  for (unsigned row = 0; row < 8; row++) {
    chars0[row * 2] = 0xFF;          /* tile $000: colour 1 */
    chars1[row * 2 + 1] = 0xFF;      /* tile $100: colour 2 */
  }
  for (unsigned quadrant = 0; quadrant < 4; quadrant++) {
    meta1[quadrant * 2] = 0x01;      /* byte-swapped SNES word $0100 */
    meta2[quadrant * 2] = 0x02;      /* byte-swapped SNES word $0200 */
  }
  WriteLiteralAsset(rom + kChr0, chars0, 0x2000);
  WriteLiteralAsset(rom + kChr1, chars1, 0x2000);
  WriteLiteralAsset(rom + kMeta1, meta1, 0x0800);
  WriteLiteralAsset(rom + kMeta2, meta2, 0x0800);
  rom[kMap1] = rom[kMap1 + 1] = 1;
  rom[kMap2] = rom[kMap2 + 1] = 1;
  WriteLiteralAsset(rom + kMap1 + 2, zeros, 0x0100);
  WriteLiteralAsset(rom + kMap2 + 2, zeros, 0x0100);
  rom[kPalette + 2] = 0xFF;         /* wrong-path palette 0 colour 1: white */
  rom[kPalette + 3] = 0x7F;
  rom[kPalette + 4] = 0x1F;         /* BGR15 red at palette 0 colour 2 */
  rom[kPalette2 + 4] = 0xE0;        /* BGR15 green at palette 0 colour 2 */
  rom[kPalette2 + 5] = 0x03;
  rom[kPaletteUpper + 2] = 0x00;    /* BGR15 blue at palette 4 colour 1 */
  rom[kPaletteUpper + 3] = 0x7C;
  /* Profile bit 1 forces common priority onto BG2 independently of the
   * permanent $01 character-bank attribute. */
  rom[kVideoProfiles + kProfile * kActionRoomSceneVideoProfileBytes + 4] =
      0x02;

  size_t at = kScript;
  rom[at++] = 'S'; rom[at++] = 'Y'; rom[at++] = 0;
  rom[at++] = 0x04; rom[at++] = 0x01;
#define COMMAND(byte, count) rom[at++] = (byte); size_t ops = at; at += (count)
  { COMMAND(0x08, 1); rom[ops] = kProfile; }
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

  ActionRoomScene scene;
  CHECK(ActionRoomScene_Load(&scene, rom, kRomSize, 0x04, 0x02));
  CHECK(scene.have_video_profile);
  CHECK(scene.have_raster_workspace);
  CHECK(!memcmp(scene.raster_workspace, chars1,
                kActionRoomSceneRasterWorkspaceBytes));
  CHECK(scene.video_profile_index == kProfile);
  CHECK(scene.video_profile[4] == 0x02);
  CHECK(ActionRoomScene_TileWidth(&scene, 1) == 32);
  CHECK(ActionRoomScene_TileHeight(&scene, 2) == 32);
  uint16_t entry = 0;
  uint8_t metatile = 0xFF;
  CHECK(ActionRoomScene_LookupTile(
      &scene, 2, 0, 0, &entry, &metatile));
  CHECK(entry == 0x2100);
  CHECK(metatile == 0);

done:
  free(pixels);
  free(meta2);
  free(meta1);
  free(chars1);
  free(chars0);
  free(zeros);
  free(rom);
}

static void TestScenePhaseResolvers(void) {
  ActionRoomScene scene;
  memset(&scene, 0, sizeof(scene));
  scene.have_character_bank[0] = true;
  scene.have_character_bank[1] = true;
  scene.have_video_profile = true;
  scene.video_profile[23] = 0x24;  /* target 0000, stride 256, four phases */
  scene.video_profile[24] = 0x88;  /* continuation, eight-frame cadence */
  for (unsigned phase = 0; phase < 4; phase++)
    scene.characters[phase * 0x100] = (uint8_t)(0xA0 + phase);

  CHECK(ActionRoomScene_HasCharacterAnimation(&scene));
  CHECK(ActionRoomScene_CharacterAnimationStride(&scene) == 0x100);
  CHECK(ActionRoomScene_CharacterAnimationPhaseCount(&scene) == 4);
  CHECK(ActionRoomScene_CharacterAnimationCadence(&scene) == 8);
  CHECK(ActionRoomScene_CharacterAnimationTarget(&scene) == 0);
  CHECK(ActionRoomScene_CharacterAnimationContinues(&scene));
  CHECK(ActionRoomScene_ResolveCharacterAnimationPhase(&scene, 16, -1) == 2);
  CHECK(ActionRoomScene_ResolveCharacterAnimationPhase(&scene, 0, 7) == 3);
  uint8_t *characters = malloc(kActionRoomSceneCharacterBytes);
  CHECK(characters != NULL);
  if (characters) {
    CHECK(ActionRoomScene_BuildCharacters(
        &scene, 16, -1, characters, kActionRoomSceneCharacterBytes));
    CHECK(characters[0] == 0xA2);
    CHECK(ActionRoomScene_BuildCharacters(
        &scene, 0, 1, characters, kActionRoomSceneCharacterBytes));
    CHECK(characters[0] == 0xA1);
    free(characters);
  }

  scene.group = 0x04;
  scene.map = 0x02;
  scene.bg[1].have_map = true;
  scene.bg[1].pages_wide = 2;
  scene.bg[1].pages_high = 2;
  scene.video_profile[19] = 0x0C;
  CHECK(ActionRoomScene_HasBg2PageCycle(&scene));
  CHECK(ActionRoomScene_Bg2PageIndex(&scene, 0, 0) == 1);
  CHECK(ActionRoomScene_Bg2PageIndex(&scene, 0, 1) == 2);
  CHECK(ActionRoomScene_Bg2PageIndex(&scene, 0, 2) == 3);
  CHECK(ActionRoomScene_Bg2PageIndex(&scene, 0, 3) == 0);
  CHECK(ActionRoomScene_ResolveBg2PagePhase(&scene, 10, -1) == 2);
}

static void InitFrameScene(ActionRoomScene *scene) {
  memset(scene, 0, sizeof(*scene));
  scene->have_video_profile = true;
  scene->have_raster_waveform = true;
  scene->have_raster_workspace = true;
  scene->video_profile[0] = 3;
  scene->video_profile[6] = 1;
  scene->video_profile[9] = 0x12;
  scene->video_profile[10] = 0x14;
  for (unsigned i = 0; i < kActionRoomSceneRasterWaveformBytes; i++)
    scene->raster_waveform[i] = (uint8_t)i;
  for (unsigned bg = 0; bg < 2; bg++) {
    scene->bg[bg].have_map = true;
    scene->bg[bg].have_metatiles = true;
    scene->bg[bg].pages_wide = 1;
    scene->bg[bg].pages_high = 1;
    scene->bg[bg].map_size = kActionRoomSceneMapPageBytes;
  }
}

static void TestFrameStateAndRasterPresets(void) {
  ActionRoomScene scene;
  InitFrameScene(&scene);
  const ActionRoomSceneFrameRequest request = {
    .camera_x = 64,
    .camera_y = 32,
    .game_frame = 10,
    .animation_phase = -1,
    .page_phase = -1,
  };
  ActionRoomSceneFrameState state;

  scene.raster_preset = kActionRoomRaster_None;
  CHECK(ActionRoomScene_BuildFrameState(&scene, &request, &state));
  CHECK(state.bg_hscroll[0][0] == 64);
  CHECK(state.bg_vscroll[0][223] == 32);
  CHECK(state.bg_hscroll[1][17] == 32);
  CHECK(state.bg_vscroll[1][17] == 8);
  CHECK(state.screen_enabled[0] == 3);
  CHECK(state.bgmode == 1);
  CHECK(state.brightness == 15);

  scene.raster_preset = kActionRoomRaster_R1;
  memset(scene.raster_workspace, 0, sizeof(scene.raster_workspace));
  scene.raster_workspace[2] = 2;
  scene.raster_workspace[5] = 1;
  CHECK(ActionRoomScene_BuildFrameState(&scene, &request, &state));
  CHECK(state.bg_hscroll[1][0] == 0x20D);
  CHECK(state.bg_hscroll[1][1] == 0x20D);
  CHECK(state.bg_hscroll[1][2] == 0x10E);
  CHECK(state.bg_hscroll[1][223] == 123);

  scene.raster_preset = kActionRoomRaster_R2;
  memset(scene.raster_workspace, 0, sizeof(scene.raster_workspace));
  scene.raster_workspace[5] = 2;
  scene.raster_workspace[8] = 1;
  CHECK(ActionRoomScene_BuildFrameState(&scene, &request, &state));
  CHECK(state.bg_hscroll[1][126] == 0);
  CHECK(state.bg_hscroll[1][127] == 0x200);
  CHECK(state.bg_hscroll[1][128] == 0x101);
  CHECK(state.bg_hscroll[1][223] == 0x36);

  scene.raster_preset = kActionRoomRaster_R3;
  memset(scene.raster_workspace, 0, sizeof(scene.raster_workspace));
  scene.raster_workspace[2] = 1;
  scene.raster_workspace[5] = 2;
  scene.raster_workspace[8] = 1;
  scene.raster_workspace[101] = 3;
  CHECK(ActionRoomScene_BuildFrameState(&scene, &request, &state));
  CHECK(state.bg_vscroll[1][126] == 0x100);
  CHECK(state.bg_vscroll[1][127] == 0x200);
  CHECK(state.bg_vscroll[1][128] == 0x1FB);
  CHECK(state.bg_vscroll[1][159] == 0x300);
  CHECK(state.bg_vscroll[1][223] == 0x300);

  scene.raster_preset = kActionRoomRaster_R4;
  memset(scene.raster_workspace, 0, sizeof(scene.raster_workspace));
  CHECK(ActionRoomScene_BuildFrameState(&scene, &request, &state));
  CHECK(state.mosaic[0] == 0x02);
  CHECK(state.mosaic[1] == 0x02);
  CHECK(state.mosaic[2] == 0x12);
  CHECK(state.mosaic[223] == 0x12);

  scene.raster_preset = kActionRoomRaster_R5;
  CHECK(ActionRoomScene_BuildFrameState(&scene, &request, &state));
  CHECK(state.bg_hscroll[1][0] == 50);
  CHECK(state.bg_hscroll[1][62] == 50);
  CHECK(state.bg_hscroll[1][63] == 25);
  CHECK(state.bg_hscroll[1][175] == 32);

  scene.raster_preset = kActionRoomRaster_R6;
  CHECK(ActionRoomScene_BuildFrameState(&scene, &request, &state));
  CHECK(state.bg_hscroll[1][0] == 68);
  CHECK(state.bg_hscroll[1][189] == 32);
  CHECK(state.bg_hscroll[1][190] == 1);
  CHECK(state.bg_hscroll[1][191] == 3);

  scene.raster_preset = kActionRoomRaster_R7;
  memset(scene.raster_workspace, 0, sizeof(scene.raster_workspace));
  scene.raster_workspace[0x802] = 3;
  scene.raster_workspace[0x805] = 2;
  CHECK(ActionRoomScene_BuildFrameState(&scene, &request, &state));
  CHECK(state.bg_hscroll[1][0] == 0x304);
  CHECK(state.bg_hscroll[1][1] == 0x304);
  CHECK(state.bg_hscroll[1][2] == 0x205);

  scene.raster_preset = kActionRoomRaster_R8;
  CHECK(ActionRoomScene_BuildFrameState(&scene, &request, &state));
  CHECK(state.bg_hscroll[1][142] == 0);
  CHECK(state.bg_hscroll[1][143] == 1021);

  scene.raster_preset = kActionRoomRaster_R9;
  memset(scene.raster_workspace, 0, sizeof(scene.raster_workspace));
  scene.raster_workspace[0x1002] = 1;
  CHECK(ActionRoomScene_BuildFrameState(&scene, &request, &state));
  CHECK(state.bg_hscroll[1][78] == 0x100);
  CHECK(state.bg_hscroll[1][79] == 16);
  CHECK(state.bg_hscroll[1][143] == 32);

  scene.raster_preset = kActionRoomRaster_R10;
  memset(scene.raster_workspace, 0, sizeof(scene.raster_workspace));
  scene.raster_workspace[2] = 2;
  scene.raster_workspace[0x802] = 1;
  CHECK(ActionRoomScene_BuildFrameState(&scene, &request, &state));
  CHECK(state.bg_hscroll[1][0] == 0x20D);
  CHECK(state.bg_hscroll[0][0] == 0x13C);
  CHECK(state.bg_hscroll[1][223] == 123);
  CHECK(state.bg_hscroll[0][223] == 206);
}

static void FillSolidCharacter(uint8_t *characters, unsigned tile,
                               uint8_t color) {
  uint8_t *target = characters + (size_t)tile * 32;
  for (unsigned row = 0; row < 8; row++) {
    if (color & 1) target[row * 2] = 0xff;
    if (color & 2) target[row * 2 + 1] = 0xff;
    if (color & 4) target[16 + row * 2] = 0xff;
    if (color & 8) target[16 + row * 2 + 1] = 0xff;
  }
}

static void TestNativeFrameCompositor(void) {
  ActionRoomScene scene;
  InitFrameScene(&scene);
  scene.have_character_bank[0] = true;
  scene.have_character_bank[1] = true;
  scene.have_palette = true;
  scene.video_profile[9] = 0x11;
  scene.video_profile[10] = 0x11;
  FillSolidCharacter(scene.characters, 0, 1);
  FillSolidCharacter(scene.characters, 0x100, 2);
  /* BG1's permanent palette-4 attribute makes colour 1 index $41. BG2's
   * permanent tile-$100 attribute selects colour 2 at index $02. */
  scene.palette[0x41 * 2] = 0x1f;
  scene.palette[0x02 * 2 + 1] = 0x7c;

  const ActionRoomSceneFrameRequest request = {
    .animation_phase = -1,
    .page_phase = -1,
  };
  ActionRoomSceneFrameState state;
  uint32_t *pixels = malloc(
      kActionRoomSceneFramePixels * sizeof(*pixels));
  CHECK(pixels != NULL);
  if (!pixels) return;

  scene.video_profile[0] = 3;
  scene.video_profile[1] = 0;
  scene.video_profile[2] = 0;
  scene.video_profile[3] = 0;
  scene.video_profile[4] = 0;
  CHECK(ActionRoomScene_BuildFrameState(&scene, &request, &state));
  CHECK(ActionRoomScene_RenderNativeFrame(
      &scene, &state, pixels, kActionRoomSceneFramePixels));
  CHECK(pixels[0] == 0xffff0000u);  /* BG1 low outranks BG2 low. */

  scene.video_profile[4] = 2;
  CHECK(ActionRoomScene_BuildFrameState(&scene, &request, &state));
  CHECK(ActionRoomScene_RenderNativeFrame(
      &scene, &state, pixels, kActionRoomSceneFramePixels));
  CHECK(pixels[1234] == 0xff0000ffu);  /* Forced-high BG2 wins. */

  scene.video_profile[0] = 2;
  scene.video_profile[1] = 1;
  scene.video_profile[2] = 2;
  scene.video_profile[3] = 0x42;
  CHECK(ActionRoomScene_BuildFrameState(&scene, &request, &state));
  CHECK(ActionRoomScene_RenderNativeFrame(
      &scene, &state, pixels, kActionRoomSceneFramePixels));
  CHECK(pixels[kActionRoomSceneFramePixels - 1] == 0xff7b007bu);
  free(pixels);
}

static void TestStockRomCatalogue(const char *path) {
  static const uint8_t kExpectedProfiles[8][9] = {
    {0},
    {0, 0x03, 0x04, 0x05, 0x06},
    {0, 0x07, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F},
    {0, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15},
    {0, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C},
    {0, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24},
    {0, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C},
    {0, 0x2D, 0x06, 0x0F, 0x15, 0x1C, 0x24, 0x2C, 0x2E},
  };
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
  int decoded = 0, scenes = 0, rendered = 0, animated = 0;
  int page_cycles = 0, raster = 0;
  int forced_bg2_priority = 0;
  for (uint8_t group = 1; group <= 7; group++) {
    for (uint8_t map = 1; map <= ActRaiser_ActionMapLast(group); map++) {
      ActionRoomScene scene;
      if (!ActionRoomScene_Load(
              &scene, rom, (size_t)length, group, map)) {
        printf("FAIL stock room scene %02X/%02X\n", group, map);
        failures++;
      } else {
        scenes++;
        CHECK(scene.have_video_profile);
        CHECK(scene.have_raster_waveform);
        CHECK(scene.video_profile_index == kExpectedProfiles[group][map]);
        CHECK((scene.video_profile[4] & 0x04) != 0);
        ActionRoomSceneFrameState state;
        const ActionRoomSceneFrameRequest request = {
          .game_frame = 37,
          .animation_phase = -1,
          .page_phase = -1,
        };
        CHECK(ActionRoomScene_BuildFrameState(&scene, &request, &state));
        if (ActionRoomScene_RenderNativeFrame(
                &scene, &state, pixels, kActionRoomSceneFramePixels))
          rendered++;
        else {
          printf("FAIL stock native frame %02X/%02X\n", group, map);
          failures++;
        }
        if (ActionRoomScene_HasCharacterAnimation(&scene)) animated++;
        if (ActionRoomScene_HasBg2PageCycle(&scene)) page_cycles++;
        if (scene.raster_preset != kActionRoomRaster_None) raster++;
        if (scene.video_profile[4] & 0x02) forced_bg2_priority++;
      }
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
  CHECK(scenes == 49);
  CHECK(rendered == 49);
  CHECK(animated == 30);
  CHECK(page_cycles == 2);
  CHECK(raster == 17);
  CHECK(forced_bg2_priority == 5);
  free(pixels);
  free(rom);
}

int main(int argc, char **argv) {
  TestLiterals();
  TestOverlappingDictionaryCopyAndTruncation();
  TestGenericRoomScriptAndInheritance();
  TestScenePhaseResolvers();
  TestFrameStateAndRasterPresets();
  TestNativeFrameCompositor();
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
