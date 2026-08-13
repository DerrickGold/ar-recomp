#include <math.h>
#include <stdio.h>
#include <string.h>

#include "action_effect_render.h"
#include "action_effect_projection.h"
#include "action_bg_plan.h"
#include "actraiser_game.h"
#include "diorama.h"

static int g_failures;

#define CHECK(condition) do {                                                \
  if (!(condition)) {                                                        \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,       \
            #condition);                                                     \
    g_failures++;                                                            \
  }                                                                          \
} while (0)

static bool EffectBatchesEqual(const ActionEffectRenderBatch *a,
                               const ActionEffectRenderBatch *b) {
  return a->vertex_count == b->vertex_count &&
      a->index_count == b->index_count &&
      (!a->vertex_count ||
       memcmp(a->vertices, b->vertices,
              (size_t)a->vertex_count * sizeof(a->vertices[0])) == 0) &&
      (!a->index_count ||
       memcmp(a->indices, b->indices,
              (size_t)a->index_count * sizeof(a->indices[0])) == 0);
}

static bool SceneBatchesEqual(const ActionSceneEffectRenderBatch *a,
                              const ActionSceneEffectRenderBatch *b) {
  return a->vertex_count == b->vertex_count &&
      a->index_count == b->index_count &&
      (!a->vertex_count ||
       memcmp(a->vertices, b->vertices,
              (size_t)a->vertex_count * sizeof(a->vertices[0])) == 0) &&
      (!a->index_count ||
       memcmp(a->indices, b->indices,
              (size_t)a->index_count * sizeof(a->indices[0])) == 0);
}

static bool IdentityProjection(void *userdata,
                               const ActionEffectInstance *effect,
                               float local_x, float local_y,
                               SDL_FPoint *point) {
  (void)userdata;
  if (!effect || !point) return false;
  *point = (SDL_FPoint){
    effect->world_x + local_x,
    effect->world_y + local_y,
  };
  return true;
}

static DioramaProjection RakedApronProjection(void) {
  DioramaProjection projection = {
    .valid = true,
    .matrix = {
      1, 0, 0, 0,
      0, 1, 0, 0,
      1, 0, 1, 0,
      0, 0, 0, 1,
    },
    .aspect_x = 2.0f,
    .height_scale = 1.0f,
    .texture_x_origin = 20,
    .texture_width = 100,
    .texture_height = 50,
    .output_width = 100,
    .output_height = 100,
  };
  projection.bg1_plane = (DioramaPlaneProjection){
    .valid = true,
    .u0 = 0.20f, .v0 = 0.0f, .u1 = 0.80f, .v1 = 1.0f,
    .z_world = 0.35f,
    .rake = 0.25f,
    .bow = 0.10f,
  };
  projection.bg2_plane = (DioramaPlaneProjection){
    .valid = true,
    .u0 = 0.20f, .v0 = 0.0f, .u1 = 0.80f, .v1 = 1.0f,
    .z_world = -0.30f,
    .rake = 0.04f,
  };
  projection.object_planes[0] = (DioramaPlaneProjection){
    .valid = true,
    .u0 = 0.20f, .v0 = 0.0f, .u1 = 0.80f, .v1 = 1.0f,
    .z_world = -0.05f,
    .rake = -0.08f,
  };
  return projection;
}

static ActionEffectInstance Fire(void) {
  ActionEffectInstance effect = {
    .pulse_generation = 7,
    .record_address = 0x06A0,
    .world_x = 100,
    .world_y = 80,
    .visual = 13,
    .phase_ticks = 4,
    .pulse_ticks = 4,
    .kind = kActionEffect_MagicalFire,
    .phase = kActionEffectPhase_FireBloom,
    .flags = kActionEffectFlag_Visible,
    .obj_priority = 0,
    .render_layer = kActionEffectRenderLayer_WorldOverlay,
    .geometry = {
      .kind = kActionEffectGeometry_Rect,
      .data.rect = { -44.0f, -29.0f, 8.0f, 30.0f },
    },
  };
  return effect;
}

static void TestFeatureSwitchesAndDeterminism(void) {
  ActionEffectFrame frame = { .effect_count = 1 };
  frame.effects[0] = Fire();
  ActionEffectRenderBatch lighting, particles, both, repeat;

  /* A lone part yields two glows: the burst-wide light spill, plus the one
   * flame its single cluster produces. */
  CHECK(ActionEffectRender_Build(&frame, true, false, IdentityProjection,
                                 NULL, &lighting));
  CHECK(lighting.vertex_count == 2 * kActionEffectGlowVertices);
  CHECK(lighting.index_count == 2 * kActionEffectGlowIndices);
  /* Bloom emits a full complement of embers, which is what makes the
   * capacity constants below tight rather than merely sufficient. */
  CHECK(ActionEffectRender_Build(&frame, false, true, IdentityProjection,
                                 NULL, &particles));
  CHECK(particles.vertex_count == kActionEffectMaxEmbers * 4);
  CHECK(particles.index_count == kActionEffectMaxEmbers * 6);
  CHECK(ActionEffectRender_Build(&frame, true, true, IdentityProjection,
                                 NULL, &both));
  CHECK(both.vertex_count == lighting.vertex_count + particles.vertex_count);
  CHECK(both.index_count == lighting.index_count + particles.index_count);
  CHECK(ActionEffectRender_Build(&frame, true, true, IdentityProjection,
                                 NULL, &repeat));
  CHECK(EffectBatchesEqual(&both, &repeat));

  memset(&repeat, 0xFF, sizeof(repeat));
  CHECK(ActionEffectRender_Build(&frame, false, false, NULL, NULL, &repeat));
  CHECK(repeat.vertex_count == 0);
  CHECK(repeat.index_count == 0);
}

static void TestClocksAndValidation(void) {
  ActionEffectFrame frame = { .effect_count = 1 };
  frame.effects[0] = Fire();
  ActionEffectRenderBatch first, changed, skipped;
  CHECK(ActionEffectRender_Build(&frame, false, true, IdentityProjection,
                                 NULL, &first));
  frame.effects[0].pulse_ticks++;
  CHECK(ActionEffectRender_Build(&frame, false, true, IdentityProjection,
                                 NULL, &changed));
  CHECK(!EffectBatchesEqual(&first, &changed));

  frame.effects[0] = Fire();
  frame.effects[0].kind = 99;
  CHECK(ActionEffectRender_Build(&frame, true, true, IdentityProjection,
                                 NULL, &skipped));
  CHECK(skipped.index_count == 0);
  frame.effects[0] = Fire();
  frame.effects[0].phase = 99;
  CHECK(ActionEffectRender_Build(&frame, true, true, IdentityProjection,
                                 NULL, &skipped));
  CHECK(skipped.index_count == 0);
  frame.effects[0] = Fire();
  frame.effects[0].geometry.kind = kActionEffectGeometry_None;
  CHECK(ActionEffectRender_Build(&frame, true, true, IdentityProjection,
                                 NULL, &skipped));
  CHECK(skipped.index_count == 0);
  frame.effects[0] = Fire();
  frame.effects[0].render_layer = 99;
  CHECK(ActionEffectRender_Build(&frame, true, true, IdentityProjection,
                                 NULL, &skipped));
  CHECK(skipped.index_count == 0);

  frame.effect_count = kActionEffectMaxInstances + 1;
  CHECK(!ActionEffectRender_Build(&frame, true, true, IdentityProjection,
                                  NULL, &skipped));
  CHECK(skipped.index_count == 0);
  CHECK(!ActionEffectRender_Build(&frame, true, true, IdentityProjection,
                                  NULL, NULL));
}

/* The published capacity must be reachable, not merely generous — otherwise
 * the Reserve() guards are never exercised by anything. The worst case is
 * every part landing in its OWN cluster, so the parts are spread far enough
 * apart that none of them touch: that yields one flame per part plus the
 * burst spill, which is exactly kActionEffectMaxGlows. */
