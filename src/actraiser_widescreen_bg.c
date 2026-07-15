/* Isolated widescreen background presentation repairs.
 *
 * This is deliberately separate from the experimental OAM port on
 * widescreen-sprites-wip/widescreen-bg. It reuses the original recompiled
 * $02:B825 column decoder to populate only BG1/BG2 tilemap VRAM margin cells;
 * $00:8C98/$00:8D68 and the original $02:B158/$02:B1AF streamers remain
 * untouched.  Fast vertical motion is covered separately with the original
 * $02:B8A0 row decoder, avoiding a full set of column rebuilds every 16px.
 *
 * Static audit of $02:B825->$02:B90D (2026-07-12): it reads level-map and
 * metatile data, writes the caller-supplied WRAM upload record, uses DP $0E,
 * and invokes the SNES multiply registers through $02:BED3. It does not touch
 * PPU/OAM/CGRAM registers. To make those temporary effects unobservable to
 * game logic, the refresh snapshots and restores the complete CpuState, all
 * 128 KiB of WRAM, and the SNES multiply/divide state. The only persistent
 * writes are the explicitly range-checked BG tilemap VRAM words below.
 *
 * Sky Palace uses a separate, state-free source path: decode its traced ROM
 * metatile page into only the offscreen BG2 columns used for presentation,
 * render, then restore the game's UI/staging tilemap verbatim. */

#include "actraiser_rtl.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "funcs.h"
#include "settings.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "widescreen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8 g_ram[0x20000];
extern Ppu *g_ppu;

/* Internal bank-02 routine used by the original $02:B158 streamer. It is
 * already emitted on main; declaring it here does not require a cfg change or
 * regeneration. */
RecompReturn bank_02_B825_M1X0(CpuState *cpu);
RecompReturn bank_02_B8A0_M1X0(CpuState *cpu);

static uint8 s_wram_snapshot[0x20000];

enum {
  kSkyPalaceBg2Base = 0x7000,
  kSkyPalaceBg2Words = 0x1000,
  kSkyPalaceSourceMap = 0x07D0A0,
};

static uint16 s_sky_palace_bg2_backup[kSkyPalaceBg2Words];
static int s_sky_palace_restore_pending;

/* The original streamers update a layer when the camera crosses a 16px map
 * boundary, not on every scanout.  Keep the host-only margin decoder on that
 * same cadence.  This is particularly important in rooms with two wide
 * layers: rebuilding every visible margin strip for BG1 and BG2 on every
 * rendered frame is enough to miss the host's 60Hz presentation deadline. */
typedef struct WsRefreshKey {
  uint8 mode, map, scene, act;
  uint16 margin_left, margin_right;
  uint16 layer[2][11];
} WsRefreshKey;

static WsRefreshKey s_refresh_key;
static int s_refresh_key_valid;
static uint16 s_row_y[2];

static inline uint16 ws_ram16(uint16 a) {
  return (uint16)(g_ram[a] | (g_ram[(uint16)(a + 1)] << 8));
}

int ActRaiser_WidescreenBgRefreshEnabled(void) {
  return g_settings.ws_bgrefresh;
}

static int ws_bg_debug_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("AR_WS_BGDBG");
    enabled = (e && e[0] && e[0] != '0');
  }
  return enabled;
}

static int ws_sky_palace_bg_enabled(void) {
  return g_settings.ws_skypalace_bg;
}

static uint16 ws_tilemap_addr_64(uint16 base, int tile_x, int tile_y) {
  tile_x &= 63;
  tile_y &= 63;
  return (uint16)(base + (tile_x & 31) + ((tile_y & 31) << 5) +
                  ((tile_x & 32) ? 0x400 : 0) +
                  ((tile_y & 32) ? 0x800 : 0));
}

void ActRaiser_WidescreenSkyPalaceRestore(void) {
  if (!s_sky_palace_restore_pending)
    return;
  if (g_ppu) {
    memcpy(&g_ppu->vram[kSkyPalaceBg2Base], s_sky_palace_bg2_backup,
           sizeof(s_sky_palace_bg2_backup));
  }
  s_sky_palace_restore_pending = 0;
}

