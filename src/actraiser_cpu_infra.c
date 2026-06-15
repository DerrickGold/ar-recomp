#include "common_cpu_infra.h"
#include "actraiser_rtl.h"

const RtlGameInfo kActRaiserGameInfo = {
  .title = "actraiser",
  .initialize = NULL,
  .run_frame = &RunOneFrameOfGame,
  .draw_ppu_frame = &ActRaiserDrawPpuFrame,
  .save_name_prefix = "save",
};
