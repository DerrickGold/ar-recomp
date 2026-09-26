#include "settings_overlay/save_slots/save_slot_menu.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app/settings.h"
#include "localization/interface_text.h"
#include "randomizer/randomizer.h"
#include "settings_overlay/settings_overlay.h"
#include "settings_overlay/settings_overlay_internal.h"
#include "settings_overlay/settings_overlay_localization.h"
#include "settings_overlay/regional/regional_menu.h"

/* Slot data, draft edits and reviewed confirmations have one lifetime. The
 * overlay owns navigation and shared text widgets; storage stays behind hooks. */
typedef struct SlotDecision {
  SettingsOverlayDecisionResult result;
  bool accept_selected, body_text;
  char title[96], body[2048], accept[96];
} SlotDecision;

static const char *Ui(const char *key) {
  return ArUiCatalog_Text(SettingsOverlay_InterfaceLocale(), key, key);
}

/* Private save-slot controller. The host owns storage; this menu owns copies
 * of reviewed data and a scratch new-game draft. No live settings are edited. */
typedef enum SlotScreen {
  kSlotScreen_Closed,
  kSlotScreen_List,
  kSlotScreen_Setup,
  kSlotScreen_Seed,
  kSlotScreen_Randomizer,
  kSlotScreen_Regions
} SlotScreen;
typedef enum SlotPage {
  kSlotPage_Summary,
  kSlotPage_Regions,
  kSlotPage_Randomizer,
  kSlotPage_Count
} SlotPage;
enum { kSlotRandomPageCount = 4, kSlotDetailCapacity = 96, kSlotSeedDigits = 9 };
static SettingsOverlaySaveSlotHooks s_slot_hooks;
static void SlotOpenDetails(void);
static struct {
  SlotScreen screen;
  SaveSlotCollection collection;
  ArRegionalSession draft;
  ActRaiserRegionalRulesView draft_view;
  int selected, top, row, tab, scroll, digit, setup_row, list_tab;
  int draft_slot;
  bool detail_focus, advanced_focus, advanced;
  bool randomized_entry, randomizer_visible, full;
  SlotDecision decision;
  const SettingDesc *advanced_action;
  unsigned reviewed_active;
  uint64_t reviewed_fingerprint;
  char error[256];
} s_slots;

void SaveSlotMenu_SetHooks(const SettingsOverlaySaveSlotHooks *hooks) {
  s_slot_hooks = hooks ? *hooks : (SettingsOverlaySaveSlotHooks){0};
}
bool SaveSlotMenu_Available(void) { return s_slot_hooks.scan != NULL; }
bool SaveSlotMenu_Active(void) { return s_slots.screen != kSlotScreen_Closed; }
static bool SlotRandomizerVisible(void) { return g_settings.show_debug_settings; }
static int SlotPageCount(void) {
  return SlotRandomizerVisible() ? kSlotPage_Count : kSlotPage_Randomizer;
}
void SaveSlotMenu_TabState(int *active_tab, int *tab_count) {
  int count = s_slots.screen == kSlotScreen_List         ? SlotPageCount()
              : s_slots.screen == kSlotScreen_Regions    ? kOverlayRegionPage_Count
              : s_slots.screen == kSlotScreen_Randomizer ? kSlotRandomPageCount
                                                         : 1;
  if (active_tab) *active_tab = count > 1 ? s_slots.tab : 0;
  if (tab_count) *tab_count = count;
}
void SaveSlotMenu_Refresh(void) {
  if (!SaveSlotMenu_Active() || s_slots.randomizer_visible == SlotRandomizerVisible()) return;
  s_slots.randomizer_visible = SlotRandomizerVisible();
  /* A live gate change must also dismiss already-open seed/help dialogs.
   * Persisted campaigns keep their recipe; only a fresh scratch draft defaults
   * to standard play when its experimental controls become unavailable. */
  SettingsOverlay_CloseDetails();
  s_slots.decision = (SlotDecision){0};
  if (!SlotRandomizerVisible()) {
    s_slots.randomized_entry = false;
    if (s_slots.draft_slot >= 0 && !s_slots.collection.slots[s_slots.draft_slot].prepared)
      s_slots.draft.randomizer.enabled = false;
    if (s_slots.list_tab >= SlotPageCount()) s_slots.list_tab = kSlotPage_Summary;
    if (s_slots.screen == kSlotScreen_List && s_slots.tab >= SlotPageCount()) {
      s_slots.tab = kSlotPage_Summary;
      s_slots.scroll = 0;
    }
    if (s_slots.screen == kSlotScreen_Seed || s_slots.screen == kSlotScreen_Randomizer)
      s_slots.screen = kSlotScreen_Setup;
  }
  s_slots.setup_row = 0;
  if (s_slots.screen == kSlotScreen_Setup) s_slots.row = s_slots.tab = s_slots.scroll = 0;
}
bool SaveSlotMenu_DecisionActive(void) {
  return s_slots.decision.result == kOverlayDecision_Pending;
}
const char *SaveSlotMenu_EditorTitle(void) {
  static char title[160];
  if (!s_slots.advanced) return NULL;
  char number[16];
  snprintf(number, sizeof(number), "%u", s_slots.collection.active + 1);
  const SaveSlotDetails *slot = &s_slots.collection.slots[s_slots.collection.active];
  ArUiTextArgument args[] = {
      {"slot", number},
      {"name", slot->summary.name[0] ? slot->summary.name : Ui("slots.new_game")}};
  ArUiCatalog_Format(title, sizeof(title), Ui("slots.advanced.target"), args, 2);
  return title;
}
bool SaveSlotMenu_ConfirmEditorAction(const SettingDesc *desc) {
  if (!s_slots.advanced || (desc->action != kSettingAction_SaveImport &&
                            desc->action != kSettingAction_SaveApplyPersist &&
                            desc->action != kSettingAction_SaveApplySession))
    return false;
  s_slots.advanced_action = desc;
  s_slots.reviewed_active = s_slots.collection.active;
  s_slots.reviewed_fingerprint = s_slots.collection.slots[s_slots.reviewed_active].fingerprint;
  char number[16];
  snprintf(number, sizeof(number), "%u", s_slots.reviewed_active + 1);
  const SaveSlotDetails *slot = &s_slots.collection.slots[s_slots.reviewed_active];
  ArUiTextArgument args[] = {
      {"slot", number},
      {"name", slot->summary.name[0] ? slot->summary.name : Ui("slots.new_game")},
      {"action", SettingsOverlay_LocalizedLabel(SettingsOverlay_InterfaceLocale(), desc)}};
  s_slots.decision = (SlotDecision){.result = kOverlayDecision_Pending, .body_text = true};
  ArUiCatalog_Format(s_slots.decision.title, sizeof(s_slots.decision.title),
                     Ui("slots.advanced.confirm"), args, 3);
  snprintf(s_slots.decision.body, sizeof(s_slots.decision.body), "%s",
           Ui(desc->action == kSettingAction_SaveImport ? "slots.advanced.import"
                                                        : "slots.advanced.edit"));
  snprintf(s_slots.decision.accept, sizeof(s_slots.decision.accept), "%s",
           desc->action == kSettingAction_SaveImport ? "slots.advanced.replace"
                                                     : "slots.advanced.apply");
  return true;
}
void SaveSlotMenu_Close(void) {
  s_slots.screen = kSlotScreen_Closed;
  s_slots.advanced = false;
  s_slots.advanced_action = NULL;
  s_slots.decision = (SlotDecision){0};
}
static bool SlotScanCollection(void) {
  SaveSlotCollection collection;
  if (!s_slot_hooks.scan || !s_slot_hooks.scan(&collection) || collection.active >= kSaveSlotCount)
    return false;
  s_slots.collection = collection;
  s_slots.full = true;
  for (int i = 0; i < kSaveSlotCount; ++i)
    if (collection.slots[i].state == kSaveSlot_Empty && !collection.slots[i].prepared)
      s_slots.full = false;
  return true;
}
bool SaveSlotMenu_Open(bool randomized) {
  if (!SaveSlotMenu_Available() || (randomized && !SlotRandomizerVisible())) return false;
  memset(&s_slots, 0, sizeof(s_slots));
  s_slots.draft_slot = -1;
  if (!SlotScanCollection()) return false;
  s_slots.screen = kSlotScreen_List;
  s_slots.selected = s_slots.collection.active;
  s_slots.randomized_entry = randomized;
  s_slots.randomizer_visible = SlotRandomizerVisible();
  if (randomized) {
    for (int i = 0; i < kSaveSlotCount; ++i) {
      if (s_slots.collection.slots[i].state == kSaveSlot_Empty &&
          !s_slots.collection.slots[i].prepared) {
        s_slots.selected = i;
        break;
      }
    }
  }
  return true;
}
static SaveSlotMenuNavResult SlotOpenAdvanced(void) {
  s_slots.screen = kSlotScreen_Closed;
  s_slots.advanced = true;
  return kSaveSlotMenuNav_OpenEditor;
}
bool SaveSlotMenu_ReturnFromEditor(void) {
  if (!s_slots.advanced) return false;
  s_slots.advanced = false;
  s_slots.advanced_action = NULL;
  s_slots.screen = kSlotScreen_List;
  /* Import/editor actions may have changed the save since entering Advanced. */
  if (!SlotScanCollection())
    snprintf(s_slots.error, sizeof(s_slots.error), "%s", Ui("slots.read_failed"));
  return true;
}

