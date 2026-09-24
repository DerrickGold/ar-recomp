#ifndef ACTRAISER_TOWN_ART_H
#define ACTRAISER_TOWN_ART_H
#include <stdbool.h>
#include "snesrecomp/game/cpu.h"
/* US $02:B2C1: Command7's accepted raw-copy tail, after the native act-completion bank
 * filter. Unknown source shapes and all title/action uploads remain native. */
bool ActRaiser_TownArtEntry(CpuState *cpu);
RecompReturn ActRaiser_LoadTownArt(CpuState *cpu);
#endif
