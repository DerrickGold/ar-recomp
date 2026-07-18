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
#include <execinfo.h>

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

/* Entry m/x invariant check — see cpu_state.h ar_entry_mx_check.
 * Set from AR_MXCHECK env in the host setup (main.c). Default off. */
int g_ar_mx_check = 0;

/* ── Runtime m/x histogram + misdecode anomaly trap (AR_MXHIST) ─────────
 * Records, per function-entry PC, how often it runs with each (m,x). A
 * misdecode = a function running a variant it normally never runs (because
 * m/x leaked), so once a PC is well-established with one dominant (m,x), the
 * FIRST time it appears with a different (m,x) is flagged live. Unlike
 * AR_MXCHECK (which only catches direct-call variant mismatches), this
 * catches a wrongly-leaked RUNTIME flag — the actual misdecode trigger. */
int g_ar_mxhist = 0;
typedef struct { uint32_t pc; uint32_t cnt[4]; uint8_t flagged; } MxHistEnt;
#define MXHIST_CAP 32768u
static MxHistEnt g_mxhist[MXHIST_CAP];
#define MXHIST_WARMUP 64u   /* dominant combo must have ≥ this many hits first */

void ar_mxhist_record(uint32_t pc24, int m, int x) {
  unsigned combo = ((unsigned)(m & 1) << 1) | (unsigned)(x & 1);
  unsigned h = (pc24 * 2654435761u) & (MXHIST_CAP - 1u);
  MxHistEnt *e = 0;
  for (unsigned i = 0; i < MXHIST_CAP; i++) {
    unsigned j = (h + i) & (MXHIST_CAP - 1u);
    if (g_mxhist[j].pc == pc24) { e = &g_mxhist[j]; break; }
    if (g_mxhist[j].pc == 0u) { g_mxhist[j].pc = pc24; e = &g_mxhist[j]; break; }
  }
  if (!e) return;  /* table full — give up silently */
  if (!e->flagged && e->cnt[combo] == 0u) {
    unsigned dom = 0, domn = 0;
    for (int c = 0; c < 4; c++) if (e->cnt[c] > domn) { domn = e->cnt[c]; dom = (unsigned)c; }
    if (domn >= MXHIST_WARMUP && dom != combo) {
      e->flagged = 1;
      extern int snes_frame_counter;
      extern const char *g_recomp_stack[]; extern int g_recomp_stack_top;
      const char *caller = (g_recomp_stack_top >= 2)
                         ? g_recomp_stack[g_recomp_stack_top - 2] : "(top)";
      fprintf(stderr,
        "[mxhist] MISDECODE? %06X ran m=%d x=%d (1st time) after m=%u x=%u x%u  f=%d caller=%s\n",
        pc24, m & 1, x & 1, (dom >> 1) & 1, dom & 1, domn, snes_frame_counter,
        caller ? caller : "?");
      fflush(stderr);
    }
  }
  e->cnt[combo]++;
}

void ar_mxhist_dump(void) {
  if (!g_ar_mxhist) return;
  int n = 0;
  fprintf(stderr, "[mxhist] === PCs that ran with >1 (m,x) combo (possible misdecodes) ===\n");
  for (unsigned j = 0; j < MXHIST_CAP; j++) {
    if (!g_mxhist[j].pc) continue;
    int nz = 0; unsigned tot = 0, mn = 0xFFFFFFFFu, mx = 0;
    for (int c = 0; c < 4; c++) { unsigned v = g_mxhist[j].cnt[c]; if (v) { nz++; tot += v; if (v < mn) mn = v; if (v > mx) mx = v; } }
    if (nz < 2) continue;
    /* lopsided split (minority < 2% of majority) = strong misdecode signal */
    int lop = (mx >= 50u && mn * 50u < mx);
    fprintf(stderr, "[mxhist] %06X  M0X0=%u M0X1=%u M1X0=%u M1X1=%u%s\n",
      g_mxhist[j].pc, g_mxhist[j].cnt[0], g_mxhist[j].cnt[1],
      g_mxhist[j].cnt[2], g_mxhist[j].cnt[3], lop ? "   <== LOPSIDED" : "");
    n++;
  }
  fprintf(stderr, "[mxhist] %d multi-combo PCs\n", n);
  fflush(stderr);
  /* AR_FNCENSUS=1: dump EVERY recorded function-entry PC (not just the
   * multi-combo ones) with per-(m,x) counts to <run-dir>/fn_census.txt. The
   * decisive tool for never-runs bugs: a routine that exists in the binary
   * but is missing from the census was never entered at all -- its trigger
   * upstream never fired (no tripwire can catch code that doesn't run). */
  if (getenv("AR_FNCENSUS")) {
    /* AR_RUN_DIR = per-run artifact dir exported by the game's run_dir.c. */
    const char *rd = getenv("AR_RUN_DIR");
    char census_path[300];
    snprintf(census_path, sizeof census_path, "%s/fn_census.txt",
             rd && rd[0] ? rd : "saves");
    FILE *f = fopen(census_path, "w");
    if (f) {
      unsigned total = 0;
      for (unsigned j = 0; j < MXHIST_CAP; j++) {
        if (!g_mxhist[j].pc) continue;
        fprintf(f, "%06X %u %u %u %u\n", g_mxhist[j].pc,
                g_mxhist[j].cnt[0], g_mxhist[j].cnt[1],
                g_mxhist[j].cnt[2], g_mxhist[j].cnt[3]);
        total++;
      }
      fclose(f);
      fprintf(stderr, "[fncensus] wrote %s (%u PCs)\n", census_path, total);
    }
  }
}

