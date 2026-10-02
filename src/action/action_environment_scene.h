#ifndef AR_ACTION_ENVIRONMENT_SCENE_H
#define AR_ACTION_ENVIRONMENT_SCENE_H

#include "action_bg_world.h"
#include "action_effects.h"
#include "action_room_scene.h"

/* Borrowed, read-only source facts. Both the native observer and the editor
 * resolve through this contract; no synthetic WRAM or gameplay is needed. */
typedef struct ActionEnvironmentTileEdit {
  uint16_t entry;
  uint8_t band, replace, blank;
  uint8_t black[8], transparent[8];
} ActionEnvironmentTileEdit;
typedef bool (*ActionEnvironmentTileEditFn)(void *context, unsigned bg, int tile_x, int tile_y,
                                            ActionEnvironmentTileEdit *out);
enum { kActionSceneryTileColumns = 96, kActionSceneryRows = 352 };
/* Caller-owned, zero-initialized CPU cache. Effective opacity is re-read every
 * capture, including VRAM animation, edited masks and priority. Exact byte
 * equality skips only run extraction; no revision/hash can hide a change. */
typedef struct ActionSceneryCaptureCache {
  uint8_t previous[kActionSceneryRows][kActionSceneryTileColumns];
  uint8_t current[kActionSceneryRows][kActionSceneryTileColumns];
  ActionMoonlightOcclusion result;
  int x, y;
  bool ready;
} ActionSceneryCaptureCache;
typedef struct ActionEnvironmentScene {
  uint8_t group, room;
  uint16_t clock;
  int camera_x[2], camera_y[2];
  ActionBgMapView maps[2];
  const uint8_t *metatiles[2];
  uint16_t word_mask[2], attributes[2];
  bool big_endian;
  uint8_t collision[256];
  uint16_t water_scroll[kActionBloodpoolWaterScrollRows];
  bool water_scroll_valid;
  /* Borrowed only while capturing. Render frames own opacity runs, not these
   * pointers. Edits describe visible artwork and never mutate collision. */
  const uint16_t *vram;
  uint16_t tile_base[2];
  ActionEnvironmentTileEditFn tile_edit;
  void *tile_edit_context;
  ActionSceneryCaptureCache *scenery_cache;
  const ActionSurfaceField *surface_fields[kActionSurfaceFieldKinds];
  bool suppress_default_ray_field;
  bool suppress_default_marsh_field;
  bool suppress_default_castle_field;
  bool suppress_default_glow_field;
  bool suppress_default_moon_field;
  bool suppress_default_water_field;
  bool suppress_default_atmosphere_field;
} ActionEnvironmentScene;

typedef struct ActionEnvironmentWaterfallTrace {
  int camera_x, camera_y;
  unsigned x0, y0, x1, y1, splash_count;
  bool valid, published;
} ActionEnvironmentWaterfallTrace;
void ActionSurfaceField_Capture(const ActionEnvironmentScene *,ActionSceneEffectFrame *,const ActionSurfaceField *const [5],ActionEnvironmentWaterfallTrace *);
bool ActionMapEnvironmentScene_UsesTorches(uint8_t group, uint8_t room);
void ActionMapEnvironmentScene_Capture(const ActionEnvironmentScene *scene,
    ActionSceneEffectFrame *frame, ActionEnvironmentWaterfallTrace *trace);

bool ActionEnvironmentScene_FromWram(ActionEnvironmentScene *out,
    const uint8_t *ram, size_t size, uint16_t clock);
bool ActionEnvironmentScene_FromRoom(ActionEnvironmentScene *out,
    const ActionRoomScene *room, const ActionRoomSceneFrameState *frame);
static inline uint16_t ActionEnvironmentScene_Word(const ActionEnvironmentScene *s, unsigned bg,
                                                   unsigned metatile, unsigned quadrant) {
  if (!s || bg > 1 || metatile > 255 || quadrant > 3 || !s->metatiles[bg]) return 0;
  const uint8_t *p = s->metatiles[bg] + metatile * 8 + quadrant * 2;
  return s->big_endian ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[0] | p[1] << 8);
}
bool ActionEnvironmentScene_OpacityRow(const ActionEnvironmentScene *scene, unsigned bg, int x,
                                       int y, uint8_t *bits);
bool ActionEnvironmentScene_CaptureScenery(const ActionEnvironmentScene *scene,
                                           ActionMoonlightOcclusion *out);
void ActionEnvironmentScene_Capture(const ActionEnvironmentScene *scene,
    ActionSceneEffectFrame *frame);
void ActionRayField_Capture(const ActionEnvironmentScene *scene,
    ActionSceneEffectFrame *frame, const ActionRayField *definition, uint32_t source);

void ActionMarshField_Capture(const ActionEnvironmentScene *,ActionSceneEffectFrame *,const ActionMarshField *,uint32_t);
void ActionMoonField_Capture(const ActionEnvironmentScene *scene,
    ActionSceneEffectFrame *frame,const ActionMoonField *definition,uint32_t source);
void ActionWaterField_Capture(const ActionEnvironmentScene *scene,
    ActionSceneEffectFrame *frame,const ActionWaterField *definition,uint32_t source);

void ActionAtmosphereField_Capture(const ActionEnvironmentScene *scene,
    ActionSceneEffectFrame *frame,const ActionAtmosphereField *definition,uint32_t source);

void ActionCastleField_Capture(const ActionEnvironmentScene *,ActionSceneEffectFrame *,const ActionCastleField *,uint32_t);
void ActionGlowField_Capture(const ActionEnvironmentScene *,ActionSceneEffectFrame *,const ActionGlowField *,uint32_t);
#endif
