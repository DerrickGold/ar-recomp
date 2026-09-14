#include "settings_overlay_localization.h"

#include <stdio.h>
#include <string.h>

#include "input_map.h"
#include "host/host_display_status.h"
#include "localization/interface_text.h"

static int CopyCaption(char *buffer, int capacity, const char *text) {
  size_t bytes = strlen(text);
  if (bytes < (size_t)capacity) memcpy(buffer, text, bytes + 1);
  else {
    buffer[0] = 0;
    (void)ArInterfaceText_Append(buffer, (size_t)capacity, text, bytes);
  }
  return (int)bytes;
}

/* Format catalog templates as data, never as printf strings. The caller keeps
 * the authoritative formatter as a fallback if a bounded substitution fails. */
static bool FormatValue(ArUiLocale locale, const char *key, char *buffer,
                        int capacity, const ArUiTextArgument *args, size_t count) {
  const char *message = ArUiCatalog_Text(locale, key, NULL);
  return message[0] && ArUiCatalog_Format(buffer, (size_t)capacity,
                                          message, args, count);
}

static bool BindingValue(ArUiLocale locale, uint32 binding,
                          char *buffer, int capacity) {
  int kind = INPUT_BIND_KIND(binding), code = INPUT_BIND_CODE(binding);
  const char *name = InputMap_BindingName(binding);
  char key[64], number[24];
  const char *message = "overlay.binding.unbound";
  snprintf(number, sizeof(number), "%d", code);
  if (kind == kInputBind_Key) {
    message = name && name[0] ? "overlay.binding.key" : "overlay.binding.key_code";
  } else if (kind == kInputBind_PadButton || kind == kInputBind_PadAxis) {
    snprintf(key, sizeof(key), "overlay.binding.%s.%d.%d",
             kind == kInputBind_PadButton ? "button" : "axis", code,
             kind == kInputBind_PadAxis && INPUT_BIND_NEG(binding));
    name = ArUiCatalog_Text(locale, key, name);
    message = name && name[0] ? "overlay.binding.pad" :
        kind == kInputBind_PadButton ? "overlay.binding.button_code"
                                    : "overlay.binding.axis_code";
  }
  const ArUiTextArgument args[] = {
    {"name", name ? name : ""}, {"code", number},
    {"sign", INPUT_BIND_NEG(binding) ? "-" : "+"},
  };
  return FormatValue(locale, message, buffer, capacity, args, 3);
}

static const char *DescriptorText(ArUiLocale locale, const SettingDesc *desc,
                                   const char *part, const char *fallback) {
  char key[192];
  int bytes = snprintf(key, sizeof(key), "setting.%s.%s", desc->key, part);
  if (bytes < 0 || (size_t)bytes >= sizeof(key)) return fallback;
  return ArUiCatalog_Text(locale, key, fallback);
}

const char *SettingsOverlay_LocalizedLabel(ArUiLocale locale,
                                         const SettingDesc *desc) {
  return desc ? DescriptorText(locale, desc, "label", desc->label) : "";
}

const char *SettingsOverlay_LocalizedHelp(ArUiLocale locale,
                                        const SettingDesc *desc) {
  return desc ? DescriptorText(locale, desc, "help", desc->tooltip) : "";
}

const char *SettingsOverlay_LocalizedGameChangeHeading(
    ArUiLocale locale, SettingGameChangeKind kind) {
  switch (kind) {
    case kSettingGameChange_OriginalBugFix:
      return ArUiCatalog_Text(locale, "overlay.group.original_bug_fixes",
                              "Original game bug fixes");
    case kSettingGameChange_QualityOfLife:
      return ArUiCatalog_Text(locale, "overlay.group.quality_of_life",
                              "Quality-of-life improvements");
    case kSettingGameChange_None:
    case kSettingGameChange_Count:
    default:
      return "";
  }
}