/* AR_ADADTRACE=1 (2026-07-01, temporary probe): every entry to bank_01_ADAD*
 * (sim-mode decoration-object OAM-record writer) -- prints X (the per-object
 * index into the DB-relative source table) and DB, plus the raw source words
 * at DB:$000a+X / $000c+X / $0008+X it's about to copy into the WRAM record.
 * Chasing the ~46 identical duplicate decoration sprites (x=77,y=44,tile=0x55)
 * stuck in WRAM $03E8-$047F -- unclear if X fails to advance between calls
 * (loop-index bug) or the SOURCE table itself is uniformly blank (all-same
 * legitimately, meaning the bug is upstream of this function). */
void ar_adad_trace(CpuState *cpu, const char *fn, uint32_t pc24) {
  if (!getenv("AR_ADADTRACE") || !fn || !strstr(fn, "bank_01_ADAD")) return;
  extern int snes_frame_counter;
  static int n;
  if (n++ >= 60000) return;
  uint16 v0a = cpu_read16(cpu, cpu->DB, (uint16)(0x000a + cpu->X));
  uint16 v0c = cpu_read16(cpu, cpu->DB, (uint16)(0x000c + cpu->X));
  uint16 v08 = cpu_read16(cpu, cpu->DB, (uint16)(0x0008 + cpu->X));
  fprintf(stderr, "[adadtrace] %s (%06X) f=%d X=$%04X DB=$%02X D=$%04X "
          "src[+0a]=$%04X src[+0c]=$%04X src[+08]=$%04X\n",
          fn, pc24, snes_frame_counter, cpu->X, cpu->DB, cpu->D, v0a, v0c, v08);
}

/* AR_TRAPFN=<substring>: the first time a function whose name contains the
 * substring is entered, dump the full recomp call stack (top -> bottom) plus
 * the runtime m/x flags. Used to find the dispatch chain that reached a
 * known-garbage misdecode variant (e.g. AR_TRAPFN=bank_03_AC8E_M1X0) -> the
 * caller between the legit entry and the wrong-m variant is the leak site. */
/* Always-on indirect-dispatch OOB tripwire (2026-07-02). Generated code
 * calls cpu_trace_dispatch_oob when a runtime index exceeds the cfg
 * `indirect_dispatch` count; in non-trace builds that used to compile to
 * a SILENT no-op — which hid the sim-mode actor-spawn root cause (B8C0's
 * idx:X switch computed _idx from a PLX-restored record pointer, so the
 * OOB arm ran on every typed record every frame for weeks with zero
 * output). Deduped per (site, idx), capped, one loud line each — same
 * philosophy as the [dispatch-miss] tripwire. AR_NOOOBWARN=1 silences. */
RecompReturn ar_dispatch_oob_warn(CpuState *cpu, uint32_t site_pc24, uint16_t idx) {
  static int off = -1;
  if (off < 0) off = getenv("AR_NOOOBWARN") ? 1 : 0;
  if (off) return RECOMP_RETURN_NORMAL;
  static struct { uint32_t site; uint16_t idx; } seen[64];
  static int nseen, capped;
  for (int i = 0; i < nseen; i++)
    if (seen[i].site == site_pc24 && seen[i].idx == idx) return RECOMP_RETURN_NORMAL;
  if (nseen >= 64) {
    if (!capped) { capped = 1; fprintf(stderr, "[dispatch-oob] (further sites suppressed, table full)\n"); }
    return RECOMP_RETURN_NORMAL;
  }
  seen[nseen].site = site_pc24; seen[nseen].idx = idx; nseen++;
  extern int snes_frame_counter;
  fprintf(stderr,
      "[dispatch-oob] site=$%06X idx=%u exceeds cfg count -- dispatch SKIPPED "
      "(m=%u x=%u X=$%04X A=$%04X S=$%04X f=%d func=%s). Always a real bug: "
      "cfg count too small, wrong idx model (idx:X after a PLX? use idx:A), "
      "or register corruption.\n",
      site_pc24, (unsigned)idx, cpu->m_flag & 1, cpu->x_flag & 1,
      cpu->X, cpu->A, cpu->S, snes_frame_counter, g_last_recomp_func);
  return RECOMP_RETURN_NORMAL;
}

const char *g_ar_trapfn = 0;
void ar_entry_trapfn(CpuState *cpu, const char *fn, uint32_t pc24) {
  if (!g_ar_trapfn || !fn || !strstr(fn, g_ar_trapfn)) return;
  static int done;
  if (done) return;
  done = 1;
  extern int snes_frame_counter;
  extern const char *g_recomp_stack[]; extern int g_recomp_stack_top;
  fprintf(stderr, "[trapfn] ENTER %s (%06X) m=%u x=%u frame=%d  call stack (top->bottom):\n",
          fn, pc24, cpu->m_flag & 1, cpu->x_flag & 1, snes_frame_counter);
  for (int i = g_recomp_stack_top - 1; i >= 0; i--)
    fprintf(stderr, "    [%2d] %s\n", i, g_recomp_stack[i] ? g_recomp_stack[i] : "?");
  /* Block-history ring (pc, m, x, S at each block): the trap fires BEFORE the
   * misdecode loop floods the ring, so this shows the path into the entry --
   * pinpoints the block where m OR x flipped, and (via S) whether a call
   * along the way drifted the stack. 2026-06-30: widened from m-only/40-deep
   * to m+x+S/200-deep (matching the AR_XTRACE dump's format) -- the earlier
   * m-only version couldn't show an X-flag corruption at all. */
  extern uint32_t g_ar_blk_ring[]; extern uint32_t g_ar_blk_aux[];
  extern uint16_t g_ar_blk_s[]; extern unsigned g_ar_blk_idx;
  fprintf(stderr, "  block path (pc, m, x, S):\n");
  for (int k = 200; k >= 1; k--) {
    unsigned idx = (g_ar_blk_idx - (unsigned)k) & 1023u;
    fprintf(stderr, "    [-%3d] pc=$%06X m=%u x=%u S=$%04X\n", k,
            g_ar_blk_ring[idx], (g_ar_blk_aux[idx] >> 16) & 1,
            (g_ar_blk_aux[idx] >> 17) & 1, g_ar_blk_s[idx]);
  }
  fflush(stderr);
}

