/* Full-town ground canvas: the quadrant-paged tilemap read-back, 4bpp tile
 * decode with flips and palette banks, and the change-detection that keeps a
 * still town from re-rendering.
 *
 * The quadrant addressing is the part that fails invisibly. A row-major read
 * of $7F:0000 produces a plausible-looking image made of the wrong quarters
 * of the town, which is exactly the mistake that made this range look like an
 * unrelated layer twice, so a tile placed in each quadrant is asserted to
 * land where $03:9C43's addressing says it must. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_town_canvas.h"

static int s_failures;
#define CHECK(expression)                                                  \
  do {                                                                     \
    if (!(expression)) {                                                   \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,     \
              #expression);                                                \
      s_failures++;                                                        \
    }                                                                      \
  } while (0)

enum {
  kWramSize = 0x20000,
  kVramWords = 0x8000,
  kBrightness = 15,
  kBackdrop = 0xFF102030,
};

static uint8_t *g_wram;
static uint16_t *g_vram;
static uint16_t g_cgram[0x100];

static void SetupSources(void) {
  memset(g_wram, 0, kWramSize);
  memset(g_vram, 0, kVramWords * sizeof(uint16_t));
  memset(g_cgram, 0, sizeof(g_cgram));
  /* Palette bank 1 entry 1 = pure red, bank 2 entry 1 = pure blue. */
  g_cgram[16 + 1] = 0x001F;
  g_cgram[32 + 1] = 0x7C00;
  /* Tile 1: colour index 1 in the top-left pixel only, colour 0 elsewhere. */
  g_vram[1 * 16 + 0] = 0x0080;   /* bitplane 0, bit 7 of row 0 */
}

static void SetTile(int tile_x, int tile_y, uint16_t entry) {
  int quadrant = (tile_y >= 32 ? 2 : 0) + (tile_x >= 32 ? 1 : 0);
  size_t word = (size_t)quadrant * kSimTownQuadrantWords +
      (size_t)(tile_y & 31) * 32 + (tile_x & 31);
  g_wram[kSimTownTilemapWram + word * 2] = (uint8_t)(entry & 0xFF);
  g_wram[kSimTownTilemapWram + word * 2 + 1] = (uint8_t)(entry >> 8);
}

static uint32_t CanvasAt(int x, int y) {
  return SimTownCanvas_Pixels()[(size_t)y * kSimTownCanvasPixels + x];
}

static void Render(uint8_t town) {
  SimTownCanvas_Render(town, g_wram, g_vram, g_cgram, kBrightness, kBackdrop);
}

/* One tile in each quadrant, at a position that would collide with another
 * quadrant under a row-major read. */
static void TestQuadrantAddressing(void) {
  SimTownCanvas_Reset();
  SetupSources();
  const struct { int tx, ty; uint16_t bank; } probes[] = {
    {  1,  1, 1 << 10 }, { 33,  1, 2 << 10 },
    {  1, 33, 1 << 10 }, { 33, 33, 2 << 10 },
  };
  for (int i = 0; i < 4; i++)
    SetTile(probes[i].tx, probes[i].ty, (uint16_t)(1 | probes[i].bank));
  Render(1);
  CHECK(SimTownCanvas_Town() == 1);
  CHECK(SimTownCanvas_Serial() != 0);

  const uint32_t red = 0xFFFF0000, blue = 0xFF0000FF;
  for (int i = 0; i < 4; i++) {
    uint32_t expect = probes[i].bank == (1 << 10) ? red : blue;
    CHECK(CanvasAt(probes[i].tx * 8, probes[i].ty * 8) == expect);
    /* Everything else in the tile is colour 0, i.e. the backdrop. */
    CHECK(CanvasAt(probes[i].tx * 8 + 1, probes[i].ty * 8) == kBackdrop);
  }
  /* A tile left empty stays backdrop, never transparent: the canvas must not
   * punch a hole in the world map drawn beneath it. */
  CHECK(CanvasAt(200, 200) == kBackdrop);
  CHECK((CanvasAt(200, 200) >> 24) == 0xFF);
}

static void TestFlips(void) {
  SimTownCanvas_Reset();
  SetupSources();
  SetTile(0, 0, (uint16_t)(1 | (1 << 10)));                 /* no flip */
  SetTile(2, 0, (uint16_t)(1 | (1 << 10) | 0x4000));        /* flip x */
  SetTile(4, 0, (uint16_t)(1 | (1 << 10) | 0x8000));        /* flip y */
  SetTile(6, 0, (uint16_t)(1 | (1 << 10) | 0xC000));        /* flip both */
  Render(1);
  const uint32_t red = 0xFFFF0000;
  CHECK(CanvasAt(0, 0) == red);                  /* top-left */
  CHECK(CanvasAt(2 * 8 + 7, 0) == red);          /* top-right */
  CHECK(CanvasAt(4 * 8, 7) == red);              /* bottom-left */
  CHECK(CanvasAt(6 * 8 + 7, 7) == red);          /* bottom-right */
}

