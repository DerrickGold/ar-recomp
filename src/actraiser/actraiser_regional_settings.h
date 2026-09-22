#ifndef ACTRAISER_REGIONAL_SETTINGS_H
#define ACTRAISER_REGIONAL_SETTINGS_H

#include "regional/regional_costs.h"

/* Game-thread settings boundary. UI gets value copies and an optimistic edit
 * token, never a session pointer, CPU/WRAM, or save-path ownership. Pricing is
 * only the currently integrated subset, not a full regional preset. */
typedef struct ActRaiserRegionalPricingView {
  uint8_t campaign[16];
  uint32_t revision;
  ArRegionalCostPolicy requested, effective;
  bool editable;
  bool miracle_in_progress;
} ActRaiserRegionalPricingView;

typedef enum ActRaiserRegionalEditResult {
  kActRaiserRegionalEdit_Invalid,
  kActRaiserRegionalEdit_Locked,
  kActRaiserRegionalEdit_Stale,
  kActRaiserRegionalEdit_Unchanged,
  kActRaiserRegionalEdit_Applied,
} ActRaiserRegionalEditResult;

/* False until an accepted New Game/Continue, without modifying output. */
bool ActRaiserRegional_CopyPricingView(ActRaiserRegionalPricingView *out);
/* The view's campaign/revision identifies the requested edit; its editable
 * and policy fields are display data, not authority. Validate again on apply.
 * Changes persist with the next completed native story save. */
ActRaiserRegionalEditResult ActRaiserRegional_RequestPricing(
    const ActRaiserRegionalPricingView *view, ArRegionalCostGroup group,
    ArRegionalCostSource source);

#endif