static void TestCapacityIsDerivedFromPublishedLimits(void) {
  ActionEffectFrame frame = { .effect_count = kActionEffectMaxInstances };
  for (unsigned i = 0; i < kActionEffectMaxInstances; i++) {
    frame.effects[i] = Fire();
    frame.effects[i].record_address += i * 0x40;
    frame.effects[i].world_x += (int)i * 4000;
  }
  ActionEffectRenderBatch batch;
  CHECK(ActionEffectRender_Build(&frame, true, true, IdentityProjection,
                                 NULL, &batch));
  CHECK(batch.vertex_count == kActionEffectRenderMaxVertices);
  CHECK(batch.index_count == kActionEffectRenderMaxIndices);

  /* And the opposite extreme: parts that all overlap must collapse to ONE
   * flame, which is the whole point of clustering. */
  for (unsigned i = 0; i < kActionEffectMaxInstances; i++)
    frame.effects[i].world_x = Fire().world_x + (int)i;
  ActionEffectRenderBatch clustered;
  CHECK(ActionEffectRender_Build(&frame, true, false, IdentityProjection,
                                 NULL, &clustered));
  CHECK(clustered.vertex_count == 2 * kActionEffectGlowVertices);
}

/* A spell routinely runs several stages at once — every measured Stardust
 * snapshot had flying stars and detonating ones alive together. Styling must
 * therefore be resolved PER PART, and nothing burst-wide may depend on which
 * part happens to be listed first.
 *
 * This is the regression that shipped: mode, palette and ember count were all
 * read from the anchor (the first visible part), so simply reordering the
 * frame changed how every other part was drawn. Reordering is exactly what
 * happens naturally as slots retire mid-cast. */
static ActionEffectInstance Star(int world_x, uint8_t phase,
                                 int16_t velocity_x, int16_t velocity_y) {
  ActionEffectInstance effect = {
    .pulse_generation = 3,
    .record_address = (uint16_t)(0x06A0 + world_x),
    .world_x = (int16_t)world_x,
    .world_y = 200,
    .velocity_x = velocity_x,
    .velocity_y = velocity_y,
    .visual = 2,
    .phase_ticks = 6,
    .pulse_ticks = 6,
    .kind = kActionEffect_MagicalStardust,
    .phase = phase,
    .role = kActionEffectRole_Body,
    .flags = kActionEffectFlag_Visible,
    .obj_priority = 0,
    .render_layer = kActionEffectRenderLayer_WorldOverlay,
    .geometry = {
      .kind = kActionEffectGeometry_Rect,
      .data.rect = { -16.0f, -16.0f, 16.0f, 16.0f },
    },
  };
  return effect;
}

static void TestMixedStagesAreOrderIndependent(void) {
  /* Far apart so they stay two clusters rather than merging into one. */
  ActionEffectInstance flying =
      Star(100, kActionEffectPhase_StardustLaunch, -8, 8);
  ActionEffectInstance bursting =
      Star(4000, kActionEffectPhase_StardustBurst, 0, 0);

  ActionEffectFrame flight_first = { .effect_count = 2 };
  flight_first.effects[0] = flying;
  flight_first.effects[1] = bursting;

  ActionEffectFrame burst_first = { .effect_count = 2 };
  burst_first.effects[0] = bursting;
  burst_first.effects[1] = flying;

  ActionEffectRenderBatch a, b;
  CHECK(ActionEffectRender_Build(&flight_first, true, true, IdentityProjection,
                                 NULL, &a));
  CHECK(ActionEffectRender_Build(&burst_first, true, true, IdentityProjection,
                                 NULL, &b));
  /* Same parts in either order must produce the same amount of geometry: one
   * spill plus two bodies, and one whole-burst ember budget. Under the old
   * anchor-driven styling the ember count alone differed by ~15% between
   * these two frames. */
  CHECK(a.vertex_count == b.vertex_count);
  CHECK(a.index_count == b.index_count);
  CHECK(a.vertex_count == 3 * kActionEffectGlowVertices +
                              kActionEffectMaxEmbers * 4);
}

static ActionEffectInstance SceneEffect(uint8_t kind, int world_x) {
  ActionEffectInstance effect = {
    .generation = (uint32_t)(0x1000 + world_x),
    .pulse_generation = (uint32_t)(0x2000 + world_x),
    .record_address = (uint16_t)(0x0C20 + world_x),
    .world_x = (int16_t)world_x,
    .world_y = 120,
    .visual = 0x24,
    .phase_ticks = 9,
    .pulse_ticks = 9,
    .kind = kind,
    .phase = kActionEffectPhase_LightningActive,
    .role = kActionEffectRole_Body,
    .flags = kActionEffectFlag_Visible,
    .obj_priority = 0,
    .render_layer = kActionEffectRenderLayer_WorldOverlay,
    .projection_plane =
        (kind == kActionEffect_WallTorch ||
         kind == kActionEffect_AitosLavaPit)
        ? kActionEffectProjectionPlane_Bg1
        : kActionEffectProjectionPlane_Obj,
    .geometry = {
      .kind = kActionEffectGeometry_Rect,
      .data.rect = {-8.0f, -8.0f, 8.0f, 8.0f},
    },
  };
  switch (kind) {
    case kActionEffect_WallTorch:
      effect.phase = kActionEffectPhase_WallTorch;
      break;
    case kActionEffect_EnemyFireball:
      effect.velocity_x = 3;
      effect.phase = kActionEffectPhase_EnemyFireballFlight;
      break;
    case kActionEffect_MarahnaFireball:
      effect.velocity_x = 3;
      effect.visual = 0x08;
      effect.phase = kActionEffectPhase_MarahnaFireballOrb;
      break;
    case kActionEffect_AitosLavaPit:
      effect.phase = kActionEffectPhase_AitosLavaPit;
      effect.geometry.data.rect =
          (ActionEffectLocalRect){-64.0f, -24.0f, 64.0f, 24.0f};
      break;
    case kActionEffect_AitosLavaFireball:
      effect.velocity_y = -4;
      effect.phase = kActionEffectPhase_AitosLavaFireballFlight;
      break;
    case kActionEffect_AitosMoltenRock:
      effect.velocity_x = -2;
      effect.velocity_y = 1;
      effect.visual = 0x2B;
      effect.phase = kActionEffectPhase_AitosMoltenRockFlight;
      break;
    case kActionEffect_AitosWaterSplash:
      effect.phase = kActionEffectPhase_AitosWaterSplash;
      effect.projection_plane = kActionEffectProjectionPlane_Bg1;
      effect.geometry.data.rect =
          (ActionEffectLocalRect){-24.0f, -16.0f, 24.0f, 16.0f};
      break;
    case kActionEffect_AitosWaterfall:
      effect.phase = kActionEffectPhase_AitosWaterfallFlow;
      effect.projection_plane = kActionEffectProjectionPlane_Bg2;
      effect.geometry.data.rect =
          (ActionEffectLocalRect){-256.0f, -176.0f, 256.0f, 312.0f};
      break;
    case kActionEffect_AitosWaterfallMist:
      effect.phase = kActionEffectPhase_AitosWaterfallMist;
      effect.render_layer = kActionEffectRenderLayer_Atmosphere;
      effect.projection_plane = kActionEffectProjectionPlane_Bg2;
      effect.geometry.data.rect =
          (ActionEffectLocalRect){-256.0f, -32.0f, 256.0f, 24.0f};
      break;
    case kActionEffect_LightningTrap:
      effect.visual = 0x1F;
      effect.geometry.data.rect =
          (ActionEffectLocalRect){0.0f, -88.0f, 8.0f, 88.0f};
      break;
    case kActionEffect_MarahnaLightningLink:
      effect.visual = 0x2E;
      effect.animation_state = 0x27;
      effect.phase = kActionEffectPhase_MarahnaLightningActive;
      effect.geometry.data.rect =
          (ActionEffectLocalRect){-40.0f, -4.0f, 40.0f, 4.0f};
      break;
    case kActionEffect_MarahnaBossLightning:
      effect.velocity_x = -4;
      effect.velocity_y = 4;
      effect.visual = 0x11;
      effect.phase = kActionEffectPhase_MarahnaBossLightningBolt;
      effect.geometry.data.rect =
          (ActionEffectLocalRect){-32.0f, 0.0f, 0.0f, 32.0f};
      break;
    case kActionEffect_BloodpoolBossLightning:
      effect.visual = 0x05;
      effect.phase = kActionEffectPhase_BossLightningStrike;
      effect.geometry.data.rect =
          (ActionEffectLocalRect){-30.0f, -83.0f, 8.0f, 21.0f};
      break;
    case kActionEffect_SwordBeam:
      effect.velocity_x = 8;
      effect.visual = 0x30;
      effect.phase = kActionEffectPhase_SwordBeamFlight;
      effect.geometry.data.rect =
          (ActionEffectLocalRect){32.0f, -33.0f, 48.0f, -1.0f};
      break;
    default:
      break;
  }
  return effect;
}

