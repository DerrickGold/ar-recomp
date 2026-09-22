#ifndef ACTRAISER_MIRACLE_H
#define ACTRAISER_MIRACLE_H

#include "regional/regional_costs.h"
#include "snesrecomp/game/cpu.h"

/* The five native command IDs are an adapter detail, not regional policy IDs.
 * A caller captures the quote before entering this controller and retains it
 * for presentation until return. No policy/filesystem lookup occurs here. */
bool ActRaiserMiracle_Rule(unsigned action, ArRegionalCostRule *rule);
bool ActRaiserMiracle_Entry(const CpuState *cpu, unsigned action);
RecompReturn ActRaiserMiracle_Run(CpuState *cpu, unsigned action,
                                 const ArRegionalCostSnapshot *quote);

#endif
