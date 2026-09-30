#include <math.h>
#include <stdio.h>
#include <string.h>

#include "action_effect_render.h"
#include "action_effect_projection.h"
#include "action_bg_plan.h"
#include "actraiser_game.h"
#include "diorama.h"

static int s_failures;

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);                \
      s_failures++;                                                                                \
    }                                                                                              \
  } while (0)

static bool EffectBatchesEqual(const ActionEffectRenderBatch *a, const ActionEffectRenderBatch *b) {
  return a->vertex_count == b->vertex_count && a->index_count == b->index_count &&
         (!a->vertex_count || memcmp(a->vertices, b->vertices,
                                     (size_t)a->vertex_count * sizeof(a->vertices[0])) == 0) &&
         (!a->index_count ||
          memcmp(a->indices, b->indices, (size_t)a->index_count * sizeof(a->indices[0])) == 0);
}

static bool SceneBatchesEqual(const ActionSceneEffectRenderBatch *a,
                              const ActionSceneEffectRenderBatch *b) {
  return a->vertex_count == b->vertex_count && a->index_count == b->index_count &&
         (!a->vertex_count || memcmp(a->vertices, b->vertices,
                                     (size_t)a->vertex_count * sizeof(a->vertices[0])) == 0) &&
         (!a->index_count ||
          memcmp(a->indices, b->indices, (size_t)a->index_count * sizeof(a->indices[0])) == 0);
}

static bool IdentityProjection(void *userdata, const ActionEffectInstance *effect, float local_x,
                               float local_y, ArRenderPointF *point) {
  (void)userdata;
  if (!effect || !point) return false;
  *point = (ArRenderPointF){
      effect->world_x + local_x,
      effect->world_y + local_y,
  };
  return true;
}

static DioramaProjection RakedApronProjection(void) {
  DioramaProjection projection = {
      .valid = true,
      .matrix =
          {
              1,
              0,
              0,
              0,
              0,
              1,
              0,
              0,
              1,
              0,
              1,
              0,
              0,
              0,
              0,
              1,
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
      .u0 = 0.20f,
      .v0 = 0.0f,
      .u1 = 0.80f,
      .v1 = 1.0f,
      .z_world = 0.35f,
      .rake = 0.25f,
      .bow = 0.10f,
  };
  projection.bg2_plane = (DioramaPlaneProjection){
      .valid = true,
      .u0 = 0.20f,
      .v0 = 0.0f,
      .u1 = 0.80f,
      .v1 = 1.0f,
      .z_world = -0.30f,
      .rake = 0.04f,
      /* Production waterfall rooms publish a folded continuation below BG2.
       * A zero-height test fold retains the simple expected projection while
       * still exercising the continuation eligibility contract. */
      .overflow_valid = true,
  };
  projection.bg1_high_plane = (DioramaPlaneProjection){
      .valid = true,
      .u0 = 0.20f,
      .v0 = 0.0f,
      .u1 = 0.80f,
      .v1 = 1.0f,
      .z_world = 0.45f,
      .rake = 0.22f,
      .bow = 0.08f,
  };
  projection.object_planes[0] = (DioramaPlaneProjection){
      .valid = true,
      .u0 = 0.20f,
      .v0 = 0.0f,
      .u1 = 0.80f,
      .v1 = 1.0f,
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
      .geometry =
          {
              .kind = kActionEffectGeometry_Rect,
              .data.rect = {-44.0f, -29.0f, 8.0f, 30.0f},
          },
  };
  return effect;
}

static void TestFeatureSwitchesAndDeterminism(void) {
  ActionEffectFrame frame = {.effect_count = 1};
  frame.effects[0] = Fire();
  ActionEffectRenderBatch lighting, particles, both, repeat;

  /* A lone part yields two glows: the burst-wide light spill, plus the one
   * flame its single cluster produces. */
  CHECK(ActionEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &lighting));
  CHECK(lighting.vertex_count == 2 * kActionEffectGlowVertices);
  CHECK(lighting.index_count == 2 * kActionEffectGlowIndices);
  /* Bloom emits a full complement of embers, which is what makes the
   * capacity constants below tight rather than merely sufficient. */
  CHECK(ActionEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == kActionEffectMaxEmbers * 4);
  CHECK(particles.index_count == kActionEffectMaxEmbers * 6);
  CHECK(ActionEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &both));
  CHECK(both.vertex_count == lighting.vertex_count + particles.vertex_count);
  CHECK(both.index_count == lighting.index_count + particles.index_count);
  CHECK(ActionEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(EffectBatchesEqual(&both, &repeat));

  memset(&repeat, 0xFF, sizeof(repeat));
  CHECK(ActionEffectRender_Build(&frame, false, false, NULL, NULL, &repeat));
  CHECK(repeat.vertex_count == 0);
  CHECK(repeat.index_count == 0);
}

static void TestClocksAndValidation(void) {
  ActionEffectFrame frame = {.effect_count = 1};
  frame.effects[0] = Fire();
  ActionEffectRenderBatch first, changed, skipped;
  CHECK(ActionEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &first));
  frame.effects[0].pulse_ticks++;
  CHECK(ActionEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &changed));
  CHECK(!EffectBatchesEqual(&first, &changed));

  frame.effects[0] = Fire();
  frame.effects[0].kind = 99;
  CHECK(ActionEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &skipped));
  CHECK(skipped.index_count == 0);
  frame.effects[0] = Fire();
  frame.effects[0].phase = 99;
  CHECK(ActionEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &skipped));
  CHECK(skipped.index_count == 0);
  frame.effects[0] = Fire();
  frame.effects[0].geometry.kind = kActionEffectGeometry_None;
  CHECK(ActionEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &skipped));
  CHECK(skipped.index_count == 0);
  frame.effects[0] = Fire();
  frame.effects[0].render_layer = 99;
  CHECK(ActionEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &skipped));
  CHECK(skipped.index_count == 0);

  frame.effect_count = kActionEffectMaxInstances + 1;
  CHECK(!ActionEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &skipped));
  CHECK(skipped.index_count == 0);
  CHECK(!ActionEffectRender_Build(&frame, true, true, IdentityProjection, NULL, NULL));
}

/* The published capacity must be reachable, not merely generous — otherwise
 * the Reserve() guards are never exercised by anything. The worst case is
 * every part landing in its OWN cluster, so the parts are spread far enough
 * apart that none of them touch: that yields one flame per part plus the
 * burst spill, which is exactly kActionEffectMaxGlows. */
static void TestCapacityIsDerivedFromPublishedLimits(void) {
  ActionEffectFrame frame = {.effect_count = kActionEffectMaxInstances};
  for (unsigned i = 0; i < kActionEffectMaxInstances; i++) {
    frame.effects[i] = Fire();
    frame.effects[i].record_address += i * 0x40;
    frame.effects[i].world_x += (int)i * 4000;
  }
  ActionEffectRenderBatch batch;
  CHECK(ActionEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == kActionEffectRenderMaxVertices);
  CHECK(batch.index_count == kActionEffectRenderMaxIndices);

  /* And the opposite extreme: parts that all overlap must collapse to ONE
   * flame, which is the whole point of clustering. */
  for (unsigned i = 0; i < kActionEffectMaxInstances; i++)
    frame.effects[i].world_x = Fire().world_x + (int)i;
  ActionEffectRenderBatch clustered;
  CHECK(ActionEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &clustered));
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
static ActionEffectInstance Star(int world_x, uint8_t phase, int16_t velocity_x,
                                 int16_t velocity_y) {
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
      .geometry =
          {
              .kind = kActionEffectGeometry_Rect,
              .data.rect = {-16.0f, -16.0f, 16.0f, 16.0f},
          },
  };
  return effect;
}

static void TestMixedStagesAreOrderIndependent(void) {
  /* Far apart so they stay two clusters rather than merging into one. */
  ActionEffectInstance flying = Star(100, kActionEffectPhase_StardustLaunch, -8, 8);
  ActionEffectInstance bursting = Star(4000, kActionEffectPhase_StardustBurst, 0, 0);

  ActionEffectFrame flight_first = {.effect_count = 2};
  flight_first.effects[0] = flying;
  flight_first.effects[1] = bursting;

  ActionEffectFrame burst_first = {.effect_count = 2};
  burst_first.effects[0] = bursting;
  burst_first.effects[1] = flying;

  ActionEffectRenderBatch a, b;
  CHECK(ActionEffectRender_Build(&flight_first, true, true, IdentityProjection, NULL, &a));
  CHECK(ActionEffectRender_Build(&burst_first, true, true, IdentityProjection, NULL, &b));
  /* Same parts in either order must produce the same amount of geometry: one
   * spill plus two bodies, and one whole-burst ember budget. Under the old
   * anchor-driven styling the ember count alone differed by ~15% between
   * these two frames. */
  CHECK(a.vertex_count == b.vertex_count);
  CHECK(a.index_count == b.index_count);
  CHECK(a.vertex_count == 3 * kActionEffectGlowVertices + kActionEffectMaxEmbers * 4);
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
      .projection_plane = (kind == kActionEffect_WallTorch || kind == kActionEffect_AitosLavaPit)
                              ? kActionEffectProjectionPlane_Bg1
                              : kActionEffectProjectionPlane_Obj,
      .geometry =
          {
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
    effect.geometry.data.rect = (ActionEffectLocalRect){-64.0f, -24.0f, 64.0f, 24.0f};
    break;
  case kActionEffect_AitosLavaReservoir:
    effect.phase = kActionEffectPhase_AitosLavaReservoir;
    effect.projection_plane = kActionEffectProjectionPlane_Bg1High;
    effect.render_layer = kActionEffectRenderLayer_Bg1HighPlane;
    effect.geometry.data.rect = (ActionEffectLocalRect){-144.0f, -4.0f, 144.0f, 4.0f};
    break;
  case kActionEffect_AitosLavaFireball:
    effect.velocity_y = -4;
    effect.phase = kActionEffectPhase_AitosLavaFireballFlight;
    break;
  case kActionEffect_AitosStatueFire:
    effect.visual = 0x1E;
    effect.phase = kActionEffectPhase_AitosStatueFireBreath;
    effect.geometry.data.rect = (ActionEffectLocalRect){-16.0f, -8.0f, 48.0f, 8.0f};
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
    effect.geometry.data.rect = (ActionEffectLocalRect){-24.0f, -16.0f, 24.0f, 16.0f};
    break;
  case kActionEffect_AitosWaterfall:
    effect.phase = kActionEffectPhase_AitosWaterfallFlow;
    effect.projection_plane = kActionEffectProjectionPlane_Bg2;
    effect.geometry.data.rect = (ActionEffectLocalRect){-256.0f, -176.0f, 256.0f, 312.0f};
    break;
  case kActionEffect_AitosWaterfallMist:
    effect.phase = kActionEffectPhase_AitosWaterfallMist;
    effect.render_layer = kActionEffectRenderLayer_Atmosphere;
    effect.projection_plane = kActionEffectProjectionPlane_Bg2;
    effect.geometry.data.rect = (ActionEffectLocalRect){-256.0f, -32.0f, 256.0f, 24.0f};
    break;
  case kActionEffect_LightningTrap:
    effect.visual = 0x1F;
    effect.geometry.data.rect = (ActionEffectLocalRect){0.0f, -88.0f, 8.0f, 88.0f};
    break;
  case kActionEffect_MarahnaLightningLink:
    effect.visual = 0x2E;
    effect.animation_state = 0x27;
    effect.phase = kActionEffectPhase_MarahnaLightningActive;
    effect.geometry.data.rect = (ActionEffectLocalRect){-40.0f, -4.0f, 40.0f, 4.0f};
    break;
  case kActionEffect_MarahnaBossLightning:
    effect.velocity_x = -4;
    effect.velocity_y = 4;
    effect.visual = 0x11;
    effect.phase = kActionEffectPhase_MarahnaBossLightningBolt;
    effect.geometry.data.rect = (ActionEffectLocalRect){-32.0f, 0.0f, 0.0f, 32.0f};
    break;
  case kActionEffect_BloodpoolBossLightning:
    effect.visual = 0x05;
    effect.phase = kActionEffectPhase_BossLightningStrike;
    effect.geometry.data.rect = (ActionEffectLocalRect){-30.0f, -83.0f, 8.0f, 21.0f};
    break;
  case kActionEffect_CentaurLightning:
    effect.visual = 0x20;
    effect.phase = kActionEffectPhase_BossLightningStrike;
    effect.geometry.data.rect = (ActionEffectLocalRect){-64, -1, 8, 111};
    break;
  case kActionEffect_NorthwallBossMagic:
    effect.visual = 0x0A;
    effect.phase = kActionEffectPhase_NorthwallMagicCharge;
    break;
  case kActionEffect_SwordBeam:
    effect.velocity_x = 8;
    effect.visual = 0x30;
    effect.phase = kActionEffectPhase_SwordBeamFlight;
    effect.geometry.data.rect = (ActionEffectLocalRect){32.0f, -33.0f, 48.0f, -1.0f};
    break;
  case kActionEffect_MinotaurAxe:
    effect.velocity_x = -4;
    effect.velocity_y = 2;
    effect.visual = 0x00;
    effect.phase = kActionEffectPhase_MinotaurAxeFlight;
    break;
  case kActionEffect_FlamingWheel:
    effect.visual = 0x0F;
    effect.composition = 0x5276;
    effect.phase = kActionEffectPhase_FlamingWheelBody;
    effect.geometry.data.rect = (ActionEffectLocalRect){-32.0f, -32.0f, 32.0f, 32.0f};
    break;
  case kActionEffect_FlamingWheelProjectile:
    effect.velocity_x = 1;
    effect.visual = 0;
    effect.composition = 0x51B5;
    effect.phase = kActionEffectPhase_FlamingWheelProjectileFlight;
    break;
  case kActionEffect_IceDragonIceBall:
    effect.velocity_x = -4;
    effect.velocity_y = -2;
    effect.visual = 0x12;
    effect.phase = kActionEffectPhase_IceDragonIceBallFlight;
    break;
  case kActionEffect_TanzaraProjectile:
    effect.velocity_x = 3;
    effect.velocity_y = 2;
    effect.visual = 0x16;
    effect.phase = kActionEffectPhase_TanzaraProjectileFlight;
    break;
  default:
    break;
  }
  return effect;
}

static void TestSceneFeatureSwitchesAndDeterminism(void) {
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  frame.effects[0] = SceneEffect(kActionEffect_WallTorch, 100);
  static ActionSceneEffectRenderBatch lighting, particles, both, repeat;
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &lighting));
  CHECK(lighting.vertex_count == 2 * kActionEffectGlowVertices);
  CHECK(lighting.index_count == 2 * kActionEffectGlowIndices);
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == 7 * 4);
  CHECK(particles.index_count == 7 * 6);
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &both));
  CHECK(both.vertex_count == lighting.vertex_count + particles.vertex_count);
  CHECK(both.index_count == lighting.index_count + particles.index_count);
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(SceneBatchesEqual(&both, &repeat));

  frame.effects[0].pulse_ticks++;
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &repeat));
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
  static ActionSceneEffectRenderBatch batch;
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 12 * kActionEffectGlowVertices + (7 + 12 * 5) * 4);
  CHECK(batch.index_count == 12 * kActionEffectGlowIndices + (7 + 12 * 5) * 6);

  frame.effects[1].kind = 99;
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 10 * kActionEffectGlowVertices + (7 + 12 * 4) * 4);
  frame.effects[0].projection_plane = 99;
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 8 * kActionEffectGlowVertices + 48 * 4);
}

static void TestBossRushEffectStyles(void) {
  ActionSceneEffectFrame frame = {.effect_count = 4, .visible_count = 4};
  frame.effects[0] = SceneEffect(kActionEffect_MinotaurAxe, 180);
  frame.effects[1] = SceneEffect(kActionEffect_FlamingWheel, 260);
  frame.effects[2] = SceneEffect(kActionEffect_IceDragonIceBall, 340);
  frame.effects[3] = SceneEffect(kActionEffect_TanzaraProjectile, 420);
  static ActionSceneEffectRenderBatch lighting, particles, both, repeat;
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &lighting));
  CHECK(lighting.vertex_count == 20 * kActionEffectGlowVertices);
  CHECK(lighting.index_count == 20 * kActionEffectGlowIndices);
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == 4 * 12 * 4);
  CHECK(particles.index_count == 4 * 12 * 6);
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &both));
  CHECK(both.vertex_count == lighting.vertex_count + particles.vertex_count);
  CHECK(both.index_count == lighting.index_count + particles.index_count);
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(SceneBatchesEqual(&both, &repeat));

  frame.effects[2].visual = 0x11;
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertex_count == both.vertex_count - 2 * kActionEffectGlowVertices - 12 * 4);
}

static void TestAitosLavaLightingAndParticles(void) {
  ActionSceneEffectFrame frame = {.effect_count = 2, .visible_count = 2};
  frame.effects[0] = SceneEffect(kActionEffect_AitosLavaPit, 400);
  frame.effects[1] = SceneEffect(kActionEffect_AitosLavaFireball, 520);
  static ActionSceneEffectRenderBatch lighting, particles, repeat;
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &lighting));
  CHECK(lighting.vertex_count == 4 * kActionEffectGlowVertices);
  CHECK(lighting.index_count == 4 * kActionEffectGlowIndices);
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == 24 * 4);
  CHECK(particles.index_count == 24 * 6);

  /* The pit's twelve births span its authored 128px surface instead of
   * collapsing into a torch-like centre plume. */
  float pit_min_x = 10000.0f, pit_max_x = -10000.0f;
  for (int i = 0; i < 12 * 4; i++) {
    if (particles.vertices[i].position.x < pit_min_x) pit_min_x = particles.vertices[i].position.x;
    if (particles.vertices[i].position.x > pit_max_x) pit_max_x = particles.vertices[i].position.x;
  }
  CHECK(pit_max_x - pit_min_x > 70.0f);

  /* The glow still covers the complete two-row bubbly volume, but the
   * isometric source plane sits one quarter-height above its geometric
   * midpoint. A quad's centroid is the projected particle position,
   * independent of its width and reach. Sweep enough ticks to cover every
   * 21-36 tick lifetime and pin the narrow +/-1.5px source band. */
  const float pit_source_y =
      frame.effects[0].world_y +
      (frame.effects[0].geometry.data.rect.y0 + frame.effects[0].geometry.data.rect.y1) * 0.5f -
      (frame.effects[0].geometry.data.rect.y1 - frame.effects[0].geometry.data.rect.y0) * 0.25f;
  float max_centroid_y[12];
  for (int particle = 0; particle < 12; particle++)
    max_centroid_y[particle] = -10000.0f;
  for (unsigned ticks = 0; ticks < 72; ticks++) {
    frame.effects[0].pulse_ticks = ticks;
    CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
    for (int particle = 0; particle < 12; particle++) {
      float centre_y = 0.0f;
      for (int vertex = 0; vertex < 4; vertex++)
        centre_y += particles.vertices[particle * 4 + vertex].position.y;
      centre_y *= 0.25f;
      CHECK(centre_y <= pit_source_y + 1.51f);
      if (centre_y > max_centroid_y[particle]) max_centroid_y[particle] = centre_y;
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

  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &repeat));
  frame.effects[0].pulse_ticks++;
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &lighting));
  CHECK(!SceneBatchesEqual(&repeat, &lighting));
}

static void TestAitosStatueFireLightingAndFacing(void) {
  ActionSceneEffectFrame frame = {.effect_count = 3, .visible_count = 3};
  frame.effects[0] = SceneEffect(kActionEffect_AitosStatueFire, 400);
  frame.effects[0].visual = 0x1F;
  frame.effects[1] = frame.effects[0];
  frame.effects[1].visual = 0x1E;
  frame.effects[1].record_address += kActRaiserActionObjectStride;
  frame.effects[1].flags |= kActionEffectFlag_FlipHorizontal;
  frame.effects[1].geometry.data.rect = (ActionEffectLocalRect){-48.0f, -8.0f, 16.0f, 8.0f};
  /* Even a malformed direct render frame cannot decorate the idle hold. */
  frame.effects[2] = frame.effects[0];
  frame.effects[2].record_address += 2 * kActRaiserActionObjectStride;
  frame.effects[2].visual = 0x17;
  static ActionSceneEffectRenderBatch lighting, particles;
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &lighting));
  CHECK(lighting.vertex_count == 4 * kActionEffectGlowVertices);
  CHECK(lighting.index_count == 4 * kActionEffectGlowIndices);
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == 24 * 4);
  CHECK(particles.index_count == 24 * 6);

  float right_mean = 0.0f, left_mean = 0.0f;
  for (int particle = 0; particle < 12; particle++) {
    for (int vertex = 0; vertex < 4; vertex++) {
      right_mean += particles.vertices[particle * 4 + vertex].position.x;
      left_mean += particles.vertices[(12 + particle) * 4 + vertex].position.x;
    }
  }
  right_mean /= 48.0f;
  left_mean /= 48.0f;
  CHECK(right_mean > frame.effects[0].world_x + 8.0f);
  CHECK(left_mean < frame.effects[1].world_x - 8.0f);
}

static void TestAitosSideLavaLightingAndHeatMesh(void) {
  ActionSceneEffectFrame frame = {
      .decoration_count = 1,
      .decoration_visible_count = 1,
  };
  frame.decorations[0] = SceneEffect(kActionEffect_AitosLavaReservoir, 400);
  static ActionSceneEffectRenderBatch lighting, particles;
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg1HighPlane, true,
                                          false, IdentityProjection, NULL, NULL, &lighting));
  CHECK(lighting.vertex_count == 6 * kActionEffectGlowVertices);
  CHECK(lighting.index_count == 6 * kActionEffectGlowIndices);
  /* A 288px lip becomes three overlapping 96px sections. The first vertex
   * of each spill/body glow is its centre, so all three local anchors remain
   * visible instead of fading toward one reservoir-wide outer ring. */
  CHECK(fabsf(lighting.vertices[0].position.x - 304.0f) < 0.001f);
  CHECK(fabsf(lighting.vertices[2 * kActionEffectGlowVertices].position.x - 400.0f) < 0.001f);
  CHECK(fabsf(lighting.vertices[4 * kActionEffectGlowVertices].position.x - 496.0f) < 0.001f);
  frame.decorations[0].geometry.data.rect = (ActionEffectLocalRect){-320.0f, -4.0f, 320.0f, 4.0f};
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg1HighPlane, true,
                                          false, IdentityProjection, NULL, NULL, &lighting));
  CHECK(lighting.vertex_count == 14 * kActionEffectGlowVertices);
  CHECK(lighting.index_count == 14 * kActionEffectGlowIndices);
  frame.decorations[0].geometry.data.rect = (ActionEffectLocalRect){-144.0f, -4.0f, 144.0f, 4.0f};
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg1HighPlane, false,
                                          true, IdentityProjection, NULL, NULL, &particles));
  CHECK(particles.vertex_count == kActionSceneEffectLavaReservoirParticleCount * 4);
  CHECK(particles.index_count == kActionSceneEffectLavaReservoirParticleCount * 6);
  float min_x = 10000.0f, max_x = -10000.0f;
  for (int i = 0; i < particles.vertex_count; i++) {
    if (particles.vertices[i].position.x < min_x) min_x = particles.vertices[i].position.x;
    if (particles.vertices[i].position.x > max_x) max_x = particles.vertices[i].position.x;
  }
  CHECK(max_x - min_x > 180.0f);

  frame.decorations[0].geometry.data.rect = (ActionEffectLocalRect){-624.0f, -4.0f, 624.0f, 4.0f};
  CHECK(!ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg1HighPlane, true,
                                           false, IdentityProjection, NULL, NULL, &lighting));
  CHECK(lighting.vertex_count == 0 && lighting.index_count == 0);
  frame.decorations[0].geometry.data.rect = (ActionEffectLocalRect){-1.0e30f, -4.0f, 1.0e30f, 4.0f};
  CHECK(!ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg1HighPlane, true,
                                           false, IdentityProjection, NULL, NULL, &lighting));
  CHECK(lighting.vertex_count == 0 && lighting.index_count == 0);

  const ArRenderRectI viewport = {120, 40, 960, 840};
  ActionHeatRenderMesh heat, repeat, advanced;
  CHECK(ActionHeatRender_Build(5009, viewport, 960, 840, 256, &heat));
  CHECK(heat.vertex_count == kActionHeatMeshVertices);
  CHECK(heat.index_count == kActionHeatMeshIndices);
  CHECK(ActionHeatRender_Build(5009, viewport, 960, 840, 256, &repeat));
  CHECK(heat.vertex_count == repeat.vertex_count);
  CHECK(heat.index_count == repeat.index_count);
  CHECK(memcmp(heat.vertices, repeat.vertices, sizeof(heat.vertices)) == 0);
  CHECK(memcmp(heat.indices, repeat.indices, sizeof(heat.indices)) == 0);
  CHECK(ActionHeatRender_Build(5010, viewport, 960, 840, 256, &advanced));
  CHECK(memcmp(heat.vertices, advanced.vertices, sizeof(heat.vertices)) != 0);
  /* Every border vertex samples its exact position; refraction cannot pull
   * black bars or undefined target pixels into the game image. */
  for (int row = 0; row <= kActionHeatMeshRows; row++) {
    for (int column = 0; column <= kActionHeatMeshColumns; column++) {
      if (row != 0 && row != kActionHeatMeshRows && column != 0 && column != kActionHeatMeshColumns)
        continue;
      const int index = row * (kActionHeatMeshColumns + 1) + column;
      CHECK(fabsf(heat.vertices[index].tex_coord.x -
                  (heat.vertices[index].position.x - viewport.x) / (float)viewport.w) < 0.00001f);
      CHECK(fabsf(heat.vertices[index].tex_coord.y -
                  (heat.vertices[index].position.y - viewport.y) / (float)viewport.h) < 0.00001f);
    }
  }
  CHECK(!ActionHeatRender_Build(1, (ArRenderRectI){0, 0, 0, 100}, 100, 100, 256, &heat));
  CHECK(heat.vertex_count == 0 && heat.index_count == 0);

  /* A high-resolution host viewport still gets a perceptible displacement.
   * Capping this at the former 3.25 output pixels made the room haze look
   * intermittent because only especially sharp edges exposed it. */
  CHECK(ActionHeatRender_Build(5009, (ArRenderRectI){10, 10, 3400, 2100}, 3400, 2100, 432, &heat));
  float max_heat_offset = 0.0f;
  for (int i = 0; i < heat.vertex_count; i++) {
    const float source_x = heat.vertices[i].tex_coord.x * 3400.0f;
    const float output_local_x = heat.vertices[i].position.x - 10.0f;
    const float offset = fabsf(source_x - output_local_x);
    if (offset > max_heat_offset) max_heat_offset = offset;
  }
  CHECK(max_heat_offset > 5.0f);
}

