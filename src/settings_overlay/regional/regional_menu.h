#ifndef SETTINGS_OVERLAY_REGIONAL_MENU_H
#define SETTINGS_OVERLAY_REGIONAL_MENU_H

#include "settings_overlay/regional/regional_ui.h"

typedef enum OverlayRegionPage {
  kOverlayRegionPage_Presets,
  kOverlayRegionPage_Action,
  kOverlayRegionPage_Towns,
  kOverlayRegionPage_Controls,
  kOverlayRegionPage_Presentation,
  kOverlayRegionPage_Count,
} OverlayRegionPage;
typedef enum OverlayRegionRowKind {
  kOverlayRegionRow_Preset,
  kOverlayRegionRow_Setting,
  kOverlayRegionRow_Difficulty,
} OverlayRegionRowKind;
typedef struct OverlayRegionRow {
  /* Stable input ID plus literal catalog keys: searchable without reconstructing
   * strings. The catalog remains the only owner of translated text. */
  const char *key;
  const char *label_key;
  const char *help_key;
  OverlayRegionRowKind kind;
  ArRegionalProfileGroup group;
  ActRaiserRegionalSettingGroup setting;
  bool binary;
} OverlayRegionRow;

typedef struct OverlayRegionNote {
  const char *text;
  bool attention;
} OverlayRegionNote;

unsigned OverlayRegionMenu_Count(OverlayRegionPage page);
const OverlayRegionRow *OverlayRegionMenu_Row(OverlayRegionPage page, unsigned row);
const char *OverlayRegionMenu_ImpactLabel(ArUiLocale locale, const ActRaiserRegionalRulesView *view,
                                          const OverlayRegionRow *row);
const char *OverlayRegionMenu_Label(ArUiLocale locale, const OverlayRegionRow *row);
bool OverlayRegionMenu_Value(ArUiLocale locale, const ActRaiserRegionalRulesView *view,
                             const OverlayRegionRow *row, bool effective, char *out,
                             size_t capacity, SettingsOverlayRegionBadge *badge);
bool OverlayRegionMenu_Description(ArUiLocale locale, const ActRaiserRegionalRulesView *view,
                                   const OverlayRegionRow *row, char *out, size_t capacity);
/* Compact controls prose describes the selected behavior; Details retains
 * the full regional comparison. Other rows use their normal description. */
bool OverlayRegionMenu_Preview(ArUiLocale locale, const ActRaiserRegionalRulesView *view,
                              const OverlayRegionRow *row, char *out, size_t capacity);
/* Presets always show the current profile (or new-game draft), independently
 * of the apply target. Other rows show Active only for a differing pending value. */
bool OverlayRegionMenu_StateLabel(ArUiLocale locale, const ActRaiserRegionalRulesView *view,
                                  const OverlayRegionRow *row, char *out, size_t capacity);
OverlayRegionNote OverlayRegionMenu_Note(ArUiLocale locale, const ActRaiserRegionalRulesView *view,
                                         const OverlayRegionRow *row);
ArRegionalSource OverlayRegionMenu_Source(const ActRaiserRegionalRulesView *view,
                                          ArRegionalProfileGroup group, bool effective);
bool OverlayRegionMenu_Pending(const ActRaiserRegionalRulesView *view,
                               ArRegionalProfileGroup group);
ArRegionalSource OverlayRegionMenu_RowSource(const ActRaiserRegionalRulesView *view,
    const OverlayRegionRow *row, bool effective);
bool OverlayRegionMenu_PresetWarning(ArUiLocale locale, const OverlayRegionRow *row,
    ArRegionalSource source, const ActRaiserRegionalEditImpact *impact, char *out, size_t capacity);
const char *OverlayRegionMenu_DifficultyLabel(ArUiLocale locale, ArRegionalDifficultyChoice choice);

#endif
