#ifndef SNESRECOMP_GAME_TRACE_H
#define SNESRECOMP_GAME_TRACE_H

#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SNESRECOMP_TRACE
#define SNESRECOMP_TRACE 0
#endif

enum {
    CPU_TR_BLOCK = 0,
    CPU_TR_PHB = 1,
    CPU_TR_PLB = 2,
    CPU_TR_PHK = 3,
    CPU_TR_PLP = 4,
    CPU_TR_PHP = 5,
    CPU_TR_RTI = 6,
    CPU_TR_JSL = 7,
    CPU_TR_RTL = 8,
    CPU_TR_MVN = 9,
    CPU_TR_MVP = 10,
    CPU_TR_DB_WRITE = 11,
    CPU_TR_PB_WRITE = 12,
    CPU_TR_FUNC_ENTRY = 13,
    CPU_TR_WRAM_WRITE = 14,
    CPU_TR_NLR_DETECT = 15,
    CPU_TR_NLR_PROPAGATE = 16,
    CPU_TR_NLR_CONSUMED = 17,
    CPU_TR_STACK_OP = 18,
};

typedef struct SrCpuTraceRecord {
    uint64 sequence;
    uint32 pc24;
    int32 frame;
    uint16 A, X, Y, S, D;
    uint16 extra1;
    uint8 DB, PB, P;
    uint8 m, x;
    uint8 event_type;
    uint8 extra0;
} SrCpuTraceRecord;

void sr_cpu_trace_reset(void);
uint64 sr_cpu_trace_count(void);
int sr_cpu_trace_copy(SrCpuTraceRecord *output, int maximum);

enum {
    CPU_STACK_OP_PHA = 1,
    CPU_STACK_OP_PHX = 2,
    CPU_STACK_OP_PHY = 3,
    CPU_STACK_OP_PHP = 4,
    CPU_STACK_OP_PHB = 5,
    CPU_STACK_OP_PHD = 6,
    CPU_STACK_OP_PHK = 7,
    CPU_STACK_OP_PEA = 8,
    CPU_STACK_OP_PEI = 9,
    CPU_STACK_OP_PER = 10,
    CPU_STACK_OP_PLA = 11,
    CPU_STACK_OP_PLX = 12,
    CPU_STACK_OP_PLY = 13,
    CPU_STACK_OP_PLP = 14,
    CPU_STACK_OP_PLB = 15,
    CPU_STACK_OP_PLD = 16,
    CPU_STACK_OP_RTS = 17,
    CPU_STACK_OP_RTL = 18,
    CPU_STACK_OP_RTI = 19,
};

enum {
    BD_EXIT_KIND_NORMAL = 0,
    BD_EXIT_KIND_NLR_PRIMARY = 1,
    BD_EXIT_KIND_SKIP_PROPAGATION = 2,
    BD_EXIT_KIND_TRAMPOLINE = 3,
};

/* The production block hook remains out of line: it maintains the compact
 * execution-history ring used by watchdog diagnostics without enabling the
 * heavyweight trace runtime. */
void cpu_trace_block(CpuState *cpu, uint32 pc24);

/* Recompiler traps report once per site (or unresolved target for shared stub
 * bodies) in EVERY build. These used to be
 * silent no-ops unless SNESRECOMP_TRACE was on, which meant a trap firing
 * during ordinary bring-up produced nothing and the failure only showed up
 * later as corruption or a hang. Each message names the cfg directive that
 * resolves it. */
RecompReturn sr_unresolved_indirect_jump(CpuState *cpu, uint32 site_pc24);
RecompReturn sr_unresolved_stub_warn(CpuState *cpu, uint32 target_pc24,
                                     const char *function_name);
RecompReturn sr_unresolved_goto_warn(CpuState *cpu, uint32 source_pc24,
                                     uint32 target_pc24,
                                     const char *function_name,
                                     const char *target_label);
RecompReturn sr_dispatch_oob_warn(CpuState *cpu, uint32 site_pc24,
                                  uint16 index);

#if SNESRECOMP_TRACE
uint64 cpu_trace_init(void);
uint64 boundary_audit_init(void);
void boundary_audit_record_entry(const char *name);
void boundary_audit_record_exit(const char *name);
void cpu_trace_func_entry(CpuState *cpu, uint32 pc24, const char *name);
void cpu_trace_event(CpuState *cpu, uint32 pc24, uint8 event_type,
                     uint8 extra0, uint16 extra1);
void cpu_trace_db_change(CpuState *cpu, uint32 pc24, uint8 old_db,
                         uint8 new_db, uint8 event_type);