static void TestFlamingWheelRimAndProjectile(void) {
  static const float kAnchors[12][2] = {
      {-24, -24}, {-8, -24}, {8, -24},  {24, -24}, {-24, -8}, {24, -8},
      {-24, 8},   {24, 8},   {-24, 24}, {-8, 24},  {8, 24},   {24, 24},
  };
  ActionSceneEffectFrame frame = {.effect_count = 2, .visible_count = 2};
  frame.effects[0] = SceneEffect(kActionEffect_FlamingWheel, 300);
  frame.effects[1] = SceneEffect(kActionEffect_FlamingWheelProjectile, 500);
  static ActionSceneEffectRenderBatch lighting, particles;
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &lighting));
  CHECK(lighting.vertex_count == 16 * kActionEffectGlowVertices);
  CHECK(lighting.index_count == 16 * kActionEffectGlowIndices);
  for (int i = 0; i < 12; i++) {
    const int centre = (2 + i) * kActionEffectGlowVertices;
    CHECK(fabsf(lighting.vertices[centre].position.x -
                (frame.effects[0].world_x + kAnchors[i][0])) < 0.001f);
    CHECK(fabsf(lighting.vertices[centre].position.y -
                (frame.effects[0].world_y + kAnchors[i][1])) < 0.001f);
  }
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == 24 * 4);
  CHECK(particles.index_count == 24 * 6);
  float projectile_mean_x = 0.0f;
  for (int i = 12 * 4; i < particles.vertex_count; i++)
    projectile_mean_x += particles.vertices[i].position.x;
  projectile_mean_x /= 12.0f * 4.0f;
  CHECK(projectile_mean_x < frame.effects[1].world_x - 5.0f);

  /* Incomplete transition artwork retains its broad body light, but cannot
   * manufacture twelve detached emitters around a rim that is not present. */
  frame.effect_count = frame.visible_count = 1;
  frame.effects[0].composition = 0x5000;
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &lighting));
  CHECK(lighting.vertex_count == 2 * kActionEffectGlowVertices);
  CHECK(lighting.index_count == 2 * kActionEffectGlowIndices);
}

static void TestMarahnaFireballFramesAndDirections(void) {
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  static ActionSceneEffectRenderBatch lighting, particles;
  ActionEffectInstance *effect = &frame.effects[0];
  *effect = SceneEffect(kActionEffect_MarahnaFireball, 400);

  static const struct {
    uint16_t visual;
    int16_t velocity_x, velocity_y;
    uint8_t phase;
    int expected_x_sign, expected_y_sign;
  } kCases[] = {
      {0x05, 0, 0, kActionEffectPhase_MarahnaFireballOrb, 0, -1},
      {0x06, 2, 0, kActionEffectPhase_MarahnaFireballOrb, -1, 0},
      {0x07, 0, 0, kActionEffectPhase_MarahnaFireballOrb, 0, -1},
      {0x08, -2, 0, kActionEffectPhase_MarahnaFireballOrb, 1, 0},
      {0x32, 0, 3, kActionEffectPhase_MarahnaFireballSplit, 0, -1},
      {0x33, -3, 0, kActionEffectPhase_MarahnaFireballSplit, 1, 0},
      {0x32, 0, -3, kActionEffectPhase_MarahnaFireballSplit, 0, 1},
      {0x33, 3, 0, kActionEffectPhase_MarahnaFireballSplit, -1, 0},
      {0x1D, -4, 0, kActionEffectPhase_MarahnaSnakeFireballShot, 1, 0},
      {0x1E, 4, 0, kActionEffectPhase_MarahnaSnakeFireballShot, -1, 0},
  };
  for (size_t c = 0; c < sizeof(kCases) / sizeof(kCases[0]); c++) {
    effect->visual = kCases[c].visual;
    effect->velocity_x = kCases[c].velocity_x;
    effect->velocity_y = kCases[c].velocity_y;
    effect->phase = kCases[c].phase;
    CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &lighting));
    CHECK(lighting.vertex_count == 2 * kActionEffectGlowVertices);
    CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
    CHECK(particles.vertex_count == kActionSceneEffectParticlesPerInstance * 4);
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
  static ActionSceneEffectRenderBatch pit, fireball;
  ArRenderPointF expected;

  /* camera-relative (-30,-7), plus display margins (60,32), produces the
   * captured anchor (30,25). This exercises the production adapter rather
   * than a test-local approximation of it. */
  frame.effects[0] = SceneEffect(kActionEffect_AitosLavaPit, 970);
  frame.effects[0].world_y = 493;
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, ActionEffectProjection_ProjectPoint,
                                      &context, &pit));
  /* Vertex zero is the outer glow's centre. The pit spill follows the full
   * two-row bubbly geometry and must use BG1's rake/bow plus the apron. */
  CHECK(Diorama_ProjectCapturedBg1Point(&projection, 30.0f, 17.8f, &expected, NULL, NULL));
  CHECK(fabsf(pit.vertices[0].position.x - expected.x) < 0.001f);
  CHECK(fabsf(pit.vertices[0].position.y - expected.y) < 0.001f);

  frame.effects[0] = SceneEffect(kActionEffect_AitosLavaFireball, 970);
  frame.effects[0].world_y = 493;
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, ActionEffectProjection_ProjectPoint,
                                      &context, &fireball));
  CHECK(Diorama_ProjectCapturedPoint(&projection, 30.0f, 25.0f, 0, &expected, NULL, NULL));
  CHECK(fabsf(fireball.vertices[0].position.x - expected.x) < 0.001f);
  CHECK(fabsf(fireball.vertices[0].position.y - expected.y) < 0.001f);

  /* The same capture point does not collapse onto one flat plane: BG1 lava
   * stays attached to the pit while its projectile occupies OBJ priority 0. */
  CHECK(fabsf(pit.vertices[0].position.x - fireball.vertices[0].position.x) > 5.0f);

  /* A BG2 waterfall uses its own camera and independently resolved backdrop
   * shape/window, rather than borrowing either BG1 or OBJ registration. */
  frame.effects[0] = SceneEffect(kActionEffect_AitosWaterfall, 970);
  frame.effects[0].world_y = 493;
  context.bg2_camera_x = 1010;
  context.bg2_camera_y = 510;
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, ActionEffectProjection_ProjectPoint,
                                      &context, &pit));
  CHECK(Diorama_ProjectCapturedBg2Point(&projection, 20.0f, 83.0f, &expected, NULL, NULL));
  CHECK(fabsf(pit.vertices[0].position.x - expected.x) < 0.001f);
  CHECK(fabsf(pit.vertices[0].position.y - expected.y) < 0.001f);

  /* The bottom atmosphere deliberately draws after the world, but it still
   * uses the production BG2 camera/rake/bow projection so its foam seam meets
   * the finite waterfall plane in Diorama mode. */
  ActionSceneEffectFrame decorations = {
      .decoration_count = 1,
      .decoration_visible_count = 1,
  };
  decorations.decorations[0] = SceneEffect(kActionEffect_AitosWaterfallMist, 970);
  decorations.decorations[0].world_y = 493;
  CHECK(ActionSceneDecorationRender_Build(&decorations, kActionEffectRenderLayer_Atmosphere, true,
                                          false, ActionEffectProjection_ProjectPoint,
                                          ActionEffectProjection_ClipBounds, &context,
                                          &pit));
  /* The first cloud anchor is deliberately stable; the remaining puffs drift
   * independently. screen X is -40, plus 60 capture margin, plus the first
   * of six lanes across local [-256,256]. screen Y is -17, plus local 48 and
   * the 32-row texture margin. */
  const float first_cloud_x = -256.0f + 512.0f * (0.5f / 6.0f);
  CHECK(Diorama_ProjectCapturedBg2Point(&projection, 20.0f + first_cloud_x, 63.0f, &expected, NULL,
                                        NULL));
  CHECK(fabsf(pit.vertices[0].position.x - expected.x) < 0.001f);
  CHECK(fabsf(pit.vertices[0].position.y - expected.y) < 0.001f);

  /* Finite plane transforms must not extrapolate attached effects into the
   * surrounding Diorama void. A broad reservoir entirely left of BG1-high's
   * published source window produces no GPU geometry; moving it into that
   * window restores the same authored effect. */
  decorations.decorations[0] = SceneEffect(kActionEffect_AitosLavaReservoir, 400);
  decorations.decorations[0].world_y = 500;
  CHECK(ActionSceneDecorationRender_Build(&decorations, kActionEffectRenderLayer_Bg1HighPlane, true,
                                          true, ActionEffectProjection_ProjectPoint,
                                          ActionEffectProjection_ClipBounds, &context,
                                          &pit));
  CHECK(pit.vertex_count == 0);
  CHECK(pit.index_count == 0);
  CHECK(!ActionEffectProjection_IntersectsFlatViewport(&context, &decorations.decorations[0]));

  decorations.decorations[0].world_x = 970;
  CHECK(ActionSceneDecorationRender_Build(&decorations, kActionEffectRenderLayer_Bg1HighPlane, true,
                                          true, ActionEffectProjection_ProjectPoint,
                                          ActionEffectProjection_ClipBounds, &context,
                                          &pit));
  CHECK(pit.vertex_count > 0);
  CHECK(pit.index_count > 0);
  CHECK(ActionEffectProjection_IntersectsFlatViewport(&context, &decorations.decorations[0]));

  /* The same production helper owns flat viewport placement. Vertical
   * extension is a Diorama texture concern and intentionally drops out here. */
  frame.effects[0] = SceneEffect(kActionEffect_AitosLavaPit, 970);
  frame.effects[0].world_y = 493;
  context.diorama_projection = NULL;
  context.visible_x0 = 10;
  context.visible_width = 400;
  ArRenderPointF flat_expected;
  CHECK(
      ActionEffectProjection_ProjectPoint(&context, &frame.effects[0], 0.0f, 0.0f, &flat_expected));
  CHECK(fabsf(flat_expected.x - 48.6f) < 0.001f);
  CHECK(fabsf(flat_expected.y - (-1.0f)) < 0.001f);
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
  scene.decoration_count = 3;
  scene.decorations[0] = SceneEffect(kActionEffect_AitosLavaPit, 300);
  scene.decorations[1] = SceneEffect(kActionEffect_AitosWaterfall, 320);
  scene.decorations[1].projection_plane = kActionEffectProjectionPlane_Bg2;
  scene.decorations[2] = SceneEffect(kActionEffect_AitosLavaReservoir, 340);
  CHECK(ActionEffectProjection_RequiredObjPriorityMask(&spells, &scene) ==
        ((1u << 0) | (1u << 2) | (1u << 3)));
  CHECK(ActionEffectProjection_RequiredObjPriorityMask(NULL, &scene) ==
        ((1u << 0) | (1u << 2) | (1u << 3)));
  CHECK(ActionEffectProjection_RequiredBgPlaneMask(&spells, &scene) ==
        ((1u << SR_PPU_OVERLAY_BG1) | (1u << SR_PPU_OVERLAY_BG2) | (1u << kDioramaPlane_Bg1Hi)));

  /* A malformed actor list fails closed as a unit, without suppressing the
   * independently valid spell request. BG-local decorations never acquire an
   * OBJ projection merely because their priority byte happens to be set. */
  scene.overflow = 1;
  CHECK(ActionEffectProjection_RequiredObjPriorityMask(&spells, &scene) == (1u << 2));
  CHECK(ActionEffectProjection_RequiredBgPlaneMask(NULL, &scene) ==
        ((1u << SR_PPU_OVERLAY_BG1) | (1u << SR_PPU_OVERLAY_BG2) | (1u << kDioramaPlane_Bg1Hi)));
  scene.decoration_overflow = 1;
  CHECK(ActionEffectProjection_RequiredBgPlaneMask(NULL, &scene) == 0);
  scene.decoration_overflow = 0;
  scene.overflow = 0;
  scene.effect_count = kActionSceneEffectMaxInstances + 1;
  CHECK(ActionEffectProjection_RequiredObjPriorityMask(NULL, &scene) == 0);
  CHECK(ActionEffectProjection_RequiredBgPlaneMask(NULL, &scene) ==
        ((1u << SR_PPU_OVERLAY_BG1) | (1u << SR_PPU_OVERLAY_BG2) | (1u << kDioramaPlane_Bg1Hi)));
}

static void TestDecorationLayerBuildsAreIndependent(void) {
  ActionSceneEffectFrame frame = {
      .decoration_count = 4,
      .decoration_visible_count = 4,
  };
  frame.decorations[0] = SceneEffect(kActionEffect_AitosWaterSplash, 100);
  frame.decorations[1] = SceneEffect(kActionEffect_AitosWaterfall, 120);
  frame.decorations[1].render_layer = kActionEffectRenderLayer_Bg2Plane;
  frame.decorations[2] = SceneEffect(kActionEffect_AitosWaterfallMist, 120);
  frame.decorations[2].world_y =
      kActRaiserAuthenticHeight + kActionBgAitosWaterfallBottomExtensionPixels;
  frame.decorations[3] = SceneEffect(kActionEffect_WallTorch, 140);
  frame.decorations[3].render_layer = kActionEffectRenderLayer_Bg1Plane;
  frame.decorations[3].projection_plane = kActionEffectProjectionPlane_Bg1;
  static ActionSceneEffectRenderBatch world, bg1, bg2, atmosphere;
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_WorldOverlay, true, true,
                                          IdentityProjection, NULL, NULL, &world));
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg1Plane, true, true,
                                          IdentityProjection, NULL, NULL, &bg1));
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg2Plane, true, true,
                                          IdentityProjection, NULL, NULL, &bg2));
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Atmosphere, true, true,
                                          IdentityProjection, NULL, NULL, &atmosphere));
  CHECK(world.index_count > 0);
  CHECK(bg1.index_count > 0);
  CHECK(bg2.index_count > 0);
  CHECK(atmosphere.index_count > 0);
  CHECK(bg1.index_count != world.index_count);
  CHECK(world.index_count != bg2.index_count);
  CHECK(atmosphere.index_count != bg2.index_count);
  CHECK(atmosphere.vertex_count ==
        kActionSceneEffectWaterfallMistCloudCount * kActionSceneEffectWaterfallMistCloudVertices +
            kActionSceneEffectWaterfallMistParticleCount * 4);
  CHECK(atmosphere.index_count ==
        kActionSceneEffectWaterfallMistCloudCount * kActionSceneEffectWaterfallMistCloudIndices +
            kActionSceneEffectWaterfallMistParticleCount * 6);

  /* The source veil remains substantial renderer geometry rather than a
   * record that gets silently filtered, while the separate atmosphere spans
   * the bottom seam and extends below the authentic 224px frame. */
  CHECK(bg2.vertex_count ==
        2 * kActionEffectGlowVertices + kActionSceneEffectWaterfallParticleCount * 4);
  float visible_veil_max_y = -10000.0f;
  for (int i = 0; i < bg2.vertex_count; i++) {
    if (bg2.vertices[i].color.a > 0.01f && bg2.vertices[i].position.y > visible_veil_max_y)
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
        kActRaiserAuthenticHeight + kActionBgAitosWaterfallBottomExtensionPixels + 20.0f);
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
  for (int cloud = 0; cloud < kActionSceneEffectWaterfallMistCloudCount; cloud++) {
    const int cloud_base = cloud * kActionSceneEffectWaterfallMistCloudVertices;
    const ArRenderVertex2D *centre = &atmosphere.vertices[cloud_base];
    if (centre->position.x < cloud_centre_min_x) cloud_centre_min_x = centre->position.x;
    if (centre->position.x > cloud_centre_max_x) cloud_centre_max_x = centre->position.x;
    if (centre->position.y < cloud_centre_min_y) cloud_centre_min_y = centre->position.y;
    if (centre->position.y > cloud_centre_max_y) cloud_centre_max_y = centre->position.y;
    CHECK(centre->color.a > 0.05f);
    const int visible_ring = cloud_base + 1 + kActionSceneEffectWaterfallMistCloudSegments;
    const int transparent_ring = visible_ring + kActionSceneEffectWaterfallMistCloudSegments;
    float cloud_bottom = -10000.0f;
    for (int segment = 0; segment < kActionSceneEffectWaterfallMistCloudSegments; segment++) {
      const ArRenderVertex2D *vertex = &atmosphere.vertices[visible_ring + segment];
      CHECK(vertex->color.a > 0.02f);
      CHECK(atmosphere.vertices[transparent_ring + segment].color.a == 0.0f);
      if (vertex->position.y > cloud_bottom) cloud_bottom = vertex->position.y;
    }
    if (cloud_bottom < shallowest_cloud_bottom) shallowest_cloud_bottom = cloud_bottom;
    if (cloud_bottom > deepest_cloud_bottom) deepest_cloud_bottom = cloud_bottom;
  }
  CHECK(cloud_centre_max_x - cloud_centre_min_x > 400.0f);
  CHECK(cloud_centre_max_y - cloud_centre_min_y > 55.0f);
  CHECK(deepest_cloud_bottom - shallowest_cloud_bottom > 75.0f);
  CHECK(deepest_cloud_bottom >
        kActRaiserAuthenticHeight + kActionBgAitosWaterfallBottomExtensionPixels + 100.0f);
  frame.decoration_overflow = 1;
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_WorldOverlay, true, true,
                                          IdentityProjection, NULL, NULL, &world));
  CHECK(world.index_count == 0);
  CHECK(!ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Count, true, true,
                                           IdentityProjection, NULL, NULL, &world));
}

static void TestLightningVisibleLightCoversCapturedArc(void) {
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  frame.effects[0] = SceneEffect(kActionEffect_LightningTrap, 300);
  static ActionSceneEffectRenderBatch batch;
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &batch));

  float visible_min_y = 10000.0f;
  float visible_max_y = -10000.0f;
  for (int i = 0; i < batch.vertex_count; i++) {
    /* Ignore the fully transparent outer falloff. This checks what a viewer
     * can actually see, which is precisely how the half-arc bug escaped the
     * earlier vertex-count coverage. */
    if (batch.vertices[i].color.a < 0.10f) continue;
    if (batch.vertices[i].position.y < visible_min_y) visible_min_y = batch.vertices[i].position.y;
    if (batch.vertices[i].position.y > visible_max_y) visible_max_y = batch.vertices[i].position.y;
  }
  bool wide_spill = false;
  for (int i = 0; i < batch.vertex_count; i++)
    if (fabsf(batch.vertices[i].position.x-frame.effects[0].world_x-4) > 50 &&
        batch.vertices[i].color.a > .02f) wide_spill = true;
  CHECK(wide_spill);
  const float top = frame.effects[0].world_y - 88.0f;
  const float bottom = frame.effects[0].world_y + 88.0f;
  CHECK(visible_min_y <= top + 2.0f);
  CHECK(visible_max_y >= bottom - 2.0f);
}

static void TestBossLightningFilamentAndStages(void) {
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  frame.effects[0] = SceneEffect(kActionEffect_BloodpoolBossLightning, 300);
  static ActionSceneEffectRenderBatch lighting, particles, repeat;
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
      effect->flags = kActionEffectFlag_Visible | (flipped ? kActionEffectFlag_FlipHorizontal : 0);
      effect->geometry.data.rect = (ActionEffectLocalRect){
          -(float)(flipped ? kRight[visual] : kLeft[visual]),
          -83.0f,
          (float)(flipped ? kLeft[visual] : kRight[visual]),
          (float)kBottom[visual],
      };
      CHECK(
          ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &lighting));
      CHECK(lighting.vertex_count ==
            2 * kActionEffectGlowVertices + (int)kSegments[visual] * 4 * 2);
      CHECK(lighting.index_count == 2 * kActionEffectGlowIndices + (int)kSegments[visual] * 6 * 2);

      const int first = 2 * kActionEffectGlowVertices;
      const int last = first + ((int)kSegments[visual] - 1) * 4;
      ArRenderPointF first_centre = {0}, last_centre = {0};
      for (int i = 0; i < 4; i++) {
        first_centre.x += lighting.vertices[first + i].position.x * 0.25f;
        first_centre.y += lighting.vertices[first + i].position.y * 0.25f;
        last_centre.x += lighting.vertices[last + i].position.x * 0.25f;
        last_centre.y += lighting.vertices[last + i].position.y * 0.25f;
      }
      const float mirror = flipped ? -1.0f : 1.0f;
      CHECK(fabsf(first_centre.x - (effect->world_x + mirror * kFirstMidX[visual])) < 0.01f);
      CHECK(fabsf(first_centre.y - (effect->world_y - 76.0f)) < 0.01f);
      CHECK(fabsf(last_centre.x - (effect->world_x + mirror * kLastMidX[visual])) < 0.01f);
      CHECK(fabsf(last_centre.y - (effect->world_y + kLastMidY[visual])) < 0.01f);
    }
  }

  frame.effects[0] = SceneEffect(kActionEffect_BloodpoolBossLightning, 300);
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &lighting));
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == kActionSceneEffectParticlesPerInstance * 4);
  CHECK(particles.index_count == kActionSceneEffectParticlesPerInstance * 6);
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &repeat));
  CHECK(SceneBatchesEqual(&lighting, &repeat));

  /* Only the linked state-$09 child receives the floor bloom. The shared
   * visual-$20 blank cycle is rejected by capture and has no renderer phase. */
  frame.effects[0].phase = kActionEffectPhase_BossLightningImpact;
  frame.effects[0].visual = 10;
  frame.effects[0].geometry.data.rect = (ActionEffectLocalRect){-16.0f, -16.0f, 16.0f, 0.0f};
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertex_count == 2 * kActionEffectGlowVertices);
  frame.effects[0].phase = 99;
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertex_count == 0);
  CHECK(repeat.index_count == 0);
}

