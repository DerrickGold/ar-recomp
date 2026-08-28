#include "settings.h"
#include "text_parse_utils.h"
#include "constants.h"
#include "randomizer.h"
#include "render_capabilities.h"
#include "actraiser_game.h"   /* kActRaiserAuthenticWidth */
#include "input_map.h"
#include "atomic_replace.h"
#include "sim/sim3d_camera_limits.h"
#include "sim/sim_town_terrain.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Descriptor-backed settings grew past the original round-number reserve when
 * the five SIM D1 A/B controls landed.  This is fixed boot storage, not a
 * serialized limit; leave useful headroom for the remaining SIM stages. */
/* Sized with headroom rather than to the current table: this backs
 * s_config_layer, and Settings_InitWithFile aborts the moment the table
 * outgrows it, so a cap that tracks the count exactly turns every new setting
 * into a crash on boot. */
enum {
  kSettingsMaxDescriptors = 320,
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

/* g_ws_active / g_ws_extra are the canonical exported widescreen symbols
 * (snesrecomp/host/widescreen.h). The framebuffer width is derived from
 * g_ws_extra exactly as main.c computes it, so we don't need main.c's
 * file-local g_snes_width. */
extern bool g_ws_active;
extern int g_ws_extra;
extern int g_ws_display_extra;

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
};

static const char *const kWindowModeLabels[] = {
  "Windowed",
  "Borderless",
  "Fullscreen",
};

