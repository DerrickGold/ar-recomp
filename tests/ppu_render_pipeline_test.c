/* ppu_render_pipeline_test.c — end-to-end SDL3 present validation with the REAL PPU.
 *
 * Stronger companion to render_pipeline_test.c: instead of a synthetic pixel
 * buffer, this instantiates the actual SNES PPU, points it at a framebuffer via
 * the same PpuBeginDrawing/ppu_runLine path main.c uses, sets a known backdrop
 * color, renders real scanlines, then pushes that framebuffer through the exact
 * SDL3 present + read-back pipeline. It proves the pixels the emulator actually
 * produces (RGB with alpha byte = 0) are VISIBLE after presenting — i.e. the
 * texture blend mode is correct and the screen is not black.
 *
 * Runs headless under the dummy video driver with the software renderer.
 */
#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes/ppu.h"
#include "actraiser_game.h"
#include "sim3d.h"
#include "sim_render_atlas.h"
#include "sim_render_metadata.h"
#include "sim_world_navigation_capture.h"

/* main.c owns this global; the PPU line renderer reads it to pick new/old path. */
bool g_new_ppu = false;

/* Trace/debug hooks the PPU references but that only fire under instrumentation
 * (SNESRECOMP_TRACE / debug server). Stub them so this standalone harness links
 * without pulling in the whole runtime. None run in a normal render. */
int ar_trace_active(void) { return 0; }
void ar_trace_ppumem(uint16_t a, uint8_t v) { (void)a; (void)v; }
void ar_trace_reg(uint16_t a, uint8_t v) { (void)a; (void)v; }
void ar_trace_vmadd(uint16_t a) { (void)a; }
void ar_trace_vram(uint16_t a, uint16_t v) { (void)a; (void)v; }
void ar_vramraw(uint16_t a, uint8_t v, int p) { (void)a; (void)v; (void)p; }
int ar_vramwatch(uint16_t a, uint8_t v) { (void)a; (void)v; return 0; }
void CpuDispatchLogWriteFile(const char *path) { (void)path; }
unsigned g_ar_blk_idx;
uint32_t g_ar_blk_ring[256];
const char *g_last_recomp_func;
uint8_t g_ram[0x20000];
int snes_frame_counter;

static int s_failures;
#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

enum { kW = 256, kH = 224 };

/* Build a BGR555 SNES color word (what CGRAM stores): 5 bits each, R low. */
static uint16_t bgr555(int r5, int g5, int b5) {
  return (uint16_t)((r5 & 0x1f) | ((g5 & 0x1f) << 5) | ((b5 & 0x1f) << 10));
}

/* Expand a 5-bit channel to 8-bit the way the PPU does ((v<<3)|(v>>2)). */
static int expand5(int v5) { return ((v5 & 0x1f) << 3) | ((v5 & 0x1f) >> 2); }

static void set_solid_4bpp_tile(Ppu *ppu, int tile, int color) {
  for (int row = 0; row < 8; row++) {
    uint16_t lo = 0, hi = 0;
    if (color & 1) lo |= 0x00ff;
    if (color & 2) lo |= 0xff00;
    if (color & 4) hi |= 0x00ff;
    if (color & 8) hi |= 0xff00;
    ppu->vram[tile * 16 + row] = lo;
    ppu->vram[tile * 16 + row + 8] = hi;
  }
}

typedef struct VirtualTilemapFixture {
  int min_x, max_x;
  int min_y, max_y;
  uint16_t even_entry;
  uint16_t odd_entry;
  int calls;
  int first_x, last_x;
  int first_y, last_y;
} VirtualTilemapFixture;

static bool lookup_virtual_tile(const void *context, int32_t tile_x,
                                int32_t tile_y, uint16_t *entry) {
  VirtualTilemapFixture *map = (VirtualTilemapFixture *)context;
  if (!map || !entry) return false;
  if (!map->calls) {
    map->first_x = map->last_x = tile_x;
    map->first_y = map->last_y = tile_y;
  } else {
    if (tile_x < map->first_x) map->first_x = tile_x;
    if (tile_x > map->last_x) map->last_x = tile_x;
    if (tile_y < map->first_y) map->first_y = tile_y;
    if (tile_y > map->last_y) map->last_y = tile_y;
  }
  map->calls++;
  if (tile_x < map->min_x || tile_x > map->max_x ||
      tile_y < map->min_y || tile_y > map->max_y)
    return false;
  *entry = (tile_x & 1) ? map->odd_entry : map->even_entry;
  return true;
}

static uint32_t rgb555(int r5, int g5, int b5) {
  return (uint32_t)expand5(r5) << 16 |
      (uint32_t)expand5(g5) << 8 | expand5(b5);
}

static void setup_virtual_bg(Ppu *ppu, int extra, uint8_t *fb,
                             size_t pitch) {
  const int bg1 = kActRaiserPpuLayer_Bg1;
  ppu_reset(ppu);
  memset(fb, 0, pitch);
  ppu->inidisp = 0x0f;
  ppu->bgmode = 1;
  ppu->screenEnabled[0] = (uint8_t)(1u << bg1);
  ppu->cgram[0] = bgr555(0, 0, 0);
  ppu->cgram[0x11] = bgr555(31, 0, 0);  /* native: red */
  ppu->cgram[0x21] = bgr555(0, 0, 31);  /* provider even: blue */
  ppu->cgram[0x31] = bgr555(0, 31, 0);  /* provider odd: green */
  ppu->cgram[0x41] = bgr555(31, 31, 0); /* BG2 priority probe: yellow */
  set_solid_4bpp_tile(ppu, 1, 1);
  set_solid_4bpp_tile(ppu, 2, 1);
  set_solid_4bpp_tile(ppu, 3, 1);
  set_solid_4bpp_tile(ppu, 4, 1);
  ppu->bgTileAdr = 0;
  ppu->bgXsc[bg1] = 0x20 | 3;
  for (int i = 0; i < 0x1000; i++)
    ppu->vram[0x2000 + i] = (uint16_t)(1 | (1 << 10));
  ppu->hScroll[bg1] = 8;
  ppu->vScroll[bg1] = 0;
  PpuSetExtraSpace(ppu, (uint8_t)extra);
  PpuBeginDrawing(ppu, fb, pitch, 0);
}

static void render_first_line(Ppu *ppu) {
  ppu_runLine(ppu, 0);
  ppu_runLine(ppu, 1);
}

