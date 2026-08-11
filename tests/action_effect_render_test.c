#include <math.h>
#include <stdio.h>
#include <string.h>

#include "action_effect_render.h"

static int g_failures;

#define CHECK(condition) do {                                                \
  if (!(condition)) {                                                        \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,       \
            #condition);                                                     \
    g_failures++;                                                            \
  }                                                                          \
} while (0)

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
  CHECK(memcmp(&both, &repeat, sizeof(both)) == 0);

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
  CHECK(memcmp(&first, &changed, sizeof(first)) != 0);

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
    .velocity_x = kind == kActionEffect_EnemyFireball ? 3
        : (kind == kActionEffect_SwordBeam ? 8 : 0),
    .visual = kind == kActionEffect_SwordBeam
        ? 0x30
        : (kind == kActionEffect_LightningTrap
        ? 0x1F
        : (kind == kActionEffect_BloodpoolBossLightning ? 0x05 : 0x24)),
    .phase_ticks = 9,
    .pulse_ticks = 9,
    .kind = kind,
    .phase = kind == kActionEffect_SwordBeam
        ? kActionEffectPhase_SwordBeamFlight
        : (kind == kActionEffect_WallTorch
        ? kActionEffectPhase_WallTorch
        : (kind == kActionEffect_EnemyFireball
            ? kActionEffectPhase_EnemyFireballFlight
            : (kind == kActionEffect_BloodpoolBossLightning
                ? kActionEffectPhase_BossLightningStrike
                : kActionEffectPhase_LightningActive))),
    .role = kActionEffectRole_Body,
    .flags = kActionEffectFlag_Visible,
    .obj_priority = 0,
    .render_layer = kActionEffectRenderLayer_WorldOverlay,
    .projection_plane = kind == kActionEffect_WallTorch
        ? kActionEffectProjectionPlane_Bg1
        : kActionEffectProjectionPlane_Obj,
    .geometry = {
      .kind = kActionEffectGeometry_Rect,
      .data.rect = {-8.0f, -8.0f, 8.0f, 8.0f},
    },
  };
  if (kind == kActionEffect_LightningTrap)
    effect.geometry.data.rect =
        (ActionEffectLocalRect){0.0f, -88.0f, 8.0f, 88.0f};
  else if (kind == kActionEffect_BloodpoolBossLightning)
    effect.geometry.data.rect =
        (ActionEffectLocalRect){-30.0f, -83.0f, 8.0f, 21.0f};
  else if (kind == kActionEffect_SwordBeam)
    effect.geometry.data.rect =
        (ActionEffectLocalRect){32.0f, -33.0f, 48.0f, -1.0f};
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
  CHECK(memcmp(&both, &repeat, sizeof(both)) == 0);

  frame.effects[0].pulse_ticks++;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &repeat));
  CHECK(memcmp(&both, &repeat, sizeof(both)) != 0);
}

static void TestSceneKindsRemainIndependent(void) {
  ActionSceneEffectFrame frame = {.effect_count = 3, .visible_count = 3};
  frame.effects[0] = SceneEffect(kActionEffect_WallTorch, 100);
  frame.effects[1] = SceneEffect(kActionEffect_EnemyFireball, 200);
  frame.effects[2] = SceneEffect(kActionEffect_LightningTrap, 300);
  ActionSceneEffectRenderBatch batch;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 6 * kActionEffectGlowVertices +
                                (7 + 12 + 12) * 4);
  CHECK(batch.index_count == 6 * kActionEffectGlowIndices +
                               (7 + 12 + 12) * 6);

  frame.effects[1].kind = 99;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 4 * kActionEffectGlowVertices +
                                (7 + 12) * 4);
  frame.effects[0].projection_plane = 99;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 2 * kActionEffectGlowVertices + 12 * 4);
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
  CHECK(memcmp(&lighting, &repeat, sizeof(lighting)) == 0);

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
  CHECK(memcmp(&particles, &repeat, sizeof(particles)) == 0);
  frame.effects[0].pulse_ticks++;
  CHECK(ActionSceneEffectRender_Build(
      &frame, false, true, IdentityProjection, NULL, &repeat));
  CHECK(memcmp(&particles, &repeat, sizeof(particles)) != 0);
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
  for (unsigned i = 2; i < kActionSceneEffectMaxInstances; i++)
    frame.effects[i] = SceneEffect(kActionEffect_LightningTrap, 100 + i * 20);
  ActionSceneEffectRenderBatch batch;
  CHECK(ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == kActionSceneEffectRenderMaxVertices);
  CHECK(batch.index_count == kActionSceneEffectRenderMaxIndices);

  /* A second active boss filament cannot arise from the mapped one-boss
   * lifecycle. Reject it as a capacity-contract violation without publishing
   * a partial batch instead of reserving 16 impossible ribbons on the stack. */
  frame.effects[2] = SceneEffect(
      kActionEffect_BloodpoolBossLightning, 140);
  CHECK(!ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 0);
  CHECK(batch.index_count == 0);
  /* The expanded comet budget likewise belongs to the one player projectile,
   * not to an arbitrary number of forged scene records. */
  frame.effects[2] = SceneEffect(kActionEffect_SwordBeam, 140);
  CHECK(!ActionSceneEffectRender_Build(
      &frame, true, true, IdentityProjection, NULL, &batch));
  CHECK(batch.vertex_count == 0);
  CHECK(batch.index_count == 0);
  frame.effects[2] = SceneEffect(kActionEffect_LightningTrap, 140);

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
  TestLightningVisibleLightCoversCapturedArc();
  TestBossLightningFilamentAndStages();
  TestSwordBeamLightingTrailAndStars();
  TestSceneCapacityAndMalformedInput();
  if (g_failures) {
    fprintf(stderr, "%d action-effect render test(s) failed\n", g_failures);
    return 1;
  }
  puts("action effect render: all tests passed");
  return 0;
}
