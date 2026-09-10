#include "settings_overlay_layers_localization.h"

#include <stdio.h>
#include <string.h>
#include "localization/interface_text.h"

static void Caption(char *out, const char *text) {
  size_t bytes = strlen(text);
  if (bytes < kOverlayLayerCaptionBytes) memcpy(out, text, bytes + 1);
  else {
    out[0] = 0;
    (void)ArInterfaceText_Append(out, kOverlayLayerCaptionBytes, text, bytes);
  }
}

static const char *Indexed(ArUiLocale locale, const char *family,
                           int value, const char *fallback) {
  char key[128];
  snprintf(key, sizeof(key), "overlay.layer.%s.%d", family, value);
  return ArUiCatalog_Text(locale, key, fallback);
}

static void Format(char *out, ArUiLocale locale, const char *key,
                    const ArUiTextArgument *args, size_t count) {
  const char *text = ArUiCatalog_Text(locale, key, NULL);
  /* The prefilled native caption survives an unknown key or rejected format. */
  if (text[0]) (void)ArUiCatalog_Format(out, kOverlayLayerCaptionBytes, text, args, count);
}

void SettingsOverlay_LocalizedDioramaRow(ArUiLocale locale,
    const DioramaEditorRow *row, SettingsOverlayLayerText *out) {
  if (!out) return;
  *out = (SettingsOverlayLayerText){.help = ""};
  if (!row) return;
  Caption(out->label, row->label);
  Caption(out->value, row->value);
  const char *native_help = DioramaLayerEditor_RowHelp(row->kind, row->param, row->strategy);
  const char *section = DioramaLayerOrder_SectionToken(row->section);
  char room[8];
  snprintf(room, sizeof(room), "%02X", row->map_number);
  const ArUiTextArgument scope[] = {{"room", room}, {"section", section ? section : ""}};
  switch (row->kind) {
    case kDioramaEditorRow_Header:
      out->help = ArUiCatalog_Text(locale, "overlay.layer.diorama.help.header", native_help);
      if (!row->room_live) {
        Caption(out->label, ArUiCatalog_Text(locale, "overlay.layer.diorama.enter", row->label));
      } else {
        Format(out->label, locale, section ? "overlay.layer.room_section" : "overlay.layer.room", scope, 2);
        Caption(out->value, ArUiCatalog_Text(locale, "overlay.layer.here", row->value));
      }
      break;
    case kDioramaEditorRow_ResetRoom:
      out->help = ArUiCatalog_Text(locale, "overlay.layer.diorama.help.reset", native_help);
      Format(out->label, locale, section ? "overlay.layer.reset_section" : "overlay.layer.reset_room", scope, 2);
      Caption(out->value, ArUiCatalog_Text(locale, "common.reset", row->value));
      break;
    case kDioramaEditorRow_Plane: {
      out->help = Indexed(locale, "diorama.help.shape", row->strategy, native_help);
      const char *shape = Indexed(locale, "diorama.shape", row->strategy,
                                   DioramaLayerOrder_StrategyName(row->strategy));
      if (row->strategy == kDioramaDepth_Flat) Caption(out->value, shape);
      else {
        char number[32];
        snprintf(number, sizeof(number), "%.2f", (double)row->shape_depth);
        const ArUiTextArgument args[] = {{"shape", shape}, {"depth", number}};
        Format(out->value, locale, "overlay.layer.diorama.shape_depth", args, 2);
      }
      break;
    }
    case kDioramaEditorRow_Param:
    case kDioramaEditorRow_ParamEnum:
      out->help = Indexed(locale, "diorama.help.param", row->param, native_help);
      Caption(out->label, Indexed(locale, "diorama.param", row->param, row->label));
      if (row->param == kDioramaEditorParam_Copies && row->strategy == kDioramaDepth_Voxel)
        Caption(out->label, ArUiCatalog_Text(locale, "overlay.layer.diorama.slices", row->label));
      if (row->param == kDioramaEditorParam_Direction)
        Caption(out->value, Indexed(locale, "diorama.direction", row->direction, row->value));
      if (row->param == kDioramaEditorParam_Source && row->effective_source == kDioramaLayerSource_Captured)
        Caption(out->value, ArUiCatalog_Text(locale, "overlay.layer.diorama.captured", row->value));
      if (row->param == kDioramaEditorParam_TransparentFill) {
        if (!row->effective_transparent_fill_set)
          Caption(out->value, ArUiCatalog_Text(locale, "common.off", row->value));
        else if (row->effective_transparent_fill_kind == kDioramaTransparentFill_Black)
          Caption(out->value, ArUiCatalog_Text(locale, "overlay.layer.black", row->value));
      }
      break;
    default: out->help = native_help; break;
  }
}

