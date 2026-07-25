#pragma once

/* diagnostic.h — RT7: the env-gated diagnostic framework split out of
 * common_cpu_infra.c. Always compiled; every entry point self-silences when
 * its AR_* environment flag is unset, so a release build pays one cached
 * getenv + branch per gated path.
 *
 * Callers largely do NOT include this header: generated code reaches these
 * symbols through cpu_state.h's inline gates (ar_entry_mx_check and friends),
 * cpu_trace.h's inline cpu_trace_block, and local extern declarations in
 * ppu.c / cpu_state.c / the game layer — all unchanged by the split. The
 * declarations here are the canonical inventory of the framework's surface,
 * kept in sync with those use-site externs (any drift is a compile error in
 * diagnostic.c, which includes this header). */

#include "types.h"
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Entry/exit/call-site m/x + stack invariant checks (cpu_state.h gates) ── */
extern int g_ar_mx_check;            /* AR_MXCHECK */
extern int g_ar_mxhist;              /* AR_MXHIST */
extern const char *g_ar_trapfn;      /* AR_TRAPFN=<substring> */
extern int g_ar_exit_mx_check;       /* AR_EXITMX */
extern int g_ar_exit_s_check;        /* AR_EXITS */
extern int g_ar_call_mx_check;       /* AR_CALLMX */
void ar_entry_mx_fail(CpuState *cpu, int em, int ex, const char *fn, uint32 pc24);
void ar_exit_mx_fail(CpuState *cpu, int exm, int exx, const char *fn, uint32 pc24);
void ar_exit_s_fail(CpuState *cpu, uint32 entry_s, uint32 ret_s,
                    const char *fn, uint32 pc24);
void ar_call_mx_fail(CpuState *cpu, int em, int ex, const char *fn, uint32 pc24);

/* ── Runtime m/x histogram + misdecode traps ──────────────────────────── */
void ar_mxhist_record(uint32 pc24, int m, int x);
void ar_mxhist_dump(void);           /* + AR_FNCENSUS census file */
void ar_entry_trapfn(CpuState *cpu, const char *fn, uint32 pc24);
void ar_garbage_variant_trap(CpuState *cpu, const char *fn, uint32 pc24);

/* Always-on indirect-dispatch OOB tripwire (AR_NOOOBWARN=1 silences). */
RecompReturn ar_dispatch_oob_warn(CpuState *cpu, uint32_t site_pc24, uint16_t idx);

/* ── Block-history readers (the ring itself lives in common_cpu_infra.c) ── */
int ar_block_history2(uint32_t *pc, uint32_t *aux, int max);
int ar_block_history3(uint32_t *pc, uint32_t *aux, uint16_t *s, int max);

/* ── AR_XTRACE: x-flag-transition ring ────────────────────────────────── */
int ar_xtrace_enabled(void);
void ar_xtrace_record(uint32_t blk, uint32_t nxt, int new_x, int m, uint32_t gf);
void ar_xtrace_dump(const char *why, int n);

/* ── AR_STRACE: windowed instruction-granular stack trace ─────────────── */
int ar_strace_active(void);
void ar_strace_op(const char *kind, uint16_t addr, uint8_t val, uint16_t s);
void ar_strace_block(uint32_t pc24, uint16_t s, int m, int x);

/* ── AR_STACKPROV: stack pusher-provenance shadow arrays ──────────────── */
extern uint32_t *g_stack_pusher;         /* lazily calloc'd when enabled */
extern unsigned *g_stack_pusher_frame;
int ar_stackprov_enabled(void);

/* ── VRAM write tracers (AR_VRAMWATCH / AR_VRAMRAW) ───────────────────── */
void ar_vramraw(uint16_t vaddr, uint8_t val, int port);
int ar_vramwatch(uint16_t vaddr, uint8_t val);

#ifdef __cplusplus
}
#endif
