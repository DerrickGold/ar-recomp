#define _XOPEN_SOURCE 600
#include "actraiser_rtl.h"
#include "variables.h"
#include "common_cpu_infra.h"
#include "snes/snes.h"
#include "cpu_state.h"
#include "funcs.h"
#include "cpu_trace.h"
#include <stdio.h>
#include <stdbool.h>
#include <ucontext.h>
#include <stdlib.h>

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
   * pause/timer gate is suppressing advancement. */
  if (getenv("AR_FRAMELOG")) {
    extern unsigned long g_recomp_push_count;
    extern uint8 g_ram[0x20000];
    static unsigned long last_push;
    /* return frame is at pre-pop S (we already did S+=2 above) */
    uint16 sp = (uint16)(cpu->S - 2);
    uint16 ret = (uint16)(((g_ram[(uint16)(sp + 2)] << 8) | g_ram[(uint16)(sp + 1)]) + 1);
    fprintf(stderr,
      "[frame] f=%d push+%lu callsite=%02x:%04x A=%04x m=%d $18=%02x $19=%02x $1A=%02x $1B=%02x $F4=%02x $F5=%02x $FB=%02x time$E6=%02x%02x HP$1D=%02x\n",
      snes_frame_counter, g_recomp_push_count - last_push, cpu->PB, ret,
      cpu->A, cpu->m_flag,
      g_ram[0x18], g_ram[0x19], g_ram[0x1A], g_ram[0x1B],
      g_ram[0xF4], g_ram[0xF5], g_ram[0xFB], g_ram[0xE7], g_ram[0xE6], g_ram[0x1D]);
    last_push = g_recomp_push_count;
  }

  /* AR_OBJLOG=1: per-frame action-stage object-table + timer health. Logs the
   * game-frame, timer ($E6/$E7), player HP ($1D), and the first few object
   * slots' status word ($06A0 stride $40) + handler ptr ($12). Reveals the
   * exact frame the object table is wiped / timer goes non-BCD (the "sprites
   * vanish + timer '?'" corruption). */
  if (getenv("AR_OBJLOG")) {
    extern uint8 g_ram[0x20000];
    if (g_ram[0x18] == 0x01) {
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

void ActRaiserDrawPpuFrame(void) {
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
    NmiHandler_M1X1(&g_cpu);
    g_ar_in_interrupt = 0;
    ActRaiser_RestoreRegs(&g_cpu, &snap);
  }
}
