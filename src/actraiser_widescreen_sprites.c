/* Staged action sprite handling for widescreen.
 *
 * $00:8D68 widens per-definition emission. The audited $00:8C98 replacement
 * independently controls initialized margin-object drawing (Stage D1) and
 * the $0400 activation boundary (Stage D2). Keeping those decisions separate
 * is essential: a fidelity run can disable either without replacing binaries.
 *
 * AR_WS_SPRITES=0 restores authentic per-definition emission.
 * AR_WS_MARGIN_OBJECTS=0 restores authentic object draw coverage.
 * AR_WS_MARGIN_ACTIVATION=0 restores the authentic $0400 boundary; Stage D2
 * is enabled by default after direct Fillmore validation. */

#include "cpu_state.h"
#include "actraiser_rtl.h"
#include "snes/ppu.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

extern uint8 g_ram[0x20000];
extern Ppu *g_ppu;
extern RecompReturn bank_00_923A_M0X0(CpuState *cpu);

RecompReturn ActRaiser_BuildObjectSprites(CpuState *cpu);

static inline uint16 ws_dp16(CpuState *cpu, uint16 off) {
  uint16 a = (uint16)(cpu->D + off);
  return (uint16)(g_ram[a] | (g_ram[(uint16)(a + 1)] << 8));
}

static inline void ws_dp16w(CpuState *cpu, uint16 off, uint16 v) {
  uint16 a = (uint16)(cpu->D + off);
  g_ram[a] = (uint8)v;
  g_ram[(uint16)(a + 1)] = (uint8)(v >> 8);
}

static int ws_sprite_widen_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("AR_WS_SPRITES");
    enabled = !(e && e[0] == '0');
  }
  return enabled;
}

static int ws_sprite_debug_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("AR_WS_SPRDBG");
    enabled = (e && e[0] && e[0] != '0');
  }
  return enabled;
}

typedef struct WsActivationCandidate {
  uint8 present;
  uint8 defbank;
  uint16 status;
  uint16 flags;
  uint16 handler;
  uint16 def;
  uint16 type;
} WsActivationCandidate;

static inline uint16 ws_ram16(uint16 a) {
  return (uint16)(g_ram[a] | (g_ram[(uint16)(a + 1)] << 8));
}

/* Match $8C98's bounding-box interpretation without touching $0400. The
 * object's leading/trailing extents are unsigned distances from its origin;
 * subtraction is interpreted in the same 16-bit signed screen space used by
 * the ROM's wrap-aware two-edge tests. */
static int ws_axis_visible(uint16 pos, uint16 leading, uint16 trailing,
                           uint16 camera, int window_lo, int window_hi,
                           int *screen_lo, int *screen_hi) {
  int lo = (int)(int16_t)(uint16)(pos - leading - camera);
  int hi = (int)(int16_t)(uint16)(pos + trailing - camera);
  if (screen_lo) *screen_lo = lo;
  if (screen_hi) *screen_hi = hi;
  return lo < window_hi && hi >= window_lo;
}

/* AR_WS_ACTDBG=1: read-only Stage-D reconnaissance. Log transitions for
 * drawable object slots whose bounding boxes intersect a live side margin but
 * not the authentic 256px activation window. This deliberately does not call
 * object logic, alter $0400, build OAM, or load graphics. */
