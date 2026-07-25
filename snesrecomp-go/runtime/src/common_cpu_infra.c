#include "common_cpu_infra.h"
#include "framedump.h"
#include "types.h"
#include "common_rtl.h"
#include "recomp_hw.h"
#include "snes/cpu.h"
#include "snes/snes.h"
#include "snes/msu1.h"
#include "util.h"
#include "cpu_trace.h"
#include "debug_server.h"
#include <setjmp.h>
#include <string.h>
#include <time.h>

Snes *g_snes;
Cpu *g_snes_cpu;

bool g_fail;
const RtlGameInfo *g_rtl_game_info;

void RtlRegisterGame(const RtlGameInfo *info) {
  g_rtl_game_info = info;
  /* Arm MSU-1 from the environment for every game, with no per-game
   * wiring. Inert (default-OFF) unless SNESRECOMP_MSU1 is set. A game's
   * main.c may additionally call msu1_set_rom_path() to enable the
   * "auto" base-from-ROM-name mode. */
  msu1_init();
}

uint8_t *SnesRomPtr(uint32 v) {
  return (uint8 *)RomPtr(v);
}

// Apply the native-mode CPU state the real ROM's reset vector would
// have established. See header comment.
void SnesEnterNativeMode(void) {
  g_snes_cpu->e = false;
  g_snes_cpu->sp = 0x01FF;
  g_snes_cpu->dp = 0;
  g_snes_cpu->mf = false;
  g_snes_cpu->xf = false;
  g_snes_cpu->d = false;
  g_snes_cpu->i = true;
}

// Resolve a 16-bit-indirect-through-DP pointer using the current
// data bank register. See comment in common_rtl.h for why this
// matters for `(dp)`, `(dp),Y`, `(dp,X)` addressing modes.
uint8_t *IndirPtrDB(uint8 dp_addr, uint16 offs) {
  LongPtr p = MAKE_LONG((uint16)g_ram[dp_addr] | ((uint16)g_ram[dp_addr + 1] << 8),
                        g_snes_cpu->db);
  return IndirPtr(p, offs);
}

// Debug: recomp function call stack for watchdog diagnostics.
const char *g_last_recomp_func = "(none)";


/* Always-on lightweight block-PC ring (works in non-trace builds, where
 * cpu_trace.c's ring is absent). Written by the inline cpu_trace_block;
 * read by the watchdog dump to reveal an infinite-loop's block cycle. */
#define AR_BLK_RING 1024
uint32_t g_ar_blk_ring[AR_BLK_RING];
uint32_t g_ar_blk_aux[AR_BLK_RING];   /* (x_flag<<17) | (m_flag<<16) | (X & 0xFFFF) */
uint16_t g_ar_blk_s[AR_BLK_RING];     /* cpu->S at each block entry (stack-drift trace) */
unsigned g_ar_blk_idx = 0;

int ar_block_history(uint32_t *out, int max) {
  if (max > AR_BLK_RING) max = AR_BLK_RING;
  unsigned total = g_ar_blk_idx;
  int n = (total < (unsigned)max) ? (int)total : max;
  for (int i = 0; i < n; i++)
    out[i] = g_ar_blk_ring[(g_ar_blk_idx - n + i) & (AR_BLK_RING - 1)];
  return n;
}


/* Software-interrupt (BRK/COP) hooks — see cpu_state.h. NULL by default; the
 * game installs a handler. Generated code calls these at BRK/COP sites. */
void (*g_cpu_brk_hook)(CpuState *cpu) = 0;
void (*g_cpu_cop_hook)(CpuState *cpu) = 0;
// Tier 1.5 call-trace depth cap. Originally 16; bumped to 64 because
// SMW peak call depth is ~10 but Tier 1.5 attribution silently
// degrades past the cap (g_last_recomp_func and parent fields go
// stale). 64 gives 6x headroom for any conceivable call chain at
// negligible memory cost (8 bytes/slot * 48 extra slots = 384 bytes).
#define RECOMP_STACK_DEPTH 64
const char *g_recomp_stack[RECOMP_STACK_DEPTH];
int g_recomp_stack_top = 0;
unsigned long g_recomp_push_count = 0;  /* total function entries; per-frame work meter */