void cpu_trace_pb_change(CpuState *cpu, uint32 pc24, uint8 old_pb,
                         uint8 new_pb, uint8 event_type);
void cpu_trace_px_record(CpuState *cpu, uint32 pc24, uint8 kind,
                         uint8 old_p, uint8 new_p);
void cpu_trace_stack_op(CpuState *cpu, uint32 pc24, uint8 operation,
                        uint16 old_stack, int8 delta);
void cpu_trace_mark_nlr_exit(uint8 kind);
void cpu_trace_arm_stack_drift_tripwire(int32 frame_min);
void cpu_trace_offrails(const char *tag, uint32 hint);
int cpu_trace_offrails_count(void);
RecompReturn cpu_trace_unresolved_goto_trap(
    CpuState *cpu, uint32 source_pc24, uint32 target_pc24,
    const char *function_name, const char *target_label);
RecompReturn cpu_trace_unresolved_stub_trap(
    CpuState *cpu, uint32 target_pc24, const char *function_name);
RecompReturn cpu_trace_dispatch_oob(CpuState *cpu, uint32 site_pc24,
                                    uint16 index);
RecompReturn cpu_trace_unresolved_indirect_jump(
    CpuState *cpu, uint32 site_pc24);
#else
static inline uint64 cpu_trace_init(void) { return 0u; }
static inline uint64 boundary_audit_init(void) { return 0u; }
static inline void boundary_audit_record_entry(const char *name) {
    (void)name;
}
static inline void boundary_audit_record_exit(const char *name) {
    (void)name;
}
static inline void cpu_trace_func_entry(CpuState *cpu, uint32 pc24,
                                        const char *name) {
    (void)cpu; (void)pc24; (void)name;
}
static inline void cpu_trace_event(CpuState *cpu, uint32 pc24,
                                   uint8 event_type, uint8 extra0,
                                   uint16 extra1) {
    (void)cpu; (void)pc24; (void)event_type; (void)extra0; (void)extra1;
}
static inline void cpu_trace_db_change(CpuState *cpu, uint32 pc24,
                                       uint8 old_db, uint8 new_db,
                                       uint8 event_type) {
    (void)cpu; (void)pc24; (void)old_db; (void)new_db; (void)event_type;
}
static inline void cpu_trace_pb_change(CpuState *cpu, uint32 pc24,
                                       uint8 old_pb, uint8 new_pb,
                                       uint8 event_type) {
    (void)cpu; (void)pc24; (void)old_pb; (void)new_pb; (void)event_type;
}
static inline void cpu_trace_px_record(CpuState *cpu, uint32 pc24,
                                       uint8 kind, uint8 old_p,
                                       uint8 new_p) {
    (void)cpu; (void)pc24; (void)kind; (void)old_p; (void)new_p;
}
static inline void cpu_trace_stack_op(CpuState *cpu, uint32 pc24,
                                      uint8 operation, uint16 old_stack,
                                      int8 delta) {
    (void)cpu; (void)pc24; (void)operation; (void)old_stack; (void)delta;
}
static inline void cpu_trace_mark_nlr_exit(uint8 kind) { (void)kind; }
static inline void cpu_trace_arm_stack_drift_tripwire(int32 frame_min) {
    (void)frame_min;
}
static inline void cpu_trace_offrails(const char *tag, uint32 hint) {
    (void)tag; (void)hint;
}
static inline int cpu_trace_offrails_count(void) { return 0; }
static inline RecompReturn cpu_trace_unresolved_indirect_jump(
    CpuState *cpu, uint32 site_pc24) {
    return sr_unresolved_indirect_jump(cpu, site_pc24);
}
static inline RecompReturn cpu_trace_unresolved_goto_trap(
    CpuState *cpu, uint32 source_pc24, uint32 target_pc24,
    const char *function_name, const char *target_label) {
    return sr_unresolved_goto_warn(cpu, source_pc24, target_pc24,
                                   function_name, target_label);
}
static inline RecompReturn cpu_trace_unresolved_stub_trap(
    CpuState *cpu, uint32 target_pc24, const char *function_name) {
    return sr_unresolved_stub_warn(cpu, target_pc24, function_name);
}
static inline RecompReturn cpu_trace_dispatch_oob(CpuState *cpu,
                                                   uint32 site_pc24,
                                                   uint16 index) {
    return sr_dispatch_oob_warn(cpu, site_pc24, index);
}
#endif

#ifdef __cplusplus
}
#endif

#endif
