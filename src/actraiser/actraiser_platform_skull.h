#ifndef AR_ACTRAISER_PLATFORM_SKULL_H
#define AR_ACTRAISER_PLATFORM_SKULL_H
/* ActRaiser platform-skull hooks: the skull platform's spawn and its X and Y
 * range under the regional platform-skull rules.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_platform_skull_test.c */
#include "snesrecomp/game/cpu.h"
bool ActRaiser_PlatformSkullSpawnEntry(CpuState *cpu);
RecompReturn ActRaiser_PlatformSkullSpawn(CpuState *cpu);
bool ActRaiser_PlatformSkullRangeXEntry(CpuState *cpu);
RecompReturn ActRaiser_PlatformSkullRangeX(CpuState *cpu);
bool ActRaiser_PlatformSkullRangeYEntry(CpuState *cpu);
RecompReturn ActRaiser_PlatformSkullRangeY(CpuState *cpu);
#endif
