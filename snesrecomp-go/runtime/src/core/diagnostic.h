#ifndef SNESRECOMP_DIAGNOSTIC_H
#define SNESRECOMP_DIAGNOSTIC_H

#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int g_sr_entry_mx_check_enabled;
extern int g_sr_mx_history_enabled;
extern const char *g_sr_trap_function;
extern int g_sr_exit_mx_check_enabled;
extern int g_sr_exit_stack_check_enabled;
extern int g_sr_call_mx_check_enabled;
void sr_entry_mx_fail(CpuState *cpu, int expected_m, int expected_x,
                      const char *function_name, uint32 pc24);
void sr_exit_mx_fail(CpuState *cpu, int expected_m, int expected_x,
                     const char *function_name, uint32 pc24);
void sr_exit_s_fail(CpuState *cpu, uint32 entry_stack, uint32 return_stack,
                    const char *function_name, uint32 pc24);
void sr_call_mx_fail(CpuState *cpu, int expected_m, int expected_x,
                     const char *function_name, uint32 pc24);
void sr_mx_history_record(uint32 pc24, int m, int x);
void sr_mx_history_dump(void);
void sr_entry_trap_function(CpuState *cpu, const char *function_name, uint32 pc24);
void sr_garbage_variant_trap(CpuState *cpu, const char *function_name,
                             uint32 pc24);
RecompReturn sr_dispatch_oob_warn(CpuState *cpu, uint32 site_pc24,
                                  uint16 index);
RecompReturn sr_unresolved_indirect_jump(CpuState *cpu, uint32 site_pc24);
RecompReturn sr_unresolved_stub_warn(CpuState *cpu, uint32 target_pc24,
                                     const char *function_name);
RecompReturn sr_unresolved_goto_warn(CpuState *cpu, uint32 source_pc24,
                                     uint32 target_pc24,
                                     const char *function_name,
                                     const char *target_label);
int sr_block_history_with_aux(uint32 *pc, uint32 *aux, int maximum);
int sr_block_history_with_stack(uint32 *pc, uint32 *aux, uint16 *stack, int maximum);
int sr_x_transition_trace_enabled(void);
void sr_x_transition_trace_record(uint32 block, uint32 next, int new_x, int m,
                      uint32 game_frame);
void sr_x_transition_trace_dump(const char *reason, int count);
int sr_stack_trace_active(void);
void sr_stack_trace_operation(const char *kind, uint16 address, uint8 value, uint16 stack);
void sr_stack_trace_block(uint32 pc24, uint16 stack, int m, int x);
extern uint32 *g_stack_pusher;
extern unsigned *g_stack_pusher_frame;
int sr_stack_provenance_enabled(void);
void sr_vram_trace_raw(uint16 address, uint8 value, int port);
int sr_vram_watch(uint16 address, uint8 value);

/* Deterministic inspection hooks for tests and host diagnostics. */
void sr_diagnostic_reset(void);
uint32 sr_mx_history_count(uint32 pc24, int m, int x);
unsigned sr_x_transition_trace_count(void);
unsigned sr_diagnostic_trap_warning_count(void);

#ifdef __cplusplus
}
#endif

#endif