static void TestSceneFeatureSwitchesAndDeterminism(void) {
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  frame.effects[0] = SceneEffect(kActionEffect_WallTorch, 100);
  ActionSceneEffectRenderBatch lighting, particles, both, repeat;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &lighting));
  CHECK(lighting.vertex_count == 2 * kActionEffectGlowVertices);
  CHECK(lighting.index_count == 2 * kActionEffectGlowIndices);
  CHECK(ActionSceneEffectRender_Build(
      &frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == 7 * 4);
  CHECK(particles.index_count == 7 * 6);
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &both));
  CHECK(both.vertex_count == lighting.vertex_count + particles.vertex_count);
  CHECK(both.index_count == lighting.index_count + particles.index_count);
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(SceneBatchesEqual(&both, &repeat));

  frame.effects[0].pulse_ticks++;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(!SceneBatchesEqual(&both, &repeat));
}

static void TestSceneKindsRemainIndependent(void) {
  ActionSceneEffectFrame frame = {.effect_count = 6, .visible_count = 6};
  frame.effects[0] = SceneEffect(kActionEffect_WallTorch, 100);
  frame.effects[1] = SceneEffect(kActionEffect_EnemyFireball, 200);
  frame.effects[2] = SceneEffect(kActionEffect_LightningTrap, 300);
  frame.effects[3] = SceneEffect(kActionEffect_MarahnaFireball, 400);
  frame.effects[4] = SceneEffect(kActionEffect_AitosLavaPit, 500);
  frame.effects[5] = SceneEffect(kActionEffect_AitosLavaFireball, 600);
  ActionSceneEffectRenderBatch batch;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 12 * kActionEffectGlowVertices +
                                 (7 + 12 * 5) * 4);
  CHECK(batch.index_count == 12 * kActionEffectGlowIndices +
                                (7 + 12 * 5) * 6);

  frame.effects[1].kind = 99;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 10 * kActionEffectGlowVertices +
                                 (7 + 12 * 4) * 4);
  frame.effects[0].projection_plane = 99;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 8 * kActionEffectGlowVertices + 48 * 4);
}

static void TestAitosLavaLightingAndParticles(void) {
  ActionSceneEffectFrame frame = {.effect_count = 2, .visible_count = 2};
  frame.effects[0] = SceneEffect(kActionEffect_AitosLavaPit, 400);
  frame.effects[1] = SceneEffect(kActionEffect_AitosLavaFireball, 520);
  ActionSceneEffectRenderBatch lighting, particles, repeat;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &lighting));
  CHECK(lighting.vertex_count == 4 * kActionEffectGlowVertices);
  CHECK(lighting.index_count == 4 * kActionEffectGlowIndices);
  CHECK(ActionSceneEffectRender_Build(
      &frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == 24 * 4);
  CHECK(particles.index_count == 24 * 6);

  /* The pit's twelve births span its authored 128px surface instead of
   * collapsing into a torch-like centre plume. */
  float pit_min_x = 10000.0f, pit_max_x = -10000.0f;
  for (int i = 0; i < 12 * 4; i++) {
    if (particles.vertices[i].position.x < pit_min_x)
      pit_min_x = particles.vertices[i].position.x;
    if (particles.vertices[i].position.x > pit_max_x)
      pit_max_x = particles.vertices[i].position.x;
  }
  CHECK(pit_max_x - pit_min_x > 70.0f);

  /* The glow still covers the complete two-row bubbly volume, but the
   * isometric source plane sits one quarter-height above its geometric
   * midpoint. A quad's centroid is the projected particle position,
   * independent of its width and reach. Sweep enough ticks to cover every
   * 21-36 tick lifetime and pin the narrow +/-1.5px source band. */
  const float pit_source_y = frame.effects[0].world_y +
      (frame.effects[0].geometry.data.rect.y0 +
       frame.effects[0].geometry.data.rect.y1) * 0.5f -
      (frame.effects[0].geometry.data.rect.y1 -
       frame.effects[0].geometry.data.rect.y0) * 0.25f;
  float max_centroid_y[12];
  for (int particle = 0; particle < 12; particle++)
    max_centroid_y[particle] = -10000.0f;
  for (unsigned ticks = 0; ticks < 72; ticks++) {
    frame.effects[0].pulse_ticks = ticks;
    CHECK(ActionSceneEffectRender_Build(
        &frame, false, true, IdentityProjection, NULL, &particles));
    for (int particle = 0; particle < 12; particle++) {
      float centre_y = 0.0f;
      for (int vertex = 0; vertex < 4; vertex++)
        centre_y += particles.vertices[particle * 4 + vertex].position.y;
      centre_y *= 0.25f;
      CHECK(centre_y <= pit_source_y + 1.51f);
      if (centre_y > max_centroid_y[particle])
        max_centroid_y[particle] = centre_y;
    }
  }
  /* Lava presentation advances at 2x, so an even lifetime with an odd birth
   * phase can approach within one tick rather than land exactly on age zero. */
  for (int particle = 0; particle < 12; particle++)
    CHECK(max_centroid_y[particle] >= pit_source_y - 2.0f);
  frame.effects[0].pulse_ticks = 9;

  /* Rising fireballs trail down from the source art. This catches the old
   * zero/default-horizontal heading and sign mistakes. */
  float fireball_min_y = 10000.0f;
  for (int i = 12 * 4; i < particles.vertex_count; i++)
    if (particles.vertices[i].position.y < fireball_min_y)
      fireball_min_y = particles.vertices[i].position.y;
  CHECK(fireball_min_y > frame.effects[1].world_y + 4.0f);

  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &repeat));
  frame.effects[0].pulse_ticks++;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &lighting));
  CHECK(!SceneBatchesEqual(&repeat, &lighting));
}

static void TestMarahnaFireballFramesAndDirections(void) {
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  ActionSceneEffectRenderBatch lighting, particles;
  ActionEffectInstance *effect = &frame.effects[0];
  *effect = SceneEffect(kActionEffect_MarahnaFireball, 400);

  static const struct {
    uint16_t visual;
    int16_t velocity_x, velocity_y;
    uint8_t phase;
    int expected_x_sign, expected_y_sign;
  } kCases[] = {
    {0x05,  0,  0, kActionEffectPhase_MarahnaFireballOrb, 0, -1},
    {0x06,  2,  0, kActionEffectPhase_MarahnaFireballOrb, -1, 0},
    {0x07,  0,  0, kActionEffectPhase_MarahnaFireballOrb, 0, -1},
    {0x08, -2,  0, kActionEffectPhase_MarahnaFireballOrb, 1, 0},
    {0x32,  0,  3, kActionEffectPhase_MarahnaFireballSplit, 0, -1},
    {0x33, -3,  0, kActionEffectPhase_MarahnaFireballSplit, 1, 0},
    {0x32,  0, -3, kActionEffectPhase_MarahnaFireballSplit, 0, 1},
    {0x33,  3,  0, kActionEffectPhase_MarahnaFireballSplit, -1, 0},
    {0x1D, -4,  0, kActionEffectPhase_MarahnaSnakeFireballShot, 1, 0},
    {0x1E,  4,  0, kActionEffectPhase_MarahnaSnakeFireballShot, -1, 0},
  };
  for (size_t c = 0; c < sizeof(kCases) / sizeof(kCases[0]); c++) {
    effect->visual = kCases[c].visual;
    effect->velocity_x = kCases[c].velocity_x;
    effect->velocity_y = kCases[c].velocity_y;
    effect->phase = kCases[c].phase;
    CHECK(ActionSceneEffectRender_Build(
        &frame, true, false, IdentityProjection, NULL, &lighting));
    CHECK(lighting.vertex_count == 2 * kActionEffectGlowVertices);
    CHECK(ActionSceneEffectRender_Build(
        &frame, false, true, IdentityProjection, NULL, &particles));
    CHECK(particles.vertex_count ==
          kActionSceneEffectParticlesPerInstance * 4);
    float mean_x = 0.0f, mean_y = 0.0f;
    for (int i = 0; i < particles.vertex_count; i++) {
      mean_x += particles.vertices[i].position.x;
      mean_y += particles.vertices[i].position.y;
    }
    mean_x /= particles.vertex_count;
    mean_y /= particles.vertex_count;
    if (kCases[c].expected_x_sign < 0) CHECK(mean_x < effect->world_x - 8.0f);
    if (kCases[c].expected_x_sign > 0) CHECK(mean_x > effect->world_x + 8.0f);
    if (kCases[c].expected_y_sign < 0) CHECK(mean_y < effect->world_y - 8.0f);
    if (kCases[c].expected_y_sign > 0) CHECK(mean_y > effect->world_y + 8.0f);
  }
}