static void TestVirtualTilemapMargins(void) {
  enum { kExtra = 8, kWidth = kW + kExtra * 2 };
  const int bg1 = kActRaiserPpuLayer_Bg1;
  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;
  static uint8_t fb[kWidth * 4];
  static uint32_t center_pixels[kW];
  static PpuZbufType center_priority[kW];

  /* The unbound pass is the exact native reference for both resolved pixels
   * and the PPU's priority/color words. */
  setup_virtual_bg(ppu, kExtra, fb, sizeof(fb));
  render_first_line(ppu);
  memcpy(center_pixels, (uint32_t *)(void *)fb + kExtra,
         sizeof(center_pixels));
  memcpy(center_priority,
         ppu->bgBuffers[0].data + kPpuExtraLeftRight,
         sizeof(center_priority));

  VirtualTilemapFixture map = {
    .min_x = 0, .max_x = 33, .min_y = 0, .max_y = 0,
    .even_entry = (uint16_t)(2 | (2 << 10)),
    .odd_entry = (uint16_t)(3 | (3 << 10)),
  };
  PpuVirtualTilemapBinding binding = {
    .lookup = lookup_virtual_tile,
    .context = &map,
    .camera_x = 8,
    .camera_y = 0,
    .hscroll_anchor = 8,
    .vscroll_anchor = 0,
  };
  setup_virtual_bg(ppu, kExtra, fb, sizeof(fb));
  CHECK(PpuSetVirtualTilemap(ppu, (uint8_t)bg1, &binding));
  render_first_line(ppu);
  const uint32_t *row = (const uint32_t *)(const void *)fb;
  CHECK(row[0] == rgb555(0, 0, 31));
  CHECK(row[kWidth - 1] == rgb555(0, 31, 0));
  CHECK(memcmp(row + kExtra, center_pixels, sizeof(center_pixels)) == 0);
  CHECK(memcmp(ppu->bgBuffers[0].data + kPpuExtraLeftRight,
               center_priority, sizeof(center_priority)) == 0);

  /* CGRAM stays live: the same provider tile word immediately follows a
   * palette swap without rebuilding or mutating the virtual map. */
  const uint32_t old_left = row[0];
  ppu->cgram[0x21] = bgr555(31, 0, 31);
  render_first_line(ppu);
  row = (const uint32_t *)(const void *)fb;
  CHECK(row[0] == rgb555(31, 0, 31));
  CHECK(row[0] != old_left);

  /* A false lookup is a transparent finite-world edge, not wrapped VRAM. */
  map.max_x = 32;
  render_first_line(ppu);
  row = (const uint32_t *)(const void *)fb;
  CHECK(row[kWidth - 1] == 0);
  CHECK(row[kExtra] == center_pixels[0]);

  /* Windows are evaluated before lookup. A hardware window ending at 255 is
   * extended through the right widescreen margin by the existing policy. */
  map.max_x = 33;
  ppu->screenWindowed[0] = (uint8_t)(1u << bg1);
  ppu->windowsel = kWindow1Enabled;
  ppu->window1left = 200;
  ppu->window1right = 255;
  render_first_line(ppu);
  row = (const uint32_t *)(const void *)fb;
  CHECK(row[kWidth - 1] == 0);
  CHECK(row[kExtra + 199] == center_pixels[199]);
  CHECK(row[kExtra + 200] == 0);

  /* Binding is transactional and frame policy cannot leak through either a
   * widescreen-policy reset or a PPU reset. */
  PpuVirtualTilemapBinding bad = binding;
  bad.hscroll_anchor = 0x400;
  CHECK(!PpuSetVirtualTilemap(ppu, (uint8_t)bg1, &bad));
  CHECK(!PpuSetVirtualTilemap(ppu, 2, &binding));
  CHECK(ppu->virtualTilemap[bg1].lookup == lookup_virtual_tile);
  PpuSetExtraSpace(ppu, kExtra);
  CHECK(ppu->virtualTilemap[bg1].lookup == NULL);
  CHECK(PpuSetVirtualTilemap(ppu, (uint8_t)bg1, &binding));
  ppu_reset(ppu);
  CHECK(ppu->virtualTilemap[bg1].lookup == NULL);

  ppu_free(ppu);
  g_new_ppu = saved_new_ppu;
}

static void TestVirtualTilemapEffects(void) {
  enum { kExtra = 8, kWidth = kW + kExtra * 2 };
  const int bg1 = kActRaiserPpuLayer_Bg1;
  const int bg2 = kActRaiserPpuLayer_Bg2;
  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;
  static uint8_t fb[kWidth * 4];

  VirtualTilemapFixture map = {
    .min_x = -8, .max_x = 64, .min_y = -8, .max_y = 64,
    .even_entry = (uint16_t)(2 | (2 << 10)),
    .odd_entry = (uint16_t)(3 | (3 << 10) | 0x2000),
  };
  PpuVirtualTilemapBinding binding = {
    .lookup = lookup_virtual_tile,
    .context = &map,
    .camera_x = 8,
    .camera_y = 0,
    .hscroll_anchor = 0,
    .vscroll_anchor = 0,
  };

  /* Signed 10-bit anchoring preserves the nearest phase displacement across
   * wrap: current 0 vs anchor 1023 is +1, and the reverse is -1. */
  setup_virtual_bg(ppu, kExtra, fb, sizeof(fb));
  ppu->hScroll[bg1] = 0;
  binding.camera_x = 7;
  binding.hscroll_anchor = 0x3ff;
  map.calls = 0;
  CHECK(PpuSetVirtualTilemap(ppu, (uint8_t)bg1, &binding));
  render_first_line(ppu);
  CHECK(map.first_x == 0);

  setup_virtual_bg(ppu, kExtra, fb, sizeof(fb));
  ppu->hScroll[bg1] = 0x3ff;
  binding.camera_x = 8;
  binding.hscroll_anchor = 0;
  map.calls = 0;
  CHECK(PpuSetVirtualTilemap(ppu, (uint8_t)bg1, &binding));
  render_first_line(ppu);
  CHECK(map.first_x == -1);

  /* A live per-line scroll change shifts virtual world lookup rather than
   * freezing the margin at the frame anchor (the HBlank/HDMA contract). */
  setup_virtual_bg(ppu, kExtra, fb, sizeof(fb));
  ppu->hScroll[bg1] = 0;
  binding.camera_x = 8;
  binding.hscroll_anchor = 0;
  CHECK(PpuSetVirtualTilemap(ppu, (uint8_t)bg1, &binding));
  render_first_line(ppu);
  uint32_t before_scroll = ((const uint32_t *)(const void *)fb)[0];
  ppu->hScroll[bg1] = 8;
  render_first_line(ppu);
  uint32_t after_scroll = ((const uint32_t *)(const void *)fb)[0];
  CHECK(before_scroll == rgb555(0, 0, 31));
  CHECK(after_scroll == rgb555(0, 31, 0));

  /* BG priority is still resolved in the shared z buffer. BG2 high priority
   * covers the provider's low-priority even tile on the left; BG1 high
   * priority covers BG2 on the right. */
  setup_virtual_bg(ppu, kExtra, fb, sizeof(fb));
  ppu->screenEnabled[0] |= (uint8_t)(1u << bg2);
  ppu->bgXsc[bg2] = 0x30 | 3;
  for (int i = 0; i < 0x1000; i++)
    ppu->vram[0x3000 + i] = (uint16_t)(4 | (4 << 10) | 0x2000);
  ppu->hScroll[bg1] = ppu->hScroll[bg2] = 0;
  ppu->vScroll[bg1] = ppu->vScroll[bg2] = 0;
  binding.camera_x = 8;
  binding.hscroll_anchor = 0;
  CHECK(PpuSetVirtualTilemap(ppu, (uint8_t)bg1, &binding));
  render_first_line(ppu);
  const uint32_t *row = (const uint32_t *)(const void *)fb;
  CHECK(row[0] == rgb555(31, 31, 0));
  CHECK(row[kWidth - 1] == rgb555(0, 31, 0));

  /* Color math remains downstream of the provider. Enabling fixed-color add
   * for BG1 changes its margin pixel without changing the lookup result. */
  setup_virtual_bg(ppu, kExtra, fb, sizeof(fb));
  ppu->hScroll[bg1] = 0;
  map.even_entry = (uint16_t)(1 | (1 << 10));
  binding.camera_x = 8;
  CHECK(PpuSetVirtualTilemap(ppu, (uint8_t)bg1, &binding));
  render_first_line(ppu);
  const uint32_t no_math = ((const uint32_t *)(const void *)fb)[0];
  ppu->fixedColor = bgr555(0, 0, 15);
  ppu->cgadsub = 1u << bg1;
  render_first_line(ppu);
  const uint32_t with_math = ((const uint32_t *)(const void *)fb)[0];
  CHECK(no_math == rgb555(31, 0, 0));
  CHECK(with_math != no_math);
  CHECK((with_math & 0xff) != 0);

  /* Flip and mosaic are applied to the live VRAM character data. Tile 2 has
   * only source x=0 opaque; mosaic size 4 expands that pixel to four columns,
   * while H-flip moves it to the opposite edge. */
  setup_virtual_bg(ppu, kExtra, fb, sizeof(fb));
  memset(&ppu->vram[2 * 16], 0, 16 * sizeof(ppu->vram[0]));
  for (int y = 0; y < 8; y++)
    ppu->vram[2 * 16 + y] = 1u << 7;
  ppu->hScroll[bg1] = 0;
  map.even_entry = (uint16_t)(2 | (2 << 10));
  binding.camera_x = 8;
  CHECK(PpuSetVirtualTilemap(ppu, (uint8_t)bg1, &binding));
  render_first_line(ppu);
  row = (const uint32_t *)(const void *)fb;
  CHECK(row[0] == rgb555(0, 0, 31));
  CHECK(row[1] == 0);
  ppu->mosaic = (uint8_t)((3 << 4) | (1u << bg1));
  render_first_line(ppu);
  row = (const uint32_t *)(const void *)fb;
  CHECK(row[0] == rgb555(0, 0, 31));
  CHECK(row[1] == rgb555(0, 0, 31));
  CHECK(row[2] == rgb555(0, 0, 31));
  CHECK(row[3] == rgb555(0, 0, 31));
  map.even_entry |= 0x4000;
  ppu->mosaic = 0;
  render_first_line(ppu);
  row = (const uint32_t *)(const void *)fb;
  CHECK(row[0] == 0);
  CHECK(row[7] == rgb555(0, 0, 31));

  ppu_free(ppu);
  g_new_ppu = saved_new_ppu;
}

