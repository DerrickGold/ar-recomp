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

static bool BandIsClear(const ActionBgBand *band) {
  return band && !band->y0 && !band->y1 &&
      band->edge == kActionBgEdge_Transparent &&
      band->horizontal_extent.mode == kActionBgExtent_Inherit &&
      !band->horizontal_extent.left && !band->horizontal_extent.right;
}

static ActionBgPresentationPolicy Compile(const ActionBgPlan *plan) {
  ActionBgPresentationPolicy policy;
  memset(&policy, 0xA5, sizeof(policy));
  CHECK(ActionBgPlan_CompilePresentation(plan, &policy));
  return policy;
}

static bool PresentationBandIs(const ActionBgPresentationPolicy *policy,
                               unsigned index, uint8_t layer,
                               uint8_t y0, uint8_t y1,
                               ActionBgEdgeMode edge) {
  if (!policy || index >= policy->band_count) return false;
  const ActionBgPresentationBand *band = &policy->bands[index];
  return band->layer == layer && band->y0 == y0 && band->y1 == y1 &&
      band->edge == edge;
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
  CHECK(!policy.band_count && !policy.clamp_layers);
  CHECK(!strcmp(ActionBgSourceKind_Name(kActionBgSource_NativeTilemap),
                "native"));
  CHECK(!strcmp(ActionBgSourceKind_Name(kActionBgSource_WorldMap), "world"));
  CHECK(!strcmp(ActionBgSourceKind_Name(kActionBgSource_AuthenticViewport),
                "viewport"));
  CHECK(!strcmp(ActionBgSourceKind_Name((ActionBgSourceKind)99), "unknown"));
  CHECK(!strcmp(ActionBgLayerRole_Name(kActionBgLayerRole_Playfield),
                "playfield"));
  CHECK(!strcmp(ActionBgLayerRole_Name(kActionBgLayerRole_Scene), "scene"));
  CHECK(!strcmp(ActionBgLayerRole_Name(kActionBgLayerRole_Backdrop),
                "backdrop"));
  CHECK(!strcmp(ActionBgLayerRole_Name((ActionBgLayerRole)99), "unknown"));
  CHECK(!strcmp(ActionBgEdgeMode_Name(kActionBgEdge_LiveWorld), "world"));
  CHECK(!strcmp(ActionBgEdgeMode_Name(kActionBgEdge_Repeat), "repeat"));
  CHECK(!strcmp(ActionBgEdgeMode_Name((ActionBgEdgeMode)99), "unknown"));
  CHECK(!strcmp(ActionBgMotionMode_Name(kActionBgMotion_FillRelative),
                "fill"));
  CHECK(!strcmp(ActionBgMotionMode_Name(kActionBgMotion_NormalScroll),
                "normal"));
  CHECK(!strcmp(ActionBgBandAnchor_Name(kActionBgBandAnchor_World),
                "world"));
  CHECK(!strcmp(ActionBgExtentMode_Name(kActionBgExtent_Inherit), "inherit"));
  CHECK(!strcmp(ActionBgExtentMode_Name(kActionBgExtent_Available),
                "available"));
  CHECK(!strcmp(ActionBgExtentMode_Name((ActionBgExtentMode)99), "unknown"));
}

