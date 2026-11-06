/* Renderer row decoding must exactly match the immutable native packet
 * oracle, including alpha/policy bits. No ROM, GPU or live PPU is needed. */
#include "diorama/diorama_bg_decode.h"
#include "support/test_assert.h"
#include <stdio.h>

static SrPpuBgPacket packet;
static unsigned rows_checked, pixels_checked;
static uint32_t random_state = UINT32_C(0x7e296b41);

static uint32_t Random(void) {
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return random_state;
}

static void Begin(unsigned width, unsigned height) {
  SrPpuBgPacket_Begin(&packet, width, height);
  packet.request_flags = SR_PPU_BG_PACKET_TILES;
  packet.owned_sources = packet.words[2] = 3;
}

static void Palette(unsigned source, unsigned y) {
  uint32_t colors[256];
  for (unsigned i = 0; i < 256; ++i)
    colors[i] = ((i & 3u) * 85u << 24) | ((i * 31u + y * 47u) & 255u) << 16 |
        ((i * 13u + source * 73u) & 255u) << 8 | ((i * 7u + y * 29u) & 255u);
  assert(SrPpuBgPacket_SetPalette(&packet, source, y, colors));
}

static uint32_t *Row(unsigned source, unsigned y, unsigned format, unsigned phase) {
  uint32_t *meta = SrPpuBgPacket_Meta(&packet, source, y);
  meta[0] = format;
  meta[1] = UINT32_C(0x7d354679) ^ (y * UINT32_C(0x00112943));
  meta[2] = packet.words[0] > 4 ? 2 : 0;
  meta[3] = packet.words[0] > 4 ? packet.words[0] - 1 : packet.words[0];
  meta[4] = format >= 3 ? phase : 0;
  Palette(source, y);
  uint32_t *pixels = SrPpuBgPacket_AllocatePixels(&packet, source, y);
  assert(pixels);
  return pixels;
}

static void Cell(uint32_t *cell, unsigned lane, unsigned value,
                 unsigned palette, unsigned flags) {
  for (unsigned plane = 0; plane < 4; ++plane)
    if (value & (1u << plane)) cell[0] |= 1u << (lane + plane * 8);
  for (unsigned plane = 0; plane < 3; ++plane)
    if (palette & (1u << plane)) cell[1] |= 1u << (lane + plane * 8);
  cell[2] |= flags << lane;
}

static void Tiles(uint32_t *pixels, unsigned phase, unsigned flip,
                  unsigned visible, unsigned band, unsigned salt) {
  const unsigned count = (packet.words[0] + phase + 7u) / 8u;
  for (unsigned tile = 0; tile < count; ++tile) {
    pixels[tile * 2] = 0;
    pixels[tile * 2 + 1] = ((tile + salt) & 7u) | (band << 3) |
        (flip ? 32u : 0) | (visible << 8) | ((2u + tile % 13u) << 16);
    for (unsigned lane = 0; lane < 8; ++lane) {
      const unsigned value = (lane + tile * 5u + salt) & 15u;
      const unsigned bit = flip ? lane : 7u - lane;
      for (unsigned plane = 0; plane < 4; ++plane)
        if (value & (1u << plane)) pixels[tile * 2] |= 1u << (bit + plane * 8);
    }
  }
}

static void Check(unsigned source, unsigned band, unsigned y, unsigned width) {
  uint32_t output[SR_PPU_BG_PACKET_WIDTH + 2];
  memset(output, 0xa5, sizeof(output));
  DioramaBgDecode_Row(&packet, source, band, y, width, output + 1);
  assert(output[0] == UINT32_C(0xa5a5a5a5));
  for (unsigned x = 0; x < width; ++x) {
    const uint32_t expected = SrPpuBgPacket_Color(&packet, source, band, x, y);
    if (output[x + 1] != expected) {
      fprintf(stderr, "decode source=%u band=%u x=%u y=%u format=%u phase=%u got=%08x expected=%08x\n",
          source, band, x, y, SrPpuBgPacket_Meta(&packet, source, y)[0],
          SrPpuBgPacket_Meta(&packet, source, y)[4], output[x + 1], expected);
      assert(output[x + 1] == expected);
    }
  }
  for (unsigned x = width + 1; x < SR_PPU_BG_PACKET_WIDTH + 2; ++x)
    assert(output[x] == UINT32_C(0xa5a5a5a5));
  ++rows_checked;
  pixels_checked += width;
}

