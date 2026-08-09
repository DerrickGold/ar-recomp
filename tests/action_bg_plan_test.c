/* ROM-free policy matrix for SPEC-bg-hle BH3. */

#include <stdio.h>
#include <string.h>

#include "action/action_bg_plan.h"

static int failures;
#define CHECK(e) do { if (!(e)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); failures++; \
} } while (0)

static ActionBgFrameState State(uint8_t group, uint8_t map) {
  ActionBgFrameState state = {
    .map_group = group,
    .map_number = map,
    .decorative_padding_enabled = true,
  };
  for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
    state.layer[layer] = (ActionBgLayerState) {
      .world_width = 512,
      .world_height = 256,
      .tilemap_base = (uint16_t)(0x6000 + layer * 0x1000),
      .bgsc = (uint8_t)(0x63 + layer * 0x10),
    };
  }
  return state;
}

static ActionBgPlan Build(ActionBgFrameState *state) {
  ActionBgPlan plan;
  memset(&plan, 0xA5, sizeof(plan));
  CHECK(ActionBgPlan_Build(state, &plan));
  CHECK(plan.valid && plan.layer[0].valid && plan.layer[1].valid);
  return plan;
}

static ActionBgPresentationPolicy Compile(const ActionBgPlan *plan) {
  ActionBgPresentationPolicy policy;
  memset(&policy, 0xA5, sizeof(policy));
  CHECK(ActionBgPlan_CompilePresentation(plan, &policy));
  return policy;
}

static void TestValidationAndFallback(void) {
  ActionBgPlan plan;
  ActionBgPresentationPolicy policy;
  ActionBgFrameState state = State(1, 1);
  CHECK(!ActionBgPlan_Build(NULL, &plan));
  CHECK(!ActionBgPlan_Build(&state, NULL));
  state.map_group = 0;
  memset(&plan, 0xA5, sizeof(plan));
  CHECK(!ActionBgPlan_Build(&state, &plan));
  CHECK(!plan.valid && !plan.layer[0].valid);
  state = State(1, 5);
  CHECK(!ActionBgPlan_Build(&state, &plan));
  state = State(7, 9);
  CHECK(!ActionBgPlan_Build(&state, &plan));
  state = State(1, 1);
  state.layer[0].world_width = 384;
  CHECK(!ActionBgPlan_Build(&state, &plan));
  state = State(1, 1);
  state.layer[1].world_height = 0;
  CHECK(!ActionBgPlan_Build(&state, &plan));
  memset(&plan, 0, sizeof(plan));
  memset(&policy, 0xA5, sizeof(policy));
  CHECK(!ActionBgPlan_CompilePresentation(&plan, &policy));
  CHECK(policy.repeat_band_layer == -1 && !policy.clamp_layers);
}

static void TestOrdinaryWorldAndNativeSource(void) {
  ActionBgFrameState state = State(1, 1);
  ActionBgPlan plan = Build(&state);
  CHECK(plan.layer[0].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[1].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[0].default_edge == kActionBgEdge_LiveWorld);
  CHECK(plan.bound_canvas_to_world);
  ActionBgPresentationPolicy policy = Compile(&plan);
  CHECK(!policy.clamp_layers && !policy.mirror_layers &&
        !policy.repeat_layers && policy.bound_canvas_to_world);

  state.layer[1].bgsc = 0x70;
  plan = Build(&state);
  CHECK(plan.layer[1].source == kActionBgSource_NativeTilemap);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_RawWrap);
}

static void TestNarrowDecorativeBg2(void) {
  ActionBgFrameState state = State(3, 1);
  state.layer[1].world_width = 256;
  state.layer[1].bgsc = 0x70;
  ActionBgPlan plan = Build(&state);
  CHECK(plan.layer[1].source == kActionBgSource_AuthenticViewport);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Mirror);
  CHECK(Compile(&plan).mirror_layers == 2);

  state.decorative_padding_enabled = false;
  plan = Build(&state);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Clamp);
  CHECK(Compile(&plan).clamp_layers == 2);

  const uint8_t repeat_cases[][2] = {
    { 4, 1 }, { 4, 3 }, { 6, 1 }, { 6, 5 }, { 6, 8 },
    { 7, 2 }, { 7, 7 },
  };
  for (size_t i = 0; i < sizeof(repeat_cases) / sizeof(repeat_cases[0]); i++) {
    state = State(repeat_cases[i][0], repeat_cases[i][1]);
    state.layer[1].world_width = 256;
    plan = Build(&state);
    CHECK(plan.layer[1].default_edge == kActionBgEdge_Repeat);
    CHECK(Compile(&plan).repeat_layers == 2);
  }

  const uint8_t mirror_cases[][2] = {
    { 4, 4 }, { 6, 6 }, { 6, 7 },
  };
  for (size_t i = 0; i < sizeof(mirror_cases) / sizeof(mirror_cases[0]); i++) {
    state = State(mirror_cases[i][0], mirror_cases[i][1]);
    state.layer[1].world_width = 256;
    plan = Build(&state);
    CHECK(plan.layer[1].default_edge == kActionBgEdge_Mirror);
  }
}

