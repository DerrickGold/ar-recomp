#ifndef ACTION_BG_PLAN_H
#define ACTION_BG_PLAN_H

#include <stdbool.h>
#include <stdint.h>

/* Pure action-background scene policy (SPEC-bg-hle BH3). The planner owns
 * source and edge classification only; live PPU registers still own pixels,
 * raster effects, priority, windows, palette and color math. */

enum {
  kActionBgPlanLayerCount = 2,
  kActionBgMaxBands = 4,
};

typedef struct ActionBgLayerState {
  uint16_t camera_x;
  uint16_t camera_y;
  uint16_t world_width;
  uint16_t world_height;
  uint16_t map_page;
  uint16_t tilemap_base;
  uint16_t metatile_table;
  uint16_t word_mask;
  uint8_t attributes;
  uint8_t bgsc;
} ActionBgLayerState;

typedef struct ActionBgFrameState {
  ActionBgLayerState layer[kActionBgPlanLayerCount];
  uint8_t map_group;
  uint8_t map_number;
  uint8_t death_heim_progress;
  uint8_t death_heim_ending_state;
  bool decorative_padding_enabled;
} ActionBgFrameState;

typedef enum ActionBgSourceKind {
  kActionBgSource_NativeTilemap = 0,
  kActionBgSource_WorldMap,
  kActionBgSource_AuthenticViewport,
} ActionBgSourceKind;

typedef enum ActionBgEdgeMode {
  kActionBgEdge_Transparent = 0,
  kActionBgEdge_LiveWorld,
  kActionBgEdge_Clamp,
  kActionBgEdge_Mirror,
  kActionBgEdge_Repeat,
  kActionBgEdge_RawWrap,
} ActionBgEdgeMode;

typedef struct ActionBgBand {
  uint16_t y0;
  uint16_t y1;
  ActionBgEdgeMode edge;
} ActionBgBand;

typedef struct ActionBgLayerPlan {
  bool valid;
  ActionBgSourceKind source;
  ActionBgEdgeMode default_edge;
  uint16_t world_width;
  uint16_t world_height;
  uint8_t band_count;
  ActionBgBand bands[kActionBgMaxBands];
} ActionBgLayerPlan;

typedef struct ActionBgPlan {
  bool valid;
  bool bound_canvas_to_world;
  ActionBgLayerPlan layer[kActionBgPlanLayerCount];
} ActionBgPlan;

/* Migration projection into the existing post-raster PPU policy. FrameSlot now
 * carries ActionBgPlan itself; this mask adapter remains only at the PPU setter
 * boundary until the behavior-neutral BH8 cleanup. */
typedef struct ActionBgPresentationPolicy {
  uint8_t clamp_layers;
  uint8_t mirror_layers;
  uint8_t repeat_layers;
  int8_t repeat_band_layer;
  uint8_t repeat_band_y0;
  uint8_t repeat_band_y1;
  bool bound_canvas_to_world;
} ActionBgPresentationPolicy;

bool ActionBgPlan_Build(const ActionBgFrameState *state, ActionBgPlan *out);
bool ActionBgPlan_CompilePresentation(
    const ActionBgPlan *plan, ActionBgPresentationPolicy *out);
/* Exact native/raw plan used for non-action frames, plus the inverse adapter
 * used only when a global/debug override deliberately replaces the canonical
 * action policy. The adapter retains source/world metadata while replacing all
 * layer edges and bands from `policy`; it never reads PPU state. */
void ActionBgPlan_InitNative(ActionBgPlan *out);
bool ActionBgPlan_ApplyPresentationPolicy(
    ActionBgPlan *plan, const ActionBgPresentationPolicy *policy);
const char *ActionBgSourceKind_Name(ActionBgSourceKind source);

#endif  /* ACTION_BG_PLAN_H */
