#pragma once

#include "types.h"

#ifdef _MSC_VER
#pragma warning(disable: 4013 4028 4033 4090 4133 4305)
#endif

typedef struct Snes Snes;
typedef struct Cpu Cpu;

extern Snes *g_snes;
extern Cpu *g_snes_cpu;
extern bool g_fail;

Snes *SnesInit(const uint8 *data, int data_size);
uint8_t *SnesRomPtr(uint32 v);

// Apply the native-mode CPU state the real ROM's reset vector would
// have established (CLC;XCE / REP #$38 / TCD / TCS / SEI). The recomp
// path never executes those opcodes, so RtlReset and SnesInit invoke
// this after snes_reset to pick up where the ROM would be at $8028.
void SnesEnterNativeMode(void);

typedef void CpuInfraInitializeFunc(void);
typedef void RunOneFrameOfGameFunc(void);
/* Return -1 to use the shared $4210/RDNMI behavior, or a byte value to
 * override the read. This keeps game-specific vblank pacing policy in the
 * per-game layer rather than the shared SNES hardware model. */
typedef int RdnmiReadHookFunc(Snes *snes);
/* Return true only for dispatch sites that should use the optional
 * BRA/BRL-follow and continuation-resume recovery below cpu_dispatch_pc_from.
 * The addresses that opt in belong in the per-game project. */
typedef bool DispatchMissRecoveryFunc(uint32 source_pc24, uint32 target_pc24);

/* Default the watchdog OFF for any TU that does not define it (the ROM-free
 * test targets, and any consumer that has not opted in). The game target sets
 * it explicitly from the CMake option. */
#ifndef AR_WATCHDOG
#define AR_WATCHDOG 0
#endif

/* Hang watchdog (AR_WATCHDOG). Bring-up instrumentation: it detects a frame
 * that never finishes (an infinite loop in recompiled code) and escapes it by
 * yielding the game coroutine. Compiled OUT of release builds — the generated
 * code calls WatchdogCheck at every loop header, so when the option is off
 * these become empty inlines and the whole mechanism costs nothing. Build with
 * -DAR_WATCHDOG=ON (default for Debug/RelWithDebInfo) to keep it. */
/* Monotonic loop-header count — an execution-volume proxy used by AR_APUPROF
 * (straight-line loops push nothing, so push counts under-report them). Kept
 * in BOTH configurations: it is a profiling counter, not watchdog logic. */
extern uint64_t g_watchdog_loop_headers;
extern const char *g_last_recomp_func;
extern const char *g_recomp_stack[];
/* True when the watchdog abandoned the current frame. Always defined so the
 * game layer can test it unconditionally; permanently 0 when disabled. */
extern int g_watchdog_tripped;

/* Declared identically in both configurations — the generated bank code also
 * declares WatchdogCheck (via funcs.h), so these must stay non-static. With
 * AR_WATCHDOG=0 the definitions in common_cpu_infra.c shrink to just the
 * profiling counter: no clock read, no trip path, nothing to abandon a frame. */
void WatchdogCheck(void);
void WatchdogFrameStart(void);
void WatchdogFrameEnd(void);
#if AR_WATCHDOG
/* The game layer registers its coroutine yield here; the trip path calls it
 * instead of longjmp (which was UB across stacks / forbidden out of a fiber). */
extern void (*g_watchdog_yield_hook)(void);
#endif
void RecompStackPush(const char *name);
void RecompStackPop(void);
/* Per-frame 65816 entry-S tracking for return-to-ancestor RTS resolution
 * (see common_cpu_infra.c). The emitted function prologue records
 * _entry_s into g_cpu_entry_s[g_recomp_stack_top-1]. */
extern int g_recomp_stack_top;
extern uint16_t g_cpu_entry_s[];
/* Parallel per-frame paired-host-caller (hrv) flag; cpu_resolve_ancestor_skip
 * stops at the nearest hrv=1 frame so a skip can't escape a JSR boundary. */
extern uint8_t g_cpu_entry_hrv[];
int cpu_resolve_ancestor_skip(uint16_t ret_s);
/* Trampoline tail-dispatch request (see common_cpu_infra.c / cpu_dispatch_pc_from). */
extern uint32_t g_tailcall_pc24;
extern uint16_t g_tailcall_miss_s;
extern uint32_t g_tailcall_src24;
void cpu_tailcall_request(uint32_t pc24, uint16_t miss_s, uint32_t src24);
void cpu_tailcall_inherit_return_context(uint16_t entry_s, uint8_t hrv);
int cpu_take_tailcall_return_context(uint16_t *entry_s, uint8_t *hrv);

typedef struct RtlGameInfo {
  const char *title;
  CpuInfraInitializeFunc *initialize;
  RunOneFrameOfGameFunc *run_frame;
  RunOneFrameOfGameFunc *draw_ppu_frame;
  RdnmiReadHookFunc *read_rdnmi;
  DispatchMissRecoveryFunc *recover_dispatch_miss;
  // Filename prefix used by RtlSaveLoad, e.g. "save" produces
  // "saves/save%d.sav". If NULL, framework uses "%s_save" with title.
  const char *save_name_prefix;
} RtlGameInfo;

extern const RtlGameInfo *g_rtl_game_info;

// Called by the game-layer before SnesInit so the framework knows
// which game it's running. Framework itself names no specific game.
void RtlRegisterGame(const RtlGameInfo *info);
