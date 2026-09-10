#include "actraiser/actraiser_localization_art.h"

#include <string.h>
#include "snes_bgr555.h"

bool ActRaiserLocalizationArt_Capture(
    ArLocalizationArtwork *art, uint16_t tile_base_words,
    const uint16_t *tile_words, unsigned tile_count,
    const uint16_t *vram, size_t vram_words,
    const uint16_t *cgram, size_t cgram_words) {
  if (!art) return false;
  memset(art, 0, sizeof(*art));
  if (!tile_words || !tile_count || tile_count > 2 || !vram ||
      vram_words < 0x8000 || !cgram || cgram_words < 32)
    return false;
  art->width = (uint8_t)(tile_count * 8);
  art->height = 8;
  for (unsigned tile = 0; tile < tile_count; ++tile) {
    const uint16_t descriptor = tile_words[tile];
    const unsigned base = tile_base_words + (descriptor & 0x3ffu) * 8;
    const unsigned palette = ((descriptor >> 10) & 7u) * 4;
    for (unsigned y = 0; y < 8; ++y) {
      const unsigned source_y = (descriptor & 0x8000u) ? 7 - y : y;
      const uint16_t planes = vram[(base + source_y) & 0x7fffu];
      for (unsigned x = 0; x < 8; ++x) {
        const unsigned shift = (descriptor & 0x4000u) ? x : 7 - x;
        const unsigned index = ((planes >> shift) & 1u) |
            ((planes >> (shift + 7u)) & 2u);
        const uint16_t color = cgram[palette + index];
        if (index)
          art->argb[y * art->width + tile * 8 + x] =
              UINT32_C(0xff000000) |
              (uint32_t)ExpandColor5(color, 15) << 16 |
              (uint32_t)ExpandColor5(color >> 5, 15) << 8 |
              ExpandColor5(color >> 10, 15);
      }
    }
  }
  art->valid = true;
  return true;
}
