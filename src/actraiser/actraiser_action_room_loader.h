#ifndef ACTRAISER_ACTION_ROOM_LOADER_H
#define ACTRAISER_ACTION_ROOM_LOADER_H

#include <stdbool.h>

#include "snesrecomp/game/cpu.h"

/* Conditional whole-body HLEs for the action-only command-5 metatile loader
 * at $02:B363 and command-4 map loader at $02:B3EB. Their predicates are
 * read-only: unsupported/non-action invocations retain the decoded ROM body. */
bool ActRaiser_ActionMetatileLoadHleEnabled(CpuState *cpu);
RecompReturn ActRaiser_LoadActionMetatiles(CpuState *cpu);

bool ActRaiser_ActionMapLoadHleEnabled(CpuState *cpu);
RecompReturn ActRaiser_LoadActionMap(CpuState *cpu);

#endif /* ACTRAISER_ACTION_ROOM_LOADER_H */
