#include "sim/sim3d/present_sim3d_effects.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "deterministic_hash.h"
#include "render/effect_batch.h"
#include "support/test_assert.h"

ArRenderDevice g_render_device;
static int draws, tints, curved_points;
static bool available = true, fail_projection, fail_tint, fail_axes;
static PresentSimGlobeBillboardAxes curved_axes;
static ArRenderColorF tint;
static ArRenderVertex2D captured[512];
static int count;
static uint64_t geometry_hash;

bool EffectRenderer_Available(void) { return available; }
bool EffectRenderer_Submit(ArRenderDevice *device, const EffectBatch *batch,
                           ArRenderBlendMode blend) {
  assert(device == &g_render_device && blend == kArRenderBlendMode_Add);
  assert(!batch->overflow && batch->vertex_count <= batch->vertex_capacity);
  assert(batch->index_count > 0 && batch->index_count <= batch->index_capacity);
  count = batch->vertex_count;
  assert(count <= (int)(sizeof(captured) / sizeof(captured[0])));
  memcpy(captured, batch->vertices, (size_t)count * sizeof(captured[0]));
  for (int i = 0; i < count; ++i) {
    assert(isfinite(captured[i].position.x) && isfinite(captured[i].position.y));
    assert(captured[i].color.a >= 0 && captured[i].color.a <= 1);
  }
  for (int i = 0; i < batch->index_count; ++i)
    assert(batch->indices[i] >= 0 && batch->indices[i] < count);
  geometry_hash = DeterministicHash_Fnv1a64(DETERMINISTIC_HASH_FNV1A64_OFFSET, captured,
                                            (size_t)count * sizeof(captured[0]));
  ++draws;
  return true;
}
bool ArRenderDevice_DrawSolidRect(ArRenderDevice *device, const ArRenderRectF *rect,
                                  ArRenderColorF color, ArRenderBlendMode blend) {
  assert(device == &g_render_device && blend == kArRenderBlendMode_Add);
  assert(rect->x == 12 && rect->y == 24 && rect->w == 640 && rect->h == 480);
  ++tints;
  tint = color;
  return !fail_tint;
}
float SimTerrainGroundHeightUnits(const FrameSlot *slot, float x, float y) {
  (void)slot;
  (void)x;
  (void)y;
  return 8;
}
float SimTerrainHeightWorld(const FrameSlot *slot, ArRenderRectI source, float units) {
  (void)slot;
  return units / source.h;
}
bool ProjectSimTexturePoint(const float matrix[16], ArRenderRectI source, ArRenderRectI viewport,
                            float x, float y, float height, Scene3DPoint *point) {
  (void)matrix;
  (void)viewport;
  *point = (Scene3DPoint){.x = x * 2, .y = (y - height * source.h) * 2};
  return !fail_projection;
}
float Scene3D_AutoFitDistance(float fov) {
  assert(fov == .4f);
  return 4;
}
bool ProjectSimAnchorAndScale(const float matrix[16], ArRenderRectI source, ArRenderRectI viewport,
                              float x, float y, float height, float reference_depth,
                              Scene3DPoint *point, float *sx, float *sy) {
  assert(reference_depth == 4);
  *sx = 2;
  *sy = 3; /* Sprite pixel aspect differs from ground's projected scale. */
  return ProjectSimTexturePoint(matrix, source, viewport, x, y, height, point);
}
float SimBillboardHeightPop(ArRenderRectI source, float height, unsigned percent) {
  assert(source.h == 224 && height > 0);
  return percent ? 1.25f : 1;
}
bool ProjectSimCurvedAnchor(const FrameSlot *slot, const PresentSimGlobeProjection *globe, float x,
                            float y, float support, float altitude, bool aerial,
                            PresentSimGlobeProjectedPoint *point) {
  (void)slot;
  assert(globe && support == 8 && !aerial);
  ++curved_points;
  point->screen = (Scene3DPoint){.x = x * 2, .y = (y - support - altitude) * 2};
  return !fail_projection;
}
bool PresentSimGlobeProject_GroundBillboardAxes(const PresentSimGlobeProjection *globe, float x,
                                                float y, const PresentSimGlobeProjectedPoint *point,
                                                PresentSimGlobeBillboardAxes *axes) {
  assert(globe && point->screen.x == x * 2 && point->screen.y == (y - 8) * 2);
  *axes = curved_axes;
  return !fail_axes;
}

static void Near(float a, float b) { assert(fabsf(a - b) < .001f); }