static void TestVirtualTilemapVerticalMargin(void) {
  enum { kTop = 8, kRows = kTop + 1 };
  const int bg1 = kActRaiserPpuLayer_Bg1;
  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;
  static uint8_t fb[kW * kRows * 4];

  ppu_reset(ppu);
  memset(fb, 0, sizeof(fb));
  ppu->inidisp = 0x0f;
  ppu->bgmode = 1;
  ppu->screenEnabled[0] = (uint8_t)(1u << bg1);
  ppu->cgram[0x11] = bgr555(31, 0, 0);
  ppu->cgram[0x21] = bgr555(0, 0, 31);
  set_solid_4bpp_tile(ppu, 1, 1);
  set_solid_4bpp_tile(ppu, 2, 1);
  ppu->bgXsc[bg1] = 0x20 | 3;
  for (int i = 0; i < 0x1000; i++)
    ppu->vram[0x2000 + i] = (uint16_t)(1 | (1 << 10));
  PpuSetExtraSpace(ppu, 0);
  PpuSetExtraVerticalSpace(ppu, kTop, 0);
  PpuBeginDrawing(ppu, fb, kW * sizeof(uint32_t), 0);

  VirtualTilemapFixture map = {
    .min_x = 0, .max_x = 31, .min_y = 0, .max_y = 0,
    .even_entry = (uint16_t)(2 | (2 << 10)),
    .odd_entry = (uint16_t)(2 | (2 << 10)),
  };
  PpuVirtualTilemapBinding binding = {
    .lookup = lookup_virtual_tile,
    .context = &map,
    .camera_x = 0,
    .camera_y = 6,
    .hscroll_anchor = 0,
    .vscroll_anchor = 0x3ff,
  };
  CHECK(PpuSetVirtualTilemap(ppu, (uint8_t)bg1, &binding));
  ppu_runLine(ppu, 0);
  for (int line = 1 - kTop; line <= 0; line++)
    ppu_runMarginLine(ppu, line);
  ppu_runLine(ppu, 1);
  const uint32_t *pixels = (const uint32_t *)(const void *)fb;
  for (int row = 0; row < kTop; row++)
    CHECK(pixels[row * kW] == rgb555(0, 0, 31));
  CHECK(pixels[kTop * kW] == rgb555(31, 0, 0));
  CHECK(map.first_y == 0 && map.last_y == 0);

  ppu_free(ppu);
  g_new_ppu = saved_new_ppu;
}

static void TestObjRangeRaster(void) {
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;
  ppu_reset(ppu);
  ppu->inidisp = 0x0f;
  ppu->obsel = 0;  /* 8x8 small objects; OBJ tiles start at VRAM word 0. */
  ppu->cgram[0x81] = bgr555(31, 0, 0);
  ppu->cgram[0x82] = bgr555(0, 0, 31);
  set_solid_4bpp_tile(ppu, 0, 1);
  set_solid_4bpp_tile(ppu, 1, 2);

  /* Two fully overlapping priority-2 entries. Normal OAM order makes slot 0
   * own the range; priority rotation beginning at slot 1 reverses ownership. */
  ppu->oam[0] = 10 | (20 << 8);
  ppu->oam[1] = 0 | (2 << 12);
  ppu->oam[2] = 10 | (20 << 8);
  ppu->oam[3] = 1 | (2 << 12);
  PpuObjRangeBounds bounds;
  CHECK(PpuGetObjRangeBounds(ppu, 0, 2, 2, &bounds));
  CHECK(bounds.x0 == 10 && bounds.y0 == 20 &&
        bounds.x1 == 18 && bounds.y1 == 28);

  uint32_t pixels[8 * 8];
  CHECK(PpuRasterizeObjRange(ppu, 0, 2, 2, &bounds, pixels, 8, 8,
                             8 * sizeof(uint32_t)));
  CHECK(pixels[0] == 0xffff0000u);
  CHECK(pixels[63] == 0xffff0000u);

  ppu->oamaddh = 0x80;
  ppu->oamaddl = 2;  /* byte index 2 = OAM slot 1 */
  CHECK(PpuRasterizeObjRange(ppu, 0, 2, 2, &bounds, pixels, 8, 8,
                             8 * sizeof(uint32_t)));
  CHECK(pixels[0] == 0xff0000ffu);

  /* High-OAM x/size bits and vertical OAM wrap are interpreted by the same
   * bounds path used by scanout: x=$1fc -> -4, y=250 -> -6, large=16. */
  ppu->oam[4] = 0xfc | (250 << 8);
  ppu->oam[5] = 0 | (1 << 12);
  ppu->highOam[0] = (1 << 4) | (1 << 5);
  CHECK(PpuGetObjRangeBounds(ppu, 2, 1, 1, &bounds));
  CHECK(bounds.x0 == -4 && bounds.y0 == -6 &&
        bounds.x1 == 12 && bounds.y1 == 10);
  CHECK(!PpuGetObjRangeBounds(ppu, 2, 1, 0, &bounds));

  /* An asymmetric source pixel proves horizontal and vertical flip handling
   * is shared with scanout rather than approximated by the atlas caller. */
  memset(&ppu->vram[2 * 16], 0, 16 * sizeof(ppu->vram[0]));
  ppu->vram[2 * 16] = 1u << 6;  /* source (x=1,y=0), color index 1 */
  ppu->oamaddh = 0;
  ppu->oam[6] = 30 | (40 << 8);
  ppu->oam[7] = 2;
  CHECK(PpuGetObjRangeBounds(ppu, 3, 1, 0, &bounds));
  CHECK(PpuRasterizeObjRange(ppu, 3, 1, 0, &bounds, pixels, 8, 8,
                             8 * sizeof(uint32_t)));
  CHECK(pixels[1] == 0xffff0000u && pixels[0] == 0);
  ppu->oam[7] |= 0xc000;  /* H+V flip -> destination (x=6,y=7). */
  CHECK(PpuRasterizeObjRange(ppu, 3, 1, 0, &bounds, pixels, 8, 8,
                             8 * sizeof(uint32_t)));
  CHECK(pixels[7 * 8 + 6] == 0xffff0000u && pixels[1] == 0);

  ppu_free(ppu);
}

static void TestWorldNavigationPartialBrightnessCapture(void) {
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;
  ppu_reset(ppu);
  ppu->bgmode = 7;
  ppu->cgram[0] = bgr555(31, 0, 0);
  /* The action-entry form is the smallest valid navigation composition: all
   * OAM hidden, so this test isolates eligibility from sprite raster data. */
  for (int slot = 0; slot < 128; slot++) {
    ppu->oam[slot * 2] = 0xE000;
    ppu->oam[slot * 2 + 1] = 0xE000;
  }

  for (uint8_t brightness = 0; brightness <= 15; brightness++) {
    SimFrameData frame = {0};
    frame.view = kSimView_WorldNavigation;
    frame.world_navigation_scene.valid = true;
    ppu->inidisp = brightness;
    CHECK(SimWorldNavigationCapture_Capture(&frame, ppu));
    CHECK(frame.view == kSimView_WorldNavigation);
    CHECK(frame.world_navigation_brightness == brightness);
    CHECK(frame.separated_backdrop_argb == 0xffff0000u);
    CHECK(frame.world_navigation_scene.composition.valid);
    CHECK(frame.world_navigation_scene.composition.empty_animation);
  }

  SimFrameData blank = {0};
  blank.view = kSimView_WorldNavigation;
  blank.world_navigation_scene.valid = true;
  ppu->inidisp = 0x8f;
  CHECK(!SimWorldNavigationCapture_Capture(&blank, ppu));
  CHECK(blank.view == kSimView_AuthenticFallback);
  CHECK(blank.world_navigation_brightness == 15);

  ppu_free(ppu);
}

