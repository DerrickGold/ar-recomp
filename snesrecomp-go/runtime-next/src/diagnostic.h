#ifndef SNESRECOMP_NEXT_DIAGNOSTIC_H
#define SNESRECOMP_NEXT_DIAGNOSTIC_H

#include "cpu_state.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int g_ar_mx_check;
extern int g_ar_mxhist;
extern const char *g_ar_trapfn;
extern int g_ar_exit_mx_check;
extern int g_ar_exit_s_check;
extern int g_ar_call_mx_check;
void ar_entry_mx_fail(CpuState *cpu, int expected_m, int expected_x,
                      const char *function_name, uint32 pc24);
void ar_exit_mx_fail(CpuState *cpu, int expected_m, int expected_x,
                     const char *function_name, uint32 pc24);
void ar_exit_s_fail(CpuState *cpu, uint32 entry_stack, uint32 return_stack,
                    const char *function_name, uint32 pc24);
void ar_call_mx_fail(CpuState *cpu, int expected_m, int expected_x,
                     const char *function_name, uint32 pc24);
void ar_mxhist_record(uint32 pc24, int m, int x);
void ar_mxhist_dump(void);
void ar_entry_trapfn(CpuState *cpu, const char *function_name, uint32 pc24);
void ar_garbage_variant_trap(CpuState *cpu, const char *function_name,
                             uint32 pc24);
RecompReturn ar_dispatch_oob_warn(CpuState *cpu, uint32 site_pc24,
                                  uint16 index);
int ar_block_history2(uint32 *pc, uint32 *aux, int maximum);
int ar_block_history3(uint32 *pc, uint32 *aux, uint16 *stack, int maximum);
int ar_xtrace_enabled(void);
void ar_xtrace_record(uint32 block, uint32 next, int new_x, int m,
                      uint32 game_frame);
void ar_xtrace_dump(const char *reason, int count);
int ar_strace_active(void);
void ar_strace_op(const char *kind, uint16 address, uint8 value, uint16 stack);
void ar_strace_block(uint32 pc24, uint16 stack, int m, int x);
extern uint32 *g_stack_pusher;
extern unsigned *g_stack_pusher_frame;
int ar_stackprov_enabled(void);
void ar_vramraw(uint16 address, uint8 value, int port);
int ar_vramwatch(uint16 address, uint8 value);

/* Deterministic inspection hooks for tests and host diagnostics. */
void ar_diagnostic_reset(void);
uint32 ar_mxhist_count(uint32 pc24, int m, int x);
unsigned ar_xtrace_count(void);

#ifdef __cplusplus
}
#endif

#endif
