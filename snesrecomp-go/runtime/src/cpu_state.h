#pragma once

/*
 * snesrecomp v2 runtime CpuState.
 *
 * The single mutable container for 65816 register + flag state at
 * runtime. Every v2-recompiled function takes `CpuState *cpu` as its
 * sole parameter and mutates `cpu->A`, `cpu->X`, etc., directly. No
 * return values, no per-function locals masquerading as registers.
 *
 * REPLACES v1's per-function locals + decode-time M/X metadata fiction
 * + struct-packed return types (RetAY, RetY, PairU16, HdmaPtrs, etc.).
 *
 * v2 hand-written runtime bodies (NMI/IRQ entry, PPU/DMA orchestration,
 * etc.) keep working because `cpu->ram` aliases `g_ram` and existing
 * `g_ram[addr]` reads/writes from those bodies see the same bytes the
 * recompiled code sees.
 *
 * `m_flag` / `x_flag` / `emulation` are mirrors of P bits 5, 4, and the
 * E flag respectively. They're carried as their own slots so codegen
 * doesn't have to re-decode P every memory access; `RepFlags` /
 * `SepFlags` keep them in sync with `P` on every update.
 */

#include "types.h"
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Register / flag state ─────────────────────────────────────────────── */

typedef struct CpuState {
    /* Accumulator and index registers. Stored as 16-bit always; the
     * M / X flags govern semantic width when codegen reads/writes them.
     * In M=1 mode only the low byte is semantically live, but the high
     * byte is preserved verbatim across 8-bit ops — it's still part of
     * the 16-bit accumulator and surfaces under XBA / TDC / PHA-16.
     *
     * NOTE: there is no separate `B` field. The byte the 65816 calls B
     * is by definition `(A >> 8) & 0xFF`. A previous shadow `B` field
     * went stale on every 16-bit LDA and produced silent XBA bugs
     * (Layer-3 stripe corruption — see TROUBLESHOOTING.md). Reads of
     * "B" route through the cpu_read_b() helper below or `(A >> 8)`
     * inline; nothing ever writes B as separate state. */
    uint16 A;
    uint16 X;
    uint16 Y;

    /* Stack and direct-page pointers, bank registers. */
    uint16 S;
    uint16 D;
    uint8  DB;
    uint8  PB;

    /* Option-1 cpu->S return-frame ABI (see IMPROVEMENTS.md). 1 when the
     * current function was entered via a direct generated JSR/JSL C call
     * (a paired host-C caller exists and pushed a matching return frame on
     * cpu->S). 0 when entered via cpu_dispatch_pc_from / PEI-RTL trampoline
     * / interrupt / dynarec (no proven paired host caller). Each function
     * captures it into a local `_hrv` at entry; the caller sets it right
     * before every invoke (direct call -> 1; tail JMP/JML -> propagate the
     * caller's _hrv; dispatch -> 0). RTS/RTL may return RECOMP_RETURN_NORMAL
     * only when _hrv==1 AND the stack was balanced at entry (cpu->S ==
     * _entry_s); otherwise it dispatches on the popped PC24. */
    uint8  host_return_valid;

    /* Status register P (full byte). Individual bit mirrors below for
     * codegen efficiency — they MUST be kept in sync via the helpers
     * declared below (or RepFlags / SepFlags / SetFlag IR ops). */
    uint8  P;

    /* Mirrors of P bits 5 and 4 plus the E flag. 1 = 8-bit width. */
    uint8  m_flag;
    uint8  x_flag;
    uint8  emulation;

    /* Per-flag bit mirrors. v2 codegen reads/writes `cpu->_flag_C`
     * etc. directly (rather than masking P each access). They MUST be
     * kept in sync with `P` via cpu_p_to_mirrors / cpu_mirrors_to_p
     * on every operation that updates P (REP/SEP/PLP/RTI). */
    uint8  _flag_N;
    uint8  _flag_V;
    uint8  _flag_Z;
    uint8  _flag_C;
    uint8  _flag_I;
    uint8  _flag_D;

    /* RAM. Points at the runtime's `g_ram[]` 128KB region — same bytes
     * the existing hand-written runtime reads/writes. v2 codegen will
     * issue cpu_readN / cpu_writeN against this pointer so DB / D / S
     * / PB-relative addressing all resolve through the cpu_ helpers. */
    uint8 *ram;
} CpuState;