static void BeginSimRecord(uint16_t record, bool world, uint16_t cursor,
                           uint16_t world_x, uint16_t world_y) {
  SimRenderMetadata_BeginRecord(
      record, world, false, 0xe000, world_x, world_y, 1, 0, 0, cursor);
}

static void TestSemanticAtlasPacking(void) {
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;
  ppu_reset(ppu);
  ppu->inidisp = 0x0f;
  ppu->cgram[0x81] = bgr555(31, 31, 31);
  set_solid_4bpp_tile(ppu, 0, 1);

  SimRenderMetadata_Reset();
  BeginSimRecord(kActRaiserWram_SimWorldRecords, true, 0, 110, 70);
  SimRenderMetadata_RecordPart(0, 1u << 12);
  SimRenderMetadata_RecordPart(4, 2u << 12);
  SimRenderMetadata_EndRecord(8);
  ppu->oam[0] = 5 | (7 << 8);
  ppu->oam[1] = 1u << 12;
  ppu->oam[2] = 13 | (7 << 8);
  ppu->oam[3] = 2u << 12;

  CHECK(SimRenderAtlas_Build(ppu, 100, 50));
  SimAtlasBuildInput atlas;
  CHECK(SimRenderMetadata_CopyAtlasInput(&atlas));
  CHECK(atlas.object_count == 2);
  CHECK(atlas.objects[0].atlas_valid && atlas.objects[1].atlas_valid);
  CHECK(atlas.objects[0].atlas_x == 1 && atlas.objects[0].atlas_y == 1);
  CHECK(atlas.objects[0].atlas_w == 8 && atlas.objects[0].atlas_h == 8);
  CHECK(atlas.objects[1].atlas_x == 10 && atlas.objects[1].atlas_y == 1);
  CHECK(atlas.objects[0].local_x0 == -5);
  CHECK(atlas.objects[0].local_y0 == -13);
  CHECK(atlas.objects[0].foot_x == 113 && atlas.objects[0].foot_y == 65);
  CHECK(atlas.objects[1].foot_x == 113 && atlas.objects[1].foot_y == 65);
  CHECK(g_sim_obj_atlas_pixels[1 * kSimObjAtlasWidth + 1] ==
        0xffffffffu);

  /* A mixed real/synthetic composition is packed from exact parts in emitter
   * order. The second part sits past OAM's positive-X decode boundary: an OAM
   * byte would alias it to the opposite side, while the explicit channel keeps
   * the union at its real 16-pixel width. Real OAM consumption stays one slot. */
  ppu_reset(ppu);
  ppu->inidisp = 0x0f;
  ppu->cgram[0x81] = bgr555(31, 31, 31);
  set_solid_4bpp_tile(ppu, 0, 1);
  SimRenderMetadata_Reset();
  BeginSimRecord(kActRaiserWram_SimWorldRecords, true, 0, 250, 30);
  {
    const PpuObjPart real = {250, 30, 1u << 12, 8};
    const PpuObjPart synthetic = {258, 30, 1u << 12, 8};
    SimRenderMetadata_RecordPart(0, real.tile_attr);
    SimRenderMetadata_RecordExactOamPart(&real);
    SimRenderMetadata_RecordSyntheticPart(4, &synthetic);
    SimRenderMetadata_EndRecord(4);
    ppu->oam[0] = (uint16_t)(250 | (30 << 8));
    ppu->oam[1] = real.tile_attr;
    PpuSetObjExactPosition(ppu, 0, real.x, real.y);
  }
  CHECK(SimRenderAtlas_Build(ppu, 0, 0));
  CHECK(SimRenderMetadata_CopyAtlasInput(&atlas));
  CHECK(atlas.object_count == 1);
  CHECK(atlas.part_count == 2);
  CHECK(atlas.objects[0].oam_count == 1);
  CHECK(atlas.objects[0].part_count == 2);
  CHECK(atlas.objects[0].synthetic_part_count == 1);
  CHECK(atlas.objects[0].atlas_valid);
  CHECK(atlas.objects[0].atlas_w == 16);
  CHECK(atlas.objects[0].atlas_h == 8);
  CHECK(atlas.objects[0].local_x0 == 0);
  CHECK(atlas.objects[0].local_x1 == 16);
  {
    uint8_t wram[kActRaiserWramSize] = {0};
    wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
    wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
    SimFrameData mixed;
    SimRenderMetadata_CaptureFrame(&mixed, wram, true, false, 0, 0, 0);
    CHECK(mixed.emitted_oam_count == 1);
    CHECK(mixed.synthetic_part_count == 1);
    CHECK(mixed.synthetic_part_overflow_count == 0);
    CHECK(mixed.sources[0].synthetic_parts == 1);
  }

  /* Fifty independent 64x64 fragments cannot all fit with the mandatory
   * gutter in a 512x512 atlas.
   *
   * This once required the builder to invalidate every descriptor and publish
   * AtlasOverflow -- "never the first 49 as a partial success" -- on the
   * principle that a partial atlas is untrustworthy. Reversed once the SIM 3D
   * view shipped, because an invalid atlas invalidated the frame's metadata,
   * which dropped the whole view to the flat composite: a full-screen
   * perspective flash standing in for one sprite that would not pack. The ROM
   * makes actors vanish at the screen edge anyway, so purging the fragment is
   * both proportionate and authentic. The condition is still reported --
   * purged objects keep atlas_valid clear and the D1 census counts them, so a
   * checkpoint still fails on it -- it just no longer costs the frame. */
  ppu_reset(ppu);
  ppu->inidisp = 0x0f;
  ppu->obsel = 2 << 5;  /* size pair 8/64 */
  SimRenderMetadata_Reset();
  for (int slot = 0; slot < 50; slot++) {
    bool world = slot >= kActRaiserSimFixedRecordCount;
    int record_index = world ? slot - kActRaiserSimFixedRecordCount : slot;
    uint16_t record = (uint16_t)(
        (world ? kActRaiserWram_SimWorldRecords
               : kActRaiserWram_SimFixedRecords) +
        record_index * (world ? kActRaiserSimWorldRecordStride
                              : kActRaiserSimFixedRecordStride));
    BeginSimRecord(record, world, (uint16_t)(slot * 4), 0, 0);
    SimRenderMetadata_RecordPart((uint16_t)(slot * 4), 1u << 12);
    SimRenderMetadata_EndRecord((uint16_t)((slot + 1) * 4));
    uint8_t index = (uint8_t)(slot * 2);
    ppu->oam[index] = 0;
    ppu->oam[index + 1] = 1u << 12;
    ppu->highOam[index >> 3] |= (uint8_t)(1u << ((index & 7) + 1));
  }
  CHECK(SimRenderAtlas_Build(ppu, 0, 0));

  uint8_t wram[kActRaiserWramSize] = {0};
  wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  wram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_Fillmore;
  SimFrameData frame;
  SimRenderMetadata_CaptureFrame(
      &frame, wram, true, false, 0, 0, 0);
  /* The frame survives, which is the entire point of the reversal. */
  CHECK(frame.metadata_valid);
  CHECK(frame.atlas_valid);
  CHECK(!(frame.integrity_flags & kSimMetadataIntegrity_AtlasOverflow));
  int packed = 0, purged = 0;
  for (int i = 0; i < frame.object_count; i++) {
    if (frame.objects[i].atlas_valid) packed++;
    else purged++;
  }
  /* 512 / (64 + 1) = 7 columns of 7 rows, so 49 of the 50 pack. */
  CHECK(packed == 49);
  CHECK(purged == 1);

  ppu_free(ppu);
}

