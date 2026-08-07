/* _XOPEN_SOURCE exposes ucontext (getcontext/makecontext/swapcontext), but on
 * macOS it also HIDES the BSD extensions — including MAP_ANON, which the
 * coroutine stack's guard page needs. _DARWIN_C_SOURCE puts them back without
 * giving up the XSI namespace. */
#define _XOPEN_SOURCE 600
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "actraiser_rtl.h"
#include "actraiser_game.h"
#include "action/action_load_pacing.h"
#include "actraiser_ws_gap.h"
#include "diorama/diorama_capture_blend.h"
#include "diorama/diorama_skybox_uv.h"
#include "diorama/diorama_planes.h"
#include "host/host_display.h"   /* kHostDisplayFramebufferHeight */
#include "settings.h"
#include "hd_replacements.h"
#include "music_replacements.h"
#include "dev/sfx_census.h"
#include "sim_render_atlas.h"
#include "sim3d.h"
#include "common_cpu_infra.h"
#include "snes/snes.h"
#include "snes/ppu.h"
#include "cpu_state.h"
#include "funcs.h"
#include "cpu_trace.h"
#include <stdio.h>
#include <stdbool.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <ucontext.h>
#include <errno.h>
#include <sys/mman.h>   /* mmap: guard page below the coroutine stack */
#include <unistd.h>
/* _XOPEN_SOURCE (needed for ucontext) hides MAP_ANONYMOUS on some libcs;
 * macOS spells it MAP_ANON. */
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif
#include <stdlib.h>
#include <string.h>

extern int snes_frame_counter;

#define GAME_STACK_SIZE (2 * 1024 * 1024)

#ifdef _WIN32
static void *g_host_fiber;   /* ConvertThreadToFiber result (driver thread) */
static void *g_game_fiber;   /* CreateFiber result (game coroutine) */
#else
static ucontext_t g_host_ctx;
static ucontext_t g_game_ctx;
static char *g_game_stack;        /* usable stack (guard page excluded) */
static void  *g_game_stack_map;   /* mmap base, including the guard page */
static size_t g_game_stack_map_len;
#endif
static bool g_game_started;

void ActRaiser_YieldToHost(void) {
#ifdef _WIN32
  SwitchToFiber(g_host_fiber);
#else
  /* swapcontext can fail with ENOMEM ("Insufficient stack space left"). An
   * unchecked failure would silently return and keep running on a stack the
   * host believes it owns; there is no recovery, so abort loudly instead. */
  if (swapcontext(&g_game_ctx, &g_host_ctx) != 0) {
    fprintf(stderr, "FATAL: swapcontext (game -> host) failed\n");
    abort();
  }
#endif
}

/* The recompiler executes the action loader's decompression and bulk graphics
 * copies as one host call. Hardware spends hundreds of display frames doing
 * that CPU work with NMI disabled, leaving the already-started Advent cue
 * playing over a forced-black screen. The inserted time is presentation/audio
 * pacing only: NMI, $0088, timers, and gameplay remain stopped. Pace only the
 * verified $00:843E force-blank write for a non-action -> action transition;
 * ordinary fades, action restarts, and non-action loads keep their existing
 * cadence. */
static unsigned g_action_load_armed_frames;
static unsigned g_action_load_hold_frames;
static uint64_t g_action_load_one_shot_token;
extern volatile int g_ar_in_interrupt;

static void ActRaiser_OnInidispWrite(uint8_t value) {
  uint32_t block = 0;
  (void)ar_block_history(&block, 1);
  unsigned frames = ActionLoadPacing_Frames(
      g_ram[kActRaiserWram_MapGroup],
      g_ram[kActRaiserWram_DestinationMapGroup], block, value);
  if (!frames || getenv("AR_NO_ACTION_LOAD_PACING"))
    return;

  g_action_load_armed_frames = frames;
  g_action_load_one_shot_token = 0;
  if (getenv("AR_LOADPACELOG")) {
    fprintf(stderr,
            "[load-pace] f=%d block=$%06X dest-group=$%02X: armed %u "
            "collapsed CPU frames\n",
            snes_frame_counter, block,
            g_ram[kActRaiserWram_DestinationMapGroup], frames);
  }
}

/* Stop immediately before the action script's $F0 halt command. This is late
 * enough that $18/$19 have entered action mode, but early enough that Advent
 * is still playing. RtlApuWrite invokes this seam before taking the APU lock,
 * so suspending the game coroutine cannot starve or deadlock audio. */
static void ActRaiser_OnApuPortPace(uint8_t port, uint8_t value) {
  extern Ppu *g_ppu;
  const ActionLoadPacingTriggerDecision decision =
      ActionLoadPacing_EvaluateTrigger(
          g_action_load_armed_frames,
          g_ram[kActRaiserWram_MapGroup], g_ppu ? g_ppu->inidisp : 0,
          port, value, g_ar_in_interrupt);
  if (decision == kActionLoadPacingTrigger_Ignore)
    return;

  /* The arm and trigger are deliberately two different hardware writes. If
   * the display or game mode moved on between them, reject the now-obviously
   * stale arm instead of turning a later $F0 into a five-second pause. */
  if (decision == kActionLoadPacingTrigger_Discard) {
    if (getenv("AR_LOADPACELOG")) {
      fprintf(stderr,
              "[load-pace] f=%d mode=$%02X/$%02X inidisp=$%02X: "
              "discarded stale arm before APU halt $F0\n",
              snes_frame_counter, g_ram[kActRaiserWram_MapGroup],
              g_ram[kActRaiserWram_CurrentMap],
              g_ppu ? g_ppu->inidisp : 0);
    }
    g_action_load_armed_frames = 0;
    g_action_load_one_shot_token = 0;
    return;
  }

  const unsigned frames = g_action_load_armed_frames;
  g_action_load_armed_frames = 0;
  bool one_shot_completed = false;
  const uint64_t one_shot_token =
      MusicReplacements_GetOneShotSnapshot(&one_shot_completed);
  if (ActionLoadPacing_ShouldReleaseForOneShot(
          frames, one_shot_token, one_shot_token, one_shot_completed)) {
    if (getenv("AR_LOADPACELOG")) {
      fprintf(stderr,
              "[load-pace] f=%d action mode=$%02X/$%02X: HD one-shot "
              "already complete; skipped %u-frame forced-blank hold\n",
              snes_frame_counter, g_ram[kActRaiserWram_MapGroup],
              g_ram[kActRaiserWram_CurrentMap], frames);
    }
    return;
  }
  g_action_load_one_shot_token = one_shot_token;
  g_action_load_hold_frames = frames - 1;
  if (getenv("AR_LOADPACELOG")) {
    fprintf(stderr,
            "[load-pace] f=%d action mode=$%02X/$%02X: holding forced blank "
            "for %u frames before APU halt $F0\n",
            snes_frame_counter, g_ram[kActRaiserWram_MapGroup],
            g_ram[kActRaiserWram_CurrentMap], frames);
  }
  ActRaiser_YieldToHost();
}

/* Keep ActRaiser's data-driven object-loop recovery policy out of the shared
 * runtime. The shared dispatcher owns the generic BRA/BRL-follow mechanism;
 * this project opts in only at the two ROM sites whose stack contract has
 * been verified. */
bool ActRaiser_RecoverDispatchMiss(uint32 source_pc24, uint32 target_pc24) {
  (void)target_pc24;
  return source_pc24 == 0x008965u || source_pc24 == 0x008966u;
}

/* ActRaiser's inline RDNMI waits need coroutine pacing at a ROM-specific set
 * of basic blocks. Returning -1 delegates ordinary reads to the shared SNES
 * hardware model; a nonnegative result overrides the $4210 byte. */
int ActRaiser_ReadRdnmi(Snes *snes) {
  extern uint32_t g_ar_blk_ring[];
  extern unsigned g_ar_blk_idx;
  static bool yielding;

  /* If the same block reads $4210 thousands of times without another traced
   * block between reads, print the gate state once instead of leaving only a
   * generic watchdog failure. */
  {
    static uint32_t wedge_blk, wedge_n;
    static unsigned wedge_idx;
    unsigned idx = g_ar_blk_idx;
    uint32_t block = g_ar_blk_ring[(g_ar_blk_idx - 1) & 1023u];
    if (block == wedge_blk && (idx == wedge_idx || idx == wedge_idx + 1)) {
      if (++wedge_n == 4096) {
        fprintf(stderr,
                "[4210-wedge] blk=$%06X f=%d x4096 consecutive reads; "
                "forceNmi=%d yielding=%d inNmi=%d nmiAvail=%d\n",
                block, snes_frame_counter, snes->forceNmi ? 1 : 0,
                yielding ? 1 : 0, snes->inNmi ? 1 : 0,
                snes->nmiAvail ? 1 : 0);
        fflush(stderr);
      }
    } else {
      wedge_blk = block;
      wedge_n = 1;
    }
    wedge_idx = idx;
  }

  /* These verified spin blocks can also execute from an interrupt context,
   * where yielding is impossible. Report vblank immediately in that case so
   * the emulated handler cannot deadlock inside its own wait. */
  if (!(snes->forceNmi && !yielding)) {
    static const uint32_t kSpinBlocksNoYield[] = {
      0x019293, 0x0192AA, 0x0287F3, 0x029AC4,
      0x02BEBF, 0x03B013, 0x03E535,
    };
    uint32_t block = g_ar_blk_ring[(g_ar_blk_idx - 1) & 1023u];
    for (unsigned i = 0;
         i < sizeof(kSpinBlocksNoYield) / sizeof(kSpinBlocksNoYield[0]); i++) {
      if (block == kSpinBlocksNoYield[i]) {
        static int warned;
        if (!warned) {
          warned = 1;
          fprintf(stderr,
                  "[4210] non-yieldable-context spin at $%06X f=%d "
                  "-> fast-exit (bit7=1, unpaced)\n",
                  block, snes_frame_counter);
        }
        return 0x82;
      }
    }
  }

  /* Latched: ActRaiser_ReadRdnmi runs on EVERY $4210 read, and the game polls
   * that register inside spin loops -- potentially thousands of times a frame,
   * not once. getenv is ~150ns here, so an unlatched read costs up to a few
   * percent of the frame budget at spin-loop rates for a switch that cannot
   * change mid-run. The other getenv calls in this file are in per-frame or
   * per-event paths where 150ns is genuinely free and latching would only add
   * state. */
  static int no_4210_yield = -1;
  if (no_4210_yield < 0) no_4210_yield = getenv("AR_NO4210YIELD") ? 1 : 0;
  if (snes->forceNmi && !yielding && !no_4210_yield) {
    static const uint32_t kSpinBlocks[] = {
      0x019293, /* intro/menu/effect wait */
      0x0192AA, /* effect-loop wait */
      0x0287F3, /* fade/transition helper */
      0x029AC4, /* boot sound-init wait */
      0x02BEBF, /* sound-code wait */
      0x03B013, /* long-form wait */
      0x03E535, /* sound-upload bracket wait */
    };
    uint32_t block = g_ar_blk_ring[(g_ar_blk_idx - 1) & 1023u];
    for (unsigned i = 0; i < sizeof(kSpinBlocks) / sizeof(kSpinBlocks[0]); i++) {
      if (block != kSpinBlocks[i])
        continue;
      if (getenv("AR_VBLOG")) {
        extern uint8 g_ram[0x20000];
        extern Ppu *g_ppu;
        static int last_frame = -1;
        if (snes_frame_counter != last_frame) {
          last_frame = snes_frame_counter;
          extern uint16 ar_cpu_S(void);
          extern uint8 ar_cpu_PB(void);
          fprintf(stderr,
                  "[vbl] f=%d bright=%d fblank=%d bgmode=%02x main=%02x "
                  "$18=%02x $19=%02x time$E6=%02x%02x HP=%02x PB=%02x "
                  "S=%04x blk=%06X\n",
                  snes_frame_counter, g_ppu->inidisp & 0xf,
                  (g_ppu->inidisp & 0x80) ? 1 : 0, g_ppu->bgmode,
                  g_ppu->screenEnabled[0], g_ram[0x18], g_ram[0x19],
                  g_ram[0xE7], g_ram[0xE6], g_ram[0x1D], ar_cpu_PB(),
                  ar_cpu_S(), block);
        }
      }
      yielding = true;
      ActRaiser_YieldToHost();
      yielding = false;
      return 0x82;
    }
    /* Clear/post/ack reads do not yield and report no vblank. */
    return 0x02;
  }

  return -1;
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
  cpu_write8(cpu, 0x00, kActRaiserWram_BrkSoundRequest,
             (uint8)(cpu->A & 0xFF));
  /* AR_SFXCENSUS=1: record the request with its caller and the index registers
   * that identify the requesting actor, so the census can join it to whatever
   * sample the SPC driver ends up keying. No-op when disabled. */
  {
    extern const char *g_last_recomp_func;
    SfxCensus_OnRequest((uint8_t)(cpu->A & 0xFF), g_last_recomp_func,
                        ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
                        (uint16_t)cpu->X, (uint16_t)cpu->Y);
  }
  /* AR_COPLOG=1: also log BRK (sound-request) posts, for contrast against COP
   * event posts below -- lets a stuck-state capture show whether the game is
   * still alive and posting routine SFX while a specific event id never posts. */
  if (getenv("AR_COPLOG")) {
    extern const char *g_last_recomp_func;
    unsigned game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    fprintf(stderr, "[brk] gf=%u fn=%s id=%02x $18=%02x $19=%02x\n",
            game_frame, g_last_recomp_func ? g_last_recomp_func : "?",
            (uint8)(cpu->A & 0xFF), g_ram[kActRaiserWram_MapGroup],
            g_ram[kActRaiserWram_CurrentMap]);
  }
}

/* Return the most recent recompiled block PC. The always-on ring is also used
 * by crash diagnostics; consulting it here lets a user-facing sound toggle
 * distinguish the dialogue composer's COP #$07 from unrelated uses of id 07. */
