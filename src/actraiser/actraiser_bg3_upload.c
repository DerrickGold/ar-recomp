#include "actraiser/actraiser_bg3_upload.h"

#include "actraiser/actraiser_credits.h"
#include "actraiser/actraiser_hud.h"
#include "actraiser_game.h"

static bool s_entering_upload;
extern RecompReturn bank_02_AEEB_M1X0(CpuState *cpu);

void ActRaiserBg3Upload_Reset(void) {
  s_entering_upload = false;
  ActRaiserHud_Reset();
  ActRaiserCredits_Reset();
}

bool ActRaiser_Bg3UploadEntry(CpuState *cpu) {
  if (s_entering_upload) {
    s_entering_upload = false;
    return false;
  }
  /* All three native NMI paths use DB-relative PPU ports and DP $F1. */
  return cpu && cpu->PB == 2 && cpu->DB == 0 && cpu->D == 0 &&
      cpu->m_flag && !cpu->x_flag && !cpu->emulation;
}

RecompReturn ActRaiser_Bg3Upload(CpuState *cpu) {
  const uint8_t group = cpu_read8(cpu, 0, kActRaiserWram_MapGroup);
  const uint8_t map = cpu_read8(cpu, 0, kActRaiserWram_CurrentMap);
  ActRaiserHud_ObserveScene(group, map);
  ActRaiserCredits_ObserveScene(group, map);
  const bool lower_rows = cpu_read8(cpu, 0, 0xf1) != 0;
  /* Delegate exactly once with the original JSR frame. AEEB does not yield;
   * its DMA writes are synchronous. Preserve all native return semantics. */
  s_entering_upload = true;
  const RecompReturn result = bank_02_AEEB_M1X0(cpu);
  s_entering_upload = false;
  if (result != RECOMP_RETURN_NORMAL) {
    ActRaiserBg3Upload_Reset();
  } else {
    ActRaiserHud_ObserveUpload();
    ActRaiserCredits_ObserveUpload(lower_rows);
  }
  return result;
}
