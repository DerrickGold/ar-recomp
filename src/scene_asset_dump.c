#include "scene_asset_dump.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "snes/ppu.h"

enum {
  kRgbaChannels = 4,
  kObjAtlasWidth = 16 * 8,
  kObjAtlasHeight = 2 * 8 * 16 * 8,
  kOamSheetWidth = 16 * 64,
  kOamSheetHeight = 8 * 64,
};

typedef struct BgDump {
  int layer;
  int bpp;
  int width;
  int height;
  int map_base;
  int tile_base;
  int enabled_main;
  int enabled_sub;
  char file[24];
} BgDump;

static const uint8_t kSpriteSizes[8][2] = {
  {8, 16}, {8, 32}, {8, 64}, {16, 32},
  {16, 64}, {32, 64}, {16, 32}, {16, 32},
};

static void BuildPath(char *out, size_t out_size, const char *directory,
                      const char *name) {
  snprintf(out, out_size, "%s/%s", directory, name);
}

static bool MakeDirectory(const char *path) {
#ifdef _WIN32
  if (_mkdir(path) == 0 || errno == EEXIST) return true;
#else
  if (mkdir(path, 0755) == 0 || errno == EEXIST) return true;
#endif
  fprintf(stderr, "[scene-assets] cannot create %s: %s\n",
          path, strerror(errno));
  return false;
}

static uint32_t CrcTableEntry(uint32_t value) {
  for (int bit = 0; bit < 8; bit++)
    value = (value >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(value & 1));
  return value;
}

static uint32_t UpdateCrc(uint32_t crc, const uint8_t *data, size_t size) {
  static uint32_t table[256];
  static bool initialized;
  if (!initialized) {
    for (int i = 0; i < 256; i++) table[i] = CrcTableEntry((uint32_t)i);
    initialized = true;
  }
  for (size_t i = 0; i < size; i++)
    crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
  return crc;
}

static void PutBe32(uint8_t *out, uint32_t value) {
  out[0] = (uint8_t)(value >> 24);
  out[1] = (uint8_t)(value >> 16);
  out[2] = (uint8_t)(value >> 8);
  out[3] = (uint8_t)value;
}

static bool WriteChunk(FILE *file, const char type[4],
                       const uint8_t *data, size_t size) {
  if (size > 0xffffffffu) return false;
  uint8_t header[8];
  PutBe32(header, (uint32_t)size);
  memcpy(header + 4, type, 4);
  uint32_t crc = UpdateCrc(0xffffffffu, header + 4, 4);
  crc = UpdateCrc(crc, data, size) ^ 0xffffffffu;
  uint8_t crc_bytes[4];
  PutBe32(crc_bytes, crc);
  return fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
         (!size || fwrite(data, 1, size, file) == size) &&
         fwrite(crc_bytes, 1, sizeof(crc_bytes), file) == sizeof(crc_bytes);
}

/* PNG's IDAT payload is a zlib stream. Stored DEFLATE blocks keep this tiny
 * writer dependency-free and lossless; asset dumps favor exactness and
 * portability over compression ratio. */
