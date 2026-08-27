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
#include "sim_town_terrain.h"
#include "sim_render_atlas.h"
#include "sim_render_metadata.h"
#include "sim_world_navigation_capture.h"
#include "runner_next_internal.h"
#include "snes/snes.h"

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

static void set_solid_2bpp_tile(Ppu *ppu, int word_address,
                                int tile, int color) {
  for (int row = 0; row < 8; row++) {
    uint16_t bits = 0;
    if (color & 1) bits |= 0x00ff;
    if (color & 2) bits |= 0xff00;
    ppu->vram[word_address + tile * 8 + row] = bits;
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

static bool lookup_virtual_band(const void *context, int32_t tile_x,
                                int32_t tile_y, uint16_t entry,
                                uint8_t *band) {
  (void)context;
  (void)tile_y;
  (void)entry;
  if (!band || tile_x < 0) return false;
  *band = (uint8_t)(tile_x % 3);
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

static void fill_virtual_native_ring(Ppu *ppu, uint16_t even_entry,
                                     uint16_t odd_entry) {
  for (int tile_y = 0; tile_y < 64; tile_y++) {
    for (int tile_x = 0; tile_x < 64; tile_x++) {
      int address = 0x2000 + (tile_x & 31) + ((tile_y & 31) << 5) +
          ((tile_x & 32) ? 0x400 : 0) +
          ((tile_y & 32) ? 0x800 : 0);
      ppu->vram[address] = (tile_x & 1) ? odd_entry : even_entry;
    }
  }
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
  bad = binding;
  bad.flags = 0x80;
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

static void TestVirtualTilemapAuthenticParity(void) {
  const int bg1 = kActRaiserPpuLayer_Bg1;
  const bool saved_new_ppu = g_new_ppu;
  const uint16_t even_entry = (uint16_t)(2 | (2 << 10));
  const uint16_t odd_entry = (uint16_t)(3 | (3 << 10) | 0x2000);
  g_new_ppu = true;
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;
  static uint8_t fb[kW * 4];
  static uint32_t native_pixels[kW];
  static PpuZbufType native_priority[kW];

  VirtualTilemapFixture map = {
    .min_x = 0, .max_x = 63, .min_y = 0, .max_y = 63,
    .even_entry = even_entry,
    .odd_entry = odd_entry,
  };
  const PpuVirtualTilemapBinding binding = {
    .lookup = lookup_virtual_tile,
    .context = &map,
    .camera_x = 8,
    .camera_y = 0,
    .hscroll_anchor = 8,
    .vscroll_anchor = 0,
    .flags = kPpuVirtualTilemapFlag_IncludeAuthentic,
  };

  /* A full-viewport provider with the same words as the native 64x64 ring
   * must be indistinguishable at both the resolved-pixel and priority-word
   * boundaries. The lookup range proves the authentic span was provider-owned. */
  setup_virtual_bg(ppu, 0, fb, sizeof(fb));
  fill_virtual_native_ring(ppu, even_entry, odd_entry);
  render_first_line(ppu);
  memcpy(native_pixels, fb, sizeof(native_pixels));
  memcpy(native_priority,
         ppu->bgBuffers[0].data + kPpuExtraLeftRight,
         sizeof(native_priority));

  setup_virtual_bg(ppu, 0, fb, sizeof(fb));
  fill_virtual_native_ring(ppu, even_entry, odd_entry);
  CHECK(PpuSetVirtualTilemap(ppu, (uint8_t)bg1, &binding));
  render_first_line(ppu);
  CHECK(map.calls > 0);
  CHECK(map.first_x == 1 && map.last_x == 32);
  CHECK(map.first_y == 0 && map.last_y == 0);
  CHECK(memcmp(fb, native_pixels, sizeof(native_pixels)) == 0);
  CHECK(memcmp(ppu->bgBuffers[0].data + kPpuExtraLeftRight,
               native_priority, sizeof(native_priority)) == 0);

  /* Mosaic samples a coarser set of source pixels, but ownership must remain
   * invisible there as well. */
  setup_virtual_bg(ppu, 0, fb, sizeof(fb));
  fill_virtual_native_ring(ppu, even_entry, odd_entry);
  ppu->mosaic = (uint8_t)((3 << 4) | (1u << bg1));
  render_first_line(ppu);
  memcpy(native_pixels, fb, sizeof(native_pixels));
  memcpy(native_priority,
         ppu->bgBuffers[0].data + kPpuExtraLeftRight,
         sizeof(native_priority));

  setup_virtual_bg(ppu, 0, fb, sizeof(fb));
  fill_virtual_native_ring(ppu, even_entry, odd_entry);
  ppu->mosaic = (uint8_t)((3 << 4) | (1u << bg1));
  map.calls = 0;
  CHECK(PpuSetVirtualTilemap(ppu, (uint8_t)bg1, &binding));
  render_first_line(ppu);
  CHECK(map.calls > 0);
  CHECK(memcmp(fb, native_pixels, sizeof(native_pixels)) == 0);
  CHECK(memcmp(ppu->bgBuffers[0].data + kPpuExtraLeftRight,
               native_priority, sizeof(native_priority)) == 0);

  ppu_free(ppu);
  g_new_ppu = saved_new_ppu;
}

static void TestVirtualTilemapPresentationBandsPreserveFlatOutput(void) {
  const int bg1 = kActRaiserPpuLayer_Bg1;
  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;
  static uint8_t fb[kW * sizeof(uint32_t)];
  static uint8_t baseline[sizeof(fb)];
  static uint32_t primary[kW], high[kW], far[kW];

  VirtualTilemapFixture map = {
    .min_x = 0, .max_x = 63, .min_y = 0, .max_y = 63,
    .even_entry = (uint16_t)(2 | (2 << 10)),
    .odd_entry = (uint16_t)(3 | (3 << 10) | 0x2000),
  };
  PpuVirtualTilemapBinding binding = {
    .lookup = lookup_virtual_tile,
    .context = &map,
    .camera_x = 8,
    .camera_y = 0,
    .hscroll_anchor = 8,
    .vscroll_anchor = 0,
    .flags = kPpuVirtualTilemapFlag_IncludeAuthentic,
  };

  setup_virtual_bg(ppu, 0, fb, sizeof(fb));
  CHECK(PpuSetVirtualTilemap(ppu, (uint8_t)bg1, &binding));
  render_first_line(ppu);
  memcpy(baseline, fb, sizeof(baseline));

  setup_virtual_bg(ppu, 0, fb, sizeof(fb));
  memset(primary, 0, sizeof(primary));
  memset(high, 0, sizeof(high));
  memset(far, 0, sizeof(far));
  binding.band_lookup = lookup_virtual_band;
  CHECK(PpuSetVirtualTilemap(ppu, (uint8_t)bg1, &binding));
  CHECK(PpuBindOverlaySurface(
      ppu, kPpuOverlaySource_Bg1, (uint8_t *)primary, sizeof(primary)));
  CHECK(PpuBindOverlayPrioSurface(
      ppu, kPpuOverlaySource_Bg1, 1, (uint8_t *)high));
  CHECK(PpuBindOverlayPrioSurface(
      ppu, kPpuOverlaySource_Bg1, 2, (uint8_t *)far));
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg1, 0, 0, kW, 1, 0));
  render_first_line(ppu);

  /* World x begins at tile 1: ordinary, high, far, then repeats. Each
   * captured pixel lands in exactly one presentation surface. */
  CHECK(primary[0] != 0 && high[0] == 0 && far[0] == 0);
  CHECK(primary[8] == 0 && high[8] != 0 && far[8] == 0);
  CHECK(primary[16] == 0 && high[16] == 0 && far[16] != 0);
  CHECK(PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg1, 0));
  CHECK(PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg1, 1));
  CHECK(PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg1, 2));
  CHECK(memcmp(fb, baseline, sizeof(baseline)) == 0);

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

static void TestObjRangeScanoutCapture(void) {
  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;
  ppu_reset(ppu);
  ppu->inidisp = 0x0f;
  ppu->obsel = 0;
  ppu->screenEnabled[0] = 1u << kPpuOverlaySource_Obj;
  ppu->cgram[0x81] = bgr555(31, 0, 0);
  ppu->cgram[0x82] = bgr555(0, 0, 31);
  set_solid_4bpp_tile(ppu, 0, 1);
  set_solid_4bpp_tile(ppu, 1, 2);

  /* Slot 0 wins the authentic overlap, but the semantic range selects slot 1
   * and must preserve its blue pixels at the instant scanout fetches them. */
  ppu->oam[0] = 10 | (20 << 8);
  ppu->oam[1] = 0 | (2 << 12);
  ppu->oam[2] = 10 | (20 << 8);
  ppu->oam[3] = 1 | (2 << 12);
  static uint8_t framebuffer[kW * kH * sizeof(uint32_t)];
  static uint32_t capture[kW * kH];
  memset(framebuffer, 0, sizeof(framebuffer));
  memset(capture, 0x5a, sizeof(capture));
  PpuSetExtraSpace(ppu, 0);
  PpuBeginDrawing(ppu, framebuffer, kW * sizeof(uint32_t), 0);
  CHECK(PpuSetObjRangeCapture(
      ppu, 1, 1, 10, 20, 8, 8, (uint8_t *)capture,
      kW * sizeof(uint32_t)));
  ppu_runLine(ppu, 0);
  ppu_runLine(ppu, 21);
  const uint32_t *frame = (const uint32_t *)(const void *)framebuffer;
  CHECK(frame[20 * kW + 10] == 0x00ff0000u);
  CHECK(capture[20 * kW + 10] == 0xff0000ffu);
  CHECK(capture[20 * kW + 17] == 0xff0000ffu);

  /* Later endpoint state cannot rewrite the already displayed row. This is
   * the transition seam the HUD handoff depends on. */
  set_solid_4bpp_tile(ppu, 1, 0);
  ppu->cgram[0x82] = 0;
  ppu_runLine(ppu, 22);
  CHECK(capture[20 * kW + 10] == 0xff0000ffu);
  CHECK(capture[21 * kW + 10] == 0);

  PpuClearOverlayCaptures(ppu);
  CHECK(ppu->objRangeCapture.count == 0);
  ppu_free(ppu);
  g_new_ppu = saved_new_ppu;
}