/* Build a box-free Sky Palace BG2 only in tile columns sampled by the side
 * margins. The game's source-map setup at $02:B6F8-$B726 copies the 16x16
 * metatile page at ROM $07:D0A0 to WRAM $7E:C200. That page supplies the
 * authentic beam/capitals/shafts/floor. Its metatile rows 9-12 are the dialog
 * box itself; the underlying colonnade is the same shaft row already present
 * in rows 3-8, so use row 8 for those four obscured rows.
 *
 * We expand the source IDs through the game's live BG2 metatile table and
 * attribute/mask descriptors, exactly as $02:B90D does, but write only
 * offscreen VRAM columns. The center keeps the game's real BG2 bounding box.
 * The complete 64x64 BG2 ring is backed up and restored after scanout, making
 * the operation invisible to game logic and future UI composition. */
void ActRaiser_WidescreenSkyPalacePrepare(void) {
  ActRaiser_WidescreenSkyPalaceRestore();

  if (!ws_sky_palace_bg_enabled() || !g_ws_active || !g_ppu ||
      g_ram[0x18] != 0x00 || g_ram[0x19] != 0x07 ||
      (g_ppu->extraLeftCur == 0 && g_ppu->extraRightCur == 0)) {
    return;
  }

  uint16 map_base = (uint16)((g_ppu->bgXsc[1] & 0xfc) << 8);
  uint16 meta_base = ws_ram16(0x56);
  uint16 meta_mask = ws_ram16(0x58);
  uint16 meta_attr = (uint16)(g_ram[0x6F] << 8);
  const uint8 *source = RomFixedPtr(kSkyPalaceSourceMap);

  /* This source decoder is deliberately specific to the statically traced
   * Sky Palace layout. Reject unexpected descriptors/ROM revisions rather
   * than writing a plausible-looking but incorrect tilemap. */
  if (map_base != kSkyPalaceBg2Base ||
      (g_ppu->bgXsc[1] & 3) != 3 || meta_base > 0xF800 ||
      source[0] != 0x43 || source[16] != 0x2A ||
      source[32] != 0x32 || source[48] != 0x00) {
    if (ws_bg_debug_enabled()) {
      fprintf(stderr,
              "[ws-sky] source reject BG2SC=$%02X base=$%04X meta=$%04X "
              "header=%02X/%02X/%02X/%02X\n",
              g_ppu->bgXsc[1], map_base, meta_base, source[0], source[16],
              source[32], source[48]);
    }
    return;
  }

  uint8 margin_col[64] = {0};
  uint8 center_col[64] = {0};
  int hscroll = g_ppu->hScroll[1] & 0x1ff;
  for (int x = 0; x < 256; x++)
    center_col[((hscroll + x) & 0x1ff) >> 3] = 1;
  for (int x = -(int)g_ppu->extraLeftCur; x < 0; x++)
    margin_col[((hscroll + x) & 0x1ff) >> 3] = 1;
  for (int x = 256; x < 256 + (int)g_ppu->extraRightCur; x++)
    margin_col[((hscroll + x) & 0x1ff) >> 3] = 1;

  memcpy(s_sky_palace_bg2_backup, &g_ppu->vram[kSkyPalaceBg2Base],
         sizeof(s_sky_palace_bg2_backup));
  s_sky_palace_restore_pending = 1;

  int patched_columns = 0;
  for (int tile_x = 0; tile_x < 64; tile_x++) {
    /* A non-tile-aligned scroll can make one boundary tile serve center and
     * margin pixels. Preserve the center in that case; Sky Palace is normally
     * aligned at scroll zero, so no required column is skipped. */
    if (!margin_col[tile_x] || center_col[tile_x])
      continue;
    patched_columns++;
    int source_tile_x = tile_x & 31;  /* repeat the authentic 256px page */
    int meta_x = source_tile_x >> 1;
    for (int tile_y = 0; tile_y < 64; tile_y++) {
      int source_tile_y = tile_y & 31;
      int meta_y = source_tile_y >> 1;
      /* The 64x64 map is four quadrant canvases (2 x-pages x 2 y-bands); the
       * game picks one per UI state via h/v scroll (menu: vscroll~504 shows
       * the top band, dialogs: vscroll~248 the bottom band). Every canvas
       * shares one scene layout: page meta rows 0-8 (sky/beam/capitals/
       * shaft) and 13-15 (lower floor) apply directly, but the page's box
       * rows 9-12 overlap scene rows that the box merely covers:
       *  - meta cols 0/15 there hold the page-seam pillar-base halves
       *    (42/40 tops at sy 22-23, 4A/48 bottoms at sy 24-25) that
       *    complete the half-base visible at each center edge;
       *  - shaft columns get their base flare + floor reflection from the
       *    dedicated metatiles $41 (flare top; upper half = plain shaft)
       *    and $49 (flare bottom), flanked one metatile to each side by
       *    the curved skirt pieces $40/$48 (left) and $42/$4A (right) --
       *    the same metatiles the page seam uses, wrapped around a shaft.
       *    This art exists only in the metatile table, never in a page row
       *    (the page's box always covers it); recovered from the words the
       *    game's own boot colonnade left in the scratch columns;
       *  - the floor plane's top two rows sit under the box bottom, so
       *    sy 24-25 continue as floor (meta 13) at plain columns
       *    (validated against the live floor edge in both bands -- the
       *    page-native floor rows 13-15 only cover sy 26-31);
       *  - everything else continues the shaft row 8.
       * Layout validated cell-by-cell against the live map of both UI
       * states (2026-07-13). */
      int seam = (meta_x == 0 || meta_x == 15);
      int shaft_col = source[8 * 16 + meta_x] == 0x39;
      int left_flank = !seam && !shaft_col && meta_x < 15 &&
                       source[8 * 16 + meta_x + 1] == 0x39;
      int right_flank = !seam && !shaft_col && meta_x > 0 &&
                        source[8 * 16 + meta_x - 1] == 0x39;
      uint8 metatile;
      if (meta_y >= 11 && meta_y <= 12 && seam) {
        metatile = source[meta_y * 16 + meta_x];       /* seam base halves */
      } else if (meta_y >= 11 && meta_y <= 12 && shaft_col) {
        metatile = meta_y == 11 ? 0x41 : 0x49;         /* pillar base flare */
      } else if (meta_y >= 11 && meta_y <= 12 && left_flank) {
        metatile = meta_y == 11 ? 0x40 : 0x48;         /* skirt left of shaft */
      } else if (meta_y >= 11 && meta_y <= 12 && right_flank) {
        metatile = meta_y == 11 ? 0x42 : 0x4A;         /* skirt right of shaft */
      } else if (meta_y == 12) {
        metatile = source[13 * 16 + meta_x];           /* floor top rows */
      } else if (meta_y >= 9 && meta_y <= 12) {
        metatile = source[8 * 16 + meta_x];            /* shaft/empty */
      } else {
        metatile = source[meta_y * 16 + meta_x];
      }
      /* Definition words are stored row-major within the metatile:
       * TL, TR, BL, BR (validated against the authentic VRAM decode of the
       * same page — the x-major order transposes each 2x2 block, rendering
       * the split shaft metatiles as 8px checkerboards). */
      int quadrant = ((source_tile_y & 1) << 1) | (source_tile_x & 1);
      uint16 def = ws_ram16((uint16)(meta_base + metatile * 8 + quadrant * 2));
      uint16 word = (uint16)((def & meta_mask) | meta_attr);
      g_ppu->vram[ws_tilemap_addr_64(map_base, tile_x, tile_y)] = word;
    }
  }

  if (!patched_columns) {
    ActRaiser_WidescreenSkyPalaceRestore();
    return;
  }

  if (ws_bg_debug_enabled()) {
    static unsigned last_gf = ~0u;
    unsigned gf = (unsigned)(g_ram[0x88] | (g_ram[0x89] << 8));
    if (gf != last_gf) {
      last_gf = gf;
      fprintf(stderr,
              "[ws-sky] gf=%u source=$07:D0A0 meta=$7E:%04X cols=%d "
              "scroll=$%04X margins=%u/%u (render-only)\n",
              gf, meta_base, patched_columns, hscroll,
              g_ppu->extraLeftCur, g_ppu->extraRightCur);
    }
  }
}

