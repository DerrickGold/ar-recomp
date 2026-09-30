#ifndef AR_SIM_WORLD_NAVIGATION_TOWNS_H
#define AR_SIM_WORLD_NAVIGATION_TOWNS_H
/* SimWorldNavigationTowns: captures all six towns' terrain and model objects.
 * Developed towns use retained cell maps and structure records; locked towns
 * use immutable initial terrain. The cached form skips unchanged inputs.
 * Phase: capture (reads WRAM).
 * Tests: tests/sim_render_metadata_test.c */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sim/voxels/sim_background_voxel_types.h"
#include "sim/sim_world_map.h"

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
  /* Terrain availability, independent of campaign development/unlock state. */
  uint8_t enabled_town_mask;
  uint8_t development_tier[kSimTownCount];
  /* Row-major semantic cells, not a WRAM alias or a projected screenshot. */
  uint8_t terrain[kSimTownCount][kSimTownCells * kSimTownCells];
  /* Locked action rings retain their native world artwork. Northwall's ring
   * snow adopts the town palette; surrounding cells use ordinary town art. */
  uint32_t native_rows[kSimTownCount][kSimTownCells];
  /* Source cells replaced by actual models. Bits name x within each row.
   * Animated model poses do not invalidate this stable ground ownership. */
  uint32_t object_rows[kSimTownCount][kSimTownCells];
} SimWorldNavigationTownGround;

typedef struct SimWorldNavigationTowns {
  uint16_t object_count;
  /* Towns with live development. Natural objects may also belong to locked
   * towns included in ground.enabled_town_mask. */
  uint8_t enabled_town_mask;
  bool overflow;
  SimWorldNavigationTownGround ground;
  SimWorldNavigationTownObject
      objects[kSimWorldNavigationTownObjectCapacity];
} SimWorldNavigationTowns;

/* Copy the ROM's initial terrain for locked towns. Init/shutdown invalidate
 * the capture cache; missing data leaves the ordinary world-map fallback.
 * Call on the producer owner before capture starts/after capture stops. */
bool SimWorldNavigationTowns_Init(const uint8_t *rom, size_t rom_size);
void SimWorldNavigationTowns_Shutdown(void);

/* Captures a low/medium-distance semantic scene using live development or
 * initial terrain. It does not inspect the active town's tile graphics:
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

#endif  /* AR_SIM_WORLD_NAVIGATION_TOWNS_H */