static void TestBloodpoolBand(void) {
  ActionBgFrameState state = State(2, 1);
  state.layer[1].world_width = 256;
  ActionBgPlan plan = Build(&state);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Mirror);
  CHECK(plan.layer[1].band_count == 1);
  CHECK(plan.layer[1].bands[0].y0 == 136 &&
        plan.layer[1].bands[0].y1 == 224 &&
        plan.layer[1].bands[0].edge == kActionBgEdge_Repeat);
  ActionBgPresentationPolicy policy = Compile(&plan);
  CHECK(policy.mirror_layers == 2 && policy.repeat_band_layer == 1);
  CHECK(policy.repeat_band_y0 == 136 && policy.repeat_band_y1 == 224);

  state.decorative_padding_enabled = false;
  plan = Build(&state);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Clamp);
  CHECK(plan.layer[1].band_count == 0);
}

static void TestDeathHeimStates(void) {
  ActionBgFrameState state = State(7, 1);
  ActionBgPlan plan = Build(&state);
  CHECK(plan.layer[0].source == kActionBgSource_AuthenticViewport);
  CHECK(plan.layer[0].default_edge == kActionBgEdge_Clamp);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Clamp);
  CHECK(plan.layer[1].band_count == 1);
  ActionBgPresentationPolicy policy = Compile(&plan);
  CHECK(policy.clamp_layers == 3 && !policy.bound_canvas_to_world);
  CHECK(!plan.bound_canvas_to_world);
  CHECK(policy.repeat_band_layer == 1 && policy.repeat_band_y0 == 144 &&
        policy.repeat_band_y1 == 224);

  state.death_heim_progress = 7;
  state.layer[0].bgsc = 0x64;
  state.layer[1].bgsc = 0x74;
  plan = Build(&state);
  policy = Compile(&plan);
  CHECK(policy.clamp_layers == 1 && policy.mirror_layers == 2);
  CHECK(policy.repeat_band_layer == -1);

  state.layer[0].bgsc = 0x60;
  state.layer[1].bgsc = 0x70;
  state.death_heim_ending_state = 3;
  plan = Build(&state);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Mirror);

  state = State(7, 8);
  state.layer[0].world_width = 256;
  state.layer[1].world_width = 256;
  plan = Build(&state);
  policy = Compile(&plan);
  CHECK(plan.layer[0].source == kActionBgSource_NativeTilemap);
  CHECK(plan.layer[1].source == kActionBgSource_NativeTilemap);
  CHECK(plan.layer[0].default_edge == kActionBgEdge_RawWrap);
  CHECK(!policy.clamp_layers && !policy.mirror_layers &&
        !policy.repeat_layers && !policy.bound_canvas_to_world);
}

static void TestEveryKnownMapClassifies(void) {
  static const uint8_t last_map[] = { 0, 4, 8, 6, 7, 8, 8, 8 };
  for (uint8_t group = 1; group <= 7; group++) {
    for (uint8_t map = 1; map <= last_map[group]; map++) {
      ActionBgFrameState state = State(group, map);
      ActionBgPlan plan;
      CHECK(ActionBgPlan_Build(&state, &plan));
      CHECK(plan.valid);
    }
  }
}

int main(void) {
  TestValidationAndFallback();
  TestOrdinaryWorldAndNativeSource();
  TestNarrowDecorativeBg2();
  TestBloodpoolBand();
  TestDeathHeimStates();
  TestEveryKnownMapClassifies();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("action_bg_plan: OK\n");
  return 0;
}