/* Garbage-variant dispatch trap (default ON). The emitter marks a function
 * variant as GARBAGE when its decode contains a split-immediate BRK — a BRK
 * that arose from decoding a 16-bit immediate at the wrong (narrow) width, which
 * a valid sibling variant decodes as one instruction (e.g. bank_03_97B0_M1X0's
 * `LDA #$0007` -> `LDA #$07` + `BRK`). Such a variant is NEVER legitimately
 * reached: entering it means a leaked m/x flag dispatched us into a misdecode.
 * The emitter calls this at the variant's entry, so it fires at the EXACT
 * dispatch where the leak first sends control wrong — far closer to the root
 * than the eventual downstream crash, and with no oracle needed. Logs the caller
 * + runtime m/x + frame; dedup per (caller,fn), capped. AR_GARBAGE_STACK=1 adds
 * the recomp call stack + block ring; AR_GARBAGE_ABORT=1 aborts on first hit;
 * AR_NOGARBAGEWARN=1 silences. See DEBUG.md "garbage-variant trap". */
void ar_garbage_variant_trap(CpuState *cpu, const char *fn, uint32_t pc24) {
  /* Unified AR_TRACE garbage channel — every misdecode-variant entry in-window. */
  { extern int ar_trace_active(void);
    extern void ar_trace_garbage(uint32_t, const char *, int, int);
    if (ar_trace_active()) ar_trace_garbage(pc24, fn, cpu->m_flag & 1, cpu->x_flag & 1); }
  static int en = -1, full = -1, doabort = -1;
  if (en < 0) en = getenv("AR_NOGARBAGEWARN") ? 0 : 1;
  if (!en) return;
  if (full < 0) full = getenv("AR_GARBAGE_STACK") ? 1 : 0;
  if (doabort < 0) doabort = getenv("AR_GARBAGE_ABORT") ? 1 : 0;
  extern int snes_frame_counter;
  extern const char *g_recomp_stack[]; extern int g_recomp_stack_top;
  const char *caller = (g_recomp_stack_top >= 2)
                     ? g_recomp_stack[g_recomp_stack_top - 2] : "(top/dispatch)";
  /* dedup per (caller, fn) string-literal pair, capped */
  static const void *seen_c[256]; static const void *seen_f[256];
  static int nseen, capped;
  for (int i = 0; i < nseen; i++)
    if (seen_c[i] == caller && seen_f[i] == fn) return;
  if (nseen < 256) { seen_c[nseen] = caller; seen_f[nseen] = fn; nseen++; }
  else if (!capped) { capped = 1;
    fprintf(stderr, "[garbage-variant] (cap 256 reached; further unique sites "
                    "suppressed)\n"); fflush(stderr); return; }
  else return;
  fprintf(stderr,
    "[garbage-variant] entered MISDECODE variant %s (%06X) m=%u x=%u f=%d — a "
    "leaked flag dispatched here from caller=%s. (This variant's decode is "
    "garbage; the flag went wrong AT or just before this dispatch.)\n",
    fn, pc24, cpu->m_flag & 1, cpu->x_flag & 1, snes_frame_counter, caller);
  if (full) {
    for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - 12; i--)
      fprintf(stderr, "[garbage-variant]   [%2d] %s\n", i,
              g_recomp_stack[i] ? g_recomp_stack[i] : "?");
    extern uint32_t g_ar_blk_ring[]; extern uint32_t g_ar_blk_aux[]; extern unsigned g_ar_blk_idx;
    extern uint16_t g_ar_blk_s[];
    /* AR_GARBAGE_HIST=<n> (default 24, max 1000): how far back to dump. The
     * 24-block default couldn't reach the m-flip origin in the sim-dev leak
     * (flip was >24 blocks upstream of the garbage dispatch). Includes S so
     * an unbalanced call shows as an S jump at the flip point. */
    static int histn = -1;
    if (histn < 0) { const char *h = getenv("AR_GARBAGE_HIST");
      histn = h ? (int)strtoul(h, NULL, 0) : 24;
      if (histn > 1000) histn = 1000; if (histn < 1) histn = 24; }
    for (int k = histn; k >= 1; k--) {
      unsigned idx = (g_ar_blk_idx - (unsigned)k) & 1023u;
      fprintf(stderr, "[garbage-variant]   [-%3d] pc=$%06X m=%u x=%u S=%04X\n", k,
              g_ar_blk_ring[idx], (g_ar_blk_aux[idx] >> 16) & 1,
              (g_ar_blk_aux[idx] >> 17) & 1, g_ar_blk_s[idx]);
    }
  }
  /* AR_XTRACE: dump the x-flip history leading INTO this garbage dispatch. Fire
   * on the FIRST garbage variant entered with x=1 (the x-leak ones, e.g. AFF8/
   * A087 at M1X1) — the first garbage ($8029) is at x=0, the x 0->1 leak happens
   * just after it, so capturing at the first x=1 garbage shows that transition.
   * The last x 0->1 with no closing 1->0 is the leak that picked the wrong-x
   * variant. Fires once, at the real fault — no frame guessing. */
  {
    extern int ar_xtrace_enabled(void);
    extern void ar_xtrace_dump(const char *why, int n);
    static int dumped;
    if (ar_xtrace_enabled() && !dumped && (cpu->x_flag & 1)) {
      dumped = 1; ar_xtrace_dump(fn, 48);
      /* Also dump the block ring WITH per-block S, back far enough to span the
       * enclosing routine's (B21F) call chain — watch S drift across a call to
       * find the subroutine that returns with S off (corrupting B21F's PLP ->
       * the x-leak). The flip site is a PLP; the cause is an unbalanced call. */
      extern uint32_t g_ar_blk_ring[]; extern uint32_t g_ar_blk_aux[];
      extern uint16_t g_ar_blk_s[]; extern unsigned g_ar_blk_idx;
      fprintf(stderr, "[xtrace] === block+S history into %s (oldest-first; watch "
              "S jump across a call; the B21F blocks $03B2xx mark each call's "
              "return) ===\n", fn);
      for (int k = 200; k >= 1; k--) {
        unsigned idx = (g_ar_blk_idx - (unsigned)k) & 1023u;
        fprintf(stderr, "[xtrace]   %06X m=%u x=%u S=%04X\n",
                g_ar_blk_ring[idx], (g_ar_blk_aux[idx] >> 16) & 1,
                (g_ar_blk_aux[idx] >> 17) & 1, g_ar_blk_s[idx]);
      }
    }
  }
  fflush(stderr);
  if (doabort) { extern void abort(void); abort(); }
}

