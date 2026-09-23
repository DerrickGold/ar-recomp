#ifndef ACTRAISER_LIVES_DISPLAY_H
#define ACTRAISER_LIVES_DISPLAY_H

#include "snesrecomp/game/cpu.h"
#include <stdbool.h>

bool ActRaiserLivesDisplay_Entry(const CpuState *cpu);
/* JP $04:91C5..91DE in the US $02:C280..C2A3 tile writer. Only two
 * character bytes change; native life stock and tile attributes are untouched.
 * Returns at the common $02:C2A4 continuation, with native SEP/ADC flags. */
void ActRaiserLivesDisplay_DrawZeroBased(CpuState *cpu);

#endif
