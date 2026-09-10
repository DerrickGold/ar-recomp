#include "actraiser/actraiser_localization_art.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static int failures;
#define CHECK(value) do { if (!(value)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); ++failures; \
} } while (0)

static bool ReadExact(const char *path, uint8_t *bytes, size_t count) {
  FILE *file = fopen(path, "rb");
  if (!file) return false;
  const bool ok = fread(bytes, 1, count, file) == count && fgetc(file) == EOF;
  return fclose(file) == 0 && ok;
}

/* Independent pixel oracle for the Go ROM asset exporter. All glyphs and
 * palettes are supplied locally; no retail bytes are checked into fixtures. */
static int RenderPage(const char *font_path, const char *page_path,
                      const char *palette_path) {
#ifdef _WIN32
  if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 2;
#endif
  uint8_t font[4096], page[2048], palette_bytes[32];
  uint16_t vram[0x8000] = {0}, palette[32] = {0};
  uint8_t rgba[256 * 224 * 4] = {0};
  if (!ReadExact(font_path, font, sizeof(font)) ||
      !ReadExact(page_path, page, sizeof(page)) ||
      !ReadExact(palette_path, palette_bytes, sizeof(palette_bytes))) return 2;
  for (size_t i = 0; i < sizeof(font) / 2; ++i)
    vram[i] = font[i*2] | (uint16_t)font[i*2+1] << 8;
  for (size_t i = 0; i < sizeof(palette_bytes) / 2; ++i)
    palette[i] = palette_bytes[i*2] | (uint16_t)palette_bytes[i*2+1] << 8;
  for (unsigned cell = 0; cell < 32*28; ++cell) {
    const uint16_t word = page[cell*2] | (uint16_t)page[cell*2+1] << 8;
    ArLocalizationArtwork art;
    if (!ActRaiserLocalizationArt_Capture(
            &art, 0, &word, 1, vram, 0x8000, palette, 32)) return 2;
    for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
      const uint32_t pixel = art.argb[y*8+x];
      uint8_t *out = &rgba[((cell/32*8+y)*256+cell%32*8+x)*4];
      out[0] = pixel >> 16; out[1] = pixel >> 8;
      out[2] = pixel; out[3] = pixel >> 24;
    }
  }
  return fwrite(rgba, 1, sizeof(rgba), stdout) == sizeof(rgba) ? 0 : 2;
}

int main(int argc, char **argv) {
  if (argc == 5 && !strcmp(argv[1], "--render-page"))
    return RenderPage(argv[2], argv[3], argv[4]);
  if (argc != 1) return 2;
  if (argc != 1) return 2;
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
