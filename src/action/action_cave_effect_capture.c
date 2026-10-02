/* Stage-owned environmental capture. Phase: capture; read-only WRAM. */
#include "action/action_environment_capture_internal.h"
#include "action_water_field.h"
#include "action_landing_dust.h"

unsigned FillmoreCaveRoom(const uint8_t *wram, size_t size) {
  if (!wram || size <= kActRaiserWram_Bg2Height + 1 ||
      Read8(wram, size, kActRaiserWram_MapGroup) != kActRaiserMapGroup_Fillmore)
    return 0;
  const unsigned room = Read8(wram, size, kActRaiserWram_CurrentMap);
  if (room < 2 || room > 4) return 0;
  static const unsigned dimensions[][4] = {
    {2048,1280,2048,1280}, {1024,1792,256,512}, {512,256,256,256},
  };
  const unsigned *d = dimensions[room - 2];
  return Read16(wram, size, kActRaiserWram_Bg1Width) == d[0] &&
      Read16(wram, size, kActRaiserWram_Bg1Height) == d[1] &&
      Read16(wram, size, kActRaiserWram_Bg2Width) == d[2] &&
      Read16(wram, size, kActRaiserWram_Bg2Height) == d[3] ? room : 0;
}

static bool CaveMapReady(const ActionBgMapView *map, unsigned room) {
  /* Distinct stone/ceiling pairs in the decoded maps, not ROM addresses or
   * palette guesses. Reject a transition's inherited or incomplete map. */
  static const unsigned signatures[][6] = {
    {326,352,0xB8, 420,432,0xB9},
    {896,128,0x39, 448,1664,0x26},
    {80,80,0x08, 96,192,0x26},
  };
  const unsigned *s = signatures[room - 2];
  uint8_t a, b;
  return ActionBgMapView_LookupMetatile(map, (int)s[0], (int)s[1], &a) && a == s[2] &&
      ActionBgMapView_LookupMetatile(map, (int)s[3], (int)s[4], &b) && b == s[5];
}

void CaptureFillmoreCaveScene(const ActionEnvironmentScene *scene, ActionSceneEffectFrame *dst) {
  const unsigned room = scene->room;
  if (scene->group != kActRaiserMapGroup_Fillmore || room < 2 || room > 4) return;
  static const unsigned dimensions[][4] = {
    {2048,1280,2048,1280}, {1024,1792,256,512}, {512,256,256,256},
  };
  const unsigned *d = dimensions[room-2];
  const unsigned width = scene->maps[0].world_width, height = scene->maps[0].world_height;
  if (width != d[0] || height != d[1] || scene->maps[1].world_width != d[2] ||
      scene->maps[1].world_height != d[3]) return;
  if (!CaveMapReady(&scene->maps[0],room)) return;
  if (room == 2) {
    uint8_t pool, fall;
    if (!ActionBgMapView_LookupMetatile(&scene->maps[1],0,896,&pool) || pool != 1 ||
        !ActionBgMapView_LookupMetatile(&scene->maps[1],720,0,&fall) || fall != 2) return;
  }
  if(room==2&&!scene->suppress_default_water_field)
    ActionWaterField_Capture(scene,dst,ActionWaterField_Bundled(),0xC2000200u);
  if(!scene->suppress_default_atmosphere_field)
    ActionAtmosphereField_Capture(scene,dst,ActionAtmosphereField_Bundled(room),0xC2000000u|(room<<8));
}

void CaptureFillmoreCaveFiltered(ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *ram, size_t size, bool native_water, bool native_atmosphere) {
  ActionEnvironmentScene scene;
  if (!ActionEnvironmentScene_FromWram(&scene,ram,size,observer->scene_clock) ||
      !FillmoreCaveRoom(ram,size) || observer->scene_map_number != scene.room ||
      !CaveMapReady(&scene.maps[0],scene.room)) {
    memset(&observer->landing_dust,0,sizeof(observer->landing_dust)); return;
  }
  if (scene.room == 2) {
    uint8_t pool, fall;
    if (!ActionBgMapView_LookupMetatile(&scene.maps[1],0,896,&pool) || pool != 1 ||
        !ActionBgMapView_LookupMetatile(&scene.maps[1],720,0,&fall) || fall != 2) {
      memset(&observer->landing_dust,0,sizeof(observer->landing_dust)); return;
    }
  }
  ActionLandingDust_Capture(&observer->landing_dust,dst,ram,size,&scene.maps[0],
      scene.room,scene.clock);
  scene.suppress_default_water_field=!native_water;
  scene.suppress_default_atmosphere_field=!native_atmosphere;
  CaptureFillmoreCaveScene(&scene,dst);
}

void CaptureFillmoreCave(ActionEffectObserver *observer,ActionSceneEffectFrame *dst,
    const uint8_t *ram,size_t size) {
  CaptureFillmoreCaveFiltered(observer,dst,ram,size,true,true);
}