static void TestResolvedPresentationProjection(void) {
  ActionBgPlan plan;
  ActionBgPlan_InitNative(&plan);
  CHECK(plan.valid && plan.layer[0].valid && plan.layer[1].valid);
  CHECK(plan.layer[0].role == kActionBgLayerRole_Unclassified);
  CHECK(plan.layer[0].source == kActionBgSource_NativeTilemap);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_RawWrap);
  CHECK(plan.layer[0].horizontal_extent.mode == kActionBgExtent_Available);
  CHECK(plan.layer[0].vertical_extent.mode == kActionBgExtent_Available);

  ActionBgPresentationPolicy policy = {
    .clamp_layers = 2,
    .band_count = 1,
    .bands = {{
      .layer = 1,
      .y0 = 144,
      .y1 = 224,
      .edge = kActionBgEdge_Repeat,
    }},
    .bound_canvas_to_world = true,
  };
  CHECK(ActionBgPlan_ApplyPresentationPolicy(&plan, &policy));
  CHECK(plan.layer[0].default_edge == kActionBgEdge_RawWrap);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Clamp);
  CHECK(plan.layer[1].band_count == 1);
  CHECK(plan.layer[1].bands[0].y0 == 144 &&
        plan.layer[1].bands[0].y1 == 224 &&
        plan.layer[1].bands[0].edge == kActionBgEdge_Repeat);
  CHECK(plan.bound_canvas_to_world);

  /* Global raw/4:3 projection retains the canonical source seam while removing
   * decorative edges and bands that were not executed for that frame. */
  plan.layer[1].source = kActionBgSource_AuthenticViewport;
  policy = (ActionBgPresentationPolicy){ 0 };
  CHECK(ActionBgPlan_ApplyPresentationPolicy(&plan, &policy));
  CHECK(plan.layer[1].source == kActionBgSource_AuthenticViewport);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_RawWrap);
  CHECK(!plan.layer[1].band_count && !plan.bound_canvas_to_world);
  CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Available);
  CHECK(plan.layer[1].vertical_extent.mode == kActionBgExtent_Available);
  ActionBgBand empty_bands[kActionBgMaxBands] = { 0 };
  CHECK(!memcmp(plan.layer[1].bands, empty_bands, sizeof(empty_bands)));

  ActionBgPlan before = plan;
  policy = (ActionBgPresentationPolicy) {
    .clamp_layers = 2,
    .mirror_layers = 2,
  };
  CHECK(!ActionBgPlan_ApplyPresentationPolicy(&plan, &policy));
  CHECK(!memcmp(&plan, &before, sizeof(plan)));
  policy = (ActionBgPresentationPolicy) {
    .band_count = 1,
    .bands = {{
      .layer = 1,
      .y0 = 224,
      .y1 = 144,
      .edge = kActionBgEdge_Repeat,
    }},
  };
  CHECK(!ActionBgPlan_ApplyPresentationPolicy(&plan, &policy));
  CHECK(!memcmp(&plan, &before, sizeof(plan)));

  policy = (ActionBgPresentationPolicy) {
    .mirror_layers = 2,
    .normal_scroll_layers = 2,
    .band_count = 3,
    .bands = {
      { .layer = 0, .y0 = 0, .y1 = 32,
        .edge = kActionBgEdge_Clamp },
      { .layer = 1, .y0 = 32, .y1 = 96,
        .edge = kActionBgEdge_Mirror,
        .motion = kActionBgMotion_NormalScroll },
      { .layer = 1, .y0 = 136, .y1 = 224,
        .edge = kActionBgEdge_Repeat },
    },
  };
  CHECK(ActionBgPlan_ApplyPresentationPolicy(&plan, &policy));
  CHECK(plan.layer[1].default_motion == kActionBgMotion_NormalScroll);
  CHECK(plan.layer[0].band_count == 1 && plan.layer[1].band_count == 2);
  CHECK(plan.layer[1].bands[0].motion == kActionBgMotion_NormalScroll);
  ActionBgPresentationPolicy round_trip = Compile(&plan);
  CHECK(round_trip.band_count == 3);
  CHECK(round_trip.normal_scroll_layers == 2);
  CHECK(PresentationBandIs(
      &round_trip, 2, 1, 136, 224, kActionBgEdge_Repeat));
}

static ActionBgLayerPlan ExtentLayer(void) {
  return (ActionBgLayerPlan) {
    .valid = true,
    .source = kActionBgSource_AuthenticViewport,
    .default_edge = kActionBgEdge_Mirror,
    .horizontal_extent = {
      .mode = kActionBgExtent_Fixed,
      .left = 48,
      .right = 64,
    },
    .vertical_extent = {
      .mode = kActionBgExtent_Available,
    },
  };
}