/* Per-frame 65816 stack-entry level (cpu->S at function entry), parallel
 * to g_recomp_stack and indexed by the same g_recomp_stack_top. The
 * function prologue records _entry_s here; pops are implicit (top--).
 * Used by cpu_resolve_ancestor_skip() to turn a return-to-ancestor RTS
 * (manual PLA/PLX/PLB rebalance to an ancestor's entry level, then RTS)
 * into a SKIP_N non-local return through the existing call-site
 * decrement contract. See ISSUES.md "shared-tail multi-level non-local
 * return" (the fish-explosion OAM wipe). */
uint16_t g_cpu_entry_s[RECOMP_STACK_DEPTH];
/* Parallel to g_cpu_entry_s: whether each frame was entered via a paired
 * host-C caller (hrv=1, i.e. a real JSR/JSL with a C return) vs a tail/
 * dispatch (hrv=0). A return-to-ancestor SKIP must NOT unwind past a paired-
 * host-caller frame — that frame owes its C caller a normal return, so a skip
 * crossing it corrupts control flow (e.g. ActRaiser action-stage object loop
 * $8915 emitting SKIP_11 that escaped its 82E2 JSR caller and blew past the
 * $82D1 fade-in loop -> black playfield). cpu_resolve_ancestor_skip stops the
 * scan at the nearest such boundary. */
uint8_t g_cpu_entry_hrv[RECOMP_STACK_DEPTH];
static uint8_t g_tailcall_context_valid;
static uint16_t g_tailcall_entry_s;
static uint8_t g_tailcall_hrv;

void cpu_tailcall_inherit_return_context(uint16_t entry_s, uint8_t hrv) {
  g_tailcall_entry_s = entry_s;
  g_tailcall_hrv = hrv;
  g_tailcall_context_valid = 1;
}

int cpu_take_tailcall_return_context(uint16_t *entry_s, uint8_t *hrv) {
  if (!g_tailcall_context_valid) return 0;
  if (entry_s) *entry_s = g_tailcall_entry_s;
  if (hrv) *hrv = g_tailcall_hrv;
  g_tailcall_context_valid = 0;
  return 1;
}

/* Trampoline: a dispatched frame (hrv=0) stashes its next computed-jump target
 * here and returns RECOMP_RETURN_TAILCALL; the cpu_dispatch_pc_from driving loop
 * consumes it and iterates instead of nesting. Set immediately before the
 * TAILCALL return and consumed immediately by the loop (no yield in between),
 * so a single global trio is safe (re-entrant NMI dispatch fully completes
 * before the main chain resumes). */
uint32_t g_tailcall_pc24;
uint16_t g_tailcall_miss_s;
uint32_t g_tailcall_src24;
void cpu_tailcall_request(uint32_t pc24, uint16_t miss_s, uint32_t src24) {
  g_tailcall_pc24 = pc24 & 0xFFFFFFu;
  g_tailcall_miss_s = miss_s;
  g_tailcall_src24 = src24 & 0xFFFFFFu;
}