static const char *const kRefreshModeLabels[] = {
  "Vsync",
  "Uncapped",
  "Limit",
  "Unlimited",
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
 * 32kHz device. main.c clamps the request and logs why; the row's description
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
bool Diorama_NewPpuCapable(void) {
  return g_settings.new_renderer || g_ws_active;
}

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

static bool FrameInterpolationActive(void) {
  return Diorama_ModeIsOn() && g_settings.gpu_interp_enabled;
}

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
  return Sim3DGroundEnabled() &&
      g_settings.sim3d_voxel_preset != kSimBackgroundVoxelPreset_Off;
#else
  return false;
#endif
}
static bool Sim3DVoxelCustomEnabled(void) {
  return Sim3DGroundEnabled() &&
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
static bool WorldNavigationLightingAvailable(void) {
  return WorldNavigation3DEnabled() &&
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
      (WorldNavigationLightingAvailable() &&
       g_settings.sim3d_world_navigation_clouds);
}
static bool Sim3DOrWorldNavigationSoftShadowAvailable(void) {
  return Sim3DSoftShadowsEnabled() ||
      (WorldNavigationLightingAvailable() &&
       g_settings.sim3d_world_navigation_clouds);
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
extern bool g_gpu_shaders_active;
static bool GpuShadersActive(void) { return g_gpu_shaders_active; }
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
                  text, "Press Enter, then the key to bind. " \
                  "Y resets it to the default."), \
  BINDING_SETTING(action_enum, kInputClass_Gamepad, "bind_pad_" id_suffix, \
                  text, "Press Enter, then the button to bind. " \
                  "Y resets it to the default.")
/* Camera rows exist for both classes: the pad gets the stick, and a keyboard
 * player who prefers keys to the mouse can bind them (unbound by default). */
#define BINDING_CAM_SETTINGS(action_enum, id_suffix, text) \
  BINDING_SETTING(action_enum, kInputClass_Keyboard, "bind_key_" id_suffix, \
                  text, "Diorama / 3D town Free Cam only. Press Enter, then " \
                  "the key to bind. Y resets it to the default."), \
  BINDING_SETTING(action_enum, kInputClass_Gamepad, "bind_pad_" id_suffix, \
                  text, "Diorama / 3D town Free Cam only. Push the stick or " \
                  "press the button to bind. Y resets it to the default.")
/* Gamepad-only host action: no keyboard twin. */
#define BINDING_HOST_SETTING(action_enum, id_suffix, text) \
  BINDING_SETTING(action_enum, kInputClass_Gamepad, "bind_pad_" id_suffix, \
                  text, "Press Enter, then the button to bind. " \
                  "Y resets it to the default.")

/* ---- Randomizer support. Enum labels and availability/refresh callbacks for
 * the descriptors below; the transform itself lives in src/randomizer.c. */
static const char *const kRandoModeLabels[] = { "Off", "Shuffle", "Random" };
static const char *const kRandoShuffleLabels[] = { "Off", "Shuffle" };
static const char *const kRandoScopeLabels[] = { "Within map", "Within act" };

static bool RandoAvailable(void) {
  return Randomizer_IsAvailable() && g_settings.rando_enable;
}
static bool RandoRuntimeAvailable(void) {
  return Randomizer_IsAvailable();
}
static bool RandoEnemyTypesOn(void) {
  return RandoAvailable() && g_settings.rando_enemy_types != kRandomMode_Off;
}
/* Any randomizer row changing re-runs the whole transform. It restores the
 * pristine image first, so re-applying is idempotent and toggling a row off
 * genuinely gives back stock data. */
static void RandoChanged(const SettingDesc *desc) {
  (void)desc;
  Randomizer_Apply();
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
#define INT_SETTING(id, env_name, text, help, cat, def, lo, hi, parser, active) \
  { #id, env_name, text, help, kSettingType_Int, kApply_Passive, cat, \
    &g_settings.id, def, lo, hi, 1, false, NULL, 0, active, NULL, \
    parser, NULL }
#define ACTION_SETTING(id, text, help) \
  { id, NULL, text, help, kSettingType_Action, kApply_Action, \
    kSettingCat_Extras, NULL, 0, 0, 0, 0, false, NULL, 0, NULL, NULL, \
    NULL, NULL }
/* Both reset actions live on their mode's Camera tab, beside the pose rows
 * they restore — hence the category here rather than the mode's Scene tab. */
#define PRESENTATION_ACTION_SETTING(id, text, help) \
  { id, NULL, text, help, kSettingType_Action, kApply_Action, \
    kSettingCat_DioramaCamera, NULL, 0, 0, 0, 0, false, NULL, 0, \
    Diorama_ModeIsOn, NULL, NULL, NULL }
#define SIM_ACTION_SETTING(id, text, help) \
  { id, NULL, text, help, kSettingType_Action, kApply_Action, \
    kSettingCat_SimCamera, NULL, 0, 0, 0, 0, false, NULL, 0, \
    Sim3D_ModeIsOn, NULL, NULL, NULL }
#define INSPECTOR_ACTION_SETTING(id, text, help) \
  { id, NULL, text, help, kSettingType_Action, kApply_Action, \
    kSettingCat_Inspector, NULL, 0, 0, 0, 0, false, NULL, 0, NULL, NULL, \
    NULL, NULL }
#define MANUAL_ACTION_SETTING(id, text, help) \
  { id, NULL, text, help, kSettingType_Action, kApply_Action, \
    kSettingCat_Manual, NULL, 0, 0, 0, 0, false, NULL, 0, NULL, NULL, \
    NULL, NULL }
#define SAVE_ACTION_SETTING(id, text, help) \
  { id, NULL, text, help, kSettingType_Action, kApply_Action, \
    kSettingCat_Save, NULL, 0, 0, 0, 0, false, NULL, 0, NULL, NULL, \
    NULL, NULL }
#define SAVE_PROGRESS_SETTING(index, id, env_name, text, help) \
  { id, env_name, text, help, kSettingType_Enum, kApply_Save, \
    kSettingCat_Save, &g_settings.save_region_progress[index], \
    kSaveProgressEdit_LeaveAsIs, kSaveProgressEdit_LeaveAsIs, \
    kSaveProgressEdit_Act2Cleared, 1, false, kSaveProgressEditLabels, \
    kSaveProgressEdit_Count, NULL, NULL, ParseSaveProgressEdit, NULL }
#define SAVE_STAGE_DIRECT(field_name, id, text, help, maximum) \
  { id, NULL, text, help, kSettingType_Int, kApply_Save, \
    kSettingCat_Save, &g_settings.field_name, 0, 0, maximum, 1, false, \
    NULL, 0, NULL, NULL, ParseStagedDirect, FormatStagedDirect }
#define SAVE_STAGE_ZERO(field_name, id, text, help, maximum) \
  { id, NULL, text, help, kSettingType_Int, kApply_Save, \
    kSettingCat_Save, &g_settings.field_name, 0, 0, (maximum) + 1, 1, false, \
    NULL, 0, NULL, NULL, ParseStagedZeroBased, FormatStagedZeroBased }
#define SAVE_ENUM_FIELD(field_ptr, id, text, help, labels) \
  { id, NULL, text, help, kSettingType_Enum, kApply_Save, \
    kSettingCat_Save, field_ptr, 0, 0, \
    (int)(sizeof(labels) / sizeof((labels)[0])) - 1, 1, false, labels, \
    (int)(sizeof(labels) / sizeof((labels)[0])), NULL, NULL, NULL, NULL }
#define SAVE_SCORE_FIELD(region, act, id, text) \
  { id, NULL, text, "Stage the saved BCD score (0-99990 by 10).", \
    kSettingType_Int, kApply_Save, kSettingCat_Save, \
    &g_settings.save_scores[region][act], 0, 0, 10000, 1, false, \
    NULL, 0, NULL, NULL, ParseStagedScore, FormatStagedScore }

const SettingDesc g_setting_descs[] = {
  { "display_mode", "AR_DISPLAY_MODE", "Render profile",
    "Switch between authentic 4:3, uncorrected wide output, and full HLE widescreen.",
    kSettingType_Enum, kApply_Callback, kSettingCat_Display,
    &g_settings.display_mode, kDisplayMode_WideFull, kDisplayMode_43,
    kDisplayMode_WideFull, 1, false, kDisplayModeLabels,
    kDisplayMode_PresetCount, WidescreenActive, DisplayModeChanged, NULL, NULL },
  { "hud_scale_percent", "AR_HUD_SCALE", "HUD output scale",
    "Scale the promoted HUD after game upscaling; 100 is native 1x output "
    "pixels. Set it anywhere; it takes effect where the HUD shows.",
    kSettingType_Int, kApply_Passive, kSettingCat_Display,
    &g_settings.hud_scale_percent, 0, 0, 400, 25, false, NULL, 0,
    NULL, NULL, ParseHudScale, FormatHudScale },
  { "menu_scale_percent", "AR_MENU_SCALE", "Menu output scale",
    "Scale host menu contents independently; Auto fits them to the window.",
    kSettingType_Int, kApply_Passive, kSettingCat_Display,
    &g_settings.menu_scale_percent, 0, 0, 800, 25, false, NULL, 0,
    NULL, NULL, ParseMenuScale, FormatMenuScale },
  BOOL_SETTING(hd_replacements, "AR_HD_REPLACEMENTS", "HD replacements",
               "Substitute HD art per game-assets/manifest.ini entries when their art is present.",
               kSettingCat_Display, 1, false, HdReplacementsAvailable, NULL),
  { "extended_aspect", "AR_EXTENDED_ASPECT_RATIO", "Screen ratio",
    "Output aspect: authentic 4:3, widescreen 16:9 / 16:10, or Stretch to "
    "fill the whole window. Video geometry updates live.",
    kSettingType_Enum, kApply_Callback, kSettingCat_Display,
    &g_settings.extended_aspect, kScreenAspect_43,
    kScreenAspect_43, kScreenAspect_Stretch, 1, false,
    kScreenAspectLabels, kScreenAspect_Count, NULL, OnScreenRatioChanged,
    ParseExtendedAspect, NULL, .modern_env = true },
  { "pixel_aspect", "AR_ASPECT_PAR", "Pixel aspect",
    "Use the original 4:3 CRT pixel stretch or square output pixels.",
    kSettingType_Enum, kApply_Callback, kSettingCat_Display,
    &g_settings.pixel_aspect, kPixelAspect_Crt43,
    kPixelAspect_Square, kPixelAspect_Crt43, 1, false,
    kPixelAspectLabels, kPixelAspect_Count, NULL, NULL,
    ParsePixelAspect, NULL, .modern_env = true },
  { "window_mode", "AR_WINDOW_MODE", "Window mode",
    "Windowed, borderless desktop-fullscreen, or exclusive fullscreen.",
    kSettingType_Enum, kApply_Callback, kSettingCat_Display,
    &g_settings.window_mode, kWindowMode_Windowed,
    kWindowMode_Windowed, kWindowMode_Exclusive, 1, false,
    kWindowModeLabels, kWindowMode_Count, NULL, NULL, NULL, NULL, .modern_env = true },
  { "window_scale", "AR_WINDOW_SCALE", "Window scale",
    "Size of the window as a multiple of the game output. This only changes "
    "the window in Windowed mode; fullscreen uses the display's resolution.",
    kSettingType_Int, kApply_Callback, kSettingCat_Display,
    &g_settings.window_scale, 3, 1, 8, 1, false, NULL, 0,
    WindowScaleAvailable, NULL, NULL, NULL, .modern_env = true },
  { "new_renderer", "AR_NEW_RENDERER", "New renderer",
    "Use the modern PPU renderer; widescreen always requires this renderer.",
    kSettingType_Bool, kApply_Callback, kSettingCat_Display,
    &g_settings.new_renderer, 1, 0, 1, 1, false, NULL, 0,
    NULL, NULL, NULL, NULL, .modern_env = true },
  { "ignore_aspect_ratio", "AR_IGNORE_ASPECT_RATIO", "Stretch to window",
    "Legacy compatibility alias for Screen ratio > Stretch.",
    kSettingType_Bool, kApply_Callback, kSettingCat_Display,
    &g_settings.ignore_aspect_ratio, 0, 0, 1, 1, false, NULL, 0,
    NULL, OnLegacyStretchChanged, NULL, NULL, .modern_env = true },
  { "refresh_mode", "AR_REFRESH_MODE", "Refresh rate",
    "Vsync delegates presentation timing to the renderer; Uncapped disables "
    "vsync and soft-caps at twice the display's nominal refresh; Limit uses "
    "a chosen FPS; Unlimited removes all presentation throttling.",
    kSettingType_Enum, kApply_Callback, kSettingCat_Display,
    &g_settings.refresh_mode, kRefreshMode_Vsync,
    kRefreshMode_Vsync, kRefreshMode_Unlimited, 1, false,
    kRefreshModeLabels, kRefreshMode_Count, NULL, NULL, NULL, NULL, .modern_env = true },
  { "frame_limit_fps", "AR_FRAME_LIMIT_FPS", "Frame limit",
    "Target frames per second when Refresh rate is Limit; independent of the "
    "display refresh.",
    kSettingType_Int, kApply_Callback, kSettingCat_Display,
    &g_settings.frame_limit_fps, 60, 20, 480, 5, false, NULL, 0,
    FrameLimitActive, NULL, NULL, NULL, .modern_env = true },
  BOOL_SETTING_MODERN(show_fps, "AR_SHOW_FPS", "FPS counter",
               "Show completed host presents per second in the top-right. Use "
               "Refresh rate: Unlimited to measure maximum rendering throughput.",
               kSettingCat_Display, 0, false, NULL, NULL),
  BOOL_SETTING_MODERN(sim3d_mode, "AR_SIM3D", "Simulation town 3D",
               "Tilt the simulation-town map into a projected ground plane. "
               "Map pickers stay in the tilted space too (build with "
               "AR_SIM3D_PICKER_TOPDOWN=1 to restore the flat picker view).",
               kSettingCat_Simulation, 0, false, Diorama_NewPpuCapable,
               NULL),
  { "sim_view_range", "AR_SIM_VIEW_RANGE", "Extended actor range",
    "Gameplay-affecting. Keep simulation actors and projectiles alive and "
    "host-renderable this many original pixels beyond the authentic view. "
    "Larger values increase pressure on the game's fixed 44 world-record "
    "slots; 0 preserves authentic lifetime.",
    kSettingType_Int, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim_view_range, 0, 0, 256, 16, false, NULL, 0,
    Sim3D_ModeIsOn, NULL, NULL, NULL,
    .modern_env = true, .player_visible = true },
  BOOL_SETTING_MODERN(sim3d_world_navigation, "AR_SIM3D_WORLD_NAV",
               "World navigation 3D",
               "Render inter-town Sky Palace navigation as a full-world 3D "
               "scene with a forced top-down camera.",
               kSettingCat_Simulation, 0, false, Diorama_NewPpuCapable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_world_navigation_lighting,
               "AR_SIM3D_WORLD_NAV_LIGHTING",
               "World navigation lighting",
               "Apply top-down colour treatment and directional cloud shadows "
               "to the inter-town world. Uses the shared light controls and "
               "does not require Simulation town 3D.",
               kSettingCat_Simulation, 1, false,
               WorldNavigation3DEnabled, NULL),
  BOOL_SETTING_MODERN(sim3d_world_navigation_clouds,
               "AR_SIM3D_WORLD_NAV_CLOUDS",
               "World navigation clouds",
               "Draw world-anchored cloud banks over the full inter-town map. "
               "The authentic zoom controls whether the camera is above the "
               "cloud deck: bodies are visible while zoomed out and fade "
               "away as the Palace descends below them.",
               kSettingCat_Simulation, 0, false,
               WorldNavigation3DEnabled, NULL),
  /* The enhanced renderer, stage by stage. Each is an ordinary toggle so a
   * stage can be turned on or off by name; `kSim3DShippedFeatures` is the one
   * list of stages with a shipped implementation, and the defaults here must
   * agree with it. A stage missing from both is invisible in normal play no
   * matter how it is tuned. */
  BOOL_SETTING_MODERN(sim3d_separated_composite, "AR_SIM3D_SEPARATED",
               "Separated layer capture",
               "Rebuild the town from captured semantic layers and the object "
               "atlas instead of the authentic composite. Every other stage "
               "below depends on this one.",
               kSettingCat_Simulation, 1, false, Sim3DSeparatedAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_ground_projection, "AR_SIM3D_GROUND",
               "Ground projection",
               "Project the map through the oblique camera instead of drawing "
               "it flat.",
               kSettingCat_Simulation, 1, false, Sim3DGroundAvailable,
               NULL),
  { "sim3d_voxel_preset", "AR_SIM3D_VOXEL_PRESET", "Voxel town quality",
    "Off preserves the original background tiles. Performance uses compact "
    "models and basic lighting. Balanced adds authored architecture, AO and "
    "pixel-clean edges. Quality enables the complete geometry, palette and "
    "Smooth 2x treatment. Custom exposes every control below without presets "
    "overwriting those stored values.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_voxel_preset, kSimBackgroundVoxelPreset_Balanced,
    kSimBackgroundVoxelPreset_Off, kSimBackgroundVoxelPreset_Custom, 1,
    false, kSimVoxelPresetLabels, kSimBackgroundVoxelPreset_Count,
    Sim3DGroundEnabled, NULL, NULL, NULL,
    .modern_env = true, .player_visible = true },
  { "sim3d_landscape_height_pct", "AR_SIM3D_LANDSCAPE_HEIGHT",
    "Landscape height (%)",
    "Scale the audited town relief independently from buildings and flying "
    "objects. 100 keeps the full landscape, 50 gives half-height hills, and "
    "0 makes the audited surface flat. Flying objects keep their authored "
    "clearance above the resulting landscape.",
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
    "Performance target for simulation-town buildings and trees. Low uses "
    "compact silhouettes for dense maps; Balanced is the conservative model; "
    "High adds finer roofs and facade relief; Ultra enables the complete "
    "voxel detail set.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_voxel_detail, kSimBackgroundVoxelDetail_High,
    kSimBackgroundVoxelDetail_Low, kSimBackgroundVoxelDetail_Ultra, 1, false,
    kSimVoxelDetailLabels, kSimBackgroundVoxelDetail_Count,
    Sim3DVoxelCustomEnabled, NULL, NULL, NULL,
    .modern_env = true, .player_visible = true },
  { "sim3d_voxel_lod", "AR_SIM3D_VOXEL_LOD", "Voxel detail scaling",
    "Fixed uses the selected model detail everywhere. Adaptive treats model "
    "detail as an upper limit and reuses cheaper cached meshes for buildings "
    "and trees whose projected silhouettes are too small to show the extra "
    "geometry.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_voxel_lod, kSimBackgroundVoxelLod_Adaptive,
    kSimBackgroundVoxelLod_Fixed, kSimBackgroundVoxelLod_Adaptive, 1, false,
    kSimVoxelLodLabels, kSimBackgroundVoxelLod_Count,
    Sim3DVoxelCustomEnabled, NULL, NULL, NULL,
    .modern_env = true, .player_visible = true },
  { "sim3d_voxel_shading", "AR_SIM3D_VOXEL_SHADING", "Voxel shading",
    "Lighting cost for simulation-town buildings and trees. Basic uses one "
    "directional value per face; Ambient occlusion adds contact gradients; "
    "Materials + AO also gives foliage, masonry, roofs and metal distinct "
    "light responses.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_voxel_shading,
    kSimBackgroundVoxelShading_MaterialAware,
    kSimBackgroundVoxelShading_Basic,
    kSimBackgroundVoxelShading_MaterialAware, 1, false,
    kSimVoxelShadingLabels, kSimBackgroundVoxelShading_Count,
    Sim3DVoxelCustomEnabled, NULL, NULL, NULL,
    .modern_env = true, .player_visible = true },
  { "sim3d_voxel_style", "AR_SIM3D_VOXEL_STYLE", "Voxel styling",
    "Geometry family for authored background models. Basic preserves clean "
    "silhouettes; Silhouette trim adds eaves and broad facade accents; "
    "Architectural adds factory courtyard treatment; Varied adds "
    "deterministic model variations without animation.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_voxel_style, kSimBackgroundVoxelStyle_Varied,
    kSimBackgroundVoxelStyle_Basic, kSimBackgroundVoxelStyle_Varied, 1,
    false, kSimVoxelStyleLabels, kSimBackgroundVoxelStyle_Count,
    Sim3DVoxelCustomEnabled, NULL, NULL, NULL,
    .modern_env = true, .player_visible = true },
  { "sim3d_voxel_facing", "AR_SIM3D_VOXEL_FACING", "Voxel camera facing",
    "Shared lean tilts every model equally toward the camera. Per-model lean "
    "keeps houses and windmills upright while preserving more roof and crown "
    "depth on factories and trees. Both paths retain real 3D volume.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_voxel_facing, kSimBackgroundVoxelFacing_PerModel,
    kSimBackgroundVoxelFacing_Shared, kSimBackgroundVoxelFacing_PerModel, 1,
    false, kSimVoxelFacingLabels, kSimBackgroundVoxelFacing_Count,
    Sim3DVoxelCustomEnabled, NULL, NULL, NULL,
    .modern_env = true, .player_visible = true },
  { "sim3d_voxel_render_scale", "AR_SIM3D_VOXEL_RENDER_SCALE",
    "Voxel render scale",
    "Native draws directly into the scene. Pixel-clean locks projected "
    "corners to the output pixel grid for crisp voxel edges. Smooth 2x "
    "renders the background voxel pass at double resolution and downsamples "
    "it on High or Ultra detail; Low and Balanced remain native, and large "
    "output windows automatically stay native to bound GPU memory and fill "
    "cost.",
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
               "Draw world records as individually placed sprites standing on "
               "the projected ground.",
               kSettingCat_Simulation, 1, false, Sim3DBillboardsAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_virtual_height, "AR_SIM3D_HEIGHT",
               "Object heights",
               "Lift flying actors and effects onto their classified height "
               "above the map. Needs object billboards.",
               kSettingCat_Simulation, 1, false, Sim3DVirtualHeightAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_shadows, "AR_SIM3D_SHADOWS", "Ground shadows",
               "Cast per-object shadows onto the ground only. Needs object "
               "billboards; darkness is set by Shadow darkness below.",
               kSettingCat_Simulation, 1, false, Sim3DShadowsAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_soft_shadows, "AR_SIM3D_SOFT_SHADOWS",
               "Soft shadows",
               "Blur the ground shadow mask instead of leaving a hard "
               "silhouette edge. Needs ground shadows; radius is set by "
               "Shadow softness.",
               kSettingCat_Simulation, 1, false, Sim3DSoftShadowsAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_rim_light, "AR_SIM3D_RIM_LIGHT", "Rim light",
               "Add a lit edge to billboard silhouettes on the side facing "
               "the light. Needs object billboards; brightness is set by Rim "
               "light strength.",
               kSettingCat_Simulation, 1, false, Sim3DRimLightAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_effect_lighting, "AR_SIM3D_EFFECT_LIGHTING",
               "Effect lighting",
               "Add transient local illumination when simulation miracles "
               "and enemy attacks emit light. Uses portable additive SDL "
               "geometry and does not require GPU shader effects.",
               kSettingCat_Simulation, 1, false,
               Sim3DEffectLightingAvailable, NULL),
  BOOL_SETTING_MODERN(sim3d_particles, "AR_SIM3D_PARTICLES", "Effect particles",
               "Add deterministic host particles synchronized to simulation "
               "miracles and enemy attacks. Uses the same captured effect "
               "lifecycle as Effect lighting.",
               kSettingCat_Simulation, 1, false,
               Sim3DParticlesAvailable, NULL),
  BOOL_SETTING_MODERN(sim3d_world_underlay, "AR_SIM3D_WORLD_UNDERLAY",
               "World map underlay",
               "Extend the ground past the town edge with the live world "
               "map, so the neighbouring regions are visible instead of "
               "empty space. Needs the ground projection; distance fade is "
               "set by World map haze.",
               kSettingCat_Simulation, 1, false, Sim3DWorldUnderlayAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_cloud_shroud, "AR_SIM3D_CLOUDS", "Cloud shroud",
               "Cover the extended ground with drifting cloud banks. Sprites "
               "can only be drawn near the camera, so the far ground is always "
               "actor-free; the clouds read that as distance instead of as an "
               "empty town. Needs the world map underlay.",
               kSettingCat_Simulation, 1, false, Sim3DCloudShroudAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_cull_haze, "AR_SIM3D_CULL_HAZE_STAGE",
               "Local-area haze",
               "Fade the town ground into the distant world map outside the "
               "sprite-drawable window, so the bright area reads as where "
               "actors can be rather than as clouds having gaps. Unlike the "
               "shroud this is continuous. During 3D world navigation the "
               "same stage keeps the ROM-selected location clear and hazes "
               "the other regions.",
               kSettingCat_Simulation, 1, false,
               Sim3DOrWorldNavigationHazeAvailable, NULL),
  { "sim3d_camera_mode", "AR_SIM3D_CAMERA_MODE", "Camera mode",
    "Free Cam: manual orbit and zoom with the right mouse button, and the "
    "pose persists. Dynamic Cam: its own baseline pose, leaning toward the "
    "angel's direction of travel and jolting when the angel is hit. Switching "
    "restores that mode's own camera rather than carrying the other one over.",
    kSettingType_Enum, kApply_Passive, kSettingCat_SimCamera,
    &g_settings.sim3d_camera_mode, kSimCam_Dynamic,
    kSimCam_Free, kSimCam_Dynamic, 1, false,
    kSimCamModeLabels, kSimCam_Count, Sim3DGroundEnabled, NULL,
    NULL, NULL, .modern_env = true },
  /* Dynamic Cam's dedicated pose. Defaults are the captured baseline (see the
   * sim3d defaults note above), so the shipped Dynamic view is the one that
   * was actually tuned rather than whatever Free Cam was last left at. */
  { "sim3d_dyncam_baseline_tilt_x_mrad", NULL, "Dynamic baseline pitch",
    "Dynamic Cam's resting pitch in milliradians. The supported range runs "
    "from the authored three-quarter view toward a low near-horizontal view.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimCamera,
    &g_settings.sim3d_dyncam_baseline_tilt_x_mrad, -575,
    kSim3DCameraPitchMinimumMrad, kSim3DCameraPitchMaximumMrad, 25, false,
    NULL, 0, Sim3DDynamicCameraAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_dyncam_baseline_tilt_y_mrad", NULL, "Dynamic baseline yaw",
    "Dynamic Cam's resting yaw in milliradians; the lean works around this.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimCamera,
    &g_settings.sim3d_dyncam_baseline_tilt_y_mrad, 0,
    kSim3DCameraYawMinimumMrad, kSim3DCameraYawMaximumMrad, 20, false,
    NULL, 0, Sim3DDynamicCameraAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_dyncam_baseline_distance_x100", NULL, "Dynamic baseline distance",
    "Dynamic Cam's resting camera distance (hundredths); 0 auto-fits.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimCamera,
    &g_settings.sim3d_dyncam_baseline_distance_x100, 300, 0, 2000, 25, false,
    NULL, 0, Sim3DDynamicCameraAvailable, NULL, NULL, NULL, .modern_env = true },
  BOOL_SETTING_MODERN(sim3d_cull_lift_inset, "AR_SIM3D_LIFT_INSET",
               "Account for flight height at the edge",
               "Pull the bottom of the in-range area in by the height flying "
               "actors are drawn at. The lit ground can only describe the "
               "boundary for something standing on it, so without this a "
               "flying actor is drawn above the edge and appears to vanish "
               "over clear ground. Costs a little of the bright area along "
               "the near edge.",
               kSettingCat_Simulation, 1, false, Sim3DCullHazeEnabled,
               NULL),
  BOOL_SETTING_MODERN(sim3d_backdrop, "AR_SIM3D_BACKDROP", "Atmospheric backdrop",
               "Draw a graded sky behind the finite ground instead of a flat "
               "clear. Town mode anchors it to the tilted map's horizon; "
               "world navigation uses its synthetic horizon beyond the "
               "finite world edges.",
               kSettingCat_Simulation, 1, false, Sim3DBackdropAvailable,
               NULL),
  BOOL_SETTING_MODERN(sim3d_picker_exit_ease, "AR_SIM3D_PICKER_EASE",
               "Ease picker exit",
               "Ease the return from a completed map picker instead of "
               "cutting; not implemented yet.",
               kSettingCat_Simulation, 0, false, Sim3DPickerEaseAvailable,
               NULL),
  { "sim3d_diagnostic_layers", "AR_SIM3D_DIAGNOSTIC_LAYERS",
    "SIM diagnostic layers",
    "Developer-only hexadecimal visibility mask for isolated SIM captures; "
    "zero shows the selected complete profile.",
    kSettingType_Mask, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_diagnostic_layers, 0, 0, 0xFFFF, 1, false,
    NULL, 0, Sim3DSeparatedEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_tilt_y_mrad", "AR_SIM3D_YAW", "Camera yaw",
    "SIM free-camera yaw in milliradians; right-drag horizontally to adjust.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimCamera,
    &g_settings.sim3d_tilt_y_mrad, 0,
    kSim3DCameraYawMinimumMrad, kSim3DCameraYawMaximumMrad,
    20, false, NULL, 0,
    Sim3DFreeCameraAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_tilt_x_mrad", "AR_SIM3D_PITCH", "Camera pitch",
    "SIM free-camera pitch in milliradians; right-drag vertically between "
    "the authored three-quarter view and a low near-horizontal view.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimCamera,
    &g_settings.sim3d_tilt_x_mrad, -575,
    kSim3DCameraPitchMinimumMrad, kSim3DCameraPitchMaximumMrad,
    25, false, NULL, 0,
    Sim3DFreeCameraAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_distance_x100", "AR_SIM3D_DISTANCE", "Camera distance",
    "SIM camera distance in hundredths; 0 auto-fits, and the mouse wheel zooms.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimCamera,
    &g_settings.sim3d_distance_x100, 300, 0, 2000, 25, false, NULL, 0,
    Sim3DFreeCameraAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_height_scale_x100", "AR_SIM3D_HEIGHT_SCALE",
    "Object height scale",
    "Scale every classified SIM flight plane as a percentage of its "
    "catalogue height; 100 keeps the documented planes and 0 grounds every "
    "billboard without disabling the height stage.",
    kSettingType_Int, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_height_scale_x100, 100, 0, 400, 10, false, NULL, 0,
    Sim3DVirtualHeightEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_shadow_opacity_pct", "AR_SIM3D_SHADOW_OPACITY",
    "Shadow darkness",
    "Darkness of town object shadows and world-navigation cloud shadows as a "
    "percentage; 0 skips the applicable shadow pass.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimLighting,
    &g_settings.sim3d_shadow_opacity_pct, kSimShadowOpacityDefaultPct,
    0, 100, 5, false, NULL, 0,
    Sim3DOrWorldNavigationShadowAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_light_azimuth_deg", "AR_SIM3D_LIGHT_AZIMUTH",
    "Light direction",
    "Compass direction the shadow is thrown, in degrees: 0 casts to the "
    "right, 90 away from the camera, 180 left, 270 toward the camera. Only "
    "matters when the light is off vertical. Shared by town objects and "
    "world-navigation cloud shadows.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimLighting,
    &g_settings.sim3d_light_azimuth_deg, kSimLightAzimuthDefaultDeg,
    0, 359, 15, false, NULL, 0,
    Sim3DOrWorldNavigationLightingAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_light_elevation_deg", "AR_SIM3D_LIGHT_ELEVATION",
    "Light height",
    "How high the light sits, in degrees above the ground: 90 is straight "
    "overhead and puts each shadow directly under its caster, lower values "
    "push shadows further out along the light direction. Lower values also "
    "strengthen world navigation's warm colour treatment.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimLighting,
    &g_settings.sim3d_light_elevation_deg, kSimLightElevationDefaultDeg,
    20, 90, 5, false, NULL, 0,
    Sim3DOrWorldNavigationLightingAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_shadow_softness_pct", "AR_SIM3D_SHADOW_SOFTNESS",
    "Shadow softness",
    "Blur radius for the shadow mask, as a percentage. 0 keeps the hard "
    "town silhouette; navigation clouds use it to spread their soft shadow "
    "samples.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimLighting,
    &g_settings.sim3d_shadow_softness_pct, kSimShadowSoftnessDefaultPct,
    0, 100, 5, false, NULL, 0,
    Sim3DOrWorldNavigationSoftShadowAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_rim_strength_pct", "AR_SIM3D_RIM_STRENGTH",
    "Rim light strength",
    "Brightness of the lit edge added to sprite silhouettes, as a percentage. "
    "0 leaves sprite colours untouched even with rim light enabled.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimLighting,
    &g_settings.sim3d_rim_strength_pct, kSimRimStrengthDefaultPct,
    0, 100, 5, false, NULL, 0, Sim3DRimLightEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_underlay_haze_pct", "AR_SIM3D_UNDERLAY_HAZE",
    "World map haze",
    "How far the world map underlay fades toward the scene backdrop, as a "
    "percentage. During world navigation this is the haze strength outside "
    "the active labelled region. 0 draws it at full strength.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_underlay_haze_pct, kSimUnderlayHazeDefaultPct,
    0, 100, 5, false, NULL, 0,
    Sim3DOrWorldNavigationHazeEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cloud_opacity_pct", "AR_SIM3D_CLOUD_OPACITY",
    "Cloud density",
    "Opacity of town cloud banks and whole-world navigation weather, as a "
    "percentage. 0 draws no clouds without disabling either stage.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cloud_opacity_pct, kSimCloudOpacityDefaultPct,
    0, 100, 5, false, NULL, 0,
    Sim3DOrWorldNavigationCloudsAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cloud_falloff_px", "AR_SIM3D_CLOUD_FALLOFF",
    "Cloud edge softness",
    "How far past the sprite-drawable edge the clouds take to reach full "
    "cover, in original pixels. Smaller values make them part sharply as you "
    "approach; larger values keep a long hazy gradient.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cloud_falloff_px, kSimCloudFalloffDefaultPx,
    16, 512, 16, false, NULL, 0, Sim3DCloudShroudEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cloud_inset_px", "AR_SIM3D_CLOUD_INSET",
    "Cloud edge overlap",
    "How far inside the sprite-drawable edge the cloud cover starts building, "
    "in original pixels. Sprites stop being drawn at that edge, so starting "
    "the build-up before it means an actor is already under cloud when it "
    "disappears. Zero starts the ramp exactly at the edge, which leaves a "
    "clear band where actors vanish into nothing.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cloud_inset_px, kSimCloudInsetDefaultPx,
    0, 512, 16, false, NULL, 0, Sim3DCloudShroudEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cull_lead_px", "AR_SIM3D_CULL_LEAD",
    "Cloud lead on culled sprites",
    "How far before the sprite-drawable edge a record's own cloud cover "
    "reaches full strength, in original pixels. The cover has to arrive "
    "before the sprite goes, not after: too small and an actor blinks out a "
    "moment ahead of the cloud that should have hidden it.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cull_lead_px, kSimCullLeadDefaultPx,
    8, 256, 8, false, NULL, 0, Sim3DCloudShroudEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cull_haze_pct", "AR_SIM3D_CULL_HAZE",
    "Out-of-range ground fade",
    "How far the town ground fades toward the distant world map outside the "
    "sprite-drawable window, as a percentage. The bright region is where "
    "actors can exist, so this reads as an area of effect rather than as "
    "sprites failing. It fades rather than dims so the target brightness is "
    "the world map's own, which is already hazed for distance. Zero keeps the "
    "ground at full opacity everywhere. World navigation uses the separate "
    "World map haze strength for its active-location boundary.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cull_haze_pct, kSimCullHazeDefaultPct,
    0, 100, 5, false, NULL, 0, Sim3DCullHazeEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cull_dim_pct", "AR_SIM3D_CULL_DIM",
    "Out-of-range darkening",
    "How far the ground outside the sprite-drawable window is darkened, as a "
    "percentage. Separate from the fade above: the fade decides which layer "
    "is showing out there, this decides how lit it is. It multiplies the "
    "colour down rather than mixing toward the sky, so raising it makes the "
    "far field darker instead of hazier.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cull_dim_pct, kSimCullDimDefaultPct,
    0, 100, 5, false, NULL, 0, Sim3DCullHazeEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cull_haze_lead_px", "AR_SIM3D_CULL_HAZE_LEAD",
    "Ground fade ramp width",
    "How many original pixels the fade takes to reach full strength, "
    "measured from the active edge. In world navigation the ROM-selected "
    "256x256 location stays fully clear and this ramp extends outward. Short "
    "by default so the town's own vision range stays bright; raise it to "
    "trade that brightness for a softer transition.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cull_haze_lead_px, kSimCullHazeLeadDefaultPx,
    16, 512, 16, false, NULL, 0,
    Sim3DOrWorldNavigationHazeEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cull_corner_px", "AR_SIM3D_CULL_CORNER",
    "Ground fade corner rounding",
    "How far the corners of the in-range area are rounded, in original "
    "pixels. Zero gives the sprite-drawable rectangle exactly, which reads as "
    "a hard-edged box laid over the world; rounding reads as framing. "
    "Rounding only ever adds cover at the corners, so it cannot expose a "
    "sprite the window was going to take away.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cull_corner_px, kSimCullCornerDefaultPx,
    0, 256, 8, false, NULL, 0, Sim3DCullHazeEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_underlay_defocus_pct", "AR_SIM3D_DEFOCUS",
    "World map defocus",
    "How far out of focus the distant world map goes outside the "
    "active town or location, as a percentage. Blur says \"too far away to "
    "resolve\" in a way dimming cannot, but it is a cheap downsample rather "
    "than a real lens. Zero keeps the map sharp everywhere.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_underlay_defocus_pct, kSimUnderlayDefocusDefaultPct,
    0, 100, 5, false, NULL, 0,
    Sim3DOrWorldNavigationHazeEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cloud_altitude_px", "AR_SIM3D_CLOUD_ALTITUDE",
    "Cloud altitude",
    "How far above the ground the cloud banks float, in original pixels. Zero "
    "lays them flat on the terrain, where they read as fog painted onto the "
    "map; lifting them puts them between the camera and the world, so they "
    "pass over trees and flying actors instead of through them. In top-down "
    "world navigation it controls both directional shadow displacement and "
    "the zoom point where the camera crosses the cloud deck.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cloud_altitude_px, kSimCloudAltitudeDefaultPx,
    0, 256, 8, false, NULL, 0,
    Sim3DOrWorldNavigationCloudsAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_cloud_drift_pct", "AR_SIM3D_CLOUD_DRIFT",
    "Cloud drift speed",
    "How fast the cloud banks move, as a percentage of their built-in rates. "
    "The layers drift at different speeds, so they pass through each other "
    "and the field churns rather than sliding across as one image. Zero holds "
    "them still.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_cloud_drift_pct, kSimCloudDriftDefaultPct,
    0, 500, 10, false, NULL, 0,
    Sim3DOrWorldNavigationCloudsAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_backdrop_strength_pct", "AR_SIM3D_BACKDROP_STRENGTH",
    "Sky gradient strength",
    "How far the sky is mixed from the scene's own backdrop colour toward blue, "
    "as a percentage. It brightens toward the horizon and deepens overhead. A "
    "town that picks a coloured backdrop tints the result; most pick black, "
    "which is why the sky is mixed toward a blue rather than derived from the "
    "backdrop alone. Zero is the flat fill the projected view used before.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_backdrop_strength_pct, kSimBackdropStrengthDefaultPct,
    0, 100, 5, false, NULL, 0, Sim3DBackdropEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_backdrop_horizon_pct", "AR_SIM3D_BACKDROP_HORIZON",
    "Sky horizon height",
    "Where the sky's bright end sits, as a percentage of screen height from "
    "the top when the projected horizon is unavailable. This is the normal "
    "world-navigation path and places the gradient where sky reads beyond "
    "the finite map.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimAtmosphere,
    &g_settings.sim3d_backdrop_horizon_pct, kSimBackdropHorizonDefaultPct,
    0, 100, 5, false, NULL, 0, Sim3DBackdropEnabled, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_reactive_strength", "AR_SIM3D_REACTIVE",
    "Camera reactivity",
    "How far the town camera leans toward the angel's direction of travel and "
    "how hard it jolts when the angel is hit, as a percentage. Zero holds the "
    "camera at the pose the pitch/yaw/zoom settings describe.",
    kSettingType_Int, kApply_Passive, kSettingCat_SimCamera,
    &g_settings.sim3d_reactive_strength, 100, 0, 200, 10, false, NULL, 0,
    Sim3DDynamicCameraAvailable, NULL, NULL, NULL, .modern_env = true },
  { "sim3d_height_pop_pct", "AR_SIM3D_HEIGHT_POP",
    "Flying sprite pop",
    "Extra size for a flying sprite at its catalogue height, as a percentage. "
    "The true perspective gain from being lifted is about 1%, too subtle to "
    "read; 0 keeps only that. Raising the object height scale raises this "
    "with it.",
    kSettingType_Int, kApply_Passive, kSettingCat_Simulation,
    &g_settings.sim3d_height_pop_pct, 0, 0, 50, 1, false, NULL, 0,
    Sim3DVirtualHeightEnabled, NULL, NULL, NULL, .modern_env = true },
  SIM_ACTION_SETTING("sim3d_reset_camera", "Reset camera",
                     "Return the camera mode in use to its default pitch, yaw, and distance."),
  BOOL_SETTING(diorama_mode, NULL, "Diorama 3D",
               "Render action-stage layers as tilted 3D planes (action stages only; needs the new renderer).",
               kSettingCat_Presentation, 0, false, Diorama_NewPpuCapable,
               DioramaModeChanged),
  /* Free and Dynamic cameras own independent poses. Dynamic applies velocity
   * lean and event kicks around its baseline; switching modes restores the
   * selected mode's own camera instead of carrying the other pose across. */
  { "diorama_camera_mode", NULL, "Camera mode",
    "Free Cam: manual orbit and zoom, with a persistent pose. Dynamic Cam: "
    "leans with gameplay motion and reacts to impacts around its own baseline.",
    kSettingType_Enum, kApply_Passive, kSettingCat_DioramaCamera,
    &g_settings.diorama_camera_mode, kDioramaCam_Free,
    kDioramaCam_Free, kDioramaCam_Dynamic, 1, false,
    kDioramaCamModeLabels, kDioramaCam_Count, Diorama_ModeIsOn, NULL,
    NULL, NULL },
  /* B4-baseline (followup doc): Dynamic Cam's dedicated pose (see the
   * Settings struct comment). Provisional literals per the doc — a gentle
   * 3/4 tilt (~0.20 rad pitch), symmetric yaw (0), auto-fit distance. Same
   * step convention as their free-cam counterparts just below. */
  { "diorama_dyncam_baseline_tilt_y_mrad", NULL, "Dynamic baseline yaw",
    "Dynamic Cam's resting yaw in milliradians; sway leans around this, not 0.",
    kSettingType_Int, kApply_Passive, kSettingCat_DioramaCamera,
    &g_settings.diorama_dyncam_baseline_tilt_y_mrad, 0, -700, 700, 20, false,
    NULL, 0, DioramaDynamicCameraAvailable, NULL, NULL, NULL },
  { "diorama_dyncam_baseline_tilt_x_mrad", NULL, "Dynamic baseline pitch",
    "Dynamic Cam's resting pitch in milliradians; sway leans around this, not 0.",
    kSettingType_Int, kApply_Passive, kSettingCat_DioramaCamera,
    &g_settings.diorama_dyncam_baseline_tilt_x_mrad, 200, -700, 700, 25, false,
    NULL, 0, DioramaDynamicCameraAvailable, NULL, NULL, NULL },
  { "diorama_dyncam_baseline_distance_x100", NULL, "Dynamic baseline distance",
    "Dynamic Cam's resting camera distance (hundredths); 0 auto-fits the frame.",
    kSettingType_Int, kApply_Passive, kSettingCat_DioramaCamera,
    &g_settings.diorama_dyncam_baseline_distance_x100, 0, 0, 2000, 25, false,
    NULL, 0, DioramaDynamicCameraAvailable, NULL, NULL, NULL },
  INT_SETTING(diorama_reactive_strength, NULL, "Reactive strength",
              "Dynamic Cam: how strongly the camera sways with gameplay "
              "motion; 0 holds it fixed at the baseline pose.",
              kSettingCat_DioramaCamera, 35, 0, 100, NULL,
              DioramaDynamicCameraAvailable),
  /* Promotes BG2 to an enveloping dimmed+DoF'd skybox so
   * camera tilt/yaw/zoom never reveals the void past the finite backdrop
   * quad's edges. Off keeps the default presentation; see DioramaSkyMode
   * (settings.h) for what the other two modes do. */
  { "diorama_skybox", NULL, "Skybox",
    "Off: BG2 is an in-box parallax plane (void may show at the margins "
    "under tilt). Skybox only: BG2 fills the background instead. Plane + "
    "skybox: both — keeps BG2's parallax AND backstops the void/gaps.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Presentation,
    &g_settings.diorama_skybox, kDioramaSky_Off,
    kDioramaSky_Off, kDioramaSky_Both, 1, false,
    kDioramaSkyModeLabels, kDioramaSky_Count, Diorama_ModeIsOn, NULL,
    NULL, NULL },
  /* One switch for all three parts of the widescreen margin fix,
   * so the black wedge at a level bound can be A/B'd live: stand at the level
   * start and toggle. Default on; Off restores every pre-fix path byte for
   * byte, which is what makes it a usable comparison rather than a
   * half-migration. */
  { "diorama_margin_fix", "AR_DIORAMA_MARGIN_FIX", "Edge margin fix",
    "Fixes the black wedge at a level's start/end: pads captured layers to the "
    "full widescreen budget, fills the frame's edge gaps with the scene "
    "backdrop instead of black, and crops the skybox to BG2's valid span. "
    "Off restores the previous behaviour for comparison.",
    kSettingType_Bool, kApply_Passive, kSettingCat_Presentation,
    &g_settings.diorama_margin_fix, 1, 0, 1, 1, false,
    NULL, 0, Diorama_ModeIsOn, NULL,
    NULL, NULL },
  /* B6 (followup doc): floor/ceiling/side-wall enclosure masking the box's
   * off-screen edges. Composes with B5 (skybox fills the far opening);
   * independent so each can be A/B'd alone. */
  BOOL_SETTING(diorama_shoebox, NULL, "Shoebox walls",
               "Enclose the layer stack in a floor, ceiling, and side "
               "walls so its off-screen edges are masked instead of "
               "ending in void.",
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
    "Diorama camera yaw in milliradians; negative swings the left edge toward you.",
    kSettingType_Int, kApply_Passive, kSettingCat_DioramaCamera,
    &g_settings.diorama_tilt_y_mrad, -180, -700, 700, 20, false, NULL, 0,
    DioramaFreeCameraAvailable, NULL, NULL, NULL },
  { "diorama_tilt_x_mrad", NULL, "Camera pitch",
    "Diorama camera pitch in milliradians; 0 keeps sprites planted on their platforms.",
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
    "Diorama camera distance (hundredths); 0 auto-fits the frame to the window.",
    kSettingType_Int, kApply_Passive, kSettingCat_DioramaCamera,
    &g_settings.diorama_distance_x100, 0, 0, 2000, 25, false, NULL, 0,
    DioramaFreeCameraAvailable, NULL, NULL, NULL },
  /* Scanlines of real world revealed on EACH side of the authentic viewport,
   * the vertical counterpart of the widescreen side margins. Defaults to 0
   * (authentic framing) because the band is where the level's vertical tilemap
   * streaming shows its seams: column strips decode only a 512px-tall window,
   * and rows outside it hold filler until a row strip covers them
   * (rendering-engine.md §4). 64 covers the measured 48px camera jump while
   * exact signed OBJ positions avoid the 8-bit OAM Y ambiguity. */
  { "diorama_vertical_extend", NULL, "Vertical extend",
    "Scanlines of extra world drawn above and below the screen. 0 keeps the "
    "authentic 224-line frame.",
    kSettingType_Int, kApply_Passive, kSettingCat_Presentation,
    &g_settings.diorama_vertical_extend, 0, 0, 64, 4, false, NULL, 0,
    Diorama_ModeIsOn, NULL, NULL, NULL },
  INT_SETTING(diorama_depth_shade, NULL, "Depth shading",
              "Strength of the atmospheric darkening applied to farther planes.",
              kSettingCat_Presentation, 100, 0, 100, NULL, Diorama_ModeIsOn),
  /* Default off: the backdrop plane is a finite quad drawn behind everything,
   * and at a level bound the never-written margin columns read as an opaque
   * black seam over the scene. With the
   * plane hidden the skybox/clear shows through instead, which is what the
   * tilted box wants in every mode. Turn it on to get the flat backdrop
   * colour back. */
  BOOL_SETTING(diorama_layer_backdrop, NULL, "Show backdrop",
               "Diorama: show the backdrop plane. Off by default — the plane "
               "is finite, so at a level's start/end its unwritten margins "
               "show as a black seam.",
               kSettingCat_Presentation, 0, false, Diorama_ModeIsOn, NULL),
  BOOL_SETTING(diorama_layer_bg2, NULL, "Show BG2",
               "Diorama: show the BG2 parallax plane.",
               kSettingCat_Presentation, 1, false, Diorama_ModeIsOn, NULL),
  BOOL_SETTING(diorama_layer_bg1, NULL, "Show BG1",
               "Diorama: show the BG1 playfield plane.",
               kSettingCat_Presentation, 1, false, Diorama_ModeIsOn, NULL),
  BOOL_SETTING(diorama_layer_obj, NULL, "Show sprites",
               "Diorama: show the sprite (OBJ) plane.",
               kSettingCat_Presentation, 1, false, Diorama_ModeIsOn, NULL),
  BOOL_SETTING(diorama_layer_bg3, NULL, "Show BG3/HUD",
               "Diorama: show the BG3 (HUD) plane.",
               kSettingCat_Presentation, 1, false, Diorama_ModeIsOn, NULL),
  BOOL_SETTING(diorama_hud_flat, NULL, "Flat HUD",
               "On: HUD (ACT/TIME/SCORE, health, boss bar) stays flat and "
               "widescreen-anchored like flat mode. Off: HUD renders as an "
               "unanchored tilted plane in the box, matching the pre-fix look.",
               kSettingCat_Presentation, 1, false, Diorama_ModeIsOn, NULL),
  PRESENTATION_ACTION_SETTING("diorama_reset", "Reset defaults",
                              "Return all diorama controls to their defaults."),
  /* Load-only migration alias; Refresh rate is the sole live control. */
  BOOL_SETTING(uncapped_framerate, NULL, "Uncapped framerate",
               "Legacy compatibility alias for Refresh rate > Uncapped.",
               kSettingCat_Graphics, 0, false, NULL,
               OnLegacyUncappedChanged),
  BOOL_SETTING(action_effect_lighting, "AR_ACTION_EFFECT_LIGHTING",
               "Action spell lighting",
               "Add transient local illumination to action-stage magic. Uses "
               "portable SDL additive geometry in both flat and diorama mode.",
               kSettingCat_Graphics, 1, false,
               ActionEffectRendererAvailable, NULL),
  BOOL_SETTING(action_effect_particles, "AR_ACTION_EFFECT_PARTICLES",
               "Action spell particles",
               "Add deterministic host particles synchronized to captured "
               "action-stage spell actors.",
               kSettingCat_Graphics, 1, false,
               ActionEffectRendererAvailable, NULL),
  /* Load/save compatibility for configurations that exposed the old optional
   * backend switch. main.c now forces this value on because SIM3D depth is a
   * baseline renderer capability; the effect rows remain independently live. */
  { "gpu_shaders_enabled", "AR_GPU_SHADERS", "GPU shader effects",
    "The GPU renderer is required. This retained setting only preserves "
    "older configuration files; individual effects remain configurable.",
    kSettingType_Bool, kApply_Restart, kSettingCat_Graphics,
    &g_settings.gpu_shaders_enabled, 0, 0, 1, 1, false, NULL, 0,
    NULL, NULL, NULL, NULL },
  BOOL_SETTING(gpu_fx_rim, "AR_GPU_FX_RIM", "Rim lighting",
               "Diorama: warm edge glow on sprite silhouettes.",
               kSettingCat_Graphics, 1, false, GpuShadersActive, NULL),
  BOOL_SETTING(gpu_fx_dof, "AR_GPU_FX_DOF", "Depth of field",
               "Diorama: blur background layers by distance from the main "
               "playfield.",
               kSettingCat_Graphics, 1, false, GpuShadersActive, NULL),
  BOOL_SETTING(gpu_fx_edgeaa, "AR_GPU_FX_EDGEAA", "Edge anti-aliasing",
               "Diorama: soften the hard rectangular edge of tilted "
               "background layers.",
               kSettingCat_Graphics, 1, false, GpuShadersActive, NULL),
  BOOL_SETTING(gpu_fx_shadow, "AR_GPU_FX_SHADOW", "Soft shadow blur",
               "Diorama: blur sprite/layer drop shadows. KNOWN ISSUE: can "
               "bleed onto transparent gaps in the layer behind it (e.g. a "
               "hazy patch over the sky) — off by default until fixed.",
               kSettingCat_Graphics, 0, false, GpuShadersActive, NULL),
  /* One existing opt-in owns the replacement frame-generation path. */
  BOOL_SETTING(gpu_interp_enabled, "AR_INTERP_ENABLE",
               "Frame interpolation",
               "Diorama: generate intermediate captured-layer frames for "
               "smoother motion on high-refresh (>60Hz) displays.",
               kSettingCat_Graphics, 0, false, Diorama_ModeIsOn, NULL),
  { "gpu_interp_source_rate", "AR_INTERP_SOURCE_RATE",
    "Interpolation source test",
    "Native preserves authentic 60 Hz gameplay. Test 30 -> 60 Hz deliberately "
    "runs Diorama gameplay in slow motion at 30 Hz while presentation remains "
    "at the selected refresh rate, making every generated midpoint visible on "
    "a 60 Hz Vsync display.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Graphics,
    &g_settings.gpu_interp_source_rate, kInterpolationSource_Native,
    kInterpolationSource_Native, kInterpolationSource_Test30, 1, false,
    kInterpolationSourceLabels, kInterpolationSource_Count,
    FrameInterpolationActive, NULL, NULL, NULL, .modern_env = true },
  /* CRT post-process (kSettingCat_Crt). One fullscreen pass at the end of
   * presentation, so unlike the diorama-only gpu_fx_* rows above it covers
   * every render mode at once. Needs the same "gpu" backend, hence the shared
   * GpuShadersActive gate; the numeric rows below are developer-only
   * (Settings_IsDebugOnly) because they author the look rather than trade it
   * off against performance. Defaults are the tuned-by-eye values. */
  BOOL_SETTING(crt_enabled, "AR_CRT", "CRT effect",
               "Simulate a curved colour CRT: barrel glass, scanlines, "
               "phosphor mask and corner falloff. Applies to every mode "
               "(flat, diorama and 3D town) at once.",
               kSettingCat_Crt, 0, false, GpuShadersActive, NULL),
  INT_SETTING(crt_curvature_x100, "AR_CRT_CURVE", "Glass curvature",
              "How far the tube bows the picture. 0 is flat glass; the "
              "corners darken and the image pulls in as this rises.",
              kSettingCat_Crt, 45, 0, 200, NULL, GpuShadersActive),
  INT_SETTING(crt_scanline_x100, "AR_CRT_SCAN", "Scanline depth",
              "How dark the gaps between the 224 source scanlines go. "
              "Follows the signal, so the count holds at any window size.",
              kSettingCat_Crt, 35, 0, 100, NULL, GpuShadersActive),
  INT_SETTING(crt_mask_x100, "AR_CRT_MASK", "Phosphor mask",
              "Strength of the red/green/blue aperture grille. Sits on the "
              "glass at output-pixel pitch, so it does not warp with the "
              "picture.",
              kSettingCat_Crt, 40, 0, 100, NULL, GpuShadersActive),
  INT_SETTING(crt_aberration_x100, "AR_CRT_ABERR", "Colour fringing",
              "Horizontal red/blue split, in output pixels. Reads as beam "
              "convergence error; high values fringe hard edges heavily.",
              kSettingCat_Crt, 25, 0, 300, NULL, GpuShadersActive),
  INT_SETTING(crt_bandwidth_x100, "AR_CRT_BAND", "Signal bandwidth",
              "Horizontal smear between neighbouring pixels, in source "
              "pixels. Composite and RF carried less bandwidth than the pixel "
              "clock, which is what let dithered art blend into extra "
              "apparent colours. Vertical detail stays sharp.",
              kSettingCat_Crt, 30, 0, 200, NULL, GpuShadersActive),
  INT_SETTING(crt_vignette_x100, "AR_CRT_VIG", "Corner falloff",
              "Darkening toward the corners of the tube.",
              kSettingCat_Crt, 25, 0, 100, NULL, GpuShadersActive),
  INT_SETTING(crt_brightness_x100, "AR_CRT_BRIGHT", "Brightness",
              "Lifts the whole picture to compensate for the light the "
              "scanlines and phosphor mask absorb. 100 is unmodified.",
              kSettingCat_Crt, 125, 50, 300, NULL, GpuShadersActive),
  { "audio_enabled", "AR_ENABLE_AUDIO", "Enable audio",
    "Mute or unmute output while authentic and enhanced audio timelines keep "
    "running.",
    kSettingType_Bool, kApply_Callback, kSettingCat_Audio,
    &g_settings.audio_enabled, 1, 0, 1, 1, false, NULL, 0,
    NULL, NULL, NULL, NULL, .modern_env = true },
  { "audio_frequency", "AR_AUDIO_FREQ", "Audio frequency",
    "Auto matches the audio device's native rate. 32.04 kHz is raised to 44.1 "
    "(SDL's device minimum).",
    kSettingType_Enum, kApply_Restart, kSettingCat_Audio,
    &g_settings.audio_frequency, kAudioFrequency_Auto,
    kAudioFrequency_32040, kAudioFrequency_Auto, 1, false,
    kAudioFrequencyLabels, kAudioFrequency_Count, NULL, NULL,
    ParseAudioFrequency, NULL, .modern_env = true },
  { "audio_samples", "AR_AUDIO_SAMPLES", "Audio buffer samples",
    "Set the host audio callback buffer size on the next device initialization.",
    kSettingType_Int, kApply_Restart, kSettingCat_Audio,
    &g_settings.audio_samples, 2048, 64, 8192, 1, false, NULL, 0,
    NULL, NULL, NULL, NULL, .modern_env = true },
  { "audio_master_volume", "AR_AUDIO_VOLUME", "Master volume",
    "Scale the final game output, including music, sound effects, and MSU-1 audio.",
    kSettingType_Int, kApply_Callback, kSettingCat_Audio,
    &g_settings.audio_master_volume, 100, 0, 100, 5, false, NULL, 0,
    NULL, NULL, ParseAudioVolume, FormatAudioVolume },
  { "audio_music_volume", "AR_MUSIC_VOLUME", "Music volume",
    "Scale authentic SPC music and enhanced replacement music without changing sound effects.",
    kSettingType_Int, kApply_Callback, kSettingCat_Audio,
    &g_settings.audio_music_volume, 100, 0, 100, 5, false, NULL, 0,
    NULL, NULL, ParseAudioVolume, FormatAudioVolume, .modern_env = true },
  { "audio_sfx_volume", "AR_SFX_VOLUME", "Sound effects volume",
    "Scale native sound effects without changing authentic or enhanced music.",
    kSettingType_Int, kApply_Callback, kSettingCat_Audio,
    &g_settings.audio_sfx_volume, 100, 0, 100, 5, false, NULL, 0,
    NULL, NULL, ParseAudioVolume, FormatAudioVolume, .modern_env = true },
  { "audio_extended_channels", "AR_EXTENDED_AUDIO_CHANNELS",
    "Extended sound channels",
    "Keep all eight music voices while queued native effects use a pool of "
    "32 added voices. Takes effect after restart.",
    kSettingType_Bool, kApply_Restart, kSettingCat_Audio,
    &g_settings.audio_extended_channels, 0, 0, 1, 1, false, NULL, 0,
    NULL, NULL, NULL, NULL, .modern_env = true },
  BOOL_SETTING(audio_dialog_blip, "AR_DIALOG_BLIP", "Dialogue text blip",
               "Play the per-character sound while Sky Palace dialogue is printed.",
               kSettingCat_Audio, 1, false, NULL, NULL),
  { "music_replacements", "AR_MUSIC_REPLACEMENTS", "Enhanced music",
    "Replace authentic music with matching OGG tracks from game-assets/manifest.ini.",
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
    "Which device drives the game. Auto keeps the keyboard and the selected "
    "gamepad both live.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Input,
    &g_settings.input_device, kInputDevice_Auto, kInputDevice_Auto,
    kInputDevice_Gamepad, 1, false, kInputDeviceLabels, kInputDevice_Count,
    NULL, NULL, NULL, NULL },
  { "input_gamepad_slot", NULL, "Gamepad",
    "Which connected controller to read. First connected follows hotplug.",
    kSettingType_Int, kApply_Passive, kSettingCat_Input,
    &g_settings.input_gamepad_slot, 0, 0, 8, 1, false, NULL, 0,
    NULL, NULL, ParseGamepadSlot, FormatGamepadSlot },
  { "input_bind_page", NULL, "Configure bindings for",
    "Choose which device's bindings the rows below show and edit.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Input,
    &g_settings.input_bind_page, kInputClass_Keyboard, kInputClass_Keyboard,
    kInputClass_Gamepad, 1, false, kInputClassLabels, kInputClass_Count,
    NULL, NULL, NULL, NULL },
  BOOL_SETTING(input_stick_as_dpad, NULL, "Left stick as D-Pad",
               "Steer with the left analog stick in addition to the D-Pad. "
               "Recommended on Steam Deck.",
               kSettingCat_Input, 1, false, NULL, NULL),
  INT_SETTING(input_stick_deadzone, NULL, "Stick deadzone",
              "How far the left stick must travel before it counts as a "
              "direction, as a percent of full travel.",
              kSettingCat_Input,
              kInputStickDeadzoneDefaultPercent,
              kInputStickDeadzoneMinimumPercent,
              kInputStickDeadzoneMaximumPercent, NULL, NULL),
  INT_SETTING(input_cam_sensitivity, NULL, "Camera sensitivity",
              "Speed of stick-driven camera orbit and zoom in the diorama and "
              "3D town, as a percent of the base rate.",
              kSettingCat_Input, 100, 10, 400, NULL, NULL),
  INT_SETTING(input_cam_deadzone, NULL, "Camera stick deadzone",
              "How far the camera stick must travel before it orbits. Lower "
              "than the D-Pad deadzone so small nudges still aim.",
              kSettingCat_Input,
              kInputCameraDeadzoneDefaultPercent,
              kInputCameraDeadzoneMinimumPercent,
              kInputCameraDeadzoneMaximumPercent, NULL, NULL),
  BOOL_SETTING(input_cam_invert_y, NULL, "Invert camera Y",
               "Push the camera stick up to pitch the view down.",
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
                  "Debug aid; needs the Cycle magic spell cheat. Press "
                  "Enter, then the key to bind. Y resets it to the default."),
  BINDING_SETTING(kInputAction_MagicCycle, kInputClass_Gamepad,
                  "bind_pad_magic_cycle", "Cycle magic spell",
                  "Debug aid; needs the Cycle magic spell cheat. Press "
                  "Enter, then the button to bind. Y resets it to the "
                  "default."),
  BINDING_SETTING(kInputAction_RenderCompare, kInputClass_Keyboard,
                  "bind_key_render_compare", "Compare rendering",
                  "Click to swap authentic and enhanced. Hold to toggle a "
                  "persistent authentic picture-in-picture."),
  BINDING_SETTING(kInputAction_RenderCompare, kInputClass_Gamepad,
                  "bind_pad_render_compare", "Compare rendering",
                  "Click to swap authentic and enhanced. Hold to toggle a "
                  "persistent authentic picture-in-picture."),

  /* This is an optional gameplay enhancement rather than a bug fix: the
   * original 128-record structure cap is authentic. Completed bridges move
   * to a checksummed extension area so they stop consuming those records,
   * while the census and redraw hooks preserve their support and tiles. */
  BOOL_SETTING(show_debug_settings, "AR_SHOW_DEBUG_SETTINGS",
               "Show debug settings",
               "Reveal developer-only rows: the diorama and town 3D numeric "
               "tuning dials, their layer and stage A/B toggles, and the scene "
               "inspector. Off keeps the menu to the master toggles and major "
               "on/off effects.",
               kSettingCat_Extras, 0, false, NULL, NULL),
  BOOL_SETTING(fix_bridge_limit, "AR_FIX_BRIDGE_LIMIT", "Bridge-free limit",
               "Completed bridges stop counting toward the 128-structure "
               "population cap; they migrate to spare save space and keep "
               "their tiles, crossing, and support.",
               kSettingCat_Enhancements, 0, true, NULL, NULL),
  BOOL_SETTING(fix_windmill_wind_stop, "AR_FIX_WINDMILL_WIND_STOP",
               "Wind stops every windmill",
               "Aitos's no-wind event only stills the windmills that already "
               "existed, so one the town builds during it keeps turning. On, "
               "every windmill in the town holds until the Wind miracle "
               "restores them. Off reproduces the original game. Affects the "
               "enhanced town view only.",
               kSettingCat_Enhancements, 1, false, NULL, NULL),
  INT_SETTING(turbo_multiplier, "AR_TURBO_MULT", "Turbo multiplier",
              "Number of game frames advanced per rendered frame while turbo is active.",
              kSettingCat_Enhancements, 8, 2, 64, ParseTurboMultiplier, NULL),
  { "warp_target", "AR_WARP", "Warp target",
    "Raw hexadecimal region/map target used by Warp now; see docs/manual.md for verified values.",
    kSettingType_Custom, kApply_Passive, kSettingCat_Extras,
    &g_settings.warp_target, 0x0101, 0, 0xffff, 1, false, NULL, 0,
    NULL, NULL, ParseWarpTarget, FormatWarpTarget },
  /* ---- Randomizer (src/randomizer.c). Descriptors only; every one of these
   * is read by the ROM-image transform, which re-runs whenever one changes. */
  BOOL_SETTING(rando_enable, "AR_RANDO", "Randomizer",
               "Rewrite the game's content tables from the seed below. Off "
               "restores the non-randomized ROM baseline. Stat changes take "
               "effect at the next spawn, placement changes at the next level "
               "load.",
               kSettingCat_RandoSeed, 0, false, RandoRuntimeAvailable,
               RandoChanged),
  INT_SETTING(rando_seed, "AR_RANDO_SEED", "Seed",
              "Runs with the same seed and the same options are identical. "
              "Each option draws from its own stream, so toggling one does not "
              "shift what another produces.",
              kSettingCat_RandoSeed, 1, 0, 999999999, NULL, RandoAvailable),
  { "rando_reroll", NULL, "New seed",
    "Draw a fresh random seed and re-apply.",
    kSettingType_Action, kApply_Action, kSettingCat_RandoSeed,
    NULL, 0, 0, 0, 0, false, NULL, 0, RandoAvailable, NULL, NULL, NULL },

  INT_SETTING(rando_enemy_hp, "AR_RANDO_ENEMY_HP", "Enemy health",
              "Percent of stock hit points for every enemy and boss. A handful "
              "of bosses set their own HP at runtime and ignore this.",
              kSettingCat_RandoEnemies, 100, 10, 1000, NULL, RandoAvailable),
  INT_SETTING(rando_enemy_atk, "AR_RANDO_ENEMY_ATK", "Enemy damage",
              "Percent of stock contact and attack damage for every enemy.",
              kSettingCat_RandoEnemies, 100, 10, 1000, NULL, RandoAvailable),
  { "rando_enemy_types", "AR_RANDO_ENEMY_TYPES", "Enemy types",
    "Shuffle which enemy stands where. Bosses and item statues are left alone.",
    kSettingType_Enum, kApply_Passive, kSettingCat_RandoEnemies,
    &g_settings.rando_enemy_types, kRandomMode_Off, kRandomMode_Off,
    kRandomMode_Shuffle, 1, false, kRandoShuffleLabels, 2,
    RandoAvailable, RandoChanged, NULL, NULL },
  { "rando_enemy_scope", "AR_RANDO_ENEMY_SCOPE", "Enemy shuffle range",
    "How far an enemy may move. Act is the widest safe range: every map of an "
    "act shares one animation set, so a type moved inside an act still draws "
    "correctly.",
    kSettingType_Enum, kApply_Passive, kSettingCat_RandoEnemies,
    &g_settings.rando_enemy_scope, kRandomScope_Act, kRandomScope_Map,
    kRandomScope_Act, 1, false, kRandoScopeLabels, kRandomScope_Count,
    RandoEnemyTypesOn, RandoChanged, NULL, NULL },

  { "rando_statue_drops", "AR_RANDO_DROPS", "Statue drops",
    "What the breakable statues hold. Shuffle redistributes the items a map "
    "already has; Random rolls each one fresh.",
    kSettingType_Enum, kApply_Passive, kSettingCat_RandoItems,
    &g_settings.rando_statue_drops, kRandomMode_Off, kRandomMode_Off,
    kRandomMode_Random, 1, false, kRandoModeLabels, kRandomMode_Count,
    RandoAvailable, RandoChanged, NULL, NULL },
  { "rando_statue_spots", "AR_RANDO_STATUES", "Statue placement",
    "Swap the statues of a map between each other's positions. Every result is "
    "somewhere a statue already stood, so none can end up unreachable.",
    kSettingType_Enum, kApply_Passive, kSettingCat_RandoItems,
    &g_settings.rando_statue_spots, kRandomMode_Off, kRandomMode_Off,
    kRandomMode_Shuffle, 1, false, kRandoShuffleLabels, 2,
    RandoAvailable, RandoChanged, NULL, NULL },

  { "rando_lair_spots", "AR_RANDO_LAIRS", "Lair positions",
    "Swap each town's four monster lairs between their own positions. Kept "
    "within a town because nothing in the game checks a lair against terrain.",
    kSettingType_Enum, kApply_Passive, kSettingCat_RandoSim,
    &g_settings.rando_lair_spots, kRandomMode_Off, kRandomMode_Off,
    kRandomMode_Shuffle, 1, false, kRandoShuffleLabels, 2,
    RandoAvailable, RandoChanged, NULL, NULL },
  { "rando_lair_types", "AR_RANDO_LAIR_TYPES", "Lair monsters",
    "Which monster each lair sends out. Any town can host any of the four.",
    kSettingType_Enum, kApply_Passive, kSettingCat_RandoSim,
    &g_settings.rando_lair_types, kRandomMode_Off, kRandomMode_Off,
    kRandomMode_Random, 1, false, kRandoModeLabels, kRandomMode_Count,
    RandoAvailable, RandoChanged, NULL, NULL },

  BOOL_SETTING(scene_inspector, "AR_SCENE_INSPECTOR", "Scene inspector",
               "Click the game to pause and identify BG tiles, OAM sprites, "
               "VRAM addresses, palettes, hashes, and manifest gates.",
               kSettingCat_Inspector, 0, false, NULL, NULL),
  INSPECTOR_ACTION_SETTING(
      "dump_scene_assets", "Dump scene assets",
      "Export every resident BG tilemap, OBJ animation-tile atlas, all 128 "
      "OAM sprites, palettes, raw PPU memory, and a metadata index as PNG "
      "and data files in this run's diagnostic folder."),
  /* The in-game manual. Its own section rather than a row under System > Tools:
   * it is something a player reaches for, not a host command, and the reader it
   * opens is a full mode rather than a toggle. */
  MANUAL_ACTION_SETTING(
      "manual_open", "Read the manual",
      "Open the scanned game manual. Arrows or the shoulder buttons turn "
      "pages, +/- zooms, Escape returns here."),
  BOOL_SETTING(manual_spreads, NULL, "Two-page spreads",
               "Show the manual as facing pages, the way the booklet reads. "
               "Artwork drawn across the gutter stays whole; turn this off to "
               "read one page at a time, which suits a wide, short manual.",
               kSettingCat_Manual, true, false, NULL, NULL),
  ACTION_SETTING("toggle_pause", "Pause / resume",
                 "Toggle game pause after the settings overlay closes."),
  ACTION_SETTING("toggle_turbo", "Toggle turbo",
                 "Toggle fast-forward using the configured turbo multiplier."),
  ACTION_SETTING("save_state", "Save state",
                 "Save quick-state slot 0, matching the F5 hotkey."),
  ACTION_SETTING("load_state", "Load state",
                 "Load quick-state slot 0, matching the F7 hotkey."),
  ACTION_SETTING("warp_now", "Warp now",
                 "Stage the configured raw warp target through the game's transition path."),
  ACTION_SETTING("take_snapshot", "Take snapshot",
                 "Capture WRAM, VRAM, CGRAM, OAM, and the current game framebuffer."),
  ACTION_SETTING("restart_game", "Restart game",
                 "Persist settings and battery SRAM, then restart the application."),
  ACTION_SETTING("exit_desktop", "Exit to desktop",
                 "Persist settings and battery SRAM, then close the application."),
  { "save_backend", "AR_SAVE_BACKEND", "Save storage format",
    "Choose the authoritative native-srm or lossless INI backend after restart.",
    kSettingType_Enum, kApply_Restart, kSettingCat_Save,
    &g_settings.save_backend, 0, 0, 1, 1, false,
    kSaveBackendLabels, 2, NULL, NULL, ParseSaveBackend, NULL },
  BOOL_SETTING(save_edit_armed, "AR_SAVE_EDIT", "Allow save edits",
               "Safety switch: permits Apply actions and next-boot staged overrides to change SRAM.",
               kSettingCat_Save, 0, false, NULL, NULL),
  BOOL_SETTING(save_autobackup, "AR_SAVE_BACKUP", "Auto-backup",
               "Back up the active save before the first persistent editor change.",
               kSettingCat_Save, 1, false, NULL, NULL),
  { "save_editor_page", NULL, "Edit section",
    "Choose Actions, Progress, Status, Magic, Items, or Scores; the overlay drives this from its tab bar.",
    kSettingType_Enum, kApply_Passive, kSettingCat_Save,
    &g_settings.save_editor_page, kSaveEditorPage_Actions,
    kSaveEditorPage_Actions, kSaveEditorPage_Count - 1, 1, false,
    kSaveEditorPageLabels, kSaveEditorPage_Count, NULL, NULL, NULL, NULL },
  SAVE_PROGRESS_SETTING(0, "save_prog_fillmore", "AR_SAVE_PROG_FILLMORE",
                        "Fillmore State", "Stage Fillmore's Act/state flags."),
  SAVE_PROGRESS_SETTING(1, "save_prog_bloodpool", "AR_SAVE_PROG_BLOODPOOL",
                        "Bloodpool State", "Stage Bloodpool's Act/state flags."),
  SAVE_PROGRESS_SETTING(2, "save_prog_kasandora", "AR_SAVE_PROG_KASANDORA",
                        "Kasandora State", "Stage Kasandora's Act/state flags."),
  SAVE_PROGRESS_SETTING(3, "save_prog_aitos", "AR_SAVE_PROG_AITOS",
                        "Aitos State", "Stage Aitos's Act/state flags."),
  SAVE_PROGRESS_SETTING(4, "save_prog_marahna", "AR_SAVE_PROG_MARAHNA",
                        "Marahna State", "Stage Marahna's Act/state flags."),
  SAVE_PROGRESS_SETTING(5, "save_prog_northwall", "AR_SAVE_PROG_NORTHWALL",
                        "Northwall State", "Stage Northwall's Act/state flags."),
  SAVE_ENUM_FIELD(&g_settings.save_death_heim_state,
                  "save_death_heim_state", "Death Heim State",
                  "Stage Death Heim as locked, unlocked, or cleared.",
                  kSaveDeathHeimLabels),
  SAVE_ENUM_FIELD(&g_settings.save_professional_mode,
                  "save_professional_mode", "Professional Mode",
                  "Stage the title-screen Professional mode unlock marker.",
                  kSaveProfessionalLabels),
  { "save_player_name", NULL, "Player Name",
    "Stage the saved player name (1-8 printable characters).",
    kSettingType_Custom, kApply_Save, kSettingCat_Save,
    g_settings.save_player_name, 0, 0, 0, 0, false,
    NULL, 0, NULL, NULL, ParseSavePlayerName, FormatSavePlayerName },
  SAVE_STAGE_DIRECT(save_master_level, "save_master_level", "Master Level",
                    "Stage the persistent Master level (1-17).", 17),
  SAVE_STAGE_DIRECT(save_master_hp, "save_master_hp", "Master HP",
                    "Stage persistent Master health (1-24).", 24),
  SAVE_STAGE_ZERO(save_master_mp, "save_master_mp", "Master MP",
                  "Stage persistent magic scrolls/MP (0-10).", 10),
  SAVE_STAGE_DIRECT(save_lives, "save_lives", "Lives",
                    "Stage the displayed life count (1-9).", 9),
  SAVE_STAGE_ZERO(save_angel_sp_current, "save_angel_sp_current",
                  "Angel Current SP", "Stage current simulation SP (0-999).", 999),
  SAVE_STAGE_ZERO(save_angel_sp_max, "save_angel_sp_max",
                  "Angel Maximum SP", "Stage maximum simulation SP (0-999).", 999),
  SAVE_STAGE_ZERO(save_angel_hp_current, "save_angel_hp_current",
                  "Angel Current HP", "Stage current Angel health (0-24).", 24),
  SAVE_STAGE_DIRECT(save_angel_hp_max, "save_angel_hp_max",
                    "Angel Maximum HP", "Stage maximum Angel health (1-24).", 24),
  SAVE_STAGE_ZERO(save_message_speed, "save_message_speed", "Message Speed",
                  "Stage the saved native dialogue-speed value (0-9).", 9),
  SAVE_ENUM_FIELD(&g_settings.save_equipped_magic,
                  "save_equipped_magic", "Equipped Magic",
                  "Stage the equipped spell; that spell must exist in a magic slot.",
                  kSaveEquippedMagicLabels),
  SAVE_ENUM_FIELD(&g_settings.save_magic_slots[0],
                  "save_magic_slot_1", "Magic Slot 1",
                  "Stage the spell stored in magic inventory slot 1.",
                  kSaveMagicSlotLabels),
  SAVE_ENUM_FIELD(&g_settings.save_magic_slots[1],
                  "save_magic_slot_2", "Magic Slot 2",
                  "Stage the spell stored in magic inventory slot 2.",
                  kSaveMagicSlotLabels),
  SAVE_ENUM_FIELD(&g_settings.save_magic_slots[2],
                  "save_magic_slot_3", "Magic Slot 3",
                  "Stage the spell stored in magic inventory slot 3.",
                  kSaveMagicSlotLabels),
  SAVE_ENUM_FIELD(&g_settings.save_magic_slots[3],
                  "save_magic_slot_4", "Magic Slot 4",
                  "Stage the spell stored in magic inventory slot 4.",
                  kSaveMagicSlotLabels),
  SAVE_ENUM_FIELD(&g_settings.save_item_slots[0],
                  "save_item_slot_1", "Item Slot 1",
                  "Stage the item stored in inventory slot 1.", kSaveItemLabels),
  SAVE_ENUM_FIELD(&g_settings.save_item_slots[1],
                  "save_item_slot_2", "Item Slot 2",
                  "Stage the item stored in inventory slot 2.", kSaveItemLabels),
  SAVE_ENUM_FIELD(&g_settings.save_item_slots[2],
                  "save_item_slot_3", "Item Slot 3",
                  "Stage the item stored in inventory slot 3.", kSaveItemLabels),
  SAVE_ENUM_FIELD(&g_settings.save_item_slots[3],
                  "save_item_slot_4", "Item Slot 4",
                  "Stage the item stored in inventory slot 4.", kSaveItemLabels),
  SAVE_ENUM_FIELD(&g_settings.save_item_slots[4],
                  "save_item_slot_5", "Item Slot 5",
                  "Stage the item stored in inventory slot 5.", kSaveItemLabels),
  SAVE_ENUM_FIELD(&g_settings.save_item_slots[5],
                  "save_item_slot_6", "Item Slot 6",
                  "Stage the item stored in inventory slot 6.", kSaveItemLabels),
  SAVE_ENUM_FIELD(&g_settings.save_item_slots[6],
                  "save_item_slot_7", "Item Slot 7",
                  "Stage the item stored in inventory slot 7.", kSaveItemLabels),
  SAVE_ENUM_FIELD(&g_settings.save_item_slots[7],
                  "save_item_slot_8", "Item Slot 8",
                  "Stage the item stored in inventory slot 8.", kSaveItemLabels),
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
  SAVE_ACTION_SETTING("save_apply_session", "Apply for session",
                      "Apply only to live SRAM; use a natural return to title, not Restart."),
  SAVE_ACTION_SETTING("save_apply_persist", "Apply and save",
                      "Back up and save staged fields; then Restart Game and Continue."),
  SAVE_ACTION_SETTING("save_import", "Import save",
                      "Import saves/import.srm, then saves/import.ini, or AR_SAVE_IMPORT."),
  SAVE_ACTION_SETTING("save_export_srm", "Export native SRAM",
                      "Export the current exact image to saves/export.srm."),
  SAVE_ACTION_SETTING("save_export_ini", "Export structured INI",
                      "Export the current lossless image to saves/export.ini."),
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
              kSettingCat_Cheats, 0, 0, 255, ParseInfHp, NULL),
  BOOL_SETTING(cheat_freeze_timer, "AR_FREEZE_TIMER", "Freeze timer",
               "Pin the action timer until the boss tally drain is detected.",
               kSettingCat_Cheats, 0, false, NULL, NULL),
  { "cheat_moonjump", "AR_MOONJUMP", "Moonjump",
    "Hold the game's normal jump button to fly upward while enabled.",
    kSettingType_Bool, kApply_Passive, kSettingCat_Cheats,
    &g_settings.cheat_moonjump, 0, 0, 1, 1, false, NULL, 0,
    NULL, NULL, ParseMoonjumpLegacy, NULL },
  INT_SETTING(cheat_moonjump_speed, "AR_MOONJUMP_SPEED", "Moonjump speed",
              "Pixels moved upward per frame while jump is held.",
              kSettingCat_Cheats, 6, 1, 255, NULL, NULL),
  { "cheat_no_knockback", "AR_NO_KNOCKBACK", "No knockback",
    "Full invulnerability using the game's own i-frames: hits register no "
    "damage, knockback, or hitstun.",
    kSettingType_Bool, kApply_Passive, kSettingCat_Cheats,
    &g_settings.cheat_no_knockback, 0, 0, 1, 1, false, NULL, 0,
    NULL, NULL, NULL, NULL },
  BOOL_SETTING(cheat_magic_cycle, "AR_MAGIC_CYCLE", "Cycle magic spell",
               "Arm the Cycle magic spell control (Controls tab) to step "
               "through the spells you have unlocked during an action stage, "
               "reloading that spell's OBJ tiles. A cheat badge is shown "
               "while this is on.",
               kSettingCat_Cheats, 0, false, NULL, NULL),
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
  BOOL_SETTING(ws_bgrefresh, "AR_WS_BGREFRESH", "Legacy BG margin refresh",
               "Load-only compatibility alias; action world margins now use "
               "the bounded HLE provider.",
               kSettingCat_Widescreen, 1, false, NULL, NULL),
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

bool Settings_IsAvailable(const SettingDesc *desc) {
  /* Cheat values are intentionally stageable from every game state. Their
   * runtime hooks decide when an effect applies; editing is never mode-gated. */
  return desc &&
         (desc->category == kSettingCat_Cheats ||
          !desc->available || desc->available());
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

bool Settings_IsMenuVisible(const SettingDesc *desc) {
  if (!desc || !desc->key) return false;
  if (Settings_IsLoadOnly(desc)) return false;
  /* Developer-only rows collapse out of the menu unless explicitly enabled.
   * Checked first so a debug row is hidden regardless of its category rules. */
  if (Settings_IsDebugOnly(desc) && !g_settings.show_debug_settings)
    return false;
  if (desc->category == kSettingCat_Extras &&
      (!strcmp(desc->key, "warp_target") ||
       !strcmp(desc->key, "warp_now") ||
       !strcmp(desc->key, "save_state") ||
       !strcmp(desc->key, "load_state")))
    return false;
  /* input_bind_page and save_editor_page are still real, persisted settings,
   * but the overlay now drives them from its tab bar rather than from a row
   * the player scrolls to — so they no longer list themselves. */
  if (!strcmp(desc->key, "input_bind_page") ||
      !strcmp(desc->key, "save_editor_page"))
    return false;

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

  /* Save storage/safety controls and the apply/import/export commands now live
   * on the dedicated Actions page instead of repeating on every payload page. */
  if (desc->type == kSettingType_Action ||
      !strcmp(desc->key, "save_backend") ||
      !strcmp(desc->key, "save_edit_armed") ||
      !strcmp(desc->key, "save_autobackup"))
    return g_settings.save_editor_page == kSaveEditorPage_Actions;

  switch (g_settings.save_editor_page) {
    case kSaveEditorPage_Progress:
      return !strncmp(desc->key, "save_prog_", 10) ||
             !strcmp(desc->key, "save_death_heim_state") ||
             !strcmp(desc->key, "save_professional_mode");
    case kSaveEditorPage_Status:
      return !strcmp(desc->key, "save_player_name") ||
             !strncmp(desc->key, "save_master_", 12) ||
             !strcmp(desc->key, "save_lives") ||
             !strncmp(desc->key, "save_angel_", 11) ||
             !strcmp(desc->key, "save_message_speed");
    case kSaveEditorPage_Magic:
      return !strcmp(desc->key, "save_equipped_magic") ||
             !strncmp(desc->key, "save_magic_slot_", 16);
    case kSaveEditorPage_Items:
      return !strncmp(desc->key, "save_item_slot_", 15);
    case kSaveEditorPage_Scores:
      return !strncmp(desc->key, "save_score_", 11);
    default:
      return false;
  }
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
      desc->category == kSettingCat_RandoSim)
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

  /* Same reasoning for the CRT tab: the master toggle is the player control,
   * while the seven numeric knobs behind it author the look. Their defaults
   * were tuned by eye and are what the effect is meant to look like, so a
   * player has no reason to move them — but a developer adjusting the tube
   * wants them all on one panel. */
  if (desc->category == kSettingCat_Crt && desc->type == kSettingType_Int)
    return true;

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
    /* The modern renderer is required for widescreen and every 3D mode; there
     * is no reason for a player to disable it. */
    "new_renderer",
  };
  for (size_t i = 0; i < sizeof(kDebugKeys) / sizeof(kDebugKeys[0]); i++)
    if (!strcmp(desc->key, kDebugKeys[i])) return true;

  return false;
}

const char *Settings_ApplyKindName(SettingApplyKind apply) {
  switch (apply) {
    case kApply_Passive: return "Live";
    case kApply_Callback: return "Live callback";
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
  if (!g_ws_active)
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
  FILE *file = fopen(path, "r");
  if (!file) {
    if (!missing_ok || errno != ENOENT)
      fprintf(stderr, "[settings] cannot read %s: %s\n", path,
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
      fprintf(stderr, "[settings] %s:%d: expected key = value\n",
              path, line_number);
      success = false;
      continue;
    }
    *equals = 0;
    TextParse_TrimRight(key);
    char *value = TextParse_TrimLeft(equals + 1);
    TextParse_StripInlineComment(value);

    /* Removed menu state may remain in settings.ini until the next save. */
    if (!strcmp(key, "cheat_moonjump_button")) continue;

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
      fprintf(stderr, "[settings] %s:%d: unknown key '%s' ignored\n",
              path, line_number, key);
      continue;
    }
    if (desc->type == kSettingType_Action) {
      fprintf(stderr, "[settings] %s:%d: action key '%s' is not persistent\n",
              path, line_number, key);
      success = false;
      continue;
    }
    bool applied = boot
        ? ApplyBootLayerValue(desc, value, rank, false)
        : Settings_SetText(desc, value) != kSettingChange_Rejected;
    if (!applied) {
      fprintf(stderr, "[settings] %s:%d: invalid value '%s' for %s\n",
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
  SettingsChangeObserver observer = s_change_observer;
  s_change_observer = NULL;
  memset(&g_settings, 0, sizeof(g_settings));
  s_boot_display_rank = 0;
  s_boot_widescreen_rank = 0;
  s_boot_display_mode = kDisplayMode_WideFull;
  s_boot_display_from_environment = false;

  for (int i = 0; i < g_setting_desc_count; i++)
    SetSettingDefault(&g_setting_descs[i]);

  for (int i = 0; i < g_setting_desc_count; i++) {
    if (s_config_layer[i].present)
      ApplyBootLayerValue(&g_setting_descs[i], s_config_layer[i].text, 1,
                          s_config_layer[i].legacy_env_syntax);
  }

  Settings_LoadInternal(path, true, 2, true);

  for (int i = 0; i < g_setting_desc_count; i++) {
    const SettingDesc *desc = &g_setting_descs[i];
    const char *text = desc->env ? getenv(desc->env) : NULL;
    if (text)
      ApplyBootLayerValue(desc, text, 3,
                          Settings_UsesLegacyEnvironmentSyntax(desc));
  }

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
    fprintf(stderr, "[display] AR_DISPLAY_MODE=%d -> %s%s\n",
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

void Settings_SetPersistenceEnabled(bool enabled) {
  s_persistence_enabled = enabled;
}

bool Settings_Save(const char *path) {
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
      fprintf(stderr,
              "[settings] persistence disabled — not writing %s "
              "(diagnostic run)\n",
              path);
    }
    return true;  /* not an error: the caller's intent is satisfied */
  }
  size_t path_length = strlen(path);
  char *temporary = (char *)malloc(path_length + 5);
  if (!temporary) return false;
  memcpy(temporary, path, path_length);
  memcpy(temporary + path_length, ".tmp", 5);

  FILE *file = fopen(temporary, "w");
  if (!file) {
    fprintf(stderr, "[settings] cannot write %s: %s\n", temporary,
            strerror(errno));
    free(temporary);
    return false;
  }

  bool success = fprintf(file,
      "# ActRaiser Recompiled user settings\n"
      "# Generated from the live descriptor registry; config.ini remains developer-owned.\n\n") >= 0;
  char value[512];
  for (int i = 0; success && i < g_setting_desc_count; i++) {
    const SettingDesc *desc = &g_setting_descs[i];
    if (desc->type == kSettingType_Action ||
        Settings_IsLoadOnly(desc))
      continue;
    /* CUSTOM is derived from the individual widescreen rows. Omitting the
     * preset lets those rows reconstruct it without a contradictory action. */
    if (desc->field == &g_settings.display_mode &&
        g_settings.display_mode == kDisplayMode_Custom)
      continue;
    Settings_FormatValue(desc, value, sizeof(value));
    success = fprintf(file, "%s = %s\n", desc->key, value) >= 0;
  }
  if (fflush(file) != 0 || ferror(file)) success = false;
  if (success && !Settings_FlushFileData(file)) success = false;
  if (fclose(file) != 0) success = false;

  if (success && !AtomicReplaceFile(temporary, path)) {
    fprintf(stderr, "[settings] cannot replace %s: %s\n", path,
            strerror(errno));
    success = false;
  }
  /* Best-effort: make the rename itself durable. */
  if (success) Settings_SyncContainingDirectory(path);
  if (!success) remove(temporary);
  free(temporary);
  return success;
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
  g_settings.ws_skypalace_bg      = corrections;
  g_settings.ws_sprites           = corrections;
  g_settings.ws_margin_objects    = corrections;
  g_settings.ws_margin_activation = corrections;
  g_settings.ws_bg2_padding       = corrections;
  g_settings.ws_sim_sprites       = corrections;
}

void Settings_ReconcileDisplayModeAfterGeometryChange(int previous_mode) {
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
  if (g_settings.display_mode == kDisplayMode_43) return g_ws_extra;
  return g_ws_extra - g_ws_display_extra;
}

int Settings_VisibleWidth(void) {
  return (g_settings.display_mode == kDisplayMode_43)
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

bool Settings_HdReplacementsAvailable(void) {
  return s_hd_replacements_available;
}

/* Backing pixels per window point (SDL_GetWindowPixelDensity), pushed from
 * main.c whenever the window moves display or changes scale. 1.0 on a
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
