#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Settings g_settings;
static SettingsChangeObserver s_change_observer;

/* g_ws_active / g_ws_extra are the canonical exported widescreen symbols
 * (snesrecomp/runner/src/widescreen.h). The framebuffer width is derived from
 * g_ws_extra exactly as main.c computes it, so we don't need main.c's
 * file-local g_snes_width. */
extern bool g_ws_active;
extern int g_ws_extra;
extern uint8 g_ram[0x20000];

static int InferDisplayMode(void);

static bool ParseInfMp(const char *text, void *field) {
  int *value = (int *)field;
  if (!text || !text[0] || text[0] == '0') *value = 0;
  else if (text[0] == '1' && !text[1]) *value = 10;
  else *value = (int)strtoul(text, NULL, 0);
  return true;
}

static bool ParseInfHp(const char *text, void *field) {
  int *value = (int *)field;
  /* Preserve the historical leading-zero disable test exactly. */
  if (!text || !text[0] || text[0] == '0') *value = 0;
  else *value = (int)strtoul(text, NULL, 0);
  return true;
}

static bool ParseMoonjump(const char *text, void *field) {
  int *value = (int *)field;
  if (!text || !text[0] || text[0] == '0') *value = 0;
  else if (text[0] == '1' && !text[1]) *value = 6;
  else *value = (int)strtoul(text, NULL, 0);
  return true;
}

static bool ParseNoKnockback(const char *text, void *field) {
  int *value = (int *)field;
  if (!text || !text[0] || text[0] == '0') *value = 0;
  else if (text[0] == '1' && !text[1]) *value = 1;
  else *value = (int)(strtoul(text, NULL, 16) & 0x3f);
  return true;
}

static int FormatNoKnockback(char *buffer, int buffer_size,
                             const void *field) {
  int value = *(const int *)field;
  return value <= 1 ? snprintf(buffer, buffer_size, "%d", value)
                    : snprintf(buffer, buffer_size, "%X", value);
}

static bool ParsePins(const char *text, void *field) {
  uint8 *count = (uint8 *)field;
  *count = 0;
  if (!text || !text[0]) return true;

  const char *p = text;
  while (*p && *count < 32) {
    char token[16] = {0};
    int len = 0;
    while (*p && *p != ',' && len < 15) token[len++] = *p++;
    if (*p == ',') p++;
    uint32 code = (uint32)strtoul(token, NULL, 16);
    uint8 bank = (uint8)(code >> 24);
    uint16 addr = (uint16)(code >> 8);
    if (len == 8 && (bank == 0x7e || bank == 0x7f)) {
      SettingsPin *pin = &g_settings.pins[*count];
      pin->off = ((uint32)(bank & 1) << 16) | addr;
      pin->val = (uint8)code;
      (*count)++;
    } else {
      fprintf(stderr, "AR_PIN: bad token '%s' "
              "(want 8-hex PAR 7Exxxxvv/7Fxxxxvv)\n", token);
    }
  }
  if (*count) fprintf(stderr, "AR_PIN: %u pin(s) active\n", (unsigned)*count);
  return true;
}

static int FormatPins(char *buffer, int buffer_size, const void *field) {
  const uint8 count = *(const uint8 *)field;
  int used = 0;
  if (!buffer || buffer_size <= 0) return 0;
  buffer[0] = 0;
  for (int i = 0; i < count; i++) {
    const SettingsPin *pin = &g_settings.pins[i];
    unsigned bank = 0x7e + ((pin->off >> 16) & 1);
    int wrote = snprintf(buffer + used, buffer_size - used, "%s%02X%04X%02X",
                         i ? "," : "", bank, (unsigned)(pin->off & 0xffff),
                         (unsigned)pin->val);
    if (wrote < 0) return used;
    if (wrote >= buffer_size - used) {
      buffer[buffer_size - 1] = 0;
      return buffer_size - 1;
    }
    used += wrote;
  }
  return used;
}

static bool ActionModeAvailable(void) {
  return g_ram[0x18] >= 0x01 && g_ram[0x18] <= 0x07;
}

static void WidescreenSettingChanged(const SettingDesc *desc) {
  (void)desc;
  g_settings.display_mode = InferDisplayMode();
}