static void TestMarahnaLightningLinksAndOrientations(void) {
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  frame.effects[0] = SceneEffect(kActionEffect_MarahnaLightningLink, 300);
  static ActionSceneEffectRenderBatch horizontal, vertical, particles, repeat;
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &horizontal));
  CHECK(horizontal.vertex_count ==
        2 * kActionEffectGlowVertices + kActionSceneEffectMarahnaLightningSegments * 4 * 2);
  CHECK(horizontal.index_count ==
        2 * kActionEffectGlowIndices + kActionSceneEffectMarahnaLightningSegments * 6 * 2);
  const int ribbon = 2 * kActionEffectGlowVertices;
  ArRenderPointF first_centre = {0}, last_centre = {0};
  for (int i = 0; i < 4; i++) {
    first_centre.x += horizontal.vertices[ribbon + i].position.x * 0.25f;
    first_centre.y += horizontal.vertices[ribbon + i].position.y * 0.25f;
    const int last = ribbon + (kActionSceneEffectMarahnaLightningSegments - 1) * 4 + i;
    last_centre.x += horizontal.vertices[last].position.x * 0.25f;
    last_centre.y += horizontal.vertices[last].position.y * 0.25f;
  }
  CHECK(fabsf(first_centre.x - 264.0f) < 0.01f);
  CHECK(fabsf(last_centre.x - 336.0f) < 0.01f);

  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == kActionSceneEffectParticlesPerInstance * 4);
  CHECK(particles.index_count == kActionSceneEffectParticlesPerInstance * 6);
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &repeat));
  CHECK(SceneBatchesEqual(&horizontal, &repeat));
  frame.effects[0].phase_ticks++;
  frame.effects[0].pulse_ticks++;
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &repeat));
  CHECK(!SceneBatchesEqual(&horizontal, &repeat));

  frame.effects[0].visual = 0x31;
  frame.effects[0].animation_state = 0x28;
  frame.effects[0].geometry.data.rect = (ActionEffectLocalRect){-5.0f, -40.0f, 5.0f, 40.0f};
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &vertical));
  CHECK(vertical.vertex_count == horizontal.vertex_count);
  first_centre = (ArRenderPointF){0};
  last_centre = (ArRenderPointF){0};
  for (int i = 0; i < 4; i++) {
    first_centre.x += vertical.vertices[ribbon + i].position.x * 0.25f;
    first_centre.y += vertical.vertices[ribbon + i].position.y * 0.25f;
    const int last = ribbon + (kActionSceneEffectMarahnaLightningSegments - 1) * 4 + i;
    last_centre.x += vertical.vertices[last].position.x * 0.25f;
    last_centre.y += vertical.vertices[last].position.y * 0.25f;
  }
  CHECK(fabsf(first_centre.y - 84.0f) < 0.01f);
  CHECK(fabsf(last_centre.y - 156.0f) < 0.01f);

  frame.effects[0].animation_state = 0x27;
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertex_count == 0);
}

static void TestMarahnaBossLightningStagesAndOrientations(void) {
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  frame.effects[0] = SceneEffect(kActionEffect_MarahnaBossLightning, 300);
  static ActionSceneEffectRenderBatch left, right, charge, orb, ground_right, ground_left,
      particles;
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &left));
  CHECK(left.vertex_count ==
        2 * kActionEffectGlowVertices + kActionSceneEffectMarahnaBossLightningSegments * 4 * 2);
  CHECK(left.index_count ==
        2 * kActionEffectGlowIndices + kActionSceneEffectMarahnaBossLightningSegments * 6 * 2);

  const int ribbon = 2 * kActionEffectGlowVertices;
  ArRenderPointF first = {0}, last = {0};
  for (int i = 0; i < 4; i++) {
    first.x += left.vertices[ribbon + i].position.x * 0.25f;
    first.y += left.vertices[ribbon + i].position.y * 0.25f;
    const int end = ribbon + (kActionSceneEffectMarahnaBossLightningSegments - 1) * 4 + i;
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
  frame.effects[0].geometry.data.rect = (ActionEffectLocalRect){0.0f, 0.0f, 32.0f, 32.0f};
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &right));
  first = (ArRenderPointF){0};
  last = (ArRenderPointF){0};
  for (int i = 0; i < 4; i++) {
    first.x += right.vertices[ribbon + i].position.x * 0.25f;
    const int end = ribbon + (kActionSceneEffectMarahnaBossLightningSegments - 1) * 4 + i;
    last.x += right.vertices[end].position.x * 0.25f;
  }
  CHECK(fabsf(first.x - 302.0f) < 5.0f);
  CHECK(fabsf(last.x - 330.0f) < 5.0f);

  frame.effects[0].velocity_x = 0;
  frame.effects[0].velocity_y = 0;
  frame.effects[0].phase = kActionEffectPhase_MarahnaBossLightningCharge;
  frame.effects[0].visual = 7;
  frame.effects[0].geometry.data.rect = (ActionEffectLocalRect){-48.0f, -40.0f, 48.0f, 8.0f};
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &charge));
  CHECK(charge.vertex_count == 2 * kActionEffectGlowVertices);
  CHECK(fabsf(charge.vertices[0].position.y - 96.0f) < 0.01f);

  frame.effects[0].phase = kActionEffectPhase_MarahnaBossLightningOrb;
  frame.effects[0].visual = 10;
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &orb));
  CHECK(orb.vertex_count == charge.vertex_count);
  CHECK(!SceneBatchesEqual(&charge, &orb));
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == kActionSceneEffectParticlesPerInstance * 4);

  frame.effects[0].phase = kActionEffectPhase_MarahnaBossLightningGroundCharge;
  frame.effects[0].visual = 0x12;
  frame.effects[0].velocity_x = 4;
  frame.effects[0].velocity_y = 0;
  frame.effects[0].geometry.data.rect = (ActionEffectLocalRect){-8.0f, -8.0f, 8.0f, 8.0f};
  CHECK(
      ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &ground_right));
  CHECK(ground_right.vertex_count == 2 * kActionEffectGlowVertices);
  CHECK(fabsf(ground_right.vertices[0].position.x - 296.0f) < 0.01f);
  CHECK(fabsf(ground_right.vertices[0].position.y - 120.0f) < 0.01f);
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == kActionSceneEffectParticlesPerInstance * 4);
  float right_particle_mean_x = 0.0f;
  for (int i = 0; i < particles.vertex_count; i++)
    right_particle_mean_x += particles.vertices[i].position.x;
  right_particle_mean_x /= (float)particles.vertex_count;
  CHECK(right_particle_mean_x < 300.0f);

  frame.effects[0].velocity_x = -4;
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &ground_left));
  CHECK(fabsf(ground_left.vertices[0].position.x - 304.0f) < 0.01f);
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
  float left_particle_mean_x = 0.0f;
  for (int i = 0; i < particles.vertex_count; i++)
    left_particle_mean_x += particles.vertices[i].position.x;
  left_particle_mean_x /= (float)particles.vertex_count;
  CHECK(left_particle_mean_x > 300.0f);

  /* All three loaded frames belong to the one ground lifecycle; the next
   * visual must fail closed even if its phase is forged. */
  for (uint16_t visual = 0x13; visual <= 0x14; visual++) {
    frame.effects[0].visual = visual;
    frame.effects[0].geometry.data.rect = (ActionEffectLocalRect){-16.0f, -16.0f, 16.0f, 16.0f};
    CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &particles));
    CHECK(particles.vertex_count > 0);
  }
  frame.effects[0].visual = 0x15;
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == 0);

  frame.effects[0].phase = 99;
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == 0);
}

static void TestSwordBeamLightingTrailAndStars(void) {
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  frame.effects[0] = SceneEffect(kActionEffect_SwordBeam, 300);
  static ActionSceneEffectRenderBatch lighting, particles, repeat;
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &lighting));
  CHECK(lighting.vertex_count == 2 * kActionEffectGlowVertices + 8);
  CHECK(lighting.index_count == 2 * kActionEffectGlowIndices + 12);
  /* Run 20260810-184935 proves the normal state-$13 crescent centre is local
   * (40,-17), not (8,15). Keep the core there and lean only the spill 2px
   * into the wake. Both haze layers meet the decoded 32px crescent height,
   * then taper over their 80px and 56px lengths. */
  CHECK(fabsf(lighting.vertices[0].position.x - 338.0f) < 0.01f);
  CHECK(fabsf(lighting.vertices[0].position.y - 103.0f) < 0.01f);
  CHECK(fabsf(lighting.vertices[kActionEffectGlowVertices].position.x - 340.0f) < 0.01f);
  const int trail = 2 * kActionEffectGlowVertices;
  CHECK(fabsf(lighting.vertices[trail + 2].position.x - 260.0f) < 0.01f);
  CHECK(lighting.vertices[trail + 2].color.a == 0.0f);
  CHECK(fabsf(lighting.vertices[trail + 6].position.x - 284.0f) < 0.01f);
  CHECK(fabsf(lighting.vertices[trail].position.y - lighting.vertices[trail + 1].position.y) >
        30.0f);

  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == kActionSceneEffectSwordStarCount * 8);
  CHECK(particles.index_count == kActionSceneEffectSwordStarCount * 12);
  float nearest_star_x = -10000.0f, farthest_star_x = 10000.0f;
  float near_min_y = 10000.0f, near_max_y = -10000.0f;
  int near_star_count = 0;
  for (int i = 0; i < kActionSceneEffectSwordStarCount; i++) {
    const int base = i * 8;
    const float star_x =
        (particles.vertices[base].position.x + particles.vertices[base + 2].position.x) * 0.5f;
    const float star_y =
        (particles.vertices[base].position.y + particles.vertices[base + 2].position.y) * 0.5f;
    CHECK(star_x < 340.0f); /* every glint is behind the rightward crescent */
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
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &repeat));
  CHECK(SceneBatchesEqual(&particles, &repeat));
  frame.effects[0].pulse_ticks++;
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &repeat));
  CHECK(!SceneBatchesEqual(&particles, &repeat));
  bool materialization_changed = false;
  for (int i = 0; i < kActionSceneEffectSwordStarCount; i++) {
    const int base = i * 8;
    const float before_x =
        (particles.vertices[base].position.x + particles.vertices[base + 2].position.x) * 0.5f;
    const float before_y =
        (particles.vertices[base].position.y + particles.vertices[base + 2].position.y) * 0.5f;
    const float after_x =
        (repeat.vertices[base].position.x + repeat.vertices[base + 2].position.x) * 0.5f;
    const float after_y =
        (repeat.vertices[base].position.y + repeat.vertices[base + 2].position.y) * 0.5f;
    CHECK(fabsf(before_x - after_x) < 0.01f);
    CHECK(fabsf(before_y - after_y) < 0.01f);
    if (fabsf(particles.vertices[base].color.a - repeat.vertices[base].color.a) > 0.001f)
      materialization_changed = true;
  }
  CHECK(materialization_changed);
  frame.effects[0].pulse_ticks--;

  frame.effects[0].velocity_x = -8;
  frame.effects[0].flags |= kActionEffectFlag_FlipHorizontal;
  frame.effects[0].geometry.data.rect = (ActionEffectLocalRect){-48.0f, -33.0f, -32.0f, -1.0f};
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &repeat));
  CHECK(fabsf(repeat.vertices[0].position.x - 262.0f) < 0.01f);
  CHECK(fabsf(repeat.vertices[trail + 2].position.x - 340.0f) < 0.01f);
  CHECK(fabsf(repeat.vertices[trail + 6].position.x - 316.0f) < 0.01f);

  frame.effects[0].visual = 0x31;
  frame.effects[0].flags &= ~kActionEffectFlag_FlipHorizontal;
  frame.effects[0].geometry.data.rect = (ActionEffectLocalRect){40.0f, -9.0f, 56.0f, 23.0f};
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertex_count > 0);
  /* Aitos's boss-authored upper/lower crescents use the same portable comet
   * style while retaining their diagonal headings and priority-2 source. */
  frame.effects[0].visual = 0x21;
  frame.effects[0].velocity_x = -3;
  frame.effects[0].velocity_y = 1;
  frame.effects[0].obj_priority = 2;
  frame.effects[0].geometry.data.rect = (ActionEffectLocalRect){-8.0f, -17.0f, 16.0f, 7.0f};
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertex_count ==
        2 * kActionEffectGlowVertices + 8 + kActionSceneEffectSwordStarCount * 8);
  frame.effects[0].visual = 0x20;
  frame.effects[0].velocity_y = -1;
  frame.effects[0].geometry.data.rect = (ActionEffectLocalRect){-8.0f, -9.0f, 16.0f, 15.0f};
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertex_count > 0);
  /* Reflected state 1 from run 20260812-224123 travels right/up. Its wake
   * must therefore taper left/down, proving the newly admitted facing reaches
   * the heading-driven renderer rather than merely passing capture. */
  frame.effects[0].visual = 0x21;
  frame.effects[0].velocity_x = 3;
  frame.effects[0].velocity_y = -1;
  frame.effects[0].flags =
      kActionEffectFlag_Visible | kActionEffectFlag_FlipHorizontal | kActionEffectFlag_FlipVertical;
  frame.effects[0].geometry.data.rect = (ActionEffectLocalRect){-16.0f, -9.0f, 8.0f, 15.0f};
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertices[trail].position.x > repeat.vertices[trail + 2].position.x);
  CHECK(repeat.vertices[trail].position.y < repeat.vertices[trail + 2].position.y);
  frame.effects[0].visual = 0x32;
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(repeat.vertex_count == 0);
}

static void TestSceneCapacityAndMalformedInput(void) {
  ActionSceneEffectFrame frame = {
      .effect_count = kActionSceneEffectMaxInstances,
      .visible_count = kActionSceneEffectMaxInstances,
  };
  frame.effects[0] = SceneEffect(kActionEffect_BloodpoolBossLightning, 100);
  frame.effects[0].visual = 0;
  frame.effects[0].geometry.data.rect = (ActionEffectLocalRect){-6.0f, -83.0f, 11.0f, 117.0f};
  frame.effects[1] = SceneEffect(kActionEffect_SwordBeam, 120);
  for (unsigned i = 2; i < 2 + kActionSceneEffectMaxMarahnaLightningLinks; i++)
    frame.effects[i] = SceneEffect(kActionEffect_MarahnaLightningLink, 100 + i * 20);
  for (unsigned i = 2 + kActionSceneEffectMaxMarahnaLightningLinks;
       i < kActionSceneEffectMaxInstances; i++)
    frame.effects[i] = SceneEffect(kActionEffect_LightningTrap, 100 + i * 20);
  frame.effects[7] = SceneEffect(kActionEffect_MarahnaBossLightning, 140);
  frame.effects[8] = SceneEffect(kActionEffect_AitosWaterfall, 180);
  frame.effects[9] = SceneEffect(kActionEffect_SwordBeam, 200);
  frame.effects[9].visual = 0x21;
  frame.effects[9].velocity_x = -3;
  frame.effects[9].velocity_y = 1;
  frame.effects[10] = SceneEffect(kActionEffect_SwordBeam, 220);
  frame.effects[10].visual = 0x20;
  frame.effects[10].velocity_x = -3;
  frame.effects[10].velocity_y = -1;
  static ActionSceneEffectRenderBatch batch;
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &batch));
  /* The shared public capacity additionally reserves one bottom-atmosphere
   * record for the decoration builder; this actor-only worst case must remain
   * within it without manufacturing that separate map-derived record. */
  CHECK(batch.vertex_count ==
        kActionSceneEffectRenderMaxVertices - kActionSceneEffectWaterfallMistExtraVertices -
            kActionSceneEffectLavaReservoirParticleExtraVertices -
            kActionSceneEffectLavaReservoirGlowExtraVertices -
            kActionSceneEffectMaxFlamingWheels * kActionSceneEffectFlamingWheelExtraVertices);
  CHECK(batch.index_count ==
        kActionSceneEffectRenderMaxIndices - kActionSceneEffectWaterfallMistExtraIndices -
            kActionSceneEffectLavaReservoirParticleExtraIndices -
            kActionSceneEffectLavaReservoirGlowExtraIndices -
            kActionSceneEffectMaxFlamingWheels * kActionSceneEffectFlamingWheelExtraIndices);

  /* Wizard and Centaur share one ribbon allowance. Reject duplicate strikes
   * from either family, including mixed-family malformed frames, atomically. */
  const ActionEffectInstance original_bolt = frame.effects[0];
  const uint8_t bolt_kinds[] = {kActionEffect_BloodpoolBossLightning,
                              kActionEffect_CentaurLightning};
  for (unsigned first = 0; first < 2; ++first) {
    for (unsigned second = 0; second < 2; ++second) {
      frame.effects[0] = SceneEffect(bolt_kinds[first], 100);
      frame.effects[7] = SceneEffect(bolt_kinds[second], 140);
      CHECK(!ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &batch));
      CHECK(batch.vertex_count == 0);
      CHECK(batch.index_count == 0);
    }
  }
  frame.effects[0] = original_bolt;
  /* The expanded comet budget admits exactly the player plus the boss's two
   * diagonal children, not an arbitrary fourth forged stream. */
  frame.effects[7] = SceneEffect(kActionEffect_SwordBeam, 140);
  CHECK(!ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 0);
  CHECK(batch.index_count == 0);
  /* The runtime emitter admits five links. A forged sixth link must fail the
   * same cardinality contract as duplicate boss/player streams. */
  frame.effects[7] = SceneEffect(kActionEffect_MarahnaLightningLink, 140);
  CHECK(!ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 0);
  CHECK(batch.index_count == 0);
  frame.effects[7] = SceneEffect(kActionEffect_MarahnaBossLightning, 140);
  frame.effects[8] = SceneEffect(kActionEffect_MarahnaBossLightning, 160);
  CHECK(!ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 0);
  CHECK(batch.index_count == 0);
  frame.effects[8] = SceneEffect(kActionEffect_LightningTrap, 160);
  frame.effects[7] = SceneEffect(kActionEffect_LightningTrap, 140);

  frame.overflow = 1;
  CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.index_count == 0);
  frame.overflow = 0;
  frame.effect_count = kActionSceneEffectMaxInstances + 1;
  CHECK(!ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.index_count == 0);
  CHECK(!ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, NULL));
}

static void TestFirstActBossMagicGeometry(void) {
  static ActionSceneEffectRenderBatch lighting, particles, again;
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  ActionEffectInstance *effect = &frame.effects[0];
  *effect = SceneEffect(kActionEffect_CentaurLightning, 10);
  static const int kSegments[] = {2, 6, 9, 13, 2, 5, 9, 13};
  for (unsigned flip = 0; flip < 2; ++flip) {
    effect->flags = kActionEffectFlag_Visible | (flip ? kActionEffectFlag_FlipHorizontal : 0);
    for (unsigned visual = 0x19; visual <= 0x20; ++visual) {
      effect->visual = visual;
      effect->phase = kActionEffectPhase_BossLightningStrike;
      CHECK(
          ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &lighting));
      CHECK(lighting.vertex_count == 2 * kActionEffectGlowVertices + kSegments[visual - 0x19] * 8);
      /* First native row is y=4, minus the OAM bias; mirrored pixel centres
       * must change sides without reversing the bolt's downward direction. */
      const int first = 2 * kActionEffectGlowVertices;
      const float x =
          (lighting.vertices[first].position.x + lighting.vertices[first + 1].position.x) * .5f;
      const float y =
          (lighting.vertices[first].position.y + lighting.vertices[first + 1].position.y) * .5f;
      const float local_x = visual < 0x1D ? -.5f : -1.5f;
      CHECK(fabsf(x - effect->world_x - (flip ? -local_x : local_x)) < .001f);
      CHECK(fabsf(y - effect->world_y - 3.5f) < .001f);
      CHECK(
          ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
      CHECK(particles.vertex_count > 0);
      CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &again));
      CHECK(SceneBatchesEqual(&particles, &again));
    }
  }
  *effect = SceneEffect(kActionEffect_NorthwallBossMagic, 10);
  const ArRenderPointF expected[][2] = {
      {{-32, -9}, {16, -9}}, {{-36, -9}, {19, -9}}, {{-36, -12}, {19, -12}},
      {{-16, 7}, {8, 7}},    {{-8, 15}, {-8, 15}},
  };
  for (unsigned flip = 0; flip < 2; ++flip) {
    effect->flags = kActionEffectFlag_Visible | (flip ? kActionEffectFlag_FlipHorizontal : 0);
    for (unsigned visual = 0xA; visual <= 0xE; ++visual) {
      effect->visual = visual;
      CHECK(
          ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &lighting));
      const unsigned hands = visual == 0xE ? 1 : 2;
      CHECK(lighting.vertex_count == (int)hands * kActionEffectGlowVertices);
      for (unsigned hand = 0; hand < hands; ++hand) {
        const ArRenderPointF centre = lighting.vertices[hand * kActionEffectGlowVertices].position;
        CHECK(fabsf(centre.x - effect->world_x - (flip ? -1 : 1) * expected[visual - 0xA][hand].x) <
              .001f);
        CHECK(fabsf(centre.y - effect->world_y - expected[visual - 0xA][hand].y) < .001f);
      }
      CHECK(
          ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
      CHECK(particles.vertex_count > 0);
    }
  }
  for (unsigned visual = 3; visual <= 9; ++visual) {
    effect->visual = visual;
    effect->phase = visual == 9 ? kActionEffectPhase_NorthwallMagicFall
                                : kActionEffectPhase_NorthwallMagicImpact;
    CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &lighting));
    CHECK(lighting.vertex_count > 0);
    CHECK(ActionSceneEffectRender_Build(&frame, false, false, IdentityProjection, NULL, &again));
    CHECK(again.vertex_count == 0);
  }
  /* European impact aliases expand the native visual-8 bounds. The halo
   * must cover the wider art without shifting its captured world centre. */
  effect->visual = 8;
  effect->phase = kActionEffectPhase_NorthwallMagicImpact;
  effect->geometry.data.rect = (ActionEffectLocalRect){-16, -9, 16, 7};
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &lighting));
  effect->geometry.data.rect = (ActionEffectLocalRect){-32, -9, 32, 7};
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &again));
  CHECK(lighting.vertex_count == again.vertex_count);
  CHECK(lighting.vertices[0].position.x == again.vertices[0].position.x);
  CHECK(lighting.vertices[0].position.y == again.vertices[0].position.y);
  float native_reach = 0, expanded_reach = 0;
  for (int i = 0; i < lighting.vertex_count; ++i) {
    native_reach = fmaxf(native_reach, fabsf(lighting.vertices[i].position.x - effect->world_x));
    expanded_reach = fmaxf(expanded_reach, fabsf(again.vertices[i].position.x - effect->world_x));
  }
  CHECK(expanded_reach > native_reach + 10);
}

