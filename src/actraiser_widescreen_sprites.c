/* Staged action and simulation-town sprite handling for widescreen.
 *
 * $00:8D68 widens per-definition emission. The audited $00:8C98 replacement
 * independently controls initialized margin-object drawing (Stage D1) and
 * the $0400 activation boundary (Stage D2). Keeping those decisions separate
 * is essential: a fidelity run can disable either without replacing binaries.
 *
 * AR_WS_SPRITES=0 restores authentic per-definition emission.
 * AR_WS_MARGIN_OBJECTS=0 restores authentic object draw coverage.
 * AR_WS_MARGIN_ACTIVATION=0 restores the authentic $0400 boundary; Stage D2
 * is enabled by default after direct Fillmore validation.
 *
 * $01:B4C6 is the town camera follow/clamp. Its faithful port keeps the wide
 * viewport inside the 512px world before OAM is composed. $01:ADAD/$01:AE6F
 * widen only $0A00+ world records, and $01:B473 extends the dedicated angel-
 * projectile lifetime check to the same finite horizontal window.
 * AR_WS_SIM=0 restores the authentic camera; AR_WS_SIM_SPRITES=0 keeps both
 * sprite/projectile predicates authentic. */

#include "cpu_state.h"
#include "actraiser_rtl.h"
#include "settings.h"
#include "snes/ppu.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

extern uint8 g_ram[0x20000];
extern Ppu *g_ppu;
extern bool g_ws_active;
extern int g_ws_extra;
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
  return g_settings.ws_sprites;
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
  return g_settings.ws_margin_objects;
}

static int ws_margin_activation_enabled(void) {
  return g_settings.ws_margin_activation;
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

/* ── Simulation-town world sprite composition ─────────────────────────────
 *
 * $01:ACD9 has two record scans which share these leaf emitters:
 *   fixed/UI  $06A0-$09FF (48 x $12 bytes)
 *   world     $0A00-$1087 (44 x $26 bytes)
 *
 * The original $01:ADAD/$01:AE6F bodies differ only in their attribute-word
 * transform. Keep every other ROM behavior—including the unusual $80 offset
 * rule, vertical clipping, rejected-slot parking, OAM high-table packing, and
 * shared $98 cursor—inside one faithful port. Widescreen changes only the
 * horizontal predicate, only for the world array, and only in town mode.
 *
 * AR_WS_SIM_SPRITES=0 restores the authentic predicate in the same generated
 * binary. AR_WS_SIM_SPRDBG=1 logs only components newly admitted into a live
 * side margin; it never changes OAM or game state. */

static int ws_sim_sprite_widen_enabled(void) {
  /* Still gated on the AR_WS_SIM master, as before the refactor. */
  return g_settings.ws_sim_sprites && g_settings.ws_sim;
}

static int ws_sim_sprite_debug_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("AR_WS_SIM_SPRDBG");
    enabled = (e && e[0] && e[0] != '0');
  }
  return enabled;
}

/* The ROM sign-extends component offsets $81-$FF, but deliberately treats
 * $80 as +128. Do not replace this with an int8_t cast. */
static uint16 ws_sim_part_offset(uint8 v) {
  return v >= 0x81 ? (uint16)(0xFF00 | v) : (uint16)v;
}

static void ws_sim_live_margins(int *left, int *right) {
  *left = 0;
  *right = 0;
  if (!g_ws_active || !ws_sim_sprite_widen_enabled() ||
      g_ram[0x18] != 0x00 ||
      g_ram[0x19] < 0x01 || g_ram[0x19] > 0x06)
    return;

  int cam = (int)ws_ram16(0x22);
  if (cam < 0) cam = 0;
  if (cam > 0x0100) cam = 0x0100;
  *left = cam < g_ws_extra ? cam : g_ws_extra;
  *right = (0x0100 - cam) < g_ws_extra ? (0x0100 - cam) : g_ws_extra;
}

static int ws_sim_camera_debug_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("AR_WS_SIM_CAMDBG");
    enabled = (e && e[0] && e[0] != '0');
  }
  return enabled;
}

static uint16 ws_sim_authentic_camera(uint16 target, uint16 center,
                                      uint16 maximum) {
  uint16 camera = (uint16)(target - center);
  if (camera & 0x8000)
    return 0;
  return camera >= maximum ? maximum : camera;
}