static void TestAitosUsesRakedDioramaSourcePlanes(void) {
  DioramaProjection projection = RakedApronProjection();
  ActionEffectProjectionContext context = {
    .bg1_camera_x = 1000,
    .bg1_camera_y = 500,
    .bg2_camera_x = 1000,
    .bg2_camera_y = 500,
    .ws_extra = 60,
    .ws_extra_top = 32,
    .visible_x0 = 0,
    .visible_width = 376,
    .snes_height = 224,
    .viewport = {11, 13, 752, 448},
    .diorama_projection = &projection,
  };
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  ActionSceneEffectRenderBatch pit, fireball;
  SDL_FPoint expected;

  /* camera-relative (-30,-7), plus display margins (60,32), produces the
   * captured anchor (30,25). This exercises the production adapter rather
   * than a test-local approximation of it. */
  frame.effects[0] = SceneEffect(kActionEffect_AitosLavaPit, 970);
  frame.effects[0].world_y = 493;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, ActionEffectProjection_ProjectPoint,
      &context, &pit));
  /* Vertex zero is the outer glow's centre. The pit spill follows the full
   * two-row bubbly geometry and must use BG1's rake/bow plus the apron. */
  CHECK(Diorama_ProjectCapturedBg1Point(
      &projection, 30.0f, 17.8f, &expected, NULL, NULL));
  CHECK(fabsf(pit.vertices[0].position.x - expected.x) < 0.001f);
  CHECK(fabsf(pit.vertices[0].position.y - expected.y) < 0.001f);

  frame.effects[0] = SceneEffect(kActionEffect_AitosLavaFireball, 970);
  frame.effects[0].world_y = 493;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, ActionEffectProjection_ProjectPoint,
      &context, &fireball));
  CHECK(Diorama_ProjectCapturedPoint(
      &projection, 30.0f, 25.0f, 0, &expected, NULL, NULL));
  CHECK(fabsf(fireball.vertices[0].position.x - expected.x) < 0.001f);
  CHECK(fabsf(fireball.vertices[0].position.y - expected.y) < 0.001f);

  /* The same capture point does not collapse onto one flat plane: BG1 lava
   * stays attached to the pit while its projectile occupies OBJ priority 0. */
  CHECK(fabsf(pit.vertices[0].position.x -
              fireball.vertices[0].position.x) > 5.0f);

  /* A BG2 waterfall uses its own camera and independently resolved backdrop
   * shape/window, rather than borrowing either BG1 or OBJ registration. */
  frame.effects[0] = SceneEffect(kActionEffect_AitosWaterfall, 970);
  frame.effects[0].world_y = 493;
  context.bg2_camera_x = 1010;
  context.bg2_camera_y = 510;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, ActionEffectProjection_ProjectPoint,
      &context, &pit));
  CHECK(Diorama_ProjectCapturedBg2Point(
      &projection, 20.0f, 83.0f, &expected, NULL, NULL));
  CHECK(fabsf(pit.vertices[0].position.x - expected.x) < 0.001f);
  CHECK(fabsf(pit.vertices[0].position.y - expected.y) < 0.001f);

  /* The bottom atmosphere deliberately draws after the world, but it still
   * uses the production BG2 camera/rake/bow projection so its foam seam meets
   * the finite waterfall plane in Diorama mode. */
  ActionSceneEffectFrame decorations = {
    .decoration_count = 1,
    .decoration_visible_count = 1,
  };
  decorations.decorations[0] =
      SceneEffect(kActionEffect_AitosWaterfallMist, 970);
  decorations.decorations[0].world_y = 493;
  CHECK(ActionSceneDecorationRender_Build(
      &decorations, kActionEffectRenderLayer_Atmosphere, true, false,
      ActionEffectProjection_ProjectPoint, &context, &pit));
  /* The first cloud anchor is deliberately stable; the remaining puffs drift
   * independently. screen X is -40, plus 60 capture margin, plus the first
   * of six lanes across local [-256,256]. screen Y is -17, plus local 48 and
   * the 32-row texture margin. */
  const float first_cloud_x = -256.0f + 512.0f * (0.5f / 6.0f);
  CHECK(Diorama_ProjectCapturedBg2Point(
      &projection, 20.0f + first_cloud_x, 63.0f,
      &expected, NULL, NULL));
  CHECK(fabsf(pit.vertices[0].position.x - expected.x) < 0.001f);
  CHECK(fabsf(pit.vertices[0].position.y - expected.y) < 0.001f);

  /* The same production helper owns flat viewport placement. Vertical
   * extension is a Diorama texture concern and intentionally drops out here. */
  frame.effects[0] = SceneEffect(kActionEffect_AitosLavaPit, 970);
  frame.effects[0].world_y = 493;
  context.diorama_projection = NULL;
  context.visible_x0 = 10;
  context.visible_width = 400;
  CHECK(ActionEffectProjection_ProjectPoint(
      &context, &frame.effects[0], 0.0f, 0.0f, &expected));
  CHECK(fabsf(expected.x - 48.6f) < 0.001f);
  CHECK(fabsf(expected.y - (-1.0f)) < 0.001f);
}

static void TestCurrentActorEffectsRequestExactObjPlanes(void) {
  ActionEffectFrame spells = {.effect_count = 2, .visible_count = 2};
  spells.effects[0] = Fire();
  spells.effects[0].obj_priority = 2;
  spells.effects[0].projection_plane = kActionEffectProjectionPlane_Obj;
  spells.effects[1] = Fire();
  spells.effects[1].flags = 0;

  ActionSceneEffectFrame scene = {.effect_count = 4, .visible_count = 4};
  scene.effects[0] = SceneEffect(kActionEffect_SwordBeam, 200);
  scene.effects[0].obj_priority = 0;
  scene.effects[1] = SceneEffect(kActionEffect_EnemyFireball, 220);
  scene.effects[1].obj_priority = 3;
  scene.effects[2] = SceneEffect(kActionEffect_WallTorch, 240);
  scene.effects[2].obj_priority = 1;
  scene.effects[3] = SceneEffect(kActionEffect_SwordBeam, 260);
  scene.effects[3].visual = 0x21;
  scene.effects[3].obj_priority = 2;
  CHECK(ActionEffectProjection_RequiredObjPriorityMask(&spells, &scene) ==
        ((1u << 0) | (1u << 2) | (1u << 3)));
  CHECK(ActionEffectProjection_RequiredObjPriorityMask(NULL, &scene) ==
        ((1u << 0) | (1u << 2) | (1u << 3)));

  /* A malformed actor list fails closed as a unit, without suppressing the
   * independently valid spell request. BG-local decorations never acquire an
   * OBJ projection merely because their priority byte happens to be set. */
  scene.overflow = 1;
  CHECK(ActionEffectProjection_RequiredObjPriorityMask(&spells, &scene) ==
        (1u << 2));
  scene.overflow = 0;
  scene.effect_count = kActionSceneEffectMaxInstances + 1;
  CHECK(ActionEffectProjection_RequiredObjPriorityMask(NULL, &scene) == 0);
}