static void DisplayModeChanged(const SettingDesc *desc) {
  Settings_SetDisplayMode(*(const int *)desc->field);
}

static int FormatHudScale(char *buffer, int buffer_size, const void *field) {
  int value = *(const int *)field;
  if (!value) return snprintf(buffer, (size_t)buffer_size, "Match game");
  return snprintf(buffer, (size_t)buffer_size, "%d.%02dx",
                  value / 100, value % 100);
}

static bool ParseHudScale(const char *text, void *field) {
  int *out = (int *)field;
  if (!strcmp(text, "Match game") || !strcmp(text, "match")) {
    *out = 0;
    return true;
  }
  char *end = NULL;
  double value = strtod(text, &end);
  if (!end || end == text) return false;
  if (*end == 'x' && end[1] == 0) {
    *out = (int)(value * 100.0 + 0.5);
    return true;
  }
  if (*end) return false;
  *out = (int)value;
  return true;
}

static const char *const kDisplayModeLabels[] = {
  "4:3 authentic",
  "Widescreen raw",
  "Widescreen full",
};

#define BOOL_SETTING(id, env_name, text, help, cat, def, is_sticky, active, changed) \
  { #id, env_name, text, help, kSettingType_Bool, kApply_Passive, cat, \
    &g_settings.id, def, 0, 1, 1, is_sticky, NULL, 0, active, changed, \
    NULL, NULL }
#define INT_SETTING(id, env_name, text, help, cat, def, lo, hi, parser, active) \
  { #id, env_name, text, help, kSettingType_Int, kApply_Passive, cat, \
    &g_settings.id, def, lo, hi, 1, false, NULL, 0, active, NULL, \
    parser, NULL }

