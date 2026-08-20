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
  /* Four little-endian 8x8 tilemap entries per 16x16 terrain metatile. */
  kTerrainDefinitionsWram = 0x2100,
  kTerrainDefinitionBytes = 8,
  /* The definition's bit 9 is terrain traversal metadata. $03:9B5A clears it
   * before the word becomes a live BG tilemap entry, so raw-source rendering
   * must do the same or it samples character $200 too high. */
  kTerrainDefinitionVisualMask = 0xFDFF,
  /* Tile priority changes painter order in the live PPU but not the opaque
   * town-space canvas; every other entry bit changes sampled pixels. */
  kTilemapVisualMask = 0xDFFF,
};

static struct {
  uint8_t town;
  uint32_t serial;
  uint32_t tilemap_serial;
  uint32_t character_serial;
  uint32_t palette_serial;
  uint32_t display_serial;
  uint32_t last_change_mask;
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
uint32_t SimTownCanvas_TilemapSerial(void) {
  return g_canvas.tilemap_serial;
}
uint32_t SimTownCanvas_CharacterSerial(void) {
  return g_canvas.character_serial;
}
uint32_t SimTownCanvas_PaletteSerial(void) {
  return g_canvas.palette_serial;
}
uint32_t SimTownCanvas_DisplaySerial(void) {
  return g_canvas.display_serial;
}
uint32_t SimTownCanvas_LastChangeMask(void) {
  return g_canvas.last_change_mask;
}
const uint32_t *SimTownCanvas_Pixels(void) { return g_canvas.pixels; }

static void AdvanceSerial(uint32_t *serial) {
  if (++*serial == 0) *serial = 1;
}

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

static unsigned TilePixelIndex(const uint16_t *chars, uint16_t entry,
                               int pixel_x, int pixel_y) {
  const uint16_t *art = chars + (size_t)(entry & 0x3FF) * kTileWords;
  bool flip_x = (entry & 0x4000) != 0, flip_y = (entry & 0x8000) != 0;
  int source_x = flip_x ? 7 - pixel_x : pixel_x;
  int source_y = flip_y ? 7 - pixel_y : pixel_y;
  uint16_t low = art[source_y];
  uint16_t high = art[source_y + 8];
  int shift = 7 - source_x;
  return ((low >> shift) & 1) |
      (((low >> (shift + 8)) & 1) << 1) |
      (((high >> shift) & 1) << 2) |
      (((high >> (shift + 8)) & 1) << 3);
}

bool SimTownCanvas_SourcePixelOpaque(const uint8 *wram,
                                     const uint16_t *vram,
                                     int pixel_x, int pixel_y) {
  if (!wram || !vram || pixel_x < 0 || pixel_y < 0 ||
      pixel_x >= kSimTownCanvasPixels || pixel_y >= kSimTownCanvasPixels)
    return false;
  int tile_x = pixel_x >> 3, tile_y = pixel_y >> 3;
  uint16_t entry = TilemapEntry(
      wram + kSimTownTilemapWram, tile_x, tile_y);
  return TilePixelIndex(vram + kBg1CharBaseWord, entry,
                        pixel_x & 7, pixel_y & 7) != 0;
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
  const uint32_t *bank =
      palette + ((entry >> 10) & 7) * kPaletteColorsPerBank;
  for (int row = 0; row < 8; row++) {
    uint32_t *out = g_canvas.pixels +
        (size_t)(tile_y * 8 + row) * kSimTownCanvasPixels + tile_x * 8;
    for (int column = 0; column < 8; column++) {
      unsigned index = TilePixelIndex(chars, entry, column, row);
      /* Colour zero is transparent on hardware and the backdrop shows
       * through it; matching that keeps the canvas opaque everywhere so it
       * never punches a hole in the world map beneath. */
      out[column] = index ? bank[index] : opaque_backdrop;
    }
  }
}

bool SimTownCanvas_RenderTerrainMetatile(
    const uint8 *wram, uint8_t metatile, uint32_t out_pixels[16 * 16]) {
  if (!wram || !out_pixels || !g_canvas.have_source) return false;
  uint32_t palette[kBgPaletteColorCount];
  for (int i = 0; i < kBgPaletteColorCount; i++)
    palette[i] = PaletteArgb(g_canvas.cgram[i], g_canvas.brightness);
  uint32_t opaque_backdrop = g_canvas.backdrop | 0xFF000000u;
  const uint8_t *definition = wram + kTerrainDefinitionsWram +
      (size_t)metatile * kTerrainDefinitionBytes;
  for (int quadrant = 0; quadrant < 4; quadrant++) {
    uint16_t entry = (uint16_t)(definition[quadrant * 2] |
        (definition[quadrant * 2 + 1] << 8));
    entry &= kTerrainDefinitionVisualMask;
    const uint32_t *bank = palette +
        ((entry >> 10) & 7) * kPaletteColorsPerBank;
    int x0 = (quadrant & 1) * 8;
    int y0 = (quadrant >> 1) * 8;
    for (int row = 0; row < 8; row++)
      for (int column = 0; column < 8; column++) {
        unsigned index = TilePixelIndex(
            g_canvas.chars, entry, column, row);
        out_pixels[(y0 + row) * 16 + x0 + column] =
            index ? bank[index] : opaque_backdrop;
      }
  }
  return true;
}

void SimTownCanvas_Render(uint8_t town, const uint8 *wram,
                          const uint16_t *vram, const uint16_t *cgram,
                          int brightness, uint32_t backdrop_argb) {
  g_canvas.last_change_mask = kSimTownCanvasChange_None;
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
  bool tilemap_visual_changed = source_changed;

  for (int tile_y = 0; tile_y < kSimTownCanvasTiles; tile_y++) {
    for (int tile_x = 0; tile_x < kSimTownCanvasTiles; tile_x++) {
      uint16_t entry = TilemapEntry(live_map, tile_x, tile_y);
      bool redraw = full_repaint;
      if (tilemap_changed && !source_changed) {
        uint16_t prior = TilemapEntry(g_canvas.tilemap, tile_x, tile_y);
        bool entry_changed = ((prior ^ entry) & kTilemapVisualMask) != 0;
        if (entry_changed) tilemap_visual_changed = true;
        if (!redraw) redraw = entry_changed;
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
  if (tilemap_visual_changed) {
    g_canvas.last_change_mask |= kSimTownCanvasChange_Tilemap;
    AdvanceSerial(&g_canvas.tilemap_serial);
  }
  if (chars_changed) {
    g_canvas.last_change_mask |= kSimTownCanvasChange_Characters;
    AdvanceSerial(&g_canvas.character_serial);
  }
  if (palette_changed) {
    g_canvas.last_change_mask |= kSimTownCanvasChange_Palette;
    AdvanceSerial(&g_canvas.palette_serial);
  }
  if (full_repaint) {
    g_canvas.last_change_mask |= kSimTownCanvasChange_Display;
    AdvanceSerial(&g_canvas.display_serial);
  }
  if (!pixels_changed) return;
  if (full_repaint) {
    for (int row = 0; row < kSimTownCanvasTiles; row++) {
      g_canvas.dirty_x0[row] = 0;
      g_canvas.dirty_x1[row] = kSimTownCanvasPixels;
    }
  }
  AdvanceSerial(&g_canvas.serial);
}
