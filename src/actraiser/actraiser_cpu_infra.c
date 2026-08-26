#include "common_cpu_infra.h"
#include "actraiser_rtl.h"
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

static void ActRaiser_BindRunnerAbi(Snes *snes, bool enabled) {
  sr_runner_bind_ppu_services(snes, enabled);
  sr_runner_set_cpu_state_provider(
      snes, enabled ? ActRaiser_QueryCpuState : NULL,
      enabled ? &g_cpu : NULL);
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
