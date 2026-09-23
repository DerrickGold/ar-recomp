#ifndef SNESRECOMP_GAME_CPU_H
#define SNESRECOMP_GAME_CPU_H

#include "snesrecomp/game/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SNESRECOMP_SEMANTIC_DISPATCH_TRACE
#define SNESRECOMP_SEMANTIC_DISPATCH_TRACE 0
#endif

typedef struct CpuState {
    uint16 A;
    uint16 X;
    uint16 Y;
    uint16 S;
    uint16 D;
    uint8 DB;
    uint8 PB;
    uint8 host_return_valid;
    uint8 P;
    uint8 m_flag;
    uint8 x_flag;
    uint8 emulation;
    uint8 _flag_N;
    uint8 _flag_V;
    uint8 _flag_Z;
    uint8 _flag_C;
    uint8 _flag_I;
    uint8 _flag_D;
    uint8 *ram;
} CpuState;

typedef enum RecompReturn {
    RECOMP_RETURN_NORMAL = 0,
    RECOMP_RETURN_SKIP_1 = 1,
    RECOMP_RETURN_SKIP_2 = 2,
    RECOMP_RETURN_SKIP_3 = 3,
    RECOMP_RETURN_TAILCALL = 0x4000,
    RECOMP_RETURN_PARKED_WAIT = 0x4001,
    RECOMP_RETURN_OWNED_UNWIND = 0x4002,
} RecompReturn;

/* Only emitted for a proven WAI + unconditional branch back to that WAI.
 * There is no ordinary continuation out of this parked loop. A scheduler may
 * retire its C activations, retain the complete emulated CPU/stack, and enter
 * a real interrupt. resume_pc is the hardware PC following WAI. */
extern uint32 g_cpu_wait_pc24;
extern uint32 g_cpu_wait_resume_pc24;

/* Generated direct-call ownership, separate from emulated CPU/save state.
 * Records live on the C stack and only cover a synchronous native call.
 * No allocation, registry lookup, or fixed recursion capacity is required.
 * The caller's entry frame is an upper boundary: an adjusted return must
 * not consume it, even if a recursive ancestor has the same return PC. */
typedef struct CpuReturnScope {
    struct CpuReturnScope *previous;
    CpuState *cpu;
    uint32 continuation;
    uint16 entry_stack;
    uint16 caller_stack_limit;
    uint8 frame_bytes;
    uint8 adjusted_return;
    int reset_activation_depth;
} CpuReturnScope;
extern CpuReturnScope *g_cpu_return_scope;
extern CpuReturnScope *g_cpu_owned_unwind_scope;
extern int g_recomp_stack_top;

/* A reset entry owns no hardware return frame and may initialize S. Hosts
 * explicitly bracket reset execution, including any tail-dispatch driver.
 * This is not permission to run arbitrary routines as reset. Nested ISR
 * activations do not inherit its unframed stack boundary. */
static inline void cpu_reset_scope_begin(CpuReturnScope *scope, CpuState *cpu) {
    scope->previous = g_cpu_return_scope;
    scope->cpu = cpu;
    scope->continuation = 0u;
    scope->entry_stack = cpu->S;
    scope->caller_stack_limit = 0x1fffu;
    scope->frame_bytes = 0u;
    scope->adjusted_return = 0u;
    scope->reset_activation_depth = g_recomp_stack_top + 1;
    g_cpu_return_scope = scope;
}

