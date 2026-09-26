#ifndef AR_ACTRAISER_LIVES_DISPLAY_H
#define AR_ACTRAISER_LIVES_DISPLAY_H
/* ActRaiserLivesDisplay: the Japanese zero-based lives display inside the US
 * tile writer ($02:C280), changing only the two digits it draws.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_lives_display_test.c */

#include "snesrecomp/game/cpu.h"
#include <stdbool.h>

bool ActRaiserLivesDisplay_Entry(const CpuState *cpu);
/* JP $04:91C5..91DE in the US $02:C280..C2A3 tile writer. Only two
 * character bytes change; native life stock and tile attributes are untouched.
 * Returns at the common $02:C2A4 continuation, with native SEP/ADC flags. */
void ActRaiserLivesDisplay_DrawZeroBased(CpuState *cpu);

#endif
