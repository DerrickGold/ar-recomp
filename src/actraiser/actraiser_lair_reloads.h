#ifndef ACTRAISER_LAIR_RELOADS_H
#define ACTRAISER_LAIR_RELOADS_H

#include "regional/towns/regional_lair_reloads.h"
#include "snesrecomp/game/cpu.h"
#include "save_system.h"

bool ActRaiserLairReloads_Entry(const CpuState *cpu);
bool ActRaiserLairReloads_Initialize(ArRegionalLairReloads *history, CpuState *cpu);
/* Called only after the legacy-history acknowledgement (or for an older
 * already-acknowledged companion). Durable image, never guessed live state. */
bool ActRaiserLairReloads_AdoptSaved(ArRegionalLairReloads *history,
                                   const uint8_t image[kActRaiserSramSize]);
bool ActRaiserLairReloads_Check(ArRegionalLairReloads *history, CpuState *cpu, ArRegionalSource source);
bool ActRaiserLairReloads_Project(ArRegionalLairReloads *history, CpuState *cpu,
                                 ArRegionalSource current, ArRegionalSource target);
/* Capture expected post-modifier values without touching CPU or native RAM.
 * Native still owns its loop, flags, stack and all writes. */
bool ActRaiserLairReloads_BeginReduction(ArRegionalLairReloads *history, CpuState *cpu,
    ArRegionalSource source, ArRegionalLairReloads *candidate, unsigned *town);
bool ActRaiserLairReloads_EndReduction(ArRegionalLairReloads *history, CpuState *cpu,
    ArRegionalSource source, const ArRegionalLairReloads *candidate, unsigned town, RecompReturn result);

#endif