static inline void cpu_return_scope_begin(CpuReturnScope *scope, CpuState *cpu,
        uint32 continuation, uint16 caller_stack_limit, uint8 frame_bytes) {
    scope->previous = g_cpu_return_scope;
    scope->cpu = cpu;
    scope->continuation = continuation & 0xffffffu;
    scope->entry_stack = cpu->S; /* after pushing the hardware call frame */
    if (scope->previous != NULL && scope->previous->cpu == cpu &&
        scope->previous->frame_bytes == 0u &&
        scope->previous->reset_activation_depth == g_recomp_stack_top)
        caller_stack_limit = scope->previous->caller_stack_limit;
    /* A caller that already moved S above its own entry (for example a
     * TXS stack reset) owns its actual pre-call stack, not the stale entry. */
    if ((uint32)cpu->S + frame_bytes <= 0xffffu &&
        (uint16)(cpu->S + frame_bytes) > caller_stack_limit)
        caller_stack_limit = (uint16)(cpu->S + frame_bytes);
    scope->caller_stack_limit = caller_stack_limit;
    scope->frame_bytes = frame_bytes;
    scope->adjusted_return = 0u;
    scope->reset_activation_depth = 0;
    g_cpu_return_scope = scope;
}
static inline void cpu_return_scope_end(CpuReturnScope *scope) {
    /* A terminal/reset boundary must not resurrect an invalidated chain. */
    if (g_cpu_return_scope == scope) g_cpu_return_scope = scope->previous;
    if (g_cpu_owned_unwind_scope == scope) g_cpu_owned_unwind_scope = NULL;
}

/* An exact native ancestor frame can finish after inner call frames were
 * deliberately discarded. No new continuation body or host-depth SKIP count
 * is needed. The matching C call consumes the token; intervening calls/tails
 * propagate it without restoring S/PB. Ordinary immediate returns are unchanged. */
int cpu_begin_owned_unwind(CpuState *cpu, uint16 return_stack,
                           uint32 target, uint8 frame_bytes);
int cpu_finish_owned_unwind(CpuReturnScope *scope, CpuState *cpu);

/* A native callee may remove its hardware frame and jump to its saved return
 * PC instead of executing RTS/RTL. Only the immediate active call, its exact
 * continuation and its exact post-pop S can resume the existing C activation.
 * Never search ancestors or treat an arbitrary registry function as a resume. */
static inline int cpu_accept_indirect_return(CpuState *cpu,
        uint16 entry_stack, uint32 target) {
    const CpuReturnScope *scope = g_cpu_return_scope;
    return cpu != NULL && !cpu->emulation && scope != NULL &&
        scope->cpu == cpu && scope->entry_stack == entry_stack &&
        (scope->frame_bytes == 2u || scope->frame_bytes == 3u) &&
        scope->continuation == (target & 0xffffffu) &&
        (uint32)entry_stack + scope->frame_bytes == cpu->S &&
        cpu->S <= scope->caller_stack_limit &&
        scope->caller_stack_limit <= 0x1fffu;
}
int cpu_accept_adjusted_return(CpuState *cpu, uint16 entry_stack,
                              uint16 return_stack, uint32 target,
                              uint8 frame_bytes);
int cpu_accept_stacked_result_return(CpuState *cpu, uint16 entry_stack,
                                     uint16 return_stack, uint32 target,
                                     uint8 frame_bytes);

/* Generated, single-basic-block return-word relocation contract. Capture
 * immediately after a native two-byte pull from this call's incoming frame.
 * The emitter must preserve that register through the matching push and RTS,
 * with every intervening store proved disjoint ordinary WRAM. This evidence
 * is stronger than matching a PC at an arbitrary (possibly ancestor) S. */
CpuReturnScope *cpu_capture_return_word(CpuState *cpu, uint16 entry_stack,
                                      uint16 source_stack, uint16 word,
                                      uint8 width);
int cpu_return_word_store_disjoint(const CpuState *cpu, uint8 bank,
                                   uint16 address, uint8 width);
int cpu_accept_return_word_relocation(CpuState *cpu, uint16 entry_stack,
                                     uint16 return_stack, uint32 target,
                                     const CpuReturnScope *origin);