/* NB: NLR pending-skip state is intentionally NOT a CpuState field.
 * It's declared as a function-local `RecompReturn _pending_skip` at
 * the top of every emitted v2 function — see emit_function.py. NLR
 * skip is C control-flow state, not 65816 hardware state, and a
 * prior `cpu->pending_skip` field on CpuState produced layout/
 * aliasing weirdness around the cpu pointer that masked the bug
 * the dynamic auditor was supposed to characterize. The local
 * variable is invisible to unrelated runtime helpers, can be kept
 * in a register by the optimizer, and matches the actual lifetime
 * of the signal (only meaningful within the currently executing
 * generated function).
 */

/* Read the B byte of the 16-bit accumulator. Always equals the high
 * byte of A; provided as a helper so call sites read intent rather
 * than reaching into A bits directly. There is no `cpu_write_b` — the
 * way to "write B" on real hardware is XBA (or TCD/TDC), and those
 * route through full-A arithmetic. */
static inline uint8 cpu_read_b(const CpuState *cpu) {
    return (uint8)((cpu->A >> 8) & 0xFF);
}

/* ── Non-local return signaling ────────────────────────────────────────
 *
 * Some 65816 functions implement "return-to-grandparent" via the
 * stack-discard idiom:
 *     PLA          ; pop own JSR return PC low
 *     PLA          ; pop own JSR return PC high
 *     ...          ; (or 3 PLAs for JSL/RTL)
 *     RTS          ; now pops grandparent's return PC
 *
 * In v2 codegen, JSR/JSL/RTS/RTL don't push or pop return-PC bytes on
 * the simulated SNES stack — those are tracked purely via C call
 * frames. So translating PLA literally as `cpu->S += 1; A = ram[S]`
 * would consume bytes that belong to AN ANCESTOR'S stack frame and
 * leave the SNES S register drifted (root cause of "DB=$C0 at
 * ProcessGameMode entry" 2026-05-02).
 *
 * Instead, the v2 ABI returns a small enum: NORMAL means "RTS to
 * immediate caller"; SKIP_N means "skip N additional levels of C
 * return" (i.e., the asm RTS would have unwound past N JSR frames).
 *
 * Callsite contract (emitted by codegen):
 *     RecompReturn _r = Callee(cpu);
 *     if (_r != RECOMP_RETURN_NORMAL) {
 *         return (RecompReturn)((int)_r - 1);
 *     }
 *
 * SKIP_N is set by NLR-pattern blocks via `cpu->pending_skip` (below);
 * the next Return op consumes it. */
typedef enum RecompReturn {
    RECOMP_RETURN_NORMAL = 0,
    RECOMP_RETURN_SKIP_1 = 1,
    RECOMP_RETURN_SKIP_2 = 2,
    RECOMP_RETURN_SKIP_3 = 3,
    /* Trampoline sentinel: a DISPATCHED frame (host_return_valid==0, i.e.
     * running inside a cpu_dispatch_pc_from driving loop) returns this in place
     * of recursively calling cpu_dispatch_pc_from for a computed-jump tail-
     * dispatch. It stashes the next target via cpu_tailcall_request() and the
     * loop iterates flat instead of nesting the C stack once per computed jump.
     * Without it, a long RAM-pointer dispatch loop (ActRaiser's $8915 object
     * loop nesting $8966) grows the 64-deep recomp stack to overflow and emits
     * a giant SKIP_N that over-unwinds past its JSR caller. Value is far above
     * any real SKIP_N (bounded by RECOMP_STACK_DEPTH=64) and is consumed only
     * by the driving loop, so it never reaches a skip-propagation site. */
    RECOMP_RETURN_TAILCALL = 0x4000,
} RecompReturn;

/* ── Typed register access ────────────────────────────────────────────────
 *
 * The 65816 accumulator/index registers carry semantic width via M/X
 * flags. The hardware contracts:
 *   - A (m=1):  ops touch only A.low; A.high (= B) is preserved.
 *   - A (m=0):  ops touch full 16 bits.
 *   - X/Y (x=1): ops touch only the low byte; the high byte is FORCED
 *                to 0 on write (hw contract — distinct from A).
 *   - X/Y (x=0): full 16-bit.
 *
 * Use the typed helpers below at every codegen site that reads or
 * writes A/X/Y. The function name encodes the width explicitly:
 *
 *   cpu_read_a8 / a16 / x8 / x16 / y8 / y16     (bare-width readers)
 *   cpu_write_a8 / a16 / x8 / x16 / y8 / y16    (bare-width writers)
 *   cpu_read_a_m  / x_x  / y_x                   (M/X-flag dispatching reads)
 *   cpu_write_a_m / x_x  / y_x                   (M/X-flag dispatching writes)
 *
 * Why typed helpers instead of raw `cpu->A`:
 *   1. The READ TYPE forces the caller to think about width. A bare
 *      `cpu->A` returns 16 bits even in M=1 contexts; the caller can
 *      forget to mask, and the bug only surfaces when the high byte
 *      happens to be non-zero (the SMW XBA stale-shadow class).
 *   2. The WRITE semantics differ between A (preserve high) and X/Y
 *      (zero high). Encoding it in the helper name removes the foot-gun
 *      of a contributor copy-pasting the wrong shape.
 *   3. M/X-flag dispatch lives in ONE place (the helper) rather than
 *      inline at every emit site. If a future hardware nuance is
 *      discovered, it lands in the helper and every site picks it up.
 */

