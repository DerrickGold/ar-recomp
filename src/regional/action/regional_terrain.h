#ifndef AR_REGIONAL_TERRAIN_H
#define AR_REGIONAL_TERRAIN_H

#include "regional/regional_source.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ArRegionalTerrainDescriptor {
  const char *key;
  uint16_t profile[kArRegionalSource_Count];
} ArRegionalTerrainDescriptor;
const ArRegionalTerrainDescriptor *ArRegionalTerrain_Descriptor(void);
bool ArRegionalTerrain_Resolve(ArRegionalSource source, uint8_t *profile);

/* Two bounded byte planes: page-ordered map IDs and 2048 bytes of big-endian
 * metatile definitions. The caller owns storage, endianness conversion and
 * synchronization. Callbacks must remain stable throughout this room-load
 * operation and writes must not fail. No CPU, renderer or ROM ownership here. */
typedef enum ArRegionalTerrainPlane {
  kArRegionalTerrain_Map,
  kArRegionalTerrain_Definitions
} ArRegionalTerrainPlane;
typedef struct ArRegionalTerrainStorage {
  void *context;
  uint8_t (*read)(void *, ArRegionalTerrainPlane, size_t);
  void (*write)(void *, ArRegionalTerrainPlane, size_t, uint8_t);
  unsigned pages_wide, pages_high;
} ArRegionalTerrainStorage;

/* Validates BOTH complete planes before any writes. Accepts independently
 * reloaded US or previously projected planes, including mixed US/JP/EU
 * generations after a native partial asset load. Reversible and idempotent.
 * Invalid inputs/signatures return false without writing. scene is room-high,
 * area-low. No allocation or per-frame work. */
bool ArRegionalTerrain_Project(uint8_t profile, uint16_t scene,
                               const ArRegionalTerrainStorage *storage);
bool ArRegionalTerrain_HasScene(uint16_t scene);

/* Coordinates are 16-pixel map cells. The Fillmore entry and its wave retry
 * anchor travel with the layout; other room coordinates stay native. */
uint8_t ArRegionalTerrain_FillmoreStartY(uint8_t profile);
uint8_t ArRegionalTerrain_FillmoreCheckpointY(uint8_t profile);

#endif
