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

int main(void) {
  TestFeatureSwitchesAndDeterminism();
  TestMixedStagesAreOrderIndependent();
  TestClocksAndValidation();
  TestCapacityIsDerivedFromPublishedLimits();
  if (g_failures) {
    fprintf(stderr, "%d action-effect render test(s) failed\n", g_failures);
    return 1;
  }
  puts("action effect render: all tests passed");
  return 0;
}