static uint32 ActRaiser_LastBlockPc(void) {
  extern int ar_block_history(uint32 *out, int max);
  uint32 pc = 0;
  return ar_block_history(&pc, 1) == 1 ? pc : 0;
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
  const uint8 id = (uint8)(cpu->A & 0xFF);
  const uint32 site = ActRaiser_LastBlockPc();
  /* $01:901C is the message composer's per-glyph pacing helper. Its
   * non-space path at $01:902D posts COP #$07 after drawing each character.
   * Suppress only this exact site: id 07 also drives unrelated game events. */
  const bool suppress_dialog_blip =
      !g_settings.audio_dialog_blip && id == 0x07 && site == 0x01902D;
  if (!suppress_dialog_blip)
    cpu_write8(cpu, 0x00, kActRaiserWram_CopRequest, id);
  /* AR_COPLOG=1: log every COP-posted event id + game-frame + calling recomp
   * function, so a Death-Heim stuck-state capture shows whether the
   * boss-defeat/next-encounter event ever posts at all, vs posting an id whose
   * consumer is unreached (see [[cop-syscall-hook-fix]] -- $C3DA consumer was
   * previously suspected still-unreached for a different event id). */
  if (getenv("AR_COPLOG")) {
    extern const char *g_last_recomp_func;
    unsigned game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    fprintf(stderr, "[cop] gf=%u fn=%s site=%06x id=%02x%s $18=%02x $19=%02x\n",
            game_frame, g_last_recomp_func ? g_last_recomp_func : "?",
            site, id, suppress_dialog_blip ? " suppressed-dialog-blip" : "",
            g_ram[kActRaiserWram_MapGroup],
            g_ram[kActRaiserWram_CurrentMap]);
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
  f = fopen(path, "wb");
  if (f) { fwrite(g_ram, 1, kActRaiserWramSize, f); fclose(f); }
  if (g_ppu) {
    snprintf(path, sizeof path, "%s.vram.bin", prefix);
    f = fopen(path, "wb"); if (f) { fwrite(g_ppu->vram, 2, 0x8000, f); fclose(f); }
    snprintf(path, sizeof path, "%s.cgram.bin", prefix);
    f = fopen(path, "wb"); if (f) { fwrite(g_ppu->cgram, 2, 0x100, f); fclose(f); }
    snprintf(path, sizeof path, "%s.oam.bin", prefix);
    f = fopen(path, "wb"); if (f) { fwrite(g_ppu->oam, 2, 0x100, f); fclose(f); }
    /* The HIGH table, as its own file so the 512-byte .oam.bin layout every
     * existing parser assumes stays exactly that. Without it a snapshot cannot
     * place or size a sprite at all: the high table carries each slot's X bit 8
     * and its size-large bit, so an OAM-only dump silently reads every sprite as
     * small and as x = (x & 0xff). That is a 256-pixel ambiguity — a sprite
     * staged off the RIGHT edge decodes as one sitting mid-screen. Cost a
     * diagnosis on 2026-08-05 (an off-screen staged sprite revealed by the
     * diorama vertical band could not be located from its snapshot). */
    snprintf(path, sizeof path, "%s.highoam.bin", prefix);
    f = fopen(path, "wb");
    if (f) { fwrite(g_ppu->highOam, 1, sizeof g_ppu->highOam, f); fclose(f); }
  }
}

static void game_coroutine(void) {
  g_action_load_armed_frames = 0;
  g_action_load_hold_frames = 0;
  g_action_load_one_shot_token = 0;
  cpu_state_init(&g_cpu, g_ram);
  g_cpu_brk_hook = ActRaiser_BrkHook;
  g_cpu_cop_hook = ActRaiser_CopHook;
  g_rtl_inidisp_hook = ActRaiser_OnInidispWrite;
  g_rtl_apu_port_pace_hook = ActRaiser_OnApuPortPace;

  ResetHandler_M1X1(&g_cpu);
  for (;;)
    ActRaiser_YieldToHost();
}

#ifdef _WIN32
static VOID CALLBACK game_coroutine_fiber(LPVOID param) {
  (void)param;
  game_coroutine();   /* never returns: for(;;) ActRaiser_YieldToHost() */
}
#endif

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
  /* Latched for the same reason: this runs on every vblank yield. */
  static int no_pop = -1;
  if (no_pop < 0) no_pop = getenv("AR_NOPOP") ? 1 : 0;
  if (!no_pop) cpu->S = (uint16)(cpu->S + 2);

  /* AR_YIELDLOG=1: dump the recomp call stack + SNES return address at each
   * vblank yield to see what the main thread is doing frame to frame. Read the
   * return frame from the PRE-pop S (sp-2, since we already added 2 above). */
  if (getenv("AR_YIELDLOG")) {
    extern const char *g_recomp_stack[];
    extern int g_recomp_stack_top;
    int top = g_recomp_stack_top;
    fprintf(stderr, "[yield] f=%d S=%04x A=%04x P=%02x depth=%d:",
            snes_frame_counter, cpu->S, cpu->A, cpu->P, top);
    for (int i = top - 1; i >= 0 && i >= top - 6; i--)
      fprintf(stderr, " %s", g_recomp_stack[i] ? g_recomp_stack[i] : "?");
    uint16 sp = no_pop ? cpu->S : (uint16)(cpu->S - 2);
    uint16 rlo = g_ram[(uint16)(sp + 1)];
    uint16 rhi = g_ram[(uint16)(sp + 2)];
    fprintf(stderr, " ret~%02x:%04x\n", cpu->PB, (uint16)(((rhi << 8) | rlo) + 1));
  }

  /* AR_FORCE18=<hex>: experimentally pin $7E0018 (game-mode byte) before the
   * next NMI's ABF0 branch, to test whether a non-zero game-mode unsticks the
   * frozen title (state machine + menu decompression). Diagnostic only. */
  {
    static int forced_map_group = -2;
    if (forced_map_group == -2) {
      const char *value = getenv("AR_FORCE18");
      forced_map_group = value ? (int)strtoul(value, NULL, 0) : -1;
    }
    if (forced_map_group >= 0)
      g_ram[kActRaiserWram_MapGroup] = (uint8)forced_map_group;
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
    uint16 game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    uint16 player_x = ActRaiser_ReadWram16(kActRaiserWram_PlayerPositionX);
    uint16 player_y = ActRaiser_ReadWram16(kActRaiserWram_PlayerPositionY);
    int16 player_velocity_x = (int16)ActRaiser_ReadWram16(
        kActRaiserWram_PlayerVelocityX);
    int16 player_velocity_y = (int16)ActRaiser_ReadWram16(
        kActRaiserWram_PlayerVelocityY);
    uint16 player_handler = ActRaiser_ReadWram16(
        kActRaiserWram_PlayerHandler);
    uint16 player_flags = ActRaiser_ReadWram16(kActRaiserWram_PlayerFlags);
    const uint8 map_group = g_ram[kActRaiserWram_MapGroup];
    const uint8 current_map = g_ram[kActRaiserWram_CurrentMap];
    static uint16 last_player_x, last_player_y;
    static uint8 last_map_group, last_map;
    int delta_x = 0, delta_y = 0;
    if (ActRaiser_IsActionMapGroup(map_group) &&
        last_map_group == map_group && last_map == current_map) {
      delta_x = (int16)(player_x - last_player_x);
      delta_y = (int16)(player_y - last_player_y);
    }
    fprintf(stderr,
      "[frame] f=%d gf=%u push+%lu callsite=%02x:%04x A=%04x m=%d $18=%02x $19=%02x $1A=%02x $1B=%02x $F4=%02x $F5=%02x $FB=%02x time$E6=%02x%02x HP$1D=%02x joy=%04x(raw=%04x) pos=%04x,%04x d=%+d,%+d vel=%+d,%+d h=%04x state=%04x boost=%02x crest=%02x\n",
      snes_frame_counter, game_frame, g_recomp_push_count - last_push,
      cpu->PB, ret,
      cpu->A, cpu->m_flag,
      map_group, current_map, g_ram[kActRaiserWram_DestinationMap],
      g_ram[kActRaiserWram_DestinationMapGroup],
      g_ram[kActRaiserWram_InputEnableMask],
      g_ram[kActRaiserWram_InputEnableMask + 1],
      g_ram[kActRaiserWram_TransitionRequest],
      g_ram[kActRaiserWram_ActionTimerHigh],
      g_ram[kActRaiserWram_ActionTimerLow],
      g_ram[kActRaiserWram_PlayerHp], joy, joy_raw, player_x, player_y,
      delta_x, delta_y, player_velocity_x, player_velocity_y,
      player_handler, player_flags,
      g_ram[kActRaiserWram_PlayerBoost],
      g_ram[kActRaiserWram_PlayerCrest]);
    last_player_x = player_x;
    last_player_y = player_y;
    last_map_group = map_group;
    last_map = current_map;
    last_push = g_recomp_push_count;
  }

  /* AR_OBJLOG=1: per-frame action-stage object-table + timer health. Logs the
   * game-frame, timer ($E6/$E7), player HP ($1D), and the first few object
   * slots' status word ($06A0 stride $40) + handler ptr ($12). Reveals the
   * exact frame the object table is wiped / timer goes non-BCD (the "sprites
   * vanish + timer '?'" corruption). */
  if (getenv("AR_OBJLOG")) {
    if (ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup])) {
      enum {
        kObjectLogSampleCount = 24,
        kActionObjectHandlerOffset = 0x12,
        kActionObjectDisabled = 0x4000,
      };
      unsigned game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
      int active_objects = 0;
      for (int i = 0; i < kObjectLogSampleCount; i++) {
        uint16 object_address = (uint16)(
            kActRaiserWram_ActionObjectTable +
            i * kActRaiserActionObjectStride);
        uint16 status = ActRaiser_ReadWram16(object_address);
        if (!(status & kActRaiserObjectStatus_End) &&
            !(status & kActionObjectDisabled)) {
          active_objects++;
        }
      }
      unsigned first_status = ActRaiser_ReadWram16(
          kActRaiserWram_ActionObjectTable);
      unsigned first_handler = ActRaiser_ReadWram16((uint16)(
          kActRaiserWram_ActionObjectTable + kActionObjectHandlerOffset));
      fprintf(stderr, "[obj] gf=%u timer=%02x%02x HP=%02x active=%d obj0.sw=%04x obj0.h=%04x\n",
              game_frame, g_ram[kActRaiserWram_ActionTimerHigh],
              g_ram[kActRaiserWram_ActionTimerLow],
              g_ram[kActRaiserWram_PlayerHp], active_objects,
              first_status, first_handler);
    }
  }

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

  ActRaiser_YieldToHost();
  return RECOMP_RETURN_NORMAL;
}

/* $02:BC56 selects the next animated BG-character frame and arms NMI DMA
 * descriptor 1. The native game assumes a force-blanked graphics load will
 * finish before another animation tick. In the recomp, an SPC command ack can
 * keep the main coroutine in $02:B63B for several host frames while NMI keeps
 * running. A tick in that window uploads the not-yet-captured $7F:B800 frame
 * over VRAM $0000; $02:BAF5 then captures that blank page and makes the
 * corruption self-perpetuating.
 *
 * Preserve BC56's native behavior and register/flag contract, except that an
 * invisible tick is deferred while INIDISP force-blank is active. Once the
 * loader clears force-blank, animation resumes from the same phase with all
 * four frames already captured. */
static void ActRaiser_TileAnimSetNz(CpuState *cpu, uint16 value,
                                    unsigned bits) {
  const uint16 sign = bits == 8 ? 0x0080 : 0x8000;
  const uint16 mask = bits == 8 ? 0x00FF : 0xFFFF;
  value &= mask;
  cpu->_flag_Z = value == 0;
  cpu->_flag_N = (value & sign) != 0;
}

static uint16 ActRaiser_TileAnimAdc16(CpuState *cpu, uint16 left,
                                     uint16 right) {
  uint16 result;
  if (!cpu->_flag_D) {
    const uint32 sum = (uint32)left + (uint32)right + cpu->_flag_C;
    result = (uint16)sum;
    cpu->_flag_C = (sum & 0x10000) != 0;
    cpu->_flag_V = ((left ^ result) & (right ^ result) & 0x8000) != 0;
  } else {
    uint32 carry = cpu->_flag_C;
    uint32 decimal_result = 0;
    uint32 carry_into_sign = 0;
    for (unsigned shift = 0; shift < 16; shift += 4) {
      uint32 digit = ((left >> shift) & 0xF) +
                     ((right >> shift) & 0xF) + carry;
      carry = digit > 9;
      if (carry) digit += 6;
      decimal_result |= (digit & 0xF) << shift;
      if (shift == 8) carry_into_sign = carry;
    }
    result = (uint16)decimal_result;
    cpu->_flag_C = carry != 0;
    const uint32 visible_sign_sum =
        (left & 0xF000) + (right & 0xF000) + (carry_into_sign << 12);
    cpu->_flag_V =
        ((left ^ visible_sign_sum) & (right ^ visible_sign_sum) & 0x8000) != 0;
  }
  ActRaiser_TileAnimSetNz(cpu, result, 16);
  cpu_write_a16(cpu, result);
  return result;
}