static void TestExtentValidationAndRowResolution(void) {
  ActionBgLayerPlan layer = ExtentLayer();
  layer.bands[0] = (ActionBgBand) {
    .y0 = 32,
    .y1 = 80,
    .edge = kActionBgEdge_Repeat,
    /* Zero initialization is the behavior-neutral inherit spelling. */
  };
  layer.bands[1] = (ActionBgBand) {
    .y0 = 136,
    .y1 = 224,
    .edge = kActionBgEdge_Repeat,
    .horizontal_extent = {
      .mode = kActionBgExtent_Available,
    },
  };
  layer.band_count = 2;
  CHECK(ActionBgLayerPlan_Validate(&layer));

  ActionBgRowPolicy row;
  memset(&row, 0xA5, sizeof(row));
  CHECK(ActionBgLayerPlan_ResolveRow(&layer, -16, &row));
  CHECK(row.edge == kActionBgEdge_Mirror);
  CHECK(row.horizontal_extent.mode == kActionBgExtent_Fixed);
  CHECK(row.horizontal_extent.left == 48 &&
        row.horizontal_extent.right == 64);

  CHECK(ActionBgLayerPlan_ResolveRow(&layer, 40, &row));
  CHECK(row.edge == kActionBgEdge_Repeat);
  CHECK(row.horizontal_extent.mode == kActionBgExtent_Fixed);
  CHECK(row.horizontal_extent.left == 48 &&
        row.horizontal_extent.right == 64);

  CHECK(ActionBgLayerPlan_ResolveRow(&layer, 136, &row));
  CHECK(row.edge == kActionBgEdge_Repeat);
  CHECK(row.horizontal_extent.mode == kActionBgExtent_Available);
  CHECK(!row.horizontal_extent.left && !row.horizontal_extent.right);

  CHECK(ActionBgLayerPlan_ResolveRow(&layer, 240, &row));
  CHECK(row.edge == kActionBgEdge_Repeat);
  CHECK(row.horizontal_extent.mode == kActionBgExtent_Available);

  /* Internal bands cannot leak into the top margin. Once the first band is
   * anchored at row zero, however, the same family owns that margin. */
  CHECK(ActionBgLayerPlan_ResolveRow(&layer, -16, &row));
  CHECK(row.edge == kActionBgEdge_Mirror);
  layer.bands[0].y0 = 0;
  CHECK(ActionBgLayerPlan_ResolveRow(&layer, -16, &row));
  CHECK(row.edge == kActionBgEdge_Repeat);
  CHECK(row.horizontal_extent.mode == kActionBgExtent_Fixed);
  CHECK(row.horizontal_extent.left == 48 &&
        row.horizontal_extent.right == 64);
  CHECK(!ActionBgLayerPlan_ResolveRow(NULL, 0, &row));
  CHECK(!ActionBgLayerPlan_ResolveRow(&layer, 0, NULL));

  ActionBgLayerPlan invalid = layer;
  invalid.role = (ActionBgLayerRole)99;
  CHECK(!ActionBgLayerPlan_Validate(&invalid));
  invalid = layer;
  invalid.horizontal_extent.mode = kActionBgExtent_Inherit;
  CHECK(!ActionBgLayerPlan_Validate(&invalid));
  invalid = layer;
  invalid.horizontal_extent.mode = kActionBgExtent_Available;
  invalid.horizontal_extent.left = 1;
  CHECK(!ActionBgLayerPlan_Validate(&invalid));
  invalid = layer;
  invalid.vertical_extent.mode = kActionBgExtent_Inherit;
  CHECK(!ActionBgLayerPlan_Validate(&invalid));
  invalid = layer;
  invalid.bands[1].y0 = 64;
  CHECK(!ActionBgLayerPlan_Validate(&invalid));
  invalid = layer;
  invalid.bands[1].y1 = 225;
  CHECK(!ActionBgLayerPlan_Validate(&invalid));
  invalid = layer;
  invalid.bands[0].horizontal_extent.mode = kActionBgExtent_Inherit;
  invalid.bands[0].horizontal_extent.right = 1;
  CHECK(!ActionBgLayerPlan_Validate(&invalid));
  invalid = layer;
  invalid.bands[0].anchor = kActionBgBandAnchor_World;
  invalid.bands[0].y1 = invalid.world_height + 1;
  CHECK(!ActionBgLayerPlan_Validate(&invalid));
  invalid = layer;
  invalid.bands[0].motion = (ActionBgMotionMode)99;
  CHECK(!ActionBgLayerPlan_Validate(&invalid));

  /* Mixed anchors must remain disjoint for the complete camera traversal, not
   * merely at the frame where a tuner edit was accepted. This safe pair keeps
   * a fixed top strip above a deep world family at camera 0 and at its native
   * maximum. The second pair is disjoint at camera 100 but would cross later. */
  ActionBgLayerPlan mixed = ExtentLayer();
  mixed.world_height = 512;
  mixed.camera_y = 100;
  mixed.bands[0] = (ActionBgBand) {
    .y0 = 0, .y1 = 32,
    .edge = kActionBgEdge_Mirror,
  };
  mixed.bands[1] = (ActionBgBand) {
    .y0 = 400, .y1 = 512,
    .edge = kActionBgEdge_Repeat,
    .anchor = kActionBgBandAnchor_World,
  };
  mixed.band_count = 2;
  CHECK(ActionBgLayerPlan_Validate(&mixed));
  mixed.camera_y = 287;
  CHECK(ActionBgLayerPlan_Validate(&mixed));
  mixed.camera_y = 100;
  mixed.bands[1].y0 = 200;
  mixed.bands[1].y1 = 256;
  int projected_y0 = 0, projected_y1 = 0;
  CHECK(ActionBgLayerPlan_ResolveBand(
      &mixed, 1, &projected_y0, &projected_y1));
  CHECK(projected_y0 == 99 && projected_y1 == 155);
  CHECK(!ActionBgLayerPlan_Validate(&mixed));

  ActionBgPlan plan = { .valid = true };
  plan.layer[0] = layer;
  plan.layer[1] = ExtentLayer();
  CHECK(ActionBgPlan_Validate(&plan));
  plan.layer[1].valid = false;
  CHECK(!ActionBgPlan_Validate(&plan));
  CHECK(!ActionBgPlan_Validate(NULL));
}

static void TestUnboundWorldFallback(void) {
  ActionBgFrameState state = State(1, 1);
  ActionBgPlan plan = Build(&state);
  CHECK(ActionBgPlan_ClampUnboundWorldLayers(&plan, 1, 3) == 2);
  CHECK(plan.layer[0].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[0].default_edge == kActionBgEdge_LiveWorld);
  CHECK(plan.layer[1].source == kActionBgSource_AuthenticViewport);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Clamp);
  CHECK(!plan.layer[1].band_count);

  plan = Build(&state);
  CHECK(!ActionBgPlan_ClampUnboundWorldLayers(&plan, 3, 3));
  CHECK(plan.layer[0].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[1].source == kActionBgSource_WorldMap);

  state = State(2, 1);
  state.layer[1].world_width = 256;
  plan = Build(&state);
  CHECK(ActionBgPlan_ClampUnboundWorldLayers(&plan, 0, 3) == 1);
  CHECK(plan.layer[0].source == kActionBgSource_AuthenticViewport);
  CHECK(plan.layer[0].default_edge == kActionBgEdge_Clamp);
  CHECK(plan.layer[1].source == kActionBgSource_AuthenticViewport);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Mirror);
  CHECK(plan.layer[1].band_count == 1);

  state = State(1, 1);
  plan = Build(&state);
  CHECK(ActionBgPlan_ClampUnboundWorldLayers(&plan, 0, 1) == 1);
  CHECK(plan.layer[0].source == kActionBgSource_AuthenticViewport);
  CHECK(plan.layer[1].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_LiveWorld);

  memset(&plan, 0, sizeof(plan));
  CHECK(!ActionBgPlan_ClampUnboundWorldLayers(&plan, 0, 3));
  CHECK(!ActionBgPlan_ClampUnboundWorldLayers(NULL, 0, 3));
}