void ar_entry_mx_fail(CpuState *cpu, int em, int ex, const char *fn, uint32_t pc24) {
  /* Rate-limit: report each distinct function at most once, cap total, so a
   * hot mis-typed call can't flood. The first occurrence per site is what
   * matters (the emitter's static m/x analysis was wrong entering `fn`). */
  static const char *seen[256];
  static unsigned nseen = 0;
  static unsigned total = 0;
  for (unsigned i = 0; i < nseen; i++)
    if (seen[i] == fn) return;            /* same string literal -> already reported */
  if (nseen < 256) seen[nseen++] = fn;
  if (total++ >= 2000) return;
  /* Caller = one below the just-pushed current function on the recomp stack. */
  extern const char *g_recomp_stack[];
  extern int g_recomp_stack_top;
  const char *caller = (g_recomp_stack_top >= 2)
                     ? g_recomp_stack[g_recomp_stack_top - 2] : "(top/dispatch)";
  fprintf(stderr,
    "[mxcheck] %s (%06X) entered with m=%u x=%u but variant expects m=%d x=%d"
    "  (caller=%s)\n",
    fn, pc24, cpu->m_flag & 1, cpu->x_flag & 1, em, ex,
    caller ? caller : "?");
  /* 2026-06-30: AR_MXCHECK_BT dumps the REAL host C call stack (backtrace(),
   * same mechanism the ppu_read crash handler uses) for a specific function
   * name substring -- bypasses g_recomp_stack entirely, so it's independent
   * of any bug/assumption in our OWN stack-bookkeeping instrumentation. Added
   * chasing $01:B898_M1X1: every g_recomp_stack-based diagnostic (AR_CALLMX,
   * AR_TRAPFN, [b898log] in _cpu_dispatch_once) proved the caller ISN'T
   * 933C_M1X0's own switch (that call site is provably clean) and ISN'T a
   * computed/miss dispatch (b898log never fires) -- yet g_recomp_stack shows
   * exactly [933C_M1X0, B898_M1X1]. This settles it directly: the true
   * compiled call chain, independent of any of that. */
  {
    static int done;
    const char *want = getenv("AR_MXCHECK_BT");
    if (want && !done && strstr(fn, want)) {
      done = 1;
      void *bt[32];
      int n = backtrace(bt, 32);
      fprintf(stderr, "[mxcheck-bt] real C call stack for %s:\n", fn);
      backtrace_symbols_fd(bt, n, 2);
    }
  }
  fflush(stderr);
}

/* Exit-mx invariant check (AR_EXITMX) — symmetric twin of ar_entry_mx_check.
 * See cpu_state.h. Fires when a function's actual runtime exit (m,x) differs
 * from the exit (m,x) the emitter told its callers; that mismatch is exactly
 * what poisons every caller's post-call decode (the $03:9156 act->sim class). */
int g_ar_exit_mx_check = 0;
void ar_exit_mx_fail(CpuState *cpu, int exm, int exx, const char *fn, uint32_t pc24) {
  static const char *seen[256];
  static unsigned nseen = 0, total = 0;
  const char *f = fn ? fn : g_last_recomp_func;
  for (unsigned i = 0; i < nseen; i++)
    if (seen[i] == f) return;             /* one report per function */
  if (nseen < 256) seen[nseen++] = f;
  if (total++ >= 2000) return;
  extern int snes_frame_counter;
  fprintf(stderr,
    "[exit-mx] %s (%06X) EXITS m=%u x=%u but callers were told m=%d x=%d"
    "  f=%d -> caller post-call decode poisoned\n",
    f ? f : "?", pc24, cpu->m_flag & 1, cpu->x_flag & 1, exm, exx,
    snes_frame_counter);
  fflush(stderr);
}

/* Exit stack-balance check (AR_EXITS) — see cpu_state.h. A paired frame whose
 * RTS/RTL is reached with cpu->S drifted from _entry_s (and no ancestor parked
 * there) pops a garbage return (the $01:B8CF PLB;PLP;RTL-on-drifted-stack
 * class). Names the drifting function at its own return, before the garbage
 * dispatch cascades. */
int g_ar_exit_s_check = 0;
void ar_exit_s_fail(CpuState *cpu, uint32_t entry_s, uint32_t ret_s,
                    const char *fn, uint32_t pc24) {
  static const char *seen[256];
  static unsigned nseen = 0, total = 0;
  const char *f = fn ? fn : g_last_recomp_func;
  for (unsigned i = 0; i < nseen; i++)
    if (seen[i] == f) return;
  if (nseen < 256) seen[nseen++] = f;
  if (total++ >= 2000) return;
  extern int snes_frame_counter;
  fprintf(stderr,
    "[exit-s] %s (%06X) RTS/RTL stack DRIFT: entry_s=$%04X ret_s=$%04X"
    " (delta %+d) f=%d -> pops a garbage return\n",
    f ? f : "?", pc24, entry_s & 0xFFFF, ret_s & 0xFFFF,
    (int)((int32_t)ret_s - (int32_t)entry_s), snes_frame_counter);
  fflush(stderr);
}