bool WritePng(const char *path, const uint8_t *rgba,
              int width, int height) {
  if (!path || !rgba || width <= 0 || height <= 0) return false;
  size_t stride = (size_t)width * kRgbaChannels;
  if (stride / kRgbaChannels != (size_t)width) return false;
  size_t raw_size = (stride + 1) * (size_t)height;
  if (raw_size / (size_t)height != stride + 1) return false;
  uint8_t *raw = (uint8_t *)malloc(raw_size);
  if (!raw) return false;
  for (int y = 0; y < height; y++) {
    size_t offset = (size_t)y * (stride + 1);
    raw[offset] = 0;
    memcpy(raw + offset + 1, rgba + (size_t)y * stride, stride);
  }

  size_t blocks = (raw_size + 65534) / 65535;
  size_t z_size = 2 + raw_size + blocks * 5 + 4;
  uint8_t *z = (uint8_t *)malloc(z_size);
  if (!z) {
    free(raw);
    return false;
  }
  size_t at = 0, source = 0;
  z[at++] = 0x78;
  z[at++] = 0x01;
  while (source < raw_size) {
    size_t remaining = raw_size - source;
    uint16_t length = (uint16_t)(remaining > 65535 ? 65535 : remaining);
    bool final = source + length == raw_size;
    z[at++] = final ? 1 : 0;
    z[at++] = (uint8_t)length;
    z[at++] = (uint8_t)(length >> 8);
    uint16_t inverse = (uint16_t)~length;
    z[at++] = (uint8_t)inverse;
    z[at++] = (uint8_t)(inverse >> 8);
    memcpy(z + at, raw + source, length);
    at += length;
    source += length;
  }
  uint32_t s1 = 1, s2 = 0;
  for (size_t i = 0; i < raw_size; i++) {
    s1 = (s1 + raw[i]) % 65521u;
    s2 = (s2 + s1) % 65521u;
  }
  PutBe32(z + at, (s2 << 16) | s1);
  at += 4;
  free(raw);

  FILE *file = fopen(path, "wb");
  if (!file) {
    free(z);
    return false;
  }
  static const uint8_t signature[8] = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
  };
  uint8_t ihdr[13] = {0};
  PutBe32(ihdr, (uint32_t)width);
  PutBe32(ihdr + 4, (uint32_t)height);
  ihdr[8] = 8;
  ihdr[9] = 6; /* RGBA */
  bool ok = fwrite(signature, 1, sizeof(signature), file) == sizeof(signature) &&
            WriteChunk(file, "IHDR", ihdr, sizeof(ihdr)) &&
            WriteChunk(file, "IDAT", z, at) &&
            WriteChunk(file, "IEND", NULL, 0);
  if (fclose(file) != 0) ok = false;
  free(z);
  if (!ok) fprintf(stderr, "[scene-assets] cannot write %s\n", path);
  return ok;
}

static uint8_t Expand5(int value) {
  value &= 31;
  return (uint8_t)((value << 3) | (value >> 2));
}

static void PutPalettePixel(const Ppu *ppu, uint8_t *rgba, int palette_index,
                            bool transparent_zero) {
  uint16_t xbgr = ppu->cgram[palette_index & 0xff] & 0x7fff;
  rgba[0] = Expand5(xbgr);
  rgba[1] = Expand5(xbgr >> 5);
  rgba[2] = Expand5(xbgr >> 10);
  rgba[3] = transparent_zero ? 0 : 255;
}

static int PlanarPixel(const Ppu *ppu, int word_address, int bpp,
                       int x, int y) {
  int shift = 7 - (x & 7);
  int pixel = 0;
  for (int pair = 0; pair < bpp / 2; pair++) {
    uint16_t planes = ppu->vram[(word_address + pair * 8 + (y & 7)) & 0x7fff];
    pixel |= ((planes >> shift) & 1) << (pair * 2);
    pixel |= ((planes >> (shift + 8)) & 1) << (pair * 2 + 1);
  }
  return pixel;
}

static int BgBpp(int mode, int layer) {
  static const int table[7][4] = {
    {2, 2, 2, 2}, {4, 4, 2, 0}, {4, 4, 0, 0},
    {8, 4, 0, 0}, {8, 2, 0, 0}, {4, 2, 0, 0},
    {4, 0, 0, 0},
  };
  return mode >= 0 && mode < 7 && layer >= 0 && layer < 4
      ? table[mode][layer] : 0;
}

static int TilemapIndex(int tx, int ty, bool wider, bool higher) {
  int index = (ty & 31) * 32 + (tx & 31);
  if ((tx & 32) && wider) index += 0x400;
  if ((ty & 32) && higher) index += wider ? 0x800 : 0x400;
  return index;
}