/* ── A-register typed access ── */

static inline uint8  cpu_read_a8(const CpuState *cpu) {
    return (uint8)(cpu->A & 0xFF);
}
static inline uint16 cpu_read_a16(const CpuState *cpu) {
    return cpu->A;
}
/* M-flag-driven read. Returns A.low zero-extended in m=1, full A in m=0.
 * Matches what `LDA` would observe when reading the accumulator at the
 * current width. */
static inline uint16 cpu_read_a_m(const CpuState *cpu) {
    return cpu->m_flag ? (uint16)cpu_read_a8(cpu) : cpu_read_a16(cpu);
}

/* AR_TRACEA=1 (2026-07-01, temporary probe): every cpu_write_a8/a16 call,
 * unconditional. Settles whether the $0019-read-then-write sequence at
 * bank00 $008106 really produces A's low byte == the value read (the C
 * source says it must; observed behavior says A ends up 0x00A1 instead
 * of the read value). If this shows the write DOES set A correctly and
 * something ELSE changes it before the next log point, that narrows the
 * corruption to between here and there; if the write itself never
 * receives the expected value, the read observed by AR_READ0019 isn't
 * actually what feeds this call. */
static inline void ar_trace_a_write(uint16 old_a, uint8 v_low, int is16, uint16 new_a) {
    extern int snes_frame_counter; extern const char *g_last_recomp_func;
    /* Gated by AR_TRACEA_GF=<game-frame> (default 600) -- A gets written
     * constantly across the whole game; an unconditional cap would
     * exhaust in the first fraction of a second, long before reaching
     * sim mode. NOT gated by g_last_recomp_func: the read immediately
     * preceding the corruption showed cur=(none), so a function-name
     * filter would risk skipping exactly the write we need to see. */
    extern uint8 g_ram[0x20000];
    static long min_gf = -2;
    if (min_gf == -2) { const char *e = getenv("AR_TRACEA_GF");
        min_gf = e ? atol(e) : 600; }
    unsigned gf = (unsigned)g_ram[0x88] | ((unsigned)g_ram[0x89] << 8);
    if ((long)gf < min_gf) return;
    static int n;
    if (n++ < 3000) {
        fprintf(stderr, "[tracea] %s A %04x -> %04x (v=%s%02x) f=%d cur=%s\n",
                is16 ? "write16" : "write8", old_a, new_a,
                is16 ? "" : "0x", v_low, snes_frame_counter,
                g_last_recomp_func ? g_last_recomp_func : "?");
    }
}

/* 8-bit A write — preserve high byte (= B). 65816 hw contract: in M=1
 * mode, ops on A leave the high half untouched (XBA / TDC observe the
 * preserved value). Distinct from cpu_write_x8 which ZEROS the high. */
static inline void cpu_write_a8(CpuState *cpu, uint8 v) {
    uint16 old_a = cpu->A;
    cpu->A = (uint16)((cpu->A & 0xFF00) | (uint16)v);
    static int en = -1;
    if (en < 0) en = getenv("AR_TRACEA") != NULL;
    if (en) ar_trace_a_write(old_a, v, 0, cpu->A);
}
static inline void cpu_write_a16(CpuState *cpu, uint16 v) {
    uint16 old_a = cpu->A;
    cpu->A = v;
    static int en = -1;
    if (en < 0) en = getenv("AR_TRACEA") != NULL;
    if (en) ar_trace_a_write(old_a, (uint8)(v & 0xFF), 1, cpu->A);
}
/* M-flag-driven write. 8-bit semantics in m=1 (preserve high), full
 * 16-bit in m=0. Caller passes a 16-bit value; we mask in m=1. */
static inline void cpu_write_a_m(CpuState *cpu, uint16 v) {
    if (cpu->m_flag) cpu_write_a8(cpu, (uint8)(v & 0xFF));
    else             cpu_write_a16(cpu, v);
}

