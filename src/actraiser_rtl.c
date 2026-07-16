#define _XOPEN_SOURCE 600
#include "actraiser_rtl.h"
#include "variables.h"
#include "settings.h"
#include "common_cpu_infra.h"
#include "snes/snes.h"
#include "cpu_state.h"
#include "funcs.h"
#include "cpu_trace.h"
#include <stdio.h>
#include <stdbool.h>
#include <ucontext.h>
#include <stdlib.h>
#include <string.h>

extern int snes_frame_counter;

#define GAME_STACK_SIZE (2 * 1024 * 1024)

static ucontext_t g_host_ctx;
static ucontext_t g_game_ctx;
static char *g_game_stack;
static bool g_game_started;

void ActRaiser_YieldToHost(void) {
  swapcontext(&g_game_ctx, &g_host_ctx);
}

/* A 65816 hardware interrupt is register-transparent to the interrupted
 * code: RTI restores P/PC/PB and a well-behaved handler save/restores
 * A/X/Y/D/DB. We invoke the recompiled NMI/IRQ handlers as plain host-C
 * calls on the shared g_cpu; if a handler body has an internal stack
 * imbalance (e.g. an x-width mismatch) its terminal RTI can pop the
 * wrong byte as P and corrupt the interrupted code's M/X width flags.
 * Snapshot the CPU register frame before the handler and restore it
 * after — the handler's RAM/PPU side effects (the point of the IRQ)
 * persist in g_ram/g_ppu, only the CPU registers are made transparent. */
typedef struct { uint16 A, X, Y, S, D; uint8 DB, PB, P, m_flag, x_flag,
  emulation, host_return_valid, fN, fV, fZ, fC, fI, fD; } CpuRegSnapshot;

static void ActRaiser_SaveRegs(CpuState *c, CpuRegSnapshot *s) {
  s->A = c->A; s->X = c->X; s->Y = c->Y; s->S = c->S; s->D = c->D;
  s->DB = c->DB; s->PB = c->PB; s->P = c->P; s->m_flag = c->m_flag;
  s->x_flag = c->x_flag; s->emulation = c->emulation;
  s->host_return_valid = c->host_return_valid;
  s->fN = c->_flag_N; s->fV = c->_flag_V; s->fZ = c->_flag_Z;
  s->fC = c->_flag_C; s->fI = c->_flag_I; s->fD = c->_flag_D;
}

static void ActRaiser_RestoreRegs(CpuState *c, const CpuRegSnapshot *s) {
  c->A = s->A; c->X = s->X; c->Y = s->Y; c->S = s->S; c->D = s->D;
  c->DB = s->DB; c->PB = s->PB; c->P = s->P; c->m_flag = s->m_flag;
  c->x_flag = s->x_flag; c->emulation = s->emulation;
  c->host_return_valid = s->host_return_valid;
  c->_flag_N = s->fN; c->_flag_V = s->fV; c->_flag_Z = s->fZ;
  c->_flag_C = s->fC; c->_flag_I = s->fI; c->_flag_D = s->fD;
}

/* Set while an NMI/IRQ handler is executing on the host stack (the calls
 * below are bracketed by SaveRegs/RestoreRegs, so cpu->S is restored after).
 * The stack-drift tripwire reads this to ignore handler-internal imbalance. */
volatile int g_ar_in_interrupt = 0;

/* ActRaiser BRK syscall. The ROM's BRK vector ($00:852F) is:
 *   PHP; SEP #$20; STA $00035B; PLP; RTI
 * i.e. it stores A's low byte to $035B (the sound-effect request port) and
 * resumes at PC+2 — registers/flags otherwise unchanged. The game uses
 * `LDA #id; BRK` as a compact "play sound id" call throughout (e.g. enemy-death
 * SFX in the object/OAM loops). Generated code invokes this at every BRK site
 * via g_cpu_brk_hook, then falls through to the next instruction. */
static void ActRaiser_BrkHook(CpuState *cpu) {
  cpu_write8(cpu, 0x00, 0x035B, (uint8)(cpu->A & 0xFF));
  /* AR_COPLOG=1: also log BRK (sound-request) posts, for contrast against COP
   * event posts below -- lets a stuck-state capture show whether the game is
   * still alive and posting routine SFX while a specific event id never posts. */
  if (getenv("AR_COPLOG")) {
    extern uint8 g_ram[0x20000];
    extern const char *g_last_recomp_func;
    unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
    fprintf(stderr, "[brk] gf=%u fn=%s id=%02x $18=%02x $19=%02x\n",
            gf, g_last_recomp_func ? g_last_recomp_func : "?",
            (uint8)(cpu->A & 0xFF), g_ram[0x18], g_ram[0x19]);
  }
}

/* ActRaiser COP syscall — the SECOND software interrupt, structurally identical
 * to BRK. The ROM's COP vector ($00:FFE4 -> $8526) is:
 *   PHP; SEP #$20; STA $00035A; PLP; RTI
 * i.e. it stores A's low byte to $035A (a request port distinct from BRK's
 * $035B) and resumes at PC+2. The game posts events via `LDA #id; COP` — e.g.
 * the post-miniboss platform/event trigger does `LDA #$07; COP`. Without this
 * hook g_cpu_cop_hook stayed NULL, so every COP was an effect-free continue and
 * $035A was never written → the event/platform never fired. Symmetric to the
 * BRK hook; found via the oracle writing $035A 90x while the recomp wrote it 0x. */
static void ActRaiser_CopHook(CpuState *cpu) {
  cpu_write8(cpu, 0x00, 0x035A, (uint8)(cpu->A & 0xFF));
  /* AR_COPLOG=1: log every COP-posted event id + game-frame + calling recomp
   * function, so a Death-Heim stuck-state capture shows whether the
   * boss-defeat/next-encounter event ever posts at all, vs posting an id whose
   * consumer is unreached (see [[cop-syscall-hook-fix]] -- $C3DA consumer was
   * previously suspected still-unreached for a different event id). */
  if (getenv("AR_COPLOG")) {
    extern uint8 g_ram[0x20000];
    extern const char *g_last_recomp_func;
    unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
    fprintf(stderr, "[cop] gf=%u fn=%s id=%02x $18=%02x $19=%02x\n",
            gf, g_last_recomp_func ? g_last_recomp_func : "?",
            (uint8)(cpu->A & 0xFF), g_ram[0x18], g_ram[0x19]);
  }
}

/* Dump the full internal state (everything but the framebuffer, which the
 * caller writes as a .ppm) so an on-demand snapshot captures both the picture
 * AND the internals: WRAM, plus the PPU memory the WRAM dump can't see — VRAM
 * (BG tilemaps + tiles), CGRAM (palette), OAM (sprites). Critical for the
 * bridge bug, whose tiles live in VRAM, invisible to any WRAM-only diff.
 * Writes <prefix>.{wram,vram,cgram,oam}.bin. */
void ActRaiser_FullSnapshot(const char *prefix) {
  extern Ppu *g_ppu;
  char path[96];
  FILE *f;
  snprintf(path, sizeof path, "%s.wram.bin", prefix);
  f = fopen(path, "wb"); if (f) { fwrite(g_ram, 1, 0x20000, f); fclose(f); }
  if (g_ppu) {
    snprintf(path, sizeof path, "%s.vram.bin", prefix);
    f = fopen(path, "wb"); if (f) { fwrite(g_ppu->vram, 2, 0x8000, f); fclose(f); }
    snprintf(path, sizeof path, "%s.cgram.bin", prefix);
    f = fopen(path, "wb"); if (f) { fwrite(g_ppu->cgram, 2, 0x100, f); fclose(f); }
    snprintf(path, sizeof path, "%s.oam.bin", prefix);
    f = fopen(path, "wb"); if (f) { fwrite(g_ppu->oam, 2, 0x100, f); fclose(f); }
  }
}