static void TestWorldNavigationPartialBrightnessCapture(void) {
  Ppu *ppu = ppu_init();
  Snes snes = {0};
  CHECK(ppu != NULL);
  if (!ppu) return;
  snes.ppu = ppu;
  snes.abiLifetimeGeneration = 7u;
  sr_runner_bind_ppu_services(&snes, true);
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
    CHECK(SimWorldNavigationCapture_Capture(
        &frame, sr_runner_handle(&snes)));
    CHECK(frame.view == kSimView_WorldNavigation);
    CHECK(frame.world_navigation_brightness == brightness);
    CHECK(frame.separated_backdrop_argb == 0xffff0000u);
    CHECK(frame.world_navigation_scene.composition.valid);
    CHECK(frame.world_navigation_scene.composition.empty_animation);
  }

  /* Synthetic copy of the measured steady navigation ownership shape: a
   * packed priority-3 UI prefix followed by the fixed 3x3 Palace. This drives
   * both caller-owned ABI raster requests, not just the empty-animation path. */
  for (int tile = 0; tile < 256; tile++)
    set_solid_4bpp_tile(ppu, tile, 1);
  ppu->cgram[0x81] = bgr555(31, 0, 0);
  ppu->cgram[0x91] = bgr555(0, 0, 31);
  for (int slot = 0; slot < 20; slot++) {
    ppu->oam[slot * 2] =
        (uint16_t)((0x11u << 8) | (uint8_t)(0x20 + slot));
    ppu->oam[slot * 2 + 1] = (uint16_t)(0x3000u | (uint8_t)slot);
  }
  static const uint8_t palace_x[9] =
      {104, 120, 136, 104, 120, 136, 104, 120, 136};
  static const uint8_t palace_y[9] =
      {81, 81, 81, 97, 97, 97, 113, 113, 113};
  static const uint8_t palace_tile[9] =
      {0x06, 0x08, 0x0a, 0x0c, 0x0e, 0x26, 0x60, 0x62, 0x64};
  for (int i = 0; i < 9; i++) {
    ppu->oam[(20 + i) * 2] =
        (uint16_t)(palace_x[i] | ((uint16_t)palace_y[i] << 8));
    ppu->oam[(20 + i) * 2 + 1] =
        (uint16_t)(palace_tile[i] | 0x3200u);
  }
  SimFrameData composed = {0};
  composed.view = kSimView_WorldNavigation;
  composed.world_navigation_scene.valid = true;
  ppu->inidisp = 0x0f;
  CHECK(SimWorldNavigationCapture_Capture(
      &composed, sr_runner_handle(&snes)));
  CHECK(composed.world_navigation_scene.composition.ui.screen_x == 32);
  CHECK(composed.world_navigation_scene.composition.ui.screen_y == 17);
  CHECK(composed.world_navigation_scene.composition.ui.width == 27);
  CHECK(composed.world_navigation_scene.composition.ui.height == 8);
  CHECK(composed.world_navigation_scene.composition.palace.screen_x == 104);
  CHECK(composed.world_navigation_scene.composition.palace.screen_y == 81);
  CHECK(composed.world_navigation_scene.composition.palace.width == 40);
  CHECK(composed.world_navigation_scene.composition.palace.height == 40);
  CHECK(g_sim_world_navigation_ui_pixels[0] == 0xffff0000u);
  CHECK(g_sim_world_navigation_palace_pixels[0] == 0xff0000ffu);

  SimFrameData blank = {0};
  blank.view = kSimView_WorldNavigation;
  blank.world_navigation_scene.valid = true;
  ppu->inidisp = 0x8f;
  CHECK(!SimWorldNavigationCapture_Capture(
      &blank, sr_runner_handle(&snes)));
  CHECK(blank.view == kSimView_AuthenticFallback);
  CHECK(blank.world_navigation_brightness == 15);

  sr_runner_bind_ppu_services(&snes, false);
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
  uint32_t *planes[kSim3DPlane_Count];
  memset(storage, 0, sizeof(storage));
  for (int plane = 0; plane < kSim3DPlane_Count; plane++)
    planes[plane] = storage[plane];

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

static void TestSim3DFlatCompositionDemand(void) {
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;

  ppu_reset(ppu);
  ppu->inidisp = 0x0f;
  ppu->bgmode = 9;
  ppu->screenEnabled[0] = 0x17;
  ppu->screenEnabled[1] = 0;
  ppu->cgram[0] = bgr555(3, 5, 7);

  enum { width = kActRaiserAuthenticWidth, height = 1 };
  uint32_t authentic[width] = {0};
  Sim3DCaptureRequest request = {
    .town = true,
    .master_enabled = true,
    .renderer_ready = true,
    .requested_features = kSimFeature_SeparatedComposite |
                          kSimFeature_GroundProjection,
    .width = width,
    .height = height,
  };

  /* Capture reports a selected-resource contract failure without reaching
   * into application shutdown policy. This keeps the producer independently
   * testable and lets the host seam own the user-facing outcome. */
  Sim3D_BeginFrame();
  request.renderer_ready = false;
  CHECK(!Sim3D_PrepareCapture(ppu, &request));
  CHECK(Sim3D_GetCaptureContractFailure() ==
        kSim3DCaptureContract_RendererUnavailable);
  request.renderer_ready = true;

  /* The projected profile has no flat-buffer reader. A sentinel proves the
   * finish path did not merely produce an equivalent-looking composite. */
  Sim3D_BeginFrame();
  CHECK(Sim3D_PrepareCapture(ppu, &request));
  for (int plane = 0; plane < kSim3DPlane_Count; plane++)
    memset(g_sim3d_layer_pixels[plane], 0, width * sizeof(uint32_t));
  for (int x = 0; x < width; x++) g_sim3d_flat_pixels[x] = 0x5a5a5a5au;
  Sim3D_FinishCapture((uint8_t *)authentic, width * (int)sizeof(uint32_t), 1);
  for (int x = 0; x < width; x++)
    CHECK(g_sim3d_flat_pixels[x] == 0x5a5a5a5au);

  CHECK(Sim3D_BeginFrame());
  PpuClearOverlayBindings(ppu);
  PpuClearOverlayCaptures(ppu);

  /* Disabling ground projection selects the flat fallback, which must keep
   * rebuilding the exact same buffer for presentation. */
  request.requested_features = kSimFeature_SeparatedComposite;
  CHECK(Sim3D_PrepareCapture(ppu, &request));
  for (int plane = 0; plane < kSim3DPlane_Count; plane++)
    memset(g_sim3d_layer_pixels[plane], 0, width * sizeof(uint32_t));
  Sim3D_FinishCapture((uint8_t *)authentic, width * (int)sizeof(uint32_t), 2);
  CHECK(g_sim3d_flat_pixels[0] != 0x5a5a5a5au);
  CHECK(g_sim3d_flat_pixels[0] == ActRaiser_BackdropArgb(ppu));

  Sim3D_BeginFrame();
  ppu_free(ppu);
}

static void TestSim3DPlaneTextureUploadMask(void) {
  const uint32_t all_planes = (1u << kSim3DPlane_Count) - 1u;
  CHECK(Sim3D_PlaneTextureUploadMask(0, all_planes) == 0);
  CHECK(Sim3D_PlaneTextureUploadMask(
            kSimFeature_SeparatedComposite, all_planes) == 0);
  CHECK(Sim3D_PlaneTextureUploadMask(
            kSimFeature_SeparatedComposite |
            kSimFeature_GroundProjection, all_planes) == all_planes);

  uint32_t billboard_mask = Sim3D_PlaneTextureUploadMask(
      kSimFeature_SeparatedComposite |
      kSimFeature_GroundProjection |
      kSimFeature_ObjectBillboards, all_planes);
  int uploaded_planes = 0;
  for (int plane = 0; plane < kSim3DPlane_Count; plane++)
    if (billboard_mask & (1u << plane)) uploaded_planes++;
  for (int priority = 0; priority < 4; priority++)
    CHECK(!(billboard_mask &
            (1u << Sim3D_ObjPlaneForPriority(priority))));
  CHECK(uploaded_planes == 6);
  /* A missing producer bit always wins over feature selection. Presentation
   * must not sample a stale texture even if a future caller violates the
   * capture/fallback contract. */
  CHECK(Sim3D_PlaneTextureUploadMask(
            kSimFeature_SeparatedComposite |
            kSimFeature_GroundProjection,
            1u << kSim3DPlane_Bg1Low) ==
        (1u << kSim3DPlane_Bg1Low));
}