static void CheckRayAxes(PresentSimGlobeBillboardAxes axes) {
  assert(count == 16 * 6 * 3);
  for (int ray = 0; ray < count / 18; ++ray) {
    const ArRenderPointF foot = captured[ray * 18 + 1].position;
    const ArRenderPointF tip = captured[ray * 18 + 16].position;
    const ArRenderPointF edge = captured[ray * 18 + 17].position;
    const float height = -((tip.x - foot.x) * axes.down.x + (tip.y - foot.y) * axes.down.y) /
                         (axes.down.x * axes.down.x + axes.down.y * axes.down.y);
    const float width = ((edge.x - tip.x) * axes.right.x + (edge.y - tip.y) * axes.right.y) /
                        (axes.right.x * axes.right.x + axes.right.y * axes.right.y);
    assert(height >= 75 && height <= 117 && width >= .9f && width <= 2);
    Near(tip.x - foot.x, -height * axes.down.x);
    Near(tip.y - foot.y, -height * axes.down.y);
    Near(edge.x - tip.x, width * axes.right.x);
    Near(edge.y - tip.y, width * axes.right.y);
  }
}

static void CheckScatteredRays(const FrameSlot *slot) {
  int quadrants[4] = {0};
  float shortest = 1000, longest = 0, faintest = 1, brightest = 0;
  for (int ray = 0; ray < count / 18; ++ray) {
    const ArRenderPointF foot = captured[ray * 18 + 1].position;
    const float x = foot.x / 2 - slot->sim.sun_miracle.target_x;
    const float y = foot.y / 2 + 8 - slot->sim.sun_miracle.target_y;
    assert(x >= 4 && x <= 60 && y >= 4 && y <= 60);
    ++quadrants[(x >= 32) + 2 * (y >= 32)];
    /* Repeated X coordinates caused rear rows to merge into four bright bars. */
    for (int earlier = 0; earlier < ray; ++earlier)
      assert(fabsf(foot.x - captured[earlier * 18 + 1].position.x) > 1);
    const float length = (foot.y - captured[ray * 18 + 16].position.y) / 3;
    shortest = fminf(shortest, length);
    longest = fmaxf(longest, length);
    float peak = 0;
    for (int i = 0; i < 18; ++i)
      peak = fmaxf(peak, captured[ray * 18 + i].color.a);
    assert(peak > 0 && peak <= .26f);
    faintest = fminf(faintest, peak);
    brightest = fmaxf(brightest, peak);
  }
  for (int i = 0; i < 4; ++i)
    assert(quadrants[i] >= 2);
  assert(longest - shortest > 20 && brightest - faintest > .02f);
}

static void TestFacing(FrameSlot *slot, SimSceneProjection *scene) {
  scene->globe = NULL;
  DrawSimSunRays(slot, true, false, scene);
  CheckRayAxes((PresentSimGlobeBillboardAxes){.right = {2, 0}, .down = {0, 3}});
  CheckScatteredRays(slot);

  /* Grounded globe billboards can roll and foreshorten as the camera turns.
   * Both shafts and diamond particles must follow those supplied axes. */
  PresentSimGlobeProjection globe = {0};
  scene->globe = &globe;
  const PresentSimGlobeBillboardAxes views[] = {
      {.right = {2, 0}, .down = {0, 3}},
      {.right = {1.6f, .8f}, .down = {-.6f, 2.4f}},
      {.right = {.7f, -.3f}, .down = {1, 2}},
  };
  for (unsigned i = 0; i < sizeof(views) / sizeof(views[0]); ++i) {
    curved_axes = views[i];
    DrawSimSunRays(slot, true, false, scene);
    CheckRayAxes(curved_axes);
    DrawSimSunRays(slot, false, true, scene);
    assert(count == 24 * 4);
    for (int p = 0; p < count; p += 4) {
      const ArRenderPointF top = captured[p].position, bottom = captured[p + 3].position;
      const ArRenderPointF right = captured[p + 1].position, left = captured[p + 2].position;
      Near(bottom.x - top.x, 2.2f * curved_axes.down.x);
      Near(bottom.y - top.y, 2.2f * curved_axes.down.y);
      Near(right.x - left.x, 1.1f * curved_axes.right.x);
      Near(right.y - left.y, 1.1f * curved_axes.right.y);
    }
    const ArRenderPointF first = captured[0].position;
    ++slot->sim.sun_miracle.phase;
    DrawSimSunRays(slot, false, true, scene);
    Near(captured[0].position.x - first.x, (80.0f / 48) * curved_axes.down.x);
    Near(captured[0].position.y - first.y, (80.0f / 48) * curved_axes.down.y);
  }
  fail_axes = true;
  const int before = draws;
  DrawSimSunRays(slot, true, true, scene);
  assert(draws == before);
  fail_axes = false;
  scene->globe = NULL;
}