static void TestDecorationLayerBuildsAreIndependent(void) {
  ActionSceneEffectFrame frame = {
    .decoration_count = 3,
    .decoration_visible_count = 3,
  };
  frame.decorations[0] = SceneEffect(kActionEffect_AitosWaterSplash, 100);
  frame.decorations[1] = SceneEffect(kActionEffect_AitosWaterfall, 120);
  frame.decorations[1].render_layer = kActionEffectRenderLayer_Bg2Plane;
  frame.decorations[2] = SceneEffect(kActionEffect_AitosWaterfallMist, 120);
  frame.decorations[2].world_y =
      kActRaiserAuthenticHeight +
      kActionBgAitosWaterfallBottomExtensionPixels;
  ActionSceneEffectRenderBatch world, bg2, atmosphere;
  CHECK(ActionSceneDecorationRender_Build(
      &frame, kActionEffectRenderLayer_WorldOverlay, true, true,
      IdentityProjection, NULL, &world));
  CHECK(ActionSceneDecorationRender_Build(
      &frame, kActionEffectRenderLayer_Bg2Plane, true, true,
      IdentityProjection, NULL, &bg2));
  CHECK(ActionSceneDecorationRender_Build(
      &frame, kActionEffectRenderLayer_Atmosphere, true, true,
      IdentityProjection, NULL, &atmosphere));
  CHECK(world.index_count > 0);
  CHECK(bg2.index_count > 0);
  CHECK(atmosphere.index_count > 0);
  CHECK(world.index_count != bg2.index_count);
  CHECK(atmosphere.index_count != bg2.index_count);
  CHECK(atmosphere.vertex_count ==
        kActionSceneEffectWaterfallMistCloudCount *
            kActionSceneEffectWaterfallMistCloudVertices +
        kActionSceneEffectWaterfallMistParticleCount * 4);
  CHECK(atmosphere.index_count ==
        kActionSceneEffectWaterfallMistCloudCount *
            kActionSceneEffectWaterfallMistCloudIndices +
        kActionSceneEffectWaterfallMistParticleCount * 6);

  /* The source veil remains substantial renderer geometry rather than a
   * record that gets silently filtered, while the separate atmosphere spans
   * the bottom seam and extends below the authentic 224px frame. */
  CHECK(bg2.vertex_count ==
        2 * kActionEffectGlowVertices +
        kActionSceneEffectWaterfallParticleCount * 4);
  float visible_veil_max_y = -10000.0f;
  for (int i = 0; i < bg2.vertex_count; i++) {
    if (bg2.vertices[i].color.a > 0.01f &&
        bg2.vertices[i].position.y > visible_veil_max_y)
      visible_veil_max_y = bg2.vertices[i].position.y;
  }
  /* The source waterfall record now covers the repeated geometry too. This
   * rejects a raw overflow quad that stops receiving the main flow veil at the
   * authentic plane edge. */
  CHECK(visible_veil_max_y > frame.decorations[1].world_y + 300.0f);
  float atmosphere_min_y = 10000.0f;
  float atmosphere_max_y = -10000.0f;
  float visible_atmosphere_min_y = 10000.0f;
  float visible_atmosphere_max_y = -10000.0f;
  for (int i = 0; i < atmosphere.vertex_count; i++) {
    if (atmosphere.vertices[i].position.y < atmosphere_min_y)
      atmosphere_min_y = atmosphere.vertices[i].position.y;
    if (atmosphere.vertices[i].position.y > atmosphere_max_y)
      atmosphere_max_y = atmosphere.vertices[i].position.y;
    if (atmosphere.vertices[i].color.a > 0.02f &&
        atmosphere.vertices[i].position.y < visible_atmosphere_min_y)
      visible_atmosphere_min_y = atmosphere.vertices[i].position.y;
    if (atmosphere.vertices[i].color.a > 0.02f) {
      if (atmosphere.vertices[i].position.y > visible_atmosphere_max_y)
        visible_atmosphere_max_y = atmosphere.vertices[i].position.y;
    }
  }
  CHECK(atmosphere_min_y < frame.decorations[2].world_y - 20.0f);
  CHECK(atmosphere_max_y > frame.decorations[2].world_y + 20.0f);
  CHECK(atmosphere_max_y > 224.0f);
  /* A transparent outer ring used to be the only geometry reaching the
   * unsupported rows. Pin visible colour beyond the 24px-safe BG2 seam. */
  CHECK(visible_atmosphere_max_y >
        kActRaiserAuthenticHeight +
            kActionBgAitosWaterfallBottomExtensionPixels + 20.0f);
  CHECK(visible_atmosphere_max_y - visible_atmosphere_min_y > 100.0f);
  /* Four tiers of six independently placed puffs replace the six huge banks.
   * Pin the volume cues: cloud centres span the camera width and several
   * heights, every second ring remains visible, every outer ring feathers to
   * zero, and the irregular visible bottoms differ enough that they cannot
   * converge into the old horizontal shelf. */
  float cloud_centre_min_x = 10000.0f, cloud_centre_max_x = -10000.0f;
  float cloud_centre_min_y = 10000.0f, cloud_centre_max_y = -10000.0f;
  float shallowest_cloud_bottom = 10000.0f;
  float deepest_cloud_bottom = -10000.0f;
  for (int cloud = 0;
       cloud < kActionSceneEffectWaterfallMistCloudCount; cloud++) {
    const int cloud_base =
        cloud * kActionSceneEffectWaterfallMistCloudVertices;
    const SDL_Vertex *centre = &atmosphere.vertices[cloud_base];
    if (centre->position.x < cloud_centre_min_x)
      cloud_centre_min_x = centre->position.x;
    if (centre->position.x > cloud_centre_max_x)
      cloud_centre_max_x = centre->position.x;
    if (centre->position.y < cloud_centre_min_y)
      cloud_centre_min_y = centre->position.y;
    if (centre->position.y > cloud_centre_max_y)
      cloud_centre_max_y = centre->position.y;
    CHECK(centre->color.a > 0.05f);
    const int visible_ring =
        cloud_base + 1 + kActionSceneEffectWaterfallMistCloudSegments;
    const int transparent_ring = visible_ring +
        kActionSceneEffectWaterfallMistCloudSegments;
    float cloud_bottom = -10000.0f;
    for (int segment = 0;
         segment < kActionSceneEffectWaterfallMistCloudSegments; segment++) {
      const SDL_Vertex *vertex =
          &atmosphere.vertices[visible_ring + segment];
      CHECK(vertex->color.a > 0.02f);
      CHECK(atmosphere.vertices[transparent_ring + segment].color.a == 0.0f);
      if (vertex->position.y > cloud_bottom)
        cloud_bottom = vertex->position.y;
    }
    if (cloud_bottom < shallowest_cloud_bottom)
      shallowest_cloud_bottom = cloud_bottom;
    if (cloud_bottom > deepest_cloud_bottom)
      deepest_cloud_bottom = cloud_bottom;
  }
  CHECK(cloud_centre_max_x - cloud_centre_min_x > 400.0f);
  CHECK(cloud_centre_max_y - cloud_centre_min_y > 55.0f);
  CHECK(deepest_cloud_bottom - shallowest_cloud_bottom > 75.0f);
  CHECK(deepest_cloud_bottom >
        kActRaiserAuthenticHeight +
            kActionBgAitosWaterfallBottomExtensionPixels + 100.0f);
  frame.decoration_overflow = 1;
  CHECK(ActionSceneDecorationRender_Build(
      &frame, kActionEffectRenderLayer_WorldOverlay, true, true,
      IdentityProjection, NULL, &world));
  CHECK(world.index_count == 0);
  CHECK(!ActionSceneDecorationRender_Build(
      &frame, kActionEffectRenderLayer_Count, true, true,
      IdentityProjection, NULL, &world));
}

static void TestLightningVisibleLightCoversCapturedArc(void) {
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  frame.effects[0] = SceneEffect(kActionEffect_LightningTrap, 300);
  ActionSceneEffectRenderBatch batch;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &batch));

  float visible_min_y = 10000.0f;
  float visible_max_y = -10000.0f;
  for (int i = 0; i < batch.vertex_count; i++) {
    /* Ignore the fully transparent outer falloff. This checks what a viewer
     * can actually see, which is precisely how the half-arc bug escaped the
     * earlier vertex-count coverage. */
    if (batch.vertices[i].color.a < 0.10f) continue;
    if (batch.vertices[i].position.y < visible_min_y)
      visible_min_y = batch.vertices[i].position.y;
    if (batch.vertices[i].position.y > visible_max_y)
      visible_max_y = batch.vertices[i].position.y;
  }
  const float top = frame.effects[0].world_y - 88.0f;
  const float bottom = frame.effects[0].world_y + 88.0f;
  CHECK(visible_min_y <= top + 2.0f);
  CHECK(visible_max_y >= bottom - 2.0f);
}