/* Typed access deliberately avoids offsets and field-size casts: the recipe
 * has bool, byte and uint16 fields, while settings descriptors use int. */
typedef enum SlotRandomOption {
  kSlotRandom_Action,
  kSlotRandom_Towns,
  kSlotRandom_HP,
  kSlotRandom_Attack,
  kSlotRandom_Enemies,
  kSlotRandom_Scope,
  kSlotRandom_Drops,
  kSlotRandom_Statues,
  kSlotRandom_Lairs,
  kSlotRandom_LairTypes
} SlotRandomOption;
typedef struct SlotRandomRow {
  const char *key;
  SlotRandomOption option;
  int tab;
} SlotRandomRow;
static const SlotRandomRow kSlotRandomRows[] = {
    {"rando_regional_action", kSlotRandom_Action, 0},
    {"rando_regional_towns", kSlotRandom_Towns, 0},
    {"rando_enemy_hp", kSlotRandom_HP, 1},
    {"rando_enemy_atk", kSlotRandom_Attack, 1},
    {"rando_enemy_types", kSlotRandom_Enemies, 1},
    {"rando_enemy_scope", kSlotRandom_Scope, 1},
    {"rando_statue_drops", kSlotRandom_Drops, 2},
    {"rando_statue_spots", kSlotRandom_Statues, 2},
    {"rando_lair_spots", kSlotRandom_Lairs, 3},
    {"rando_lair_types", kSlotRandom_LairTypes, 3},
};
static const SlotRandomRow *SlotRandomAt(int index) {
  for (unsigned i = 0; i < sizeof(kSlotRandomRows) / sizeof(kSlotRandomRows[0]); ++i)
    if (kSlotRandomRows[i].tab == s_slots.tab && index-- == 0) return &kSlotRandomRows[i];
  return NULL;
}
static int SlotRecipeValue(const RandomizerConfig *recipe, SlotRandomOption option) {
  switch (option) {
  case kSlotRandom_Action:
    return recipe->regional_action;
  case kSlotRandom_Towns:
    return recipe->regional_towns;
  case kSlotRandom_HP:
    return recipe->hp_percent;
  case kSlotRandom_Attack:
    return recipe->attack_percent;
  case kSlotRandom_Enemies:
    return recipe->enemy_types;
  case kSlotRandom_Scope:
    return recipe->enemy_scope;
  case kSlotRandom_Drops:
    return recipe->statue_drops;
  case kSlotRandom_Statues:
    return recipe->statue_spots;
  case kSlotRandom_Lairs:
    return recipe->lair_spots;
  case kSlotRandom_LairTypes:
    return recipe->lair_types;
  }
  return 0;
}
static void SlotRecipeSet(RandomizerConfig *recipe, SlotRandomOption option, int value) {
  switch (option) {
  case kSlotRandom_Action:
    recipe->regional_action = value;
    break;
  case kSlotRandom_Towns:
    recipe->regional_towns = value;
    break;
  case kSlotRandom_HP:
    recipe->hp_percent = value;
    break;
  case kSlotRandom_Attack:
    recipe->attack_percent = value;
    break;
  case kSlotRandom_Enemies:
    recipe->enemy_types = value;
    break;
  case kSlotRandom_Scope:
    recipe->enemy_scope = value;
    break;
  case kSlotRandom_Drops:
    recipe->statue_drops = value;
    break;
  case kSlotRandom_Statues:
    recipe->statue_spots = value;
    break;
  case kSlotRandom_Lairs:
    recipe->lair_spots = value;
    break;
  case kSlotRandom_LairTypes:
    recipe->lair_types = value;
    break;
  }
}
static void SlotRandomText(const RandomizerConfig *recipe, const SlotRandomRow *row, char *out,
                           size_t capacity) {
  const SettingDesc *desc = row ? Settings_Find(row->key) : NULL;
  if (!desc) {
    snprintf(out, capacity, "%s", Ui("slots.unknown"));
    return;
  }
  int value = SlotRecipeValue(recipe, row->option);
  if (row->option == kSlotRandom_HP || row->option == kSlotRandom_Attack) {
    snprintf(out, capacity, "%d%%", value);
    return;
  }
  bool boolean = value != 0;
  SettingDesc copy = *desc;
  copy.field = desc->type == kSettingType_Bool ? (void *)&boolean : (void *)&value;
  SettingsOverlay_LocalizedValue(SettingsOverlay_InterfaceLocale(), &copy, out, capacity);
}
static void SlotRandomChange(const SlotRandomRow *row, int direction) {
  const SettingDesc *desc = row ? Settings_Find(row->key) : NULL;
  if (!desc) return;
  int value = SlotRecipeValue(&s_slots.draft.randomizer, row->option) + direction * (int)desc->step;
  if (value < desc->minval) value = (int)desc->maxval;
  if (value > desc->maxval) value = (int)desc->minval;
  RandomizerConfig candidate = s_slots.draft.randomizer;
  SlotRecipeSet(&candidate, row->option, value);
  if (RandomizerConfig_Valid(&candidate)) s_slots.draft.randomizer = candidate;
}
static int SlotProfileSource(const ActRaiserRegionalRulesView *view, ArRegionalProfileGroup group) {
  int source = view->profiles[group].source;
  return source >= 0 && source <= kArRegionalSource_Count ? source : kArRegionalSource_Count;
}
static int SlotSetupCode(int row) {
  if (!SlotRandomizerVisible()) return row >= 0 && row < 2 ? row + 4 : -1;
  if (s_slots.draft.randomizer.enabled) return row;
  const int standard[] = {0, 4, 5};
  return row >= 0 && row < 3 ? standard[row] : -1;
}
static int SlotRowCount(void) {
  if (s_slots.screen == kSlotScreen_Setup)
    return !SlotRandomizerVisible() ? 2 : s_slots.draft.randomizer.enabled ? 6 : 3;
  if (s_slots.screen == kSlotScreen_Regions)
    return (int)OverlayRegionMenu_Count((OverlayRegionPage)s_slots.tab);
  if (s_slots.screen == kSlotScreen_Randomizer) {
    int n = 0;
    while (SlotRandomAt(n))
      ++n;
    return n;
  }
  return 1;
}
const char *SaveSlotMenu_SelectedKey(void) {
  if (s_slots.decision.result == kOverlayDecision_Pending) return "slot_confirm";
  if (s_slots.screen == kSlotScreen_Seed) return "slot_seed";
  if (s_slots.screen == kSlotScreen_List) {
    if (s_slots.advanced_focus) return "slot_advanced";
    return s_slots.detail_focus ? "slot_use" : "slot_list";
  }
  if (s_slots.screen == kSlotScreen_Randomizer) {
    const SlotRandomRow *r = SlotRandomAt(s_slots.row);
    return r ? r->key : "";
  }
  if (s_slots.screen == kSlotScreen_Regions) {
    const OverlayRegionRow *r = OverlayRegionMenu_Row(s_slots.tab, s_slots.row);
    return r ? r->key : "";
  }
  const char *keys[] = {"slot_type",    "slot_seed",    "slot_new_seed",
                        "slot_options", "slot_regions", "slot_start"};
  int n = SlotSetupCode(s_slots.row);
  return n >= 0 && n < 6 ? keys[n] : "";
}
static void SlotDraftOpen(void) {
  SaveError error = {{0}};
  if (!s_slots.collection.writable || !s_slot_hooks.draft ||
      (s_slots.draft_slot != s_slots.selected &&
       !s_slot_hooks.draft(s_slots.selected, &s_slots.draft, &error))) {
    snprintf(s_slots.error, sizeof(s_slots.error), "%s",
             error.message[0] ? error.message : Ui("slots.draft_failed"));
    return;
  }
  if (s_slots.draft_slot != s_slots.selected) {
    if (!s_slots.collection.slots[s_slots.selected].prepared) {
      if (!SlotRandomizerVisible())
        s_slots.draft.randomizer.enabled = false;
      else if (s_slots.randomized_entry)
        s_slots.draft.randomizer.enabled = true;
    }
    s_slots.draft_slot = s_slots.selected;
  }
  s_slots.list_tab = s_slots.tab;
  s_slots.screen = kSlotScreen_Setup;
  s_slots.row = s_slots.tab = s_slots.scroll = 0;
  s_slots.error[0] = 0;
}
static void SlotConfirm(void) {
  s_slots.advanced_action = NULL;
  if (!s_slots.collection.writable) return;
  const SaveSlotDetails *slot = &s_slots.collection.slots[s_slots.selected];
  bool create = s_slots.screen == kSlotScreen_Setup;
  if (!create &&
      (slot->state != kSaveSlot_Ready || s_slots.selected == (int)s_slots.collection.active))
    return;
  s_slots.decision = (SlotDecision){.result = kOverlayDecision_Pending, .body_text = true};
  snprintf(s_slots.decision.accept, sizeof(s_slots.decision.accept), "%s",
           create ? "slots.start_restart" : "slots.switch_restart");
  char number[16], seed[16];
  snprintf(number, sizeof(number), "%u", s_slots.selected + 1);
  snprintf(seed, sizeof(seed), "%u", s_slots.draft.randomizer.seed);
  ArUiTextArgument args[] = {
      {"slot", number},
      {"seed", seed},
      {"name", slot->summary.name[0] ? slot->summary.name : Ui("slots.unknown")}};
  ArUiCatalog_Format(s_slots.decision.title, sizeof(s_slots.decision.title),
                     Ui(create ? "slots.confirm.create_title" : "slots.confirm.switch_title"), args,
                     3);
  const char *key =
      create ? (SlotRandomizerVisible() && s_slots.draft.randomizer.enabled ? "slots.confirm.random"
                                                                            : "slots.confirm.new")
             : "slots.confirm.load";
  ArUiCatalog_Format(s_slots.decision.body, sizeof(s_slots.decision.body), Ui(key), args, 3);
}
static void SlotReturnToSetup(void) {
  s_slots.screen = kSlotScreen_Setup;
  s_slots.row = s_slots.setup_row;
  s_slots.tab = s_slots.scroll = 0;
}
static void SlotEnterSetupPage(SlotScreen screen) {
  s_slots.setup_row = s_slots.row;
  s_slots.screen = screen;
  s_slots.row = s_slots.tab = s_slots.scroll = s_slots.digit = 0;
}
SaveSlotMenuNavResult SaveSlotMenu_HandleNav(MenuNav nav, bool repeat) {
  if (!SaveSlotMenu_Active() && !SaveSlotMenu_DecisionActive()) return kSaveSlotMenuNav_Unhandled;
  if (s_slots.decision.result == kOverlayDecision_Pending) {
    if (repeat) return kSaveSlotMenuNav_Handled;
    if (nav == kMenuNav_Up || nav == kMenuNav_Down || nav == kMenuNav_Left || nav == kMenuNav_Right)
      s_slots.decision.accept_selected = !s_slots.decision.accept_selected;
    else if (nav == kMenuNav_Back || nav == kMenuNav_Close) {
      s_slots.decision.result = kOverlayDecision_None;
      s_slots.advanced_action = NULL;
    } else if (nav == kMenuNav_Confirm) {
      bool accept = s_slots.decision.accept_selected;
      if (!accept) s_slots.advanced_action = NULL;
      s_slots.decision.result = kOverlayDecision_None;
      if (accept && s_slots.advanced_action) {
        const SettingDesc *action = s_slots.advanced_action;
        s_slots.advanced_action = NULL;
        if (!SlotScanCollection() || !s_slots.collection.writable ||
            s_slots.collection.active != s_slots.reviewed_active ||
            s_slots.collection.slots[s_slots.reviewed_active].state == kSaveSlot_Unavailable ||
            s_slots.collection.slots[s_slots.reviewed_active].fingerprint !=
                s_slots.reviewed_fingerprint) {
          SettingsOverlay_SetStatus(Ui("slots.advanced.changed"));
          return kSaveSlotMenuNav_Handled;
        }
        bool success = Settings_InvokeAction(action);
        if (SettingsOverlay_IsOpen()) {
          (void)SlotScanCollection();
          SettingsOverlay_SetStatus(
              Ui(success ? "overlay.status.action_complete" : "overlay.status.action_failed"));
        }
      } else if (accept) {
        SaveError error = {{0}};
        if (!s_slot_hooks.start ||
            !s_slot_hooks.start(
                s_slots.selected, s_slots.collection.slots[s_slots.selected].fingerprint,
                s_slots.screen == kSlotScreen_Setup ? &s_slots.draft : NULL, &error)) {
          snprintf(s_slots.error, sizeof(s_slots.error), "%s",
                   error.message[0] ? error.message : Ui("slots.start_failed"));
          /* A failed restart may have successfully stored the prepared draft.
           * Re-review its fingerprint so retrying does not use stale metadata. */
          (void)SlotScanCollection();
        }
      }
    }
    return kSaveSlotMenuNav_Handled;
  }
  if (nav == kMenuNav_Close && !repeat) return kSaveSlotMenuNav_CloseOverlay;
  if (nav == kMenuNav_Details && !repeat) {
    SlotOpenDetails();
    return kSaveSlotMenuNav_Handled;
  }
  if (nav == kMenuNav_Back && !repeat) {
    s_slots.error[0] = 0;
    if (s_slots.screen == kSlotScreen_List) {
      if (s_slots.detail_focus) {
        s_slots.detail_focus = false;
        s_slots.scroll = 0;
      } else {
        SaveSlotMenu_Close();
        return kSaveSlotMenuNav_ReturnToParent;
      }
    } else if (s_slots.screen == kSlotScreen_Setup) {
      s_slots.screen = kSlotScreen_List;
      s_slots.tab = s_slots.list_tab;
      s_slots.scroll = 0;
    } else
      SlotReturnToSetup();
    return kSaveSlotMenuNav_Handled;
  }
  int direction = (nav == kMenuNav_Up || nav == kMenuNav_Left || nav == kMenuNav_TabPrev) ? -1 : 1;
  if (s_slots.screen == kSlotScreen_List) {
    /* Nothing is reset in the slot list. Reuse the bound Y action as a
     * shortcut to the same active-save tools as the Advanced footer row. */
    if (nav == kMenuNav_Reset && !repeat)
      return SlotOpenAdvanced();
    else if (nav == kMenuNav_Up || nav == kMenuNav_Down) {
      if (s_slots.detail_focus) {
        if (direction > 0 || s_slots.scroll > 0) s_slots.scroll += direction;
      } else {
        int item = s_slots.advanced_focus ? kSaveSlotCount : s_slots.selected;
        item = (item + direction + kSaveSlotCount + 1) % (kSaveSlotCount + 1);
        s_slots.advanced_focus = item == kSaveSlotCount;
        if (!s_slots.advanced_focus) s_slots.selected = item;
        s_slots.scroll = 0;
        s_slots.error[0] = 0;
      }
    } else if (nav == kMenuNav_Left || nav == kMenuNav_Right || nav == kMenuNav_TabPrev ||
               nav == kMenuNav_TabNext) {
      if (!s_slots.advanced_focus) {
        int count = SlotPageCount();
        s_slots.tab = (s_slots.tab + direction + count) % count;
        s_slots.scroll = 0;
      }
    } else if (nav == kMenuNav_Confirm && !repeat) {
      if (s_slots.advanced_focus)
        return SlotOpenAdvanced();
      else if (s_slots.collection.slots[s_slots.selected].state == kSaveSlot_Empty)
        SlotDraftOpen();
      else if (!s_slots.detail_focus)
        s_slots.detail_focus = true;
      else
        SlotConfirm();
    }
    return kSaveSlotMenuNav_Handled;
  }
  if (s_slots.screen == kSlotScreen_Seed) {
    if (nav == kMenuNav_Left || nav == kMenuNav_Right)
      s_slots.digit = (s_slots.digit + direction + kSlotSeedDigits) % kSlotSeedDigits;
    else if (nav == kMenuNav_Up || nav == kMenuNav_Down) {
      uint32_t place = 1;
      for (int i = kSlotSeedDigits - 1; i > s_slots.digit; --i)
        place *= 10;
      int digit = (s_slots.draft.randomizer.seed / place) % 10;
      int next = (digit - direction + 10) % 10;
      s_slots.draft.randomizer.seed =
          (uint32_t)((int64_t)s_slots.draft.randomizer.seed + (next - digit) * (int64_t)place);
    } else if (nav == kMenuNav_Confirm && !repeat)
      SlotReturnToSetup();
    return kSaveSlotMenuNav_Handled;
  }
  if (nav == kMenuNav_Up || nav == kMenuNav_Down) {
    int count = SlotRowCount();
    if (count > 0) s_slots.row = (s_slots.row + direction + count) % count;
  } else if (nav == kMenuNav_TabPrev || nav == kMenuNav_TabNext) {
    if (s_slots.screen == kSlotScreen_Randomizer || s_slots.screen == kSlotScreen_Regions) {
      int count =
          s_slots.screen == kSlotScreen_Regions ? kOverlayRegionPage_Count : kSlotRandomPageCount;
      s_slots.tab = (s_slots.tab + direction + count) % count;
      s_slots.row = s_slots.scroll = 0;
    }
  } else if (nav == kMenuNav_Left || nav == kMenuNav_Right ||
             (nav == kMenuNav_Confirm && !repeat)) {
    if (s_slots.screen == kSlotScreen_Setup) {
      int row = SlotSetupCode(s_slots.row);
      if (row == 0 && SlotRandomizerVisible())
        s_slots.draft.randomizer.enabled = !s_slots.draft.randomizer.enabled;
      else if (nav == kMenuNav_Confirm) {
        if (row == 1)
          SlotEnterSetupPage(kSlotScreen_Seed);
        else if (row == 2)
          s_slots.draft.randomizer.seed = Randomizer_NewSeed();
        else if (row == 3)
          SlotEnterSetupPage(kSlotScreen_Randomizer);
        else if (row == 4)
          SlotEnterSetupPage(kSlotScreen_Regions);
        else if (row == 5)
          SlotConfirm();
      }
    } else if (s_slots.screen == kSlotScreen_Randomizer)
      SlotRandomChange(SlotRandomAt(s_slots.row), direction);
    else if (s_slots.screen == kSlotScreen_Regions && s_slot_hooks.view && s_slot_hooks.edit) {
      const OverlayRegionRow *row = OverlayRegionMenu_Row(s_slots.tab, s_slots.row);
      if (!row || !s_slot_hooks.view(&s_slots.draft, &s_slots.draft_view))
        return kSaveSlotMenuNav_Handled;
      int current = row->kind == kOverlayRegionRow_Difficulty
                        ? ActRaiserRegionalSettings_DifficultyChoice(&s_slots.draft_view, false)
                        : OverlayRegionMenu_RowSource(&s_slots.draft_view, row, false);
      int count = row->kind == kOverlayRegionRow_Difficulty ? kArRegionalDifficultyChoice_Count
                  : row->binary                             ? 2
                                                            : 3;
      if (!s_slot_hooks.edit(&s_slots.draft, row, (current + direction + count) % count))
        snprintf(s_slots.error, sizeof(s_slots.error), "%s", Ui("slots.incompatible"));
      else
        s_slots.error[0] = 0;
    }
  }
  return kSaveSlotMenuNav_Handled;
}

