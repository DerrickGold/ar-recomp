#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Settings g_settings;

/* g_ws_active / g_ws_extra are the canonical exported widescreen symbols
 * (snesrecomp/runner/src/widescreen.h). The framebuffer width is derived from
 * g_ws_extra exactly as main.c computes it, so we don't need main.c's
 * file-local g_snes_width. */
extern bool g_ws_active;
extern int g_ws_extra;

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

#define BOOL_SETTING(id, env_name, text, help, cat, def, is_sticky) \
  { #id, env_name, text, help, kSettingType_Bool, kApply_Passive, cat, \
    &g_settings.id, def, 0, 1, 1, is_sticky, NULL }
#define INT_SETTING(id, env_name, text, help, cat, def, lo, hi, parser) \
  { #id, env_name, text, help, kSettingType_Int, kApply_Passive, cat, \
    &g_settings.id, def, lo, hi, 1, false, parser }

const SettingDesc g_setting_descs[] = {
  BOOL_SETTING(cheat_all_magic, "AR_ALL_MAGIC", "All magic",
               "Unlock all four spells; disabling cannot undo unlocks already written.",
               kSettingCat_Cheats, 0, true),
  BOOL_SETTING(cheat_ranged_sword, "AR_RANGED_SWORD", "Ranged sword",
               "Pin the sword-projectile flag while enabled.",
               kSettingCat_Cheats, 0, false),
  INT_SETTING(cheat_inf_mp, "AR_INF_MP", "Infinite MP",
              "Pin working magic scrolls; 1 maps to 10 for env compatibility.",
              kSettingCat_Cheats, 0, 0, 255, ParseInfMp),
  BOOL_SETTING(cheat_inf_sp, "AR_INF_SP", "Infinite SP",
               "Pin current simulation SP to its live maximum.",
               kSettingCat_Cheats, 0, false),
  BOOL_SETTING(cheat_angel_hp, "AR_ANGEL_HP", "Infinite angel HP",
               "Pin current angel HP to its live maximum.",
               kSettingCat_Cheats, 0, false),
  INT_SETTING(cheat_inf_hp, "AR_INF_HP", "Infinite action HP",
              "1 tracks the stage high-water mark; larger values are literal HP.",
              kSettingCat_Cheats, 0, 0, 255, ParseInfHp),
  BOOL_SETTING(cheat_freeze_timer, "AR_FREEZE_TIMER", "Freeze timer",
               "Pin the action timer until the boss tally drain is detected.",
               kSettingCat_Cheats, 0, false),
  INT_SETTING(cheat_moonjump_speed, "AR_MOONJUMP", "Moonjump speed",
              "Pixels per frame while the configured button is held; 1 maps to 6.",
              kSettingCat_Cheats, 0, 0, 255, ParseMoonjump),
  { "cheat_moonjump_button", "AR_MOONJUMP_BTN", "Moonjump button",
    "SNES auto-joypad mask; default $8000 is B.", kSettingType_Mask,
    kApply_Passive, kSettingCat_Cheats, &g_settings.cheat_moonjump_button,
    0x8000, 0, 0xffff, 1, false, NULL },
  INT_SETTING(cheat_no_knockback, "AR_NO_KNOCKBACK", "No knockback",
              "1 is full invulnerability; other values are raw hex object offsets.",
              kSettingCat_Cheats, 0, 0, 0x3f, ParseNoKnockback),
  { "pins", "AR_PIN", "Custom PAR pins",
    "Comma-separated 7Exxxxvv/7Fxxxxvv codes, enforced every frame.",
    kSettingType_Custom, kApply_Passive, kSettingCat_Cheats,
    &g_settings.pin_count, 0, 0, 32, 1, false, ParsePins },

  BOOL_SETTING(ws_action, "AR_WS_ACTION", "Wide action stages",
               "Enable wide geometry in action regions.",
               kSettingCat_Widescreen, 1, false),
  BOOL_SETTING(ws_sim, "AR_WS_SIM", "Wide simulation towns",
               "Enable wide geometry in simulation towns.",
               kSettingCat_Widescreen, 1, false),
  BOOL_SETTING(ws_bgrefresh, "AR_WS_BGREFRESH", "BG margin refresh",
               "Decode true action tilemap content into side margins.",
               kSettingCat_Widescreen, 1, false),
  BOOL_SETTING(ws_skypalace_bg, "AR_WS_SKYPALACE_BG", "Sky Palace BG repair",
               "Reconstruct box-free colonnade tiles in BG2 margins.",
               kSettingCat_Widescreen, 1, false),
  BOOL_SETTING(ws_sprites, "AR_WS_SPRITES", "Wide action sprites",
               "Emit action sprite components into side margins.",
               kSettingCat_Widescreen, 1, false),
  BOOL_SETTING(ws_margin_objects, "AR_WS_MARGIN_OBJECTS", "Draw margin objects",
               "Draw initialized action objects in the wide view.",
               kSettingCat_Widescreen, 1, false),
  BOOL_SETTING(ws_margin_activation, "AR_WS_MARGIN_ACTIVATION", "Activate margin objects",
               "Extend action object activation to the live wide window.",
               kSettingCat_Widescreen, 1, false),
  BOOL_SETTING(ws_bg2_padding, "AR_WS_BG2_MIRROR", "Decorative BG2 padding",
               "Use the mapped mirror/repeat strategies for 256px BG2 layers.",
               kSettingCat_Widescreen, 1, false),
  BOOL_SETTING(ws_sim_sprites, "AR_WS_SIM_SPRITES", "Wide simulation sprites",
               "Widen town world sprites and angel projectile lifetime.",
               kSettingCat_Widescreen, 1, false),
};

