#ifndef SIM_STRUCTURE_VISUALS_H
#define SIM_STRUCTURE_VISUALS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The native structure step machine publishes presentation by copying one
 * metatile from the structure atlas into a plot. These are the four families
 * whose authentic art the enhanced town replaces with authored geometry. */
typedef enum SimStructureVisualFamily {
  kSimStructureVisual_House,
  kSimStructureVisual_Bridge,
  kSimStructureVisual_Windmill,
  kSimStructureVisual_Factory,
  kSimStructureVisualFamilyCount,
} SimStructureVisualFamily;

/* Common presentation state shared by every replaced structure family.
 * Animation phase remains separate: a finished windmill can occupy any of
 * three blade positions without becoming a construction frame. */
typedef enum SimStructureVisualState {
  kSimStructureVisualState_Unknown,
  kSimStructureVisualState_Finished,
  kSimStructureVisualState_Construction0,
  kSimStructureVisualState_Construction1,
  kSimStructureVisualState_Construction2,
  kSimStructureVisualStateCount,
} SimStructureVisualState;

typedef struct SimStructureVisualFrame {
  uint8_t metatile;
  uint8_t state;
  uint8_t animation_phase;
} SimStructureVisualFrame;

/* ROM-derived catalog of the top-left metatiles drawn by the construction and
 * rebuild program families rooted at $03:D4D2/$03:D4E2. The returned storage
 * is static. */
const SimStructureVisualFrame *SimStructureVisuals_Frames(
    SimStructureVisualFamily family, size_t *count);

bool SimStructureVisuals_IsConstruction(uint8_t state);
const char *SimStructureVisuals_FamilyName(SimStructureVisualFamily family);

#endif  /* SIM_STRUCTURE_VISUALS_H */