static inline uint8 cpu_read_b(const CpuState *cpu) {
    return (uint8)(cpu->A >> 8);
}
static inline uint8 cpu_read_a8(const CpuState *cpu) { return (uint8)cpu->A; }
static inline uint16 cpu_read_a16(const CpuState *cpu) { return cpu->A; }
static inline uint16 cpu_read_a_m(const CpuState *cpu) {
    return cpu->m_flag ? (uint8)cpu->A : cpu->A;
}
static inline void cpu_write_a8(CpuState *cpu, uint8 value) {
    cpu->A = (uint16)((cpu->A & 0xff00u) | value);
}
static inline void cpu_write_a16(CpuState *cpu, uint16 value) { cpu->A = value; }
static inline void cpu_write_a_m(CpuState *cpu, uint16 value) {
    if (cpu->m_flag) cpu_write_a8(cpu, (uint8)value);
    else cpu_write_a16(cpu, value);
}
static inline uint8 cpu_read_x8(const CpuState *cpu) { return (uint8)cpu->X; }
static inline uint16 cpu_read_x16(const CpuState *cpu) { return cpu->X; }
static inline uint16 cpu_read_x_x(const CpuState *cpu) {
    return cpu->x_flag ? (uint8)cpu->X : cpu->X;
}
static inline void cpu_write_x8(CpuState *cpu, uint8 value) { cpu->X = value; }
static inline void cpu_write_x16(CpuState *cpu, uint16 value) { cpu->X = value; }
static inline void cpu_write_x_x(CpuState *cpu, uint16 value) {
    if (cpu->x_flag) cpu_write_x8(cpu, (uint8)value);
    else cpu_write_x16(cpu, value);
}
static inline uint8 cpu_read_y8(const CpuState *cpu) { return (uint8)cpu->Y; }
static inline uint16 cpu_read_y16(const CpuState *cpu) { return cpu->Y; }
static inline uint16 cpu_read_y_x(const CpuState *cpu) {
    return cpu->x_flag ? (uint8)cpu->Y : cpu->Y;
}
static inline void cpu_write_y8(CpuState *cpu, uint8 value) { cpu->Y = value; }
static inline void cpu_write_y16(CpuState *cpu, uint16 value) { cpu->Y = value; }
static inline void cpu_write_y_x(CpuState *cpu, uint16 value) {
    if (cpu->x_flag) cpu_write_y8(cpu, (uint8)value);
    else cpu_write_y16(cpu, value);
}

#define CPU_P_C 0x01u
#define CPU_P_Z 0x02u
#define CPU_P_I 0x04u
#define CPU_P_D 0x08u
#define CPU_P_X 0x10u
#define CPU_P_M 0x20u
#define CPU_P_V 0x40u
#define CPU_P_N 0x80u

static inline void cpu_p_to_mirrors(CpuState *cpu) {
    cpu->m_flag = (cpu->P & CPU_P_M) != 0u;
    cpu->x_flag = (cpu->P & CPU_P_X) != 0u;
    cpu->_flag_C = (cpu->P & CPU_P_C) != 0u;
    cpu->_flag_Z = (cpu->P & CPU_P_Z) != 0u;
    cpu->_flag_I = (cpu->P & CPU_P_I) != 0u;
    cpu->_flag_D = (cpu->P & CPU_P_D) != 0u;
    cpu->_flag_V = (cpu->P & CPU_P_V) != 0u;
    cpu->_flag_N = (cpu->P & CPU_P_N) != 0u;
    if (cpu->x_flag) {
        cpu->X &= 0x00ffu;
        cpu->Y &= 0x00ffu;
    }
}

static inline void cpu_mirrors_to_p(CpuState *cpu) {
    cpu->P = (uint8)((cpu->_flag_C ? CPU_P_C : 0u) |
                     (cpu->_flag_Z ? CPU_P_Z : 0u) |
                     (cpu->_flag_I ? CPU_P_I : 0u) |
                     (cpu->_flag_D ? CPU_P_D : 0u) |
                     (cpu->x_flag ? CPU_P_X : 0u) |
                     (cpu->m_flag ? CPU_P_M : 0u) |
                     (cpu->_flag_V ? CPU_P_V : 0u) |
                     (cpu->_flag_N ? CPU_P_N : 0u));
}

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address);
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address);
/* Indirect control-flow pointer fetches stay in their pointer bank, unlike
 * ordinary data words which may carry into the next bank. Keep read order
 * explicit for bank-zero pointers that overlap side-effecting registers. */