int SettingsOverlay_LocalizedValue(ArUiLocale locale, const SettingDesc *desc,
                                   char *buffer, int capacity) {
  if (!desc || !buffer || capacity <= 0) return 0;
  const char *text = NULL;
  if (desc->type == kSettingType_Binding) {
    if (BindingValue(locale, *(const uint32 *)desc->field, buffer, capacity))
      return (int)strlen(buffer);
    return InputMap_DescribeBinding(buffer, capacity, *(const uint32 *)desc->field);
  }
  if (desc->field == &g_settings.input_gamepad_slot) {
    int slot = g_settings.input_gamepad_slot, connected = InputMap_GamepadCount();
    char number[24];
    snprintf(number, sizeof(number), "%d", slot);
    const ArUiTextArgument args[] = {
      {"slot", number}, {"name", InputMap_GamepadName(slot > 0 ? slot - 1 : 0)},
    };
    const char *key = slot <= 0
        ? (connected ? "overlay.gamepad.first_named" : "overlay.gamepad.first")
        : (slot > connected ? "overlay.gamepad.disconnected" : "overlay.gamepad.named");
    if (FormatValue(locale, key, buffer, capacity, args, 2))
      return (int)strlen(buffer);
  }
  if (desc->field == &g_settings.interface_language) {
    /* Autonyms stay identifiable even when the currently selected UI language
     * is unfamiliar. Hosts without Unicode can display the serialized code. */
    int value = g_settings.interface_language;
    if (value >= 0 && value < kArUiLocale_Count) {
      char key[32];
      snprintf(key,sizeof(key),"interface.autonym.%s",ArUiCatalog_LocaleTag((ArUiLocale)value));
      text = ArUiCatalog_Text(locale,key,NULL);
    }
  } else if (desc->field == &g_settings.hud_scale_percent && !g_settings.hud_scale_percent) {
    text = ArUiCatalog_Text(locale, "overlay.value.match_game", NULL);
  } else if (desc->field == &g_settings.menu_scale_percent && !g_settings.menu_scale_percent) {
    text = ArUiCatalog_Text(locale, "overlay.value.auto", NULL);
  } else if ((desc->apply == kApply_Save && desc->type == kSettingType_Int &&
              desc->format && !*(const int *)desc->field) ||
             (desc->field == g_settings.save_player_name &&
              !g_settings.save_player_name[0])) {
    text = ArUiCatalog_Text(locale, "overlay.value.leave", NULL);
  } else if (desc->type == kSettingType_Bool && !desc->format) {
    text = ArUiCatalog_Text(locale,
        *(const bool *)desc->field ? "common.on" : "common.off", NULL);
  } else if (desc->type == kSettingType_Action && !desc->format) {
    text = ArUiCatalog_Text(locale, "common.run", NULL);
  } else if (desc->field == &g_settings.localization_presentation &&
             g_settings.localization_content != 0) {
    text = ArUiCatalog_Text(locale, "setting.localization_presentation.required", NULL);
  } else if (desc->type == kSettingType_Enum) {
    /* Only built-in enum values have catalog IDs. External language packs,
     * device names and other custom formatters keep their own display text. */
    int value = *(const int *)desc->field;
    if (value >= 0 && value < desc->enum_count) {
      char part[32];
      snprintf(part, sizeof(part), "value.%d", value);
      text = DescriptorText(locale, desc, part, NULL);
      if (text && !text[0]) text = NULL;
    }
  }
  if (desc->field == &g_settings.display_mode) {
    char part[32];
    snprintf(part, sizeof(part), "short.%d", g_settings.display_mode);
    text = DescriptorText(locale, desc, part, text);
  }
  if (desc->field == &g_settings.refresh_mode &&
      g_settings.refresh_mode == kRefreshMode_Vsync) {
    /* Report the renderer's actual state, not the requested setting. */
    if (!HostDisplayStatus_VsyncActive()) {
      text = ArUiCatalog_Text(locale, "overlay.value.vsync_unavailable", NULL);
    } else if (HostDisplayStatus_NominalRefreshHz() > 0) {
      char number[24];
      snprintf(number, sizeof(number), "%d", HostDisplayStatus_NominalRefreshHz());
      const ArUiTextArgument arg = {"hz", number};
      if (FormatValue(locale, "overlay.value.vsync_hz", buffer, capacity, &arg, 1))
        return (int)strlen(buffer);
    }
  }
  return text ? CopyCaption(buffer, capacity, text)
              : Settings_FormatValue(desc, buffer, capacity);
}
