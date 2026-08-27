#ifndef ACTRAISER_ACTION_VIDEO_CONFIG_H
#define ACTRAISER_ACTION_VIDEO_CONFIG_H

#include <stdbool.h>

#include "snesrecomp/game/cpu.h"

/* Conditional whole-body HLE for the action-only command-3 video-profile
 * applicator at $02:B4E8. The predicate is read-only; non-action, unsupported
 * CPU modes, and unaudited profile operands retain the decoded ROM body. */
bool ActRaiser_ActionVideoConfigHleEnabled(CpuState *cpu);
RecompReturn ActRaiser_ApplyActionVideoConfig(CpuState *cpu);

#endif /* ACTRAISER_ACTION_VIDEO_CONFIG_H */