/* Call-site invariant check (AR_CALLMX) — see cpu_state.h. Dedup by SITE
 * (pc24), not function name: a function can have many call sites and each is
 * an independent point where corruption could first become visible. */
int g_ar_call_mx_check = 0;
void ar_call_mx_fail(CpuState *cpu, int em, int ex, const char *fn, uint32_t pc24) {
  static uint32_t seen[512];
  static unsigned nseen = 0, total = 0;
  for (unsigned i = 0; i < nseen; i++)
    if (seen[i] == pc24) return;
  if (nseen < 512) seen[nseen++] = pc24;
  if (total++ >= 2000) return;
  extern int snes_frame_counter;
  fprintf(stderr,
    "[call-mx] %s call-site $%06X: runtime m=%u x=%u but decoder assumed "
    "m=%d x=%d here -> (m,x) corrupted between fn entry and this call"
    "  f=%d\n",
    fn ? fn : "?", pc24, cpu->m_flag & 1, cpu->x_flag & 1, em, ex,
    snes_frame_counter);
  /* Block-history ring (pc m x S), oldest-first: shows the path INTO the
   * failing call site, i.e. the block where the runtime flag diverged from
   * the decoder's assumption. Same format as the trapfn/watchdog dumps. */
  {
    extern uint32_t g_ar_blk_ring[], g_ar_blk_aux[];
    extern uint16_t g_ar_blk_s[];
    extern unsigned g_ar_blk_idx;
    fprintf(stderr, "[call-mx] last 48 blocks (pc m x S X), oldest-first:\n");
    for (int i = 48; i >= 1; i--) {
      unsigned j = (g_ar_blk_idx - (unsigned)i) & 1023u;
      uint32_t aux = g_ar_blk_aux[j];
      fprintf(stderr, "    %06X m=%u x=%u S=%04X X=%04X\n",
              g_ar_blk_ring[j], (aux >> 16) & 1, (aux >> 17) & 1,
              g_ar_blk_s[j], aux & 0xFFFF);
    }
  }
  fflush(stderr);
}

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

/* AR_XTRACE: dedicated x-flag-transition ring. Records ONLY the blocks where x
 * flips (so 512 entries span a huge slice of execution, unlike the per-block
 * ring), then dumps automatically the instant a garbage variant is entered
 * (ar_garbage_variant_trap) — capturing the REAL fatal path's x history with no
 * frame-number guessing. The block recorded is where the flip happened (a
 * SEP/REP #$10/#$30, or a PLP/RTI restoring x from the stack); reading the gen
 * for that block tells us SEP-leak (skipped REP) vs PLP-leak (bad stack byte). */
#define AR_XTR_RING 512
static uint32_t g_xtr_blk[AR_XTR_RING];   /* block PC where the flip occurred */
static uint32_t g_xtr_nxt[AR_XTR_RING];   /* the following block PC */
static uint32_t g_xtr_gf[AR_XTR_RING];    /* game-frame $0088 */
static uint8_t  g_xtr_dir[AR_XTR_RING];   /* new x value (0 or 1) */
static uint8_t  g_xtr_m[AR_XTR_RING];     /* m at the flip block */
static unsigned g_xtr_idx;

int ar_xtrace_enabled(void) {
  static int e = -1;
  if (e < 0) e = getenv("AR_XTRACE") ? 1 : 0;
  return e;
}

void ar_xtrace_record(uint32_t blk, uint32_t nxt, int new_x, int m, uint32_t gf) {
  unsigned i = g_xtr_idx++ & (AR_XTR_RING - 1);
  g_xtr_blk[i] = blk; g_xtr_nxt[i] = nxt; g_xtr_gf[i] = gf;
  g_xtr_dir[i] = (uint8_t)new_x; g_xtr_m[i] = (uint8_t)m;
}

/* Dump the last `n` x-transitions (newest last). Called from the garbage trap. */
void ar_xtrace_dump(const char *why, int n) {
  if (g_xtr_idx == 0) { fprintf(stderr, "[xtrace] (%s) no x flips recorded\n", why);
    return; }
  if (n > AR_XTR_RING) n = AR_XTR_RING;
  if ((unsigned)n > g_xtr_idx) n = (int)g_xtr_idx;
  fprintf(stderr, "[xtrace] === x-flip history into %s (newest last; the last "
          "0->1 with no closing 1->0 is the leak) ===\n", why);
  for (int k = n; k >= 1; k--) {
    unsigned i = (g_xtr_idx - (unsigned)k) & (AR_XTR_RING - 1);
    fprintf(stderr, "[xtrace]   gf=%u  x ->%d  IN block $%06X (m=%u) -> $%06X\n",
            g_xtr_gf[i], g_xtr_dir[i], g_xtr_blk[i], g_xtr_m[i], g_xtr_nxt[i]);
  }
  fflush(stderr);
}

/* AR_STACKPROV: stack pusher-provenance shadow array. For each bank-0 stack
 * byte address, record the recomp block-PC that last PUSHED there. Lets a
 * bad-RTS / dispatch-miss name the function whose push left the corrupt return
 * frame — or, if a slot reads back NEVER-PUSHED, reveal that S is pointing at
 * stale memory it never wrote (wrong-S relocation, not a bad push). The two
 * cases need opposite fixes, so distinguishing them is the whole point.
 * Runner-only, gated, ~256KB. Written from cpu_write8's push heuristic
 * (bank==0 && addr==cpu->S, before the S decrement); read at the dispatch-miss. */
uint32_t g_stack_pusher[0x10000];
unsigned g_stack_pusher_frame[0x10000];