static void TestCanvasOwner(void) {
  ActionBgFrameState state = State(1, 1);
  ActionBgPlan plan = Build(&state);
  CHECK(ActionBgPlan_PlayfieldLayer(&plan) == 0);
  CHECK(ActionBgPlan_PrimaryLayer(&plan) == 0);
  CHECK(ActionBgPlan_CanvasOwner(&plan) == 0);

  plan.bound_canvas_to_world = false;
  CHECK(ActionBgPlan_CanvasOwner(&plan) == -1);
  plan.bound_canvas_to_world = true;
  plan.layer[0].source = kActionBgSource_AuthenticViewport;
  CHECK(ActionBgPlan_PlayfieldLayer(&plan) == 0);
  CHECK(ActionBgPlan_CanvasOwner(&plan) == -1);

  plan = Build(&state);
  plan.layer[1].role = kActionBgLayerRole_Playfield;
  CHECK(ActionBgPlan_PlayfieldLayer(&plan) == -1);
  CHECK(ActionBgPlan_PrimaryLayer(&plan) == -1);
  CHECK(ActionBgPlan_CanvasOwner(&plan) == -1);

  state = State(7, 8);
  state.layer[0].world_width = 256;
  state.layer[1].world_width = 256;
  plan = Build(&state);
  CHECK(ActionBgPlan_PlayfieldLayer(&plan) == -1);
  CHECK(ActionBgPlan_PrimaryLayer(&plan) == 0);
  CHECK(ActionBgPlan_CanvasOwner(&plan) == -1);

  ActionBgPlan_InitNative(&plan);
  CHECK(ActionBgPlan_PlayfieldLayer(&plan) == -1);
  CHECK(ActionBgPlan_PrimaryLayer(&plan) == -1);
  CHECK(ActionBgPlan_CanvasOwner(&plan) == -1);
  CHECK(ActionBgPlan_PlayfieldLayer(NULL) == -1);
  CHECK(ActionBgPlan_PrimaryLayer(NULL) == -1);
  CHECK(ActionBgPlan_CanvasOwner(NULL) == -1);
}

static void TestOrdinaryWorldAndNativeSource(void) {
  ActionBgFrameState state = State(1, 1);
  ActionBgPlan plan = Build(&state);
  CHECK(plan.layer[0].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[1].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[0].role == kActionBgLayerRole_Playfield);
  CHECK(plan.layer[1].role == kActionBgLayerRole_Backdrop);
  CHECK(plan.layer[0].default_edge == kActionBgEdge_LiveWorld);
  CHECK(plan.bound_canvas_to_world);
  CHECK(plan.layer[0].horizontal_extent.mode == kActionBgExtent_Available);
  CHECK(plan.layer[0].vertical_extent.mode == kActionBgExtent_Available);
  ActionBgPresentationPolicy policy = Compile(&plan);
  CHECK(!policy.clamp_layers && !policy.mirror_layers &&
        !policy.repeat_layers && policy.bound_canvas_to_world);

  state.layer[1].bgsc = 0x70;
  plan = Build(&state);
  CHECK(plan.layer[1].source == kActionBgSource_NativeTilemap);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_RawWrap);

  plan.layer[0].default_edge = kActionBgEdge_Transparent;
  CHECK(Compile(&plan).clamp_layers == 1);
}