/* Faithful replacement for $01:B4C6, the camera writer called before the
 * town's behavior/OAM pass. Native horizontal follow is
 *
 *   cameraX = clamp($0AEE-$80, 0, $100)
 *
 * for a 512px world and 256px viewport. A wide viewport must instead keep
 * [cameraX-extra, cameraX+$100+extra) inside [0,$200), yielding
 * [extra,$100-extra]. Apply the same bounds to transient camera shake so an
 * outward shake cannot reveal a cleared margin at a map edge. Vertical follow,
 * both shake fields, their one-frame clear, final A=0, caller P/DB, and the RTL
 * stack contract remain authentic.
 *
 * RAW widescreen deliberately retains the native camera as a before/after
 * reference. AR_WS_SIM=0 and non-town $00 submodes do the same. */
RecompReturn ActRaiser_UpdateSimCamera(CpuState *cpu) {
  const uint8 saved_p = cpu->P;
  const uint8 saved_db = cpu->DB;
  const uint16 saved_x = cpu->X;
  const uint16 saved_y = cpu->Y;

  const uint16 target_x = cpu_read16(cpu, 0x01, 0x0AEE);
  const uint16 target_y = cpu_read16(cpu, 0x01, 0x0AF0);
  const uint16 native_x = ws_sim_authentic_camera(target_x, 0x0080, 0x0100);
  uint16 camera_x = native_x;
  uint16 camera_y = ws_sim_authentic_camera(target_y, 0x0070, 0x011F);

  const int wide = g_ws_active && g_settings.ws_sim &&
      g_settings.display_mode != kDisplayMode_43 &&
      g_settings.display_mode != kDisplayMode_WideRaw &&
      g_ram[0x18] == 0x00 &&
      g_ram[0x19] >= 0x01 && g_ram[0x19] <= 0x06;
  int margin = wide ? g_ws_extra : 0;
  if (margin < 0) margin = 0;
  if (margin > 0x80) margin = 0x80;
  const uint16 left = (uint16)margin;
  const uint16 right = (uint16)(0x0100 - margin);
  if (camera_x < left) camera_x = left;
  if (camera_x > right) camera_x = right;

  const uint16 shake_x = cpu_read16(cpu, 0x7F, 0x9F65);
  const uint16 shake_y = cpu_read16(cpu, 0x7F, 0x9F67);
  const uint16 shaken_x = (uint16)(camera_x + shake_x);
  const uint16 shaken_y = (uint16)(camera_y + shake_y);
  const int accept_x = !(shaken_x & 0x8000) &&
                       shaken_x >= left && shaken_x <= right;
  const int accept_y = !(shaken_y & 0x8000) && shaken_y <= 0x011F;
  if (accept_x) camera_x = shaken_x;
  if (accept_y) camera_y = shaken_y;

  ws_dp16w(cpu, 0x22, camera_x);
  ws_dp16w(cpu, 0x24, camera_y);
  cpu_write16(cpu, 0x7F, 0x9F65, 0);
  cpu_write16(cpu, 0x7F, 0x9F67, 0);

  if (wide && ws_sim_camera_debug_enabled()) {
    static uint16 last_camera = 0xFFFF;
    static uint8 last_town = 0xFF;
    static int last_clamped = -1;
    const int clamped = camera_x != native_x;
    if (camera_x != last_camera || g_ram[0x19] != last_town ||
        clamped != last_clamped) {
      fprintf(stderr,
              "[ws-sim-camera] gf=%u town=%u target=%u native=%u wide=%u "
              "bounds=%u-%u shake=%d/%d\n",
              (unsigned)ws_ram16(0x88), (unsigned)g_ram[0x19],
              (unsigned)target_x, (unsigned)native_x, (unsigned)camera_x,
              (unsigned)left, (unsigned)right, accept_x, accept_y);
      last_camera = camera_x;
      last_town = g_ram[0x19];
      last_clamped = clamped;
    }
  }

  /* PLB/PLP restore the caller's DB/P; RTL consumes the three-byte JSL frame.
   * A is zero from the two shake-field clears. X/Y are untouched by the ROM. */
  cpu->A = 0;
  cpu->DB = saved_db;
  cpu->P = saved_p;
  cpu_p_to_mirrors(cpu);
  cpu->X = saved_x;
  cpu->Y = saved_y;
  if (cpu->x_flag) {
    cpu->X &= 0x00FF;
    cpu->Y &= 0x00FF;
  }
  cpu->S = (uint16)(cpu->S + 3);
  return RECOMP_RETURN_NORMAL;
}