static void TestNorthwallWaterSplash(void) {
  static ActionSceneEffectRenderBatch particles, again;
  ActionSceneEffectFrame frame = {.effect_count = 1, .visible_count = 1};
  ActionEffectInstance *effect = &frame.effects[0];
  *effect = SceneEffect(kActionEffect_NorthwallBossMagic, 300);
  effect->world_y = 448;
  effect->phase = kActionEffectPhase_NorthwallMagicImpact;
  effect->visual = 3;
  effect->geometry.data.rect = (ActionEffectLocalRect){-8, -17, 8, -1};
  const unsigned ages[] = {3, 9, 13};
  ArRenderPointF droplet[3];
  for (unsigned age = 0; age < 3; ++age) {
    effect->phase_ticks = ages[age];
    CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
    CHECK(particles.vertex_count > 0 &&
          particles.vertex_count <= kActionSceneEffectParticlesPerInstance * 4);
    if (age == 0) CHECK(particles.vertex_count == kActionSceneEffectParticlesPerInstance * 4);
    droplet[age] = (ArRenderPointF){
        (particles.vertices[0].position.x + particles.vertices[2].position.x) * .5f,
        (particles.vertices[0].position.y + particles.vertices[2].position.y) * .5f};
  }
  CHECK(droplet[0].x < effect->world_x);
  CHECK(droplet[2].x < droplet[1].x && droplet[1].x < droplet[0].x);
  CHECK(droplet[1].y < droplet[0].y); /* Rises, then falls back toward the water. */
  CHECK(droplet[2].y > droplet[1].y && droplet[2].y < 447);

  effect->phase_ticks = 6;
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &again));
  CHECK(SceneBatchesEqual(&particles, &again)); /* Re-present / pause never advances the burst. */
  for (int i = 0; i < particles.vertex_count; i += 4) {
    const float x = (particles.vertices[i].position.x + particles.vertices[i + 2].position.x) * .5f;
    const float y = (particles.vertices[i].position.y + particles.vertices[i + 2].position.y) * .5f;
    CHECK((i / 4) & 1 ? x > effect->world_x : x < effect->world_x);
    CHECK(y < 447 && y > 417);
  }
  effect->flags |= kActionEffectFlag_FlipHorizontal;
  effect->visual = 8;
  effect->geometry.data.rect = (ActionEffectLocalRect){-32, -9, 32, -1};
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &again));
  CHECK(SceneBatchesEqual(&particles, &again)); /* Art growth/facing cannot move the waterline. */
  effect->world_y += 2; /* Regional placement follows the captured actor, not a fixed world Y. */
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &again));
  CHECK(again.vertex_count == particles.vertex_count);
  for (int i = 0; i < particles.vertex_count; ++i) {
    CHECK(again.vertices[i].position.x == particles.vertices[i].position.x);
    CHECK(fabsf(again.vertices[i].position.y - particles.vertices[i].position.y - 2) < .001f);
  }
  effect->phase_ticks = 20;
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == 0); /* Fits the shorter European impact as well. */
  effect->phase_ticks = 1000;
  CHECK(ActionSceneEffectRender_Build(&frame, false, true, IdentityProjection, NULL, &particles));
  CHECK(particles.vertex_count == 0); /* No looping / respawn on later impact visuals. */
  CHECK(ActionSceneEffectRender_Build(&frame, true, false, IdentityProjection, NULL, &again));
  CHECK(again.vertex_count > 0); /* The native magic's light has its own lifetime. */
}

static void TestNorthwallGeometryBudget(void) {
  static ActionSceneEffectRenderBatch batch;
  ActionSceneEffectFrame frame = {
      .effect_count = kActionSceneEffectMaxInstances,
      .visible_count = kActionSceneEffectMaxInstances,
  };
  /* Saturate the actor list to check the ordinary per-actor allowance. The
   * charge has two palm glows; fall/impact have one. All can emit 12 quads. */
  const uint8_t phases[] = {kActionEffectPhase_NorthwallMagicCharge,
                          kActionEffectPhase_NorthwallMagicFall,
                          kActionEffectPhase_NorthwallMagicImpact};
  const uint8_t visuals[] = {0x0A, 9, 3};
  for (unsigned phase = 0; phase < 3; ++phase) {
    for (unsigned i = 0; i < kActionSceneEffectMaxInstances; ++i) {
      frame.effects[i] = SceneEffect(kActionEffect_NorthwallBossMagic, 100 + i * 20);
      frame.effects[i].phase = phases[phase];
      frame.effects[i].visual = visuals[phase];
      frame.effects[i].phase_ticks = 3; /* Every impact particle is alive. */
    }
    CHECK(ActionSceneEffectRender_Build(&frame, true, true, IdentityProjection, NULL, &batch));
    const int glows = phase == 0 ? 2 : 1;
    CHECK(batch.vertex_count == kActionSceneEffectMaxInstances *
                                    (glows * kActionEffectGlowVertices + 12 * 4));
    CHECK(batch.index_count == kActionSceneEffectMaxInstances *
                                   (glows * kActionEffectGlowIndices + 12 * 6));
  }
}

/* Sample the actual triangle/color field, independent of vertex numbering.
 * Clipping can split triangles but must preserve every interior sample. */
static float SampleForestLight(
    const ActionSceneEffectRenderBatch *batch, float x, float y, bool foreground) {
  for (int i = 0; i < batch->index_count; i += 3) {
    const ArRenderVertex2D *a = &batch->vertices[batch->indices[i]];
    const ArRenderVertex2D *b = &batch->vertices[batch->indices[i + 1]];
    const ArRenderVertex2D *c = &batch->vertices[batch->indices[i + 2]];
    const float denominator = (b->position.y - c->position.y) *
        (a->position.x - c->position.x) + (c->position.x - b->position.x) *
        (a->position.y - c->position.y);
    if (fabsf(denominator) < .0001f) continue;
    const float u = ((b->position.y - c->position.y) * (x - c->position.x) +
        (c->position.x - b->position.x) * (y - c->position.y)) / denominator;
    const float v = ((c->position.y - a->position.y) * (x - c->position.x) +
        (a->position.x - c->position.x) * (y - c->position.y)) / denominator;
    const float w = 1 - u - v;
    if (u < -.00001f || v < -.00001f || w < -.00001f) continue;
    return foreground ? u * a->color.r + v * b->color.r + w * c->color.r :
                        u * a->color.a + v * b->color.a + w * c->color.a;
  }
  return 0;
}

static void TestForestClippingPreservesField(int16_t world_x, int16_t world_y) {
  ActionSceneEffectFrame frame = {.decoration_count = 1, .decoration_visible_count = 1};
  ActionEffectInstance *effect = &frame.decorations[0];
  *effect = (ActionEffectInstance){
    .phase = kActionEffectPhase_ForestCanopyLight, .flags = kActionEffectFlag_Visible,
    .world_x = world_x, .world_y = world_y, .phase_ticks = 512,
    .projection_plane = kActionEffectProjectionPlane_BetweenBackgrounds,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
  };
  static ActionSceneEffectRenderBatch reference, clipped;
  for (int foreground = 0; foreground < 2; foreground++) {
    effect->kind = foreground ? kActionEffect_ForestForwardLight : kActionEffect_ForestCanopyLight;
    effect->render_layer = foreground ? kActionEffectRenderLayer_ForegroundLight :
                                       kActionEffectRenderLayer_Bg2Plane;
    effect->flags = kActionEffectFlag_Visible;
    CHECK(ActionSceneDecorationRender_Build(&frame, effect->render_layer, true, false,
        IdentityProjection, NULL, NULL, &reference));
    for (int crop = 0; crop < 8; crop++) {
      effect->flags |= kActionEffectFlag_ClipToRect;
      effect->clip_rect = (ActionEffectLocalRect){-320 + crop * 41.3f, 37.2f + crop * 19,
                                                 345.7f - crop * 22, 530.1f - crop * 13};
      CHECK(ActionSceneDecorationRender_Build(&frame, effect->render_layer, true, false,
          IdentityProjection, NULL, NULL, &clipped));
      CHECK(clipped.vertex_count > 0 && clipped.vertex_count <= 1680);
      const ActionEffectLocalRect r = {effect->world_x + effect->clip_rect.x0,
          effect->world_y + effect->clip_rect.y0, effect->world_x + effect->clip_rect.x1,
          effect->world_y + effect->clip_rect.y1};
      for (int i = 0; i < clipped.vertex_count; i++) {
        const ArRenderPointF p = clipped.vertices[i].position;
        CHECK(p.x >= r.x0 - .001f && p.x <= r.x1 + .001f);
        CHECK(p.y >= r.y0 - .001f && p.y <= r.y1 + .001f);
      }
      float peak = 0, difference = 0;
      for (float y = r.y0 + .1f; y < r.y1; y += 7.1f) {
        for (float x = r.x0 + .1f; x < r.x1; x += 7.3f) {
          const float expected = SampleForestLight(&reference, x, y, foreground);
          const float actual = SampleForestLight(&clipped, x, y, foreground);
          peak = fmaxf(peak, expected);
          difference = fmaxf(difference, fabsf(actual - expected));
        }
      }
      CHECK(peak > .1f && difference < .0005f);
    }
    /* Fully hidden rays vanish instead of collapsing onto a border. */
    effect->clip_rect = (ActionEffectLocalRect){500,0,600,544};
    CHECK(ActionSceneDecorationRender_Build(&frame, effect->render_layer, true, false,
        IdentityProjection, NULL, NULL, &clipped));
    CHECK(clipped.vertex_count == 0 && clipped.index_count == 0);
    effect->flags = kActionEffectFlag_Visible;
    frame.decorations[1] = *effect;
    frame.decoration_count = 2;
    CHECK(!ActionSceneDecorationRender_Build(&frame, effect->render_layer, true, false,
        IdentityProjection, NULL, NULL, &clipped));
    CHECK(clipped.vertex_count == 0 && clipped.index_count == 0);
    frame.decoration_count = 1;
  }
}

static void TestForestCanopyGeometry(void) {
  ActionSceneEffectFrame frame = {.decoration_count = 1, .decoration_visible_count = 1};
  frame.decorations[0] = (ActionEffectInstance){
    .kind = kActionEffect_ForestCanopyLight,
    .phase = kActionEffectPhase_ForestCanopyLight,
    .flags = kActionEffectFlag_Visible,
    .generation = 1, .pulse_generation = 2, .phase_ticks = 123,
    .render_layer = kActionEffectRenderLayer_Bg2Plane,
    .projection_plane = kActionEffectProjectionPlane_BetweenBackgrounds,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384, 0, 384, 544}},
  };
  static ActionSceneEffectRenderBatch first, repeat, clipped;
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg2Plane,
      true, true, IdentityProjection, NULL, NULL, &first));
  CHECK(first.vertex_count >= 30 && first.vertex_count <= 878);
  CHECK(first.index_count >= 120 && first.index_count <= 1692);
  /* Individual shafts have transparent edges, a bright core, and slope down
   * toward the left; they must not regress to a uniform screen-space wash. */
  CHECK(SampleForestLight(&first, -320, 20, false) == 0);
  CHECK(SampleForestLight(&first, 230, 20, false) > .18f);
  CHECK(SampleForestLight(&first, 122, 200, false) > .18f);
  CHECK(SampleForestLight(&first, 230, 200, false) < .01f);
  CHECK(ActionEffectProjection_RequiredBgPlaneMask(NULL, &frame) ==
      (1u << SR_PPU_OVERLAY_BG2));
  for (int i = 0; i < first.vertex_count; i++)
    CHECK(first.vertices[i].color.a >= 0 && first.vertices[i].color.a <= 0.941f);
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg2Plane,
      true, true, IdentityProjection, NULL, NULL, &repeat));
  CHECK(SceneBatchesEqual(&first, &repeat));
  frame.decorations[0].phase_ticks++;
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg2Plane,
      true, true, IdentityProjection, NULL, NULL, &repeat));
  CHECK(!SceneBatchesEqual(&first, &repeat));
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg1Plane,
      true, true, IdentityProjection, NULL, NULL, &repeat));
  CHECK(repeat.vertex_count == 0);
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg2Plane,
      false, false, IdentityProjection, NULL, NULL, &repeat));
  CHECK(repeat.vertex_count == 0);

  DioramaProjection projection = {
    .valid = true, .matrix = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},
    .aspect_x = 1, .height_scale = 1, .texture_width = 256, .texture_height = 224,
    .output_width = 256, .output_height = 224,
    .bg2_plane = {.valid = true, .u1 = 1, .v1 = 1},
    .bg1_plane = {.valid = true, .u1 = 1, .v1 = 1},
  };
  ActionEffectProjectionContext context = {.diorama_projection = &projection};
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg2Plane,
      true, false, ActionEffectProjection_ProjectPoint,
      ActionEffectProjection_ClipBounds, &context, &clipped));
  CHECK(clipped.vertex_count > 0 && clipped.vertex_count <= 1400);
  for (int i = 0; i < clipped.vertex_count; i++) {
    const ArRenderPointF p = clipped.vertices[i].position;
    CHECK(p.x >= 64 && p.x <= 192 && p.y >= 56 && p.y <= 168);
  }
  /* Point particles still reject off-plane samples. Only mesh triangles are
   * clipped, so motes cannot accumulate along the frame edge. */
  ArRenderPointF point;
  CHECK(!ActionEffectProjection_ProjectPoint(&context, &frame.decorations[0],
      -1, 20, &point));
  CHECK(!ActionEffectProjection_ProjectPoint(&context, &frame.decorations[0],
      1, 225, &point));

  /* Cropped extended-row planes use fractional UV bounds and a nonzero
   * texture origin. Clipping must not lose the rays to roundoff at
   * the boundary when a UV is converted back to capture coordinates. */
  projection.texture_width = 1024;
  projection.texture_height = 352;
  projection.texture_x_origin = 83;
  projection.bg2_plane.u0 = 0.1f;
  projection.bg2_plane.u1 = 0.77f;
  projection.bg2_plane.v0 = 0.127f;
  projection.bg2_plane.v1 = 0.937f;
  context.ws_extra_top = 64;
  context.bg2_camera_y = 220;
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg2Plane,
      true, false, ActionEffectProjection_ProjectPoint,
      ActionEffectProjection_ClipBounds, &context, &clipped));
  CHECK(clipped.vertex_count > 0 && clipped.vertex_count <= 1400);
  CHECK(clipped.index_count <= 600);
  for (int i = 0; i < clipped.vertex_count; i++) {
    CHECK(isfinite(clipped.vertices[i].position.x));
    CHECK(isfinite(clipped.vertices[i].position.y));
  }
}

static void TestForestFanOut(void) {
  ActionSceneEffectFrame frame = {.decoration_count = 1, .decoration_visible_count = 1};
  frame.decorations[0] = (ActionEffectInstance){
    .kind = kActionEffect_ForestCanopyLight,
    .phase = kActionEffectPhase_ForestCanopyLight, .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_Bg2Plane,
    .projection_plane = kActionEffectProjectionPlane_BetweenBackgrounds,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
  };
  static ActionSceneEffectRenderBatch batch;
  const float heights[] = {32, 200};
  for (int tick = 123; tick < 2048; tick += 701) {
    frame.decorations[0].phase_ticks = (uint16_t)tick;
    CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg2Plane,
        true, false, IdentityProjection, NULL, NULL, &batch));
    float left[2][2] = {{0}}, right[2][2] = {{0}}, centre[2][2] = {{0}};
    for (int row = 0; row < 2; row++) {
      int ray = -1;
      bool inside = false;
      float peak = 0;
      for (float x = -300; x < 384; x += .25f) {
        const float light = SampleForestLight(&batch, x, heights[row], false);
        if (light > .0001f) {
          if (!inside) {
            ray++;
            peak = 0;
            CHECK(ray < 2);
            if (ray >= 2) return;
            left[row][ray] = x;
          }
          right[row][ray] = x;
          if (light > peak) centre[row][ray] = x;
          peak = fmaxf(peak, light);
        }
        inside = light > .0001f;
      }
      CHECK(ray == 1);
    }
    /* Sample the rendered light: two distinct rays spread apart downward,
     * and each widens in the same proportion as their separation. Extrapolating
     * their cores and edges therefore reaches one source above the view. */
    const float top_gap = centre[0][1] - centre[0][0];
    const float bottom_gap = centre[1][1] - centre[1][0];
    CHECK(bottom_gap > top_gap + 10);
    const float origin_y = heights[0] - top_gap * (heights[1] - heights[0]) /
        (bottom_gap - top_gap);
    CHECK(origin_y < -224);
    for (int ray = 0; ray < 2; ray++) {
      const float width_ratio = (right[1][ray] - left[1][ray]) /
          (right[0][ray] - left[0][ray]);
      CHECK(fabsf(width_ratio - bottom_gap / top_gap) < .04f);
    }
  }
}

static void TestForestParticles(void) {
  ActionSceneEffectFrame frame = {.decoration_count = 1, .decoration_visible_count = 1};
  frame.decorations[0] = (ActionEffectInstance){
    .kind = kActionEffect_ForestCanopyLight,
    .phase = kActionEffectPhase_ForestCanopyLight,
    .flags = kActionEffectFlag_Visible,
    .generation = 0x46000000u, .pulse_generation = 0x66000000u,
    .world_x = 928, .world_y = 80,
    .render_layer = kActionEffectRenderLayer_Bg2Plane,
    .projection_plane = kActionEffectProjectionPlane_BetweenBackgrounds,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
  };
  static ActionSceneEffectRenderBatch particles, repeat, light;
  float brightest = 0;
  for (int ticks = 0; ticks < 2048; ticks += 128) {
    frame.decorations[0].phase_ticks = (uint16_t)ticks;
    CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg2Plane,
        false, true, IdentityProjection, NULL, NULL, &particles));
    CHECK(particles.vertex_count <= 856 && particles.index_count <= 1284);
    CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg2Plane,
        true, false, IdentityProjection, NULL, NULL, &light));
    int fine_motes = 0;
    for (int i = 0; i < particles.vertex_count; i += 4) {
      const ArRenderVertex2D *v = &particles.vertices[i];
      const float radius = hypotf(v[1].position.x - v[3].position.x,
          v[1].position.y - v[3].position.y) * .5f;
      if (radius >= 1) { /* The original large drifting motes. */
        brightest = fmaxf(brightest, v[0].color.a);
        continue;
      }
      fine_motes++;
      CHECK(radius >= .449f && radius <= .751f);
      const float x = (v[0].position.x + v[2].position.x) * .5f;
      const float y = (v[0].position.y + v[2].position.y) * .5f;
      CHECK(SampleForestLight(&light, x, y, false) > .0001f);
    }
    CHECK(fine_motes >= 8); /* Small dust pockets remain present in the shafts. */
    CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg2Plane,
        false, true, IdentityProjection, NULL, NULL, &repeat));
    CHECK(SceneBatchesEqual(&particles, &repeat));
    frame.decorations[0].phase_ticks = (uint16_t)(ticks + 65536 - 2048);
    CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg2Plane,
        false, true, IdentityProjection, NULL, NULL, &repeat));
    CHECK(SceneBatchesEqual(&particles, &repeat)); /* Continuous across clock wrap. */
  }
  CHECK(brightest > .65f); /* Motes must be readable, not subpixel faint sparks. */
  frame.decorations[0].kind = kActionEffect_ForestLeaves;
  frame.decorations[0].render_layer = kActionEffectRenderLayer_Bg2Alpha;
  int visible_frames = 0;
  for (int ticks = 0; ticks < 2048; ticks += 128) {
    frame.decorations[0].phase_ticks = (uint16_t)ticks;
    CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg2Alpha,
        true, true, IdentityProjection, NULL, NULL, &particles));
    CHECK(particles.vertex_count <= 144 && particles.index_count <= 240);
    if (particles.vertex_count) visible_frames++;
    for (int i = 0; i < particles.vertex_count; i++) {
      /* Dark body and a distinct lit rim share the same alpha pass. */
      CHECK(i % 9 < 6 ? particles.vertices[i].color.r < .15f :
                       particles.vertices[i].color.r > .60f);
      CHECK(particles.vertices[i].color.a >= 0 && particles.vertices[i].color.a <= .901f);
    }
    CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg2Alpha,
        true, true, IdentityProjection, NULL, NULL, &repeat));
    CHECK(SceneBatchesEqual(&particles, &repeat));
    /* The last particle cycle before the 16-bit scene clock wraps must match
     * the first, avoiding a synchronized position jump during a long stay. */
    frame.decorations[0].phase_ticks = (uint16_t)(ticks + 65536 - 2048);
    CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg2Alpha,
        true, true, IdentityProjection, NULL, NULL, &repeat));
    CHECK(SceneBatchesEqual(&particles, &repeat));
  }
  CHECK(visible_frames == 16);
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg2Plane,
      true, true, IdentityProjection, NULL, NULL, &repeat));
  CHECK(repeat.vertex_count == 0); /* Silhouettes cannot enter the additive batch. */
  frame.decorations[0].kind = kActionEffect_ForestForwardLight;
  frame.decorations[0].render_layer = kActionEffectRenderLayer_ForegroundLight;
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_ForegroundLight,
      true, false, IdentityProjection, NULL, NULL, &repeat));
  CHECK(repeat.vertex_count >= 30 && repeat.vertex_count <= 150);
  CHECK(repeat.index_count <= 600);
  float peak = 0;
  for (int i = 0; i < repeat.vertex_count; i++) {
    const ArRenderVertex2D *v = &repeat.vertices[i];
    CHECK(v->color.a == 1 && v->color.r <= .601f);
    if (v->position.y <= frame.decorations[0].world_y + 224) CHECK(v->color.r == 0);
    peak = fmaxf(peak, v->color.r);
  }
  CHECK(peak > .54f);
}

static void TestForestBossClearingLight(void) {
  ActionSceneEffectFrame frame = {.decoration_count = 1, .decoration_visible_count = 1};
  frame.decorations[0] = (ActionEffectInstance){
    .kind = kActionEffect_ForestForwardLight,
    .phase = kActionEffectPhase_ForestCanopyLight, .flags = kActionEffectFlag_Visible,
    /* Observed arena cameras: BG1 (3720,543), BG2 (1860,181). */
    .world_x = 2918, .world_y = 202,
    .render_layer = kActionEffectRenderLayer_ForegroundLight,
    .projection_plane = kActionEffectProjectionPlane_BetweenBackgrounds,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
  };
  static ActionSceneEffectRenderBatch batch;
  for (int ticks = 0; ticks < 2048; ticks += 512) {
    frame.decorations[0].phase_ticks = (uint16_t)ticks;
    CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_ForegroundLight,
        true, false, IdentityProjection, NULL, NULL, &batch));
    for (int i = 0; i < batch.vertex_count; i++) {
      const ArRenderVertex2D *v = &batch.vertices[i];
      CHECK(v->color.r <= .881f && v->color.a == 1);
      if (v->position.y <= 394) CHECK(v->color.r == 0); /* Clear the HUD rows. */
    }
    /* Sample rider, horse and ground heights across the battle lane. A moving
     * boss must encounter both strong highlights and distinct shaded gaps. */
    const float heights[] = {426, 475, 530};
    for (int row = 0; row < 3; row++) {
      float peak = 0, minimum = 1, total = 0;
      int lit = 0, samples = 0;
      for (float x = 2770; x <= 3170; x += 4) {
        const float value = SampleForestLight(&batch, x, heights[row], true);
        peak = fmaxf(peak, value);
        minimum = fminf(minimum, value);
        total += value;
        samples++;
        if (value > .12f) lit++;
      }
      CHECK(peak > .75f && minimum < .10f);
      CHECK(total / samples > .25f && lit * 2 > samples);
    }
  }
}