static inline uint16 cpu_read16_bank_wrap(CpuState *cpu, uint8 bank, uint16 address) {
    uint8 low = cpu_read8(cpu, bank, address);
    uint8 high = cpu_read8(cpu, bank, (uint16)(address + 1u));
    return (uint16)((uint16)low | ((uint16)high << 8));
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value);
void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value);

static inline uint16 cpu_read16_dp(CpuState *cpu, uint16 address) {
    return (uint16)cpu_read8(cpu, 0u, address) |
           ((uint16)cpu_read8(cpu, 0u, (uint16)(address + 1u)) << 8);
}

extern void (*g_cpu_brk_hook)(CpuState *cpu);
extern void (*g_cpu_cop_hook)(CpuState *cpu);

static inline void cpu_push_interrupt_frame(CpuState *cpu) {
    cpu_mirrors_to_p(cpu);
    if (!cpu->emulation) {
        cpu_write8(cpu, 0u, cpu->S, cpu->PB);
        --cpu->S;
    }
    cpu_write8(cpu, 0u, cpu->S, 0u);
    --cpu->S;
    cpu_write8(cpu, 0u, cpu->S, 0u);
    --cpu->S;
    cpu_write8(cpu, 0u, cpu->S, cpu->P);
    --cpu->S;
}

static inline void cpu_push_jsr_return_frame(CpuState *cpu) {
    cpu_write8(cpu, 0u, cpu->S, 0u);
    --cpu->S;
    cpu_write8(cpu, 0u, cpu->S, 0u);
    --cpu->S;
    cpu->host_return_valid = 1u;
}

static inline void cpu_push_jsl_return_frame(CpuState *cpu) {
    cpu_write8(cpu, 0u, cpu->S, 0xffu);
    --cpu->S;
    cpu_write8(cpu, 0u, cpu->S, 0xffu);
    --cpu->S;
    cpu_write8(cpu, 0u, cpu->S, 0xffu);
    --cpu->S;
    cpu->host_return_valid = 1u;
}

void cpu_state_init(CpuState *cpu, uint8 *ram);
extern CpuState g_cpu;
uint16 sr_cpu_stack_pointer(void);
uint8 sr_cpu_program_bank(void);
void cpu_dbg_funcname(const char *name);

extern int g_sr_entry_mx_check_enabled;
extern int g_sr_mx_history_enabled;
extern const char *g_sr_trap_function;
extern int g_sr_exit_mx_check_enabled;
extern int g_sr_exit_stack_check_enabled;
extern int g_sr_call_mx_check_enabled;
void sr_entry_mx_fail(CpuState *cpu, int expected_m, int expected_x,
                      const char *function_name, uint32 pc24);
void sr_mx_history_record(uint32 pc24, int m, int x);
void sr_mx_history_dump(void);
void sr_entry_trap_function(CpuState *cpu, const char *function_name, uint32 pc24);
void sr_garbage_variant_trap(CpuState *cpu, const char *function_name,
                             uint32 pc24);
void sr_exit_mx_fail(CpuState *cpu, int expected_m, int expected_x,
                     const char *function_name, uint32 pc24);
void sr_exit_s_fail(CpuState *cpu, uint32 entry_stack, uint32 return_stack,
                    const char *function_name, uint32 pc24);
void sr_call_mx_fail(CpuState *cpu, int expected_m, int expected_x,
                     const char *function_name, uint32 pc24);
int sr_trace_active(void);
void sr_trace_func(uint32 pc24, const char *name, int m, int x,
                   int expected_m, int expected_x);