void ActRaiser_WidescreenSpriteActivationProbe(void) {
  static int enabled = -1;
  static WsActivationCandidate prior[64];
  if (enabled < 0) {
    const char *e = getenv("AR_WS_ACTDBG");
    enabled = (e && e[0] && e[0] != '0');
  }
  if (!enabled || !g_ppu || g_ram[0x18] < 0x01 || g_ram[0x18] > 0x07)
    return;

  int ext_l = g_ppu->extraLeftCur;
  int ext_r = g_ppu->extraRightCur;
  if (!(ext_l | ext_r))
    return;

  uint16 cam_x = ws_ram16(0x22);
  uint16 cam_y = ws_ram16(0x24);
  unsigned gf = (unsigned)ws_ram16(0x88);

  for (int slot = 0; slot < 64; slot++) {
    uint16 obj = (uint16)(0x06A0 + slot * 0x40);
    uint16 status = ws_ram16(obj);
    if (status & 0x8000)
      break;

    int xlo = 0, xhi = 0;
    int eligible = !(status & 0x4C00);
    int vertical = eligible && ws_axis_visible(
        ws_ram16((uint16)(obj + 0x04)), ws_ram16((uint16)(obj + 0x0C)),
        ws_ram16((uint16)(obj + 0x10)), cam_y, 0, 224, NULL, NULL);
    int authentic = vertical && ws_axis_visible(
        ws_ram16((uint16)(obj + 0x02)), ws_ram16((uint16)(obj + 0x0A)),
        ws_ram16((uint16)(obj + 0x0E)), cam_x, 0, 256, &xlo, &xhi);
    int wide = vertical && ws_axis_visible(
        ws_ram16((uint16)(obj + 0x02)), ws_ram16((uint16)(obj + 0x0A)),
        ws_ram16((uint16)(obj + 0x0E)), cam_x, -ext_l, 256 + ext_r,
        &xlo, &xhi);
    int candidate = wide && !authentic;

    uint16 flags = ws_ram16((uint16)(obj + 0x30));
    uint16 handler = ws_ram16((uint16)(obj + 0x12));
    uint8 defbank = g_ram[(uint16)(obj + 0x18)];
    uint16 def = ws_ram16((uint16)(obj + 0x20));
    uint16 type = ws_ram16((uint16)(obj + 0x16));
    WsActivationCandidate *p = &prior[slot];

    if (candidate) {
      int changed = !p->present || p->status != status || p->flags != flags ||
                    p->handler != handler || p->defbank != defbank ||
                    p->def != def || p->type != type;
      if (changed) {
        fprintf(stderr,
                "[ws-activate] gf=%u slot=%d obj=$%04X event=%s side=%c "
                "span=[%d,%d] margins=%d/%d status=$%04X flags30=$%04X "
                "handler=$%04X type=$%04X def=$%02X:%04X\n",
                gf, slot, obj, p->present ? "change" : "enter",
                xhi < 0 ? 'L' : 'R', xlo, xhi, ext_l, ext_r, status,
                flags, handler, type, defbank, def);
      }
      p->present = 1;
      p->status = status;
      p->flags = flags;
      p->handler = handler;
      p->defbank = defbank;
      p->def = def;
      p->type = type;
    } else if (p->present) {
      fprintf(stderr,
              "[ws-activate] gf=%u slot=%d obj=$%04X event=exit reason=%s "
              "span=[%d,%d] status=$%04X flags30=$%04X\n",
              gf, slot, obj, authentic ? "authentic" : "outside",
              xlo, xhi, status, flags);
      p->present = 0;
    }
  }
}

static int ws_margin_objects_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("AR_WS_MARGIN_OBJECTS");
    enabled = !(e && e[0] == '0');
  }
  return enabled;
}

static int ws_margin_activation_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("AR_WS_MARGIN_ACTIVATION");
    enabled = !(e && e[0] == '0');
  }
  return enabled;
}

static int ws_scan_axis_visible(uint16 pos, uint16 leading, uint16 trailing,
                                uint16 camera, int left, int right,
                                uint16 limit) {
  uint16 span = (uint16)(limit + left + right);
  uint16 edge0 = (uint16)(pos - leading - camera + left);
  if (edge0 < span)
    return 1;
  if (edge0 & 0x8000) {
    uint16 edge1 = (uint16)(pos + trailing - camera + left);
    if (edge1 < span)
      return 1;
  }
  return 0;
}

