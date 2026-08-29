#ifndef SNESRECOMP_GAME_GENERATED_SUPPORT_H
#define SNESRECOMP_GAME_GENERATED_SUPPORT_H

#include "snesrecomp/game/types.h"

#ifdef __cplusplus
extern "C" {
#endif

uint8 *SnesRomPtr(uint32 address);
void SnesEnterNativeMode(void);
uint8 *IndirPtrDB(uint8 direct_page_address, uint16 offset);

#ifndef SNESRECOMP_WATCHDOG
#define SNESRECOMP_WATCHDOG 0
#endif

extern uint64_t g_watchdog_loop_headers;
extern const char *g_last_recomp_func;
extern const char *g_recomp_stack[];
extern unsigned long g_recomp_push_count;
extern int g_watchdog_tripped;
void WatchdogCheck(void);
void WatchdogFrameStart(void);
void WatchdogFrameEnd(void);
#if SNESRECOMP_WATCHDOG
extern void (*g_watchdog_yield_hook)(void);
#endif
void RecompStackPush(const char *name);
void RecompStackPop(void);
void RecompStackDump(void);
extern int g_recomp_stack_top;
extern uint16_t g_cpu_entry_s[];
extern uint8_t g_cpu_entry_hrv[];
int cpu_resolve_ancestor_skip(uint16_t return_stack);
/** Number of valid entries currently retained by the block-history ring.
 * This can exceed the count copied by sr_block_history when the caller's
 * output buffer is shorter than the retained history. */
int sr_block_history_available(void);
int sr_block_history(uint32 *output, int maximum);
extern uint32 g_sr_block_ring[kRuntimeBlockTraceRingCapacity];
extern uint32 g_sr_block_aux[kRuntimeBlockTraceRingCapacity];
extern uint16 g_sr_block_stack[kRuntimeBlockTraceRingCapacity];
extern unsigned g_sr_block_index;

extern uint32_t g_tailcall_pc24;
extern uint16_t g_tailcall_miss_s;
extern uint32_t g_tailcall_src24;
void cpu_tailcall_request(uint32_t pc24, uint16_t miss_stack,
                          uint32_t source_pc24);
void cpu_tailcall_inherit_return_context(uint16_t entry_stack, uint8_t hrv);
int cpu_take_tailcall_return_context(uint16_t *entry_stack, uint8_t *hrv);

#ifdef __cplusplus
}
#endif

#endif
