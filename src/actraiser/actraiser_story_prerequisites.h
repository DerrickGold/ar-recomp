#ifndef ACTRAISER_STORY_PREREQUISITES_H
#define ACTRAISER_STORY_PREREQUISITES_H

#include "snesrecomp/game/cpu.h"
#include "regional/regional_story_prerequisites.h"

/* US $03:E13E LDA prefix; $E142 keeps the original strict comparison and
 * prerequisite setter. Only the two identified ROM records are replaceable. */
bool ActRaiserStory_ThresholdEntry(CpuState *cpu, ArRegionalStoryRule *rule);
bool ActRaiserStory_LoadThreshold(CpuState *cpu, uint16_t threshold);
/* US $03:EB35, after event-8 prerequisite clearing. Skipping to $EB3D
 * preserves the original three-pop/SEC/RTS rejection unwind. */
bool ActRaiserStory_CompassEntry(const CpuState *cpu);

#endif