static void TestSim3DRawObjCaptureFallbackContract(void) {
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;

  ppu_reset(ppu);
  ppu->inidisp = 0x0f;
  ppu->bgmode = 9;
  ppu->screenEnabled[0] = 0x17;
  ppu->screenEnabled[1] = 0;

  Sim3DCaptureRequest request = {
    .town = true,
    .master_enabled = true,
    .renderer_ready = true,
    .billboard_atlas_ready = true,
    .billboard_renderer_ready = true,
    .requested_features = kSimFeature_SeparatedComposite |
                          kSimFeature_GroundProjection |
                          kSimFeature_ObjectBillboards,
    .width = kActRaiserAuthenticWidth,
    .height = 1,
  };
  uint32_t authentic[kActRaiserAuthenticWidth] = {0};

  /* Ordinary billboard presentation has a current atlas and no raw-plane
   * reader. OBJ is fully unbound before scanout, including every band and its
   * capture, while the six BG planes remain active. */
  Sim3D_BeginFrame();
  CHECK(Sim3D_PrepareCapture(ppu, &request));
  CHECK(ppu->overlayRenderBuffer[kPpuOverlaySource_Bg1] != NULL);
  CHECK(ppu->overlayRenderBuffer[kPpuOverlaySource_Obj] == NULL);
  for (int band = 0; band < 3; band++)
    CHECK(ppu->overlayRenderBands[kPpuOverlaySource_Obj][band] == NULL);
  CHECK(ppu->overlayCaptures[kPpuOverlaySource_Obj].x1 <=
        ppu->overlayCaptures[kPpuOverlaySource_Obj].x0);
  CHECK(Sim3D_BeginFrame());
  PpuClearOverlayBindings(ppu);

  /* The inspector consumes the exact ten-plane composition even though the
   * selected presentation profile uses billboards. */
  request.inspector_active = true;
  CHECK(Sim3D_PrepareCapture(ppu, &request));
  CHECK(ppu->overlayRenderBuffer[kPpuOverlaySource_Obj] != NULL);
  for (int band = 0; band < 3; band++)
    CHECK(ppu->overlayRenderBands[kPpuOverlaySource_Obj][band] != NULL);
  CHECK(ppu->overlayCaptures[kPpuOverlaySource_Obj].oamCount == 128);
  CHECK(Sim3D_BeginFrame());
  PpuClearOverlayBindings(ppu);
  request.inspector_active = false;

  /* A current-frame metadata/atlas failure retains the raw fallback. */
  request.billboard_atlas_ready = false;
  SimRenderMetadata_Reset();
  CHECK(Sim3D_PrepareCapture(ppu, &request));
  CHECK(ppu->overlayRenderBuffer[kPpuOverlaySource_Obj] != NULL);
  Sim3D_FinishCapture((uint8_t *)authentic,
                      kActRaiserAuthenticWidth * (int)sizeof(uint32_t), 1);
  SimFrameData fallback_frame = {0};
  Sim3DTuning fallback_tuning = {0};
  fallback_tuning.landscape_height_pct = 55;
  fallback_tuning.height_scale_x100 = 130;
  Sim3D_AnnotateFrame(&fallback_frame, &fallback_tuning);
  CHECK(fallback_frame.landscape_height_pct == 55);
  CHECK(fallback_frame.height_scale_x100 == 130);
  fallback_tuning.landscape_height_pct = -1;
  Sim3D_AnnotateFrame(&fallback_frame, &fallback_tuning);
  CHECK(fallback_frame.landscape_height_pct ==
        kSimTownTerrainLandscapeHeightMinimumPct);
  fallback_tuning.landscape_height_pct =
      kSimTownTerrainLandscapeHeightMaximumPct + 1;
  Sim3D_AnnotateFrame(&fallback_frame, &fallback_tuning);
  CHECK(fallback_frame.landscape_height_pct ==
        kSimTownTerrainLandscapeHeightMaximumPct);
  CHECK(fallback_frame.separated_valid);
  CHECK(fallback_frame.separated_status == kSim3DCapture_AtlasInvalid);
  CHECK(fallback_frame.separated_plane_mask ==
        (1u << kSim3DPlane_Count) - 1u);
  CHECK(Sim3D_BeginFrame());
  PpuClearOverlayBindings(ppu);
  request.billboard_atlas_ready = true;

  /* The GPU atlas is also part of the contract. Its absence retains raw OBJ
   * and removes billboards from the implemented feature set after finish. */
  request.billboard_renderer_ready = false;
  CHECK(Sim3D_PrepareCapture(ppu, &request));
  CHECK(ppu->overlayRenderBuffer[kPpuOverlaySource_Obj] != NULL);
  Sim3D_FinishCapture((uint8_t *)authentic,
                      kActRaiserAuthenticWidth * (int)sizeof(uint32_t), 1);
  CHECK(Sim3D_ImplementedFeatures() & kSimFeature_GroundProjection);
  CHECK(!(Sim3D_ImplementedFeatures() & kSimFeature_ObjectBillboards));
  CHECK(Sim3D_BeginFrame());
  PpuClearOverlayBindings(ppu);
  request.billboard_renderer_ready = true;

  /* Turning billboards off is the intentional raw-plane profile. */
  request.requested_features &= ~kSimFeature_ObjectBillboards;
  CHECK(Sim3D_PrepareCapture(ppu, &request));
  CHECK(ppu->overlayRenderBuffer[kPpuOverlaySource_Obj] != NULL);
  CHECK(Sim3D_BeginFrame());

  ppu_free(ppu);
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
  /* The exact gf18992 menu-open hourglass: earlier menu sprites own slots
   * 0-10, while phase $ED's four pieces move to slots 11-14. The enhanced-view
   * overlay gate must follow the validated capture range rather than assume
   * the ordinary slots 0-3. */
  enum { kMenuHourglassFirst = 11 };
  static const uint16_t kMenuHourglass[] = {
    0x0B94, 0x31ED, 0x0B9B, 0x71ED,
    0x1394, 0x31FD, 0x139B, 0x71FD,
  };
  memcpy(&ppu->oam[kMenuHourglassFirst * 2], kMenuHourglass,
         sizeof(kMenuHourglass));

  /* PpuSetOverlayCapture stores flags through a WHITELIST, so a flag that is
   * declared in ppu.h but missing from that mask is accepted by the setter and
   * then silently dropped -- the capture behaves as if the caller never asked.
   * That is how kPpuOverlayFlag_MarkBgHalfAdd shipped inert on first attempt:
   * the F4 log line reported "captured at 50% alpha" while every captured pixel
   * came back 0xff. Assert every declared flag survives a round trip. */
  {
    const uint8_t kAllFlags = kPpuOverlayFlag_RemoveFromGame |
                              kPpuOverlayFlag_MarkObjColorMath |
                              kPpuOverlayFlag_MarkBgHalfAdd |
                              kPpuOverlayFlag_ApplyBgFixedColorSubtract |
                              kPpuOverlayFlag_MarkFullAddSubscreen |
                              kPpuOverlayFlag_MarkMainScreenWinner |
                              kPpuOverlayFlag_MarkOwningScreenWinner;
    CHECK(PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg2, 0, 0,
                               kActRaiserAuthenticWidth,
                               kActRaiserAuthenticHeight,
                               (uint8_t)(kAllFlags | 0x80u)));
    /* Fill is structured capture state now; its former high bit is once again
     * unknown and must be rejected by the flag whitelist. */
    CHECK(ppu->overlayCaptures[kPpuOverlaySource_Bg2].flags == kAllFlags);
    PpuClearOverlayCaptures(ppu);
  }

  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg3, 0, 0,
      kActRaiserAuthenticWidth, kActRaiserSimulationHudHeight,
      kPpuOverlayFlag_RemoveFromGame));
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Obj, 0, 0,
      kActRaiserAuthenticWidth, kActRaiserSimulationHudHeight,
      kPpuOverlayFlag_RemoveFromGame));
  CHECK(PpuSetOverlayOamRange(
      ppu, kMenuHourglassFirst, kActRaiserHudObjOamCount));

  const int extra = 43;
  const int width = kActRaiserAuthenticWidth + 2 * extra;
  static uint32_t hud_bg[kSim3DMaxWidth * kActRaiserAuthenticHeight];
  static uint32_t hud_obj[kSim3DMaxWidth * kActRaiserAuthenticHeight];
  static uint32_t authentic[kSim3DMaxWidth * kActRaiserAuthenticHeight];
  /* Give every referenced OBJ tile an opaque test colour. The handoff
   * rasterizes the promoted range before capture ownership changes, so this
   * makes its output observable without running a complete PPU frame. */
  memset(ppu->vram, 0xff, sizeof(ppu->vram));
  ppu->cgram[0xff] = bgr555(31, 0, 31);
  CHECK(PpuBindOverlaySurface(
      ppu, kPpuOverlaySource_Obj, (uint8_t *)hud_obj,
      (size_t)width * sizeof(uint32_t)));
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Obj, 0, 0,
      kActRaiserAuthenticWidth, kActRaiserSimulationHudHeight,
      kPpuOverlayFlag_RemoveFromGame));
  CHECK(PpuSetOverlayOamRange(
      ppu, kMenuHourglassFirst, kActRaiserHudObjOamCount));

  Sim3DCaptureRequest request = {
    .town = true,
    .master_enabled = true,
    .renderer_ready = true,
    .requested_features = kSimFeature_SeparatedComposite,
    .width = width,
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

  /* The promoted HUD surface must be rebuilt identically whether raw OBJ is
   * the selected fallback or billboards suppress all four raw bands. This is
   * the stale/cleared-buffer boundary: PrepareHudHandoff retains the original
   * HUD destination before OBJ is rebound or unbound, and FinishCapture
   * repopulates it from the independent range raster. */
  for (int suppress_raw = 0; suppress_raw <= 1; suppress_raw++) {
    PpuClearOverlayBindings(ppu);
    memset(hud_bg, 0, sizeof(hud_bg));
    memset(hud_obj, 0, sizeof(hud_obj));
    memset(authentic, 0, sizeof(authentic));
    CHECK(PpuBindOverlaySurface(
        ppu, kPpuOverlaySource_Bg3, (uint8_t *)hud_bg,
        (size_t)width * sizeof(uint32_t)));
    CHECK(PpuBindOverlaySurface(
        ppu, kPpuOverlaySource_Obj, (uint8_t *)hud_obj,
        (size_t)width * sizeof(uint32_t)));
    CHECK(PpuSetOverlayCapture(
        ppu, kPpuOverlaySource_Bg3, 0, 0,
        kActRaiserAuthenticWidth, kActRaiserSimulationHudHeight,
        kPpuOverlayFlag_RemoveFromGame));
    CHECK(PpuSetOverlayCapture(
        ppu, kPpuOverlaySource_Obj, 0, 0,
        kActRaiserAuthenticWidth, kActRaiserSimulationHudHeight,
        kPpuOverlayFlag_RemoveFromGame));
    CHECK(PpuSetOverlayOamRange(
        ppu, kMenuHourglassFirst, kActRaiserHudObjOamCount));
    request.requested_features = kSimFeature_SeparatedComposite |
        (suppress_raw ? kSimFeature_GroundProjection |
                            kSimFeature_ObjectBillboards
                      : 0);
    request.billboard_atlas_ready = suppress_raw;
    request.billboard_renderer_ready = suppress_raw;
    CHECK(Sim3D_PrepareCapture(ppu, &request));
    CHECK((ppu->overlayRenderBuffer[kPpuOverlaySource_Obj] == NULL) ==
          (suppress_raw != 0));
    Sim3D_FinishCapture(
        (uint8_t *)authentic, width * (int)sizeof(uint32_t), 1);
    CHECK(hud_obj[(size_t)kActRaiserHudObjUpperY * width +
                  extra + kActRaiserSimulationHourglassLeftX] != 0);
    if (!suppress_raw) {
      Sim3DOutputSurfaceViews views = {0};
      Sim3D_CaptureOutputSurfaceViews(&views);
      CHECK(views.planes[kSim3DPlane_Bg3Low].data ==
            (const uint8_t *)g_sim3d_layer_pixels[kSim3DPlane_Bg3Low]);
      CHECK(views.planes[kSim3DPlane_Bg3Low].width_pixels == (uint32_t)width);
      CHECK(views.planes[kSim3DPlane_Bg3Low].height_pixels ==
            kActRaiserAuthenticHeight);
      CHECK((views.planes[kSim3DPlane_Bg3Low].flags &
             SR_PPU_SURFACE_HAS_CONTENT) != 0u);
      CHECK(views.planes[kSim3DPlane_Obj0].data ==
            (const uint8_t *)g_sim3d_layer_pixels[kSim3DPlane_Obj0]);
      CHECK(views.hud_bg.data == (const uint8_t *)hud_bg);
      CHECK(views.hud_bg.height_pixels == kActRaiserSimulationHudHeight);
      CHECK(views.hud_obj.data == (const uint8_t *)hud_obj);
      CHECK(views.hud_obj.width_pixels == (uint32_t)width);
      CHECK(views.hud_obj.height_pixels == kActRaiserSimulationHudHeight);
    }
    CHECK(Sim3D_BeginFrame());
  }

  /* The same four-slot-sized capture at the old allocation is not the
   * promoted icon and must remain an overlay conflict. */
  PpuClearOverlayCaptures(ppu);
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
  CHECK(!Sim3D_PrepareCapture(ppu, &request));
  CHECK(!Sim3D_BeginFrame());

  /* An unrelated layer capture still owns its source and must fail closed. */
  PpuClearOverlayCaptures(ppu);
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg1, 0, 0, 16, 16,
      kPpuOverlayFlag_RemoveFromGame));
  CHECK(!Sim3D_PrepareCapture(ppu, &request));
  CHECK(!Sim3D_BeginFrame());
  ppu_free(ppu);
}

