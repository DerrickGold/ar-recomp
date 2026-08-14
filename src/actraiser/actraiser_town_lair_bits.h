#ifndef ACTRAISER_TOWN_LAIR_BITS_H
#define ACTRAISER_TOWN_LAIR_BITS_H

#include "cpu_state.h"

/* Per-town mask operations. Y selects a six-entry WRAM-pointer table and the
 * low accumulator byte selects a bit within the current town's mask. */
RecompReturn ActRaiser_TownLairMaskTest(CpuState *cpu);
RecompReturn ActRaiser_TownLairMaskSet(CpuState *cpu);
RecompReturn ActRaiser_TownLairMaskClear(CpuState *cpu);
RecompReturn ActRaiser_TownLairMaskResolveBit(CpuState *cpu);

/* Global town-event flags. The low accumulator byte selects a bit in the
 * fixed mask beginning at $90FF in the active data bank. */
RecompReturn ActRaiser_TownGlobalFlagTest(CpuState *cpu);
RecompReturn ActRaiser_TownGlobalFlagSet(CpuState *cpu);
RecompReturn ActRaiser_TownGlobalFlagClear(CpuState *cpu);
RecompReturn ActRaiser_TownGlobalFlagResolveBit(CpuState *cpu);

#endif /* ACTRAISER_TOWN_LAIR_BITS_H */
