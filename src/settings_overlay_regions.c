#include "settings_overlay_regions.h"

#include <stdio.h>

const char *SettingsOverlayRegions_RowKey(ArRegionalCostGroup group) {
  switch (group) {
    case kArRegionalCostGroup_Scrolls: return "regional_scroll_prices";
    case kArRegionalCostGroup_Miracles: return "regional_miracle_prices";
    default: return "";
  }
}

const char *SettingsOverlayRegions_RowLabel(ArUiLocale locale, ArRegionalCostGroup group) {
  switch (group) {
    case kArRegionalCostGroup_Scrolls:
      return ArUiCatalog_Text(locale, "overlay.region.scroll_label", "Spell scroll costs");
    case kArRegionalCostGroup_Miracles:
      return ArUiCatalog_Text(locale, "overlay.region.miracle_label", "Miracle SP costs");
    default: return "";
  }
}

const char *SettingsOverlayRegions_EditStatus(ArUiLocale locale, ActRaiserRegionalEditResult result) {
  switch (result) {
    case kActRaiserRegionalEdit_Applied:
    case kActRaiserRegionalEdit_Unchanged:
      return ArUiCatalog_Text(locale, "overlay.region.saved_with_story", NULL);
    case kActRaiserRegionalEdit_Locked:
      return ArUiCatalog_Text(locale, "overlay.region.replay_locked", NULL);
    case kActRaiserRegionalEdit_Stale:
      return ArUiCatalog_Text(locale, "overlay.region.stale", NULL);
    default: return ArUiCatalog_Text(locale, "overlay.status.unavailable", NULL);
  }
}

bool SettingsOverlayRegions_NextSource(const ArRegionalCostPolicy *policy,
                                      ArRegionalCostGroup group, int direction,
                                      ArRegionalCostSource *source) {
  SettingsOverlayRegionBadge badge;
  if (!source || !SettingsOverlayRegions_CostBadge(policy, group, &badge)) return false;
  ArRegionalCostSource current;
  if (!ArRegionalCosts_GroupSource(policy, group, &current)) {
    *source = direction < 0 ? kArRegionalCost_Europe : kArRegionalCost_US;
  } else {
    *source = (ArRegionalCostSource)((current +
        (direction < 0 ? kArRegionalCostSource_Count - 1 : 1)) % kArRegionalCostSource_Count);
  }
  return true;
}

bool SettingsOverlayRegions_CostBadge(const ArRegionalCostPolicy *policy,
                                     ArRegionalCostGroup group,
                                     SettingsOverlayRegionBadge *badge) {
  ArRegionalCostSnapshot snapshot;
  if (!badge || (unsigned)group >= kArRegionalCostGroup_Count ||
      !ArRegionalCosts_Resolve(policy, &snapshot)) return false;
  ArRegionalCostSource source;
  if (!ArRegionalCosts_GroupSource(policy, group, &source)) {
    *badge = kOverlayRegionBadge_Mixed;
    return true;
  }
  switch (source) {
    case kArRegionalCost_US: *badge = kOverlayRegionBadge_US; break;
    case kArRegionalCost_Japan: *badge = kOverlayRegionBadge_Japan; break;
    case kArRegionalCost_Europe: *badge = kOverlayRegionBadge_Europe; break;
    default: return false;
  }
  return true;
}

const char *SettingsOverlayRegions_BadgeLabel(ArUiLocale locale,
                                             SettingsOverlayRegionBadge badge) {
  switch (badge) {
    case kOverlayRegionBadge_US:
      return ArUiCatalog_Text(locale, "overlay.region.us", "US");
    case kOverlayRegionBadge_Japan:
      return ArUiCatalog_Text(locale, "overlay.region.japan", "Japan");
    case kOverlayRegionBadge_Europe:
      return ArUiCatalog_Text(locale, "overlay.region.europe", "Europe");
    case kOverlayRegionBadge_Mixed:
      return ArUiCatalog_Text(locale, "overlay.region.custom", "Custom");
    default: return "";
  }
}

bool SettingsOverlayRegions_CostDescription(ArUiLocale locale,
                                           const ArRegionalCostPolicy *policy,
                                           ArRegionalCostGroup group,
                                           char *output, size_t capacity) {
  ArRegionalCostSnapshot snapshot;
  SettingsOverlayRegionBadge badge;
  if (!ArRegionalCosts_Resolve(policy, &snapshot) ||
      !SettingsOverlayRegions_CostBadge(policy, group, &badge)) return false;

  char values[kArRegionalCostRule_Count][8];
  ArUiTextArgument args[kArRegionalCostRule_Count + 1];
  size_t count = 0;
  args[count++] = (ArUiTextArgument){"region", SettingsOverlayRegions_BadgeLabel(locale, badge)};
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
    const ArRegionalCostDescriptor *desc = ArRegionalCosts_Descriptor((ArRegionalCostRule)i);
    if (desc->group != group) continue;
    snprintf(values[i], sizeof(values[i]), "%u", (unsigned)snapshot.price[i]);
    /* Stable semantic keys are also named template arguments. */
    args[count++] = (ArUiTextArgument){desc->key, values[i]};
  }
  const char *key = group == kArRegionalCostGroup_Scrolls
      ? "overlay.region.scroll_prices" : "overlay.region.miracle_prices";
  const char *message = ArUiCatalog_Text(locale, key, NULL);
  return message[0] && ArUiCatalog_Format(output, capacity, message, args, count);
}