static void CheckBackgrounds(void) {
  assert(SrPpuBgPacket_Valid(&packet));
  for (unsigned source = 0; source < 2; ++source)
    for (unsigned y = 0; y < packet.words[1]; ++y)
      for (unsigned band = 0; band < 3; ++band)
        Check(source, band, y, packet.words[0]);
}

static void CompactLanes(void) {
  /* Every visibility mask, phase, flip and native band, with both sources
   * and the format4-without-edits case. Width19 crosses partial tile ends. */
  for (unsigned source = 0; source < 2; ++source)
    for (unsigned format = 3; format <= 4; ++format)
      for (unsigned phase = 0; phase < 8; ++phase)
        for (unsigned flip = 0; flip < 2; ++flip)
          for (unsigned visible = 0; visible < 256; ++visible)
            for (unsigned native_band = 0; native_band < 4; ++native_band) {
              Begin(19, 1);
              uint32_t *pixels = Row(source, 0, format, phase);
              Tiles(pixels, phase, flip, visible, native_band, visible + phase);
              assert(SrPpuBgPacket_Valid(&packet));
              for (unsigned band = 0; band < 3; ++band) Check(source, band, 0, 19);
            }
}

static void PaintedCells(void) {
  for (unsigned phase = 0; phase < 8; ++phase)
    for (unsigned owned = 0; owned < 256; ++owned) {
      Begin(25, 2);
      for (unsigned source = 0; source < 2; ++source)
        for (unsigned y = 0; y < 2; ++y) {
          uint32_t *tiles = Row(source, y, 4, phase);
          Tiles(tiles, phase, y, 255, owned % 3, phase + source);
          uint32_t *edits = SrPpuBgPacket_AllocateEdits(&packet, SrPpuBgPacket_Meta(&packet, source, y));
          assert(edits);
          for (unsigned x = 0; x < 25; ++x)
            for (unsigned band = 0; band < 3; ++band) {
              const unsigned lane = x & 7u;
              const unsigned value = x % 4 == 0 ? 0 : (x + band) % 15 + 1;
              const unsigned flags = (owned & (1u << lane) ? 1u << 24 : 0) |
                  (x % 7 == 0 ? 1u : 0) | (x % 3 == 1 ? 1u << 8 : 0) |
                  (x & 1u ? 1u << 16 : 0);
              Cell(edits + x / 8 * 9 + band * 3, lane, value, (x + y) & 7u, flags);
            }
        }
      CheckBackgrounds();
    }
}

static void RandomRows(unsigned width, unsigned height, unsigned format) {
  Begin(width, height);
  for (unsigned source = 0; source < 2; ++source)
    for (unsigned y = 0; y < height; ++y) {
      const unsigned phase = Random() & 7u;
      uint32_t *pixels = Row(source, y, format, phase);
      uint32_t *meta = SrPpuBgPacket_Meta(&packet, source, y);
      meta[1] = Random();
      meta[2] = Random() % (width + 1);
      meta[3] = meta[2] + Random() % (width + 1 - meta[2]);
      if (format >= 3) {
        const unsigned count = (width + phase + 7u) / 8u;
        for (unsigned i = 0; i < count; ++i) {
          pixels[i * 2] = Random();
          pixels[i * 2 + 1] = (Random() & 7u) | ((Random() % 3) << 3) |
              (Random() & 32u) | ((Random() & 255u) << 8) | ((Random() & 15u) << 16);
        }
        if (format == 4 && (Random() & 1u)) {
          uint32_t *edits = SrPpuBgPacket_AllocateEdits(&packet, meta);
          assert(edits);
          for (unsigned i = 0; i < (width + 7u) / 8u * 9u; ++i) edits[i] = Random();
        }
      } else if (format == 2) {
        for (unsigned i = 0; i < (width + 7u) / 8u * 9u; ++i) pixels[i] = Random();
      } else {
        const unsigned codes[] = {0, 1, 128, 256, 257, 258, 259, 511};
        for (unsigned x = 0; x < width; ++x) {
          pixels[x] = Random() & UINT32_C(0x80000000);
          for (unsigned band = 0; band < 3; ++band)
            pixels[x] |= codes[(x + band + y) & 7u] << (band * 9);
        }
      }
    }
}

