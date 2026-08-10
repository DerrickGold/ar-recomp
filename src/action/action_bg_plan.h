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
  uint16_t world_width;
  uint16_t world_height;
  uint16_t tilemap_base;
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

/* An extent limits presentation outside the authentic viewport; it never
 * creates pixels that the selected edge/source cannot provide. `Inherit` is
 * valid only on a row band. `Available` applies no additional cap. */
typedef enum ActionBgExtentMode {
  kActionBgExtent_Inherit = 0,
  kActionBgExtent_Available,
  kActionBgExtent_Fixed,
} ActionBgExtentMode;

typedef struct ActionBgHorizontalExtent {
  ActionBgExtentMode mode;
  uint16_t left;
  uint16_t right;
} ActionBgHorizontalExtent;

typedef struct ActionBgVerticalExtent {
  ActionBgExtentMode mode;
  uint16_t top;
  uint16_t bottom;
} ActionBgVerticalExtent;

typedef struct ActionBgBand {
  uint16_t y0;
  uint16_t y1;
  ActionBgEdgeMode edge;
  /* Inherit by default, so existing edge-only bands retain the layer extent. */
  ActionBgHorizontalExtent horizontal_extent;
} ActionBgBand;

typedef struct ActionBgLayerPlan {
  bool valid;
  ActionBgSourceKind source;
  ActionBgEdgeMode default_edge;
  uint16_t world_width;
  uint16_t world_height;
  ActionBgHorizontalExtent horizontal_extent;
  ActionBgVerticalExtent vertical_extent;
  uint8_t band_count;
  ActionBgBand bands[kActionBgMaxBands];
} ActionBgLayerPlan;

typedef struct ActionBgPlan {
  bool valid;
  bool bound_canvas_to_world;
  ActionBgLayerPlan layer[kActionBgPlanLayerCount];
} ActionBgPlan;

/* Fully resolved policy for one authentic scanline. Signed rows outside
 * 0..223 deliberately use the layer default; bands identify authentic content
 * families, not capture-space rows. */
typedef struct ActionBgRowPolicy {
  ActionBgEdgeMode edge;
  ActionBgHorizontalExtent horizontal_extent;
} ActionBgRowPolicy;

/* Mechanical projection into the generic post-raster PPU policy. Map-specific
 * classification remains in ActionBgPlan; these masks exist only at the PPU
 * setter boundary and for deliberate global/debug presentation overrides. */
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
bool ActionBgLayerPlan_Validate(const ActionBgLayerPlan *layer);
bool ActionBgPlan_Validate(const ActionBgPlan *plan);
bool ActionBgLayerPlan_ResolveRow(const ActionBgLayerPlan *layer,
                                  int authentic_y,
                                  ActionBgRowPolicy *out);
bool ActionBgPlan_CompilePresentation(
    const ActionBgPlan *plan, ActionBgPresentationPolicy *out);
/* Exact native/raw plan used for non-action frames, plus the inverse adapter
 * used only when a global/debug override deliberately replaces the canonical
 * action policy. The adapter retains source/world metadata while replacing all
 * layer edges and bands from `policy`; it never reads PPU state. */
void ActionBgPlan_InitNative(ActionBgPlan *out);
bool ActionBgPlan_ApplyPresentationPolicy(
    ActionBgPlan *plan, const ActionBgPresentationPolicy *policy);
/* Convert planned world layers that did not receive a provider binding into
 * authentic-viewport clamps. Returns the PPU clamp mask to add for this frame.
 * Native/decorative layers and successfully bound world layers are unchanged. */
uint8_t ActionBgPlan_ClampUnboundWorldLayers(
    ActionBgPlan *plan, uint8_t bound_layers, uint8_t visible_layers);
const char *ActionBgSourceKind_Name(ActionBgSourceKind source);
const char *ActionBgEdgeMode_Name(ActionBgEdgeMode edge);
const char *ActionBgExtentMode_Name(ActionBgExtentMode mode);

#endif  /* ACTION_BG_PLAN_H */
