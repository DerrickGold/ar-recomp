#ifndef AR_ACTRAISER_DEATH_HEIM_EYES_H
#define AR_ACTRAISER_DEATH_HEIM_EYES_H

#include "actraiser/actraiser_death_heim_rush.h"
#include "actraiser/regional/actraiser_actor_art.h"
#include "snesrecomp/game/cpu.h"

/* Presentation only. The rush owner supplies logical identities once per hub
 * visit; the native game frame drives both eyes' original animation sequences. */
void ActRaiserDeathHeimEyes_BeginHub(CpuState *cpu,
    const ActRaiserDeathHeimEncounter *next,
    const ActRaiserDeathHeimEncounter *last);

enum { kActRaiserDeathHeimEyeParts = 6 };
typedef struct ActRaiserDeathHeimEyesDraw {
  unsigned count;
  ActRaiserActorArtPart parts[kActRaiserDeathHeimEyeParts];
} ActRaiserDeathHeimEyesDraw;
/* Parts use the caller's current native origin, including regional artwork
 * compensation. True with count zero is a fully extinguished glow actor. */
bool ActRaiserDeathHeimEyes_Draw(CpuState *cpu, uint16_t owner,
    bool flip_x, bool flip_y, int16_t left, int16_t top,
    ActRaiserDeathHeimEyesDraw *out);
int ActRaiserDeathHeimEyes_FlareOffset(CpuState *cpu, uint16_t owner);

#endif
