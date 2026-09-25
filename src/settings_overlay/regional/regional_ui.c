#include "settings_overlay/regional/regional_ui.h"

#include <stdio.h>

bool SettingsOverlayRegions_EditWarning(ArUiLocale locale, const char *label,
                                        ArRegionalSource source,
                                        const ActRaiserRegionalEditImpact *impact, char *out,
                                        size_t capacity) {
  if (!label || !impact || (unsigned)source >= kArRegionalSource_Count) return false;
  const char *key = impact->towns == kArRegionalTownImpact_Redevelopment
                        ? "overlay.region.warning.redevelopment"
                        : "overlay.region.warning.future";
  const SettingsOverlayRegionBadge badge = source == kArRegionalSource_US ? kOverlayRegionBadge_US
                                           : source == kArRegionalSource_Japan
                                               ? kOverlayRegionBadge_Japan
                                               : kOverlayRegionBadge_Europe;
  const ArUiTextArgument args[] = {
      {"setting", label},
      {"region", SettingsOverlayRegions_BadgeLabel(locale, badge)},
      {"history", impact->estimated_history
                      ? ArUiCatalog_Text(locale, "overlay.region.warning.estimated", NULL)
                      : ""},
  };
  return ArUiCatalog_Format(out, capacity, ArUiCatalog_Text(locale, key, NULL), args, 3);
}

const char *SettingsOverlayRegions_BadgeCode(ArUiLocale locale, SettingsOverlayRegionBadge badge) {
  switch (badge) {
    case kOverlayRegionBadge_US:
      return "US";
    case kOverlayRegionBadge_Japan:
      return "JP";
    case kOverlayRegionBadge_Europe:
      return "EU";
    default:
      return SettingsOverlayRegions_BadgeLabel(locale, badge);
  }
}

bool SettingsOverlayRegions_PopulationConfirmation(ArUiLocale locale, ArRegionalSource source,
                                                   const uint16_t removed[6], char *output,
                                                   size_t capacity) {
  if ((unsigned)source >= kArRegionalSource_Count || !removed) return false;
  char towns[512] = {0};
  size_t used = 0;
  for (unsigned town = 0; town < 6; ++town)
    if (removed[town]) {
      char key[32];
      snprintf(key, sizeof(key), "overlay.region.town.%u", town);
      const int count = snprintf(towns + used, sizeof(towns) - used, "%s%s %u", used ? ", " : "",
                                 ArUiCatalog_Text(locale, key, NULL), removed[town]);
      if (count < 0 || (size_t)count >= sizeof(towns) - used) return false;
      used += (size_t)count;
    }
  const SettingsOverlayRegionBadge badge =
      source == kArRegionalSource_Japan    ? kOverlayRegionBadge_Japan
      : source == kArRegionalSource_Europe ? kOverlayRegionBadge_Europe
                                           : kOverlayRegionBadge_US;
  const ArUiTextArgument args[] = {
      {"region", SettingsOverlayRegions_BadgeLabel(locale, badge)},
      {"towns", used ? towns : ArUiCatalog_Text(locale, "overlay.region.population_none", NULL)}};
  return ArUiCatalog_Format(output, capacity,
                            ArUiCatalog_Text(locale, "overlay.region.population_confirm", NULL),
                            args, 2);
}

const char *SettingsOverlayRegions_EditStatus(ArUiLocale locale,
                                              ActRaiserRegionalEditResult result) {
  switch (result) {
    case kActRaiserRegionalEdit_Applied:
    case kActRaiserRegionalEdit_Unchanged:
      return ArUiCatalog_Text(locale, "overlay.region.saved_with_story", NULL);
    case kActRaiserRegionalEdit_Locked:
      return ArUiCatalog_Text(locale, "overlay.region.replay_locked", NULL);
    case kActRaiserRegionalEdit_Stale:
      return ArUiCatalog_Text(locale, "overlay.region.stale", NULL);
    case kActRaiserRegionalEdit_HistoryUnavailable:
      return ArUiCatalog_Text(locale, "overlay.region.lair_unavailable", NULL);
    case kActRaiserRegionalEdit_Deferred:
      return ArUiCatalog_Text(locale, "overlay.region.population_pending", NULL);
    case kActRaiserRegionalEdit_Incompatible:
      return ArUiCatalog_Text(locale, "overlay.region.population_incompatible", NULL);
    case kActRaiserRegionalEdit_SaveFailed:
      return ArUiCatalog_Text(locale, "overlay.region.save_failed", NULL);
    case kActRaiserRegionalEdit_RequiresGame:
      return ArUiCatalog_Text(locale, "overlay.region.continue_for_redevelopment", NULL);
    default:
      return ArUiCatalog_Text(locale, "overlay.status.unavailable", NULL);
  }
}

const char *SettingsOverlayRegions_BadgeLabel(ArUiLocale locale, SettingsOverlayRegionBadge badge) {
  switch (badge) {
    case kOverlayRegionBadge_US:
      return ArUiCatalog_Text(locale, "overlay.region.us", "US");
    case kOverlayRegionBadge_Japan:
      return ArUiCatalog_Text(locale, "overlay.region.japan", "Japan");
    case kOverlayRegionBadge_Europe:
      return ArUiCatalog_Text(locale, "overlay.region.europe", "Europe");
    case kOverlayRegionBadge_Mixed:
      return ArUiCatalog_Text(locale, "overlay.region.custom", "Custom");
    default:
      return "";
  }
}
