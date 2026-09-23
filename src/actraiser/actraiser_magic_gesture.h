#ifndef ACTRAISER_MAGIC_GESTURE_H
#define ACTRAISER_MAGIC_GESTURE_H

#include "snesrecomp/game/cpu.h"

bool ActRaiserMagicGesture_Entry(const CpuState *cpu);
/* Raw SNES auto-joypad bits, before native release/debounce masking. */
bool ActRaiserMagicGesture_ControlsReleased(uint16_t buttons);
/* JP ground attack prefix, translated into the US memory/continuation ABI.
 * Native attack/cast bodies, airborne attacks and payment stay untouched. */
uint32_t ActRaiserMagicGesture_Attack(CpuState *cpu);

#endif
