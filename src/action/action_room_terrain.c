#include "action_room_terrain.h"
#include "regional/regional_terrain.h"

static uint8_t Read(void *context, ArRegionalTerrainPlane plane, size_t offset) {
  const ActionRoomSceneBg *bg = context;
  return plane == kArRegionalTerrain_Map ? bg->map[offset] : bg->metatiles[offset];
}
static void Write(void *context, ArRegionalTerrainPlane plane, size_t offset, uint8_t byte) {
  ActionRoomSceneBg *bg = context;
  if (plane == kArRegionalTerrain_Map) bg->map[offset] = byte;
  else bg->metatiles[offset] = byte;
}
bool ActionRoomTerrain_Project(ActionRoomScene *scene, uint8_t profile) {
  if (!scene || !scene->bg[0].have_map || !scene->bg[0].have_metatiles) return false;
  ActionRoomSceneBg *bg = &scene->bg[0];
  if (bg->map_size != (size_t)bg->pages_wide*bg->pages_high*256 ||
      bg->map_size > sizeof(bg->map)) return false;
  const ArRegionalTerrainStorage storage = {bg,Read,Write,bg->pages_wide,bg->pages_high};
  return ArRegionalTerrain_Project(profile,(uint16_t)(scene->group | scene->map<<8),&storage);
}
