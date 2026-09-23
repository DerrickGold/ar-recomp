#ifndef ACTRAISER_TOWN_STATUS_RUNTIME_H
#define ACTRAISER_TOWN_STATUS_RUNTIME_H

#include "actraiser_town_status.h"

/* Game-internal policy boundary, implemented by the campaign owner. A read
 * never activates rules; the outer native transaction requests activation. */
bool ActRaiserRegional_TownStatusSnapshot(bool activate, ArRegionalTownStatusSnapshot *out);
void ActRaiserTownStatusRuntime_Reset(void);
/* Compose reporting with the construction owner's independent price policy.
 * The callback is the generated native body, with its original frame/token. */
RecompReturn ActRaiserTownStatusRuntime_Run(CpuState *cpu, RecompReturn (*native)(CpuState *));
bool ActRaiser_RegionalTownStatusCycleEntry(CpuState *cpu);
bool ActRaiser_RegionalTownStatusReportEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalTownStatusCycle(CpuState *cpu);
RecompReturn ActRaiser_RegionalTownStatusReport(CpuState *cpu);
RecompReturn ActRaiser_RegionalTownStatusPlot(CpuState *cpu);
RecompReturn ActRaiser_RegionalTownStatusVisiblePlot(CpuState *cpu);
bool ActRaiser_RegionalTownStatusLowEntry(CpuState *cpu);
bool ActRaiser_RegionalTownStatusPlotCountEntry(CpuState *cpu);
bool ActRaiser_RegionalTownStatusMergeEntry(CpuState *cpu);
bool ActRaiser_RegionalTownStatusFoodEntry(CpuState *cpu);
bool ActRaiser_RegionalTownStatusClassifierEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalTownStatusLow(CpuState *cpu);
RecompReturn ActRaiser_RegionalTownStatusReportLow(CpuState *cpu);
RecompReturn ActRaiser_RegionalTownStatusPlotCount(CpuState *cpu);
RecompReturn ActRaiser_RegionalTownStatusMerge(CpuState *cpu);
RecompReturn ActRaiser_RegionalTownStatusFood(CpuState *cpu);
RecompReturn ActRaiser_RegionalTownStatusClassifier(CpuState *cpu);

#endif