RecompReturn ActRaiser_TileAnimationTick(CpuState *cpu) {
  const unsigned entry_m_bits = cpu->m_flag ? 8 : 16;
  const uint16 entry_m_mask = cpu->m_flag ? 0x00FF : 0xFFFF;
  const uint16 frame = cpu->m_flag
      ? cpu_read8(cpu, 0x7E, (uint16)(cpu->D + 0x0088))
      : cpu_read16(cpu, 0x7E, (uint16)(cpu->D + 0x0088));
  const uint16 period = cpu->m_flag
      ? cpu_read8(cpu, 0x7E, (uint16)(cpu->D + 0x00DE))
      : cpu_read16(cpu, 0x7E, (uint16)(cpu->D + 0x00DE));
  const uint16 due = (frame & period) & entry_m_mask;
  cpu_write_a_m(cpu, due);
  ActRaiser_TileAnimSetNz(cpu, due, entry_m_bits);

  if (due != 0 || (g_ppu && PPU_forcedBlank(g_ppu))) {
    cpu_mirrors_to_p(cpu);
    cpu->S = (uint16)(cpu->S + 3);  /* replaced RTL */
    return RECOMP_RETURN_NORMAL;
  }

  const uint16 phase_word = cpu->m_flag
      ? cpu_read8(cpu, 0x7E, (uint16)(cpu->D + 0x00E0))
      : cpu_read16(cpu, 0x7E, (uint16)(cpu->D + 0x00E0));
  const uint16 phase_mask = cpu->m_flag
      ? cpu_read8(cpu, 0x7E, (uint16)(cpu->D + 0x00DF))
      : cpu_read16(cpu, 0x7E, (uint16)(cpu->D + 0x00DF));
  const uint16 phase_plus_one =
      (uint16)(((phase_word & phase_mask) + 1) & entry_m_mask);
  cpu_write_a_m(cpu, phase_plus_one);
  ActRaiser_TileAnimSetNz(cpu, phase_plus_one, entry_m_bits);
  cpu_write_x_x(cpu, phase_plus_one);
  ActRaiser_TileAnimSetNz(cpu, cpu_read_x_x(cpu), cpu->x_flag ? 8 : 16);

  cpu_mirrors_to_p(cpu);
  cpu->P |= CPU_P_X;               /* SEP #$10 */
  cpu_p_to_mirrors(cpu);
  cpu->X &= 0x00FF;
  cpu_mirrors_to_p(cpu);
  cpu->P &= (uint8)~CPU_P_M;       /* REP #$20 */
  cpu_p_to_mirrors(cpu);

  cpu_write_a16(cpu, 0);
  ActRaiser_TileAnimSetNz(cpu, 0, 16);
  for (;;) {
    cpu_write_x8(cpu, (uint8)(cpu_read_x8(cpu) - 1));
    ActRaiser_TileAnimSetNz(cpu, cpu_read_x8(cpu), 8);
    if (cpu_read_x8(cpu) == 0) break;
    cpu->_flag_C = 0;
    ActRaiser_TileAnimAdc16(
        cpu, cpu_read_a16(cpu),
        cpu_read16(cpu, 0x7E, (uint16)(cpu->D + 0x00E1)));
  }

  cpu->_flag_C = 0;
  const uint16 source =
      ActRaiser_TileAnimAdc16(cpu, cpu_read_a16(cpu), 0xB800);
  cpu_write16(cpu, 0x7E, (uint16)(cpu->D + 0x00D7), source);

  cpu_mirrors_to_p(cpu);
  cpu->P |= CPU_P_M;               /* SEP #$20 */
  cpu_p_to_mirrors(cpu);
  cpu_mirrors_to_p(cpu);
  cpu->P &= (uint8)~CPU_P_X;       /* REP #$10 */
  cpu_p_to_mirrors(cpu);

  cpu_write_x16(cpu,
                cpu_read16(cpu, 0x7E, (uint16)(cpu->D + 0x00E1)));
  ActRaiser_TileAnimSetNz(cpu, cpu_read_x16(cpu), 16);
  cpu_write16(cpu, 0x7E, (uint16)(cpu->D + 0x00DC), cpu_read_x16(cpu));

  const uint8 next_phase =
      (uint8)(cpu_read8(cpu, 0x7E, (uint16)(cpu->D + 0x00E0)) + 1);
  cpu_write8(cpu, 0x7E, (uint16)(cpu->D + 0x00E0), next_phase);
  ActRaiser_TileAnimSetNz(cpu, next_phase, 8);
  cpu_mirrors_to_p(cpu);

  cpu->S = (uint16)(cpu->S + 3);  /* replaced RTL */
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
enum {
  kAitosParallaxMapFirst = 0x01,
  kAitosParallaxMapLast = 0x03,
  kNorthwallParallaxMapFirst = 0x01,
  kNorthwallParallaxMapLast = 0x05,
  kNorthwallBossParallaxMap = 0x08,
  kBloodpoolAct1Map = 0x01,
  kBloodpoolWaterStartY = 136,
  kDeathHeimFogStartY = 144,
  kEndingSkyBg1TilemapPage = 0x64,
  kEndingSkyBg2TilemapPage = 0x74,
  kBgTilemapPageMask = 0xFC,
};

/* Per-frame VERTICAL margin policy — the transpose of the bounded-world side
 * margin clamp in ActRaiser_ApplyWidescreenPolicy, and deliberately built the
 * same way: ask the game's own camera and layer-dimension state how much world
 * actually exists past the viewport edge, and never request more than that.
 *
 * The camera routine at $02:B091 clamps V to [0, $30 - $E1] with $E1 = 225 --
 * the hardcoded viewport height, exactly as the H clamp's $100 is the
 * hardcoded 256 width (rendering-engine.md §6). So `camera_y` IS the number of
 * world rows above the viewport, and `height - 225 - camera_y` the number
 * below. At the top of a level both the camera and the available margin are 0,
 * which is what stops the band from showing the void the level ends at.
 *
 * Action stages only. Simulation towns get their 3D treatment from sim3d.c and
 * the world map is Mode 7, whose per-scanline matrix cannot be extrapolated
 * past the visible band (see PpuDrawBackground_mode7). */
static void ActRaiser_ApplyVerticalMarginPolicy(uint8_t map_group,
                                                uint8_t map_number) {
  extern int g_ws_extra_top;
  extern int g_ws_extra_bottom;
  extern bool Diorama_IsActiveThisFrame(void);

  g_ws_extra_top = 0;
  g_ws_extra_bottom = 0;

  int budget = g_settings.diorama_vertical_extend;
  if (budget > 0 && Diorama_IsActiveThisFrame() &&
      ActRaiser_IsActionMapGroup(map_group) &&
      !ActRaiser_IsSimulationTown(map_group, map_number)) {
    if (budget > kPpuExtraTopBottom) budget = kPpuExtraTopBottom;
    int camera_y = ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY);
    int available_top = camera_y;
    if (available_top < 0) available_top = 0;
    g_ws_extra_top = available_top < budget ? available_top : budget;
    /* No bottom band: see g_ws_extra_bottom (main.c). The world below the
     * viewport is streamed and drawable, but every sprite standing on it would
     * be drawn at a negative screen Y instead, so the band would show
     * background with the actors missing. */
  }
  PpuSetExtraVerticalSpace(g_ppu, g_ws_extra_top, g_ws_extra_bottom);
  /* Only meaningful while a band exists, and re-applied every frame because
   * ppu_reset zeroes the field like the rest of the margin state. */

  /* AR_VEXT_TILES=1: classify the BG1 tilemap rows the band will read. A
   * "uniform" row (one tile id repeated across the whole ring width) is the
   * column-strip FILLER signature (rendering-engine.md §4) -- the band is
   * showing a row no row-strip has refreshed. Logged with cam_y's 256-page
   * phase because that is the suspected predictor. */
  /* AR_VEXT_TILES=1: dump the BG1 tilemap ids the band reads, next to the first
   * VISIBLE row, so a filler row is distinguishable from a legitimately uniform
   * one (an all-sky row is uniform too -- that false positive is why the first
   * cut of this probe reported 100%). Column-strip filler is a row whose ids do
   * not belong to the surrounding content (rendering-engine.md §4). */
  if (getenv("AR_VEXT_TILES") && g_ws_extra_top > 0) {
    int base = PPU_bgTilemapAdr(g_ppu, 0);
    bool wider = PPU_bgTilemapWider(g_ppu, 0);
    bool higher = PPU_bgTilemapHigher(g_ppu, 0);
    int cam_y = ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY);
    char buf[512]; int n = 0;
    for (int py = cam_y - g_ws_extra_top; py < cam_y + 16; py += 8) {
      int off = base + (((py >> 3) & 0x1f) << 5);
      if ((py & 0x100) && higher) off += wider ? 0x800 : 0x400;
      unsigned ids[4];
      for (int k = 0; k < 4; k++)
        ids[k] = g_ppu->vram[(off + k * 7) & 0x7fff] & 0x3ff;
      n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%03X,%03X,%03X,%03X",
                    py == cam_y ? " | vis:" : (n ? " " : ""),
                    ids[0], ids[1], ids[2], ids[3]);
      if (n >= (int)sizeof(buf) - 32) break;
    }
    fprintf(stderr, "[vext-tiles] gf=%u camY=%4d phase=%3d band: %s\n",
            ActRaiser_ReadWram16(kActRaiserWram_GameFrame), cam_y,
            cam_y & 0xFF, buf);
  }

  if (getenv("AR_VEXT_LOG")) {
    static unsigned last;
    unsigned gf = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    if (gf != last) {
      last = gf;
      fprintf(stderr,
              "[vext] gf=%u top=%d camY=%d bg1H=%d vscroll=%d/%d "
              "screenEn=$%02x bg1tm=$%04x\n",
              gf, g_ws_extra_top,
              ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY),
              ActRaiser_ReadWram16(kActRaiserWram_Bg1Height),
              g_ppu->vScroll[0], g_ppu->vScroll[1],
              g_ppu->screenEnabled[0], PPU_bgTilemapAdr(g_ppu, 0));
    }
  }
}