/* AR_STRACE: instruction-granular cpu->S trace scoped to a PC window (default the
 * B20C/B21F loop $03B200..$03B260). Logs every stack push/pop (from cpu_write8/
 * read8) + every block entry, with cpu->S, so the EXACT micro-op where S drifts
 * +1 across a loop iteration is visible (block-level tracing can't see it).
 * Capped. Window overridable via AR_STRACE_LO/HI (hex). */
static int      g_strace_en = -1;
static uint32_t g_strace_lo, g_strace_hi;
static int      g_strace_n;
static int ar_strace_setup(void) {
  if (g_strace_en < 0) {
    g_strace_en = getenv("AR_STRACE") ? 1 : 0;
    const char *l = getenv("AR_STRACE_LO"); g_strace_lo = l ? (uint32_t)strtoul(l, 0, 16) : 0x03B200u;
    const char *h = getenv("AR_STRACE_HI"); g_strace_hi = h ? (uint32_t)strtoul(h, 0, 16) : 0x03B260u;
  }
  return g_strace_en;
}
/* True when the currently-executing block (newest ring entry) is in the window. */
int ar_strace_active(void) {
  if (!ar_strace_setup() || g_strace_n >= 400 || g_ar_blk_idx == 0) return 0;
  uint32_t pc = g_ar_blk_ring[(g_ar_blk_idx - 1u) & (AR_BLK_RING - 1)];
  return pc >= g_strace_lo && pc <= g_strace_hi;
}
void ar_strace_op(const char *kind, uint16_t addr, uint8_t val, uint16_t s) {
  uint32_t pc = g_ar_blk_ring[(g_ar_blk_idx - 1u) & (AR_BLK_RING - 1)];
  fprintf(stderr, "[strace]   %-4s $%04X = $%02X   S=%04X  (in $%06X)\n",
          kind, addr, val, s, pc);
  if (++g_strace_n == 400) fprintf(stderr, "[strace] (cap 400)\n");
  fflush(stderr);
}
/* Block-entry marker (called from cpu_trace_block when in window). */
void ar_strace_block(uint32_t pc24, uint16_t s, int m, int x) {
  if (!ar_strace_setup() || g_strace_n >= 400) return;
  if (pc24 < g_strace_lo || pc24 > g_strace_hi) return;
  fprintf(stderr, "[strace] BLOCK $%06X  S=%04X m=%u x=%u\n", pc24, s, m, x);
  if (++g_strace_n == 400) fprintf(stderr, "[strace] (cap 400)\n");
  fflush(stderr);
}

int ar_stackprov_enabled(void) {
  static int e = -1;
  if (e < 0) e = getenv("AR_STACKPROV") ? 1 : 0;
  return e;
}

int ar_block_history2(uint32_t *pc, uint32_t *aux, int max) {
  if (max > AR_BLK_RING) max = AR_BLK_RING;
  unsigned total = g_ar_blk_idx;
  int n = (total < (unsigned)max) ? (int)total : max;
  for (int i = 0; i < n; i++) {
    unsigned k = (g_ar_blk_idx - n + i) & (AR_BLK_RING - 1);
    pc[i] = g_ar_blk_ring[k];
    aux[i] = g_ar_blk_aux[k];
  }
  return n;
}

/* Same as ar_block_history2 but also returns cpu->S per block — lets the diag
 * dump show stack drift across a call chain (find the subroutine that returns
 * with S off, corrupting a later PLP). */
