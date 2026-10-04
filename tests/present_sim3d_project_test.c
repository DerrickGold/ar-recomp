/* Actor/effect placement with actual audited town heights and registration.
 * Renderer readiness and unused upload telemetry are substituted; no GPU or runner. */
#include "sim/sim3d/present_sim3d_project.h"
#include "sim/world_nav/sim_world_navigation_terrain.h"
#include "sim/town/sim_town_terrain.h"
#include "app/performance_metrics.h"
#include "sim/sim3d/sim3d.h"
#include "support/test_assert.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ArRenderDevice g_render_device;
bool SimBackgroundVoxelRenderer_Ready(uint32_t serial) { return serial == 42; }
void PerformanceMetrics_Add(PerformanceCount counter, uint64_t value) {
  (void)counter;
  (void)value;
}

static void Near(float a, float b) { assert(fabsf(a - b) < .001f); }

static void TestEffects(void) {
  FrameSlot *slot = calloc(1, sizeof(*slot));
  assert(slot);
  slot->ws_extra = 52;
  slot->sim.camera_x = 64;
  slot->sim.camera_y = 80;
  slot->sim.background_voxel_enabled = true;
  slot->sim.background_voxel_serial = 42;
  slot->sim.height_scale_x100 = 125;
  slot->sim.height_pop_pct = 15;
  const ArRenderRectI source = {4, 8, 360, 224}, viewport = {13, 17, 800, 600};
  const Scene3DCamera camera = {-.75f, .1f, 4.5f, .4f};
  const float aspect = (float)viewport.w / viewport.h;
  float matrix[16], embedded[16];
  Scene3D_BuildViewProjection(&camera, viewport.w, viewport.h, matrix);
  const float scale[3] = {16 * aspect / source.w, 16.0f / source.h, 16.0f / source.h};
  const float offset[3] = {
      ((slot->ws_extra - slot->sim.camera_x + 256.0f - source.x) / source.w - .5f) * aspect,
      .5f - (256.0f - slot->sim.camera_y - source.y) / source.h, 0};
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 3; ++col)
      embedded[col * 4 + row] = matrix[col * 4 + row] * scale[col];
    embedded[12 + row] = matrix[12 + row];
    for (int col = 0; col < 3; ++col)
      embedded[12 + row] += matrix[col * 4 + row] * offset[col];
  }
  const unsigned heights[] = {0, 40, 100, 150};
  const SimEffectKind kinds[] = {kSimEffect_RedDemonFire, kSimEffect_VolcanoFireball,
                                 kSimEffect_GroundFire};
  unsigned checked = 0;
  for (uint8_t town = 1; town <= 6; ++town) {
    slot->sim.town = town;
    int ox, oy;
    assert(SimWorldMap_OriginForTown(town, &ox, &oy));
    for (unsigned h = 0; h < 4; ++h) {
      slot->sim.landscape_height_pct = heights[h];
      SimGlobeMapping map;
      assert(SimGlobeMapping_Build(town, ox, oy, 288, 2, heights[h] / 100.0f, &map));
      PresentSimGlobeProjection globe;
      assert(PresentSimGlobeProject_Build(&map, embedded, source, viewport,
                                          Scene3D_AutoFitDistance(camera.fov_y), &globe));
      for (unsigned curved = 0; curved < 2; ++curved)
        for (unsigned kind = 0; kind < 3; ++kind)
          for (unsigned space = 0; space <= kSimEffectSpace_Screen; ++space)
            for (unsigned step = 0; step < 17; ++step) {
              const SimSceneProjection scene = {source, viewport, &camera, matrix,
                                                curved ? &globe : NULL};
              SimEffectInstance effect = {
                  .kind = kinds[kind],
                  .world_x = 16 + step * 28,
                  .world_y = 32 + step * 24,
                  .geometry = {.kind = kSimEffectGeometry_Point, .space = space}};
              const SimEffectLocalPoint local = {8, 12, 24};
              Scene3DPoint actual;
              float sx, sy;
              assert(ProjectSimEffectPoint(slot, &effect, &local, &scene, &actual, &sx, &sy));
              if (space == kSimEffectSpace_Screen) {
                Near(actual.x, viewport.x + local.x * (float)viewport.w / source.w);
                Near(actual.y, viewport.y + local.y * (float)viewport.h / source.h);
                Near(sx, (float)viewport.w / source.w);
                Near(sy, (float)viewport.h / source.h);
              } else {
                const bool aerial = effect.kind == kSimEffect_RedDemonFire;
                const int local_support_x = effect.kind == kSimEffect_VolcanoFireball ? 0 : local.x;
                const int local_support_y = effect.kind == kSimEffect_VolcanoFireball ? 0 : local.y;
                const float units =
                    aerial ? SimTownTerrain_MaximumUnits(town)
                           : SimTownTerrain_HeightUnitsAt(town, effect.world_x + local_support_x,
                                                          effect.world_y + local_support_y);
                const float altitude = local.height * 1.25f;
                const float support_world =
                    SimTownTerrain_ScaledHeightPixels(units, heights[h]) / source.h;
                const float x =
                    effect.world_x + (space == kSimEffectSpace_WorldLocal ? local.x : 0);
                const float y =
                    effect.world_y + (space == kSimEffectSpace_WorldLocal ? local.y : 0);
                Scene3DPoint expected;
                float ex, ey;
                if (curved) {
                  float registered;
                  assert(SimWorldNavigationTerrain_RegisterTownFloor(
                      town, aerial ? 16 : x / 16, aerial ? 16 : y / 16, units, &registered));
                  PresentSimGlobeProjectedPoint point;
                  assert(PresentSimGlobeProject_Point(&globe, x, y, registered, altitude, &point));
                  expected = point.screen;
                  ex = point.pixel_scale[0];
                  ey = point.pixel_scale[1];
                } else {
                  const float wx =
                      ((slot->ws_extra + x - slot->sim.camera_x - source.x) / source.w - .5f) *
                      aspect;
                  const float wy = .5f - (y - slot->sim.camera_y - source.y) / source.h;
                  const float z = support_world + altitude / source.h;
                  assert(Scene3D_ProjectWorldPoint(matrix, wx, wy, z, viewport.w, viewport.h,
                                                   &expected));
                  expected.x += viewport.x;
                  expected.y += viewport.y;
                  const float s = Scene3D_ProjectBillboardScale(
                      matrix, wx, wy, z, Scene3D_AutoFitDistance(camera.fov_y));
                  ex = (float)viewport.w / source.w * s;
                  ey = (float)viewport.h / source.h * s;
                }
                if (space == kSimEffectSpace_RecordLocal) {
                  const float pop = 1 + (support_world * source.h + altitude) / 24 * .15f;
                  ex *= pop;
                  ey *= pop;
                  expected.x += local.x * ex;
                  expected.y += local.y * ey;
                }
                Near(actual.x, expected.x);
                Near(actual.y, expected.y);
                Near(sx, ex);
                Near(sy, ey);
              }
              ++checked;
            }
    }
  }
  printf("SIM placement: %u flat/curved, screen/world/record-local, flight/crater samples at "
         "0/40/100/150%% across six towns\n",
         checked);
  free(slot);
}