static void TestMarahnaCyclicBackdrop(void) {
  ActionBgFrameState state = State(5, 1);
  state.layer[0].world_width = 2048;
  state.layer[0].world_height = 512;
  state.layer[0].camera_x = 543;
  state.layer[1].world_width = 512;
  state.layer[1].world_height = 512;
  state.layer[1].camera_x = 543;

  ActionBgPlan plan = Build(&state);
  CHECK(plan.layer[0].source == kActionBgSource_WorldMap);
  CHECK(!plan.layer[0].wrap_world_x);
  CHECK(plan.layer[1].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[1].wrap_world_x);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_LiveWorld);
  ActionBgPresentationPolicy policy = Compile(&plan);
  CHECK(!policy.clamp_layers && !policy.mirror_layers &&
        !policy.repeat_layers);

  /* The relationship persists across subsection changes; no individual $19
   * value is part of the classifier. */
  state.map_number = 2;
  plan = Build(&state);
  CHECK(plan.layer[1].wrap_world_x);

  /* Each structural input is required, preventing a merely 512px background
   * from being promoted to an authored cycle. */
  state.layer[1].camera_x = 542;
  plan = Build(&state);
  CHECK(!plan.layer[1].wrap_world_x);
  state.layer[1].camera_x = 543;
  state.layer[0].world_width = 512;
  plan = Build(&state);
  CHECK(!plan.layer[1].wrap_world_x);
  state.layer[0].world_width = 2048;
  state.layer[1].world_width = 768;
  plan = Build(&state);
  CHECK(!plan.layer[1].wrap_world_x);
  state.layer[1].world_width = 512;
  state.map_group = 1;
  state.map_number = 1;
  plan = Build(&state);
  CHECK(!plan.layer[1].wrap_world_x);

  /* Cyclic lookup is a decoded-world property, never a viewport/native
   * presentation spelling. */
  state.map_group = 5;
  state.map_number = 2;
  plan = Build(&state);
  plan.layer[1].wrap_world_x = true;
  plan.layer[1].source = kActionBgSource_AuthenticViewport;
  CHECK(!ActionBgPlan_Validate(&plan));
}

static void TestMarahnaMap5BackdropExtent(void) {
  ActionBgFrameState state = State(5, 5);
  state.layer[0].world_width = 2048;
  state.layer[0].world_height = 512;
  state.layer[0].camera_x = 543;
  state.layer[1].world_width = 512;
  state.layer[1].world_height = 512;
  state.layer[1].camera_x = 543;

  ActionBgPlan plan = Build(&state);
  CHECK(plan.layer[0].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[0].default_edge == kActionBgEdge_LiveWorld);
  CHECK(plan.layer[0].horizontal_extent.mode == kActionBgExtent_Available);
  CHECK(plan.layer[1].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[1].wrap_world_x);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Repeat);
  CHECK(plan.layer[1].default_motion == kActionBgMotion_FillRelative);
  CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Fixed);
  CHECK(plan.layer[1].horizontal_extent.left == 128 &&
        plan.layer[1].horizontal_extent.right == 128);
  CHECK(plan.layer[1].vertical_extent.mode == kActionBgExtent_Available);
  CHECK(plan.layer[1].band_count == 0);
  ActionBgPresentationPolicy policy = Compile(&plan);
  CHECK(policy.repeat_layers == 2 && policy.normal_scroll_layers == 0);

  state.decorative_padding_enabled = false;
  plan = Build(&state);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_LiveWorld);
  CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Available);
}

static void TestFillmoreAct1BackdropExtent(void) {
  ActionBgFrameState state = State(1, 1);
  ActionBgPlan plan = Build(&state);
  CHECK(plan.layer[0].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[0].default_edge == kActionBgEdge_LiveWorld);
  CHECK(plan.layer[0].horizontal_extent.mode == kActionBgExtent_Available);
  CHECK(plan.layer[1].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_LiveWorld);
  CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Fixed);
  CHECK(plan.layer[1].horizontal_extent.left == 128 &&
        plan.layer[1].horizontal_extent.right == 128);
  CHECK(plan.layer[1].vertical_extent.mode == kActionBgExtent_Available);

  state.decorative_padding_enabled = false;
  plan = Build(&state);
  CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Available);

  state.decorative_padding_enabled = true;
  state.layer[1].bgsc = 0x70;
  plan = Build(&state);
  CHECK(plan.layer[1].source == kActionBgSource_NativeTilemap);
  CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Available);
}

static void TestNarrowDecorativeBg2(void) {
  ActionBgFrameState state = State(3, 1);
  state.layer[1].world_width = 256;
  state.layer[1].bgsc = 0x70;
  ActionBgPlan plan = Build(&state);
  CHECK(plan.layer[1].source == kActionBgSource_AuthenticViewport);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Mirror);
  CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Fixed);
  CHECK(!plan.layer[1].horizontal_extent.left &&
        !plan.layer[1].horizontal_extent.right);
  CHECK(Compile(&plan).mirror_layers == 2);

  state.decorative_padding_enabled = false;
  plan = Build(&state);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Clamp);
  CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Available);
  CHECK(Compile(&plan).clamp_layers == 2);

  const uint8_t repeat_cases[][2] = {
    /* Moving cloud/snow families retained from the legacy widescreen policy. */
    { 4, 1 }, { 4, 2 }, { 4, 3 },
    { 6, 1 }, { 6, 2 }, { 6, 3 }, { 6, 4 }, { 6, 5 }, { 6, 8 },
    { 7, 2 }, { 7, 3 }, { 7, 4 }, { 7, 5 }, { 7, 6 }, { 7, 7 },
  };
  for (size_t i = 0; i < sizeof(repeat_cases) / sizeof(repeat_cases[0]); i++) {
    state = State(repeat_cases[i][0], repeat_cases[i][1]);
    state.layer[1].world_width = 256;
    plan = Build(&state);
    CHECK(plan.layer[1].default_edge == kActionBgEdge_Repeat);
    CHECK(plan.layer[1].horizontal_extent.mode ==
          kActionBgExtent_Available);
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
    CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Fixed);
  }
}

