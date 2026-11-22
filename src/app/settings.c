#include "snesrecomp/support/utf8_fs.h"
#include "app/runtime_log.h"

#include "app/settings.h"
#include "localization/ui_catalog.h"
#include "text_parse_utils.h"
#include "constants.h"
#include "randomizer/randomizer.h"
#include "actraiser/actraiser_death_heim_rush.h"
#include "render/render_capabilities.h"
#include "actraiser_game.h"   /* kActRaiserAuthenticWidth */
#include "present/display_geometry.h"
#include "app/input_map.h"
#include "actraiser/actraiser_angel_input.h"
#include "host/atomic_replace.h"
#include "sim/sim3d/sim3d_camera_limits.h"
#include "sim/town/sim_town_terrain.h"
#include "host/host_video.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SettingsDeathHeimHost s_death_heim_host;
void Settings_SetDeathHeimHost(const SettingsDeathHeimHost *host) {
  s_death_heim_host = host ? *host : (SettingsDeathHeimHost){0};
}
static bool DeathHeimCanChange(void) {
  return !s_death_heim_host.can_change ||
      s_death_heim_host.can_change(s_death_heim_host.context);
}
static bool DeathHeimRosterAvailable(long roster) {
  return !s_death_heim_host.roster_available ||
      s_death_heim_host.roster_available(s_death_heim_host.context, (unsigned)roster);
}
static bool DeathHeimPrepareChange(const SettingDesc *desc, long value) {
  if (!s_death_heim_host.prepare) return true; /* Boot preferences precede game ownership. */
  const unsigned roster = desc->field == &g_settings.death_heim_roster
      ? (unsigned)value : (unsigned)g_settings.death_heim_roster;
  const bool shuffle = desc->field == &g_settings.death_heim_shuffle_order
      ? value != 0 : g_settings.death_heim_shuffle_order;
  const uint32_t seed = desc->field == &g_settings.death_heim_seed
      ? (uint32_t)value : (uint32_t)g_settings.death_heim_seed;
  return s_death_heim_host.prepare(s_death_heim_host.context, roster, shuffle, seed);
}
static const char *const kDeathHeimRosterLabels[] = {
  "Standard", "Act boss swap", "Extended boss rush"
};
static bool ParseDeathHeimRoster(const char *text, void *field) {
  static const char *const keys[] = {"standard", "act1", "extended"};
  for (unsigned i = 0; i < 3; ++i)
    if (!strcmp(text, keys[i]) || !strcmp(text, kDeathHeimRosterLabels[i]) ||
        (text[0] == (char)('0' + i) && !text[1])) {
      *(int *)field = (int)i;
      return true;
    }
  return false;
}
static int SerializeDeathHeimRoster(char *out, int capacity, const void *field) {
  static const char *const keys[] = {"standard", "act1", "extended"};
  const unsigned roster = (unsigned)*(const int *)field;
  return snprintf(out, capacity, "%s", roster < 3 ? keys[roster] : "standard");
}
#ifndef AR_SIM3D_TERRAIN_ELEVATION
#define AR_SIM3D_TERRAIN_ELEVATION 0
#endif
#ifdef _WIN32
#include <windows.h>
#include <io.h>       /* _get_osfhandle/_fileno: Settings_Save durability */
#else
#include <fcntl.h>    /* open: directory fsync after rename */
#include <unistd.h>   /* fsync/fileno/close: Settings_Save durability */
#endif

Settings g_settings;
static SettingsChangeObserver s_change_observer;
static SettingsActionObserver s_action_observer;

enum {
  kSettingsLayerValueSize = 512,
  kSettingsIniLineCapacity = 1024,
};
typedef struct SettingsLayerValue {
  bool present;
  bool legacy_env_syntax;
  char text[kSettingsLayerValueSize];
} SettingsLayerValue;

static SettingsLayerValue s_config_layer[kSettingsMaxDescriptors];
static int s_boot_display_rank;
static int s_boot_widescreen_rank;
static int s_boot_display_mode;
static bool s_boot_display_from_environment;
static int s_saved_window_mode = kWindowMode_Count;

/* The framebuffer width is derived from the canonical ActRaiser display
 * geometry, so this module does not need host_display's g_snes_width. */

static int InferDisplayMode(void);

static const char *const kLocalizationContentLabels[] = {
  "Native US", "Configured pack",
};
static const char *const kInterfaceLanguageLabels[] = {"en", "fr", "de", "ja"};
_Static_assert(sizeof(kInterfaceLanguageLabels) / sizeof(*kInterfaceLanguageLabels) ==
                   kArUiLocale_Count, "interface locale settings/catalog agreement");
static SettingsLocalizationPack s_localization_packs[kSettingsLocalizationMaximumPacks];
static size_t s_localization_pack_count;

static int LocalizationNameCompare(const char *a, const char *b) {
  for (;;) {
    unsigned char left = (unsigned char)*a++, right = (unsigned char)*b++;
    if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
    if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
    if (left != right || !left) return (int)left - (int)right;
  }
}

static int CompareLocalizationPacks(const void *left, const void *right) {
  const SettingsLocalizationPack *a = left, *b = right;
  int cmp = LocalizationNameCompare(a->name, b->name);
  return cmp ? cmp : strcmp(a->id, b->id);
}

bool Settings_SetLocalizationPacks(const SettingsLocalizationPack *packs,
                                    size_t count) {
  if ((!packs && count) || count > kSettingsLocalizationMaximumPacks) return false;
  for (size_t i = 0; i < count; ++i) {
    const SettingsLocalizationPack *p = &packs[i];
    if (!p->id[0] || !p->name[0] || !p->locale[0] || !p->manifest[0] ||
        !memchr(p->id, 0, sizeof(p->id)) ||
        !memchr(p->name, 0, sizeof(p->name)) ||
        !memchr(p->locale, 0, sizeof(p->locale)) ||
        !memchr(p->manifest, 0, sizeof(p->manifest))) return false;
    for (size_t j = 0; j < i; ++j)
      if (!LocalizationNameCompare(p->id, packs[j].id)) return false;
  }
  if (count) memcpy(s_localization_packs, packs, count * sizeof(*packs));
  s_localization_pack_count = count;
  qsort(s_localization_packs, count, sizeof(*packs), CompareLocalizationPacks);
  return true;
}

const char *Settings_LocalizationPackPath(int content) {
  if (content == 1) return getenv("AR_LOCALIZATION_PACK");
  return content >= 2 && (size_t)(content - 2) < s_localization_pack_count
      ? s_localization_packs[content - 2].manifest : NULL;
}

static long LocalizationContentMaximum(void) { return (long)s_localization_pack_count + 1; }
static bool ParseLocalizationContent(const char *text, void *field) {
  for (int i = 0; i < 2; ++i) {
    if (!strcmp(text, kLocalizationContentLabels[i]) ||
        (text[0] == '0' + i && !text[1])) { *(int *)field = i; return true; }
  }
  for (size_t i = 0; i < s_localization_pack_count; ++i)
    if (!strcmp(text, s_localization_packs[i].id)) { *(int *)field = (int)i + 2; return true; }
  return false;
}
static int SerializeLocalizationContent(char *buffer, int size, const void *field) {
  int value = *(const int *)field;
  return snprintf(buffer, size, "%s", value >= 2 && (size_t)(value - 2) < s_localization_pack_count
      ? s_localization_packs[value - 2].id : kLocalizationContentLabels[value == 1]);
}
static int FormatLocalizationContent(char *buffer, int size, const void *field) {
  int value = *(const int *)field;
  if (value < 2 || (size_t)(value - 2) >= s_localization_pack_count)
    return SerializeLocalizationContent(buffer, size, field);
  const SettingsLocalizationPack *p = &s_localization_packs[value - 2];
  return snprintf(buffer, size, "%s (%s) [%s]", p->name, p->locale, p->id);
}
static const char *const kLocalizationPresentationLabels[] = {
  "Native", "Enhanced",
};
static const char *const kLocalizationButtonPromptLabels[] = {"Text", "Button glyphs"};
static const char *const kLocalizationSamplingLabels[] = {"Crisp", "Smooth"};
static const char *const kLocalizationPixelationLabels[] = {
  "None", "Low resolution", "Mosaic",
};

static bool EnhancedTextSelected(void) {
  return g_settings.localization_presentation == 1;
}

static bool TextPresentationSelectable(void) {
  return g_settings.localization_content == 0;
}

static int SerializeLocalizationPresentation(char *buffer, int size,
                                             const void *field) {
  return snprintf(buffer, size, "%s",
                  kLocalizationPresentationLabels[*(const int *)field != 0]);
}

static int FormatLocalizationPresentation(char *buffer, int size,
                                          const void *field) {
  return snprintf(buffer, size, "%s", TextPresentationSelectable()
      ? kLocalizationPresentationLabels[*(const int *)field != 0]
      : "Enhanced (required by pack)");
}

static bool TextPixelationSelected(void) {
  return EnhancedTextSelected() && g_settings.localization_font_pixelation != 0;
}

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

static bool ParseMoonjumpLegacy(const char *text, void *field) {
  bool *enabled = (bool *)field;
  if (!text || !text[0] || text[0] == '0') {
    *enabled = false;
    return true;
  }
  *enabled = true;
  if (text[0] == '1' && !text[1]) return true;

  /* AR_MOONJUMP historically accepted the flight speed directly. Keep that
   * developer-config shorthand while the menu exposes separate controls. */
  unsigned long speed = strtoul(text, NULL, 0);
  if (speed < 1) speed = 1;
  if (speed > 255) speed = 255;
  g_settings.cheat_moonjump_speed = (int)speed;
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
      RuntimeLog_Printf("AR_PIN: bad token '%s' "
              "(want 8-hex PAR 7Exxxxvv/7Fxxxxvv)\n", token);
    }
  }
  if (*count) RuntimeLog_Printf("AR_PIN: %u pin(s) active\n", (unsigned)*count);
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

static void WidescreenSettingChanged(const SettingDesc *desc) {
  (void)desc;
  g_settings.display_mode = InferDisplayMode();
}

static void DisplayModeChanged(const SettingDesc *desc) {
  Settings_SetDisplayMode(*(const int *)desc->field);
}

/* Defined by the host: re-resolves video geometry for the widened
 * diorama render margin and rebinds the PPU surfaces. Weakly relevant to the
 * settings tests, which link their own no-op. */
void Diorama_OnModeChanged(void);

static void DioramaModeChanged(const SettingDesc *desc) {
  (void)desc;
  Diorama_OnModeChanged();
}

static int FormatHudScale(char *buffer, int buffer_size, const void *field) {
  int value = *(const int *)field;
  if (!value) return snprintf(buffer, (size_t)buffer_size, "Match game");
  return snprintf(buffer, (size_t)buffer_size, "%d.%02dx",
                  value / kPercentScale, value % kPercentScale);
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
    *out = (int)(value * (double)kPercentScale + 0.5);
    return true;
  }
  if (*end) return false;
  *out = (int)value;
  return true;
}

static int FormatMenuScale(char *buffer, int buffer_size, const void *field) {
  int value = *(const int *)field;
  if (!value) return snprintf(buffer, (size_t)buffer_size, "Auto");
  return snprintf(buffer, (size_t)buffer_size, "%d.%02dx",
                  value / kPercentScale, value % kPercentScale);
}

static bool ParseMenuScale(const char *text, void *field) {
  if (!strcmp(text, "Auto") || !strcmp(text, "auto")) {
    *(int *)field = 0;
    return true;
  }
  return ParseHudScale(text, field);
}

static bool ParseAudioVolume(const char *text, void *field) {
  if (!text || !text[0]) return false;
  char *end = NULL;
  long value = strtol(text, &end, 0);
  if (!end || (*end && !(*end == '%' && end[1] == 0))) return false;
  if (value < 0) value = 0;
  if (value > kPercentScale) value = kPercentScale;
  *(int *)field = (int)(value / 5 * 5);
  return true;
}

static int FormatAudioVolume(char *buffer, int buffer_size,
                             const void *field) {
  return snprintf(buffer, (size_t)buffer_size, "%d%%", *(const int *)field);
}

static bool ParseAudioFrequency(const char *text, void *field) {
  int *value = (int *)field;
  if (!text) return false;
  if (!strcmp(text, "32040") || !strcmp(text, "32.04 kHz") ||
      !strcmp(text, "32.04khz") || !strcmp(text, "32.04")) {
    *value = kAudioFrequency_32040;
    return true;
  }
  if (!strcmp(text, "44100") || !strcmp(text, "44.1 kHz") ||
      !strcmp(text, "44.1khz") || !strcmp(text, "44.1")) {
    *value = kAudioFrequency_44100;
    return true;
  }
  if (!strcmp(text, "48000") || !strcmp(text, "48 kHz") ||
      !strcmp(text, "48khz") || !strcmp(text, "48")) {
    *value = kAudioFrequency_48000;
    return true;
  }
  if (!strcmp(text, "auto") || !strcmp(text, "Auto") ||
      !strcmp(text, "Auto (device)") || !strcmp(text, "device") ||
      !strcmp(text, "0 Hz") || !strcmp(text, "3")) {
    *value = kAudioFrequency_Auto;
    return true;
  }
  /* Enum indices remain accepted for generated/headless settings input. */
  if (!strcmp(text, "0")) {
    *value = kAudioFrequency_32040;
    return true;
  }
  if (!strcmp(text, "1")) {
    *value = kAudioFrequency_44100;
    return true;
  }
  if (!strcmp(text, "2")) {
    *value = kAudioFrequency_48000;
    return true;
  }
  return false;
}

static bool ParseExtendedAspect(const char *text, void *field) {
  int *value = (int *)field;
  if (!text || !text[0] || !strcmp(text, "off") || !strcmp(text, "Off") ||
      !strcmp(text, "0") || !strcmp(text, "0:0") ||
      !strcmp(text, "4:3")) {
    *value = kScreenAspect_43;
    return true;
  }
  if (!strcmp(text, "16:9") || !strcmp(text, "1")) {
    *value = kScreenAspect_169;
    return true;
  }
  if (!strcmp(text, "16:10") || !strcmp(text, "2")) {
    *value = kScreenAspect_1610;
    return true;
  }
  if (!strcmp(text, "stretch") || !strcmp(text, "Stretch") ||
      !strcmp(text, "3")) {
    *value = kScreenAspect_Stretch;
    return true;
  }
  if (!strcmp(text, "auto") || !strcmp(text, "Auto") || !strcmp(text, "4")) {
    *value = kScreenAspect_Auto;
    return true;
  }
  return false;
}

static bool ParseWarpTarget(const char *text, void *field) {
  uint16 *value = (uint16 *)field;
  if (!text || !text[0]) {
    *value = 0x0101;
    return true;
  }
  if (text[0] == '$') text++;
  else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
  if (!text[0] || strlen(text) > 4) return false;
  char *end = NULL;
  unsigned long parsed = strtoul(text, &end, 16);
  if (!end || *end || parsed > 0xffff) return false;
  *value = (uint16)parsed;
  return true;
}

static int FormatWarpTarget(char *buffer, int buffer_size,
                            const void *field) {
  return snprintf(buffer, (size_t)buffer_size, "%04X",
                  (unsigned)*(const uint16 *)field);
}

static bool ParseSaveBackend(const char *text, void *field) {
  if (!text || !field) return false;
  if (!strcmp(text, "native-srm") || !strcmp(text, "native") ||
      !strcmp(text, "0")) {
    *(int *)field = 0;
    return true;
  }
  if (!strcmp(text, "ini") || !strcmp(text, "1")) {
    *(int *)field = 1;
    return true;
  }
  return false;
}

static bool ParseSaveProgressEdit(const char *text, void *field) {
  if (!text || !field) return false;
  static const char *const names[kSaveProgressEdit_Count] = {
    "leave-as-is", "act1", "act1-cleared", "act2", "act2-cleared"
  };
  static const char *const labels[kSaveProgressEdit_Count] = {
    "Leave as-is", "Act 1", "Act 1 cleared", "Act 2", "Act 2 cleared"
  };
  for (int i = 0; i < kSaveProgressEdit_Count; i++) {
    if (!strcmp(text, names[i]) || !strcmp(text, labels[i])) {
      *(int *)field = i;
      return true;
    }
  }
  char *end = NULL;
  long value = strtol(text, &end, 0);
  if (!end || *end || value < 0 || value >= kSaveProgressEdit_Count)
    return false;
  *(int *)field = (int)value;
  return true;
}

static bool ParseStagedDirect(const char *text, void *field) {
  if (!text || !field) return false;
  if (!strcmp(text, "Leave as-is") || !strcmp(text, "leave-as-is")) {
    *(int *)field = 0;
    return true;
  }
  char *end = NULL;
  long value = strtol(text, &end, 0);
  if (!end || *end) return false;
  *(int *)field = (int)value;
  return true;
}

static bool ParseStagedZeroBased(const char *text, void *field) {
  if (!text || !field) return false;
  if (!strcmp(text, "Leave as-is") || !strcmp(text, "leave-as-is")) {
    *(int *)field = 0;
    return true;
  }
  char *end = NULL;
  long value = strtol(text, &end, 0);
  if (!end || *end || value < 0 || value >= INT_MAX) return false;
  *(int *)field = (int)value + 1;
  return true;
}

static int FormatStagedDirect(char *buffer, int buffer_size,
                              const void *field) {
  int value = *(const int *)field;
  return value == 0
      ? snprintf(buffer, (size_t)buffer_size, "Leave as-is")
      : snprintf(buffer, (size_t)buffer_size, "%d", value);
}

static int FormatStagedZeroBased(char *buffer, int buffer_size,
                                 const void *field) {
  int value = *(const int *)field;
  return value == 0
      ? snprintf(buffer, (size_t)buffer_size, "Leave as-is")
      : snprintf(buffer, (size_t)buffer_size, "%d", value - 1);
}

static bool ParseStagedScore(const char *text, void *field) {
  if (!text || !field) return false;
  if (!strcmp(text, "Leave as-is") || !strcmp(text, "leave-as-is")) {
    *(int *)field = 0;
    return true;
  }
  char *end = NULL;
  long score = strtol(text, &end, 0);
  if (!end || *end || score < 0 || score > 99990 || score % 10) return false;
  *(int *)field = (int)(score / 10) + 1;
  return true;
}

static int FormatStagedScore(char *buffer, int buffer_size,
                             const void *field) {
  int value = *(const int *)field;
  return value == 0
      ? snprintf(buffer, (size_t)buffer_size, "Leave as-is")
      : snprintf(buffer, (size_t)buffer_size, "%d", (value - 1) * 10);
}

static bool ParseSavePlayerName(const char *text, void *field) {
  if (!text || !field) return false;
  char *name = (char *)field;
  if (!text[0] || !strcmp(text, "Leave as-is") ||
      !strcmp(text, "leave-as-is")) {
    name[0] = 0;
    return true;
  }
  size_t length = strlen(text);
  if (length > 8) return false;
  for (size_t i = 0; i < length; i++) {
    unsigned char ch = (unsigned char)text[i];
    if (ch < 0x20 || ch > 0x7e) return false;
  }
  memcpy(name, text, length + 1);
  return true;
}

static int FormatSavePlayerName(char *buffer, int buffer_size,
                                const void *field) {
  const char *name = (const char *)field;
  return snprintf(buffer, (size_t)buffer_size, "%s",
                  name[0] ? name : "Leave as-is");
}

static bool ParseTurboMultiplier(const char *text, void *field) {
  if (!text || !text[0]) return false;
  char *end = NULL;
  long value = strtol(text, &end, 0);
  if (!end || *end) return false;
  if (value < 2) value = 2;
  if (value > 64) value = 64;
  *(int *)field = (int)value;
  return true;
}

static bool ParsePixelAspect(const char *text, void *field) {
  int *value = (int *)field;
  if (!text) return false;
  if (!strcmp(text, "square") || !strcmp(text, "Square pixels") ||
      !strcmp(text, "0")) {
    *value = kPixelAspect_Square;
    return true;
  }
  if (!strcmp(text, "4:3") || !strcmp(text, "crt") ||
      !strcmp(text, "4:3 CRT") || !strcmp(text, "1")) {
    *value = kPixelAspect_Crt43;
    return true;
  }
  return false;
}

static const char *const kDisplayModeLabels[] = {
  "4:3 authentic",
  "Widescreen raw",
  "Widescreen full",
};

static const char *const kPixelAspectLabels[] = {
  "Square pixels",
  "4:3 CRT",
};

static const char *const kDioramaCamModeLabels[] = {
  "Free Cam",
  "Dynamic Cam",
};

/* Persist stable mode names; the UI catalog marks Voxel 3D as WIP. */
static const char *const kAction3DModeLabels[] = {"Off", "Diorama", "Voxel 3D"};

static bool ParseAction3DMode(const char *text, void *field) {
  if (!text || !text[0]) return false;
  for (int i = 0; i < kAction3D_Count; ++i) {
    if (!strcmp(text, kAction3DModeLabels[i]) ||
        (text[0] == '0' + i && !text[1])) {
      *(int *)field = i;
      return true;
    }
  }
  /* Saved boolean labels remain loadable; the next save uses mode names. */
  const char *const off[] = {"off", "false", "False", "no", "No"};
  const char *const on[] = {"On", "on", "true", "True", "yes", "Yes"};
  for (unsigned i = 0; i < sizeof(off) / sizeof(*off); ++i)
    if (!strcmp(text, off[i])) { *(int *)field = kAction3D_Off; return true; }
  for (unsigned i = 0; i < sizeof(on) / sizeof(*on); ++i)
    if (!strcmp(text, on[i])) { *(int *)field = kAction3D_Diorama; return true; }
  return false;
}

static const char *const kSimCamModeLabels[] = {
  "Free Cam",
  "Dynamic Cam",
};

static const char *const kSimVoxelDetailLabels[] = {
  "Low",
  "Balanced",
  "High",
  "Ultra",
};

static const char *const kSimVoxelPresetLabels[] = {
  "Off",
  "Performance",
  "Balanced",
  "Quality",
  "Custom",
};

static const char *const kSimVoxelLodLabels[] = {
  "Fixed",
  "Adaptive",
};

static const char *const kSimVoxelShadingLabels[] = {
  "Basic",
  "Ambient occlusion",
  "Materials + AO",
};

static const char *const kSimVoxelStyleLabels[] = {
  "Basic",
  "Silhouette trim",
  "Architectural",
  "Varied",
};

static const char *const kSimVoxelFacingLabels[] = {
  "Shared lean",
  "Per-model lean",
};

static const char *const kSimVoxelRenderScaleLabels[] = {
  "Native",
  "Smooth 2x",
  "Pixel-clean",
};

static const char *const kDioramaSkyModeLabels[] = {
  "Off",
  "Skybox only",
  "Plane + skybox",
};

static const char *const kScreenAspectLabels[] = {
  "4:3",
  "16:9",
  "16:10",
  "Stretch",
  "Auto",
};

static const char *const kWindowModeLabels[] = {
  "Windowed",
  "Borderless",
  "Fullscreen",
};

static const char *const kPictureInPictureCornerLabels[] = {
  "Bottom right", "Bottom left", "Top right", "Top left",
};

static const char *const kRefreshModeLabels[] = {
  "Vsync",
  "Uncapped",
  "Limit",
  "Unlimited",
};
static const char *const kPerformanceOverlayLabels[] = {"Off", "Summary", "Detailed"};

/* Also the settings.ini values: keep these spellings. */
static const char *const kGpuBackendLabels[kGpuBackend_Count] = {
  "Automatic", "Direct3D 12", "Vulkan", "Metal",
};

static const char *const kInterpolationSourceLabels[] = {
  "Native 60 Hz",
  "Test 30 -> 60 Hz",
};

/* NOTE: these strings are ALSO the settings.ini values (Settings_FormatValue
 * writes the label, ParseAudioFrequency reads it), so they are a persisted
 * format — do not reword them without teaching the parser the old spelling.
 * "32.04 kHz" is the SNES S-DSP's native rate, offered for completeness, but
 * SDL clamps the device rate to at least 44100: selecting it does not open a
 * 32kHz device. host_audio.c clamps the request and logs why; the row's description
 * says so too. */
static const char *const kAudioFrequencyLabels[] = {
  "32.04 kHz",
  "44.1 kHz",
  "48 kHz",
  "Auto (device)",
};

static const char *const kSaveBackendLabels[] = {
  "native-srm",
  "ini",
};

static const char *const kSaveProgressEditLabels[] = {
  "Leave as-is",
  "Act 1",
  "Act 1 cleared",
  "Act 2",
  "Act 2 cleared",
};

static const char *const kSaveEditorPageLabels[] = {
  "Actions", "Progress", "Status", "Magic", "Items", "Scores",
};

static const char *const kSaveProfessionalLabels[] = {
  "Leave as-is", "Locked", "Unlocked",
};

static const char *const kSaveDeathHeimLabels[] = {
  "Leave as-is", "Locked", "Unlocked", "Cleared",
};

static const char *const kSaveEquippedMagicLabels[] = {
  "Leave as-is", "None", "Magical Fire", "Magical Stardust",
  "Magical Aura", "Magical Light",
};

static const char *const kSaveMagicSlotLabels[] = {
  "Leave as-is", "Empty", "Magical Fire", "Magical Stardust",
  "Magical Aura", "Magical Light",
};

static const char *const kSaveItemLabels[] = {
  "Leave as-is", "Empty", "Source of Life", "Source of Magic",
  "Loaf of Bread", "Wheat", "Herb", "Bridge", "Harmonious Music",
  "Ancient Tablet", "Magic Skull", "Sheep's Fleece", "Bomb",
  "Compass", "Strength of Angel",
};

