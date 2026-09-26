#ifndef AR_ACTRAISER_STAGE_TERRAIN_H
#define AR_ACTRAISER_STAGE_TERRAIN_H
/* ActRaiser stage-terrain hooks: stage terrain, checkpoints and start positions
 * under the regional terrain rules and media.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_stage_terrain_test.c */
#include "snesrecomp/game/cpu.h"
bool ActRaiser_StageTerrainEntry(CpuState *cpu);
RecompReturn ActRaiser_StageTerrain(CpuState *cpu);
bool ActRaiser_TerrainStartEntry(CpuState *cpu);
RecompReturn ActRaiser_TerrainStart(CpuState *cpu);
bool ActRaiser_TerrainCheckpointEntry(CpuState *cpu);
RecompReturn ActRaiser_TerrainCheckpoint(CpuState *cpu);
#endif
