/* Stage-owned environmental capture. Phase: capture; read-only WRAM. */
#include "action/action_environment_capture_internal.h"
#include "action_cave_surface.h"
#include "action_landing_dust.h"
#include "action_floor_support.h"

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

_Static_assert(kActionLandingDustMaxPuffs + 7 + kActionTempleMistMaxSpans <=
                   kActionSceneDecorationMaxInstances,
               "temple floor mist must leave room for landing dust and ambient fields");

static int TempleMistFloor(const ActionEnvironmentScene *scene, int x) {
  /* Search only the lower hall using the same exposed-floor predicate as
   * authored mist and the editor collision overlay. */
  const int top = scene->room == 2 ? 1120 : 1664;
  const int bottom = scene->room == 2 ? 1248 : 1712;
  for (int y=top;y<=bottom;y+=16)
    if (ActionFloorSupport_Cell(scene,x,y)&16) return y;
  return 0;
}

static void CaptureTempleMist(ActionSceneEffectFrame *dst, const ActionEnvironmentScene *scene) {
  const unsigned room=scene->room;
  const uint16_t clock=scene->clock;
  const uint8_t count_before = dst->decoration_count;
  const uint8_t visible_before = dst->decoration_visible_count;
  const int start = room == 2 ? 880 : 512;
  const int end = room == 2 ? 1760 : 960;
  int left = start, floor = 0;
  unsigned spans = 0;
  for (int x = start; x <= end; x += 16) {
    const int next_floor = x < end ? TempleMistFloor(scene,x) : 0;
    if (next_floor == floor) continue;
    if (floor) {
      /* A changed/fragmented map may exceed the cosmetic budget. Omit only
       * this family instead of consuming actor or landing-particle records. */
      if (++spans > kActionTempleMistMaxSpans ||
          dst->decoration_count >= kActionSceneDecorationMaxInstances) {
        dst->decoration_count = count_before;
        dst->decoration_visible_count = visible_before;
        return;
      }
      const ActionEffectInstance effect = {
        .generation = 0xC3000000u | (room << 16) | (unsigned)left,
        .pulse_generation = 0xD3000000u | (room << 16) | (unsigned)left,
        .world_x = (int16_t)left, .world_y = (int16_t)floor, .environment_room = (uint16_t)room,
        .age_ticks = clock, .phase_ticks = clock, .pulse_ticks = clock,
        .kind = kActionEffect_TempleGroundMist, .phase = kActionEffectPhase_CaveEnvironment,
        .flags = kActionEffectFlag_Visible | kActionEffectFlag_ClipToRect,
        .render_layer = kActionEffectRenderLayer_Bg1Mist,
        .projection_plane = kActionEffectProjectionPlane_Bg1,
        .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {0,-26,x-left,0}},
        .clip_rect = {0,-26,x-left,0},
      };
      (void)SceneDecorationAppend(dst, &effect);
    }
    left = x;
    floor = next_floor;
  }
}

_Static_assert(kActionCaveWetSourceCount <= 16, "cave sources must fit the captured mask");

static uint16_t CaveWetSourceMask(const ActionBgMapView *map) {
  uint16_t mask = 0;
  for (unsigned i = 0; i < kActionCaveWetSourceCount; i++) {
    const ActionCaveWetSource *s = &kActionCaveWetSources[i];
    uint8_t tip, landing;
    if (ActionBgMapView_LookupMetatile(map, s->x, s->ceiling_y-1, &tip) &&
        ActionBgMapView_LookupMetatile(map, s->x, s->landing_y, &landing) &&
        tip == s->ceiling_tile && landing == s->landing_tile)
      mask |= (uint16_t)(1u << i);
  }
  return mask;
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
  const ActionBgMapView playfield = scene->maps[0];
  if (!CaveMapReady(&playfield,room)) return;
  if (room == 2) {
    uint8_t pool, fall;
    if (!ActionBgMapView_LookupMetatile(&scene->maps[1],0,896,&pool) || pool != 1 ||
        !ActionBgMapView_LookupMetatile(&scene->maps[1],720,0,&fall) || fall != 2) return;
  }
  const uint16_t wet_sources = room == 2 ? CaveWetSourceMask(&playfield) : 0;
  /* Aggregate fields cap this family at seven records, independent of how
   * many water tiles/emitters exist. Actor slots and their budget are untouched. */
  enum { cave = 1u << 2, temple = 1u << 3, tower = 1u << 4 };
  static const struct {
    uint8_t kind, rooms, layer, plane;
  } fields[] = {
    {kActionEffect_CaveWater, cave,
     kActionEffectRenderLayer_Bg2HighPlane, kActionEffectProjectionPlane_Bg2High},
    {kActionEffect_CaveDrips, cave,
     kActionEffectRenderLayer_WorldOverlay, kActionEffectProjectionPlane_Bg1},
    {kActionEffect_TempleDust, cave | temple | tower,
     kActionEffectRenderLayer_WorldOverlay, kActionEffectProjectionPlane_Bg1},
    {kActionEffect_TowerWindowLight, tower,
     kActionEffectRenderLayer_ForegroundLight, kActionEffectProjectionPlane_Bg1},
    {kActionEffect_CaveMist, cave,
     kActionEffectRenderLayer_WorldDust, kActionEffectProjectionPlane_Bg2High},
    {kActionEffect_CaveSheen, cave,
     kActionEffectRenderLayer_Bg1Plane, kActionEffectProjectionPlane_Bg1},
    {kActionEffect_CaveAmbientLight, cave | temple,
     kActionEffectRenderLayer_ForegroundLight, kActionEffectProjectionPlane_Bg1},
    {kActionEffect_TempleGrit, cave | temple,
     kActionEffectRenderLayer_WorldDust, kActionEffectProjectionPlane_Bg1},
  };
  for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
    if (!(fields[i].rooms & (1u << room))) continue;
    const uint8_t kind = fields[i].kind;
    const bool water = fields[i].plane == kActionEffectProjectionPlane_Bg2High;
    const int x = scene->camera_x[water ? 1 : 0] + 128;
    const int y = scene->camera_y[water ? 1 : 0] - 160;
    const ActionEffectInstance effect = {
      .generation = 0xC2000000u | (room << 8) | kind,
      .pulse_generation = 0xD2000000u | (room << 8) | kind,
      .world_x = (int16_t)x, .world_y = (int16_t)y, .environment_room = (uint16_t)room,
      .age_ticks = scene->clock, .phase_ticks = scene->clock,
      .pulse_ticks = scene->clock,
      .kind = kind, .phase = kActionEffectPhase_CaveEnvironment,
      .source_mask = (kind == kActionEffect_CaveDrips || kind == kActionEffect_CaveSheen) ?
          wet_sources : 0,
      .flags = kActionEffectFlag_Visible | kActionEffectFlag_ClipToRect,
      .render_layer = fields[i].layer,
      .projection_plane = fields[i].plane,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
      .clip_rect = {-(float)x, -(float)y, (float)width - x, (float)height - y},
    };
    if (!SceneDecorationAppend(dst, &effect)) {
      dst->decoration_count = dst->decoration_visible_count = 0;
      return;
    }
  }
  if (room <= 3) CaptureTempleMist(dst, scene);
}

void CaptureFillmoreCave(ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *ram, size_t size) {
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
  CaptureFillmoreCaveScene(&scene,dst);
}
