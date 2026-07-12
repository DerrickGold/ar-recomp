/* Isolated action-stage widescreen background refresh.
 *
 * This is deliberately separate from the experimental OAM port on
 * widescreen-sprites-wip/widescreen-bg. It reuses the original recompiled
 * $02:B825 column decoder to populate only BG1/BG2 tilemap VRAM margin cells;
 * $00:8C98/$00:8D68 and the original $02:B158/$02:B1AF streamers remain
 * untouched.
 *
 * Static audit of $02:B825->$02:B90D (2026-07-12): it reads level-map and
 * metatile data, writes the caller-supplied WRAM upload record, uses DP $0E,
 * and invokes the SNES multiply registers through $02:BED3. It does not touch
 * PPU/OAM/CGRAM registers. To make those temporary effects unobservable to
 * game logic, the refresh snapshots and restores the complete CpuState, all
 * 128 KiB of WRAM, and the SNES multiply/divide state. The only persistent
 * writes are the explicitly range-checked BG tilemap VRAM words below. */

#include "actraiser_rtl.h"
#include "cpu_state.h"
#include "funcs.h"
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

static uint8 s_wram_snapshot[0x20000];

static inline uint16 ws_ram16(uint16 a) {
  return (uint16)(g_ram[a] | (g_ram[(uint16)(a + 1)] << 8));
}

int ActRaiser_WidescreenBgRefreshEnabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("AR_WS_BGREFRESH");
    enabled = !(e && e[0] == '0');
  }
  return enabled;
}

static int ws_bg_debug_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("AR_WS_BGDBG");
    enabled = (e && e[0] && e[0] != '0');
  }
  return enabled;
}

/* Invoke the already-emitted recompiled builder with the same paired-call
 * stack protocol as a generated JSR site. Complete WRAM/CPU restoration at
 * the outer boundary removes the synthetic return frame and all scratch. */
static int ws_call_b825(CpuState *cpu) {
  uint16 s = cpu->S;
  cpu_write8(cpu, 0x00, cpu->S, 0xb1);
  cpu->S = (uint16)(cpu->S - 1);
  cpu_write8(cpu, 0x00, cpu->S, 0xab);
  cpu->S = (uint16)(cpu->S - 1);
  cpu->host_return_valid = 1;
  RecompReturn r = bank_02_B825_M1X0(cpu);
  cpu->S = s;
  return r == RECOMP_RETURN_NORMAL;
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

void ActRaiser_WidescreenMarginRefresh(void) {
  if (!ActRaiser_WidescreenBgRefreshEnabled() ||
      !g_ws_active || !g_ppu || !g_snes)
    return;
  if (g_ram[0x18] < 0x01 || g_ram[0x18] > 0x07)
    return;
  if (g_ppu->extraLeftCur == 0 && g_ppu->extraRightCur == 0)
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

  int built = 0;
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
      if (wx >= 0 && wx < (int)width)
        built += ws_build_strip(cpu, layer_x, (uint16)wx, world_y);
    }
    for (int m = 0; m <= right_strips; m++) {
      int wx = (int)right_col + m * 16;
      if (wx >= 0 && wx < (int)width)
        built += ws_build_strip(cpu, layer_x, (uint16)wx, world_y);
    }
  }

  /* Transaction boundary: discard every game-visible scratch write. */
  memcpy(g_ram, s_wram_snapshot, sizeof(s_wram_snapshot));
  g_cpu = cpu_snapshot;
  g_snes->multiplyA = multiply_a;
  g_snes->multiplyResult = multiply_result;
  g_snes->divideA = divide_a;
  g_snes->divideResult = divide_result;

  if (ws_bg_debug_enabled()) {
    static unsigned last_gf = ~0u;
    unsigned gf = (unsigned)(g_ram[0x88] | (g_ram[0x89] << 8));
    if (gf != last_gf) {
      last_gf = gf;
      fprintf(stderr, "[ws-bg] gf=%u strips=%d margins=%u/%u (state restored)\n",
              gf, built, g_ppu->extraLeftCur, g_ppu->extraRightCur);
    }
  }
}
