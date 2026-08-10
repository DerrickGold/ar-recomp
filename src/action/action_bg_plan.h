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

/* Semantic ownership is independent of source mechanics. It lets global
 * canvas policy grow a verified playfield without assuming that a particular
 * PPU layer always serves that role. Non-action/native projections remain
 * unclassified and therefore cannot opt themselves into canvas growth. */
typedef enum ActionBgLayerRole {
  kActionBgLayerRole_Unclassified = 0,
  kActionBgLayerRole_Playfield,
  kActionBgLayerRole_Scene,
  kActionBgLayerRole_Backdrop,
} ActionBgLayerRole;

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
  ActionBgLayerRole role;
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
/* Return the unique semantic playfield layer, or -1 for invalid, absent, or
 * ambiguous plans. Unlike CanvasOwner this does not require a finite provider
 * source or an active horizontal world bound. */
int ActionBgPlan_PlayfieldLayer(const ActionBgPlan *plan);
/* Return the unique primary plane that anchors vertical scene growth. Ordinary
 * action rooms use their playfield; special native raster rooms may nominate a
 * scene plane without falsely classifying it as platform art. */
int ActionBgPlan_PrimaryLayer(const ActionBgPlan *plan);
/* Return the unique finite-world layer authorized to bound the global action
 * canvas, or -1 when the plan is invalid, unbounded, ambiguous, or has no
 * provider-backed playfield. */
int ActionBgPlan_CanvasOwner(const ActionBgPlan *plan);
const char *ActionBgSourceKind_Name(ActionBgSourceKind source);
const char *ActionBgLayerRole_Name(ActionBgLayerRole role);
const char *ActionBgEdgeMode_Name(ActionBgEdgeMode edge);
const char *ActionBgExtentMode_Name(ActionBgExtentMode mode);

#endif  /* ACTION_BG_PLAN_H */
