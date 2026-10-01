/* Stage-owned environmental capture. Phase: capture; read-only WRAM. */
#include "action/action_environment_capture_internal.h"

bool IsFillmoreForest(const uint8_t *wram, size_t wram_size) {
  return wram && wram_size > kActRaiserWram_Bg2Height + 1 &&
      Read8(wram, wram_size, kActRaiserWram_MapGroup) ==
          kActRaiserMapGroup_Fillmore &&
      Read8(wram, wram_size, kActRaiserWram_CurrentMap) == 1 &&
      Read16(wram, wram_size, kActRaiserWram_Bg1Width) == 4096 &&
      Read16(wram, wram_size, kActRaiserWram_Bg1Height) == 768 &&
      Read16(wram, wram_size, kActRaiserWram_Bg2Width) == 2304 &&
      Read16(wram, wram_size, kActRaiserWram_Bg2Height) == 512;
}

void CaptureFillmoreForestScene(const ActionEnvironmentScene *scene, ActionSceneEffectFrame *dst) {
  if (scene->group != kActRaiserMapGroup_Fillmore || scene->room != 1 ||
      scene->maps[0].world_width != 4096 || scene->maps[0].world_height != 768 ||
      scene->maps[1].world_width != 2304 || scene->maps[1].world_height != 512) return;
  /* Forest light uses the mean BG1/BG2 camera for independent parallax;
   * authored sources stay in world space and the origin only culls work. */
  const ActionBgMapView map = scene->maps[1];
  uint8_t tile, below;
  if (!ActionBgMapView_LookupMetatile(&map, 144, 112, &tile) ||
      !ActionBgMapView_LookupMetatile(&map, 144, 128, &below) ||
      tile != 0x0F || below != 0x01)
    return; /* Room signature only; these are not light-source positions. */
  const int bg2_x = scene->camera_x[1];
  const int bg2_y = scene->camera_y[1];
  const int cx = (scene->camera_x[0] +
                  scene->camera_x[1]) / 2;
  const int cy = (scene->camera_y[0] +
                  scene->camera_y[1]) / 2;
  const int x = cx + 128;
  const ActionEffectInstance effect = {
    .generation = 0x46000000u,
    .pulse_generation = 0x66000000u,
    .world_x = (int16_t)x, .world_y = (int16_t)(cy - 160),
    .age_ticks = scene->clock,
    .phase_ticks = scene->clock,
    .pulse_ticks = scene->clock,
    .kind = kActionEffect_ForestCanopyLight,
    .phase = kActionEffectPhase_ForestCanopyLight,
    .flags = kActionEffectFlag_Visible | kActionEffectFlag_ClipToRect,
    .render_layer = kActionEffectRenderLayer_Bg2Plane,
    .projection_plane = kActionEffectProjectionPlane_BetweenBackgrounds,
    .geometry = {.kind = kActionEffectGeometry_Rect,
                 .data.rect = {-384, 0, 384, 544}},
    .clip_rect = {cx - bg2_x - x, 160 - bg2_y,
                  2304 + cx - bg2_x - x, 512 + 160 - bg2_y},
  };
  ActionEffectInstance leaves = effect;
  leaves.kind = kActionEffect_ForestLeaves;
  leaves.render_layer = kActionEffectRenderLayer_Bg2Alpha;
  ActionEffectInstance forward = effect;
  forward.kind = kActionEffect_ForestForwardLight;
  forward.render_layer = kActionEffectRenderLayer_ForegroundLight;
  if (!SceneDecorationAppend(dst, &effect) || !SceneDecorationAppend(dst, &leaves) ||
      !SceneDecorationAppend(dst, &forward))
    dst->decoration_count = dst->decoration_visible_count = 0;
}

void CaptureFillmoreForest(ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *ram, size_t size) {
  ActionEnvironmentScene scene;
  if (ActionEnvironmentScene_FromWram(&scene,ram,size,observer->scene_clock))
    CaptureFillmoreForestScene(&scene,dst);
}