int cpu_resolve_ancestor_skip(uint16_t ret_s) {
  /* The current (top-1) frame is the one whose RTS we are resolving; it
   * is NOT a match (its entry_s != ret_s, else the balanced host-return
   * path handled it). Scan STRICT ancestors for the nearest frame whose
   * entry_s == ret_s — that frame should host-return NORMAL to its
   * caller (which resumes at its natural continuation). Return the SKIP
   * count = how many RECOMP_RETURN levels to unwind to reach it; -1 if
   * none (caller falls back to the normal dispatch-miss path, no change
   * in behavior). */
  int top = g_recomp_stack_top;
  if (top < 2 || top > RECOMP_STACK_DEPTH) return -1;
  /* Reconstruct the RTS return PC from the SNES stack (same math as the
   * generated callsite) so the log shows WHERE the loop is trying to return. */
  extern uint8 g_ram[0x20000];
  uint16 _rpcl = g_ram[(uint16)(ret_s + 1)];
  uint16 _rpch = g_ram[(uint16)(ret_s + 2)];
  uint16 _rpc  = (uint16)((((_rpch << 8) | _rpcl) + 1) & 0xFFFFu);
  for (int i = top - 2; i >= 0; i--) {
    if (g_cpu_entry_s[i] == ret_s) {
      int dist = (top - 1) - i;
      if (dist >= 2 && getenv("AR_ANCLOG")) {
        extern const char *g_recomp_stack[];
        extern int snes_frame_counter;
        static int n;
        if (n++ < 80) {
          fprintf(stderr, "[anc] skip=%d retpc=%04x target[%d]=%s (top-1)=%s hrv@tgt=%u f=%d | stk:",
                  dist, _rpc, i, g_recomp_stack[i] ? g_recomp_stack[i] : "?",
                  g_recomp_stack[top-1] ? g_recomp_stack[top-1] : "?",
                  (unsigned)g_cpu_entry_hrv[i], snes_frame_counter);
          for (int j = top - 1; j >= 0 && j >= top - 12; j--)
            fprintf(stderr, " [%d]%s@%04x", j, g_recomp_stack[j] ? g_recomp_stack[j] : "?",
                    g_cpu_entry_s[j]);
          fprintf(stderr, "\n");
        }
      }
      return dist;
    }
  }
  if (getenv("AR_ANCLOG")) {
    extern const char *g_recomp_stack[];
    extern int snes_frame_counter;
    static int nm;
    if (nm++ < 40)
      fprintf(stderr, "[anc] NO-MATCH retpc=%04x ret_s=%04x (top-1)=%s top=%d f=%d\n",
              _rpc, ret_s, g_recomp_stack[top-1] ? g_recomp_stack[top-1] : "?",
              top, snes_frame_counter);
  }
  return -1;
}

// Function-boundary WRAM snapshot history (Phase B koopa-stomp).
// When a TCP client sets g_recomp_snap_on_func to a non-NULL name,
// every RecompStackPush whose name matches captures the LOW 8KB of
// WRAM ($0000-$1FFF — DP + game-state region used by SMW for all
// sprite/level/player state) into a ring buffer of 256 slots.
//
// Ring keeps the most recent 256 calls; older entries get overwritten.
// Each slot has: absolute call index (the count at capture time),
// frame number at capture, and the 8KB WRAM slice. Total: 256 × 8KB
// = 2 MB per side. Fits comfortably; 256 calls ≈ 4 seconds at 60Hz
// and ≈ 256 frames in SMW (one HandlePlayerPhysics call per frame).
//
// Probes use func_snap_get_n <call_idx> to fetch a specific historic
// snapshot and bisect for the first diverging call.
#define RECOMP_SNAP_SLICE_LEN  0x2000  /* $0000-$1FFF */
#define RECOMP_SNAP_RING_LEN   256

const char *g_recomp_snap_on_func = NULL;
int        g_recomp_snap_count    = 0;     /* total calls observed */
int        g_recomp_snap_frame    = -1;    /* most recent capture's frame */
typedef struct {
    int     call_idx;                       /* g_recomp_snap_count value at capture */
    int     frame;
    uint8_t wram_slice[RECOMP_SNAP_SLICE_LEN];
} recomp_snap_entry;
recomp_snap_entry *g_recomp_snap_ring = NULL;   /* lazily calloc'd on first capture */

/* Lookup an entry by absolute call index. Returns NULL if the index
 * is out of the ring's current window. */
const recomp_snap_entry* recomp_snap_lookup(int call_idx) {
    if (call_idx < 1) return NULL;
    if (!g_recomp_snap_ring) return NULL;
    int slot = (call_idx - 1) % RECOMP_SNAP_RING_LEN;
    if (g_recomp_snap_ring[slot].call_idx != call_idx) return NULL;
    return &g_recomp_snap_ring[slot];
}