/* Invoke the already-emitted recompiled builder with the same paired-call
 * stack protocol as a generated JSR site. Complete WRAM/CPU restoration at
 * the outer boundary removes the synthetic return frame and all scratch. */
static int ws_call_builder(CpuState *cpu,
                           RecompReturn (*builder)(CpuState *),
                           uint16 jsr_return) {
  uint16 s = cpu->S;
  cpu_write8(cpu, 0x00, cpu->S, (uint8)(jsr_return >> 8));
  cpu->S = (uint16)(cpu->S - 1);
  cpu_write8(cpu, 0x00, cpu->S, (uint8)jsr_return);
  cpu->S = (uint16)(cpu->S - 1);
  cpu->host_return_valid = 1;
  RecompReturn r = builder(cpu);
  cpu->S = s;
  return r == RECOMP_RETURN_NORMAL;
}

static int ws_call_b825(CpuState *cpu) {
  /* $02:B1A9 JSR $B825 leaves $B1AB on the emulated stack. */
  return ws_call_builder(cpu, bank_02_B825_M1X0, 0xB1AB);
}

static int ws_call_b8a0(CpuState *cpu) {
  /* $02:B1F1 JSR $B8A0 leaves $B1F3 on the emulated stack. */
  return ws_call_builder(cpu, bank_02_B8A0_M1X0, 0xB1F3);
}

