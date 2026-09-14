#include "actraiser/actraiser_event_bugfixes.h"

#include <limits.h>

enum {
  /* Full linear WRAM offsets. Bank $7F begins at $10000. */
  kCurrentTownIndex = 0x17BFB,
  kEventPrerequisiteBase = 0x19107,
  kEventFiredBase = 0x1911F,
  kEventDispatchedBase = 0x19137,
  kPendingEventLatch = 0x1920E,

  /* $7BFB is the zero-based town index doubled for ROM pointer tables. */
  kAitosEncodedTownIndex = 6,
  kTownEventBitmapBytes = 4,
  kAitosAllMonstersEvent = 1,
  kAitosMountainEvent = 4,
  kForcedEventBit = 0x80,
};

static uint16_t Read16(const uint8_t *wram, size_t offset) {
  return (uint16_t)(wram[offset] | ((uint16_t)wram[offset + 1] << CHAR_BIT));
}

static bool EventBitSet(const uint8_t *wram, size_t bitmap_base,
                        unsigned town, unsigned event_id) {
  const size_t byte = bitmap_base + town * kTownEventBitmapBytes +
                      event_id / CHAR_BIT;
  const uint8_t mask = (uint8_t)(0x80u >> (event_id % CHAR_BIT));
  return (wram[byte] & mask) != 0;
}

bool ActRaiser_RepairAitosEventQueueCollision(uint8_t *wram,
                                               size_t wram_size) {
  if (!wram || wram_size <= kPendingEventLatch ||
      wram_size <= kCurrentTownIndex + 1)
    return false;

  const uint16_t encoded_town_index = Read16(wram, kCurrentTownIndex);
  if (encoded_town_index != kAitosEncodedTownIndex ||
      wram[kPendingEventLatch] !=
          (kForcedEventBit | kAitosAllMonstersEvent))
    return false;

  const unsigned town = encoded_town_index / 2;
  if (!EventBitSet(wram, kEventFiredBase, town,
                   kAitosAllMonstersEvent) ||
      !EventBitSet(wram, kEventPrerequisiteBase, town,
                   kAitosMountainEvent) ||
      EventBitSet(wram, kEventFiredBase, town, kAitosMountainEvent) ||
      !EventBitSet(wram, kEventDispatchedBase, town,
                   kAitosMountainEvent))
    return false;

  /* $03:DFFA treats bit 7 as an unconditional replay request. Clearing the
   * stale request lets the unchanged selector scan its native event bitmaps. */
  wram[kPendingEventLatch] = 0;
  return true;
}