static void TestDioramaFixedColorSubtractCapture(void) {
  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;

  static uint8_t fb[kW * sizeof(uint32_t)];
  static uint32_t capture[kW];
  setup_virtual_bg(ppu, 0, fb, sizeof(fb));
  memset(capture, 0, sizeof(capture));
  ppu->cgram[0x11] = bgr555(10, 1, 3);
  ppu->fixedColor = bgr555(2, 1, 2); /* snap_02_gf4230: fixed=$0822 */
  ppu->cgwsel = 0x00;
  ppu->cgadsub = 0x81;               /* full subtract, math on BG1 */
  render_first_line(ppu);
  const uint32_t flat_pixel = ((const uint32_t *)(const void *)fb)[0];
  CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg1,
                              (uint8_t *)capture, sizeof(capture)));
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg1, 0, 0, kW, 1,
      kPpuOverlayFlag_RemoveFromGame |
          kPpuOverlayFlag_ApplyBgFixedColorSubtract));
  render_first_line(ppu);

  /* Subtraction happens before 5->8-bit expansion and clamps each component. */
  CHECK(capture[0] == (0xff000000u | rgb555(8, 0, 1)));
  CHECK((capture[0] & 0x00ffffffu) == (flat_pixel & 0x00ffffffu));

  PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg1, NULL, 0);
  g_new_ppu = saved_new_ppu;
  ppu_free(ppu);
}

/* Diorama capture follows a visual source onto the subscreen instead of
 * treating TM as the complete layer manifest. Marahna act 1 is the measured
 * real-game case: main=$06 (BG2+BG3), sub=$11 (BG1+OBJ). This focused fixture
 * keeps BG1 off main and proves the generic PPU exporter still produces the
 * isolated plane. */
static void TestSubscreenOnlyOverlayCapture(void) {
  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;

  static uint8_t fb[kW * sizeof(uint32_t)];
  static uint32_t capture[kW];
  setup_virtual_bg(ppu, 0, fb, sizeof(fb));
  memset(capture, 0, sizeof(capture));

  /* setup_virtual_bg authors a solid red BG1 on main. Move only that source
   * to sub and enable subscreen colour math so the PPU actually renders TS. */
  ppu->screenEnabled[0] = 0;
  ppu->screenEnabled[1] = 1u << kActRaiserPpuLayer_Bg1;
  ppu->cgwsel = 0x02;   /* colour-math addend is the subscreen */
  ppu->cgadsub = 0x01;  /* nonzero math mask makes the sub pass live */
  CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg1,
                              (uint8_t *)capture, sizeof(capture)));
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg1, 0, 0, kW, 1,
      kPpuOverlayFlag_RemoveFromGame));
  render_first_line(ppu);

  CHECK(capture[0] == (0xff000000u | rgb555(31, 0, 0)));
  CHECK(PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg1, 0));

  /* Main remains the authority when a source is enabled on both screens. A
   * main-only visibility window suppresses x=0; if TS overwrote the export,
   * that pixel would incorrectly become red again. */
  memset(capture, 0, sizeof(capture));
  ppu->screenEnabled[0] = 1u << kActRaiserPpuLayer_Bg1;
  ppu->screenWindowed[0] = 1u << kActRaiserPpuLayer_Bg1;
  ppu->screenWindowed[1] = 0;
  ppu->windowsel = 0x02; /* BG1 main: window 1 enabled, inside disabled */
  ppu->window1left = 0;
  ppu->window1right = 0;
  render_first_line(ppu);
  CHECK(capture[0] == 0);
  CHECK(capture[1] == (0xff000000u | rgb555(31, 0, 0)));

  PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg1, NULL, 0);

  /* OBJ extraction is screen-agnostic once the frontend claims its OAM range.
   * Exercise the measured Marahna ownership (OBJ on TS only) so changing the
   * frontend gate back to TM cannot be mistaken for a PPU limitation. */
  static uint32_t obj_capture[kW];
  ppu_reset(ppu);
  memset(fb, 0, sizeof(fb));
  memset(obj_capture, 0, sizeof(obj_capture));
  ppu->inidisp = 0x0f;
  ppu->bgmode = 1;
  ppu->screenEnabled[0] = 0;
  ppu->screenEnabled[1] = 1u << kPpuOverlaySource_Obj;
  ppu->cgwsel = 0x02;
  ppu->cgadsub = 0x01;
  ppu->cgram[0x81] = bgr555(31, 31, 0);
  set_solid_4bpp_tile(ppu, 0, 1);
  for (int slot = 0; slot < 128; slot++)
    ppu->oam[slot * 2] = (uint16_t)(0 | (0xe0u << 8));
  ppu->oam[0] = (uint16_t)(24 | (0u << 8));
  ppu->oam[1] = 0;
  PpuBeginDrawing(ppu, fb, sizeof(fb), 0);
  CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Obj,
                              (uint8_t *)obj_capture, sizeof(obj_capture)));
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Obj, 0, 0, kW, 1,
      kPpuOverlayFlag_RemoveFromGame));
  CHECK(PpuSetOverlayOamRange(ppu, 0, 1));
  render_first_line(ppu);
  CHECK(obj_capture[24] == (0xff000000u | rgb555(31, 31, 0)));
  CHECK(PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Obj, 0));

  PpuBindOverlaySurface(ppu, kPpuOverlaySource_Obj, NULL, 0);
  g_new_ppu = saved_new_ppu;
  ppu_free(ppu);
}

/* Marahna act 1's screens are two simultaneous inputs, not alternate layer
 * manifests: TM=$06 supplies BG2/BG3 and TS=$11 supplies BG1/OBJ, with a full
 * add selected by CGWSEL=$02/CGADSUB=$03. The addend captures must contain only
 * the winning TS source at each pixel so drawing all of them additively cannot
 * double-add an overlapping BG and sprite. */
static void TestFullAddSubscreenWinnerCapture(void) {
  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;

  static uint8_t fb[kW * sizeof(uint32_t)];
  static uint32_t bg1_capture[kW];
  static uint32_t bg2_capture[kW];
  static uint32_t bg3_capture[kW];
  static uint32_t obj_capture[kW];
  ppu_reset(ppu);
  memset(fb, 0, sizeof(fb));
  memset(bg1_capture, 0, sizeof(bg1_capture));
  memset(bg2_capture, 0, sizeof(bg2_capture));
  memset(bg3_capture, 0, sizeof(bg3_capture));
  memset(obj_capture, 0, sizeof(obj_capture));
  ppu->inidisp = 0x0f;
  ppu->bgmode = 1;
  ppu->screenEnabled[0] = 1u << kActRaiserPpuLayer_Bg2;
  ppu->screenEnabled[1] =
      (1u << kActRaiserPpuLayer_Bg1) |
      (1u << kPpuOverlaySource_Obj);
  ppu->cgwsel = 0x02;
  ppu->cgadsub = 0x03;
  ppu->cgram[0x11] = bgr555(31, 0, 0);  /* TS BG1: red addend */
  ppu->cgram[0x21] = bgr555(0, 0, 31);  /* TM BG2: blue base */
  ppu->cgram[0x81] = bgr555(0, 31, 0);  /* TS OBJ: green addend */
  set_solid_4bpp_tile(ppu, 0, 1);
  set_solid_4bpp_tile(ppu, 1, 1);
  ppu->bgTileAdr = 0;
  ppu->bgXsc[kActRaiserPpuLayer_Bg1] = 0x20;
  ppu->bgXsc[kActRaiserPpuLayer_Bg2] = 0x24;
  for (int i = 0; i < 0x400; i++) {
    ppu->vram[0x2000 + i] = (uint16_t)(1 | (1 << 10));
    ppu->vram[0x2400 + i] = (uint16_t)(1 | (2 << 10));
  }
  for (int slot = 0; slot < 128; slot++)
    ppu->oam[slot * 2] = (uint16_t)(0 | (0xe0u << 8));
  ppu->oam[0] = (uint16_t)(24 | (0u << 8));
  ppu->oam[1] = (uint16_t)(3u << 12); /* priority 3 wins TS over BG1 */

  PpuBeginDrawing(ppu, fb, sizeof(fb), 0);
  CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg1,
                              (uint8_t *)bg1_capture, sizeof(bg1_capture)));
  CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2,
                              (uint8_t *)bg2_capture, sizeof(bg2_capture)));
  CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Obj,
                              (uint8_t *)obj_capture, sizeof(obj_capture)));
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg1, 0, 0, kW, 1,
      kPpuOverlayFlag_RemoveFromGame |
          kPpuOverlayFlag_MarkFullAddSubscreen));
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg2, 0, 0, kW, 1,
      kPpuOverlayFlag_RemoveFromGame));
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Obj, 0, 0, kW, 1,
      kPpuOverlayFlag_RemoveFromGame |
          kPpuOverlayFlag_MarkFullAddSubscreen));
  CHECK(PpuSetOverlayOamRange(ppu, 0, 1));
  render_first_line(ppu);

  CHECK(bg2_capture[0] == (0xff000000u | rgb555(0, 0, 31)));
  CHECK(bg1_capture[0] == (0xff000000u | rgb555(31, 0, 0)));
  CHECK(obj_capture[0] == 0);
  CHECK(bg1_capture[24] == 0);
  CHECK(obj_capture[24] == (0xff000000u | rgb555(0, 31, 0)));

  /* BG1 is TS-only in this measured screen configuration. Its owning-screen
   * mask is white where BG1 supplies the colour addend and opaque black where
   * the higher-priority OBJ sprite wins, which lets a host wall light retain
   * Viper's authentic sprite occlusion. */
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg1, 0, 0, kW, 1,
      kPpuOverlayFlag_MarkOwningScreenWinner));
  memset(bg1_capture, 0, sizeof(bg1_capture));
  render_first_line(ppu);
  CHECK(bg1_capture[0] == 0xffffffffu);
  CHECK(bg1_capture[24] == 0xff000000u);
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg1, 0, 0, kW, 1,
      kPpuOverlayFlag_RemoveFromGame |
          kPpuOverlayFlag_MarkFullAddSubscreen));

  /* A host-relocated OBJ (the Marahna status icon in flat-HUD mode) is not a
   * world addend. Removing it must reveal the BG1 pixel it covered rather than
   * leave an icon-shaped hole, while the ordinary OAM capture range remains
   * valid for all other sprites. */
  CHECK(!PpuSetOverlayRelocatedOamRange(ppu, 1, 1));
  CHECK(PpuSetOverlayRelocatedOamRange(ppu, 0, 1));
  memset(bg1_capture, 0, sizeof(bg1_capture));
  memset(obj_capture, 0, sizeof(obj_capture));
  render_first_line(ppu);
  CHECK(bg1_capture[24] == (0xff000000u | rgb555(31, 0, 0)));
  CHECK(obj_capture[24] == 0);

  /* A captured BG3 is reinserted after the host's additive pass (or moved to
   * the anchored flat HUD). It must not punch its opaque glyphs out of the
   * underlying world addend. Model the Mode-1 priority quirk with a solid
   * priority-1 BG3 in front of BG2: BG1 remains captured underneath it. */
  ppu->screenEnabled[0] |= 1u << kActRaiserPpuLayer_Bg3;
  ppu->bgmode = 9;
  ppu->bgTileAdr = 0x0400; /* BG3 character base $4000; BG1/BG2 remain $0000 */
  ppu->bgXsc[kActRaiserPpuLayer_Bg3] = 0x28;
  ppu->cgram[5] = bgr555(31, 31, 31);
  set_solid_2bpp_tile(ppu, 0x4000, 0, 1);
  for (int i = 0; i < 0x400; i++)
    ppu->vram[0x2800 + i] = (uint16_t)((1 << 10) | (1 << 13));
  CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg3,
                              (uint8_t *)bg3_capture, sizeof(bg3_capture)));
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg3, 0, 0, kW, 1,
      kPpuOverlayFlag_RemoveFromGame));
  memset(bg1_capture, 0, sizeof(bg1_capture));
  render_first_line(ppu);
  CHECK(bg3_capture[0] == (0xff000000u | rgb555(31, 31, 31)));
  CHECK(bg1_capture[0] == (0xff000000u | rgb555(31, 0, 0)));

  /* The filter also keys on the reconstructed main winner's CGADSUB bit. A
   * manually stale tag must not brighten BG2 when only BG1 math is selected. */
  memset(bg1_capture, 0, sizeof(bg1_capture));
  memset(obj_capture, 0, sizeof(obj_capture));
  ppu->cgadsub = 0x01;
  render_first_line(ppu);
  CHECK(bg1_capture[0] == 0);
  CHECK(obj_capture[24] == 0);

  PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg1, NULL, 0);
  PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2, NULL, 0);
  PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg3, NULL, 0);
  PpuBindOverlaySurface(ppu, kPpuOverlaySource_Obj, NULL, 0);
  g_new_ppu = saved_new_ppu;
  ppu_free(ppu);
}