/* Drain a freshly built two-column record exactly like $02:ACE5, but only
 * after proving every target stays within this layer's 4 KiB tilemap. */
static int ws_drain_column_record(uint16 buf, uint16 map_base) {
  uint8 *rec = &g_ram[buf];
  uint16 base = (uint16)(rec[0] | (rec[1] << 8));
  static const uint16 chunk_vram[4] = { 0x0000, 0x0001, 0x0800, 0x0801 };
  static const uint16 chunk_word[4] = { 1, 0x21, 0x41, 0x61 };

  if (base == 0)
    return 0;

  /* Highest target = base + $801 + 31*$20. This per-layer check is stricter
   * than the experimental branch's global $6000-$7FFF check: a bad BG1
   * record cannot reach BG2, and neither layer can reach OBJ chars. */
  uint32 hi = (uint32)base + 0x0801u + 31u * 0x20u;
  if (base < map_base || hi >= (uint32)map_base + 0x1000u) {
    if (ws_bg_debug_enabled()) {
      unsigned gf = (unsigned)(g_ram[0x88] | (g_ram[0x89] << 8));
      fprintf(stderr,
              "[ws-bg] gf=%u reject buf=$%04X map=$%04X base=$%04X hi=$%04X\n",
              gf, buf, map_base, base, (unsigned)hi);
    }
    return 0;
  }

  for (int c = 0; c < 4; c++) {
    uint16 addr = (uint16)(base + chunk_vram[c]);
    const uint8 *p = rec + 2 * chunk_word[c];
    for (int k = 0; k < 32; k++) {
      g_ppu->vram[addr] = (uint16)(p[0] | (p[1] << 8));
      p += 2;
      addr = (uint16)(addr + 0x20);
    }
  }
  return 1;
}

/* Drain a freshly built two-row record exactly like the row half of
 * $02:ACE5. VMAIN=$80 makes each 64-byte chunk advance horizontally. */
static int ws_drain_row_record(uint16 buf, uint16 map_base) {
  uint8 *rec = &g_ram[buf];
  uint16 base = (uint16)(rec[0] | (rec[1] << 8));
  static const uint16 chunk_vram[4] = { 0x0000, 0x0020, 0x0400, 0x0420 };
  static const uint16 chunk_word[4] = { 1, 0x21, 0x41, 0x61 };

  if (base == 0)
    return 0;

  uint32 hi = (uint32)base + 0x0420u + 31u;
  if (base < map_base || hi >= (uint32)map_base + 0x1000u) {
    if (ws_bg_debug_enabled()) {
      unsigned gf = (unsigned)(g_ram[0x88] | (g_ram[0x89] << 8));
      fprintf(stderr,
              "[ws-bg] gf=%u reject row buf=$%04X map=$%04X base=$%04X hi=$%04X\n",
              gf, buf, map_base, base, (unsigned)hi);
    }
    return 0;
  }

  for (int c = 0; c < 4; c++) {
    uint16 addr = (uint16)(base + chunk_vram[c]);
    const uint8 *p = rec + 2 * chunk_word[c];
    for (int k = 0; k < 32; k++) {
      g_ppu->vram[addr++] = (uint16)(p[0] | (p[1] << 8));
      p += 2;
    }
  }
  return 1;
}