static void game_coroutine(void) {
  cpu_state_init(&g_cpu, g_ram);
  g_cpu_brk_hook = ActRaiser_BrkHook;
  g_cpu_cop_hook = ActRaiser_CopHook;

  ResetHandler_M1X1(&g_cpu);
  for (;;)
    ActRaiser_YieldToHost();
}

RecompReturn ActRaiser_WaitForVblank(CpuState *cpu) {
  /* A85E (and the identical $00:8418) are HLE'd to this function. The real ROM
   * routine is PHP / SEP #$20 / PHA / {spin on $4210 bit 7} / PLA / PLP / RTS —
   * internally stack-neutral, and its terminating RTS pops the 2-byte return
   * frame the caller's JSR pushed. This HLE replaces the whole routine with a
   * host yield, so unless we emulate that RTS the caller's frame is orphaned on
   * the SNES stack: a 2-byte/call leak that, over a long wait loop, marches S
   * down out of page 1 into zero-page and clobbers game variables (the old
   * AF86/$2100 open-bus crash). So pop the frame here to keep S balanced, which
   * is exactly what the hardware routine does. */
  if (!getenv("AR_NOPOP")) cpu->S = (uint16)(cpu->S + 2);

  /* AR_YIELDLOG=1: dump the recomp call stack + SNES return address at each
   * vblank yield to see what the main thread is doing frame to frame. Read the
   * return frame from the PRE-pop S (sp-2, since we already added 2 above). */
  if (getenv("AR_YIELDLOG")) {
    extern const char *g_recomp_stack[];
    extern int g_recomp_stack_top;
    int top = g_recomp_stack_top;
    extern uint8 g_ram[0x20000];
    fprintf(stderr, "[yield] f=%d S=%04x A=%04x P=%02x depth=%d:",
            snes_frame_counter, cpu->S, cpu->A, cpu->P, top);
    for (int i = top - 1; i >= 0 && i >= top - 6; i--)
      fprintf(stderr, " %s", g_recomp_stack[i] ? g_recomp_stack[i] : "?");
    uint16 sp = getenv("AR_NOPOP") ? cpu->S : (uint16)(cpu->S - 2);
    uint16 rlo = g_ram[(uint16)(sp + 1)];
    uint16 rhi = g_ram[(uint16)(sp + 2)];
    fprintf(stderr, " ret~%02x:%04x\n", cpu->PB, (uint16)(((rhi << 8) | rlo) + 1));
  }

  /* AR_FORCE18=<hex>: experimentally pin $7E0018 (game-mode byte) before the
   * next NMI's ABF0 branch, to test whether a non-zero game-mode unsticks the
   * frozen title (state machine + menu decompression). Diagnostic only. */
  {
    static int f18 = -2;
    if (f18 == -2) { const char *e = getenv("AR_FORCE18");
      f18 = e ? (int)strtoul(e, NULL, 0) : -1; }
    if (f18 >= 0) { extern uint8 g_ram[0x20000]; g_ram[0x18] = (uint8)f18; }
  }

  /* AR_FRAMELOG=1: at each vblank yield, report how much game code ran since the
   * previous yield (push delta) plus the key action-engine RAM bytes. A large,
   * steady push delta with $E6 (time) ticking = engine running. A tiny push delta
   * = the main loop is spinning on the vblank wait WITHOUT running per-frame logic
   * (dispatch/gate problem). $E6 frozen while pushes are large = logic runs but a
   * pause/timer gate is suppressing advancement. Action fields also expose the
   * actual movement result: position delta, velocity, current player handler/
   * flags, and the walking-cycle Crest/Boost counters ($08BC/$08C4). */
  if (getenv("AR_FRAMELOG")) {
    extern unsigned long g_recomp_push_count;
    extern uint8 g_ram[0x20000];
    extern Snes *g_snes;
    static unsigned long last_push;
    /* return frame is at pre-pop S (we already did S+=2 above) */
    uint16 sp = (uint16)(cpu->S - 2);
    uint16 ret = (uint16)(((g_ram[(uint16)(sp + 2)] << 8) | g_ram[(uint16)(sp + 1)]) + 1);
    /* joypad raw + SwapInputBits'd (same order AR_MOONJUMP reads) -- added
     * 2026-07-01 for the sim-mode freeze investigation: correlates whether
     * input is even reaching the frame against which per-frame path fires
     * (see AR_SIMTRACE in cpu_trace.h). */
    uint16 joy_raw = g_snes->input1_currentState;
    uint16 joy = SwapInputBits(joy_raw);
    uint16 gf = (uint16)(g_ram[0x88] | (g_ram[0x89] << 8));
    uint16 px = (uint16)(g_ram[0x08A2] | (g_ram[0x08A3] << 8));
    uint16 py = (uint16)(g_ram[0x08A4] | (g_ram[0x08A5] << 8));
    int16 pvx = (int16)(g_ram[0x08A6] | (g_ram[0x08A7] << 8));
    int16 pvy = (int16)(g_ram[0x08A8] | (g_ram[0x08A9] << 8));
    uint16 ph = (uint16)(g_ram[0x08B2] | (g_ram[0x08B3] << 8));
    uint16 ps = (uint16)(g_ram[0x08D0] | (g_ram[0x08D1] << 8));
    static uint16 last_px, last_py;
    static uint8 last_region, last_map;
    int dx = 0, dy = 0;
    if (g_ram[0x18] >= 0x01 && g_ram[0x18] <= 0x07 &&
        last_region == g_ram[0x18] && last_map == g_ram[0x19]) {
      dx = (int16)(px - last_px);
      dy = (int16)(py - last_py);
    }
    fprintf(stderr,
      "[frame] f=%d gf=%u push+%lu callsite=%02x:%04x A=%04x m=%d $18=%02x $19=%02x $1A=%02x $1B=%02x $F4=%02x $F5=%02x $FB=%02x time$E6=%02x%02x HP$1D=%02x joy=%04x(raw=%04x) pos=%04x,%04x d=%+d,%+d vel=%+d,%+d h=%04x state=%04x boost=%02x crest=%02x\n",
      snes_frame_counter, gf, g_recomp_push_count - last_push, cpu->PB, ret,
      cpu->A, cpu->m_flag,
      g_ram[0x18], g_ram[0x19], g_ram[0x1A], g_ram[0x1B],
      g_ram[0xF4], g_ram[0xF5], g_ram[0xFB], g_ram[0xE7], g_ram[0xE6], g_ram[0x1D],
      joy, joy_raw, px, py, dx, dy, pvx, pvy, ph, ps,
      g_ram[0x08C4], g_ram[0x08BC]);
    last_px = px;
    last_py = py;
    last_region = g_ram[0x18];
    last_map = g_ram[0x19];
    last_push = g_recomp_push_count;
  }

  /* AR_OBJLOG=1: per-frame action-stage object-table + timer health. Logs the
   * game-frame, timer ($E6/$E7), player HP ($1D), and the first few object
   * slots' status word ($06A0 stride $40) + handler ptr ($12). Reveals the
   * exact frame the object table is wiped / timer goes non-BCD (the "sprites
   * vanish + timer '?'" corruption). */
  if (getenv("AR_OBJLOG")) {
    extern uint8 g_ram[0x20000];
    if (g_ram[0x18] >= 0x01 && g_ram[0x18] <= 0x07) {
      unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
      int active = 0;
      for (int i = 0; i < 24; i++) {
        unsigned b = 0x06A0 + i * 0x40;
        unsigned sw = g_ram[b] | (g_ram[b + 1] << 8);
        if (!(sw & 0x8000) && !(sw & 0x4000)) active++;
      }
      unsigned o0 = g_ram[0x06A0] | (g_ram[0x06A1] << 8);
      unsigned o0h = g_ram[0x06B2] | (g_ram[0x06B3] << 8);
      fprintf(stderr, "[obj] gf=%u timer=%02x%02x HP=%02x active=%d obj0.sw=%04x obj0.h=%04x\n",
              gf, g_ram[0xE7], g_ram[0xE6], g_ram[0x1D], active, o0, o0h);
    }
  }

  /* AR_CTACTION=1: one-shot full call trace of a single stuck action-mode frame.
   * State machine: arm tracing for the next inter-yield batch the first time we
   * see $18==1, then disarm at the following yield. Captures exactly the ~45
   * functions of the frozen action loop. */
  /* AR_PPULOG=1: per-frame display state — INIDISP (brightness + forced-blank),
   * BG mode, and main/sub screen layer-enable masks. A black screen with the
   * game running (no freeze) is usually forced-blank set, brightness 0, or all
   * main-screen layers disabled. */
  if (getenv("AR_PPULOG")) {
    extern Ppu *g_ppu;
    static int lf = -1;
    if (snes_frame_counter != lf) {
      lf = snes_frame_counter;
      extern uint8 g_snesrecomp_last_hdmaen;
      fprintf(stderr, "[ppu] f=%d inidisp=%02x bright=%d fblank=%d bgmode=%02x main=%02x sub=%02x hdmaen=%02x\n",
              snes_frame_counter, g_ppu->inidisp, g_ppu->inidisp & 0xf,
              (g_ppu->inidisp & 0x80) ? 1 : 0, g_ppu->bgmode,
              g_ppu->screenEnabled[0], g_ppu->screenEnabled[1],
              g_snesrecomp_last_hdmaen);
    }
  }

  if (getenv("AR_CTACTION")) {
    /* End of an inter-yield batch: flush it if it contained the corrupting
     * 8465_M0X0, else discard and start fresh. Captures the exact m=1->0 frame. */
    extern void RecompBatchYield(void);
    RecompBatchYield();
  }

  ActRaiser_YieldToHost();
  return RECOMP_RETURN_NORMAL;
}

