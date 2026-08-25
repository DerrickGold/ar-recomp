#include "sim_structure_visuals.h"

/* House visual class 0 has eight regional/development art rows. Each row has
 * two shared scaffold frames and two possible completed facings. The $40
 * record bit selects the fourth entry; it never selects construction. */
static const SimStructureVisualFrame kHouseFrames[] = {
  {0x00, kSimStructureVisualState_Construction0, 0},
  {0x01, kSimStructureVisualState_Construction1, 1},
  {0x02, kSimStructureVisualState_Finished, 0},
  {0x03, kSimStructureVisualState_Finished, 0},
  {0x08, kSimStructureVisualState_Construction0, 0},
  {0x09, kSimStructureVisualState_Construction1, 1},
  {0x0A, kSimStructureVisualState_Finished, 0},
  {0x0B, kSimStructureVisualState_Finished, 0},
  {0x10, kSimStructureVisualState_Construction0, 0},
  {0x11, kSimStructureVisualState_Construction1, 1},
  {0x12, kSimStructureVisualState_Finished, 0},
  {0x13, kSimStructureVisualState_Finished, 0},
  {0x18, kSimStructureVisualState_Construction0, 0},
  {0x19, kSimStructureVisualState_Construction1, 1},
  {0x1A, kSimStructureVisualState_Finished, 0},
  {0x1B, kSimStructureVisualState_Finished, 0},
  {0x20, kSimStructureVisualState_Construction0, 0},
  {0x21, kSimStructureVisualState_Construction1, 1},
  {0x22, kSimStructureVisualState_Finished, 0},
  {0x23, kSimStructureVisualState_Finished, 0},
  {0x28, kSimStructureVisualState_Construction0, 0},
  {0x29, kSimStructureVisualState_Construction1, 1},
  {0x2A, kSimStructureVisualState_Finished, 0},
  {0x2B, kSimStructureVisualState_Finished, 0},
  {0x30, kSimStructureVisualState_Construction0, 0},
  {0x31, kSimStructureVisualState_Construction1, 1},
  {0x32, kSimStructureVisualState_Finished, 0},
  {0x33, kSimStructureVisualState_Finished, 0},
  {0x38, kSimStructureVisualState_Construction0, 0},
  {0x39, kSimStructureVisualState_Construction1, 1},
  {0x3A, kSimStructureVisualState_Finished, 0},
  {0x3B, kSimStructureVisualState_Finished, 0},
};

/* Bridge visual class 2. Variants 0/4 are the first construction stage and
 * 2/6 the completed bridge; +8 selects Northwall's ice art. */
static const SimStructureVisualFrame kBridgeFrames[] = {
  {0x4C, kSimStructureVisualState_Construction0, 0},
  {0x44, kSimStructureVisualState_Finished, 0},
  {0x4D, kSimStructureVisualState_Construction0, 0},
  {0x45, kSimStructureVisualState_Finished, 0},
  {0xEA, kSimStructureVisualState_Construction0, 0},
  {0xE2, kSimStructureVisualState_Finished, 0},
  {0xEB, kSimStructureVisualState_Construction0, 0},
  {0xE3, kSimStructureVisualState_Finished, 0},
};

/* Windmill visual class 6. The construction program lands on $16, which is
 * already the third completed blade position. */
static const SimStructureVisualFrame kWindmillFrames[] = {
  {0x04, kSimStructureVisualState_Construction0, 0},
  {0x06, kSimStructureVisualState_Construction1, 1},
  {0x14, kSimStructureVisualState_Construction2, 2},
  {0x24, kSimStructureVisualState_Finished, 0},
  {0x26, kSimStructureVisualState_Finished, 1},
  {0x16, kSimStructureVisualState_Finished, 2},
};

/* Factory visual class 8 has one scaffold and one completed frame. */
static const SimStructureVisualFrame kFactoryFrames[] = {
  {0x34, kSimStructureVisualState_Construction0, 0},
  {0x36, kSimStructureVisualState_Finished, 0},
};

const SimStructureVisualFrame *SimStructureVisuals_Frames(
    SimStructureVisualFamily family, size_t *count) {
  if (count) *count = 0;
  switch (family) {
    case kSimStructureVisual_House:
      if (count) *count = sizeof(kHouseFrames) / sizeof(kHouseFrames[0]);
      return kHouseFrames;
    case kSimStructureVisual_Bridge:
      if (count) *count = sizeof(kBridgeFrames) / sizeof(kBridgeFrames[0]);
      return kBridgeFrames;
    case kSimStructureVisual_Windmill:
      if (count)
        *count = sizeof(kWindmillFrames) / sizeof(kWindmillFrames[0]);
      return kWindmillFrames;
    case kSimStructureVisual_Factory:
      if (count) *count = sizeof(kFactoryFrames) / sizeof(kFactoryFrames[0]);
      return kFactoryFrames;
    case kSimStructureVisualFamilyCount:
      break;
  }
  return NULL;
}

bool SimStructureVisuals_IsConstruction(uint8_t state) {
  return state >= kSimStructureVisualState_Construction0 &&
      state <= kSimStructureVisualState_Construction2;
}

const char *SimStructureVisuals_FamilyName(SimStructureVisualFamily family) {
  switch (family) {
    case kSimStructureVisual_House: return "house";
    case kSimStructureVisual_Bridge: return "bridge";
    case kSimStructureVisual_Windmill: return "windmill";
    case kSimStructureVisual_Factory: return "factory";
    case kSimStructureVisualFamilyCount: break;
  }
  return "unknown";
}