static void TestSim3DFlatComposition(void) {
  enum { width = 3, height = 2 };
  uint32_t storage[kSim3DPlane_Count][width * height];
  uint8_t *planes[kSim3DPlane_Count];
  memset(storage, 0, sizeof(storage));
  for (int plane = 0; plane < kSim3DPlane_Count; plane++)
    planes[plane] = (uint8_t *)storage[plane];

  storage[kSim3DPlane_Bg3Low][1] = 0xffff0000u;
  storage[kSim3DPlane_Obj0][1] = 0xff00ff00u;
  storage[kSim3DPlane_Obj1][1] = 0x000000ffu; /* transparent despite RGB */
  storage[kSim3DPlane_Bg1High][1] = 0xff0000ffu;
  storage[kSim3DPlane_Obj3][1] = 0xffffff00u;
  storage[kSim3DPlane_Bg3High][1] = 0xffff00ffu;

  uint32_t output[(width + 1) * height];
  memset(output, 0xcc, sizeof(output));
  Sim3D_ComposeFlatPixels(
      output, width, height, (width + 1) * (int)sizeof(uint32_t),
      0xff112233u, 1, 3, planes, 0, 0);
  CHECK(output[0] == 0xff000000u);
  CHECK(output[1] == 0xffff00ffu); /* last hardware-rank plane wins */
  CHECK(output[2] == 0xff112233u);
  CHECK(output[width + 1] == 0xff000000u);
  CHECK(output[width + 2] == 0xff112233u);
  CHECK(output[width + 3] == 0xff112233u);

  Sim3D_ComposeFlatPixels(
      output, width, 1, (width + 1) * (int)sizeof(uint32_t),
      0xff112233u, 1, 3, planes, 1u << kSim3DPlane_Obj0, 0);
  CHECK(output[1] == 0xff00ff00u);

  Sim3D_ComposeFlatPixels(
      output, width, 1, (width + 1) * (int)sizeof(uint32_t),
      0xff112233u, 1, 3, planes, 1u << kSim3DPlane_Obj1, 0);
  CHECK(output[1] == 0xff112233u); /* alpha-zero capture cannot cover */
}

static void TestSim3DWidescreenHudCaptureHandoff(void) {
  CHECK(Sim3D_ObjPlaneForPriority(0) == kSim3DPlane_Obj0);
  CHECK(Sim3D_ObjPlaneForPriority(1) == kSim3DPlane_Obj1);
  CHECK(Sim3D_ObjPlaneForPriority(2) == kSim3DPlane_Obj2);
  CHECK(Sim3D_ObjPlaneForPriority(3) == kSim3DPlane_Obj3);
  CHECK(Sim3D_ObjPlaneForPriority(-1) == -1);
  CHECK(Sim3D_ObjPlaneForPriority(4) == -1);

  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;
  ppu_reset(ppu);
  ppu->inidisp = 0x0f;
  ppu->bgmode = 9;
  ppu->screenEnabled[0] = 0x17;
  ppu->screenEnabled[1] = 0;
  PpuSetWidescreenHudSplit(
      ppu, kActRaiserSimulationHudHeight,
      kActRaiserSimulationHudSplit, kActRaiserSimulationHudSplit,
      kActRaiserSimulationHudHeight, kActRaiserSimulationHudHeight);
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg3, 0, 0,
      kActRaiserAuthenticWidth, kActRaiserSimulationHudHeight,
      kPpuOverlayFlag_RemoveFromGame));
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Obj, 0, 0,
      kActRaiserAuthenticWidth, kActRaiserSimulationHudHeight,
      kPpuOverlayFlag_RemoveFromGame));
  CHECK(PpuSetOverlayOamRange(
      ppu, kActRaiserHudObjOamFirst, kActRaiserHudObjOamCount));

  /* PpuSetOverlayCapture stores flags through a WHITELIST, so a flag that is
   * declared in ppu.h but missing from that mask is accepted by the setter and
   * then silently dropped -- the capture behaves as if the caller never asked.
   * That is how kPpuOverlayFlag_MarkBgHalfAdd shipped inert on first attempt:
   * the F4 log line reported "captured at 50% alpha" while every captured pixel
   * came back 0xff. Assert every declared flag survives a round trip. */
  {
    const uint8_t kAllFlags = kPpuOverlayFlag_RemoveFromGame |
                              kPpuOverlayFlag_MarkObjColorMath |
                              kPpuOverlayFlag_MarkBgHalfAdd;
    CHECK(PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg2, 0, 0,
                               kActRaiserAuthenticWidth,
                               kActRaiserAuthenticHeight, kAllFlags));
    CHECK(ppu->overlayCaptures[kPpuOverlaySource_Bg2].flags == kAllFlags);
    /* And an undeclared bit is still rejected -- the whitelist must stay a
     * whitelist rather than becoming a passthrough. */
    CHECK(PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg2, 0, 0,
                               kActRaiserAuthenticWidth,
                               kActRaiserAuthenticHeight,
                               (uint8_t)(kAllFlags | 0x80)));
    CHECK(ppu->overlayCaptures[kPpuOverlaySource_Bg2].flags == kAllFlags);
    PpuClearOverlayCaptures(ppu);
  }

  const int extra = 43;
  Sim3DCaptureRequest request = {
    .town = true,
    .master_enabled = true,
    .renderer_ready = true,
    .requested_features = kSimFeature_SeparatedComposite,
    .width = kActRaiserAuthenticWidth + 2 * extra,
    .height = kActRaiserAuthenticHeight,
  };
  Sim3D_BeginFrame();
  CHECK(Sim3D_PrepareCapture(ppu, &request));
  const PpuOverlayCapture *bg3 =
      &ppu->overlayCaptures[kPpuOverlaySource_Bg3];
  const PpuOverlayCapture *obj =
      &ppu->overlayCaptures[kPpuOverlaySource_Obj];
  CHECK(bg3->x0 == -extra && bg3->x1 == kActRaiserAuthenticWidth + extra);
  CHECK(bg3->y0 == 0 && bg3->y1 == kActRaiserAuthenticHeight);
  CHECK(bg3->flags == 0);
  CHECK(obj->x0 == -extra && obj->x1 == kActRaiserAuthenticWidth + extra);
  CHECK(obj->oamFirst == 0 && obj->oamCount == 128);
  CHECK(Sim3D_BeginFrame());

  /* An unrelated layer capture still owns its source and must fail closed. */
  PpuClearOverlayCaptures(ppu);
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg1, 0, 0, 16, 16,
      kPpuOverlayFlag_RemoveFromGame));
  CHECK(!Sim3D_PrepareCapture(ppu, &request));
  CHECK(!Sim3D_BeginFrame());
  ppu_free(ppu);
}

