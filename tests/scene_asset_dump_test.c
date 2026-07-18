#define _POSIX_C_SOURCE 200809L

#include "scene_asset_dump.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "snes/ppu.h"

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

  Ppu *ppu = (Ppu *)calloc(1, sizeof(Ppu));
  uint8_t *wram = (uint8_t *)calloc(1, 0x20000);
  CHECK(ppu != NULL && wram != NULL);
  if (!ppu || !wram) return 1;
  ppu->bgmode = 1;
  ppu->screenEnabled[0] = 0x17;
  ppu->bgXsc[0] = 0x60;
  ppu->bgXsc[1] = 0x70;
  ppu->bgXsc[2] = 0x78;
  ppu->bgTileAdr = 0x0500;
  ppu->obsel = 3;
  ppu->cgram[1] = 0x001f;
  ppu->cgram[0x81] = 0x7c00;
  /* One opaque pixel in planar tile 1 at BG1's base. */
  ppu->vram[16] = 0x0080;
  ppu->vram[0x6000] = 1;
  wram[0x18] = 1;
  wram[0x19] = 2;
  wram[0x88] = 0x34;
  wram[0x89] = 0x12;

  CHECK(SceneAssetDump_Write(directory, ppu, wram, 5678));
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
  CHECK(FileSize(directory, "vram.bin") == (long)sizeof(ppu->vram));
  CHECK(FileSize(directory, "cgram.bin") == (long)sizeof(ppu->cgram));
  CHECK(FileSize(directory, "oam.bin") ==
        (long)(sizeof(ppu->oam) + sizeof(ppu->highOam)));
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
  free(ppu);
  free(wram);
  if (s_failures) {
    fprintf(stderr, "%d scene asset dump test(s) failed\n", s_failures);
    return 1;
  }
  puts("scene asset dump tests passed");
  return 0;
}