/* Read-only formatting of copied campaign details. */

static void SlotFormatTime(uint64_t timestamp, char *out, size_t cap) {
  time_t value = (time_t)timestamp;
  struct tm *local = localtime(&value);
  if (!timestamp || !local || !strftime(out, cap, "%Y-%m-%d %H:%M", local))
    snprintf(out, cap, "%s", Ui("slots.unknown"));
}
static void SlotRelativeTime(uint64_t timestamp, char *out, size_t cap) {
  time_t now = time(NULL);
  uint64_t elapsed = now > 0 && (uint64_t)now > timestamp ? (uint64_t)now - timestamp : 0;
  char count[32];
  const char *key;
  if (!timestamp) {
    snprintf(out, cap, "%s", Ui("slots.unknown"));
    return;
  }
  if (now > 0 && timestamp > (uint64_t)now) {
    SlotFormatTime(timestamp, out, cap);
    return;
  }
  if (elapsed < 60) {
    snprintf(out, cap, "%s", Ui("slots.just_saved"));
    return;
  }
  uint64_t n = elapsed / 60;
  key = "slots.minutes";
  if (n >= 60) {
    n /= 60;
    key = "slots.hours";
    if (n >= 24) {
      n /= 24;
      key = "slots.days";
    }
  }
  snprintf(count, sizeof(count), "%llu", (unsigned long long)n);
  ArUiTextArgument arg = {"n", count};
  ArUiCatalog_Format(out, cap, Ui(key), &arg, 1);
}
typedef struct SlotDetailLine {
  char label[160], value[256];
} SlotDetailLine;
static void SlotRegionValue(const ActRaiserRegionalRulesView *view, const OverlayRegionRow *row,
                            bool effective, char *out, size_t capacity) {
  SettingsOverlayRegionBadge badge;
  if (!OverlayRegionMenu_Value(SettingsOverlay_InterfaceLocale(), view, row, effective, out,
                               capacity, &badge))
    snprintf(out, capacity, "%s", Ui("slots.unknown"));
}
static int SlotDetails(SlotDetailLine lines[kSlotDetailCapacity], bool expanded) {
  const SaveSlotDetails *slot = &s_slots.collection.slots[s_slots.selected];
  int n = 0;
#define SLOT_LINE(label_, fmt_, ...)                                                               \
  do {                                                                                             \
    snprintf(lines[n].label, sizeof(lines[n].label), "%s", label_);                                \
    snprintf(lines[n++].value, sizeof(lines[0].value), fmt_, __VA_ARGS__);                         \
  } while (0)
  if (slot->state != kSaveSlot_Ready && !slot->prepared) return 0;
  if (s_slots.tab == kSlotPage_Summary) {
    if (slot->state == kSaveSlot_Ready) {
      char time_text[64];
      SlotFormatTime(slot->saved_at, time_text, sizeof(time_text));
      SLOT_LINE(Ui(slot->approximate_time ? "slots.file_time" : "slots.last_saved"), "%s",
                time_text);
      if (slot->summary.level < 0)
        SLOT_LINE(Ui("slots.level"), "%s", Ui("slots.unknown"));
      else
        SLOT_LINE(Ui("slots.level"), "%d", slot->summary.level);
      if (slot->summary.acts_cleared < 0)
        SLOT_LINE(Ui("slots.acts"), "%s", Ui("slots.unknown"));
      else
        SLOT_LINE(Ui("slots.acts"), "%d / 12", slot->summary.acts_cleared);
      const char *heim = slot->summary.death_heim == 4   ? "slots.cleared"
                         : slot->summary.death_heim == 1 ? "slots.unlocked"
                         : slot->summary.death_heim == 0 ? "slots.locked"
                                                         : "slots.unknown";
      SLOT_LINE("Death Heim", "%s", Ui(heim));
    } else
      SLOT_LINE(Ui("slots.prepared"), "%s", Ui("slots.not_saved"));
    const char *sources[] = {"US", "JP", "EU", Ui("overlay.region.custom")};
    SLOT_LINE(Ui("slots.gameplay"), "%s",
              sources[SlotProfileSource(&slot->regions, kArRegionalProfile_Gameplay)]);
    SLOT_LINE(Ui("slots.presentation"), "%s",
              sources[SlotProfileSource(&slot->regions, kArRegionalProfile_Presentation)]);
    SLOT_LINE(Ui("slots.difficulty"), "%s",
              OverlayRegionMenu_DifficultyLabel(
                  SettingsOverlay_InterfaceLocale(),
                  ActRaiserRegionalSettings_DifficultyChoice(&slot->regions, false)));
    if (SlotRandomizerVisible()) {
      if (slot->legacy)
        SLOT_LINE(Ui("slots.randomizer"), "%s", Ui("slots.legacy"));
      else if (slot->randomizer.enabled)
        SLOT_LINE(Ui("slots.seed"), "%u", slot->randomizer.seed);
      else
        SLOT_LINE(Ui("slots.randomizer"), "%s", Ui("slots.off"));
    }
    const char *towns[] = {"Fillmore", "Bloodpool", "Kasandora", "Aitos", "Marahna", "Northwall"};
    for (int t = 0; expanded && slot->state == kSaveSlot_Ready && t < 6; ++t) {
      int state = slot->summary.towns[t];
      const char *key = state == 0   ? "slots.act1"
                        : state == 2 ? "slots.act1_done"
                        : state == 3 ? "slots.act2"
                        : state == 4 ? "slots.cleared"
                                     : "slots.unknown";
      SLOT_LINE(towns[t], "%s", Ui(key));
    }
  } else if (s_slots.tab == kSlotPage_Regions) {
    const SaveSlotDetails *active = &s_slots.collection.slots[s_slots.collection.active];
    bool comparison =
        active->state == kSaveSlot_Ready && s_slots.selected != (int)s_slots.collection.active;
    for (int page = 1; page < kOverlayRegionPage_Count && n < kSlotDetailCapacity - 1; ++page)
      for (unsigned i = 0; i < OverlayRegionMenu_Count(page) && n < kSlotDetailCapacity - 1; ++i) {
        const OverlayRegionRow *row = OverlayRegionMenu_Row(page, i);
        if (row->kind == kOverlayRegionRow_Preset) continue;
        char before[100], after[100];
        SlotRegionValue(&slot->regions, row, false, after, sizeof(after));
        bool pending = row->kind == kOverlayRegionRow_Difficulty
                           ? ActRaiserRegionalSettings_DifficultyChoice(&slot->regions, false) !=
                                 ActRaiserRegionalSettings_DifficultyChoice(&slot->regions, true)
                           : OverlayRegionMenu_RowSource(&slot->regions, row, false) !=
                                 OverlayRegionMenu_RowSource(&slot->regions, row, true);
        if (comparison) {
          SlotRegionValue(&active->regions, row, false, before, sizeof(before));
          if (!strcmp(before, after) && !pending) continue;
          SLOT_LINE(OverlayRegionMenu_Label(SettingsOverlay_InterfaceLocale(), row), "%s > %s",
                    before, after);
        } else
          SLOT_LINE(OverlayRegionMenu_Label(SettingsOverlay_InterfaceLocale(), row), "%s", after);
        if (pending && n < kSlotDetailCapacity - 1) {
          SlotRegionValue(&slot->regions, row, true, before, sizeof(before));
          SLOT_LINE(Ui("slots.pending"), "%s > %s", before, after);
        }
      }
    if (!n) SLOT_LINE(Ui("slots.regions"), "%s", Ui("slots.same"));
  } else if (SlotRandomizerVisible()) {
    if (slot->legacy) {
      SLOT_LINE(Ui("slots.randomizer"), "%s", Ui("slots.legacy"));
      return n;
    }
    SLOT_LINE(Ui("slots.randomizer"), "%s",
              Ui(slot->randomizer.enabled ? "slots.on" : "slots.off"));
    SLOT_LINE(Ui("slots.seed"), "%u", slot->randomizer.seed);
    SLOT_LINE(Ui("slots.generator"), "%u", slot->randomizer.generator);
    for (unsigned i = 0; i < sizeof(kSlotRandomRows) / sizeof(kSlotRandomRows[0]); ++i) {
      const SlotRandomRow *r = &kSlotRandomRows[i];
      const SettingDesc *desc = Settings_Find(r->key);
      if (!desc) continue;
      char text[128];
      SlotRandomText(&slot->randomizer, r, text, sizeof(text));
      SLOT_LINE(SettingsOverlay_LocalizedLabel(SettingsOverlay_InterfaceLocale(), desc), "%s",
                text);
    }
  }
#undef SLOT_LINE
  return n;
}
static void SlotOpenDetails(void) {
  SettingsOverlayDetailsText text = {0};
  if (s_slots.screen == kSlotScreen_Regions && s_slot_hooks.view) {
    if (!s_slot_hooks.view(&s_slots.draft, &s_slots.draft_view)) return;
    const OverlayRegionRow *row = OverlayRegionMenu_Row(s_slots.tab, s_slots.row);
    if (!OverlayRegionMenu_Description(SettingsOverlay_InterfaceLocale(), &s_slots.draft_view, row,
                                       text.body, sizeof(text.body)))
      return;
    snprintf(text.title, sizeof(text.title), "%s",
             OverlayRegionMenu_Label(SettingsOverlay_InterfaceLocale(), row));
  } else if (s_slots.screen == kSlotScreen_Randomizer) {
    const SlotRandomRow *row = SlotRandomAt(s_slots.row);
    const SettingDesc *desc = row ? Settings_Find(row->key) : NULL;
    if (!desc) return;
    snprintf(text.title, sizeof(text.title), "%s",
             SettingsOverlay_LocalizedLabel(SettingsOverlay_InterfaceLocale(), desc));
    snprintf(text.body, sizeof(text.body), "%s",
             SettingsOverlay_LocalizedHelp(SettingsOverlay_InterfaceLocale(), desc));
  } else if (s_slots.screen == kSlotScreen_List && !s_slots.advanced_focus) {
    SlotDetailLine lines[kSlotDetailCapacity];
    int count = SlotDetails(lines, true);
    size_t used = 0;
    snprintf(text.title, sizeof(text.title), "%s", Ui("slots.manager"));
    for (int i = 0; i < count; ++i) {
      int n = snprintf(text.body + used, sizeof(text.body) - used, "%s: %s\n", lines[i].label,
                       lines[i].value);
      if (n < 0 || (size_t)n >= sizeof(text.body) - used) break;
      used += (size_t)n;
    }
  }
  if (!text.body[0]) return;
  SettingsOverlay_ShowDetails(&text);
}