static void TestOverlayContentMetadata(void) {
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;
  ppu_reset(ppu);

  static uint8_t fb[kW * 2 * sizeof(uint32_t)];
  static uint32_t primary[kW * 2];
  static uint32_t high[kW * 2];
  memset(fb, 0, sizeof(fb));
  memset(primary, 0, sizeof(primary));
  memset(high, 0, sizeof(high));

  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;
  ppu->inidisp = 0x0f;
  ppu->bgmode = 1;
  ppu->screenEnabled[0] = 1u << kActRaiserPpuLayer_Bg2;
  ppu->cgram[0x21] = bgr555(31, 0, 31);
  set_solid_4bpp_tile(ppu, 1, 1);
  ppu->bgTileAdr = 0;
  ppu->bgXsc[kActRaiserPpuLayer_Bg2] = 0x20;
  for (int i = 0; i < 0x400; i++)
    ppu->vram[0x2000 + i] = (uint16_t)(1 | (2 << 10));

  PpuBeginDrawing(ppu, fb, kW * sizeof(uint32_t), 0);
  CHECK(PpuBindOverlaySurface(
      ppu, kPpuOverlaySource_Bg2, (uint8_t *)primary,
      kW * sizeof(uint32_t)));
  CHECK(PpuBindOverlayPrioSurface(
      ppu, kPpuOverlaySource_Bg2, 1, (uint8_t *)high));
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg2, 0, 0, kW, 2, 0));

  ppu_runLine(ppu, 0);
  ppu_runLine(ppu, 1);
  CHECK(PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg2, 0));
  CHECK(!PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg2, 1));

  /* The tile-priority bit routes the same art to the high surface. Frame start
   * must clear the prior content bits before this line is rendered. */
  for (int i = 0; i < 0x400; i++)
    ppu->vram[0x2000 + i] |= 1u << 13;
  ppu_runLine(ppu, 0);
  ppu_runLine(ppu, 1);
  CHECK(!PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg2, 0));
  CHECK(PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg2, 1));

  /* An active capture with its layer disabled still clears the surfaces, but
   * correctly reports no content in either destination. */
  ppu->screenEnabled[0] = 0;
  ppu_runLine(ppu, 0);
  ppu_runLine(ppu, 1);
  CHECK(!PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg2, 0));
  CHECK(!PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg2, 1));
  CHECK(!PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg2, 4));
  CHECK(!PpuOverlaySurfaceHasContent(NULL, kPpuOverlaySource_Bg2, 0));

  g_new_ppu = saved_new_ppu;
  ppu_free(ppu);
}

/* A vertical margin is shared by every captured plane, but the planes do not
 * necessarily share a camera. Fillmore act 2 has BG1 deep in a tall castle
 * while BG2 remains at Y=0; blindly wrapping BG2's negative margin rows reads
 * red high-priority geometry from the bottom of its 512px tilemap. Prove the
 * layer-local clip removes only that wrapped BG2 contribution, preserves BG1,
 * and cannot affect the first authentic scanline. */
static void TestVerticalMarginLayerClip(void) {
  enum { kTop = 32, kRows = kTop + 1, kPitch = kW * 4 };
  const int bg1 = kActRaiserPpuLayer_Bg1;
  const int bg2 = kActRaiserPpuLayer_Bg2;

  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;

  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;
  static uint8_t fb[kW * kRows * 4];
  static uint32_t bg1_capture[kW * kRows];
  static uint32_t bg2_capture[kW * kRows];
  uint32_t authentic_bg2 = 0;

  static const int clip_cases[] = { -1, 0, 4 };
  for (size_t case_index = 0;
       case_index < sizeof(clip_cases) / sizeof(clip_cases[0]);
       case_index++) {
    const int clip_rows = clip_cases[case_index];
    ppu_reset(ppu);
    memset(fb, 0, sizeof(fb));
    memset(bg1_capture, 0, sizeof(bg1_capture));
    memset(bg2_capture, 0, sizeof(bg2_capture));

    ppu->inidisp = 0x0f;
    ppu->bgmode = 1;
    ppu->screenEnabled[0] = (uint8_t)((1u << bg1) | (1u << bg2));
    ppu->cgram[0x11] = bgr555(0, 0, 31); /* BG1 palette 1, blue */
    ppu->cgram[0x21] = bgr555(31, 0, 0); /* BG2 palette 2, red */
    ppu->cgram[0x22] = bgr555(0, 31, 0); /* BG2 palette 2, green */
    set_solid_4bpp_tile(ppu, 1, 1);
    set_solid_4bpp_tile(ppu, 2, 2);
    set_solid_4bpp_tile(ppu, 3, 1);
    ppu->bgTileAdr = 0;

    /* 32x64 tilemaps. At vScroll 0, margin line -31 selects pixel row
     * 993 -> physical row 481 in this 512px map (the same wrap as the live
     * Fillmore BG2). BG1 is blue throughout. BG2 is red in the wrapped bottom
     * half but green on authentic row 1, making provenance unambiguous. */
    ppu->bgXsc[bg1] = 0x30 | 0x2;
    ppu->bgXsc[bg2] = 0x20 | 0x2;
    for (int i = 0; i < 0x800; i++) {
      ppu->vram[0x3000 + i] = (uint16_t)(3 | (1 << 10));
      ppu->vram[0x2000 + i] = (uint16_t)(1 | (2 << 10));
    }
    for (int x = 0; x < 32; x++)
      ppu->vram[0x2000 + x] = (uint16_t)(2 | (2 << 10));
    ppu->hScroll[bg1] = ppu->hScroll[bg2] = 0;
    ppu->vScroll[bg1] = ppu->vScroll[bg2] = 0;

    PpuSetExtraVerticalSpace(ppu, kTop, 0);
    if (clip_rows >= 0)
      PpuSetVerticalMarginLayerClip(ppu, (uint8_t)bg2, clip_rows);
    PpuBeginDrawing(ppu, fb, kPitch, 0);
    CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg1,
                                (uint8_t *)bg1_capture, kPitch));
    CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2,
                                (uint8_t *)bg2_capture, kPitch));
    CHECK(PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg1,
                               0, -kTop, kW, kRows,
                               kPpuOverlayFlag_RemoveFromGame));
    CHECK(PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg2,
                               0, -kTop, kW, kRows,
                               kPpuOverlayFlag_RemoveFromGame));

    ppu_runLine(ppu, 0);
    for (int line = 1 - kTop; line <= 0; line++)
      ppu_runMarginLine(ppu, line);
    ppu_runLine(ppu, 1);

    /* BG1 was not clipped and keeps its valid extended world in every arm. */
    CHECK((bg1_capture[0] & 0xffffffu) != 0);
    CHECK((bg1_capture[(kRows - 1) * kW] & 0xffffffu) != 0);
    /* Authentic BG2 row 1 is green and must be identical regardless of the
     * synthetic-margin policy. */
    const uint32_t authentic = bg2_capture[(kRows - 1) * kW];
    CHECK((authentic & 0xffffffu) != 0);
    if (!authentic_bg2)
      authentic_bg2 = authentic;
    else
      CHECK(authentic == authentic_bg2);

    if (clip_rows < 0) {
      /* Legacy/unbounded behavior: bottom-of-map red wraps into the band. */
      CHECK((bg2_capture[0] & 0xffffffu) != 0);
      CHECK(bg2_capture[0] != authentic);
    } else if (clip_rows == 0) {
      /* Camera Y=0: BG2 owns no rows above the viewport. */
      for (int row = 0; row < kTop; row++)
        CHECK(bg2_capture[row * kW] == 0);
    } else {
      /* Four real rows above: line -4 is five rows away and clipped, while
       * line -3 is four rows away and still rendered. */
      CHECK(bg2_capture[27 * kW] == 0);
      CHECK((bg2_capture[28 * kW] & 0xffffffu) != 0);
    }

    PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg1, NULL, 0);
    PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2, NULL, 0);
  }

  g_new_ppu = saved_new_ppu;
  ppu_free(ppu);
}

/* Fix A (SPEC-backdrop-clip.md): a CAPTURED layer's synthesized mirror/repeat
 * padding must reach the full centering budget, not stop at the live per-side
 * margin — otherwise a host that samples the whole fixed capture span reads
 * never-written (transparent) columns at a world bound.
 *
 * The load-bearing assertion is the GATE PROOF at the end: the game's own
 * framebuffer must keep the live margin exactly. That half must pass both before
 * and after the fix, so it is the guard against a flat-mode regression.
 */
