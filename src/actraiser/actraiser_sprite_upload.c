#include "actraiser/actraiser_sprite_ownership.h"
#include "actraiser_game.h"
#include "snesrecomp/game/cpu.h"

static bool s_entering_build, s_entering_upload;
extern RecompReturn bank_01_ACD9_M0X0(CpuState *cpu);
extern RecompReturn bank_01_ACD9_M1X0(CpuState *cpu);
extern RecompReturn bank_02_ACA3_M1X0(CpuState *cpu);

bool ActRaiser_SimSpriteBuildEntry(CpuState *cpu) {
  if (s_entering_build) { s_entering_build = false; return false; }
  return cpu && cpu->PB == 1 && cpu->D == 0 && !cpu->x_flag &&
      !cpu->emulation;
}

RecompReturn ActRaiser_SimSpriteBuild(CpuState *cpu) {
  ActRaiserSpriteOwnership_Begin(g_ram[kActRaiserWram_MapGroup],
      g_ram[kActRaiserWram_CurrentMap],
      ActRaiser_ReadWram16(kActRaiserWram_WorldLocation));
  s_entering_build = true;
  const RecompReturn result = cpu->m_flag
      ? bank_01_ACD9_M1X0(cpu) : bank_01_ACD9_M0X0(cpu);
  s_entering_build = false;
  if (result == RECOMP_RETURN_NORMAL)
    ActRaiserSpriteOwnership_Complete(g_ram + kActRaiserOamShadow);
  else
    ActRaiserSpriteOwnership_Reset();
  return result;
}

bool ActRaiser_SpriteUploadEntry(CpuState *cpu) {
  if (s_entering_upload) { s_entering_upload = false; return false; }
  return cpu && cpu->PB == 2 && cpu->DB == 0 && cpu->D == 0 &&
      cpu->m_flag && !cpu->x_flag && !cpu->emulation;
}

RecompReturn ActRaiser_SpriteUpload(CpuState *cpu) {
  /* Both delegates retain their original native stack frame and return
   * contract. ACA3 transfers the entire $0380..059F shadow synchronously. */
  s_entering_upload = true;
  const RecompReturn result = bank_02_ACA3_M1X0(cpu);
  s_entering_upload = false;
  if (result == RECOMP_RETURN_NORMAL)
    ActRaiserSpriteOwnership_Upload(g_ram[kActRaiserWram_MapGroup],
        g_ram[kActRaiserWram_CurrentMap], g_ram + kActRaiserOamShadow);
  else
    ActRaiserSpriteOwnership_Reset();
  return result;
}