/* A neighboring 512px row band aliases half of the same 64-column ring. Do
 * not drain that whole record: doing so would fix one margin by replacing the
 * opposite visible half with world data 512px away. Copy only tile columns
 * intersecting the out-of-band margin. */
static int ws_drain_row_record_range(uint16 buf, uint16 map_base,
                                     int world_left, int world_right) {
  uint8 *rec = &g_ram[buf];
  uint16 base = (uint16)(rec[0] | (rec[1] << 8));
  uint32 hi = (uint32)base + 0x0420u + 31u;

  if (base == 0 || world_right <= world_left)
    return 0;
  if (base < map_base || hi >= (uint32)map_base + 0x1000u) {
    if (ws_bg_debug_enabled()) {
      unsigned gf = (unsigned)(g_ram[0x88] | (g_ram[0x89] << 8));
      fprintf(stderr,
              "[ws-bg] gf=%u reject partial row buf=$%04X map=$%04X base=$%04X hi=$%04X\n",
              gf, buf, map_base, base, (unsigned)hi);
    }
    return 0;
  }

  int first_tile = world_left >> 3;
  int last_tile = (world_right + 7) >> 3;
  for (int tile = first_tile; tile < last_tile; tile++) {
    int col = tile & 0x3F;
    int half_col = col & 0x1F;
    uint16 top_addr = (uint16)(base + half_col + (col >= 32 ? 0x400 : 0));
    uint16 bottom_addr = (uint16)(top_addr + 0x20);
    int top_off = 2 + half_col * 2 + (col >= 32 ? 0x80 : 0);
    int bottom_off = top_off + 0x40;
    g_ppu->vram[top_addr] =
        (uint16)(rec[top_off] | (rec[top_off + 1] << 8));
    g_ppu->vram[bottom_addr] =
        (uint16)(rec[bottom_off] | (rec[bottom_off + 1] << 8));
  }
  return 1;
}

/* Marshal one 16px-wide world strip as $02:B158 does, ask the original
 * decoder to fill its fixed record buffer, then copy that record to VRAM. */
static int ws_build_strip(CpuState *cpu, uint16 layer_x, uint16 world_x,
                          uint16 world_y) {
  const uint16 expected_buf = layer_x == 0 ? 0x3900 : 0x3B04;
  const uint16 map_base = layer_x == 0 ? 0x6000 : 0x7000;

  g_ram[0x14] = (uint8)world_x;
  g_ram[0x15] = (uint8)(world_x >> 8);
  g_ram[0x16] = (uint8)world_y;
  g_ram[0x17] = (uint8)(world_y >> 8);

  uint16 v = ws_ram16((uint16)(0x48 + layer_x));
  g_ram[0x00] = (uint8)v; g_ram[0x01] = (uint8)(v >> 8);
  v = ws_ram16((uint16)(0x46 + layer_x));
  g_ram[0x02] = (uint8)v; g_ram[0x03] = (uint8)(v >> 8);
  v = ws_ram16((uint16)(0x2E + layer_x));
  g_ram[0x04] = (uint8)v; g_ram[0x05] = (uint8)(v >> 8);
  g_ram[0x06] = 0;
  g_ram[0x07] = g_ram[(uint16)(0x6B + layer_x)];

  /* $B158's first $BED3 call multiplies the two bytes assembled from
   * $2F/$31. Compute it directly so only $B825's internal multiply touches
   * the emulated math unit (which the outer snapshot restores). */
  uint16 product = (uint16)(g_ram[(uint16)(0x2F + layer_x)] *
                            g_ram[(uint16)(0x31 + layer_x)]);
  g_ram[0x08] = 0;
  g_ram[0x09] = (uint8)(product + g_ram[(uint16)(0x47 + layer_x)]);
  v = ws_ram16((uint16)(0x54 + layer_x));
  g_ram[0x0A] = (uint8)v; g_ram[0x0B] = (uint8)(v >> 8);
  v = ws_ram16((uint16)(0x52 + layer_x));
  g_ram[0xA5] = (uint8)v; g_ram[0xA6] = (uint8)(v >> 8);

  uint16 buf = ws_ram16((uint16)(0x5E + layer_x));
  if (buf != expected_buf) {
    if (ws_bg_debug_enabled()) {
      unsigned gf = (unsigned)(g_ram[0x88] | (g_ram[0x89] << 8));
      fprintf(stderr, "[ws-bg] gf=%u reject layerX=%u cursor=$%04X expected=$%04X\n",
              gf, layer_x, buf, expected_buf);
    }
    return 0;
  }

  cpu->A = product;
  cpu->X = buf;
  cpu->Y = buf;
  if (!ws_call_b825(cpu))
    return 0;
  return ws_drain_column_record(buf, map_base);
}