void sr_trace_call(uint32 pc24, const char *name, int m, int x,
                   int expected_m, int expected_x);

#ifndef SNESRECOMP_TRACE_RECORDER
#define SNESRECOMP_TRACE_RECORDER 0
#endif

static inline void sr_entry_mx_check(CpuState *cpu, int expected_m,
                                     int expected_x, const char *name,
                                     uint32 pc24) {
    if (g_sr_entry_mx_check_enabled && ((cpu->m_flag & 1) != expected_m ||
                          (cpu->x_flag & 1) != expected_x)) {
        sr_entry_mx_fail(cpu, expected_m, expected_x, name, pc24);
    }
    if (g_sr_mx_history_enabled) sr_mx_history_record(pc24, cpu->m_flag & 1, cpu->x_flag & 1);
    if (g_sr_trap_function != NULL) sr_entry_trap_function(cpu, name, pc24);
#if SNESRECOMP_TRACE_RECORDER
    if (sr_trace_active()) {
        sr_trace_func(pc24, name, cpu->m_flag & 1, cpu->x_flag & 1,
                      expected_m, expected_x);
    }
#endif
}

static inline void sr_exit_mx_check(CpuState *cpu, int expected_m,
                                    int expected_x, const char *name,
                                    uint32 pc24) {
    if (g_sr_exit_mx_check_enabled && ((cpu->m_flag & 1) != expected_m ||
                               (cpu->x_flag & 1) != expected_x)) {
        sr_exit_mx_fail(cpu, expected_m, expected_x, name, pc24);
    }
}

static inline void sr_exit_s_check(CpuState *cpu, uint16 entry_stack,
                                   uint16 return_stack, const char *name,
                                   uint32 pc24) {
    if (g_sr_exit_stack_check_enabled && entry_stack != return_stack) {
        sr_exit_s_fail(cpu, entry_stack, return_stack, name, pc24);
    }
}

static inline void sr_call_mx_check(CpuState *cpu, int expected_m,
                                    int expected_x, const char *name,
                                    uint32 pc24) {
    if (g_sr_call_mx_check_enabled && ((cpu->m_flag & 1) != expected_m ||
                               (cpu->x_flag & 1) != expected_x)) {
        sr_call_mx_fail(cpu, expected_m, expected_x, name, pc24);
    }
#if SNESRECOMP_TRACE_RECORDER
    if (sr_trace_active()) {
        sr_trace_call(pc24, name, cpu->m_flag & 1, cpu->x_flag & 1,
                      expected_m, expected_x);
    }
#endif
}

void sr_indirect_suppressed_log(CpuState *cpu, uint32 site_pc24,
                                uint8 bank, uint16 table_base,
                                uint16 x_register);

typedef struct DispatchEntry {
    uint32 pc24;
    RecompReturn (*variant[4])(CpuState *cpu);
} DispatchEntry;

extern const DispatchEntry g_dispatch_table[];
extern const unsigned g_dispatch_table_count;
RecompReturn cpu_dispatch_pc(CpuState *cpu, uint32 pc24,
                             uint16 miss_restore_stack);
RecompReturn cpu_dispatch_pc_from(CpuState *cpu, uint32 pc24,
                                  uint16 miss_restore_stack,
                                  uint32 source_pc24);
/** Tail to a split body after popping the current generated activation.
 * Retains its paired caller context, and reuses a driver only at the same
 * CPU, hardware entry stack and generated activation depth. Ordinary nested
 * JSR/JSL calls must keep their own driver/return boundary. */
RecompReturn cpu_dispatch_paired_tail_from(CpuState *cpu, uint32 pc24,
        uint16 entry_stack, uint8 hrv, uint32 source_pc24);