static void TestForestLayerScroll(void) {
  ActionSceneEffectFrame frame = {.decoration_count = 1, .decoration_visible_count = 1};
  frame.decorations[0] = (ActionEffectInstance){
    .kind = kActionEffect_ForestForwardLight,
    .phase = kActionEffectPhase_ForestCanopyLight,
    .flags = kActionEffectFlag_Visible,
    .world_x = 928, .world_y = 80, .phase_ticks = 512,
    .render_layer = kActionEffectRenderLayer_ForegroundLight,
    .projection_plane = kActionEffectProjectionPlane_BetweenBackgrounds,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
  };
  ActionEffectProjectionContext context = {
    .bg1_camera_x = 1000, .bg2_camera_x = 600,
    .bg1_camera_y = 320, .bg2_camera_y = 160,
    .visible_width = 512, .snes_height = 224, .viewport = {0,0,512,224},
  };
  static ActionSceneEffectRenderBatch first, moved;
  for (int foreground = 0; foreground < 2; foreground++) {
    const uint8_t layer = foreground ? kActionEffectRenderLayer_ForegroundLight :
                                      kActionEffectRenderLayer_Bg2Plane;
    frame.decorations[0].kind = foreground ? kActionEffect_ForestForwardLight :
                                            kActionEffect_ForestCanopyLight;
    frame.decorations[0].render_layer = layer;
    CHECK(ActionSceneDecorationRender_Build(&frame, layer, true, false,
        ActionEffectProjection_ProjectPoint, ActionEffectProjection_ClipBounds, &context, &first));
    /* A mean-camera movement of 16 must move each light left by 16. The old
     * camera-anchored front shaft stayed in exactly the same screen position. */
    frame.decorations[0].world_x += 16;
    context.bg1_camera_x += 24;
    context.bg2_camera_x += 8;
    CHECK(ActionSceneDecorationRender_Build(&frame, layer, true, false,
        ActionEffectProjection_ProjectPoint, ActionEffectProjection_ClipBounds, &context, &moved));
    CHECK(first.vertex_count > 0 && moved.vertex_count > 0);
    float difference = 0, peak = 0;
    for (float y = 8.3f; y < 216; y += 9.1f) {
      for (float x = 1.2f; x < 496; x += 7.3f) {
        const float expected = SampleForestLight(&first, x + 16, y, foreground);
        const float actual = SampleForestLight(&moved, x, y, foreground);
        peak = fmaxf(peak, expected);
        difference = fmaxf(difference, fabsf(actual - expected));
      }
    }
    CHECK(peak > .1f && difference < .0005f);
    /* Sweep the complete room, including camera boundaries and sway phases.
     * Culling may never exceed the fixed geometry allocation. */
    for (int x = 128; x < 3456; x += 31) {
      frame.decorations[0].world_x = (int16_t)x;
      for (int y = -160; y <= 352; y += 128) {
        frame.decorations[0].world_y = (int16_t)y;
        frame.decorations[0].phase_ticks = (uint16_t)(x + y);
        CHECK(ActionSceneDecorationRender_Build(&frame, layer, true, true,
            IdentityProjection, NULL, NULL, &moved));
        CHECK(moved.vertex_count <= 2536 && moved.index_count <= 4884);
      }
    }
    frame.decorations[0].world_x = 928;
    frame.decorations[0].world_y = 80;
    frame.decorations[0].phase_ticks = 512;
    context.bg1_camera_x = 1000;
    context.bg2_camera_x = 600;
  }
}

static void TestBetweenBackgroundsProjection(void) {
  DioramaProjection projection = {
    .valid = true, .matrix = {1,0,0,0, 0,1,0,0, 0,0,1,.25f, 0,0,0,1},
    .aspect_x = 1, .height_scale = 1, .texture_width = 512, .texture_height = 352,
    .output_width = 720, .output_height = 448,
    .bg2_plane = {.valid = true, .u1 = 1, .v1 = 1, .z_world = -.4f,
                  .rake = .1f, .bow = .2f},
    .bg1_plane = {.valid = true, .u1 = 1, .v1 = 1, .z_world = .2f,
                  .rake = -.3f, .bow = .1f},
  };
  ActionEffectProjectionContext context = {
    .diorama_projection = &projection, .ws_extra = 128, .ws_extra_top = 64,
    .bg1_camera_x = 1000, .bg1_camera_y = 300,
    .bg2_camera_x = 500, .bg2_camera_y = 200,
  };
  ActionEffectInstance light = {
    .world_x = 800, .world_y = 200, .flags = kActionEffectFlag_Visible,
    .projection_plane = kActionEffectProjectionPlane_BetweenBackgrounds,
    .render_layer = kActionEffectRenderLayer_Bg2Plane,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-448,0,64,544}},
  };
  ArRenderPointF actual, expected;
  CHECK(ActionEffectProjection_ProjectPoint(&context, &light, 20, 100, &actual));
  /* Independent midpoint camera, but the same finite backdrop footprint.
   * This keeps light out of the diorama void even at perspective side edges. */
  CHECK(Diorama_ProjectCapturedBg2Point(&projection, 198, 114,
      &expected, NULL, NULL));
  CHECK(fabsf(actual.x - expected.x) < .0001f);
  CHECK(fabsf(actual.y - expected.y) < .0001f);
  projection.bg2_plane.valid = false;
  CHECK(!ActionEffectProjection_ProjectPoint(&context, &light, 20, 100, &actual));
  context.diorama_projection = NULL;
  context.viewport = (ArRenderRectI){0,0,512,224};
  context.visible_width = 512;
  context.snes_height = 224;
  CHECK(ActionEffectProjection_ProjectPoint(&context, &light, 20, 100, &actual));
  CHECK(actual.x == 198 && actual.y == 50);
  CHECK(ActionEffectProjection_IntersectsFlatViewport(&context, &light));
  /* Room padding is outside the illuminated volume even when it lies inside
   * the capture plane. Clip meshes there and reject individual motes. */
  light.flags |= kActionEffectFlag_ClipToRect;
  light.clip_rect = (ActionEffectLocalRect){10,20,80,120};
  CHECK(!ActionEffectProjection_ProjectPoint(&context, &light, 9, 100, &actual));
  CHECK(ActionEffectProjection_ProjectPoint(&context, &light, 10, 100, &expected));
  ActionEffectLocalRect bounds;
  CHECK(ActionEffectProjection_ClipBounds(&context, &light, &bounds));
  CHECK(bounds.x0 == 10 && bounds.x1 == 64 && bounds.y0 == 50 && bounds.y1 == 120);
  CHECK(!ActionEffectProjection_ProjectPoint(&context, &light, -100, 100, &actual));
  light.clip_rect.x1 = light.clip_rect.x0;
  CHECK(!ActionEffectProjection_ProjectPoint(&context, &light, 20, 100, &actual));
  CHECK(!ActionEffectProjection_ClipBounds(&context, &light, &bounds));
  light.world_x = 2000;
  CHECK(!ActionEffectProjection_IntersectsFlatViewport(&context, &light));
}

static void TestCaveWaterUsesHighPlane(void) {
  DioramaProjection projection = RakedApronProjection();
  projection.texture_width = 1024;
  projection.texture_height = 768;
  projection.texture_x_origin = 384;
  projection.bg2_high_plane = (DioramaPlaneProjection){
    .valid = true, .u1 = 1, .v1 = 1, .z_world = .4f, .rake = .17f, .bow = .11f,
  };
  ActionEffectProjectionContext context = {
    .bg1_camera_x = 1000, .bg1_camera_y = 300,
    .bg2_camera_x = 608, .bg2_camera_y = 752,
    .ws_extra = 52, .ws_extra_top = 64, .diorama_projection = &projection,
  };
  ActionEffectInstance effect = {
    .world_x = 736, .world_y = 896,
    .projection_plane = kActionEffectProjectionPlane_Bg2High,
    .render_layer = kActionEffectRenderLayer_Bg2HighPlane,
  };
  ArRenderPointF actual, expected, wrong;
  CHECK(ActionEffectProjection_ProjectPoint(&context, &effect, 0, 0, &actual));
  CHECK(Diorama_ProjectCapturedBg2HighPoint(&projection, 180, 208, &expected, NULL, NULL));
  CHECK(fabsf(actual.x - expected.x) < .001f && fabsf(actual.y - expected.y) < .001f);
  projection.bg2_plane.u0 = projection.bg2_plane.v0 = 0;
  projection.bg2_plane.u1 = projection.bg2_plane.v1 = 1;
  CHECK(Diorama_ProjectCapturedBg2Point(&projection, 180, 208, &wrong, NULL, NULL));
  CHECK(fabsf(actual.x - wrong.x) > 1); /* Exact high-band depth, not low BG2. */
  /* Replacing the distant backdrop cannot retarget foreground water/mist. */
  projection.bg2_plane.valid = false;
  projection.bg2_skybox = (DioramaSkyboxProjection){.count = 1, .active_band = -1,
      .bands = {{0,0,512,352,0,1}}};
  CHECK(ActionEffectProjection_ProjectPoint(&context, &effect, 0, 0, &actual));
  CHECK(fabsf(actual.x - expected.x) < .001f && fabsf(actual.y - expected.y) < .001f);
  projection.bg2_high_plane.valid = false;
  CHECK(!ActionEffectProjection_ProjectPoint(&context, &effect, 0, 0, &actual));
}

static void TestCaveEnvironmentGeometry(void) {
  ActionSceneEffectFrame frame = {.decoration_count = 1, .decoration_visible_count = 1};
  static ActionSceneEffectRenderBatch first, again;
  const uint8_t kinds[] = {kActionEffect_CaveWater, kActionEffect_CaveDrips,
                          kActionEffect_TempleDust, kActionEffect_TowerWindowLight,
                          kActionEffect_CaveMist, kActionEffect_CaveSheen,
                          kActionEffect_CaveAmbientLight, kActionEffect_TempleGrit,
                          kActionEffect_TempleGroundMist};
  const int vertices[] = {1100,960,1300,672,4032,1320,4032,512,3360};
  const int indices[] = {3000,1440,1950,1440,8640,1980,8640,1536,7200};
  for (unsigned family = 0; family < 9; family++) {
    ActionEffectInstance *e = &frame.decorations[0];
    *e = (ActionEffectInstance){
      .kind = kinds[family], .phase = kActionEffectPhase_CaveEnvironment, .source_mask = 0xFF,
      .flags = kActionEffectFlag_Visible, .visual = family == 3 ? 4 : family >= 6 ? 3 : 2,
      .render_layer = family == 0 ? kActionEffectRenderLayer_Bg2HighPlane :
          family == 3 || family == 6 ? kActionEffectRenderLayer_ForegroundLight :
          family == 4 || family == 7 ? kActionEffectRenderLayer_WorldDust :
          family == 5 ? kActionEffectRenderLayer_Bg1Plane :
          family == 8 ? kActionEffectRenderLayer_Bg1Mist : kActionEffectRenderLayer_WorldOverlay,
      .projection_plane = family == 0 || family == 4 ? kActionEffectProjectionPlane_Bg2High :
                                                     kActionEffectProjectionPlane_Bg1,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
    };
    if (family == 8) e->geometry.data.rect = (ActionEffectLocalRect){0,-26,64,0};
    int total = 0;
    for (int x = 128; x < 2048; x += 173) {
      for (int y = -160; y < 1792; y += 157) {
        e->world_x = (int16_t)x;
        e->world_y = (int16_t)y;
        e->phase_ticks = (uint16_t)(x + y);
        CHECK(ActionSceneDecorationRender_Build(&frame, e->render_layer, true, true,
            IdentityProjection, NULL, NULL, &first));
        total += first.vertex_count;
        CHECK(first.vertex_count <= vertices[family] && first.index_count <= indices[family]);
        for (int i = 0; i < first.vertex_count; i++) {
          CHECK(isfinite(first.vertices[i].position.x) && isfinite(first.vertices[i].position.y));
          CHECK(first.vertices[i].color.a >= 0 && first.vertices[i].color.a <= 1);
        }
        e->phase_ticks = (uint16_t)(e->phase_ticks + 4096);
        CHECK(ActionSceneDecorationRender_Build(&frame, e->render_layer, true, true,
            IdentityProjection, NULL, NULL, &again));
        CHECK(SceneBatchesEqual(&first, &again)); /* Also covers the 16-bit clock seam. */
      }
    }
    CHECK(total > 0);
    frame.decorations[1] = *e;
    frame.decoration_count = 2;
    CHECK(!ActionSceneDecorationRender_Build(&frame, e->render_layer, true, true,
        IdentityProjection, NULL, NULL, &first));
    CHECK(!first.vertex_count && !first.index_count);
    frame.decoration_count = 1;
  }
  ActionSceneEffectFrame water = {.decoration_count = 1, .decoration_visible_count = 1};
  water.decorations[0] = (ActionEffectInstance){
    .world_x = 736, .world_y = 592, .kind = kActionEffect_CaveWater,
    .phase = kActionEffectPhase_CaveEnvironment, .visual = 2,
    .flags = kActionEffectFlag_Visible, .phase_ticks = 32,
    .render_layer = kActionEffectRenderLayer_Bg2HighPlane,
    .projection_plane = kActionEffectProjectionPlane_Bg2High,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
  };
  CHECK(ActionSceneDecorationRender_Build(&water, kActionEffectRenderLayer_Bg2HighPlane,
      true, true, IdentityProjection, NULL, NULL, &first));
  /* A soft, finite splash around the fall's foot, not a collapsed glow mesh. */
  CHECK(SampleForestLight(&first, 736, 906, false) > .02f);
  CHECK(SampleForestLight(&first, 745, 914, false) == 0);
  ActionEffectInstance *e = &frame.decorations[0]; /* Tower surface light. */
  e->kind = kActionEffect_TowerWindowLight;
  e->visual = 4;
  e->phase_ticks = 0;
  e->render_layer = kActionEffectRenderLayer_ForegroundLight;
  e->world_x = 128;
  e->world_y = 0;
  e->geometry.data.rect = (ActionEffectLocalRect){-384,0,384,544};
  CHECK(ActionSceneDecorationRender_Build(&frame, e->render_layer, true, true,
      IdentityProjection, NULL, NULL, &first));
  CHECK(SampleForestLight(&first, 112, 90, true) < .02f);
  CHECK(SampleForestLight(&first, 130, 180, true) > .20f);
  CHECK(SampleForestLight(&first, 130, 240, true) == 0);
  e->flags |= kActionEffectFlag_ClipToRect;
  e->clip_rect = (ActionEffectLocalRect){-10,120,100,201};
  CHECK(ActionSceneDecorationRender_Build(&frame, e->render_layer, true, true,
      IdentityProjection, NULL, NULL, &again));
  CHECK(again.vertex_count > 0);
  for (float y = 120.1f; y < 201; y += 7.1f)
    for (float x = 118.1f; x < 228; x += 6.1f)
      CHECK(fabsf(SampleForestLight(&first, x, y, true) -
                  SampleForestLight(&again, x, y, true)) < .0005f);
  /* Broad surface light has no cone tip or hard edge. Cropping preserves
   * its interior RGB field rather than stretching it to fit the viewport. */
  e->kind = kActionEffect_CaveAmbientLight;
  e->visual = 3;
  e->world_x = 896;
  e->world_y = 1440;
  e->flags = kActionEffectFlag_Visible;
  e->render_layer = kActionEffectRenderLayer_ForegroundLight;
  CHECK(ActionSceneDecorationRender_Build(&frame, e->render_layer, true, true,
      IdentityProjection, NULL, NULL, &first));
  CHECK(SampleForestLight(&first, 856,1590,true) > .12f);
  CHECK(SampleForestLight(&first, 856,1590,true) < .26f);
  CHECK(fabsf(SampleForestLight(&first, 856,1590,true) -
              SampleForestLight(&first, 906,1590,true)) < .056f);
  e->flags |= kActionEffectFlag_ClipToRect;
  e->clip_rect = (ActionEffectLocalRect){-48,110,20,210};
  CHECK(ActionSceneDecorationRender_Build(&frame, e->render_layer, true, true,
      IdentityProjection, NULL, NULL, &again));
  CHECK(again.vertex_count > 0);
  for (float y = 1551.1f; y < 1649; y += 6.1f)
    for (float x = 849.1f; x < 915; x += 4.1f)
      CHECK(fabsf(SampleForestLight(&first,x,y,true) -
                  SampleForestLight(&again,x,y,true)) < .0005f);
}

static void TestCaveAmbientScroll(void) {
  ActionSceneEffectFrame frame = {.decoration_count = 1, .decoration_visible_count = 1};
  frame.decorations[0] = (ActionEffectInstance){
    .kind = kActionEffect_CaveAmbientLight, .phase = kActionEffectPhase_CaveEnvironment,
    .flags = kActionEffectFlag_Visible, .visual = 3, .phase_ticks = 512,
    .world_x = 828, .world_y = 1270,
    .render_layer = kActionEffectRenderLayer_ForegroundLight,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
  };
  ActionEffectProjectionContext context = {
    .bg1_camera_x = 700, .bg1_camera_y = 1430,
    .visible_width = 384, .snes_height = 224, .viewport = {0,0,384,224},
  };
  static ActionSceneEffectRenderBatch first, moved;
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_ForegroundLight,
      true, false, ActionEffectProjection_ProjectPoint, ActionEffectProjection_ClipBounds,
      &context, &first));
  /* The aggregate follows the camera, but its illumination must stay fixed
   * to the room on both axes, including partially clipped patches. */
  context.bg1_camera_x += 23;
  context.bg1_camera_y += 37;
  frame.decorations[0].world_x += 23;
  frame.decorations[0].world_y += 37;
  CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_ForegroundLight,
      true, false, ActionEffectProjection_ProjectPoint, ActionEffectProjection_ClipBounds,
      &context, &moved));
  float peak = 0;
  for (float y = 1.3f; y < 187; y += 7.1f)
    for (float x = 1.2f; x < 361; x += 9.3f) {
      const float expected = SampleForestLight(&first, x+23, y+37, true);
      peak = fmaxf(peak, expected);
      CHECK(fabsf(expected-SampleForestLight(&moved, x, y, true)) < .0005f);
    }
  CHECK(peak > .1f);
}

static void TestTempleGroundMist(void) {
  ActionSceneEffectFrame frame = {.decoration_count = 5, .decoration_visible_count = 5};
  const int surfaces[][3] = {
    {528,592,1664}, {592,656,1680}, {688,736,1680}, {768,832,1680}, {864,928,1664},
  };
  for (unsigned i = 0; i < 5; i++) {
    frame.decorations[i] = (ActionEffectInstance){
      .kind = kActionEffect_TempleGroundMist, .phase = kActionEffectPhase_CaveEnvironment,
      .flags = kActionEffectFlag_Visible, .visual = 3, .generation = i+1,
      .world_x = (int16_t)surfaces[i][0], .world_y = (int16_t)surfaces[i][2],
      .render_layer = kActionEffectRenderLayer_Bg1Mist,
      .projection_plane = kActionEffectProjectionPlane_Bg1,
      .geometry = {.kind = kActionEffectGeometry_Rect,
                   .data.rect = {0,-26,surfaces[i][1]-surfaces[i][0],0}},
    };
  }
  const uint8_t layer = kActionEffectRenderLayer_Bg1Mist;
  static ActionSceneEffectRenderBatch first, moved;
  for (unsigned t = 0; t < 1024; t += 31) {
    for (unsigned i = 0; i < 5; i++) frame.decorations[i].phase_ticks = (uint16_t)t;
    CHECK(ActionSceneDecorationRender_Build(&frame, layer, false, true,
        IdentityProjection, NULL, NULL, &first));
    CHECK(first.vertex_count > 0 && first.vertex_count <= 525);
    for (int i = 0; i < first.vertex_count; i++) {
      const ArRenderVertex2D *v = &first.vertices[i];
      bool supported = false;
      for (unsigned j = 0; j < 5; j++)
        if (v->position.x > surfaces[j][0] && v->position.x < surfaces[j][1]) {
          supported = true;
          CHECK(v->position.y >= surfaces[j][2]-26 && v->position.y <= surfaces[j][2]);
        }
      CHECK(supported);
      CHECK(v->color.a >= 0 && v->color.a < .42f);
    }
    for (unsigned i = 0; i < 5; i++) {
      const float x = (surfaces[i][0]+surfaces[i][1])*.5f, y = surfaces[i][2];
      CHECK(SampleForestLight(&first,x,y-.1f,false) > .09f); /* Base touches ground. */
      CHECK(SampleForestLight(&first,x,y+1,false) == 0); /* Never inside solid floor. */
      CHECK(SampleForestLight(&first,x,y-27,false) == 0);
    }
    /* A low floor is 16 pixels below the adjacent step. Pit centers stay clear. */
    CHECK(SampleForestLight(&first,624,1679.9f,false) > .09f);
    CHECK(SampleForestLight(&first,560,1679.9f,false) == 0);
    for (float y = 1638.3f; y < 1712; y += 2.1f) {
      const float gaps[] = {672,752,848,944};
      for (unsigned i = 0; i < sizeof(gaps)/sizeof(gaps[0]); i++)
        CHECK(SampleForestLight(&first, gaps[i], y, false) == 0);
    }
  }
  for (unsigned i = 0; i < 5; i++) {
    ActionEffectInstance *e = &frame.decorations[i];
    e->flags |= kActionEffectFlag_ClipToRect;
    e->clip_rect = (ActionEffectLocalRect){580-e->world_x,1655-e->world_y,
                                         810-e->world_x,1679-e->world_y};
  }
  CHECK(ActionSceneDecorationRender_Build(&frame, layer, false, true,
      IdentityProjection, NULL, NULL, &moved));
  CHECK(moved.vertex_count > 0);
  for (float y = 1655.1f; y < 1679; y += .9f)
    for (float x = 580.1f; x < 810; x += 3.3f)
      CHECK(fabsf(SampleForestLight(&first,x,y,false) -
                  SampleForestLight(&moved,x,y,false)) < .0005f);
  for (unsigned i = 0; i < 5; i++) frame.decorations[i].flags = kActionEffectFlag_Visible;
  ActionEffectProjectionContext context = {
    .bg1_camera_x = 500, .bg1_camera_y = 1440,
    .visible_width = 256, .snes_height = 224, .viewport = {0,0,256,224},
  };
  CHECK(ActionSceneDecorationRender_Build(&frame, layer, false, true,
      ActionEffectProjection_ProjectPoint, ActionEffectProjection_ClipBounds, &context, &first));
  context.bg1_camera_x += 13;
  context.bg1_camera_y += 19;
  CHECK(ActionSceneDecorationRender_Build(&frame, layer, false, true,
      ActionEffectProjection_ProjectPoint, ActionEffectProjection_ClipBounds, &context, &moved));
  float peak = 0;
  for (float y = 180.3f; y < 205; y += .9f)
    for (float x = 1.3f; x < 243; x += 3.1f) {
      const float expected = SampleForestLight(&first,x+13,y+19,false);
      peak = fmaxf(peak,expected);
      CHECK(fabsf(expected-SampleForestLight(&moved,x,y,false)) < .0005f);
    }
  CHECK(peak > .06f);
  context.bg1_camera_y = 600;
  CHECK(ActionSceneDecorationRender_Build(&frame, layer, false, true,
      ActionEffectProjection_ProjectPoint, ActionEffectProjection_ClipBounds, &context, &moved));
  CHECK(moved.vertex_count == 0); /* Never follow the viewport up the shaft. */
  for (unsigned i = 5; i <= kActionTempleMistMaxSpans; i++) {
    frame.decorations[i] = frame.decorations[0];
    frame.decorations[i].generation = i+1;
  }
  frame.decoration_count = kActionTempleMistMaxSpans;
  CHECK(ActionSceneDecorationRender_Build(&frame, layer, false, true,
      IdentityProjection, NULL, NULL, &first));
  CHECK(first.vertex_count <= kActionTempleMistMaxSpans*105);
  frame.decoration_count++;
  CHECK(!ActionSceneDecorationRender_Build(&frame, layer, false, true,
      IdentityProjection, NULL, NULL, &first));
  CHECK(!first.vertex_count && !first.index_count);
  /* Spike pockets settle on their own floor, 32px beneath the tips. */
  frame.decoration_count = 1;
  frame.decorations[0].world_x = 656;
  frame.decorations[0].world_y = 1712;
  frame.decorations[0].geometry.data.rect = (ActionEffectLocalRect){0,-26,32,0};
  frame.decorations[0].phase_ticks = 400;
  CHECK(ActionSceneDecorationRender_Build(&frame,layer,false,true,
      IdentityProjection,NULL,NULL,&first));
  CHECK(SampleForestLight(&first,672,1711,false) > .09f);
  CHECK(SampleForestLight(&first,672,1680,false) == 0);
  frame.decorations[0].phase_ticks++;
  CHECK(ActionSceneDecorationRender_Build(&frame,layer,false,true,
      IdentityProjection,NULL,NULL,&moved));
  CHECK(first.vertex_count == moved.vertex_count);
  for (int i = 0; i < first.vertex_count && i < moved.vertex_count; i++) {
    CHECK(fabsf(first.vertices[i].position.y-moved.vertices[i].position.y) < .2f);
    CHECK(fabsf(first.vertices[i].color.a-moved.vertices[i].color.a) < .02f);
    CHECK(first.vertices[i].color.b > first.vertices[i].color.r);
  }
  /* The first room's authored lower hall is wider than the climbing room. */
  frame.decorations[0].environment_room = 2;
  frame.decorations[0].geometry.data.rect.x1 = 880;
  CHECK(ActionSceneDecorationRender_Build(&frame,layer,false,true,
      IdentityProjection,NULL,NULL,&first));
  CHECK(first.vertex_count > 0);
  frame.decorations[0].geometry.data.rect.x1 = 896;
  CHECK(ActionSceneDecorationRender_Build(&frame,layer,false,true,
      IdentityProjection,NULL,NULL,&first));
  CHECK(!first.vertex_count && !first.index_count); /* Unproved spans are omitted. */
}

