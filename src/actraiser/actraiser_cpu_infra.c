#include "common_cpu_infra.h"
#include "actraiser_rtl.h"

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
#endif
};