/* Marshal one 16px-tall world strip as $02:B1AF does. world_x must remain
 * 256px-aligned: $B8A0's level-map fetch is page-keyed. */
static int ws_build_row(CpuState *cpu, uint16 layer_x, uint16 world_x,
                        uint16 world_y, int range_left, int range_right) {
  const uint16 expected_buf = layer_x == 0 ? 0x3A02 : 0x3C06;
  const uint16 map_base = layer_x == 0 ? 0x6000 : 0x7000;

  g_ram[0x14] = (uint8)world_x;
  g_ram[0x15] = (uint8)(world_x >> 8);
  g_ram[0x16] = (uint8)world_y;
  g_ram[0x17] = (uint8)(world_y >> 8);

  uint16 v = ws_ram16((uint16)(0x48 + layer_x));
  g_ram[0x00] = (uint8)v; g_ram[0x01] = (uint8)(v >> 8);
  v = ws_ram16((uint16)(0x46 + layer_x));
  g_ram[0x02] = (uint8)v; g_ram[0x03] = (uint8)(v >> 8);
  v = ws_ram16((uint16)(0x2E + layer_x));
  g_ram[0x04] = (uint8)v; g_ram[0x05] = (uint8)(v >> 8);
  g_ram[0x06] = 0;
  g_ram[0x07] = g_ram[(uint16)(0x6B + layer_x)];
  v = ws_ram16((uint16)(0x54 + layer_x));
  g_ram[0x0A] = (uint8)v; g_ram[0x0B] = (uint8)(v >> 8);
  v = ws_ram16((uint16)(0x52 + layer_x));
  g_ram[0xA5] = (uint8)v; g_ram[0xA6] = (uint8)(v >> 8);

  uint16 buf = ws_ram16((uint16)(0x60 + layer_x));
  if (buf != expected_buf) {
    if (ws_bg_debug_enabled()) {
      unsigned gf = (unsigned)(g_ram[0x88] | (g_ram[0x89] << 8));
      fprintf(stderr,
              "[ws-bg] gf=%u reject row layerX=%u cursor=$%04X expected=$%04X\n",
              gf, layer_x, buf, expected_buf);
    }
    return 0;
  }

  cpu->X = buf;
  cpu->Y = buf;
  if (!ws_call_b8a0(cpu))
    return 0;
  if (range_left >= 0)
    return ws_drain_row_record_range(buf, map_base,
                                     range_left, range_right);
  return ws_drain_row_record(buf, map_base);
}

/* Build the authentic page-aligned row band plus a neighboring band when a
 * wide margin falls outside its 512px span. This directly addresses the
 * phase hole documented in rendering-engine.md without changing $B8A0. */
