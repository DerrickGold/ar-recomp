#ifndef SIM_WORLD_NAVIGATION_ART_H
#define SIM_WORLD_NAVIGATION_ART_H

#include <stdbool.h>
#include <stdint.h>

#include "sim_world_map.h"
#include "sim_world_navigation_towns.h"

enum {
  kSimWorldNavigationArtScale = 2,
  kSimWorldNavigationArtPixels =
      kSimWorldMapPixels * kSimWorldNavigationArtScale,
  /* Four world cells is wide enough to hide the square composition stamp,
   * while leaving the central 24x24 cells of every town fully authored. */
  kSimWorldNavigationTownFeatherPixels =
      4 * kSimWorldMapTilePixels,
};

/* Contribution of developed-town art at one 1024-space pixel. Outside all
 * six town windows the developed map is already the world and returns 1. */
float SimWorldNavigationArt_TownWeight(int source_x, int source_y);

/* Feather compatible boundary shades, preserving live material/state changes,
 * then apply deterministic Scale2x. The pristine map cannot repaint cleansed
 * water or reclaimed land. Produces a real 2048-square navigation texture
 * using a bounded three-row working set, with no scratch heap allocation. */
bool SimWorldNavigationArt_Build(
    uint32_t *out_pixels, int out_pitch_pixels,
    const uint32_t *developed_pixels, int developed_pitch_pixels,
    const uint32_t *baseline_pixels, int baseline_pitch_pixels);

/* Replaces ground-facing town artwork at its native 16px/cell resolution.
 * Mountain art is excluded: its drawn silhouette is not a top-down material.
 * Cliff art is included only when the caller supplies owned-corner geometry
 * with closed skirts (`cliff_geometry`). Model cells receive clean ground,
 * or retain the overview glyph when models are disabled. Feathering uses only
 * the current developed map below, never the pristine/uncleansed baseline.
 * Allocation/source failure leaves the caller's existing atlas unchanged. */
bool SimWorldNavigationArt_OverlayTownGround(
    uint32_t *out_pixels, int out_pitch_pixels,
    const SimWorldNavigationTownGround *ground, bool models_enabled, bool cliff_geometry,
    uint8_t animation_phase);

typedef struct SimWorldNavigationArtChanges {
  uint8_t cells[kSimWorldMapBytes];
} SimWorldNavigationArtChanges;

/* Animation-only update of an already composed atlas. Geography and model/
 * cliff gates must match its full bake. world_cells optionally identifies
 * changed overview cells (including Scale2x neighbours); NULL means only
 * native town animation changed. Ground may be NULL when detail is disabled.
 * Rebuilds the original Scale2x under each changed tile before feathering, so
 * alpha changes and repeated phase cycles cannot accumulate blending error.
 * Reports affected world cells without renderer types. Caller must reapply
 * mountain cleanup/material patches to these cells only, then upload them.
 * Invalid/source failure leaves both atlas and change mask untouched. */
bool SimWorldNavigationArt_UpdateAnimation(
    uint32_t *out_pixels, int out_pitch_pixels,
    const uint32_t *developed_pixels, int developed_pitch_pixels,
    const uint32_t *baseline_pixels, int baseline_pitch_pixels,
    const uint8_t *world_cells,
    const SimWorldNavigationTownGround *ground, bool models_enabled, bool cliff_geometry,
    uint8_t previous_phase, uint8_t animation_phase,
    SimWorldNavigationArtChanges *changes);

#endif  /* SIM_WORLD_NAVIGATION_ART_H */