static void TestAitosWaterfallVerticalExtent(void) {
  ActionBgFrameState state = State(4, 2);
  /* The captured PPU uses a native 32x32 map (`bgsc=$70`) even though the
   * decoded record declares a 512px world. Preserve that source/edge and
   * promote only the live-tuned vertical budget. */
  state.layer[1].bgsc = 0x70;
  state.layer[1].world_width = 512;
  state.layer[1].world_height = 512;
  ActionBgPlan plan = Build(&state);
  CHECK(plan.layer[1].source == kActionBgSource_NativeTilemap);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_RawWrap);
  CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Available);
  CHECK(plan.layer[1].vertical_extent.mode == kActionBgExtent_Fixed);
  CHECK(plan.layer[1].vertical_extent.top == 24);
  CHECK(plan.layer[1].vertical_extent.bottom == 24);

  /* Source/edge guards prevent the tuning from silently reclassifying a
   * future world-backed layout or a neighbouring Aitos room. */
  state.layer[1].bgsc = 0x73;
  plan = Build(&state);
  CHECK(plan.layer[1].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[1].vertical_extent.mode == kActionBgExtent_Available);
  state = State(4, 3);
  state.layer[1].bgsc = 0x70;
  plan = Build(&state);
  CHECK(plan.layer[1].vertical_extent.mode == kActionBgExtent_Available);
}

static void TestBloodpoolBand(void) {
  ActionBgFrameState state = State(2, 1);
  state.layer[1].world_width = 256;
  ActionBgPlan plan = Build(&state);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Mirror);
  CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Fixed);
  CHECK(plan.layer[1].horizontal_extent.left == 76 &&
        plan.layer[1].horizontal_extent.right == 100);
  CHECK(plan.layer[1].band_count == 1);
  CHECK(plan.layer[1].bands[0].y0 == 136 &&
        plan.layer[1].bands[0].y1 == 224 &&
        plan.layer[1].bands[0].edge == kActionBgEdge_Repeat);
  CHECK(plan.layer[1].bands[0].horizontal_extent.mode ==
        kActionBgExtent_Available);
  ActionBgRowPolicy row;
  CHECK(ActionBgLayerPlan_ResolveRow(&plan.layer[1], 100, &row));
  CHECK(row.edge == kActionBgEdge_Mirror);
  CHECK(row.horizontal_extent.mode == kActionBgExtent_Fixed);
  CHECK(row.horizontal_extent.left == 76 &&
        row.horizontal_extent.right == 100);
  CHECK(ActionBgLayerPlan_ResolveRow(&plan.layer[1], 180, &row));
  CHECK(row.edge == kActionBgEdge_Repeat);
  CHECK(row.horizontal_extent.mode == kActionBgExtent_Available);
  ActionBgPresentationPolicy policy = Compile(&plan);
  CHECK(policy.mirror_layers == 2 && policy.band_count == 1);
  CHECK(PresentationBandIs(
      &policy, 0, 1, 136, 224, kActionBgEdge_Repeat));

  state.decorative_padding_enabled = false;
  plan = Build(&state);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Clamp);
  CHECK(plan.layer[1].band_count == 0);

  state = State(2, 2);
  state.layer[1].world_width = 256;
  plan = Build(&state);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Mirror);
  CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Fixed);
  CHECK(plan.layer[1].horizontal_extent.left == 68 &&
        plan.layer[1].horizontal_extent.right == 68);
  CHECK(plan.layer[1].band_count == 1);
  CHECK(plan.layer[1].bands[0].y0 == 136 &&
        plan.layer[1].bands[0].y1 == 224 &&
        plan.layer[1].bands[0].horizontal_extent.mode ==
            kActionBgExtent_Inherit);
  CHECK(ActionBgLayerPlan_ResolveRow(&plan.layer[1], 180, &row));
  CHECK(row.edge == kActionBgEdge_Repeat);
  CHECK(row.horizontal_extent.mode == kActionBgExtent_Fixed);
  CHECK(row.horizontal_extent.left == 68 &&
        row.horizontal_extent.right == 68);
}

