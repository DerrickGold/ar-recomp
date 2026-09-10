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
  return failures ? 1 : 0;
}