static void TestBossLightningFilamentAndStages(void) {
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  frame.effects[0] = SceneEffect(
      kActionEffect_BloodpoolBossLightning, 300);
  ActionSceneEffectRenderBatch lighting, particles, repeat;
  static const uint8_t kLeft[] = {6, 6, 1, 48, 36, 30};
  static const uint8_t kRight[] = {11, 11, 11, 8, 8, 8};
  static const uint8_t kBottom[] = {117, 69, 21, 117, 69, 21};
  static const unsigned kSegments[] = {24, 18, 12, 24, 18, 12};
  static const float kFirstMidX[] = {4, 4, 4, -2, -2, -2};
  static const float kLastMidX[] = {5, 4.5f, 6.5f, -44, -29, -24};
  static const float kLastMidY[] = {108, 60, 12, 108, 60, 12};

  /* Every `$7E:5000` strike visual has its own composition path. Check both
   * authored families, all three lengths, and horizontal mirroring by reading
   * the segment-centre positions back out of the generated ribbon quads. */
  for (unsigned visual = 0; visual < 6; visual++) {
    for (unsigned flipped = 0; flipped < 2; flipped++) {
      ActionEffectInstance *effect = &frame.effects[0];
      effect->visual = (uint16_t)visual;
      effect->flags = kActionEffectFlag_Visible |
          (flipped ? kActionEffectFlag_FlipHorizontal : 0);
      effect->geometry.data.rect = (ActionEffectLocalRect){
        -(float)(flipped ? kRight[visual] : kLeft[visual]), -83.0f,
        (float)(flipped ? kLeft[visual] : kRight[visual]),
        (float)kBottom[visual],
      };
      CHECK(ActionSceneEffectRender_Build(
          &frame, true, false, IdentityProjection, NULL, &lighting));
      CHECK(lighting.vertex_count == 2 * kActionEffectGlowVertices +
                                        (int)kSegments[visual] * 4 * 2);
      CHECK(lighting.index_count == 2 * kActionEffectGlowIndices +
                                       (int)kSegments[visual] * 6 * 2);

      const int first = 2 * kActionEffectGlowVertices;
      const int last = first + ((int)kSegments[visual] - 1) * 4;
      SDL_FPoint first_centre = {0}, last_centre = {0};
      for (int i = 0; i < 4; i++) {
        first_centre.x += lighting.vertices[first + i].position.x * 0.25f;
        first_centre.y += lighting.vertices[first + i].position.y * 0.25f;
        last_centre.x += lighting.vertices[last + i].position.x * 0.25f;
        last_centre.y += lighting.vertices[last + i].position.y * 0.25f;
      }
      const float mirror = flipped ? -1.0f : 1.0f;
      CHECK(fabsf(first_centre.x -
                  (effect->world_x + mirror * kFirstMidX[visual])) < 0.01f);
      CHECK(fabsf(first_centre.y - (effect->world_y - 76.0f)) < 0.01f);
      CHECK(fabsf(last_centre.x -
                  (effect->world_x + mirror * kLastMidX[visual])) < 0.01f);
      CHECK(fabsf(last_centre.y -
                  (effect->world_y + kLastMidY[visual])) < 0.01f);
    }
  }

  frame.effects[0] = SceneEffect(
      kActionEffect_BloodpoolBossLightning, 300);
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &lighting));
  CHECK(ActionSceneEffectRender_Build(
      &frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count ==
        kActionSceneEffectParticlesPerInstance * 4);
  CHECK(particles.index_count ==
        kActionSceneEffectParticlesPerInstance * 6);
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &repeat));
  CHECK(SceneBatchesEqual(&lighting, &repeat));

  /* Only the linked state-$09 child receives the floor bloom. The shared
   * visual-$20 blank cycle is rejected by capture and has no renderer phase. */
  frame.effects[0].phase = kActionEffectPhase_BossLightningImpact;
  frame.effects[0].visual = 10;
  frame.effects[0].geometry.data.rect =
      (ActionEffectLocalRect){-16.0f, -16.0f, 16.0f, 0.0f};
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertex_count == 2 * kActionEffectGlowVertices);
  frame.effects[0].phase = 99;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertex_count == 0);
  CHECK(repeat.index_count == 0);
}

static void TestMarahnaLightningLinksAndOrientations(void) {
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  frame.effects[0] = SceneEffect(
      kActionEffect_MarahnaLightningLink, 300);
  ActionSceneEffectRenderBatch horizontal, vertical, particles, repeat;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &horizontal));
  CHECK(horizontal.vertex_count == 2 * kActionEffectGlowVertices +
      kActionSceneEffectMarahnaLightningSegments * 4 * 2);
  CHECK(horizontal.index_count == 2 * kActionEffectGlowIndices +
      kActionSceneEffectMarahnaLightningSegments * 6 * 2);
  const int ribbon = 2 * kActionEffectGlowVertices;
  SDL_FPoint first_centre = {0}, last_centre = {0};
  for (int i = 0; i < 4; i++) {
    first_centre.x += horizontal.vertices[ribbon + i].position.x * 0.25f;
    first_centre.y += horizontal.vertices[ribbon + i].position.y * 0.25f;
    const int last = ribbon +
        (kActionSceneEffectMarahnaLightningSegments - 1) * 4 + i;
    last_centre.x += horizontal.vertices[last].position.x * 0.25f;
    last_centre.y += horizontal.vertices[last].position.y * 0.25f;
  }
  CHECK(fabsf(first_centre.x - 264.0f) < 0.01f);
  CHECK(fabsf(last_centre.x - 336.0f) < 0.01f);

  CHECK(ActionSceneEffectRender_Build(
      &frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count ==
        kActionSceneEffectParticlesPerInstance * 4);
  CHECK(particles.index_count ==
        kActionSceneEffectParticlesPerInstance * 6);
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &repeat));
  CHECK(SceneBatchesEqual(&horizontal, &repeat));
  frame.effects[0].phase_ticks++;
  frame.effects[0].pulse_ticks++;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &repeat));
  CHECK(!SceneBatchesEqual(&horizontal, &repeat));

  frame.effects[0].visual = 0x31;
  frame.effects[0].animation_state = 0x28;
  frame.effects[0].geometry.data.rect =
      (ActionEffectLocalRect){-5.0f, -40.0f, 5.0f, 40.0f};
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &vertical));
  CHECK(vertical.vertex_count == horizontal.vertex_count);
  first_centre = (SDL_FPoint){0};
  last_centre = (SDL_FPoint){0};
  for (int i = 0; i < 4; i++) {
    first_centre.x += vertical.vertices[ribbon + i].position.x * 0.25f;
    first_centre.y += vertical.vertices[ribbon + i].position.y * 0.25f;
    const int last = ribbon +
        (kActionSceneEffectMarahnaLightningSegments - 1) * 4 + i;
    last_centre.x += vertical.vertices[last].position.x * 0.25f;
    last_centre.y += vertical.vertices[last].position.y * 0.25f;
  }
  CHECK(fabsf(first_centre.y - 84.0f) < 0.01f);
  CHECK(fabsf(last_centre.y - 156.0f) < 0.01f);

  frame.effects[0].animation_state = 0x27;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertex_count == 0);
}

