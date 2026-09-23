#ifndef ACTRAISER_ACTION_MOTION_H
#define ACTRAISER_ACTION_MOTION_H
#include "snesrecomp/game/cpu.h"
/* Shared native animation-row acquisition seam. Independent motion and
 * collision policies change only fresh actor outputs; never shared asset
 * bytes, current elapsed phases, graphics or native CPU/return ownership. */
bool ActRaiser_ActionMotionEntry(CpuState *cpu);
RecompReturn ActRaiser_ActionMotion(CpuState *cpu);
/* Common spawn/reinitialization after first row, before bottom anchoring.
 * Uses the selected descriptor in Y; +32 is not installed at first birth. */
bool ActRaiser_ActionCollisionBirthEntry(CpuState *cpu);
RecompReturn ActRaiser_ActionCollisionBirth(CpuState *cpu);
/* Ordinary Kasandora firing-loop prefix; native animation/allocation and
 * Pharaoh children remain separate owners. Only bypasses a new pause. */
bool ActRaiser_WallHeadPauseEntry(CpuState *cpu);
RecompReturn ActRaiser_WallHeadPause(CpuState *cpu);
bool ActRaiser_EmitterPositionEntry(CpuState *cpu);
RecompReturn ActRaiser_EmitterPosition(CpuState *cpu);
bool ActRaiser_EmitterCadenceEntry(CpuState *cpu);
RecompReturn ActRaiser_EmitterCadence(CpuState *cpu);
#endif
