#include "common_cpu_infra.h"
#include "actraiser_rtl.h"
#include "ar_trace.h"
#include "cpu_state.h"
#include "runner_next.h"
#include "runner_next_internal.h"

static SrResult ActRaiser_QueryCpuState(
    Snes *snes, SrCpuStateSnapshot *out_state) {
  uint32 execution_pc24 = 0u;
  int execution_count;
  if (!snes || !out_state) return SR_RESULT_INVALID_ARGUMENT;
  execution_count = ar_block_history(&execution_pc24, 1);
  out_state->frame_counter =
      snes_frame_counter >= 0 ? (uint64_t)snes_frame_counter : 0u;
  out_state->flags =
      (g_cpu.m_flag ? SR_CPU_STATE_M_FLAG : 0u) |
      (g_cpu.x_flag ? SR_CPU_STATE_X_FLAG : 0u) |
      (g_cpu.emulation ? SR_CPU_STATE_EMULATION : 0u) |
      (g_cpu.host_return_valid ? SR_CPU_STATE_HOST_RETURN_VALID : 0u) |
      (execution_count > 0 ? SR_CPU_STATE_EXECUTION_PC_VALID : 0u);
  out_state->execution_pc24 = execution_pc24 & 0x00ffffffu;
  out_state->a = g_cpu.A;
  out_state->x = g_cpu.X;
  out_state->y = g_cpu.Y;
  out_state->s = g_cpu.S;
  out_state->d = g_cpu.D;
  out_state->db = g_cpu.DB;
  out_state->pb = g_cpu.PB;
  out_state->p = g_cpu.P;
  return SR_RESULT_OK;
}

static SrResult ActRaiser_QueryExecutionState(
    Snes *snes, SrExecutionSnapshot *out_state) {
  uint32_t depth;
  uint32_t frame;
  uint32_t history_count;
  uint32_t history_start;
  if (!snes || !out_state) return SR_RESULT_INVALID_ARGUMENT;

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

static void ActRaiser_BindRunnerAbi(Snes *snes, bool enabled) {
  ar_trace_bind_runner(snes, enabled);
  sr_runner_bind_ppu_services(snes, enabled);
  sr_runner_set_cpu_state_provider(
      snes, enabled ? ActRaiser_QueryCpuState : NULL,
      enabled ? &g_cpu : NULL);
  sr_runner_set_execution_state_provider(
      snes, enabled ? ActRaiser_QueryExecutionState : NULL);
}

const RtlGameInfo kActRaiserGameInfo = {
  .title = "actraiser",
  .initialize = NULL,
  .run_frame = &RunOneFrameOfGame,
  .draw_ppu_frame = &ActRaiserDrawPpuFrame,
  .read_rdnmi = &ActRaiser_ReadRdnmi,
  .recover_dispatch_miss = &ActRaiser_RecoverDispatchMiss,
  .save_name_prefix = "save",
#ifdef SNESRECOMP_NEXT_COMMON_CPU_INFRA_H
  .spc_upload_source = &ActRaiser_SpcUploadSource,
  .spc_upload_customize = &ActRaiser_SpcUploadCustomize,
  .spc_upload_commit = &ActRaiser_SpcUploadCommit,
  .spc_upload_stack_pop = &ActRaiser_SpcUploadStackPop,
  .bind_runner_abi = &ActRaiser_BindRunnerAbi,
#endif
};
