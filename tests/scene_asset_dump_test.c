#define _POSIX_C_SOURCE 200809L

#include "scene_asset_dump.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

static int s_failures;

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

static uint32_t ReadBe32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
         (uint32_t)bytes[2] << 8 | bytes[3];
}

static void CheckPngSize(const char *directory, const char *name,
                         int width, int height) {
  char path[320];
  snprintf(path, sizeof(path), "%s/%s", directory, name);
  FILE *file = fopen(path, "rb");
  CHECK(file != NULL);
  if (!file) return;
  uint8_t header[24] = {0};
  CHECK(fread(header, 1, sizeof(header), file) == sizeof(header));
  fclose(file);
  static const uint8_t signature[8] = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
  };
  CHECK(!memcmp(header, signature, sizeof(signature)));
  CHECK(!memcmp(header + 12, "IHDR", 4));
  CHECK((int)ReadBe32(header + 16) == width);
  CHECK((int)ReadBe32(header + 20) == height);
  int decoded_width = 0, decoded_height = 0, channels = 0;
  uint8_t *decoded = stbi_load(path, &decoded_width, &decoded_height,
                               &channels, 4);
  CHECK(decoded != NULL);
  CHECK(decoded_width == width && decoded_height == height);
  stbi_image_free(decoded);
}

static long FileSize(const char *directory, const char *name) {
  char path[320];
  snprintf(path, sizeof(path), "%s/%s", directory, name);
  FILE *file = fopen(path, "rb");
  if (!file) return -1;
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fclose(file);
  return size;
}

