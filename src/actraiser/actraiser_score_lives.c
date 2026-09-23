#include "actraiser_score_lives.h"
#include "actraiser_regional_runtime.h"
#include "actraiser_hle_fatal.h"

extern RecompReturn bank_00_873C_M0X0(CpuState *cpu);
extern RecompReturn bank_00_873C_M1X0(CpuState *cpu);
static bool s_delegate;

bool ActRaiser_ScoreLivesEntry(CpuState *cpu) {
  if(s_delegate) {s_delegate=false;return false;}
  return ActRaiserRegional_ScoreLivesEnabled() && cpu && !cpu->PB && !cpu->DB &&
      !cpu->D && !cpu->x_flag && !cpu->emulation;
}
RecompReturn ActRaiser_ScoreLives(CpuState *cpu) {
  if(!ActRaiser_ScoreLivesEntry(cpu))ActRaiserHleFatal("Unsupported score-life transaction");
  const uint16_t before=cpu_read16(cpu,0,0x1f);
  /* The native helper retains the original JSR frame and owns decimal score
   * addition, saturation and PHP/PLP. It is synchronous in either M width. */
  s_delegate=true;
  const RecompReturn result=cpu->m_flag?bank_00_873C_M1X0(cpu):bank_00_873C_M0X0(cpu);
  s_delegate=false;
  if(result!=RECOMP_RETURN_NORMAL)return result;
  if(ArRegionalScoreLives_Award(before,cpu_read16(cpu,0,0x1f),cpu_read8(cpu,0,0x349)!=0)) {
    cpu_write8(cpu,0,0x1c,ArRegionalScoreLives_Increment(cpu_read8(cpu,0,0x1c)));
    /* Use the same runtime COP boundary as generated code, so extended audio
     * and request tracing still observe the event. The handler preserves CPU
     * registers/flags; PAL's final PLA/PLP restores the old score and entry P. */
    cpu->A=0x008d;
    if(g_cpu_cop_hook)g_cpu_cop_hook(cpu);
  }
  cpu->A=before;
  return result;
}