/* A BG2-stage host effect needs the final main-screen winner, not merely the
 * isolated BG2 pixels. Put BG1 high-priority art over the right half and prove
 * the exported mask is white only where BG2 remains drawable; its untouched
 * region is opaque black so SDL_BLENDMODE_MUL actually erases effect RGB. */
static void TestMainScreenWinnerMask(void) {
  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;
  static uint8_t fb[kW * sizeof(uint32_t)];
  static uint32_t mask[kW];
  ppu_reset(ppu);
  memset(fb, 0, sizeof(fb));
  memset(mask, 0, sizeof(mask));
  ppu->inidisp = 0x0f;
  ppu->bgmode = 1;
  ppu->screenEnabled[0] =
      (1u << kActRaiserPpuLayer_Bg1) |
      (1u << kActRaiserPpuLayer_Bg2);
  ppu->cgram[0x11] = bgr555(31, 0, 0);
  ppu->cgram[0x21] = bgr555(0, 0, 31);
  set_solid_4bpp_tile(ppu, 1, 1);
  set_solid_4bpp_tile(ppu, 2, 1);
  ppu->bgTileAdr = 0;
  ppu->bgXsc[kActRaiserPpuLayer_Bg1] = 0x20;
  ppu->bgXsc[kActRaiserPpuLayer_Bg2] = 0x24;
  for (int x = 0; x < 32; x++) {
    /* Transparent BG1 on the left; high-priority BG1 on the right. */
    ppu->vram[0x2000 + x] = x < 16 ? 0 : (uint16_t)(1 | (1 << 10) |
                                                     (1 << 13));
    ppu->vram[0x2400 + x] = (uint16_t)(2 | (2 << 10));
  }
  PpuBeginDrawing(ppu, fb, sizeof(fb), 0);
  CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2,
                              (uint8_t *)mask, sizeof(mask)));
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg2, 0, 0, kW, 1,
      kPpuOverlayFlag_MarkMainScreenWinner));
  render_first_line(ppu);
  CHECK(mask[0] == 0xffffffffu);
  CHECK(mask[127] == 0xffffffffu);
  CHECK(mask[128] == 0xff000000u);
  CHECK(mask[255] == 0xff000000u);
  CHECK(PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg2, 0));

  /* The owning-screen form must reduce to the same mask for a TM source; its
   * extra behavior is only the TS fallback exercised by the Marahna fixture. */
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg2, 0, 0, kW, 1,
      kPpuOverlayFlag_MarkOwningScreenWinner));
  memset(mask, 0, sizeof(mask));
  render_first_line(ppu);
  CHECK(mask[0] == 0xffffffffu);
  CHECK(mask[127] == 0xffffffffu);
  CHECK(mask[128] == 0xff000000u);
  CHECK(mask[255] == 0xff000000u);
  PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2, NULL, 0);
  g_new_ppu = saved_new_ppu;
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

  /* A room-scoped black resolve makes the sparse primary BG2 an opaque black
   * backing, including underneath high-priority BG2 art. It is extraction-only:
   * the authentic scanout still resolves a transparent tile to CGRAM backdrop. */
  ppu->cgram[0] = bgr555(0, 31, 0);
  for (int i = 0; i < 0x400; i++)
    ppu->vram[0x2000 + i] = 0;
  ppu->vram[0x2000] = (uint16_t)(1 | (2 << 10) | (1u << 13));
  /* Fill and geometry are independently configured. Setting the fill first
   * must survive PpuSetOverlayCapture; call order is not presentation policy. */
  CHECK(PpuSetOverlayTransparentFill(
      ppu, kPpuOverlaySource_Bg2,
      kPpuOverlayTransparentFill_Black, 0));
  CHECK(ppu->overlayCaptures[kPpuOverlaySource_Bg2]
            .transparentFillConfigured == 1);
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg2, 0, 0, kW, 2,
      0));
  CHECK(PpuOverlayTransparentFillColor(
            ppu, kPpuOverlaySource_Bg2) == 0xff000000u);
  ppu_runLine(ppu, 0);
  ppu_runLine(ppu, 1);
  CHECK(primary[0] == 0xff000000u);
  CHECK(primary[8] == 0xff000000u);
  CHECK(high[0] == 0xffff00ffu);
  CHECK(high[8] == 0);
  CHECK(PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg2, 0));
  CHECK(PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg2, 1));
  CHECK(((const uint32_t *)(const void *)fb)[8] == rgb555(0, 31, 0));

  /* CGRAM index zero is a valid opaque fill choice even though tile colour
   * zero remains transparent. A non-zero index follows live palette changes. */
  CHECK(PpuSetOverlayTransparentFill(
      ppu, kPpuOverlaySource_Bg2,
      kPpuOverlayTransparentFill_Cgram, 0));
  CHECK(PpuOverlayTransparentFillColor(
            ppu, kPpuOverlaySource_Bg2) == 0xff00ff00u);
  ppu_runLine(ppu, 0);
  ppu_runLine(ppu, 1);
  CHECK(primary[8] == 0xff00ff00u);
  ppu->cgram[0x21] = bgr555(0, 0, 31);
  CHECK(PpuSetOverlayTransparentFill(
      ppu, kPpuOverlaySource_Bg2,
      kPpuOverlayTransparentFill_Cgram, 0x21));
  CHECK(PpuOverlayTransparentFillColor(
            ppu, kPpuOverlaySource_Bg2) == 0xff0000ffu);

  /* An active capture with its layer disabled still clears the surfaces, but
   * correctly reports no content in either destination. */
  ppu->screenEnabled[0] = 0;
  ppu_runLine(ppu, 0);
  ppu_runLine(ppu, 1);
  CHECK(!PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg2, 0));
  CHECK(!PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg2, 1));
  CHECK(!PpuOverlaySurfaceHasContent(ppu, kPpuOverlaySource_Bg2, 4));
  CHECK(!PpuOverlaySurfaceHasContent(NULL, kPpuOverlaySource_Bg2, 0));
  CHECK(!PpuSetOverlayTransparentFill(
      ppu, kPpuOverlaySource_Bg2,
      (PpuOverlayTransparentFill)99, 0));
  CHECK(PpuSetOverlayTransparentFill(
      ppu, kPpuOverlaySource_Bg2,
      kPpuOverlayTransparentFill_None, 0));
  CHECK(ppu->overlayCaptures[kPpuOverlaySource_Bg2]
            .transparentFillConfigured == 1);
  CHECK(PpuOverlayTransparentFillColor(
            ppu, kPpuOverlaySource_Bg2) == 0);
  PpuClearOverlayCaptures(ppu);
  CHECK(ppu->overlayCaptures[kPpuOverlaySource_Bg2]
            .transparentFillConfigured == 0);
  CHECK(PpuOverlayTransparentFillColor(
            ppu, kPpuOverlaySource_Bg2) == 0);

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
      PpuSetVerticalMarginLayerClip(
          ppu, (uint8_t)bg2, clip_rows, kTop);
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

/* The bottom half of the same contract. A layer can reach its finite-world
 * floor before the primary playfield does; rows after that point must become
 * transparent rather than wrapping to the layer's top. */
static void TestVerticalMarginBottomLayerClip(void) {
  enum { kBottom = 8, kRows = kBottom + 1, kPitch = kW * 4 };
  const int bg2 = kActRaiserPpuLayer_Bg2;
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;

  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;
  static uint8_t fb[kW * (kPpuYPixels + kBottom) * 4];
  static uint32_t capture[kW * (kPpuYPixels + kBottom)];

  static const int clip_cases[] = { -1, 0, 4 };
  for (size_t case_index = 0;
       case_index < sizeof(clip_cases) / sizeof(clip_cases[0]);
       case_index++) {
    const int clip_rows = clip_cases[case_index];
    ppu_reset(ppu);
    memset(fb, 0, sizeof(fb));
    memset(capture, 0, sizeof(capture));
    ppu->inidisp = 0x0f;
    ppu->bgmode = 1;
    ppu->screenEnabled[0] = (uint8_t)(1u << bg2);
    ppu->cgram[0x21] = bgr555(31, 0, 31);
    set_solid_4bpp_tile(ppu, 1, 1);
    ppu->bgXsc[bg2] = 0x20 | 3;
    for (int i = 0; i < 0x1000; i++)
      ppu->vram[0x2000 + i] = (uint16_t)(1 | (2 << 10));

    PpuSetExtraVerticalSpace(ppu, 0, kBottom);
    if (clip_rows >= 0)
      PpuSetVerticalMarginLayerClip(ppu, (uint8_t)bg2, 0, clip_rows);
    PpuBeginDrawing(ppu, fb, kPitch, 0);
    CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2,
                                (uint8_t *)capture, kPitch));
    CHECK(PpuSetOverlayCapture(
        ppu, kPpuOverlaySource_Bg2, 0, kPpuYPixels - 1,
        kW, kRows, kPpuOverlayFlag_RemoveFromGame));
    ppu_runLine(ppu, kPpuYPixels);
    for (int line = kPpuYPixels + 1;
         line <= kPpuYPixels + kBottom; line++)
      ppu_runMarginLine(ppu, line);

    const int base = kPpuYPixels - 1;
    CHECK((capture[base * kW] & 0xffffffu) != 0);
    if (clip_rows < 0) {
      CHECK((capture[(base + kBottom) * kW] & 0xffffffu) != 0);
    } else {
      for (int row = 1; row <= clip_rows; row++)
        CHECK((capture[(base + row) * kW] & 0xffffffu) != 0);
      for (int row = clip_rows + 1; row <= kBottom; row++)
        CHECK(capture[(base + row) * kW] == 0);
    }
    PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2, NULL, 0);
  }

  g_new_ppu = saved_new_ppu;
  ppu_free(ppu);
}