/** Queue a branch-only HLE continuation from inside its generated wrapper.
 * Inherits that activation's original entry stack/return ownership, which can
 * differ from current S after a pushed-target dispatch. Does not pop a frame
 * or run a nested driver. On success the HLE must immediately return
 * RECOMP_RETURN_TAILCALL so its wrapper can retire. A paired wrapper drives
 * that branch to its native return before resuming its host caller; an
 * unpaired wrapper leaves it to the outer dispatch loop. Returns zero, unchanged,
 * outside an active generated wrapper. Not for JSR/JSL calls or raw HLE bodies
 * lacking a generated prologue. */
int cpu_hle_tailcall_request(uint32 pc24, uint32 source_pc24);
/* Synchronous, balanced RTS leaf call from a game-owned HLE controller.
 * Creates a paired two-byte call frame and return scope; the continuation is
 * an ownership/diagnostic label, not a synthesized instruction or resume PC.
 * The audited leaf must not yield, retain its return word, change banks, or
 * escape to another activation. Native mode / low WRAM stack only. Invalid
 * arguments fail before mutation. A callee contract failure returns zero
 * without repairing CPU/stack state: the caller MUST abandon execution, never
 * retry or continue the controller. Does not consume the HLE's caller frame. */
int cpu_invoke_rts_leaf(CpuState *cpu, RecompReturn (*leaf)(CpuState *),
                        uint32 continuation_pc24);
/* Generated-wrapper epilogue only, after its activation pop. Drives an owned
 * paired HLE branch with the standard flat-tail driver; escaped child returns
 * and unpaired transfers propagate. Game HLEs return tokens, never call this. */
RecompReturn cpu_finish_hle_return(CpuState *cpu, RecompReturn result,
                                 uint16 entry_stack, uint8 hrv);
/* Suspend an intact compiled activation on this execution thread while the
 * host schedules another tick. The callback must resume at this exact call;
 * no frame-pacing longjmp, machine replacement, or reset-and-resume. Nested
 * checkpoint/poll yields are supported. Native registers/RAM are not copied
 * or restored. Terminal abandonment must use the normal shutdown/reset path.
 * Protects return scopes and flat-tail ownership across WatchdogFrameStart;
 * games without stack-preserving schedulers need not call this helper. */
void cpu_yield_execution(void (*yield_to_host)(void *), void *context);

void cpu_poll_wait(CpuState *cpu, uint32 resume_pc24,
                   uint32 read_address24, uint32 read_width_bytes);
int cpu_dispatch_has_entry(CpuState *cpu, uint32 pc24);
/* Bounded bring-up diagnostics for computed targets that have no live M/X
 * registry body. RTS/RTL continuation sources are deliberately excluded. */
void cpu_dispatch_diagnostic_reset(void);
unsigned cpu_dispatch_missing_warning_count(void);
uint32 cpu_dispatch_missing_warning_hits(uint32 site_pc24,
                                         uint32 target_pc24);
/* Generated direct dispatches call this only in semantic-trace builds. The
 * implementation shares the generic registry dispatch event constructor so
 * validation observes an edge independently of its lowering strategy. */
void cpu_trace_resolved_dispatch(CpuState *cpu, uint32 pc24,
                                 uint32 source_pc24);
/* Records the computed target of an unresolved indirect edge without
 * executing it. Generated code calls this immediately before its hard trap. */
void cpu_trace_trapped_dispatch(CpuState *cpu, uint32 pc24,
                                uint32 source_pc24);
/* Trace-only evidence for a missing software-pushed RTS/RTL target, after
 * generated code has proved the active caller frame remains below it.
 * Does not execute the target or classify arbitrary returns as handlers. */
void cpu_trace_missing_pushed_target(CpuState *cpu, uint32 pc24,
                                     uint32 source_pc24);
void dbg_rts_trace(CpuState *cpu, uint32 source_pc, uint16 entry_stack,
                   uint16 return_stack, uint32 popped_pc, uint8 hrv);
void dbg_oam_block_trace(CpuState *cpu, uint32 pc24);

#ifdef __cplusplus
}
#endif

#endif