static void TestMarahnaBossLightningStagesAndOrientations(void) {
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  frame.effects[0] = SceneEffect(
      kActionEffect_MarahnaBossLightning, 300);
  ActionSceneEffectRenderBatch left, right, charge, orb, ground_right,
      ground_left, particles;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &left));
  CHECK(left.vertex_count == 2 * kActionEffectGlowVertices +
      kActionSceneEffectMarahnaBossLightningSegments * 4 * 2);
  CHECK(left.index_count == 2 * kActionEffectGlowIndices +
      kActionSceneEffectMarahnaBossLightningSegments * 6 * 2);

  const int ribbon = 2 * kActionEffectGlowVertices;
  SDL_FPoint first = {0}, last = {0};
  for (int i = 0; i < 4; i++) {
    first.x += left.vertices[ribbon + i].position.x * 0.25f;
    first.y += left.vertices[ribbon + i].position.y * 0.25f;
    const int end = ribbon +
        (kActionSceneEffectMarahnaBossLightningSegments - 1) * 4 + i;
    last.x += left.vertices[end].position.x * 0.25f;
    last.y += left.vertices[end].position.y * 0.25f;
  }
  /* These are first/last segment centres, so each includes one animated
   * interior joint. Keep them close to the measured quadrant endpoints while
   * allowing the deliberate electrical bend. */
  CHECK(fabsf(first.x - 298.0f) < 5.0f);
  CHECK(fabsf(first.y - 122.0f) < 5.0f);
  CHECK(fabsf(last.x - 270.0f) < 5.0f);
  CHECK(fabsf(last.y - 150.0f) < 5.0f);

  frame.effects[0].velocity_x = 4;
  frame.effects[0].geometry.data.rect =
      (ActionEffectLocalRect){0.0f, 0.0f, 32.0f, 32.0f};
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &right));
  first = (SDL_FPoint){0};
  last = (SDL_FPoint){0};
  for (int i = 0; i < 4; i++) {
    first.x += right.vertices[ribbon + i].position.x * 0.25f;
    const int end = ribbon +
        (kActionSceneEffectMarahnaBossLightningSegments - 1) * 4 + i;
    last.x += right.vertices[end].position.x * 0.25f;
  }
  CHECK(fabsf(first.x - 302.0f) < 5.0f);
  CHECK(fabsf(last.x - 330.0f) < 5.0f);

  frame.effects[0].velocity_x = 0;
  frame.effects[0].velocity_y = 0;
  frame.effects[0].phase =
      kActionEffectPhase_MarahnaBossLightningCharge;
  frame.effects[0].visual = 7;
  frame.effects[0].geometry.data.rect =
      (ActionEffectLocalRect){-48.0f, -40.0f, 48.0f, 8.0f};
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &charge));
  CHECK(charge.vertex_count == 2 * kActionEffectGlowVertices);
  CHECK(fabsf(charge.vertices[0].position.y - 96.0f) < 0.01f);

  frame.effects[0].phase = kActionEffectPhase_MarahnaBossLightningOrb;
  frame.effects[0].visual = 10;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &orb));
  CHECK(orb.vertex_count == charge.vertex_count);
  CHECK(!SceneBatchesEqual(&charge, &orb));
  CHECK(ActionSceneEffectRender_Build(
      &frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count ==
        kActionSceneEffectParticlesPerInstance * 4);

  frame.effects[0].phase =
      kActionEffectPhase_MarahnaBossLightningGroundCharge;
  frame.effects[0].visual = 0x12;
  frame.effects[0].velocity_x = 4;
  frame.effects[0].velocity_y = 0;
  frame.effects[0].geometry.data.rect =
      (ActionEffectLocalRect){-8.0f, -8.0f, 8.0f, 8.0f};
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &ground_right));
  CHECK(ground_right.vertex_count == 2 * kActionEffectGlowVertices);
  CHECK(fabsf(ground_right.vertices[0].position.x - 296.0f) < 0.01f);
  CHECK(fabsf(ground_right.vertices[0].position.y - 120.0f) < 0.01f);
  CHECK(ActionSceneEffectRender_Build(
      &frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count ==
        kActionSceneEffectParticlesPerInstance * 4);
  float right_particle_mean_x = 0.0f;
  for (int i = 0; i < particles.vertex_count; i++)
    right_particle_mean_x += particles.vertices[i].position.x;
  right_particle_mean_x /= (float)particles.vertex_count;
  CHECK(right_particle_mean_x < 300.0f);

  frame.effects[0].velocity_x = -4;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &ground_left));
  CHECK(fabsf(ground_left.vertices[0].position.x - 304.0f) < 0.01f);
  CHECK(ActionSceneEffectRender_Build(
      &frame, false, true, IdentityProjection, NULL, &particles));
  float left_particle_mean_x = 0.0f;
  for (int i = 0; i < particles.vertex_count; i++)
    left_particle_mean_x += particles.vertices[i].position.x;
  left_particle_mean_x /= (float)particles.vertex_count;
  CHECK(left_particle_mean_x > 300.0f);

  /* All three loaded frames belong to the one ground lifecycle; the next
   * visual must fail closed even if its phase is forged. */
  for (uint16_t visual = 0x13; visual <= 0x14; visual++) {
    frame.effects[0].visual = visual;
    frame.effects[0].geometry.data.rect =
        (ActionEffectLocalRect){-16.0f, -16.0f, 16.0f, 16.0f};
    CHECK(ActionSceneEffectRender_Build(
        &frame, true, true, IdentityProjection, NULL, &particles));
    CHECK(particles.vertex_count > 0);
  }
  frame.effects[0].visual = 0x15;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == 0);

  frame.effects[0].phase = 99;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == 0);
}

static void TestSwordBeamLightingTrailAndStars(void) {
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  frame.effects[0] = SceneEffect(kActionEffect_SwordBeam, 300);
  ActionSceneEffectRenderBatch lighting, particles, repeat;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &lighting));
  CHECK(lighting.vertex_count == 2 * kActionEffectGlowVertices + 8);
  CHECK(lighting.index_count == 2 * kActionEffectGlowIndices + 12);
  /* Run 20260810-184935 proves the normal state-$13 crescent centre is local
   * (40,-17), not (8,15). Keep the core there and lean only the spill 2px
   * into the wake. Both haze layers meet the decoded 32px crescent height,
   * then taper over their 80px and 56px lengths. */
  CHECK(fabsf(lighting.vertices[0].position.x - 338.0f) < 0.01f);
  CHECK(fabsf(lighting.vertices[0].position.y - 103.0f) < 0.01f);
  CHECK(fabsf(lighting.vertices[kActionEffectGlowVertices].position.x -
              340.0f) < 0.01f);
  const int trail = 2 * kActionEffectGlowVertices;
  CHECK(fabsf(lighting.vertices[trail + 2].position.x - 260.0f) < 0.01f);
  CHECK(lighting.vertices[trail + 2].color.a == 0.0f);
  CHECK(fabsf(lighting.vertices[trail + 6].position.x - 284.0f) < 0.01f);
  CHECK(fabsf(lighting.vertices[trail].position.y -
              lighting.vertices[trail + 1].position.y) > 30.0f);

  CHECK(ActionSceneEffectRender_Build(
      &frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == kActionSceneEffectSwordStarCount * 8);
  CHECK(particles.index_count == kActionSceneEffectSwordStarCount * 12);
  float nearest_star_x = -10000.0f, farthest_star_x = 10000.0f;
  float near_min_y = 10000.0f, near_max_y = -10000.0f;
  int near_star_count = 0;
  for (int i = 0; i < kActionSceneEffectSwordStarCount; i++) {
    const int base = i * 8;
    const float star_x =
        (particles.vertices[base].position.x +
         particles.vertices[base + 2].position.x) * 0.5f;
    const float star_y =
        (particles.vertices[base].position.y +
         particles.vertices[base + 2].position.y) * 0.5f;
    CHECK(star_x < 340.0f);  /* every glint is behind the rightward crescent */
    if (star_x > nearest_star_x) nearest_star_x = star_x;
    if (star_x < farthest_star_x) farthest_star_x = star_x;
    if (star_x > 320.0f) {
      near_star_count++;
      if (star_y < near_min_y) near_min_y = star_y;
      if (star_y > near_max_y) near_max_y = star_y;
    }
  }
  CHECK(nearest_star_x - farthest_star_x > 70.0f);
  CHECK(near_star_count >= 6);
  CHECK(near_max_y - near_min_y > 24.0f);
  CHECK(ActionSceneEffectRender_Build(
      &frame, false, true, IdentityProjection, NULL, &repeat));
  CHECK(SceneBatchesEqual(&particles, &repeat));
  frame.effects[0].pulse_ticks++;
  CHECK(ActionSceneEffectRender_Build(
      &frame, false, true, IdentityProjection, NULL, &repeat));
  CHECK(!SceneBatchesEqual(&particles, &repeat));
  bool materialization_changed = false;
  for (int i = 0; i < kActionSceneEffectSwordStarCount; i++) {
    const int base = i * 8;
    const float before_x =
        (particles.vertices[base].position.x +
         particles.vertices[base + 2].position.x) * 0.5f;
    const float before_y =
        (particles.vertices[base].position.y +
         particles.vertices[base + 2].position.y) * 0.5f;
    const float after_x =
        (repeat.vertices[base].position.x +
         repeat.vertices[base + 2].position.x) * 0.5f;
    const float after_y =
        (repeat.vertices[base].position.y +
         repeat.vertices[base + 2].position.y) * 0.5f;
    CHECK(fabsf(before_x - after_x) < 0.01f);
    CHECK(fabsf(before_y - after_y) < 0.01f);
    if (fabsf(particles.vertices[base].color.a -
              repeat.vertices[base].color.a) > 0.001f)
      materialization_changed = true;
  }
  CHECK(materialization_changed);
  frame.effects[0].pulse_ticks--;

  frame.effects[0].velocity_x = -8;
  frame.effects[0].flags |= kActionEffectFlag_FlipHorizontal;
  frame.effects[0].geometry.data.rect =
      (ActionEffectLocalRect){-48.0f, -33.0f, -32.0f, -1.0f};
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, false, IdentityProjection, NULL, &repeat));
  CHECK(fabsf(repeat.vertices[0].position.x - 262.0f) < 0.01f);
  CHECK(fabsf(repeat.vertices[trail + 2].position.x - 340.0f) < 0.01f);
  CHECK(fabsf(repeat.vertices[trail + 6].position.x - 316.0f) < 0.01f);

  frame.effects[0].visual = 0x31;
  frame.effects[0].flags &= ~kActionEffectFlag_FlipHorizontal;
  frame.effects[0].geometry.data.rect =
      (ActionEffectLocalRect){40.0f, -9.0f, 56.0f, 23.0f};
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertex_count > 0);
  /* Aitos's boss-authored upper/lower crescents use the same portable comet
   * style while retaining their diagonal headings and priority-2 source. */
  frame.effects[0].visual = 0x21;
  frame.effects[0].velocity_x = -3;
  frame.effects[0].velocity_y = 1;
  frame.effects[0].obj_priority = 2;
  frame.effects[0].geometry.data.rect =
      (ActionEffectLocalRect){-8.0f, -17.0f, 16.0f, 7.0f};
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertex_count ==
        2 * kActionEffectGlowVertices + 8 +
        kActionSceneEffectSwordStarCount * 8);
  frame.effects[0].visual = 0x20;
  frame.effects[0].velocity_y = -1;
  frame.effects[0].geometry.data.rect =
      (ActionEffectLocalRect){-8.0f, -9.0f, 16.0f, 15.0f};
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertex_count > 0);
  /* Reflected state 1 from run 20260812-224123 travels right/up. Its wake
   * must therefore taper left/down, proving the newly admitted facing reaches
   * the heading-driven renderer rather than merely passing capture. */
  frame.effects[0].visual = 0x21;
  frame.effects[0].velocity_x = 3;
  frame.effects[0].velocity_y = -1;
  frame.effects[0].flags = kActionEffectFlag_Visible |
      kActionEffectFlag_FlipHorizontal | kActionEffectFlag_FlipVertical;
  frame.effects[0].geometry.data.rect =
      (ActionEffectLocalRect){-16.0f, -9.0f, 8.0f, 15.0f};
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertices[trail].position.x >
        repeat.vertices[trail + 2].position.x);
  CHECK(repeat.vertices[trail].position.y <
        repeat.vertices[trail + 2].position.y);
  frame.effects[0].visual = 0x32;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertex_count == 0);
}

