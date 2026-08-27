/* ppu_exact_pos_test.c — pins the equivalence that makes Phase 1 byte-identical.
 *
 * The exact-position sideband bypasses the OAM modular decode (9-bit X with the
 * extraRightCur-relative wrap, 8-bit Y with the 224 threshold). Phase 1's
 * byte-identity claim rests on: for every position the emitters produce TODAY
 * (inside the display cap), the exact value and the decoded value agree, so
 * routing PpuObjScreenX/Y through the sideband cannot move a pixel. This test
 * walks that whole window — explicitly INCLUDING the left-straddle negatives
 * and above-screen Ys, the historically load-bearing wrap cases — and asserts
 * bounds computed from bytes alone equal bounds computed from the sideband.
 *
 * It then pins the divergence point: one step past the wrap threshold the two
 * MUST differ (the decode wraps, the exact value does not), because that
 * divergence is the entire reason the sideband exists.
 *
 * Pure PPU: no SDL, no ROM.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes/ppu.h"
#include "runner_internal.h"

/* Externs the PPU references; only fire under instrumentation. */
bool g_new_ppu = true;
int sr_trace_active(void) { return 0; }
void sr_trace_ppumem(uint16_t a, uint8_t v) { (void)a; (void)v; }
void sr_trace_reg(uint16_t a, uint8_t v) { (void)a; (void)v; }
void sr_trace_vmadd(uint16_t a) { (void)a; }
void sr_trace_vram(uint16_t a, uint16_t v) { (void)a; (void)v; }
void sr_vram_trace_raw(uint16_t a, uint8_t v, int p) { (void)a; (void)v; (void)p; }
int sr_vram_watch(uint16_t a, uint8_t v) { (void)a; (void)v; return 0; }
void debug_server_on_oam_render(void) {}
void CpuDispatchLogWriteFile(const char *path) { (void)path; }
unsigned g_sr_block_index;
uint32_t g_sr_block_ring[256];
const char *g_last_recomp_func;
uint8_t g_ram[0x20000];
int snes_frame_counter;
SrEventMask g_sr_runner_event_mask;
void sr_runner_emit_ppu_memory_write(Ppu *ppu, SrMemoryRegion region,
                                     uint32_t address,
                                     uint32_t previous_value,
                                     uint32_t value,
                                     uint32_t width_bytes) {
  (void)ppu;
  (void)region;
  (void)address;
  (void)previous_value;
  (void)value;
  (void)width_bytes;
}

static int g_failures;

#define CHECK(cond, ...)                                        \
  do {                                                          \
    if (!(cond)) {                                              \
      g_failures++;                                             \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
      fprintf(stderr, __VA_ARGS__);                             \
      fprintf(stderr, "\n");                                    \
    }                                                           \
  } while (0)

/* Write slot 0's OAM exactly as an emitter stores a screen position:
 * X = 9-bit value (byte + high-table bit), Y = 8-bit byte, both modular. */
static void write_slot0(Ppu *ppu, int screen_x, int screen_y) {
  uint16_t x9 = (uint16_t)(screen_x & 0x1FF);
  uint16_t yb = (uint16_t)(screen_y & 0xFF);
  ppu->oam[0] = (uint16_t)((x9 & 0xFF) | (yb << 8));
  ppu->oam[1] = 0; /* tile 0, palette 0, priority 0, no flips */
  ppu->highOam[0] = (uint8_t)((x9 >> 8) & 1); /* bit 0 = X bit 8; small size */
}