static void ws_sim_set_nz16(CpuState *cpu, uint16 value) {
  cpu->_flag_Z = value == 0;
  cpu->_flag_N = (value & 0x8000) != 0;
  cpu->P = (uint8)((cpu->P & ~0x82) |
                   (cpu->_flag_N ? 0x80 : 0) |
                   (cpu->_flag_Z ? 0x02 : 0));
}

static uint16 ws_sim_adc16(CpuState *cpu, uint16 lhs, uint16 rhs) {
  const uint32 sum = (uint32)lhs + (uint32)rhs +
                     (uint32)(cpu->_flag_C ? 1 : 0);
  const uint16 result = (uint16)sum;
  cpu->_flag_C = sum > 0xFFFF;
  cpu->_flag_V = ((~(lhs ^ rhs) & (lhs ^ result) & 0x8000) != 0);
  ws_sim_set_nz16(cpu, result);
  cpu->P = (uint8)((cpu->P & ~0x41) |
                   (cpu->_flag_V ? 0x40 : 0) |
                   (cpu->_flag_C ? 0x01 : 0));
  return result;
}

static void ws_sim_cmp16(CpuState *cpu, uint16 lhs, uint16 rhs) {
  const uint16 result = (uint16)(lhs - rhs);
  cpu->_flag_C = lhs >= rhs;
  ws_sim_set_nz16(cpu, result);
  cpu->P = (uint8)((cpu->P & ~0x01) |
                   (cpu->_flag_C ? 0x01 : 0));
}

/* $01:B473 is the visibility/lifetime leaf used only by the angel's arrow
 * record $0B0A. The original state-2 update moves the arrow, calls this leaf,
 * and immediately destroys the record when carry returns set. That made the
 * arrow disappear at x=0/256 before the widened ADAD emitter could draw it.
 *
 * Keep the ROM's x+4 anchor, 512x512 hard bounds, 224px vertical viewport,
 * DP-$00 scratch writes, and carry contract. Only the horizontal camera
 * comparisons gain the finite live town margins. */
