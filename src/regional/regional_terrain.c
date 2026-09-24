#include "regional_terrain.h"

typedef struct TerrainCell { uint16_t offset; uint8_t us, selected; } TerrainCell;
typedef struct TerrainPatch { const TerrainCell *cells; unsigned count; } TerrainPatch;
typedef struct TerrainRoom {
  uint16_t scene;
  uint8_t width, height;
  uint32_t map_hash[3], definition_hash[2];
  TerrainPatch patches[3]; /* JP map, European map, European definitions. */
} TerrainRoom;
#include "regional_terrain_data.inc"

static const ArRegionalTerrainDescriptor kDescriptor = {"terrain_layout", {0,1,2}};
const ArRegionalTerrainDescriptor *ArRegionalTerrain_Descriptor(void) { return &kDescriptor; }
bool ArRegionalTerrain_Resolve(ArRegionalSource source, uint8_t *profile) {
  if (!profile || (unsigned)source >= kArRegionalSource_Count) return false;
  *profile = (uint8_t)kDescriptor.profile[source];
  return true;
}
static const TerrainRoom *Room(uint16_t scene) {
  for (unsigned i = 0; i < sizeof(kRooms)/sizeof(kRooms[0]); ++i)
    if (kRooms[i].scene == scene) return &kRooms[i];
  return NULL;
}
bool ArRegionalTerrain_HasScene(uint16_t scene) { return Room(scene) != NULL; }

static uint32_t Hash(const ArRegionalTerrainStorage *s, ArRegionalTerrainPlane plane, size_t count) {
  uint32_t hash = UINT32_C(2166136261);
  for (size_t i = 0; i < count; ++i)
    hash = (hash ^ s->read(s->context,plane,i)) * UINT32_C(16777619);
  return hash;
}
static void Patch(const ArRegionalTerrainStorage *s, ArRegionalTerrainPlane plane,
                  const TerrainPatch *patch, bool selected) {
  for (unsigned i = 0; i < patch->count; ++i) {
    const TerrainCell *c = &patch->cells[i];
    s->write(s->context,plane,c->offset,selected ? c->selected : c->us);
  }
}
bool ArRegionalTerrain_Project(uint8_t profile, uint16_t scene,
                               const ArRegionalTerrainStorage *s) {
  const TerrainRoom *r = Room(scene);
  if (profile >= 3 || !r || !s || !s->read || !s->write ||
      s->pages_wide != r->width || s->pages_high != r->height) return false;
  const uint32_t map = Hash(s,kArRegionalTerrain_Map,(size_t)r->width*r->height*256);
  const uint32_t defs = Hash(s,kArRegionalTerrain_Definitions,2048);
  unsigned current = 0;
  while (current < 3 && map != r->map_hash[current]) ++current;
  if (current == 3 || (defs != r->definition_hash[0] && defs != r->definition_hash[1]))
    return false;
  if (map != r->map_hash[profile]) {
    if (current) Patch(s,kArRegionalTerrain_Map,&r->patches[current-1],false);
    if (profile) Patch(s,kArRegionalTerrain_Map,&r->patches[profile-1],true);
  }
  const unsigned target_defs = profile == 2;
  if (defs != r->definition_hash[target_defs])
    Patch(s,kArRegionalTerrain_Definitions,&r->patches[2],target_defs != 0);
  return true;
}
uint8_t ArRegionalTerrain_FillmoreStartY(uint8_t profile) { return profile == 1 ? 27 : 34; }
uint8_t ArRegionalTerrain_FillmoreCheckpointY(uint8_t profile) { return profile == 1 ? 25 : 23; }
