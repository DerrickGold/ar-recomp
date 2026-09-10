#ifndef SIM_WORLD_NAVIGATION_MOUNTAIN_TRANSITION_H
#define SIM_WORLD_NAVIGATION_MOUNTAIN_TRANSITION_H

#include "sim_world_navigation_mountains.h"

enum { kSimWorldNavigationMountainTransitionAxis = kSimWorldMapTiles + 1 };
typedef struct SimWorldNavigationMountainMaterialPatch {
  uint16_t cell;
  /* Native rock RGB with blend weight in alpha, not a transparent surface. */
  uint32_t pixels[kSimTownCellPixels * kSimTownCellPixels];
} SimWorldNavigationMountainMaterialPatch;

/* Presentation-owned, zero initialized. Rebuild on source/geography/chart-radius changes,
 * not on camera movement or water animation. No backend or runner resources. */
typedef struct SimWorldNavigationMountainTransition {
  SimWorldNavigationMountainMaterialPatch *patches;
  size_t patch_count;
  float ridge_scale[kSimWorldNavigationMountainTransitionAxis *
                    kSimWorldNavigationMountainTransitionAxis];
  /* Mesh-constrained boundary rise in physical globe tiles (not relief
   * slider units), plus its four-cell exterior blend. Zero weight is exact
   * fallback. All adjacent town cells must be unoccupied mountain ground. */
  float join_rise[kSimWorldNavigationMountainTransitionAxis *
                  kSimWorldNavigationMountainTransitionAxis];
  float join_weight[kSimWorldNavigationMountainTransitionAxis *
                    kSimWorldNavigationMountainTransitionAxis];
  size_t join_anchor_count;
  /* Lower-only limits for inferred rock just outside continued stamp
   * footprints. Shared footprint vertices stay untouched, so the heightfield
   * cannot be lifted through native slopes. Same physical units/blend rules. */
  float continuation_rise[kSimWorldNavigationMountainTransitionAxis *
                          kSimWorldNavigationMountainTransitionAxis];
  float continuation_weight[kSimWorldNavigationMountainTransitionAxis *
                            kSimWorldNavigationMountainTransitionAxis];
  size_t continuation_anchor_count;
} SimWorldNavigationMountainTransition;

bool SimWorldNavigationMountainTransition_Build(
    const SimWorldNavigationMountainScene *scene,
    const SimWorldNavigationTownGround *ground,
    SimWorldNavigationMountainTransition *out);
/* Same ownership/failure contract as Build, with an explicit globe chart
 * radius matching the native mountain geometry's local metric. */
bool SimWorldNavigationMountainTransition_BuildAtRadius(
    const SimWorldNavigationMountainScene *scene,
    const SimWorldNavigationTownGround *ground, float radius_tiles,
    SimWorldNavigationMountainTransition *out);
void SimWorldNavigationMountainTransition_Destroy(SimWorldNavigationMountainTransition *out);
bool SimWorldNavigationMountainTransition_Apply(
    const SimWorldNavigationMountainTransition *transition, uint32_t *pixels, int pitch,
    const uint8_t *cells); /* NULL for all patches; otherwise kSimWorldMapBytes. */

#endif  /* SIM_WORLD_NAVIGATION_MOUNTAIN_TRANSITION_H */