RecompReturn ActRaiser_SimProjectileVisible(CpuState *cpu) {
  const uint16 record = cpu->X;
  const uint16 cam_x = ws_dp16(cpu, 0x22);
  const uint16 cam_y = ws_dp16(cpu, 0x24);
  int ext_l = 0, ext_r = 0;
  ws_sim_live_margins(&ext_l, &ext_r);

  /* Preserve the original DP scratch value even though the widened upper
   * bound itself remains host-local. */
  cpu->_flag_C = 0;  /* CLC */
  cpu->P &= (uint8)~0x01;
  const uint16 authentic_right = ws_sim_adc16(cpu, cam_x, 0x0100);
  ws_dp16w(cpu, 0x00, authentic_right);

  uint16 value = cpu_read16(cpu, cpu->DB, (uint16)(record + 0x0A));
  cpu->A = value;
  ws_sim_set_nz16(cpu, value);
  cpu->_flag_C = 0;  /* CLC */
  cpu->P &= (uint8)~0x01;
  value = ws_sim_adc16(cpu, value, 0x0004);
  cpu->A = value;

  int culled = (value & 0x8000) != 0;
  if (!culled) {
    ws_sim_cmp16(cpu, value, 0x0200);
    culled = cpu->_flag_C;
  }

  const uint16 wide_left = (uint16)(cam_x - ext_l);
  const uint16 wide_right = (uint16)(authentic_right + ext_r);
  if (!culled) {
    ws_sim_cmp16(cpu, value, wide_left);
    culled = !cpu->_flag_C;
  }
  if (!culled) {
    ws_sim_cmp16(cpu, value, wide_right);
    culled = cpu->_flag_C;
  }

  if (!culled) {
    cpu->A = cam_y;
    ws_sim_set_nz16(cpu, cam_y);
    cpu->_flag_C = 0;  /* CLC */
    cpu->P &= (uint8)~0x01;
    const uint16 vertical_bottom = ws_sim_adc16(cpu, cam_y, 0x00E0);
    cpu->A = vertical_bottom;
    ws_dp16w(cpu, 0x00, vertical_bottom);

    value = cpu_read16(cpu, cpu->DB, (uint16)(record + 0x0C));
    cpu->A = value;
    ws_sim_set_nz16(cpu, value);
    if (value & 0x8000) {
      culled = 1;
    } else {
      ws_sim_cmp16(cpu, value, 0x0200);
      if (cpu->_flag_C) {
        culled = 1;
      } else {
        ws_sim_cmp16(cpu, value, cam_y);
        if (!cpu->_flag_C) {
          culled = 1;
        } else {
          ws_sim_cmp16(cpu, value, vertical_bottom);
          culled = cpu->_flag_C;
        }
      }
    }
  }

  if (!culled && (ext_l || ext_r) && ws_sim_sprite_debug_enabled()) {
    const uint16 x4 = (uint16)(
        cpu_read16(cpu, cpu->DB, (uint16)(record + 0x0A)) + 4);
    if (x4 < cam_x || x4 >= authentic_right) {
      fprintf(stderr,
              "[ws-sim-projectile] gf=%u record=$%04X world=%u,%u "
              "camera=%u,%u margins=%d/%d\n",
              (unsigned)ws_ram16(0x88), record,
              (unsigned)cpu_read16(cpu, cpu->DB,
                                   (uint16)(record + 0x0A)),
              (unsigned)cpu_read16(cpu, cpu->DB,
                                   (uint16)(record + 0x0C)),
              (unsigned)cam_x, (unsigned)cam_y, ext_l, ext_r);
    }
  }

  /* $B44B branches to destruction on carry set. SEC/CLC do not disturb the
   * final comparison's N/Z/V state. */
  cpu->_flag_C = culled ? 1 : 0;
  cpu->P = (uint8)((cpu->P & ~0x01) | (culled ? 0x01 : 0));
  cpu->S = (uint16)(cpu->S + 2);  /* replaced RTS */
  return RECOMP_RETURN_NORMAL;
}

