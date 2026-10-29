#ifndef AR_ACTION_STORM_GROUND_H
#define AR_ACTION_STORM_GROUND_H
/* Capture-only terrain contacts. Reads original/edit-resolved 4bpp artwork,
 * never rendered pixels or GPU memory. Each active channel has a bounded walk. */
#include "action_storm.h"
#include "action_environment_scene.h"

static inline const ActionRoomSceneFrameState *ActionStorm_GroundFrame(
    const ActionEnvironmentScene *scene, ActionRoomSceneFrameState *scratch) {
  if (!scene) return NULL;
  if (scene->background_frame) return scene->background_frame;
  return scene->capture_background_frame && scene->capture_background_frame(scene, scratch) ? scratch : NULL;
}

static inline bool ActionStorm_GroundPixel(const ActionEnvironmentScene *scene,
    const ActionRoomSceneFrameState *frame, const float *depth,
    const ActionEffectInstance *e, ArRenderPointF point) {
  const int width = (int)scene->maps[1].world_width;
  const int height = (int)scene->maps[1].world_height;
  if (width <= 0 || height <= 0) return false;
  const float camera_x = ActionStorm_CustomParallax(e) ?
      scene->camera_x[0] * e->field_style.storm.parallax_x : scene->camera_x[1];
  const float screen_x = e->world_x + point.x - camera_x;
  const float screen_y = e->world_y + point.y - scene->camera_y[1];
  const unsigned row = (unsigned)fminf(223, fmaxf(0, floorf(screen_y)));
  /* A nearer/farther ridge is not the receiver for this cloud's channel. The
   * native row's camera sensitivity is the backdrop's authored depth cue. */
  if (ActionStorm_CustomParallax(e) &&
      (!depth || fabsf(depth[row]-e->field_style.storm.parallax_x)>.0001f)) return false;
  /* Use the native row's whole scroll, including its own camera rate. Native
   * BG2 artwork repeats; the storm's unwrapped source never repeats with it. */
  int x = (int)floorf(screen_x + frame->bg_hscroll[1][row]) % width;
  int y = (int)floorf(screen_y + frame->bg_vscroll[1][row]);
  if (x < 0) x += width;
  if (y < 0 || y >= height) return false;
  unsigned index;
  return ActionEnvironmentScene_PaletteIndex(scene, 1, x, y, &index) &&
      index / 16 == e->field_style.storm.ground_palette &&
      (e->field_style.storm.ground_colors & (1u << (index & 15)));
}

static inline float ActionStorm_GroundEnd(const ActionEnvironmentScene *scene,
    const ActionRoomSceneFrameState *frame, const float *depth, const ActionEffectInstance *e,
    const ArRenderPointF *points, unsigned segments) {
  for (unsigned i = 0; i < segments; ++i) {
    const ArRenderPointF a = points[i], b = points[i + 1];
    /* Half-texel steps cannot skip a one-pixel ridge. Authored size/bends
     * bound this to at most 2048 samples per segment, including diagonals. */
    const unsigned steps = (unsigned)fminf(2048,
        fmaxf(1, ceilf(fmaxf(fabsf(b.x - a.x), fabsf(b.y - a.y)) * 2)));
    for (unsigned j = 0; j <= steps; ++j) {
      const float t = (float)j / steps;
      if (ActionStorm_GroundPixel(scene, frame, depth, e,
              (ArRenderPointF){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}))
        return i + t;
    }
  }
  return (float)segments;
}

static inline void ActionStorm_CaptureGround(const ActionEnvironmentScene *scene,
    const ActionRoomSceneFrameState *frame, const float *depth, ActionEffectInstance *e, uint32_t seed) {
  e->storm_contact.valid = 0;
  if (!scene || !scene->vram || !frame) { e->flags &= (uint8_t)~kActionEffectFlag_Visible; return; }
  const int source_row=(int)floorf(e->world_y+ActionStorm_SourceY(e)-scene->camera_y[1]);
  if (ActionStorm_CustomParallax(e) && (!depth || source_row<0 || source_row>=224 ||
      fabsf(depth[source_row]-e->field_style.storm.parallax_x)>.0001f)) {
    e->flags &= (uint8_t)~kActionEffectFlag_Visible;
    return;
  }
  ActionStormChannel path;
  ActionStorm_Path(e, seed, &path);
  e->storm_contact.end[0] = ActionStorm_GroundEnd(scene, frame, depth, e, path.main, kActionStormMainSegments);
  if (e->storm_contact.end[0] <= 0 || e->storm_contact.end[0] >= kActionStormMainSegments) {
    /* A terrain strike with no receiver must not turn into a floating spark. */
    e->flags &= (uint8_t)~kActionEffectFlag_Visible;
    return;
  }
  for (unsigned i = 0; i + 1 < e->field_style.strands; ++i)
    e->storm_contact.end[i + 1] = ActionStorm_GroundEnd(scene, frame, depth, e,
        path.forks[i], kActionStormForkSegments);
  e->storm_contact.valid = 1;
}
#endif