static void TestCapturedPaddingReachesBudget(void) {
  enum { kBudget = 120, kLiveRight = 120,
         kCaptureWidth = 256 + 2 * kBudget };
  const int bg2 = kActRaiserPpuLayer_Bg2;

  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;

  /* The overlay-capture and widescreen-padding paths live in PpuDrawWholeLine,
   * which ppu_runLine only reaches when g_new_ppu is set; the old path ignores
   * both. Diorama mode always implies the new renderer (Diorama_NewPpuCapable),
   * so this matches the only configuration the fix can run in. */
  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;

  static uint32_t capture[kCaptureWidth * 4];
  static uint8_t fb[kCaptureWidth * 4 * 4];

  /* Render one line twice: once with the widened padding enabled, once with it
   * off, and compare the same column. */
  uint32_t left_on = 0, left_off = 0;
  for (int enabled = 1; enabled >= 0; enabled--) {
    ppu_reset(ppu);
    memset(capture, 0, sizeof(capture));
    memset(fb, 0, sizeof(fb));

    ppu->inidisp = 0x0f;               /* brightness 15, force-blank off */
    ppu->bgmode = 1;                   /* Mode 1: BG1/BG2 are 4bpp */
    ppu->screenEnabled[0] = 1u << bg2; /* BG2 on the main screen only */
    ppu->cgram[0] = bgr555(0, 0, 0);
    ppu->cgram[0x21] = bgr555(31, 0, 31); /* BG2 palette 2, colour 1 */
    /* BG2 tile data at VRAM word 0 (bgTileAdr nibble for layer 1 == 0), one
     * solid 4bpp tile using palette colour 1. */
    set_solid_4bpp_tile(ppu, 1, 1);
    ppu->bgTileAdr = 0;
    /* BG2 tilemap at word 0x2000: PPU_bgTilemapAdr is (bgXsc & 0xfc) << 8, so
     * 0x20 << 8 == 0x2000. Fill the whole 64x64 map with that tile, palette 2
     * (bits 10-12), so every column the renderer touches is non-transparent. */
    ppu->bgXsc[bg2] = 0x20 | 0x3;      /* wider + higher tilemap */
    for (int i = 0; i < 0x1000; i++)
      ppu->vram[0x2000 + i] = (uint16_t)(1 | (2 << 10));
    ppu->hScroll[bg2] = 0;
    ppu->vScroll[bg2] = 0;

    /* Symmetric budget, then narrow the LEFT margin to 0: the level-start case. */
    PpuSetExtraSpace(ppu, kBudget);
    PpuSetWidescreenLayerMirror(ppu, (uint8_t)(1u << bg2));
    PpuSetWidescreenPadCapturedToBudget(ppu, (uint8_t)enabled);
    PpuSetExtraSideSpace(ppu, 0, kLiveRight, 0);

    PpuBeginDrawing(ppu, fb, kCaptureWidth * 4, 0);
    CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2,
                                (uint8_t *)capture,
                                kCaptureWidth * sizeof(uint32_t)));
    CHECK(PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg2, -kBudget, 0,
                               kCaptureWidth, 2,
                               kPpuOverlayFlag_RemoveFromGame));
    ppu_runLine(ppu, 1);

    /* Column 0 of the capture is screen x = -120, i.e. the far end of the
     * collapsed left margin. */
    if (enabled) {
      left_on = capture[0];
    } else {
      left_off = capture[0];
    }

    /* The already-live RIGHT side must be identical either way — the fix must
     * not disturb a margin that was already being rendered. */
    const uint32_t right = capture[kCaptureWidth - 1];
    CHECK((right & 0xffffffu) != 0);

    PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2, NULL, 0);
  }

  /* With the fix ON the collapsed side carries mirrored content... */
  CHECK((left_on & 0xffffffu) != 0);
  /* ...and with it OFF that column was never written, which is the bug. */
  CHECK(left_off == 0);

  /* The mirror image: a level's END collapses the RIGHT margin instead. Without
   * this the `captured` term on margin_right is unobservable, and dropping it
   * would pass every left-side assertion above. */
  uint32_t right_on = 0, right_off = 0;
  for (int enabled = 1; enabled >= 0; enabled--) {
    ppu_reset(ppu);
    memset(capture, 0, sizeof(capture));
    memset(fb, 0, sizeof(fb));
    ppu->inidisp = 0x0f;
    ppu->bgmode = 1;
    ppu->screenEnabled[0] = (uint8_t)(1u << bg2);
    ppu->cgram[0] = bgr555(0, 0, 0);
    ppu->cgram[0x21] = bgr555(31, 0, 31);
    set_solid_4bpp_tile(ppu, 1, 1);
    ppu->bgTileAdr = 0;
    ppu->bgXsc[bg2] = 0x20 | 0x3;
    for (int i = 0; i < 0x1000; i++)
      ppu->vram[0x2000 + i] = (uint16_t)(1 | (2 << 10));
    ppu->hScroll[bg2] = 0;
    ppu->vScroll[bg2] = 0;
    PpuSetExtraSpace(ppu, kBudget);
    PpuSetWidescreenLayerMirror(ppu, (uint8_t)(1u << bg2));
    PpuSetWidescreenPadCapturedToBudget(ppu, (uint8_t)enabled);
    PpuSetExtraSideSpace(ppu, kBudget, 0, 0);   /* right margin collapsed */

    PpuBeginDrawing(ppu, fb, kCaptureWidth * 4, 0);
    CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2,
                                (uint8_t *)capture,
                                kCaptureWidth * sizeof(uint32_t)));
    CHECK(PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg2, -kBudget, 0,
                               kCaptureWidth, 2,
                               kPpuOverlayFlag_RemoveFromGame));
    ppu_runLine(ppu, 1);
    if (enabled) right_on = capture[kCaptureWidth - 1];
    else right_off = capture[kCaptureWidth - 1];
    /* The already-live LEFT side renders either way. */
    CHECK((capture[0] & 0xffffffu) != 0);
    PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2, NULL, 0);
  }
  CHECK((right_on & 0xffffffu) != 0);
  CHECK(right_off == 0);

  /* GATE PROOF. The game's own framebuffer must NOT gain the widened padding:
   * a narrower margin there is the intended pillarbox at a world edge, and
   * HUD-split rows composite the full budget. Framebuffer column 0 stays the
   * cleared/backdrop pixel even with the fix enabled. Removing the `captured`
   * condition in PpuDrawBackground_4bpp_policy makes this fail. */
  /* GATE PROOF. The game's own framebuffer must NOT gain the widened padding: a
   * narrower margin there is the intended pillarbox at a world edge, and
   * HUD-split rows composite the full budget, so widening it would change flat
   * output and break byte-identical replay.
   *
   * This needs its own pass with NO overlay surface bound. The passes above use
   * kPpuOverlayFlag_RemoveFromGame, which strips BG2 from the framebuffer
   * outright — so their framebuffer is unconditionally blank and proves nothing.
   * With nothing bound, PpuDrawBackground_4bpp_policy receives bgBuffers rather
   * than overlayBuffers, which is exactly the condition the gate tests.
   *
   * Deleting the `dstbuf == &ppu->overlayBuffers[layer]` term makes this fail. */
  {
    ppu_reset(ppu);
    memset(fb, 0, sizeof(fb));
    ppu->inidisp = 0x0f;
    ppu->bgmode = 1;
    ppu->screenEnabled[0] = (uint8_t)(1u << bg2);
    ppu->cgram[0] = bgr555(0, 0, 0);
    ppu->cgram[0x21] = bgr555(31, 0, 31);
    set_solid_4bpp_tile(ppu, 1, 1);
    ppu->bgTileAdr = 0;
    ppu->bgXsc[bg2] = 0x20 | 0x3;
    for (int i = 0; i < 0x1000; i++)
      ppu->vram[0x2000 + i] = (uint16_t)(1 | (2 << 10));
    ppu->hScroll[bg2] = 0;
    ppu->vScroll[bg2] = 0;
    PpuSetExtraSpace(ppu, kBudget);
    PpuSetWidescreenLayerMirror(ppu, (uint8_t)(1u << bg2));
    PpuSetWidescreenPadCapturedToBudget(ppu, 1);  /* enabled, yet must be inert */
    /* A HUD split is REQUIRED to observe this. Outside the split the compositor
     * clamps the framebuffer to the live window (ppu.c: composite_left =
     * extraLeftCur), so a widened layer buffer is discarded and the leak is
     * invisible. On split rows composite_left becomes extraLeftRight — the full
     * budget — so anything the layer wrote in the collapsed margin reaches the
     * framebuffer. That is exactly the flat-mode regression the gate prevents. */
    PpuSetWidescreenHudSplit(ppu, 8, 100, 100, 8, 8);
    PpuSetExtraSideSpace(ppu, 0, kLiveRight, 0);

    PpuBeginDrawing(ppu, fb, kCaptureWidth * 4, 0);
    ppu_runLine(ppu, 1);   /* screen y = 0, inside the split */

    const uint32_t *row = (const uint32_t *)(const void *)fb;
    /* Screen x = -120 .. -1 is the collapsed left margin: it must stay backdrop
     * even though the padding feature is enabled, because this frame's BG2 went
     * to bgBuffers rather than an overlay buffer. */
    CHECK((row[0] & 0xffffffu) == 0);
    CHECK((row[kBudget - 1] & 0xffffffu) == 0);
    /* Sanity: the authentic centre and the live right margin DID render, so a
     * blank framebuffer cannot make the two checks above pass for free. */
    CHECK((row[kBudget + 10] & 0xffffffu) != 0);
    CHECK((row[kCaptureWidth - 1] & 0xffffffu) != 0);
  }

  /* A normal (non-HUD) scanline writes only the live finite-world interval.
   * Seed the fixed-width target with a loud stale value, contract each side by
   * one pixel, and prove the two columns that just left the interval are
   * explicitly blacked rather than retaining the previous frame. */
  {
    ppu_reset(ppu);
    memset(fb, 0x5a, sizeof(fb));
    ppu->inidisp = 0x0f;
    ppu->bgmode = 1;
    ppu->screenEnabled[0] = (uint8_t)(1u << bg2);
    ppu->cgram[0] = bgr555(0, 0, 0);
    ppu->cgram[0x21] = bgr555(31, 0, 31);
    set_solid_4bpp_tile(ppu, 1, 1);
    ppu->bgTileAdr = 0;
    ppu->bgXsc[bg2] = 0x20 | 0x3;
    for (int i = 0; i < 0x1000; i++)
      ppu->vram[0x2000 + i] = (uint16_t)(1 | (2 << 10));
    PpuSetExtraSpace(ppu, kBudget);
    PpuSetExtraSideSpace(ppu, kBudget - 1, kBudget - 1, 0);

    PpuBeginDrawing(ppu, fb, kCaptureWidth * 4, 0);
    ppu_runLine(ppu, 1);

    const uint32_t *row = (const uint32_t *)(const void *)fb;
    CHECK(row[0] == 0);
    CHECK(row[kCaptureWidth - 1] == 0);
    CHECK((row[1] & 0xffffffu) != 0);
    CHECK((row[kCaptureWidth - 2] & 0xffffffu) != 0);
  }

  g_new_ppu = saved_new_ppu;
}

