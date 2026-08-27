#include "common_cpu_infra.h"
#include "actraiser_action_bg.h"
#include "actraiser_rtl.h"
#include "cpu_state.h"
#include "hd_replacements.h"
#include "native_audio_mixer.h"
#include "native_audio_extension.h"
#include "runner_next.h"
#include "sim/sim_world_map_build.h"

static SrResult ActRaiser_QueryCpuState(
    void *user_data, SrCpuStateSnapshot *out_state) {
  const CpuState *cpu = (const CpuState *)user_data;
  uint32 execution_pc24 = 0u;
  int execution_count;
  if (!cpu || !out_state) return SR_RESULT_INVALID_ARGUMENT;
  execution_count = ar_block_history(&execution_pc24, 1);
  out_state->frame_counter =
      snes_frame_counter >= 0 ? (uint64_t)snes_frame_counter : 0u;
  out_state->flags =
      (cpu->m_flag ? SR_CPU_STATE_M_FLAG : 0u) |
      (cpu->x_flag ? SR_CPU_STATE_X_FLAG : 0u) |
      (cpu->emulation ? SR_CPU_STATE_EMULATION : 0u) |
      (cpu->host_return_valid ? SR_CPU_STATE_HOST_RETURN_VALID : 0u) |
      (execution_count > 0 ? SR_CPU_STATE_EXECUTION_PC_VALID : 0u);
  out_state->execution_pc24 = execution_pc24 & 0x00ffffffu;
  out_state->a = cpu->A;
  out_state->x = cpu->X;
  out_state->y = cpu->Y;
  out_state->s = cpu->S;
  out_state->d = cpu->D;
  out_state->db = cpu->DB;
  out_state->pb = cpu->PB;
  out_state->p = cpu->P;
  return SR_RESULT_OK;
}

static SrResult ActRaiser_QueryExecutionState(
    void *user_data, SrExecutionSnapshot *out_state) {
  uint32_t depth;
  uint32_t frame;
  uint32_t history_count;
  uint32_t history_start;
  if (user_data != &g_cpu || !out_state) return SR_RESULT_INVALID_ARGUMENT;

  out_state->block_serial = g_ar_blk_idx;
  if (g_ar_blk_idx > 0u) {
    out_state->current_block_pc24 =
        g_ar_blk_ring[(g_ar_blk_idx - 1u) & kRuntimeBlockTraceRingMask] &
        0x00ffffffu;
    out_state->flags |= SR_EXECUTION_CURRENT_BLOCK_VALID;
  }
  if (g_last_recomp_func) {
    out_state->current_function = g_last_recomp_func;
    out_state->flags |= SR_EXECUTION_CURRENT_FUNCTION_VALID;
  }

  depth = g_recomp_stack_top > 0 ? (uint32_t)g_recomp_stack_top : 0u;
  out_state->stack_depth = depth;
  if (depth > SR_EXECUTION_STACK_CAPACITY) {
    depth = SR_EXECUTION_STACK_CAPACITY;
    out_state->flags |= SR_EXECUTION_STACK_TRUNCATED;
  }
  for (frame = 0u; frame < depth; ++frame) {
    out_state->stack[frame].function_name = g_recomp_stack[frame];
    out_state->stack[frame].entry_stack = g_cpu_entry_s[frame];
    out_state->stack[frame].host_return_valid = g_cpu_entry_hrv[frame];
  }

  history_count = g_ar_blk_idx < SR_EXECUTION_HISTORY_CAPACITY
      ? g_ar_blk_idx : SR_EXECUTION_HISTORY_CAPACITY;
  history_start = g_ar_blk_idx - history_count;
  out_state->history_count = history_count;
  for (frame = 0u; frame < history_count; ++frame) {
    const uint32_t slot =
        (history_start + frame) & kRuntimeBlockTraceRingMask;
    SrExecutionBlock *block = &out_state->history[frame];
    block->pc24 = g_ar_blk_ring[slot] & 0x00ffffffu;
    block->cpu_flags =
        ((g_ar_blk_aux[slot] >> 16) & 1u ? SR_CPU_STATE_M_FLAG : 0u) |
        ((g_ar_blk_aux[slot] >> 17) & 1u ? SR_CPU_STATE_X_FLAG : 0u);
    block->register_x = (uint16_t)g_ar_blk_aux[slot];
    block->stack_pointer = g_ar_blk_s[slot];
  }
  return SR_RESULT_OK;
}

