/**
 * @file game.h
 * @brief Versioned module implemented by a linked recompiled game.
 * @ingroup sr_game
 */
#ifndef SNESRECOMP_GAME_H
#define SNESRECOMP_GAME_H

#include "snesrecomp/game_audio.h"
#include "snesrecomp/game_runtime.h"
#include "snesrecomp/runner.h"
#include "snesrecomp/spc_upload.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup sr_game
 *  @{
 */

typedef struct CpuState CpuState;

#define RTL_GAME_MODULE_ABI_VERSION 2u

#define RTL_GAME_MODULE_CAP_IDENTITY UINT64_C(0x0000000000000001)
#define RTL_GAME_MODULE_CAP_EXECUTION UINT64_C(0x0000000000000002)
#define RTL_GAME_MODULE_CAP_LIFECYCLE UINT64_C(0x0000000000000004)
#define RTL_GAME_MODULE_CAP_STATE_PROVIDERS UINT64_C(0x0000000000000008)
#define RTL_GAME_MODULE_CAP_AUDIO UINT64_C(0x0000000000000010)
#define RTL_GAME_MODULE_CAP_SUPPORTED                                   \
    (RTL_GAME_MODULE_CAP_IDENTITY | RTL_GAME_MODULE_CAP_EXECUTION |      \
     RTL_GAME_MODULE_CAP_LIFECYCLE |                                    \
     RTL_GAME_MODULE_CAP_STATE_PROVIDERS | RTL_GAME_MODULE_CAP_AUDIO)
#define RTL_GAME_MODULE_CAP_REQUIRED                                    \
    (RTL_GAME_MODULE_CAP_IDENTITY | RTL_GAME_MODULE_CAP_EXECUTION)

/* Stable game identity. game_id is the machine-readable identifier used by
 * diagnostics and legacy SRAM migration. display_name may be localized by the
 * host and is therefore optional. All strings remain module-owned for the
 * registered module's lifetime. */
typedef struct RtlGameIdentity {
    uint32_t struct_size;
    uint32_t flags;
    const char *game_id;
    const char *display_name;
    const char *save_name_prefix;
} RtlGameIdentity;

#define RTL_GAME_IDENTITY_V1_SIZE                                      \
    ((uint32_t)(offsetof(RtlGameIdentity, save_name_prefix) +           \
                sizeof(((RtlGameIdentity *)0)->save_name_prefix)))

#define RTL_GAME_INITIALIZE_HAS_ROM UINT32_C(0x00000001)

/** Callback-lifetime initialization state. A loaded ROM is mutable only inside
 * this callback, which runs after cartridge loading and before the first reset.
 * This is the intended seam for verified per-game ROM transforms. The runner
 * handle may be retained; rom_data may not. */
typedef struct RtlGameInitializeContext {
    uint32_t struct_size;
    uint32_t flags;
    SrRunnerHandle *runner;
    uint8_t *rom_data;
    uint64_t rom_byte_size;
} RtlGameInitializeContext;

#define RTL_GAME_INITIALIZE_CONTEXT_V1_SIZE                             \
    ((uint32_t)(offsetof(RtlGameInitializeContext, rom_byte_size) +      \
                sizeof(((RtlGameInitializeContext *)0)->rom_byte_size)))

typedef bool RtlGameInitializeFunc(
    const RtlGameInitializeContext *context);
/** Receives the active opaque runner, or NULL before that runner is destroyed.
 * The module may bind long-lived host/game services but must not inspect a
 * concrete runner layout. */
typedef void RtlGameRunnerChangedFunc(SrRunnerHandle *runner);

typedef struct RtlGameLifecycleApi {
    uint32_t struct_size;
    uint32_t flags;
    RtlGameInitializeFunc *initialize;
    RtlGameRunnerChangedFunc *runner_changed;
} RtlGameLifecycleApi;

#define RTL_GAME_LIFECYCLE_API_V1_SIZE                                 \
    ((uint32_t)(offsetof(RtlGameLifecycleApi, runner_changed) +         \
                sizeof(((RtlGameLifecycleApi *)0)->runner_changed)))

typedef void RtlGameFrameFunc(void);
typedef int RtlGameRdnmiReadFunc(const RtlRdnmiReadContext *context);
typedef bool RtlGameDispatchMissRecoveryFunc(
    uint32_t source_pc24, uint32_t target_pc24);
typedef void RtlGamePpuDisplayControlWriteFunc(uint8_t value);

/** Recompiled execution policy. run_frame is the sole required callback and
 * denotes one host tick, not a universal SNES hardware phase. The game adapter
 * owns the recovered ordering of its body, NMI handling, and scanout within
 * that callback. The runner caches this table at registration, so nesting adds
 * no frame-time lookup or allocation. */
typedef struct RtlGameExecutionApi {
    uint32_t struct_size;
    uint32_t flags;
    RtlGameFrameFunc *run_frame;
    RtlGameFrameFunc *draw_ppu_frame;
    RtlGameRdnmiReadFunc *read_rdnmi;
    RtlGameDispatchMissRecoveryFunc *recover_dispatch_miss;
    RtlGamePpuDisplayControlWriteFunc *ppu_display_control_write;
} RtlGameExecutionApi;

#define RTL_GAME_EXECUTION_API_V2_SIZE                                  \
    ((uint32_t)(offsetof(RtlGameExecutionApi, ppu_display_control_write) + \
                sizeof(((RtlGameExecutionApi *)0)->                      \
                           ppu_display_control_write)))

typedef SrResult RtlGameCpuStateQueryFunc(
    void *user_data, SrCpuStateSnapshot *out_state);
typedef SrResult RtlGameExecutionStateQueryFunc(
    void *user_data, SrExecutionSnapshot *out_state);

/** Optional generated-code observers. user_data and cpu_component_handle stay
 * module-owned. The latter is returned only as the opaque CPU component handle
 * and is never dereferenced by the generic runner. */
typedef struct RtlGameStateProviderApi {
    uint32_t struct_size;
    uint32_t flags;
    void *user_data;
    const void *cpu_component_handle;
    RtlGameCpuStateQueryFunc *query_cpu_state;
    RtlGameExecutionStateQueryFunc *query_execution_state;
} RtlGameStateProviderApi;

#define RTL_GAME_STATE_PROVIDER_API_V1_SIZE                            \
    ((uint32_t)(offsetof(RtlGameStateProviderApi,                       \
                         query_execution_state) +                       \
                sizeof(((RtlGameStateProviderApi *)0)->                \
                           query_execution_state)))

typedef bool RtlGameSpcUploadSourceFunc(CpuState *cpu, uint32_t *source24);
typedef bool RtlGameSpcUploadCustomizeFunc(
    CpuState *cpu, const SrSpcUploadContext *upload, uint32_t source24);
typedef void RtlGameSpcUploadCommitFunc(SrSpcUploadContext *upload);
typedef int RtlGameSpcUploadStackPopFunc(const CpuState *cpu);
typedef void RtlGameAudioDspWriteRoutingFunc(
    const RtlAudioDspWriteContext *context,
    RtlAudioDspWriteRouting *routing);
typedef void RtlGameAudioStateLoadedRoutingFunc(
    const RtlAudioStateLoadedContext *context,
    RtlAudioStateLoadedRouting *routing);
typedef bool RtlGameAudioExtensionDspWriteFunc(
    RtlAudioExtensionContext *context, uint8_t address, uint8_t *value);
typedef void RtlGameAudioExtensionSpcOpcodeFunc(
    RtlAudioExtensionContext *context, uint16_t opcode_pc);
typedef int RtlGameAudioExtensionSpcCycleFunc(
    uint16_t opcode_pc, int cycles);
typedef void RtlGameAudioExtensionSaveFunc(RtlAudioSaveContext *context);
typedef void RtlGameAudioExtensionUploadFunc(
    RtlAudioExtensionContext *context, uint32_t source24);
typedef void RtlGameApuPortWriteFunc(uint8_t port, uint8_t value);
typedef void RtlGameSpcUploadCompletedFunc(uint32_t source24);
typedef void RtlGameAudioMixFunc(int16_t *stereo_buffer, int frames);

#define RTL_GAME_AUDIO_CAP_SPC_UPLOAD UINT64_C(0x0000000000000001)
#define RTL_GAME_AUDIO_CAP_VOICE_ROUTING UINT64_C(0x0000000000000002)
#define RTL_GAME_AUDIO_CAP_EXTENSION UINT64_C(0x0000000000000004)
#define RTL_GAME_AUDIO_CAP_PRESENTATION UINT64_C(0x0000000000000008)
#define RTL_GAME_AUDIO_CAP_SUPPORTED                                   \
    (RTL_GAME_AUDIO_CAP_SPC_UPLOAD | RTL_GAME_AUDIO_CAP_VOICE_ROUTING | \
     RTL_GAME_AUDIO_CAP_EXTENSION | RTL_GAME_AUDIO_CAP_PRESENTATION)

/** Privileged linked-game audio policy. This table deliberately remains
 * separate from the public read-only audio observer API: its callbacks run at
 * existing APU/SPC/DSP safe points and may request bounded mutations. The
 * presentation callbacks support game-aware pacing, replacement-track state,
 * and final host mixing without mutable process-global hook variables. */
typedef struct RtlGameAudioApi {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t capabilities;
    RtlGameSpcUploadSourceFunc *spc_upload_source;
    RtlGameSpcUploadCustomizeFunc *spc_upload_customize;
    RtlGameSpcUploadCommitFunc *spc_upload_commit;
    RtlGameSpcUploadStackPopFunc *spc_upload_stack_pop;
    RtlGameAudioDspWriteRoutingFunc *dsp_write_routing;
    RtlGameAudioStateLoadedRoutingFunc *state_loaded_routing;
    RtlGameAudioExtensionDspWriteFunc *extension_dsp_write;
    RtlGameAudioExtensionSpcOpcodeFunc *extension_spc_opcode;
    RtlGameAudioExtensionSpcCycleFunc *extension_spc_cycle;
    RtlGameAudioExtensionSaveFunc *extension_save;
    RtlGameAudioExtensionUploadFunc *extension_upload;
    RtlGameApuPortWriteFunc *apu_port_pace;
    RtlGameApuPortWriteFunc *apu_port_write;
    RtlGameSpcUploadCompletedFunc *spc_upload_completed;
    RtlGameAudioMixFunc *mix_output;
} RtlGameAudioApi;

#define RTL_GAME_AUDIO_API_V2_SIZE                                      \
    ((uint32_t)(offsetof(RtlGameAudioApi, mix_output) +                  \
                sizeof(((RtlGameAudioApi *)0)->mix_output)))

/** One immutable module is registered before runner creation and remains alive
 * until it is replaced while no runner exists. Optional table pointers must
 * agree exactly with their module capability bits. */
typedef struct RtlGameModule {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t capabilities;
    const RtlGameIdentity *identity;
    const RtlGameLifecycleApi *lifecycle;
    const RtlGameExecutionApi *execution;
    const RtlGameStateProviderApi *state_providers;
    const RtlGameAudioApi *audio;
} RtlGameModule;

#define RTL_GAME_MODULE_V2_SIZE                                       \
    ((uint32_t)(offsetof(RtlGameModule, audio) +                        \
                sizeof(((RtlGameModule *)0)->audio)))

/** @} */

#ifdef __cplusplus
}
#endif

#endif
