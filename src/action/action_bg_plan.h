#ifndef ACTION_BG_PLAN_H
#define ACTION_BG_PLAN_H

#include <stdbool.h>
#include <stdint.h>

/* Pure action-background scene policy (SPEC-bg-hle BH3). The planner owns
 * semantic roles, source/edge classification, and presentation extents; live
 * PPU registers still own pixels, raster effects, priority, windows, palette
 * and color math. */

enum {
  kActionBgPlanLayerCount = 2,
  kActionBgMaxBands = 4,
  /* Aitos `$04/$02` may safely expose this many BG2 rows below the authentic
   * 224-line view. The waterfall atmosphere uses the same seam; keeping the
   * policy value public prevents its emitter from drifting back to the
   * authentic edge while the drawable waterfall continues below it. */
  kActionBgAitosWaterfallBottomExtensionPixels = 24,
  kActionBgPresentationBandMax =
      kActionBgPlanLayerCount * kActionBgMaxBands,
};

typedef struct ActionBgLayerState {
  /* Full world camera. X participates in shared-camera topology detection;
   * Y also anchors world-coordinate presentation bands. */
  uint16_t camera_x;
  uint16_t camera_y;
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

/* Fill and apparent horizontal motion are independent. FillRelative preserves
 * the historical renderer exactly: reflecting an already-scrolled scanline
 * also reflects its apparent movement. NormalScroll compensates that reflected
 * sampling phase so a mirrored cloud/water family travels in the same screen
 * direction as the authentic layer. Repeat and non-padding fills are identical
 * in both modes. */
typedef enum ActionBgMotionMode {
  kActionBgMotion_FillRelative = 0,
  kActionBgMotion_NormalScroll,
} ActionBgMotionMode;

/* Screen bands are authored directly in authentic rows [0,224). World bands
 * follow vertical camera/parallax movement and are projected through the
 * layer's canonical camera row each frame. A mixed-anchor table is valid only
 * when its authored order remains non-overlapping across native camera travel,
 * preventing a policy accepted on one frame from crossing on a later frame. */
typedef enum ActionBgBandAnchor {
  kActionBgBandAnchor_Screen = 0,
  kActionBgBandAnchor_World,
} ActionBgBandAnchor;

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
  ActionBgMotionMode motion;
  ActionBgBandAnchor anchor;
  /* Inherit by default, so existing edge-only bands retain the layer extent. */
  ActionBgHorizontalExtent horizontal_extent;
} ActionBgBand;

typedef struct ActionBgLayerPlan {
  bool valid;
  /* The decoded map is one authored horizontal cycle rather than a finite
   * endpoint. Provider tile coordinates wrap by world_width; presentation
   * edges remain independently classified by default_edge/bands. */
  bool wrap_world_x;
  ActionBgLayerRole role;
  ActionBgSourceKind source;
  ActionBgEdgeMode default_edge;
  ActionBgMotionMode default_motion;
  /* Canonical world row sampled immediately above authentic row zero. */
  uint16_t camera_y;
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

/* Fully resolved policy for one authentic or synthetic presentation row.
 * Bands identify authentic content families rather than capture-space rows.
 * A band whose resolved interval owns authentic row 0 or 223 also owns that
 * adjacent synthetic margin, so an edge-anchored moving family continues with
 * the same fill, motion and extent; other outside rows use the layer default. */
typedef struct ActionBgRowPolicy {
  ActionBgEdgeMode edge;
  ActionBgMotionMode motion;
  ActionBgHorizontalExtent horizontal_extent;
} ActionBgRowPolicy;

typedef struct ActionBgPresentationBand {
  uint8_t layer;
  uint8_t y0;
  uint8_t y1;
  ActionBgEdgeMode edge;
  ActionBgMotionMode motion;
} ActionBgPresentationBand;

/* Mechanical projection into the generic post-raster PPU policy. Map-specific
 * classification remains in ActionBgPlan; these masks exist only at the PPU
 * setter boundary and for deliberate global/debug presentation overrides. */
typedef struct ActionBgPresentationPolicy {
  uint8_t clamp_layers;
  uint8_t mirror_layers;
  uint8_t repeat_layers;
  uint8_t normal_scroll_layers;
  uint8_t band_count;
  ActionBgPresentationBand bands[kActionBgPresentationBandMax];
  bool bound_canvas_to_world;
} ActionBgPresentationPolicy;

bool ActionBgPlan_Build(const ActionBgFrameState *state, ActionBgPlan *out);
bool ActionBgLayerPlan_Validate(const ActionBgLayerPlan *layer);
bool ActionBgPlan_Validate(const ActionBgPlan *plan);
bool ActionBgLayerPlan_ResolveRow(const ActionBgLayerPlan *layer,
                                  int authentic_y,
                                  ActionBgRowPolicy *out);
/* Resolve one authored band to un-clipped authentic-screen coordinates. World
 * bands may return rows outside [0,224); callers clip only at their concrete
 * presentation boundary. */
bool ActionBgLayerPlan_ResolveBand(const ActionBgLayerPlan *layer,
                                  unsigned band,
                                  int *screen_y0, int *screen_y1);
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
const char *ActionBgMotionMode_Name(ActionBgMotionMode motion);
const char *ActionBgBandAnchor_Name(ActionBgBandAnchor anchor);
const char *ActionBgExtentMode_Name(ActionBgExtentMode mode);

#endif  /* ACTION_BG_PLAN_H */