/* ── X-register typed access ── */

static inline uint8  cpu_read_x8(const CpuState *cpu) {
    return (uint8)(cpu->X & 0xFF);
}
static inline uint16 cpu_read_x16(const CpuState *cpu) {
    return cpu->X;
}
static inline uint16 cpu_read_x_x(const CpuState *cpu) {
    return cpu->x_flag ? (uint16)cpu_read_x8(cpu) : cpu_read_x16(cpu);
}

/* 8-bit X write — ZEROS high byte (65816 hw contract for x=1). This is
 * the critical difference vs cpu_write_a8 (which preserves high). The
 * historical "8-bit X/Y zero-extend" bug class (snesrecomp 6o, b39e99b)
 * happened because emitters treated X/Y like A and let stale high bytes
 * leak through indexed reads. */
static inline void cpu_write_x8(CpuState *cpu, uint8 v) {
    cpu->X = (uint16)v;
}
static inline void cpu_write_x16(CpuState *cpu, uint16 v) {
    cpu->X = v;
}
static inline void cpu_write_x_x(CpuState *cpu, uint16 v) {
    if (cpu->x_flag) cpu_write_x8(cpu, (uint8)(v & 0xFF));
    else             cpu_write_x16(cpu, v);
}

/* ── Y-register typed access (mirrors X) ── */

static inline uint8  cpu_read_y8(const CpuState *cpu) {
    return (uint8)(cpu->Y & 0xFF);
}
static inline uint16 cpu_read_y16(const CpuState *cpu) {
    return cpu->Y;
}
static inline uint16 cpu_read_y_x(const CpuState *cpu) {
    return cpu->x_flag ? (uint16)cpu_read_y8(cpu) : cpu_read_y16(cpu);
}
static inline void cpu_write_y8(CpuState *cpu, uint8 v) {
    cpu->Y = (uint16)v;  /* x=1 zeros high */
}
static inline void cpu_write_y16(CpuState *cpu, uint16 v) {
    cpu->Y = v;
}
static inline void cpu_write_y_x(CpuState *cpu, uint16 v) {
    if (cpu->x_flag) cpu_write_y8(cpu, (uint8)(v & 0xFF));
    else             cpu_write_y16(cpu, v);
}

/* P-bit positions (matches 65816 hardware). */
#define CPU_P_C  0x01u  /* Carry */
#define CPU_P_Z  0x02u  /* Zero */
#define CPU_P_I  0x04u  /* IRQ disable */
#define CPU_P_D  0x08u  /* Decimal */
#define CPU_P_X  0x10u  /* Index width (1=8-bit) */
#define CPU_P_M  0x20u  /* Memory/A width (1=8-bit) */
#define CPU_P_V  0x40u  /* Overflow */
#define CPU_P_N  0x80u  /* Negative */

/* Sync P <-> mirrors. Codegen calls these whenever P is touched in a
 * way that updates the bit mirrors (REP, SEP, PLP, RTI). */
static inline void cpu_p_to_mirrors(CpuState *cpu) {
    cpu->m_flag  = (cpu->P & CPU_P_M) ? 1 : 0;
    cpu->x_flag  = (cpu->P & CPU_P_X) ? 1 : 0;
    cpu->_flag_C = (cpu->P & CPU_P_C) ? 1 : 0;
    cpu->_flag_Z = (cpu->P & CPU_P_Z) ? 1 : 0;
    cpu->_flag_I = (cpu->P & CPU_P_I) ? 1 : 0;
    cpu->_flag_D = (cpu->P & CPU_P_D) ? 1 : 0;
    cpu->_flag_V = (cpu->P & CPU_P_V) ? 1 : 0;
    cpu->_flag_N = (cpu->P & CPU_P_N) ? 1 : 0;
}

static inline void cpu_mirrors_to_p(CpuState *cpu) {
    cpu->P = (uint8)(
        (cpu->m_flag  ? CPU_P_M : 0) |
        (cpu->x_flag  ? CPU_P_X : 0) |
        (cpu->_flag_C ? CPU_P_C : 0) |
        (cpu->_flag_Z ? CPU_P_Z : 0) |
        (cpu->_flag_I ? CPU_P_I : 0) |
        (cpu->_flag_D ? CPU_P_D : 0) |
        (cpu->_flag_V ? CPU_P_V : 0) |
        (cpu->_flag_N ? CPU_P_N : 0)
    );
}

/* ── Memory access ──────────────────────────────────────────────────────── */

