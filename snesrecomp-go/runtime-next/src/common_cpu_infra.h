#ifndef SNESRECOMP_NEXT_COMMON_CPU_INFRA_H
#define SNESRECOMP_NEXT_COMMON_CPU_INFRA_H

#include "runner_game_module.h"
#include "types.h"

typedef struct Snes Snes;
typedef struct Cpu Cpu;
extern Snes *g_snes;
extern Cpu *g_snes_cpu;
extern bool g_fail;

Snes *SnesInit(const uint8 *data, int data_size);
void SnesShutdown(void);
uint8 *SnesRomPtr(uint32 address);
void SnesEnterNativeMode(void);
uint8 *IndirPtrDB(uint8 direct_page_address, uint16 offset);

#ifndef AR_WATCHDOG
#define AR_WATCHDOG 0
#endif

extern uint64_t g_watchdog_loop_headers;
extern const char *g_last_recomp_func;
extern const char *g_recomp_stack[];
extern unsigned long g_recomp_push_count;
extern int g_watchdog_tripped;
void WatchdogCheck(void);
void WatchdogFrameStart(void);
void WatchdogFrameEnd(void);
#if AR_WATCHDOG
extern void (*g_watchdog_yield_hook)(void);
#endif
void RecompStackPush(const char *name);
void RecompStackPop(void);
void RecompStackDump(void);
extern int g_recomp_stack_top;
extern uint16_t g_cpu_entry_s[];
extern uint8_t g_cpu_entry_hrv[];
int cpu_resolve_ancestor_skip(uint16_t return_stack);
int ar_block_history(uint32 *output, int maximum);
extern uint32 g_ar_blk_ring[kRuntimeBlockTraceRingCapacity];
extern uint32 g_ar_blk_aux[kRuntimeBlockTraceRingCapacity];
extern uint16 g_ar_blk_s[kRuntimeBlockTraceRingCapacity];
extern unsigned g_ar_blk_idx;

extern uint32_t g_tailcall_pc24;
extern uint16_t g_tailcall_miss_s;
extern uint32_t g_tailcall_src24;
void cpu_tailcall_request(uint32_t pc24, uint16_t miss_stack,
                          uint32_t source_pc24);
void cpu_tailcall_inherit_return_context(uint16_t entry_stack, uint8_t hrv);
int cpu_take_tailcall_return_context(uint16_t *entry_stack, uint8_t *hrv);

/* Registration is atomic and accepted only while no runner is active. It
 * returns UNSUPPORTED for unknown ABI versions/capabilities, BUSY while a
 * runner exists, and INVALID_ARGUMENT for malformed descriptors. */
SrResult RtlRegisterGame(const RtlGameModule *module);
const char *RtlGameIdentifier(void);
bool RtlGameDrawPpuFrame(void);
/* Restart-class configuration. Disabled mode installs no hot audio-extension
 * hooks; enabled callbacks run only at existing DSP/SPC/APU seams. */
void RtlAudioExtensionConfigure(bool enabled);
/* Called only from the runner's SPC-upload safe point while it owns the APU
 * lock. The callback may therefore mutate its borrowed ARAM view directly. */
void RtlAudioExtensionNotifyUploadLocked(uint32 source24);

#endif