static void TestBloodpoolUnbandedBackdropExtents(void) {
  static const struct {
    uint8_t map;
    uint16_t extent;
  } cases[] = {
    { 6, 68 },
    { 7, 92 },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    ActionBgFrameState state = State(2, cases[i].map);
    state.layer[1].world_width = 256;
    ActionBgPlan plan = Build(&state);
    CHECK(plan.layer[0].source == kActionBgSource_WorldMap);
    CHECK(plan.layer[0].horizontal_extent.mode == kActionBgExtent_Available);
    CHECK(plan.layer[1].source == kActionBgSource_AuthenticViewport);
    CHECK(plan.layer[1].default_edge == kActionBgEdge_Mirror);
    CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Fixed);
    CHECK(plan.layer[1].horizontal_extent.left == cases[i].extent &&
          plan.layer[1].horizontal_extent.right == cases[i].extent);
    CHECK(plan.layer[1].vertical_extent.mode == kActionBgExtent_Available);
    CHECK(plan.layer[1].band_count == 0);
    for (int band = 0; band < kActionBgMaxBands; band++)
      CHECK(BandIsClear(&plan.layer[1].bands[band]));
  }

  ActionBgFrameState state = State(2, 8);
  state.layer[1].world_width = 256;
  ActionBgPlan plan = Build(&state);
  CHECK(plan.layer[0].role == kActionBgLayerRole_Playfield);
  CHECK(plan.layer[0].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[0].default_edge == kActionBgEdge_Mirror);
  CHECK(plan.layer[0].default_motion == kActionBgMotion_FillRelative);
  CHECK(plan.layer[0].horizontal_extent.mode == kActionBgExtent_Fixed);
  CHECK(plan.layer[0].horizontal_extent.left == 16 &&
        plan.layer[0].horizontal_extent.right == 16);
  CHECK(plan.layer[0].vertical_extent.mode == kActionBgExtent_Available);
  CHECK(plan.layer[0].band_count == 0);
  CHECK(plan.layer[1].role == kActionBgLayerRole_Backdrop);
  CHECK(plan.layer[1].source == kActionBgSource_AuthenticViewport);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Mirror);
  CHECK(plan.layer[1].default_motion == kActionBgMotion_FillRelative);
  CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Fixed);
  CHECK(plan.layer[1].horizontal_extent.left == 0 &&
        plan.layer[1].horizontal_extent.right == 0);
  CHECK(plan.layer[1].vertical_extent.mode == kActionBgExtent_Available);
  CHECK(plan.layer[1].band_count == 0);
  ActionBgPresentationPolicy policy = Compile(&plan);
  CHECK(policy.mirror_layers == 3);
  CHECK(policy.normal_scroll_layers == 0 && policy.band_count == 0);
}

static void TestKasandoraHybridBackdrop(void) {
  static const struct {
    uint8_t map;
    uint16_t camera_y;
    uint16_t dune_y;
  } cases[] = {
    { 1, 173, 82 },
    { 2, 162, 93 },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    ActionBgFrameState state = State(3, cases[i].map);
    state.layer[1].camera_y = cases[i].camera_y;
    state.layer[1].world_height = 512;
    ActionBgPlan plan = Build(&state);
    CHECK(plan.layer[0].source == kActionBgSource_WorldMap);
    CHECK(plan.layer[0].default_edge == kActionBgEdge_LiveWorld);
    CHECK(plan.layer[1].source == kActionBgSource_AuthenticViewport);
    CHECK(plan.layer[1].default_edge == kActionBgEdge_Mirror);
    CHECK(plan.layer[1].default_motion == kActionBgMotion_FillRelative);
    CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Fixed);
    CHECK(plan.layer[1].horizontal_extent.left == 128 &&
          plan.layer[1].horizontal_extent.right == 128);
    CHECK(plan.layer[1].vertical_extent.mode == kActionBgExtent_Available);
    CHECK(plan.layer[1].band_count == 1);
    CHECK(plan.layer[1].bands[0].y0 == 256 &&
          plan.layer[1].bands[0].y1 == 512 &&
          plan.layer[1].bands[0].anchor == kActionBgBandAnchor_World &&
          plan.layer[1].bands[0].edge == kActionBgEdge_Repeat);
    CHECK(plan.layer[1].bands[0].horizontal_extent.mode ==
          kActionBgExtent_Available);
    ActionBgRowPolicy row;
    CHECK(ActionBgLayerPlan_ResolveRow(
        &plan.layer[1], cases[i].dune_y - 1, &row));
    CHECK(row.edge == kActionBgEdge_Mirror);
    CHECK(ActionBgLayerPlan_ResolveRow(
        &plan.layer[1], cases[i].dune_y, &row));
    CHECK(row.edge == kActionBgEdge_Repeat);
    ActionBgPresentationPolicy policy = Compile(&plan);
    CHECK(policy.mirror_layers == 2 && policy.band_count == 1);
    CHECK(PresentationBandIs(&policy, 0, 1, cases[i].dune_y, 224,
                             kActionBgEdge_Repeat));
  }

  ActionBgFrameState state = State(3, 1);
  state.layer[1].world_height = 512;
  state.layer[1].camera_y = 0;
  ActionBgPlan plan = Build(&state);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Mirror);
  CHECK(plan.layer[1].band_count == 1);
  CHECK(Compile(&plan).band_count == 0);

  state.layer[1].camera_y = 255;
  plan = Build(&state);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Mirror);
  CHECK(plan.layer[1].band_count == 1);
  ActionBgPresentationPolicy full_dunes = Compile(&plan);
  CHECK(full_dunes.mirror_layers == 2 && full_dunes.band_count == 1);
  CHECK(PresentationBandIs(
      &full_dunes, 0, 1, 0, 224, kActionBgEdge_Repeat));

  state.decorative_padding_enabled = false;
  plan = Build(&state);
  CHECK(plan.layer[1].source == kActionBgSource_WorldMap);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_LiveWorld);
  CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Available);
}