/*
 * Memory helpers map a 24-bit logical address (bank << 16 | abs) onto
 * the runtime's flat `g_ram[0x20000]` according to the existing
 * snesrecomp memory map (see common_rtl.h). They do NOT perform any
 * banking arithmetic of their own beyond what the existing runtime
 * already does — they're a thin shim so the v2 codegen can speak in
 * terms of (bank, abs) without re-implementing the map.
 *
 * Width: 1 byte or 2 bytes (LE).
 *
 * The DB / D / S / PB-relative resolution lives in higher-level
 * helpers added in Phase 5/6 alongside the codegen — for Phase 4 we
 * ship just the raw byte/word read/write primitives.
 */

uint8  cpu_read8 (CpuState *cpu, uint8 bank, uint16 addr);
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr);
void   cpu_write8 (CpuState *cpu, uint8 bank, uint16 addr, uint8  v);
void   cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v);

/* Direct-page / stack POINTER fetch: a 16-bit read whose two bytes are
 * confined to bank $00 — the high byte wraps within the bank
 * (addr $FFFF -> $0000), it does NOT cross into bank $01. This is the
 * native-mode 65816 rule for the indirect-addressing pointer fetch and
 * differs from cpu_read16, which is used for the final DATA access and
 * DOES cross the bank boundary. Using cpu_read16 for the pointer fetch
 * mis-read the high byte when (D+dp) or (S+sr) landed at $00:FFFF.
 * Verified vs Harte 65816 vectors. */
static inline uint16 cpu_read16_dp(CpuState *cpu, uint16 addr) {
    uint8 lo = cpu_read8(cpu, 0x00, addr);
    uint8 hi = cpu_read8(cpu, 0x00, (uint16)(addr + 1));
    return (uint16)((uint16)lo | ((uint16)hi << 8));
}

/* Software-interrupt (BRK/COP) hooks. Generated code calls these at a BRK/COP
 * site to model the vector handler's effect, then FALLS THROUGH to PC+2 (the
 * handler RTIs there). The game installs a hook replicating its BRK/COP handler
 * (e.g. ActRaiser's BRK handler stores A -> $035B, the sound-request port).
 * NULL when the game doesn't use them — then BRK/COP is an effect-free continue. */
extern void (*g_cpu_brk_hook)(CpuState *cpu);
extern void (*g_cpu_cop_hook)(CpuState *cpu);

/* ── Interrupt-frame ABI (Option-1 cpu->S model) ────────────────────────── */

/* Model the 65816 hardware interrupt-entry push so a handler invoked via a
 * plain host-C call (the game's rtl I_NMI / I_IRQ wrappers) leaves cpu->S in
 * the state the handler's RTI expects. Hardware pushes, in order (S
 * decreasing): native = PB, PCH, PCL, P (4 bytes); emulation = PCH, PCL, P
 * (3 bytes). The handler's RTI (codegen _emit_return op.interrupt) pops in
 * the mirror order: pull P (top), discard PC (2), native discard PB (1).
 * PC/PB are discarded on RTI (host-C return carries control), so only P must
 * be live — it is restored to the interrupted code. MUST be paired with that
 * RTI pop; without this push the RTI over-pops cpu->S. */
static inline void cpu_push_interrupt_frame(CpuState *cpu) {
    /* v2 CpuState has no runtime PC (control flow is host-C call structure),
     * so the PC bytes are pushed as zero placeholders — RTI discards them.
     * Only P is live; cpu_mirrors_to_p syncs it from the flag mirrors first. */
    cpu_mirrors_to_p(cpu);
    if (!cpu->emulation) {
        cpu_write8(cpu, 0x00, cpu->S, cpu->PB); cpu->S = (uint16)(cpu->S - 1);
    }
    cpu_write8(cpu, 0x00, cpu->S, 0x00); cpu->S = (uint16)(cpu->S - 1);  /* PCH */
    cpu_write8(cpu, 0x00, cpu->S, 0x00); cpu->S = (uint16)(cpu->S - 1);  /* PCL */
    cpu_write8(cpu, 0x00, cpu->S, cpu->P); cpu->S = (uint16)(cpu->S - 1);
}

/* Model a 65816 hardware JSR's 2-byte return-frame push. Used by game glue
 * that bare-calls a recomp entry point whose body TAIL-dispatches to handlers
 * (host_return_valid=0) — those handlers' terminal RTS pops the *caller's*
 * JSR frame (the hardware main loop did `JSR ProcessGameMode`). When the glue
 * replaces that main loop with a plain host-C call it must push the same
 * 2-byte frame, else the dispatched handler's trampoline-RTS over-pops cpu->S
 * by 2 on every call (SMW's per-frame GameMode dispatch leaked exactly this).
 * PC bytes are placeholders — the trampoline-RTS dispatch-misses on them and
 * miss-restores S to entry_s + 2. Symmetric to cpu_push_interrupt_frame. */