static bool WritePlanarBackground(const char *directory, const Ppu *ppu,
                                  int mode, int layer, BgDump *dump) {
  int bpp = BgBpp(mode, layer);
  if (!bpp) return false;
  bool wider = PPU_bgTilemapWider(ppu, layer) != 0;
  bool higher = PPU_bgTilemapHigher(ppu, layer) != 0;
  int tile_width = wider ? 64 : 32;
  int tile_height = higher ? 64 : 32;
  int width = tile_width * 8;
  int height = tile_height * 8;
  size_t byte_count = (size_t)width * height * kRgbaChannels;
  uint8_t *pixels = (uint8_t *)calloc(1, byte_count);
  if (!pixels) return false;
  int map_base = PPU_bgTilemapAdr(ppu, layer);
  int tile_base = PPU_bgTileAdr(ppu, layer);
  int words_per_tile = bpp * 4;
  for (int ty = 0; ty < tile_height; ty++) {
    for (int tx = 0; tx < tile_width; tx++) {
      int map_address = (map_base + TilemapIndex(tx, ty, wider, higher)) & 0x7fff;
      uint16_t entry = ppu->vram[map_address];
      int tile = entry & 0x3ff;
      int palette = (entry >> 10) & 7;
      bool hflip = (entry & 0x4000) != 0;
      bool vflip = (entry & 0x8000) != 0;
      int char_address = (tile_base + tile * words_per_tile) & 0x7fff;
      for (int py = 0; py < 8; py++) {
        for (int px = 0; px < 8; px++) {
          int sx = hflip ? 7 - px : px;
          int sy = vflip ? 7 - py : py;
          int pixel = PlanarPixel(ppu, char_address, bpp, sx, sy);
          int palette_base = bpp == 8 ? 0 : palette * (1 << bpp);
          if (mode == 0) palette_base += layer * 32;
          uint8_t *out = pixels +
              ((size_t)(ty * 8 + py) * width + tx * 8 + px) * 4;
          PutPalettePixel(ppu, out, palette_base + pixel, pixel == 0);
        }
      }
    }
  }
  snprintf(dump->file, sizeof(dump->file), "bg%d.png", layer + 1);
  char path[512];
  BuildPath(path, sizeof(path), directory, dump->file);
  bool ok = WritePng(path, pixels, width, height);
  free(pixels);
  *dump = (BgDump){
    .layer = layer + 1, .bpp = bpp, .width = width, .height = height,
    .map_base = map_base, .tile_base = tile_base,
    .enabled_main = (ppu->screenEnabled[0] >> layer) & 1,
    .enabled_sub = (ppu->screenEnabled[1] >> layer) & 1,
  };
  snprintf(dump->file, sizeof(dump->file), "bg%d.png", layer + 1);
  return ok;
}

static bool WriteMode7Background(const char *directory, const Ppu *ppu,
                                 BgDump *dump) {
  enum { kCanvas = 1024 };
  uint8_t *pixels = (uint8_t *)calloc(
      (size_t)kCanvas * kCanvas, kRgbaChannels);
  if (!pixels) return false;
  for (int ty = 0; ty < 128; ty++) {
    for (int tx = 0; tx < 128; tx++) {
      int tile = ppu->vram[ty * 128 + tx] & 0xff;
      for (int py = 0; py < 8; py++) {
        for (int px = 0; px < 8; px++) {
          int char_address = (tile * 64 + py * 8 + px) & 0x7fff;
          int pixel = ppu->vram[char_address] >> 8;
          uint8_t *out = pixels +
              ((size_t)(ty * 8 + py) * kCanvas + tx * 8 + px) * 4;
          PutPalettePixel(ppu, out, pixel, pixel == 0);
        }
      }
    }
  }
  char path[512];
  BuildPath(path, sizeof(path), directory, "bg1_mode7.png");
  bool ok = WritePng(path, pixels, kCanvas, kCanvas);
  free(pixels);
  *dump = (BgDump){
    .layer = 1, .bpp = 8, .width = kCanvas, .height = kCanvas,
    .map_base = 0, .tile_base = 0,
    .enabled_main = ppu->screenEnabled[0] & 1,
    .enabled_sub = ppu->screenEnabled[1] & 1,
  };
  snprintf(dump->file, sizeof(dump->file), "bg1_mode7.png");
  return ok;
}

