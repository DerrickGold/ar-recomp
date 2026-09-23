#ifndef ACTRAISER_TOWN_CENSUS_H
#define ACTRAISER_TOWN_CENSUS_H
#include "regional/regional_support.h"
#include "snesrecomp/game/cpu.h"
/* Existing $03:C07E owner, parameterized only by support coefficients. Keeps
 * native house occupancy, adjustment arithmetic, act-completion gate, scratch,
 * register/flag results and return frame. Does not demolish, activate settings,
 * refresh the total, award levels, or invoke ordinary earthquake feedback. */
RecompReturn ActRaiserTownCensus_Run(CpuState *cpu,const ArRegionalSupportSnapshot *snapshot);
RecompReturn ActRaiser_TownCensus(CpuState *cpu);
/* Semantic refresh for an already quiescent town transaction. Shares the
 * native census body, but touches only the selected town's population/support
 * outputs (and retains its act-completion gate). No fabricated CPU call frame,
 * register/scratch/index changes, policy activation, totals or level awards.
 * Native modular bias arithmetic remains; a destructive caller must preflight
 * population validity itself. Invalid input returns false without writes. */
bool ActRaiserTownCensus_Refresh(CpuState *cpu, unsigned town,
                               const ArRegionalSupportSnapshot *snapshot);
#endif