/* OAM Y aliases a below-screen sprite into the above-screen byte range. The
 * exact signed sideband disambiguates it; without that sideband neither
 * vertical margin may trust the slot. */
static void TestVerticalMarginExactObj(void) {
  enum { kBottom = 8, kPitch = kW * 4, kSpriteY = 228 };
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;

  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;
  static uint8_t fb[kW * (kPpuYPixels + kBottom) * 4];
  static uint32_t capture[kW * (kPpuYPixels + kBottom)];
  ppu_reset(ppu);
  memset(fb, 0, sizeof(fb));
  memset(capture, 0, sizeof(capture));
  ppu->inidisp = 0x0f;
  ppu->screenEnabled[0] = 1u << 4;  /* OBJ */
  ppu->cgram[0x81] = bgr555(31, 31, 0);
  set_solid_4bpp_tile(ppu, 0, 1);
  ppu->oam[0] = (uint16_t)(24 | ((kSpriteY & 0xff) << 8));
  ppu->oam[1] = 0;
  PpuSetExtraVerticalSpace(ppu, 0, kBottom);
  PpuBeginDrawing(ppu, fb, kPitch, 0);
  CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Obj,
                              (uint8_t *)capture, kPitch));
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Obj, 0, 0, kW,
      kPpuYPixels + kBottom, kPpuOverlayFlag_RemoveFromGame));
  CHECK(PpuSetOverlayOamRange(ppu, 0, 1));

  PpuSetObjExactPosition(ppu, 0, 24, kSpriteY);
  ppu_runLine(ppu, 0);
  ppu_runMarginLine(ppu, kSpriteY + 1);
  CHECK((capture[kSpriteY * kW + 24] & 0xffffffu) != 0);

  PpuClearObjExactPositions(ppu);
  ppu_runLine(ppu, 0);
  ppu_runMarginLine(ppu, kSpriteY + 1);
  CHECK(capture[kSpriteY * kW + 24] == 0);

  PpuBindOverlaySurface(ppu, kPpuOverlaySource_Obj, NULL, 0);
  g_new_ppu = saved_new_ppu;
  ppu_free(ppu);
}

/* Layer extents cap presentation independently of the global canvas and of the
 * edge strategy. The first two rows prove a band can remove the default cap;
 * the vertical arm proves only synthetic rows are removed. */
static void TestLayerPresentationExtents(void) {
  enum { kBudget = 8, kWidth = kW + 2 * kBudget };
  const int bg2 = kActRaiserPpuLayer_Bg2;
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;

  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;
  static uint8_t fb[kWidth * 5 * 4];
  static uint32_t capture[kWidth * 3];

  ppu_reset(ppu);
  memset(fb, 0, sizeof(fb));
  memset(capture, 0, sizeof(capture));
  ppu->inidisp = 0x0f;
  ppu->bgmode = 1;
  ppu->screenEnabled[0] = (uint8_t)(1u << bg2);
  ppu->cgram[0x21] = bgr555(31, 0, 31);
  set_solid_4bpp_tile(ppu, 1, 1);
  ppu->bgTileAdr = 0;
  ppu->bgXsc[bg2] = 0x20 | 3;
  for (int i = 0; i < 0x1000; i++)
    ppu->vram[0x2000 + i] = (uint16_t)(1 | (2 << 10));

  PpuSetExtraSpace(ppu, kBudget);
  PpuSetWidescreenLayerMirror(ppu, (uint8_t)(1u << bg2));
  PpuSetWidescreenPadCapturedToBudget(ppu, 1);
  PpuSetWidescreenLayerExtent(
      ppu, (uint8_t)bg2, 3, 5,
      kPpuWidescreenExtentAvailable, kPpuWidescreenExtentAvailable);
  PpuSetWidescreenLayerExtentBand(
      ppu, (uint8_t)bg2, 1, 2,
      kPpuWidescreenExtentAvailable, kPpuWidescreenExtentAvailable);

  PpuBeginDrawing(ppu, fb, kWidth * 4, 0);
  CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2,
                              (uint8_t *)capture, kWidth * 4));
  CHECK(PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg2,
                             -kBudget, 0, kWidth, 3,
                             kPpuOverlayFlag_RemoveFromGame));
  ppu_runLine(ppu, 1);
  ppu_runLine(ppu, 2);

  /* Default row: only three left and five right extension pixels survive. */
  CHECK(capture[kBudget - 4] == 0);
  CHECK((capture[kBudget - 3] & 0xffffffu) != 0);
  CHECK((capture[kBudget + kW + 4] & 0xffffffu) != 0);
  CHECK(capture[kBudget + kW + 5] == 0);
  CHECK((capture[kBudget + 10] & 0xffffffu) != 0);
  /* Band row: available removes the layer default cap and reaches the budget. */
  CHECK((capture[kWidth] & 0xffffffu) != 0);
  CHECK((capture[2 * kWidth - 1] & 0xffffffu) != 0);
  PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2, NULL, 0);

  enum { kTop = 4, kRows = kTop + 1 };
  static uint32_t vertical_capture[kW * kRows];
  ppu_reset(ppu);
  memset(fb, 0, sizeof(fb));
  memset(vertical_capture, 0, sizeof(vertical_capture));
  ppu->inidisp = 0x0f;
  ppu->bgmode = 1;
  ppu->screenEnabled[0] = (uint8_t)(1u << bg2);
  ppu->cgram[0x21] = bgr555(31, 0, 31);
  set_solid_4bpp_tile(ppu, 1, 1);
  ppu->bgTileAdr = 0;
  ppu->bgXsc[bg2] = 0x20 | 3;
  for (int i = 0; i < 0x1000; i++)
    ppu->vram[0x2000 + i] = (uint16_t)(1 | (2 << 10));
  PpuSetExtraVerticalSpace(ppu, kTop, 0);
  PpuSetWidescreenLayerExtent(
      ppu, (uint8_t)bg2,
      kPpuWidescreenExtentAvailable, kPpuWidescreenExtentAvailable,
      2, kPpuWidescreenExtentAvailable);
  PpuBeginDrawing(ppu, fb, kW * 4, 0);
  CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2,
                              (uint8_t *)vertical_capture, kW * 4));
  CHECK(PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg2,
                             0, -kTop, kW, kRows,
                             kPpuOverlayFlag_RemoveFromGame));
  ppu_runLine(ppu, 0);
  for (int line = 1 - kTop; line <= 0; line++)
    ppu_runMarginLine(ppu, line);
  ppu_runLine(ppu, 1);
  CHECK(vertical_capture[0] == 0);
  CHECK(vertical_capture[kW] == 0);
  CHECK((vertical_capture[2 * kW] & 0xffffffu) != 0);
  CHECK((vertical_capture[3 * kW] & 0xffffffu) != 0);
  CHECK((vertical_capture[4 * kW] & 0xffffffu) != 0);

  enum { kBottom = 3, kBottomRows = kBottom + 1 };
  static uint32_t bottom_capture[kW * (kPpuYPixels + kBottom)];
  memset(bottom_capture, 0, sizeof(bottom_capture));
  PpuSetWidescreenLayerExtent(
      ppu, (uint8_t)bg2,
      kPpuWidescreenExtentAvailable, kPpuWidescreenExtentAvailable,
      kPpuWidescreenExtentAvailable, 1);
  CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2,
                              (uint8_t *)bottom_capture, kW * 4));
  CHECK(PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg2,
                             0, kPpuYPixels - 1, kW, kBottomRows,
                             kPpuOverlayFlag_RemoveFromGame));
  ppu_runLine(ppu, kPpuYPixels);
  for (int line = kPpuYPixels + 1;
       line <= kPpuYPixels + kBottom; line++)
    ppu_runMarginLine(ppu, line);
  const int bottom_y0 = kPpuYPixels - 1;
  CHECK((bottom_capture[(bottom_y0 + 0) * kW] & 0xffffffu) != 0);
  CHECK((bottom_capture[(bottom_y0 + 1) * kW] & 0xffffffu) != 0);
  CHECK(bottom_capture[(bottom_y0 + 2) * kW] == 0);
  CHECK(bottom_capture[(bottom_y0 + 3) * kW] == 0);

  PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2, NULL, 0);
  g_new_ppu = saved_new_ppu;
  ppu_free(ppu);
}

/* A row band that reaches the bottom of the authentic viewport describes the
 * same content family in the synthetic bottom margin. Bloodpool's moving water
 * is the motivating case: falling back to the BG2 default there reflects the
 * animation and reverses its apparent direction. This fixture also pins the
 * band's Available extent, because retaining the default 0/0 cap would leave
 * the margin transparent even if repeat mode itself were selected. */
