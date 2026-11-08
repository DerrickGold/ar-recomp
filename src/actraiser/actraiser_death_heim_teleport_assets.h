#ifndef AR_ACTRAISER_DEATH_HEIM_TELEPORT_ASSETS_H
#define AR_ACTRAISER_DEATH_HEIM_TELEPORT_ASSETS_H
/* Scoped native teleport resources while an Act 1 arena's actors are frozen.
 * The runtime owns hiding the player, animation and gameplay admission. */

#include "snesrecomp/game/cpu.h"
#include <stdbool.h>

typedef struct SrRunnerHandle SrRunnerHandle;
void ActRaiserDeathHeimTeleportAssets_BindRunner(SrRunnerHandle *runner);
bool ActRaiserDeathHeimTeleportAssets_Prepare(CpuState *cpu);
void ActRaiserDeathHeimTeleportAssets_Restore(CpuState *cpu);
/* Restore an abandoned transition in the same room/lifetime. A subsequent
 * room load or runner reset already owns its resources and is never undone. */
void ActRaiserDeathHeimTeleportAssets_Reset(void);

#endif
