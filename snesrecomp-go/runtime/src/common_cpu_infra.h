#pragma once

#include "types.h"

#ifdef _MSC_VER
#pragma warning(disable: 4013 4028 4033 4090 4133 4305 4715 4716)
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

void WatchdogCheck(void);
void WatchdogFrameStart(void);
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
#include <setjmp.h>
extern jmp_buf g_watchdog_jmp;
extern int g_watchdog_tripped;

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