/* Per-frame widescreen policy — the single seam where game mode decides how
 * much of the extra-column budget (g_ws_extra, set at startup from
 * ExtendedAspectRatio) is visible this frame. Phase 1: pillarbox everywhere
 * (authentic 256 columns centered); later phases widen per mode via
 * $18/$19 and clamp per camera/level bounds with PpuSetExtraSideSpace.
 * Must run every frame: ppu_reset zeroes the PPU margin fields.
 * AR_WS_SURVEY=1 forces raw symmetric margins in EVERY mode — the Phase-2
 * artifact-survey knob (stale tiles/pop-in expected; not for normal play). */
static void ActRaiser_ApplyWidescreenPolicy(void) {
  extern bool g_ws_active;
  extern int g_ws_extra;
  if (!g_ws_active) return;
  static int survey = -1;
  if (survey < 0) {
    const char *e = getenv("AR_WS_SURVEY");
    survey = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  /* Per-mode widescreen policy (docs/widescreen-survey.md). Two knobs per
   * mode: (1) does it use the wide view at all, and (2) a per-layer clamp
   * mask (bit L keeps BG(L+1) at 256) for scenes that mix wide world layers
   * with 256-wide UI/dialog layers whose offscreen tilemap data must not tile
   * into the margins. We keep the classification explicit here (not an
   * auto-heuristic) so we don't touch game code while proving the base recomp
   * accurate — a mis-widened layer is a policy line, not a decode bug.
   * BG3 (layer 2, the HUD) is already margin-clamped by the engine default. */
  int wide = survey;
  uint8 clamp = 0;
  uint8 mirror = 0;
  uint8 repeat = 0;
  int repeat_band_layer = -1;
  uint8 repeat_band_y0 = 0, repeat_band_y1 = 0;
  int bg2_gap = 0;  /* margin source gap px/side for BG2 (UI staging strip) */
  uint8 hud_split_height = 0;
  uint8 hud_split_left_end = 0;
  uint8 hud_split_right_start = 0;
  uint8 hud_left_only_y = 0;
  /* True when the current wide world has finite horizontal bounds. The PPU
   * still owns the fixed centering budget; this policy narrows the live left
   * and right margins as the camera approaches either world edge. */
  int bounded_world_margins = 0;
  if (!survey && g_ram[0x18] == 0x00) {
    switch (g_ram[0x19]) {
      case 0x07:            /* Sky Palace hub. BG1 = sky/clouds (wide, clean).
                               BG2 = pillars plus game-owned offscreen dialog
                               staging farther around its 64x64 tilemap. Keep
                               BG2 raw-wide; the render transaction decodes a
                               box-free ROM source into only margin columns.
                               The authentic center retains its BG2 box. */
        wide = 1;
        break;

      case 0x09:            /* Mode 7 world map: fully wide, no UI layers. */
        wide = 1; 
        break;
      
      case 0x00: 
        // Title is always not wide screen since the backdrop is black
        wide = 0;
        break;
    
      case 0x01:            /* Fillmore simulation town. */
      case 0x02:            /* Bloodpool simulation town. */
      case 0x03:            /* Kasandora simulation town. */
      case 0x04:            /* Aitos simulation town. */
      case 0x05:            /* Marahna simulation town. */
      case 0x06: {          /* Northwall simulation town. */
        /* $01:B4C6 clamps camera X ($22) to $0000-$0100, proving 256px of
         * world on either side of the authentic viewport. AR_WS_SIM=0 is the
         * same-binary authentic baseline for town regression captures. BG2 is
         * the bounded dialog/overlay plane and remains center-clamped. The
         * separate ADAD/AE6F and B473 ports use this same $01-$06 range. */
        wide = g_settings.ws_sim;
        bounded_world_margins = wide;
        clamp = 0x02;
        break;
      }

      case 0x08:
      // Temple cut scenes don't need wide screen support
      // background is black
        wide = 0;
        break;

      default:              /* title(00), temple cutscene(08),
                               transitions, unknown: pillarbox (the temple is a
                               black backdrop with no wide-worthy layer). */
        wide = 0;
        break;
    }
  } else if (!survey && g_ram[0x18] >= 0x01 && g_ram[0x18] <= 0x07) {
    /* Validated action-wide path, shared by all seven action-region handler
     * tables. Original tile streamers remain active; a separate host-side
     * transaction refreshes only BG tilemap VRAM margins with true map data,
     * while the audited $8C98/$8D68 seams widen drawing and activation.
     *
     * AR_WS_ACTION=0 restores the pillarboxed action baseline in the same
     * binary. AR_WS_BGREFRESH=0 returns exactly to Stage A (raw wide renderer,
     * stale/wrapped margin BG) without changing the action geometry. */
    wide = g_settings.ws_action;
    bounded_world_margins =
        wide && ActRaiser_WidescreenBgRefreshEnabled();

    /* Some action sections declare BG2 as a single 256x256 screen
     * ($32/$34=$0100) while BG1 is the scrolling world. There is no BG2 world
     * data to decode into side margins, and its offscreen tilemap half is
     * scratch/stale storage. The margin refresher therefore skips it; pad its
     * authentic render (or clamp it) instead of exposing frozen tiles. */
    uint16 bg2_width = (uint16)(g_ram[0x32] | (g_ram[0x33] << 8));
    if (bg2_width < 0x0200) {
      /* A 256-wide decorative BG2 has no real margin data. By default pad from
       * its authentic rendered image; AR_WS_BG2_MIRROR=0 restores the proven
       * clamp for A/B testing in the same binary.
       *
       * Bloodpool's mostly symmetric decoration benefits from reflection.
       * Aitos act 1 (raw maps $01-$03), Northwall maps $01-$05 and $08, and
       * Death Heim maps $02-$08 show moving cloud/snow/mountain bands; reflection
       * reverses their slope/motion at each boundary. Cyclically repeat the
       * already-rendered scanline there so every parallax/raster band continues
       * in the same direction without exposing stale BG2 VRAM. */
      int bg2_mirror = g_settings.ws_bg2_padding;
      if (!bg2_mirror) {
        clamp |= 0x02;
      } else if ((g_ram[0x18] == 0x04 &&
                  g_ram[0x19] >= 0x01 && g_ram[0x19] <= 0x03) ||
                 (g_ram[0x18] == 0x06 &&
                  ((g_ram[0x19] >= 0x01 && g_ram[0x19] <= 0x05) ||
                   g_ram[0x19] == 0x08)) ||
                 (g_ram[0x18] == 0x07 &&
                  g_ram[0x19] >= 0x02 && g_ram[0x19] <= 0x08)) {
        repeat |= 0x02;
      } else {
        mirror |= 0x02;
      }
    }

    /* Bloodpool Act 1 ($02:$01) is another mixed-content narrow BG2. Its
     * static mountain silhouette benefits from the normal reflected margins,
     * but tile row 17 and below is animated water. Reflecting those lower
     * scanlines reverses the apparent flow at both authentic-screen seams.
     * Keep reflection above y=136 and cyclically continue the already-rendered
     * water scanline below it. When AR_WS_BG2_MIRROR=0 selected the authentic
     * clamp fallback, leave the repeat band disabled as part of that A/B gate. */
    if (wide && g_ram[0x18] == 0x02 && g_ram[0x19] == 0x01 &&
        (mirror & 0x02)) {
      repeat_band_layer = 1;  /* BG2 */
      repeat_band_y0 = 136;
      repeat_band_y1 = 224;
    }

    /* Death Heim's boss-warp room ($07:$01) is already composed as a bounded
     * SNES-width scene: BG1 is the central causeway, while BG2 contains both
     * the face statues and the animated fog/water. Open the full symmetric
     * canvas despite camera X=0. During the boss rush, clamp both scenery
     * layers and cyclically continue only BG2's border/fog rows; the split at
     * tile row 18 (screen Y=144) is below every face and above the water.
     *
     * After the final boss, $0347=$07. The fade-to-black/removal sequencer at
     * $00:F5C2-$F5EF then selects the sky maps by writing BG1SC/BG2SC=$64/$74
     * at $F5F0-$F619, before its fade-in. Use those live page bases as the
     * exact render handoff; $0334=$03 is retained as a settled-state fallback
     * but is written much later at $F650. Keep the causeway BG1 bounded, but
     * mirror the whole live BG2 scanline so the non-periodic cloud edges join
     * cleanly at both margins. */
    if (wide && g_ram[0x18] == 0x07 && g_ram[0x19] == 0x01) {
      const int ending_sky_pages =
          (g_ppu->bgXsc[0] & 0xfc) == 0x64 &&
          (g_ppu->bgXsc[1] & 0xfc) == 0x74;
      bounded_world_margins = 0;
      if (g_ram[0x0347] >= 0x07 &&
          (ending_sky_pages || g_ram[0x0334] >= 0x03)) {
        clamp |= 0x01;
        mirror |= 0x02;
      } else {
        clamp |= 0x03;
        repeat_band_layer = 1;  /* BG2 */
        repeat_band_y0 = 144;
        repeat_band_y1 = 224;
      }
    }

    /* Death Heim's final arena ($07:$08) stacks two 256px star-road layers
     * and applies independent scanline/sine motion. Both live BGSC registers
     * select 32x32 tilemaps ($60/$70), so native tile fetch already wraps at
     * 256px with the current per-line scroll. The generic world-edge budget
     * was the only reason the margins stayed black. Open the symmetric canvas
     * and draw both layers raw: this preserves their raster phase and avoids
     * the isolated-buffer clear/merge cost of presentation-layer repeat. */
    if (wide && g_ram[0x18] == 0x07 && g_ram[0x19] == 0x08) {
      bounded_world_margins = 0;
      clamp &= (uint8)~0x03;
      mirror &= (uint8)~0x03;
      repeat &= (uint8)~0x03;
    }
  }
  /* AR_WS_ONLYBG=N (1..4): isolate a single BG layer for capture — masks the
   * main-screen enable to just that layer so a snapshot shows exactly which
   * layer carries the sky / dialog / pillars. Temporary Phase-4 probe. */
  { const char *ob = getenv("AR_WS_ONLYBG");
    if (ob && ob[0]) {
      int L = atoi(ob) - 1;
      if (L >= 0 && L < 4) g_ppu->screenEnabled[0] = (uint8)(1u << L);
      wide = 1; clamp = 0; mirror = 0; repeat = 0;  /* raw tilemap data */
      repeat_band_layer = -1;
    } }
  /* AR_WS_CLAMP=<hex mask>: override the per-layer clamp for tuning. */
  { const char *cm = getenv("AR_WS_CLAMP");
    if (cm && cm[0]) {
      wide = 1; clamp = (uint8)strtoul(cm, NULL, 16);
      mirror = 0; repeat = 0;
      repeat_band_layer = -1;
    } }
  /* Capture presets are final policy overrides, intentionally after the
   * scene-specific rules and diagnostic clamp knob. This makes promotional
   * comparisons deterministic:
   *
   *   4:3  authentic centre 256, no HLE presentation
   *   RAW  full wide canvas, no clamp/pad/repeat/gap/world-edge correction
   *   FULL scene policy plus every HLE gate enabled by Settings_SetDisplayMode
   *
   * RAW must not inherit a sim BG2 clamp or an action finite-world margin just
   * because those policies are normally useful. The individual HLE builders
   * and sprite/activation seams are disabled by the RAW preset's ws_* flags. */
  if (g_settings.display_mode == kDisplayMode_43) {
    wide = 0; clamp = 0; mirror = 0; repeat = 0;
    bg2_gap = 0; bounded_world_margins = 0;
    repeat_band_layer = -1;
  } else if (g_settings.display_mode == kDisplayMode_WideRaw) {
    wide = 1; clamp = 0; mirror = 0; repeat = 0;
    bg2_gap = 0; bounded_world_margins = 0;
    repeat_band_layer = -1;
  }

  /* Host-overlay HUD layout. BG3 rows 0-3 are the live status compose band
   * ($7F:B000 -> VRAM $5800). The PPU extracts this band into a transparent
   * surface and the host independently scales/anchors its source chunks after
   * the game framebuffer has been upscaled. The 2026-07-14 action dump places
   * its three occupied groups at columns 0-10, 11-20, and 21-31. Simulation
   * has no centered status group, so it uses the two-way form: columns 0-20
   * anchor left and 21-31 anchor right. Selected-magic OAM is promoted
   * separately after its exact four-slot signature is validated below.
   *
   * RAW and 4:3 remain unsplit comparison modes. Sky Palace ($00:$07) uses
   * the same simulation header; temple/world-map flows do not. */
  if (!survey && wide &&
      g_settings.display_mode != kDisplayMode_43 &&
      g_settings.display_mode != kDisplayMode_WideRaw) {
    if (g_ram[0x18] >= 0x01 && g_ram[0x18] <= 0x07) {
      /* BG3's effective screen placement puts tilemap row 2 at y=20-27. That
       * row contains PLAYER health on the left and magic-scroll tiles on the
       * right, so it must retain the three-way split. Row 3 (ENEMY and its
       * long health bar) begins at y=28; anchor only that lower band left. */
      hud_split_height = 40;
      hud_split_left_end = 88;
      hud_split_right_start = 168;
      hud_left_only_y = 28;
    } else if (g_ram[0x18] == 0x00 &&
               g_ram[0x19] >= 0x01 && g_ram[0x19] <= 0x07) {
      hud_split_height = 32;
      hud_split_left_end = 168;
      hud_split_right_start = 168;
      hud_left_only_y = 32;
    }
  }

  /* The HUD split is persistent PPU policy state, unlike the layer-clamp
   * arrays reset by PpuSetExtraSpace. Clear it explicitly on every mode flip
   * path before optionally enabling this frame's prototype. */
  PpuSetWidescreenHudSplit(g_ppu, 0, 0, 0, 0);
  if (wide) {
    PpuSetExtraSpace(g_ppu, (uint8)g_ws_extra);
    PpuSetWidescreenLayerClamp(g_ppu, clamp);
    PpuSetWidescreenLayerMirror(g_ppu, mirror);
    PpuSetWidescreenLayerRepeat(g_ppu, repeat);
    if (repeat_band_layer >= 0)
      PpuSetWidescreenLayerRepeatBand(g_ppu, (uint8)repeat_band_layer,
                                      repeat_band_y0, repeat_band_y1);
    if (bg2_gap)
      PpuSetWidescreenLayerMarginGap(g_ppu, 1, (uint8)bg2_gap, (uint8)bg2_gap);
    if (hud_split_height)
      PpuSetWidescreenHudSplit(g_ppu, hud_split_height,
                               hud_split_left_end, hud_split_right_start,
                               hud_left_only_y);
    if (hud_split_height)
      PpuSetOverlayCapture(g_ppu, kPpuOverlaySource_Bg3,
                           0, 0, 256, hud_split_height,
                           kPpuOverlayFlag_RemoveFromGame);
    if (bounded_world_margins) {
      /* Clamp each side to real BG1 world space. Action width is section
       * state $2E; simulation towns are the fixed 512px world proven by
       * $01:B4C6's camera clamp. Outside [0,width) stays black. */
      int cam = (int)(uint16)(g_ram[0x22] | (g_ram[0x23] << 8));
      int width = (g_ram[0x18] == 0x00 &&
                   g_ram[0x19] >= 0x01 && g_ram[0x19] <= 0x06)
                      ? 0x0200
                      : (int)(uint16)(g_ram[0x2E] | (g_ram[0x2F] << 8));
      int room_l = cam;
      int room_r = width - 256 - cam;
      if (room_l < 0) room_l = 0;
      if (room_r < 0) room_r = 0;
      int ml = room_l < g_ws_extra ? room_l : g_ws_extra;
      int mr = room_r < g_ws_extra ? room_r : g_ws_extra;
      PpuSetExtraSideSpace(g_ppu, ml, mr, 0);
    }
  } else {
    PpuSetExtraSpaceCentered(g_ppu, (uint8)g_ws_extra);
    PpuSetWidescreenLayerClamp(g_ppu, 0);
  }
  /* One line per policy flip — cheap, and makes "why isn't this screen
   * wide?" diagnosable from any console.log (mode bytes included). */
  static int last_wide = -1, last_clamp = -1, last_mirror = -1,
             last_repeat = -1, last_repeat_band_layer = -2,
             last_repeat_band_y0 = -1, last_repeat_band_y1 = -1,
             last_hud_split_height = -1, last_hud_split_left_end = -1,
             last_hud_split_right_start = -1,
             last_hud_left_only_y = -1;
  if (wide != last_wide || clamp != last_clamp || mirror != last_mirror ||
      repeat != last_repeat ||
      repeat_band_layer != last_repeat_band_layer ||
      repeat_band_y0 != last_repeat_band_y0 ||
      repeat_band_y1 != last_repeat_band_y1 ||
      hud_split_height != last_hud_split_height ||
      hud_split_left_end != last_hud_split_left_end ||
      hud_split_right_start != last_hud_split_right_start ||
      hud_left_only_y != last_hud_left_only_y) {
    last_wide = wide;
    last_clamp = clamp;
    last_mirror = mirror;
    last_repeat = repeat;
    last_repeat_band_layer = repeat_band_layer;
    last_repeat_band_y0 = repeat_band_y0;
    last_repeat_band_y1 = repeat_band_y1;
    last_hud_split_height = hud_split_height;
    last_hud_split_left_end = hud_split_left_end;
    last_hud_split_right_start = hud_split_right_start;
    last_hud_left_only_y = hud_left_only_y;
    fprintf(stderr, "[widescreen] gf=%u $18=%02x $19=%02x -> %s "
            "clamp=%02x mirror=%02x repeat=%02x rband=%d/%u-%u "
            "hud=%u/%u/%u left-only-y=%u\n",
            (unsigned)(g_ram[0x88] | (g_ram[0x89] << 8)),
            g_ram[0x18], g_ram[0x19], wide ? "WIDE" : "pillarbox",
            clamp, mirror, repeat, repeat_band_layer,
            (unsigned)repeat_band_y0, (unsigned)repeat_band_y1,
            (unsigned)hud_split_height, (unsigned)hud_split_left_end,
            (unsigned)hud_split_right_start, (unsigned)hud_left_only_y);
  }
  /* AR_WS_LAYERS=1: dump per-frame PPU layer/tilemap state — which BG a
   * margin artifact lives on, and whether that BG's tilemap is 32-wide (wraps
   * into the margin) or 64-wide (real content). Temporary Phase-4 probe. */
  if (getenv("AR_WS_LAYERS")) {
    static int lf = -1;
    unsigned gf = (unsigned)(g_ram[0x88] | (g_ram[0x89] << 8));
    if ((int)gf != lf) {
      lf = (int)gf;
      fprintf(stderr, "[ws-layers] gf=%u mode=%d main=%02x sub=%02x wsel=%06x cgwsel=%02x cgadsub=%02x",
              gf, g_ppu->bgmode & 7, g_ppu->screenEnabled[0], g_ppu->screenEnabled[1],
              g_ppu->windowsel, g_ppu->cgwsel, g_ppu->cgadsub);
      for (int L = 0; L < 4; L++)
        fprintf(stderr, " BG%d[w%d h%02x hs=%d]", L + 1,
                (g_ppu->bgXsc[L] & 1), (g_ppu->bgXsc[L] & 0xfc),
                g_ppu->hScroll[L]);
      fprintf(stderr, " win1=[%d,%d] win2=[%d,%d]\n",
              g_ppu->window1left, g_ppu->window1right,
              g_ppu->window2left, g_ppu->window2right);
    }
  }
  /* The compositor writes only the active window. Finite action/town worlds
   * can leave steady gap strips at the framebuffer edges, so clear those gaps
   * every frame. Other modes retain the change-triggered full clear. */
  static int last_l = -1, last_r = -1;
  int l = g_ppu->extraLeftCur, r = g_ppu->extraRightCur;
  if (bounded_world_margins && g_ppu->renderBuffer) {
    int budget = g_ppu->extraLeftRight;
    int gap_l = budget - l;
    int gap_r = budget - r;
    size_t pitch = g_ppu->renderPitch;
    if (gap_l > 0)
      for (int y = 0; y < 224; y++)
        memset(g_ppu->renderBuffer + (size_t)y * pitch, 0,
               (size_t)gap_l * 4);
    if (gap_r > 0)
      for (int y = 0; y < 224; y++)
        memset(g_ppu->renderBuffer + (size_t)y * pitch +
                   ((size_t)budget + 256 + r) * 4,
               0, (size_t)gap_r * 4);
    last_l = l;
    last_r = r;
  } else if (l != last_l || r != last_r) {
    last_l = l;
    last_r = r;
    if (g_ppu->renderBuffer)
      memset(g_ppu->renderBuffer, 0, (size_t)g_ppu->renderPitch * 224);
  }
}

/* $00:923A emits the selected-magic icon as the first four OAM entries, using
 * tiles $D4-$D7 in a 2x2 grid at y=$0B/$13. Promote only that exact signature
 * into the host HUD object surface. No emulated OAM/WRAM state is changed. */
static void ActRaiser_WidescreenMagicHudPromote(void) {
  if (!g_ppu || g_ppu->wsHudSplitHeight != 40 ||
      g_ppu->wsHudLeftEnd != 88 || g_ppu->wsHudRightStart != 168 ||
      g_ppu->wsHudLeftOnlyY != 28 ||
      g_ram[0x18] < 0x01 || g_ram[0x18] > 0x07 ||
      !g_ppu->extraLeftRight)
    return;

  for (int slot = 0; slot < 4; slot++) {
    int index = slot * 2;
    uint8 tile = (uint8)g_ppu->oam[index + 1];
    uint8 y = (uint8)(g_ppu->oam[index] >> 8);
    uint8 expected_y = slot < 2 ? 0x0B : 0x13;
    if (tile != (uint8)(0xD4 + slot) || y != expected_y)
      return;
  }
  if (PpuSetOverlayCapture(g_ppu, kPpuOverlaySource_Obj,
                           0, 0, 256, 40,
                           kPpuOverlayFlag_RemoveFromGame))
    PpuSetOverlayOamRange(g_ppu, 0, 4);
}

void ActRaiserDrawPpuFrame(void) {
  /* Overlay bindings are host-owned and persistent; capture policy is
   * game-owned and rebuilt every frame so no prior mode can leak a region. */
  PpuClearOverlayCaptures(g_ppu);
  ActRaiser_ApplyWidescreenPolicy();
  /* Stage D reconnaissance: read-only classification of objects that intersect
   * a live side margin but remain outside the authentic activation window. */
  ActRaiser_WidescreenSpriteActivationProbe();
  /* Stage B: populate BG1/BG2 margin tilemap cells transactionally before
   * scanline rendering. No-op under AR_WS_BGREFRESH=0 and outside action. */
  ActRaiser_WidescreenMarginRefresh();
  /* Sky Palace: synthesize only BG2's offscreen margin columns from its ROM
   * source page. The paired restore after scanout preserves UI staging. */
  ActRaiser_WidescreenSkyPalacePrepare();
  ActRaiser_WidescreenMagicHudPromote();
  /* Process ALL 8 HDMA channels, not a fixed subset. ActRaiser drives its
   * per-scanline effects (e.g. the Mode-7 title matrix animation) on channels
   * 2/3 — the old code only ran 5/6/7 (a stale assumption carried over from
   * another game's port), so those HDMA tables were started by HDMAEN but
   * never applied, leaving the title raster effect dead. SimpleHdma_Init
   * early-returns on inactive channels, so iterating 0..7 is a safe no-op for
   * channels the game isn't using this frame. */
  SimpleHdma hdma_chans[8];
  Dma *dma = g_dma;

  dma_startDma(dma, g_snesrecomp_last_hdmaen, true);

  for (int ch = 0; ch < 8; ch++)
    SimpleHdma_Init(&hdma_chans[ch], &dma->channel[ch]);

  int trigger = g_snes->vIrqEnabled ? g_snes->vTimer + 1 : -1;

  for (int i = 0; i <= 224; i++) {
    ppu_runLine(g_ppu, i);
    for (int ch = 0; ch < 8; ch++)
      SimpleHdma_DoLine(&hdma_chans[ch]);
    if (i == trigger) {
      g_snes->inIrq = true;
      CpuRegSnapshot snap;
      ActRaiser_SaveRegs(&g_cpu, &snap);
      cpu_push_interrupt_frame(&g_cpu);
      g_ar_in_interrupt = 1;
      IrqHandler_M1X1(&g_cpu);
      g_ar_in_interrupt = 0;
      ActRaiser_RestoreRegs(&g_cpu, &snap);
      trigger = g_snes->vIrqEnabled ? g_snes->vTimer + 1 : -1;
    }
  }
  ActRaiser_WidescreenSkyPalaceRestore();
}

/* Reload the selector-dependent part of the action OBJ atlas after a live
 * magic selection change. The native level-entry loader $02:BC9E copies 128
 * words from $06:A400 + (selector-1)*$80 to VRAM $2D40. Merely changing
 * $02AC during an action stage would therefore run the new spell with the old
 * spell's resident tiles and produce a misleading graphics failure. This
 * targeted host copy reproduces only that selector-dependent upload; the
 * common atlas and palettes remain untouched. */
static void ActRaiser_ReloadSelectedMagicTiles(uint8 selector) {
  if (selector < 1 || selector > 4) return;
  uint16 source = (uint16)(0xA400 + (selector - 1) * 0x80);
  for (uint16 word = 0; word < 0x80; word++)
    g_ppu->vram[0x2D40 + word] =
        cpu_read16(&g_cpu, 0x06, (uint16)(source + word * 2));
}

/* AR_MAGIC_CYCLE=1 reserves one SNES button as an action-mode spell-cycle
 * edge. The mask is in the auto-joypad word returned by SwapInputBits;
 * AR_MAGIC_CYCLE_BTN defaults to $0020 = L ($0010 = R). This stays env-backed
 * for now so it can be used while the descriptor/overlay work is in flight.
 * The configured input is consumed before NMI samples the controller, making
 * arbitrary remaps safe instead of also triggering their original action. */
static void ActRaiser_ApplyMagicCycle(void) {
  static int enabled = -1;
  static uint16 button_mask;
  static uint16 previous_buttons;
  if (enabled < 0) {
    const char *e = getenv("AR_MAGIC_CYCLE");
    const char *b = getenv("AR_MAGIC_CYCLE_BTN");
    enabled = e && e[0] && e[0] != '0';
    button_mask = (b && b[0]) ? (uint16)strtoul(b, NULL, 0) : 0x0020;
    if (!button_mask) enabled = 0;
  }

  uint16 buttons = SwapInputBits(g_snes->input1_currentState);
  uint16 pressed = (uint16)(buttons & button_mask);
  uint16 was_pressed = (uint16)(previous_buttons & button_mask);
  previous_buttons = buttons;

  if (!enabled || g_ram[0x18] < 0x01 || g_ram[0x18] > 0x07)
    return;

  /* Reserve/consume the configured button for this aid. SwapInputBits is its
   * own inverse, converting the SNES auto-joypad mask back to runner input. */
  g_snes->input1_currentState &= (uint16)~SwapInputBits(button_mask);
  if (!pressed || was_pressed) return;

  if (g_ram[0x00F8] != 0) {
    fprintf(stderr, "[magic-cycle] cast still active; selection unchanged\n");
    return;
  }

  static const char *const names[] = {
    "none", "Magical Fire", "Magical Stardust", "Magical Aura",
    "Magical Light"
  };
  uint8 current = g_ram[0x02AC];
  uint8 next = (current >= 1 && current < 4) ? (uint8)(current + 1) : 1;
  g_ram[0x02AC] = next;
  ActRaiser_ReloadSelectedMagicTiles(next);
  fprintf(stderr, "[magic-cycle] selected %u/4: %s ($02AC=$%02X, "
          "VRAM $2D40 refreshed)\n", (unsigned)next, names[next], next);
}

/* Host-side cheat hooks (debug-menu scaffold). All settings-gated and seeded
 * OFF from their legacy env names, so they never affect a normal run. Applied
 * once per frame at the START of RunOneFrameOfGame (before the game's frame
 * logic), so a value pinned here is what the frame sees -> effective for death
 * prevention (HP) and physics override (moonjump). RAM is g_ram (WRAM): low
 * direct-page addrs map 1:1 ($1D player HP, $E6/$E7 timer, player object $08A0).
 * This is the framework the planned debug menu plugs into — see
 * docs/SEAMS.md "Gameplay / Tunable seams" + memory debug-menu-warp-roadmap. */
void ActRaiser_ApplyCheats(void) {
  extern uint8 g_ram[0x20000];

  /* AR_PIN=<parcode>[,<parcode>...] — generic PAR/ZSNES cheat-code pinner
   * (2026-07-06). Each code is the standard 8-hex-digit PAR form BBAAAAVV
   * (bank $7E/$7F, 16-bit addr, byte value), applied every frame in EVERY
   * mode (unlike the mode-gated hand cheats below). Turns the whole
   * ./codes.txt catalogue (flamingspinach's 88 engineered codes — see
   * docs/ram-map.md "Cheat-derived WRAM map") into ready-made debug cheats
   * AND address-mapping probes with zero per-cheat C. Example:
   *   AR_PIN=7E00210A,7E029901   (INF MP + HAVE FIRE — the §7.18 kit)
   * Bad tokens are reported once and skipped. Max 32 pins. */
  for (int i = 0; i < g_settings.pin_count; i++)
    g_ram[g_settings.pins[i].off] = g_settings.pins[i].val;

  /* Action-stage gameplay tweaks only. $18 = region/mode: 01-07 = an action
   * stage (region N); 00 = intro/overworld, 08 = sim, $20+ = transitions. Gate
   * on the whole action range so cheats persist across ALL regions, not just
   * Fillmore ($18==01) — that bug disabled them after warping to region 2+. The
   * player object/HP/timer fields are shared by the action engine across every
   * region, so the same writes apply everywhere. */
  /* ── ALL-MODE cheats (above the action-stage gate: they feed the sim-mode
   * equip menu / angel, so they must pin in every mode) ─────────────────── */

  /* AR_ALL_MAGIC=1: unlock all four spells. HAVE flags $0299-$029C = 01/02/03/04
   * (cheat-map values, docs/ram-map.md). Pinned in ALL modes so the sim-mode
   * equip menu lists them; SELECTING one still goes through the menu (the equip
   * routine $01:915D derives $02AC from these). DEBUG.md #18 has the full
   * magic wiring map. */
  {
    if (g_settings.cheat_all_magic) {
      g_ram[0x0299] = 0x01;   /* Magical Fire */
      g_ram[0x029A] = 0x02;   /* Magical Stardust */
      g_ram[0x029B] = 0x03;   /* Magical Aura */
      g_ram[0x029C] = 0x04;   /* Magical Light */
    }
  }

  /* AR_RANGED_SWORD=1: sword fires a projectile ($E4 = $80, PAR 7E00E480). */
  if (g_settings.cheat_ranged_sword) g_ram[0x00E4] = 0x80;

  /* AR_INF_MP: infinite magic scrolls. =1 -> pin the WORKING count $21 to 10
   * (PAR 7E00210A); =<n> -> pin to n. Deliberately does NOT touch the
   * PERSISTENT count $0295 (DEBUG.md #18b: $21 is the act-mode working copy,
   * loaded from $0295 at $02:84E0) so the cheat never bakes into save.srm. */
  if (g_settings.cheat_inf_mp)
    g_ram[0x21] = (uint8)g_settings.cheat_inf_mp;

  /* AR_INF_SP=1: infinite sim-mode SP (miracle points). Self-calibrating: pins
   * current SP $0282/16 to max SP $0284/16 once max is known (vs the PAR code's
   * blunt $FF, which over-fills early-game maxima). */
  {
    if (g_settings.cheat_inf_sp && (g_ram[0x0284] | g_ram[0x0285])) {
      g_ram[0x0282] = g_ram[0x0284];
      g_ram[0x0283] = g_ram[0x0285];
    }
  }

  /* AR_ANGEL_HP=1: infinite sim-mode angel health. Self-calibrating: pins
   * current HP $0286 to max HP $0287 (the PAR code 7E028608 hardcodes 8, which
   * would UNDER-fill after level-ups raise the max). */
  if (g_settings.cheat_angel_hp && g_ram[0x0287])
    g_ram[0x0286] = g_ram[0x0287];

  if (g_ram[0x18] < 0x01 || g_ram[0x18] > 0x07) return;

  /* Live action-stage magic selection/asset reload for effect testing. */
  ActRaiser_ApplyMagicCycle();

  /* AR_INF_HP: infinite health. =1 -> auto: pin player HP ($1D) to the
   * high-water max seen this stage (self-calibrates to "full" once you've been
   * at full, so we needn't know max HP statically). =<n> -> pin to literal n. */
  {
    static int prior_mode;
    static unsigned hi;
    int mode = g_settings.cheat_inf_hp;
    if (mode != prior_mode) {
      /* Entering/re-entering auto mode must calibrate from the current stage,
       * not reuse a high-water value captured before a live toggle. */
      if (mode == 1) hi = 0;
      prior_mode = mode;
    }
    if (mode) {
      if (mode > 1) { g_ram[0x1D] = (uint8)mode; }
      else { unsigned hp = g_ram[0x1D];
        if (hp > hi && hp <= 0xFF) hi = hp;
        if (hi) g_ram[0x1D] = (uint8)hi; }
    }
  }

  /* AR_FREEZE_TIMER=1: pin the action-stage timer ($E6/$E7, BCD) to its first
   * captured value -> infinite time.
   *
   * Backs off automatically once the boss-defeat point-tally sequence starts
   * draining the timer: normal countdown never drops the BCD value by more
   * than 1 (roughly once per real-time second), so any single-frame drop
   * bigger than that can only be the drain script deliberately driving the
   * timer down, not the stage clock. When that happens, stop re-pinning and
   * let the game own the timer for the rest of THIS stage -- otherwise the
   * frozen timer blocks the drain and the boss->sim transition never
   * completes. No separate "boss defeated" flag needed; the abnormal
   * decrement rate IS the signal (2026-07-01). Latch resets on region change
   * ($18) so a fresh stage re-arms the freeze instead of staying stuck off
   * from a previous boss fight. */
  {
    static uint8 fe6, fe7; static int got;
    static int latched_off; static uint8 last_region = 0xFF;
    static int was_enabled;
    if (!g_settings.cheat_freeze_timer) {
      /* A future live off/on toggle starts a fresh capture and drain latch. */
      was_enabled = 0;
      got = 0;
      latched_off = 0;
      last_region = 0xFF;
    } else {
      if (!was_enabled) {
        was_enabled = 1;
        got = 0;
        latched_off = 0;
        last_region = 0xFF;
      }
      if (g_ram[0x18] != last_region) {
        last_region = g_ram[0x18]; got = 0; latched_off = 0;
      }
      if (!got) { fe6 = g_ram[0xE6]; fe7 = g_ram[0xE7]; got = 1; }
      if (!latched_off) {
        int pinned = (fe6 & 0x0F) + ((fe6 >> 4) & 0x0F) * 10
                   + ((fe7 & 0x0F) + ((fe7 >> 4) & 0x0F) * 10) * 100;
        int cur = (g_ram[0xE6] & 0x0F) + ((g_ram[0xE6] >> 4) & 0x0F) * 10
                + ((g_ram[0xE7] & 0x0F) + ((g_ram[0xE7] >> 4) & 0x0F) * 10) * 100;
        if (pinned - cur > 1) {
          latched_off = 1;   /* drain detected -- let the game own it from here */
        } else {
          g_ram[0xE6] = fe6; g_ram[0xE7] = fe7;
        }
      }
    }
  }

  /* AR_MOONJUMP=1: hold the jump button to FLY UP. We move the player
   * Y-POSITION ($08A4, +$04) directly rather than the Y-velocity ($08A8, +$08) —
   * object fields are polymorphic by state, so $08A8 is "Y-velocity" only in the
   * AIR state; while grounded it means something else (writing it there didn't
   * launch and leaked into other movement). Position is always position, so
   * decrementing $08A4 (screen-Y grows downward, so −Y = up) is a reliable
   * state-independent fly. =<n> sets up-speed in pixels/frame (default 6).
   *   AR_MOONJUMP_BTN=<hexmask> -> button mask vs the auto-joypad word
   *      (default 0x8000 = B; SwapInputBits order). Override without a rebuild
   *      if B isn't jump (verify bits with AR_JOYLOG=1). */
  {
    if (g_settings.cheat_moonjump_speed) {
      extern Snes *g_snes;
      uint16 buttons = SwapInputBits(g_snes->input1_currentState);
      if (buttons & g_settings.cheat_moonjump_button) {
        uint16 y = (uint16)(g_ram[0x08A4] | (g_ram[0x08A5] << 8));
        y = (uint16)(y - g_settings.cheat_moonjump_speed); /* −Y = up */
        g_ram[0x08A4] = (uint8)(y & 0xFF);
        g_ram[0x08A5] = (uint8)((y >> 8) & 0xFF);
      }
    }
  }

  /* AR_NO_KNOCKBACK=1: permanent invuln -> no hit registers -> no damage, no
   * knockback, no hitstun (speedrun "ignore hits"), using the game's own
   * i-frames. The hit-check gates on the INVULN FLAG ($08D0 bit 0x2000, +$30),
   * which the game sets on a hit and clears when the i-frame TIMER ($08C6, +$26)
   * counts down to 0. This authentic invulnerability state also suppresses
   * water drag; disable the cheat for movement/terrain-physics validation.
   * So we (a) pin the timer to 0xFF so the game never clears
   * the flag, and (b) SET the flag ourselves each frame so invuln is active from
   * frame one (without needing a first hit to bootstrap it — that was the
   * "works only after getting hit once" gap). Offsets found via AR_WATCHOBJ=08A0
   * while taking a hit. AR_NO_KNOCKBACK=<hexoff> (other than 1) instead raw-pins
   * $08A0+off to 0xFF for experimentation. */
  {
    static int prior_mode;
    int mode = g_settings.cheat_no_knockback;
    if (prior_mode == 1 && mode != 1) {
      /* Release only the two fields owned by full-invulnerability mode. Raw
       * experimental offset pins are intentionally not guessed/restored. */
      g_ram[0x08C6] = 0;
      g_ram[0x08D1] &= (uint8)~0x20;
    }
    prior_mode = mode;
    if (mode) {
      if (mode == 1) {
        g_ram[0x08C6] = 0xFF;     /* pin i-frame timer (+$26) so flag never clears */
        /* MAGIC EXCEPTION (2026-07-07, DEBUG.md #18): the cast gate ($00:9843 ->
         * $00:9DE1) does BIT #$2008 on player state $08D0 and refuses to cast
         * while the invuln flag ($2000) is set -- pinning it unconditionally made
         * magic permanently dead. Lift the pin ONLY when a cast will actually
         * fire this frame, i.e. when every condition of the game's own gate
         * holds: cast button held ($00A0 & $C0 -- the NMI joypad shadow of
         * $4218&$F4, bit7=A bit6=X, level-sensitive so the 1-frame NMI lag is
         * fine), no cast in progress ($F8==0), magic equipped ($02AC!=0), and
         * MP available ($21>0). The instant the cast starts the game sets $F8
         * nonzero -> the pin snaps back ON for the whole cast animation.
         * Residual vulnerability: only the 1-2 frames between press and cast
         * start; holding the button with no magic/MP no longer drops invuln
         * at all (the old version was vulnerable the entire time the button
         * was held). */
        int cast_imminent = (g_ram[0x00A0] & 0xC0) != 0
                         && g_ram[0xF8] == 0
                         && g_ram[0x02AC] != 0
                         && g_ram[0x21] != 0;
        if (cast_imminent)
          g_ram[0x08D1] &= (uint8)~0x20;
        else
          g_ram[0x08D1] |= 0x20;  /* set invuln flag $08D0 bit 0x2000 (+$30) */
      } else {
        g_ram[0x08A0 + (unsigned)mode] = 0xFF;
      }
    }
  }
}

/* Level-warp: stage the game's OWN sim->act transition to a chosen region/map,
 * bypassing the (broken) sim-mode UI. The intro/overworld stages an act entry by
 * writing the transition-DEST vars + a request flag, which the transition
 * processor then consumes (full fade + level-load + mode switch). Observed entry
 * into Fillmore act 1: $1B=01 (-> $18 region), $1A=01 (-> $19 act), $FB|=0x80
 * (request). We replicate that. Best triggered from a transition-capable state
 * (the intro, $18==00, which WORKS — unlike the post-act sim cascade). Hooked to
 * F6 in main.c with the raw target from AR_WARP=<region_hex><map_hex>. The map
 * byte is written directly to $19 and is not a uniform act number. */
void ActRaiser_Warp(unsigned region, unsigned map) {
  extern uint8 g_ram[0x20000];
  uint8 from_region = g_ram[0x18];
  uint8 from_map = g_ram[0x19];
  g_ram[0x1B] = (uint8)region;   /* -> $18 (region/mode) on the switch */
  g_ram[0x1A] = (uint8)map;      /* -> $19 (raw map/sub-flow within region) */
  g_ram[0xFB] |= 0x80;           /* transition-request flag */
  fprintf(stderr, "[warp] from $18=$%02X $19=$%02X staged region=$%02X "
          "map=$%02X ($1B/$1A/$FB set); transition processor should pick it up.\n",
          from_region, from_map, region & 0xFF, map & 0xFF);
  if (from_region >= 0x01 && from_region <= 0x07) {
    fprintf(stderr, "[warp] WARNING: action->action is not a naturally observed "
            "transition; inherited timing/object state may affect fidelity.\n");
  }
}

void RunOneFrameOfGame(void) {
  if (!g_game_started) {
    g_game_started = true;
    g_game_stack = malloc(GAME_STACK_SIZE);
    if (!g_game_stack) {
      fprintf(stderr, "Failed to allocate game coroutine stack\n");
      return;
    }
    getcontext(&g_game_ctx);
    g_game_ctx.uc_stack.ss_sp = g_game_stack;
    g_game_ctx.uc_stack.ss_size = GAME_STACK_SIZE;
    g_game_ctx.uc_link = &g_host_ctx;
    makecontext(&g_game_ctx, game_coroutine, 0);
  }

  ActRaiser_ApplyCheats();   /* host-side cheats (live settings, default off) */

  { extern int ar_trace_active(void); extern void ar_trace_frame(const char *);
    if (ar_trace_active()) ar_trace_frame("vblank"); }  /* frame boundary marker */
  g_snes->forceNmi = true;
  g_snes->nmiAvail = true;   /* fresh RDNMI ($4210 bit7) vblank token this frame */
  swapcontext(&g_host_ctx, &g_game_ctx);
  g_snes->forceNmi = false;

  g_snes->inNmi = true;
  /* NmiHandler ends in RTI, which pops a hardware interrupt frame
   * (P/PC/PB). Push the matching frame first — otherwise the RTI
   * over-pops the stack and loads garbage into cpu->P, corrupting the
   * M/X width flags of the interrupted game code. Symmetric to the
   * IRQ path in ActRaiserDrawPpuFrame. */
  {
    CpuRegSnapshot snap;
    ActRaiser_SaveRegs(&g_cpu, &snap);
    cpu_push_interrupt_frame(&g_cpu);
    g_ar_in_interrupt = 1;
    { extern int ar_trace_active(void); extern void ar_trace_frame(const char *);
      if (ar_trace_active()) ar_trace_frame("nmi"); }
    NmiHandler_M1X1(&g_cpu);
    g_ar_in_interrupt = 0;
    ActRaiser_RestoreRegs(&g_cpu, &snap);
  }
}