/* Diorama availability gates (§10.6). The mode row is offered only when the
 * new PPU path can run; the camera/layer rows only once the mode is on. The
 * inert-in-diorama Display rows use the negation so contradictory combos are
 * greyed out rather than silently ignored (§D15). */
bool Diorama_ModeIsOn(void) { return g_settings.diorama_mode; }
bool Sim3D_ModeIsOn(void) { return g_settings.sim3d_mode; }
bool Sim3D_ChurchIsOn(void) { return g_settings.sim3d_mode && g_settings.sim3d_church; }
static bool SimMenuModernSelected(void) { return g_settings.sim_menu_style == 1; }
/* Screen ratio owns stretching. Keep the old bool as a load-only alias so
 * existing config.ini/settings.ini files still migrate cleanly, but never let
 * runtime code acquire a second source of truth. A legacy true enables the
 * modern Stretch enum; false is intentionally a no-op because old generated
 * files commonly contain both `extended_aspect = 16:9` and
 * `ignore_aspect_ratio = Off`. */
static void OnScreenRatioChanged(const SettingDesc *desc) {
  (void)desc;
  g_settings.ignore_aspect_ratio =
      g_settings.extended_aspect == kScreenAspect_Stretch;
}

static void OnLegacyStretchChanged(const SettingDesc *desc) {
  if (*(const bool *)desc->field)
    g_settings.extended_aspect = kScreenAspect_Stretch;
  g_settings.ignore_aspect_ratio = Settings_IgnoreAspectRatio();
}

/* Refresh rate superseded the old uncapped bool. Preserve its historical
 * presentation policy by migrating On to Uncapped; Off leaves an explicitly
 * selected modern refresh mode alone. */
static void OnLegacyUncappedChanged(const SettingDesc *desc) {
  if (*(const bool *)desc->field)
    g_settings.refresh_mode = kRefreshMode_Uncapped;
}

static bool WidescreenActive(void) { return g_ws_active; }
static bool WindowScaleAvailable(void) {
  return g_settings.window_mode == kWindowMode_Windowed;
}

static bool PictureInPictureWindowAvailable(void) {
#ifdef __ANDROID__
  return false;
#else
  return true;
#endif
}

static bool PictureInPictureFullscreenAvailable(void) {
  return PictureInPictureWindowAvailable() && g_settings.picture_in_picture_window;
}

static bool s_ar2_master_available;
static bool Ar2MasterAvailable(void) { return s_ar2_master_available; }
void Settings_SetAr2MasterAvailable(bool available) { s_ar2_master_available = available; }
static bool s_imported_level_active;
static bool ImportedLevelActive(void) { return s_imported_level_active; }
void Settings_SetImportedLevelActive(bool active) { s_imported_level_active = active; }

static bool s_hd_replacements_available;
static bool HdReplacementsAvailable(void) {
  return s_hd_replacements_available;
}

static bool DioramaFreeCameraAvailable(void) {
  return Diorama_ModeIsOn() &&
      g_settings.diorama_camera_mode == kDioramaCam_Free;
}
static bool DioramaDynamicCameraAvailable(void) {
  return Diorama_ModeIsOn() &&
      g_settings.diorama_camera_mode == kDioramaCam_Dynamic;
}

/* The frame-limit FPS row only matters when Refresh rate is Limit. */
static bool FrameLimitActive(void) {
  return g_settings.refresh_mode == kRefreshMode_Limit;
}

bool Diorama_InterpolationIsOn(void) {
  return Diorama_ModeIsOn() && g_settings.gpu_interp_enabled;
}
static bool FrameInterpolationActive(void) { return Diorama_InterpolationIsOn(); }

/* Sim-town 3D defaults (2026-07-22).
 *
 * The numeric sim3d defaults below, and the constants they name in
 * sim_render_metadata.h, are a **captured baseline**: a snapshot of a tuned
 * live session, taken deliberately so that a settings reset and a fresh save
 * both land on a configuration that has actually been looked at. They are not
 * derived, and several of them differ from the value the surrounding comment
 * argues for on first principles -- where that happens the comment says so and
 * why the looked-at value won.
 *
 * Consequence worth knowing before changing one: `settings.ini` persists these
 * keys and beats the compiled default, so editing a number here changes
 * nothing for anyone who already has an ini. It changes what a reset restores
 * and what a new install starts from, which is exactly what it is for.
 *
 * Every numeric default must sit on its row's `step` grid or the settings
 * round-trip test fails. */

/* The enhanced SIM renderer is configured as individual stage toggles. The
 * resolver and the frame payload still work in one mask, so this is the single
 * place the toggles are folded into one -- the toggles are the only stored
 * state, and no mask is persisted anywhere. */
static const struct {
  SimRenderFeatureMask bit;
  bool *field;
} kSim3DStageToggles[] = {
  { kSimFeature_SeparatedComposite, &g_settings.sim3d_separated_composite },
  { kSimFeature_GroundProjection, &g_settings.sim3d_ground_projection },
  { kSimFeature_ObjectBillboards, &g_settings.sim3d_object_billboards },
  { kSimFeature_VirtualHeight, &g_settings.sim3d_virtual_height },
  { kSimFeature_Shadows, &g_settings.sim3d_shadows },
  { kSimFeature_SoftShadows, &g_settings.sim3d_soft_shadows },
  { kSimFeature_RimLight, &g_settings.sim3d_rim_light },
  { kSimFeature_EffectLighting, &g_settings.sim3d_effect_lighting },
  { kSimFeature_Particles, &g_settings.sim3d_particles },
  { kSimFeature_WorldUnderlay, &g_settings.sim3d_world_underlay },
  { kSimFeature_GlobeUnderlay, &g_settings.sim3d_globe_underlay },
  { kSimFeature_CloudShroud, &g_settings.sim3d_cloud_shroud },
  { kSimFeature_CullHaze, &g_settings.sim3d_cull_haze },
  { kSimFeature_Backdrop, &g_settings.sim3d_backdrop },
  { kSimFeature_PickerExitEase, &g_settings.sim3d_picker_exit_ease },
};
static const int kSim3DStageToggleCount =
    (int)(sizeof(kSim3DStageToggles) / sizeof(kSim3DStageToggles[0]));

SimRenderFeatureMask Settings_Sim3DRequestedFeatures(void) {
  SimRenderFeatureMask mask = 0;
  for (int i = 0; i < kSim3DStageToggleCount; i++)
    if (*kSim3DStageToggles[i].field) mask |= kSim3DStageToggles[i].bit;
  /* A complete authored town facade is required. Do not capture neighbours
   * for an underlay that the presenter cannot use. */
  if (g_settings.sim3d_voxel_preset == kSimBackgroundVoxelPreset_Off)
    mask &= ~kSimFeature_GlobeUnderlay;
  return mask;
}

/* The requested-feature contract also reserves future stages. The resolver
 * records and clears unimplemented bits for traces, while Settings_IsLoadOnly
 * keeps those prototypes out of the user menu/save file until they ship. */
static bool Sim3DStageImplemented(SimRenderFeatureMask bit) {
  return g_settings.sim3d_mode && (kSim3DShippedFeatures & bit) != 0;
}
static bool Sim3DSeparatedAvailable(void) {
  return Sim3DStageImplemented(kSimFeature_SeparatedComposite);
}
static bool Sim3DSeparatedEnabled(void) {
  return Sim3DSeparatedAvailable() && g_settings.sim3d_separated_composite;
}
static bool Sim3DGroundAvailable(void) {
  return Sim3DSeparatedEnabled() &&
      Sim3DStageImplemented(kSimFeature_GroundProjection);
}
static bool Sim3DGroundEnabled(void) {
  return Sim3DGroundAvailable() && g_settings.sim3d_ground_projection;
}
static bool Sim3DLandscapeHeightAvailable(void) {
#if AR_SIM3D_TERRAIN_ELEVATION
  return g_settings.sim3d_world_navigation ||
      (Sim3DGroundEnabled() &&
       g_settings.sim3d_voxel_preset != kSimBackgroundVoxelPreset_Off);
#else
  return false;
#endif
}
static bool Sim3DVoxelCustomEnabled(void) {
  return Sim3DGroundEnabled() &&
      g_settings.sim3d_voxel_preset == kSimBackgroundVoxelPreset_Custom;
}
static bool Sim3DOrWorldNavigationModelsAvailable(void) {
  return Sim3DGroundEnabled() ||
      (g_settings.sim3d_world_navigation && g_settings.sim3d_world_navigation_towns);
}
static bool Sim3DOrWorldNavigationModelCustomEnabled(void) {
  return Sim3DOrWorldNavigationModelsAvailable() &&
      g_settings.sim3d_voxel_preset == kSimBackgroundVoxelPreset_Custom;
}
static bool Sim3DBillboardsAvailable(void) {
  return Sim3DSeparatedEnabled() &&
      Sim3DStageImplemented(kSimFeature_ObjectBillboards);
}
static bool Sim3DBillboardsEnabled(void) {
  return Sim3DBillboardsAvailable() && g_settings.sim3d_object_billboards;
}
static bool Sim3DVirtualHeightAvailable(void) {
  return Sim3DBillboardsEnabled() &&
      Sim3DStageImplemented(kSimFeature_VirtualHeight);
}
static bool Sim3DVirtualHeightEnabled(void) {
  return Sim3DVirtualHeightAvailable() && g_settings.sim3d_virtual_height;
}
static bool Sim3DShadowsAvailable(void) {
  return Sim3DBillboardsEnabled() &&
      Sim3DStageImplemented(kSimFeature_Shadows);
}
static bool Sim3DShadowsEnabled(void) {
  return Sim3DShadowsAvailable() && g_settings.sim3d_shadows;
}
static bool Sim3DSoftShadowsAvailable(void) {
  return Sim3DShadowsEnabled() &&
      Sim3DStageImplemented(kSimFeature_SoftShadows);
}
static bool Sim3DSoftShadowsEnabled(void) {
  return Sim3DSoftShadowsAvailable() && g_settings.sim3d_soft_shadows;
}
/* W4-2: the rim mask needs a destination-alpha blend mode, and capability
 * flags alone cannot prove that a backend can create and submit it. The
 * presenter latches the first rejected resource or draw, so this reflects
 * real runtime capability rather than an assumption, exactly as
 * GpuShadersActive does for the shader effects. Grey
 * the row out rather than offering a toggle that cannot do anything: an option
 * that silently does nothing is the dishonesty findings R8 and R13 were about.
 *
 * The read-only accessor is stubbed by ROM-free tests. It defaults to true in
 * present_sim3d.c and atomically latches false when a backend rejects it. */
/* The standard effect path verifies additive blending and untextured geometry
 * independently. A backend failure in either half greys both rows because both
 * current stages use the same batched pass; future shader/target capabilities
 * must get their own flags instead of broadening either of these. */
static bool Sim3DRimLightAvailable(void) {
  return Sim3DBillboardsEnabled() &&
      Sim3DStageImplemented(kSimFeature_RimLight) &&
      Present_SimRimMaskSupported();
}
static bool Sim3DRimLightEnabled(void) {
  return Sim3DRimLightAvailable() && g_settings.sim3d_rim_light;
}
static bool Sim3DEffectLightingAvailable(void) {
  return Sim3DGroundEnabled() &&
      Sim3DStageImplemented(kSimFeature_EffectLighting) &&
      Present_EffectRendererSupported();
}
static bool Sim3DParticlesAvailable(void) {
  return Sim3DGroundEnabled() &&
      Sim3DStageImplemented(kSimFeature_Particles) &&
      Present_EffectRendererSupported();
}
static bool Sim3DWorldUnderlayAvailable(void) {
  return Sim3DGroundEnabled() &&
      Sim3DStageImplemented(kSimFeature_WorldUnderlay);
}
static bool Sim3DWorldUnderlayEnabled(void) {
  return Sim3DWorldUnderlayAvailable() && g_settings.sim3d_world_underlay;
}
static bool Sim3DGlobeUnderlayAvailable(void) {
  return Sim3DWorldUnderlayEnabled() &&
      g_settings.sim3d_voxel_preset != kSimBackgroundVoxelPreset_Off;
}
static bool WorldGlobeAvailable(void) {
  return g_settings.sim3d_world_navigation ||
      (Sim3DGlobeUnderlayAvailable() && g_settings.sim3d_globe_underlay);
}
static bool Sim3DCloudShroudAvailable(void) {
  return Sim3DWorldUnderlayEnabled() &&
      Sim3DStageImplemented(kSimFeature_CloudShroud);
}
static bool Sim3DCloudShroudEnabled(void) {
  return Sim3DCloudShroudAvailable() && g_settings.sim3d_cloud_shroud;
}
static bool WorldNavigation3DEnabled(void) {
  return g_settings.sim3d_world_navigation;
}
static bool CompletionSceneAvailable(void) {
  return g_settings.diorama_mode;
}
static bool CompletionEffectsAvailable(void) {
  return CompletionSceneAvailable() && g_settings.death_heim_completion_world;
}
static bool SkyPalaceCloudsAvailable(void) {
  return WorldNavigation3DEnabled() && g_settings.sim3d_sky_palace &&
      g_settings.sim3d_world_navigation_clouds;
}
static bool WorldNavigationLightingAvailable(void) {
  return WorldGlobeAvailable() &&
      g_settings.sim3d_world_navigation_lighting;
}
static bool WorldNavigationCloudsAvailable(void) {
  return WorldNavigation3DEnabled() &&
      g_settings.sim3d_world_navigation_clouds;
}
static bool Sim3DOrWorldNavigationLightingAvailable(void) {
  return Sim3DShadowsEnabled() || WorldNavigationLightingAvailable();
}
static bool Sim3DOrWorldNavigationShadowAvailable(void) {
  return Sim3DShadowsEnabled() ||
      (WorldNavigation3DEnabled() && WorldNavigationLightingAvailable() &&
       g_settings.sim3d_world_navigation_clouds &&
       g_settings.sim3d_world_navigation_cloud_shadows);
}
static bool Sim3DOrWorldNavigationSoftShadowAvailable(void) {
  return Sim3DSoftShadowsEnabled() ||
      (WorldNavigation3DEnabled() && WorldNavigationLightingAvailable() &&
       g_settings.sim3d_world_navigation_clouds &&
       g_settings.sim3d_world_navigation_cloud_shadows);
}
static bool Sim3DOrWorldNavigationCloudsAvailable(void) {
  return Sim3DCloudShroudEnabled() || WorldNavigationCloudsAvailable();
}
static bool Sim3DFreeCameraAvailable(void) {
  return Sim3DGroundEnabled() &&
      g_settings.sim3d_camera_mode == kSimCam_Free;
}
static bool Sim3DDynamicCameraAvailable(void) {
  return Sim3DGroundEnabled() &&
      g_settings.sim3d_camera_mode == kSimCam_Dynamic;
}
static bool Sim3DCullHazeAvailable(void) {
  return Sim3DWorldUnderlayEnabled() &&
      Sim3DStageImplemented(kSimFeature_CullHaze);
}
static bool Sim3DCullHazeEnabled(void) {
  return Sim3DCullHazeAvailable() && g_settings.sim3d_cull_haze;
}
static bool Sim3DOrWorldNavigationHazeAvailable(void) {
  return Sim3DCullHazeAvailable() || WorldNavigation3DEnabled();
}
static bool Sim3DOrWorldNavigationHazeEnabled(void) {
  return Sim3DCullHazeEnabled() ||
      (WorldNavigation3DEnabled() && g_settings.sim3d_cull_haze);
}
static bool Sim3DBackdropAvailable(void) {
  return (Sim3DGroundEnabled() &&
          Sim3DStageImplemented(kSimFeature_Backdrop)) ||
      WorldNavigation3DEnabled();
}
static bool Sim3DBackdropEnabled(void) {
  return Sim3DBackdropAvailable() && g_settings.sim3d_backdrop;
}
static bool Sim3DPickerEaseAvailable(void) {
  return Sim3DSeparatedEnabled() &&
      Sim3DStageImplemented(kSimFeature_PickerExitEase);
}

/* Graphics availability gate (kSettingCat_Graphics): the per-effect rows
 * only matter once the mandatory GPU renderer is running. */
static bool GpuShadersActive(void) { return g_gpu_shaders_active; }

/* Automatic only until platform boot publishes its backends, so hosts that
 * never do (tests, tools) keep the Graphics API row hidden. */
static uint32_t s_gpu_backends_offered = 1u << kGpuBackend_Automatic;

void Settings_SetGpuBackendsOffered(uint32_t backend_mask) {
  const uint32_t explicit_backends = backend_mask &
      ((1u << kGpuBackend_Count) - 1u) & ~(1u << kGpuBackend_Automatic);
  /* A single compiled backend is exactly what Automatic already selects
   * (Metal on macOS, Vulkan on Linux and Steam Deck); listing it would be a
   * choice with no alternative. */
  const bool several = (explicit_backends & (explicit_backends - 1u)) != 0;
  s_gpu_backends_offered =
      (1u << kGpuBackend_Automatic) | (several ? explicit_backends : 0);
}

static bool GpuBackendOffered(long value) {
  return value >= 0 && value < kGpuBackend_Count &&
      ((s_gpu_backends_offered >> value) & 1u) != 0;
}

static int s_gpu_backend_active = kGpuBackend_Automatic;

void Settings_SetGpuBackendActive(int backend) {
  s_gpu_backend_active = backend > kGpuBackend_Automatic &&
      backend < kGpuBackend_Count ? backend : kGpuBackend_Automatic;
}

int Settings_GpuBackendActive(void) { return s_gpu_backend_active; }
static bool ActionEffectRendererAvailable(void) {
  return Present_EffectRendererSupported();
}

static const char *const kInputDeviceLabels[kInputDevice_Count] = {
  "Auto", "Keyboard", "Gamepad",
};
static const char *const kInputClassLabels[kInputClass_Count] = {
  "Keyboard", "Gamepad",
};

/* Round-trip partner for FormatGamepadSlot: settings.ini stores the rendered
 * text, so the parser has to read its own output back (and a bare number, for
 * a hand-edited file). */
static bool ParseGamepadSlot(const char *text, void *field) {
  if (!text) return false;
  while (*text == ' ') text++;
  int slot = 0;
  if (!strncmp(text, "First connected", 15)) {
    slot = 0;
  } else if (sscanf(text, "Gamepad %d", &slot) == 1) {
    /* named-but-absent form */
  } else if (sscanf(text, "%d", &slot) != 1) {
    return false;
  }
  if (slot < 0 || slot > 8) return false;
  *(int *)field = slot;
  return true;
}

/* Slot 0 follows hotplug; 1..N name a specific pad. Showing the live product
 * name is the whole point of the row on a Deck, where "Gamepad 1" could be
 * the built-in controls or a paired external pad. */
static int FormatGamepadSlot(char *buffer, int buffer_size,
                             const void *field) {
  int slot = *(const int *)field;
  int connected = InputMap_GamepadCount();
  if (slot <= 0) {
    if (!connected) return snprintf(buffer, buffer_size, "First connected");
    return snprintf(buffer, buffer_size, "First connected (%s)",
                    InputMap_GamepadName(0));
  }
  if (slot > connected)
    return snprintf(buffer, buffer_size, "Gamepad %d (not connected)", slot);
  return snprintf(buffer, buffer_size, "%d: %s", slot,
                  InputMap_GamepadName(slot - 1));
}

/* One binding row per (device class, action). The key is what lands in
 * settings.ini, so it is spelled out rather than derived from the enum. */
#define BINDING_SETTING(action_enum, class_enum, id, text, help) \
  { id, NULL, text, help, kSettingType_Binding, kApply_Passive, \
    kSettingCat_InputBinds, &g_settings.input_bind[class_enum][action_enum], \
    0, 0, 0, 0, false, NULL, 0, NULL, NULL, \
    InputMap_ParseBindingField, InputMap_FormatBindingField }
#define BINDING_SETTINGS(action_enum, id_suffix, text) \
  BINDING_SETTING(action_enum, kInputClass_Keyboard, "bind_key_" id_suffix, \
                  text, "Press Enter, then press the key you want to assign. Use the Reset " \
                  "shortcut to restore the default."), \
  BINDING_SETTING(action_enum, kInputClass_Gamepad, "bind_pad_" id_suffix, \
                  text, "Confirm, then press the button you want to assign. Use the Reset " \
                  "shortcut to restore the default.")
/* Camera rows exist for both classes: the pad gets the stick, and a keyboard
 * player who prefers keys to the mouse can bind them (unbound by default). */
#define BINDING_CAM_SETTINGS(action_enum, id_suffix, text) \
  BINDING_SETTING(action_enum, kInputClass_Keyboard, "bind_key_" id_suffix, \
                  text, "Works in Free Cam for diorama and 3D towns. Press Enter, then " \
                  "press the key you want to assign."), \
  BINDING_SETTING(action_enum, kInputClass_Gamepad, "bind_pad_" id_suffix, \
                  text, "Works in Free Cam for diorama and 3D towns. Confirm, then move the " \
                  "stick or press the button you want to assign.")
/* Town twin-stick rows. Unbound by default; a pad row may take a camera
 * stick direction, and either row may share an SNES button (input_map.c). */
#define BINDING_SHOOT_SETTINGS(action_enum, id_suffix, text) \
  BINDING_SETTING(action_enum, kInputClass_Keyboard, "bind_key_" id_suffix, \
                  text, "Fire in this direction while steering in towns. Press Enter, then " \
                  "press the key you want to assign."), \
  BINDING_SETTING(action_enum, kInputClass_Gamepad, "bind_pad_" id_suffix, \
                  text, "Fire in this direction while steering in towns. Assigning a " \
                  "right-stick direction replaces the camera binding for that " \
                  "direction.")
/* Gamepad-only host action: no keyboard twin. */
#define BINDING_HOST_SETTING(action_enum, id_suffix, text) \
  BINDING_SETTING(action_enum, kInputClass_Gamepad, "bind_pad_" id_suffix, \
                  text, "Confirm, then press the button you want to assign. Use the Reset " \
                  "shortcut to restore the default.")

/* ---- Randomizer support. Enum labels and availability/refresh callbacks for
 * the descriptors below; the transform itself lives in src/randomizer/randomizer.c. */
static const char *const kRandoModeLabels[] = { "Off", "Shuffle", "Random" };
static const char *const kRandoShuffleLabels[] = { "Off", "Shuffle" };
static const char *const kRandoScopeLabels[] = { "Within map", "Within act" };

static bool RandoAvailable(void) {
  return Randomizer_IsAvailable() && !Randomizer_CampaignBound() && g_settings.rando_enable;
}
static bool RandoRuntimeAvailable(void) {
  return Randomizer_IsAvailable() && !Randomizer_CampaignBound();
}
static bool RandoEnemyTypesOn(void) {
  return RandoAvailable() && g_settings.rando_enemy_types != kRandomMode_Off;
}
#define BOOL_SETTING(id, env_name, text, help, cat, def, is_sticky, active, changed) \
  { #id, env_name, text, help, kSettingType_Bool, kApply_Passive, cat, \
    &g_settings.id, def, 0, 1, 1, is_sticky, NULL, 0, active, changed, \
    NULL, NULL }
/* Same row, but its env var is a modern alias parsed like settings.ini rather
 * than with the historical AR_* leading-zero/default-polarity rules. A separate
 * macro rather than a parameter on BOOL_SETTING: only 19 of the 63 bool rows are
 * modern, and the split is not derivable from any other field (see modern_env in
 * settings.h), so naming it at the call site is what keeps it honest. */
#define BOOL_SETTING_MODERN(id, env_name, text, help, cat, def, is_sticky, active, changed) \
  { #id, env_name, text, help, kSettingType_Bool, kApply_Passive, cat, \
    &g_settings.id, def, 0, 1, 1, is_sticky, NULL, 0, active, changed, \
    NULL, NULL, true }
/* The completion scene has one player-facing switch. Individual effects are
 * default-on INI overrides, retained when ordinary menu changes save settings. */
#define COMPLETION_EFFECT_SETTING(id, env_name, text, help) \
  { .key = #id, .env = env_name, .label = text, .tooltip = help, \
    .type = kSettingType_Bool, .apply = kApply_Passive, \
    .category = kSettingCat_Presentation, .field = &g_settings.id, \
    .defval = 1, .maxval = 1, .step = 1, \
    .available = CompletionEffectsAvailable, .modern_env = true, .menu_hidden = true }
#define INT_SETTING(id, env_name, text, help, cat, def, lo, hi, parser, active) \
  { #id, env_name, text, help, kSettingType_Int, kApply_Passive, cat, \
    &g_settings.id, def, lo, hi, 1, false, NULL, 0, active, NULL, \
    parser, NULL }
/* Rows on System > Game must state whether they repair an original-game bug
 * or intentionally change authentic behaviour for convenience. Designated
 * fields keep this metadata independent of SettingDesc's positional tail. */
#define GAME_CHANGE_BOOL_SETTING(id, env_name, text, help, def, is_sticky, kind) \
  { .key = #id, .env = env_name, .label = text, .tooltip = help, \
    .type = kSettingType_Bool, .apply = kApply_Passive, \
    .category = kSettingCat_Enhancements, .field = &g_settings.id, \
    .defval = def, .minval = 0, .maxval = 1, .step = 1, \
    .sticky = is_sticky, .game_change_kind = kind }