const int g_setting_desc_count =
    (int)(sizeof(g_setting_descs) / sizeof(g_setting_descs[0]));

static void SeedSetting(const SettingDesc *desc) {
  switch (desc->type) {
    case kSettingType_Bool:
      *(bool *)desc->field = desc->defval != 0;
      break;
    case kSettingType_Int:
      *(int *)desc->field = (int)desc->defval;
      break;
    case kSettingType_Mask:
      *(uint16 *)desc->field = (uint16)desc->defval;
      break;
    case kSettingType_Custom:
      break;
  }

  const char *text = desc->env ? getenv(desc->env) : NULL;
  if (!text) return;
  if (desc->parse) {
    desc->parse(text, desc->field);
  } else if (desc->type == kSettingType_Bool) {
    /* Preserve both historical polarities: default-on repairs are disabled
     * only by a leading zero; default-off cheats require a nonempty nonzero. */
    *(bool *)desc->field = desc->defval
                              ? text[0] != '0'
                              : text[0] && text[0] != '0';
  } else if (desc->type == kSettingType_Int && text[0]) {
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
    SeedSetting(&g_setting_descs[i]);

  /* Name the state the env actually produced. This matters when one AR_WS_*
   * switch is disabled: calling that combination FULL would make the source
   * of truth lie to the future settings UI. */
  g_settings.display_mode = InferDisplayMode();

  /* AR_DISPLAY_MODE=0|1|2 picks the starting preset (0=4:3, 1=wide raw,
   * 2=wide full). Applying a preset overwrites the individual ws_* flags
   * above, so it is the last word — this is the headless/scripted equivalent
   * of pressing F9, and how the mode comparison is captured without a UI. */
  const char *dm = getenv("AR_DISPLAY_MODE");
  if (dm && dm[0]) {
    int m = atoi(dm);
    if (m >= 0 && m < kDisplayMode_PresetCount) {
      Settings_SetDisplayMode(m);
      fprintf(stderr, "[display] AR_DISPLAY_MODE=%d -> %s\n", m,
              Settings_DisplayModeName(m));
    } else {
      fprintf(stderr, "[display] AR_DISPLAY_MODE=%s out of range (0..%d)\n",
              dm, kDisplayMode_PresetCount - 1);
    }
  }
}

void Settings_SetDisplayMode(int mode) {
  if (mode < 0 || mode >= kDisplayMode_PresetCount) return;
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

void Settings_MarkDisplayModeCustom(void) {
  if (g_ws_active)
    g_settings.display_mode = kDisplayMode_Custom;
}

int Settings_CycleDisplayMode(void) {
  /* A bespoke env/menu combination enters the comparison cycle at the
   * authentic baseline; the cycle itself always remains exactly three steps. */
  int next = (g_settings.display_mode >= 0 &&
              g_settings.display_mode < kDisplayMode_PresetCount)
                 ? (g_settings.display_mode + 1) % kDisplayMode_PresetCount
                 : kDisplayMode_43;
  Settings_SetDisplayMode(next);
  return next;
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
