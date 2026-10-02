/* Stage-owned environmental capture. Phase: capture; read-only WRAM. */
#include "action/action_environment_capture_internal.h"
#include "action_bloodpool_occluders.h"
#include "action_castle_sources.h"


/* Merge scanline runs, then identical runs in adjacent rows. The 768x352
 * window includes the largest extended capture plus its horizontal apron.
 * Reads are bounded by the validated native map/definition table. */
static bool CaptureMoonOccluders(ActionMoonlightOcclusion *dst,
    const ActionBgMapView *map, const ActionEnvironmentScene *scene, unsigned mask, unsigned attributes) {
  const int camera_x = scene->camera_x[0];
  const int camera_y = scene->camera_y[0];
  const int x0 = camera_x > 256 ? (camera_x-256)&~7 : 0;
  const int x1 = camera_x+512 < 4096 ? (camera_x+519)&~7 : 4096;
  const int y0 = camera_y > 64 ? camera_y-64 : 0;
  const int y1 = camera_y+288 < 512 ? camera_y+288 : 512;
  /* Alternating pixels are the worst case; rounding the window to whole CHR
   * tiles can add seven columns. No heap or full-resolution mask is needed. */
  uint16_t runs[2][388];
  uint16_t words[97];
  unsigned previous_count = 0;
  dst->count = 0;
  for (int y = y0; y < y1; y++) {
    if (y == y0 || !(y&7)) {
      for (int x = x0; x < x1; x += 8) {
        uint8_t metatile;
        uint16_t word = 0xFF;
        if (ActionBgMapView_LookupMetatile(map,x,y,&metatile)) {
          const unsigned quadrant = ((unsigned)y&8u)/4+((unsigned)x&8u)/8;
          word = (ActionEnvironmentScene_Word(scene,0,metatile,quadrant)&mask)|attributes;
        }
        words[(x-x0)/8] = word;
      }
    }
    uint16_t *previous = runs[(y-y0)&1], *current = runs[(y-y0+1)&1];
    unsigned count = 0, match = 0;
    int start = -1;
    for (int tile_x = x0; tile_x <= x1; tile_x += 8) {
      unsigned bits = 0;
      if (tile_x < x1) {
        const uint16_t word = words[(tile_x-x0)/8];
        unsigned row = (unsigned)y&7u;
        if (word&0x8000u) row = 7-row;
        if (!(word&0x2000u)) bits = (unsigned)(kBloodpoolTileOpacity[word&255u] >> (row*8))&255u;
        if (word&0x4000u) {
          bits = ((bits&0x55u)<<1)|((bits>>1)&0x55u);
          bits = ((bits&0x33u)<<2)|((bits>>2)&0x33u);
          bits = (bits<<4)|(bits>>4);
          bits &= 255u;
        }
      }
      if (bits == 255u) { if (start < 0) start = tile_x; continue; }
      if (!bits && start < 0) continue;
      for (int pixel = 0; pixel < 8; pixel++) {
        const int x = tile_x+pixel;
        if (bits&(1u<<pixel)) { if (start < 0) start = x; continue; }
        if (start < 0) continue;
        while (match < previous_count && dst->rectangles[previous[match]].x0 < start) match++;
        unsigned index;
        if (match < previous_count && dst->rectangles[previous[match]].x0 == start &&
            dst->rectangles[previous[match]].x1 == x) {
          index = previous[match++];
          dst->rectangles[index].y1 = (int16_t)(y+1);
        } else {
          if (dst->count == kActionMoonlightMaxOccluders) return false;
          index = dst->count++;
          dst->rectangles[index] = (ActionMoonlightOccluder){
            (int16_t)start,(int16_t)y,(int16_t)x,(int16_t)(y+1)};
        }
        if (count == sizeof(runs[0])/sizeof(runs[0][0])) return false;
        current[count++] = (uint16_t)index;
        start = -1;
      }
    }
    previous_count = count;
  }
  dst->valid = true;
  return true;
}