/* Re-rendering 512x512 on every frame would be wasteful, and re-rendering on
 * no change would upload a full texture 60 times a second. */
static void TestChangeDetection(void) {
  SimTownCanvas_Reset();
  SetupSources();
  SetTile(1, 1, (uint16_t)(1 | (1 << 10)));
  Render(1);
  uint32_t serial = SimTownCanvas_Serial();
  int x, y, w, h;
  CHECK(SimTownCanvas_TakeDirtyRect(&x, &y, &w, &h));
  CHECK(x == 0 && y == 0 && w == kSimTownCanvasPixels &&
        h == kSimTownCanvasPixels);

  Render(1);
  CHECK(SimTownCanvas_Serial() == serial);
  CHECK(!SimTownCanvas_TakeDirtyRect(&x, &y, &w, &h));

  /* Construction happening anywhere in the town, on-screen or not, rewrites
   * the tilemap -- which is the whole reason this renders instead of
   * accumulating captured frames. */
  SetTile(40, 50, (uint16_t)(1 | (2 << 10)));
  Render(1);
  CHECK(SimTownCanvas_Serial() != serial);
  CHECK(CanvasAt(40 * 8, 50 * 8) == 0xFF0000FF);

  /* Animated tile graphics land in VRAM, not the tilemap. */
  serial = SimTownCanvas_Serial();
  SimTownCanvas_TakeDirtyRect(&x, &y, &w, &h);
  g_vram[1 * 16 + 0] = 0x0040;   /* move the lit pixel one column right */
  Render(1);
  CHECK(SimTownCanvas_Serial() != serial);
  CHECK(CanvasAt(1 * 8 + 1, 1 * 8) == 0xFFFF0000);

  /* So does a palette fade. */
  serial = SimTownCanvas_Serial();
  g_cgram[16 + 1] = 0x03E0;
  Render(1);
  CHECK(SimTownCanvas_Serial() != serial);
  CHECK(CanvasAt(1 * 8 + 1, 1 * 8) == 0xFF00FF00);
}

static void TestBrightnessAndTownChange(void) {
  SimTownCanvas_Reset();
  SetupSources();
  SetTile(0, 0, (uint16_t)(1 | (1 << 10)));
  SimTownCanvas_Render(1, g_wram, g_vram, g_cgram, 0, kBackdrop);
  /* Force blank scales every channel to zero, exactly as the captured planes
   * do, so a fade-out does not leave a bright town hanging outside it. */
  CHECK(CanvasAt(0, 0) == 0xFF000000);
  SimTownCanvas_Render(1, g_wram, g_vram, g_cgram, kBrightness, kBackdrop);
  CHECK(CanvasAt(0, 0) == 0xFFFF0000);

  /* A different town must not inherit this one's ground. */
  memset(g_wram + kSimTownTilemapWram, 0, 0x2000);
  SimTownCanvas_Render(2, g_wram, g_vram, g_cgram, kBrightness, kBackdrop);
  CHECK(SimTownCanvas_Town() == 2);
  CHECK(CanvasAt(0, 0) == kBackdrop);
}

static void TestRejectsMissingSources(void) {
  SimTownCanvas_Reset();
  SetupSources();
  SimTownCanvas_Render(0, g_wram, g_vram, g_cgram, kBrightness, kBackdrop);
  CHECK(SimTownCanvas_Serial() == 0);
  SimTownCanvas_Render(1, NULL, g_vram, g_cgram, kBrightness, kBackdrop);
  SimTownCanvas_Render(1, g_wram, NULL, g_cgram, kBrightness, kBackdrop);
  SimTownCanvas_Render(1, g_wram, g_vram, NULL, kBrightness, kBackdrop);
  CHECK(SimTownCanvas_Serial() == 0);
}

int main(void) {
  g_wram = malloc(kWramSize);
  g_vram = malloc(kVramWords * sizeof(uint16_t));
  TestQuadrantAddressing();
  TestFlips();
  TestChangeDetection();
  TestBrightnessAndTownChange();
  TestRejectsMissingSources();
  SimTownCanvas_Reset();
  free(g_wram);
  free(g_vram);
  printf("sim town canvas tests: %s\n", s_failures ? "FAIL" : "pass");
  return s_failures ? 1 : 0;
}
