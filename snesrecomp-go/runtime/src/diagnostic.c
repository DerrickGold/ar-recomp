/* diagnostic.c — RT7: the always-compiled, env-gated diagnostic framework,
 * split out of common_cpu_infra.c so core infra (stack bookkeeping, watchdog,
 * dispatch/tailcall context, SnesInit) is separable from the misdecode/leak
 * hunting instrumentation. Everything here is self-silencing: each entry point
 * gates on its AR_* environment flag and is a cheap early-return when unset.
 * Symbols keep their historical names and linkage — generated code reaches
 * them through cpu_state.h's inline gates (ar_entry_mx_check and friends) and
 * cpu_trace.h, which are unchanged. */
#include "diagnostic.h"

#include "common_cpu_infra.h"
#include "cpu_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#define AR_HAVE_BACKTRACE 1
#endif

/* The always-on block-PC ring stays in common_cpu_infra.c (it is written by
 * the inline cpu_trace_block on every block entry — core infra, not gated
 * diagnostics). The dumps here only read it. This size mirror must match the
 * AR_BLK_RING definition next to the ring. */
#define AR_BLK_RING 1024
extern uint32_t g_ar_blk_ring[];
extern uint32_t g_ar_blk_aux[];
extern uint16_t g_ar_blk_s[];
extern unsigned g_ar_blk_idx;

/* Core-infra call-attribution state (owned by common_cpu_infra.c). */
extern const char *g_last_recomp_func;

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
#ifdef AR_HAVE_BACKTRACE
      void *bt[32];
      int n = backtrace(bt, 32);
      fprintf(stderr, "[mxcheck-bt] real C call stack for %s:\n", fn);
      backtrace_symbols_fd(bt, n, 2);
#endif
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
uint32_t *g_stack_pusher = NULL;         /* 256KB, lazily calloc'd when AR_STACKPROV on */
unsigned *g_stack_pusher_frame = NULL;   /* 256KB, lazily calloc'd when AR_STACKPROV on */

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
  if (e < 0) {
    e = getenv("AR_STACKPROV") ? 1 : 0;
    if (e && !g_stack_pusher) {
      g_stack_pusher       = calloc(0x10000, sizeof(uint32_t));
      g_stack_pusher_frame = calloc(0x10000, sizeof(unsigned));
      if (!g_stack_pusher || !g_stack_pusher_frame) e = 0;  /* alloc failed: stay disabled */
    }
  }
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