int ar_block_history3(uint32_t *pc, uint32_t *aux, uint16_t *s, int max) {
  if (max > AR_BLK_RING) max = AR_BLK_RING;
  unsigned total = g_ar_blk_idx;
  int n = (total < (unsigned)max) ? (int)total : max;
  for (int i = 0; i < n; i++) {
    unsigned k = (g_ar_blk_idx - n + i) & (AR_BLK_RING - 1);
    pc[i] = g_ar_blk_ring[k];
    aux[i] = g_ar_blk_aux[k];
    s[i] = g_ar_blk_s[k];
  }
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
int g_trace_one_frame = 0;  /* AR_CTACTION: trace exactly one inter-yield batch */
int g_ct_arm_request = 0;   /* set when 8465_M0X0 seen; WaitForVblank arms next batch */

/* AR_CTACTION batch buffer: buffer each inter-yield batch's call trace and flush
 * only the batch that contains 8465_M0X0 — captures the exact m=1->0 corrupting
 * frame from its start. Reset/flushed by RecompBatchYield() at each vblank. */
static char g_batch_buf[256 * 1024];
static int  g_batch_len;
static int  g_batch_had_8465;
static int  g_batch_done;   /* once we've flushed the corrupting batch, stop */

void RecompBatchYield(void) {
  if (!getenv("AR_CTACTION") || g_batch_done) return;
  if (g_batch_had_8465) {
    fwrite(g_batch_buf, 1, (size_t)g_batch_len, stderr);
    fprintf(stderr, "[1f] flushed corrupting batch (%d bytes)\n", g_batch_len);
    g_batch_done = 1;
  }
  g_batch_len = 0;
  g_batch_had_8465 = 0;
}

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
recomp_snap_entry g_recomp_snap_ring[RECOMP_SNAP_RING_LEN];

/* Lookup an entry by absolute call index. Returns NULL if the index
 * is out of the ring's current window. */
const recomp_snap_entry* recomp_snap_lookup(int call_idx) {
    if (call_idx < 1) return NULL;
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
  // TEMP DIAGNOSTIC: tripwires for the action-level fade-freeze investigation.
  //   AR_TRAP8465 — fire when the misdecoded bank_00_8465_M0X0 (BRK stub that
  //                 never pops its return frame) is entered: dump frame, cpu->S,
  //                 m/x, and the full recomp call stack (innermost first). This
  //                 should NEVER run in a correct game; it's the smoking gun.
  //   Also counts entries into bank_00_8966 (the new computed-jump continuation
  //                 dispatch entry) to confirm the fix path is exercised.
  if ((getenv("AR_TRAP8465") || getenv("AR_CTACTION")) && name
      && strcmp(name, "bank_00_8465_M0X0") == 0) {
    extern int snes_frame_counter;
    g_ct_arm_request = 1;   /* ask WaitForVblank to trace the NEXT full batch */
    static int once;
    if (!once) {
      once = 1;
      fprintf(stderr, "[trap] *** bank_00_8465_M0X0 (BRK misdecode) entered f=%d top=%d ***\n",
              snes_frame_counter, g_recomp_stack_top);
      for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - 12; i--)
        fprintf(stderr, "[trap]   [%d] %s\n", i, g_recomp_stack[i] ? g_recomp_stack[i] : "?");
    }
  }
  // TEMP DIAGNOSTIC: AR_92CBLOG — when the level object-stream rebuild $92CB
  // runs, log the section key it loads: cpu->D, cpu->DB, and the 16-bit
  // [$7E:D+$18] compared against the $B100 index table. Lets us see whether the
  // recomp loads a different section than the oracle (28 vs 49 objects).
  if (getenv("AR_92CBLOG") && name && strncmp(name, "bank_00_92CB", 12) == 0) {
    extern int snes_frame_counter;
    extern CpuState g_cpu;
    extern uint8 g_ram[];
    uint32 a = (uint32)((g_cpu.D + 0x18) & 0x1ffff);
    unsigned key = (unsigned)g_ram[a] | ((unsigned)g_ram[(a + 1) & 0x1ffff] << 8);
    fprintf(stderr, "[92cb] f=%d D=%04x DB=%02x [$7E:D+18]=%04x  ($7E0018=%04x)\n",
            snes_frame_counter, g_cpu.D, g_cpu.DB, key,
            (unsigned)g_ram[0x18] | ((unsigned)g_ram[0x19] << 8));
  }
  // TEMP DIAGNOSTIC: AR_SPAWNLOG — log X (grid slot) at the object-spawn /
  // slot-init routines, to see which slot each spawn targets and whether the
  // free-slot pointer advances. The terminator dies because spawns keep hitting
  // the same slot ($11E0) without advancing it.
  if (getenv("AR_SPAWNLOG") && name) {
    extern int snes_frame_counter;
    extern CpuState g_cpu;
    extern uint8 g_ram[];
    const char *m = NULL;
    if (strncmp(name, "bank_00_A758", 12) == 0) m = "A758";
    else if (strncmp(name, "bank_00_85B7", 12) == 0) m = "85B7(init $4000)";
    else if (strncmp(name, "bank_02_B127", 12) == 0) m = "B127";
    /* AR_MLOG: log entry m-flag of the 82E2 sub-call chain to localize the
     * m=0 leak that makes B127 misdecode (needs m=1). */
    if (getenv("AR_MLOG") && (strncmp(name, "bank_00_845F", 12) == 0 ||
        strncmp(name, "bank_00_8915", 12) == 0 || strncmp(name, "bank_02_B030", 12) == 0 ||
        strncmp(name, "bank_02_B091", 12) == 0 || strncmp(name, "bank_02_B127", 12) == 0)) {
      extern int snes_frame_counter;
      unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
      if (gf >= 1900 && gf <= 1918)
        fprintf(stderr, "[mlog] gf=%u %-20s entry m=%u x=%u S=%04x\n", gf, name,
                (unsigned)g_cpu.m_flag, (unsigned)g_cpu.x_flag, g_cpu.S);
    }
    if (m && strcmp(m, "B127") == 0)
      fprintf(stderr, "[b127] entry name=%s m=%u x=%u caller=%s caller2=%s\n",
              name, (unsigned)g_cpu.m_flag, (unsigned)g_cpu.x_flag,
              g_recomp_stack_top >= 1 && g_recomp_stack[g_recomp_stack_top-1] ? g_recomp_stack[g_recomp_stack_top-1] : "?",
              g_recomp_stack_top >= 2 && g_recomp_stack[g_recomp_stack_top-2] ? g_recomp_stack[g_recomp_stack_top-2] : "?");
    if (m) fprintf(stderr, "[spawn] gf=%u f=%d %-16s X=%04x A=%04x Y=%04x  $008a=%04x $00cc=%04x\n",
                   (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8),
                   snes_frame_counter, m, g_cpu.X, g_cpu.A, g_cpu.Y,
                   (unsigned)g_ram[0x8a] | ((unsigned)g_ram[0x8b] << 8),
                   (unsigned)g_ram[0xcc] | ((unsigned)g_ram[0xcd] << 8));
  }
  // TEMP DIAGNOSTIC: log the first frame each function is entered, to see the
  // recomp's execution progression (AR_FUNCLOG=1). AR_FUNCLOG=<substr> filters.
  if (getenv("AR_FUNCLOG")) {
    static const void *seen[8192]; static int nseen;
    int found = 0;
    for (int i = 0; i < nseen; i++) if (seen[i] == (const void*)name) { found = 1; break; }
    if (!found && nseen < 8192) {
      seen[nseen++] = name;
      const char *filt = getenv("AR_FUNCLOG");
      extern int snes_frame_counter;
      if (filt[0] == '1' || strstr(name, filt))
        fprintf(stderr, "[func] frame %d: %s\n", snes_frame_counter, name);
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
  // TEMP DIAGNOSTIC: AR_CTACTION one-shot trace. ActRaiser_WaitForVblank arms
  // g_trace_one_frame for exactly one inter-yield batch once the game enters
  // action mode ($18==1), to capture the stuck per-frame loop without flooding.
  if (getenv("AR_CTACTION") && !g_batch_done && name) {
    extern int snes_frame_counter;
    extern CpuState g_cpu;
    if (strcmp(name, "bank_00_8465_M0X0") == 0) g_batch_had_8465 = 1;
    if (g_batch_len < (int)sizeof(g_batch_buf) - 256)
      g_batch_len += snprintf(g_batch_buf + g_batch_len,
              sizeof(g_batch_buf) - (size_t)g_batch_len,
              "[1f f%d m=%u x=%u] %*s%s\n", snes_frame_counter,
              (unsigned)g_cpu.m_flag, (unsigned)g_cpu.x_flag,
              g_recomp_stack_top * 2, "", name);
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
static clock_t g_frame_start_clock;
static int g_watchdog_enabled;
static int g_watchdog_counter;
jmp_buf g_watchdog_jmp;
int g_watchdog_tripped;

void WatchdogFrameStart(void) {
  g_frame_start_clock = clock();
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
  // Only check clock() every 10000 iterations to avoid overhead
  if (++g_watchdog_counter < 10000) return;
  g_watchdog_counter = 0;
  double elapsed = (double)(clock() - g_frame_start_clock) / CLOCKS_PER_SEC;
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


/* AR_VRAMWATCH: BG-tilemap VRAM-write tracer (lair-seal corruption hunt). Logs
 * writes into [$0000,$1000) word range with the issuing game function + game
 * frame, within [AR_VW_LO,AR_VW_HI] game-frames. Dedups per (func,vaddr-hi). */
/* AR_VRAMRAW=1: un-deduped raw VMDATA-write log for a tiny VRAM window
 * [0,AR_VRAW_VHI] within game-frames [AR_VW_LO,AR_VW_HI]. Logs EVERY $2118/$2119
 * write (port arg) with the issuing func + block PC — to catch a writer the
 * deduped ar_vramwatch collapses or misses. */
void ar_vramraw(uint16_t vaddr, uint8_t val, int port) {
  static int en = -1; static unsigned lo, hi, vhi;
  if (en < 0) { en = getenv("AR_VRAMRAW") ? 1 : 0;
    const char *l = getenv("AR_VW_LO"), *h = getenv("AR_VW_HI"), *v = getenv("AR_VRAW_VHI");
    lo = l ? (unsigned)strtoul(l, NULL, 0) : 0;
    hi = h ? (unsigned)strtoul(h, NULL, 0) : 0xffffffffu;
    vhi = v ? (unsigned)strtoul(v, NULL, 0) : 4; }
  if (!en) return;
  if (vaddr > vhi) return;
  extern uint8_t g_ram[0x20000];
  extern int snes_frame_counter;
  /* AR_HF_LO/HI: gate on HOST frame (monotonic) instead of game-frame $0088,
   * which is unreliable near the cutscene. */
  static long hflo = -2, hfhi = -2;
  if (hflo == -2) { const char *a = getenv("AR_HF_LO"), *b = getenv("AR_HF_HI");
    hflo = a ? atol(a) : -1; hfhi = b ? atol(b) : -1; }
  if (hflo >= 0) {
    if (snes_frame_counter < hflo || (hfhi >= 0 && snes_frame_counter > hfhi)) return;
  } else {
    unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
    if (gf < lo || gf > hi) return;
  }
  unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
  extern const char *g_last_recomp_func;
  extern uint32_t g_ar_blk_ring[]; extern unsigned g_ar_blk_idx;
  uint32_t blk = g_ar_blk_ring[(g_ar_blk_idx - 1u) & 1023u];
  static int nl; if (nl++ < 4000)
    fprintf(stderr, "[vramraw] hf=%d gf=%u port=%02x vram=$%04x val=%02x blk=$%06X func=%s\n",
            snes_frame_counter, gf, port, vaddr, val, blk, g_last_recomp_func ? g_last_recomp_func : "?");
}

int ar_vramwatch(uint16_t vaddr, uint8_t val) {
  static int en = -1; static unsigned lo, hi, vlo, vhi;
  if (en < 0) {
    en = getenv("AR_VRAMWATCH") ? 1 : 0;
    const char *l = getenv("AR_VW_LO"), *h = getenv("AR_VW_HI");
    const char *vl = getenv("AR_VW_VLO"), *vh = getenv("AR_VW_VHI");
    lo = l ? (unsigned)strtoul(l, NULL, 0) : 0;
    hi = h ? (unsigned)strtoul(h, NULL, 0) : 0xffffffffu;
    vlo = vl ? (unsigned)strtoul(vl, NULL, 0) : 0;      /* default all VRAM */
    vhi = vh ? (unsigned)strtoul(vh, NULL, 0) : 0x7fff;
  }
  if (!en) return 0;
  if (vaddr < vlo || vaddr > vhi) return 0;
  extern uint8_t g_ram[0x20000];
  unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
  if (gf < lo || gf > hi) return 0;
  extern const char *g_last_recomp_func;
  extern uint32_t g_ar_blk_ring[]; extern unsigned g_ar_blk_idx;
  const char *fn = g_last_recomp_func ? g_last_recomp_func : "?";
  /* Per-FRAME dedup on (fn, vaddr>>5): collapse each frame's burst to a few
   * lines but show the frame-by-frame timeline (the array clears each frame). */
  static const void *sf[512]; static unsigned sv[512]; static int n;
  static unsigned last_gf = 0xffffffffu; static unsigned wr_this_frame;
  if (gf != last_gf) { last_gf = gf; n = 0; wr_this_frame = 0; }
  wr_this_frame++;
  unsigned key = vaddr >> 5;
  for (int i = 0; i < n; i++) if (sf[i] == fn && sv[i] == key) return 0;
  if (n < 512) { sf[n] = fn; sv[n] = key; n++; }
  uint32_t blk = g_ar_blk_ring[(g_ar_blk_idx - 1u) & 1023u];
  fprintf(stderr, "[vramwatch] gf=%u vram=$%04x val=%02x blk=$%06X func=%s\n",
          gf, vaddr, val, blk, fn);
  return 0;
}