/* Stage D1/D2 replacement for $00:8C98. Object drawing and $0400 activation
 * use separate horizontal windows and retain independent fidelity switches.
 * This reproduces the ROM's PHP/PLP stack byte and its normal two-bit-per-slot
 * high-table flush, which the historical scan port did not. */
RecompReturn ActRaiser_ObjectVisibilityScanWide(CpuState *cpu) {
  cpu_mirrors_to_p(cpu);
  cpu_write8(cpu, 0x00, cpu->S, cpu->P);
  cpu->S = (uint16)(cpu->S - 1);          /* PHP */
  cpu->P &= (uint8)~0x30;                /* REP #$30 */
  cpu_p_to_mirrors(cpu);

  uint16 saved_s = cpu->S;
  ws_dp16w(cpu, 0x94, (uint16)(ws_dp16(cpu, 0x22) - 0x10));
  ws_dp16w(cpu, 0x96, (uint16)(ws_dp16(cpu, 0x24) - 0x10));
  ws_dp16w(cpu, 0x9A, 0x0580);
  ws_dp16w(cpu, 0x9C, 4);
  ws_dp16w(cpu, 0x00, saved_s);
  for (int i = 0; i < 0x0200; i += 2) {
    g_ram[0x0380 + i] = 0x80;
    g_ram[0x0381 + i] = 0xE0;
  }

  cpu->A = saved_s;
  cpu->X = 0;
  cpu->Y = 0;
  cpu->_flag_Z = 1;
  cpu->_flag_N = 0;
  cpu->P = (uint8)((cpu->P & ~0x82) | 0x02);
  {
    uint16 call_s = cpu->S;
    cpu_write8(cpu, 0x00, cpu->S, 0x8C); cpu->S--;
    cpu_write8(cpu, 0x00, cpu->S, 0xDD); cpu->S--;
    cpu->host_return_valid = 1;
    RecompReturn r = bank_00_923A_M0X0(cpu);
    cpu->S = call_s;
    if (r != RECOMP_RETURN_NORMAL)
      return r;
  }

  uint16 obj = 0x06A0;
  uint16 oam_y = cpu->Y;
  uint16 terminal_status = 0;
  int oam_full = 0;
  int live_l = 0, live_r = 0;
  if (g_ppu && g_ram[0x18] >= 0x01 && g_ram[0x18] <= 0x07) {
    live_l = g_ppu->extraLeftCur;
    live_r = g_ppu->extraRightCur;
  }
  int draw_l = ws_margin_objects_enabled() ? live_l : 0;
  int draw_r = ws_margin_objects_enabled() ? live_r : 0;
  int activation_wide = ws_margin_activation_enabled();
  int activation_l = activation_wide ? live_l : 0;
  int activation_r = activation_wide ? live_r : 0;
  const char *actdbg = getenv("AR_WS_ACTDBG");
  int activation_debug = actdbg && actdbg[0] && actdbg[0] != '0';
  uint16 cam_x = ws_dp16(cpu, 0x22);
  uint16 cam_y = ws_dp16(cpu, 0x24);

  for (;;) {
    uint16 status = cpu_read16(cpu, cpu->DB, obj);
    if (status & 0x8000) {
      terminal_status = status;
      break;
    }
    if (!(status & 0x4C00)) {
      uint16 px = cpu_read16(cpu, cpu->DB, (uint16)(obj + 0x02));
      uint16 py = cpu_read16(cpu, cpu->DB, (uint16)(obj + 0x04));
      uint16 xl = cpu_read16(cpu, cpu->DB, (uint16)(obj + 0x0A));
      uint16 yt = cpu_read16(cpu, cpu->DB, (uint16)(obj + 0x0C));
      uint16 xr = cpu_read16(cpu, cpu->DB, (uint16)(obj + 0x0E));
      uint16 yb = cpu_read16(cpu, cpu->DB, (uint16)(obj + 0x10));
      int vertical = ws_scan_axis_visible(py, yt, yb, cam_y, 0, 0, 0x00E0);
      int authentic = vertical &&
          ws_scan_axis_visible(px, xl, xr, cam_x, 0, 0, 0x0100);
      int draw = vertical &&
          ws_scan_axis_visible(px, xl, xr, cam_x, draw_l, draw_r, 0x0100);
      int activation = vertical && ws_scan_axis_visible(
          px, xl, xr, cam_x, activation_l, activation_r, 0x0100);

      if (draw && !(status & 0x2000)) {
        cpu->X = obj;
        cpu->Y = oam_y;
        uint16 call_s = cpu->S;
        cpu_write8(cpu, 0x00, cpu->S, 0x8D); cpu->S--;
        cpu_write8(cpu, 0x00, cpu->S, 0x35); cpu->S--;
        cpu->host_return_valid = 1;
        RecompReturn r = ActRaiser_BuildObjectSprites(cpu);
        cpu->S = call_s;
        if (r != RECOMP_RETURN_NORMAL)
          return r;
        oam_y = cpu->Y;
        if (cpu->_flag_C) {
          oam_full = 1;
          break;
        }
      }

      uint16 flags = cpu_read16(cpu, cpu->DB, (uint16)(obj + 0x30));
      uint16 next_flags = activation ? (uint16)(flags & ~0x0400)
                                     : (uint16)(flags | 0x0400);
      if (activation_debug && ((flags ^ next_flags) & 0x0400)) {
        int xlo = (int)(int16_t)(uint16)(px - xl - cam_x);
        int xhi = (int)(int16_t)(uint16)(px + xr - cam_x);
        uint16 handler = cpu_read16(cpu, cpu->DB, (uint16)(obj + 0x12));
        uint16 type = cpu_read16(cpu, cpu->DB, (uint16)(obj + 0x16));
        uint8 defbank = cpu_read8(cpu, cpu->DB, (uint16)(obj + 0x18));
        uint16 def = cpu_read16(cpu, cpu->DB, (uint16)(obj + 0x20));
        fprintf(stderr,
                "[ws-activation-state] gf=%u slot=%u obj=$%04X "
                "$0400=%u->%u mode=%s authentic=%d draw=%d active=%d "
                "span=[%d,%d] margins=%d/%d handler=$%04X type=$%04X "
                "def=$%02X:%04X\n",
                (unsigned)ws_dp16(cpu, 0x88),
                (unsigned)((obj - 0x06A0) / 0x40), obj,
                !!(flags & 0x0400), !!(next_flags & 0x0400),
                activation_wide ? "wide" : "authentic",
                authentic, draw, activation, xlo, xhi, live_l, live_r,
                handler, type, defbank, def);
      }
      cpu_write16(cpu, cpu->DB, (uint16)(obj + 0x30), next_flags);
    }
    obj = (uint16)(obj + 0x40);
  }

  if (!oam_full) {
    uint8 acc = g_ram[(uint16)(cpu->D + 0x00)];
    uint16 count = ws_dp16(cpu, 0x9C);
    do {
      acc >>= 2;
      count--;
    } while (count != 0);
    ws_dp16w(cpu, 0x9C, count);
    g_ram[ws_dp16(cpu, 0x9A)] = acc;
    /* SEP #$20; LDA $00 changes only A.low. A.high remains the terminator
     * status high byte and X remains the terminator object address. */
    cpu->A = (uint16)((terminal_status & 0xFF00) | acc);
    cpu->X = obj;
    cpu->Y = oam_y;
    cpu->_flag_Z = 1;
    cpu->_flag_N = 0;
    cpu->P = (uint8)((cpu->P & ~0x82) | 0x02);
  }

  cpu->S = (uint16)(cpu->S + 1);          /* PLP */
  cpu->P = cpu_read8(cpu, 0x00, cpu->S);
  cpu_p_to_mirrors(cpu);
  if (cpu->x_flag) {
    cpu->X &= 0x00FF;
    cpu->Y &= 0x00FF;
  }
  cpu->S = (uint16)(cpu->S + 2);          /* RTS */
  return RECOMP_RETURN_NORMAL;
}

