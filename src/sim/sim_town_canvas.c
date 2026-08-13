#include "sim_town_canvas.h"

#include "snes_bgr555.h"
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
  kBgPaletteBankCount = 8,
  kPaletteColorsPerBank = 16,
  kBgPaletteColorCount = kBgPaletteBankCount * kPaletteColorsPerBank,
  /* Tile priority changes painter order in the live PPU but not the opaque
   * town-space canvas; every other entry bit changes sampled pixels. */
  kTilemapVisualMask = 0xDFFF,
};

static struct {
  uint8_t town;
  uint32_t serial;
  bool have_source;
  uint32_t backdrop;
  int brightness;
  uint8_t tilemap[kSimTownCanvasTiles * kSimTownCanvasTiles * 2];
  uint16_t cgram[kBgPaletteColorCount];
  uint16_t chars[kCharWords];
  /* Half-open horizontal span for each tile row; x1<=x0 means clean. */
  int dirty_x0[kSimTownCanvasTiles], dirty_x1[kSimTownCanvasTiles];
  uint32_t pixels[kSimTownCanvasPixels * kSimTownCanvasPixels];
} g_canvas;

void SimTownCanvas_Reset(void) { memset(&g_canvas, 0, sizeof(g_canvas)); }

uint8_t SimTownCanvas_Town(void) { return g_canvas.town; }
uint32_t SimTownCanvas_Serial(void) { return g_canvas.serial; }
const uint32_t *SimTownCanvas_Pixels(void) { return g_canvas.pixels; }

bool SimTownCanvas_TakeDirtyRect(int *x, int *y, int *width, int *height) {
  int first_row = 0;
  while (first_row < kSimTownCanvasTiles &&
         g_canvas.dirty_x1[first_row] <= g_canvas.dirty_x0[first_row])
    first_row++;
  if (first_row == kSimTownCanvasTiles) return false;

  int dirty_x0 = g_canvas.dirty_x0[first_row];
  int dirty_x1 = g_canvas.dirty_x1[first_row];
  int end_row = first_row;
  do {
    g_canvas.dirty_x0[end_row] = kSimTownCanvasPixels;
    g_canvas.dirty_x1[end_row] = 0;
    end_row++;
  } while (end_row < kSimTownCanvasTiles &&
           g_canvas.dirty_x0[end_row] == dirty_x0 &&
           g_canvas.dirty_x1[end_row] == dirty_x1);

  if (x) *x = dirty_x0;
  if (y) *y = first_row * 8;
  if (width) *width = dirty_x1 - dirty_x0;
  if (height) *height = (end_row - first_row) * 8;
  return true;
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

static void MarkDirtyTile(int tile_x, int tile_y) {
  int x0 = tile_x * 8, x1 = x0 + 8;
  if (g_canvas.dirty_x1[tile_y] <= g_canvas.dirty_x0[tile_y]) {
    g_canvas.dirty_x0[tile_y] = x0;
    g_canvas.dirty_x1[tile_y] = x1;
    return;
  }
  if (x0 < g_canvas.dirty_x0[tile_y]) g_canvas.dirty_x0[tile_y] = x0;
  if (x1 > g_canvas.dirty_x1[tile_y]) g_canvas.dirty_x1[tile_y] = x1;
}

static void RenderTile(int tile_x, int tile_y, uint16_t entry,
                       const uint16_t *chars, const uint32_t *palette,
                       uint32_t opaque_backdrop) {
  const uint16_t *art = chars + (size_t)(entry & 0x3FF) * kTileWords;
  const uint32_t *bank =
      palette + ((entry >> 10) & 7) * kPaletteColorsPerBank;
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
       * through it; matching that keeps the canvas opaque everywhere so it
       * never punches a hole in the world map beneath. */
      out[column] = index ? bank[index] : opaque_backdrop;
    }
  }
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
  bool source_changed = !g_canvas.have_source;
  bool full_repaint = source_changed || brightness != g_canvas.brightness ||
      backdrop_argb != g_canvas.backdrop;
  bool tilemap_changed = source_changed ||
      memcmp(g_canvas.tilemap, live_map, sizeof(g_canvas.tilemap)) != 0;
  bool palette_changed = source_changed ||
      memcmp(g_canvas.cgram, cgram, sizeof(g_canvas.cgram)) != 0;
  bool chars_changed = source_changed ||
      memcmp(g_canvas.chars, live_chars, sizeof(g_canvas.chars)) != 0;
  if (!full_repaint && !tilemap_changed && !palette_changed && !chars_changed)
    return;

  bool changed_chars[kTileCount] = {false};
  bool changed_palettes[kBgPaletteBankCount] = {false};
  if (!full_repaint && chars_changed) {
    for (int tile = 0; tile < kTileCount; tile++) {
      const size_t first = (size_t)tile * kTileWords;
      changed_chars[tile] =
          memcmp(g_canvas.chars + first, live_chars + first,
                 kTileWords * sizeof(uint16_t)) != 0;
    }
  }
  if (!full_repaint && palette_changed) {
    for (int bank = 0; bank < kBgPaletteBankCount; bank++) {
      const size_t first =
          (size_t)bank * kPaletteColorsPerBank + 1;  /* colour 0 is backdrop */
      changed_palettes[bank] =
          memcmp(g_canvas.cgram + first, cgram + first,
                 (kPaletteColorsPerBank - 1) * sizeof(uint16_t)) != 0;
    }
  }

  uint32_t palette[kBgPaletteColorCount];
  for (int i = 0; i < kBgPaletteColorCount; i++)
    palette[i] = PaletteArgb(cgram[i], brightness);
  uint32_t opaque_backdrop = backdrop_argb | 0xFF000000u;
  bool pixels_changed = false;

  for (int tile_y = 0; tile_y < kSimTownCanvasTiles; tile_y++) {
    for (int tile_x = 0; tile_x < kSimTownCanvasTiles; tile_x++) {
      uint16_t entry = TilemapEntry(live_map, tile_x, tile_y);
      bool redraw = full_repaint;
      if (!redraw && tilemap_changed) {
        uint16_t prior = TilemapEntry(g_canvas.tilemap, tile_x, tile_y);
        redraw = ((prior ^ entry) & kTilemapVisualMask) != 0;
      }
      if (!redraw && chars_changed) redraw = changed_chars[entry & 0x3FF];
      if (!redraw && palette_changed)
        redraw = changed_palettes[(entry >> 10) & 7];
      if (!redraw) continue;
      RenderTile(tile_x, tile_y, entry, live_chars, palette, opaque_backdrop);
      if (!full_repaint) MarkDirtyTile(tile_x, tile_y);
      pixels_changed = true;
    }
  }

  memcpy(g_canvas.tilemap, live_map, sizeof(g_canvas.tilemap));
  memcpy(g_canvas.cgram, cgram, sizeof(g_canvas.cgram));
  memcpy(g_canvas.chars, live_chars, sizeof(g_canvas.chars));
  g_canvas.brightness = brightness;
  g_canvas.backdrop = backdrop_argb;
  g_canvas.have_source = true;
  if (!pixels_changed) return;
  if (full_repaint) {
    for (int row = 0; row < kSimTownCanvasTiles; row++) {
      g_canvas.dirty_x0[row] = 0;
      g_canvas.dirty_x1[row] = kSimTownCanvasPixels;
    }
  }
  if (++g_canvas.serial == 0) g_canvas.serial = 1;
}
