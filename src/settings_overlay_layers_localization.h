#ifndef AR_SETTINGS_OVERLAY_LAYERS_LOCALIZATION_H
#define AR_SETTINGS_OVERLAY_LAYERS_LOCALIZATION_H

#include "action/action_bg_tuner.h"
#include "diorama/diorama_layer_editor.h"
#include "localization/ui_catalog.h"

/* Bounded presentation of an immutable editor row. Authoring tokens, numeric
 * values and source addresses remain literal; translated captions cannot alter
 * an edit target. No renderer, live-draft queries or allocations. */
enum { kOverlayLayerCaptionBytes = 256 };
typedef struct SettingsOverlayLayerText {
  char label[kOverlayLayerCaptionBytes];
  char value[kOverlayLayerCaptionBytes];
  const char *help;
} SettingsOverlayLayerText;

void SettingsOverlay_LocalizedDioramaRow(ArUiLocale locale,
    const DioramaEditorRow *row, SettingsOverlayLayerText *out);
void SettingsOverlay_LocalizedActionBgRow(ArUiLocale locale,
    const ActionBgTunerRow *row, SettingsOverlayLayerText *out);

#endif
