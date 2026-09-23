#ifndef ACTRAISER_ARRIVAL_RUNTIME_H
#define ACTRAISER_ARRIVAL_RUNTIME_H
#include "snesrecomp/game/cpu.h"
/* Capture only at an eligible final departure or Japanese Palace gate. An
 * already-started event keeps its effective policy through announcement. */
bool ActRaiserRegional_ArrivalSnapshot(bool latch,bool continuing,bool *japanese);
void ActRaiserArrivalRuntime_Reset(void);
bool ActRaiser_RegionalArrivalDepartureEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalArrivalDeparture(CpuState *cpu);
bool ActRaiser_RegionalArrivalPalaceEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalArrivalPalace(CpuState *cpu);
#endif