static bool WritePalette(const char *directory, const Ppu *ppu) {
  enum { kSwatch = 16, kSize = 16 * kSwatch };
  uint8_t *pixels = (uint8_t *)malloc((size_t)kSize * kSize * 4);
  if (!pixels) return false;
  for (int y = 0; y < kSize; y++) {
    for (int x = 0; x < kSize; x++) {
      int index = (y / kSwatch) * 16 + x / kSwatch;
      PutPalettePixel(ppu, pixels + ((size_t)y * kSize + x) * 4,
                      index, false);
    }
  }
  char path[512];
  BuildPath(path, sizeof(path), directory, "palette.png");
  bool ok = WritePng(path, pixels, kSize, kSize);
  free(pixels);
  return ok;
}

static bool WriteObjAtlas(const char *directory, const Ppu *ppu) {
  uint8_t *pixels = (uint8_t *)calloc(
      (size_t)kObjAtlasWidth * kObjAtlasHeight, 4);
  if (!pixels) return false;
  for (int name_select = 0; name_select < 2; name_select++) {
    int base = name_select ? PPU_objTileAdr2(ppu) : PPU_objTileAdr1(ppu);
    for (int palette = 0; palette < 8; palette++) {
      int section = name_select * 8 + palette;
      for (int tile = 0; tile < 256; tile++) {
        int char_address = (base + tile * 16) & 0x7fff;
        int tile_x = (tile & 15) * 8;
        int tile_y = section * 128 + (tile >> 4) * 8;
        for (int py = 0; py < 8; py++) {
          for (int px = 0; px < 8; px++) {
            int pixel = PlanarPixel(ppu, char_address, 4, px, py);
            uint8_t *out = pixels +
                ((size_t)(tile_y + py) * kObjAtlasWidth + tile_x + px) * 4;
            PutPalettePixel(ppu, out, 0x80 + palette * 16 + pixel,
                            pixel == 0);
          }
        }
      }
    }
  }
  char path[512];
  BuildPath(path, sizeof(path), directory, "obj_tiles.png");
  bool ok = WritePng(path, pixels, kObjAtlasWidth, kObjAtlasHeight);
  free(pixels);
  return ok;
}