static void TestDeathHeimStates(void) {
  ActionBgFrameState state = State(7, 1);
  ActionBgPlan plan = Build(&state);
  CHECK(plan.layer[0].source == kActionBgSource_AuthenticViewport);
  CHECK(plan.layer[0].default_edge == kActionBgEdge_Clamp);
  CHECK(plan.layer[1].default_edge == kActionBgEdge_Clamp);
  CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Fixed);
  CHECK(plan.layer[1].band_count == 1);
  CHECK(plan.layer[1].bands[0].horizontal_extent.mode ==
        kActionBgExtent_Available);
  ActionBgPresentationPolicy policy = Compile(&plan);
  CHECK(policy.clamp_layers == 3 && !policy.bound_canvas_to_world);
  CHECK(!plan.bound_canvas_to_world);
  CHECK(policy.band_count == 1 && PresentationBandIs(
      &policy, 0, 1, 144, 224, kActionBgEdge_Repeat));

  state.death_heim_progress = 7;
  state.layer[0].bgsc = 0x64;
  state.layer[1].bgsc = 0x74;
  plan = Build(&state);
  policy = Compile(&plan);
  CHECK(policy.clamp_layers == 1 && policy.mirror_layers == 2);
  CHECK(!policy.band_count);
  CHECK(plan.layer[1].horizontal_extent.mode == kActionBgExtent_Fixed);

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
  CHECK(plan.layer[0].role == kActionBgLayerRole_Scene);
  CHECK(plan.layer[1].role == kActionBgLayerRole_Backdrop);
  CHECK(plan.layer[0].default_edge == kActionBgEdge_RawWrap);
  CHECK(plan.layer[0].horizontal_extent.mode ==
        kActionBgExtent_Available);
  CHECK(plan.layer[1].horizontal_extent.mode ==
        kActionBgExtent_Available);
  CHECK(!plan.layer[0].band_count && !plan.layer[1].band_count);
  for (int layer = 0; layer < kActionBgPlanLayerCount; layer++)
    for (int band = 0; band < kActionBgMaxBands; band++)
      CHECK(BandIsClear(&plan.layer[layer].bands[band]));
  CHECK(!policy.clamp_layers && !policy.mirror_layers &&
        !policy.repeat_layers && !policy.bound_canvas_to_world);
}

static void TestEveryKnownMapClassifies(void) {
  static const uint8_t last_map[] = { 0, 4, 8, 6, 7, 8, 8, 8 };
  for (uint8_t group = 1; group <= 7; group++) {
    for (uint8_t map = 1; map <= last_map[group]; map++) {
      ActionBgFrameState state = State(group, map);
      if (group == 3 && (map == 1 || map == 2))
        state.layer[1].world_height = 512;
      ActionBgPlan plan;
      CHECK(ActionBgPlan_Build(&state, &plan));
      CHECK(plan.valid);
      CHECK(ActionBgPlan_Validate(&plan));
      CHECK(plan.layer[0].role ==
            (group == 7 && map == 8 ? kActionBgLayerRole_Scene
                                    : kActionBgLayerRole_Playfield));
      CHECK(plan.layer[1].role == kActionBgLayerRole_Backdrop);
      for (unsigned layer = 0; layer < kActionBgPlanLayerCount; layer++) {
        const bool fixed_fillmore_bg2 =
            layer == 1 && group == 1 && map == 1;
        const bool fixed_death_heim_bg2 =
            layer == 1 && group == 7 && map == 1;
        const bool fixed_kasandora_bg2 =
            layer == 1 && group == 3 && (map == 1 || map == 2);
        const bool fixed_marahna_bg2 =
            layer == 1 && group == 5 && map == 5;
        const bool fixed_bloodpool_boss_bg1 =
            layer == 0 && group == 2 && map == 8;
        CHECK(plan.layer[layer].horizontal_extent.mode ==
              (fixed_fillmore_bg2 || fixed_death_heim_bg2 ||
                   fixed_kasandora_bg2 || fixed_marahna_bg2 ||
                   fixed_bloodpool_boss_bg1
                   ? kActionBgExtent_Fixed : kActionBgExtent_Available));
        CHECK(plan.layer[layer].vertical_extent.mode ==
              kActionBgExtent_Available);
      }
    }
  }
}

int main(void) {
  TestValidationAndFallback();
  TestResolvedPresentationProjection();
  TestExtentValidationAndRowResolution();
  TestUnboundWorldFallback();
  TestCanvasOwner();
  TestOrdinaryWorldAndNativeSource();
  TestMarahnaCyclicBackdrop();
  TestMarahnaMap5BackdropExtent();
  TestFillmoreAct1BackdropExtent();
  TestNarrowDecorativeBg2();
  TestAitosWaterfallVerticalExtent();
  TestBloodpoolBand();
  TestBloodpoolUnbandedBackdropExtents();
  TestKasandoraHybridBackdrop();
  TestDeathHeimStates();
  TestEveryKnownMapClassifies();
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("action_bg_plan: OK\n");
  return 0;
}
