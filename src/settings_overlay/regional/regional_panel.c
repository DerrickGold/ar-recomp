#include "settings_overlay/regional/regional_panel.h"

#include <stdio.h>
#include <string.h>
#include "settings_overlay/settings_overlay_localization.h"
#include "localization/interface_text.h"

/* Campaign view, unapplied preset choices, commands and advisory confirmation
 * live together. The host owns campaign policy; the shell owns navigation. */
/* Overlay-local advisory. Separate from host-owned save/Continue decisions:
 * dismissing it returns to the same tab; no host can consume its result. */
static struct {
  OverlayDecision dialog;
  ActRaiserRegionalRulesView view;
  const OverlayRegionRow *row;
  ArRegionalSource source;
} s_regional_confirmation;
static SettingsOverlayRegionalHooks s_regional_hooks;
static ActRaiserRegionalRulesView s_regional_view;
static bool s_regional_valid;
/* Unapplied preset choices belong to the menu, not the campaign. */
static int s_regional_preset_choice[2] = {-1, -1};

static const char *Ui(const char *key) {
  return ArUiCatalog_Text(SettingsOverlay_InterfaceLocale(), key, key);
}

void SettingsOverlay_SetRegionalHooks(const SettingsOverlayRegionalHooks *hooks) {
  SettingsOverlay_CloseDetails();
  s_regional_hooks = hooks ? *hooks : (SettingsOverlayRegionalHooks){0};
  s_regional_valid = false;
  s_regional_preset_choice[0] = s_regional_preset_choice[1] = -1;
  memset(&s_regional_confirmation, 0, sizeof(s_regional_confirmation));
}

void RegionalMenu_Refresh(void) {
  s_regional_valid = s_regional_hooks.copy && s_regional_hooks.copy(&s_regional_view);
}
bool RegionalMenu_Available(void) { return s_regional_valid; }
int RegionalMenu_Count(OverlayRegionPage page) {
  return s_regional_valid ? (int)OverlayRegionMenu_Count(page) : 1;
}
const char *RegionalMenu_Key(OverlayRegionPage page, int selected) {
  const OverlayRegionRow *row = OverlayRegionMenu_Row(page, (unsigned)selected);
  return s_regional_valid && row ? row->key : "regional_no_campaign";
}
void RegionalMenu_Open(void) { s_regional_preset_choice[0] = s_regional_preset_choice[1] = -1; }
void RegionalMenu_Close(void) {
  memset(&s_regional_confirmation, 0, sizeof(s_regional_confirmation));
}
bool RegionalMenu_DrawConfirmation(const MenuLayout *layout) {
  if (s_regional_confirmation.dialog.result != kOverlayDecision_Pending) return false;
  DrawDecision(layout, &s_regional_confirmation.dialog);
  return true;
}

static const char *RegionalNotice(void) {
  return Ui(!s_regional_valid           ? "overlay.region.enter_campaign"
            : !s_regional_view.editable ? "overlay.region.replay_locked"
            : s_regional_view.new_game
                ? (s_regional_view.persistent ? "overlay.region.empty_slot_saved"
                                              : "overlay.region.new_game_draft")
            : s_regional_view.population_pending ? "overlay.region.population_pending"
            : s_regional_view.persistent         ? "overlay.region.saved_immediately"
                                                 : "overlay.region.saved_with_story");
}

static ArRegionalSource RegionalPresetChoice(const OverlayRegionRow *row) {
  const int chosen = s_regional_preset_choice[row->group == kArRegionalProfile_Presentation];
  const ArRegionalSource source = chosen >= 0
                                      ? (ArRegionalSource)chosen
                                      : OverlayRegionMenu_RowSource(&s_regional_view, row, false);
  return source == kArRegionalSource_Count ? kArRegionalSource_US : source;
}

static void RegionalReportResult(ActRaiserRegionalEditResult result) {
  /* Successful cycling should show the newly selected behavior immediately,
   * not cover its explanation with the generic save reminder. */
  if (result == kActRaiserRegionalEdit_Applied || result == kActRaiserRegionalEdit_Unchanged)
    SettingsOverlay_SetStatus("");
  else
    SettingsOverlay_SetStatus(
        SettingsOverlayRegions_EditStatus(SettingsOverlay_InterfaceLocale(), result));
}

