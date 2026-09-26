#ifndef AR_ACTRAISER_MAGIC_GESTURE_H
#define AR_ACTRAISER_MAGIC_GESTURE_H
/* ActRaiserMagicGesture: the Japanese ground-attack magic gesture, translated
 * onto the US memory and continuation, reading raw joypad bits.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_magic_gesture_test.c */

#include "snesrecomp/game/cpu.h"

bool ActRaiserMagicGesture_Entry(const CpuState *cpu);
/* Raw SNES auto-joypad bits, before native release/debounce masking. */
bool ActRaiserMagicGesture_ControlsReleased(uint16_t buttons);
/* JP ground attack prefix, translated into the US memory/continuation ABI.
 * Native attack/cast bodies, airborne attacks and payment stay untouched. */
uint32_t ActRaiserMagicGesture_Attack(CpuState *cpu);

#endif