static void ActRaiser_ApplyWidescreenPolicy(void) {
  extern bool g_ws_active;
  extern int g_ws_extra;
  if (!g_ws_active) return;
  static int survey = -1;
  if (survey < 0) {
    const char *e = getenv("AR_WS_SURVEY");
    survey = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  const uint8 map_group = g_ram[kActRaiserWram_MapGroup];
  const uint8 map_number = g_ram[kActRaiserWram_CurrentMap];
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
  uint8 hud_player_row_y = 0;
  uint8 hud_left_only_y = 0;
  /* True when the current wide world has finite horizontal bounds. The PPU
   * still owns the fixed centering budget; this policy narrows the live left
   * and right margins as the camera approaches either world edge. */
  int bounded_world_margins = 0;
  if (!survey && map_group == kActRaiserMapGroup_NonAction) {
    switch (map_number) {
      case kActRaiserNonActionMap_SkyPalace:
                            /* Sky Palace hub. BG1 = sky/clouds (wide, clean).
                               BG2 = pillars plus game-owned offscreen dialog
                               staging farther around its 64x64 tilemap. Keep
                               BG2 raw-wide; the render transaction decodes a
                               box-free ROM source into only margin columns.
                               The authentic center retains its BG2 box. */
        wide = 1;
        break;

      case kActRaiserNonActionMap_WorldMap:
                            /* Mode 7 world map: fully wide, no UI layers. */
        wide = 1; 
        break;
      
      case kActRaiserNonActionMap_Title:
        // Title is always not wide screen since the backdrop is black
        wide = 0;
        break;
    
      case kActRaiserNonActionMap_Fillmore:
      case kActRaiserNonActionMap_Bloodpool:
      case kActRaiserNonActionMap_Kasandora:
      case kActRaiserNonActionMap_Aitos:
      case kActRaiserNonActionMap_Marahna:
      case kActRaiserNonActionMap_Northwall: {
        /* $01:B4C6 clamps camera X ($22) to $0000-$0100, proving 256px of
         * world on either side of the authentic viewport. AR_WS_SIM=0 is the
         * same-binary authentic baseline for town regression captures. BG2 is
         * the bounded dialog/overlay plane and remains center-clamped. The
         * separate ADAD/AE6F and B473 ports use this same $01-$06 range. */
        wide = g_settings.ws_sim;
        bounded_world_margins = wide;
        clamp = kActRaiserBgLayerMask_Bg2;
        break;
      }

      case kActRaiserNonActionMap_Temple:
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
  } else if (!survey && ActRaiser_IsActionMapGroup(map_group)) {
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
    uint16 bg2_width = ActRaiser_ReadWram16(kActRaiserWram_Bg2Width);
    if (bg2_width < kActRaiserTownWorldWidth) {
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
        clamp |= kActRaiserBgLayerMask_Bg2;
      } else if ((map_group == kActRaiserMapGroup_Aitos &&
                  map_number >= kAitosParallaxMapFirst &&
                  map_number <= kAitosParallaxMapLast) ||
                 (map_group == kActRaiserMapGroup_Northwall &&
                  ((map_number >= kNorthwallParallaxMapFirst &&
                    map_number <= kNorthwallParallaxMapLast) ||
                   map_number == kNorthwallBossParallaxMap)) ||
                 (map_group == kActRaiserMapGroup_DeathHeim &&
                  map_number >= kActRaiserDeathHeimMap_FirstBoss &&
                  map_number <= kActRaiserDeathHeimMap_FinalBoss)) {
        repeat |= kActRaiserBgLayerMask_Bg2;
      } else {
        mirror |= kActRaiserBgLayerMask_Bg2;
      }
    }

    /* Bloodpool Act 1 ($02:$01) is another mixed-content narrow BG2. Its
     * static mountain silhouette benefits from the normal reflected margins,
     * but tile row 17 and below is animated water. Reflecting those lower
     * scanlines reverses the apparent flow at both authentic-screen seams.
     * Keep reflection above y=136 and cyclically continue the already-rendered
     * water scanline below it. When AR_WS_BG2_MIRROR=0 selected the authentic
     * clamp fallback, leave the repeat band disabled as part of that A/B gate. */
    if (wide && map_group == kActRaiserMapGroup_Bloodpool &&
        map_number == kBloodpoolAct1Map &&
        (mirror & kActRaiserBgLayerMask_Bg2)) {
      repeat_band_layer = 1;  /* BG2 */
      repeat_band_y0 = kBloodpoolWaterStartY;
      repeat_band_y1 = kActRaiserAuthenticHeight;
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
    if (wide && map_group == kActRaiserMapGroup_DeathHeim &&
        map_number == kActRaiserDeathHeimMap_Hub) {
      const int ending_sky_pages =
          (g_ppu->bgXsc[0] & kBgTilemapPageMask) ==
              kEndingSkyBg1TilemapPage &&
          (g_ppu->bgXsc[1] & kBgTilemapPageMask) ==
              kEndingSkyBg2TilemapPage;
      bounded_world_margins = 0;
      if (g_ram[kActRaiserWram_DeathHeimProgress] >=
              kActRaiserDeathHeimProgress_FinalBossBeaten &&
          (ending_sky_pages ||
           g_ram[kActRaiserWram_DeathHeimEndingState] >=
               kActRaiserDeathHeimEndingState_SkySettled)) {
        clamp |= kActRaiserBgLayerMask_Bg1;
        mirror |= kActRaiserBgLayerMask_Bg2;
      } else {
        clamp |= kActRaiserBgLayerMask_Bg1AndBg2;
        repeat_band_layer = 1;  /* BG2 */
        repeat_band_y0 = kDeathHeimFogStartY;
        repeat_band_y1 = kActRaiserAuthenticHeight;
      }
    }

    /* Death Heim's final arena ($07:$08) stacks two 256px star-road layers
     * and applies independent scanline/sine motion. Both live BGSC registers
     * select 32x32 tilemaps ($60/$70), so native tile fetch already wraps at
     * 256px with the current per-line scroll. The generic world-edge budget
     * was the only reason the margins stayed black. Open the symmetric canvas
     * and draw both layers raw: this preserves their raster phase and avoids
     * the isolated-buffer clear/merge cost of presentation-layer repeat. */
    if (wide && map_group == kActRaiserMapGroup_DeathHeim &&
        map_number == kActRaiserDeathHeimMap_FinalBoss) {
      bounded_world_margins = 0;
      clamp &= (uint8)~kActRaiserBgLayerMask_Bg1AndBg2;
      mirror &= (uint8)~kActRaiserBgLayerMask_Bg1AndBg2;
      repeat &= (uint8)~kActRaiserBgLayerMask_Bg1AndBg2;
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
    if (ActRaiser_IsActionMapGroup(map_group)) {
      /* Three horizontal bands in the 40px action HUD:
       *   y= 0-19  ACT/TIME/SCORE — 3-way left/center/right
       *   y=20-27  PLAYER health + magic-scroll — left+right at rightStart
       *   y=28-39  ENEMY health — full-width left-only (boss spans 256px) */
      hud_split_height = kActRaiserActionHudHeight;
      hud_split_left_end = kActRaiserActionHudLeftEnd;
      hud_split_right_start = kActRaiserActionHudRightStart;
      hud_player_row_y = kActRaiserActionHudPlayerRowY;
      hud_left_only_y = kActRaiserActionHudEnemyRowY;
    } else if (map_group == kActRaiserMapGroup_NonAction &&
               map_number >= kActRaiserSimulationTown_First &&
               map_number <= kActRaiserNonActionMap_SkyPalace) {
      hud_split_height = kActRaiserSimulationHudHeight;
      hud_split_left_end = kActRaiserSimulationHudSplit;
      hud_split_right_start = kActRaiserSimulationHudSplit;
      hud_player_row_y = kActRaiserSimulationHudHeight;
      hud_left_only_y = kActRaiserSimulationHudHeight;
    }
  }

  /* The HUD split is persistent PPU policy state, unlike the layer-clamp
   * arrays reset by PpuSetExtraSpace. Clear it explicitly on every mode flip
   * path before optionally enabling this frame's prototype. */
  PpuSetWidescreenHudSplit(g_ppu, 0, 0, 0, 0, 0);
  if (wide) {
    PpuSetExtraSpace(g_ppu, (uint8)g_ws_extra);
    PpuSetWidescreenLayerClamp(g_ppu, clamp);
    PpuSetWidescreenLayerMirror(g_ppu, mirror);
    PpuSetWidescreenLayerRepeat(g_ppu, repeat);
    /* Fix A (SPEC-backdrop-clip.md). Only meaningful in diorama mode, which is
     * the only thing that captures these layers; gating on the setting keeps
     * this a live A/B and keeps flat output untouched either way. Must come
     * after PpuSetExtraSpace above, which resets the per-frame policy bits. */
    PpuSetWidescreenPadCapturedToBudget(
        g_ppu, (uint8)(g_settings.diorama_mode &&
                       g_settings.diorama_margin_fix));
    if (repeat_band_layer >= 0)
      PpuSetWidescreenLayerRepeatBand(g_ppu, (uint8)repeat_band_layer,
                                      repeat_band_y0, repeat_band_y1);
    if (bg2_gap)
      PpuSetWidescreenLayerMarginGap(
          g_ppu, kActRaiserPpuLayer_Bg2,
          (uint8)bg2_gap, (uint8)bg2_gap);
    if (hud_split_height)
      PpuSetWidescreenHudSplit(g_ppu, hud_split_height,
                               hud_split_left_end, hud_split_right_start,
                               hud_player_row_y, hud_left_only_y);
    if (hud_split_height)
      PpuSetOverlayCapture(g_ppu, kPpuOverlaySource_Bg3,
                           0, 0, kActRaiserAuthenticWidth, hud_split_height,
                           kPpuOverlayFlag_RemoveFromGame);
    if (bounded_world_margins) {
      /* Clamp each side to real BG1 world space. Action width is section
       * state $2E; simulation towns are the fixed 512px world proven by
       * $01:B4C6's camera clamp. Outside [0,width) stays black. */
      int camera_x = ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraX);
      int world_width = ActRaiser_IsSimulationTown(map_group, map_number)
          ? kActRaiserTownWorldWidth
          : ActRaiser_ReadWram16(kActRaiserWram_Bg1Width);
      int available_left = camera_x;
      int available_right =
          world_width - kActRaiserAuthenticWidth - camera_x;
      if (available_left < 0) available_left = 0;
      if (available_right < 0) available_right = 0;
      int margin_left = available_left < g_ws_extra
          ? available_left : g_ws_extra;
      int margin_right = available_right < g_ws_extra
          ? available_right : g_ws_extra;
      PpuSetExtraSideSpace(g_ppu, margin_left, margin_right, 0);
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
            (unsigned)ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
            map_group, map_number, wide ? "WIDE" : "pillarbox",
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
    unsigned game_frame = ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    if ((int)game_frame != lf) {
      lf = (int)game_frame;
      fprintf(stderr, "[ws-layers] gf=%u mode=%d main=%02x sub=%02x wsel=%06x cgwsel=%02x cgadsub=%02x",
              game_frame, g_ppu->bgmode & 7,
              g_ppu->screenEnabled[0], g_ppu->screenEnabled[1],
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
    /* Fix C (SPEC-backdrop-clip.md). In flat mode these strips are the intended
     * pillarbox at a world edge and stay black. In diorama mode this same
     * framebuffer becomes the backdrop PLANE, drawn with SDL_BLENDMODE_NONE
     * behind the whole stack, so black here paints an opaque wedge across ~a
     * fifth of the screen; the scene's own backdrop colour makes it continuous.
     * Gating on diorama_mode is what keeps flat output byte-identical, and the
     * settings gate keeps the old behaviour available for the A/B. */
    uint32 gap_fill =
        (g_settings.diorama_mode && g_settings.diorama_margin_fix)
            ? ActRaiser_BackdropArgb(g_ppu)
            : 0u;
    ActRaiserFillMarginGaps(g_ppu->renderBuffer, g_ppu->renderPitch,
                            kActRaiserAuthenticHeight,
                            g_ppu->extraLeftRight, l, r, gap_fill);
    last_l = l;
    last_r = r;
  } else if (l != last_l || r != last_r) {
    last_l = l;
    last_r = r;
    if (g_ppu->renderBuffer)
      memset(g_ppu->renderBuffer, 0,
             (size_t)g_ppu->renderPitch * kActRaiserAuthenticHeight);
  }
}

/* The OAM slots ActRaiser_WidescreenHudObjPromote validated THIS frame.
 *
 * Kept separately from overlayCaptures[Obj] on purpose: the diorama block
 * further down ActRaiserDrawPpuFrame legitimately re-captures OBJ as a
 * full-frame scene layer over OAM slots 0..127, and PpuSetOverlayCapture
 * resets oamFirst/oamCount, so the capture stops being able to answer "which
 * sprites are the flat HUD icon" the moment the diorama is on. Anything that
 * needs that answer must read this record via ActRaiser_HudObjIconRange
 * instead of re-deriving it from the capture. */
static uint8_t s_hud_obj_icon_first;
static uint8_t s_hud_obj_icon_count;

bool ActRaiser_HudObjIconRange(uint8_t *first, uint8_t *count) {
  /* Writes both outputs on every path, including "nothing promoted" (0/0):
   * FrameSlot slots are recycled, so leaving them untouched would republish
   * the previous occupant's icon range on a frame that has no icon. */
  if (first) *first = s_hud_obj_icon_first;
  if (count) *count = s_hud_obj_icon_count;
  return s_hud_obj_icon_count != 0;
}

/* Promote a validated fixed-screen HUD icon out of OAM.
 *
 * Action's $00:923A icon uses tiles $D4-$D7 in the first four slots.
 * Simulation's 2026-07-16 Fillmore capture proves the hourglass uses the same
 * slots at x=$94/$9B, y=$0B/$13; ROM frames $01:DD4B/$DD60/$DD75/$DD8A cycle
 * upper tiles $EC-$EF and paired lower tiles $FC-$FF, with horizontal flip on
 * each right half. Sky Palace's selected-magic icon is neither fixed-slot nor
 * fixed-shape and is scanned for -- see ActRaiser_SkyPalaceMagicIconSlots.
 *
 * All three land on the same 16x16 footprint, which is what lets the host draw
 * whatever this promotes as one 16x16 chunk beside the right HUD group. No
 * OAM/WRAM state is changed. */
static void ActRaiser_WidescreenHudObjPromote(void) {
  enum { kActRaiserPpuOamSlots = kPpuOamWords / 2 };
  s_hud_obj_icon_first = 0;
  s_hud_obj_icon_count = 0;
  if (!g_ppu || !g_ppu->extraLeftRight)
    return;

  uint8 capture_height = 0;
  uint8 map_group = g_ram[kActRaiserWram_MapGroup];
  uint8 map_number = g_ram[kActRaiserWram_CurrentMap];
  if (g_ppu->wsHudSplitHeight == kActRaiserActionHudHeight &&
      g_ppu->wsHudLeftEnd == kActRaiserActionHudLeftEnd &&
      g_ppu->wsHudRightStart == kActRaiserActionHudRightStart &&
      g_ppu->wsHudPlayerRowY == kActRaiserActionHudPlayerRowY &&
      g_ppu->wsHudLeftOnlyY == kActRaiserActionHudEnemyRowY &&
      ActRaiser_IsActionMapGroup(map_group)) {
    for (int slot = 0; slot < kActRaiserHudObjOamCount; slot++) {
      int index = slot * 2;
      uint8 tile = (uint8)g_ppu->oam[index + 1];
      uint8 y = (uint8)(g_ppu->oam[index] >> 8);
      uint8 expected_y = slot < 2
          ? kActRaiserHudObjUpperY : kActRaiserHudObjLowerY;
      if (tile != (uint8)(kActRaiserMagicHudFirstTile + slot) ||
          y != expected_y)
        return;
    }
    capture_height = kActRaiserActionHudHeight;
  } else if (g_ppu->wsHudSplitHeight == kActRaiserSimulationHudHeight &&
             g_ppu->wsHudLeftEnd == kActRaiserSimulationHudSplit &&
             g_ppu->wsHudRightStart == kActRaiserSimulationHudSplit &&
             g_ppu->wsHudLeftOnlyY == kActRaiserSimulationHudHeight &&
             map_group == kActRaiserMapGroup_NonAction &&
             map_number >= kActRaiserSimulationTown_First &&
             map_number <= kActRaiserNonActionMap_SkyPalace) {
    if (map_number == kActRaiserNonActionMap_SkyPalace) {
      /* Sky Palace magic icon shifts OAM slots when dialog sprites appear, and
       * changes SHAPE with the selected spell (see the two forms documented on
       * ActRaiser_SkyPalaceMagicIconSlots) -- so scan for the signature rather
       * than hardcoding either a slot or a slot count. */
      const int large_px = PpuObjSizeForSizeBit(g_ppu, 1);
      int found_slot = -1, found_count = 0;
      for (int s = kActRaiserSkyPalaceMagicOamFirst;
           s < kActRaiserPpuOamSlots && found_slot < 0; s++) {
        const int index = s * 2;
        const int large = (g_ppu->highOam[s >> 2] >> ((s & 3) * 2 + 1)) & 1;
        const int count = ActRaiser_SkyPalaceMagicIconSlots(
            (uint8)g_ppu->oam[index], (uint8)(g_ppu->oam[index] >> 8),
            (uint8)(g_ppu->oam[index + 1] >> 8), large, large_px);
        if (!count || s + count > kActRaiserPpuOamSlots)
          continue;
        /* The lead slot matched. The quad form spends three more slots on the
         * same icon, so confirm those before claiming the range; the single
         * large slot has no companions and this loop is a no-op for it. */
        int ok = 1;
        for (int i = 1; i < count && ok; i++) {
          const int q = (s + i) * 2;
          uint8 expected_y = 0, expected_attr = 0;
          ActRaiser_SkyPalaceMagicQuadSlot(i, &expected_y, &expected_attr);
          if ((uint8)(g_ppu->oam[q] >> 8) != expected_y ||
              (uint8)(g_ppu->oam[q + 1] >> 8) != expected_attr)
            ok = 0;
        }
        if (!ok)
          continue;
        found_slot = s;
        found_count = count;
      }
      /* AR_HUDICON=1: one line per change of scan outcome, the same
       * change-triggered shape as the [widescreen] policy line above. This is
       * the answer to "why is the magic icon still at centre screen?" — a
       * slot=-1 line names the spell whose OAM shape the scan does not know,
       * which is exactly how the four-small-slots/one-large-slot split was
       * found. Costs a getenv and three compares per Sky Palace frame. */
      if (getenv("AR_HUDICON")) {
        static int last_spell = -1, last_slot = -2, last_count = -1;
        int spell = g_ram[kActRaiserWram_SelectedMagic];
        if (spell != last_spell || found_slot != last_slot ||
            found_count != last_count) {
          last_spell = spell; last_slot = found_slot; last_count = found_count;
          fprintf(stderr,
                  "[hud-icon] gf=%u sky-palace spell=%d -> slot=%d count=%d\n",
                  (unsigned)ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
                  spell, found_slot, found_count);
        }
      }
      if (found_slot < 0)
        return;
      if (PpuSetOverlayCapture(g_ppu, kPpuOverlaySource_Obj,
                               0, 0, kActRaiserAuthenticWidth,
                               kActRaiserSimulationHudHeight,
                               kPpuOverlayFlag_RemoveFromGame) &&
          PpuSetOverlayOamRange(g_ppu, found_slot, found_count)) {
        s_hud_obj_icon_first = (uint8_t)found_slot;
        s_hud_obj_icon_count = (uint8_t)found_count;
      }
      return;
    }
    /* Town sim: hourglass is a 4-sprite animated icon in OAM slots 0-3. */
    uint8 upper_tile = (uint8)g_ppu->oam[1];
    if (upper_tile < kActRaiserSimulationHourglassFirstUpperTile ||
        upper_tile >= kActRaiserSimulationHourglassFirstUpperTile +
                          kActRaiserSimulationHourglassFrameCount)
      return;
    for (int slot = 0; slot < kActRaiserHudObjOamCount; slot++) {
      int index = slot * 2;
      uint16 xy = g_ppu->oam[index];
      uint16 tile_attr = g_ppu->oam[index + 1];
      uint8 expected_x = (slot & 1)
          ? kActRaiserSimulationHourglassRightX
          : kActRaiserSimulationHourglassLeftX;
      uint8 expected_y = slot < 2
          ? kActRaiserHudObjUpperY : kActRaiserHudObjLowerY;
      uint8 expected_tile = slot < 2 ? upper_tile
          : (uint8)(upper_tile +
                    kActRaiserSimulationHourglassLowerTileOffset);
      uint8 expected_attr = (slot & 1)
          ? kActRaiserSimulationHourglassRightAttr
          : kActRaiserSimulationHourglassLeftAttr;
      if ((uint8)xy != expected_x || (uint8)(xy >> 8) != expected_y ||
          (uint8)tile_attr != expected_tile ||
          (uint8)(tile_attr >> 8) != expected_attr)
        return;
    }
    capture_height = kActRaiserSimulationHudHeight;
  } else {
    return;
  }

  if (PpuSetOverlayCapture(g_ppu, kPpuOverlaySource_Obj,
                           0, 0, kActRaiserAuthenticWidth,
                           capture_height,
                           kPpuOverlayFlag_RemoveFromGame) &&
      PpuSetOverlayOamRange(g_ppu, kActRaiserHudObjOamFirst,
                            kActRaiserHudObjOamCount)) {
    s_hud_obj_icon_first = kActRaiserHudObjOamFirst;
    s_hud_obj_icon_count = kActRaiserHudObjOamCount;
  }
}

/* Split the promoted HUD icon back out of the diorama's OBJ planes. The
 * action-mode counterpart of sim3d.c's PrepareHudHandoff/RestoreTownHudPolicy
 * pair, and the reason the selected-magic icon is pinned beside the right HUD
 * group in diorama mode instead of riding the tilted scene at its authentic
 * centre-screen X.
 *
 * Why this is needed at all: ActRaiser_WidescreenHudObjPromote captures the
 * icon's four OAM slots into g_hud_obj_pixels for present.c to anchor, but the
 * diorama block later in this same frame re-captures OBJ as a full-frame scene
 * layer over slots 0..127 -- a strictly wider claim on the ONE capture slot the
 * PPU gives each source, and PpuSetOverlayCapture resets the OAM range as it
 * lands. That claim is right for the player and enemies and wrong for the HUD
 * icon, and there is no second OBJ capture to put the icon in, so the split has
 * to happen on captured pixels rather than on capture policy.
 *
 * Two phases, for the same reason sim3d has two:
 *
 *   Prepare, BEFORE scanout -- rasterize the icon's own OAM range. It must run
 *     here because the frame's mid-scanline IRQ handlers and HDMA rewrite VRAM
 *     and CGRAM as the picture is drawn: rasterizing afterwards reads whatever
 *     tile data the game streamed in next and yields a fully transparent icon,
 *     which is exactly the silent failure this comment exists to prevent.
 *   Finish, AFTER scanout -- publish that raster to g_hud_obj_pixels for the
 *     anchored overlay and clear the same pixels from the diorama plane that
 *     holds the icon's priority band, which only has content once scanout has
 *     run. The diorama's OBJ capture is RemoveFromGame, so the icon is already
 *     out of the backdrop frame; only the plane needs erasing. */
enum { kActRaiserHudIconRasterLimit = 64 };
static uint32_t s_hud_icon_raster[kActRaiserHudIconRasterLimit *
                                  kActRaiserHudIconRasterLimit];
static PpuObjRangeBounds s_hud_icon_bounds;
static int s_hud_icon_priority;
static bool s_hud_icon_ready;

/* The icon-footprint restore layer (see the block comment in
 * ActRaiser_DioramaHudObjPrepare): per footprint pixel, the colour and the
 * priority band of the sprite the promoted icon was covering. Static rather
 * than stack — three 64x64 scratch buffers is 36 KB, well past a sane frame. */
enum { kActRaiserHudRestoreNone = 0xFF };
static uint32_t s_hud_restore_argb[kActRaiserHudIconRasterLimit *
                                   kActRaiserHudIconRasterLimit];
static uint8_t s_hud_restore_prio[kActRaiserHudIconRasterLimit *
                                  kActRaiserHudIconRasterLimit];
static uint32_t s_hud_restore_slot[kActRaiserHudIconRasterLimit *
                                   kActRaiserHudIconRasterLimit];

/* g_diorama_layer_pixels[] index for an OBJ priority band. Band N is the plane
 * the diorama's kPrioBands table bound for band N, and band == OAM priority
 * (ppu.c's split does band = z >> 14, and SPRITE_PRIO_TO_PRIO puts the OAM
 * priority in those two bits). Band 0 is the primary source slot. */
static int ActRaiser_DioramaObjPlaneForPriority(int priority) {
  return priority ? kDioramaPlane_Obj1 + (priority - 1) : kPpuOverlaySource_Obj;
}

static void ActRaiser_DioramaHudObjPrepare(void) {
  extern bool g_diorama_frame_active;

  s_hud_icon_ready = false;
  /* Only the flat-HUD variant anchors a host overlay. With diorama_hud_flat
   * off the whole status bar is deliberately a tilted plane (see the A5/A7
   * note in the capture block), and the icon belongs on it. */
  if (!g_ppu || !g_diorama_frame_active || !g_settings.diorama_hud_flat ||
      !s_hud_obj_icon_count)
    return;

  const uint8_t first = s_hud_obj_icon_first;
  const uint8_t count = s_hud_obj_icon_count;
  const int priority = (g_ppu->oam[first * 2 + 1] >> 12) & 3;
  PpuObjRangeBounds bounds;
  if (!PpuGetObjRangeBounds(g_ppu, first, count, (uint8_t)priority, &bounds))
    return;
  const int raster_width = bounds.x1 - bounds.x0;
  const int raster_height = bounds.y1 - bounds.y0;
  /* Every promoted signature is 16x16, whether the ROM spent four small slots
   * on it or one large one; this ceiling is slack, not a target, and a range
   * that overruns it is refused rather than clipped. */
  if (raster_width <= 0 || raster_height <= 0 ||
      raster_width > kActRaiserHudIconRasterLimit ||
      raster_height > kActRaiserHudIconRasterLimit)
    return;
  if (!PpuRasterizeObjRange(g_ppu, first, count, (uint8_t)priority, &bounds,
                            s_hud_icon_raster, raster_width, raster_height,
                            (size_t)raster_width * sizeof(uint32_t)))
    return;

  s_hud_icon_bounds = bounds;
  s_hud_icon_priority = priority;
  s_hud_icon_ready = true;

  /* --- Restore layer: what the promoted sprites were HIDING ---------------
   *
   * Every captured OBJ pixel competes in ONE shared z-buffer
   * (ppu.c `overlayBuffers[kPpuOverlaySource_Obj]`: first opaque writer wins,
   * OAM walked in slot order); only at scanout is the surviving pixel routed
   * to the plane matching its own priority. The promoted icon is the LEADING
   * slot range, so wherever it overlaps a world sprite it wins the z-test and
   * that sprite's pixels are never captured into ANY plane.
   *
   * That is correct on hardware -- the icon is in front, so it hides what is
   * behind it. It stops being correct the moment we MOVE the icon to the HUD
   * anchor: zeroing it out of its own band then leaves a hole shaped like the
   * icon cut out of whatever stood behind it. Found 2026-08-05 as a bite taken
   * out of a level gargoyle, and it needs only an on-screen overlap, so it is
   * independent of the vertical band.
   *
   * Fix: replay the same first-writer-wins rule over the icon's footprint with
   * the promoted slots REMOVED, keeping each restored pixel's colour AND the
   * priority band it belongs to. Rasterised here, pre-scanout, for exactly the
   * reason the icon is -- mid-frame IRQ/HDMA rewrite VRAM and CGRAM.
   *
   * Not a full re-render: the hardware sprite-per-line limits are not replayed,
   * so a footprint contested by more than 34 slivers on a line could restore a
   * pixel the PPU would have dropped. Bounded by a 16x16 HUD icon, and failing
   * that way (showing the sprite) beats failing the other (a hole). */
  memset(s_hud_restore_prio, kActRaiserHudRestoreNone,
         (size_t)raster_width * raster_height);
  uint8_t index = PPU_objPriority(g_ppu) ? (uint8_t)(g_ppu->oamaddl & 0xfe) : 0;
  for (int evaluated = 0; evaluated < 128;
       evaluated++, index = (uint8_t)(index + 2)) {
    const int slot = index >> 1;
    if (slot >= first && slot < first + count)
      continue;                       /* the promoted sprites themselves */
    const int slot_priority = (g_ppu->oam[slot * 2 + 1] >> 12) & 3;
    PpuObjRangeBounds sb;
    if (!PpuGetObjRangeBounds(g_ppu, (uint8_t)slot, 1, (uint8_t)slot_priority,
                              &sb))
      continue;
    /* Cheap reject before rasterising: most slots cannot touch the icon. */
    if (sb.x1 <= bounds.x0 || sb.x0 >= bounds.x1 ||
        sb.y1 <= bounds.y0 || sb.y0 >= bounds.y1)
      continue;
    const int sw = sb.x1 - sb.x0, sh = sb.y1 - sb.y0;
    if (sw <= 0 || sh <= 0 ||
        sw > kActRaiserHudIconRasterLimit || sh > kActRaiserHudIconRasterLimit)
      continue;
    if (!PpuRasterizeObjRange(g_ppu, (uint8_t)slot, 1, (uint8_t)slot_priority,
                              &sb, s_hud_restore_slot, sw, sh,
                              (size_t)sw * sizeof(uint32_t)))
      continue;
    for (int y = 0; y < sh; y++) {
      const int fy = sb.y0 + y - bounds.y0;
      if (fy < 0 || fy >= raster_height) continue;
      for (int x = 0; x < sw; x++) {
        const int fx = sb.x0 + x - bounds.x0;
        if (fx < 0 || fx >= raster_width) continue;
        const size_t fi = (size_t)fy * raster_width + fx;
        if (s_hud_restore_prio[fi] != kActRaiserHudRestoreNone)
          continue;                   /* an earlier slot already won here */
        const uint32_t px = s_hud_restore_slot[(size_t)y * sw + x];
        if (!px) continue;
        s_hud_restore_argb[fi] = px;
        s_hud_restore_prio[fi] = (uint8_t)slot_priority;
      }
    }
  }
}

static void ActRaiser_DioramaHudObjFinish(int width) {
  extern uint8_t *g_diorama_layer_pixels[];
  extern uint8_t g_hud_obj_pixels[];

  /* Rows of g_hud_obj_pixels this pass last wrote. present.c uploads that
   * surface over the FULL capture height (224 rows in diorama mode), so rows
   * left behind by an earlier icon position would persist in the texture;
   * clearing just them beats a 224-row blanket memset for a 16x16 icon. */
  static int last_y0, last_y1;
  static size_t last_pitch;

  /* Bound against the PLANE width -- the wider of the two destinations, and the
   * one the apron grew. Both surfaces are allocated kPpuSurfaceWidth wide. */
  if (!s_hud_icon_ready || width <= 0 ||
      width + kPpuObjApron * 2 > kPpuSurfaceWidth)
    return;


  const int raster_width = s_hud_icon_bounds.x1 - s_hud_icon_bounds.x0;
  const int raster_height = s_hud_icon_bounds.y1 - s_hud_icon_bounds.y0;
  /* Band index == OAM priority (ppu.c's priority-split resolve does
   * band = z >> 14, and SPRITE_PRIO_TO_PRIO puts the OAM priority in those two
   * bits), so this is the same plane the diorama's kPrioBands table bound. */
  uint32_t *plane = (uint32_t *)g_diorama_layer_pixels[
      ActRaiser_DioramaObjPlaneForPriority(s_hud_icon_priority)];
  const size_t pitch = (size_t)width * 4;
  const int extra = (width - kActRaiserAuthenticWidth) / 2;
  /* TWO destinations, TWO widths. g_hud_obj_pixels stays at the DISPLAY width
   * (it is a screen-anchored overlay); the diorama OBJ planes are bound
   * APRON-wide. One `width` indexing both put the punch-out at the wrong stride
   * and the wrong column, so the promoted icon was never erased from the tilted
   * plane -- it rode the OBJ plane into the scene as a full-size sprite (the
   * priority-3 fire icon floating mid-scene, measured at gf1636 of
   * saves/artifacts2.rec).
   *
   * The plane side goes through the apron geometry rather than re-deriving it,
   * so this function and the apron pass cannot disagree about where a screen
   * column lands -- disagreeing is exactly what the bug WAS. */
  const ActionApronGeometry plane_geom = { extra, kPpuObjApron };
  const int plane_width = ActionApron_SurfaceWidth(&plane_geom);
  /* Two destinations, two row origins. g_hud_obj_pixels is consumed in
   * authentic screen space (the promoted HUD overlay), so it indexes by
   * screen_y. The diorama PLANE is consumed in capture space, whose row 0 is
   * screen y = -g_ws_extra_top, so the hole punched in it must carry that
   * offset -- otherwise the icon is erased from the wrong rows and a ghost of
   * it stays in the tilted OBJ plane. Zero without a vertical margin. */
  extern int g_ws_extra_top;
  const int plane_row_bias = g_ws_extra_top;

  /* Clear at the pitch the rows were WRITTEN at: a display-mode change between
   * frames alters `width`, and clearing at the new one would walk off the old
   * rows and leave the icon's last position smeared in the texture. */
  if (last_pitch)
    for (int y = last_y0; y < last_y1; y++)
      memset(g_hud_obj_pixels + (size_t)y * last_pitch, 0, last_pitch);
  last_y0 = last_y1 = 0;
  last_pitch = pitch;

  for (int y = 0; y < raster_height; y++) {
    const int screen_y = s_hud_icon_bounds.y0 + y;
    if (screen_y < 0 || screen_y >= kActRaiserAuthenticHeight) continue;
    uint32_t *dst = (uint32_t *)(g_hud_obj_pixels + (size_t)screen_y * pitch);
    for (int x = 0; x < raster_width; x++) {
      const int texture_x = s_hud_icon_bounds.x0 + x + extra;
      if (texture_x < 0 || texture_x >= width) continue;
      const uint32_t pixel = s_hud_icon_raster[(size_t)y * raster_width + x];
      if (!pixel) continue;
      dst[texture_x] = pixel;
      const size_t plane_index =
          (size_t)(screen_y + plane_row_bias) * plane_width +
          ActionApron_SurfaceColumn(&plane_geom, s_hud_icon_bounds.x0 + x);
      if (plane) plane[plane_index] = 0;
      /* Hand the pixel back to whatever the icon was covering, in ITS band --
       * the capture never recorded it, because the icon won the shared OBJ
       * z-test here. Only inside the icon's OPAQUE footprint: where the icon
       * was transparent the real capture already placed the right pixel in the
       * right band. Ordered after the zero above so a restore into the icon's
       * own band survives it. */
      const size_t footprint_index = (size_t)y * raster_width + x;
      const uint8_t restore_priority = s_hud_restore_prio[footprint_index];
      if (restore_priority != kActRaiserHudRestoreNone) {
        uint32_t *restore_plane = (uint32_t *)g_diorama_layer_pixels[
            ActRaiser_DioramaObjPlaneForPriority(restore_priority)];
        if (restore_plane)
          restore_plane[plane_index] = s_hud_restore_argb[footprint_index];
      }
      if (!last_y1) last_y0 = screen_y;
      if (screen_y + 1 > last_y1) last_y1 = screen_y + 1;
    }
  }
}

ActionApronGeometry ActRaiser_ObjApronGeometry(void) {
  extern bool g_ws_active;
  extern int g_ws_extra;
  ActionApronGeometry g = { g_ws_extra, 0 };
  /* The same condition host_display.c uses to pin the margin budget to
   * kWsExtraMax. Read from settings + geometry rather than from a per-frame
   * diorama flag on purpose: the emitter runs during game logic, BEFORE
   * ActRaiserDrawPpuFrame sets g_diorama_frame_active, so a per-frame flag
   * would be one frame stale exactly when it matters. */
  if (g_settings.diorama_mode && g_ws_active)
    g.apron = kPpuObjApron;
  return g;
}

/* Draw the apron part channel into the captured OBJ planes.
 *
 * Runs AFTER scanout, so the planes already hold this frame's in-window
 * sprites, and writes ONLY the two apron column bands. Never the display
 * window: those columns are scanout's, and writing them would both double-draw
 * a straddling part (with no z-test against the sprites it lost to) and break
 * the byte-identity gate this phase is judged on. The clip is structural --
 * PpuRasterizeParts takes the band as its `bounds` and crops to it.
 *
 * Ordering and band routing follow the hardware: OAM order decides who owns an
 * overlapping pixel via ONE shared z-test, and only the survivor's priority
 * decides which plane it lands on (see PpuWriteOverlayRenderLine's
 * priority-split resolve). So parts are drawn one at a time in list order and a
 * pixel already opaque in ANY of the four OBJ planes is left alone -- which is
 * exactly first-writer-wins across bands, with the planes themselves as the
 * claimed-set. That works because PpuClearOverlayRenderLine clears the full
 * bound pitch, apron included, every frame. */
static void ActRaiser_DioramaApronFinish(const ActionApronGeometry *geom) {
  extern uint8_t *g_diorama_layer_pixels[];
  extern int g_ws_extra_top;

  if (!geom || geom->apron <= 0 || !g_ppu || !ActionApron_Count())
    return;

  /* AR_APRONLOG=1: the channel's sizing verdict. peak/overflow answer "is
   * kActionApronMaxParts right?" without guessing, which is what the plan asks
   * for instead of assuming a capacity. */
  if (getenv("AR_APRONLOG"))
    fprintf(stderr, "[apron] gf=%u parts=%d peak=%d overflow=%d\n",
            ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
            ActionApron_Count(), ActionApron_PeakCount(),
            ActionApron_Overflow());

  const int surface_width = ActionApron_SurfaceWidth(geom);
  if (surface_width > kPpuSurfaceWidth)
    return;
  const PpuObjPart *parts = ActionApron_Parts();
  const int count = ActionApron_Count();
  const int rows = kActRaiserAuthenticHeight + g_ws_extra_top;

  /* Big enough for the largest SNES sprite (64x64). */
  static uint32_t scratch[64 * 64];

  int spans[2][2];
  ActionApron_LeftSpan(geom, &spans[0][0], &spans[0][1]);
  ActionApron_RightSpan(geom, &spans[1][0], &spans[1][1]);

  /* Resolved once, not per pixel: the claimed-set test below reads all four,
   * and re-deriving them inside the innermost loop made the plane mapping the
   * hottest thing in the pass. */
  uint32_t *planes[4];
  for (int p = 0; p < 4; p++)
    planes[p] = (uint32_t *)g_diorama_layer_pixels[
        ActRaiser_DioramaObjPlaneForPriority(p)];

  for (int i = 0; i < count; i++) {
    const PpuObjPart *part = &parts[i];
    const int priority = (part->tile_attr >> 12) & 3;
    uint32_t *plane = planes[priority];
    if (!plane)
      continue;
    const bool color_math =
        (g_ppu->overlayCaptures[kPpuOverlaySource_Obj].flags &
         kPpuOverlayFlag_MarkObjColorMath) &&
        ActionApron_PartUsesColorMath(part->tile_attr);

    for (int band = 0; band < 2; band++) {
      /* Intersect the part with this apron band; skip when it does not reach. */
      PpuObjRangeBounds win;
      win.x0 = (int16_t)(part->x > spans[band][0] ? part->x : spans[band][0]);
      win.x1 = (int16_t)(part->x + part->size < spans[band][1]
                             ? part->x + part->size
                             : spans[band][1]);
      if (win.x1 <= win.x0)
        continue;
      win.y0 = part->y;
      win.y1 = (int16_t)(part->y + part->size);
      const int w = win.x1 - win.x0, h = win.y1 - win.y0;
      if (w <= 0 || h <= 0 || w > 64 || h > 64)
        continue;
      if (!PpuRasterizeParts(g_ppu, part, 1, &win, scratch, w, h,
                             (size_t)w * sizeof(uint32_t)))
        continue;

      /* The destination columns are a contiguous run, so resolve the base once
       * per part instead of mapping and bounds-checking every pixel. The run is
       * inside the surface by construction: `win` was clipped to an apron band,
       * and a band's columns are always within [0, surface_width). */
      const int base_col = ActionApron_SurfaceColumn(geom, win.x0);
      if (base_col < 0 || base_col + w > surface_width)
        continue;

      for (int y = 0; y < h; y++) {
        /* Plane rows are CAPTURE space: row 0 is screen y = -g_ws_extra_top,
         * the same bias ActRaiser_DioramaHudObjFinish applies. */
        const int row = win.y0 + y + g_ws_extra_top;
        if (row < 0 || row >= rows)
          continue;
        const size_t row_base = (size_t)row * surface_width + base_col;
        for (int x = 0; x < w; x++) {
          uint32_t pixel = scratch[(size_t)y * w + x];
          if (!pixel)
            continue;
          const size_t index = row_base + x;
          bool claimed = false;
          for (int p = 0; p < 4 && !claimed; p++)
            if (planes[p] && planes[p][index])
              claimed = true;
          if (claimed)
            continue;
          if (color_math)
            pixel = (pixel & 0x00ffffffu) | 0x80000000u;
          plane[index] = pixel;
        }
      }
    }
  }
}

/* Fix B (SPEC-backdrop-clip.md): margin geometry of the last rendered frame,
 * latched at the end of ActRaiserDrawPpuFrame. See ActRaiser_LiveMargins. */
static int s_live_margin_left;
static int s_live_margin_right;
static int s_live_bg2_margin_source;
static int s_live_margin_top;

void ActRaiserDrawPpuFrame(void) {
  /* Overlay bindings are host-owned and persistent; capture policy is
   * game-owned and rebuilt every frame so no prior mode can leak a region. */
  if (Sim3D_BeginFrame())
    ActRaiser_RebindPpuOutputSurfaces();
  PpuClearOverlayCaptures(g_ppu);
  /* The exact-position OAM sideband (PpuSetObjExactPosition) is action-owned
   * state, rebuilt by ActRaiser_ObjectVisibilityScanWide at every object scan
   * — a scan that never runs outside action maps. An override that survives
   * the exit resurrects slots the next scene PARKS: the sprite evaluator
   * trusts a valid override over the OAM bytes on every line, so the first
   * sim cutscene drew parked tile-$000 slots at the last action frame's Ys
   * (ledger §34). Non-action frames only: action pause/freeze frames skip the
   * scan with OAM frozen, and their band sprites must keep rendering from the
   * same overrides that drew them. */
  if (!ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup]))
    PpuClearObjExactPositions(g_ppu);
  ActRaiser_ApplyWidescreenPolicy();
  /* Outside ApplyWidescreenPolicy, not inside it: that function early-returns
   * when widescreen is off, and the vertical margin must be re-resolved (most
   * often back to zero) on EVERY frame regardless -- a stale band would
   * otherwise survive a mode change. */
  ActRaiser_ApplyVerticalMarginPolicy(g_ram[kActRaiserWram_MapGroup],
                                      g_ram[kActRaiserWram_CurrentMap]);
  /* Stage D reconnaissance: read-only classification of objects that intersect
   * a live side margin but remain outside the authentic activation window. */
  ActRaiser_WidescreenSpriteActivationProbe();
  /* Stage B: populate BG1/BG2 margin tilemap cells transactionally before
   * scanline rendering. No-op under AR_WS_BGREFRESH=0 and outside action. */
  ActRaiser_WidescreenMarginRefresh();
  /* Sky Palace: synthesize only BG2's offscreen margin columns from its ROM
   * source page. The paired restore after scanout preserves UI staging. */
  ActRaiser_WidescreenSkyPalacePrepare();
  ActRaiser_WidescreenHudObjPromote();
  /* Manifest-driven HD substitutions (game-assets/manifest.ini) — e.g. the
   * settled title logo. Runs after the HUD/OAM capture policies so a busy
   * source is detected rather than clobbered; entries without host-loaded
   * art never request captures, keeping headless/oracle output authentic. */
  HdReplacements_EvaluateFrame();

  /* Diorama per-layer capture: when active (D toggle) or armed for a one-shot
   * dump (Shift+D), override all existing capture policies with full-frame
   * RemoveFromGame captures for BG1/2/3/OBJ. Bind dedicated diorama buffers
   * so we don't collide with the HUD/HD overlay surfaces. The captures
   * overwrite whatever the widescreen HUD split and HD replacements set
   * above — mutual exclusion for this frame. */
  {
    extern bool Diorama_IsActiveThisFrame(void);
    extern bool g_diorama_dump_pending;
    extern bool g_diorama_frame_active;
    extern uint8_t *g_diorama_layer_pixels[];
    bool active = Diorama_IsActiveThisFrame();
    bool want_capture = active ||
        (g_diorama_dump_pending &&
         ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup]));
    g_diorama_frame_active = active;
    if (want_capture) {
      extern bool g_ws_active;
      extern int g_ws_extra;
      extern int g_ws_extra_top;
      extern int g_ws_extra_bottom;
      int width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
      /* Apron-wide, matching the main framebuffer bind: the capture rect stays
       * scanline-bounded (the scanline path cannot fill apron columns), but the
       * wider pitch makes PpuWriteOverlayRenderLine's texture_extra centre the
       * captured span, leaving the apron columns free for capture-time part
       * rasterization to fill. */
      size_t pitch = ActionApron_SurfacePitch(width, kPpuObjApron);
      /* Capture rectangles are expressed in AUTHENTIC screen space, so
       * the vertical band starts at a negative y exactly as the side
       * margins start at -g_ws_extra. The PPU maps that onto row 0 of the
       * destination surface (PpuOutputRow), so a plane's texture row 0 is
       * screen y = -g_ws_extra_top -- the transpose of column 0 meaning
       * screen x = -g_ws_extra. */
      int capture_height =
          kActRaiserAuthenticHeight + g_ws_extra_top + g_ws_extra_bottom;
      /* A7/A5 (followup doc): BG3 (the status bar) is excluded from this
       * diorama capture loop whenever diorama_hud_flat is on (default) —
       * leaving the line-906 widescreen HUD split capture (PpuSetOverlayCapture
       * ... kPpuOverlaySource_Bg3 ... RemoveFromGame, above) standing instead
       * of being overridden by this block. That capture feeds
       * g_hud_bg_pixels/g_hud_bg_texture exactly as in flat mode, which is
       * what lets PresentComposite's diorama branch call
       * PresentHudOverlayComposited (present.c) and get the same widescreen
       * HUD anchoring (ACT/TIME/SCORE spread, boss-health full width) flat
       * mode already has. Before A7 BG3 was unconditionally rebound here, so
       * the last bind won — the anchored capture never survived and the HUD
       * only ever showed as an unanchored tilted plane.
       *
       * diorama_hud_flat=false (A5's A/B option) restores that pre-A7
       * behavior on purpose: BG3 captured here as an ordinary diorama layer,
       * rendered as the tilted plane (diorama.c's kDioramaLayers table), with
       * no anchored overlay. This is a game-thread read of the setting — the
       * present-side choice (whether to call PresentHudOverlayComposited)
       * uses the FrameSlot-snapshotted copy per D6. */
      static const PpuOverlaySource kCaptureLayersCommon[] = {
        kPpuOverlaySource_Bg1, kPpuOverlaySource_Bg2, kPpuOverlaySource_Obj,
      };
      /* F4 (2026-07-26 handback: "missing transparency on background layers in
       * diorama mode"). SNES colour math is not reproduced by the capture, so a
       * half-added BG used to arrive fully opaque and HIDE the planes behind it
       * instead of tinting them. Annotate those planes so the compositor draws
       * them at 50% instead — see kPpuOverlayFlag_MarkBgHalfAdd.
       *
       * Only the exact half-add-with-subscreen state qualifies, and it fails
       * closed: a full add is not an alpha blend, and a subtract is not either,
       * so both keep today's opaque capture rather than guessing.
       * Measured in Fillmore act 2 as cgwsel=$02 cgadsub=$43.
       *
       * The policy itself lives in diorama_capture_blend.c so it can be tested
       * without a ROM or a renderer. */
      for (int i = 0; i < (int)(sizeof(kCaptureLayersCommon) /
                                sizeof(kCaptureLayersCommon[0])); i++) {
        PpuOverlaySource src = kCaptureLayersCommon[i];
        if (!g_diorama_layer_pixels[src])
          g_diorama_layer_pixels[src] = calloc(1, kPpuSurfaceWidth * 4 * kHostDisplayFramebufferHeight);
        PpuBindOverlaySurface(g_ppu, src, g_diorama_layer_pixels[src], pitch);
        if (g_ppu->screenEnabled[0] & (1 << src)) {
          uint8_t flags = kPpuOverlayFlag_RemoveFromGame;
          /* OBJ keeps its own per-palette-group flag; this one is BG-only. */
          if (src != kPpuOverlaySource_Obj &&
              DioramaCaptureBlend_LayerIsHalfAdded(
                  (uint8_t)g_ppu->cgwsel, (uint8_t)g_ppu->cgadsub,
                  (uint8_t)g_ppu->screenEnabled[1], (uint8_t)(1 << src))) {
            flags |= kPpuOverlayFlag_MarkBgHalfAdd;
            /* Once per source: this is a fidelity change to the captured image,
             * so it should be visible in a log rather than inferred from
             * pixels. Silent on every stage that does not use this math.
             * AR_DIORAMA_BLEND_LOG=1 makes it per-frame instead, which is how
             * to tell a stage that never qualifies from one that qualifies only
             * on some frames (CGWSEL/CGADSUB are HDMA-writable per scanline, so
             * the value at capture-setup time is not necessarily the value
             * during scanout). */
            static bool reported[kPpuOverlaySource_Count];
            static int verbose = -1;
            if (verbose < 0) verbose = getenv("AR_DIORAMA_BLEND_LOG") ? 1 : 0;
            if (!reported[src] || verbose) {
              reported[src] = true;
              fprintf(stderr,
                      "[diorama-blend] gf=%u BG%d half-added with subscreen "
                      "(cgwsel=$%02x cgadsub=$%02x main=$%02x sub=$%02x) "
                      "-> captured at 50%% alpha\n",
                      ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
                      src + 1, g_ppu->cgwsel, g_ppu->cgadsub,
                      g_ppu->screenEnabled[0], g_ppu->screenEnabled[1]);
            }
          }
          PpuSetOverlayCapture(g_ppu, src, -g_ws_extra, -g_ws_extra_top,
                               width, capture_height, flags);
        }
      }
      /* BG3 needs its OWN branch, not just an on/off entry in the loop above:
       * PpuBindOverlaySurface is the ONLY thing that changes a source's
       * bound destination buffer, so a source simply left OUT of a frame's
       * capture list keeps whatever buffer the LAST frame bound it to —
       * there is no implicit "unbind." With diorama_hud_flat=true excluding
       * BG3 from a shared loop (as A7 originally did), toggling
       * flat->tilted->flat left BG3 permanently bound to the diorama layer
       * buffer from the tilted frame: g_hud_bg_pixels silently stopped
       * receiving fresh captures (frozen HUD) while the diorama buffer kept
       * getting live writes and diorama.c kept drawing it — the HUD showing
       * in two places at once (a live tilted ghost plus a frozen flat
       * overlay). Explicitly rebinding BOTH ways, every frame, is
       * self-healing regardless of toggle history. */
      if (g_settings.diorama_hud_flat) {
        extern uint8_t g_hud_bg_pixels[];
        /* The NARROW pitch, deliberately -- not the apron-wide `pitch` the
         * diorama planes bind at. This surface is not a diorama plane: it feeds
         * the anchored flat HUD overlay, which present.c uploads at
         * snes_width*4 (PresentUpload's hud rect). Binding it apron-wide made
         * the PPU write rows 2*kPpuObjApron columns apart while the upload read
         * them snes_width apart, shearing the HUD across the top of the screen.
         * The apron is resolve headroom for content that slides in past a
         * tilted plane's edge; a screen-anchored HUD has no such edge. */
        PpuBindOverlaySurface(g_ppu, kPpuOverlaySource_Bg3, g_hud_bg_pixels,
                              (size_t)width * 4);
        /* Do NOT issue the generic wide capture here — the line-906
         * HUD-split-specific capture region (0,0,kActRaiserAuthenticWidth,
         * hud_split_height) stays the authority for this source's X range;
         * that wide path is only for the diorama's OWN layers.
         *
         * BG3 carries more than the status bar, though: the act-title card
         * ("FILLMORE / ACT-1", BG3 tilemap rows 8/10 -> y=64..88) and the
         * pause text are the same layer, just below the split. In flat mode
         * they stay in the game framebuffer and are simply visible. In
         * diorama mode that framebuffer becomes the BACKDROP plane, drawn
         * first and then painted over by the BG2/BG1/OBJ planes — the text
         * silently disappeared behind the scene. Extend the SAME capture
         * rectangle down the full authentic height so those rows land in
         * g_hud_bg_pixels too; present.c draws everything below
         * hud_split_height as one flat, centered "body" chunk (the HUD
         * split's three anchored bands are driven by wsHudSplitHeight, not
         * by this rectangle, so their geometry is unchanged).
         *
         * Only extend an ALREADY-ACTIVE capture: when the widescreen policy
         * declined a HUD split (non-action map, 4:3 / RAW display mode) BG3
         * is not captured at all, and adding a RemoveFromGame capture with
         * nothing on the present side to draw it would erase BG3 outright. */
        PpuOverlayCapture *bg3_capture =
            &g_ppu->overlayCaptures[kPpuOverlaySource_Bg3];
        if (bg3_capture->y1 > bg3_capture->y0 &&
            bg3_capture->y1 < kActRaiserAuthenticHeight)
          PpuSetOverlayCapture(g_ppu, kPpuOverlaySource_Bg3,
                               bg3_capture->x0, bg3_capture->y0,
                               bg3_capture->x1 - bg3_capture->x0,
                               kActRaiserAuthenticHeight - bg3_capture->y0,
                               bg3_capture->flags);
      } else {
        if (!g_diorama_layer_pixels[kPpuOverlaySource_Bg3])
          g_diorama_layer_pixels[kPpuOverlaySource_Bg3] =
              calloc(1, kPpuSurfaceWidth * 4 * kHostDisplayFramebufferHeight);
        PpuBindOverlaySurface(g_ppu, kPpuOverlaySource_Bg3,
                              g_diorama_layer_pixels[kPpuOverlaySource_Bg3],
                              pitch);
        if (g_ppu->screenEnabled[0] & (1 << kPpuOverlaySource_Bg3))
          PpuSetOverlayCapture(g_ppu, kPpuOverlaySource_Bg3, -g_ws_extra,
                               -g_ws_extra_top, width, capture_height,
                               kPpuOverlayFlag_RemoveFromGame);
      }
      if (g_ppu->screenEnabled[0] & (1 << kPpuOverlaySource_Obj))
        PpuSetOverlayOamRange(g_ppu, 0, 128);
      /* Priority-band splits: scanout routes each captured pixel to the
       * surface matching its hardware priority (Mode-1 tile priority bit for
       * BGs, the 2-bit OAM priority for sprites), so the diorama can draw
       * the true Mode-1 interleave — foreground tiles over sprites, low
       * priority sprites behind the playfield. Bound after their primaries
       * because a primary rebind drops the band family. */
      static const struct { PpuOverlaySource src; int band; int plane; }
      kPrioBands[] = {
        { kPpuOverlaySource_Bg1, 1, kDioramaPlane_Bg1Hi },
        { kPpuOverlaySource_Bg2, 1, kDioramaPlane_Bg2Hi },
        { kPpuOverlaySource_Obj, 1, kDioramaPlane_Obj1 },
        { kPpuOverlaySource_Obj, 2, kDioramaPlane_Obj2 },
        { kPpuOverlaySource_Obj, 3, kDioramaPlane_Obj3 },
      };
      for (int i = 0; i < (int)(sizeof(kPrioBands) / sizeof(kPrioBands[0])); i++) {
        if (!g_diorama_layer_pixels[kPrioBands[i].plane])
          g_diorama_layer_pixels[kPrioBands[i].plane] =
              calloc(1, kPpuSurfaceWidth * 4 * kHostDisplayFramebufferHeight);
        PpuBindOverlayPrioSurface(g_ppu, kPrioBands[i].src,
                                  kPrioBands[i].band,
                                  g_diorama_layer_pixels[kPrioBands[i].plane]);
      }
    }
  }

  /* D2: claim observational full-frame Mode-1 captures only after every
   * pre-existing HUD/HD/diorama policy has had a chance to declare a
   * conflict. The original PPU framebuffer remains intact as same-frame A0. */
  {
    extern bool g_sim3d_textures_ready;
    extern bool g_diorama_frame_active;
    extern int g_ws_extra;
    uint8_t map_group = g_ram[kActRaiserWram_MapGroup];
    uint8_t map_number = g_ram[kActRaiserWram_CurrentMap];
    bool town = ActRaiser_IsSimulationTown(map_group, map_number);
    Sim3DCaptureRequest request = {
      .town = town,
      .master_enabled = g_settings.sim3d_mode,
      /* Mirror16, not ReadWram16: the picker flag lives at $7F:9215, so its
       * constant is 0x19215 -- 17 bits. The low-WRAM helper takes a uint16 and
       * silently truncated it to 0x9215, reading an unrelated byte pair in bank
       * $7E, so the picker never activated. ActRaiser_ReadWramMirror16 exists
       * precisely to make that impossible (actraiser_game.h) and every other
       * reader of this address already used it -- this was the one call site that
       * did not. -Wall reported it as "changes value from 102933 to 37397". */
      .picker_active = town && ActRaiser_SimMapPickerActiveForState(
          map_group, map_number,
          ActRaiser_ReadWramMirror16(kActRaiserWram_SimMapPickerFlag)),
      .renderer_ready = g_sim3d_textures_ready,
      .diorama_active = g_diorama_frame_active,
      /* The inspector panel is the only on-screen reader of the capture's
       * diagnostic hash; with it off, that pass is skipped. */
      .inspector_active = g_settings.scene_inspector,
      .requested_features = Settings_Sim3DRequestedFeatures(),
      .diagnostic_layer_mask = g_settings.sim3d_diagnostic_layers,
      .width = kActRaiserAuthenticWidth + 2 * g_ws_extra,
      .height = kActRaiserAuthenticHeight,
    };
    Sim3D_PrepareCapture(g_ppu, &request);
  }

  /* AR_TILE_CENSUS=1: read-only HD tile-pack sizing survey (hd_tile_census.c). */
  { extern void HdTileCensus_Frame(void); HdTileCensus_Frame(); }
  /* AR_TITLELOG=1: per-frame title-screen PPU probe (map bytes, BG mode,
   * HDMAEN, Mode-7 matrix, INIDISP) for deriving/validating the settled-logo
   * gate above. Diagnostic only. */
  if (getenv("AR_TITLELOG")) {
    static int last_gf = -1;
    int gf = (int)ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    if (gf != last_gf) {
      last_gf = gf;
      fprintf(stderr, "[titlelog] gf=%d $18=%02x $19=%02x bgmode=%02x "
              "hdmaen=%02x m7=[%04x %04x %04x %04x] inidisp=%02x\n",
              gf, g_ram[kActRaiserWram_MapGroup],
              g_ram[kActRaiserWram_CurrentMap],
              g_ppu->bgmode, g_snesrecomp_last_hdmaen,
              (uint16)g_ppu->m7matrix[0], (uint16)g_ppu->m7matrix[1],
              (uint16)g_ppu->m7matrix[2], (uint16)g_ppu->m7matrix[3],
              g_ppu->inidisp);
    }
  }
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

  /* D1b: isolate each semantic (record, priority) OAM fragment before the
   * scanline renderer consumes this exact OAM/VRAM/CGRAM state. The atlas is
   * presentation-only and the authentic framebuffer remains untouched. */
  if (ActRaiser_IsSimulationTown(g_ram[kActRaiserWram_MapGroup],
                                 g_ram[kActRaiserWram_CurrentMap])) {
    SimRenderAtlas_Build(
        g_ppu,
        ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraX),
        ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY));
  }

  /* Must precede the scanline loop: the IRQ handlers and HDMA below rewrite
   * VRAM/CGRAM mid-picture, so this is the last point at which the icon's tile
   * data is still the data the frame is being drawn with. */
  ActRaiser_DioramaHudObjPrepare();

  for (int i = 0; i <= 224; i++) {
    ppu_runLine(g_ppu, i);
    /* Vertical top margin (diorama). Placed HERE, not before the loop, for two
     * reasons that both have to hold:
     *
     *  - ppu_runLine(0) is the frame's setup pass (mosaic table, sprite
     *    overflow flags, the overlay content mask). Rendering an above-screen
     *    scanline before it would use last frame's state.
     *  - no HDMA channel has advanced yet, so these lines see the register
     *    values the frame STARTED with. That is the hold-first policy: there is
     *    no authentic per-scanline state above line 1 to reproduce, and holding
     *    the first line's is the only choice that is stable frame to frame.
     *
     * Rendered top-down so the band reads in the same order as the screen. */
    if (i == 0) {
      for (int m = g_ppu->extraTopCur; m >= 1; m--)
        ppu_runMarginLine(g_ppu, 1 - m);
    }
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
  /* Bottom margin, after the authentic loop, so these lines see the registers
   * the last visible scanline left behind (hold-last, the mirror of the
   * hold-first policy above). Inert today -- ActRaiser_ApplyWidescreenPolicy
   * never requests a bottom band, because OAM cannot express a below-screen
   * sprite (see g_ws_extra_bottom, main.c) -- but the loop is here so the
   * background half is already correct if that changes. */
  for (int m = 1; m <= g_ppu->extraBottomCur; m++)
    ppu_runMarginLine(g_ppu, 224 + m);
  {
    extern uint8_t g_pixels[];
    extern int g_ws_extra;
    int width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
    /* g_pixels is bound apron-wide; the authentic frame starts kPpuObjApron
     * columns in. Offset the base and pass the real pitch. */
    Sim3D_FinishCapture(
        g_pixels + ActionApron_DisplayOffset(kPpuObjApron),
        ActionApron_SurfacePitch(width, kPpuObjApron),
        ActRaiser_ReadWram16(kActRaiserWram_GameFrame));
    /* After scanout (the diorama planes only hold this frame's sprites now) and
     * before FrameSlot_Capture publishes them to the present thread. */
    ActRaiser_DioramaHudObjFinish(width);
    /* After the HUD-icon promote, not before: that pass PUNCHES the promoted
     * icon out of the OBJ planes, and the apron's claimed-set test reads those
     * planes. Running first would let a hole it is about to punch look like
     * free space. */
    const ActionApronGeometry apron_geom = ActRaiser_ObjApronGeometry();
    ActRaiser_DioramaApronFinish(&apron_geom);
  }
  /* Fix B (SPEC-backdrop-clip.md): latch the margin state the frame was ACTUALLY
   * rendered with, here, rather than letting FrameSlot_Capture read live g_ppu.
   * Between this function and the frame slot capture, main.c may call
   * ActRaiser_RebindPpuOutputSurfaces(), which reaches PpuSetExtraSpaceCentered
   * (hd_replacement_host.c) and ZEROES both live margins — reading g_ppu later
   * would silently describe a different frame than the pixels came from. That
   * path is !diorama_mode-gated today, so the bug would be latent rather than
   * live; latching makes it impossible either way. */
  s_live_margin_top = g_ppu->extraTopCur;

  if (getenv("AR_VEXT_LOG")) {
    /* Where each destination's content actually LANDED, which is the check that
     * catches the row-origin class of bug: the HUD surfaces are consumed in
     * AUTHENTIC screen space and must not move when the vertical margin
     * changes, while the diorama planes are consumed in CAPTURE space and must
     * move by exactly the margin. Both on one line so a regression in either
     * is one diff apart. */
    extern uint8_t g_hud_bg_pixels[];
    extern uint8_t *g_diorama_layer_pixels[];
    extern int g_ws_extra;
    int width = kActRaiserAuthenticWidth + 2 * g_ws_extra;
    size_t pitch = (size_t)width * 4;
    int hud0 = -1, hud1 = -1, plane0 = -1, plane1 = -1;
    for (int y = 0; y < kHostDisplayFramebufferHeight; y++) {
      const uint32_t *r = (const uint32_t *)(g_hud_bg_pixels + (size_t)y * pitch);
      for (int x = 0; x < width; x++)
        if (r[x]) { if (hud0 < 0) hud0 = y; hud1 = y; break; }
    }
    const uint8_t *bg2 = g_diorama_layer_pixels[kPpuOverlaySource_Bg2];
    /* The plane's own pitch, not the HUD's: the diorama planes are bound
     * apron-wide. A diagnostic that exists to catch origin bugs must not carry
     * one, and the wrong stride would slide its reported rows a little further
     * every row it walked. */
    const size_t plane_pitch = ActionApron_SurfacePitch(width, kPpuObjApron);
    if (bg2)
      for (int y = 0; y < kHostDisplayFramebufferHeight; y++) {
        const uint32_t *r = (const uint32_t *)(bg2 + (size_t)y * plane_pitch);
        for (int x = 0; x < width + kPpuObjApron * 2; x++)
          if (r[x]) { if (plane0 < 0) plane0 = y; plane1 = y; break; }
      }
    fprintf(stderr,
            "[vext-rows] gf=%u top=%d hudbg=[%d..%d] bg2plane=[%d..%d] "
            "objs_unlocked=%u\n",
            ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
            (int)g_ppu->extraTopCur, hud0, hud1, plane0, plane1,
            ActRaiser_TakeVextUnlockedObjects());
  }
  s_live_margin_left = g_ppu->extraLeftCur;
  s_live_margin_right = g_ppu->extraRightCur;
  s_live_bg2_margin_source = DioramaBg2MarginSource_Classify(
      g_ppu->wsLayerClamp, g_ppu->wsLayerMirror, g_ppu->wsLayerRepeat,
      g_ppu->wsRepeatY1[kActRaiserPpuLayer_Bg2] >
          g_ppu->wsRepeatY0[kActRaiserPpuLayer_Bg2]);

  /* Sky Palace BG2 is prepared at the top of this function and ALWAYS restored
   * here at the end. ActRaiserDrawPpuFrame has no early returns, so a pending
   * restore can never be stranded — keep it that way if you add control flow
   * above. */
  ActRaiser_WidescreenSkyPalaceRestore();
}

