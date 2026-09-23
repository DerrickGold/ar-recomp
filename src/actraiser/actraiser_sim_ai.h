#ifndef ACTRAISER_SIM_AI_H
#define ACTRAISER_SIM_AI_H
#include "regional/regional_sim_ai.h"
#include "snesrecomp/game/cpu.h"
typedef enum ActRaiserSimAiSeam {
  kActRaiserSimAi_DragonReset,
  kActRaiserSimAi_DragonGate,
  kActRaiserSimAi_DragonRecursion,
  kActRaiserSimAi_Candidate,
  kActRaiserSimAi_Pool,
  kActRaiserSimAi_BatChance,
  kActRaiserSimAi_BatWait,
  kActRaiserSimAi_Count
} ActRaiserSimAiSeam;
bool ActRaiserSimAi_Entry(CpuState *cpu,ActRaiserSimAiSeam seam,unsigned *town,unsigned *slot);
/* Japanese leaf only; callers retain native code for numerically US rules. */
RecompReturn ActRaiserSimAi_Run(CpuState *cpu,ActRaiserSimAiSeam seam,uint32_t *continuation);
uint32_t ActRaiserSimAi_SourcePC(ActRaiserSimAiSeam seam);
ArRegionalSimAiRule ActRaiserSimAi_Rule(ActRaiserSimAiSeam seam);
#endif