void RegionalMenu_Change(OverlayRegionPage page, int selected, int direction, bool reset,
                         bool activate) {
  const OverlayRegionRow *row = OverlayRegionMenu_Row(page, (unsigned)selected);
  if (!s_regional_valid || !s_regional_hooks.request || !s_regional_view.editable) {
    SettingsOverlay_SetStatus(RegionalNotice());
    return;
  }
  if (!row) return;
  if (row->kind == kOverlayRegionRow_Difficulty) {
    if (!s_regional_hooks.difficulty) return;
    const unsigned current = ActRaiserRegionalSettings_DifficultyChoice(&s_regional_view, false);
    const ArRegionalDifficultyChoice next =
        reset ? kArRegionalDifficultyChoice_Original
        : current == kArRegionalDifficultyChoice_Custom
            ? kArRegionalDifficultyChoice_Original
            : (ArRegionalDifficultyChoice)((current + (direction < 0 ? 3u : 1u)) %
                                           kArRegionalDifficultyChoice_Count);
    RegionalReportResult(s_regional_hooks.difficulty(&s_regional_view, next));
    RegionalMenu_Refresh();
    return;
  }
  ArRegionalSource source = kArRegionalSource_US;
  if (row->kind == kOverlayRegionRow_Preset) {
    source = RegionalPresetChoice(row);
    if (!activate) {
      s_regional_preset_choice[row->group == kArRegionalProfile_Presentation] =
          reset ? kArRegionalSource_US
                : (source + (direction < 0 ? 2 : 1)) % kArRegionalSource_Count;
      SettingsOverlay_SetStatus("");
      return;
    }
  } else if (!reset) {
    const ArRegionalSource current = OverlayRegionMenu_RowSource(&s_regional_view, row, false);
    source =
        current == kArRegionalSource_Count
            ? (direction < 0 ? kArRegionalSource_Europe : kArRegionalSource_US)
            : (ArRegionalSource)((current + (direction < 0 ? 2 : 1)) % kArRegionalSource_Count);
    if (row->binary)
      source = current == kArRegionalSource_Japan ? kArRegionalSource_US : kArRegionalSource_Japan;
  }
  const bool narrow = row->kind == kOverlayRegionRow_Setting;
  if (narrow ? (!s_regional_hooks.preview_setting || !s_regional_hooks.setting)
             : !s_regional_hooks.preview) {
    SettingsOverlay_SetStatus(SettingsOverlayRegions_EditStatus(SettingsOverlay_InterfaceLocale(),
                                                                kActRaiserRegionalEdit_Invalid));
    return;
  }
  ActRaiserRegionalEditImpact impact;
  const ActRaiserRegionalEditResult preview =
      narrow ? s_regional_hooks.preview_setting(&s_regional_view, row->setting, source, &impact)
             : s_regional_hooks.preview(&s_regional_view, row->group, source, &impact);
  if (preview != kActRaiserRegionalEdit_Applied && preview != kActRaiserRegionalEdit_Unchanged &&
      preview != kActRaiserRegionalEdit_Deferred) {
    SettingsOverlay_SetStatus(
        SettingsOverlayRegions_EditStatus(SettingsOverlay_InterfaceLocale(), preview));
    return;
  }
  if (row->kind == kOverlayRegionRow_Preset || impact.towns != kArRegionalTownImpact_None ||
      impact.estimated_history) {
    OverlayDecision dialog = {.result = kOverlayDecision_Pending, .body_text = true};
    snprintf(dialog.title, sizeof(dialog.title), "%s",
             row->kind == kOverlayRegionRow_Preset ? "overlay.region.preset.title"
                                                   : "overlay.region.warning.title");
    snprintf(dialog.accept, sizeof(dialog.accept), "%s",
             impact.towns == kArRegionalTownImpact_Redevelopment ? "overlay.region.warning.queue"
                                                                 : "overlay.region.warning.apply");
    const bool formatted =
        row->kind == kOverlayRegionRow_Preset
            ? OverlayRegionMenu_PresetWarning(SettingsOverlay_InterfaceLocale(), row, source,
                                              &impact, dialog.body, sizeof(dialog.body))
            : SettingsOverlayRegions_EditWarning(
                  SettingsOverlay_InterfaceLocale(),
                  OverlayRegionMenu_Label(SettingsOverlay_InterfaceLocale(), row), source, &impact,
                  dialog.body, sizeof(dialog.body));
    if (!formatted) return;
    s_regional_confirmation.dialog = dialog;
    s_regional_confirmation.view = s_regional_view;
    s_regional_confirmation.row = row;
    s_regional_confirmation.source = source;
    return;
  }
  const ActRaiserRegionalEditResult result =
      narrow ? s_regional_hooks.setting(&s_regional_view, row->setting, source)
             : s_regional_hooks.request(&s_regional_view, row->group, source);
  RegionalReportResult(result);
  RegionalMenu_Refresh();
  return;
}

