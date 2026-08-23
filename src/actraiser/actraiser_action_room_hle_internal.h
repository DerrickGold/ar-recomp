#ifndef ACTRAISER_ACTION_ROOM_HLE_INTERNAL_H
#define ACTRAISER_ACTION_ROOM_HLE_INTERNAL_H

#include <stdint.h>

#include "actraiser_game.h"
#include "cpu_state.h"

/* Shared 65816 primitives for the action-room command HLEs. Keeping these
 * exact bus operations in one place prevents the loader, graphics, and video
 * implementations from drifting while still preserving every emulated read,
 * write, and native-mode stack byte. */
static inline uint8_t ActionRoomHle_ReadDirectPage8(
    CpuState *cpu, uint16_t offset) {
  return cpu_read8(cpu, kSnesLowWramBank,
                   (uint16_t)(cpu->D + offset));
}

static inline uint16_t ActionRoomHle_ReadDirectPage16(
    CpuState *cpu, uint16_t offset) {
  return cpu_read16(cpu, kSnesLowWramBank,
                    (uint16_t)(cpu->D + offset));
}

static inline void ActionRoomHle_WriteDirectPage8(
    CpuState *cpu, uint16_t offset, uint8_t value) {
  cpu_write8(cpu, kSnesLowWramBank,
             (uint16_t)(cpu->D + offset), value);
}

static inline void ActionRoomHle_WriteDirectPage16(
    CpuState *cpu, uint16_t offset, uint16_t value) {
  cpu_write16(cpu, kSnesLowWramBank,
              (uint16_t)(cpu->D + offset), value);
}

static inline uint8_t ActionRoomHle_ReadLongIndexed(
    CpuState *cpu, uint16_t direct_page_pointer, uint16_t index) {
  const uint32_t base =
      (uint32_t)ActionRoomHle_ReadDirectPage16(cpu, direct_page_pointer) |
      ((uint32_t)ActionRoomHle_ReadDirectPage8(
           cpu, (uint16_t)(direct_page_pointer + 2u)) << 16);
  const uint32_t address = (base + index) & 0xFFFFFFu;
  return cpu_read8(cpu, (uint8_t)(address >> 16), (uint16_t)address);
}

static inline void ActionRoomHle_PushStackWord(
    CpuState *cpu, uint16_t value) {
  cpu->S = (uint16_t)(cpu->S - 1u);
  cpu_write16(cpu, 0x00, cpu->S, value);
  cpu->S = (uint16_t)(cpu->S - 1u);
}

static inline uint16_t ActionRoomHle_PopStackWord(CpuState *cpu) {
  cpu->S = (uint16_t)(cpu->S + 1u);
  const uint16_t value = cpu_read16(cpu, 0x00, cpu->S);
  cpu->S = (uint16_t)(cpu->S + 1u);
  return value;
}

#endif /* ACTRAISER_ACTION_ROOM_HLE_INTERNAL_H */