static RecompReturn ws_sim_build_sprites(CpuState *cpu, int alternate_attr) {
  const uint16 record = cpu->X;
  const int world_record = record >= 0x0A00 && record < 0x1088;
  int ext_l = 0, ext_r = 0;
  if (world_record)
    ws_sim_live_margins(&ext_l, &ext_r);

  const uint16 base_x = (uint16)(
      cpu_read16(cpu, cpu->DB, (uint16)(record + 0x0A)) -
      ws_dp16(cpu, 0x94));
  const uint16 base_y = (uint16)(
      cpu_read16(cpu, cpu->DB, (uint16)(record + 0x0C)) -
      ws_dp16(cpu, 0x96));
  ws_dp16w(cpu, 0x14, base_x);
  ws_dp16w(cpu, 0x16, base_y);

  uint16 part = cpu_read16(cpu, cpu->DB, (uint16)(record + 0x08));
  uint16 count = cpu_read8(cpu, cpu->DB, part);
  part = (uint16)(part + 1);
  ws_dp16w(cpu, 0x0E, count);

  uint16 oam = ws_dp16(cpu, 0x98);
  uint16 final_a = 0;
  int final_c = 0;
  unsigned part_index = 0;

  do {
    const uint16 x_biased = (uint16)(
        base_x + ws_sim_part_offset(cpu_read8(
                     cpu, cpu->DB, (uint16)(part + 1))));
    const int authentic_x = x_biased < 0x0110;
    const uint16 wide_x = (uint16)(x_biased + ext_l);
    const uint16 wide_bound = (uint16)(0x0110 + ext_l + ext_r);

    if (wide_x < wide_bound) {
      const uint16 screen_x = (uint16)(x_biased - 0x0010);
      cpu_write8(cpu, cpu->DB, (uint16)(0x0380 + oam), (uint8)screen_x);

      uint16 highp = ws_dp16(cpu, 0x9A);
      uint8 mask = (uint8)ws_dp16(cpu, 0x9C);
      uint8 high = cpu_read8(cpu, cpu->DB, highp);
      if (screen_x & 0x0100)
        high |= mask;
      else
        high &= (uint8)~mask;
      if (cpu_read8(cpu, cpu->DB, part) & 0x01)
        high |= (uint8)(mask << 1);
      else
        high &= (uint8)~(mask << 1);
      cpu_write8(cpu, cpu->DB, highp, high);

      const uint16 y_biased = (uint16)(
          base_y + ws_sim_part_offset(cpu_read8(
                       cpu, cpu->DB, (uint16)(part + 2))));
      if (y_biased < 0x00F0) {
        const uint8 screen_y = (uint8)(y_biased - 0x0011);
        cpu_write8(cpu, cpu->DB, (uint16)(0x0381 + oam), screen_y);

        const uint16 raw_attr =
            cpu_read16(cpu, cpu->DB, (uint16)(part + 3));
        const uint16 attr = alternate_attr
            ? (uint16)((raw_attr & 0xF1FF) | 0x0600 | ws_dp16(cpu, 0x8F))
            : (uint16)(raw_attr | ws_dp16(cpu, 0x8F));
        cpu_write16(cpu, cpu->DB, (uint16)(0x0382 + oam), attr);
        final_a = attr;

        if (world_record && !authentic_x && ws_sim_sprite_debug_enabled()) {
          int sx = screen_x & 0x01FF;
          if (sx >= 0x0100 + ext_r)
            sx -= 0x0200;
          fprintf(stderr,
                  "[ws-sim-sprite] gf=%u emitter=%s record=$%04X "
                  "part=%u oam=$%03X x=%d y=%u margins=%d/%d "
                  "tile=$%03X attr=$%04X\n",
                  (unsigned)ws_ram16(0x88),
                  alternate_attr ? "AE6F" : "ADAD", record, part_index,
                  oam, sx, (unsigned)screen_y, ext_l, ext_r,
                  (unsigned)(attr & 0x01FF), attr);
        }

        oam = (uint16)(oam + 4);
        if (oam == 0x0200) {
          final_c = 1;  /* CPX #$0200 equality carry survives PLX. */
          break;
        }

        if (mask == 0x40) {
          ws_dp16w(cpu, 0x9A, (uint16)(highp + 1));
          ws_dp16w(cpu, 0x9C, 1);
        } else {
          ws_dp16w(cpu, 0x9C, (uint16)(mask << 2));
        }
      } else {
        /* The ROM has already touched x/high bits at this point, then parks
         * the unallocated low-table slot without advancing either cursor. */
        cpu_write16(cpu, cpu->DB, (uint16)(0x0380 + oam), 0xE000);
      }
    } else {
      cpu_write16(cpu, cpu->DB, (uint16)(0x0380 + oam), 0xE000);
    }

    {
      uint16 next = (uint16)(part + 5);
      final_c = next < part;  /* C from the ROM's TYA;CLC;ADC #5. */
      part = next;
      final_a = part;
    }
    count = (uint16)(count - 1);
    ws_dp16w(cpu, 0x0E, count);
    part_index++;
  } while (count != 0);

  ws_dp16w(cpu, 0x98, oam);
  cpu->A = final_a;
  cpu->X = record;  /* PLX */
  cpu->Y = part;
  cpu->_flag_C = final_c;
  cpu->_flag_Z = record == 0;
  cpu->_flag_N = (record & 0x8000) != 0;
  cpu->P = (uint8)((cpu->P & ~0x83) |
                   (cpu->_flag_N ? 0x80 : 0) |
                   (cpu->_flag_Z ? 0x02 : 0) |
                   (cpu->_flag_C ? 0x01 : 0));
  cpu->m_flag = 0;
  cpu->x_flag = 0;
  cpu->P &= (uint8)~0x30;

  /* Emulate the replaced RTS; the generated paired caller restores S. The
   * original PHX/PLX pair is net-neutral and represented by the saved record. */
  cpu->S = (uint16)(cpu->S + 2);
  return RECOMP_RETURN_NORMAL;
}

RecompReturn ActRaiser_BuildSimSprites(CpuState *cpu) {
  return ws_sim_build_sprites(cpu, 0);
}

RecompReturn ActRaiser_BuildSimSpritesAlt(CpuState *cpu) {
  return ws_sim_build_sprites(cpu, 1);
}
