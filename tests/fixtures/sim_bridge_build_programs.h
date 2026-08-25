#ifndef TESTS_FIXTURES_SIM_BRIDGE_BUILD_PROGRAMS_H
#define TESTS_FIXTURES_SIM_BRIDGE_BUILD_PROGRAMS_H

#include <stdint.h>

#include "sim/sim_structure_visuals.h"

/* Exact visual-class-2 producer fixture decoded from ar.sfc
 * SHA-256 b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0.
 * Programs $03:D754-$D77E each hold their draw list for 15 ticks, then stop.
 * This substitutes a deterministic ROM fixture for a live bridge-build run;
 * both semantic marker axes and Northwall's +8 ice variants are represented. */
typedef struct SimBridgeBuildProgramFixture {
  uint8_t town;
  uint8_t marker;
  uint8_t metatile;
  uint8_t state;
  uint16_t program;
  uint16_t draw_list;
} SimBridgeBuildProgramFixture;

static const SimBridgeBuildProgramFixture kBridgeBuildProgramFrames[] = {
  {1, 0xE2, 0x4C, kSimStructureVisualState_Construction0, 0xD754, 0xDC18},
  {1, 0xE2, 0x44, kSimStructureVisualState_Finished,      0xD75A, 0xDC1C},
  {1, 0xE1, 0x4D, kSimStructureVisualState_Construction0, 0xD760, 0xDC20},
  {1, 0xE1, 0x45, kSimStructureVisualState_Finished,      0xD766, 0xDC24},
  {6, 0xE2, 0xEA, kSimStructureVisualState_Construction0, 0xD76C, 0xDC28},
  {6, 0xE2, 0xE2, kSimStructureVisualState_Finished,      0xD772, 0xDC2C},
  {6, 0xE1, 0xEB, kSimStructureVisualState_Construction0, 0xD778, 0xDC30},
  {6, 0xE1, 0xE3, kSimStructureVisualState_Finished,      0xD77E, 0xDC34},
};

#endif  /* TESTS_FIXTURES_SIM_BRIDGE_BUILD_PROGRAMS_H */