/* Same latch, same reason (see ActRaiser_LiveMargins): the vertical band the
 * frame was ACTUALLY rendered with, not whatever g_ppu holds by the time the
 * frame slot is captured. Separate accessor rather than more out-params so
 * the existing three-way callers stay untouched. */
int ActRaiser_LiveVerticalMargin(void) { return s_live_margin_top; }

/* See the latch above. Reports the margin geometry of the most recently rendered
 * frame, which is what a consumer of that frame's captured pixels must use. */
void ActRaiser_LiveMargins(int *left, int *right, int *bg2_margin_source) {
  if (left) *left = s_live_margin_left;
  if (right) *right = s_live_margin_right;
  if (bg2_margin_source) *bg2_margin_source = s_live_bg2_margin_source;
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

static const char *const kActRaiserMagicNames[] = {
  "none", "Magical Fire", "Magical Stardust", "Magical Aura", "Magical Light"
};

uint8 ActRaiser_SelectedMagic(void) {
  uint8 selected = g_ram[kActRaiserWram_SelectedMagic];
  return selected <= 4 ? selected : 0;
}

/* Set by the host input thread's edge dispatch, consumed once per frame by
 * ActRaiser_ApplyMagicCycle. Both sides run on the main thread (the present
 * thread is the one that was split out), so a plain flag is sufficient and a
 * held key cannot queue a burst: it is cleared unconditionally on read. */
static bool s_magic_cycle_requested;

void ActRaiser_RequestMagicCycle(void) { s_magic_cycle_requested = true; }

/* Which spells the save actually owns. The four inventory bytes $0299-$029C
 * hold spell ids (1..4) or 0 for an empty slot, and they are NOT sorted or
 * positional — All magic writes 1/2/3/4 in order, but a real save fills them
 * in pickup order. Collect the distinct ids so the cycle visits each unlocked
 * spell exactly once, in canonical Fire/Stardust/Aura/Light order rather than
 * in whatever order the player happened to find them. */
static unsigned ActRaiser_UnlockedMagic(uint8 out[4]) {
  bool present[5] = { false, false, false, false, false };
  for (unsigned slot = 0; slot < 4; slot++) {
    uint8 id = g_ram[kActRaiserWram_MagicInventory + slot];
    if (id >= 1 && id <= 4) present[id] = true;
  }
  unsigned count = 0;
  for (uint8 id = 1; id <= 4; id++)
    if (present[id]) out[count++] = id;
  return count;
}

/* Debug aid: step the action-stage spell selection to the next spell the save
 * has unlocked. Armed by the "Cycle magic spell" cheat and triggered by the
 * kInputAction_MagicCycle binding (keyboard or pad) — it no longer reserves a
 * SNES button, so it cannot shadow L in normal play. */
static void ActRaiser_ApplyMagicCycle(void) {
  bool requested = s_magic_cycle_requested;
  s_magic_cycle_requested = false;

  if (!requested || !g_settings.cheat_magic_cycle) return;

  /* Action stages only: $02AC and the $2D40 tile window are act-mode state,
   * and the sim-mode equip menu owns the selection there. Gated here rather
   * than at the call site so a press made in town is dropped outright instead
   * of firing the moment the next act loads. */
  if (!ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup])) {
    fprintf(stderr, "[magic-cycle] not in an action stage; ignored\n");
    return;
  }

  /* $00F8 is the act-mode cast state. Rewriting $02AC mid-cast would leave
   * the in-flight spell's actors running against the new spell's tiles. */
  if (g_ram[kActRaiserWram_MagicCastState] != 0) {
    fprintf(stderr, "[magic-cycle] cast still active; selection unchanged\n");
    return;
  }

  uint8 unlocked[4];
  unsigned count = ActRaiser_UnlockedMagic(unlocked);
  if (!count) {
    fprintf(stderr, "[magic-cycle] no spells unlocked; selection unchanged "
            "(enable the All magic cheat to test every spell)\n");
    return;
  }

  /* Advance past the current selection, wrapping. An unknown or unowned
   * current value (including 0 / "none") starts the cycle at the first
   * unlocked spell rather than being treated as an error. */
  uint8 current = g_ram[kActRaiserWram_SelectedMagic];
  unsigned index = 0;
  for (unsigned i = 0; i < count; i++) {
    if (unlocked[i] == current) { index = (i + 1) % count; break; }
  }
  uint8 next = unlocked[index];

  g_ram[kActRaiserWram_SelectedMagic] = next;
  ActRaiser_ReloadSelectedMagicTiles(next);
  fprintf(stderr, "[magic-cycle] selected %s (%u of %u unlocked, $02AC=$%02X, "
          "VRAM $2D40 refreshed)\n", kActRaiserMagicNames[next],
          index + 1, count, next);
}