static void TestHudAtlasOwnership(void) {
  static FrameSlot slot;
  SimRenderObject object = {.tier = kSimRecordTier_Fixed, .oam_first = 11, .oam_count = 4};
  /* Broad scene claims must not hide ordinary menu artwork. */
  slot.overlay_captures[kFrameSlotOverlay_Obj].oamCount = 128;
  assert(!SimObjectIsPromotedHud(&slot, &object));
  slot.hud_icon = (HudIconFrame){.first = 11, .count = 4, .scene_removed = true};
  assert(SimObjectIsPromotedHud(&slot, &object));
  /* Retained metadata is enough, even after borrowed pixels are released. */
  slot.overlay_captures[kFrameSlotOverlay_Obj].oamCount = 0;
  assert(SimObjectIsPromotedHud(&slot, &object));
  object.oam_first = 15;
  assert(!SimObjectIsPromotedHud(&slot, &object));
  object.oam_first = 10;
  assert(!SimObjectIsPromotedHud(&slot, &object));
  object.oam_first = 11;
  object.oam_count = slot.hud_icon.count = 1;
  assert(SimObjectIsPromotedHud(&slot, &object));
  slot.hud_icon.scene_removed = false;
  assert(!SimObjectIsPromotedHud(&slot, &object));
  slot.hud_icon.scene_removed = true;
  object.tier = kSimRecordTier_World;
  assert(!SimObjectIsPromotedHud(&slot, &object));
}

int main(void) {
  TestHudAtlasOwnership();
  static FrameSlot slot;
  const float matrix[16] = {0};
  const Scene3DCamera camera = {.fov_y = .4f};
  const ArRenderRectI viewport = {12, 24, 640, 480};
  SimSceneProjection scene = {
      .source = {0, 0, 256, 224}, .viewport = viewport, .matrix = matrix, .camera = &camera};
  slot.sim.sun_miracle =
      (SimSunMiracle){.active = true, .phase = 70, .target_x = 192, .target_y = 256};
  slot.sim.separated_valid = true;
  slot.sim.separated_brightness = 15;
  slot.sim.separated_cgadsub = 1;
  slot.sim.separated_fixed_color = 15;
  assert(DrawSimSunTint(&slot, viewport) && tints == 1);
  assert(fabsf(tint.r - 15.0f / 31) < .00001f && tint.g == 0 && tint.b == 0);
  slot.sim.separated_fixed_color = 3;
  assert(DrawSimSunTint(&slot, viewport) && tint.r < .1f);
  slot.sim.separated_cgadsub = 0;
  assert(DrawSimSunTint(&slot, viewport) && tints == 2);
  slot.sim.separated_cgadsub = 0x81;
  assert(DrawSimSunTint(&slot, viewport) && tints == 2);
  slot.sim.separated_cgadsub = 1;
  fail_tint = true;
  assert(!DrawSimSunTint(&slot, viewport));
  fail_tint = false;

  DrawSimSunLight(&slot, true, &scene);
  assert(draws == 1);
  for (int i = 0; i < count; ++i) {
    assert(captured[i].position.x >= 384 && captured[i].position.x <= 512);
    assert(captured[i].position.y >= 496 && captured[i].position.y <= 624);
    if (captured[i].position.x == 384 || captured[i].position.x == 512 ||
        captured[i].position.y == 496 || captured[i].position.y == 624)
      assert(captured[i].color.a == 0);
  }
  DrawSimSunRays(&slot, true, true, &scene);
  const uint64_t first = geometry_hash;
  DrawSimSunRays(&slot, true, true, &scene);
  assert(geometry_hash == first); /* Paused redraws cannot advance the shower. */
  ++slot.sim.sun_miracle.phase;
  DrawSimSunRays(&slot, true, true, &scene);
  assert(geometry_hash != first);
  DrawSimSunRays(&slot, true, false, &scene);
  const int ray_vertices = count;
  DrawSimSunRays(&slot, false, true, &scene);
  assert(count > 0 && count < ray_vertices);
  const uint64_t particles = geometry_hash;
  ++slot.sim.sun_miracle.target_x;
  DrawSimSunRays(&slot, false, true, &scene);
  assert(geometry_hash != particles);
  TestFacing(&slot, &scene);

  PresentSimGlobeProjection globe = {0};
  scene.globe = &globe;
  DrawSimSunLight(&slot, true, &scene);
  DrawSimSunRays(&slot, true, true, &scene);
  assert(curved_points > 0);
  const int before = draws;
  DrawSimSunLight(&slot, false, &scene);
  DrawSimSunRays(&slot, false, false, &scene);
  fail_projection = true;
  DrawSimSunLight(&slot, true, &scene);
  DrawSimSunRays(&slot, true, true, &scene);
  fail_projection = false;
  available = false;
  DrawSimSunLight(&slot, true, &scene);
  DrawSimSunRays(&slot, true, true, &scene);
  available = true;
  for (int endpoint = 0; endpoint < 2; ++endpoint) {
    slot.sim.sun_miracle.phase = endpoint ? kSimSunDuration : 0;
    DrawSimSunLight(&slot, true, &scene);
    DrawSimSunRays(&slot, true, true, &scene);
  }
  slot.sim.sun_miracle.phase = 70;
  slot.sim.sun_miracle.active = false;
  DrawSimSunLight(&slot, true, &scene);
  DrawSimSunRays(&slot, true, true, &scene);
  assert(draws == before);
  const int before_tints = tints;
  assert(DrawSimSunTint(&slot, viewport) && tints == before_tints);
  puts("present_sim_sun_test: PASS");
  return 0;
}