static void TestLandingParticleGeometry(void) {
  ActionSceneEffectFrame frame = {.decoration_count = kActionLandingDustMaxPuffs};
  static ActionSceneEffectRenderBatch batch, paused;
  for (unsigned i = 0; i < kActionLandingDustMaxPuffs; i++)
    frame.decorations[i] = (ActionEffectInstance){
      .kind = kActionEffect_LandingDust, .phase = kActionEffectPhase_CaveEnvironment,
      .flags = kActionEffectFlag_Visible, .visual = 3, .generation = i + 1,
      .world_x = 128, .world_y = 192, .projection_plane = kActionEffectProjectionPlane_Bg1,
      .render_layer = kActionEffectRenderLayer_WorldDust,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-104,-72,104,2}},
    };
  for (unsigned t = 0; t <= kActionLandingDustLifetime; t++) {
    for (unsigned i = 0; i < frame.decoration_count; i++) frame.decorations[i].phase_ticks = t;
    CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_WorldDust,
        false, true, IdentityProjection, NULL, NULL, &batch));
    CHECK(batch.vertex_count <= 6*28*4 && batch.index_count <= 6*28*6);
    for (int i = 0; i < batch.vertex_count; i++) {
      CHECK(batch.vertices[i].position.y <= 192.5f); /* Grains rise off the surface. */
      CHECK(batch.vertices[i].color.a >= 0 && batch.vertices[i].color.a <= 1);
      CHECK(isfinite(batch.vertices[i].position.x));
      CHECK(batch.vertices[i].position.x >= 24 && batch.vertices[i].position.x <= 232);
      CHECK(batch.vertices[i].position.y >= 120);
    }
    if (t == 12) {
      CHECK(batch.vertex_count > 0);
      CHECK(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_WorldDust,
          false, true, IdentityProjection, NULL, NULL, &paused));
      CHECK(SceneBatchesEqual(&batch, &paused));
    }
    if (t == kActionLandingDustLifetime) CHECK(!batch.vertex_count);
  }
  for (unsigned i = 0; i < frame.decoration_count; i++) frame.decorations[i].phase_ticks = 8;
  CHECK(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,
      false,true,IdentityProjection,NULL,NULL,&batch));
  for (unsigned i = 0; i < frame.decoration_count; i++) frame.decorations[i].phase_ticks = 12;
  CHECK(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,
      false,true,IdentityProjection,NULL,NULL,&paused));
  CHECK(batch.vertex_count == paused.vertex_count);
  for (int i = 0; i+3 < batch.vertex_count && i+3 < paused.vertex_count; i += 4) {
    const float width = batch.vertices[i+2].position.x-batch.vertices[i].position.x;
    const float later = paused.vertices[i+2].position.x-paused.vertices[i].position.x;
    CHECK(width > .8f && width <= 2.21f && fabsf(width-later) < .0001f);
  }
  frame.decorations[frame.decoration_count++] = frame.decorations[0];
  CHECK(!ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_WorldDust,
      false, true, IdentityProjection, NULL, NULL, &batch));
  CHECK(!batch.vertex_count && !batch.index_count);
}

static void TestCavePolishGeometry(void) {
  static ActionSceneEffectRenderBatch first, next;
  ActionSceneEffectFrame frame = {.decoration_count = 1};
  ActionEffectInstance *e = &frame.decorations[0];
  *e = (ActionEffectInstance){
    .kind = kActionEffect_CaveSheen, .phase = kActionEffectPhase_CaveEnvironment,
    .flags = kActionEffectFlag_Visible, .visual = 2, .source_mask = 2,
    .world_x = 326, .world_y = 440, .phase_ticks = 80,
    .render_layer = kActionEffectRenderLayer_Bg1Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
  };
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
      IdentityProjection,NULL,NULL,&first));
  CHECK(first.vertex_count > 0);
  float low = 10000, high = -10000;
  for (int i = 0; i < first.vertex_count; i++) {
    const ArRenderPointF p = first.vertices[i].position;
    CHECK(p.x >= 310 && p.x <= 343 && p.y >= 435 && p.y < 473);
    low = fminf(low,p.y);
    high = fmaxf(high,p.y);
  }
  CHECK(high-low > 8); /* The glint climbs the actual slope instead of floating flat. */
  e->source_mask = 0;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
      IdentityProjection,NULL,NULL,&next));
  CHECK(!next.vertex_count);
  *e = (ActionEffectInstance){
    .kind = kActionEffect_LandingDust, .phase = kActionEffectPhase_CaveEnvironment,
    .flags = kActionEffectFlag_Visible, .visual = 2, .generation = 1,
    .world_x = 128, .world_y = 192, .phase_ticks = 14,
    .render_layer = kActionEffectRenderLayer_WorldDust,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-104,-72,104,2}},
  };
  unsigned changed_counts = 0;
  for (unsigned generation = 1; generation <= 32; generation++) {
    e->generation = generation;
    CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,true,
        IdentityProjection,NULL,NULL,&next));
    CHECK(next.vertex_count > 0);
    if (generation > 1) {
      CHECK(!SceneBatchesEqual(&first,&next));
      if (first.vertex_count != next.vertex_count) ++changed_counts;
    }
    for (int i = 0; i < next.vertex_count; i++)
      CHECK(next.vertices[i].color.r > next.vertices[i].color.b);
    first = next;
  }
  CHECK(changed_counts > 8); /* Independent grains and lifetimes vary the burst density. */
  frame = (ActionSceneEffectFrame){.effect_count = 1, .visible_count = 1};
  frame.effects[0] = SceneEffect(kActionEffect_EnemyFireball,128);
  frame.effects[0].kind = kActionEffect_FillmoreStatueOrb;
  frame.effects[0].visual = 0x1B;
  CHECK(ActionSceneEffectRender_Build(&frame,true,false,IdentityProjection,NULL,&first));
  CHECK(first.vertex_count == 2*kActionEffectGlowVertices);
  CHECK(ActionSceneEffectRender_Build(&frame,false,true,IdentityProjection,NULL,&next));
  CHECK(next.vertex_count > 0);
  CHECK(ActionSceneEffectRender_Build(&frame,false,false,IdentityProjection,NULL,&next));
  CHECK(!next.vertex_count);
}

/* Simulate a different displayed BG1 transform while the moon stays on BG2. */
static bool MoonTestProjection(void *userdata, const ActionEffectInstance *effect,
    float x, float y, ArRenderPointF *point) {
  if (!IdentityProjection(NULL,effect,x,y,point)) return false;
  if (effect->projection_plane == kActionEffectProjectionPlane_Bg1)
    point->x += *(const float *)userdata;
  return true;
}

static bool SceneAlphaAt(const ActionSceneEffectRenderBatch *batch, float x, float y, float *alpha) {
  for (int i = 0; i < batch->index_count; i += 3) {
    const ArRenderVertex2D *a = &batch->vertices[batch->indices[i]];
    const ArRenderVertex2D *b = &batch->vertices[batch->indices[i+1]];
    const ArRenderVertex2D *c = &batch->vertices[batch->indices[i+2]];
    const float ax = a->position.x, ay = a->position.y;
    const float bx = b->position.x, by = b->position.y;
    const float cx = c->position.x, cy = c->position.y;
    const float denominator = (by-cy)*(ax-cx)+(cx-bx)*(ay-cy);
    if (fabsf(denominator) < .00001f) continue;
    const float u = ((by-cy)*(x-cx)+(cx-bx)*(y-cy))/denominator;
    const float v = ((cy-ay)*(x-cx)+(ax-cx)*(y-cy))/denominator;
    const float w = 1-u-v;
    if (u >= -.0001f && v >= -.0001f && w >= -.0001f) {
      *alpha = u*a->color.a+v*b->color.a+w*c->color.a;
      return true;
    }
  }
  *alpha = 0;
  return false;
}

static bool SceneAdditiveAlphaAt(const ActionSceneEffectRenderBatch *batch, float x, float y, float *alpha) {
  *alpha = 0;
  bool covered = false;
  for (int i = 0; i < batch->index_count; i += 3) {
    const ArRenderVertex2D *a = &batch->vertices[batch->indices[i]];
    const ArRenderVertex2D *b = &batch->vertices[batch->indices[i+1]];
    const ArRenderVertex2D *c = &batch->vertices[batch->indices[i+2]];
    const float ax = a->position.x, ay = a->position.y;
    const float bx = b->position.x, by = b->position.y;
    const float cx = c->position.x, cy = c->position.y;
    const float denominator = (by-cy)*(ax-cx)+(cx-bx)*(ay-cy);
    if (fabsf(denominator) < .00001f) continue;
    const float u = ((by-cy)*(x-cx)+(cx-bx)*(y-cy))/denominator;
    const float v = ((cy-ay)*(x-cx)+(ax-cx)*(y-cy))/denominator;
    const float w = 1-u-v;
    /* Probes avoid edges: sum interior fragments without counting a shared
     * mesh edge twice, unlike the tolerant single-hit helper above. */
    if (u > 0 && v > 0 && w > 0) {
      *alpha += u*a->color.a+v*b->color.a+w*c->color.a;
      covered = true;
    }
  }
  return covered;
}

static float MoonAlphaAt(const ActionSceneEffectRenderBatch *batch, float x, float y) {
  float alpha;
  CHECK(SceneAlphaAt(batch,x,y,&alpha));
  return alpha;
}

static void TestBloodpoolMoonlight(void) {
  ActionSceneEffectFrame frame = {.decoration_count = 1, .decoration_visible_count = 1};
  ActionEffectInstance *e = &frame.decorations[0];
  *e = (ActionEffectInstance){
    .kind = kActionEffect_BloodpoolMoonlight, .phase = kActionEffectPhase_BloodpoolEnvironment,
    .visual = 1, .world_x = 112, .world_y = 62,
    .flags = kActionEffectFlag_Visible | kActionEffectFlag_ClipToRect,
    .render_layer = kActionEffectRenderLayer_Bg2Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg2,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,194}},
    .clip_rect = {-190,18,240,194},
  };
  static ActionSceneEffectRenderBatch lit, repeat;
  frame.moonlight.valid = true;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&lit) && lit.vertex_count);
  CHECK(lit.vertex_count < 6000 && lit.index_count < 16000);
  for (int i = 0; i < lit.vertex_count; i++) {
    const ArRenderVertex2D *v = &lit.vertices[i];
    CHECK(isfinite(v->position.x) && isfinite(v->position.y));
    CHECK(v->position.x >= -78 && v->position.x <= 352);
    CHECK(v->position.y >= 80 && v->position.y <= 256);
    CHECK(v->color.a >= 0 && v->color.a <= 1);
  }
  e->phase_ticks = 16384;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&repeat) && SceneBatchesEqual(&lit,&repeat));
  const float clear = MoonAlphaAt(&lit,112,187.75f);
  const float clear_mid = MoonAlphaAt(&lit,112,142.25f);
  CHECK(clear > 0);
  frame.moonlight.count = 1;
  frame.moonlight.rectangles[0] = (ActionMoonlightOccluder){0,128,4096,136};
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&repeat));
  /* A ledge locally shadows the middle fan, while distant light survives
   * beneath it. It cannot extinguish the lower beam as an infinite 2D blocker. */
  const float solid_mid = MoonAlphaAt(&repeat,112,142.25f);
  CHECK(solid_mid < clear_mid*.94f && solid_mid > clear_mid*.2f);
  CHECK(MoonAlphaAt(&repeat,112,187.75f) > clear*.99f);
  CHECK(lit.vertex_count == repeat.vertex_count && lit.index_count == repeat.index_count);
  for (int i = 0; i < lit.vertex_count; i++)
    CHECK(!memcmp(&lit.vertices[i].position,&repeat.vertices[i].position,sizeof(ArRenderPointF)));
  /* A four-pixel opening passes part of the finite moon's disk, even though
   * its two edges lie in one sparse ray interval near the bottom of the fan. */
  frame.moonlight.count = 2;
  frame.moonlight.rectangles[0].x1 = 110;
  frame.moonlight.rectangles[1] = (ActionMoonlightOccluder){114,128,4096,136};
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&repeat));
  CHECK(MoonAlphaAt(&repeat,112,142.25f) > solid_mid+.01f);
  CHECK(MoonAlphaAt(&repeat,112,187.75f) > clear*.99f);
  /* Covering the visible moon with a small foreground post changes the view
   * of the source, not illumination throughout the haze behind the post. */
  frame.moonlight.count = 1;
  frame.moonlight.rectangles[0] = (ActionMoonlightOccluder){106,56,118,68};
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&repeat));
  CHECK(MoonAlphaAt(&repeat,112,142.25f) > clear_mid*.99f);
  CHECK(MoonAlphaAt(&repeat,112,187.75f) > clear*.99f);
  /* Subpixel scrolling changes coverage, never the mesh. Use a narrower
   * silhouette to check both gradual edge motion and BG1/BG2 independence. */
  frame.moonlight.count = 1;
  frame.moonlight.rectangles[0] = (ActionMoonlightOccluder){102,128,114,136};
  float previous = 0;
  for (int step = 0; step <= 200; step++) {
    const float offset = step*.1f;
    CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
        MoonTestProjection,NULL,(void *)&offset,&repeat));
    const float alpha = MoonAlphaAt(&repeat,112,142.25f)/clear_mid;
    if (step) CHECK(fabsf(alpha-previous) < .08f);
    else CHECK(alpha < .99f);
    if (step == 200) CHECK(alpha > .99f);
    previous = alpha;
    CHECK(lit.vertex_count == repeat.vertex_count);
    for (int i = 0; i < lit.vertex_count; i++)
      CHECK(!memcmp(&lit.vertices[i].position,&repeat.vertices[i].position,sizeof(ArRenderPointF)));
  }
  frame.moonlight.valid = false;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&repeat) && !repeat.vertex_count);
  frame.moonlight.valid = true;
  frame.moonlight.count = kActionMoonlightMaxOccluders+1;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&repeat) && !repeat.vertex_count);
  frame.moonlight.count = 1;
  frame.moonlight.rectangles[0].x1 = -1;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&repeat) && !repeat.vertex_count);
  /* Sweep clipping across both fan edges, including thin edge slivers. The
   * shared workspace also has to retain room for the reflection batch. */
  frame.moonlight.count = 0;
  for (int x = -384; x < 384; x += 47) {
    for (int y = 0; y < 194; y += 37) {
      e->clip_rect = (ActionEffectLocalRect){x,y,fminf(384,x+181),fminf(194,y+97)};
      CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
          IdentityProjection,NULL,NULL,&repeat));
      CHECK(repeat.vertex_count < kActionSceneEffectRenderMaxVertices-1400);
      CHECK(repeat.index_count < kActionSceneEffectRenderMaxIndices-3000);
    }
  }
  e->kind = kActionEffect_BloodpoolMoonReflection;
  e->render_layer = kActionEffectRenderLayer_Bg2Plane;
  e->projection_plane = kActionEffectProjectionPlane_Bg2;
  /* Cut through glints to exercise edge clipping. */
  e->clip_rect = (ActionEffectLocalRect){-17,90,31,180};
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,true,
      IdentityProjection,NULL,NULL,&lit) && lit.vertex_count);
  for (int i = 0; i < lit.vertex_count; i++) {
    CHECK(lit.vertices[i].position.x >= 95 && lit.vertices[i].position.x <= 143);
    CHECK(lit.vertices[i].position.y >= 152 && lit.vertices[i].position.y <= 242);
  }
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,false,
      IdentityProjection,NULL,NULL,&lit) && !lit.vertex_count);
  frame.decorations[1] = *e;
  frame.decoration_count = 2;
  CHECK(!ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
      IdentityProjection,NULL,NULL,&lit));
}

static void TestDistantWaveCapsFollowRaster(void) {
  ActionSceneEffectFrame frame = {.decoration_count = 1, .decoration_visible_count = 1};
  ActionEffectInstance *e = &frame.decorations[0];
  *e = (ActionEffectInstance){
    .kind = kActionEffect_BloodpoolMoonReflection, .phase = kActionEffectPhase_BloodpoolEnvironment,
    .environment_room = 1, .world_x = 112, .world_y = 62, .phase_ticks = 400,
    .flags = kActionEffectFlag_Visible, .render_layer = kActionEffectRenderLayer_Bg2Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg2,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,194}},
  };
  static ActionSceneEffectRenderBatch baseline, waves, changed;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,true,
      IdentityProjection,NULL,NULL,&baseline));
  frame.bloodpool.water_scroll_valid = true;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,true,
      IdentityProjection,NULL,NULL,&waves));
  const int wave_vertices = waves.vertex_count-baseline.vertex_count;
  CHECK(wave_vertices > 0 && wave_vertices <= 12*4*4*7);
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,true,
      IdentityProjection,NULL,NULL,&changed) && SceneBatchesEqual(&waves,&changed));
  /* Only the first water row advances; its caps move left by exactly seven
   * native pixels. Other rows stay put, independent of the effect clock. */
  frame.bloodpool.water_scroll[147-kActionBloodpoolWaterScrollFirstRow] = 7;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,true,
      IdentityProjection,NULL,NULL,&changed));
  const int changed_vertices = changed.vertex_count-baseline.vertex_count;
  unsigned checked = 0;
  for (int i = 0; i < wave_vertices; i++) {
    const ArRenderPointF p = waves.vertices[i].position;
    CHECK(p.y >= 147 && p.y <= 236); /* No glints in sky, hills or near water. */
    if (p.x < -150 || p.x > 350) continue;
    const float expected_x = p.x-(p.y <= 148 ? 7 : 0);
    bool found = false;
    for (int j = 0; j < changed_vertices; j++)
      if (fabsf(changed.vertices[j].position.x-expected_x) < .001f &&
          fabsf(changed.vertices[j].position.y-p.y) < .001f) found = true;
    CHECK(found);
    checked++;
  }
  CHECK(checked > 60);
  /* A native 256px wrap repeats the same CHR and cap positions exactly. */
  frame.bloodpool.water_scroll[147-kActionBloodpoolWaterScrollFirstRow] = 256;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,true,
      IdentityProjection,NULL,NULL,&changed) && SceneBatchesEqual(&waves,&changed));
  e->phase_ticks++;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,true,
      IdentityProjection,NULL,NULL,&changed));
  CHECK(!SceneBatchesEqual(&waves,&changed));
  for (int i = 0; i < wave_vertices; i++)
    CHECK(!memcmp(&waves.vertices[i].position,&changed.vertices[i].position,
                  sizeof(ArRenderPointF))); /* Shimmer cannot slide over the artwork. */
  /* Rays, the old reflection and the new caps share the existing batch. */
  frame.decorations[1] = *e;
  frame.decorations[1].kind = kActionEffect_BloodpoolMoonlight;
  frame.decoration_count = 2;
  frame.moonlight.valid = true;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
      IdentityProjection,NULL,NULL,&changed));
  CHECK(changed.vertex_count < kActionSceneEffectRenderMaxVertices);
}

static void TestMoonDependencyScope(void) {
  ActionSceneEffectFrame frame = {.decoration_count = 3, .decoration_visible_count = 3};
  frame.decorations[0] = (ActionEffectInstance){
    .kind = kActionEffect_CastleLight, .phase = kActionEffectPhase_CastleEnvironment,
    .environment_room = 8, .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_Bg1Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {0,0,256,256}},
  };
  frame.decorations[1] = (ActionEffectInstance){
    .kind = kActionEffect_BloodpoolMoonlight, .phase = kActionEffectPhase_BloodpoolEnvironment,
    .environment_room = 1, .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_Bg2Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg2,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,194}},
  };
  frame.decorations[2] = frame.decorations[1];
  static ActionSceneEffectRenderBatch batch;
  /* An unrelated stage pass does not consume or reject this duplicated moon. */
  CHECK(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_Bg1Plane,
      true,true,IdentityProjection,NULL,NULL,&batch));
  frame.decorations[0].kind = kActionEffect_BloodpoolWater;
  frame.decorations[0].phase = kActionEffectPhase_BloodpoolEnvironment;
  frame.decorations[0].environment_room = 1;
  frame.decorations[0].render_layer = kActionEffectRenderLayer_Bg1HighPlane;
  frame.decorations[0].projection_plane = kActionEffectProjectionPlane_Bg1High;
  CHECK(!ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_Bg1HighPlane,
      true,true,IdentityProjection,NULL,NULL,&batch));
  CHECK(!batch.vertex_count && !batch.index_count);
  frame.decorations[2].flags = 0;
  CHECK(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_Bg1HighPlane,
      true,true,IdentityProjection,NULL,NULL,&batch));
}

static void TestBloodpoolGeometry(void) {
  ActionSceneEffectFrame frame = {.decoration_count = 1, .decoration_visible_count = 1};
  static ActionSceneEffectRenderBatch first, again;
  const int spans[][2] = {{176,880},{960,1184},{1248,1360},{1440,2144},
                         {2240,2368},{2464,2560},{2688,2864},{2912,4096}};
  for (unsigned family = 0; family < 2; family++) {
    ActionEffectInstance *e = &frame.decorations[0];
    *e = (ActionEffectInstance){
      .kind = family ? kActionEffect_BloodpoolMist : kActionEffect_BloodpoolWater,
      .phase = kActionEffectPhase_BloodpoolEnvironment, .visual = 1, .source_mask = 0xFF,
      .world_y = 480, .flags = kActionEffectFlag_Visible,
      .render_layer = family ? kActionEffectRenderLayer_Bg2HighAlpha :
                               kActionEffectRenderLayer_Bg1HighPlane,
      .projection_plane = family ? kActionEffectProjectionPlane_Bg1 :
                                   kActionEffectProjectionPlane_Bg1High,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,-48,384,32}},
    };
    for (int x = -128; x <= 4224; x += 157) {
      e->world_x = (int16_t)x;
      e->phase_ticks = (uint16_t)(x*17);
      CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,true,
          IdentityProjection,NULL,NULL,&first));
      CHECK(first.vertex_count < 1500 && first.index_count < 5000);
      for (int v = 0; v < first.vertex_count; v++) {
        const ArRenderVertex2D *vertex = &first.vertices[v];
        const float px = vertex->position.x, py = vertex->position.y;
        CHECK(isfinite(px) && isfinite(py) && vertex->color.a >= 0 && vertex->color.a <= 1);
        CHECK(px >= x-384-.001f && px <= x+384+.001f);
        CHECK(py >= (family ? 440 : 488) && py <= (family ? 486 : 511));
        bool on_water = false;
        for (unsigned pool = 0; pool < 8; pool++)
          on_water |= px >= spans[pool][0]+6-.001f && px <= spans[pool][1]-6+.001f;
        CHECK(on_water); /* No drifting patches over dry gaps, even at camera edges. */
      }
      e->phase_ticks = (uint16_t)(e->phase_ticks+2048);
      CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,true,
          IdentityProjection,NULL,NULL,&again));
      CHECK(SceneBatchesEqual(&first,&again)); /* Paused/periodic clock, no new RNG. */
    }
    e->world_x = 512;
    CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,false,
        IdentityProjection,NULL,NULL,&again) && !again.vertex_count);
    e->source_mask = 0;
    CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,true,
        IdentityProjection,NULL,NULL,&again) && !again.vertex_count);
    e->source_mask = 0xFF;
    frame.decorations[1] = *e;
    frame.decoration_count = 2;
    CHECK(!ActionSceneDecorationRender_Build(&frame,e->render_layer,false,true,
        IdentityProjection,NULL,NULL,&again));
    frame.decoration_count = 1;
  }
}

