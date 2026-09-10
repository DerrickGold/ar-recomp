#include "sim_town_ground_art.h"

#include <stdlib.h>
#include <string.h>

#include "byte_order.h"
#include "snes_bgr555.h"

enum {
  /* Map 00/01..06 asset commands at file $028038..$028209. Command 5
   * copies the raw big-endian definitions, then $02:B3CE swaps each word.
   * $02:C58C selects $0C:8000 for tier <2, $0C:C000 for tier >=2. */
  kDefinitions = 0x0C881A,
  kCharacters = 0x060000,
  kCharacterBankBytes = 0x4000,
  kTemperatePalette = 0x0E3B93,
  kSnowPalette = 0x0E3D93,
  kPaletteColors = 128,
  kTiles = 256,
  kTilePixels = kSimTownCellPixels * kSimTownCellPixels,
  kVariants = 4,
  /* All six town asset scripts select video profile 2 ($02:893E + 2*28).
   * Its $24/$08 bytes configure four $100-byte slices, every eight ticks.
   * $02:BAF5 snapshots the loaded CHR sheet; $02:AF30 uploads the selected
   * slice back to byte zero. No other town CHR pages animate. */
  kAnimationProfile = 0x01093E + 2 * 28,
  kAnimationBytes = 0x100,
  kAnimationCharacters = kAnimationBytes / 32,
};

static struct {
  bool available;
  uint8_t definitions[kTiles * 8];
  uint8_t chars[2][kCharacterBankBytes];
  uint32_t palette[2][kPaletteColors];
  uint32_t *pixels[kVariants];
  bool animation_available;
  uint16_t animation_count;
  int16_t animation_index[kTiles];
  uint32_t *animated[kVariants][kSimTownGroundAnimationFrames - 1];
  bool animation_failed[kVariants];
} s_art;

static uint32_t PaletteColor(const uint8_t *rom, size_t at) {
  const uint16_t color = ByteOrder_ReadLe16(rom + at);
  return 0xFF000000u |
      (uint32_t)ExpandColor5(color & 31, 15) << 16 |
      (uint32_t)ExpandColor5((color >> 5) & 31, 15) << 8 |
      ExpandColor5((color >> 10) & 31, 15);
}

void SimTownGroundArt_Shutdown(void) {
  for (unsigned i = 0; i < kVariants; i++) {
    free(s_art.pixels[i]);
    for (unsigned phase = 0; phase < kSimTownGroundAnimationFrames - 1; phase++)
      free(s_art.animated[i][phase]);
  }
  memset(&s_art, 0, sizeof(s_art));
}

bool SimTownGroundArt_Available(void) { return s_art.available; }

bool SimTownGroundArt_AnimationAvailable(void) {
  return s_art.available && s_art.animation_available && s_art.animation_count;
}

uint8_t SimTownGroundArt_AnimationPhase(uint16_t game_frame) {
  return SimTownGroundArt_AnimationAvailable()
      ? (uint8_t)(((uint16_t)(game_frame - 1) / kSimTownGroundAnimationTicks) &
                   (kSimTownGroundAnimationFrames - 1)) : 0;
}

bool SimTownGroundArt_Init(const uint8_t *rom, size_t rom_size) {
  SimTownGroundArt_Shutdown();
  if (!rom || rom_size < kSnowPalette + kPaletteColors * 2) return false;
  memcpy(s_art.definitions, rom + kDefinitions, sizeof(s_art.definitions));
  memcpy(s_art.chars, rom + kCharacters, sizeof(s_art.chars));
  for (unsigned bank = 0; bank < 2; bank++)
    for (unsigned i = 0; i < kPaletteColors; i++)
      s_art.palette[bank][i] = PaletteColor(
          rom, (bank ? kSnowPalette : kTemperatePalette) + i * 2);
  s_art.animation_available = rom[kAnimationProfile + 23] == 0x24 &&
      rom[kAnimationProfile + 24] == kSimTownGroundAnimationTicks;
  for (unsigned tile = 0; tile < kTiles; tile++) {
    s_art.animation_index[tile] = -1;
    for (unsigned q = 0; q < 4; q++) {
      const uint8_t *definition = s_art.definitions + tile * 8 + q * 2;
      const unsigned character = ((unsigned)definition[0] << 8 | definition[1]) & 0x1FF;
      if (character < kAnimationCharacters) {
        s_art.animation_index[tile] = (int16_t)s_art.animation_count++;
        break;
      }
    }
  }
  s_art.available = true;
  return true;
}