void RegionalMenu_OpenDetails(OverlayRegionPage page, int selected) {
  if (!s_regional_valid) return;
  SettingsOverlayDetailsText text = {0};
  const OverlayRegionRow *row = OverlayRegionMenu_Row(page, (unsigned)selected);
  if (!row) return;
  char help[2048], current[256] = "";
  if (!OverlayRegionMenu_Description(SettingsOverlay_InterfaceLocale(), &s_regional_view, row, help,
                                     sizeof(help)))
    return;
  if (!OverlayRegionMenu_StateLabel(SettingsOverlay_InterfaceLocale(), &s_regional_view, row,
                                    current, sizeof(current)))
    return;
  const int written = snprintf(
      text.body, sizeof(text.body), "%s\n\n%s%s%s\n\n%s", help,
      OverlayRegionMenu_ImpactLabel(SettingsOverlay_InterfaceLocale(), &s_regional_view, row),
      *current ? "\n" : "", current, RegionalNotice());
  if (written < 0 || (size_t)written >= sizeof(text.body)) return;
  snprintf(text.title, sizeof(text.title), "%s",
           OverlayRegionMenu_Label(SettingsOverlay_InterfaceLocale(), row));
  SettingsOverlay_ShowDetails(&text);
}

bool RegionalMenu_HandleConfirmation(MenuNav nav, bool repeat) {
  if (s_regional_confirmation.dialog.result == kOverlayDecision_Pending) {
    if (repeat) return true;
    if (nav == kMenuNav_Up || nav == kMenuNav_Down || nav == kMenuNav_Left ||
        nav == kMenuNav_Right) {
      s_regional_confirmation.dialog.accept_selected =
          !s_regional_confirmation.dialog.accept_selected;
    } else if (nav == kMenuNav_Confirm) {
      const bool apply = s_regional_confirmation.dialog.accept_selected;
      s_regional_confirmation.dialog.result = kOverlayDecision_None;
      if (apply && s_regional_hooks.request) {
        const OverlayRegionRow *row = s_regional_confirmation.row;
        const ActRaiserRegionalEditResult result =
            row->kind == kOverlayRegionRow_Setting
                ? s_regional_hooks.setting(&s_regional_confirmation.view, row->setting,
                                           s_regional_confirmation.source)
                : s_regional_hooks.request(&s_regional_confirmation.view, row->group,
                                           s_regional_confirmation.source);
        RegionalReportResult(result);
      }
      RegionalMenu_Refresh();
    } else if (nav == kMenuNav_Back || nav == kMenuNav_Close) {
      memset(&s_regional_confirmation, 0, sizeof(s_regional_confirmation));
    }
    return true;
  }
  return false;
}

int RegionalMenu_DrawRows(const MenuLayout *layout, OverlayRegionPage page,
                          const MenuRowViewport *viewport) {
  const int right_x = viewport->x, right_width = viewport->width;
  const int first_row_y = viewport->first_y, value_right = viewport->value_right;
  const int selector_x = right_x + 12, label_x = right_x + 22;
  const int value_chars = 18;
  const int count = RegionalMenu_Count(page);
  /* Region codes share one column, with room for either a pending marker
   * or a preset arrow. Mixed values can widen the entire column, never just
   * push an individual flag into its setting's label. Difficulty is text. */
  int region_value_chars = 4;
  for (int row = 0; s_regional_valid && row < count; ++row) {
    const OverlayRegionRow *entry = OverlayRegionMenu_Row(page, (unsigned)row);
    char value[128];
    SettingsOverlayRegionBadge badge;
    if (entry->kind != kOverlayRegionRow_Setting ||
        !OverlayRegionMenu_Value(SettingsOverlay_InterfaceLocale(), &s_regional_view, entry, false,
                                 value, sizeof(value), &badge))
      continue;
    const int length = CappedTextLength(value, value_chars);
    if (length > region_value_chars) region_value_chars = length;
  }
  for (int row = 0; row < count; ++row) {
    if (row < viewport->top || row >= viewport->top + viewport->visible) continue;
    const int y = first_row_y + (row - viewport->top) * 13;
    if (!s_regional_valid) {
      DrawSmallTextN(layout, label_x, y, Ui("overlay.region.enter_campaign"),
                     (value_right - label_x) / kDebugGlyphWidth, kMutedText);
      continue;
    }
    const OverlayRegionRow *entry = OverlayRegionMenu_Row(page, (unsigned)row);
    SettingsOverlayRegionBadge badge;
    char value[128];
    if (!OverlayRegionMenu_Value(SettingsOverlay_InterfaceLocale(), &s_regional_view, entry, false,
                                 value, sizeof(value), &badge))
      continue;
    if (entry->kind == kOverlayRegionRow_Preset) {
      const ArRegionalSource source = RegionalPresetChoice(entry);
      badge = source == kArRegionalSource_US      ? kOverlayRegionBadge_US
              : source == kArRegionalSource_Japan ? kOverlayRegionBadge_Japan
                                                  : kOverlayRegionBadge_Europe;
      snprintf(value, sizeof(value), "%s >",
               SettingsOverlayRegions_BadgeCode(SettingsOverlay_InterfaceLocale(), badge));
    }
    const bool selected = viewport->focused && row == viewport->selected;
    if (selected) {
      FillLogicalRect(layout, right_x + 9, y - 2, right_width - 18, 11, kHighlight);
      FillLogicalRect(layout, right_x + 9, y - 2, 2, 11, kSelectYellow);
      DrawGlyph(layout, selector_x + viewport->cursor_offset, y, '>', kText_Warning);
    }
    const TextStyle style =
        viewport->focused && s_regional_view.editable ? kText_Normal : kText_Dim;
    const int shown = entry->kind == kOverlayRegionRow_Difficulty
                          ? CappedTextLength(value, value_chars)
                          : region_value_chars;
    const int badge_x =
        value_right - shown * kGlyphSize - (entry->kind == kOverlayRegionRow_Difficulty ? 0 : 16);
    DrawTextN(layout, label_x, y, OverlayRegionMenu_Label(SettingsOverlay_InterfaceLocale(), entry),
              (badge_x - label_x - 4) / kGlyphSize, style);
    DrawTextN(layout, value_right - shown * kGlyphSize, y, value, shown,
              style == kText_Normal ? kText_Value : style);
    const ArRenderTexture texture = SettingsOverlayArtwork_Get()->region_badges;
    if (entry->kind != kOverlayRegionRow_Difficulty && ArRenderTexture_IsValid(texture)) {
      const ArRenderRectF source = {(float)(badge * kRegionBadgeWidth), 0, kRegionBadgeWidth,
                                    kRegionBadgeHeight};
      const ArRenderRectF destination = ToRenderRect(LogicalRect(layout, badge_x, y, 12, 8));
      (void)ArRenderDevice_DrawTextureTinted(
          SettingsOverlayWidgets_RenderDevice(), texture, &source, &destination,
          (ArRenderColorF){1, 1, 1, style == kText_Normal ? 1.0f : 0.5f});
    }
  }
  return count;
}