#define GAME_CHANGE_INT_SETTING(id, env_name, text, help, def, lo, hi, parser, kind) \
  { .key = #id, .env = env_name, .label = text, .tooltip = help, \
    .type = kSettingType_Int, .apply = kApply_Passive, \
    .category = kSettingCat_Enhancements, .field = &g_settings.id, \
    .defval = def, .minval = lo, .maxval = hi, .step = 1, \
    .parse = parser, .game_change_kind = kind }
#define ACTION_SETTING(id, action_id, category_id, available_fn, hidden, text, help) \
  { .key = id, .label = text, .tooltip = help, \
    .type = kSettingType_Action, .apply = kApply_Action, \
    .category = category_id, .available = available_fn, \
    .action = action_id, .menu_hidden = hidden }
#define SAVE_PROGRESS_SETTING(index, id, env_name, text, help) \
  { id, env_name, text, help, kSettingType_Enum, kApply_Save, \
    kSettingCat_Save, &g_settings.save_region_progress[index], \
    kSaveProgressEdit_LeaveAsIs, kSaveProgressEdit_LeaveAsIs, \
    kSaveProgressEdit_Act2Cleared, 1, false, kSaveProgressEditLabels, \
    kSaveProgressEdit_Count, NULL, NULL, ParseSaveProgressEdit, NULL, \
    .save_page = kSaveEditorPage_Progress }
#define SAVE_STAGE_DIRECT(field_name, id, text, help, maximum) \
  { id, NULL, text, help, kSettingType_Int, kApply_Save, \
    kSettingCat_Save, &g_settings.field_name, 0, 0, maximum, 1, false, \
    NULL, 0, NULL, NULL, ParseStagedDirect, FormatStagedDirect, \
    .save_page = kSaveEditorPage_Status }
#define SAVE_STAGE_ZERO(field_name, id, text, help, maximum) \
  { id, NULL, text, help, kSettingType_Int, kApply_Save, \
    kSettingCat_Save, &g_settings.field_name, 0, 0, (maximum) + 1, 1, false, \
    NULL, 0, NULL, NULL, ParseStagedZeroBased, FormatStagedZeroBased, \
    .save_page = kSaveEditorPage_Status }
#define SAVE_ENUM_FIELD(page, field_ptr, id, text, help, labels) \
  { id, NULL, text, help, kSettingType_Enum, kApply_Save, \
    kSettingCat_Save, field_ptr, 0, 0, \
    (int)(sizeof(labels) / sizeof((labels)[0])) - 1, 1, false, labels, \
    (int)(sizeof(labels) / sizeof((labels)[0])), NULL, NULL, NULL, NULL, \
    .save_page = page }
#define SAVE_SCORE_FIELD(region, act, id, text) \
  { id, NULL, text, "Set the saved act score from 0 to 99,990, in steps of 10.", \
    kSettingType_Int, kApply_Save, kSettingCat_Save, \
    &g_settings.save_scores[region][act], 0, 0, 10000, 1, false, \
    NULL, 0, NULL, NULL, ParseStagedScore, FormatStagedScore, \
    .save_page = kSaveEditorPage_Scores }

