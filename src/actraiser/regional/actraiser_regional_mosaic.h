#ifndef ACTRAISER_REGIONAL_MOSAIC_H
#define ACTRAISER_REGIONAL_MOSAIC_H
#include "snesrecomp/game/cpu.h"
/* US $02:939C action-room mosaic service. Uses the room-pinned presentation
 * snapshot; never activates an overlay selection during an in-flight effect. */
bool ActRaiser_RegionalMosaicEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalMosaic(CpuState *cpu);
#endif
