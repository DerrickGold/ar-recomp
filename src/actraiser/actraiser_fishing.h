#ifndef ACTRAISER_FISHING_H
#define ACTRAISER_FISHING_H

#include "snesrecomp/game/cpu.h"

/* Fillmore event10's prefix only. Native selector owns eligibility/fired
 * guards; native continuations own the waiting flag and the single reward.
 * Non-normal return is the raw leaf token; entry adapter stops and propagates
 * it once. continuation is written only when the prefix completes normally. */
bool ActRaiserFishing_Entry(const CpuState *cpu);
RecompReturn ActRaiserFishing_Prefix(CpuState *cpu, uint8_t target,
                                    bool reconcile, uint16_t *continuation);

#endif