static void ws_build_visible_row(CpuState *cpu, uint16 layer_x,
                                 uint16 world_y, int *built, int *requested) {
  uint16 cam = ws_ram16((uint16)(0x22 + layer_x));
  uint16 width = ws_ram16((uint16)(0x2E + layer_x));
  uint16 base = (uint16)(cam & 0xFF00);
  int view_left = (int)cam - (int)g_ppu->extraLeftCur;
  int view_right = (int)cam + 0x100 + (int)g_ppu->extraRightCur;

  (*requested)++;
  *built += ws_build_row(cpu, layer_x, base, world_y, -1, -1);

  if (view_left < (int)base && base >= 0x100) {
    (*requested)++;
    *built += ws_build_row(cpu, layer_x, (uint16)(base - 0x100), world_y,
                           view_left, base);
  }
  if (view_right > (int)base + 0x200 && (uint32)base + 0x100u < width) {
    (*requested)++;
    *built += ws_build_row(cpu, layer_x, (uint16)(base + 0x100), world_y,
                           (int)base + 0x200, view_right);
  }
}

void ActRaiser_WidescreenMarginRefresh(void) {
  if (!ActRaiser_WidescreenBgRefreshEnabled() ||
      !g_ws_active || !g_ppu || !g_snes) {
    s_refresh_key_valid = 0;
    return;
  }
  if (g_ram[0x18] < 0x01 || g_ram[0x18] > 0x07) {
    s_refresh_key_valid = 0;
    return;
  }
  /* Death Heim $07:$01 presents both BG1 and BG2 from their authentic center;
   * only BG2's lower fog band is repeated by the renderer. No margin tilemap
   * cells are sampled, so running the world-strip transaction is unnecessary. */
  if (g_ram[0x18] == 0x07 && g_ram[0x19] == 0x01) {
    s_refresh_key_valid = 0;
    return;
  }
  if (g_ppu->extraLeftCur == 0 && g_ppu->extraRightCur == 0) {
    s_refresh_key_valid = 0;
    return;
  }

  WsRefreshKey key;
  memset(&key, 0, sizeof(key));
  key.mode = g_ram[0x18];
  key.map = g_ram[0x19];
  key.scene = g_ram[0x1A];
  key.act = g_ram[0x1B];
  key.margin_left = g_ppu->extraLeftCur;
  key.margin_right = g_ppu->extraRightCur;
  for (int layer = 0; layer < 2; layer++) {
    uint16 x = (uint16)(layer * 4);
    key.layer[layer][0] = (uint16)(ws_ram16((uint16)(0x22 + x)) & 0xFFF0);
    key.layer[layer][1] = (uint16)(ws_ram16((uint16)(0x24 + x)) & 0xFF00);
    key.layer[layer][2] = ws_ram16((uint16)(0x2E + x));
    key.layer[layer][3] = ws_ram16((uint16)(0x30 + x));
    key.layer[layer][4] = ws_ram16((uint16)(0x46 + x));
    key.layer[layer][5] = ws_ram16((uint16)(0x48 + x));
    key.layer[layer][6] = ws_ram16((uint16)(0x52 + x));
    key.layer[layer][7] = ws_ram16((uint16)(0x54 + x));
    key.layer[layer][8] = ws_ram16((uint16)(0x5E + x));
    key.layer[layer][9] = (uint16)(g_ram[(uint16)(0x47 + x)] |
                                   (g_ram[(uint16)(0x6B + x)] << 8));
    key.layer[layer][10] = (uint16)(g_ram[(uint16)(0x2F + x)] |
                                    (g_ram[(uint16)(0x31 + x)] << 8));
  }
  int rebuild_columns = !s_refresh_key_valid ||
      memcmp(&key, &s_refresh_key, sizeof(key)) != 0;
  uint16 row_y[2];
  int rebuild_rows = 0;
  for (int layer = 0; layer < 2; layer++) {
    uint16 x = (uint16)(layer * 4);
    row_y[layer] = (uint16)(ws_ram16((uint16)(0x24 + x)) & 0xFFF0);
    if (!rebuild_columns && row_y[layer] != s_row_y[layer] &&
        ws_ram16((uint16)(0x2E + x)) >= 0x200 &&
        g_ram[(uint16)(0x31 + x)] >= 3)
      rebuild_rows = 1;
  }
  if (!rebuild_columns && !rebuild_rows)
    return;

  CpuState cpu_snapshot = g_cpu;
  uint8 multiply_a = g_snes->multiplyA;
  uint16 multiply_result = g_snes->multiplyResult;
  uint16 divide_a = g_snes->divideA;
  uint16 divide_result = g_snes->divideResult;
  memcpy(s_wram_snapshot, g_ram, sizeof(s_wram_snapshot));

  CpuState *cpu = &g_cpu;
  cpu->D = 0;
  cpu->m_flag = 1;
  cpu->x_flag = 0;
  cpu->P = (uint8)((cpu->P & ~0x30) | 0x20);
  cpu->host_return_valid = 0;

  int built_columns = 0, requested_columns = 0;
  int built_rows = 0, requested_rows = 0;
  if (rebuild_columns) {
    for (int layer = 0; layer < 2; layer++) {
      uint16 layer_x = (uint16)(layer * 4);
      uint16 cam = ws_ram16((uint16)(0x22 + layer_x));
      uint16 width = ws_ram16((uint16)(0x2E + layer_x));
      if (width < 0x200)
        continue;

      uint16 world_y = (uint16)(ws_ram16((uint16)(0x24 + layer_x)) & 0xFF00);
      uint16 left_col = (uint16)(cam & 0xFFF0);
      uint16 right_col = (uint16)((cam + 0x100) & 0xFFF0);
      int left_strips = g_ppu->extraLeftCur
          ? (g_ppu->extraLeftCur + 15) / 16 + 1 : 0;
      int right_strips = g_ppu->extraRightCur
          ? (g_ppu->extraRightCur + 15) / 16 + 1 : 0;

      for (int m = 1; m <= left_strips; m++) {
        int wx = (int)left_col - m * 16;
        if (wx >= 0 && wx < (int)width) {
          requested_columns++;
          built_columns += ws_build_strip(cpu, layer_x, (uint16)wx, world_y);
        }
      }
      for (int m = 0; m <= right_strips; m++) {
        int wx = (int)right_col + m * 16;
        if (wx >= 0 && wx < (int)width) {
          requested_columns++;
          built_columns += ws_build_strip(cpu, layer_x, (uint16)wx, world_y);
        }
      }
    }
  } else {
    for (int layer = 0; layer < 2; layer++) {
      uint16 layer_x = (uint16)(layer * 4);
      if (row_y[layer] == s_row_y[layer] ||
          ws_ram16((uint16)(0x2E + layer_x)) < 0x200 ||
          g_ram[(uint16)(0x31 + layer_x)] < 3)
        continue;

      if (row_y[layer] > s_row_y[layer]) {
        for (int y = (int)s_row_y[layer] + 0x10;
             y <= (int)row_y[layer]; y += 0x10)
          ws_build_visible_row(cpu, layer_x, (uint16)(y + 0x100),
                               &built_rows, &requested_rows);
      } else {
        for (int y = (int)s_row_y[layer] - 0x10;; y -= 0x10) {
          ws_build_visible_row(cpu, layer_x, (uint16)y,
                               &built_rows, &requested_rows);
          if (y == (int)row_y[layer])
            break;
        }
      }
    }
  }

  /* Transaction boundary: discard every game-visible scratch write. */
  memcpy(g_ram, s_wram_snapshot, sizeof(s_wram_snapshot));
  g_cpu = cpu_snapshot;
  g_snes->multiplyA = multiply_a;
  g_snes->multiplyResult = multiply_result;
  g_snes->divideA = divide_a;
  g_snes->divideResult = divide_result;

  /* A rejected record may be transient during a room load.  Do not cache the
   * partial result; retry it on the next rendered frame. */
  if (built_columns == requested_columns && built_rows == requested_rows) {
    if (rebuild_columns) {
      s_refresh_key = key;
      s_refresh_key_valid = 1;
    }
    s_row_y[0] = row_y[0];
    s_row_y[1] = row_y[1];
  } else {
    s_refresh_key_valid = 0;
  }

  if (ws_bg_debug_enabled()) {
    static unsigned last_gf = ~0u;
    unsigned gf = (unsigned)(g_ram[0x88] | (g_ram[0x89] << 8));
    if (gf != last_gf) {
      last_gf = gf;
      fprintf(stderr,
              "[ws-bg] gf=%u strips=%d/%d rows=%d/%d margins=%u/%u (state restored)\n",
              gf, built_columns, requested_columns, built_rows, requested_rows,
              g_ppu->extraLeftCur, g_ppu->extraRightCur);
    }
  }
}