static void TestSceneCapacityAndMalformedInput(void) {
  ActionSceneEffectFrame frame = {
    .effect_count = kActionSceneEffectMaxInstances,
    .visible_count = kActionSceneEffectMaxInstances,
  };
  frame.effects[0] = SceneEffect(
      kActionEffect_BloodpoolBossLightning, 100);
  frame.effects[0].visual = 0;
  frame.effects[0].geometry.data.rect =
      (ActionEffectLocalRect){-6.0f, -83.0f, 11.0f, 117.0f};
  frame.effects[1] = SceneEffect(kActionEffect_SwordBeam, 120);
  for (unsigned i = 2;
       i < 2 + kActionSceneEffectMaxMarahnaLightningLinks; i++)
    frame.effects[i] = SceneEffect(
        kActionEffect_MarahnaLightningLink, 100 + i * 20);
  for (unsigned i = 2 + kActionSceneEffectMaxMarahnaLightningLinks;
       i < kActionSceneEffectMaxInstances; i++)
    frame.effects[i] = SceneEffect(kActionEffect_LightningTrap, 100 + i * 20);
  frame.effects[7] = SceneEffect(
      kActionEffect_MarahnaBossLightning, 140);
  frame.effects[8] = SceneEffect(kActionEffect_AitosWaterfall, 180);
  frame.effects[9] = SceneEffect(kActionEffect_SwordBeam, 200);
  frame.effects[9].visual = 0x21;
  frame.effects[9].velocity_x = -3;
  frame.effects[9].velocity_y = 1;
  frame.effects[10] = SceneEffect(kActionEffect_SwordBeam, 220);
  frame.effects[10].visual = 0x20;
  frame.effects[10].velocity_x = -3;
  frame.effects[10].velocity_y = -1;
  ActionSceneEffectRenderBatch batch;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &batch));
  /* The shared public capacity additionally reserves one bottom-atmosphere
   * record for the decoration builder; this actor-only worst case must remain
   * within it without manufacturing that separate map-derived record. */
  CHECK(batch.vertex_count ==
        kActionSceneEffectRenderMaxVertices -
        kActionSceneEffectWaterfallMistExtraVertices);
  CHECK(batch.index_count ==
        kActionSceneEffectRenderMaxIndices -
        kActionSceneEffectWaterfallMistExtraIndices);

  /* A second active boss filament cannot arise from the mapped one-boss
   * lifecycle. Reject it as a capacity-contract violation without publishing
   * a partial batch instead of reserving 16 impossible ribbons on the stack. */
  frame.effects[7] = SceneEffect(
      kActionEffect_BloodpoolBossLightning, 140);
  CHECK(!ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 0);
  CHECK(batch.index_count == 0);
  /* The expanded comet budget admits exactly the player plus the boss's two
   * diagonal children, not an arbitrary fourth forged stream. */
  frame.effects[7] = SceneEffect(kActionEffect_SwordBeam, 140);
  CHECK(!ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 0);
  CHECK(batch.index_count == 0);
  /* The runtime emitter admits five links. A forged sixth link must fail the
   * same cardinality contract as duplicate boss/player streams. */
  frame.effects[7] = SceneEffect(
      kActionEffect_MarahnaLightningLink, 140);
  CHECK(!ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 0);
  CHECK(batch.index_count == 0);
  frame.effects[7] = SceneEffect(
      kActionEffect_MarahnaBossLightning, 140);
  frame.effects[8] = SceneEffect(
      kActionEffect_MarahnaBossLightning, 160);
  CHECK(!ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 0);
  CHECK(batch.index_count == 0);
  frame.effects[8] = SceneEffect(kActionEffect_LightningTrap, 160);
  frame.effects[7] = SceneEffect(kActionEffect_LightningTrap, 140);

  frame.overflow = 1;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.index_count == 0);
  frame.overflow = 0;
  frame.effect_count = kActionSceneEffectMaxInstances + 1;
  CHECK(!ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.index_count == 0);
  CHECK(!ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, NULL));
}

int main(void) {
  TestFeatureSwitchesAndDeterminism();
  TestMixedStagesAreOrderIndependent();
  TestClocksAndValidation();
  TestCapacityIsDerivedFromPublishedLimits();
  TestSceneFeatureSwitchesAndDeterminism();
  TestSceneKindsRemainIndependent();
  TestAitosLavaLightingAndParticles();
  TestMarahnaFireballFramesAndDirections();
  TestAitosUsesRakedDioramaSourcePlanes();
  TestCurrentActorEffectsRequestExactObjPlanes();
  TestDecorationLayerBuildsAreIndependent();
  TestLightningVisibleLightCoversCapturedArc();
  TestBossLightningFilamentAndStages();
  TestMarahnaLightningLinksAndOrientations();
  TestMarahnaBossLightningStagesAndOrientations();
  TestSwordBeamLightingTrailAndStars();
  TestSceneCapacityAndMalformedInput();
  if (g_failures) {
    fprintf(stderr, "%d action-effect render test(s) failed\n", g_failures);
    return 1;
  }
  puts("action effect render: all tests passed");
  return 0;
}
