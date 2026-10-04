#ifndef AR_DIORAMA_BG_POLICY_H
#define AR_DIORAMA_BG_POLICY_H

/* Sparse, persisted background-policy overrides shared by game and editor. */
#include "action/action_bg_plan.h"
#include <string.h>

/* Saved room policy is sparse: untouched controls continue to use the planner's
 * defaults. Bands are an explicit replacement, including an empty table. */
typedef struct DioramaBgPolicyOverride {
  bool set_edge, set_motion, set_horizontal, set_vertical, set_bands;
  ActionBgEdgeMode edge;
  ActionBgMotionMode motion;
  ActionBgHorizontalExtent horizontal;
  ActionBgVerticalExtent vertical;
  uint8_t band_count, band_mask;
  ActionBgBand bands[kActionBgMaxBands];
} DioramaBgPolicyOverride;

static inline bool DioramaBgPolicy_IsAuthored(const DioramaBgPolicyOverride *p) {
  return p && (p->set_edge || p->set_motion || p->set_horizontal ||
               p->set_vertical || p->set_bands || p->band_mask);
}

/* Atomic application; validation covers all camera positions, including mixed
 * screen/world bands. Both the game and offline editor use this boundary. */
static inline bool DioramaBgPolicy_Apply(const DioramaBgPolicyOverride edits[2],
                                        ActionBgPlan *plan) {
  if (!edits || !plan) return false;
  ActionBgPlan next = *plan;
  for (unsigned bg = 0; bg < 2; bg++) {
    const DioramaBgPolicyOverride *p = &edits[bg];
    ActionBgLayerPlan *layer = &next.layer[bg];
    if (p->set_edge) layer->default_edge = p->edge;
    if (p->set_motion) layer->default_motion = p->motion;
    if (p->set_horizontal) layer->horizontal_extent = p->horizontal;
    if (p->set_vertical) layer->vertical_extent = p->vertical;
    if (p->band_mask && !p->set_bands) return false;
    if (p->set_bands) {
      if (p->band_count > kActionBgMaxBands ||
          p->band_mask != (1u << p->band_count) - 1u) return false;
      layer->band_count = p->band_count;
      memset(layer->bands, 0, sizeof(layer->bands));
      memcpy(layer->bands, p->bands, p->band_count * sizeof(p->bands[0]));
    }
  }
  if (!ActionBgPlan_Validate(&next)) return false;
  *plan = next;
  return true;
}

#endif  /* AR_DIORAMA_BG_POLICY_H */