static void ActRaiser_RunnerChanged(SrRunnerHandle *runner) {
  HdReplacements_BindRunner(runner);
  SimWorldMapBuild_BindRunner(runner);
  ActRaiser_SpcUploadBindRunner(runner);
  ActRaiserActionBg_BindRunner(runner);
  ActRaiser_WidescreenSpritesBindRunner(runner);
  NativeAudioMixer_BindRunner(runner);
}

static const RtlGameIdentity kActRaiserIdentity = {
  .struct_size = RTL_GAME_IDENTITY_V1_SIZE,
  .game_id = "actraiser",
  .display_name = "ActRaiser",
  .save_name_prefix = "save",
};

static const RtlGameLifecycleApi kActRaiserLifecycle = {
  .struct_size = RTL_GAME_LIFECYCLE_API_V1_SIZE,
  .initialize = &ActRaiser_InitializeGame,
  .runner_changed = &ActRaiser_RunnerChanged,
};

static const RtlGameExecutionApi kActRaiserExecution = {
  .struct_size = RTL_GAME_EXECUTION_API_V1_SIZE,
  .run_frame = &RunOneFrameOfGame,
  .draw_ppu_frame = &ActRaiserDrawPpuFrame,
  .read_rdnmi = &ActRaiser_ReadRdnmi,
  .recover_dispatch_miss = &ActRaiser_RecoverDispatchMiss,
};

static const RtlGameStateProviderApi kActRaiserStateProviders = {
  .struct_size = RTL_GAME_STATE_PROVIDER_API_V1_SIZE,
  .user_data = &g_cpu,
  .cpu_component_handle = &g_cpu,
  .query_cpu_state = &ActRaiser_QueryCpuState,
  .query_execution_state = &ActRaiser_QueryExecutionState,
};

static const RtlGameAudioApi kActRaiserAudio = {
  .struct_size = RTL_GAME_AUDIO_API_V1_SIZE,
  .capabilities = RTL_GAME_AUDIO_CAP_SPC_UPLOAD |
                  RTL_GAME_AUDIO_CAP_VOICE_ROUTING |
                  RTL_GAME_AUDIO_CAP_EXTENSION,
  .spc_upload_source = &ActRaiser_SpcUploadSource,
  .spc_upload_customize = &ActRaiser_SpcUploadCustomize,
  .spc_upload_commit = &ActRaiser_SpcUploadCommit,
  .spc_upload_stack_pop = &ActRaiser_SpcUploadStackPop,
  .dsp_write_routing = &NativeAudioMixer_RouteDspWrite,
  .state_loaded_routing = &NativeAudioMixer_RouteStateLoaded,
  .extension_dsp_write = &NativeAudioExtension_FilterDspWrite,
  .extension_spc_opcode = &NativeAudioExtension_PatchSpcOpcode,
  .extension_spc_cycle =
      &NativeAudioExtension_AdjustSpcOpcodeCycles,
  .extension_save = &NativeAudioExtension_SaveState,
  .extension_upload = &NativeAudioExtension_OnSpcUpload,
};

const RtlGameModule kActRaiserGameModule = {
  .abi_version = RTL_GAME_MODULE_ABI_VERSION,
  .struct_size = RTL_GAME_MODULE_V1_SIZE,
  .capabilities = RTL_GAME_MODULE_CAP_IDENTITY |
                  RTL_GAME_MODULE_CAP_LIFECYCLE |
                  RTL_GAME_MODULE_CAP_EXECUTION |
                  RTL_GAME_MODULE_CAP_STATE_PROVIDERS |
                  RTL_GAME_MODULE_CAP_AUDIO,
  .identity = &kActRaiserIdentity,
  .lifecycle = &kActRaiserLifecycle,
  .execution = &kActRaiserExecution,
  .state_providers = &kActRaiserStateProviders,
  .audio = &kActRaiserAudio,
};
