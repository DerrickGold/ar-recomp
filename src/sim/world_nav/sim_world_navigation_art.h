#ifndef AR_SIM_WORLD_NAVIGATION_ART_H
#define AR_SIM_WORLD_NAVIGATION_ART_H
/* SimWorldNavigationArt: builds the 2048-pixel world navigation texture from
 * the current developed world map (Scale2x), blends each town's native ground
 * into that live map at its boundary, and redraws only animated dirty rows.
 * Phase: pure.
 * Tests: tests/sim_world_navigation_art_test.c */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sim/sim_world_map.h"
#include "sim/world_nav/sim_world_navigation_towns.h"

enum {
  kSimWorldNavigationArtScale = 2,
  kSimWorldNavigationArtPixels =
      kSimWorldMapPixels * kSimWorldNavigationArtScale,
  /* Keep the central 24x24 town cells fully detailed. */
  kSimWorldNavigationTownFeatherPixels = 4 * kSimWorldMapTilePixels,
};

/* Apply deterministic Scale2x to current developed art. The overlay below
 * blends native materials into this live base, preserving current development
 * and cleansed water. Produces a real 2048-square navigation texture
 * using a bounded three-row working set, with no scratch heap allocation. */
bool SimWorldNavigationArt_Build(
    uint32_t *out_pixels, int out_pitch_pixels,
    const uint32_t *developed_pixels, int developed_pitch_pixels);

/* Replaces ground-facing town artwork at its native 16px/cell resolution.
 * Locked rings retain native shapes; Northwall's ring snow uses town colours.
 * Mountain art is excluded: its drawn silhouette is not a top-down material.
 * Cliff art is included only when the caller supplies owned-corner geometry
 * with closed skirts (`cliff_geometry`). Model cells receive clean ground,
 * or retain the overview glyph when models are disabled. Each owned cell
 * receives complete clean ground even at the town border. Other cells blend
 * into the live overview over four boundary cells; interiors stay native.
 * Unreplaced tree bundles remain present; suppression follows model ownership.
 * With detailed_ground disabled, only model-owned cells are overlaid.
 * Allocation/source failure leaves the caller's existing atlas unchanged. */
bool SimWorldNavigationArt_OverlayTownGround(
    uint32_t *out_pixels, int out_pitch_pixels,
    const SimWorldNavigationTownGround *ground, bool detailed_ground,
    bool models_enabled, bool cliff_geometry,
    uint8_t animation_phase);

typedef struct SimWorldNavigationArtChanges {
  uint8_t cells[kSimWorldMapBytes];
} SimWorldNavigationArtChanges;

/* Prepared pixel work, not a renderer resource. Preparation resolves borrowed
 * atlas tiles on their owner before execution. Keep all input pixels and the
 * town-art module unchanged until every requested row range has completed;
 * do not retain the plan across source updates/reset. Output must not alias
 * inputs. Disjoint world-cell row ranges write disjoint output pixel rows,
 * without atlas lookups, allocation, global mutation or thread dependencies.
 * Caller owns this fixed ~280 KiB value and may reuse its storage. */
typedef struct SimWorldNavigationArtAnimation {
  uint32_t *output;
  const uint32_t *developed;
  int output_pitch, developed_pitch;
  SimWorldNavigationArtChanges changes;
  struct {
    const uint32_t *pixels;
    uint8_t x, y;
    bool model_owned;
  } overlay[kSimWorldMapBytes];
  /* Owned native ring art with the town snow palette; pointers in overlay
   * remain valid throughout every worker row range in this prepared plan. */
  uint32_t northwall_ring[4][kSimTownCellPixels * kSimTownCellPixels];
  unsigned feather[kSimTownCells * kSimTownCellPixels];
  bool ready;
} SimWorldNavigationArtAnimation;

bool SimWorldNavigationArt_PrepareAnimation(
    SimWorldNavigationArtAnimation *work,
    uint32_t *out_pixels, int out_pitch_pixels,
    const uint32_t *developed_pixels, int developed_pitch_pixels,
    const uint8_t *world_cells,
    const SimWorldNavigationTownGround *ground, bool detailed_ground,
    bool models_enabled, bool cliff_geometry,
    uint8_t previous_phase, uint8_t animation_phase);
/* Empty/out-of-range or unprepared requests do nothing. Ranges are [first,end)
 * within kSimWorldMapTiles, not pixel rows. Publication belongs to the caller. */
void SimWorldNavigationArt_RenderAnimationRows(
    const SimWorldNavigationArtAnimation *work, size_t first, size_t end);

/* Animation-only update of an already composed atlas. Geography and model/
 * cliff gates must match its full bake. world_cells optionally identifies
 * changed overview cells (including Scale2x neighbours); NULL means only
 * native town animation changed. Ground may be NULL when both detailed ground
 * and models are disabled; model cleanup remains active without ground detail.
 * Rebuilds the original Scale2x under each changed tile before reapplying town
 * art, so blending and transparent phases cannot accumulate stale pixels.
 * Reports affected world cells without renderer types. Caller must reapply
 * mountain cleanup/material patches to these cells only, then upload them.
 * Invalid/source failure leaves both atlas and change mask untouched. */
bool SimWorldNavigationArt_UpdateAnimation(
    uint32_t *out_pixels, int out_pitch_pixels,
    const uint32_t *developed_pixels, int developed_pitch_pixels,
    const uint8_t *world_cells,
    const SimWorldNavigationTownGround *ground, bool detailed_ground,
    bool models_enabled, bool cliff_geometry,
    uint8_t previous_phase, uint8_t animation_phase,
    SimWorldNavigationArtChanges *changes);

#endif  /* AR_SIM_WORLD_NAVIGATION_ART_H */