/* Host-side cheat hooks (debug-menu scaffold). All settings-gated and seeded
 * OFF from their legacy env names, so they never affect a normal run. Applied
 * once per frame at the START of RunOneFrameOfGame (before the game's frame
 * logic), so a value pinned here is what the frame sees -> effective for death
 * prevention (HP) and physics override (moonjump). RAM is g_ram (WRAM): low
 * direct-page addrs map 1:1 ($1D player HP, $E6/$E7 timer, player object $08A0).
 * This is the framework the planned debug menu plugs into — see
 * docs/SEAMS.md "Gameplay / Tunable seams" + memory debug-menu-warp-roadmap. */
static int ActRaiser_BcdTimerToSeconds(uint8 low, uint8 high) {
  int low_pair = (low & 0x0F) + ((low >> 4) & 0x0F) * 10;
  int high_pair = (high & 0x0F) + ((high >> 4) & 0x0F) * 10;
  return low_pair + high_pair * 100;
}

void ActRaiser_ApplyCheats(void) {

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

  /* Action-stage gameplay tweaks only. MapGroup $01-$06 selects an ordinary
   * action region, $07 is Death Heim, $00 is the non-action town/world/UI
   * engine, and $08 is the ending presenter. Gate on the whole action range so
   * cheats persist across every region, not just Fillmore ($18==$01) — that
   * bug disabled them after warping to region 2+. The player object/HP/timer
   * fields are shared by the action engine across every region, so the same
   * writes apply everywhere. */
  /* ── ALL-MODE cheats (above the action-stage gate: they feed the sim-mode
   * equip menu / angel, so they must pin in every mode) ─────────────────── */

  /* AR_ALL_MAGIC=1: unlock all four spells. HAVE flags $0299-$029C = 01/02/03/04
   * (cheat-map values, docs/ram-map.md). Pinned in ALL modes so the sim-mode
   * equip menu lists them; SELECTING one still goes through the menu (the equip
   * routine $01:915D derives $02AC from these). DEBUG.md #18 has the full
   * magic wiring map. */
  {
    if (g_settings.cheat_all_magic) {
      g_ram[kActRaiserWram_MagicInventory + 0] = 0x01; /* Magical Fire */
      g_ram[kActRaiserWram_MagicInventory + 1] = 0x02; /* Magical Stardust */
      g_ram[kActRaiserWram_MagicInventory + 2] = 0x03; /* Magical Aura */
      g_ram[kActRaiserWram_MagicInventory + 3] = 0x04; /* Magical Light */
    }
  }

  /* AR_RANGED_SWORD=1: sword fires a projectile ($E4 = $80, PAR 7E00E480). */
  if (g_settings.cheat_ranged_sword)
    g_ram[kActRaiserWram_RangedSwordFlag] = 0x80;

  /* AR_INF_MP: infinite magic scrolls. =1 -> pin the WORKING count $21 to 10
   * (PAR 7E00210A); =<n> -> pin to n. Deliberately does NOT touch the
   * PERSISTENT count $0295 (DEBUG.md #18b: $21 is the act-mode working copy,
   * loaded from $0295 at $02:84E0) so the cheat never bakes into save.srm. */
  if (g_settings.cheat_inf_mp)
    g_ram[kActRaiserWram_WorkingMagicPoints] =
        (uint8)g_settings.cheat_inf_mp;

  /* AR_INF_SP=1: infinite sim-mode SP (miracle points). Self-calibrating: pins
   * current SP $0282/16 to max SP $0284/16 once max is known (vs the PAR code's
   * blunt $FF, which over-fills early-game maxima). */
  {
    if (g_settings.cheat_inf_sp &&
        ActRaiser_ReadWram16(kActRaiserWram_AngelMaximumSp)) {
      ActRaiser_WriteWram16(
          kActRaiserWram_AngelCurrentSp,
          ActRaiser_ReadWram16(kActRaiserWram_AngelMaximumSp));
    }
  }

  /* AR_ANGEL_HP=1: infinite sim-mode angel health. Self-calibrating: pins
   * current HP $0286 to max HP $0287 (the PAR code 7E028608 hardcodes 8, which
   * would UNDER-fill after level-ups raise the max). */
  if (g_settings.cheat_angel_hp &&
      g_ram[kActRaiserWram_AngelMaximumHp])
    g_ram[kActRaiserWram_AngelCurrentHp] =
        g_ram[kActRaiserWram_AngelMaximumHp];

  /* Live action-stage magic selection/asset reload for effect testing. Above
   * the action-stage gate so a request made outside an act is consumed and
   * discarded rather than queued until the next act loads; the handler
   * applies the gate itself. */
  ActRaiser_ApplyMagicCycle();

  if (!ActRaiser_IsActionMapGroup(g_ram[kActRaiserWram_MapGroup])) return;

  /* AR_INF_HP: infinite health. =1 -> auto: pin player HP ($1D) to the
   * high-water max seen this stage (self-calibrates to "full" once you've been
   * at full, so we needn't know max HP statically). =<n> -> pin to literal n. */
  {
    static int previous_cheat_mode;
    static unsigned highest_hp;
    int cheat_mode = g_settings.cheat_inf_hp;
    if (cheat_mode != previous_cheat_mode) {
      /* Entering/re-entering auto mode must calibrate from the current stage,
       * not reuse a high-water value captured before a live toggle. */
      if (cheat_mode == 1) highest_hp = 0;
      previous_cheat_mode = cheat_mode;
    }
    if (cheat_mode) {
      if (cheat_mode > 1) {
        g_ram[kActRaiserWram_PlayerHp] = (uint8)cheat_mode;
      } else {
        unsigned current_hp = g_ram[kActRaiserWram_PlayerHp];
        if (current_hp > highest_hp && current_hp <= 0xFF)
          highest_hp = current_hp;
        if (highest_hp)
          g_ram[kActRaiserWram_PlayerHp] = (uint8)highest_hp;
      }
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
    static uint8 captured_timer_low;
    static uint8 captured_timer_high;
    static int timer_captured;
    static int drain_detected;
    static uint8 last_map_group = kActRaiserUnknownMapGroup;
    static int cheat_was_enabled;
    if (!g_settings.cheat_freeze_timer) {
      /* A future live off/on toggle starts a fresh capture and drain latch. */
      cheat_was_enabled = 0;
      timer_captured = 0;
      drain_detected = 0;
      last_map_group = kActRaiserUnknownMapGroup;
    } else {
      if (!cheat_was_enabled) {
        cheat_was_enabled = 1;
        timer_captured = 0;
        drain_detected = 0;
        last_map_group = kActRaiserUnknownMapGroup;
      }
      if (g_ram[kActRaiserWram_MapGroup] != last_map_group) {
        last_map_group = g_ram[kActRaiserWram_MapGroup];
        timer_captured = 0;
        drain_detected = 0;
      }
      if (!timer_captured) {
        captured_timer_low = g_ram[kActRaiserWram_ActionTimerLow];
        captured_timer_high = g_ram[kActRaiserWram_ActionTimerHigh];
        timer_captured = 1;
      }
      if (!drain_detected) {
        int captured_seconds = ActRaiser_BcdTimerToSeconds(
            captured_timer_low, captured_timer_high);
        int current_seconds = ActRaiser_BcdTimerToSeconds(
            g_ram[kActRaiserWram_ActionTimerLow],
            g_ram[kActRaiserWram_ActionTimerHigh]);
        if (captured_seconds - current_seconds > 1) {
          drain_detected = 1;
        } else {
          g_ram[kActRaiserWram_ActionTimerLow] = captured_timer_low;
          g_ram[kActRaiserWram_ActionTimerHigh] = captured_timer_high;
        }
      }
    }
  }

  /* AR_MOONJUMP=1: hold the game's jump button (SNES B) to FLY UP. We move the player
   * Y-POSITION ($08A4, +$04) directly rather than the Y-velocity ($08A8, +$08) —
   * object fields are polymorphic by state, so $08A8 is "Y-velocity" only in the
   * AIR state; while grounded it means something else (writing it there didn't
   * launch and leaked into other movement). Position is always position, so
   * decrementing $08A4 (screen-Y grows downward, so −Y = up) is a reliable
   * state-independent fly. AR_MOONJUMP_SPEED sets up-speed in pixels/frame
   * (default 6). The trigger follows the game's fixed jump mapping instead of
   * exposing a second, potentially contradictory cheat binding. */
  {
    enum { kActRaiserJoypadJump = 0x8000 };  /* auto-joypad SNES B bit */
    if (g_settings.cheat_moonjump) {
      extern Snes *g_snes;
      uint16 buttons = SwapInputBits(g_snes->input1_currentState);
      if (buttons & kActRaiserJoypadJump) {
        uint16 player_y =
            ActRaiser_ReadWram16(kActRaiserWram_PlayerPositionY);
        player_y = (uint16)(player_y - g_settings.cheat_moonjump_speed);
        ActRaiser_WriteWram16(kActRaiserWram_PlayerPositionY, player_y);
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
      g_ram[kActRaiserWram_PlayerInvulnerabilityTimer] = 0;
      g_ram[kActRaiserWram_PlayerFlags + 1] &=
          (uint8)~kActRaiserPlayerFlag_InvulnerableHighByte;
    }
    prior_mode = mode;
    if (mode) {
      if (mode == 1) {
        g_ram[kActRaiserWram_PlayerInvulnerabilityTimer] = 0xFF;
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
        int cast_imminent =
            (g_ram[kActRaiserWram_InputHeldHigh] & 0xC0) != 0 &&
            g_ram[kActRaiserWram_MagicCastState] == 0 &&
            g_ram[kActRaiserWram_SelectedMagic] != 0 &&
            g_ram[kActRaiserWram_WorkingMagicPoints] != 0;
        if (cast_imminent)
          g_ram[kActRaiserWram_PlayerFlags + 1] &=
              (uint8)~kActRaiserPlayerFlag_InvulnerableHighByte;
        else
          g_ram[kActRaiserWram_PlayerFlags + 1] |=
              kActRaiserPlayerFlag_InvulnerableHighByte;
      } else {
        g_ram[kActRaiserWram_PlayerObject + (unsigned)mode] = 0xFF;
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
  uint8 source_map_group = g_ram[kActRaiserWram_MapGroup];
  uint8 source_map = g_ram[kActRaiserWram_CurrentMap];
  g_ram[kActRaiserWram_DestinationMapGroup] = (uint8)region;
  g_ram[kActRaiserWram_DestinationMap] = (uint8)map;
  g_ram[kActRaiserWram_TransitionRequest] |= kActRaiserTransitionRequestBit;
  fprintf(stderr, "[warp] from $18=$%02X $19=$%02X staged region=$%02X "
          "map=$%02X ($1B/$1A/$FB set); transition processor should pick it up.\n",
          source_map_group, source_map, region & 0xFF, map & 0xFF);
  if (ActRaiser_IsActionMapGroup(source_map_group)) {
    fprintf(stderr, "[warp] WARNING: action->action is not a naturally observed "
            "transition; inherited timing/object state may affect fidelity.\n");
  }
}

/* (Re)create the game coroutine on its own 2MB stack. Split out of
 * RunOneFrameOfGame so the watchdog-trip path can rebuild it: a tripped frame
 * was left SUSPENDED mid-spin (the trip yields rather than unwinding — see
 * WatchdogCheck), so resuming that context would re-enter the same infinite
 * loop. Returns false if the coroutine could not be created. */
static bool CreateGameCoroutine(void) {
#ifdef _WIN32
  /* FIBER_FLAG_FLOAT_SWITCH is REQUIRED, not optional: MS documents that with
   * flags zero "the floating-point state on x86 systems is not switched and
   * data can be corrupted if a fiber uses floating-point arithmetic" — and the
   * game coroutine does use FP (the watchdog's `double elapsed`, the DSP
   * resample phase). Committing 64KB of the 2MB reserve up front instead of the
   * whole thing keeps the fiber cheap to (re)create while still reserving the
   * full stack; the recompiled dispatch stack can go 64 frames deep. */
  if (!g_host_fiber) {
    g_host_fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    if (!g_host_fiber) {
      fprintf(stderr, "Failed to convert driver thread to fiber\n");
      return false;
    }
  }
  if (g_game_fiber) {
    DeleteFiber(g_game_fiber);
    g_game_fiber = NULL;
  }
  g_game_fiber = CreateFiberEx(64 * 1024, GAME_STACK_SIZE,
                               FIBER_FLAG_FLOAT_SWITCH,
                               game_coroutine_fiber, NULL);
  if (!g_game_fiber) {
    fprintf(stderr, "Failed to create game coroutine fiber\n");
    return false;
  }
#else
  if (!g_game_stack) {
    /* mmap with a PROT_NONE GUARD PAGE below the stack rather than malloc.
     * makecontext requires the caller to supply the stack, and with an
     * app-supplied stack "it is the application's responsibility to handle
     * stack overflow" — a malloc'd stack has no guard, so a deep recompiled
     * dispatch chain that overruns it would silently scribble over whatever
     * the allocator placed underneath (heap corruption, arbitrary later
     * crash). With a guard page the overflow faults immediately, at the site
     * that caused it. */
    long page = sysconf(_SC_PAGESIZE);
    size_t guard = page > 0 ? (size_t)page : 4096u;
    size_t total = GAME_STACK_SIZE + guard;
    void *map = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) {
      fprintf(stderr, "Failed to allocate game coroutine stack\n");
      return false;
    }
    /* Stacks grow DOWN, so the guard belongs at the lowest address. */
    if (mprotect(map, guard, PROT_NONE) != 0)
      fprintf(stderr, "[coroutine] guard page unavailable (%s) — stack "
                      "overflow will not fault cleanly\n", strerror(errno));
    g_game_stack_map = map;
    g_game_stack_map_len = total;
    g_game_stack = (char *)map + guard;
  }
  /* The abandoned context is just register state pointing into this stack;
   * re-running makecontext over the SAME buffer resets the entry point, so no
   * unmap/remap is needed (and none would be safe while the old context's
   * frames still nominally live there). */
  if (getcontext(&g_game_ctx) != 0) {
    fprintf(stderr, "Failed to capture game coroutine context\n");
    return false;
  }
  g_game_ctx.uc_stack.ss_sp = g_game_stack;
  g_game_ctx.uc_stack.ss_size = GAME_STACK_SIZE;
  g_game_ctx.uc_link = &g_host_ctx;
  makecontext(&g_game_ctx, game_coroutine, 0);
#endif
  return true;
}

/* Release the coroutine's stack/fiber. Called from the game's shutdown path so
 * the guard-page mapping and the fiber are not leaked, and so a leak checker
 * run against a clean exit stays quiet. Safe to call without a coroutine. */
void ActRaiser_DestroyGameCoroutine(void) {
#ifdef _WIN32
  if (g_game_fiber) {
    DeleteFiber(g_game_fiber);
    g_game_fiber = NULL;
  }
#else
  if (g_game_stack_map) {
    munmap(g_game_stack_map, g_game_stack_map_len);
    g_game_stack_map = NULL;
    g_game_stack_map_len = 0;
    g_game_stack = NULL;
  }
#endif
  g_game_started = false;
}

void RunOneFrameOfGame(void) {
  if (!g_game_started) {
    g_game_started = true;
#if AR_WATCHDOG
    /* Give the runtime watchdog the coroutine yield to escape a stuck frame
     * with (the old longjmp out of this coroutine was UB / fiber-forbidden). */
    { extern void (*g_watchdog_yield_hook)(void);
      g_watchdog_yield_hook = ActRaiser_YieldToHost; }
#endif
    if (!CreateGameCoroutine()) return;
  }

  /* A previous frame tripped the watchdog: its coroutine is suspended inside
   * the spin it never left, so rebuild rather than resume. The emulated CPU
   * state is re-initialized by game_coroutine's ResetHandler — i.e. this is a
   * soft reset, which is the honest outcome of an unrecoverable hang. */
  if (g_watchdog_tripped) {
    fprintf(stderr, "[watchdog] rebuilding the game coroutine after a hang "
                    "(the abandoned frame cannot be resumed)\n");
    g_watchdog_tripped = 0;
    if (!CreateGameCoroutine()) return;
  }

  ActRaiser_ApplyCheats();   /* host-side cheats (live settings, default off) */

  { extern int ar_trace_active(void); extern void ar_trace_frame(const char *);
    if (ar_trace_active()) ar_trace_frame("vblank"); }  /* frame boundary marker */
  if (g_action_load_hold_frames) {
    bool one_shot_completed = false;
    uint64_t one_shot_token = 0;
    if (g_action_load_one_shot_token) {
      one_shot_token =
          MusicReplacements_GetOneShotSnapshot(&one_shot_completed);
    }
    if (ActionLoadPacing_ShouldReleaseForOneShot(
            g_action_load_hold_frames, g_action_load_one_shot_token,
            one_shot_token, one_shot_completed)) {
      if (getenv("AR_LOADPACELOG")) {
        fprintf(stderr,
                "[load-pace] f=%d HD one-shot complete; released forced "
                "blank %u frame(s) early\n",
                snes_frame_counter, g_action_load_hold_frames);
      }
      g_action_load_hold_frames = 0;
      g_action_load_one_shot_token = 0;
    } else {
      g_action_load_hold_frames--;
      if (!g_action_load_hold_frames)
        g_action_load_one_shot_token = 0;
      return;
    }
  }
  g_snes->forceNmi = true;
  g_snes->nmiAvail = true;   /* fresh RDNMI ($4210 bit7) vblank token this frame */
#ifdef _WIN32
  SwitchToFiber(g_game_fiber);
#else
  if (swapcontext(&g_host_ctx, &g_game_ctx) != 0) {
    fprintf(stderr, "FATAL: swapcontext (host -> game) failed\n");
    abort();
  }
#endif
  g_snes->forceNmi = false;

  /* $4200 bit 7 is the hardware NMI gate. This matters when a game-coroutine
   * yield models CPU time rather than a vblank wait: the display and APU still
   * advance, but the disabled NMI handler must not advance $0088 or game state.
   * RDNMI's vblank token above remains available independently of this gate. */
  if (!g_snes->nmiEnabled)
    return;

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
