#ifndef AR_ACTRAISER_SOURCES_H
#define AR_ACTRAISER_SOURCES_H
/* ActRaiserSources: the regional rules for Source items: the compare at
 * collection and whether a carried Source is kept.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_sources_test.c */

#include "snesrecomp/game/cpu.h"

bool ActRaiserSources_CollectionEntry(const CpuState *cpu);
/* Accepted Source removed from town inventory and pushed at $01:8915.
 * Reproduce the regional compare prefix; native insertion/effect owns all
 * inventory writes, acknowledgements, overflow, sound and return-to-town. */
uint32_t ActRaiserSources_CollectionRoute(CpuState *cpu, bool automatic);
/* At the two Source consumption callsites only. The original return chain
 * identifies automatic collection, without persisting a host modal flag.
 * No inventory mutation here; explicit Use retains native consumption. */
bool ActRaiserSources_KeepCarried(CpuState *cpu, uint8_t item);

#endif
