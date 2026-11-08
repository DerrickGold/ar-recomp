#ifndef AR_ACTRAISER_DEATH_HEIM_ARENA_H
#define AR_ACTRAISER_DEATH_HEIM_ARENA_H

/* Act 1 encounters retain their original map, collision and boss programs.
 * The native blanked loader replays the room's inherited asset chain, then
 * its ordinary player/camera initialization starts at the arena entrance.
 * Tests: tests/actraiser_death_heim_arena_test.c */
#include "snesrecomp/game/cpu.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct ActRaiserDeathHeimArena {
  uint8_t city, map;
  uint16_t player_x, player_ground_y;
  uint16_t placement, source;
  uint8_t boss_x, boss_y, parameter, type;
} ActRaiserDeathHeimArena;

const ActRaiserDeathHeimArena *ActRaiserDeathHeimArenas_Find(unsigned city);
bool ActRaiserDeathHeimArenas_Begin(CpuState *cpu, unsigned city);
/* True only while this encounter owns the native inherited-room asset VM. */
bool ActRaiserDeathHeimArenas_Loading(CpuState *cpu);
void ActRaiserDeathHeimArenas_Reset(void);

bool ActRaiser_DeathHeimArenaAssetsBeginEntry(CpuState *cpu);
RecompReturn ActRaiser_DeathHeimArenaAssetsBegin(CpuState *cpu);
bool ActRaiser_DeathHeimArenaAssetsEndEntry(CpuState *cpu);
RecompReturn ActRaiser_DeathHeimArenaAssetsEnd(CpuState *cpu);
bool ActRaiser_DeathHeimArenaPlayerEntry(CpuState *cpu);
RecompReturn ActRaiser_DeathHeimArenaPlayer(CpuState *cpu);
bool ActRaiser_DeathHeimArenaPlacementsEntry(CpuState *cpu);
RecompReturn ActRaiser_DeathHeimArenaPlacements(CpuState *cpu);
bool ActRaiser_DeathHeimArenaRetryEntry(CpuState *cpu);
RecompReturn ActRaiser_DeathHeimArenaRetry(CpuState *cpu);

#endif