static inline void cpu_push_jsr_return_frame(CpuState *cpu) {
    cpu_write8(cpu, 0x00, cpu->S, 0x00); cpu->S = (uint16)(cpu->S - 1);  /* PCH */
    cpu_write8(cpu, 0x00, cpu->S, 0x00); cpu->S = (uint16)(cpu->S - 1);  /* PCL */
    cpu->host_return_valid = 1;
}

/* Model a 65816 hardware JSL's 3-byte return-frame push for host glue
 * that calls a long subroutine alias directly. The sentinel address is
 * only observed on a trampoline miss; a balanced RTL host-returns before
 * dispatching on it. */
static inline void cpu_push_jsl_return_frame(CpuState *cpu) {
    cpu_write8(cpu, 0x00, cpu->S, 0xFF); cpu->S = (uint16)(cpu->S - 1);  /* PB */
    cpu_write8(cpu, 0x00, cpu->S, 0xFF); cpu->S = (uint16)(cpu->S - 1);  /* PCH */
    cpu_write8(cpu, 0x00, cpu->S, 0xFF); cpu->S = (uint16)(cpu->S - 1);  /* PCL */
    cpu->host_return_valid = 1;
}

/* ── Initialisation ─────────────────────────────────────────────────────── */

/* Initialise `cpu` to a 65816 reset state: emulation=1, P=0x34
 * (M=X=I=1, others clear), S=0x01FF, D=0, DB=PB=0, A/B/X/Y zero.
 * Caller supplies the ram pointer (typically &g_ram[0]). */
void cpu_state_init(CpuState *cpu, uint8 *ram);

/* The singleton runtime CpuState. Defined alongside g_ram in
 * common_rtl.c. v2-recompiled code passes &g_cpu when it doesn't
 * thread `cpu` explicitly. */
extern CpuState g_cpu;

/* Diagnostic — generated functions can call this to log entry. */
void cpu_dbg_funcname(const char *name);

/* ── Entry m/x invariant check (AR_MXCHECK) ───────────────────────────
 * Each generated variant bank_XX_PC_MmXn was emitted ASSUMING entry
 * m=m,x=n. Dispatched calls always match (the dispatch switch picks the
 * variant by runtime flags), so a mismatch can only happen on a DIRECT
 * call, where the emitter's STATIC m/x analysis chose the variant — i.e.
 * a mismatch means that analysis was wrong (the recurring "m/x leak /
 * wrong-variant" bug class), caught at the exact call site. Opt-in via
 * AR_MXCHECK=1; near-free when off (one global load + predicted branch).
 * Logs (rate-limited) and continues — never aborts — so a playthrough
 * surfaces the whole cluster of sites in one run. */
extern int g_ar_mx_check;  /* set once from AR_MXCHECK env */
extern int g_ar_mxhist;    /* set once from AR_MXHIST env */
extern const char *g_ar_trapfn; /* AR_TRAPFN substring; dump call stack on entry */
void ar_entry_mx_fail(CpuState *cpu, int em, int ex, const char *fn, uint32 pc24);
void ar_mxhist_record(uint32 pc24, int m, int x);
void ar_mxhist_dump(void);
void ar_entry_trapfn(CpuState *cpu, const char *fn, uint32 pc24);
void ar_garbage_variant_trap(CpuState *cpu, const char *fn, uint32 pc24);
void ar_adad_trace(CpuState *cpu, const char *fn, uint32 pc24);
/* Unified runtime trace (ar_trace.c) — declared here so the inline
 * ar_entry_mx_check func-entry hook can call it without pulling ar_trace.h. */
int  ar_trace_active(void);
void ar_trace_func(uint32 pc24, const char *name, int m, int x, int em, int ex);
void ar_trace_call(uint32 pc24, const char *name, int m, int x, int em, int ex);
static inline void ar_entry_mx_check(CpuState *cpu, int em, int ex,
                                     const char *fn, uint32 pc24) {
  if (g_ar_mx_check
      && (((cpu->m_flag & 1) != em) || ((cpu->x_flag & 1) != ex)))
    ar_entry_mx_fail(cpu, em, ex, fn, pc24);
  if (g_ar_mxhist)
    ar_mxhist_record(pc24, cpu->m_flag & 1, cpu->x_flag & 1);
  if (g_ar_trapfn)
    ar_entry_trapfn(cpu, fn, pc24);
  ar_adad_trace(cpu, fn, pc24);
  /* Unified AR_TRACE func-entry channel (records runtime + expected m/x, so a
   * misdecode is visible inline). No-op unless AR_TRACE is set + in window. */
  if (ar_trace_active())
    ar_trace_func(pc24, fn, cpu->m_flag & 1, cpu->x_flag & 1, em, ex);
}