const SettingDesc g_setting_descs[] = {
  { "interface_language", "AR_INTERFACE_LANGUAGE", "Interface language",
    "Choose the language for this menu. Game text has its own language setting.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Interface,
    &g_settings.interface_language, 0, 0, kArUiLocale_Count - 1, 1, false,
    kInterfaceLanguageLabels, kArUiLocale_Count, NULL, NULL, NULL, NULL, true },
  { "localization_content", "AR_LOCALIZATION_CONTENT", "Text source",
    "Choose an installed translation by name. Packs in the same language are listed "
    "separately. Packs require Enhanced text rendering. Native US works with either "
    "renderer.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Localization,
    &g_settings.localization_content, 0, 0, kSettingsLocalizationMaximumPacks + 1, 1, false,
    kLocalizationContentLabels, 2, NULL, NULL,
    ParseLocalizationContent, FormatLocalizationContent, true,
    .enum_maximum = LocalizationContentMaximum, .serialize = SerializeLocalizationContent },
  { "localization_presentation", "AR_LOCALIZATION_PRESENTATION", "Text rendering",
    "Native shows the original US text and font. Enhanced can show translation packs and "
    "use the font options below. Choose Native US to use Native rendering.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Localization,
    &g_settings.localization_presentation, 0, 0, 1, 1, false,
    kLocalizationPresentationLabels, 2, TextPresentationSelectable, NULL, NULL,
    FormatLocalizationPresentation, true,
    .serialize = SerializeLocalizationPresentation },
  { "localization_button_prompts", "AR_LOCALIZATION_BUTTON_PROMPTS", "Button prompts",
    "Choose Text for button names in the game's style or Button glyphs for controller "
    "icons. Keyboard prompts use text. Applies at the next dialogue.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Localization,
    &g_settings.localization_button_prompts, 0, 0, 1, 1, false,
    kLocalizationButtonPromptLabels, 2, EnhancedTextSelected, NULL, NULL, NULL, true },
  { "localization_font_scale_percent", "AR_LOCALIZATION_FONT_SCALE_PERCENT",
    "Font size (%)", "Resize enhanced game text. The game fits it inside the space "
                     "available.",
    kSettingType_Int, kApply_Passive, kSettingCat_LocalizationFont,
    &g_settings.localization_font_scale_percent, 140, 80, 140, 5, false,
    NULL, 0, EnhancedTextSelected, NULL, NULL, NULL, true },
  { "localization_font_sampling", "AR_LOCALIZATION_FONT_SAMPLING", "Sampling",
    "Crisp keeps text edges sharp. Smooth softens them when the text is scaled.",
    kSettingType_Enum, kApply_Passive, kSettingCat_LocalizationFont,
    &g_settings.localization_font_sampling, 0, 0, 1, 1, false,
    kLocalizationSamplingLabels, 2, EnhancedTextSelected, NULL, NULL, NULL, true },
  { "localization_font_pixelation", "AR_LOCALIZATION_FONT_PIXELATION",
    "Pixelation", "None keeps text at full resolution. Low resolution enlarges a smaller "
    "image. Mosaic adds pixel blocks to the full-resolution font.",
    kSettingType_Enum, kApply_Passive, kSettingCat_LocalizationFont,
    &g_settings.localization_font_pixelation, 2, 0, 2, 1, false,
    kLocalizationPixelationLabels, 3, EnhancedTextSelected, NULL, NULL, NULL, true },
  { "localization_font_pixel_size", "AR_LOCALIZATION_FONT_PIXEL_SIZE",
    "Pixel size", "Set the size of the pixel blocks on screen, in pixels.",
    kSettingType_Int, kApply_Passive, kSettingCat_LocalizationFont,
    &g_settings.localization_font_pixel_size, 2, 2, 8, 2, false,
    NULL, 0, TextPixelationSelected, NULL, NULL, NULL, true },
  { "display_mode", "AR_DISPLAY_MODE", "Render profile",
    "4:3 authentic shows the original frame. Widescreen raw widens it without fixes. "
    "Widescreen full adds scenery and object fixes for the wider view.",
    kSettingType_Enum, kApply_Callback, kSettingCat_Display,
    &g_settings.display_mode, kDisplayMode_WideFull, kDisplayMode_43,
    kDisplayMode_WideFull, 1, false, kDisplayModeLabels,
    kDisplayMode_PresetCount, WidescreenActive, DisplayModeChanged, NULL, NULL },
  { "hud_scale_percent", "AR_HUD_SCALE", "HUD output scale",
    "Resize the enhanced HUD. At 100%, it draws at its original size in output pixels. The "
    "change appears wherever the enhanced HUD is visible.",
    kSettingType_Int, kApply_Passive, kSettingCat_Display,
    &g_settings.hud_scale_percent, 0, 0, 400, 25, false, NULL, 0,
    NULL, NULL, ParseHudScale, FormatHudScale },
  { "menu_scale_percent", "AR_MENU_SCALE", "Menu output scale",
    "Resize the settings menu. Auto fits the menu to the window.",
    kSettingType_Int, kApply_Passive, kSettingCat_Display,
    &g_settings.menu_scale_percent, 0, 0, 800, 25, false, NULL, 0,
    NULL, NULL, ParseMenuScale, FormatMenuScale },
  BOOL_SETTING(hd_replacements, "AR_HD_REPLACEMENTS", "HD replacements",
               "Use installed HD artwork for matching game assets.",
               kSettingCat_Display, 1, false, HdReplacementsAvailable, NULL),
  { "extended_aspect", "AR_EXTENDED_ASPECT_RATIO", "Screen ratio",
    "Auto fits action stages, world navigation, and towns to the window without stretching. "
    "Extra height needs an action scene or a 3D world or town view. Scene limits can leave "
    "borders. Action stages go up to 16:9.",
    kSettingType_Enum, kApply_Callback, kSettingCat_Display,
    &g_settings.extended_aspect, kScreenAspect_43,
    kScreenAspect_43, kScreenAspect_Auto, 1, false,
    kScreenAspectLabels, kScreenAspect_Count, NULL, OnScreenRatioChanged,
    ParseExtendedAspect, NULL, .modern_env = true },
  { "render_resolution", "AR_RENDER_RESOLUTION", "Render resolution",
    "Set the height of the game image. Screen ratio sets its width. A fixed preset also "
    "sizes the window if it fits on your display. Native follows the output size.",
    kSettingType_Enum, kApply_Callback, kSettingCat_Display,
    &g_settings.render_resolution, kRenderResolution_Native,
    kRenderResolution_Native, kRenderResolution_Count - 1, 1, false,
    (const char *const[]){"Native", "480p", "540p", "720p", "800p", "900p",
                         "1080p", "1440p", "2160p"},
    kRenderResolution_Count, NULL, NULL, NULL, NULL, .modern_env = true },
  BOOL_SETTING_MODERN(scene_supersampling, "AR_SCENE_SUPERSAMPLING", "3D supersampling",
    "Smooth 3D edges by drawing at a higher resolution and shrinking the image. Choose a "
    "fixed Render resolution above the view size. This uses more GPU time.",
    kSettingCat_Display, 0, false, NULL, NULL),
  { "scene_antialiasing", "AR_SCENE_ANTIALIASING", "3D anti-aliasing",
    "Smooth jagged edges in 3D scenes. FXAA is faster. SMAA can keep more detail. "
    "Supersampling uses its own filter when active.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Display,
    &g_settings.scene_antialiasing, kSceneAntiAliasing_Off,
    kSceneAntiAliasing_Off, kSceneAntiAliasing_Count-1, 1, false,
    (const char *const[]){"Off", "FXAA", "SMAA"}, kSceneAntiAliasing_Count,
    NULL, NULL, NULL, NULL, .modern_env = true },
  { "pixel_aspect", "AR_ASPECT_PAR", "Pixel aspect",
    "Choose the original 4:3 CRT proportions or use square pixels.",
    kSettingType_Enum, kApply_Callback, kSettingCat_Display,
    &g_settings.pixel_aspect, kPixelAspect_Crt43,
    kPixelAspect_Square, kPixelAspect_Crt43, 1, false,
    kPixelAspectLabels, kPixelAspect_Count, NULL, NULL,
    ParsePixelAspect, NULL, .modern_env = true },
  { "window_mode", "AR_WINDOW_MODE", "Window mode",
    "Choose a window, a borderless window that fills the desktop, or exclusive fullscreen.",
    kSettingType_Enum, kApply_Callback, kSettingCat_Display,
    &g_settings.window_mode, kWindowMode_Windowed,
    kWindowMode_Windowed, kWindowMode_Exclusive, 1, false,
    kWindowModeLabels, kWindowMode_Count, NULL, NULL, NULL, NULL, .modern_env = true },
  { "window_scale", "AR_WINDOW_SCALE", "Window scale",
    "Resize the window by a multiple of the game image. This applies only in Windowed mode.",
    kSettingType_Int, kApply_Callback, kSettingCat_Display,
    &g_settings.window_scale, 3, 1, 8, 1, false, NULL, 0,
    WindowScaleAvailable, NULL, NULL, NULL, .modern_env = true, .menu_hidden = true },
  { "picture_in_picture_corner", "AR_PICTURE_IN_PICTURE_CORNER", "PiP corner",
    "Choose a corner for the small authentic view. Hold Compare rendering to show or hide "
    "it.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Display,
    &g_settings.picture_in_picture_corner, kPictureInPicture_BottomRight,
    kPictureInPicture_BottomRight, kPictureInPicture_TopLeft, 1, false,
    kPictureInPictureCornerLabels, kPictureInPictureCorner_Count,
    NULL, NULL, NULL, NULL, .modern_env = true },
  BOOL_SETTING_MODERN(picture_in_picture_window, "AR_PICTURE_IN_PICTURE_WINDOW",
    "PiP separate window", "Put the authentic view in its own window. Move it to another "
    "display to compare both views side by side. Hold Compare "
    "rendering to show or hide it.", kSettingCat_Display, 0, false,
    PictureInPictureWindowAvailable, NULL),
  BOOL_SETTING_MODERN(picture_in_picture_fullscreen, "AR_PICTURE_IN_PICTURE_FULLSCREEN",
    "PiP fullscreen", "Fill the display used by the separate PiP window. Move that window "
    "to the display you want before turning this on.", kSettingCat_Display, 0, false,
    PictureInPictureFullscreenAvailable, NULL),
  { "ignore_aspect_ratio", "AR_IGNORE_ASPECT_RATIO", "Stretch to window",
    "Legacy compatibility alias for Screen ratio > Stretch.",
    kSettingType_Bool, kApply_Callback, kSettingCat_Display,
    &g_settings.ignore_aspect_ratio, 0, 0, 1, 1, false, NULL, 0,
    NULL, OnLegacyStretchChanged, NULL, NULL, .modern_env = true },
  { "refresh_mode", "AR_REFRESH_MODE", "Refresh rate",
    "Vsync follows the display refresh. Uncapped turns off Vsync and aims to stay below "
    "twice the display refresh. Limit uses Frame limit. Unlimited removes the frame cap.",
    kSettingType_Enum, kApply_Callback, kSettingCat_Display,
    &g_settings.refresh_mode, kRefreshMode_Vsync,
    kRefreshMode_Vsync, kRefreshMode_Unlimited, 1, false,
    kRefreshModeLabels, kRefreshMode_Count, NULL, NULL, NULL, NULL, .modern_env = true },
  { "frame_limit_fps", "AR_FRAME_LIMIT_FPS", "Frame limit",
    "Set the target frames per second used by Refresh rate: Limit.",
    kSettingType_Int, kApply_Callback, kSettingCat_Display,
    &g_settings.frame_limit_fps, 60, 20, 480, 5, false, NULL, 0,
    FrameLimitActive, NULL, NULL, NULL, .modern_env = true },
  /* SDL's Direct3D 12 backend makes every upload buffer a committed resource;
   * its Vulkan backend sub-allocates them. A Windows Town 3D slowdown report
   * (2026-09-16) pointed at that path, so players can pick the API that suits
   * their system. */
  { "gpu_backend", "AR_GPU_BACKEND", "Graphics API",
    "Automatic picks the default for your platform (Direct3D 12 on Windows). Try another "
    "API if you see graphics or performance problems. If it fails to start, the game uses "
    "Automatic.",
    kSettingType_Enum, kApply_Restart, kSettingCat_Display,
    &g_settings.gpu_backend, kGpuBackend_Automatic,
    kGpuBackend_Automatic, kGpuBackend_Metal, 1, false,
    kGpuBackendLabels, kGpuBackend_Count, NULL, NULL, NULL, NULL,
    .modern_env = true, .value_available = GpuBackendOffered },
  BOOL_SETTING_MODERN(show_fps, "AR_SHOW_FPS", "FPS counter",
               "Show frames presented per second in the top-right corner. Choose Unlimited "
               "refresh rate to measure peak rendering speed.",
               kSettingCat_Display, 0, false, NULL, NULL),
  { "performance_overlay", "AR_PERFORMANCE_OVERLAY", "Performance overlay",
    "Show CPU timings, frame pacing, and worker activity. Detailed breaks down more steps. "
    "GPU timings are unavailable.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Display,
    &g_settings.performance_overlay, 0, 0, 2, 1, false,
    kPerformanceOverlayLabels, 3,
    NULL, NULL, NULL, NULL, .modern_env = true },
  BOOL_SETTING_MODERN(sim3d_mode, "AR_SIM3D", "Simulation town 3D",
               "Tilt town maps into a 3D view. Map pickers also use the tilted view.",
               kSettingCat_Simulation, 0, false, NULL,
               NULL),
  BOOL_SETTING_MODERN(sim3d_church, "AR_SIM3D_CHURCH", "Church interior 3D",
               "Show audiences and offerings in a softly lit church overlooking the town. "
               "Requires Simulation town 3D.",
               kSettingCat_Simulation, 0, false, Sim3D_ModeIsOn, NULL),
  { "sim_menu_style", "AR_SIM_MENU_STYLE", "SIM menu",
    "Original uses the classic town menu. Modern uses a compact crossbar. Use Describe menu "
    "item to read the selected item's help. Set its shortcut under Controls.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim_menu_style, 0, 0, 1, 1, false,
    (const char *const[]){"Original", "Modern"}, 2,
    NULL, NULL, NULL, NULL, .modern_env = true, .player_visible = true },
  { "sim_menu_scale_percent", "AR_SIM_MENU_SCALE", "SIM menu scale (%)",
    "Resize the modern town menu, prompts, and Message Speed selector. Dialogue, the HUD, "
    "and the authentic comparison view have their own size.",
    kSettingType_Int, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim_menu_scale_percent, 50, 50, 100, 5, false, NULL, 0,
    SimMenuModernSelected, NULL, NULL, NULL,
    .modern_env = true, .player_visible = true },
  { "sim_view_range", "AR_SIM_VIEW_RANGE", "Extended actor range",
    "Keep town actors and projectiles active farther offscreen, in original game pixels. "
    "The game has 44 slots for them, so higher values can fill the slots sooner. 0 uses the "
    "original range.",
    kSettingType_Int, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim_view_range, 0, 0, 256, 16, false, NULL, 0,
    Sim3D_ModeIsOn, NULL, NULL, NULL,
    .modern_env = true, .player_visible = true },
  BOOL_SETTING_MODERN(sim3d_world_navigation, "AR_SIM3D_WORLD_NAV",
               "World navigation 3D",
               "Travel between towns on a 3D globe with raised terrain.",
               kSettingCat_Simulation, 0, false, NULL,
               NULL),
  BOOL_SETTING_MODERN(sim3d_sky_palace, "AR_SIM3D_SKY_PALACE",
               "Sky Palace globe backdrop",
               "Look across the globe from cloud level behind the Sky Palace pillars. "
               "Requires World navigation 3D.",
               kSettingCat_Simulation, 1, false,
               WorldNavigation3DEnabled, NULL),
  BOOL_SETTING_MODERN(sim3d_sky_palace_volumetric, "AR_SIM3D_SKY_PALACE_VOLUMETRIC",
               "Sky Palace volumetric clouds",
               "Draw clouds with depth and lighting around the Sky Palace. Turn this off to "
               "use a faster single cloud layer. Cloud density and drift apply to both.",
               kSettingCat_Simulation, 1, false, SkyPalaceCloudsAvailable, NULL),
  BOOL_SETTING_MODERN(sim3d_world_navigation_lighting,
               "AR_SIM3D_WORLD_NAV_LIGHTING",
               "World navigation lighting",
               "Light the globe, its terrain, and the connected world underlay in towns. "
               "World navigation cloud shadows use this lighting too.",
               kSettingCat_Simulation, 1, false,
               WorldGlobeAvailable, NULL),
  BOOL_SETTING_MODERN(sim3d_world_navigation_clouds,
               "AR_SIM3D_WORLD_NAV_CLOUDS",
               "World navigation clouds",
               "Show cloud banks over the globe. They are visible when zoomed out and fade "
               "as the Sky Palace descends beneath them.",
               kSettingCat_Simulation, 1, false,
               WorldNavigation3DEnabled, NULL),
  BOOL_SETTING_MODERN(sim3d_world_navigation_cloud_shadows,
               "AR_SIM3D_WORLD_NAV_CLOUD_SHADOWS",
               "World navigation cloud shadows",
               "Cast cloud shadows on the globe. Turning this off saves rendering work "
               "while the clouds stay visible. Requires world navigation lighting and "
               "clouds.",
               kSettingCat_Simulation, 1, false,
               WorldNavigation3DEnabled, NULL),
  BOOL_SETTING_MODERN(sim3d_world_navigation_atmosphere,
               "AR_SIM3D_WORLD_NAV_ATMOSPHERE",
               "World navigation atmosphere",
               "Show the blue atmosphere around the globe. Clouds, haze, and the space "
               "backdrop have separate controls.",
               kSettingCat_Simulation, 1, false,
               WorldNavigation3DEnabled, NULL),
  BOOL_SETTING_MODERN(sim3d_world_navigation_towns,
               "AR_SIM3D_WORLD_NAV_TOWNS",
               "World navigation town models",
               "Show 3D buildings and trees in developed towns on the globe. Off shows "
               "their live map artwork. The 3D view inside a town has its own setting.",
               kSettingCat_Simulation, 1, false,
               WorldGlobeAvailable, NULL),
  BOOL_SETTING_MODERN(sim3d_world_navigation_relief,
               "AR_SIM3D_WORLD_NAV_RELIEF",
               "World navigation terrain relief",
               "Raise mountains and town terrain above the globe. Off keeps the globe "
               "smooth. Terrain inside towns has its own setting.",
               kSettingCat_Simulation, 1, false,
               WorldGlobeAvailable, NULL),
  BOOL_SETTING_MODERN(sim3d_world_navigation_ground_detail,
               "AR_SIM3D_WORLD_NAV_GROUND_DETAIL",
               "World navigation detailed ground",
               "Show live paths, shorelines, and terrain at the original town map "
               "resolution on the globe.",
               kSettingCat_Simulation, 1, false,
               WorldGlobeAvailable, NULL),
  BOOL_SETTING_MODERN(sim3d_world_navigation_mountains,
               "AR_SIM3D_WORLD_NAV_MOUNTAINS",
               "World navigation native mountains",
               "Show detailed mountain faces and walls from the town maps on the globe. Off "
               "uses simpler relief. Requires World navigation terrain relief.",
               kSettingCat_Simulation, 1, false,
               WorldGlobeAvailable, NULL),
  /* The enhanced renderer, stage by stage. Each is an ordinary toggle so a
   * stage can be turned on or off by name; `kSim3DShippedFeatures` is the one
   * list of stages with a shipped implementation, and the defaults here must
   * agree with it. A stage missing from both is invisible in normal play no
   * matter how it is tuned. */
  BOOL_SETTING_MODERN(sim3d_separated_composite, "AR_SIM3D_SEPARATED",
               "Separated layer capture",
               "Draw the town from separate map and sprite layers. The other town 3D "
               "effects depend on this step.",
               kSettingCat_Simulation, 1, false, Sim3DSeparatedAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_ground_projection, "AR_SIM3D_GROUND",
               "Ground projection",
               "Tilt the town map back into the scene. Off draws the map flat.",
               kSettingCat_Simulation, 1, false, Sim3DGroundAvailable,
               NULL),
  { "sim3d_voxel_preset", "AR_SIM3D_VOXEL_PRESET", "Voxel town quality",
    "Off uses tile art. Performance uses simpler models. Balanced adds detail, contact "
    "shadows, and crisp edges. Quality adds Smooth 2x and more detail. Custom uses the "
    "controls below. The globe shares model detail and style.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_voxel_preset, kSimBackgroundVoxelPreset_Balanced,
    kSimBackgroundVoxelPreset_Off, kSimBackgroundVoxelPreset_Custom, 1,
    false, kSimVoxelPresetLabels, kSimBackgroundVoxelPreset_Count,
    Sim3DOrWorldNavigationModelsAvailable, NULL, NULL, NULL,
    .modern_env = true, .player_visible = true },
  { "sim3d_landscape_height_pct", "AR_SIM3D_LANDSCAPE_HEIGHT",
    "Landscape height (%)",
    "Set the height of terrain in towns and on the globe. 100% keeps full height, 50% "
    "halves it, and 0% flattens it. Buildings and flying objects have their own height.",
    kSettingType_Int, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_landscape_height_pct,
    kSimTownTerrainLandscapeHeightDefaultPct,
    kSimTownTerrainLandscapeHeightMinimumPct,
    kSimTownTerrainLandscapeHeightMaximumPct,
    kSimTownTerrainLandscapeHeightStepPct, false,
    NULL, 0, Sim3DLandscapeHeightAvailable, NULL, NULL, NULL,
    .modern_env = true,
    .player_visible = AR_SIM3D_TERRAIN_ELEVATION != 0 },
  { "sim3d_voxel_detail", "AR_SIM3D_VOXEL_DETAIL", "Voxel model detail",
    "Set how much detail buildings and trees have. Low favors speed. Balanced keeps common "
    "details. High adds finer roofs and walls. Ultra uses the full models.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_voxel_detail, kSimBackgroundVoxelDetail_High,
    kSimBackgroundVoxelDetail_Low, kSimBackgroundVoxelDetail_Ultra, 1, false,
    kSimVoxelDetailLabels, kSimBackgroundVoxelDetail_Count,
    Sim3DOrWorldNavigationModelCustomEnabled, NULL, NULL, NULL,
    .modern_env = true, .player_visible = true },
  { "sim3d_voxel_lod", "AR_SIM3D_VOXEL_LOD", "Voxel detail scaling",
    "Fixed uses your chosen detail everywhere. Adaptive uses simpler models when buildings "
    "or trees appear too small to show the extra detail.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_voxel_lod, kSimBackgroundVoxelLod_Adaptive,
    kSimBackgroundVoxelLod_Fixed, kSimBackgroundVoxelLod_Adaptive, 1, false,
    kSimVoxelLodLabels, kSimBackgroundVoxelLod_Count,
    Sim3DVoxelCustomEnabled, NULL, NULL, NULL,
    .modern_env = true, .player_visible = true },
  { "sim3d_voxel_shading", "AR_SIM3D_VOXEL_SHADING", "Voxel shading",
    "Basic lights each face evenly. Ambient occlusion adds contact shadows. Materials + AO "
    "also lights foliage, stone, roofs, and metal differently.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_voxel_shading,
    kSimBackgroundVoxelShading_MaterialAware,
    kSimBackgroundVoxelShading_Basic,
    kSimBackgroundVoxelShading_MaterialAware, 1, false,
    kSimVoxelShadingLabels, kSimBackgroundVoxelShading_Count,
    Sim3DVoxelCustomEnabled, NULL, NULL, NULL,
    .modern_env = true, .player_visible = true },
  { "sim3d_voxel_style", "AR_SIM3D_VOXEL_STYLE", "Voxel styling",
    "Basic keeps simple shapes. Silhouette trim adds eaves and wall details. Architectural "
    "adds factory courtyards. Varied gives models more individual details.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_voxel_style, kSimBackgroundVoxelStyle_Varied,
    kSimBackgroundVoxelStyle_Basic, kSimBackgroundVoxelStyle_Varied, 1,
    false, kSimVoxelStyleLabels, kSimBackgroundVoxelStyle_Count,
    Sim3DOrWorldNavigationModelCustomEnabled, NULL, NULL, NULL,
    .modern_env = true, .player_visible = true },
  { "sim3d_voxel_facing", "AR_SIM3D_VOXEL_FACING", "Voxel camera facing",
    "Shared lean tilts every model toward the camera by the same amount. Per-model lean "
    "keeps houses and windmills upright and shows more depth in factories and trees.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_voxel_facing, kSimBackgroundVoxelFacing_PerModel,
    kSimBackgroundVoxelFacing_Shared, kSimBackgroundVoxelFacing_PerModel, 1,
    false, kSimVoxelFacingLabels, kSimBackgroundVoxelFacing_Count,
    Sim3DVoxelCustomEnabled, NULL, NULL, NULL,
    .modern_env = true, .player_visible = true },
  { "sim3d_voxel_render_scale", "AR_SIM3D_VOXEL_RENDER_SCALE",
    "Voxel edge quality",
    "Native draws at scene resolution. Pixel-clean aligns edges with screen pixels. Smooth "
    "2x draws at double resolution on High or Ultra detail. It uses Native in large "
    "windows.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_voxel_render_scale,
    kSimBackgroundVoxelRenderScale_PixelClean,
    kSimBackgroundVoxelRenderScale_Native,
    kSimBackgroundVoxelRenderScale_PixelClean, 1, false,
    kSimVoxelRenderScaleLabels, kSimBackgroundVoxelRenderScale_Count,
    Sim3DVoxelCustomEnabled, NULL, NULL, NULL,
    .modern_env = true, .player_visible = true },
  BOOL_SETTING_MODERN(sim3d_object_billboards, "AR_SIM3D_BILLBOARDS",
               "Object billboards",
               "Draw objects as separate sprites standing on the tilted town map.",
               kSettingCat_Simulation, 1, false, Sim3DBillboardsAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_virtual_height, "AR_SIM3D_HEIGHT",
               "Object heights",
               "Draw flying actors and effects above the ground at their assigned heights. "
               "Requires Object billboards.",
               kSettingCat_Simulation, 1, false, Sim3DVirtualHeightAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_shadows, "AR_SIM3D_SHADOWS", "Ground shadows",
               "Cast shadows from objects onto the town ground. Requires Object billboards. "
               "Set darkness with Shadow darkness.",
               kSettingCat_Simulation, 1, false, Sim3DShadowsAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_soft_shadows, "AR_SIM3D_SOFT_SHADOWS",
               "Soft shadows",
               "Soften the edges of ground shadows. Requires Ground shadows. Set the blur "
               "with Shadow softness.",
               kSettingCat_Simulation, 1, false, Sim3DSoftShadowsAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_rim_light, "AR_SIM3D_RIM_LIGHT", "Rim light",
               "Light the edges of sprites facing the light. Requires Object billboards. "
               "Set brightness with Rim light strength.",
               kSettingCat_Simulation, 1, false, Sim3DRimLightAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_effect_lighting, "AR_SIM3D_EFFECT_LIGHTING",
               "Effect lighting",
               "Light up the area around town miracles and enemy attacks as they happen.",
               kSettingCat_Simulation, 1, false,
               Sim3DEffectLightingAvailable, NULL),
  BOOL_SETTING_MODERN(sim3d_particles, "AR_SIM3D_PARTICLES", "Effect particles",
               "Add particles to town miracles and enemy attacks.",
               kSettingCat_Simulation, 1, false,
               Sim3DParticlesAvailable, NULL),
  BOOL_SETTING_MODERN(sim3d_world_underlay, "AR_SIM3D_WORLD_UNDERLAY",
               "World map underlay",
               "Show the world map beyond the town edges, including nearby regions. "
               "Requires Ground projection. World map haze controls the fade.",
               kSettingCat_Simulation, 1, false, Sim3DWorldUnderlayAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_globe_underlay, "AR_SIM3D_GLOBE_UNDERLAY",
               "Connected world underlay",
               "Show the globe and nearby towns behind the active town. This limits orbit "
               "and zoom to the local area. Requires World map underlay and a Voxel town "
               "quality other than Off.",
               kSettingCat_Simulation, 1, false, Sim3DGlobeUnderlayAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_cloud_shroud, "AR_SIM3D_CLOUDS", "Cloud shroud",
               "Cover the distant ground with drifting clouds where the game stops drawing "
               "sprites. Requires World map underlay.",
               kSettingCat_Simulation, 1, false, Sim3DCloudShroudAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_cull_haze, "AR_SIM3D_CULL_HAZE_STAGE",
               "Local-area haze",
               "Fade the ground where the game stops drawing town sprites. In world "
               "navigation, the same effect keeps the selected location clear and hazes "
               "nearby regions.",
               kSettingCat_Simulation, 1, false,
               Sim3DOrWorldNavigationHazeAvailable, NULL),
  { "sim3d_camera_mode", "AR_SIM3D_CAMERA_MODE", "Camera mode",
    "Free Cam remembers your manual orbit and zoom. Dynamic Cam has its own resting "
    "position, leans as the angel moves, and jolts when hit.",
    kSettingType_Enum, kApply_Passive, kSettingCat_SimCamera,
    &g_settings.sim3d_camera_mode, kSimCam_Dynamic,
    kSimCam_Free, kSimCam_Dynamic, 1, false,
    kSimCamModeLabels, kSimCam_Count, Sim3DGroundEnabled, NULL,
    NULL, NULL, .modern_env = true },
  /* Dynamic Cam's dedicated pose. Defaults are the captured baseline (see the
   * sim3d defaults note above), so the shipped Dynamic view is the one that
   * was actually tuned rather than whatever Free Cam was last left at. */
  { "sim3d_dyncam_baseline_tilt_x_mrad", NULL, "Dynamic baseline pitch",
    "Set the Dynamic Cam's resting pitch in milliradians, from a high three-quarter view "
    "toward ground level.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimCamera,
    &g_settings.sim3d_dyncam_baseline_tilt_x_mrad, -575,
    kSim3DCameraPitchMinimumMrad, kSim3DCameraPitchMaximumMrad, 25, false,
    NULL, 0, Sim3DDynamicCameraAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_dyncam_baseline_tilt_y_mrad", NULL, "Dynamic baseline yaw",
    "Set the Dynamic Cam's resting yaw in milliradians. The camera leans around this angle.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimCamera,
    &g_settings.sim3d_dyncam_baseline_tilt_y_mrad, 0,
    kSim3DCameraYawMinimumMrad, kSim3DCameraYawMaximumMrad, 20, false,
    NULL, 0, Sim3DDynamicCameraAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_dyncam_baseline_distance_x100", NULL, "Dynamic baseline distance",
    "Set the Dynamic Cam's resting distance in hundredths. 0 fits the scene automatically.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimCamera,
    &g_settings.sim3d_dyncam_baseline_distance_x100, 300, 0, 2000, 25, false,
    NULL, 0, Sim3DDynamicCameraAvailable, NULL, NULL, NULL, .modern_env = true },
  BOOL_SETTING_MODERN(sim3d_cull_lift_inset, "AR_SIM3D_LIFT_INSET",
               "Account for flight height at the edge",
               "Move the clear area's near edge inward so flying sprites disappear under "
               "cover. This leaves less clear ground along that edge.",
               kSettingCat_Simulation, 1, false, Sim3DCullHazeEnabled,
               NULL),
  BOOL_SETTING_MODERN(sim3d_backdrop, "AR_SIM3D_BACKDROP", "Atmospheric backdrop",
               "Draw a sky gradient behind the ground. It follows the tilted town horizon "
               "or the globe's on-screen horizon in world navigation.",
               kSettingCat_Simulation, 1, false, Sim3DBackdropAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_picker_exit_ease, "AR_SIM3D_PICKER_EASE",
               "Ease picker exit",
               "Reserved for a smooth camera return after choosing a map location.",
               kSettingCat_Simulation, 0, false, Sim3DPickerEaseAvailable,
               NULL),
  { "sim3d_diagnostic_layers", "AR_SIM3D_DIAGNOSTIC_LAYERS",
    "SIM diagnostic layers",
    "Show only the town 3D layers selected by this hexadecimal mask. Use 0 to draw the full "
    "profile.",
    kSettingType_Mask, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_diagnostic_layers, 0, 0, 0xFFFF, 1, false,
    NULL, 0, Sim3DSeparatedEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_tilt_y_mrad", "AR_SIM3D_YAW", "Camera yaw",
    "Turn the Free Cam left or right, in milliradians. You can also drag horizontally with "
    "the right mouse button.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimCamera,
    &g_settings.sim3d_tilt_y_mrad, 0,
    kSim3DCameraYawMinimumMrad, kSim3DCameraYawMaximumMrad,
    20, false, NULL, 0,
    Sim3DFreeCameraAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_tilt_x_mrad", "AR_SIM3D_PITCH", "Camera pitch",
    "Tilt the Free Cam up or down, in milliradians. You can also drag vertically with the "
    "right mouse button.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimCamera,
    &g_settings.sim3d_tilt_x_mrad, -575,
    kSim3DCameraPitchMinimumMrad, kSim3DCameraPitchMaximumMrad,
    25, false, NULL, 0,
    Sim3DFreeCameraAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_distance_x100", "AR_SIM3D_DISTANCE", "Camera distance",
    "Set camera distance in hundredths. 0 fits the scene automatically. The mouse wheel "
    "also zooms.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimCamera,
    &g_settings.sim3d_distance_x100, 300, 0, 2000, 25, false, NULL, 0,
    Sim3DFreeCameraAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_height_scale_x100", "AR_SIM3D_HEIGHT_SCALE",
    "Object height scale",
    "Scale the height of flying sprites. 100% uses their assigned heights. 0% draws them at "
    "ground level.",
    kSettingType_Int, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_height_scale_x100, 100, 0, 400, 10, false, NULL, 0,
    Sim3DVirtualHeightEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_shadow_opacity_pct", "AR_SIM3D_SHADOW_OPACITY",
    "Shadow darkness",
    "Set how dark object shadows and world-navigation cloud shadows appear. 0% skips "
    "drawing those shadows.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimLighting,
    &g_settings.sim3d_shadow_opacity_pct, kSimShadowOpacityDefaultPct,
    0, 100, 5, false, NULL, 0,
    Sim3DOrWorldNavigationShadowAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_light_azimuth_deg", "AR_SIM3D_LIGHT_AZIMUTH",
    "Light direction",
    "Set which way shadows fall. 0° casts right, 90° away from the camera, 180° left, and "
    "270° toward the camera. This also applies to cloud shadows.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimLighting,
    &g_settings.sim3d_light_azimuth_deg, kSimLightAzimuthDefaultDeg,
    0, 359, 15, false, NULL, 0,
    Sim3DOrWorldNavigationLightingAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_light_elevation_deg", "AR_SIM3D_LIGHT_ELEVATION",
    "Light height",
    "Set the light angle above the ground. 90° puts shadows directly below objects. Lower "
    "angles cast longer shadows and warm the light in world navigation.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimLighting,
    &g_settings.sim3d_light_elevation_deg, kSimLightElevationDefaultDeg,
    20, 90, 5, false, NULL, 0,
    Sim3DOrWorldNavigationLightingAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_shadow_softness_pct", "AR_SIM3D_SHADOW_SOFTNESS",
    "Shadow softness",
    "Soften shadow edges. 0% keeps hard town shadows. Higher values spread cloud shadows in "
    "world navigation.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimLighting,
    &g_settings.sim3d_shadow_softness_pct, kSimShadowSoftnessDefaultPct,
    0, 100, 5, false, NULL, 0,
    Sim3DOrWorldNavigationSoftShadowAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_rim_strength_pct", "AR_SIM3D_RIM_STRENGTH",
    "Rim light strength",
    "Set how bright the lit side of a sprite's outline appears. 0% leaves the sprite's "
    "colors unchanged.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimLighting,
    &g_settings.sim3d_rim_strength_pct, kSimRimStrengthDefaultPct,
    0, 100, 5, false, NULL, 0, Sim3DRimLightEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_underlay_haze_pct", "AR_SIM3D_UNDERLAY_HAZE",
    "World map haze",
    "Fade the distant world map into the backdrop. In world navigation, this controls the "
    "haze around the selected region. 0% removes the haze.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_underlay_haze_pct, kSimUnderlayHazeDefaultPct,
    0, 100, 5, false, NULL, 0,
    Sim3DOrWorldNavigationHazeEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cloud_opacity_pct", "AR_SIM3D_CLOUD_OPACITY",
    "Cloud density",
    "Set how opaque town and world-navigation clouds look. 0% makes them invisible.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cloud_opacity_pct, kSimCloudOpacityDefaultPct,
    0, 100, 5, false, NULL, 0,
    Sim3DOrWorldNavigationCloudsAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cloud_falloff_px", "AR_SIM3D_CLOUD_FALLOFF",
    "Cloud edge softness",
    "Set the width of the cloud fade beyond the sprite range, in original game pixels. "
    "Higher values give a softer edge.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cloud_falloff_px, kSimCloudFalloffDefaultPx,
    16, 512, 16, false, NULL, 0, Sim3DCloudShroudEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cloud_inset_px", "AR_SIM3D_CLOUD_INSET",
    "Cloud edge overlap",
    "Start the cloud fade this many original game pixels inside the sprite range. Higher "
    "values cover sprites sooner, before they disappear.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cloud_inset_px, kSimCloudInsetDefaultPx,
    0, 512, 16, false, NULL, 0, Sim3DCloudShroudEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cull_lead_px", "AR_SIM3D_CULL_LEAD",
    "Cloud lead on culled sprites",
    "Set how far inside the sprite range clouds fully cover a sprite, in original game "
    "pixels. Small values can make sprites pop out of view.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cull_lead_px, kSimCullLeadDefaultPx,
    8, 256, 8, false, NULL, 0, Sim3DCloudShroudEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cull_haze_pct", "AR_SIM3D_CULL_HAZE",
    "Out-of-range ground fade",
    "Fade the ground into the distant map beyond the range where sprites are drawn. 0% "
    "keeps the ground fully visible. World navigation uses World map haze instead.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cull_haze_pct, kSimCullHazeDefaultPct,
    0, 100, 5, false, NULL, 0, Sim3DCullHazeEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cull_dim_pct", "AR_SIM3D_CULL_DIM",
    "Out-of-range darkening",
    "Darken the ground outside the range where sprites are drawn. Out-of-range ground fade "
    "controls how much of the distant map shows through.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cull_dim_pct, kSimCullDimDefaultPct,
    0, 100, 5, false, NULL, 0, Sim3DCullHazeEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cull_haze_lead_px", "AR_SIM3D_CULL_HAZE_LEAD",
    "Ground fade ramp width",
    "Set how many original game pixels the ground fade takes to reach full strength. A "
    "longer fade softens the edge. In world navigation, the selected location stays clear.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cull_haze_lead_px, kSimCullHazeLeadDefaultPx,
    16, 512, 16, false, NULL, 0,
    Sim3DOrWorldNavigationHazeEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cull_corner_px", "AR_SIM3D_CULL_CORNER",
    "Ground fade corner rounding",
    "Round the corners of the clear area, in original game pixels. 0 keeps the clear area "
    "rectangular.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cull_corner_px, kSimCullCornerDefaultPx,
    0, 256, 8, false, NULL, 0, Sim3DCullHazeEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_underlay_defocus_pct", "AR_SIM3D_DEFOCUS",
    "World map defocus",
    "Blur the distant map outside the active town or location. 0% keeps it sharp.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_underlay_defocus_pct, kSimUnderlayDefocusDefaultPct,
    0, 100, 5, false, NULL, 0,
    Sim3DOrWorldNavigationHazeEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cloud_altitude_px", "AR_SIM3D_CLOUD_ALTITUDE",
    "Cloud altitude",
    "Raise the clouds above the ground, in original game pixels. 0 lays them on the "
    "terrain. On the globe, this also affects where shadows fall and when the view crosses "
    "the clouds.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cloud_altitude_px, kSimCloudAltitudeDefaultPx,
    0, 256, 8, false, NULL, 0,
    Sim3DOrWorldNavigationCloudsAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cloud_drift_pct", "AR_SIM3D_CLOUD_DRIFT",
    "Cloud drift speed",
    "Set how fast the cloud layers drift. Each layer has its own base speed. 0% holds them "
    "still.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cloud_drift_pct, kSimCloudDriftDefaultPct,
    0, 500, 10, false, NULL, 0,
    Sim3DOrWorldNavigationCloudsAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_backdrop_strength_pct", "AR_SIM3D_BACKDROP_STRENGTH",
    "Sky gradient strength",
    "Blend the scene's backdrop color into a blue sky that brightens toward the horizon. 0% "
    "uses the flat backdrop color.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_backdrop_strength_pct, kSimBackdropStrengthDefaultPct,
    0, 100, 5, false, NULL, 0, Sim3DBackdropEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_backdrop_horizon_pct", "AR_SIM3D_BACKDROP_HORIZON",
    "Sky horizon height",
    "Set the sky gradient's horizon as a percentage from the top of the screen. Used when "
    "the scene has no projected horizon, including world navigation.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_backdrop_horizon_pct, kSimBackdropHorizonDefaultPct,
    0, 100, 5, false, NULL, 0, Sim3DBackdropEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_reactive_strength", "AR_SIM3D_REACTIVE",
    "Camera reactivity",
    "Set how strongly the camera leans as the angel moves and jolts when hit. 0% keeps it "
    "at its resting position.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimCamera,
    &g_settings.sim3d_reactive_strength, 100, 0, 200, 10, false, NULL, 0,
    Sim3DDynamicCameraAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_height_pop_pct", "AR_SIM3D_HEIGHT_POP",
    "Flying sprite pop",
    "Make flying sprites look larger at their assigned height. 0% keeps natural "
    "perspective. Higher Object height scale also increases this effect.",
    kSettingType_Int, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_height_pop_pct, 0, 0, 50, 1, false, NULL, 0,
    Sim3DVirtualHeightEnabled, NULL, NULL, NULL, .modern_env = true },
  ACTION_SETTING("sim3d_reset_camera", kSettingAction_SimCameraReset,
                 kSettingCat_SimCamera, Sim3D_ModeIsOn, false, "Reset camera",
                     "Restore the current camera mode's default angle and zoom."),
  { "diorama_mode", NULL, "Action 3D",
    "Off keeps action stages flat. Diorama adds depth between layers and is the default 3D "
    "mode. Voxel 3D adds modeled scenery in supported rooms and is still in development.",
    kSettingType_Enum, kApply_Callback, kSettingCat_Presentation,
    &g_settings.diorama_mode, kAction3D_Off, kAction3D_Off, kAction3D_Voxel,
    1, false, kAction3DModeLabels, kAction3D_Count, NULL,
    DioramaModeChanged, ParseAction3DMode, NULL },
  /* Free and Dynamic cameras own independent poses. Dynamic applies velocity
   * lean and event kicks around its baseline; switching modes restores the
   * selected mode's own camera instead of carrying the other pose across. */
  { "diorama_camera_mode", NULL, "Camera mode",
    "Free Cam remembers your manual orbit and zoom. Dynamic Cam follows gameplay motion and "
    "reacts to impacts. It returns to its resting position after manual adjustments.",
    kSettingType_Enum, kApply_Passive, kSettingCat_DioramaCamera,
    &g_settings.diorama_camera_mode, kDioramaCam_Dynamic,
    kDioramaCam_Free, kDioramaCam_Dynamic, 1, false,
    kDioramaCamModeLabels, kDioramaCam_Count, Diorama_ModeIsOn, NULL,
    NULL, NULL },
  /* B4-baseline (followup doc): Dynamic Cam's dedicated pose (see the
   * Settings struct comment). Provisional literals per the doc — a gentle
   * 3/4 tilt (~0.20 rad pitch), symmetric yaw (0), auto-fit distance. Same
   * step convention as their free-cam counterparts just below. */
  { "diorama_dyncam_baseline_tilt_y_mrad", NULL, "Dynamic baseline yaw",
    "Set the Dynamic Cam's resting yaw in milliradians. The camera sways around this angle.",
    kSettingType_Int, kApply_Passive, kSettingCat_DioramaCamera,
    &g_settings.diorama_dyncam_baseline_tilt_y_mrad, 0, -700, 700, 20, false,
    NULL, 0, DioramaDynamicCameraAvailable, NULL, NULL, NULL },
  { "diorama_dyncam_baseline_tilt_x_mrad", NULL, "Dynamic baseline pitch",
    "Set the Dynamic Cam's resting pitch in milliradians. The camera sways around this "
    "angle.",
    kSettingType_Int, kApply_Passive, kSettingCat_DioramaCamera,
    &g_settings.diorama_dyncam_baseline_tilt_x_mrad, 200, -700, 700, 25, false,
    NULL, 0, DioramaDynamicCameraAvailable, NULL, NULL, NULL },
  { "diorama_dyncam_baseline_distance_x100", NULL, "Dynamic baseline distance",
    "Set the Dynamic Cam's resting distance in hundredths. 0 fits the frame automatically.",
    kSettingType_Int, kApply_Passive, kSettingCat_DioramaCamera,
    &g_settings.diorama_dyncam_baseline_distance_x100, 0, 0, 2000, 25, false,
    NULL, 0, DioramaDynamicCameraAvailable, NULL, NULL, NULL },
  INT_SETTING(diorama_reactive_strength, NULL, "Reactive strength",
              "Set how strongly Dynamic Cam sways with gameplay. 0 keeps it at its resting "
              "position.",
              kSettingCat_DioramaCamera, 35, 0, 100, NULL,
              DioramaDynamicCameraAvailable),
  /* Promotes BG2 to an enveloping dimmed+DoF'd skybox so
   * camera tilt/yaw/zoom never reveals the void past the finite backdrop
   * quad's edges. Off keeps the default presentation; see DioramaSkyMode
   * (settings.h) for what the other two modes do. */
  { "diorama_skybox", NULL, "Skybox",
    "Off keeps BG2 on a scrolling plane, which can leave gaps at the edges. Skybox only "
    "fills the background. Plane + skybox keeps the scrolling plane and fills its gaps.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Presentation,
    &g_settings.diorama_skybox, kDioramaSky_Off,
    kDioramaSky_Off, kDioramaSky_Both, 1, false,
    kDioramaSkyModeLabels, kDioramaSky_Count, Diorama_ModeIsOn, NULL,
    NULL, NULL },
    BOOL_SETTING_MODERN(
        death_heim_completion_world, "AR_DEATH_HEIM_COMPLETION_WORLD", "Death Heim completion scene",
        "Show a sunlit ocean when you clear Death Heim. Requires Diorama or Voxel 3D. The "
        "scene sets its own skybox and enclosure.",
        kSettingCat_Presentation, 1, false, CompletionSceneAvailable, NULL),
    COMPLETION_EFFECT_SETTING(death_heim_completion_waves, "AR_DEATH_HEIM_COMPLETION_WAVES",
                        "Ocean waves", "Animate the completion ocean's waves and spray."),
    COMPLETION_EFFECT_SETTING(death_heim_completion_sun_glints, "AR_DEATH_HEIM_COMPLETION_SUN_GLINTS",
                        "Sun glint sparkles",
                        "Scatter brief sunlight twinkles across the completion ocean, with smaller "
                        "glints near the horizon."),
    COMPLETION_EFFECT_SETTING(death_heim_completion_pixel_water, "AR_DEATH_HEIM_COMPLETION_PIXEL_WATER",
                        "Pixel water detail",
                        "Add subtle animated 16-bit water texture beneath the ocean lighting."),
    COMPLETION_EFFECT_SETTING(death_heim_completion_clouds, "AR_DEATH_HEIM_COMPLETION_CLOUDS",
                        "Heavenly clouds",
                        "Draw the luminous cloud arch and foreground wisps around the opening."),
    COMPLETION_EFFECT_SETTING(death_heim_completion_light_shafts, "AR_DEATH_HEIM_COMPLETION_RAYS",
                        "Light shafts", "Scatter light from the opening toward the hero and ocean."),
    COMPLETION_EFFECT_SETTING(death_heim_completion_cherubs, "AR_DEATH_HEIM_COMPLETION_CHERUBS",
                        "Cherubs", "Animate distant angel silhouettes within the cloud opening."),
    COMPLETION_EFFECT_SETTING(death_heim_completion_feathers, "AR_DEATH_HEIM_COMPLETION_FEATHERS",
                        "Floating feathers",
                        "Let a few softly lit pixel feathers drift down from the opening."),
    COMPLETION_EFFECT_SETTING(
        death_heim_completion_platform_details, "AR_DEATH_HEIM_COMPLETION_PLATFORM",
        "Platform water effects",
        "Add gentle ripples, a broken reflection and light-catching spray around the platform."),
    COMPLETION_EFFECT_SETTING(
        death_heim_completion_rim_light, "AR_DEATH_HEIM_COMPLETION_RIM", "Golden rim light",
        "Add restrained warm highlights to the hero and platform's illuminated edges."),
  /* One switch for all three parts of the widescreen margin fix,
   * so the black wedge at a level bound can be A/B'd live: stand at the level
   * start and toggle. Default on; Off restores every pre-fix path byte for
   * byte, which is what makes it a usable comparison rather than a
   * half-migration. */
  { "diorama_margin_fix", "AR_DIORAMA_MARGIN_FIX", "Edge margin fix",
    "Fill gaps at the start and end of an action stage with the scene backdrop. This also "
    "keeps the skybox inside the valid BG2 area.",
    kSettingType_Bool, kApply_Passive, kSettingCat_Presentation,
    &g_settings.diorama_margin_fix, 1, 0, 1, 1, false,
    NULL, 0, Diorama_ModeIsOn, NULL,
    NULL, NULL },
  /* B6 (followup doc): floor/ceiling/side-wall enclosure masking the box's
   * off-screen edges. Composes with B5 (skybox fills the far opening);
   * independent so each can be A/B'd alone. */
  BOOL_SETTING(diorama_shoebox, NULL, "Shoebox walls",
               "Add a floor, ceiling, and walls to hide the edges of the diorama's layers.",
               kSettingCat_Presentation, 0, false, Diorama_ModeIsOn, NULL),
  /* M5 (followup doc): these three were INT_SETTING, which hardcodes
   * step=1 — an arrow press moved the camera by 1 mrad / 0.01x, ~1400
   * presses to traverse the tilt range. Written as full descriptor literals
   * instead (same pattern as hud_scale_percent/menu_scale_percent above)
   * so each press jumps by a coarse, usable step. */
  /* step=20, not 25: NormalizeLong rounds to minval + k*step, and the
   * default -180 is only grid-aligned there (-180 - -700 = 520 = 20*26;
   * 520 isn't a multiple of 25, so a step of 25 would snap the untouched
   * default to -200 the first time anything round-trips it). */
  { "diorama_tilt_y_mrad", NULL, "Camera yaw",
    "Turn the diorama camera left or right, in milliradians. Negative values bring the left "
    "edge closer.",
    kSettingType_Int, kApply_Passive, kSettingCat_DioramaCamera,
    &g_settings.diorama_tilt_y_mrad, -180, -700, 700, 20, false, NULL, 0,
    DioramaFreeCameraAvailable, NULL, NULL, NULL },
  { "diorama_tilt_x_mrad", NULL, "Camera pitch",
    "Tilt the diorama camera up or down, in milliradians. Use 0 to line sprites up with "
    "their platforms.",
    kSettingType_Int, kApply_Passive, kSettingCat_DioramaCamera,
    &g_settings.diorama_tilt_x_mrad, 0, -700, 700, 25, false, NULL, 0,
    DioramaFreeCameraAvailable, NULL, NULL, NULL },
  /* M5 dead-zone note: 0 is the auto-fit sentinel and the usable minimum is
   * kDioramaDistMin (200, i.e. 2.0x) — values 1-199 are a reachable "dead
   * zone" the range alone can't exclude (0..2000 must stay contiguous to
   * cover both the sentinel and the real range). Diorama_Render enforces
   * the floor at consume time (diorama.c) so a stray value in that gap
   * renders a valid scene instead of clipping into the near plane. */
  { "diorama_distance_x100", NULL, "Camera distance",
    "Set the diorama camera distance in hundredths. 0 fits the frame to the window.",
    kSettingType_Int, kApply_Passive, kSettingCat_DioramaCamera,
    &g_settings.diorama_distance_x100, 0, 0, 2000, 25, false, NULL, 0,
    DioramaFreeCameraAvailable, NULL, NULL, NULL },
  /* Scanlines of real world revealed on EACH side of the authentic viewport,
   * the vertical counterpart of the widescreen side margins. 64 covers the
   * measured 48px camera jump while exact signed OBJ positions avoid the
   * 8-bit OAM Y ambiguity. Auto uses the same Diorama capture budget. */
  { "diorama_vertical_extend", NULL, "Vertical extend",
    "Draw extra rows above and below the scene, including with Screen ratio set to Auto. 0 "
    "keeps the original 224-line height.",
    kSettingType_Int, kApply_Passive, kSettingCat_Presentation,
    &g_settings.diorama_vertical_extend, 64, 0, 64, 4, false, NULL, 0,
    Diorama_ModeIsOn, NULL, NULL, NULL },
  INT_SETTING(diorama_depth_shade, NULL, "Depth shading",
              "Set how much darker distant layers appear.",
              kSettingCat_Presentation, 100, 0, 100, NULL, Diorama_ModeIsOn),
  /* Default off: the backdrop plane is a finite quad drawn behind everything,
   * and at a level bound the never-written margin columns read as an opaque
   * black seam over the scene. With the
   * plane hidden the skybox/clear shows through instead, which is what the
   * tilted box wants in every mode. Turn it on to get the flat backdrop
   * colour back. */
  BOOL_SETTING(diorama_layer_backdrop, NULL, "Show backdrop",
               "Show the diorama backdrop plane. Its edges can leave a dark seam at the "
               "start or end of a stage.",
               kSettingCat_Presentation, 0, false, Diorama_ModeIsOn, NULL),
  BOOL_SETTING(diorama_layer_bg2, NULL, "Show BG2",
               "Show the BG2 scrolling background in the diorama.",
               kSettingCat_Presentation, 1, false, Diorama_ModeIsOn, NULL),
  BOOL_SETTING(diorama_layer_bg1, NULL, "Show BG1",
               "Show the BG1 playfield in the diorama.",
               kSettingCat_Presentation, 1, false, Diorama_ModeIsOn, NULL),
  BOOL_SETTING(diorama_layer_obj, NULL, "Show sprites",
               "Show the sprite layer in the diorama.",
               kSettingCat_Presentation, 1, false, Diorama_ModeIsOn, NULL),
  BOOL_SETTING(diorama_layer_bg3, NULL, "Show BG3/HUD",
               "Show the BG3 HUD layer in the diorama.",
               kSettingCat_Presentation, 1, false, Diorama_ModeIsOn, NULL),
  BOOL_SETTING(diorama_hud_flat, NULL, "Flat HUD",
               "On keeps the HUD flat, scaled, and fixed to the screen. Off tilts it with "
               "the other diorama layers.",
               kSettingCat_Presentation, 1, false, Diorama_ModeIsOn, NULL),
  ACTION_SETTING("diorama_reset", kSettingAction_DioramaReset,
                 kSettingCat_DioramaCamera, Diorama_ModeIsOn, false, "Reset defaults",
                              "Return all diorama controls to their defaults."),
  /* Load-only migration alias; Refresh rate is the sole live control. */
  BOOL_SETTING(uncapped_framerate, NULL, "Uncapped framerate",
               "Legacy compatibility alias for Refresh rate > Uncapped.",
               kSettingCat_Graphics, 0, false, NULL,
               OnLegacyUncappedChanged),
  BOOL_SETTING(action_effect_lighting, "AR_ACTION_EFFECT_LIGHTING",
               "Action spell lighting",
               "Light up the area around spells cast in action stages.",
               kSettingCat_Graphics, 1, false,
               ActionEffectRendererAvailable, NULL),
  BOOL_SETTING(action_effect_particles, "AR_ACTION_EFFECT_PARTICLES",
               "Action spell particles",
               "Add particles that follow spell effects in action stages.",
               kSettingCat_Graphics, 1, false,
               ActionEffectRendererAvailable, NULL),
  BOOL_SETTING(action_environmental_effects, "AR_ACTION_ENVIRONMENTAL_EFFECTS",
               "Environmental effects",
               "Add lighting and particles to action-stage scenery. Spell effects have "
               "separate controls.",
               kSettingCat_Graphics, 1, false,
               ActionEffectRendererAvailable, NULL),
  /* Load/save compatibility for configurations that exposed the old optional
   * backend switch. host_video.c forces this value on because SIM3D depth is a
   * baseline renderer capability; the effect rows remain independently live. */
  { "gpu_shaders_enabled", "AR_GPU_SHADERS", "GPU shader effects",
    "Kept for older settings files. The GPU renderer is required. Each effect has its own "
    "switch.",
    kSettingType_Bool, kApply_Restart, kSettingCat_Graphics,
    &g_settings.gpu_shaders_enabled, 0, 0, 1, 1, false, NULL, 0,
    NULL, NULL, NULL, NULL },
  BOOL_SETTING(gpu_fx_rim, "AR_GPU_FX_RIM", "Rim lighting",
               "Add a warm glow around sprite edges in the diorama.",
               kSettingCat_Graphics, 1, false, GpuShadersActive, NULL),
  BOOL_SETTING(gpu_fx_dof, "AR_GPU_FX_DOF", "Depth of field",
               "Blur layers farther from the main playfield in the diorama.",
               kSettingCat_Graphics, 1, false, GpuShadersActive, NULL),
  BOOL_SETTING(gpu_fx_edgeaa, "AR_GPU_FX_EDGEAA", "Edge anti-aliasing",
               "Smooth the edges of tilted diorama layers with a one-pixel border.",
               kSettingCat_Graphics, 1, false, GpuShadersActive, NULL),
  BOOL_SETTING(gpu_fx_shadow, "AR_GPU_FX_SHADOW", "Soft shadow blur",
               "Soften shadows in the diorama. This can leave a haze in transparent areas, "
               "including the sky. Off by default.",
               kSettingCat_Graphics, 0, false, GpuShadersActive, NULL),
  /* One existing opt-in owns the replacement frame-generation path. */
  BOOL_SETTING(gpu_interp_enabled, "AR_INTERP_ENABLE",
               "Frame interpolation",
               "Generate extra frames between game frames for smoother diorama motion on "
               "displays above 60 Hz.",
               kSettingCat_Graphics, 0, false, Diorama_ModeIsOn, NULL),
  { "gpu_interp_source_rate", "AR_INTERP_SOURCE_RATE",
    "Interpolation source test",
    "Native keeps gameplay at 60 Hz. Test 30 -> 60 Hz slows diorama gameplay to 30 Hz so "
    "you can see the generated frames on a 60 Hz display with Vsync.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Graphics,
    &g_settings.gpu_interp_source_rate, kInterpolationSource_Native,
    kInterpolationSource_Native, kInterpolationSource_Test30, 1, false,
    kInterpolationSourceLabels, kInterpolationSource_Count,
    FrameInterpolationActive, NULL, NULL, NULL, .modern_env = true },
  /* CRT post-process (kSettingCat_Crt). One fullscreen pass at the end of
   * presentation, so unlike the diorama-only gpu_fx_* rows above it covers
   * every render mode at once. Needs the same "gpu" backend, hence the shared
   * GpuShadersActive gate. The numeric rows below are player-facing: what a
   * tube should look like is taste, not a correct value, so each is a named
   * bounded control rather than a developer dial. Defaults are the
   * tuned-by-eye values, which is what the effect is meant to look like. */
  BOOL_SETTING(crt_enabled, "AR_CRT", "CRT effect",
               "Give every scene curved glass, scanlines, a phosphor mask, and darker "
               "corners like a colour CRT.",
               kSettingCat_Crt, 0, false, GpuShadersActive, NULL),
  INT_SETTING(crt_curvature_x100, "AR_CRT_CURVE", "Glass curvature",
              "Curve the image like CRT glass. Higher values pull in the image and darken "
              "the corners. 0 keeps the image flat.",
              kSettingCat_Crt, 45, 0, 200, NULL, GpuShadersActive),
  INT_SETTING(crt_scanline_x100, "AR_CRT_SCAN", "Scanline depth",
              "Darken the gaps between the 224 original scanlines. The line count stays the "
              "same at any window size.",
              kSettingCat_Crt, 35, 0, 100, NULL, GpuShadersActive),
  INT_SETTING(crt_mask_x100, "AR_CRT_MASK", "Phosphor mask",
              "Set how visible the red, green, and blue phosphor stripes are. The pattern "
              "follows the screen's pixels.",
              kSettingCat_Crt, 40, 0, 100, NULL, GpuShadersActive),
  INT_SETTING(crt_aberration_x100, "AR_CRT_ABERR", "Colour fringing",
              "Separate red and blue horizontally, in output pixels. Higher values make "
              "colored fringes stand out along sharp edges.",
              kSettingCat_Crt, 25, 0, 300, NULL, GpuShadersActive),
  INT_SETTING(crt_bandwidth_x100, "AR_CRT_BAND", "Signal bandwidth",
              "Blend neighboring pixels horizontally, like a composite or RF signal. This "
              "softens dithered patterns and keeps vertical detail sharp.",
              kSettingCat_Crt, 30, 0, 200, NULL, GpuShadersActive),
  INT_SETTING(crt_vignette_x100, "AR_CRT_VIG", "Corner falloff",
              "Darken the image toward the corners.",
              kSettingCat_Crt, 25, 0, 100, NULL, GpuShadersActive),
  INT_SETTING(crt_brightness_x100, "AR_CRT_BRIGHT", "Brightness",
              "Add brightness to make up for light lost to scanlines and the phosphor mask. "
              "100% adds no extra light.",
              kSettingCat_Crt, 125, 50, 300, NULL, GpuShadersActive),
  { "audio_enabled", "AR_ENABLE_AUDIO", "Enable audio",
    "Turn sound output on or off. Music and sound effects continue to advance when muted.",
    kSettingType_Bool, kApply_Callback, kSettingCat_Audio,
    &g_settings.audio_enabled, 1, 0, 1, 1, false, NULL, 0,
    NULL, NULL, NULL, NULL, .modern_env = true },
  { "audio_frequency", "AR_AUDIO_FREQ", "Audio frequency",
    "Set the audio output rate. Auto follows the audio device. Selecting 32.04 kHz uses "
    "44.1 kHz for output.",
    kSettingType_Enum, kApply_Restart, kSettingCat_Audio,
    &g_settings.audio_frequency, kAudioFrequency_Auto,
    kAudioFrequency_32040, kAudioFrequency_Auto, 1, false,
    kAudioFrequencyLabels, kAudioFrequency_Count, NULL, NULL,
    ParseAudioFrequency, NULL, .modern_env = true },
  { "audio_samples", "AR_AUDIO_SAMPLES", "Audio buffer samples",
    "Set the number of samples in the audio buffer. The change takes effect the next time "
    "the audio device opens.",
    kSettingType_Int, kApply_Restart, kSettingCat_Audio,
    &g_settings.audio_samples, 2048, 64, 8192, 1, false, NULL, 0,
    NULL, NULL, NULL, NULL, .modern_env = true },
  { "audio_master_volume", "AR_AUDIO_VOLUME", "Master volume",
    "Adjust the overall volume for music and sound effects, including MSU-1 audio.",
    kSettingType_Int, kApply_Callback, kSettingCat_Audio,
    &g_settings.audio_master_volume, 100, 0, 100, 5, false, NULL, 0,
    NULL, NULL, ParseAudioVolume, FormatAudioVolume },
  { "audio_music_volume", "AR_MUSIC_VOLUME", "Music volume",
    "Adjust both original and replacement music.",
    kSettingType_Int, kApply_Callback, kSettingCat_Audio,
    &g_settings.audio_music_volume, 100, 0, 100, 5, false, NULL, 0,
    NULL, NULL, ParseAudioVolume, FormatAudioVolume, .modern_env = true },
  { "audio_sfx_volume", "AR_SFX_VOLUME", "Sound effects volume",
    "Adjust the volume of original sound effects.",
    kSettingType_Int, kApply_Callback, kSettingCat_Audio,
    &g_settings.audio_sfx_volume, 100, 0, 100, 5, false, NULL, 0,
    NULL, NULL, ParseAudioVolume, FormatAudioVolume, .modern_env = true },
  { "audio_extended_channels", "AR_EXTENDED_AUDIO_CHANNELS",
    "Extended sound channels",
    "Keep all eight music voices and give sound effects four extra channels. A repeated "
    "sound can restart on its own channel.",
    kSettingType_Bool, kApply_Restart, kSettingCat_Audio,
    &g_settings.audio_extended_channels, 0, 0, 1, 1, false, NULL, 0,
    NULL, NULL, NULL, NULL, .modern_env = true },
  BOOL_SETTING(audio_dialog_blip, "AR_DIALOG_BLIP", "Dialogue text blip",
               "Play a short blip for each character as Sky Palace dialogue appears.",
               kSettingCat_Audio, 1, false, NULL, NULL),
  { "music_replacements", "AR_MUSIC_REPLACEMENTS", "Enhanced music",
    "Play installed replacement music when a matching OGG track is available.",
    kSettingType_Bool, kApply_Callback, kSettingCat_Audio,
    &g_settings.music_replacements, 1, 0, 1, 1, false, NULL, 0,
    NULL, NULL, NULL, NULL },

  /* --- Controls (kSettingCat_Input) --------------------------------------
   * The binding rows come in two parallel sets, keyboard and gamepad. Both
   * sets are always stored and always loaded; input_bind_page decides which
   * one the menu lists (Settings_IsMenuVisible), the same paging trick the
   * save editor uses, so the category stays readable instead of showing 34
   * rows at once. */
  { "input_device", NULL, "Input device",
    "Choose what controls the game. Auto uses the gamepad on Steam Deck. Elsewhere, Auto "
    "accepts keyboard input when the selected gamepad is idle.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Input,
    &g_settings.input_device, kInputDevice_Auto, kInputDevice_Auto,
    kInputDevice_Gamepad, 1, false, kInputDeviceLabels, kInputDevice_Count,
    NULL, NULL, NULL, NULL },
  { "input_gamepad_slot", NULL, "Gamepad",
    "Choose which controller to use. First connected follows controllers as they connect or "
    "disconnect.",
    kSettingType_Int, kApply_Passive, kSettingCat_Input,
    &g_settings.input_gamepad_slot, 0, 0, 8, 1, false, NULL, 0,
    NULL, NULL, ParseGamepadSlot, FormatGamepadSlot },
  { "input_bind_page", NULL, "Configure bindings for",
    "Choose whether the rows below edit keyboard or gamepad bindings.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Input,
    &g_settings.input_bind_page, kInputClass_Keyboard, kInputClass_Keyboard,
    kInputClass_Gamepad, 1, false, kInputClassLabels, kInputClass_Count,
    NULL, NULL, NULL, NULL, .menu_hidden = true },
  BOOL_SETTING(input_stick_as_dpad, NULL, "Left stick as D-Pad",
               "Use the left stick as well as the D-Pad for directions. Useful on Steam "
               "Deck.",
               kSettingCat_Input, 1, false, NULL, NULL),
  INT_SETTING(input_stick_deadzone, NULL, "Stick deadzone",
              "Set how far you must move the left stick before it counts as a D-Pad "
              "direction, as a percentage of full travel.",
              kSettingCat_Input,
              kInputStickDeadzoneDefaultPercent,
              kInputStickDeadzoneMinimumPercent,
              kInputStickDeadzoneMaximumPercent, NULL, NULL),
  BOOL_SETTING(input_analog_angel, NULL, "Analog angel movement",
               "Move the angel with the left stick's direction and distance. Hold fire to "
               "keep facing the same way and release it to turn. Movement follows the map "
               "axes. Recordings and replays use digital controls.",
               kSettingCat_Input, 1, false, NULL, NULL),
  INT_SETTING(input_angel_deadzone, NULL, "Angel stick deadzone",
              "Set how far you must move the left stick from its center before the angel "
              "moves, as a percentage of full travel.",
              kSettingCat_Input, kAngelDeadzoneDefaultPercent, 0,
              kAngelDeadzoneMaximumPercent, NULL, NULL),
  INT_SETTING(input_cam_sensitivity, NULL, "Camera sensitivity",
              "Set how fast the camera stick orbits and zooms in the diorama and 3D towns.",
              kSettingCat_Input, 100, 10, 400, NULL, NULL),
  INT_SETTING(input_cam_deadzone, NULL, "Camera stick deadzone",
              "Set how far you must move the camera stick before the view moves. Lower "
              "values respond to smaller nudges.",
              kSettingCat_Input,
              kInputCameraDeadzoneDefaultPercent,
              kInputCameraDeadzoneMinimumPercent,
              kInputCameraDeadzoneMaximumPercent, NULL, NULL),
  BOOL_SETTING(input_cam_invert_y, NULL, "Invert camera Y",
               "Push the camera stick up to tilt the view down.",
               kSettingCat_Input, 0, false, NULL, NULL),

  BINDING_SETTINGS(kInputAction_Up, "up", "Up"),
  BINDING_SETTINGS(kInputAction_Down, "down", "Down"),
  BINDING_SETTINGS(kInputAction_Left, "left", "Left"),
  BINDING_SETTINGS(kInputAction_Right, "right", "Right"),
  BINDING_SETTINGS(kInputAction_B, "b", "B  - jump/confirm"),
  BINDING_SETTINGS(kInputAction_A, "a", "A  - cancel"),
  BINDING_SETTINGS(kInputAction_Y, "y", "Y  - attack"),
  BINDING_SETTINGS(kInputAction_X, "x", "X  - magic"),
  BINDING_SETTINGS(kInputAction_L, "l", "L"),
  BINDING_SETTINGS(kInputAction_R, "r", "R"),
  BINDING_SETTINGS(kInputAction_Start, "start", "Start"),
  BINDING_SETTINGS(kInputAction_Select, "select", "Select"),

  /* Camera rows. These do nothing outside the diorama / 3D sim town in Free
   * Cam, which is what the availability gate below says on the row itself
   * rather than leaving a player wondering why the stick is dead. */
  BINDING_CAM_SETTINGS(kInputAction_CamYawLeft, "cam_yaw_left",
                       "Cam yaw left"),
  BINDING_CAM_SETTINGS(kInputAction_CamYawRight, "cam_yaw_right",
                       "Cam yaw right"),
  BINDING_CAM_SETTINGS(kInputAction_CamPitchUp, "cam_pitch_up",
                       "Cam pitch up"),
  BINDING_CAM_SETTINGS(kInputAction_CamPitchDown, "cam_pitch_down",
                       "Cam pitch down"),
  BINDING_CAM_SETTINGS(kInputAction_CamZoomIn, "cam_zoom_in",
                       "Cam zoom in"),
  BINDING_CAM_SETTINGS(kInputAction_CamZoomOut, "cam_zoom_out",
                       "Cam zoom out"),
  BINDING_HOST_SETTING(kInputAction_CamReset, "cam_reset", "Cam reset"),

  /* Host actions are gamepad-only rows (see input_map.h): the keyboard's
     Esc/F1, P, T, F5, and F7 hotkeys stay hard-wired so a bad rebind can
     never lock a desktop player out of the menu. On a Deck there is no
     keyboard, so the pad must be able to reach the overlay by itself. */
  BINDING_HOST_SETTING(kInputAction_Menu, "menu", "Open menu"),
  BINDING_HOST_SETTING(kInputAction_Pause, "pause", "Pause emulation"),
  BINDING_HOST_SETTING(kInputAction_Turbo, "turbo", "Fast forward"),
  BINDING_HOST_SETTING(kInputAction_SaveState, "savestate", "Save state"),
  BINDING_HOST_SETTING(kInputAction_LoadState, "loadstate", "Load state"),
  /* The one host action with a keyboard row (input_map.h): it is a debug
     control, not a way in or out of the menu, so a bad rebind costs nothing.
     Inert until the Cheats tab's "Cycle magic spell" toggle arms it. */
  BINDING_SETTING(kInputAction_MagicCycle, kInputClass_Keyboard,
                  "bind_key_magic_cycle", "Cycle magic spell",
                  "Enable the Cycle magic spell cheat to use this shortcut. Press Enter, "
                  "then press the key you want to assign."),
  BINDING_SETTING(kInputAction_MagicCycle, kInputClass_Gamepad,
                  "bind_pad_magic_cycle", "Cycle magic spell",
                  "Enable the Cycle magic spell cheat to use this shortcut. Confirm, then "
                  "press the button you want to assign."),
  BINDING_SETTING(kInputAction_RenderCompare, kInputClass_Keyboard,
                  "bind_key_render_compare", "Compare rendering",
                  "Press briefly to switch between authentic and enhanced rendering. Hold "
                  "to show or hide the small authentic view."),
  BINDING_SETTING(kInputAction_RenderCompare, kInputClass_Gamepad,
                  "bind_pad_render_compare", "Compare rendering",
                  "Press briefly to switch between authentic and enhanced rendering. Hold "
                  "to show or hide the small authentic view."),
  BINDING_SETTINGS(kInputAction_SimDescribe, "sim_describe", "Describe menu item"),
  BINDING_SHOOT_SETTINGS(kInputAction_ShootUp, "shoot_up", "Shoot up"),
  BINDING_SHOOT_SETTINGS(kInputAction_ShootRight, "shoot_right", "Shoot right"),
  BINDING_SHOOT_SETTINGS(kInputAction_ShootDown, "shoot_down", "Shoot down"),
  BINDING_SHOOT_SETTINGS(kInputAction_ShootLeft, "shoot_left", "Shoot left"),

  /* This is an optional gameplay enhancement rather than a bug fix: the
   * original 128-record structure cap is authentic. Completed bridges move
   * to a checksummed extension area so they stop consuming those records,
   * while the census and redraw hooks preserve their support and tiles. */
  BOOL_SETTING(show_debug_settings, "AR_SHOW_DEBUG_SETTINGS",
               "Show debug settings",
               "Show advanced controls for diorama and 3D town rendering, along with the "
               "scene inspector and other developer tools.",
               kSettingCat_Extras, 0, false, NULL, NULL),
  { .key = "ar2_master", .env = "AR_AR2_MASTER", .label = "ActRaiser 2 Master",
    .tooltip = "Switch to the ActRaiser 2 Master mid-level to glide, dive, and use his "
               "sword moves. First import your ActRaiser 2 (USA) ROM in Workshop > Sights & "
               "sounds > Game imports.",
    .type = kSettingType_Bool, .apply = kApply_Passive,
    .category = kSettingCat_Enhancements, .field = &g_settings.ar2_master,
    .defval = 0, .maxval = 1, .step = 1, .available = Ar2MasterAvailable,
    .modern_env = true, .game_change_kind = kSettingGameChange_QualityOfLife,
    .hide_when_unavailable = true },
  GAME_CHANGE_BOOL_SETTING(
      native_menu_quick_use, "AR_NATIVE_MENU_QUICK_USE", "Native menu quick use",
      "In the Original town menu, Use skips optional explanations. Describe menu item runs "
      "the full command flow with explanations. Set the shortcut under Controls.",
      0, false, kSettingGameChange_QualityOfLife),
  GAME_CHANGE_BOOL_SETTING(
      fix_bridge_limit, "AR_FIX_BRIDGE_LIMIT", "Bridge-free limit",
      "Free up space for other buildings by excluding completed bridges from a town's "
      "128-structure limit. The bridges still work as usual.",
      0, true, kSettingGameChange_QualityOfLife),
  GAME_CHANGE_BOOL_SETTING(
      remember_last_town, "AR_REMEMBER_LAST_TOWN", "Remember last town",
      "Continue puts the Sky Palace above the last town you visited in this save. Save game "
      "progress separately. This is disabled during recording or replay.",
      1, false, kSettingGameChange_QualityOfLife),
  GAME_CHANGE_BOOL_SETTING(
      fix_aitos_event_queue, "AR_FIX_AITOS_EVENT_QUEUE",
      "Correct Aitos messages",
      "Keep Aitos from repeating the all-monsters-defeated message when the northeast "
      "mountain discovery overlaps it. Off uses the original behavior.",
      0, false, kSettingGameChange_OriginalBugFix),
  GAME_CHANGE_BOOL_SETTING(
      fix_windmill_wind_stop, "AR_FIX_WINDMILL_WIND_STOP",
      "Wind stops every windmill",
      "Stop all Aitos windmills during the no-wind event, including newly built ones. The "
      "Wind miracle starts them again. Applies to the enhanced town view.",
      1, false, kSettingGameChange_OriginalBugFix),
  GAME_CHANGE_INT_SETTING(
      turbo_multiplier, "AR_TURBO_MULT", "Turbo multiplier",
      "Set how many game frames advance per drawn frame during turbo.",
      8, 2, 64, ParseTurboMultiplier, kSettingGameChange_QualityOfLife),
  { "warp_target", "AR_WARP", "Warp target",
    "Enter the hexadecimal destination for Warp now. The manual lists verified region and "
    "map values.",
    kSettingType_Custom, kApply_Passive, kSettingCat_Extras,
    &g_settings.warp_target, 0x0101, 0, 0xffff, 1, false, NULL, 0,
    NULL, NULL, ParseWarpTarget, FormatWarpTarget, .menu_hidden = true },
  { .key = "death_heim_roster", .env = "AR_DEATH_HEIM_ROSTER", .label = "Death Heim boss roster",
    .tooltip = "Standard uses six Act 2 bosses. Act boss swap uses six Act 1 bosses. "
               "Extended fights Act 1 then Act 2 per town. The final boss comes last. "
               "Locked during a rush or recording/replay.",
    .type = kSettingType_Enum, .apply = kApply_Passive, .category = kSettingCat_Enhancements,
    .field = &g_settings.death_heim_roster, .defval = kActRaiserDeathHeimRoster_Standard,
    .maxval = kActRaiserDeathHeimRoster_Extended, .step = 1,
    .enum_labels = kDeathHeimRosterLabels, .enum_count = 3,
    .parse = ParseDeathHeimRoster, .serialize = SerializeDeathHeimRoster,
    .modern_env = true, .game_change_kind = kSettingGameChange_QualityOfLife,
    .can_change = DeathHeimCanChange,
    .value_available = DeathHeimRosterAvailable, .prepare_change = DeathHeimPrepareChange },
  { .key = "death_heim_shuffle_order", .env = "AR_DEATH_HEIM_SHUFFLE_ORDER",
    .label = "Shuffle boss order",
    .tooltip = "Shuffle Death Heim’s bosses without repeats. The final boss stays last. The "
               "same roster and seed give the same order on retries.",
    .type = kSettingType_Bool, .apply = kApply_Passive, .category = kSettingCat_RandoBosses,
    .field = &g_settings.death_heim_shuffle_order, .maxval = 1, .step = 1,
    .modern_env = true, .can_change = DeathHeimCanChange,
    .prepare_change = DeathHeimPrepareChange },
  { .key = "death_heim_seed", .env = "AR_DEATH_HEIM_SEED", .label = "Boss order seed",
    .tooltip = "Set the seed for Death Heim's boss order before entering. Your campaign "
               "saves this seed separately from the enemy and item randomizer.",
    .type = kSettingType_Int, .apply = kApply_Passive, .category = kSettingCat_RandoBosses,
    .field = &g_settings.death_heim_seed, .defval = 1,
    .maxval = kActRaiserDeathHeimRushSeedMax, .step = 1, .modern_env = true,
    .can_change = DeathHeimCanChange, .prepare_change = DeathHeimPrepareChange },
  /* ---- Randomizer title draft (src/randomizer/randomizer.c). Confirmed campaigns bind
   * the full recipe, rather than reading mutable per-install settings. */
  { "rando_start", NULL, "Start new randomized game...", "Start a new campaign in an empty "
                                                         "slot using this randomizer setup.",
    kSettingType_Action, kApply_Action, kSettingCat_RandoSeed,
    NULL, 0, 0, 0, 0, false, NULL, 0, NULL, NULL, NULL, NULL,
    .action = kSettingAction_NewRandomizedGame },
  { "rando_enable", "AR_RANDO", "Randomizer",
    "Choose what to randomize before starting a new game. Continue restores that campaign's "
    "seed and rules. Return to the title screen to set up a different run.",
    kSettingType_Bool, kApply_Passive, kSettingCat_RandoSeed,
    &g_settings.rando_enable, 0, 0, 1, 1, false, NULL, 0,
    RandoRuntimeAvailable, Randomizer_DraftSettingChanged, NULL, NULL,
    .can_change = Randomizer_CanEditDraft },
  { "rando_seed", "AR_RANDO_SEED", "Seed",
    "Set the number that drives randomization. The same seed, options, and starting "
    "regional rules give the same setup. Continue restores the saved seed.",
    kSettingType_Int, kApply_Passive, kSettingCat_RandoSeed,
    &g_settings.rando_seed, 1, 0, 999999999, 1, false, NULL, 0,
    RandoAvailable, Randomizer_DraftSettingChanged, NULL, NULL,
    .can_change = Randomizer_CanEditDraft },
  { "rando_reroll", NULL, "New seed",
    "Pick a fresh seed for the next new game. Existing saves keep their seeds.",
    kSettingType_Action, kApply_Action, kSettingCat_RandoSeed,
    NULL, 0, 0, 0, 0, false, NULL, 0, RandoAvailable, NULL, NULL, NULL,
    .action = kSettingAction_Reroll },

  { "rando_regional_action", "AR_RANDO_REGIONAL_ACTION", "Regional action rules",
    "Pick each action rule from the US, Japanese, or European version when you start a "
    "game. Enemy health and damage multipliers apply afterward. Difficulty, title access, "
    "controls, and art stay as selected.",
    kSettingType_Bool, kApply_Passive, kSettingCat_RandoSeed,
    &g_settings.rando_regional_action, 0, 0, 1, 1, false, NULL, 0,
    RandoAvailable, Randomizer_DraftSettingChanged, NULL, NULL,
    .can_change = Randomizer_CanEditDraft },
  { "rando_regional_towns", "AR_RANDO_REGIONAL_TOWNS", "Regional town rules",
    "Pick regional town rules at random when you start a new game, including construction, "
    "lairs, and miracles. Continue keeps the rules for the towns you've already started.",
    kSettingType_Bool, kApply_Passive, kSettingCat_RandoSeed,
    &g_settings.rando_regional_towns, 0, 0, 1, 1, false, NULL, 0,
    RandoAvailable, Randomizer_DraftSettingChanged, NULL, NULL,
    .can_change = Randomizer_CanEditDraft },

  { "rando_enemy_hp", "AR_RANDO_ENEMY_HP", "Enemy health",
    "Scale enemy health when an enemy spawns, before difficulty adjustments. Some bosses "
    "set their own health later.",
    kSettingType_Int, kApply_Passive, kSettingCat_RandoEnemies,
    &g_settings.rando_enemy_hp, 100, 10, 1000, 1, false, NULL, 0,
    RandoAvailable, Randomizer_DraftSettingChanged, NULL, NULL,
    .can_change = Randomizer_CanEditDraft },
  { "rando_enemy_atk", "AR_RANDO_ENEMY_ATK", "Enemy damage",
    "Scale enemy contact and attack damage when an enemy spawns, before difficulty "
    "adjustments. Terrain damage has its own rules.",
    kSettingType_Int, kApply_Passive, kSettingCat_RandoEnemies,
    &g_settings.rando_enemy_atk, 100, 10, 1000, 1, false, NULL, 0,
    RandoAvailable, Randomizer_DraftSettingChanged, NULL, NULL,
    .can_change = Randomizer_CanEditDraft },
  { "rando_enemy_types", "AR_RANDO_ENEMY_TYPES", "Enemy types",
    "Shuffle which regular enemy appears at each position. Bosses and item statues keep "
    "their places.",
    kSettingType_Enum, kApply_Passive, kSettingCat_RandoEnemies,
    &g_settings.rando_enemy_types, kRandomMode_Off, kRandomMode_Off,
    kRandomMode_Shuffle, 1, false, kRandoShuffleLabels, 2,
    RandoAvailable, Randomizer_DraftSettingChanged, NULL, NULL,
    .can_change = Randomizer_CanEditDraft },
  { "rando_enemy_scope", "AR_RANDO_ENEMY_SCOPE", "Enemy shuffle range",
    "Shuffle enemies within each map or across a whole act. Maps in the same act share "
    "animations, so moved enemies still display correctly.",
    kSettingType_Enum, kApply_Passive, kSettingCat_RandoEnemies,
    &g_settings.rando_enemy_scope, kRandomScope_Act, kRandomScope_Map,
    kRandomScope_Act, 1, false, kRandoScopeLabels, kRandomScope_Count,
    RandoEnemyTypesOn, Randomizer_DraftSettingChanged, NULL, NULL,
    .can_change = Randomizer_CanEditDraft },

  { "rando_statue_drops", "AR_RANDO_DROPS", "Statue drops",
    "Shuffle moves existing drops among a map's breakable statues. Random picks a fresh "
    "drop for each statue.",
    kSettingType_Enum, kApply_Passive, kSettingCat_RandoItems,
    &g_settings.rando_statue_drops, kRandomMode_Off, kRandomMode_Off,
    kRandomMode_Random, 1, false, kRandoModeLabels, kRandomMode_Count,
    RandoAvailable, Randomizer_DraftSettingChanged, NULL, NULL,
    .can_change = Randomizer_CanEditDraft },
  { "rando_statue_spots", "AR_RANDO_STATUES", "Statue placement",
    "Swap breakable statues among their existing positions within each map.",
    kSettingType_Enum, kApply_Passive, kSettingCat_RandoItems,
    &g_settings.rando_statue_spots, kRandomMode_Off, kRandomMode_Off,
    kRandomMode_Shuffle, 1, false, kRandoShuffleLabels, 2,
    RandoAvailable, Randomizer_DraftSettingChanged, NULL, NULL,
    .can_change = Randomizer_CanEditDraft },

  { "rando_lair_spots", "AR_RANDO_LAIRS", "Lair positions",
    "Swap a town's four monster lairs among their existing positions.",
    kSettingType_Enum, kApply_Passive, kSettingCat_RandoSim,
    &g_settings.rando_lair_spots, kRandomMode_Off, kRandomMode_Off,
    kRandomMode_Shuffle, 1, false, kRandoShuffleLabels, 2,
    RandoAvailable, Randomizer_DraftSettingChanged, NULL, NULL,
    .can_change = Randomizer_CanEditDraft },
  { "rando_lair_types", "AR_RANDO_LAIR_TYPES", "Lair monsters",
    "Shuffle swaps existing monster types across all lairs. Random picks one of the four "
    "types for each lair.",
    kSettingType_Enum, kApply_Passive, kSettingCat_RandoSim,
    &g_settings.rando_lair_types, kRandomMode_Off, kRandomMode_Off,
    kRandomMode_Random, 1, false, kRandoModeLabels, kRandomMode_Count,
    RandoAvailable, Randomizer_DraftSettingChanged, NULL, NULL,
    .can_change = Randomizer_CanEditDraft },

  BOOL_SETTING(scene_inspector, "AR_SCENE_INSPECTOR", "Scene inspector",
               "Click the game view to pause and inspect tiles, sprites, palettes, memory "
               "addresses, and asset rules.",
               kSettingCat_Inspector, 0, false, NULL, NULL),
  ACTION_SETTING("dump_scene_assets", kSettingAction_DumpSceneAssets,
                 kSettingCat_Inspector, NULL, false, "Dump scene assets",
      "Export scene tilemaps, sprite art, palettes, and graphics memory to the current "
      "run's diagnostic folder as images and data files."),
  /* The in-game manual. Its own section rather than a row under System > Tools:
   * it is something a player reaches for, not a host command, and the reader it
   * opens is a full mode rather than a toggle. */
  ACTION_SETTING("manual_open", kSettingAction_Manual,
                 kSettingCat_Manual, NULL, false, "Read the manual",
      "Read the scanned manual. Use arrows or shoulder buttons to turn pages, + or - to "
      "zoom, and Escape to return."),
  BOOL_SETTING(manual_spreads, NULL, "Two-page spreads",
               "Show facing pages together so artwork across the center stays whole. Turn "
               "this off to read one page at a time.",
               kSettingCat_Manual, true, false, NULL, NULL),
  ACTION_SETTING("toggle_pause", kSettingAction_TogglePause,
                 kSettingCat_Extras, NULL, false, "Pause / resume",
                 "Pause or resume the game when you close the overlay."),
  ACTION_SETTING("toggle_turbo", kSettingAction_ToggleTurbo,
                 kSettingCat_Extras, NULL, false, "Toggle turbo",
                 "Turn fast-forward on or off. Turbo multiplier sets its speed."),
  ACTION_SETTING("save_state", kSettingAction_SaveState,
                 kSettingCat_Extras, NULL, true, "Save state",
                 "Capture a memory and hardware snapshot for debugging. Use an in-game save "
                 "to keep playable progress."),
  ACTION_SETTING("load_state", kSettingAction_LoadState,
                 kSettingCat_Extras, NULL, true, "Load state",
                 "Debug snapshot restore is unavailable. Use Continue to load an in-game "
                 "save."),
  ACTION_SETTING("warp_now", kSettingAction_Warp,
                 kSettingCat_Extras, NULL, true, "Warp now",
                 "Go to the destination set in Warp target using the game's transition."),
  ACTION_SETTING("take_snapshot", kSettingAction_Snapshot,
                 kSettingCat_Extras, NULL, false, "Take snapshot",
                 "Save a diagnostic snapshot of the current image, game memory, palettes, "
                 "and sprite data."),
  ACTION_SETTING("restart_game", kSettingAction_Restart,
                 kSettingCat_Extras, NULL, false, "Restart game",
                 "Save settings and cartridge data, then restart the game in this window."),
  { .key = "retry_imported_level", .label = "Retry level",
    .tooltip = "Start this level again from its entrance.",
    .type = kSettingType_Action, .apply = kApply_Action, .category = kSettingCat_Extras,
    .action = kSettingAction_RetryImportedLevel, .available = ImportedLevelActive,
    .hide_when_unavailable = true },
  { .key = "return_imported_world", .label = "Return to world",
    .tooltip = "Leave this level and return to the palace on the globe.",
    .type = kSettingType_Action, .apply = kApply_Action, .category = kSettingCat_Extras,
    .action = kSettingAction_ReturnImportedWorld, .available = ImportedLevelActive,
    .hide_when_unavailable = true },
  ACTION_SETTING("exit_desktop", kSettingAction_Exit,
                 kSettingCat_Extras, NULL, false, "Exit to desktop",
                 "Save settings and cartridge data, then close the game."),
  { "save_slots", NULL, "Slots", "Manage campaigns and start a new game in an empty slot.",
    kSettingType_Action, kApply_Action, kSettingCat_Save,
    NULL, 0, 0, 0, 0, false, NULL, 0, NULL, NULL, NULL, NULL,
    .action = kSettingAction_SaveSlots },
  { "save_backend", "AR_SAVE_BACKEND", "New slot format",
    "Choose the format for new slots. Existing slots keep their format. External diagnostic "
    "saves use the selected format after restart.",
    kSettingType_Enum, kApply_Restart, kSettingCat_Save,
    &g_settings.save_backend, 0, 0, 1, 1, false,
    kSaveBackendLabels, 2, NULL, NULL, ParseSaveBackend, NULL },
  BOOL_SETTING(save_edit_armed, "AR_SAVE_EDIT", "Allow save edits",
               "Allow edits to an external diagnostic save. Applies to the Apply actions "
               "and any changes staged for the next launch.",
               kSettingCat_Save, 0, false, NULL, NULL),
  BOOL_SETTING(save_autobackup, "AR_SAVE_BACKUP", "Auto-backup",
               "Back up the active campaign to saves/backups before the first edit that "
               "writes to disk or replaces the slot.",
               kSettingCat_Save, 1, false, NULL, NULL),
  { "save_editor_page", NULL, "Edit section",
    "Choose a page of save editor controls. The overlay uses the tabs to switch pages.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Save,
    &g_settings.save_editor_page, kSaveEditorPage_Actions,
    kSaveEditorPage_Actions, kSaveEditorPage_Count - 1, 1, false,
    kSaveEditorPageLabels, kSaveEditorPage_Count, NULL, NULL, NULL, NULL, .menu_hidden = true },
  SAVE_PROGRESS_SETTING(0, "save_prog_fillmore", "AR_SAVE_PROG_FILLMORE",
                        "Fillmore State", "Choose which act is available or cleared for "
                                          "this town in the save."),
  SAVE_PROGRESS_SETTING(1, "save_prog_bloodpool", "AR_SAVE_PROG_BLOODPOOL",
                        "Bloodpool State", "Choose which act is available or cleared for "
                                           "this town in the save."),
  SAVE_PROGRESS_SETTING(2, "save_prog_kasandora", "AR_SAVE_PROG_KASANDORA",
                        "Kasandora State", "Choose which act is available or cleared for "
                                           "this town in the save."),
  SAVE_PROGRESS_SETTING(3, "save_prog_aitos", "AR_SAVE_PROG_AITOS",
                        "Aitos State", "Choose which act is available or cleared for this "
                                       "town in the save."),
  SAVE_PROGRESS_SETTING(4, "save_prog_marahna", "AR_SAVE_PROG_MARAHNA",
                        "Marahna State", "Choose which act is available or cleared for this "
                                         "town in the save."),
  SAVE_PROGRESS_SETTING(5, "save_prog_northwall", "AR_SAVE_PROG_NORTHWALL",
                        "Northwall State", "Choose which act is available or cleared for "
                                           "this town in the save."),
  SAVE_ENUM_FIELD(kSaveEditorPage_Progress, &g_settings.save_death_heim_state,
                  "save_death_heim_state", "Death Heim State",
                  "Choose whether the saved Death Heim state is locked, unlocked, or "
                  "cleared.",
                  kSaveDeathHeimLabels),
  SAVE_ENUM_FIELD(kSaveEditorPage_Progress, &g_settings.save_professional_mode,
                  "save_professional_mode", "Professional Mode",
                  "Choose whether Professional mode is unlocked on the title screen.",
                  kSaveProfessionalLabels),
  { "save_player_name", NULL, "Player Name",
    "Set the saved player name. Use 1 to 8 printable characters.",
    kSettingType_Custom, kApply_Save, kSettingCat_Save,
    g_settings.save_player_name, 0, 0, 0, 0, false,
    NULL, 0, NULL, NULL, ParseSavePlayerName, FormatSavePlayerName,
    .save_page = kSaveEditorPage_Status },
  SAVE_STAGE_DIRECT(save_master_level, "save_master_level", "Master Level",
                    "Set the Master's saved level from 1 to 17.", 17),
  SAVE_STAGE_DIRECT(save_master_hp, "save_master_hp", "Master HP",
                    "Set the Master's saved health from 1 to 24.", 24),
  SAVE_STAGE_ZERO(save_master_mp, "save_master_mp", "Master MP",
                  "Set the Master's saved magic scroll count from 0 to 10.", 10),
  SAVE_STAGE_DIRECT(save_lives, "save_lives", "Lives",
                    "Set the saved life count shown on screen, from 1 to 9.", 9),
  SAVE_STAGE_ZERO(save_angel_sp_current, "save_angel_sp_current",
                  "Angel Current SP", "Set the angel's current miracle points (SP) from 0 "
                                      "to 999.", 999),
  SAVE_STAGE_ZERO(save_angel_sp_max, "save_angel_sp_max",
                  "Angel Maximum SP", "Set the angel's maximum miracle points (SP) from 0 "
                                      "to 999.", 999),
  SAVE_STAGE_ZERO(save_angel_hp_current, "save_angel_hp_current",
                  "Angel Current HP", "Set the angel's current health from 0 to 24.", 24),
  SAVE_STAGE_DIRECT(save_angel_hp_max, "save_angel_hp_max",
                    "Angel Maximum HP", "Set the angel's maximum health from 1 to 24.", 24),
  SAVE_STAGE_ZERO(save_message_speed, "save_message_speed", "Message Speed",
                  "Set the saved dialogue speed from 0 to 9.", 9),
  SAVE_ENUM_FIELD(kSaveEditorPage_Magic, &g_settings.save_equipped_magic,
                  "save_equipped_magic", "Equipped Magic",
                  "Choose the active spell from the ones in your magic slots. Choose None "
                  "to unequip it.",
                  kSaveEquippedMagicLabels),
  SAVE_ENUM_FIELD(kSaveEditorPage_Magic, &g_settings.save_magic_slots[0],
                  "save_magic_slot_1", "Magic Slot 1",
                  "Choose a spell for this magic slot, or Empty to clear it.",
                  kSaveMagicSlotLabels),
  SAVE_ENUM_FIELD(kSaveEditorPage_Magic, &g_settings.save_magic_slots[1],
                  "save_magic_slot_2", "Magic Slot 2",
                  "Choose a spell for this magic slot, or Empty to clear it.",
                  kSaveMagicSlotLabels),
  SAVE_ENUM_FIELD(kSaveEditorPage_Magic, &g_settings.save_magic_slots[2],
                  "save_magic_slot_3", "Magic Slot 3",
                  "Choose a spell for this magic slot, or Empty to clear it.",
                  kSaveMagicSlotLabels),
  SAVE_ENUM_FIELD(kSaveEditorPage_Magic, &g_settings.save_magic_slots[3],
                  "save_magic_slot_4", "Magic Slot 4",
                  "Choose a spell for this magic slot, or Empty to clear it.",
                  kSaveMagicSlotLabels),
  SAVE_ENUM_FIELD(kSaveEditorPage_Items, &g_settings.save_item_slots[0],
                  "save_item_slot_1", "Item Slot 1",
                  "Choose an item for this inventory slot, or Empty to clear it.", kSaveItemLabels),
  SAVE_ENUM_FIELD(kSaveEditorPage_Items, &g_settings.save_item_slots[1],
                  "save_item_slot_2", "Item Slot 2",
                  "Choose an item for this inventory slot, or Empty to clear it.", kSaveItemLabels),
  SAVE_ENUM_FIELD(kSaveEditorPage_Items, &g_settings.save_item_slots[2],
                  "save_item_slot_3", "Item Slot 3",
                  "Choose an item for this inventory slot, or Empty to clear it.", kSaveItemLabels),
  SAVE_ENUM_FIELD(kSaveEditorPage_Items, &g_settings.save_item_slots[3],
                  "save_item_slot_4", "Item Slot 4",
                  "Choose an item for this inventory slot, or Empty to clear it.", kSaveItemLabels),
  SAVE_ENUM_FIELD(kSaveEditorPage_Items, &g_settings.save_item_slots[4],
                  "save_item_slot_5", "Item Slot 5",
                  "Choose an item for this inventory slot, or Empty to clear it.", kSaveItemLabels),
  SAVE_ENUM_FIELD(kSaveEditorPage_Items, &g_settings.save_item_slots[5],
                  "save_item_slot_6", "Item Slot 6",
                  "Choose an item for this inventory slot, or Empty to clear it.", kSaveItemLabels),
  SAVE_ENUM_FIELD(kSaveEditorPage_Items, &g_settings.save_item_slots[6],
                  "save_item_slot_7", "Item Slot 7",
                  "Choose an item for this inventory slot, or Empty to clear it.", kSaveItemLabels),
  SAVE_ENUM_FIELD(kSaveEditorPage_Items, &g_settings.save_item_slots[7],
                  "save_item_slot_8", "Item Slot 8",
                  "Choose an item for this inventory slot, or Empty to clear it.", kSaveItemLabels),
  SAVE_SCORE_FIELD(0, 0, "save_score_fillmore_1", "Fillmore Act 1"),
  SAVE_SCORE_FIELD(0, 1, "save_score_fillmore_2", "Fillmore Act 2"),
  SAVE_SCORE_FIELD(1, 0, "save_score_bloodpool_1", "Bloodpool Act 1"),
  SAVE_SCORE_FIELD(1, 1, "save_score_bloodpool_2", "Bloodpool Act 2"),
  SAVE_SCORE_FIELD(2, 0, "save_score_kasandora_1", "Kasandora Act 1"),
  SAVE_SCORE_FIELD(2, 1, "save_score_kasandora_2", "Kasandora Act 2"),
  SAVE_SCORE_FIELD(3, 0, "save_score_aitos_1", "Aitos Act 1"),
  SAVE_SCORE_FIELD(3, 1, "save_score_aitos_2", "Aitos Act 2"),
  SAVE_SCORE_FIELD(4, 0, "save_score_marahna_1", "Marahna Act 1"),
  SAVE_SCORE_FIELD(4, 1, "save_score_marahna_2", "Marahna Act 2"),
  SAVE_SCORE_FIELD(5, 0, "save_score_northwall_1", "Northwall Act 1"),
  SAVE_SCORE_FIELD(5, 1, "save_score_northwall_2", "Northwall Act 2"),
  ACTION_SETTING("save_apply_session", kSettingAction_SaveApplySession,
                 kSettingCat_Save, NULL, false, "Apply for session",
                      "Apply edits in memory for this session. To load them, return to the "
                      "title screen through normal play and choose Continue. Restart "
                      "discards session edits."),
  ACTION_SETTING("save_apply_persist", kSettingAction_SaveApplyPersist,
                 kSettingCat_Save, NULL, false, "Apply and save",
                      "Write edits to the save file, with a backup if enabled. Choose "
                      "Restart game, then Continue to load them."),
  ACTION_SETTING("save_apply_restart", kSettingAction_SaveApplyRestart,
                 kSettingCat_Save, NULL, false, "Apply and restart",
                 "Save your edits and restart in this window. Choose Continue to load the "
                 "changes."),
  ACTION_SETTING("save_import", kSettingAction_SaveImport,
                 kSettingCat_Save, NULL, false, "Import save",
                      "Pick a campaign (.arsave), SRAM (.srm), or INI file. After you "
                      "confirm, it replaces the active slot and restarts the game."),
  ACTION_SETTING("save_export_campaign", kSettingAction_SaveExportCampaign,
                 kSettingCat_Save, NULL, false, "Export campaign",
                      "Export saved progress, campaign settings, and the player name to a "
                      "new .arsave file. Save in-game first to include your latest "
                      "progress."),
  ACTION_SETTING("save_export_srm", kSettingAction_SaveExportSrm,
                 kSettingCat_Save, NULL, false, "Export raw SRAM",
                      "Export cartridge data to a new .srm file for emulators. Use Export "
                      "campaign to include campaign settings and the enhanced player name."),
  ACTION_SETTING("save_export_ini", kSettingAction_SaveExportIni,
                 kSettingCat_Save, NULL, false, "Export raw INI",
                      "Export cartridge data to a new readable .ini file for tools. Use "
                      "Export campaign to include campaign settings and the enhanced player "
                      "name."),
  BOOL_SETTING(cheat_all_magic, "AR_ALL_MAGIC", "All magic",
               "Unlock all four spells. The spells stay unlocked after you turn this off.",
               kSettingCat_Cheats, 0, true, NULL, NULL),
  BOOL_SETTING(cheat_ranged_sword, "AR_RANGED_SWORD", "Ranged sword",
               "Give the Master's sword a ranged projectile attack.",
               kSettingCat_Cheats, 0, false, NULL, NULL),
  INT_SETTING(cheat_inf_mp, "AR_INF_MP", "Infinite MP",
              "Keep the Master's magic scrolls at the selected count. A value of 1 refills "
              "them to 10.",
              kSettingCat_Cheats, 0, 0, 255, ParseInfMp, NULL),
  BOOL_SETTING(cheat_inf_sp, "AR_INF_SP", "Infinite SP",
               "Keep the angel's miracle points (SP) at their current maximum.",
               kSettingCat_Cheats, 0, false, NULL, NULL),
  BOOL_SETTING(cheat_angel_hp, "AR_ANGEL_HP", "Infinite angel HP",
               "Keep the angel's health at its current maximum.",
               kSettingCat_Cheats, 0, false, NULL, NULL),
  INT_SETTING(cheat_inf_hp, "AR_INF_HP", "Infinite action HP",
              "Refill action health continuously. 1 uses the highest health reached in the "
              "stage. Higher values set the exact health to maintain.",
              kSettingCat_Cheats, 0, 0, 255, ParseInfHp, NULL),
  BOOL_SETTING(cheat_freeze_timer, "AR_FREEZE_TIMER", "Freeze timer",
               "Hold the action timer steady until the boss tally starts.",
               kSettingCat_Cheats, 0, false, NULL, NULL),
  { "cheat_moonjump", "AR_MOONJUMP", "Moonjump",
    "Hold the game's normal jump button to fly upward while enabled.",
    kSettingType_Bool, kApply_Passive, kSettingCat_Cheats,
    &g_settings.cheat_moonjump, 0, 0, 1, 1, false, NULL, 0,
    NULL, NULL, ParseMoonjumpLegacy, NULL },
  INT_SETTING(cheat_moonjump_speed, "AR_MOONJUMP_SPEED", "Moonjump speed",
              "Set how many pixels the Master rises per frame while you hold Jump with "
              "Moonjump on.",
              kSettingCat_Cheats, 6, 1, 255, NULL, NULL),
  { "cheat_no_knockback", "AR_NO_KNOCKBACK", "No knockback",
    "Prevent damage, knockback, and the brief stun from a hit. The Master flashes while "
    "this cheat is active.",
    kSettingType_Bool, kApply_Passive, kSettingCat_Cheats,
    &g_settings.cheat_no_knockback, 0, 0, 1, 1, false, NULL, 0,
    NULL, NULL, NULL, NULL },
  BOOL_SETTING(cheat_magic_cycle, "AR_MAGIC_CYCLE", "Cycle magic spell",
               "Cycle through unlocked spells during action stages. Set the shortcut under "
               "Controls. This shows a cheat badge.",
               kSettingCat_Cheats, 0, false, NULL, NULL),
  { "pins", "AR_PIN", "Custom PAR pins",
    "Enter comma-separated Pro Action Replay codes in 7Exxxxvv or 7Fxxxxvv format. The game "
    "applies them every frame.",
    kSettingType_Custom, kApply_Passive, kSettingCat_Cheats,
    &g_settings.pin_count, 0, 0, 32, 1, true, NULL, 0, NULL, NULL,
    ParsePins, FormatPins },

  BOOL_SETTING(ws_action, "AR_WS_ACTION", "Wide action stages",
               "Draw action stages beyond the original 4:3 playfield in widescreen.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
  BOOL_SETTING(ws_sim, "AR_WS_SIM", "Wide simulation towns",
               "Draw more of the map in widescreen simulation towns.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
  BOOL_SETTING(ws_bgrefresh, "AR_WS_BGREFRESH", "Legacy BG margin refresh",
               "Kept for older settings files. Action-stage margins now use the current "
               "widescreen renderer.",
               kSettingCat_Widescreen, 1, false, NULL, NULL),
  BOOL_SETTING(ws_skypalace_bg, "AR_WS_SKYPALACE_BG", "Sky Palace BG repair",
               "Fill the sides of the Sky Palace backdrop with matching pillar tiles.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
  BOOL_SETTING(ws_sprites, "AR_WS_SPRITES", "Wide action sprites",
               "Draw action-stage sprites in the widescreen margins.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
  BOOL_SETTING(ws_margin_objects, "AR_WS_MARGIN_OBJECTS", "Draw margin objects",
               "Draw active action-stage objects in the widescreen margins.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
  BOOL_SETTING(ws_margin_activation, "AR_WS_MARGIN_ACTIVATION", "Activate margin objects",
               "Let action-stage objects activate when they enter the wider view.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
  BOOL_SETTING(ws_bg2_padding, "AR_WS_BG2_MIRROR", "Decorative BG2 padding",
               "Fill the sides of narrow scrolling backgrounds with the room's mirror or "
               "repeat rules.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
  BOOL_SETTING(ws_sim_sprites, "AR_WS_SIM_SPRITES", "Wide simulation sprites",
               "Draw town sprites in the wider view and keep angel projectiles active "
               "there.",
               kSettingCat_Widescreen, 1, false, NULL,
               WidescreenSettingChanged),
};

const int g_setting_desc_count =
    (int)(sizeof(g_setting_descs) / sizeof(g_setting_descs[0]));
_Static_assert(sizeof(g_setting_descs) / sizeof(g_setting_descs[0]) <=
                   kSettingsMaxDescriptors,
               "settings descriptor registry exceeds fixed boot-layer storage");

static int Settings_FindIndexByKey(const char *key) {
  if (!key) return -1;
  for (int i = 0; i < g_setting_desc_count; i++)
    if (!strcmp(g_setting_descs[i].key, key)) return i;
  return -1;
}

static int Settings_FindIndexByEnvironment(const char *env) {
  if (!env) return -1;
  for (int i = 0; i < g_setting_desc_count; i++)
    if (g_setting_descs[i].env && !strcmp(g_setting_descs[i].env, env))
      return i;
  return -1;
}

/* T2d: this was a ~60-clause chain of `desc->field != &g_settings.<row>`
 * pointer comparisons sitting a thousand lines away from the table it
 * described, so a new modern setting whose author did not think to come here
 * silently inherited the legacy AR_* parse. The answer now lives on the row
 * itself (see modern_env in settings.h). The classification is unchanged for
 * all 259 descriptors -- 198 legacy, 61 modern -- proven by diffing the probe
 * dump against the pre-refactor baseline vector. */
static bool Settings_UsesLegacyEnvironmentSyntax(const SettingDesc *desc) {
  return !desc->modern_env;
}

static bool Settings_StageConfigIndex(int index, const char *value,
                                      bool legacy_env_syntax) {
  if (index < 0 || index >= g_setting_desc_count ||
      index >= kSettingsMaxDescriptors || !value ||
      strlen(value) >= kSettingsLayerValueSize)
    return false;
  SettingsLayerValue *slot = &s_config_layer[index];
  slot->present = true;
  slot->legacy_env_syntax = legacy_env_syntax;
  memcpy(slot->text, value, strlen(value) + 1);
  return true;
}

void Settings_ClearConfigLayer(void) {
  memset(s_config_layer, 0, sizeof(s_config_layer));
}

bool Settings_StageConfigValue(const char *key, const char *value) {
  return Settings_StageConfigIndex(Settings_FindIndexByKey(key), value, false);
}

bool Settings_StageConfigEnvironment(const char *env, const char *value) {
  int index = Settings_FindIndexByEnvironment(env);
  if (index < 0) return false;
  return Settings_StageConfigIndex(
      index, value, Settings_UsesLegacyEnvironmentSyntax(&g_setting_descs[index]));
}

const SettingDesc *Settings_Find(const char *key) {
  int index = Settings_FindIndexByKey(key);
  return index >= 0 ? &g_setting_descs[index] : NULL;
}

static struct HardwareSetting {
  bool *field;
  RenderFeatureMask required;
  bool suppressed, requested;
} s_hardware_settings[] = {
  {&g_settings.sim3d_mode, kRenderFeature_Depth},
  {&g_settings.sim3d_world_navigation, kRenderFeature_Depth},
  {&g_settings.sim3d_globe_underlay, kRenderFeature_ConnectedGlobe},
  {&g_settings.gpu_fx_rim, kRenderFeature_DioramaRim},
  {&g_settings.gpu_fx_dof, kRenderFeature_DioramaBlur | kRenderFeature_DioramaDof},
  {&g_settings.gpu_fx_shadow, kRenderFeature_DioramaBlur},
  {&g_settings.crt_enabled, kRenderFeature_Crt},
  {&g_settings.sim3d_rim_light, kRenderFeature_SimRim},
  {&g_settings.sim3d_effect_lighting, kRenderFeature_Effects},
  {&g_settings.sim3d_particles, kRenderFeature_Effects},
  {&g_settings.sim3d_soft_shadows, kRenderFeature_SimSoftShadows},
  {&g_settings.action_effect_lighting, kRenderFeature_Effects},
  {&g_settings.action_effect_particles, kRenderFeature_Effects},
  {&g_settings.action_environmental_effects, kRenderFeature_Effects},
};
static bool s_hardware_known;
static RenderFeatureMask s_hardware_supported;

static struct HardwareSetting *HardwareSettingFor(const SettingDesc *desc) {
  if (!desc) return NULL;
  for (size_t i = 0; i < sizeof(s_hardware_settings) / sizeof(s_hardware_settings[0]); ++i)
    if (desc->field == s_hardware_settings[i].field) return &s_hardware_settings[i];
  return NULL;
}

const char *Settings_HardwareUnavailableReason(const SettingDesc *desc) {
  if (!s_hardware_known || !desc) return NULL;
  const struct HardwareSetting *setting = HardwareSettingFor(desc);
  const RenderFeatureMask required = desc->category == kSettingCat_Crt
      ? kRenderFeature_Crt : setting ? setting->required : 0;
  return (s_hardware_supported & required) == required ? NULL
      : "Unavailable on this graphics device (startup capability check).";
}

void Settings_ApplyRenderCapabilities(uint32_t supported) {
  s_hardware_known = true;
  s_hardware_supported = supported & kRenderFeature_All;
  for (size_t i = 0; i < sizeof(s_hardware_settings) / sizeof(s_hardware_settings[0]); ++i) {
    struct HardwareSetting *setting = &s_hardware_settings[i];
    if (setting->suppressed) *setting->field = setting->requested;
    setting->requested = *setting->field;
    setting->suppressed = (supported & setting->required) != setting->required;
    if (!setting->suppressed) continue;
    *setting->field = false;
    for (int row = 0; row < g_setting_desc_count; ++row)
      if (g_setting_descs[row].field == setting->field) {
        RuntimeLog_Printf("[graphics-capabilities] %s blocked (required=$%x prepared=$%x)\n",
            g_setting_descs[row].key, setting->required, s_hardware_supported);
        break;
      }
  }
}

bool Settings_RenderCapabilitiesRetained(uint32_t supported) {
  return !s_hardware_known || (supported & s_hardware_supported) == s_hardware_supported;
}

bool Settings_IsAvailable(const SettingDesc *desc) {
  /* Cheat values are intentionally stageable from every game state. Their
   * runtime hooks decide when an effect applies; editing is never mode-gated. */
  return desc && !Settings_HardwareUnavailableReason(desc) &&
         (desc->category == kSettingCat_Cheats ||
          !desc->available || desc->available());
}
bool Settings_IsRandomizer(const SettingDesc *desc) {
  return desc &&
      (desc->category == kSettingCat_RandoSeed || desc->category == kSettingCat_RandoEnemies ||
       desc->category == kSettingCat_RandoItems || desc->category == kSettingCat_RandoSim);
}

/* Kept in the registry so old files/env values parse without warnings and so
 * descriptor indexes stay stable, but these are not active user settings.
 * The picker easing bit remains a reserved trace/prototyping input until an
 * implementation ships. */
static bool Settings_IsLoadOnly(const SettingDesc *desc) {
  return desc &&
      (desc->field == &g_settings.ignore_aspect_ratio ||
       desc->field == &g_settings.uncapped_framerate ||
       desc->field == &g_settings.ws_bgrefresh ||
       desc->field == &g_settings.sim3d_picker_exit_ease);
}

bool Settings_ValueAvailable(const SettingDesc *desc, long value) {
  return desc && (!desc->value_available || desc->value_available(value));
}

static bool OffersSeveralValues(const SettingDesc *desc) {
  const long step = desc->step > 0 ? desc->step : 1;
  int offered = 0;
  for (long value = desc->minval; value <= Settings_Maximum(desc) && offered < 2;
       value += step)
    offered += Settings_ValueAvailable(desc, value);
  return offered >= 2;
}

bool Settings_IsMenuVisible(const SettingDesc *desc) {
  if (!desc || !desc->key) return false;
  if (Settings_IsLoadOnly(desc)) return false;
  /* A platform-dependent row with nothing to choose between is not a
   * setting on this platform. */
  if (desc->value_available && !OffersSeveralValues(desc)) return false;
  /* Developer-only rows collapse out of the menu unless explicitly enabled.
   * Checked first so a debug row is hidden regardless of its category rules. */
  if (Settings_IsDebugOnly(desc) && !g_settings.show_debug_settings)
    return false;
  if (desc->menu_hidden) return false;

  if (desc->hide_when_unavailable && !Settings_IsAvailable(desc)) return false;

  /* The Gamepad row is pointless with nothing plugged in. */
  if (desc->category == kSettingCat_Input)
    return strcmp(desc->key, "input_gamepad_slot") != 0 ||
           InputMap_GamepadCount() > 0;

  /* Bindings exist for both device classes at all times, but only the class
   * selected by input_bind_page (i.e. the active Controls tab) is listed. */
  if (desc->category == kSettingCat_InputBinds) {
    InputClass klass;
    if (!InputMap_DescribeRow(desc, NULL, &klass)) return true;
    return (int)klass == g_settings.input_bind_page;
  }

  if (desc->category != kSettingCat_Save) return true;

  return (int)desc->save_page == g_settings.save_editor_page;
}

bool Settings_GetLong(const SettingDesc *desc, long *value) {
  if (!desc || !value) return false;
  switch (desc->type) {
    case kSettingType_Bool: *value = *(const bool *)desc->field; return true;
    case kSettingType_Int:
    case kSettingType_Enum: *value = *(const int *)desc->field; return true;
    case kSettingType_Mask: *value = *(const uint16 *)desc->field; return true;
    case kSettingType_Custom:
    case kSettingType_Binding:
    case kSettingType_Action:
      return false;
  }
  return false;
}

long Settings_Maximum(const SettingDesc *desc) {
  return desc && desc->enum_maximum ? desc->enum_maximum() : desc ? desc->maxval : 0;
}

static long NormalizeLong(const SettingDesc *desc, long value) {
  if (desc->field == &g_settings.localization_presentation &&
      g_settings.localization_content != 0)
    return 1;
  if (desc->type == kSettingType_Bool) return value != 0;
  if (desc->field == &g_settings.display_mode && !g_ws_active)
    return kDisplayMode_43;
  if (value < desc->minval) value = desc->minval;
  if (value > Settings_Maximum(desc)) value = Settings_Maximum(desc);
  if (desc->step > 1)
    value = desc->minval +
            ((value - desc->minval) / desc->step) * desc->step;
  return value;
}

static SettingChangeResult FinishChange(const SettingDesc *desc,
                                        bool sticky_disable) {
  /* Apply the coupled source/rendering invariant before the runtime observer
   * sees the selection, so pack/font validation remains one transaction. */
  if (g_settings.localization_content != 0)
    g_settings.localization_presentation = 1;
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
  if (desc && desc->can_change && !desc->can_change())
    return kSettingChange_Rejected;
  long old_value;
  if (!Settings_GetLong(desc, &old_value)) return kSettingChange_Rejected;
  value = NormalizeLong(desc, value);
  struct HardwareSetting *hardware = HardwareSettingFor(desc);
  if (hardware && s_hardware_known && Settings_HardwareUnavailableReason(desc)) {
    if (value != 0) return kSettingChange_Rejected;
    hardware->requested = false;
  }
  if (old_value == value) return kSettingChange_Unchanged;
  if (desc->prepare_change && !desc->prepare_change(desc, value))
    return kSettingChange_Rejected;

  switch (desc->type) {
    case kSettingType_Bool: *(bool *)desc->field = value != 0; break;
    case kSettingType_Int:
    case kSettingType_Enum: *(int *)desc->field = (int)value; break;
    case kSettingType_Mask: *(uint16 *)desc->field = (uint16)value; break;
    case kSettingType_Custom:
    case kSettingType_Binding:
    case kSettingType_Action:
      return kSettingChange_Rejected;
  }
  return FinishChange(desc, desc->sticky && old_value != 0 && value == 0);
}

SettingChangeResult Settings_SetText(const SettingDesc *desc,
                                     const char *text) {
  if (!desc || !text) return kSettingChange_Rejected;
  if (desc->type == kSettingType_Action) return kSettingChange_Rejected;
  if (desc->type == kSettingType_Custom ||
      desc->type == kSettingType_Binding) {
    if (desc->can_change && !desc->can_change())
      return kSettingChange_Rejected;
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
    if (desc->parse) {
      int parsed = 0;
      if (!desc->parse(text, &parsed)) return kSettingChange_Rejected;
      value = parsed;
    } else {
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
    }
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
  if (desc->type == kSettingType_Action) return kSettingChange_Rejected;
  if (desc->type == kSettingType_Custom ||
      desc->type == kSettingType_Binding)
    return Settings_SetText(desc, "");
  return Settings_SetLong(desc, desc->defval);
}

SettingChangeResult Settings_ResetCategory(SettingCategory category) {
  if (category < 0 || category >= kSettingCat_Count)
    return kSettingChange_Rejected;
  SettingChangeResult aggregate = kSettingChange_Unchanged;
  for (int i = 0; i < g_setting_desc_count; i++) {
    const SettingDesc *desc = &g_setting_descs[i];
    if (desc->category != category || desc->type == kSettingType_Action)
      continue;
    SettingChangeResult result = Settings_Reset(desc);
    /* The enum is deliberately ordered by consequence for accepted changes:
     * ordinary < sticky history remains < restart required. */
    if (result > aggregate) aggregate = result;
  }
  return aggregate;
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
    case kSettingType_Binding:
      buffer[0] = 0;
      return 0;
    case kSettingType_Action:
      return snprintf(buffer, buffer_size, "RUN");
  }
  buffer[0] = 0;
  return 0;
}

void Settings_SetChangeObserver(SettingsChangeObserver observer) {
  s_change_observer = observer;
}

void Settings_SetActionObserver(SettingsActionObserver observer) {
  s_action_observer = observer;
}

bool Settings_InvokeAction(const SettingDesc *desc) {
  return desc && desc->type == kSettingType_Action &&
         s_action_observer && s_action_observer(desc);
}

const char *Settings_CategoryName(SettingCategory category) {
  switch (category) {
    case kSettingCat_Cheats: return "Cheats";
    case kSettingCat_Widescreen: return "Widescreen";
    case kSettingCat_Display: return "Display";
    case kSettingCat_Presentation: return "Diorama";
    case kSettingCat_DioramaCamera: return "Diorama camera";
    case kSettingCat_Simulation: return "Simulation";
    case kSettingCat_SimCamera: return "Town camera";
    case kSettingCat_SimLighting: return "Town lighting";
    case kSettingCat_SimAtmosphere: return "Town atmosphere";
    case kSettingCat_Graphics: return "Graphics";
    case kSettingCat_Crt: return "CRT";
    case kSettingCat_Audio: return "Audio";
    case kSettingCat_Input: return "Controls";
    case kSettingCat_InputBinds: return "Bindings";
    case kSettingCat_Save: return "Save editor";
    case kSettingCat_Extras: return "Tools";
    case kSettingCat_Enhancements: return "Game";
    case kSettingCat_Inspector: return "Inspector";
    case kSettingCat_Manual: return "Manual";
    case kSettingCat_RandoSeed: return "Seed";
    case kSettingCat_RandoEnemies: return "Enemies";
    case kSettingCat_RandoItems: return "Items";
    case kSettingCat_RandoSim: return "Simulation";
    case kSettingCat_Localization: return "Localization";
    case kSettingCat_LocalizationFont: return "Enhanced font";
    case kSettingCat_Interface: return "Interface";
    case kSettingCat_RandoBosses: return "Bosses";
    case kSettingCat_Count: break;
  }
  return "Unknown";
}

bool Settings_CategoryIsSim3D(SettingCategory category) {
  return category == kSettingCat_Simulation ||
         category == kSettingCat_SimCamera ||
         category == kSettingCat_SimLighting ||
         category == kSettingCat_SimAtmosphere;
}

bool Settings_IsDebugOnly(const SettingDesc *desc) {
  if (!desc || !desc->key) return false;
  if (desc->player_visible) return false;

  /* The scene inspector and its asset dump are development tools end to end. */
  if (desc->category == kSettingCat_Inspector) return true;

  /* The randomizer is verified against the ROM but has never been played
   * through, so it stays behind the debug flag until a seeded run has been
   * validated. Gated by category rather than by key so a row moved to another
   * randomizer tab keeps the gate. Its section is debug_only too — hiding only
   * the rows would leave an empty section in the nav column. */
  if (desc->category == kSettingCat_RandoSeed ||
      desc->category == kSettingCat_RandoEnemies ||
      desc->category == kSettingCat_RandoItems ||
      desc->category == kSettingCat_RandoSim ||
      desc->category == kSettingCat_RandoBosses)
    return true;

  /* The granular widescreen flags are all preset by Screen ratio / Render
   * profile; toggling them individually is a developer A/B, not a player
   * control. The whole Widescreen tab collapses when debug is off. */
  if (desc->category == kSettingCat_Widescreen) return true;

  /* The diorama and town 3D renderers carry dozens of fine numeric dials —
   * camera pose in milliradians, cloud ramp widths in pixels, haze/opacity
   * percentages. None are things a player tunes for performance; that is what
   * the on/off effect toggles are for. So every Int/Mask row in those
   * categories is developer-only, while the Bool effect switches, the camera
   * mode selector, and the reset action stay visible. */
  bool three_d = desc->category == kSettingCat_Presentation ||
                 desc->category == kSettingCat_DioramaCamera ||
                 Settings_CategoryIsSim3D(desc->category);
  if (three_d &&
      (desc->type == kSettingType_Int || desc->type == kSettingType_Mask))
    return true;

  /* The CRT knobs are deliberately NOT here. The 3D dials above are geometry:
   * a camera pose in milliradians has one correct value and moving it breaks
   * the projection. A tube's look has no correct value — how much curvature,
   * scanline depth or phosphor mask reads right depends on the display it is
   * shown on and on what the player remembers a CRT looking like. Each row is
   * a named, bounded, tuned-by-eye control, which is a taste setting, so they
   * ship visible under the master toggle.
   *
   * If the CRT tab ever grows a genuine internal A/B, list its key below
   * rather than re-hiding the category by type. */

  /* A few Bool rows in those same categories are internal A/B or plumbing
   * toggles rather than real user-facing effects: the separated compositor
   * stage, the cull-lift inset, the picker exit easing, the per-layer capture
   * toggles, and the flat-HUD A/B curiosity. */
  static const char *const kDebugKeys[] = {
    "sim3d_separated_composite", "sim3d_cull_lift_inset",
    /* Town 3D stages that should never be off in normal play — turning them
     * off just breaks the projection. */
    "sim3d_ground_projection", "sim3d_object_billboards", "sim3d_virtual_height",
    "diorama_layer_bg1", "diorama_layer_bg2", "diorama_layer_bg3",
    "diorama_layer_obj", "diorama_layer_backdrop", "diorama_hud_flat",
  };
  for (size_t i = 0; i < sizeof(kDebugKeys) / sizeof(kDebugKeys[0]); i++)
    if (!strcmp(desc->key, kDebugKeys[i])) return true;

  return false;
}

const char *Settings_ApplyKindName(SettingApplyKind apply) {
  switch (apply) {
    case kApply_Passive: return "Live";
    case kApply_Callback: return "Applies now";
    case kApply_Restart: return "Restart required";
    case kApply_Save: return "Staged save edit";
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

static void SetSettingDefault(const SettingDesc *desc) {
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
    case kSettingType_Binding:
      if (desc->parse) desc->parse("", desc->field);
      break;
    case kSettingType_Action:
      break;
  }
}

static bool ApplyLegacyEnvironmentValue(const SettingDesc *desc,
                                        const char *text) {
  if (!text) return false;
  if (desc->parse) {
    return desc->parse(text, desc->field);
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
  } else if (desc->type == kSettingType_Action) {
    return false;
  }
  return true;
}

static int InferDisplayMode(void) {
  if (!g_ws_active && g_settings.extended_aspect != kScreenAspect_Auto)
    return kDisplayMode_43;

  const bool raw = g_settings.ws_action && g_settings.ws_sim &&
                   !g_settings.ws_skypalace_bg &&
                   !g_settings.ws_sprites &&
                   !g_settings.ws_margin_objects &&
                   !g_settings.ws_margin_activation &&
                   !g_settings.ws_bg2_padding &&
                   !g_settings.ws_sim_sprites;
  if (raw)
    return kDisplayMode_WideRaw;

  const bool full = g_settings.ws_action && g_settings.ws_sim &&
                    g_settings.ws_skypalace_bg &&
                    g_settings.ws_sprites &&
                    g_settings.ws_margin_objects &&
                    g_settings.ws_margin_activation &&
                    g_settings.ws_bg2_padding &&
                    g_settings.ws_sim_sprites;
  return full ? kDisplayMode_WideFull : kDisplayMode_Custom;
}

static bool ParseEnumLayerValue(const SettingDesc *desc, const char *text,
                                int *value) {
  if (!desc || !text || !value || desc->type != kSettingType_Enum)
    return false;
  if (desc->parse) return desc->parse(text, value);

  char *end = NULL;
  long parsed = strtol(text, &end, 0);
  if (end && !*end) {
    if (parsed < desc->minval || parsed > desc->maxval) return false;
    *value = (int)parsed;
    return true;
  }
  for (int i = 0; i < desc->enum_count; i++) {
    if (!strcmp(text, desc->enum_labels[i])) {
      *value = i;
      return true;
    }
  }
  return false;
}

static bool ApplyBootLayerValue(const SettingDesc *desc, const char *text,
                                int rank, bool legacy_env_syntax) {
  if (!desc || !text) return false;
  if (desc->field == &g_settings.display_mode) {
    int mode = 0;
    if (!ParseEnumLayerValue(desc, text, &mode)) return false;
    s_boot_display_mode = mode;
    s_boot_display_rank = rank;
    if (legacy_env_syntax) s_boot_display_from_environment = true;
    return true;
  }

  bool ok = legacy_env_syntax
      ? ApplyLegacyEnvironmentValue(desc, text)
      : Settings_SetText(desc, text) != kSettingChange_Rejected;
  if (ok && desc->category == kSettingCat_Widescreen &&
      !Settings_IsLoadOnly(desc) &&
      rank > s_boot_widescreen_rank)
    s_boot_widescreen_rank = rank;
  return ok;
}

static bool Settings_LoadInternal(const char *path, bool boot, int rank,
                                  bool missing_ok) {
  if (!path || !path[0]) return true;
  FILE *file = sr_fopen(path, "r");
  if (!file) {
    if (!missing_ok || errno != ENOENT)
      RuntimeLog_Printf("[settings] cannot read %s: %s\n", path,
              strerror(errno));
    return missing_ok && errno == ENOENT;
  }

  bool success = true;
  bool moonjump_toggle_seen = false;
  bool legacy_moonjump_speed_seen = false;
  bool legacy_moonjump_enabled = false;
  char line[kSettingsIniLineCapacity];
  int line_number = 0;
  while (fgets(line, sizeof(line), file)) {
    line_number++;
    char *key = TextParse_TrimLeft(line);
    TextParse_TrimRight(key);
    if (!key[0] || key[0] == '#' || key[0] == ';' || key[0] == '[')
      continue;
    char *equals = strchr(key, '=');
    if (!equals) {
      RuntimeLog_Printf("[settings] %s:%d: expected key = value\n",
              path, line_number);
      success = false;
      continue;
    }
    *equals = 0;
    TextParse_TrimRight(key);
    char *value = TextParse_TrimLeft(equals + 1);
    TextParse_StripInlineComment(value);

    /* Retired controls may remain until the next save. Native nebula lighting
     * now follows environmental effects; its old flag must not override them. */
    if (!strcmp(key, "cheat_moonjump_button") ||
        !strcmp(key, "death_heim_final_battle")) continue;

    if (!strcmp(key, "cheat_moonjump")) {
      moonjump_toggle_seen = true;
    } else if (!strcmp(key, "cheat_moonjump_speed")) {
      char *end = NULL;
      long old_speed = strtol(value, &end, 0);
      if (end && !*end) {
        legacy_moonjump_speed_seen = true;
        legacy_moonjump_enabled = old_speed != 0;
      }
    }

    const SettingDesc *desc = Settings_Find(key);
    if (!desc) {
      RuntimeLog_Printf("[settings] %s:%d: unknown key '%s' ignored\n",
              path, line_number, key);
      continue;
    }
    if (desc->type == kSettingType_Action) {
      RuntimeLog_Printf("[settings] %s:%d: action key '%s' is not persistent\n",
              path, line_number, key);
      success = false;
      continue;
    }
    bool applied = boot
        ? ApplyBootLayerValue(desc, value, rank, false)
        : Settings_SetText(desc, value) != kSettingChange_Rejected;
    if (!applied) {
      RuntimeLog_Printf("[settings] %s:%d: invalid value '%s' for %s\n",
              path, line_number, value, key);
      success = false;
    }
  }
  if (ferror(file)) success = false;
  fclose(file);
  if (!moonjump_toggle_seen && legacy_moonjump_speed_seen)
    Settings_SetLong(Settings_Find("cheat_moonjump"),
                     legacy_moonjump_enabled);
  return success;
}

void Settings_InitWithFile(const char *path) {
  Settings_SetDeathHeimHost(NULL);
  SettingsChangeObserver observer = s_change_observer;
  s_change_observer = NULL;
  /* Startup owns hardware discovery. Never carry a previous initialization's
   * effective values or suppressed preferences into a freshly loaded config. */
  s_hardware_known = false;
  s_hardware_supported = 0;
  for (size_t i = 0; i < sizeof(s_hardware_settings) / sizeof(s_hardware_settings[0]); ++i) {
    s_hardware_settings[i].suppressed = false;
    s_hardware_settings[i].requested = false;
  }
  memset(&g_settings, 0, sizeof(g_settings));
  s_boot_display_rank = 0;
  s_boot_widescreen_rank = 0;
  s_boot_display_mode = kDisplayMode_WideFull;
  s_boot_display_from_environment = false;
  s_saved_window_mode = kWindowMode_Count;

  for (int i = 0; i < g_setting_desc_count; i++)
    SetSettingDefault(&g_setting_descs[i]);

  /* Preserve the private visual-fixture bootstrap as a default only. Explicit
   * config/file/environment selections (including Native) still win. */
  const char *development_pack = getenv("AR_LOCALIZATION_PACK");
  if (development_pack && development_pack[0]) {
    g_settings.localization_content = 1;
    g_settings.localization_presentation = 1;
  }

  for (int i = 0; i < g_setting_desc_count; i++) {
    if (s_config_layer[i].present)
      ApplyBootLayerValue(&g_setting_descs[i], s_config_layer[i].text, 1,
                          s_config_layer[i].legacy_env_syntax);
  }

  Settings_LoadInternal(path, true, 2, true);
  const int saved_window_mode = g_settings.window_mode;

  for (int i = 0; i < g_setting_desc_count; i++) {
    const SettingDesc *desc = &g_setting_descs[i];
    const char *text = desc->env ? getenv(desc->env) : NULL;
    if (text)
      ApplyBootLayerValue(desc, text, 3,
                          Settings_UsesLegacyEnvironmentSyntax(desc));
  }

  const char *session_window_mode = getenv("AR_SESSION_WINDOW_MODE");
  int window_mode;
  if (session_window_mode && *session_window_mode &&
      ParseEnumLayerValue(Settings_Find("window_mode"), session_window_mode,
                          &window_mode)) {
    s_saved_window_mode = saved_window_mode;
    g_settings.window_mode = window_mode;
  }

  if (g_settings.localization_content != 0)
    g_settings.localization_presentation = 1;

  /* Finalization waits until main has derived the boot framebuffer budget from
   * the resolved aspect settings. Keep a truthful placeholder in the meantime. */
  g_settings.display_mode = s_boot_display_rank
      ? s_boot_display_mode : kDisplayMode_Custom;
  s_change_observer = observer;
}

void Settings_Init(void) {
  Settings_InitWithFile(NULL);
  Settings_FinalizeDisplayMode();
}

void Settings_FinalizeDisplayMode(void) {
  /* A display preset wins over individual widescreen fields from its own or a
   * lower layer (matching legacy AR_DISPLAY_MODE semantics), but a higher-layer
   * individual field must be allowed to produce CUSTOM. */
  if (s_boot_display_rank &&
      s_boot_display_rank >= s_boot_widescreen_rank) {
    Settings_SetDisplayMode(s_boot_display_mode);
  } else {
    g_settings.display_mode = InferDisplayMode();
  }

  if (s_boot_display_from_environment) {
    RuntimeLog_Printf("[display] AR_DISPLAY_MODE=%d -> %s%s\n",
            s_boot_display_mode,
            Settings_DisplayModeName(g_settings.display_mode),
            g_settings.display_mode != s_boot_display_mode
                ? " (wide framebuffer unavailable or overridden)" : "");
  }
}

bool Settings_Load(const char *path) {
  return Settings_LoadInternal(path, false, 0, false);
}

/* Durability half of the atomic write — see the long note in
 * save_system.c's WriteAtomic. MOVEFILE_WRITE_THROUGH's documented flush
 * guarantee is worded for the cross-volume copy+delete case, and POSIX
 * fsync(2) does not cover the containing directory entry, so both platforms
 * need an explicit data flush and POSIX needs a directory fsync. */
static bool Settings_FlushFileData(FILE *file) {
#ifdef _WIN32
  HANDLE h = (HANDLE)_get_osfhandle(_fileno(file));
  if (h == INVALID_HANDLE_VALUE) return false;
  return FlushFileBuffers(h) != 0;
#else
  return fsync(fileno(file)) == 0;
#endif
}

static void Settings_SyncContainingDirectory(const char *path) {
#ifndef _WIN32
  char dir[kHostPathCapacity];
  snprintf(dir, sizeof dir, "%s", path);
  char *slash = strrchr(dir, '/');
  if (slash) *slash = '\0';
  else snprintf(dir, sizeof dir, ".");
  int fd = open(dir[0] ? dir : "/", O_RDONLY);
  if (fd < 0) return;
  (void)fsync(fd);
  close(fd);
#else
  (void)path;
#endif
}

/* Set false for diagnostic runs (see Settings_SetPersistenceEnabled). Owned
 * here rather than queried from input_replay.c so settings.c keeps no dependency
 * on the replay module -- several test targets compile settings.c without it. */
static bool s_persistence_enabled = true;
static SettingsSaveHost s_save_host;

void Settings_SetSaveHost(const SettingsSaveHost *host) {
  s_save_host = host ? *host : (SettingsSaveHost){0};
}

void Settings_SetPersistenceEnabled(bool enabled) {
  s_persistence_enabled = enabled;
}

static bool SaveSettings(const char *path, bool deferred) {
  if (!path || !path[0]) return false;
  /* A replay is a DIAGNOSTIC run and must not mutate the player's configuration
   * — the same reason InputReplay_ShouldProtectSaveData already refuses to
   * persist SRAM. Settings were not covered by that, and the gap is not
   * theoretical: with `diorama_camera_mode = Dynamic Cam` the dynamic camera
   * drifts its own baseline during a long replay, and the 500 ms dirty-flush in
   * Diorama_FlushSettingsIfDirty then wrote that drift into the user's
   * settings.ini (observed 2026-07-27: diorama_dyncam_baseline_distance_x100
   * moved 294 -> 330 purely from replaying a recording).
   *
   * Guarded here rather than at the six call sites so no future writer can miss
   * it. Reported once so a replay that expected to persist is not silently
   * confusing. */
  if (!s_persistence_enabled) {
    static bool reported;
    if (!reported) {
      reported = true;
      RuntimeLog_Printf("[settings] persistence disabled — not writing %s "
              "(diagnostic run)\n",
              path);
    }
    return true;  /* not an error: the caller's intent is satisfied */
  }
  /* Snapshot on the registry owner. Workers never follow descriptor pointers
   * into g_settings or read live hardware-suppressed preferences. */
  const size_t capacity = (size_t)g_setting_desc_count * kSettingsIniLineCapacity + 256;
  char *text = malloc(capacity);
  if (!text) return false;
  int written = snprintf(text, capacity,
      "# ActRaiser Recompiled user settings\n"
      "# Generated from the live descriptor registry; config.ini remains developer-owned.\n\n");
  size_t used = written > 0 ? (size_t)written : capacity;
  char value[512];
  for (int i = 0; used < capacity && i < g_setting_desc_count; i++) {
    const SettingDesc *desc = &g_setting_descs[i];
    if (desc->type == kSettingType_Action || Settings_IsLoadOnly(desc)) continue;
    if (desc->field == &g_settings.display_mode &&
        g_settings.display_mode == kDisplayMode_Custom) continue;
    const struct HardwareSetting *hardware = HardwareSettingFor(desc);
    /* A launcher override and any subsequent window-mode changes apply to this session. */
    if (desc->field == &g_settings.window_mode && s_saved_window_mode != kWindowMode_Count)
      snprintf(value, sizeof(value), "%s", kWindowModeLabels[s_saved_window_mode]);
    else if (hardware && hardware->suppressed)
      snprintf(value, sizeof(value), "%s", hardware->requested ? "On" : "Off");
    else if (desc->serialize) desc->serialize(value, sizeof(value), desc->field);
    else Settings_FormatValue(desc, value, sizeof(value));
    written = snprintf(text + used, capacity - used, "%s = %s\n", desc->key, value);
    if (written < 0 || (size_t)written >= capacity - used) used = capacity;
    else used += (size_t)written;
  }
  bool success = false;
  if (used < capacity) {
    if (s_save_host.submit && s_save_host.drain) {
      /* One persistence owner for both modes. A synchronous save publishes
       * the latest snapshot and waits for its durable result; it must not
       * bypass the host and leave older failure bookkeeping behind. */
      success = s_save_host.submit(s_save_host.context, path, text, used);
      if (success && !deferred) success = s_save_host.drain(s_save_host.context);
    } else {
      success = Settings_WriteSnapshot(path, text, used);
    }
  }
  free(text);
  return success;
}

bool Settings_Save(const char *path) { return SaveSettings(path, false); }
bool Settings_SaveDeferred(const char *path) { return SaveSettings(path, true); }

bool Settings_WriteSnapshot(const char *path, const char *text, size_t size) {
  if (!path || !path[0] || !text || !size) return false;
  size_t path_length = strlen(path);
  char *temporary = (char *)malloc(path_length + 5);
  if (!temporary) return false;
  memcpy(temporary, path, path_length);
  memcpy(temporary + path_length, ".tmp", 5);

  FILE *file = sr_fopen(temporary, "w");
  if (!file) {
    RuntimeLog_Printf("[settings] cannot write %s: %s\n", temporary,
            strerror(errno));
    free(temporary);
    return false;
  }

  bool success = fwrite(text, 1, size, file) == size;
  if (fflush(file) != 0 || ferror(file)) success = false;
  if (success && !Settings_FlushFileData(file)) success = false;
  if (fclose(file) != 0) success = false;

  if (success && !AtomicReplaceFile(temporary, path)) {
    RuntimeLog_Printf("[settings] cannot replace %s: %s\n", path,
            strerror(errno));
    success = false;
  }
  /* Best-effort: make the rename itself durable. */
  if (success) Settings_SyncContainingDirectory(path);
  if (!success) sr_remove(temporary);
  free(temporary);
  return success;
}

void Settings_SetDisplayMode(int mode) {
  if (mode < 0 || mode >= kDisplayMode_PresetCount) return;
  /* Auto has no drawable budget before renderer creation. Its saved profile
   * is a preference, not a statement about the current capture dimensions. */
  if (!g_ws_active && g_settings.extended_aspect != kScreenAspect_Auto &&
      mode != kDisplayMode_43)
    mode = kDisplayMode_43;
  g_settings.display_mode = mode;

  /* 4:3 clears every flag; the policy additionally forces `wide = 0` outright,
   * because scenes like the Sky Palace hub and the Mode-7 world map set wide
   * unconditionally and would otherwise ignore these. */
  bool wide = (mode != kDisplayMode_43);
  bool corrections = (mode == kDisplayMode_WideFull);

  g_settings.ws_action            = wide;
  g_settings.ws_sim               = wide;
  g_settings.ws_skypalace_bg      = corrections;
  g_settings.ws_sprites           = corrections;
  g_settings.ws_margin_objects    = corrections;
  g_settings.ws_margin_activation = corrections;
  g_settings.ws_bg2_padding       = corrections;
  g_settings.ws_sim_sprites       = corrections;
}

void Settings_ReconcileDisplayModeAfterGeometryChange(int previous_mode) {
  /* Auto changes effective scene drawing, never the retained manual profile. */
  if (g_settings.extended_aspect == kScreenAspect_Auto) return;
  if (!g_ws_active) {
    Settings_SetDisplayMode(kDisplayMode_43);
  } else if (previous_mode == kDisplayMode_43 ||
             previous_mode < kDisplayMode_43 ||
             previous_mode > kDisplayMode_Custom) {
    Settings_SetDisplayMode(kDisplayMode_WideFull);
  } else {
    /* RAW/FULL/CUSTOM already own the exact ws_* state to preserve. */
    g_settings.display_mode = previous_mode;
  }
}

int Settings_CycleDisplayMode(void) {
  /* A bespoke env/menu combination enters the comparison cycle at the
   * authentic baseline; the cycle itself always remains exactly three steps. */
  int next = (g_settings.display_mode >= 0 &&
              g_settings.display_mode < kDisplayMode_PresetCount)
                 ? (g_settings.display_mode + 1) % kDisplayMode_PresetCount
                 : kDisplayMode_43;
  /* A1 (followup doc): route through Settings_SetLong on the descriptor
   * rather than calling Settings_SetDisplayMode directly, so FinishChange
   * fires the runtime change observer (OnRuntimeSettingChanged) — the same
   * HostDisplay_RecomputeLogicalPresentation() path every other renderer-mutating settings
   * change gets. DisplayModeChanged
   * (the descriptor's on_change) still calls Settings_SetDisplayMode to set
   * the ws_* flags; no recursion, since that function writes fields
   * directly rather than going back through Settings_SetLong. */
  Settings_SetLong(Settings_Find("display_mode"), next);
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
 * authentic view is the centre 256 columns starting at g_ws_extra. When the
 * render margin exceeds the display margin (diorama mode), the visible
 * window is the centre 256+2*display_extra columns. */
int Settings_VisibleX0(void) {
  if (g_settings.extended_aspect == kScreenAspect_Auto)
    return g_ws_extra - g_ws_display_extra;
  if (g_settings.display_mode == kDisplayMode_43) return g_ws_extra;
  return g_ws_extra - g_ws_display_extra;
}

int Settings_VisibleWidth(void) {
  return (g_settings.display_mode == kDisplayMode_43 &&
          g_settings.extended_aspect != kScreenAspect_Auto)
             ? kActRaiserAuthenticWidth
             : kActRaiserAuthenticWidth + 2 * g_ws_display_extra;
}

int Settings_ExtendedAspectX(void) {
  switch (g_settings.extended_aspect) {
    case kScreenAspect_169:
    case kScreenAspect_1610:
    case kScreenAspect_Stretch:  /* wide content, then filled to the window */
      return 16;
    default:
      return 0;
  }
}

int Settings_ExtendedAspectY(void) {
  switch (g_settings.extended_aspect) {
    case kScreenAspect_169: return 9;
    case kScreenAspect_1610: return 10;
    case kScreenAspect_Stretch: return 9;
    default: return 0;
  }
}

bool Settings_IgnoreAspectRatio(void) {
  return g_settings.extended_aspect == kScreenAspect_Stretch;
}

void Settings_SetHdReplacementsAvailable(bool available) {
  s_hd_replacements_available = available;
}

/* Backing pixels per window point (SDL_GetWindowPixelDensity), pushed from
 * host_display.c whenever the window moves display or changes scale. 1.0 on a
 * non-scaled display; 2.0 on Retina; fractional under Wayland fractional
 * scaling.
 *
 * Why the pinned scale rows need it: SDL_WINDOW_HIGH_PIXEL_DENSITY makes
 * SDL_GetRenderOutputSize report PHYSICAL pixels, but hud_scale_percent and
 * menu_scale_percent are documented in SNES-pixels-per-output-pixel terms
 * ("100 means one output pixel per SNES pixel vertically"). Without scaling
 * them, a saved 200% rendered at half its former PHYSICAL size the moment the
 * flag was added, and the same saved number meant different things on a Mac
 * (density 2) and on Windows/X11 (density 1). The auto (0) rows derive from the
 * output size and self-correct; only the explicit percentages need this. */
static float s_host_pixel_density = 1.0f;
void Settings_SetHostPixelDensity(float density) {
  s_host_pixel_density = density > 0.0f ? density : 1.0f;
}
float Settings_HostPixelDensity(void) { return s_host_pixel_density; }

int Settings_ScalePercentToOutput(int percent) {
  if (percent <= 0) return percent;   /* 0 = auto; resolved from output size */
  int scaled = (int)((float)percent * s_host_pixel_density + 0.5f);
  return scaled > 0 ? scaled : 1;
}

int Settings_AudioFrequencyHz(void) {
  switch (g_settings.audio_frequency) {
    case kAudioFrequency_32040: return 32040;
    case kAudioFrequency_48000: return 48000;
    case kAudioFrequency_44100: return 44100;
    case kAudioFrequency_Auto:
    default: return 0;   /* 0 = resolve to the device's native rate at open */
  }
}
