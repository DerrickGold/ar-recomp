#ifndef ACTRAISER_SPEED_SELECTOR_H
#define ACTRAISER_SPEED_SELECTOR_H

#include "snesrecomp/game/cpu.h"

/* Small native prefixes, with no input polling, renderer or campaign access. */
bool ActRaiserSpeedSelector_Entry(const CpuState *cpu, bool wide);
void ActRaiserSpeedSelector_Position(CpuState *cpu, uint16_t maximum);
uint32_t ActRaiserSpeedSelector_Right(CpuState *cpu, uint16_t maximum);
/* After the native synchronous scale compositor, before it can be uploaded.
 * Changes only the ten digit cells, retaining their native attributes. */
void ActRaiserSpeedSelector_DrawNativeScale(CpuState *cpu, uint16_t maximum);

#endif
