#include "action_environment_scene.h"

static bool BackgroundDepth(const ActionEnvironmentScene *scene, float *rows) {
  const ActionRoomSceneFrameRequest request = {.camera_x=scene->camera_x[0],
      .camera_y=scene->camera_y[0],.game_frame=scene->clock,.animation_phase=-1,.page_phase=-1};
  return ActionRoomScene_RasterCameraSensitivity(scene->background_scene,&request,rows);
}

bool ActionEnvironmentScene_FromRoom(ActionEnvironmentScene *out,
    const ActionRoomScene *room, const ActionRoomSceneFrameState *frame) {
  if (!out || !room || !frame) return false;
  ActionEnvironmentScene s = {.group = room->group, .room = room->map,
    .clock = (uint16_t)frame->game_frame, .big_endian = true,
    .background_frame = frame, .background_scene = room, .capture_background_depth = BackgroundDepth};
  for (unsigned bg = 0; bg < 2; ++bg) {
    const ActionRoomSceneBg *source = &room->bg[bg];
    if (!ActionRoomScene_HasBackground(room,bg+1)) return false;
    s.maps[bg] = (ActionBgMapView){source->map,source->map_size,
      source->pages_wide * 256u,source->pages_high * 256u,source->pages_wide};
    s.metatiles[bg] = source->metatiles;
    s.word_mask[bg] = kActionRoomSceneTileWordMask;
    s.attributes[bg] = ActionRoomScene_BgAttributes(room,bg+1);
    s.camera_x[bg] = frame->layer_camera_x[bg];
    s.camera_y[bg] = frame->layer_camera_y[bg];
  }
  /* Native $02:BAC1 packs bit $0200 from TL/TR/BL/BR into the LUT. */
  for (unsigned i = 0; i < 256; ++i)
    for (unsigned q = 0; q < 4; ++q)
      s.collision[i] |= ((ActionEnvironmentScene_Word(&s,0,i,q) >> 9) & 1) << q;
  s.water_scroll_valid = frame->raster_effect == kActionRoomRaster_Bg2LowerPerspective;
  for (unsigned i = 0; i < kActionBloodpoolWaterScrollRows; ++i)
    s.water_scroll[i] = frame->bg_hscroll[1][kActionBloodpoolWaterScrollFirstRow + i];
  *out = s;
  return true;
}