/* Native overlay presentation: game-font rows, shared panel art and input
 * hints. Smaller text is reserved for timestamps, tabs and explanatory copy. */

static void SlotTextRow(const MenuLayout *layout, int x, int y, int width, const char *label,
                        const char *value, bool selected, bool metadata) {
  if (selected) {
    FillLogicalRect(layout, x - 6, y - 3, width + 12, 15, kHighlight);
    FillLogicalRect(layout, x - 6, y - 3, 2, 15, kSelectYellow);
  }
  int cell = metadata ? kDebugGlyphWidth : kGlyphSize;
  int value_chars = CappedTextLength(value, width / (cell * 2));
  int label_chars = (width - value_chars * cell - (value_chars ? 12 : 0)) / cell;
  if (metadata) {
    DrawSmallTextN(layout, x, y, label, label_chars, kMutedText);
    DrawSmallTextN(layout, x + width - value_chars * cell, y, value, value_chars, kSteelBlue);
  } else {
    DrawTextN(layout, x, y, label, label_chars, selected ? kText_Value : kText_Normal);
    DrawTextN(layout, x + width - value_chars * cell, y, value, value_chars, kText_Value);
  }
}
static void SlotTabs(const MenuLayout *layout, int x, int y, int width, const char *const *tabs,
                     int count) {
  int total = 0;
  for (int i = 0; i < count; ++i)
    total += SmallTextWidth(Ui(tabs[i])) + 14;
  if (total <= width) {
    for (int i = 0; i < count; ++i) {
      int span = SmallTextWidth(Ui(tabs[i])) + 12;
      if (i == s_slots.tab) {
        FillLogicalRect(layout, x, y - 3, span, 14, kHighlight);
        FillLogicalRect(layout, x, y + 10, span, 1, kSelectYellow);
      }
      DrawSmallText(layout, x + 6, y, Ui(tabs[i]), i == s_slots.tab ? kSelectYellow : kSteelBlue);
      x += span + 2;
    }
  } else {
    char page[24];
    snprintf(page, sizeof(page), "< %d/%d >", s_slots.tab + 1, count);
    int page_width = SmallTextWidth(page);
    DrawSmallTextN(layout, x, y, Ui(tabs[s_slots.tab]),
                   (width - page_width - 12) / kDebugGlyphWidth, kSelectYellow);
    DrawSmallText(layout, x + width - page_width, y, page, kSteelBlue);
  }
}
static int SlotWrappedText(const MenuLayout *layout, int x, int y, const char *text, int columns,
                           int limit, bool draw, bool primary) {
  size_t remaining = strlen(text);
  int lines = 0;
  while (remaining && lines < limit) {
    ArInterfaceTextLine slice;
    char buffer[kArInterfaceTextMaximumBytes + 1];
    if (!ArInterfaceText_WrapLine(text, remaining, columns, sizeof(buffer) - 1, &slice) ||
        !slice.consumed)
      break;
    memcpy(buffer, text, slice.bytes);
    buffer[slice.bytes] = 0;
    if (draw) {
      if (primary)
        DrawTextN(layout, x, y + lines * 12, buffer, columns, kText_Normal);
      else
        DrawSmallText(layout, x, y + lines * kSmallLineHeight, buffer, kSteelBlue);
    }
    text += slice.consumed;
    remaining -= slice.consumed;
    ++lines;
  }
  return lines;
}
static void SlotDrawConfirmation(const MenuLayout *layout) {
  const SlotDecision *decision = &s_slots.decision;
  int width = layout->logical_width < 448 ? layout->logical_width - 32 : 416;
  int title_lines =
      SlotWrappedText(layout, 0, 0, decision->title, (width - 32) / kGlyphSize, 2, false, true);
  int body_lines = SlotWrappedText(layout, 0, 0, decision->body, (width - 32) / kDebugGlyphWidth,
                                   10, false, false);
  int height = title_lines * 12 + body_lines * kSmallLineHeight + 80;
  if (height > layout->logical_height - 32) height = layout->logical_height - 32;
  int x = (layout->logical_width - width) / 2, y = (layout->logical_height - height) / 2;
  FillLogicalRect(layout, 0, 0, layout->logical_width, layout->logical_height, ARGB(180, 0, 0, 0));
  DrawDialogPanel(layout, x, y, width, height);
  SlotWrappedText(layout, x + 16, y + 16, decision->title, (width - 32) / kGlyphSize, 2, true,
                  true);
  SlotWrappedText(layout, x + 16, y + 24 + title_lines * 12, decision->body,
                  (width - 32) / kDebugGlyphWidth,
                  (height - title_lines * 12 - 80) / kSmallLineHeight, true, false);
  for (int i = 0; i < 2; ++i) {
    bool selected = decision->accept_selected == (i == 0);
    SlotTextRow(layout, x + 18, y + height - 43 + i * 20, width - 36,
                Ui(i ? "overlay.decision.cancel" : decision->accept), "", selected, false);
  }
}
static void SlotDrawList(const MenuLayout *layout, int width, int height) {
  int advanced_y = height - 12;
  int visible = (advanced_y - 46) / 32;
  if (visible < 1) visible = 1;
  if (s_slots.selected < s_slots.top) s_slots.top = s_slots.selected;
  if (s_slots.selected >= s_slots.top + visible) s_slots.top = s_slots.selected - visible + 1;
  DrawDialogPanel(layout, 8, 8, width, height);
  DrawTextN(layout, 20, 20, Ui("slots.title"), (width - 24) / kGlyphSize, kText_Normal);
  for (int i = s_slots.top; i < kSaveSlotCount && i < s_slots.top + visible; ++i) {
    const SaveSlotDetails *slot = &s_slots.collection.slots[i];
    int y = 42 + (i - s_slots.top) * 32;
    bool selected = i == s_slots.selected && !s_slots.advanced_focus;
    if (selected) {
      FillLogicalRect(layout, 14, y - 4, width - 22, 29,
                      s_slots.detail_focus ? kPanel : kHighlight);
      if (!s_slots.detail_focus) FillLogicalRect(layout, 14, y - 4, 2, 29, kSelectYellow);
    }
    const char *name = Ui(slot->state == kSaveSlot_Unavailable ? "slots.unavailable"
                          : slot->prepared                     ? "slots.prepared"
                                                               : "slots.empty");
    if (slot->state == kSaveSlot_Ready)
      name = slot->summary.name[0] ? slot->summary.name : Ui("slots.unknown");
    char label[128], note[64];
    snprintf(label, sizeof(label), "%02u %s", i + 1, name);
    DrawTextN(layout, 22, y, label, (width - 34) / kGlyphSize, selected ? kText_Normal : kText_Dim);
    if (slot->state == kSaveSlot_Ready)
      SlotRelativeTime(slot->saved_at, note, sizeof(note));
    else
      snprintf(note, sizeof(note), "%s",
               Ui(slot->state == kSaveSlot_Empty ? "slots.not_saved" : "slots.recovery"));
    bool active = i == (int)s_slots.collection.active;
    int cells = (width - 34) / kDebugGlyphWidth;
    int active_cells = active ? CappedTextLength(Ui("slots.active"), 8) : 0;
    DrawSmallTextN(layout, 22, y + 13, note, cells - (active ? active_cells + 1 : 0), kMutedText);
    if (active)
      DrawSmallTextN(layout, 22 + (cells - active_cells) * kDebugGlyphWidth, y + 13,
                     Ui("slots.active"), active_cells, kSteelBlue);
  }
  DrawScrollBar(layout, 8 + width - 12, 38, visible * 32, kSaveSlotCount, visible, s_slots.top,
                kSteelBlue);
  FillLogicalRect(layout, 20, advanced_y - 9, width - 24, 1, kSteelDim);
  SlotTextRow(layout, 22, advanced_y, width - 36, Ui("slots.advanced"), ">", s_slots.advanced_focus,
              false);
}
void SaveSlotMenu_Draw(const MenuLayout *layout) {
  if (s_slots.decision.result == kOverlayDecision_Pending) {
    SlotDrawConfirmation(layout);
    return;
  }
  int width = layout->logical_width, height = layout->logical_height, top_height = height - 80;
  bool list = s_slots.screen == kSlotScreen_List;
  int left = 8, panel_width = width - 16;
  if (list) {
    int list_width = 152;
    left = list_width + 16;
    panel_width = width - left - 8;
    SlotDrawList(layout, list_width, top_height);
  }
  DrawDialogPanel(layout, left, 8, panel_width, top_height);
  if (list && s_slots.detail_focus)
    FillLogicalRect(layout, left + 6, 16, 2, top_height - 16, kSelectYellow);
  DrawOverlayIcon(layout, left + 12, 16, 16, kOverlayIcon_Save, true, 255);
  const SaveSlotDetails *selected = &s_slots.collection.slots[s_slots.selected];
  char heading[320];
  snprintf(heading, sizeof(heading), "%02u  %s", s_slots.selected + 1,
           list ? (selected->state == kSaveSlot_Ready && selected->summary.name[0]
                       ? selected->summary.name
                       : Ui(selected->prepared ? "slots.prepared" : "slots.manager"))
                : Ui("slots.new_game"));
  DrawTextN(layout, left + 34, 20, heading, (panel_width - 46) / kGlyphSize, kText_Normal);
  int x = left + 12, content_width = panel_width - 28;
  if (list) {
    const char *tabs[] = {"slots.summary", "slots.regions_tab", "slots.randomizer"};
    SlotTabs(layout, x, 40, content_width, tabs, SlotPageCount());
    int action_y = top_height - 14;
    int content_y = 65;
    if (s_slots.tab == kSlotPage_Regions && s_slots.selected != (int)s_slots.collection.active &&
        s_slots.collection.slots[s_slots.collection.active].state == kSaveSlot_Ready) {
      char baseline[96], number[16];
      snprintf(number, sizeof(number), "%u", s_slots.collection.active + 1);
      ArUiTextArgument arg = {"slot", number};
      ArUiCatalog_Format(baseline, sizeof(baseline), Ui("slots.compared"), &arg, 1);
      DrawSmallTextN(layout, x, 57, baseline, content_width / kDebugGlyphWidth, kMutedText);
      content_y = 72;
    }
    if (selected->state == kSaveSlot_Unavailable) {
      const char *error =
          selected->error.message[0] ? selected->error.message : Ui("slots.recovery");
      DrawWrappedSmallText(layout, x, content_y, error, content_width / kDebugGlyphWidth,
                           (action_y - content_y - 8) / kSmallLineHeight, kSteelBlue);
    } else if (selected->state == kSaveSlot_Empty && !selected->prepared) {
      DrawWrappedSmallText(layout, x, content_y, Ui("slots.empty.help"),
                           content_width / kDebugGlyphWidth,
                           (action_y - content_y - 8) / kSmallLineHeight, kSteelBlue);
    } else {
      SlotDetailLine lines[kSlotDetailCapacity];
      int count = SlotDetails(lines, false), visible = (action_y - content_y - 8) / 16;
      if (visible < 1) visible = 1;
      if (s_slots.scroll > count - visible) s_slots.scroll = count > visible ? count - visible : 0;
      for (int i = s_slots.scroll; i < count && i < s_slots.scroll + visible; ++i) {
        bool metadata = !strcmp(lines[i].label, Ui("slots.last_saved")) ||
                        !strcmp(lines[i].label, Ui("slots.file_time"));
        SlotTextRow(layout, x, content_y + (i - s_slots.scroll) * 16, content_width, lines[i].label,
                    lines[i].value, false, metadata);
      }
      DrawScrollBar(layout, left + panel_width - 10, content_y, visible * 16, count, visible,
                    s_slots.scroll, kSteelBlue);
    }
    const char *action = "slots.switch";
    bool enabled = s_slots.collection.writable;
    if (selected->state == kSaveSlot_Unavailable) {
      action = "slots.unavailable";
      enabled = false;
    } else if (selected->state == kSaveSlot_Empty)
      action = selected->prepared ? "slots.review_setup" : "slots.new_game";
    else if (s_slots.selected == (int)s_slots.collection.active) {
      action = "slots.already_active";
      enabled = false;
    }
    FillLogicalRect(layout, x, action_y - 8, content_width, 1, kSteelDim);
    SlotTextRow(layout, x, action_y, content_width, Ui(action), "", s_slots.detail_focus && enabled,
                false);
  } else if (s_slots.screen == kSlotScreen_Seed) {
    DrawWrappedSmallText(layout, x, 44, Ui("slots.seed.help"), content_width / kDebugGlyphWidth, 3,
                         kSteelBlue);
    char seed[16];
    snprintf(seed, sizeof(seed), "%09u", s_slots.draft.randomizer.seed);
    DrawTextN(layout, x, 84, seed, kSlotSeedDigits, kText_Value);
    DrawSmallText(layout, x + s_slots.digit * kGlyphSize, 96, "^", kSelectYellow);
  } else {
    const char *random_tabs[] = {"overlay.tab.seed", "overlay.tab.enemies", "overlay.tab.items",
                                 "overlay.tab.simulation"};
    const char *region_tabs[] = {"overlay.region.tabs.presets", "overlay.region.tabs.action",
                                 "overlay.region.tabs.towns", "overlay.region.tabs.controls",
                                 "overlay.region.tabs.presentation"};
    if (s_slots.screen == kSlotScreen_Randomizer)
      SlotTabs(layout, x, 40, content_width, random_tabs, kSlotRandomPageCount);
    else if (s_slots.screen == kSlotScreen_Regions)
      SlotTabs(layout, x, 40, content_width, region_tabs, kOverlayRegionPage_Count);
    else
      DrawSmallTextN(layout, x, 40, Ui("slots.setup"), content_width / kDebugGlyphWidth,
                     kSteelBlue);
    int count = SlotRowCount(), visible = (top_height - 56) / 20;
    if (visible < 1) visible = 1;
    if (s_slots.row < s_slots.scroll) s_slots.scroll = s_slots.row;
    if (s_slots.row >= s_slots.scroll + visible) s_slots.scroll = s_slots.row - visible + 1;
    bool view_valid = s_slots.screen != kSlotScreen_Regions ||
                      (s_slot_hooks.view && s_slot_hooks.view(&s_slots.draft, &s_slots.draft_view));
    for (int i = s_slots.scroll; i < count && i < s_slots.scroll + visible; ++i) {
      const char *label = "";
      char value[128] = "";
      if (s_slots.screen == kSlotScreen_Setup) {
        const char *labels[] = {
            "slots.type", "slots.seed", "slots.new_seed", "slots.options", "slots.starting_regions",
            "slots.start"};
        int code = SlotSetupCode(i);
        label = Ui(labels[code]);
        if (code == 0)
          snprintf(value, sizeof(value), "%s",
                   Ui(s_slots.draft.randomizer.enabled ? "slots.randomized" : "slots.standard"));
        else if (code == 1)
          snprintf(value, sizeof(value), "%u", s_slots.draft.randomizer.seed);
        else if (code == 3 || code == 4)
          snprintf(value, sizeof(value), ">");
      } else if (s_slots.screen == kSlotScreen_Randomizer) {
        const SlotRandomRow *row = SlotRandomAt(i);
        const SettingDesc *desc = row ? Settings_Find(row->key) : NULL;
        if (!desc) continue;
        label = SettingsOverlay_LocalizedLabel(SettingsOverlay_InterfaceLocale(), desc);
        SlotRandomText(&s_slots.draft.randomizer, row, value, sizeof(value));
      } else {
        const OverlayRegionRow *row = OverlayRegionMenu_Row(s_slots.tab, i);
        label = OverlayRegionMenu_Label(SettingsOverlay_InterfaceLocale(), row);
        if (view_valid)
          SlotRegionValue(&s_slots.draft_view, row, false, value, sizeof(value));
        else
          snprintf(value, sizeof(value), "%s", Ui("slots.unknown"));
      }
      SlotTextRow(layout, x, 62 + (i - s_slots.scroll) * 20, content_width, label, value,
                  i == s_slots.row, false);
    }
    DrawScrollBar(layout, left + panel_width - 10, 58, visible * 20, count, visible, s_slots.scroll,
                  kSteelBlue);
  }
  int bottom = height - 64;
  DrawDialogPanel(layout, 8, bottom, width - 16, 56);
  const char *action =
      list ? (s_slots.detail_focus ? "slots.use" : "slots.select") : "slots.draft.help";
  if (list) {
    if (s_slots.advanced_focus)
      action = "slots.advanced.help";
    else if (s_slots.full)
      action = "slots.full";
    else if (selected->state == kSaveSlot_Unavailable)
      action = "slots.recovery";
    else if (selected->prepared && selected->state == kSaveSlot_Empty)
      action = "slots.prepared.help";
    else if (s_slots.detail_focus && s_slots.selected == (int)s_slots.collection.active)
      action = "slots.active.help";
    else if (selected->approximate_time && s_slots.tab == kSlotPage_Summary)
      action = "slots.file_time.help";
  }
  const char *message = s_slots.error[0]                      ? s_slots.error
                        : s_slots.collection.error.message[0] ? s_slots.collection.error.message
                                                              : Ui(action);
  DrawSmallTextPreview(layout, 20, bottom + 10, message, (width - 40) / kDebugGlyphWidth, 2,
                       s_slots.error[0] ? kGameGold : kSteelBlue);
  char move[64], tabs[64], confirm[64], back[64], details[64], advanced[64];
  InputClass device = SettingsOverlay_MenuInputDevice();
  OverlayMenuInput_PairHint(move, sizeof(move), kMenuNav_Up, kMenuNav_Down, device);
  OverlayMenuInput_PairHint(tabs, sizeof(tabs), kMenuNav_TabPrev, kMenuNav_TabNext, device);
  OverlayMenuInput_Hint(confirm, sizeof(confirm), kMenuNav_Confirm, device);
  OverlayMenuInput_Hint(back, sizeof(back), kMenuNav_Back, device);
  OverlayMenuInput_Hint(details, sizeof(details), kMenuNav_Details, device);
  OverlayMenuInput_Hint(advanced, sizeof(advanced), kMenuNav_Reset, device);
  MenuHints hints = {0};
  AddMenuHint(&hints, move,
              Ui(s_slots.screen == kSlotScreen_Seed ? "overlay.hint.adjust"
                 : list && !s_slots.detail_focus    ? "slots.select_hint"
                                                    : "overlay.hint.scroll"));
  if (s_slots.screen == kSlotScreen_Seed) {
    OverlayMenuInput_PairHint(tabs, sizeof(tabs), kMenuNav_Left, kMenuNav_Right, device);
    AddMenuHint(&hints, tabs, Ui("slots.select_hint"));
  }
  if ((list && !s_slots.advanced_focus) || s_slots.screen == kSlotScreen_Regions ||
      s_slots.screen == kSlotScreen_Randomizer)
    AddMenuHint(&hints, tabs, Ui("overlay.hint.page"));
  AddMenuHint(&hints, confirm, Ui("slots.choose"));
  AddMenuHint(&hints, back, Ui("overlay.hint.back"));
  if (list) AddMenuHint(&hints, advanced, Ui("slots.advanced.shortcut"));
  if ((list && !s_slots.advanced_focus) || s_slots.screen == kSlotScreen_Regions ||
      s_slots.screen == kSlotScreen_Randomizer)
    AddMenuHint(&hints, details, Ui("overlay.hint.details"));
  DrawMenuHints(layout, 20, bottom + 37, width - 40, &hints, kSteelBlue, kMutedText);
}