static bool MoonWaterPerspective(void *userdata, const ActionEffectInstance *effect,
    float x, float y, ArRenderPointF *point) {
  if (!IdentityProjection(NULL,effect,x,y,point)) return false;
  if (effect->projection_plane == kActionEffectProjectionPlane_Bg1High)
    point->x += *(const float *)userdata;
  const float px = point->x, py = point->y;
  const float w = 1+.0005f*px+.0008f*py;
  *point = (ArRenderPointF){(px+.15f*py)/w,(py+.05f*px)/w};
  return true;
}

static void TestBloodpoolWaterMoonlight(void) {
  ActionSceneEffectFrame frame = {.decoration_count = 2, .decoration_visible_count = 2};
  frame.decorations[0] = (ActionEffectInstance){
    .kind = kActionEffect_BloodpoolWater, .phase = kActionEffectPhase_BloodpoolEnvironment,
    .visual = 1, .source_mask = 1, .world_x = 512, .world_y = 480,
    .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_Bg1HighPlane,
    .projection_plane = kActionEffectProjectionPlane_Bg1High,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,-48,384,32}},
  };
  frame.decorations[1] = (ActionEffectInstance){
    .kind = kActionEffect_BloodpoolMoonlight, .phase = kActionEffectPhase_BloodpoolEnvironment,
    .visual = 1, .world_x = 112, .world_y = 62, .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_Bg2Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg2,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,194}},
  };
  frame.moonlight.valid = true;
  static ActionSceneEffectRenderBatch clear, shadow;
  const uint8_t layer = kActionEffectRenderLayer_Bg1HighPlane;
  CHECK(ActionSceneDecorationRender_Build(&frame,layer,true,false,
      IdentityProjection,NULL,NULL,&clear) && clear.vertex_count);
  /* The low ray at slope .35 must actually land on the foreground water. */
  const float x = 112+.35f*(499-62);
  const float bright = MoonAlphaAt(&clear,x,499);
  CHECK(bright > .25f);
  CHECK(MoonAlphaAt(&clear,112+.7f*(499-62),499) < bright*.2f);
  for (int i = 0; i < clear.vertex_count; i++) {
    const ArRenderVertex2D *v = &clear.vertices[i];
    CHECK(v->position.x >= 182 && v->position.x <= 874);
    CHECK(v->position.y >= 488 && v->position.y <= 511);
    CHECK(isfinite(v->color.a) && v->color.a >= 0 && v->color.a <= 1);
  }
  frame.moonlight.count = 1;
  frame.moonlight.rectangles[0] = (ActionMoonlightOccluder){0,420,4096,479};
  CHECK(ActionSceneDecorationRender_Build(&frame,layer,true,false,
      IdentityProjection,NULL,NULL,&shadow));
  CHECK(MoonAlphaAt(&shadow,x,499) < bright*.01f);
  /* A gap in the actual opaque silhouette admits light onto the water. */
  frame.moonlight.count = 2;
  frame.moonlight.rectangles[0].x1 = 244;
  frame.moonlight.rectangles[1] = (ActionMoonlightOccluder){250,420,4096,479};
  CHECK(ActionSceneDecorationRender_Build(&frame,layer,true,false,
      IdentityProjection,NULL,NULL,&shadow));
  CHECK(MoonAlphaAt(&shadow,x,499) > bright*.5f);
  CHECK(clear.vertex_count == shadow.vertex_count && clear.index_count == shadow.index_count);
  for (int i = 0; i < clear.vertex_count; i++)
    CHECK(!memcmp(&clear.vertices[i].position,&shadow.vertices[i].position,sizeof(ArRenderPointF)));
  frame.moonlight.valid = false;
  CHECK(ActionSceneDecorationRender_Build(&frame,layer,true,false,
      IdentityProjection,NULL,NULL,&shadow) && !shadow.vertex_count);
  frame.moonlight.valid = true;
  frame.moonlight.count = 0;
  frame.decorations[1].phase_ticks = 16384;
  CHECK(ActionSceneDecorationRender_Build(&frame,layer,true,false,
      IdentityProjection,NULL,NULL,&shadow) && SceneBatchesEqual(&clear,&shadow));
  /* A perspective camera and independent foreground parallax must preserve
   * ray/receiver alignment. The illuminated world point moves with BG1-high. */
  const float foreground_offset = 40;
  ArRenderPointF projected;
  CHECK(MoonWaterPerspective((void *)&foreground_offset,&frame.decorations[0],
      x-foreground_offset-512,19,&projected));
  CHECK(ActionSceneDecorationRender_Build(&frame,layer,true,false,
      MoonWaterPerspective,NULL,(void *)&foreground_offset,&shadow));
  CHECK(fabsf(MoonAlphaAt(&shadow,projected.x,projected.y)-bright) < .015f);
  frame.decorations[1].flags = 0;
  CHECK(ActionSceneDecorationRender_Build(&frame,layer,true,false,
      IdentityProjection,NULL,NULL,&shadow) && !shadow.vertex_count);
  frame.decorations[1].flags = kActionEffectFlag_Visible;
  CHECK(ActionSceneDecorationRender_Build(&frame,layer,false,false,
      IdentityProjection,NULL,NULL,&shadow) && !shadow.vertex_count);
}

static unsigned BloodpoolRipplePoints(const ActionSceneEffectRenderBatch *batch,
    ArRenderPointF *points, unsigned capacity) {
  unsigned count = 0;
  for (int i = 0; i < batch->vertex_count; i++) {
    const ArRenderVertex2D *v = &batch->vertices[i];
    if (fabsf(v->color.r-.85f) > .0001f || fabsf(v->color.g-.34f) > .0001f ||
        fabsf(v->color.b-.43f) > .0001f) continue;
    CHECK(count < capacity);
    if (count < capacity) points[count] = v->position;
    count++;
  }
  return count;
}

static void TestBloodpoolMarshDetails(void) {
  ActionSceneEffectFrame frame = {.decoration_count = 2, .decoration_visible_count = 2};
  ActionEffectInstance *detail = &frame.decorations[0], *moon = &frame.decorations[1];
  *detail = (ActionEffectInstance){
    .phase = kActionEffectPhase_BloodpoolEnvironment, .visual = 1, .source_mask = 1,
    .world_x = 512, .flags = kActionEffectFlag_Visible,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,512}},
  };
  *moon = (ActionEffectInstance){
    .kind = kActionEffect_BloodpoolMoonlight, .phase = kActionEffectPhase_BloodpoolEnvironment,
    .visual = 1, .world_x = 112, .world_y = 62, .flags = kActionEffectFlag_Visible,
    .render_layer = kActionEffectRenderLayer_Bg2Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg2,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,194}},
  };
  frame.moonlight.valid = true;
  frame.bloodpool = (ActionBloodpoolDetails){.valid = true, .timber_count = 1, .post_count = 1,
    .timber = {{.x0=208,.x1=224,.y=352,.drip_x=222,.drip_y=360,.landing_y=400}},
    .posts = {{.x0=278,.x1=283,.y=485}},
  };
  static ActionSceneEffectRenderBatch first, repeat;
  detail->kind = kActionEffect_BloodpoolTimber;
  detail->render_layer = kActionEffectRenderLayer_Bg1Plane;
  CHECK(ActionSceneDecorationRender_Build(&frame,detail->render_layer,true,false,
      IdentityProjection,NULL,NULL,&first) && first.vertex_count);
  float brightest = 0;
  for (int i = 0; i < first.vertex_count; i++) {
    CHECK(first.vertices[i].position.y >= 352 && first.vertices[i].position.y <= 353);
    CHECK(first.vertices[i].position.x >= 208 && first.vertices[i].position.x <= 224);
    brightest = fmaxf(brightest,first.vertices[i].color.a);
  }
  CHECK(brightest > .03f);
  detail->kind = kActionEffect_BloodpoolAir;
  detail->render_layer = kActionEffectRenderLayer_Bg2HighAlpha;
  unsigned visible_drips = 0;
  for (unsigned tick = 0; tick < 1024; tick += 7) {
    detail->phase_ticks = (uint16_t)tick;
    CHECK(ActionSceneDecorationRender_Build(&frame,detail->render_layer,false,true,
        IdentityProjection,NULL,NULL,&first) && first.vertex_count);
    for (int i = 0; i < first.vertex_count; i++) {
      const ArRenderVertex2D *v = &first.vertices[i];
      if (fabsf(v->color.g-.65f) < .0001f) {
        visible_drips++;
        CHECK(v->position.y <= 400); /* Never fall through the lower platform. */
      }
    }
    detail->phase_ticks = (uint16_t)(tick+16384);
    CHECK(ActionSceneDecorationRender_Build(&frame,detail->render_layer,false,true,
        IdentityProjection,NULL,NULL,&repeat) && SceneBatchesEqual(&first,&repeat));
  }
  CHECK(visible_drips > 0);
  detail->kind = kActionEffect_BloodpoolWater;
  detail->render_layer = kActionEffectRenderLayer_Bg1HighPlane;
  detail->projection_plane = kActionEffectProjectionPlane_Bg1High;
  detail->world_y = 480;
  detail->geometry.data.rect = (ActionEffectLocalRect){-384,-48,384,32};
  bool saw_ripple = false;
  for (unsigned tick = 0; tick < 256; tick += 13) {
    detail->phase_ticks = (uint16_t)tick;
    frame.bloodpool.valid = false;
    CHECK(ActionSceneDecorationRender_Build(&frame,detail->render_layer,false,true,
        IdentityProjection,NULL,NULL,&first));
    frame.bloodpool.valid = true;
    CHECK(ActionSceneDecorationRender_Build(&frame,detail->render_layer,false,true,
        IdentityProjection,NULL,NULL,&repeat));
    saw_ripple |= repeat.vertex_count > first.vertex_count;
    for (int i = 0; i < repeat.vertex_count; i++) {
      const ArRenderVertex2D *v = &repeat.vertices[i];
      CHECK(v->position.x >= 182 && v->position.x <= 874);
      CHECK(v->position.y >= 480 && v->position.y <= 511);
      CHECK(v->color.r > v->color.b); /* Red lake, including reflected light. */
    }
  }
  CHECK(saw_ripple);
  /* Both native post depths must be used, including shallow contacts above
   * the full-water row. Moving the capture anchor preserves world placement. */
  frame.bloodpool.timber_count = 0;
  ArRenderPointF anchored[96], moved[96];
  unsigned ripple_count = 0;
  for (unsigned tick = 0; tick < 256; tick++) {
    detail->phase_ticks = (uint16_t)tick;
    CHECK(ActionSceneDecorationRender_Build(&frame,detail->render_layer,false,true,
        IdentityProjection,NULL,NULL,&repeat));
    ripple_count = BloodpoolRipplePoints(&repeat,anchored,96);
    if (ripple_count > 0) break;
  }
  CHECK(ripple_count > 0);
  for (unsigned contact = 0; contact < 2; contact++) {
    frame.bloodpool.posts[0].y = contact ? 492 : 485;
    detail->world_x = 560;
    CHECK(ActionSceneDecorationRender_Build(&frame,detail->render_layer,false,true,
        IdentityProjection,NULL,NULL,&repeat));
    CHECK(BloodpoolRipplePoints(&repeat,moved,96) == ripple_count);
    float left = 4096, right = 0, top = 512, bottom = 0;
    for (unsigned i = 0; i < ripple_count && i < 96; i++) {
      const ArRenderPointF p = moved[i], original = anchored[i];
      CHECK(fabsf(p.x-original.x) < .0001f);
      CHECK(fabsf(p.y-original.y-(contact ? 7 : 0)) < .0001f);
      left = fminf(left,p.x);
      right = fmaxf(right,p.x);
      top = fminf(top,p.y);
      bottom = fmaxf(bottom,p.y);
    }
    CHECK(fabsf((left+right)*.5f-280.5f) < .0001f);
    CHECK(fabsf((top+bottom)*.5f-frame.bloodpool.posts[0].y) < .0001f);
  }
  /* Ripple vertices follow the same perspective/parallax transform as their
   * BG1-high post; BG2's independently moving moon cannot move the contact. */
  frame.bloodpool.posts[0].y = 485;
  const float foreground_offset = 40;
  CHECK(ActionSceneDecorationRender_Build(&frame,detail->render_layer,false,true,
      MoonWaterPerspective,NULL,(void *)&foreground_offset,&repeat));
  CHECK(BloodpoolRipplePoints(&repeat,moved,96) == ripple_count);
  for (unsigned i = 0; i < ripple_count && i < 96; i++) {
    const ArRenderPointF original = anchored[i], actual = moved[i];
    ArRenderPointF expected;
    CHECK(MoonWaterPerspective((void *)&foreground_offset,detail,
        original.x-detail->world_x,original.y-detail->world_y,&expected));
    CHECK(fabsf(actual.x-expected.x) < .0001f && fabsf(actual.y-expected.y) < .0001f);
  }
  detail->kind = kActionEffect_BloodpoolCloud;
  detail->render_layer = kActionEffectRenderLayer_Bg2Alpha;
  detail->projection_plane = kActionEffectProjectionPlane_Bg2;
  detail->world_x = 112;
  detail->world_y = 62;
  detail->geometry.data.rect = (ActionEffectLocalRect){-100,-40,100,44};
  float previous = 0;
  for (unsigned tick = 0; tick < 16384; tick += 16) {
    detail->phase_ticks = (uint16_t)tick;
    CHECK(ActionSceneDecorationRender_Build(&frame,detail->render_layer,true,false,
        IdentityProjection,NULL,NULL,&first));
    const float alpha = MoonAlphaAt(&first,112,62);
    CHECK(alpha > .025f && alpha < .15f);
    if (tick) CHECK(fabsf(alpha-previous) < .002f); /* Slow coordinated veil, no flicker. */
    previous = alpha;
  }
  frame.bloodpool.valid = false;
  detail->kind = kActionEffect_BloodpoolAir;
  detail->render_layer = kActionEffectRenderLayer_Bg2HighAlpha;
  detail->projection_plane = kActionEffectProjectionPlane_Bg1;
  CHECK(ActionSceneDecorationRender_Build(&frame,detail->render_layer,false,true,
      IdentityProjection,NULL,NULL,&first) && !first.vertex_count);
}

static bool CastleTestClip(void *userdata, const ActionEffectInstance *effect,
    ActionEffectLocalRect *clip) {
  (void)effect;
  *clip = *(ActionEffectLocalRect *)userdata;
  return true;
}

/* Count local highlights above nearby samples, independently of mesh layout.
 * Smooth window washes should not develop the former comb of fine strands. */
static unsigned CastleLightPeakCount(const ActionSceneEffectRenderBatch *batch,
    float left, float right, float y) {
  unsigned peaks = 0;
  float older = 0, previous = 0;
  for (float x = left; x <= right; x += .125f) {
    float alpha;
    SceneAdditiveAlphaAt(batch,x,y,&alpha);
    if (previous > older && previous >= alpha) {
      float before, after;
      SceneAdditiveAlphaAt(batch,x-.875f,y,&before);
      SceneAdditiveAlphaAt(batch,x+.625f,y,&after);
      if (previous-fmaxf(before,after) > .004f) peaks++;
    }
    older = previous;
    previous = alpha;
  }
  return peaks;
}

static void TestCastleStackedRays(void) {
  static ActionSceneEffectRenderBatch joined, clipped, single;
  ActionSceneEffectFrame frame = {.decoration_count = 1, .decoration_visible_count = 1};
  ActionEffectInstance *e = &frame.decorations[0];
  *e = (ActionEffectInstance){
    .kind = kActionEffect_CastleLight, .phase = kActionEffectPhase_CastleEnvironment,
    .phase_ticks = 987, .flags = kActionEffectFlag_Visible|kActionEffectFlag_ClipToRect,
    .render_layer = kActionEffectRenderLayer_Bg1Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {0,0,1792,1024}},
    .clip_rect = {0,0,1792,1024},
  };
  /* Native shaft, closely spaced narrow windows, and the long narrow fan
   * that previously continued across the next opening. */
  static const struct { unsigned room, mask; float x, top, bottom; } stacks[] = {
    {3,(1u<<2)|(1u<<3),640,236,290},
    {5,(1u<<0)|(1u<<1),152,344,370},
    {5,(1u<<3)|(1u<<4),776,376,418},
  };
  for (unsigned s = 0; s < sizeof(stacks)/sizeof(stacks[0]); s++) {
    e->visual = (uint16_t)stacks[s].room;
    e->source_mask = (uint16_t)stacks[s].mask;
    CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
        IdentityProjection,NULL,NULL,&joined));
    float previous = 0;
    unsigned lit_rows = 0, sampled_rows = 0;
    for (float y = stacks[s].top+1.173f; y < stacks[s].bottom; y += 1) {
      float peak = 0;
      for (float x = stacks[s].x-63.831f; x <= stacks[s].x+64; x += .25f) {
        float alpha;
        SceneAdditiveAlphaAt(&joined,x,y,&alpha);
        peak = fmaxf(peak,alpha);
      }
      CHECK(peak >= 0 && peak < .6f); /* No additive hot seam. */
      sampled_rows++;
      if (peak > .008f) lit_rows++;
      /* The last few pixels fade into the arch itself; check the transition
       * between the two fans independently of that intentional root fade. */
      if (previous && y < stacks[s].bottom-3) CHECK(fabsf(peak-previous) < .10f);
      previous = peak;
    }
    CHECK(lit_rows*5 >= sampled_rows*4); /* Shorter washes retain most of the join. */
    const float middle = (stacks[s].top+stacks[s].bottom)*.5f+.137f;
    const ActionEffectLocalRect clip = {stacks[s].x-48,middle-3,stacks[s].x+48,middle+3};
    CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
        IdentityProjection,CastleTestClip,(void *)&clip,&clipped));
    for (float x = clip.x0+1.137f; x < clip.x1; x += .5f) {
      float full_alpha, clipped_alpha;
      SceneAdditiveAlphaAt(&joined,x,middle+.3f,&full_alpha);
      SceneAdditiveAlphaAt(&clipped,x,middle+.3f,&clipped_alpha);
      /* Subpixel fibers amplify float intersection rounding at world X~800.
       * Require agreement within 1/16 of one 8-bit alpha step. */
      CHECK(fabsf(full_alpha-clipped_alpha) < 1.0f/4096);
    }
  }
  /* The connected pair stops before the next glass. Only the faint opening
   * glow is allowed there; the lower fan must not form a curtain across it. */
  for (float x = 712.137f; x <= 840; x += .5f) {
    float alpha;
    SceneAdditiveAlphaAt(&joined,x,438,&alpha);
    CHECK(alpha <= .0651f);
    if (fabsf(x-776) > 4) CHECK(alpha == 0);
  }
  e->source_mask = 1u<<3;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&single));
  float isolated_peak = 0;
  for (float x = 712.137f; x <= 840; x += .5f) {
    float alpha;
    SceneAdditiveAlphaAt(&single,x,438,&alpha);
    isolated_peak = fmaxf(isolated_peak,alpha);
  }
  CHECK(isolated_peak > .02f);
}

static void TestCastleWaterGeometry(void) {
  static ActionSceneEffectRenderBatch batch, repeat;
  ActionSceneEffectFrame frame = {.decoration_count = 1, .decoration_visible_count = 1};
  ActionEffectInstance *e = &frame.decorations[0];
  *e = (ActionEffectInstance){.kind = kActionEffect_CastleWater,
    .phase = kActionEffectPhase_CastleEnvironment, .visual = 5, .source_mask = 255,
    .world_y = 944, .phase_ticks = 123, .flags = kActionEffectFlag_Visible|kActionEffectFlag_ClipToRect,
    .render_layer = kActionEffectRenderLayer_Bg1Plane, .projection_plane = kActionEffectProjectionPlane_Bg1,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {592,0,1744,16}},
    .clip_rect = {592,0,1744,16},
  };
  for (unsigned phase = 0; phase < 512; phase += 17) {
    e->phase_ticks = (uint16_t)phase;
    CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
        IdentityProjection,NULL,NULL,&batch));
    CHECK(batch.index_count > 0);
    for (int i = 0; i < batch.vertex_count; i++) {
      CHECK(batch.vertices[i].position.x >= 592 && batch.vertices[i].position.x <= 1744);
      CHECK(batch.vertices[i].position.y >= 944 && batch.vertices[i].position.y <= 960);
    }
  }
  e->phase_ticks += 512;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
      IdentityProjection,NULL,NULL,&repeat));
  CHECK(SceneBatchesEqual(&batch,&repeat));
  /* Camera-local negative coordinates must not wrap through unsigned strip
   * arithmetic and silently discard the water to the left of the camera. */
  e->world_x = 1200;
  e->geometry.data.rect = e->clip_rect = (ActionEffectLocalRect){-608,0,544,16};
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
      IdentityProjection,NULL,NULL,&repeat));
  CHECK(batch.vertex_count == repeat.vertex_count && batch.index_count == repeat.index_count);
  for (int i = 0; i < batch.vertex_count && i < repeat.vertex_count; i++) {
    CHECK(fabsf(batch.vertices[i].position.x-repeat.vertices[i].position.x) < 1.0f/2048);
    CHECK(fabsf(batch.vertices[i].position.y-repeat.vertices[i].position.y) < 1.0f/2048);
    CHECK(fabsf(batch.vertices[i].color.a-repeat.vertices[i].color.a) < .0001f);
  }
  e->source_mask = 1;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
      IdentityProjection,NULL,NULL,&batch));
  for (int i = 0; i < batch.vertex_count; i++) CHECK(batch.vertices[i].position.x <= 736);
  e->source_mask = 0;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
      IdentityProjection,NULL,NULL,&batch));
  CHECK(batch.index_count == 0);
  /* Water joins the complete interior light field and three native torches
   * in the same scratch/batch, without increasing either capacity. */
  e->source_mask = 255;
  frame.decoration_count = frame.decoration_visible_count = 5;
  frame.decorations[1] = *e;
  ActionEffectInstance *light = &frame.decorations[1];
  light->kind = kActionEffect_CastleLight;
  light->world_x = light->world_y = 0;
  light->geometry.data.rect = light->clip_rect = (ActionEffectLocalRect){0,0,1792,1024};
  for (unsigned i = 2; i < 5; i++) {
    frame.decorations[i] = SceneEffect(kActionEffect_WallTorch, 500+i*40);
    frame.decorations[i].render_layer = kActionEffectRenderLayer_Bg1Plane;
    frame.decorations[i].projection_plane = kActionEffectProjectionPlane_Bg1;
  }
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
      IdentityProjection,NULL,NULL,&batch));
}