static void TestMovingEdgePoliciesInVerticalMargins(void) {
  enum {
    kBudget = 8,
    kWidth = kW + 2 * kBudget,
    kRows = kPpuYPixels + 1,
  };
  const int bg2 = kActRaiserPpuLayer_Bg2;
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;

  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;
  static uint8_t fb[kWidth * kRows * 4];
  static uint32_t capture[kWidth * kRows];
  ppu_reset(ppu);
  memset(fb, 0, sizeof(fb));
  memset(capture, 0, sizeof(capture));
  ppu->inidisp = 0x0f;
  ppu->bgmode = 1;
  ppu->screenEnabled[0] = (uint8_t)(1u << bg2);
  ppu->cgram[0x21] = bgr555(31, 0, 31);
  ppu->cgram[0x22] = bgr555(0, 31, 31);
  set_solid_4bpp_tile(ppu, 1, 1);
  set_solid_4bpp_tile(ppu, 2, 2);
  ppu->bgTileAdr = 0;
  ppu->bgXsc[bg2] = 0x20 | 3;
  /* Alternate tile columns so both edge selection and motion-phase
   * compensation remain observable after a non-zero horizontal scroll. */
  for (int i = 0; i < 0x1000; i++)
    ppu->vram[0x2000 + i] = (uint16_t)(
        ((i & 31) & 1 ? 2 : 1) | (2 << 10));

  PpuSetExtraSpace(ppu, kBudget);
  PpuSetWidescreenLayerMirror(ppu, (uint8_t)(1u << bg2));
  PpuSetWidescreenLayerRepeatBand(ppu, (uint8_t)bg2, 136, 224);
  PpuSetWidescreenPadCapturedToBudget(ppu, 1);
  PpuSetWidescreenLayerExtent(
      ppu, (uint8_t)bg2, 0, 0,
      kPpuWidescreenExtentAvailable, kPpuWidescreenExtentAvailable);
  PpuSetWidescreenLayerExtentBand(
      ppu, (uint8_t)bg2, 136, 224,
      kPpuWidescreenExtentAvailable, kPpuWidescreenExtentAvailable);
  PpuSetExtraVerticalSpace(ppu, 0, 1);

  PpuBeginDrawing(ppu, fb, kWidth * 4, 0);
  CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2,
                              (uint8_t *)capture, kWidth * 4));
  CHECK(PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg2,
                             -kBudget, kPpuYPixels, kWidth, 1,
                             kPpuOverlayFlag_RemoveFromGame));
  ppu_runLine(ppu, kPpuYPixels);
  ppu_runMarginLine(ppu, kPpuYPixels + 1);

  const uint32_t *bottom_row = capture + kPpuYPixels * kWidth;
  const uint32_t left_margin =
      bottom_row[kBudget - 1];                          /* screen x=-1 */
  const uint32_t left_source = bottom_row[kBudget + 255];
  const uint32_t reflected_left_source = bottom_row[kBudget + 1];
  CHECK((left_margin & 0xffffffu) != 0);
  CHECK(left_margin == left_source);
  CHECK(left_margin != reflected_left_source);

  const uint32_t right_margin =
      bottom_row[kBudget + kW];                         /* screen x=256 */
  const uint32_t right_source = bottom_row[kBudget];
  const uint32_t reflected_right_source = bottom_row[kBudget + 254];
  CHECK((right_margin & 0xffffffu) != 0);
  CHECK(right_margin == right_source);
  CHECK(right_margin != reflected_right_source);

  PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2, NULL, 0);

  /* Preserve the legacy whole-mask precedence used by diagnostics and older
   * callers: padding overrides Clamp, and Repeat overrides Mirror. */
  memset(fb, 0, sizeof(fb));
  memset(capture, 0, sizeof(capture));
  PpuSetExtraSpace(ppu, kBudget);
  PpuSetWidescreenLayerClamp(ppu, (uint8_t)(1u << bg2));
  PpuSetWidescreenLayerMirror(ppu, (uint8_t)(1u << bg2));
  PpuSetWidescreenLayerRepeat(ppu, (uint8_t)(1u << bg2));
  PpuSetWidescreenPadCapturedToBudget(ppu, 1);
  PpuBeginDrawing(ppu, fb, kWidth * 4, 0);
  CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2,
                              (uint8_t *)capture, kWidth * 4));
  CHECK(PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg2,
                             -kBudget, 0, kWidth, 1,
                             kPpuOverlayFlag_RemoveFromGame));
  ppu_runLine(ppu, 1);
  CHECK(capture[kBudget - 1] == capture[kBudget + 255]);
  CHECK(capture[kBudget - 1] != capture[kBudget + 1]);
  PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2, NULL, 0);

  /* Two independent bands on the same layer: upper art mirrors while keeping
   * the authentic scroll direction, lower art cyclically repeats. The normal
   * mirror phase samples x=-1 from source 249 at hscroll=4; legacy reflection
   * would sample source 1 and visibly reverse the moving pattern. */
  memset(fb, 0, sizeof(fb));
  memset(capture, 0, sizeof(capture));
  ppu->hScroll[bg2] = 4;
  PpuSetExtraSpace(ppu, kBudget);
  PpuSetWidescreenLayerClamp(ppu, (uint8_t)(1u << bg2));
  PpuSetWidescreenLayerBand(
      ppu, (uint8_t)bg2, 0, 64, kPpuWidescreenBandFill_Mirror,
      kPpuWidescreenMotion_NormalScroll);
  PpuSetWidescreenLayerBand(
      ppu, (uint8_t)bg2, 64, 128, kPpuWidescreenBandFill_Repeat,
      kPpuWidescreenMotion_FillRelative);
  PpuSetWidescreenPadCapturedToBudget(ppu, 1);
  int mapped_x = 0;
  PpuWidescreenLayerPolicy mapped_policy;
  CHECK(PpuMapWidescreenLayerX(
      ppu, (uint8_t)bg2, 0, -1, &mapped_x, &mapped_policy));
  CHECK(mapped_x == 249 && mapped_policy.band_override &&
        mapped_policy.fill == kPpuWidescreenBandFill_Mirror &&
        mapped_policy.motion == kPpuWidescreenMotion_NormalScroll);
  CHECK(PpuMapWidescreenLayerX(
      ppu, (uint8_t)bg2, 80, -1, &mapped_x, &mapped_policy));
  CHECK(mapped_x == 255 && mapped_policy.band_override &&
        mapped_policy.fill == kPpuWidescreenBandFill_Repeat);
  CHECK(!PpuMapWidescreenLayerX(
      ppu, (uint8_t)bg2, 160, -1, &mapped_x, &mapped_policy));
  CHECK(!mapped_policy.band_override &&
        mapped_policy.fill == kPpuWidescreenBandFill_Clamp);
  PpuBeginDrawing(ppu, fb, kWidth * 4, 0);
  CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2,
                              (uint8_t *)capture, kWidth * 4));
  CHECK(PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg2,
                             -kBudget, 0, kWidth, 128,
                             kPpuOverlayFlag_RemoveFromGame));
  ppu_runLine(ppu, 1);   /* authentic row 0: normal-motion Mirror */
  ppu_runLine(ppu, 81);  /* authentic row 80: legacy Repeat */

  const uint32_t *mirror_row = capture;
  CHECK(mirror_row[kBudget - 1] == mirror_row[kBudget + 249]);
  CHECK(mirror_row[kBudget - 1] != mirror_row[kBudget + 1]);
  const uint32_t *repeat_row = capture + 80 * kWidth;
  CHECK(repeat_row[kBudget - 1] == repeat_row[kBudget + 255]);
  CHECK(repeat_row[kBudget + kW] == repeat_row[kBudget]);

  PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2, NULL, 0);

  /* Aitos, Northwall, and the Death Heim rematches use whole-layer Repeat for
   * moving cloud/snow BG2. Pin the synthetic TOP margin separately: it does not
   * need a row band and must never fall back to reflection there. */
  memset(fb, 0, sizeof(fb));
  memset(capture, 0, sizeof(capture));
  ppu->hScroll[bg2] = 0;
  PpuSetExtraSpace(ppu, kBudget);
  PpuSetWidescreenLayerRepeat(ppu, (uint8_t)(1u << bg2));
  PpuSetWidescreenPadCapturedToBudget(ppu, 1);
  PpuSetExtraVerticalSpace(ppu, 1, 0);
  PpuBeginDrawing(ppu, fb, kWidth * 4, 0);
  CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2,
                              (uint8_t *)capture, kWidth * 4));
  CHECK(PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg2,
                             -kBudget, -1, kWidth, 1,
                             kPpuOverlayFlag_RemoveFromGame));
  ppu_runMarginLine(ppu, 0);

  const uint32_t *top_row = capture;
  CHECK((top_row[kBudget - 1] & 0xffffffu) != 0);
  CHECK(top_row[kBudget - 1] == top_row[kBudget + 255]);
  CHECK(top_row[kBudget - 1] != top_row[kBudget + 1]);
  CHECK((top_row[kBudget + kW] & 0xffffffu) != 0);
  CHECK(top_row[kBudget + kW] == top_row[kBudget]);
  CHECK(top_row[kBudget + kW] != top_row[kBudget + 254]);

  PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg2, NULL, 0);
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

  /* Padding to the fixed capture budget applies only to synthesized
   * mirror/repeat layers. A raw-wrap BG3 HUD split can occupy the live margin,
   * but it must not leak wrapped HUD tiles into a side whose finite-world
   * margin has collapsed. The sim-arrowbug replay exposed this as a solid
   * 24x32 mismatch at the far-left edge of every separated composite. */
  {
    const int bg3 = kActRaiserPpuLayer_Bg3;
    ppu_reset(ppu);
    memset(capture, 0, sizeof(capture));
    memset(fb, 0, sizeof(fb));
    ppu->inidisp = 0x0f;
    ppu->bgmode = 1;
    ppu->screenEnabled[0] = (uint8_t)(1u << bg3);
    ppu->cgram[0] = bgr555(0, 0, 0);
    ppu->cgram[1] = bgr555(31, 0, 31);
    set_solid_2bpp_tile(ppu, 0, 1, 1);
    ppu->bgTileAdr = 0;
    ppu->bgXsc[bg3] = 0x20 | 0x3;
    for (int i = 0; i < 0x1000; i++)
      ppu->vram[0x2000 + i] = 1;
    PpuSetExtraSpace(ppu, kBudget);
    PpuSetWidescreenPadCapturedToBudget(ppu, 1);
    PpuSetWidescreenHudSplit(ppu, 8, 100, 100, 8, 8);
    PpuSetExtraSideSpace(ppu, 0, kLiveRight, 0);

    PpuBeginDrawing(ppu, fb, kCaptureWidth * 4, 0);
    CHECK(PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg3,
                                (uint8_t *)capture,
                                kCaptureWidth * sizeof(uint32_t)));
    CHECK(PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg3, -kBudget, 0,
                               kCaptureWidth, 2, 0));
    ppu_runLine(ppu, 1);

    CHECK(capture[0] == 0);
    CHECK((capture[kBudget + 10] & 0xffffffu) != 0);
    PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg3, NULL, 0);
  }

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

static void TestAuthenticComparisonSurface(void) {
  const bool saved_new_ppu = g_new_ppu;
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;

  static uint8_t fb[kW * sizeof(uint32_t)];
  static uint32_t authentic[kW];
  static uint32_t isolated_bg1[kW];

  /* The enhanced diorama path removes BG1 from the ordinary framebuffer. The
   * parallel authentic pass must independently render the complete native
   * winner, not merely copy the presentation framebuffer after extraction. */
  g_new_ppu = true;
  setup_virtual_bg(ppu, 0, fb, sizeof(fb));
  memset(authentic, 0, sizeof(authentic));
  memset(isolated_bg1, 0, sizeof(isolated_bg1));
  CHECK(PpuBindAuthenticSurface(
      ppu, (uint8_t *)authentic, sizeof(authentic)));
  CHECK(PpuBindOverlaySurface(
      ppu, kPpuOverlaySource_Bg1,
      (uint8_t *)isolated_bg1, sizeof(isolated_bg1)));
  CHECK(PpuSetOverlayCapture(
      ppu, kPpuOverlaySource_Bg1, 0, 0, kW, 1,
      kPpuOverlayFlag_RemoveFromGame));
  render_first_line(ppu);
  CHECK((isolated_bg1[0] & 0x00ffffffu) == rgb555(31, 0, 0));
  CHECK((((const uint32_t *)(const void *)fb)[0] & 0x00ffffffu) == 0);
  CHECK((authentic[0] & 0x00ffffffu) == rgb555(31, 0, 0));
  CHECK((authentic[0] & 0xff000000u) == 0);

  /* Configurations using the legacy renderer have no extracted layers; their
   * complete scanout is mirrored byte-for-byte into the same surface. */
  g_new_ppu = false;
  ppu_reset(ppu);
  memset(fb, 0, sizeof(fb));
  memset(authentic, 0x5a, sizeof(authentic));
  PpuBeginDrawing(ppu, fb, sizeof(fb), 0);
  CHECK(PpuBindAuthenticSurface(
      ppu, (uint8_t *)authentic, sizeof(authentic)));
  ppu->inidisp = 0x0f;
  ppu->cgram[0] = bgr555(7, 13, 29);
  render_first_line(ppu);
  CHECK(memcmp(fb, authentic, sizeof(fb)) == 0);

  g_new_ppu = saved_new_ppu;
  ppu_free(ppu);
}

