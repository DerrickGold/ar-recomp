#ifndef ACTRAISER_PLATFORM_SKULL_H
#define ACTRAISER_PLATFORM_SKULL_H
#include "snesrecomp/game/cpu.h"
bool ActRaiser_PlatformSkullSpawnEntry(CpuState *cpu);
RecompReturn ActRaiser_PlatformSkullSpawn(CpuState *cpu);
bool ActRaiser_PlatformSkullRangeXEntry(CpuState *cpu);
RecompReturn ActRaiser_PlatformSkullRangeX(CpuState *cpu);
bool ActRaiser_PlatformSkullRangeYEntry(CpuState *cpu);
RecompReturn ActRaiser_PlatformSkullRangeY(CpuState *cpu);
#endif
