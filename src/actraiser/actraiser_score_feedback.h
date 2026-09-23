#ifndef ACTRAISER_SCORE_FEEDBACK_H
#define ACTRAISER_SCORE_FEEDBACK_H

#include "snesrecomp/game/cpu.h"

/* Game-side prefixes only. The regional owner pins a policy for the whole
 * settlement; generated continuations own prologues, calls, rewards and
 * native return frames. These functions never load/save policy or histories. */
bool ActRaiserScoreFeedback_Entry(const CpuState *cpu);
/* US03:D0B3 branch prefix, after the native completed-town setup. */
bool ActRaiserScoreFeedback_Route(CpuState *cpu, bool japanese, uint32_t *continuation);
/* JP conversion using the US scratch addresses. Valid four-digit BCD only;
 * returns to the existing US03:D10A RTS, not a synthetic host return. */
bool ActRaiserScoreFeedback_ConvertJP(CpuState *cpu);
/* US03:B525-B548 replacement. The native prefix already pushed the per-lair
 * amount and selected X=8*town. Native B549 retains PLY/PLB/PLP/PLX/RTL. */
bool ActRaiserScoreFeedback_Subtract(CpuState *cpu);

#endif
