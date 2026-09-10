#include "actraiser/actraiser_localization_art.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(value) do { if (!(value)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); ++failures; \
} } while (0)

int main(void) {
  uint16_t vram[0x8000] = {0};
  uint16_t palette[32] = {0};
  palette[5] = 31; /* palette 1 red */
  palette[6] = 31 << 10; /* blue */
  vram[8] = 0x0080; /* tile 1, top left, plane 0 */
  vram[23] = 0x0100; /* tile 2, bottom right, plane 1 */
  const uint16_t cells[] = {0x0401, 0xc402};
  ArLocalizationArtwork art;
  CHECK(ActRaiserLocalizationArt_Capture(
      &art, 0, cells, 2, vram, 0x8000, palette, 32));
  CHECK(art.valid && art.width == 16 && art.height == 8);
  CHECK(art.argb[0] == 0xffff0000u);
  CHECK(art.argb[8] == 0xff0000ffu); /* flipped in both axes */
  CHECK(art.argb[1] == 0 && art.argb[16] == 0);
  /* Native blinking clears the object tile; keep its transparency rather
   * than synthesizing an always-on replacement arrow. */
  const uint16_t blank = 0;
  CHECK(ActRaiserLocalizationArt_Capture(
      &art, 0, &blank, 1, vram, 0x8000, palette, 32));
  for (unsigned i = 0; i < kArLocalizationArtworkPixels; ++i)
    CHECK(art.argb[i] == 0);
  CHECK(!ActRaiserLocalizationArt_Capture(
      &art, 0, cells, 3, vram, 0x8000, palette, 32));
  CHECK(!art.valid);

  /* The keyboard's finish and backspace glyphs are font characters $7E and
   * $7F, resolved through the BG3 tile base the same way the selector glyph
   * $3E is. Pin that arithmetic: a tile lives at base + index * 8 words, so
   * a descriptor naming $7F must read the row written there and nowhere
   * else. */
  enum { kTileBase = 0x1000, kBackspace = 0x7f };
  uint16_t keyboard[0x8000] = {0};
  keyboard[kTileBase + kBackspace * 8] = 0x8080; /* both planes, leftmost */
  const uint16_t backspace_cell[] = {kBackspace};
  CHECK(ActRaiserLocalizationArt_Capture(
      &art, kTileBase, backspace_cell, 1, keyboard, 0x8000, palette, 32));
  CHECK(art.valid && art.width == 8 && art.height == 8);
  CHECK(art.argb[0] != 0);          /* colour index 3, palette 0 */
  CHECK(art.argb[1] == 0);
  /* The neighbouring character must not bleed into it. */
  const uint16_t finish_cell[] = {0x7e};
  CHECK(ActRaiserLocalizationArt_Capture(
      &art, kTileBase, finish_cell, 1, keyboard, 0x8000, palette, 32));
  for (unsigned i = 0; i < kArLocalizationArtworkPixels; ++i)
    CHECK(art.argb[i] == 0);
  return failures ? 1 : 0;
}
