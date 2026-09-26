#ifndef AR_ACTRAISER_QUAKE_H
#define AR_ACTRAISER_QUAKE_H
/* ActRaiserQuake: the Japanese earthquake-miracle selector prefix; native code
 * keeps the queued actions and house feedback.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_quake_test.c */

#include "regional/towns/regional_quake.h"
#include "snesrecomp/game/cpu.h"

bool ActRaiserQuake_SelectorEntry(const CpuState *cpu);
/* JP selector prefix only. Native continuations own queued actions and house
 * feedback. Raw callee escapes propagate; target is written on NORMAL only. */
RecompReturn ActRaiserQuake_Select(CpuState *cpu, ArRegionalQuakeRule rule, uint32_t *target);
uint32_t ActRaiserQuake_SelectorPC(ArRegionalQuakeRule rule);

#endif