int main(void) {
  Ppu *ppu = ppu_init();
  ppu_reset(ppu);
  /* The display cap the wrap threshold anchors to: 120 per side (kWsExtraMax's
   * value, restated because widescreen.h is a runner header the PPU must not
   * depend on). PpuSetExtraSpace clamps to kPpuExtraLeftRight. */
  enum { kCap = 120 };
  PpuSetExtraSpace(ppu, kCap);

  /* 8x8 sprites (obsel size 0, high size bit clear). */
  ppu->obsel = 0;

  int checked = 0;
  for (int sx = -63; sx < 256 + kCap; sx++) {
    for (int sy = -32; sy < 224; sy += 7) { /* stride keeps runtime sane */
      write_slot0(ppu, sx, sy);

      PpuClearObjExactPositions(ppu);
      PpuObjRangeBounds decoded;
      bool got_decoded = PpuGetObjRangeBounds(ppu, 0, 1, 0, &decoded);

      PpuSetObjExactPosition(ppu, 0, sx, sy);
      PpuObjRangeBounds exact;
      bool got_exact = PpuGetObjRangeBounds(ppu, 0, 1, 0, &exact);

      CHECK(got_decoded && got_exact, "bounds failed at (%d,%d)", sx, sy);
      if (got_decoded && got_exact) {
        CHECK(decoded.x0 == exact.x0 && decoded.y0 == exact.y0 &&
                  decoded.x1 == exact.x1 && decoded.y1 == exact.y1,
              "exact != decoded at (%d,%d): byte (%d,%d) vs exact (%d,%d)",
              sx, sy, decoded.x0, decoded.y0, exact.x0, exact.y0);
      }
      checked++;
    }
  }
  fprintf(stderr, "equivalence: %d positions checked\n", checked);

  /* Divergence point: one step past the wrap threshold the byte decode wraps
   * to the far side while the exact value stays put. If this ever stops
   * differing, the threshold moved and the in-cap sweep above no longer covers
   * the emitter's range. */
  {
    int sx = 256 + kCap; /* first unrepresentable right-margin position */
    write_slot0(ppu, sx, 10);
    PpuClearObjExactPositions(ppu);
    PpuObjRangeBounds decoded;
    CHECK(PpuGetObjRangeBounds(ppu, 0, 1, 0, &decoded), "decode past cap");
    CHECK(decoded.x0 == sx - kPpuObjXWrap,
          "expected the wrap at the threshold, got x0=%d", decoded.x0);
    PpuSetObjExactPosition(ppu, 0, sx, 10);
    PpuObjRangeBounds exact;
    CHECK(PpuGetObjRangeBounds(ppu, 0, 1, 0, &exact), "exact past cap");
    CHECK(exact.x0 == sx, "exact position lost past the cap: x0=%d", exact.x0);
  }

  /* Explicit-part entry points agree with the OAM-backed wrappers, including
   * the paint result — the property that lets the apron rasterize synthetic
   * parts indistinguishably from OAM-backed ones. Two overlapping 8x8 sprites
   * exercise the earlier-owns-the-pixel rule. */
  {
    /* Give the tiles some non-zero pixels: plane 0 all-ones for tile 0. */
    for (int row = 0; row < 8; row++)
      ppu->vram[row] = 0x00FF;
    for (int c = 0; c < 16; c++)
      ppu->cgram[0x80 + c] = (uint16_t)(0x1F + c); /* arbitrary colors */

    ppu->oam[0] = (uint16_t)((20 & 0xFF) | (30 << 8));
    ppu->oam[1] = 0;
    ppu->oam[2] = (uint16_t)((24 & 0xFF) | (34 << 8));
    ppu->oam[3] = 0;
    ppu->highOam[0] = 0;
    PpuClearObjExactPositions(ppu);

    PpuObjRangeBounds bounds;
    CHECK(PpuGetObjRangeBounds(ppu, 0, 2, 0, &bounds), "pair bounds");
    int w = bounds.x1 - bounds.x0, h = bounds.y1 - bounds.y0;
    uint32_t via_range[32 * 32], via_parts[32 * 32];
    CHECK(w * h <= 32 * 32, "unexpected pair bounds %dx%d", w, h);
    CHECK(PpuRasterizeObjRange(ppu, 0, 2, 0, &bounds, via_range, w, h,
                               (size_t)w * 4),
          "range rasterize");
    PpuObjPart parts[2];
    int n = 0;
    CHECK(PpuResolveObjSlots(ppu, 0, 2, 0, parts, 2, &n) && n == 2,
          "resolve pair");
    CHECK(PpuRasterizeParts(ppu, parts, n, &bounds, via_parts, w, h,
                            (size_t)w * 4),
          "part rasterize");
    CHECK(memcmp(via_range, via_parts, (size_t)w * h * 4) == 0,
          "range and explicit-part rasterization diverged");
  }

  if (g_failures) {
    fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  fprintf(stderr, "ppu_exact_pos_test: all checks passed\n");
  return 0;
}