static void TestAuthenticCameraAndSurfaceContract(void) {
  enum { kExtra = 8, kWidth = kW + kExtra * 2, kGuard = 8 };
  const bool saved_new_ppu = g_new_ppu;
  g_new_ppu = true;
  Ppu *ppu = ppu_init();
  CHECK(ppu != NULL);
  if (!ppu) return;

  static uint8_t fb[kWidth * sizeof(uint32_t)];
  static uint32_t authentic[kWidth];
  static uint16_t native_bg1[kH];
  static uint16_t native_bg2[kH];

  /* The enhanced camera sees the even tile while the independent native BG1
   * camera starts one tile later. This cannot pass by translating an already
   * composited widescreen image: the authentic pass consumes its own BG1
   * scroll phase before priority/color resolve. */
  setup_virtual_bg(ppu, kExtra, fb, sizeof(fb));
  fill_virtual_native_ring(
      ppu, (uint16_t)(1 | (1 << 10)),
      (uint16_t)(2 | (2 << 10)));
  ppu->hScroll[kActRaiserPpuLayer_Bg1] = 0;
  for (int y = 0; y < kH; y++) native_bg1[y] = 8;
  memset(authentic, 0, sizeof(authentic));
  CHECK(PpuBindAuthenticSurface(
      ppu, (uint8_t *)authentic, sizeof(authentic)));
  CHECK(PpuSetAuthenticCameraFrame(
      ppu, kPpuAuthenticCameraLayer_Bg1, native_bg1, NULL, 8));
  CHECK(PpuAuthenticCameraFrameReady(
      ppu, kPpuAuthenticCameraLayer_Bg1));
  CHECK(!PpuAuthenticCameraFrameReady(
      ppu, kPpuAuthenticCameraLayer_All));
  render_first_line(ppu);
  const uint32_t *enhanced = (const uint32_t *)(const void *)fb;
  CHECK(enhanced[kExtra] == rgb555(31, 0, 0));
  CHECK(authentic[kExtra] == rgb555(0, 0, 31));
  CHECK(ppu->hScroll[kActRaiserPpuLayer_Bg1] == 0);

  /* BG2 receives its own phase rather than inheriting a BG1-sized crop. This
   * pins parallax and camera-driven raster layers to the independent pass. */
  setup_virtual_bg(ppu, kExtra, fb, sizeof(fb));
  ppu->screenEnabled[0] =
      (uint8_t)(1u << kActRaiserPpuLayer_Bg2);
  ppu->bgXsc[kActRaiserPpuLayer_Bg2] = 0x20 | 3;
  fill_virtual_native_ring(
      ppu, (uint16_t)(1 | (1 << 10)),
      (uint16_t)(2 | (2 << 10)));
  ppu->hScroll[kActRaiserPpuLayer_Bg2] = 0;
  for (int y = 0; y < kH; y++) native_bg2[y] = 8;
  memset(authentic, 0, sizeof(authentic));
  CHECK(PpuBindAuthenticSurface(
      ppu, (uint8_t *)authentic, sizeof(authentic)));
  CHECK(PpuSetAuthenticCameraFrame(
      ppu, kPpuAuthenticCameraLayer_Bg2, NULL, native_bg2, 8));
  render_first_line(ppu);
  enhanced = (const uint32_t *)(const void *)fb;
  CHECK(enhanced[kExtra] == rgb555(31, 0, 0));
  CHECK(authentic[kExtra] == rgb555(0, 0, 31));
  CHECK(ppu->hScroll[kActRaiserPpuLayer_Bg2] == 0);

  /* Exact-coordinate availability must not decide camera ownership. Keep an
   * exact world sprite and an exact HUD sprite on the same scanline: only the
   * explicitly camera-relative world slot follows the authentic offset. */
  ppu_reset(ppu);
  memset(fb, 0, sizeof(fb));
  memset(authentic, 0, sizeof(authentic));
  for (int slot = 0; slot < 128; slot++)
    ppu->oam[slot * 2] = (uint16_t)(0x80 | (0xe0u << 8));
  ppu->inidisp = 0x0f;
  ppu->screenEnabled[0] = 1u << kPpuOverlaySource_Obj;
  ppu->cgram[0x81] = bgr555(31, 31, 0);
  set_solid_4bpp_tile(ppu, 0, 1);
  ppu->oam[0] = (uint16_t)(40 | (0u << 8));
  ppu->oam[1] = 0;
  ppu->oam[2] = (uint16_t)(100 | (0u << 8));
  ppu->oam[3] = 0;
  /* Thirty-one more live sprites force the native pass across its 32-sprite
   * hardware limit. The enhanced pass explicitly lifts that limit, so any
   * leaked range-over status below can only have come from the comparison
   * pass. */
  for (int slot = 2; slot < 33; slot++) {
    ppu->oam[slot * 2] = (uint16_t)(200 | (0u << 8));
    ppu->oam[slot * 2 + 1] = 0;
  }
  PpuSetObjExactPosition(ppu, 0, 40, 0);
  PpuSetObjExactPosition(ppu, 1, 100, 0);
  PpuSetObjCameraRelative(ppu, 0, true);
  PpuSetExtraSpace(ppu, kExtra);
  PpuBeginDrawing(
      ppu, fb, sizeof(fb), kPpuRenderFlags_NoSpriteLimits);
  CHECK(PpuBindAuthenticSurface(
      ppu, (uint8_t *)authentic, sizeof(authentic)));
  CHECK(PpuSetAuthenticCameraFrame(ppu, 0, NULL, NULL, 8));
  static const uint32_t mode7_sentinel = 0xff00ff00u;
  ppu->m7Override = (PpuMode7Override){
    .rgba = &mode7_sentinel,
    .width = 1,
    .height = 1,
    .canvasX1 = 1,
    .canvasY1 = 1,
    .wrap = 1,
  };
  const PpuMode7Override expected_m7_override = ppu->m7Override;
  uint8_t *const expected_overlay =
      ppu->overlayRenderBuffer[kPpuOverlaySource_Bg1];
  const uint32_t expected_overlay_pitch =
      ppu->overlayRenderPitch[kPpuOverlaySource_Bg1];
  render_first_line(ppu);
  enhanced = (const uint32_t *)(const void *)fb;
  CHECK(enhanced[kExtra + 40] == rgb555(31, 31, 0));
  CHECK(enhanced[kExtra + 100] == rgb555(31, 31, 0));
  CHECK(authentic[kExtra + 40] == 0);
  CHECK(authentic[kExtra + 48] == rgb555(31, 31, 0));
  CHECK(authentic[kExtra + 100] == rgb555(31, 31, 0));
  CHECK(ppu->extraLeftRight == kExtra);
  CHECK(ppu->extraLeftCur == kExtra && ppu->extraRightCur == kExtra);
  CHECK(ppu->renderFlags == kPpuRenderFlags_NoSpriteLimits);
  CHECK(!ppu->rangeOver && !ppu->timeOver);
  CHECK(!memcmp(
      &ppu->m7Override, &expected_m7_override,
      sizeof(expected_m7_override)));
  CHECK(ppu->overlayRenderBuffer[kPpuOverlaySource_Bg1] ==
        expected_overlay);
  CHECK(ppu->overlayRenderPitch[kPpuOverlaySource_Bg1] ==
        expected_overlay_pitch);

  /* A surface that was valid before margins widened must fail closed instead
   * of accepting writes past its 256-pixel pitch. Guard words prove both the
   * row and its neighbours remain untouched. */
  struct GuardedAuthenticRow {
    uint32_t before[kGuard];
    uint32_t pixels[kW];
    uint32_t after[kGuard];
  } guarded;
  memset(&guarded, 0x5a, sizeof(guarded));
  setup_virtual_bg(ppu, 0, fb, sizeof(fb));
  CHECK(PpuBindAuthenticSurface(
      ppu, (uint8_t *)guarded.pixels, sizeof(guarded.pixels)));
  CHECK(PpuAuthenticSurfaceReady(ppu));
  PpuSetExtraSpace(ppu, kExtra);
  CHECK(!PpuAuthenticSurfaceReady(ppu));
  render_first_line(ppu);
  for (size_t i = 0; i < sizeof(guarded) / sizeof(uint32_t); i++)
    CHECK(((const uint32_t *)(const void *)&guarded)[i] == 0x5a5a5a5au);
  CHECK(!PpuBindAuthenticSurface(
      ppu, (uint8_t *)guarded.pixels, sizeof(guarded.pixels)));

  g_new_ppu = saved_new_ppu;
  ppu_free(ppu);
}

int main(void) {
  TestWorldNavigationPartialBrightnessCapture();
  TestObjRangeRaster();
  TestObjRangeScanoutCapture();
  TestSemanticAtlasPacking();
  TestSim3DFlatComposition();
  TestSim3DFlatCompositionDemand();
  TestSim3DPlaneTextureUploadMask();
  TestSim3DRawObjCaptureFallbackContract();
  TestSim3DWidescreenHudCaptureHandoff();
  TestOverlayContentMetadata();
  TestDioramaFixedColorSubtractCapture();
  TestSubscreenOnlyOverlayCapture();
  TestFullAddSubscreenWinnerCapture();
  TestMainScreenWinnerMask();
  TestVerticalMarginLayerClip();
  TestVerticalMarginBottomLayerClip();
  TestVerticalMarginExactObj();
  TestLayerPresentationExtents();
  TestMovingEdgePoliciesInVerticalMargins();
  TestCapturedPaddingReachesBudget();
  TestAuthenticComparisonSurface();
  TestAuthenticCameraAndSurfaceContract();
  TestVirtualTilemapMargins();
  TestVirtualTilemapEffects();
  TestVirtualTilemapAuthenticParity();
  TestVirtualTilemapPresentationBandsPreserveFlatOutput();
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