static void FullAdd(void) {
  for (unsigned y = 0; y < packet.words[1]; ++y) {
    uint32_t *row = SrPpuBgPacket_AllocateFullAdd(&packet, y);
    assert(row);
    row[0] = Random() & UINT32_C(0x3f13);
    row[1] = Random() & 65535u;
    row[2] = packet.words[0] << 16;
    row[3] = Random() & 3u;
    for (unsigned x = 0; x < packet.words[0]; ++x)
      row[4 + x] = ((Random() & 15u) << 3) | (Random() % 5) |
          ((((Random() & 15u) << 3) | (Random() % 5)) << 8) | ((Random() & 15u) << 16);
    /* An allocated table with absent individual rows still takes the raw
     * compact path, unlike the neighboring winner-dependent full-add rows. */
    if (y & 1u) packet.words[packet.words[7] + y] = 0;
  }
}

static void RandomAndLegacy(void) {
  const unsigned widths[] = {1, 7, 8, 9, 15, 17, 31, 257, 623, 624, 639, 640};
  for (unsigned trial = 0; trial < 384; ++trial) {
    const unsigned format = 1 + (trial / 12) % 4;
    RandomRows(widths[trial % 12], 1 + Random() % 4, format);
    if (format >= 3 && (trial & 4u)) FullAdd();
    CheckBackgrounds();
    Check(0, trial % 3, 0, packet.words[0] / 2); /* Prefix ending inside a tile. */
  }
  for (unsigned format = 0; format <= 4; ++format) {
    Begin(17, 1);
    for (unsigned source = 0; source < 2; ++source) {
      Row(source, 0, format, format >= 3 ? 7 : 0);
      uint32_t *meta = SrPpuBgPacket_Meta(&packet, source, 0);
      if (format == 4) {
        uint32_t *edits = SrPpuBgPacket_AllocateEdits(&packet, meta);
        assert(edits);
        edits[2] = UINT32_MAX;
      }
      meta[7] = 0; /* Backing only; even edit ownership must be ignored. */
    }
    CheckBackgrounds();
    Check(0, 0, 0, 0); /* Empty span must not write a guard word. */
  }
}

static void SkyboxAliases(void) {
  for (unsigned source = 0; source < 2; ++source)
    for (unsigned full_add = 0; full_add < 2; ++full_add) {
      RandomRows(640, 2, 4);
      if (full_add) FullAdd();
      packet.words[2] |= 4u;
      packet.words[4] = 17;
      packet.words[5] = 1;
      packet.words[6] = source;
      uint32_t *pixels = Row(2, 0, 2, 0);
      uint32_t *meta = SrPpuBgPacket_Meta(&packet, 2, 0);
      meta[2] = 0; meta[3] = 17;
      meta[4] = 2u | (15u << 16);
      meta[5] = 631; /* Alias crosses the fixed640 source-row boundary. */
      for (unsigned i = 0; i < 9; ++i) pixels[i] = Random();
      assert(SrPpuBgPacket_Valid(&packet));
      Check(2, 0, 0, 17);
      meta[7] = 0;
      Check(2, 0, 0, 17); /* Alias plus backing, without independent cells. */
    }
}

int main(void) {
  CompactLanes();
  PaintedCells();
  RandomAndLegacy();
  SkyboxAliases();
  printf("BG row decode: %u rows / %u exact ARGB comparisons passed\n",
      rows_checked, pixels_checked);
  return 0;
}