void RecompStackPush(const char *name) {
  /* AR_CALLTRACE entry log: pairs with the exit log in RecompStackPop. Net
   * (exitS - entryS) per call = +2 for a balanced JSR fn, +3 for JSL; any
   * other value is a stack-imbalanced (leaking) generated function. */
  {
    static int ctp = -2;
    if (ctp == -2) { const char *e = getenv("AR_CALLTRACE"); ctp = e ? atoi(e) : -1; }
    if (ctp >= 0) {
      extern int snes_frame_counter; extern CpuState g_cpu; extern uint8 g_ram[];
      static long ctpgf = -2;
      if (ctpgf == -2) { const char *e = getenv("AR_CALLTRACE_GF"); ctpgf = e ? atol(e) : -1; }
      unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
      if (snes_frame_counter >= ctp && snes_frame_counter <= ctp + 4 &&
          (ctpgf < 0 || (long)gf == ctpgf))
        fprintf(stderr, "[ctPUSH S=%04x m=%u] %*s>%s\n", g_cpu.S, (unsigned)g_cpu.m_flag,
                g_recomp_stack_top * 2, "", name ? name : "?");
    }
  }
  // TEMP DIAGNOSTIC: AR_CALLTRACE=N logs EVERY function entry during frames
  // [N, N+2], with indentation by stack depth, for ground-truth control flow.
  {
    static int ct = -2;
    if (ct == -2) { const char *e = getenv("AR_CALLTRACE"); ct = e ? atoi(e) : -1; }
    if (ct >= 0) {
      extern int snes_frame_counter;
      extern CpuState g_cpu;
      /* AR_CALLTRACE_GF=N: restrict the trace to game-frame $0088==N (cuts the
       * action-frame noise to the one corrupting frame). S= shows the SNES stack
       * pointer at entry, to spot a handler that leaks (next sibling S lower). */
      static long ctgf = -2;
      if (ctgf == -2) { const char *e = getenv("AR_CALLTRACE_GF"); ctgf = e ? atol(e) : -1; }
      unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
      if (snes_frame_counter >= ct && snes_frame_counter <= ct + 4 &&
          (ctgf < 0 || (long)gf == ctgf))
        fprintf(stderr, "[ct S=%04x m=%u] %*s%s\n", g_cpu.S, (unsigned)g_cpu.m_flag,
                g_recomp_stack_top * 2, "", name);
    }
  }
  g_recomp_push_count++;   /* monotonic; AR_FRAMELOG measures per-frame work */
  if (g_recomp_stack_top < RECOMP_STACK_DEPTH)
    g_recomp_stack[g_recomp_stack_top++] = name;
  g_last_recomp_func = name;
  debug_server_profile_push(name);
  // Boundary auditor (always-on; no-op when SNESRECOMP_TRACE=0).
  // Recorded AFTER the stack push so stack_depth reflects post-push state.
  boundary_audit_record_entry(name);
  // Function-boundary snapshot: if a client set a target function
  // name, and this push matches it, capture WRAM. Frame execution
  // continues afterward — no longjmp. Compare the snapshot at
  // matching points across recomp + oracle for sub-frame-precise
  // state diff regardless of NMI ordering.
  if (g_recomp_snap_on_func) {
    extern int snes_frame_counter;
    int match;
    if (name == g_recomp_snap_on_func) match = 1;
    else if (strcmp(g_recomp_snap_on_func, name) == 0) {
      g_recomp_snap_on_func = name;  /* cache pointer for fast path */
      match = 1;
    } else {
      match = 0;
    }
    if (match) {
      if (!g_recomp_snap_ring) {
        g_recomp_snap_ring = calloc(RECOMP_SNAP_RING_LEN, sizeof(recomp_snap_entry));
        if (!g_recomp_snap_ring) return;   /* alloc failed: skip capture, do not crash */
      }
      g_recomp_snap_count++;
      g_recomp_snap_frame = snes_frame_counter;
      int slot = (g_recomp_snap_count - 1) % RECOMP_SNAP_RING_LEN;
      g_recomp_snap_ring[slot].call_idx = g_recomp_snap_count;
      g_recomp_snap_ring[slot].frame    = snes_frame_counter;
      memcpy(g_recomp_snap_ring[slot].wram_slice, g_ram, RECOMP_SNAP_SLICE_LEN);
    }
  }
}

