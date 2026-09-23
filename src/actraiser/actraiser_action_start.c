#include "actraiser_action_start.h"
#include "actraiser_regional_runtime.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"

extern RecompReturn bank_02_AB05_M1X0(CpuState *cpu);
static bool s_delegate;
bool ActRaiser_ActionStartEntry(CpuState *cpu) {
  if(s_delegate) {s_delegate=false;return false;}
  return cpu && cpu->PB==2 && !cpu->DB && !cpu->D && cpu->m_flag &&
      !cpu->x_flag && !cpu->emulation;
}
RecompReturn ActRaiser_ActionStart(CpuState *cpu) {
  ArRegionalActionStartSnapshot start;
  if(!ActRaiser_ActionStartEntry(cpu) || !ActRaiserRegional_BeginActionStart(&start))
    ActRaiserHleFatal("Invalid action-run initialization");
  /* The bounded initializer retains its real JSL/RTL frame, native progression,
   * sword-power, stock, score and equipped-spell resets. Neither ordinary
   * retry nor room loading enters this new-run helper. No yielded work occurs
   * between its initial values and the selected allowances below. */
  s_delegate=true;
  const RecompReturn result=bank_02_AB05_M1X0(cpu);
  s_delegate=false;
  if(result!=RECOMP_RETURN_NORMAL)return result;
  if(!ActRaiserRegional_StartInventory())ActRaiserHleFatal("Cannot initialize action-run inventory");
  cpu_write8(cpu,0,0x1c,start.spares);
  cpu->A=(cpu->A&0xff00)|start.health;
  cpu_write8(cpu,0,0x1e,start.health);cpu_write8(cpu,0,0x1d,start.health);
  ActRaiserCpuHle_SetNegativeZero8(cpu,start.health);
  return result;
}