void SettingsOverlay_LocalizedActionBgRow(ArUiLocale locale,
    const ActionBgTunerRow *row, SettingsOverlayLayerText *out) {
  if (!out) return;
  *out = (SettingsOverlayLayerText){.help = ""};
  if (!row) return;
  Caption(out->label, Indexed(locale, "action.label", row->kind, row->label));
  Caption(out->value, row->value);
  out->help = Indexed(locale, "action.help", row->kind, ActionBgTuner_RowHelp(row));
  const char *family = NULL, *key = NULL;
  switch (row->kind) {
    case kActionBgTunerRow_Header: {
      Caption(out->label, ArUiCatalog_Text(locale, row->room_live
          ? "overlay.layer.action.live" : "overlay.layer.action.enter", row->label));
      if (row->room_live) {
        char address[16];
        snprintf(address, sizeof(address), "%02X/%02X", row->map_group, row->map_number);
        const ArUiTextArgument args[] = {{"room", address}, {"state",
          ArUiCatalog_Text(locale, row->enum_value ? "overlay.layer.draft" : "overlay.layer.canonical", NULL)}};
        Format(out->value, locale, "overlay.layer.action.room_state", args, 2);
      }
      break;
    }
    case kActionBgTunerRow_Apply:
    case kActionBgTunerRow_Guides:
    case kActionBgTunerRow_IgnoreSideBounds:
    case kActionBgTunerRow_IgnoreVerticalBounds:
      key = row->enum_value ? "common.on" : "common.off"; break;
    case kActionBgTunerRow_Layer: {
      Caption(out->label, row->label); /* BG1/BG2 are authoring tokens. */
      const ArUiTextArgument args[] = {
        {"role", Indexed(locale, "action.role", row->role, "?")},
        {"source", Indexed(locale, "action.source", row->source, "?")},
      };
      Format(out->value, locale, row->expanded ? "overlay.layer.action.layer_open" : "overlay.layer.action.layer", args, 2);
      break;
    }
    case kActionBgTunerRow_BandHeader: {
      char first[16], last[16];
      snprintf(first, sizeof(first), "%u", row->first_row);
      snprintf(last, sizeof(last), "%u", row->end_row ? row->end_row - 1 : 0);
      const ArUiTextArgument args[] = {
        {"first", first}, {"last", last},
        {"anchor", Indexed(locale, "action.anchor", row->anchor, "?")},
        {"edge", Indexed(locale, "action.edge", row->enum_value, "?")},
      };
      Format(out->label, locale, "overlay.layer.action.band_rows", args, 4);
      if (row->expanded)
        Format(out->value, locale, "overlay.layer.action.edge_open", args, 4);
      else Caption(out->value, args[3].value);
      break;
    }
    case kActionBgTunerRow_Edge:
    case kActionBgTunerRow_BandEdge: family = "action.edge"; break;
    case kActionBgTunerRow_Motion:
    case kActionBgTunerRow_BandMotion: family = "action.motion"; break;
    case kActionBgTunerRow_BandAnchor: family = "action.anchor"; break;
    case kActionBgTunerRow_HorizontalMode:
    case kActionBgTunerRow_VerticalMode:
    case kActionBgTunerRow_BandMode: family = "action.extent"; break;
    case kActionBgTunerRow_BandDelete: key = "overlay.layer.delete"; break;
    case kActionBgTunerRow_BandAdd:
      key = row->selectable ? "overlay.layer.add" : "overlay.layer.full"; break;
    case kActionBgTunerRow_Print: key = "overlay.layer.log"; break;
    case kActionBgTunerRow_Reset: key = "common.reset"; break;
    default: break; /* Numeric quantities and technical tokens stay literal. */
  }
  if (family) Caption(out->value, Indexed(locale, family, row->enum_value, row->value));
  if (key) Caption(out->value, ArUiCatalog_Text(locale, key, row->value));
}