static bool WriteOamSheet(const char *directory, const Ppu *ppu) {
  uint8_t *pixels = (uint8_t *)calloc(
      (size_t)kOamSheetWidth * kOamSheetHeight, 4);
  if (!pixels) return false;
  for (int slot = 0; slot < 128; slot++) {
    int index = slot * 2;
    int oam1 = ppu->oam[index + 1];
    int size_bit =
        (ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1;
    int size = kSpriteSizes[PPU_objSize(ppu)][size_bit];
    bool hflip = (oam1 & 0x4000) != 0;
    bool vflip = (oam1 & 0x8000) != 0;
    int base = (oam1 & 0x100) ? PPU_objTileAdr2(ppu)
                              : PPU_objTileAdr1(ppu);
    int palette = (oam1 >> 9) & 7;
    int cell_x = (slot & 15) * 64;
    int cell_y = (slot >> 4) * 64;
    for (int py = 0; py < size; py++) {
      for (int px = 0; px < size; px++) {
        int used_x = hflip ? size - 1 - px : px;
        int used_y = vflip ? size - 1 - py : py;
        int tile = ((((oam1 & 0xff) >> 4) + (used_y >> 3)) << 4) |
                   (((oam1 & 15) + (used_x >> 3)) & 15);
        tile &= 0xff;
        int char_address = (base + tile * 16) & 0x7fff;
        int pixel = PlanarPixel(ppu, char_address, 4,
                                used_x & 7, used_y & 7);
        uint8_t *out = pixels +
            ((size_t)(cell_y + py) * kOamSheetWidth + cell_x + px) * 4;
        PutPalettePixel(ppu, out, 0x80 + palette * 16 + pixel,
                        pixel == 0);
      }
    }
  }
  char path[512];
  BuildPath(path, sizeof(path), directory, "oam_sprites.png");
  bool ok = WritePng(path, pixels, kOamSheetWidth, kOamSheetHeight);
  free(pixels);
  return ok;
}

static bool WriteBinary(const char *directory, const char *name,
                        const void *data, size_t size) {
  char path[512];
  BuildPath(path, sizeof(path), directory, name);
  FILE *file = fopen(path, "wb");
  bool ok = file && fwrite(data, 1, size, file) == size;
  if (file && fclose(file) != 0) ok = false;
  if (!ok) fprintf(stderr, "[scene-assets] cannot write %s\n", path);
  return ok;
}

static bool WriteOamBinary(const char *directory, const Ppu *ppu) {
  char path[512];
  BuildPath(path, sizeof(path), directory, "oam.bin");
  FILE *file = fopen(path, "wb");
  bool ok = file &&
      fwrite(ppu->oam, 1, sizeof(ppu->oam), file) == sizeof(ppu->oam) &&
      fwrite(ppu->highOam, 1, sizeof(ppu->highOam), file) ==
          sizeof(ppu->highOam);
  if (file && fclose(file) != 0) ok = false;
  if (!ok) fprintf(stderr, "[scene-assets] cannot write %s\n", path);
  return ok;
}

static bool WriteMetadata(const char *directory, const Ppu *ppu,
                          const uint8_t *wram, int host_frame,
                          const BgDump *backgrounds, int background_count) {
  char path[512];
  BuildPath(path, sizeof(path), directory, "metadata.json");
  FILE *file = fopen(path, "w");
  if (!file) return false;
  unsigned game_frame = wram ? wram[0x88] | (wram[0x89] << 8) : 0;
  unsigned state18 = wram ? wram[0x18] : 0;
  unsigned state19 = wram ? wram[0x19] : 0;
  fprintf(file,
      "{\n  \"format\": 1,\n  \"host_frame\": %d,\n"
      "  \"game_frame\": %u,\n  \"state_18\": %u,\n"
      "  \"state_19\": %u,\n  \"ppu\": {\n"
      "    \"mode\": %u, \"bgmode_raw\": %u, \"brightness\": %u,\n"
      "    \"main_screen\": %u, \"sub_screen\": %u,\n"
      "    \"obsel\": %u, \"obj_base_1_word\": %u, "
      "\"obj_base_2_word\": %u\n  },\n  \"backgrounds\": [\n",
      host_frame, game_frame, state18, state19, PPU_mode(ppu), ppu->bgmode,
      PPU_brightness(ppu), ppu->screenEnabled[0], ppu->screenEnabled[1],
      ppu->obsel, PPU_objTileAdr1(ppu), PPU_objTileAdr2(ppu));
  for (int i = 0; i < background_count; i++) {
    const BgDump *bg = &backgrounds[i];
    fprintf(file,
        "    %s{\"layer\": %d, \"file\": \"%s\", \"bpp\": %d, "
        "\"width\": %d, \"height\": %d, \"map_base_word\": %d, "
        "\"tile_base_word\": %d, \"main\": %d, \"sub\": %d}",
        i ? ",\n" : "", bg->layer, bg->file, bg->bpp, bg->width,
        bg->height, bg->map_base, bg->tile_base,
        bg->enabled_main, bg->enabled_sub);
  }
  fprintf(file,
      "\n  ],\n  \"palette\": {\"file\": \"palette.png\", "
      "\"layout\": \"16x16 CGRAM entries; each swatch is 16x16 pixels\"},\n"
      "  \"obj_tiles\": {\"file\": \"obj_tiles.png\", "
      "\"width\": %d, \"height\": %d, "
      "\"layout\": \"16 sections: name-select 0 palettes 0-7, then "
      "name-select 1 palettes 0-7; each section is a 16x16 tile sheet\"},\n"
      "  \"oam_sheet\": {\"file\": \"oam_sprites.png\", "
      "\"width\": %d, \"height\": %d, \"count\": 128, "
      "\"layout\": \"16 columns x 8 rows; one 64x64 cell per OAM slot\"},\n"
      "  \"oam\": [\n",
      kObjAtlasWidth, kObjAtlasHeight, kOamSheetWidth, kOamSheetHeight);
  for (int slot = 0; slot < 128; slot++) {
    int index = slot * 2;
    int x_raw = ppu->oam[index] & 0xff;
    x_raw |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
    int x = x_raw;
    if (x >= 256 + ppu->extraRightCur) x -= 512;
    int y_raw = ppu->oam[index] >> 8;
    int y = y_raw >= 224 ? y_raw - 256 : y_raw;
    int oam1 = ppu->oam[index + 1];
    int size_bit =
        (ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1;
    int size = kSpriteSizes[PPU_objSize(ppu)][size_bit];
    fprintf(file,
        "    %s{\"slot\": %d, \"x\": %d, \"y\": %d, "
        "\"x_raw_9bit\": %d, \"y_raw\": %d, "
        "\"size\": %d, \"tile\": %u, \"name_select\": %u, "
        "\"palette\": %u, \"priority\": %u, \"hflip\": %u, "
        "\"vflip\": %u}",
        slot ? ",\n" : "", slot, x, y, x_raw, y_raw, size, oam1 & 0xff,
        (oam1 >> 8) & 1, (oam1 >> 9) & 7, (oam1 >> 12) & 3,
        (oam1 >> 14) & 1, (oam1 >> 15) & 1);
  }
  fprintf(file,
      "\n  ],\n  \"raw\": {\"vram\": \"vram.bin\", "
      "\"cgram\": \"cgram.bin\", \"oam\": \"oam.bin\", "
      "\"wram\": \"wram.bin\"}\n}\n");
  bool ok = fclose(file) == 0;
  if (!ok) fprintf(stderr, "[scene-assets] cannot write %s\n", path);
  return ok;
}

bool SceneAssetDump_Write(const char *directory, const Ppu *ppu,
                          const uint8_t *wram, int host_frame) {
  if (!directory || !directory[0] || !ppu || !MakeDirectory(directory))
    return false;

  bool ok = true;
  BgDump backgrounds[4];
  int background_count = 0;
  int mode = PPU_mode(ppu);
  if (mode == 7) {
    ok &= WriteMode7Background(directory, ppu,
                               &backgrounds[background_count++]);
  } else {
    for (int layer = 0; layer < 4; layer++) {
      if (!BgBpp(mode, layer)) continue;
      ok &= WritePlanarBackground(directory, ppu, mode, layer,
                                  &backgrounds[background_count++]);
    }
  }
  ok &= WritePalette(directory, ppu);
  ok &= WriteObjAtlas(directory, ppu);
  ok &= WriteOamSheet(directory, ppu);
  ok &= WriteBinary(directory, "vram.bin", ppu->vram, sizeof(ppu->vram));
  ok &= WriteBinary(directory, "cgram.bin", ppu->cgram, sizeof(ppu->cgram));
  ok &= WriteOamBinary(directory, ppu);
  if (wram) ok &= WriteBinary(directory, "wram.bin", wram, 0x20000);
  ok &= WriteMetadata(directory, ppu, wram, host_frame,
                      backgrounds, background_count);
  if (ok)
    fprintf(stderr, "[scene-assets] complete resident asset dump -> %s/\n",
            directory);
  return ok;
}
