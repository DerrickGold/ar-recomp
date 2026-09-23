#ifndef ACTRAISER_SIM_AI_RUNTIME_H
#define ACTRAISER_SIM_AI_RUNTIME_H
#include "actraiser_sim_ai.h"
#define AR_SIM_AI_SEAM(name) bool ActRaiser_RegionalSim##name##Entry(CpuState *cpu); RecompReturn ActRaiser_RegionalSim##name(CpuState *cpu)
AR_SIM_AI_SEAM(DragonReset);AR_SIM_AI_SEAM(DragonGate);AR_SIM_AI_SEAM(DragonRecursion);
AR_SIM_AI_SEAM(Candidate);AR_SIM_AI_SEAM(Pool);AR_SIM_AI_SEAM(BatChance);AR_SIM_AI_SEAM(BatWait);
#undef AR_SIM_AI_SEAM
#endif