int main(void) {
  char directory[256];
  snprintf(directory, sizeof(directory),
           "/tmp/actraiser-scene-assets-%ld", (long)getpid());

  uint16_t *vram = (uint16_t *)calloc(
      (size_t)SR_PPU_VRAM_WORD_COUNT, sizeof(uint16_t));
  uint16_t *cgram = (uint16_t *)calloc(
      (size_t)SR_PPU_CGRAM_WORD_COUNT, sizeof(uint16_t));
  uint16_t *oam = (uint16_t *)calloc(
      (size_t)SR_PPU_OAM_WORD_COUNT, sizeof(uint16_t));
  uint8_t *high_oam = (uint8_t *)calloc(
      (size_t)SR_PPU_HIGH_OAM_BYTE_COUNT, 1u);
  uint8_t *wram = (uint8_t *)calloc(1, 0x20000);
  CHECK(vram != NULL && cgram != NULL && oam != NULL &&
        high_oam != NULL && wram != NULL);
  if (!vram || !cgram || !oam || !high_oam || !wram) return 1;
  SceneAssetDumpSource source = {
    .ppu = {
      .struct_size = sizeof(SrPpuStateSnapshot),
      .object_select = 3,
      .bg_mode_control = 1,
      .bg_mode = 1,
      .main_screen = 0x17,
      .object_size_select = 0,
      .object_tile_base_1_word = 0x6000,
      .object_tile_base_2_word = 0x7000,
      .backgrounds = {
        {0, 0, 0x6000, 0x0000, 32, 32, 8, 4},
        {0, 0, 0x7000, 0x0000, 32, 32, 8, 4},
        {0, 0, 0x7800, 0x5000, 32, 32, 8, 2},
        {0, 0, 0x0000, 0x0000, 32, 32, 8, 0},
      },
    },
    .vram = {sizeof(SrBorrowedU16Span), SR_MEMORY_VRAM, vram,
             SR_PPU_VRAM_WORD_COUNT, 0},
    .cgram = {sizeof(SrBorrowedU16Span), SR_MEMORY_CGRAM, cgram,
              SR_PPU_CGRAM_WORD_COUNT, 0},
    .oam = {sizeof(SrBorrowedU16Span), SR_MEMORY_OAM, oam,
            SR_PPU_OAM_WORD_COUNT, 0},
    .high_oam = {sizeof(SrBorrowedSpan), SR_MEMORY_HIGH_OAM, high_oam,
                 SR_PPU_HIGH_OAM_BYTE_COUNT, 0},
    .wram = {sizeof(SrBorrowedSpan), SR_MEMORY_WRAM, wram, 0x20000, 0},
  };
  cgram[1] = 0x001f;
  cgram[0x81] = 0x7c00;
  /* One opaque pixel in planar tile 1 at BG1's base. */
  vram[16] = 0x0080;
  vram[0x6000] = 1;
  wram[0x18] = 1;
  wram[0x19] = 2;
  wram[0x88] = 0x34;
  wram[0x89] = 0x12;

  source.vram.lifetime_generation = 1;
  CHECK(!SceneAssetDump_Write(directory, &source, 5678));
  source.vram.lifetime_generation = 0;
  CHECK(SceneAssetDump_Write(directory, &source, 5678));
  CheckPngSize(directory, "bg1.png", 256, 256);
  CheckPngSize(directory, "bg2.png", 256, 256);
  CheckPngSize(directory, "bg3.png", 256, 256);
  CheckPngSize(directory, "palette.png", 256, 256);
  CheckPngSize(directory, "obj_tiles.png", 128, 2048);
  CheckPngSize(directory, "oam_sprites.png", 1024, 512);
  {
    char bg1_path[320];
    snprintf(bg1_path, sizeof(bg1_path), "%s/bg1.png", directory);
    int width = 0, height = 0, channels = 0;
    uint8_t *pixels = stbi_load(bg1_path, &width, &height, &channels, 4);
    CHECK(pixels != NULL);
    if (pixels) {
      CHECK(pixels[0] == 255 && pixels[1] == 0 && pixels[2] == 0 &&
            pixels[3] == 255);
      stbi_image_free(pixels);
    }
  }
  CHECK(FileSize(directory, "vram.bin") ==
        (long)(SR_PPU_VRAM_WORD_COUNT * sizeof(uint16_t)));
  CHECK(FileSize(directory, "cgram.bin") ==
        (long)(SR_PPU_CGRAM_WORD_COUNT * sizeof(uint16_t)));
  CHECK(FileSize(directory, "oam.bin") ==
        (long)(SR_PPU_OAM_WORD_COUNT * sizeof(uint16_t) +
               SR_PPU_HIGH_OAM_BYTE_COUNT));
  CHECK(FileSize(directory, "wram.bin") == 0x20000);

  char metadata_path[320];
  snprintf(metadata_path, sizeof(metadata_path), "%s/metadata.json", directory);
  FILE *metadata = fopen(metadata_path, "rb");
  CHECK(metadata != NULL);
  if (metadata) {
    fseek(metadata, 0, SEEK_END);
    long size = ftell(metadata);
    fseek(metadata, 0, SEEK_SET);
    char *text = (char *)malloc((size_t)size + 1);
    CHECK(text != NULL);
    if (text) {
      CHECK(fread(text, 1, (size_t)size, metadata) == (size_t)size);
      text[size] = 0;
      CHECK(strstr(text, "\"game_frame\": 4660") != NULL);
      CHECK(strstr(text, "\"count\": 128") != NULL);
      CHECK(strstr(text, "\"file\": \"obj_tiles.png\"") != NULL);
      free(text);
    }
    fclose(metadata);
  }

  static const char *const files[] = {
    "bg1.png", "bg2.png", "bg3.png", "palette.png", "obj_tiles.png",
    "oam_sprites.png", "vram.bin", "cgram.bin", "oam.bin", "wram.bin",
    "metadata.json",
  };
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
    remove(path);
  }
  rmdir(directory);
  free(vram);
  free(cgram);
  free(oam);
  free(high_oam);
  free(wram);
  if (s_failures) {
    fprintf(stderr, "%d scene asset dump test(s) failed\n", s_failures);
    return 1;
  }
  puts("scene asset dump tests passed");
  return 0;
}
