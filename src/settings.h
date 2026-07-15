#pragma once
#include "types.h"

/* Live runtime settings — the first slice of the g_settings refactor described
 * in docs/settings-system.md (§3.1/§4). Existing cheat and widescreen behavior
 * gates are descriptor-backed here. Their enforcement was already per-frame,
 * so the live fields replace cached getenv() values without moving the seams.
 *
 * Seeded once at boot from the same AR_* env vars the gates used to read, with
 * the same defaults and special value encodings. */

/* Display-mode cycle (F9) — a preset selector over the flags below, for
 * capturing before/after widescreen comparisons without a UI. Custom is not
 * part of the cycle: it identifies an env/menu-authored combination which no
 * longer matches one of the three deterministic capture presets. */
typedef enum {
  kDisplayMode_43 = 0,      /* authentic 4:3: pillarbox, present centre 256px */
  kDisplayMode_WideRaw,     /* wide geometry, zero corrections ("before") */
  kDisplayMode_WideFull,    /* wide + every correction ("after") */
  kDisplayMode_Custom,
  kDisplayMode_Count
} DisplayMode;

enum { kDisplayMode_PresetCount = kDisplayMode_Custom };

typedef enum {
  kSettingType_Bool,
  kSettingType_Int,
  kSettingType_Mask,
  kSettingType_Custom,
} SettingType;

typedef enum {
  kApply_Passive,
  kApply_Callback,
  kApply_Restart,
  kApply_Action,
} SettingApplyKind;

typedef enum {
  kSettingCat_Cheats,
  kSettingCat_Widescreen,
  kSettingCat_Aspect,
  kSettingCat_Display,
  kSettingCat_Audio,
  kSettingCat_Qol,
} SettingCategory;

typedef struct SettingDesc {
  const char *key;          /* stable settings.ini/menu id */
  const char *env;          /* boot-only compatibility seed */
  const char *label;
  const char *tooltip;
  SettingType type;
  SettingApplyKind apply;
  SettingCategory category;
  void *field;
  long defval, minval, maxval, step;
  bool sticky;              /* disabling stops enforcement, cannot undo history */
  bool (*parse)(const char *text, void *field);
} SettingDesc;

typedef struct SettingsPin {
  uint32 off;
  uint8 val;
} SettingsPin;

typedef struct Settings {
  int display_mode;

  /* Cheat values. Zero/false means disabled. Stateful enforcement latches are
   * deliberately kept private to ActRaiser_ApplyCheats, not stored here. */
  bool cheat_all_magic;
  bool cheat_ranged_sword;
  int  cheat_inf_mp;
  bool cheat_inf_sp;
  bool cheat_angel_hp;
  int  cheat_inf_hp;
  bool cheat_freeze_timer;
  int  cheat_moonjump_speed;
  uint16 cheat_moonjump_button;
  int  cheat_no_knockback;
  uint8 pin_count;
  SettingsPin pins[32];

  /* Widescreen behavior. All default ON; the per-frame gates read these. */
  bool ws_action;             /* AR_WS_ACTION            action stages wide */
  bool ws_sim;                /* AR_WS_SIM               sim town wide */
  bool ws_bgrefresh;          /* AR_WS_BGREFRESH         true-content margins */
  bool ws_skypalace_bg;       /* AR_WS_SKYPALACE_BG      sky palace BG2 repair */
  bool ws_sprites;            /* AR_WS_SPRITES           widen sprite emission */
  bool ws_margin_objects;     /* AR_WS_MARGIN_OBJECTS    draw margin objects */
  bool ws_margin_activation;  /* AR_WS_MARGIN_ACTIVATION extend $0400 window */
  bool ws_bg2_padding;        /* AR_WS_BG2_MIRROR        pad decorative BG2 */
  bool ws_sim_sprites;        /* AR_WS_SIM_SPRITES       widen sim components */
} Settings;

extern Settings g_settings;
extern const SettingDesc g_setting_descs[];
extern const int g_setting_desc_count;

void Settings_Init(void);

/* Apply a deterministic display-mode preset over the ws_* flags. Presets
 * overwrite them; editing an individual widescreen field should call
 * Settings_MarkDisplayModeCustom() so the UI never claims a preset is active
 * when its contents have diverged. */
void Settings_SetDisplayMode(int mode);
void Settings_MarkDisplayModeCustom(void);
int  Settings_CycleDisplayMode(void);
const char *Settings_DisplayModeName(int mode);

/* The framebuffer sub-rect the current mode should present: columns
 * [Settings_VisibleX0(), +Settings_VisibleWidth()). 4:3 presents only the
 * authentic centre 256 so the margins are cropped rather than shown as bars.
 * The PPU always renders the full width; nothing is reallocated. */
int Settings_VisibleX0(void);
int Settings_VisibleWidth(void);
