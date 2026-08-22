#ifndef ACTRAISER_ACTION_ROOM_GRAPHICS_H
#define ACTRAISER_ACTION_ROOM_GRAPHICS_H

#include <stdbool.h>

#include "cpu_state.h"

/* Conditional whole-body HLEs for the action-only command-7 character loader
 * at $02:B28E and command-6 palette loader at $02:B330. Their predicates are
 * read-only: unsupported/non-action invocations retain the decoded ROM body. */
bool ActRaiser_ActionCharacterLoadHleEnabled(CpuState *cpu);
RecompReturn ActRaiser_LoadActionCharacters(CpuState *cpu);

bool ActRaiser_ActionPaletteLoadHleEnabled(CpuState *cpu);
RecompReturn ActRaiser_LoadActionPalette(CpuState *cpu);

#endif /* ACTRAISER_ACTION_ROOM_GRAPHICS_H */
