#include "sim_town_canvas.h"

#include <string.h>

enum {
  /* The sim town's BG1 character base is VRAM $0000 (confirmed by the
   * captured BG register layout: bgTileAdr $0500). 4bpp, so one tile is 16
   * VRAM words: bitplanes 0/1 in the low/high byte of words 0-7 and
   * bitplanes 2/3 in words 8-15. */
  kBg1CharBaseWord = 0x0000,
  kTileWords = 16,
  kTileCount = 0x400,
  kCharWords = kTileCount * kTileWords,
};

static struct {
  uint8_t town;
  uint32_t serial;
  bool have_source;
  uint32_t backdrop;
  int brightness;
  uint8_t tilemap[kSimTownCanvasTiles * kSimTownCanvasTiles * 2];
  uint16_t cgram[0x100];
  uint16_t chars[kCharWords];
  int dirty_x0, dirty_y0, dirty_x1, dirty_y1;  /* half-open; x1<=x0 = clean */
  uint32_t pixels[kSimTownCanvasPixels * kSimTownCanvasPixels];
} g_canvas;

void SimTownCanvas_Reset(void) { memset(&g_canvas, 0, sizeof(g_canvas)); }

uint8_t SimTownCanvas_Town(void) { return g_canvas.town; }
uint32_t SimTownCanvas_Serial(void) { return g_canvas.serial; }
const uint32_t *SimTownCanvas_Pixels(void) { return g_canvas.pixels; }

bool SimTownCanvas_TakeDirtyRect(int *x, int *y, int *width, int *height) {
  if (g_canvas.dirty_x1 <= g_canvas.dirty_x0 ||
      g_canvas.dirty_y1 <= g_canvas.dirty_y0)
    return false;
  if (x) *x = g_canvas.dirty_x0;
  if (y) *y = g_canvas.dirty_y0;
  if (width) *width = g_canvas.dirty_x1 - g_canvas.dirty_x0;
  if (height) *height = g_canvas.dirty_y1 - g_canvas.dirty_y0;
  g_canvas.dirty_x0 = g_canvas.dirty_y0 = kSimTownCanvasPixels;
  g_canvas.dirty_x1 = g_canvas.dirty_y1 = 0;
  return true;
}

/* Same 5-bit expansion and brightness scaling the captured planes use, so a
 * canvas pixel and the authentic pixel of the same tile agree exactly. */
static uint8_t ExpandColor5(uint32_t value, int brightness) {
  uint32_t expanded = (value << 3) | (value >> 2);
  return (uint8_t)(expanded * (uint32_t)brightness / 15);
}

static uint32_t PaletteArgb(uint16_t colour, int brightness) {
  return 0xFF000000u |
      (uint32_t)ExpandColor5(colour & 0x1F, brightness) << 16 |
      (uint32_t)ExpandColor5((colour >> 5) & 0x1F, brightness) << 8 |
      ExpandColor5((colour >> 10) & 0x1F, brightness);
}

/* $03:9C43's addressing, read back: four 32x32-tile quadrant pages. */
static uint16_t TilemapEntry(const uint8_t *tilemap, int tile_x, int tile_y) {
  int quadrant = (tile_y >= 32 ? 2 : 0) + (tile_x >= 32 ? 1 : 0);
  size_t word = (size_t)quadrant * kSimTownQuadrantWords +
      (size_t)(tile_y & 31) * 32 + (tile_x & 31);
  return (uint16_t)(tilemap[word * 2] | (tilemap[word * 2 + 1] << 8));
}

void SimTownCanvas_Render(uint8_t town, const uint8 *wram,
                          const uint16_t *vram, const uint16_t *cgram,
                          int brightness, uint32_t backdrop_argb) {
  if (!town || !wram || !vram || !cgram) return;
  if (town != g_canvas.town) {
    SimTownCanvas_Reset();
    g_canvas.town = town;
  }

  const uint8_t *live_map = wram + kSimTownTilemapWram;
  const uint16_t *live_chars = vram + kBg1CharBaseWord;
  bool changed = !g_canvas.have_source ||
      brightness != g_canvas.brightness ||
      backdrop_argb != g_canvas.backdrop ||
      memcmp(g_canvas.tilemap, live_map, sizeof(g_canvas.tilemap)) != 0 ||
      memcmp(g_canvas.cgram, cgram, sizeof(g_canvas.cgram)) != 0 ||
      memcmp(g_canvas.chars, live_chars, sizeof(g_canvas.chars)) != 0;
  if (!changed) return;

  memcpy(g_canvas.tilemap, live_map, sizeof(g_canvas.tilemap));
  memcpy(g_canvas.cgram, cgram, sizeof(g_canvas.cgram));
  memcpy(g_canvas.chars, live_chars, sizeof(g_canvas.chars));
  g_canvas.brightness = brightness;
  g_canvas.backdrop = backdrop_argb;
  g_canvas.have_source = true;

  uint32_t palette[0x100];
  for (int i = 0; i < 0x100; i++)
    palette[i] = PaletteArgb(g_canvas.cgram[i], brightness);
  uint32_t opaque_backdrop = backdrop_argb | 0xFF000000u;

  for (int tile_y = 0; tile_y < kSimTownCanvasTiles; tile_y++) {
    for (int tile_x = 0; tile_x < kSimTownCanvasTiles; tile_x++) {
      uint16_t entry = TilemapEntry(g_canvas.tilemap, tile_x, tile_y);
      const uint16_t *art = g_canvas.chars + (size_t)(entry & 0x3FF) * kTileWords;
      const uint32_t *bank = palette + ((entry >> 10) & 7) * 16;
      bool flip_x = (entry & 0x4000) != 0, flip_y = (entry & 0x8000) != 0;
      for (int row = 0; row < 8; row++) {
        int source_row = flip_y ? 7 - row : row;
        uint16_t low = art[source_row];
        uint16_t high = art[source_row + 8];
        uint32_t *out = g_canvas.pixels +
            (size_t)(tile_y * 8 + row) * kSimTownCanvasPixels + tile_x * 8;
        for (int column = 0; column < 8; column++) {
          int shift = 7 - (flip_x ? 7 - column : column);
          unsigned index =
              ((low >> shift) & 1) | (((low >> (shift + 8)) & 1) << 1) |
              (((high >> shift) & 1) << 2) | (((high >> (shift + 8)) & 1) << 3);
          /* Colour zero is transparent on hardware and the backdrop shows
           * through it; matching that keeps the canvas opaque everywhere so
           * it never punches a hole in the world map beneath. */
          out[column] = index ? bank[index] : opaque_backdrop;
        }
      }
    }
  }

  g_canvas.dirty_x0 = g_canvas.dirty_y0 = 0;
  g_canvas.dirty_x1 = g_canvas.dirty_y1 = kSimTownCanvasPixels;
  if (++g_canvas.serial == 0) g_canvas.serial = 1;
}