static void TestBubbleMountainPlacement(void) {
  SimRenderObject object = {.height_class = kSimHeightClass_Grounded};
  assert(SimObjectUsesMountainSurface(&object));
  object.traits = kSimObjectTrait_StructureOverlay | kSimObjectTrait_Overhead;
  assert(!SimObjectUsesMountainSurface(&object));
  /* Overhead alone does not remove terrain support from ground effects. */
  object.height_class = kSimHeightClass_GroundEffect;
  object.traits = kSimObjectTrait_Overhead;
  assert(SimObjectUsesMountainSurface(&object));
  object.height_class = kSimHeightClass_Flying;
  assert(!SimObjectUsesMountainSurface(&object));
  assert(!SimObjectUsesMountainSurface(NULL));
}

typedef struct GroundCaptureCheck {
  ArRenderRectI source, viewport;
  float matrix[16];
  int draws;
} GroundCaptureCheck;

static bool CheckGroundCapture(void *context, ArRenderTexture texture,
    const ArRenderVertex2D *vertices, int vertex_count,
    const int32_t *indices, int index_count, const ArRenderDrawState *state) {
  (void)state;
  GroundCaptureCheck *check = context;
  assert(texture.value == 1 && vertex_count > 0 && index_count > 0);
  bool top = false, bottom = false;
  for (int i=0; i<vertex_count; ++i) {
    const ArRenderVertex2D *v = &vertices[i];
    const float x = v->tex_coord.x * kSim3DMaxWidth;
    const float y = v->tex_coord.y * kSim3DMaxHeight;
    assert(y >= 0 && y <= 224.001f);
    top |= y == 0;
    bottom |= fabsf(y-224) < .001f;
    Scene3DPoint expected;
    assert(ProjectSimTexturePoint(check->matrix, check->source, check->viewport,
                                   x, y, 0, &expected));
    Near(v->position.x, expected.x);
    Near(v->position.y, expected.y);
  }
  for (int i=0; i<index_count; ++i)
    assert(indices[i] >= 0 && indices[i] < vertex_count);
  assert(top && bottom);
  ++check->draws;
  return true;
}

static void TestExpandedTownCanvas(void) {
  const ArRenderBackendOps ops = {.draw_geometry = CheckGroundCapture};
  GroundCaptureCheck check = {0};
  /* This unit observes only the geometry dispatch; no resources are created. */
  g_render_device = (ArRenderDevice){.ops=&ops, .context=&check};
  const int rows[] = {0,16,37,64,0};
  for (unsigned i=0; i<sizeof(rows)/sizeof(rows[0]); ++i) {
    check.source = (ArRenderRectI){43,-rows[i],256,224+2*rows[i]};
    check.viewport = (ArRenderRectI){11,17,768,check.source.h*3};
    const Scene3DCamera camera = {0,0,Scene3D_AutoFitDistance(.4f),.4f};
    Scene3D_BuildViewProjection(&camera,check.viewport.w,check.viewport.h,check.matrix);
    DrawSimGroundPlane((ArRenderTexture){1},check.source,224,check.viewport,check.matrix,NULL);
    Scene3DPoint centre;
    assert(ProjectSimTexturePoint(check.matrix,check.source,check.viewport,
                                   43+128,112,0,&centre));
    Near(centre.x,check.viewport.x+check.viewport.w*.5f);
    Near(centre.y,check.viewport.y+check.viewport.h*.5f);
  }
  assert(check.draws == 5);
  ArRenderDevice_Reset(&g_render_device);
}

int main(void) {
  TestExpandedTownCanvas();
  TestBubbleMountainPlacement();
  TestEffects();
  puts("present_sim3d_project_test: PASS");
}
