#include "actraiser_sim_ai_runtime.h"
#include "actraiser_regional_runtime.h"
#include "actraiser_hle_fatal.h"
static bool Entry(CpuState *cpu,ActRaiserSimAiSeam seam) {
  unsigned town,slot;uint16_t snapshot,value;
  if (!ActRaiserSimAi_Entry(cpu,seam,&town,&slot) || !ActRaiserRegional_SimActorAiSnapshot(town,slot,&snapshot)) return false;
  const ArRegionalSimAiRule rule=ActRaiserSimAi_Rule(seam);
  return ArRegionalSimAi_Value(snapshot,rule,&value) && value!=ArRegionalSimAi_Descriptor(rule)->value[kArRegionalSource_US];
}
static RecompReturn Run(CpuState *cpu,ActRaiserSimAiSeam seam) {
  uint32_t target;
  if (!Entry(cpu,seam)) ActRaiserHleFatal("SIM AI rule lost its generation owner");
  const RecompReturn result=ActRaiserSimAi_Run(cpu,seam,&target);
  if (result!=RECOMP_RETURN_NORMAL) return result>=RECOMP_RETURN_TAILCALL?result:(RecompReturn)(result-1);
  if (!cpu_hle_tailcall_request(target,ActRaiserSimAi_SourcePC(seam))) ActRaiserHleFatal("SIM AI has no native continuation");
  return RECOMP_RETURN_TAILCALL;
}
#define AR_SIM_AI_SEAM(name) \
bool ActRaiser_RegionalSim##name##Entry(CpuState *cpu) { return Entry(cpu,kActRaiserSimAi_##name); } \
RecompReturn ActRaiser_RegionalSim##name(CpuState *cpu) { return Run(cpu,kActRaiserSimAi_##name); }
AR_SIM_AI_SEAM(DragonReset) AR_SIM_AI_SEAM(DragonGate) AR_SIM_AI_SEAM(DragonRecursion)
AR_SIM_AI_SEAM(Candidate) AR_SIM_AI_SEAM(Pool) AR_SIM_AI_SEAM(BatChance) AR_SIM_AI_SEAM(BatWait)
#undef AR_SIM_AI_SEAM