int main(void) {
  TestWorldNavigationPartialBrightnessCapture();
  TestObjRangeRaster();
  TestSemanticAtlasPacking();
  TestSim3DFlatComposition();
  TestSim3DWidescreenHudCaptureHandoff();
  TestOverlayContentMetadata();
  TestVerticalMarginLayerClip();
  TestCapturedPaddingReachesBudget();
  TestVirtualTilemapMargins();
  TestVirtualTilemapEffects();
  TestVirtualTilemapVerticalMargin();
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  CHECK(SDL_Init(SDL_INIT_VIDEO));

  /* ---- Real PPU: draw a solid backdrop, full brightness, force-blank off ---- */
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) { SDL_Quit(); return 1; }
  ppu_reset(ppu);

  /* Framebuffer the PPU renders into (ARGB8888, 4 bytes/px), same shape as
   * main.c's g_pixels. The PPU writes RGB and leaves the alpha byte 0. */
  static uint8_t fb[kW * kH * 4];
  memset(fb, 0, sizeof(fb));
  PpuBeginDrawing(ppu, fb, kW * 4, 0);

  /* Backdrop = CGRAM entry 0. Pick a distinctive color: R=25,G=10,B=3 (5-bit).
   * With no BG/OBJ layers enabled, every pixel resolves to this backdrop. */
  const int R5 = 25, G5 = 10, B5 = 3;
  ppu->cgram[0] = bgr555(R5, G5, B5);
  ppu->inidisp = 0x0f;   /* brightness 15, force-blank (bit 7) OFF */
  ppu->bgmode = 0;       /* mode 0; no layers enabled -> pure backdrop */

  for (int line = 0; line <= kH; line++) ppu_runLine(ppu, line);

  /* Sanity: the PPU wrote RGB with alpha byte 0 (BGRA byte order in memory:
   * [0]=B [1]=G [2]=R [3]=A). Confirm a mid-screen pixel matches the backdrop
   * and its alpha byte is indeed 0 (the condition that black-screens under a
   * BLEND texture). */
  const uint8_t *mid = fb + ((size_t)(kH / 2) * kW + (kW / 2)) * 4;
  int exp_r = expand5(R5), exp_g = expand5(G5), exp_b = expand5(B5);
  fprintf(stderr, "[ppu-test] backdrop pixel BGRA = %d,%d,%d,%d (expected RGB %d,%d,%d, A 0)\n",
          mid[0], mid[1], mid[2], mid[3], exp_r, exp_g, exp_b);
  CHECK(mid[2] == exp_r && mid[1] == exp_g && mid[0] == exp_b);
  CHECK(mid[3] == 0);   /* the alpha-0 framebuffer that triggered the bug */

  /* ---- Present through the exact SDL3 path main.c uses ---- */
  SDL_Window *window =
      SDL_CreateWindow("ppu-pipeline-test", kW * 3, kH * 3, SDL_WINDOW_HIDDEN);
  CHECK(window != NULL);
  SDL_Renderer *renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
  CHECK(renderer != NULL);

  if (renderer) {
    SDL_Texture *texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kW, kH);
    CHECK(texture != NULL);
    /* The fix under test: force NONE so the alpha-0 framebuffer blits opaque.
     * (SDL3 defaults to BLEND, which would present this as a black screen.) */
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

    CHECK(SDL_UpdateTexture(texture, NULL, fb, kW * 4));
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    CHECK(SDL_RenderClear(renderer));
    CHECK(SDL_RenderTexture(renderer, texture, NULL, NULL));
    CHECK(SDL_RenderPresent(renderer));

    SDL_Surface *raw = SDL_RenderReadPixels(renderer, NULL);
    SDL_Surface *argb =
        raw ? SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888) : NULL;
    if (raw) SDL_DestroySurface(raw);
    CHECK(argb != NULL);

    if (argb) {
      /* Center of the presented output must be the backdrop RGB, not black. */
      const uint8_t *row = (const uint8_t *)argb->pixels +
                           (size_t)(argb->h / 2) * argb->pitch;
      const uint8_t *cp = row + (size_t)(argb->w / 2) * 4;  /* B,G,R,A */
      int got_b = cp[0], got_g = cp[1], got_r = cp[2];
      long nonblack = 0;
      for (int y = 0; y < argb->h; y++) {
        const uint8_t *r = (const uint8_t *)argb->pixels + (size_t)y * argb->pitch;
        for (int x = 0; x < argb->w; x++)
          if (r[x * 4] || r[x * 4 + 1] || r[x * 4 + 2]) nonblack++;
      }
      fprintf(stderr, "[ppu-test] presented center RGB = %d,%d,%d; nonblack=%ld/%ld\n",
              got_r, got_g, got_b, nonblack, (long)argb->w * argb->h);
      CHECK(got_r == exp_r && got_g == exp_g && got_b == exp_b);
      CHECK(nonblack == (long)argb->w * argb->h);  /* whole frame visible */
      SDL_DestroySurface(argb);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
  }

  SDL_DestroyWindow(window);
  ppu_free(ppu);
  SDL_Quit();

  if (s_failures) {
    fprintf(stderr, "ppu render pipeline tests: %d failure(s)\n", s_failures);
    return 1;
  }
  fprintf(stderr, "ppu render pipeline tests: pass\n");
  return 0;
}