const SettingDesc g_setting_descs[] = {
  { "display_mode", "AR_DISPLAY_MODE", "Render profile",
    "Switch between authentic 4:3, uncorrected wide output, and full HLE widescreen.",
    kSettingType_Enum, kApply_Action, kSettingCat_Display,
    &g_settings.display_mode, kDisplayMode_WideFull, kDisplayMode_43,
    kDisplayMode_WideFull, 1, false, kDisplayModeLabels,
    kDisplayMode_PresetCount, NULL, DisplayModeChanged, NULL, NULL },
  { "hud_scale_percent", "AR_HUD_SCALE", "HUD output scale",
    "Scale the promoted HUD after game upscaling; 100 is native 1x output pixels.",
    kSettingType_Int, kApply_Passive, kSettingCat_Display,
    &g_settings.hud_scale_percent, 0, 0, 400, 25, false, NULL, 0,
    NULL, NULL, ParseHudScale, FormatHudScale },
  BOOL_SETTING(cheat_all_magic, "AR_ALL_MAGIC", "All magic",
               "Unlock all four spells; disabling cannot undo unlocks already written.",
               kSettingCat_Cheats, 0, true, NULL, NULL),
  BOOL_SETTING(cheat_ranged_sword, "AR_RANGED_SWORD", "Ranged sword",
               "Pin the sword-projectile flag while enabled.",
               kSettingCat_Cheats, 0, false, NULL, NULL),
  INT_SETTING(cheat_inf_mp, "AR_INF_MP", "Infinite MP",
              "Pin working magic scrolls; 1 maps to 10 for env compatibility.",
              kSettingCat_Cheats, 0, 0, 255, ParseInfMp, NULL),
  BOOL_SETTING(cheat_inf_sp, "AR_INF_SP", "Infinite SP",
               "Pin current simulation SP to its live maximum.",
               kSettingCat_Cheats, 0, false, NULL, NULL),
  BOOL_SETTING(cheat_angel_hp, "AR_ANGEL_HP", "Infinite angel HP",
               "Pin current angel HP to its live maximum.",
               kSettingCat_Cheats, 0, false, NULL, NULL),
  INT_SETTING(cheat_inf_hp, "AR_INF_HP", "Infinite action HP",
              "1 tracks the stage high-water mark; larger values are literal HP.",
              kSettingCat_Cheats, 0, 0, 255, ParseInfHp, ActionModeAvailable),
  BOOL_SETTING(cheat_freeze_timer, "AR_FREEZE_TIMER", "Freeze timer",
               "Pin the action timer until the boss tally drain is detected.",
               kSettingCat_Cheats, 0, false, ActionModeAvailable, NULL),
  INT_SETTING(cheat_moonjump_speed, "AR_MOONJUMP", "Moonjump speed",
              "Pixels per frame while the configured button is held; 1 maps to 6.",
              kSettingCat_Cheats, 0, 0, 255, ParseMoonjump,
              ActionModeAvailable),
  { "cheat_moonjump_button", "AR_MOONJUMP_BTN", "Moonjump button",
    "SNES auto-joypad mask; default $8000 is B.", kSettingType_Mask,
    kApply_Passive, kSettingCat_Cheats, &g_settings.cheat_moonjump_button,
    0x8000, 0, 0xffff, 1, false, NULL, 0, ActionModeAvailable, NULL,
    NULL, NULL },
  { "cheat_no_knockback", "AR_NO_KNOCKBACK", "No knockback",
    "1 is full invulnerability; other values are raw hex object offsets.",
    kSettingType_Int, kApply_Passive, kSettingCat_Cheats,
    &g_settings.cheat_no_knockback, 0, 0, 0x3f, 1, false, NULL, 0,
    ActionModeAvailable, NULL, ParseNoKnockback, FormatNoKnockback },
  { "pins", "AR_PIN", "Custom PAR pins",
    "Comma-separated 7Exxxxvv/7Fxxxxvv codes, enforced every frame.",
    kSettingType_Custom, kApply_Passive, kSettingCat_Cheats,
    &g_settings.pin_count, 0, 0, 32, 1, true, NULL, 0, NULL, NULL,
    ParsePins, FormatPins },

  BOOL_SETTING(ws_action, "AR_WS_ACTION", "Wide action stages",
               "Enable wide geometry in action regions.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
  BOOL_SETTING(ws_sim, "AR_WS_SIM", "Wide simulation towns",
               "Enable wide geometry in simulation towns.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
  BOOL_SETTING(ws_bgrefresh, "AR_WS_BGREFRESH", "BG margin refresh",
               "Decode true action tilemap content into side margins.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
  BOOL_SETTING(ws_skypalace_bg, "AR_WS_SKYPALACE_BG", "Sky Palace BG repair",
               "Reconstruct box-free colonnade tiles in BG2 margins.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
  BOOL_SETTING(ws_sprites, "AR_WS_SPRITES", "Wide action sprites",
               "Emit action sprite components into side margins.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
  BOOL_SETTING(ws_margin_objects, "AR_WS_MARGIN_OBJECTS", "Draw margin objects",
               "Draw initialized action objects in the wide view.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
  BOOL_SETTING(ws_margin_activation, "AR_WS_MARGIN_ACTIVATION", "Activate margin objects",
               "Extend action object activation to the live wide window.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
  BOOL_SETTING(ws_bg2_padding, "AR_WS_BG2_MIRROR", "Decorative BG2 padding",
               "Use the mapped mirror/repeat strategies for 256px BG2 layers.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
  BOOL_SETTING(ws_sim_sprites, "AR_WS_SIM_SPRITES", "Wide simulation sprites",
               "Widen town world sprites and angel projectile lifetime.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
};

const int g_setting_desc_count =
    (int)(sizeof(g_setting_descs) / sizeof(g_setting_descs[0]));

const SettingDesc *Settings_Find(const char *key) {
  if (!key) return NULL;
  for (int i = 0; i < g_setting_desc_count; i++)
    if (strcmp(g_setting_descs[i].key, key) == 0)
      return &g_setting_descs[i];
  return NULL;
}

bool Settings_IsAvailable(const SettingDesc *desc) {
  return desc && (!desc->available || desc->available());
}

bool Settings_GetLong(const SettingDesc *desc, long *value) {
  if (!desc || !value) return false;
  switch (desc->type) {
    case kSettingType_Bool: *value = *(const bool *)desc->field; return true;
    case kSettingType_Int:
    case kSettingType_Enum: *value = *(const int *)desc->field; return true;
    case kSettingType_Mask: *value = *(const uint16 *)desc->field; return true;
    case kSettingType_Custom: return false;
  }
  return false;
}

static long NormalizeLong(const SettingDesc *desc, long value) {
  if (desc->type == kSettingType_Bool) return value != 0;
  if (desc->field == &g_settings.display_mode && !g_ws_active)
    return kDisplayMode_43;
  if (value < desc->minval) value = desc->minval;
  if (value > desc->maxval) value = desc->maxval;
  if (desc->step > 1)
    value = desc->minval +
            ((value - desc->minval) / desc->step) * desc->step;
  return value;
}

static SettingChangeResult FinishChange(const SettingDesc *desc,
                                        bool sticky_disable) {
  if (desc->on_change) desc->on_change(desc);
  SettingChangeResult result = sticky_disable
      ? kSettingChange_AppliedStickyDisable
      : desc->apply == kApply_Restart
          ? kSettingChange_RestartPending
          : kSettingChange_Applied;
  if (s_change_observer) s_change_observer(desc, result);
  return result;
}

SettingChangeResult Settings_SetLong(const SettingDesc *desc, long value) {
  long old_value;
  if (!Settings_GetLong(desc, &old_value)) return kSettingChange_Rejected;
  value = NormalizeLong(desc, value);
  if (old_value == value) return kSettingChange_Unchanged;

  switch (desc->type) {
    case kSettingType_Bool: *(bool *)desc->field = value != 0; break;
    case kSettingType_Int:
    case kSettingType_Enum: *(int *)desc->field = (int)value; break;
    case kSettingType_Mask: *(uint16 *)desc->field = (uint16)value; break;
    case kSettingType_Custom: return kSettingChange_Rejected;
  }
  return FinishChange(desc, desc->sticky && old_value != 0 && value == 0);
}

SettingChangeResult Settings_SetText(const SettingDesc *desc,
                                     const char *text) {
  if (!desc || !text) return kSettingChange_Rejected;
  if (desc->type == kSettingType_Custom) {
    if (!desc->parse) return kSettingChange_Rejected;
    char before[512], after[512];
    Settings_FormatValue(desc, before, sizeof(before));
    if (!desc->parse(text, desc->field)) return kSettingChange_Rejected;
    Settings_FormatValue(desc, after, sizeof(after));
    if (strcmp(before, after) == 0) return kSettingChange_Unchanged;
    return FinishChange(desc, desc->sticky && before[0] && !after[0]);
  }

  long value = 0;
  if (desc->type == kSettingType_Bool) {
    if (!strcmp(text, "off") || !strcmp(text, "Off") ||
        !strcmp(text, "false") || !strcmp(text, "False") ||
        !strcmp(text, "no") || !strcmp(text, "No") || text[0] == '0')
      value = 0;
    else if (text[0])
      value = 1;
    else
      return kSettingChange_Rejected;
  } else if (desc->type == kSettingType_Enum) {
    char *end = NULL;
    value = strtol(text, &end, 0);
    if (!end || *end) {
      for (int i = 0; i < desc->enum_count; i++) {
        if (!strcmp(text, desc->enum_labels[i])) {
          value = i;
          end = (char *)text + strlen(text);
          break;
        }
      }
    }
    if (!end || *end) return kSettingChange_Rejected;
  } else if (desc->type == kSettingType_Mask) {
    const char *number = text[0] == '$' ? text + 1 : text;
    char *end = NULL;
    value = strtol(number, &end, 16);
    if (!end || *end) return kSettingChange_Rejected;
  } else {
    int parsed = 0;
    if (desc->parse) {
      if (!desc->parse(text, &parsed)) return kSettingChange_Rejected;
      value = parsed;
    } else {
      char *end = NULL;
      value = strtol(text, &end, 0);
      if (!end || *end) return kSettingChange_Rejected;
    }
  }
  return Settings_SetLong(desc, value);
}

SettingChangeResult Settings_Reset(const SettingDesc *desc) {
  if (!desc) return kSettingChange_Rejected;
  if (desc->type == kSettingType_Custom)
    return Settings_SetText(desc, "");
  return Settings_SetLong(desc, desc->defval);
}

int Settings_FormatValue(const SettingDesc *desc, char *buffer,
                         int buffer_size) {
  if (!desc || !buffer || buffer_size <= 0) return 0;
  if (desc->format) return desc->format(buffer, buffer_size, desc->field);
  switch (desc->type) {
    case kSettingType_Bool:
      return snprintf(buffer, buffer_size, "%s",
                      *(const bool *)desc->field ? "On" : "Off");
    case kSettingType_Int:
      return snprintf(buffer, buffer_size, "%d", *(const int *)desc->field);
    case kSettingType_Enum: {
      int value = *(const int *)desc->field;
      if (value >= 0 && value < desc->enum_count)
        return snprintf(buffer, buffer_size, "%s", desc->enum_labels[value]);
      return snprintf(buffer, buffer_size, "%d", value);
    }
    case kSettingType_Mask:
      return snprintf(buffer, buffer_size, "$%04X",
                      (unsigned)*(const uint16 *)desc->field);
    case kSettingType_Custom:
      buffer[0] = 0;
      return 0;
  }
  buffer[0] = 0;
  return 0;
}

void Settings_SetChangeObserver(SettingsChangeObserver observer) {
  s_change_observer = observer;
}

const char *Settings_CategoryName(SettingCategory category) {
  switch (category) {
    case kSettingCat_Cheats: return "Cheats";
    case kSettingCat_Widescreen: return "Widescreen";
    case kSettingCat_Aspect: return "Aspect";
    case kSettingCat_Display: return "Display";
    case kSettingCat_Audio: return "Audio";
    case kSettingCat_Qol: return "Quality of life";
  }
  return "Unknown";
}

const char *Settings_ApplyKindName(SettingApplyKind apply) {
  switch (apply) {
    case kApply_Passive: return "Live";
    case kApply_Callback: return "Live callback";
    case kApply_Restart: return "Restart required";
    case kApply_Action: return "Action";
  }
  return "Unknown";
}

const char *Settings_ChangeResultName(SettingChangeResult result) {
  switch (result) {
    case kSettingChange_Rejected: return "rejected";
    case kSettingChange_Unchanged: return "unchanged";
    case kSettingChange_Applied: return "applied";
    case kSettingChange_AppliedStickyDisable: return "applied (sticky history remains)";
    case kSettingChange_RestartPending: return "saved (restart pending)";
  }
  return "unknown";
}

static void SeedSetting(const SettingDesc *desc, bool seed_env) {
  switch (desc->type) {
    case kSettingType_Bool:
      *(bool *)desc->field = desc->defval != 0;
      break;
    case kSettingType_Int:
    case kSettingType_Enum:
      *(int *)desc->field = (int)desc->defval;
      break;
    case kSettingType_Mask:
      *(uint16 *)desc->field = (uint16)desc->defval;
      break;
    case kSettingType_Custom:
      break;
  }

  const char *text = seed_env && desc->env ? getenv(desc->env) : NULL;
  if (!text) return;
  if (desc->parse) {
    desc->parse(text, desc->field);
  } else if (desc->type == kSettingType_Bool) {
    /* Preserve both historical polarities: default-on repairs are disabled
     * only by a leading zero; default-off cheats require a nonempty nonzero. */
    *(bool *)desc->field = desc->defval
                              ? text[0] != '0'
                              : text[0] && text[0] != '0';
  } else if ((desc->type == kSettingType_Int ||
              desc->type == kSettingType_Enum) && text[0]) {
    *(int *)desc->field = (int)strtoul(text, NULL, 0);
  } else if (desc->type == kSettingType_Mask && text[0]) {
    *(uint16 *)desc->field = (uint16)strtoul(text, NULL, 16);
  }
}

static int InferDisplayMode(void) {
  if (!g_ws_active)
    return kDisplayMode_43;

  const bool raw = g_settings.ws_action && g_settings.ws_sim &&
                   !g_settings.ws_bgrefresh &&
                   !g_settings.ws_skypalace_bg &&
                   !g_settings.ws_sprites &&
                   !g_settings.ws_margin_objects &&
                   !g_settings.ws_margin_activation &&
                   !g_settings.ws_bg2_padding &&
                   !g_settings.ws_sim_sprites;
  if (raw)
    return kDisplayMode_WideRaw;

  const bool full = g_settings.ws_action && g_settings.ws_sim &&
                    g_settings.ws_bgrefresh &&
                    g_settings.ws_skypalace_bg &&
                    g_settings.ws_sprites &&
                    g_settings.ws_margin_objects &&
                    g_settings.ws_margin_activation &&
                    g_settings.ws_bg2_padding &&
                    g_settings.ws_sim_sprites;
  return full ? kDisplayMode_WideFull : kDisplayMode_Custom;
}

void Settings_Init(void) {
  memset(&g_settings, 0, sizeof(g_settings));
  for (int i = 0; i < g_setting_desc_count; i++)
    SeedSetting(&g_setting_descs[i],
                g_setting_descs[i].field != &g_settings.display_mode);

  /* Name the state the env actually produced. This matters when one AR_WS_*
   * switch is disabled: calling that combination FULL would make the source
   * of truth lie to the future settings UI. */
  g_settings.display_mode = InferDisplayMode();

  /* AR_DISPLAY_MODE=0|1|2 picks the starting preset (0=4:3, 1=wide raw,
   * 2=wide full). Applying a preset overwrites the individual ws_* flags
   * above, so it is the last word — this is the headless/scripted equivalent
   * of pressing F9, and how the mode comparison is captured without a UI. */
  const SettingDesc *display = Settings_Find("display_mode");
  const char *dm = display && display->env ? getenv(display->env) : NULL;
  if (dm && dm[0]) {
    int m = atoi(dm);
    if (m >= 0 && m < kDisplayMode_PresetCount) {
      Settings_SetDisplayMode(m);
      fprintf(stderr, "[display] AR_DISPLAY_MODE=%d -> %s%s\n", m,
              Settings_DisplayModeName(g_settings.display_mode),
              g_settings.display_mode != m
                  ? " (wide framebuffer unavailable)" : "");
    } else {
      fprintf(stderr, "[display] AR_DISPLAY_MODE=%s out of range (0..%d)\n",
              dm, kDisplayMode_PresetCount - 1);
    }
  }
}

void Settings_SetDisplayMode(int mode) {
  if (mode < 0 || mode >= kDisplayMode_PresetCount) return;
  if (!g_ws_active && mode != kDisplayMode_43)
    mode = kDisplayMode_43;
  g_settings.display_mode = mode;

  /* 4:3 clears every flag; the policy additionally forces `wide = 0` outright,
   * because scenes like the Sky Palace hub and the Mode-7 world map set wide
   * unconditionally and would otherwise ignore these. */
  bool wide = (mode != kDisplayMode_43);
  bool corrections = (mode == kDisplayMode_WideFull);

  g_settings.ws_action            = wide;
  g_settings.ws_sim               = wide;
  g_settings.ws_bgrefresh         = corrections;
  g_settings.ws_skypalace_bg      = corrections;
  g_settings.ws_sprites           = corrections;
  g_settings.ws_margin_objects    = corrections;
  g_settings.ws_margin_activation = corrections;
  g_settings.ws_bg2_padding       = corrections;
  g_settings.ws_sim_sprites       = corrections;
}

int Settings_CycleDisplayMode(void) {
  /* A bespoke env/menu combination enters the comparison cycle at the
   * authentic baseline; the cycle itself always remains exactly three steps. */
  int next = (g_settings.display_mode >= 0 &&
              g_settings.display_mode < kDisplayMode_PresetCount)
                 ? (g_settings.display_mode + 1) % kDisplayMode_PresetCount
                 : kDisplayMode_43;
  Settings_SetDisplayMode(next);
  return g_settings.display_mode;
}

const char *Settings_DisplayModeName(int mode) {
  switch (mode) {
    case kDisplayMode_43:       return "4:3 authentic";
    case kDisplayMode_WideRaw:  return "widescreen RAW (no corrections)";
    case kDisplayMode_WideFull: return "widescreen FULL (all corrections)";
    case kDisplayMode_Custom:   return "widescreen CUSTOM";
    default:                    return "?";
  }
}

/* Framebuffer layout is [extra][256][extra] (g_snes_width total), so the
 * authentic view is the centre 256 columns starting at g_ws_extra. */
int Settings_VisibleX0(void) {
  return (g_settings.display_mode == kDisplayMode_43) ? g_ws_extra : 0;
}

int Settings_VisibleWidth(void) {
  return (g_settings.display_mode == kDisplayMode_43) ? 256
                                                      : 256 + 2 * g_ws_extra;
}