static void TestCastleGeometry(void) {
  static ActionSceneEffectRenderBatch batch, again;
  ActionSceneEffectFrame frame = {0};
  frame.decoration_count = frame.decoration_visible_count = 1;
  ActionEffectInstance *e = &frame.decorations[0];
  *e = (ActionEffectInstance){
    .kind = kActionEffect_CastleLight, .phase = kActionEffectPhase_CastleEnvironment,
    .visual = 8, .source_mask = 1, .phase_ticks = 987,
    .flags = kActionEffectFlag_Visible|kActionEffectFlag_ClipToRect,
    .render_layer = kActionEffectRenderLayer_Bg1Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {0,0,256,256}},
    .clip_rect = {0,0,256,256},
  };
  ActionEffectLocalRect clip = {100,100,170,220};
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
      IdentityProjection,CastleTestClip,&clip,&batch));
  CHECK(batch.vertex_count > 0);
  for (int i = 0; i < batch.vertex_count; i++) {
    const ArRenderVertex2D *v = &batch.vertices[i];
    CHECK(v->position.x >= clip.x0-.001f && v->position.x <= clip.x1+.001f);
    CHECK(v->position.y >= clip.y0-.001f && v->position.y <= clip.y1+.001f);
    CHECK(isfinite(v->color.a) && v->color.a >= 0 && v->color.a <= 1);
  }
  e->phase_ticks += 4096;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
      IdentityProjection,CastleTestClip,&clip,&again));
  CHECK(SceneBatchesEqual(&batch,&again));
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,true,
      IdentityProjection,NULL,NULL,&batch));
  CHECK(batch.index_count > 0); /* Dust remains independently renderable. */
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,true,
      IdentityProjection,NULL,NULL,&again) && SceneBatchesEqual(&batch,&again));
  e->phase_ticks += 30;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,false,true,
      IdentityProjection,NULL,NULL,&again));
  CHECK(batch.vertex_count == again.vertex_count);
  float travel = 0;
  for (int i = 0; i < batch.vertex_count && i < again.vertex_count; i++)
    travel = fmaxf(travel,hypotf(batch.vertices[i].position.x-again.vertices[i].position.x,
                               batch.vertices[i].position.y-again.vertices[i].position.y));
  CHECK(travel > 1); /* Clearly moves over half a second, rather than 0.12px orbits. */
  e->phase_ticks -= 30;

  e->source_mask = 0;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
      IdentityProjection,NULL,NULL,&batch));
  CHECK(batch.index_count == 0);
  e->source_mask = 1;
  /* Separate arch/sill scattering must not recreate a curtain through the
   * boss opening. The short upper flare is narrower than the lower spill. */
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&batch));
  float upper_width = 0, lower_width = 0;
  unsigned above_arch = 0, below_sill = 0;
  for (int i = 0; i < batch.vertex_count; i++) {
    const ArRenderVertex2D *v = &batch.vertices[i];
    if (v->position.y > 64 && v->position.y < 166) {
      CHECK(fabsf(v->position.x-128) <= 32);
      CHECK(v->color.a <= .0851f); /* Faint opening, no strong shaft in the glass. */
    }
    if (v->position.y < 38) {
      above_arch++;
      upper_width = fmaxf(upper_width,fabsf(v->position.x-128));
    }
    if (v->position.y > 172) {
      below_sill++;
      lower_width = fmaxf(lower_width,fabsf(v->position.x-128));
    }
  }
  CHECK(above_arch && below_sill && lower_width > upper_width*1.5f);
  /* Tip vertices are transparent: sample the upper bounce's interior to
   * require visible arch light, rather than merely nonempty geometry. */
  float arch_peak = 0;
  for (float x = 90.137f; x < 166; x += .125f) {
    float alpha;
    SceneAdditiveAlphaAt(&batch,x,30.173f,&alpha);
    arch_peak = fmaxf(arch_peak,alpha);
  }
  CHECK(arch_peak > .02f);
  /* The boss has broad concentrations rather than a comb of fine strands.
   * Their falloff must remain smooth farther from the sill. */
  const unsigned near_peaks = CastleLightPeakCount(&batch,32.137f,224,182.173f);
  CHECK(near_peaks <= 4);
  float centre, left, right, gap_left, gap_right;
  SceneAdditiveAlphaAt(&batch,128.137f,182.173f,&centre);
  SceneAdditiveAlphaAt(&batch,107.137f,182.173f,&left);
  SceneAdditiveAlphaAt(&batch,149.137f,182.173f,&right);
  SceneAdditiveAlphaAt(&batch,117.137f,182.173f,&gap_left);
  SceneAdditiveAlphaAt(&batch,138.137f,182.173f,&gap_right);
  CHECK(centre > .2f && left > .04f && right > .04f);
  CHECK(gap_left < left*.8f && gap_right < right*.8f);
  CHECK(CastleLightPeakCount(&batch,32.137f,224,209.173f) <= 1);
  float coverage = 1, highlight = 0;
  for (float x = 100.137f; x < 150; x += .125f) {
    float alpha;
    SceneAdditiveAlphaAt(&batch,x,190.173f,&alpha);
    coverage = fminf(coverage,alpha);
    highlight = fmaxf(highlight,alpha);
  }
  CHECK(coverage > .015f && highlight-coverage > .06f);
  /* Independent coordinates measured at the first row below native window
   * frame artwork, including both openings of the paired narrow window. */
  static const struct {
    unsigned room, source;
    float x, y, radius, arch_y, arch_radius, arch_min, arch_max;
  } sills[] = {
    {3,0,168,552,4,514,4,0,5},{3,1,184,552,4,514,4,0,5},
    {3,2,640,236,16,178,16,-2,8},{3,3,640,348,16,290,16,-2,8},
    {3,4,640,460,16,402,16,-2,8},{3,5,640,572,16,514,16,-2,8},
    {3,6,640,684,16,626,16,-2,8},{3,7,640,796,16,738,16,-2,8},
    {5,0,152,344,4,306,4,0,5},{5,1,152,408,4,370,4,0,5},
    {5,2,152,472,4,434,4,0,5},{5,3,776,376,4,338,4,0,5},
    {5,4,776,456,4,418,4,0,5},
    {7,0,424,204,16,114,16,-2,8},{7,1,584,204,16,114,16,-2,8},
    {7,2,744,204,16,114,16,-2,8},{8,0,128,172,32,38,32,-6,20},
    {7,3,384,204,16,114,16,-2,8},{7,4,464,204,16,114,16,-2,8},
    {7,5,504,204,16,114,16,-2,8},{7,6,544,204,16,114,16,-2,8},
    {7,7,624,204,16,114,16,-2,8},{7,8,664,204,16,114,16,-2,8},
    {7,9,704,204,16,114,16,-2,8},{7,10,784,204,16,114,16,-2,8},
  };
  e->geometry.data.rect = e->clip_rect = (ActionEffectLocalRect){0,0,1792,1024};
  for (unsigned s = 0; s < sizeof(sills)/sizeof(sills[0]); s++) {
    e->visual = (uint16_t)sills[s].room;
    e->source_mask = (uint16_t)(1u<<sills[s].source);
    CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
        IdentityProjection,NULL,NULL,&batch));
    unsigned roots = 0;
    for (int i = 0; i < batch.vertex_count; i++) {
      const ArRenderVertex2D *v = &batch.vertices[i];
      if (fabsf(v->position.y-sills[s].y) > .001f) continue;
      CHECK(fabsf(v->position.x-sills[s].x) <= sills[s].radius);
      if (v->color.a > .03f) roots++;
    }
    CHECK(roots > 0);
    /* Upper bounce remains close to each measured native arch, including
     * the stepped boss crown. Soft fields no longer have twenty bright roots. */
    unsigned arch_light = 0;
    for (int i = 0; i < batch.vertex_count; i++) {
      const ArRenderVertex2D *v = &batch.vertices[i];
      if (v->color.a > .005f && v->position.y < sills[s].arch_y &&
          fabsf(v->position.x-sills[s].x) < sills[s].arch_radius)
        arch_light++;
    }
    CHECK(arch_light > 0);
    /* The arch flare must not create a bright intrusion into the glass.
     * Its new opening glow stays faint at these native-art landmarks. */
    static const float pointed_probe[][2] = {{0,2},{2,5}};
    static const float broad_probe[][2] = {{0,2},{4,1},{8,5},{12,9}};
    static const float boss_probe[][2] = {{0,-2},{4,-2},{8,0},{12,5},{18,14},{24,18},{28,23}};
    const float (*probes)[2] = sills[s].room == 8 ? boss_probe :
        sills[s].arch_radius == 4 ? pointed_probe : broad_probe;
    const unsigned probe_count = sills[s].room == 8 ? 7 : sills[s].arch_radius == 4 ? 2 : 4;
    for (unsigned probe = 0; probe < probe_count; probe++) for (int side = -1; side <= 1; side += 2) {
      float alpha;
      SceneAdditiveAlphaAt(&batch,sills[s].x+side*(probes[probe][0]+.137f),
          sills[s].arch_y+probes[probe][1]+.173f,&alpha);
      CHECK(alpha < .04f);
    }

  }
  /* The accepted soft wash must fan symmetrically from the sill, without
   * an authored lean or interpolation bias. */
  e->visual = 5;
  e->source_mask = 1u<<4;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&batch));
  for (float offset = 2.137f; offset < 24; offset += 2) {
    float left, right;
    SceneAdditiveAlphaAt(&batch,776-offset,496.173f,&left);
    SceneAdditiveAlphaAt(&batch,776+offset,496.173f,&right);
    CHECK(fabsf(left-right) < .0001f);
    if (offset < 18) CHECK(left > .005f);
  }
  /* The extended warm falloff reaches masonry beyond the former 66px radius. */
  e->visual = 5;
  e->source_mask = 1u<<5;
  e->geometry.data.rect = e->clip_rect = (ActionEffectLocalRect){0,0,1792,1024};
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&batch));
  bool wide_torch = false;
  for (int i = 0; i < batch.vertex_count; i++)
    if (fabsf(batch.vertices[i].position.x-840) > 70 && batch.vertices[i].color.a > .01f)
      wide_torch = true;
  CHECK(wide_torch);
  e->source_mask = 0xFFFF;
  for (unsigned room = 2; room <= 8; room++) for (unsigned phase = 0; phase < 4096; phase += 127) {
    e->visual = (uint16_t)room;
    e->phase_ticks = (uint16_t)phase;
    e->geometry.data.rect = e->clip_rect = (ActionEffectLocalRect){0,0,1792,1024};
    CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
        IdentityProjection,NULL,NULL,&batch));
  }
  /* Full gallery and cropped strips stress both horizontal and vertical
   * clipping with all eleven sources enabled and animated dust. */
  e->visual = 7;
  e->source_mask = 0x7FF;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&batch));
  for (int x = 384; x <= 784; x += 40) {
    float floor, rim_left, rim_right, pillar, gap;
    SceneAdditiveAlphaAt(&batch,x+.137f,210.173f,&floor);
    SceneAdditiveAlphaAt(&batch,x-17.363f,164.173f,&rim_left);
    SceneAdditiveAlphaAt(&batch,x+17.637f,164.173f,&rim_right);
    SceneAdditiveAlphaAt(&batch,x+20.137f,164.173f,&pillar);
    SceneAdditiveAlphaAt(&batch,x+20.137f,210.173f,&gap);
    CHECK(floor > .15f && rim_left > .05f && rim_right > .05f);
    CHECK(pillar < .001f && gap < .001f); /* The pillars keep their dark intervals. */
  }
  e->source_mask &= ~(1u<<6); /* Authored opening at x=544 is now missing. */
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&again));
  float missing_floor;
  SceneAdditiveAlphaAt(&again,544.137f,210.173f,&missing_floor);
  CHECK(missing_floor == 0);
  e->source_mask = 0x7FF;
  for (int left = 336; left < 816; left += 31) {
    const ActionEffectLocalRect cropped = {left,90.173f,left+53,221.137f};
    CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
        IdentityProjection,CastleTestClip,(void *)&cropped,&batch));
  }
  /* Cropping the same field may clip geometry, but must not move or reweight
   * its floor pools and column highlights. */
  const ActionEffectLocalRect gallery_clip = {493,132,623,225};
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,CastleTestClip,(void *)&gallery_clip,&batch));
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&again));
  for (float x = 494.137f; x < 622; x += 3) for (float y = 133.173f; y < 224; y += 9) {
    float full, clipped;
    SceneAdditiveAlphaAt(&again,x,y,&full);
    SceneAdditiveAlphaAt(&batch,x,y,&clipped);
    CHECK(fabsf(full-clipped) < .001f);
  }
  e->kind = kActionEffect_CastleSky;
  e->render_layer = kActionEffectRenderLayer_Bg2Plane;
  e->projection_plane = kActionEffectProjectionPlane_Bg2;
  e->geometry.data.rect = e->clip_rect = (ActionEffectLocalRect){-384,-48,384,194};
  const unsigned moon_rooms[] = {2,6,7,8};
  for (unsigned room = 0; room < sizeof(moon_rooms)/sizeof(moon_rooms[0]); room++) {
    e->visual = (uint16_t)moon_rooms[room];
    e->phase_ticks = 127;
    CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
        IdentityProjection,NULL,NULL,&batch));
    CHECK(batch.vertex_count > 500); /* Both Act 1 ray families, no occluder catalogue needed. */
    bool left_ray = false, right_ray = false;
    for (int i = 0; i < batch.vertex_count; i++) {
      const ArRenderVertex2D *v = &batch.vertices[i];
      if (v->color.a < .01f || v->position.y < 60) continue;
      left_ray |= v->position.x < -80;
      right_ray |= v->position.x > 80;
    }
    CHECK(left_ray && right_ray);
    e->phase_ticks += 16384;
    CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
        IdentityProjection,NULL,NULL,&again));
    CHECK(SceneBatchesEqual(&batch,&again));
  }
  /* Gallery exposure changes only brightness. The moon, ray silhouettes and
   * occlusion geometry remain the same as the exterior/boss sky treatment. */
  e->visual = 2;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&batch));
  e->visual = 7;
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,false,
      IdentityProjection,NULL,NULL,&again));
  CHECK(batch.vertex_count == again.vertex_count && batch.index_count == again.index_count);
  unsigned boosted = 0;
  for (int i = 0; i < batch.vertex_count && i < again.vertex_count; i++) {
    const ArRenderVertex2D *before = &batch.vertices[i], *after = &again.vertices[i];
    CHECK(before->position.x == after->position.x && before->position.y == after->position.y);
    if (before->position.y > 60 && before->color.a > .01f) {
      CHECK(after->color.a > before->color.a);
      CHECK(after->color.a <= .9501f);
      boosted++;
    }
  }
  CHECK(boosted > 100);
  e->visual = 8;
  e->projection_plane = kActionEffectProjectionPlane_Bg1;
  e->kind = kActionEffect_CastleMist;
  e->render_layer = kActionEffectRenderLayer_Bg1Mist;
  e->world_x = 32;
  e->world_y = 224;
  e->geometry.data.rect = e->clip_rect = (ActionEffectLocalRect){0,-18,192,0};
  CHECK(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
      IdentityProjection,NULL,NULL,&batch));
  CHECK(batch.index_count > 0);
  for (int i = 0; i < batch.vertex_count; i++) {
    CHECK(batch.vertices[i].position.y <= 224.001f);
    CHECK(batch.vertices[i].position.y >= 205.999f);
  }
  frame.decorations[1] = *e;
  frame.decoration_count = 2;
  CHECK(!ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
      IdentityProjection,NULL,NULL,&batch));
  CHECK(batch.index_count == 0); /* Duplicated aggregate cannot exhaust a batch. */
}

static void TestSkyboxEffectProjection(void) {
  DioramaProjection p = {.valid = true, .output_width = 800, .output_height = 400,
      .output_x = 10, .output_y = 20,
      .bg2_skybox = {.count = 1, .active_band = 0,
                    .bands = {{130,66,382,290,0,1}}}};
  ActionEffectProjectionContext context = {.diorama_projection = &p,
      .ws_extra = 128, .ws_extra_top = 64};
  ActionEffectInstance moon = {.world_x = 112, .world_y = 62,
      .projection_plane = kActionEffectProjectionPlane_Bg2,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,-100,384,300}}};
  ActionEffectLocalRect bounds;
  ArRenderPointF point;
  CHECK(ActionEffectProjection_ClipBounds(&context,&moon,&bounds));
  CHECK(bounds.x0 == -110 && bounds.x1 == 142 && bounds.y0 == -60 && bounds.y1 == 164);
  CHECK(ActionEffectProjection_ProjectPoint(&context,&moon,0,0,&point));
  CHECK(fabsf(point.x-(10+110*800.0f/252)) < .001f);
  CHECK(fabsf(point.y-(20+60*400.0f/224)) < .001f);
  CHECK(!ActionEffectProjection_ProjectPoint(&context,&moon,-111,0,&point));
  /* Forest light and leaves use the midpoint camera, then the same skybox
   * footprint; they must not fall back to the absent BG2 depth plane. */
  moon.projection_plane = kActionEffectProjectionPlane_BetweenBackgrounds;
  context.bg1_camera_x = 80;
  context.bg2_camera_x = 40;
  CHECK(ActionEffectProjection_ProjectPoint(&context,&moon,0,0,&point));
  CHECK(fabsf(point.x-(10+50*800.0f/252)) < .001f);
  CHECK(ActionEffectProjection_ClipBounds(&context,&moon,&bounds));
  CHECK(bounds.x0 == -50 && bounds.x1 == 202);
  p.bg2_skybox.count = 0;
  CHECK(!ActionEffectProjection_ClipBounds(&context,&moon,&bounds));
  CHECK(!ActionEffectProjection_ProjectPoint(&context,&moon,0,0,&point));
}

static void TestMoonRaysStayContinuousAcrossSkyboxBands(void) {
  DioramaProjection projection = {
    .valid = true, .output_width = 800, .output_height = 400,
    .bg2_skybox = {.count = 2, .active_band = 0,
      .bands = {{44,64,476,200,0,136.0f/224},
                {0,200,496,288,136.0f/224,1}}},
  };
  ActionEffectProjectionContext context = {
    .ws_extra = 120, .ws_extra_top = 64, .diorama_projection = &projection,
  };
  ActionEffectInstance moon = {
    .kind = kActionEffect_BloodpoolMoonlight, .world_x = 112, .world_y = 62,
    .phase = kActionEffectPhase_BloodpoolEnvironment, .visual = 1,
    .flags = kActionEffectFlag_Visible | kActionEffectFlag_ClippedMesh,
    .render_layer = kActionEffectRenderLayer_Bg2Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg2,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,194}},
  };
  ArRenderPointF upper, lower;
  CHECK(ActionEffectProjection_ProjectPoint(&context,&moon,32,74,&upper));
  ActionEffectLocalRect upper_clip, lower_clip;
  CHECK(ActionEffectProjection_ClipBounds(&context,&moon,&upper_clip));
  projection.bg2_skybox.active_band = 1;
  CHECK(ActionEffectProjection_ProjectPoint(&context,&moon,32,74,&lower));
  CHECK(ActionEffectProjection_ClipBounds(&context,&moon,&lower_clip));
  CHECK(fabsf(upper.x-lower.x) < .001f);
  CHECK(fabsf(upper.y-lower.y) < .001f);
  CHECK(fabsf(upper_clip.x0-lower_clip.x0) < .001f);
  CHECK(fabsf(upper_clip.x1-lower_clip.x1) < .001f);
  CHECK(fabsf(upper_clip.y1-lower_clip.y0) < .001f);
  ActionSceneEffectFrame frame = {
    .decoration_count = 1, .decoration_visible_count = 1,
    .moonlight = {.valid = true},
  };
  frame.decorations[0] = moon;
  static ActionSceneEffectRenderBatch bands[2], continuous;
  const DioramaSkyboxProjection source_bands = projection.bg2_skybox;
  for (int band = 0; band < 2; band++) {
    projection.bg2_skybox.active_band = band;
    CHECK(ActionSceneDecorationRender_Build(&frame,moon.render_layer,true,false,
        ActionEffectProjection_ProjectPoint,ActionEffectProjection_ClipBounds,
        &context,&bands[band]));
    CHECK(bands[band].index_count > 0);
    for (int i = 0; i < bands[band].vertex_count; i++) {
      const float y = bands[band].vertices[i].position.y;
      CHECK(y >= 400*source_bands.bands[band].output_y0-.001f);
      CHECK(y <= 400*source_bands.bands[band].output_y1+.001f);
    }
  }
  projection.bg2_skybox = (DioramaSkyboxProjection){
    .count = 1, .active_band = 0, .bands = {{44,64,476,288,0,1}},
  };
  CHECK(ActionSceneDecorationRender_Build(&frame,moon.render_layer,true,false,
      ActionEffectProjection_ProjectPoint,ActionEffectProjection_ClipBounds,
      &context,&continuous));
  for (int band = 0; band < 2; band++) {
    const float y = upper.y + (band ? 2 : -2);
    const float expected = MoonAlphaAt(&continuous,upper.x,y);
    CHECK(expected > 0);
    CHECK(fabsf(MoonAlphaAt(&bands[band],upper.x,y)-expected) < .001f);
  }
  projection.bg2_skybox = source_bands;
  /* Water reflections retain the native row-band projection. Only the
   * airborne light field uses one transform through the water horizon. */
  moon.kind = kActionEffect_BloodpoolMoonReflection;
  CHECK(ActionEffectProjection_ProjectPoint(&context,&moon,32,74,&lower));
  projection.bg2_skybox.active_band = 0;
  CHECK(ActionEffectProjection_ProjectPoint(&context,&moon,32,74,&upper));
  CHECK(fabsf(upper.x-lower.x) > 10);
}

static void TestMoonCloudStaysContinuousAcrossSkyboxBands(void) {
  DioramaProjection projection = {
    .valid = true, .output_width = 800, .output_height = 400,
    .bg2_skybox = {.count = 2, .active_band = 0,
      .bands = {{44,64,476,128,0,64.0f/224},
                {0,128,496,288,64.0f/224,1}}},
  };
  ActionEffectProjectionContext context = {
    .ws_extra = 120, .ws_extra_top = 64, .diorama_projection = &projection,
  };
  ActionSceneEffectFrame frame = {.decoration_count = 1, .decoration_visible_count = 1};
  frame.decorations[0] = (ActionEffectInstance){
    .kind = kActionEffect_BloodpoolCloud, .world_x = 112, .world_y = 62,
    .phase = kActionEffectPhase_BloodpoolEnvironment, .environment_room = 1,
    .flags = kActionEffectFlag_Visible, .phase_ticks = 600,
    .render_layer = kActionEffectRenderLayer_Bg2Alpha,
    .projection_plane = kActionEffectProjectionPlane_Bg2,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-100,-40,100,44}},
  };
  static ActionSceneEffectRenderBatch bands[2], continuous;
  for (int band = 0; band < 2; band++) {
    projection.bg2_skybox.active_band = band;
    CHECK(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_Bg2Alpha,true,false,
        ActionEffectProjection_ProjectPoint,ActionEffectProjection_ClipBounds,
        &context,&bands[band]));
    CHECK(bands[band].index_count > 0);
  }
  projection.bg2_skybox = (DioramaSkyboxProjection){
    .count = 1, .active_band = 0, .bands = {{44,64,476,288,0,1}},
  };
  CHECK(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_Bg2Alpha,true,false,
      ActionEffectProjection_ProjectPoint,ActionEffectProjection_ClipBounds,
      &context,&continuous));
  for (int band = 0; band < 2; band++) {
    const float y = 400*64.0f/224 + (band ? 2 : -2);
    for (float x = 250; x < 430; x += 17) {
      const float expected = MoonAlphaAt(&continuous,x,y);
      CHECK(expected > .005f);
      CHECK(fabsf(MoonAlphaAt(&bands[band],x,y)-expected) < .001f);
    }
  }
}

int main(void) {
  TestDistantWaveCapsFollowRaster();
  TestMoonCloudStaysContinuousAcrossSkyboxBands();
  TestMoonRaysStayContinuousAcrossSkyboxBands();
  TestSkyboxEffectProjection();
  TestCastleStackedRays();
  TestBloodpoolMarshDetails();
  TestCastleGeometry();
  TestCastleWaterGeometry();
  TestBloodpoolWaterMoonlight();
  TestMoonDependencyScope();
  TestBloodpoolGeometry();
  TestBloodpoolMoonlight();
  TestCavePolishGeometry();
  TestLandingParticleGeometry();
  TestCaveEnvironmentGeometry();
  TestCaveWaterUsesHighPlane();
  TestCaveAmbientScroll();
  TestTempleGroundMist();
  TestForestClippingPreservesField(928, 80);
  TestForestClippingPreservesField(2918, 202);
  TestForestCanopyGeometry();
  TestForestFanOut();
  TestForestParticles();
  TestForestBossClearingLight();
  TestForestLayerScroll();
  TestBetweenBackgroundsProjection();
  TestNorthwallGeometryBudget();
  TestNorthwallWaterSplash();
  TestFirstActBossMagicGeometry();
  TestFeatureSwitchesAndDeterminism();
  TestMixedStagesAreOrderIndependent();
  TestClocksAndValidation();
  TestCapacityIsDerivedFromPublishedLimits();
  TestSceneFeatureSwitchesAndDeterminism();
  TestSceneKindsRemainIndependent();
  TestBossRushEffectStyles();
  TestAitosLavaLightingAndParticles();
  TestAitosStatueFireLightingAndFacing();
  TestAitosSideLavaLightingAndHeatMesh();
  TestFlamingWheelRimAndProjectile();
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
  if (s_failures) {
    fprintf(stderr, "%d action-effect render test(s) failed\n", s_failures);
    return 1;
  }
  puts("action effect render: all tests passed");
  return 0;
}
