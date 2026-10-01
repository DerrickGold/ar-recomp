#ifndef AR_ACTION_ENVIRONMENT_SCENE_H
#define AR_ACTION_ENVIRONMENT_SCENE_H

#include "action_bg_world.h"
#include "action_effects.h"
#include "action_room_scene.h"

/* Borrowed, read-only source facts. Both the native observer and the editor
 * resolve through this contract; no synthetic WRAM or gameplay is needed. */
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
} ActionEnvironmentScene;

typedef struct ActionEnvironmentWaterfallTrace {
  int camera_x, camera_y;
  unsigned x0, y0, x1, y1, splash_count;
  bool valid, published;
} ActionEnvironmentWaterfallTrace;
bool ActionMapEnvironmentScene_UsesTorches(uint8_t group, uint8_t room);
void ActionMapEnvironmentScene_Capture(const ActionEnvironmentScene *scene,
    ActionSceneEffectFrame *frame, ActionEnvironmentWaterfallTrace *trace);

bool ActionEnvironmentScene_FromWram(ActionEnvironmentScene *out,
    const uint8_t *ram, size_t size, uint16_t clock);
bool ActionEnvironmentScene_FromRoom(ActionEnvironmentScene *out,
    const ActionRoomScene *room, const ActionRoomSceneFrameState *frame);
uint16_t ActionEnvironmentScene_Word(const ActionEnvironmentScene *scene,
    unsigned bg, unsigned metatile, unsigned quadrant);
void ActionEnvironmentScene_Capture(const ActionEnvironmentScene *scene,
    ActionSceneEffectFrame *frame);

#endif