void RecompStackDump(void) {
  fprintf(stderr, "Recomp call stack (%d deep):\n", g_recomp_stack_top);
  for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - RECOMP_STACK_DEPTH; i--)
    fprintf(stderr, "  [%d] %s\n", g_recomp_stack_top - 1 - i, g_recomp_stack[i]);
}

void RecompStackPop(void) {
  /* AR_CALLTRACE exit log: pairs with the entry log; net (exitS-entryS) reveals
   * the stack-leaking function (balanced JSR fn: +2, JSL fn: +3). */
  {
    static int ct = -2;
    if (ct == -2) { const char *e = getenv("AR_CALLTRACE"); ct = e ? atoi(e) : -1; }
    if (ct >= 0 && g_recomp_stack_top > 0) {
      extern int snes_frame_counter; extern CpuState g_cpu; extern uint8 g_ram[];
      static long ctgf = -2;
      if (ctgf == -2) { const char *e = getenv("AR_CALLTRACE_GF"); ctgf = e ? atol(e) : -1; }
      unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
      if (snes_frame_counter >= ct && snes_frame_counter <= ct + 4 &&
          (ctgf < 0 || (long)gf == ctgf))
        fprintf(stderr, "[ct S=%04x m=%u] %*s/%s\n", g_cpu.S, (unsigned)g_cpu.m_flag,
                g_recomp_stack_top * 2, "", g_recomp_stack[g_recomp_stack_top - 1]);
    }
  }
  // Record exit BEFORE the pop so stack_depth reflects pre-pop state and
  // the function name is still the topmost entry. Defensive against
  // empty stack: the auditor must NOT consume an entry_seq it didn't push.
  if (g_recomp_stack_top > 0) {
    boundary_audit_record_exit(g_recomp_stack[g_recomp_stack_top - 1]);
    g_recomp_stack_top--;
  }
  g_last_recomp_func = g_recomp_stack_top > 0 ? g_recomp_stack[g_recomp_stack_top - 1] : "(none)";
}

// Frame watchdog: detect infinite loops in generated code.
// Set before calling run_frame, checked by generated code periodically.
// Wall-clock (monotonic) time source. clock() measures process-wide CPU
// time across all threads on glibc/Linux, so a busy audio/present thread
// makes it outrun wall time and can trip the watchdog on a game that is
// not actually hung. Use a monotonic wall clock instead. SDL is game-layer
// only; this SDL-agnostic runtime TU uses the platform primitive directly.
#if defined(_WIN32)
#include <windows.h>
#endif
static uint64_t watchdog_monotonic_ns(void) {
#if defined(_WIN32)
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  /* Divide-before-scale: QueryPerformanceCounter counts since boot, so
   * c.QuadPart * 1e9 overflows uint64 after ~1.84e10 counts (~30 min at a
   * 10 MHz QPF, seconds at a TSC-frequency QPF). That made watchdog time
   * non-monotonic and could underflow the elapsed subtraction into a bogus
   * multi-billion-second value, falsely tripping the 5 s hang watchdog on a
   * game that is not hung. Split into whole seconds + fractional remainder so
   * no intermediate product overflows. */
  uint64_t cnt = (uint64_t)c.QuadPart;
  uint64_t freq = (uint64_t)f.QuadPart;
  if (freq == 0) return 0;
  return (cnt / freq) * 1000000000ULL +
         ((cnt % freq) * 1000000000ULL) / freq;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}
static uint64_t g_frame_start_ns;
static int g_watchdog_enabled;
static int g_watchdog_counter;
jmp_buf g_watchdog_jmp;
int g_watchdog_tripped;

void WatchdogFrameStart(void) {
  g_frame_start_ns = watchdog_monotonic_ns();
  g_watchdog_enabled = 1;
  g_watchdog_tripped = 0;
  g_watchdog_counter = 0;
  g_recomp_stack_top = 0;
  g_tailcall_context_valid = 0;
}

/* Monotonic loop-header count — an execution-volume proxy for AR_APUPROF
 * (straight-line loops push nothing, so push counts under-report them). */
uint64_t g_watchdog_loop_headers;

