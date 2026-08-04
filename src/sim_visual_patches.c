#include "sim_visual_patches.h"

enum {
  /* LoROM $01:A838. The three entries are [duration, composition word], then
   * FE 01 loops back to the script base. */
  kHouseFireScriptOffset = 0xA838,
  kHouseFireFrameCount = 3,
  kHouseFireFrameBytes = 3,
  kHouseFireLoopOffset = 0xA841,
  kHouseFireSourceDuration = 1,
  /* Four 60 Hz logic ticks per source frame gives the tiny three-frame flame
   * a readable 15 fps flicker without changing its actor-owned lifetime. */
  kHouseFireAdjustedDuration = 4,
};

static uint16_t ReadLe16(const uint8_t *p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

bool SimVisualPatches_Apply(uint8_t *rom_data, size_t rom_size) {
  static const uint16_t kHouseFireCompositions[kHouseFireFrameCount] = {
    0xDD2D, 0xDD33, 0xDD39,
  };
  if (!rom_data || rom_size <= kHouseFireLoopOffset + 1)
    return false;

  /* Validate the whole loop before changing one byte. Accepting the adjusted
   * duration as well makes the operation idempotent, while any other value or
   * composition fails closed rather than guessing at a different ROM. */
  for (size_t i = 0; i < kHouseFireFrameCount; i++) {
    size_t offset = kHouseFireScriptOffset + i * kHouseFireFrameBytes;
    uint8_t duration = rom_data[offset];
    if ((duration != kHouseFireSourceDuration &&
         duration != kHouseFireAdjustedDuration) ||
        ReadLe16(&rom_data[offset + 1]) != kHouseFireCompositions[i])
      return false;
  }
  if (rom_data[kHouseFireLoopOffset] != 0xFE ||
      rom_data[kHouseFireLoopOffset + 1] != 0x01)
    return false;

  for (size_t i = 0; i < kHouseFireFrameCount; i++)
    rom_data[kHouseFireScriptOffset + i * kHouseFireFrameBytes] =
        kHouseFireAdjustedDuration;
  return true;
}
