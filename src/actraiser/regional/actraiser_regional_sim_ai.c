#include "actraiser/regional/actraiser_regional_sim_ai.h"
#include "actraiser/regional/actraiser_regional_runtime.h"
#include "actraiser/actraiser_hle_fatal.h"
static bool Entry(CpuState *cpu,ActRaiserSimAiSeam seam) {
  unsigned town, slot;
  uint16_t snapshot, value;
  if (!ActRaiserSimAi_Entry(cpu, seam, &town, &slot) ||
      !ActRaiserRegional_SimActorAiSnapshot(town, slot, &snapshot))
    return false;
  const ArRegionalSimAiRule rule=ActRaiserSimAi_Rule(seam);
  return ArRegionalSimAi_Value(snapshot, rule, &value) &&
      value != ArRegionalSimAi_Descriptor(rule)->value[kArRegionalSource_US];
}
static RecompReturn Run(CpuState *cpu,ActRaiserSimAiSeam seam) {
  uint32_t target;
  if (!Entry(cpu,seam)) ActRaiserHleFatal("SIM AI rule lost its generation owner");
  const RecompReturn result=ActRaiserSimAi_Run(cpu,seam,&target);
  if (result != RECOMP_RETURN_NORMAL)
    return result >= RECOMP_RETURN_TAILCALL ? result : (RecompReturn)(result - 1);
  if (!cpu_hle_tailcall_request(target, ActRaiserSimAi_SourcePC(seam)))
    ActRaiserHleFatal("SIM AI has no native continuation");
  return RECOMP_RETURN_TAILCALL;
}

bool ActRaiser_RegionalSimDragonResetEntry(CpuState *cpu) {
  return Entry(cpu, kActRaiserSimAi_DragonReset);
}
RecompReturn ActRaiser_RegionalSimDragonReset(CpuState *cpu) {
  return Run(cpu, kActRaiserSimAi_DragonReset);
}
bool ActRaiser_RegionalSimDragonGateEntry(CpuState *cpu) {
  return Entry(cpu, kActRaiserSimAi_DragonGate);
}
RecompReturn ActRaiser_RegionalSimDragonGate(CpuState *cpu) {
  return Run(cpu, kActRaiserSimAi_DragonGate);
}
bool ActRaiser_RegionalSimDragonRecursionEntry(CpuState *cpu) {
  return Entry(cpu, kActRaiserSimAi_DragonRecursion);
}
RecompReturn ActRaiser_RegionalSimDragonRecursion(CpuState *cpu) {
  return Run(cpu, kActRaiserSimAi_DragonRecursion);
}
bool ActRaiser_RegionalSimCandidateEntry(CpuState *cpu) {
  return Entry(cpu, kActRaiserSimAi_Candidate);
}
RecompReturn ActRaiser_RegionalSimCandidate(CpuState *cpu) {
  return Run(cpu, kActRaiserSimAi_Candidate);
}
bool ActRaiser_RegionalSimPoolEntry(CpuState *cpu) {
  return Entry(cpu, kActRaiserSimAi_Pool);
}
RecompReturn ActRaiser_RegionalSimPool(CpuState *cpu) {
  return Run(cpu, kActRaiserSimAi_Pool);
}
bool ActRaiser_RegionalSimBatChanceEntry(CpuState *cpu) {
  return Entry(cpu, kActRaiserSimAi_BatChance);
}
RecompReturn ActRaiser_RegionalSimBatChance(CpuState *cpu) {
  return Run(cpu, kActRaiserSimAi_BatChance);
}
bool ActRaiser_RegionalSimBatWaitEntry(CpuState *cpu) {
  return Entry(cpu, kActRaiserSimAi_BatWait);
}
RecompReturn ActRaiser_RegionalSimBatWait(CpuState *cpu) {
  return Run(cpu, kActRaiserSimAi_BatWait);
}
