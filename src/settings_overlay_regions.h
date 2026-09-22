#ifndef SETTINGS_OVERLAY_REGIONS_H
#define SETTINGS_OVERLAY_REGIONS_H

#include "localization/ui_catalog.h"
#include "regional/regional_costs.h"
#include "settings_overlay_artwork.h"
#include "actraiser/actraiser_regional_settings.h"

/* Pricing-subset UI adapter, never full regional gameplay presets. No CPU,
 * save I/O or mutable campaign ownership. */
typedef struct SettingsOverlayRegionalHooks {
  bool (*copy)(ActRaiserRegionalPricingView *out);
  ActRaiserRegionalEditResult (*request)(const ActRaiserRegionalPricingView *view,
                                       ArRegionalCostGroup group,
                                       ArRegionalCostSource source);
} SettingsOverlayRegionalHooks;

const char *SettingsOverlayRegions_RowKey(ArRegionalCostGroup group);
const char *SettingsOverlayRegions_RowLabel(ArUiLocale locale, ArRegionalCostGroup group);
const char *SettingsOverlayRegions_EditStatus(ArUiLocale locale, ActRaiserRegionalEditResult result);
bool SettingsOverlayRegions_NextSource(const ArRegionalCostPolicy *policy,
                                      ArRegionalCostGroup group, int direction,
                                      ArRegionalCostSource *source);
bool SettingsOverlayRegions_CostBadge(const ArRegionalCostPolicy *policy,
                                     ArRegionalCostGroup group,
                                     SettingsOverlayRegionBadge *badge);
const char *SettingsOverlayRegions_BadgeLabel(ArUiLocale locale,
                                             SettingsOverlayRegionBadge badge);
bool SettingsOverlayRegions_CostDescription(ArUiLocale locale,
                                           const ArRegionalCostPolicy *policy,
                                           ArRegionalCostGroup group,
                                           char *output, size_t capacity);

#endif
