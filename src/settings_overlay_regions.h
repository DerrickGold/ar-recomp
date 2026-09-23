#ifndef SETTINGS_OVERLAY_REGIONS_H
#define SETTINGS_OVERLAY_REGIONS_H

#include "localization/ui_catalog.h"
#include "regional/regional_costs.h"
#include "settings_overlay_artwork.h"
#include "actraiser/actraiser_regional_settings.h"

/* Integrated-rule UI adapter, never full regional gameplay presets. No CPU,
 * save I/O or mutable campaign ownership. */
typedef struct SettingsOverlayRegionalHooks {
  bool (*copy)(ActRaiserRegionalRulesView *out);
  ActRaiserRegionalEditResult (*request)(const ActRaiserRegionalRulesView *view,
                                       ActRaiserRegionalSettingGroup group,
                                       ArRegionalSource source);
  ActRaiserRegionalEditResult (*difficulty)(const ActRaiserRegionalRulesView *view,
                                           ArRegionalDifficulty level);
} SettingsOverlayRegionalHooks;

const char *SettingsOverlayRegions_RowKey(ActRaiserRegionalSettingGroup group);
const char *SettingsOverlayRegions_RowLabel(ArUiLocale locale, ActRaiserRegionalSettingGroup group);
const char *SettingsOverlayRegions_ValueLabel(ArUiLocale locale, const ActRaiserRegionalRulesView *view,
    ActRaiserRegionalSettingGroup group, bool effective);
const char *SettingsOverlayRegions_EditStatus(ArUiLocale locale, ActRaiserRegionalEditResult result);
bool SettingsOverlayRegions_NextSource(const ActRaiserRegionalRulesView *view,
                                      ActRaiserRegionalSettingGroup group, int direction,
                                      ArRegionalSource *source);
bool SettingsOverlayRegions_ViewBadge(const ActRaiserRegionalRulesView *view,
                                      ActRaiserRegionalSettingGroup group, bool effective,
                                      SettingsOverlayRegionBadge *badge);
bool SettingsOverlayRegions_ViewDescription(ArUiLocale locale,
    const ActRaiserRegionalRulesView *view, ActRaiserRegionalSettingGroup group,
    char *output, size_t capacity);
bool SettingsOverlayRegions_PopulationConfirmation(ArUiLocale locale,
    ArRegionalSource source, const uint16_t removed[6], char *output, size_t capacity);
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