// Called at loop headers in generated code — detect infinite loops
void WatchdogCheck(void) {
  g_watchdog_loop_headers++;
  if (!g_watchdog_enabled) return;
  // Only check the clock every 10000 iterations to avoid overhead
  if (++g_watchdog_counter < 10000) return;
  g_watchdog_counter = 0;
  double elapsed = (double)(watchdog_monotonic_ns() - g_frame_start_ns) / 1e9;
  /* Boot has no watchdog. I_RESET runs once and uploads the SPC
   * engine + samples through the IPL handshake, which is real-time
   * paced by the audio thread (the SPC bootROM can only echo bytes
   * at ~1 MHz). For SMW the upload is ~12 KB and naturally takes
   * tens of seconds wall time; that's expected, not a hang. After
   * I_RESET returns the runtime falls into the normal per-frame
   * cadence (I_NMI + Internal) which completes comfortably under 5 s.
   *
   * Detecting "still in boot" via snes_frame_counter == 0 is robust:
   * the recompiled NMI handler increments snes_frame_counter, and
   * the very first NMI only fires after I_RESET returns and frame 1
   * starts. */
  if (snes_frame_counter == 0) return;
  if (elapsed > 5.0) {
    fprintf(stderr,
      "\n=== WATCHDOG: Frame %d exceeded %.1fs ===\n"
      "Game mode: %d | WatchdogCheck calls: %d\n"
      "Call stack (most recent first):\n",
      snes_frame_counter, elapsed, g_ram[0x100], g_watchdog_counter * 10000);
    for (int i = g_recomp_stack_top - 1; i >= 0; i--)
      fprintf(stderr, "  [%d] %s\n", g_recomp_stack_top - 1 - i, g_recomp_stack[i]);
    if (g_recomp_stack_top == 0)
      fprintf(stderr, "  (empty — last was %s)\n", g_last_recomp_func);
    fprintf(stderr, "\n");
    fflush(stderr);
    /* Capture the frozen state to saves/ for offline analysis. The watchdog
     * fires inside the coroutine, so g_cpu/g_ram/g_recomp_stack hold the exact
     * stuck state (the SDL event loop is wedged, so the F9/exit dumps can't
     * run during a hang). Weak so non-app linkers don't require it. */
    { extern void DumpDiagState(const char *) __attribute__((weak));
      if (DumpDiagState) DumpDiagState("watchdog"); }
    /* Also flush the AR_TRACE_WATCH ring: a hang is exactly the case the always-on
     * capture exists for — the ring holds the lead-up to the spin. */
    { extern void ar_trace_flush(const char *) __attribute__((weak));
      if (ar_trace_flush) ar_trace_flush("watchdog"); }
    g_watchdog_enabled = 0;
    g_watchdog_tripped = 1;
    { extern int snes_frame_counter;
      debug_server_profile_latch(snes_frame_counter); }
    longjmp(g_watchdog_jmp, 1);
  }
}

Snes *SnesInit(const uint8 *data, int data_size) {
  g_snes = snes_init(g_ram);
  g_snes_cpu = g_snes->cpu;
  g_dma = g_snes->dma;
  g_ppu = g_snes->ppu;

  if (data_size != 0) {
    bool loaded = snes_loadRom(g_snes, data, data_size);
    if (!loaded) {
      return NULL;
    }
    g_rom = g_snes->cart->rom;

    assert(g_rtl_game_info && "RtlRegisterGame must be called before SnesInit");

    if (g_rtl_game_info->initialize)
      g_rtl_game_info->initialize();
    snes_reset(g_snes, true); // reset after loading
    SnesEnterNativeMode();
  } else {
    g_snes->cart->ramSize = 2048;
    g_snes->cart->ram = calloc(1, 2048);
    assert(g_rtl_game_info && "RtlRegisterGame must be called before SnesInit");
    if (g_rtl_game_info->initialize)
      g_rtl_game_info->initialize();
    ppu_reset(g_snes->ppu);
    dma_reset(g_snes->dma);
  }

  g_sram = g_snes->cart->ram;
  g_sram_size = g_snes->cart->ramSize;
  return g_snes;
}