void RegionalMenu_DrawDescription(const MenuLayout *layout, OverlayRegionPage page, int selected,
                                  int description_x, int header_y, int width) {
  const int description_chars = width / kDebugGlyphWidth;
  const uint32_t structure = kSteelBlue, structure_dim = kSteelDim;
  const OverlayRegionRow *entry = OverlayRegionMenu_Row(page, (unsigned)selected);
  const char *label = s_regional_valid
                          ? OverlayRegionMenu_Label(SettingsOverlay_InterfaceLocale(), entry)
                          : Ui("overlay.region.tab");
  char help[2048];
  char current[256] = "";
  if (s_regional_valid && entry)
    OverlayRegionMenu_StateLabel(SettingsOverlay_InterfaceLocale(), &s_regional_view, entry,
                                 current, sizeof(current));
  const int current_x = description_x + width - SmallTextWidth(current);
  DrawSmallTextN(layout, description_x, header_y, label,
                 (current_x - description_x - 8) / kDebugGlyphWidth, structure);
  DrawSmallText(layout, current_x, header_y, current, kMutedText);
  FillLogicalRect(layout, description_x, header_y + 10, width, 1, structure_dim);
  if (!s_regional_valid || !OverlayRegionMenu_Preview(SettingsOverlay_InterfaceLocale(),
                                                      &s_regional_view, entry, help, sizeof(help)))
    snprintf(help, sizeof(help), "%s", RegionalNotice());
  DrawSmallTextPreview(layout, description_x, header_y + 14, help, description_chars, 3,
                       ARGB(255, 208, 220, 232));
  const OverlayRegionNote note =
      s_regional_valid
          ? OverlayRegionMenu_Note(SettingsOverlay_InterfaceLocale(), &s_regional_view, entry)
          : (OverlayRegionNote){"", false};
  DrawSmallTextN(layout, description_x, header_y + 14 + 3 * kSmallLineHeight, note.text,
                 description_chars, note.attention ? kGameGold : kMutedText);
}

void RegionalMenu_AddHints(MenuHints *hints, OverlayRegionPage page, const char *change,
                           const char *confirm, const char *tabs, const char *reset,
                           const char *details) {
  if (page == kOverlayRegionPage_Presets) {
    AddMenuHint(hints, change, Ui("overlay.hint.select"));
    AddMenuHint(hints, confirm, Ui("overlay.region.hint.review"));
    AddMenuHint(hints, tabs, Ui("overlay.hint.tab"));
  } else {
    AddMenuHint(hints, change, Ui("overlay.hint.change"));
    AddMenuHint(hints, tabs, Ui("overlay.hint.tab"));
    AddMenuHint(hints, reset, Ui("overlay.hint.reset"));
  }
  if (s_regional_valid) AddMenuHint(hints, details, Ui("overlay.hint.details"));
}
