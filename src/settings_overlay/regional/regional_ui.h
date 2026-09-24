#ifndef SETTINGS_OVERLAY_REGIONAL_UI_H
#define SETTINGS_OVERLAY_REGIONAL_UI_H

#include "localization/ui_catalog.h"
#include "regional/regional_costs.h"
#include "settings_overlay_artwork.h"
#include "actraiser/regional/actraiser_regional_settings.h"

/* Value-copy UI adapter. No CPU,
 * save I/O or mutable campaign ownership. */
typedef struct SettingsOverlayRegionalHooks {
  bool (*copy)(ActRaiserRegionalRulesView *out);
  ActRaiserRegionalEditResult (*request)(const ActRaiserRegionalRulesView *view,
                                         ArRegionalProfileGroup group, ArRegionalSource source);
  ActRaiserRegionalEditResult (*difficulty)(const ActRaiserRegionalRulesView *view,
                                            ArRegionalDifficultyChoice choice);
  ActRaiserRegionalEditResult (*setting)(const ActRaiserRegionalRulesView *view,
      ActRaiserRegionalSettingGroup group, ArRegionalSource source);
  ActRaiserRegionalEditResult (*preview_setting)(const ActRaiserRegionalRulesView *view,
      ActRaiserRegionalSettingGroup group, ArRegionalSource source, ActRaiserRegionalEditImpact *out);
  ActRaiserRegionalEditResult (*preview)(const ActRaiserRegionalRulesView *view,
                                         ArRegionalProfileGroup group, ArRegionalSource source,
                                         ActRaiserRegionalEditImpact *out);
} SettingsOverlayRegionalHooks;

const char *SettingsOverlayRegions_EditStatus(ArUiLocale locale,
                                              ActRaiserRegionalEditResult result);
bool SettingsOverlayRegions_PopulationConfirmation(ArUiLocale locale, ArRegionalSource source,
                                                   const uint16_t removed[6], char *output,
                                                   size_t capacity);
const char *SettingsOverlayRegions_BadgeLabel(ArUiLocale locale, SettingsOverlayRegionBadge badge);
/* Compact row values; full names remain in help and confirmations. */
const char *SettingsOverlayRegions_BadgeCode(ArUiLocale locale, SettingsOverlayRegionBadge badge);
bool SettingsOverlayRegions_EditWarning(ArUiLocale locale, const char *label,
                                        ArRegionalSource source,
                                        const ActRaiserRegionalEditImpact *impact, char *out,
                                        size_t capacity);

#endif