static void CaptureBloodpoolMoon(const ActionEnvironmentScene *scene, ActionSceneEffectFrame *dst,
    const ActionBgMapView *foreground) {
  const ActionBgMapView sky = scene->maps[1];
  uint8_t top, bottom;
  const unsigned mask = scene->word_mask[0], attributes = scene->attributes[0];
  if (dst->decoration_count > kActionSceneDecorationMaxInstances-2 ||
      !scene->metatiles[0] || mask != 0xECFF || attributes != 0x1000 ||
      !ActionBgMapView_LookupMetatile(&sky,112,48,&top) || top != 0x3C ||
      !ActionBgMapView_LookupMetatile(&sky,112,64,&bottom) || bottom != 0x44)
    return;
  if (!CaptureMoonOccluders(&dst->moonlight,foreground,scene,mask,attributes)) {
    dst->moonlight.count = 0;
    dst->moonlight.valid = false;
    return;
  }

}

void CaptureBloodpoolMarshScene(const ActionEnvironmentScene *scene,ActionSceneEffectFrame *dst) {
  if(scene->group!=kActRaiserMapGroup_Bloodpool||scene->room!=1)return;
  if(!scene->suppress_default_marsh_field)
    ActionMarshField_Capture(scene,dst,ActionMarshField_Bundled(),0xB1000000u);
  CaptureBloodpoolMoon(scene,dst,&scene->maps[0]);
  if(!scene->suppress_default_moon_field&&dst->moonlight.valid)
    ActionMoonField_Capture(scene,dst,ActionMoonField_Bundled(),0xB1000002u);
}

void CaptureBloodpoolCastleScene(const ActionEnvironmentScene *scene,ActionSceneEffectFrame *dst) {
  if(scene->group==kActRaiserMapGroup_Bloodpool&&!scene->suppress_default_castle_field)
    ActionCastleField_Capture(scene,dst,ActionCastleField_Bundled(scene->room),0xCA000000u|scene->room);
}

bool IsBloodpoolMarsh(const uint8_t *wram, size_t size) {
  return wram && size > kActRaiserWram_Bg2Height + 1 &&
      Read8(wram, size, kActRaiserWram_MapGroup) == kActRaiserMapGroup_Bloodpool &&
      Read8(wram, size, kActRaiserWram_CurrentMap) == 1 &&
      Read16(wram, size, kActRaiserWram_Bg1Width) == 4096 &&
      Read16(wram, size, kActRaiserWram_Bg1Height) == 512 &&
      Read16(wram, size, kActRaiserWram_Bg2Width) == 256 &&
      Read16(wram, size, kActRaiserWram_Bg2Height) == 256;
}

unsigned BloodpoolCastleRoom(const uint8_t *ram, size_t size) {
  if (!ram || Read8(ram,size,kActRaiserWram_MapGroup) != kActRaiserMapGroup_Bloodpool)
    return 0;
  const unsigned room = Read8(ram,size,kActRaiserWram_CurrentMap);
  if (room < 2 || room > 8) return 0;
  static const uint16_t dimensions[][4] = {
    {768,512,256,256}, {1024,1024,256,256}, {512,512,256,256},
    {1792,1024,1792,1024}, {768,256,256,256}, {1024,1024,256,256}, {256,256,256,256},
  };
  const uint16_t *d = dimensions[room-2];
  return Read16(ram,size,kActRaiserWram_Bg1Width) == d[0] &&
      Read16(ram,size,kActRaiserWram_Bg1Height) == d[1] &&
      Read16(ram,size,kActRaiserWram_Bg2Width) == d[2] &&
      Read16(ram,size,kActRaiserWram_Bg2Height) == d[3] ? room : 0;
}

void CaptureBloodpoolMarsh(ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *ram, size_t size,bool native_moon,bool native_marsh) {
  ActionEnvironmentScene scene;
  if (ActionEnvironmentScene_FromWram(&scene,ram,size,observer->scene_clock) &&
      observer->scene_map_number == scene.room) {
    scene.suppress_default_moon_field=!native_moon;
    scene.suppress_default_marsh_field=!native_marsh;
    CaptureBloodpoolMarshScene(&scene,dst);
  }
}

void CaptureBloodpoolCastle(ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *ram, size_t size,bool native_castle) {
  ActionEnvironmentScene scene;
  if (ActionEnvironmentScene_FromWram(&scene,ram,size,observer->scene_clock) &&
      observer->scene_map_number == scene.room) {
    scene.suppress_default_castle_field=!native_castle;CaptureBloodpoolCastleScene(&scene,dst);
  }
}
