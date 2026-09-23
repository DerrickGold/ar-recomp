#ifndef ACTRAISER_SIM_ACTOR_H
#define ACTRAISER_SIM_ACTOR_H
#include "snesrecomp/game/cpu.h"
#include <stdbool.h>
/* Game-owned native address mapping, shared by combat and AI adapters.
 * Caller validates its own register-width and call-site contract. */
bool ActRaiserSimActor_Town(CpuState *cpu,unsigned *town);
bool ActRaiserSimActor_Locate(CpuState *cpu,unsigned address,unsigned *town,unsigned *slot);
#endif