/* ── Exit-side invariant checks (symmetric twins of ar_entry_mx_check) ──
 * Every function validates its ENTRY (m,x) but historically returned
 * completely unchecked — the blind spot where both the $03:9156 (exit-m
 * misdecode) and $01:B8CF (exit stack-drift) act->sim crashes lived.
 *
 * ar_exit_mx_check: at a clean host-return, runtime (m,x) MUST equal the
 *   exit (m,x) the emitter recorded for this variant (the value propagated
 *   to CALLERS). A mismatch = the exit-mx analysis was wrong -> every caller
 *   decoded its post-call code at the wrong width. Names the culprit at its
 *   source in one run (would have flagged $9156's bogus (1,0) exit).
 * ar_exit_s_check: at an RTS/RTL reached with cpu->S != entry_s on a paired
 *   (host-JSR) frame with no ancestor parked there, the function DRIFTED its
 *   stack -> the RTS/RTL pops a garbage return ($01:B8CF PLB;PLP;RTL class).
 * Both rate-limited, log-and-continue, env-gated (AR_EXITMX / AR_EXITS). */
extern int g_ar_exit_mx_check;  /* set once from AR_EXITMX env */
extern int g_ar_exit_s_check;   /* set once from AR_EXITS env */
void ar_exit_mx_fail(CpuState *cpu, int exm, int exx, const char *fn, uint32 pc24);
void ar_exit_s_fail(CpuState *cpu, uint32 entry_s, uint32 ret_s,
                    const char *fn, uint32 pc24);
static inline void ar_exit_mx_check(CpuState *cpu, int exm, int exx,
                                    const char *fn, uint32 pc24) {
  if (g_ar_exit_mx_check
      && (((cpu->m_flag & 1) != exm) || ((cpu->x_flag & 1) != exx)))
    ar_exit_mx_fail(cpu, exm, exx, fn, pc24);
}
static inline void ar_exit_s_check(CpuState *cpu, uint16 entry_s, uint16 ret_s,
                                   const char *fn, uint32 pc24) {
  if (g_ar_exit_s_check && ret_s != entry_s)
    ar_exit_s_fail(cpu, entry_s, ret_s, fn, pc24);
}

/* ── Call-site invariant check (AR_CALLMX, 2026-06-30) ──────────────────
 * A MID-function twin of ar_entry_mx_check: at every JSR/JSL, verify runtime
 * (m,x) still matches what the decoder statically knew at THIS instruction
 * (Call.entry_m/entry_x — the caller's own flags at the call site, since
 * JSR/JSL don't change M/X). A mismatch means (m,x) was corrupted SOMEWHERE
 * between function entry (already verified clean by ar_entry_mx_check) and
 * this call — narrows a "runtime flags disagree with decode" bug down to a
 * specific instruction, catching corruption from ANYWHERE upstream (not just
 * decode-time mistakes the entry/exit checks cover), at the first call site
 * downstream of it. Added chasing the $01:933C -> $01:B898 anomaly (933C's
 * own generated C, traced instruction-by-instruction, provably never
 * touches x_flag between entry and this call — yet runtime disagreed). */
extern int g_ar_call_mx_check;  /* set once from AR_CALLMX env */
void ar_call_mx_fail(CpuState *cpu, int em, int ex, const char *fn, uint32 pc24);
static inline void ar_call_mx_check(CpuState *cpu, int em, int ex,
                                    const char *fn, uint32 pc24) {
  if (g_ar_call_mx_check
      && (((cpu->m_flag & 1) != em) || ((cpu->x_flag & 1) != ex)))
    ar_call_mx_fail(cpu, em, ex, fn, pc24);
  /* Unified AR_TRACE `call` channel — logs the DECODER-expected (em,ex) vs
   * runtime m/x at this call site; a mismatch (`leak`) is the m-leak boundary.
   * This is AR_CALLMX folded into the one-run trace. No-op unless AR_TRACE on. */
  if (ar_trace_active())
    ar_trace_call(pc24, fn, cpu->m_flag & 1, cpu->x_flag & 1, em, ex);
}

