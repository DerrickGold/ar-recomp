#ifndef SNESRECOMP_NEXT_COMMON_CPU_INFRA_H
#define SNESRECOMP_NEXT_COMMON_CPU_INFRA_H

#include "audio_adapter.h"
#include "types.h"

typedef struct Snes Snes;
typedef struct Cpu Cpu;
typedef struct CpuState CpuState;
typedef struct SrSpcUploadContext SrSpcUploadContext;

extern Snes *g_snes;
extern Cpu *g_snes_cpu;
extern bool g_fail;

Snes *SnesInit(const uint8 *data, int data_size);
void SnesShutdown(void);
uint8 *SnesRomPtr(uint32 address);
void SnesEnterNativeMode(void);
uint8 *IndirPtrDB(uint8 direct_page_address, uint16 offset);

typedef void CpuInfraInitializeFunc(void);
typedef void RunOneFrameOfGameFunc(void);
typedef int RdnmiReadHookFunc(Snes *snes);
typedef bool DispatchMissRecoveryFunc(uint32 source_pc24, uint32 target_pc24);
typedef bool RtlSpcUploadSourceFunc(CpuState *cpu, uint32 *source24);
typedef bool RtlSpcUploadCustomizeFunc(CpuState *cpu,
                                       const SrSpcUploadContext *upload,
                                       uint32 source24);
typedef void RtlSpcUploadCommitFunc(SrSpcUploadContext *upload);
typedef int RtlSpcUploadStackPopFunc(const CpuState *cpu);
typedef void RtlAudioDspWriteRoutingFunc(
    const RtlAudioDspWriteContext *context,
    RtlAudioDspWriteRouting *routing);
typedef void RtlAudioStateLoadedRoutingFunc(
    const RtlAudioStateLoadedContext *context,
    RtlAudioStateLoadedRouting *routing);
typedef bool RtlAudioExtensionDspWriteFunc(
    RtlAudioExtensionContext *context, uint8_t address, uint8_t *value);
typedef void RtlAudioExtensionSpcOpcodeFunc(
    RtlAudioExtensionContext *context, uint16_t opcode_pc);
typedef int RtlAudioExtensionSpcCycleFunc(uint16_t opcode_pc, int cycles);
typedef void RtlAudioExtensionSaveFunc(RtlAudioSaveContext *context);
typedef void RtlAudioExtensionUploadFunc(
    RtlAudioExtensionContext *context, uint32_t source24);
typedef void RunnerAbiBindFunc(Snes *snes, bool enabled);

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

typedef struct RtlGameInfo {
    const char *title;
    CpuInfraInitializeFunc *initialize;
    RunOneFrameOfGameFunc *run_frame;
    RunOneFrameOfGameFunc *draw_ppu_frame;
    RdnmiReadHookFunc *read_rdnmi;
    DispatchMissRecoveryFunc *recover_dispatch_miss;
    const char *save_name_prefix;
    RtlSpcUploadSourceFunc *spc_upload_source;
    RtlSpcUploadCustomizeFunc *spc_upload_customize;
    RtlSpcUploadCommitFunc *spc_upload_commit;
    RtlSpcUploadStackPopFunc *spc_upload_stack_pop;
    RtlAudioDspWriteRoutingFunc *audio_dsp_write_routing;
    RtlAudioStateLoadedRoutingFunc *audio_state_loaded_routing;
    RtlAudioExtensionDspWriteFunc *audio_extension_dsp_write;
    RtlAudioExtensionSpcOpcodeFunc *audio_extension_spc_opcode;
    RtlAudioExtensionSpcCycleFunc *audio_extension_spc_cycle;
    RtlAudioExtensionSaveFunc *audio_extension_save;
    RtlAudioExtensionUploadFunc *audio_extension_upload;
    /* Optional game adapter for authoritative recompiled-CPU state. */
    RunnerAbiBindFunc *bind_runner_abi;
} RtlGameInfo;

extern const RtlGameInfo *g_rtl_game_info;
void RtlRegisterGame(const RtlGameInfo *info);
/* Restart-class configuration. Disabled mode installs no hot audio-extension
 * hooks; enabled callbacks run only at existing DSP/SPC/APU seams. */
void RtlAudioExtensionConfigure(bool enabled);
/* Called only from the runner's SPC-upload safe point while it owns the APU
 * lock. The callback may therefore mutate its borrowed ARAM view directly. */
void RtlAudioExtensionNotifyUploadLocked(uint32 source24);

#endif
