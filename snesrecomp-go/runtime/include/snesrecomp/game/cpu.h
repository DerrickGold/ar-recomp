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
} RecompReturn;

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
void dbg_rts_trace(CpuState *cpu, uint32 source_pc, uint16 entry_stack,
                   uint16 return_stack, uint32 popped_pc, uint8 hrv);
void dbg_oam_block_trace(CpuState *cpu, uint32 pc24);

#ifdef __cplusplus
}
#endif

#endif