/* AR_INDIRLOG (2026-07-01): every `JSR (abs,X)` the decoder severed for
 * lack of cfg `indirect_call_table` authorisation calls this right where
 * the real dispatch would have happened. Names the site, table base, and
 * runtime (m,x,X-reg) so we can tell a genuine unauthorised static-ROM
 * table (worth an `indirect_call_table` cfg line) apart from a table
 * base that lands below $8000 (SNES hardware-register space under this
 * bank's LoROM mapping, per cpu_state.c's address-routing comment) --
 * which would mean the "JSR (abs,X)" itself is a decode artifact (wrong
 * entry m/x desyncing operand bytes into something that merely LOOKS
 * like a real instruction), the same failure class as the $01:B898 fix.
 * Gated + capped so it's opt-in and bounded; NOT for permanent use — it
 * performs a real read at the computed effective address for inspection,
 * which is a new side effect at a site that was previously never
 * executed at all (a concern only if that address is a read-sensitive
 * hardware register). */
void ar_indirect_suppressed_log(CpuState *cpu, uint32 site_pc24,
                                 uint8 bank, uint16 table_base, uint16 x_reg);

/* ── PEI-trampoline dispatch (2026-05-24, narrow detector) ─────────────
 *
 * Codegen emits a trampoline-aware RTS/RTL ONLY for functions flagged
 * by the PEI/PEA/PER detector in emit_function.py. On those functions,
 * when cpu->S != _entry_s at the Return site, _emit_return pops the
 * topmost frame (2 bytes for RTS, 3 for RTL), computes (PB:PC+1), and
 * tail-calls cpu_dispatch_pc(cpu, pc24). The helper binary-searches
 * g_dispatch_table for an entry matching `pc24` and invokes the variant
 * fnptr for the runtime (m,x) flags. If pc24 isn't a known function
 * entry (typically a mid-function PC reached at the bottom of a
 * trampoline chain), the helper returns RECOMP_RETURN_NORMAL and the
 * host C call stack unwinds.
 *
 * The dispatch table is emitted by v2_regen.py at the end of each
 * regen run into `<prefix>_dispatch_v2.c`. Entries are sorted by
 * pc24 for binary search. Each row holds up to 4 fnptrs (one per
 * (m,x) variant). */
typedef struct DispatchEntry {
    uint32 pc24;
    /* Indexed by ((m_flag & 1) << 1) | (x_flag & 1):
     *   0 = M0X0, 1 = M0X1, 2 = M1X0, 3 = M1X1.
     * NULL = variant not emitted; cpu_dispatch_pc returns NORMAL. */
    RecompReturn (*variant[4])(CpuState *);
} DispatchEntry;

extern const DispatchEntry g_dispatch_table[];
extern const unsigned       g_dispatch_table_count;

/* Dispatch on a popped trampoline target.
 * `entry_s_for_miss_restore`: on lookup MISS, cpu->S is set to this
 * value before returning RECOMP_RETURN_NORMAL. This prevents the
 * trampoline pop's bytes-consumed effect from leaking cpu->S drift up
 * to the caller. On lookup HIT, the dispatched function runs and its
 * own cpu->S manipulations are preserved (no restore). The trampoline-
 * emit caller in codegen._emit_return passes its function's _entry_s
 * as the restore value, matching what real hw would leave cpu->S at
 * once the trampoline chain bottomed out at the caller's continuation.
 */
RecompReturn cpu_dispatch_pc(CpuState *cpu, uint32 pc24,
                              uint16 entry_s_for_miss_restore);
RecompReturn cpu_dispatch_pc_from(CpuState *cpu, uint32 pc24,
                                  uint16 entry_s_for_miss_restore,
                                  uint32 source_pc24);

/* Read-only dispatch-table probe (task #7 RTS-decision trace). */
int cpu_dispatch_has_entry(CpuState *cpu, uint32 pc24);

/* Focused OAM-overflow observability recorders (debug_server.c).
 * dbg_rts_trace is emitted by the RTS/RTL lowering; dbg_oam_block_trace
 * is called from cpu_trace_block. Both are PC-range-filtered and frozen
 * by the [oamdrop] g_boundary_frozen tripwire. */
void dbg_rts_trace(CpuState *cpu, uint32 src_pc, uint16 entry_s,
                   uint16 ret_s, uint32 popped_pc, uint8 hrv);
void dbg_oam_block_trace(CpuState *cpu, uint32 pc24);

#ifdef __cplusplus
}
#endif