static void DecodeIndices(unsigned variant, unsigned tile, unsigned phase, uint8_t *out) {
  const uint8_t *chars = s_art.chars[variant & 1];
  for (unsigned q = 0; q < 4; q++) {
    const uint8_t *definition = s_art.definitions + tile * 8 + q * 2;
    /* $0200 is traversal metadata, not character-address bit 9. */
    const uint16_t entry =
        (uint16_t)(((uint16_t)definition[0] << 8 | definition[1]) & 0xFDFF);
    const unsigned character = entry & 0x1FF;
    const uint8_t *art = chars + character * 32 +
        (character < kAnimationCharacters ? phase * kAnimationBytes : 0);
    for (unsigned y = 0; y < 8; y++)
      for (unsigned x = 0; x < 8; x++) {
        const unsigned sy = entry & 0x8000 ? 7 - y : y;
        const unsigned shift = entry & 0x4000 ? x : 7 - x;
        const unsigned index =
            ((art[sy * 2] >> shift) & 1) |
            (((art[sy * 2 + 1] >> shift) & 1) << 1) |
            (((art[sy * 2 + 16] >> shift) & 1) << 2) |
            (((art[sy * 2 + 17] >> shift) & 1) << 3);
        out[((q >> 1) * 8 + y) * kSimTownCellPixels + (q & 1) * 8 + x] =
            index ? (uint8_t)(((entry >> 10) & 7) * 16 + index) : 0;
      }
  }
}

static void DecodeTile(unsigned variant, unsigned tile, unsigned phase, uint32_t *out) {
  uint8_t indices[kTilePixels];
  DecodeIndices(variant, tile, phase, indices);
  const uint32_t *palette = s_art.palette[variant >> 1];
  for (unsigned p = 0; p < kTilePixels; p++)
    out[p] = indices[p] ? palette[indices[p]] : 0;
}

bool SimTownGroundArt_ColorIndexMask(
    uint8_t town, uint8_t development_tier, uint8_t tile,
    uint8_t color_index, uint8_t *mask) {
  if (!s_art.available || !mask || town < 1 || town > kSimTownCount ||
      color_index >= kPaletteColors) return false;
  const unsigned variant = (town == 6 ? 2u : 0u) | (development_tier >= 2 ? 1u : 0u);
  uint8_t indices[kTilePixels];
  DecodeIndices(variant, tile, 0, indices);
  for (unsigned p = 0; p < kTilePixels; p++)
    mask[p] = indices[p] && indices[p] == color_index;
  return true;
}

static bool DecodeVariant(unsigned variant) {
  if (s_art.pixels[variant]) return true;
  uint32_t *pixels = malloc((size_t)kTiles * kTilePixels * sizeof(*pixels));
  if (!pixels) return false;
  for (unsigned tile = 0; tile < kTiles; tile++)
    DecodeTile(variant, tile, 0, pixels + (size_t)tile * kTilePixels);
  s_art.pixels[variant] = pixels;
  return true;
}

const uint32_t *SimTownGroundArt_AnimatedMetatile(
    uint8_t town, uint8_t development_tier, uint8_t tile, uint8_t phase) {
  if (phase >= kSimTownGroundAnimationFrames) return NULL;
  const uint32_t *base = SimTownGroundArt_Metatile(town, development_tier, tile);
  if (!base || !phase || !SimTownGroundArt_AnimationAvailable()) return base;
  const unsigned variant = (town == 6 ? 2u : 0u) | (development_tier >= 2 ? 1u : 0u);
  if (s_art.animation_failed[variant]) return base;
  if (!s_art.animated[variant][phase - 1]) {
    uint32_t *pixels = malloc((size_t)s_art.animation_count * kTilePixels * sizeof(*pixels));
    if (!pixels) {
      /* Keep detailed static ground, including accepted cliff materials,
       * instead of dropping its entire overlay or retrying every frame. */
      s_art.animation_failed[variant] = true;
      return base;
    }
    for (unsigned at = 0; at < kTiles; at++)
      if (s_art.animation_index[at] >= 0)
        DecodeTile(variant, at, phase,
            pixels + (size_t)s_art.animation_index[at] * kTilePixels);
    s_art.animated[variant][phase - 1] = pixels;
  }
  const int index = s_art.animation_index[tile];
  return index < 0 ? base : s_art.animated[variant][phase - 1] + (size_t)index * kTilePixels;
}

const uint32_t *SimTownGroundArt_Metatile(
    uint8_t town, uint8_t development_tier, uint8_t tile) {
  if (!s_art.available || town < 1 || town > kSimTownCount) return NULL;
  const unsigned variant = (town == 6 ? 2u : 0u) |
      (development_tier >= 2 ? 1u : 0u);
  return DecodeVariant(variant)
      ? s_art.pixels[variant] + (size_t)tile * kTilePixels : NULL;
}