/* hle_func replacement for $00:8D68. Entry contract from original $8C98:
 * M=0, X=0, X=object base, Y=next OAM-shadow byte offset. Return preserves
 * the object in X, advances Y, and reports OAM-full through carry. */
RecompReturn ActRaiser_BuildObjectSprites(CpuState *cpu) {
  uint16 obj = cpu->X;
  uint16 oam_y = cpu->Y;
  int full = 0;

  uint16 s14 = (uint16)(cpu_read16(cpu, cpu->DB, (uint16)(obj + 0x02)) -
                        cpu_read16(cpu, cpu->DB, (uint16)(obj + 0x0A)) -
                        ws_dp16(cpu, 0x94));
  uint16 s16 = (uint16)(cpu_read16(cpu, cpu->DB, (uint16)(obj + 0x04)) -
                        cpu_read16(cpu, cpu->DB, (uint16)(obj + 0x0C)) -
                        ws_dp16(cpu, 0x96));
  ws_dp16w(cpu, 0x14, s14);
  ws_dp16w(cpu, 0x16, s16);

  uint16 flip = (uint16)(cpu_read16(cpu, cpu->DB,
                                    (uint16)(obj + 0x28)) ^ 0x0100);
  ws_dp16w(cpu, 0x9E, flip);
  if (cpu_read16(cpu, cpu->DB, (uint16)(obj + 0x30)) & 0x2008)
    ws_dp16w(cpu, 0x8F, (uint16)(ws_dp16(cpu, 0x8F) | 0x0E00));

  uint8 defbank = cpu_read8(cpu, cpu->DB, (uint16)(obj + 0x18));
  uint16 def = (uint16)(cpu_read16(cpu, cpu->DB,
                                   (uint16)(obj + 0x20)) + 4);
  uint16 count = cpu_read8(cpu, defbank, def);
  def++;
  ws_dp16w(cpu, 0x0C, count);

  int ext_l = 0, ext_r = 0;
  if (ws_sprite_widen_enabled() && g_ppu &&
      g_ram[0x18] >= 0x01 && g_ram[0x18] <= 0x07) {
    ext_l = g_ppu->extraLeftCur;
    ext_r = g_ppu->extraRightCur;
  }

  for (;;) {
    uint16 a9e = ws_dp16(cpu, 0x9E);
    uint16 ypair = (uint16)(cpu_read8(cpu, defbank, (uint16)(def + 3)) |
                             (cpu_read8(cpu, defbank,
                                        (uint16)(def + 4)) << 8));
    uint16 dy = (a9e & 0x8000) ? (uint16)(ypair >> 8)
                               : (uint16)(ypair & 0xFF);
    uint16 y = (uint16)(dy + ws_dp16(cpu, 0x16));

    if (y < 0x00F0) {
      /* CMP failed with carry clear, so the ROM's SBC #$0010 stores y-$11. */
      uint16 y_store = (uint16)(y - 0x11);
      cpu_write16(cpu, defbank, (uint16)(0x0381 + oam_y), y_store);

      uint16 tile = (uint16)(cpu_read8(cpu, defbank,
                                      (uint16)(def + 5)) |
                              (cpu_read8(cpu, defbank,
                                       (uint16)(def + 6)) << 8));
      uint16 attr = (uint16)((tile ^ a9e) | ws_dp16(cpu, 0x8F));
      cpu_write16(cpu, defbank, (uint16)(0x0382 + oam_y), attr);

      uint16 xpair = (uint16)(cpu_read8(cpu, defbank,
                                       (uint16)(def + 1)) |
                              (cpu_read8(cpu, defbank,
                                       (uint16)(def + 2)) << 8));
      uint16 dx = (a9e & 0x4000) ? (uint16)(xpair >> 8)
                                 : (uint16)(xpair & 0xFF);
      uint16 x_biased = (uint16)(dx + ws_dp16(cpu, 0x14));

      /* Authentic: x<$110 => screen-x in [-16,256). Wide:
       * (x+L)<$110+L+R => screen-x in [-16-L,256+R). The 16px left reach is
       * exactly ActRaiser's maximum OAM tile width. The historical fixed-64
       * reach over-emitted invisible definitions and is intentionally gone. */
      uint16 wide_x = (uint16)(x_biased + ext_l);
      uint16 wide_bound = (uint16)(0x0110 + ext_l + ext_r);
      if (wide_x < wide_bound) {
        uint16 screen_x = (uint16)(x_biased - 0x10);
        cpu_write8(cpu, defbank, (uint16)(0x0380 + oam_y),
                   (uint8)screen_x);

        uint8 acc = g_ram[(uint16)(cpu->D + 0x00)];
        acc = (uint8)((acc >> 1) | (((screen_x >> 8) & 1) << 7));
        acc = (uint8)((acc >> 1) |
                      ((cpu_read8(cpu, defbank, def) & 1) << 7));
        g_ram[(uint16)(cpu->D + 0x00)] = acc;

        uint16 slots = (uint16)(ws_dp16(cpu, 0x9C) - 1);
        ws_dp16w(cpu, 0x9C, slots);
        if (slots == 0) {
          uint16 highp = ws_dp16(cpu, 0x9A);
          g_ram[highp] = acc;
          ws_dp16w(cpu, 0x9A, (uint16)(highp + 1));
          ws_dp16w(cpu, 0x9C, 4);
        }

        if (ws_sprite_debug_enabled()) {
          int sx = screen_x & 0x8000 ? (int)screen_x - 0x10000
                                     : (int)screen_x;
          if (sx < -16 || sx >= 256) {
            unsigned gf = (unsigned)(g_ram[0x88] | (g_ram[0x89] << 8));
            fprintf(stderr,
                    "[ws-sprite] gf=%u obj=$%04X def=$%02X:%04X x=%d y=%u tile=$%02X\n",
                    gf, obj, defbank, (unsigned)(uint16)(def - 1), sx,
                    (unsigned)(y_store & 0xFF), (unsigned)(tile & 0xFF));
          }
        }

        oam_y = (uint16)(oam_y + 4);
        if (oam_y == 0x0200) {
          full = 1;
          break;
        }
      } else {
        /* Original reject occurs after y/tile writes. Re-park x/y only. */
        cpu_write16(cpu, defbank, (uint16)(0x0380 + oam_y), 0xE080);
      }
    }

    def = (uint16)(def + 7);
    count--;
    ws_dp16w(cpu, 0x0C, count);
    if (count == 0)
      break;
  }

  ws_dp16w(cpu, 0x8F, (uint16)(ws_dp16(cpu, 0x8F) & ~0x0E00));
  cpu->A = 0x0E00;
  cpu->X = obj;
  cpu->Y = oam_y;
  cpu->_flag_C = full ? 1 : 0;
  cpu->_flag_Z = obj == 0;
  cpu->_flag_N = (obj & 0x8000) != 0;
  cpu->P = (uint8)((cpu->P & ~0x83) |
                   (cpu->_flag_N ? 0x80 : 0) |
                   (cpu->_flag_Z ? 0x02 : 0) |
                   (cpu->_flag_C ? 0x01 : 0));
  cpu->m_flag = 0;
  cpu->P &= (uint8)~0x20;

  /* Emulate the replaced RTS; the generated paired caller then restores S. */
  cpu->S = (uint16)(cpu->S + 2);
  return RECOMP_RETURN_NORMAL;
}
