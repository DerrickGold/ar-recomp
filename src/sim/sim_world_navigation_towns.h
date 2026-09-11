#ifndef SIM_WORLD_NAVIGATION_TOWNS_H
#define SIM_WORLD_NAVIGATION_TOWNS_H

#include <stdbool.h>
#include <stdint.h>

#include "sim_background_voxel_types.h"
#include "sim_world_map.h"

enum {
  /* Capture every semantic object. Distance/detail selection belongs to the
   * renderer, so approaching a forest restores the same per-cell trees used
   * inside the town instead of enlarging a handful of replacement shapes. */
  kSimWorldNavigationTownObjectCapacity = 6 * kSimBackgroundMaxObjects,
};

enum {
  /* Navigation-only LOD materials that have no full-town voxel counterpart.
   * Keep them above the shared kind range so ordinary structures retain the
   * exact same identity in both renderers. */
  kSimWorldNavigationTown_Field = kSimBackgroundVoxelKindCount,
  kSimWorldNavigationTown_Support,
};

/* The actual town model identity, including regional/tier variations,
 * connected foliage, source-vs-footprint perspective, and bridge banks. */
typedef SimBackgroundVoxelObject SimWorldNavigationTownObject;

typedef struct SimWorldNavigationTownGround {
  uint8_t enabled_town_mask;
  uint8_t development_tier[kSimTownCount];
  /* Row-major semantic cells, not a WRAM alias or a projected screenshot. */
  uint8_t terrain[kSimTownCount][kSimTownCells * kSimTownCells];
  /* Source cells replaced by actual models. Bits name x within each row.
   * Animated model poses do not invalidate this stable ground ownership. */
  uint32_t object_rows[kSimTownCount][kSimTownCells];
} SimWorldNavigationTownGround;

typedef struct SimWorldNavigationTowns {
  uint16_t object_count;
  uint8_t enabled_town_mask;
  bool overflow;
  SimWorldNavigationTownGround ground;
  SimWorldNavigationTownObject
      objects[kSimWorldNavigationTownObjectCapacity];
} SimWorldNavigationTowns;

/* Captures a low/medium-distance semantic scene for every developed town from
 * the same all-town cell maps and structure records used to compose the Mode-7
 * world. It deliberately does not inspect the active town's tile graphics:
 * those are resident for only one town, whereas navigation must publish all
 * six in one immutable frame. */
void SimWorldNavigationTowns_Capture(
    const uint8_t *wram, SimWorldNavigationTowns *out);

/* Producer-owner-thread memoized form. Compares all classification inputs by
 * value, never retains WRAM pointers, and always copies a complete output.
 * The pure Capture entry point remains available for independent callers and
 * reference validation. One bounded application cache is shared by the two
 * mutually exclusive globe modes; it is not part of the frame/runner ABI. */
void SimWorldNavigationTowns_CaptureCached(
    const uint8_t *wram, SimWorldNavigationTowns *out);
void SimWorldNavigationTowns_ResetCache(void);

#endif  /* SIM_WORLD_NAVIGATION_TOWNS_H */
