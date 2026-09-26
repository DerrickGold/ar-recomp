#include "settings_overlay/layers/layer_menu.h"

#include <stdio.h>
#include <string.h>
#include "settings_overlay/layers/layer_localization.h"
#include "settings_overlay/layers/layer_palette.h"
#include "settings_overlay/settings_overlay_localization.h"
#include "action/action_bg_tuner.h"
#include "diorama/diorama_layer_editor.h"
#include "localization/interface_text.h"

/* The layer editor owns expansion, edits, palette commits and presentation.
 * Host hooks supply the live room and persistent table without a renderer dependency. */
static SettingsOverlayLayerTableFn s_layer_table_provider;
static SettingsOverlayLayerRoomFn s_layer_room_provider;
static SettingsOverlayLayerSaveFn s_layer_save_provider;
static SettingsOverlayLayerPaletteFn s_layer_palette_provider;
/* Which plane's parameters are expanded, or -1. Held rather than derived from
 * the cursor so the expansion does not collapse while the player steps DOWN
 * through its own parameter rows. */
static int s_layer_plane = -1;

static const char *Ui(const char *key) {
  return ArUiCatalog_Text(SettingsOverlay_InterfaceLocale(), key, key);
}
void SettingsOverlay_SetLayerEditorHooks(SettingsOverlayLayerTableFn table,
                                         SettingsOverlayLayerRoomFn room,
                                         SettingsOverlayLayerSaveFn save) {
  s_layer_table_provider = table;
  s_layer_room_provider = room;
  s_layer_save_provider = save;
}

void SettingsOverlay_SetLayerPaletteProvider(SettingsOverlayLayerPaletteFn provider) {
  s_layer_palette_provider = provider;
  if (!provider) {
    SettingsOverlayPalette_Close();
    SettingsOverlayPalette_ReleaseTexture();
  }
}

/* Row-name suffix for SettingsOverlay_SelectedKey, so a test can navigate to
 * "bg2hi.copies" rather than counting keypresses through a list whose shape
 * changes with the active shape. These names are a TEST seam, not the manifest
 * grammar -- the file's own keys live in diorama_layer_order.c -- but they are
 * spelled the same so a failure message reads against the file. */
static const char kLayerResetRoomKey[] = "layer_reset_room";
static const char *LayerParamKey(DioramaEditorParam param) {
  switch (param) {
  case kDioramaEditorParam_Depth:
    return "depth";
  case kDioramaEditorParam_Copies:
    return "copies";
  case kDioramaEditorParam_Density:
    return "density";
  case kDioramaEditorParam_Direction:
    return "dir";
  case kDioramaEditorParam_Z:
    return "z";
  case kDioramaEditorParam_Alpha:
    return "alpha";
  case kDioramaEditorParam_TransparentFill:
    return "transparent";
  case kDioramaEditorParam_Source:
    return "source";
  case kDioramaEditorParam_Order:
    return "order";
  case kDioramaEditorParam_None:
  default:
    return "";
  }
}

enum { kLayerMenuRowMax = kDioramaEditorRowMax };
_Static_assert(kActionBgTunerRowMax <= kLayerMenuRowMax,
               "shared Layers row buffer must fit the BG tuner");

typedef enum LayerMenuRowOwner {
  kLayerMenuRow_Diorama = 0,
  kLayerMenuRow_ActionBg,
} LayerMenuRowOwner;

typedef struct LayerMenuRow {
  LayerMenuRowOwner owner;
  char key[48];
  bool nested;
  bool selectable;
  bool separator_before;
  union {
    DioramaEditorRow diorama;
    ActionBgTunerRow action_bg;
  } source;
} LayerMenuRow;

static bool IsBgTuner(int tab) { return tab == kDioramaEditorLevelCount; }

static int LayerEditorRows(int tab, DioramaEditorRow *rows, int capacity) {
  DioramaEditorContext context;
  memset(&context, 0, sizeof(context));
  context.selected_plane = s_layer_plane;
  if (s_layer_room_provider)
    context.room_live =
        s_layer_room_provider(&context.map_group, &context.map_number, &context.section);
  const DioramaLayerOrderTable *table = s_layer_table_provider ? s_layer_table_provider() : NULL;
  return DioramaLayerEditor_BuildRows(table, &context, tab, rows, capacity);
}

static int LayerMenuRows(int tab, LayerMenuRow *out, int capacity) {
  if (!out || capacity <= 0) return 0;
  int count = 0;
  if (IsBgTuner(tab)) {
    ActionBgTunerRow rows[kActionBgTunerRowMax];
    int n = ActionBgTuner_BuildRows(rows, kActionBgTunerRowMax);
    for (int i = 0; i < n && count < capacity; i++) {
      LayerMenuRow *dst = &out[count++];
      *dst = (LayerMenuRow){.owner = kLayerMenuRow_ActionBg};
      dst->source.action_bg = rows[i];
      snprintf(dst->key, sizeof(dst->key), "%s", rows[i].key);
      dst->nested = rows[i].nested;
      dst->selectable = rows[i].selectable;
      dst->separator_before = rows[i].separator_before;
    }
    return count;
  }

  DioramaEditorRow rows[kDioramaEditorRowMax];
  int n = LayerEditorRows(tab, rows, kDioramaEditorRowMax);
  for (int i = 0; i < n && count < capacity; i++) {
    LayerMenuRow *dst = &out[count++];
    *dst = (LayerMenuRow){.owner = kLayerMenuRow_Diorama};
    dst->source.diorama = rows[i];
    dst->nested = rows[i].nested;
    dst->selectable = rows[i].selectable;
    dst->separator_before = rows[i].kind == kDioramaEditorRow_ResetRoom;
    if (rows[i].kind == kDioramaEditorRow_ResetRoom) {
      snprintf(dst->key, sizeof(dst->key), "%s", kLayerResetRoomKey);
    } else if (rows[i].kind != kDioramaEditorRow_Header) {
      const char *token = DioramaLayerOrder_PlaneToken(rows[i].plane);
      if (token && rows[i].kind == kDioramaEditorRow_Plane)
        snprintf(dst->key, sizeof(dst->key), "%s", token);
      else if (token)
        snprintf(dst->key, sizeof(dst->key), "%s.%s", token, LayerParamKey(rows[i].param));
    }
  }
  return count;
}

/* Resolve captions only for rows actually drawn, not every navigation/count
 * probe. The immutable source row remains the authority for both text and edits. */
static void LocalizeLayerRow(const LayerMenuRow *row, SettingsOverlayLayerText *text) {
  if (row->owner == kLayerMenuRow_ActionBg)
    SettingsOverlay_LocalizedActionBgRow(SettingsOverlay_InterfaceLocale(), &row->source.action_bg,
                                         text);
  else
    SettingsOverlay_LocalizedDioramaRow(SettingsOverlay_InterfaceLocale(), &row->source.diorama,
                                        text);
}

/* The selected row, or NULL when the cursor is on a header (which is not
 * selectable) or the section is not the editor. */
static const LayerMenuRow *SelectedLayerRow(int tab, int selected, LayerMenuRow *rows, int capacity,
                                            int *out_count) {
  int n = LayerMenuRows(tab, rows, capacity);
  if (out_count) *out_count = n;
  if (selected < 0 || selected >= n) return NULL;
  return &rows[selected];
}

void LayerMenu_ResetNavigation(void) { s_layer_plane = -1; }
int LayerMenu_Count(int tab) {
  LayerMenuRow rows[kLayerMenuRowMax];
  return LayerMenuRows(tab, rows, kLayerMenuRowMax);
}
bool LayerMenu_RowExists(int tab, int selected) {
  return selected >= 0 && selected < LayerMenu_Count(tab);
}
bool LayerMenu_RowSelectable(int tab, int selected) {
  LayerMenuRow rows[kLayerMenuRowMax];
  const LayerMenuRow *row = SelectedLayerRow(tab, selected, rows, kLayerMenuRowMax, NULL);
  return !row || row->selectable;
}
const char *LayerMenu_Key(int tab, int selected) {
  static char key[48];
  LayerMenuRow rows[kLayerMenuRowMax];
  const LayerMenuRow *row = SelectedLayerRow(tab, selected, rows, kLayerMenuRowMax, NULL);
  if (!row || !row->key[0]) return "";
  snprintf(key, sizeof(key), "%s", row->key);
  return key;
}

/* ── Layer editor dispatch ───────────────────────────────────────────────
 *
 * Key handling and drawing run synchronously on the main thread, so an override
 * edit is ordered before the next frame reads it. This is what makes live A/B
 * editing safe without locking.
 *
 * Returns true when the row belonged to the editor, so the ordinary descriptor
 * paths are skipped. */
static DioramaPlaneOverride *LayerPlaneForRow(const DioramaEditorRow *row, bool create) {
  if (!row || row->plane < 0 || !s_layer_table_provider) return NULL;
  DioramaLayerOrderTable *table = s_layer_table_provider();
  if (!table) return NULL;
  DioramaRoomOverride *room = NULL;
  if (create) {
    /* The first committed edit creates its room entry. A full table returns
     * NULL and is reported rather than dropping the edit silently. */
    room = DioramaLayerOrder_FindOrAddSection(table, row->map_group, row->map_number, row->section);
  } else {
    /* Clear only an already-existing record; reset/preview paths must not
     * allocate one of the bounded section slots. */
    room =
        DioramaLayerOrder_FindMutableSection(table, row->map_group, row->map_number, row->section);
  }
  if (!room) return NULL;
  return &room->planes[row->plane];
}

static void LayerPruneEmptySection(const DioramaEditorRow *row) {
  if (!row || !s_layer_table_provider) return;
  DioramaLayerOrderTable *table = s_layer_table_provider();
  if (!table) return;
  const DioramaRoomOverride *room =
      DioramaLayerOrder_FindSection(table, row->map_group, row->map_number, row->section);
  if (room && !DioramaLayerOrder_RoomIsActive(room))
    DioramaLayerOrder_ResetSection(table, row->map_group, row->map_number, row->section);
}

static void LayerSaveEdit(void) {
  if (s_layer_save_provider && !s_layer_save_provider())
    SettingsOverlay_SetStatus(Ui("overlay.status.save_failed"));
}

static void CommitLayerPalette(const DioramaEditorRow *row, bool reset, uint8_t index) {
  DioramaPlaneOverride *plane = LayerPlaneForRow(row, !reset);
  if (reset) {
    if (plane) {
      DioramaLayerEditor_ClearParam(plane, kDioramaEditorParam_TransparentFill);
      LayerPruneEmptySection(row);
      SettingsOverlay_SetStatus(Ui("overlay.status.fill_cleared"));
      LayerSaveEdit();
    } else {
      SettingsOverlay_SetStatus(Ui("overlay.status.inherited"));
    }
    return;
  }
  if (!plane) {
    SettingsOverlay_SetStatus(Ui("overlay.status.no_room"));
    return;
  }
  plane->set_transparent_fill = true;
  plane->transparent_fill_kind = kDioramaTransparentFill_Cgram;
  plane->transparent_fill_cgram = index;
  SettingsOverlay_SetStatus(Ui("overlay.status.fill_applied"));
  LayerSaveEdit();
}

static bool LayerOpenPalette(const DioramaEditorRow *row) {
  uint16_t palette[kSettingsOverlayLayerPaletteEntries];
  if (!row || row->param != kDioramaEditorParam_TransparentFill || !s_layer_palette_provider ||
      !s_layer_palette_provider(palette)) {
    SettingsOverlay_SetStatus(Ui("overlay.status.palette_unavailable"));
    return false;
  }
  SettingsOverlayPalette_Open(row, palette, CommitLayerPalette);
  return true;
}

static void ReportActionBgTunerResult(ActionBgTunerResult result) {
  switch (result) {
  case kActionBgTunerResult_Changed:
    SettingsOverlay_SetStatus(Ui("overlay.status.draft_updated"));
    break;
  case kActionBgTunerResult_AtLimit:
    SettingsOverlay_SetStatus(Ui("overlay.status.at_limit"));
    break;
  case kActionBgTunerResult_Printed:
    SettingsOverlay_SetStatus(Ui("overlay.status.printed"));
    break;
  case kActionBgTunerResult_Reset:
    SettingsOverlay_SetStatus(Ui("overlay.status.draft_reset"));
    break;
  case kActionBgTunerResult_Unchanged:
  default:
    break;
  }
}

void LayerMenu_Change(int tab, int selected, int direction) {
  LayerMenuRow rows[kLayerMenuRowMax];
  const LayerMenuRow *row = SelectedLayerRow(tab, selected, rows, kLayerMenuRowMax, NULL);
  if (!row || !row->selectable) return; /* owned, nothing to do */
  if (row->owner == kLayerMenuRow_ActionBg) {
    if (row->source.action_bg.kind == kActionBgTunerRow_Print ||
        row->source.action_bg.kind == kActionBgTunerRow_Reset) {
      SettingsOverlay_SetStatus(Ui("overlay.status.activate"));
      return;
    }
    ReportActionBgTunerResult(ActionBgTuner_Change(&row->source.action_bg, direction));
    return;
  }
  const DioramaEditorRow *diorama = &row->source.diorama;

  if (diorama->kind == kDioramaEditorRow_ResetRoom) {
    SettingsOverlay_SetStatus(Ui("overlay.status.reset_room"));
    return;
  }

  DioramaPlaneOverride *plane = LayerPlaneForRow(diorama, true);
  if (!plane) {
    SettingsOverlay_SetStatus(Ui("overlay.status.no_room"));
    return;
  }

  if (diorama->kind == kDioramaEditorRow_Plane) {
    DioramaDepthStrategy next = DioramaLayerEditor_CycleStrategy(plane, direction);
    /* Expanding the plane the player just changed puts its parameters under the
     * cursor immediately, which is the next thing they want. */
    s_layer_plane = diorama->plane;
    char key[64];
    snprintf(key, sizeof(key), "overlay.layer.diorama.shape.%d", next);
    SettingsOverlay_SetStatus(ArUiCatalog_Text(SettingsOverlay_InterfaceLocale(), key,
                                               DioramaLayerOrder_StrategyName(next)));
    LayerSaveEdit();
    return;
  }

  /* A scoped row displays the renderer-resolved source, which may be inherited
   * from its base room. Seed a first local edit from that displayed value so
   * Right means "next source" rather than jumping from hidden Captured state. */
  if (diorama->param == kDioramaEditorParam_Source && !plane->set_source) {
    plane->source = diorama->effective_source;
    plane->set_source = true;
  }

  if (!DioramaLayerEditor_StepParam(plane, diorama->param, direction)) {
    SettingsOverlay_SetStatus(Ui("overlay.status.at_limit"));
    return;
  }
  LayerSaveEdit();
  return;
}

void LayerMenu_Activate(int tab, int selected) {
  LayerMenuRow rows[kLayerMenuRowMax];
  const LayerMenuRow *row = SelectedLayerRow(tab, selected, rows, kLayerMenuRowMax, NULL);
  if (!row || !row->selectable) return;
  if (row->owner == kLayerMenuRow_ActionBg) {
    ReportActionBgTunerResult(ActionBgTuner_Activate(&row->source.action_bg));
    return;
  }
  const DioramaEditorRow *diorama = &row->source.diorama;

  if (diorama->kind == kDioramaEditorRow_ResetRoom) {
    DioramaLayerOrderTable *table = s_layer_table_provider ? s_layer_table_provider() : NULL;
    if (!table) {
      SettingsOverlay_SetStatus(Ui("overlay.status.no_reset_room"));
      return;
    }
    DioramaLayerOrder_ResetPlaneOverridesSection(table, diorama->map_group, diorama->map_number,
                                                 diorama->section);
    s_layer_plane = -1;
    SettingsOverlay_SetStatus(Ui("overlay.status.planes_reset"));
    LayerSaveEdit();
    return;
  }

  if (diorama->kind == kDioramaEditorRow_Plane) {
    s_layer_plane = (s_layer_plane == diorama->plane) ? -1 : diorama->plane;
    return;
  }
  if (diorama->param == kDioramaEditorParam_TransparentFill) {
    (void)LayerOpenPalette(diorama);
    return;
  }
  /* A parameter row: confirm is one fine step up, matching what an Int
   * descriptor row does elsewhere in this menu. */
  LayerMenu_Change(tab, selected, +1);
}

void LayerMenu_Reset(int tab, int selected) {
  LayerMenuRow rows[kLayerMenuRowMax];
  const LayerMenuRow *row = SelectedLayerRow(tab, selected, rows, kLayerMenuRowMax, NULL);
  if (!row || !row->selectable) return;
  if (row->owner == kLayerMenuRow_ActionBg) {
    ReportActionBgTunerResult(ActionBgTuner_ResetRow(&row->source.action_bg));
    return;
  }
  const DioramaEditorRow *diorama = &row->source.diorama;
  if (diorama->kind == kDioramaEditorRow_ResetRoom) {
    LayerMenu_Activate(tab, selected);
    return;
  }

  DioramaPlaneOverride *plane = LayerPlaneForRow(diorama, false);
  if (!plane) {
    SettingsOverlay_SetStatus(Ui("overlay.status.inherited"));
    return;
  }
  if (diorama->kind == kDioramaEditorRow_Plane) {
    DioramaLayerEditor_ClearPlane(plane);
    SettingsOverlay_SetStatus(Ui("overlay.status.plane_cleared"));
  } else {
    DioramaLayerEditor_ClearParam(plane, diorama->param);
    SettingsOverlay_SetStatus(Ui("overlay.status.cleared"));
  }
  LayerPruneEmptySection(diorama);
  LayerSaveEdit();
  return;
}

int LayerMenu_DrawRows(const MenuLayout *layout, int tab, const MenuRowViewport *viewport) {
  const int right_x = viewport->x, right_width = viewport->width;
  const int first_row_y = viewport->first_y, value_right = viewport->value_right;
  const int selector_x = right_x + 12, label_x = right_x + 22, value_chars = 18;
  const uint32_t structure = kSteelBlue;
  LayerMenuRow rows[kLayerMenuRowMax];
  int n = LayerMenuRows(tab, rows, kLayerMenuRowMax);
  for (int i = 0; i < n; i++) {
    int row = i;
    if (row < viewport->top || row >= viewport->top + viewport->visible) continue;
    int y = first_row_y + (row - viewport->top) * kMenuRowHeight;
    const LayerMenuRow *entry = &rows[i];
    SettingsOverlayLayerText text;
    LocalizeLayerRow(entry, &text);
    /* An unselectable row is never drawn as selected, even when the cursor sits
     * on it -- which happens on a tab whose every row is a notice, since there
     * is nothing for SkipUnselectableRow to move to. Highlighting it with the
     * blinking cursor would invite a keypress that does nothing. */
    const bool selected = viewport->focused && row == viewport->selected && entry->selectable;

    /* A rule above the reset row, matching how the Save and Extras tabs fence
     * their destructive commands off from the settings above them. */
    if (entry->separator_before)
      FillLogicalRect(layout, right_x + 12, y - 3, right_width - 24, 1, ARGB(160, 190, 96, 76));
    if (selected) {
      FillLogicalRect(layout, right_x + 9, y - 2, right_width - 18, 11, kHighlight);
      FillLogicalRect(layout, right_x + 9, y - 2, 2, 11, kSelectYellow);
      DrawGlyph(layout, selector_x + viewport->cursor_offset, y, '>', kText_Warning);
    }

    /* A caption is structure, not a control, so it takes the panel's structure
     * color and no value styling. A nested parameter indents under its plane
     * and dims, so the eye reads the grouping without a box. */
    TextStyle style = viewport->focused ? kText_Normal : kText_Dim;
    int row_label_x = label_x + (entry->nested ? 3 * kGlyphSize : 0);
    if (!entry->selectable) {
      int shown = CappedTextLength(text.value, value_chars);
      int value_x = value_right - shown * kDebugGlyphWidth;
      DrawSmallTextN(layout, row_label_x, y + 1, text.label,
                     (value_x - row_label_x - 8) / kDebugGlyphWidth, structure);
      if (shown) DrawSmallTextN(layout, value_x, y + 1, text.value, shown, kGameGold);
      continue;
    }

    int shown = CappedTextLength(text.value, value_chars);
    int label_chars = (value_right - shown * kGlyphSize - 12 - row_label_x - 4) / kGlyphSize;
    if (label_chars < 1) label_chars = 1;
    DrawTextN(layout, row_label_x, y, text.label, label_chars,
              entry->nested && !selected ? kText_Dim : style);
    bool reset_row = (entry->owner == kLayerMenuRow_Diorama &&
                      entry->source.diorama.kind == kDioramaEditorRow_ResetRoom) ||
                     (entry->owner == kLayerMenuRow_ActionBg &&
                      entry->source.action_bg.kind == kActionBgTunerRow_Reset);
    DrawTextRight(layout, value_right, y, text.value, value_chars,
                  reset_row ? (viewport->focused ? kText_Warning : kText_Dim)
                            : (style == kText_Normal ? kText_Value : style));
  }
  return n;
}

void LayerMenu_DrawDescription(const MenuLayout *layout, int tab, int selected, int description_x,
                               int header_y, int width) {
  LayerMenuRow rows[kLayerMenuRowMax];
  const LayerMenuRow *help_row = SelectedLayerRow(tab, selected, rows, kLayerMenuRowMax, NULL);
  if (!help_row) return;
  const int description_chars = width / kDebugGlyphWidth;
  const uint32_t structure = kSteelBlue, structure_dim = kSteelDim;
  SettingsOverlayLayerText text;
  LocalizeLayerRow(help_row, &text);
  const DioramaEditorRow *diorama =
      help_row->owner == kLayerMenuRow_Diorama ? &help_row->source.diorama : NULL;
  char label[2 * kOverlayLayerCaptionBytes + 8];
  if (diorama && diorama->kind == kDioramaEditorRow_Plane)
    snprintf(label, sizeof(label), "%s -- %s", text.label, text.value);
  else
    snprintf(label, sizeof(label), "%s", text.label);
  /* The right-hand slug says WHEN a change takes effect, which for these is
   * always "the next frame" -- that immediacy is the point of the tool. */
  const char *kApplyNow = Ui("overlay.apply.0");
  int apply_x = description_x + width - SmallTextWidth(kApplyNow);
  DrawSmallTextN(layout, description_x, header_y, label,
                 (apply_x - description_x - 8) / kDebugGlyphWidth, structure);
  DrawSmallTextN(layout, description_x + width - SmallTextWidth(kApplyNow), header_y, kApplyNow,
                 description_chars, kMutedText);
  FillLogicalRect(layout, description_x, header_y + 10, width, 1, structure_dim);
  DrawWrappedSmallText(layout, description_x, header_y + 14, text.help, description_chars, 4,
                       ARGB(255, 208, 220, 232));
}

void LayerMenu_AddHints(MenuHints *hints, int tab, int selected, bool multiple_tabs,
                        const char *change, const char *confirm, const char *tabs,
                        const char *reset) {
  LayerMenuRow rows[kLayerMenuRowMax];
  const LayerMenuRow *help_row = SelectedLayerRow(tab, selected, rows, kLayerMenuRowMax, NULL);
  if (!help_row) return;
#define HINT(key, text) AddMenuHint(hints, key, Ui(text))
  /* The editor's verbs differ enough to be worth spelling out: Left/Right
   * cycles the SHAPE on a plane row but steps a number on a parameter row,
   * and B expands rather than edits. */
  if (help_row->owner == kLayerMenuRow_ActionBg) {
    switch (help_row->source.action_bg.kind) {
    case kActionBgTunerRow_Layer:
      HINT(confirm, "overlay.hint.settings");
      HINT(reset, "overlay.hint.clear_layer");
      break;
    case kActionBgTunerRow_BandHeader:
      HINT(confirm, "overlay.hint.settings");
      HINT(reset, "overlay.hint.canonical_bands");
      break;
    case kActionBgTunerRow_Print:
      HINT(confirm, "overlay.hint.print");
      break;
    case kActionBgTunerRow_Reset:
      HINT(confirm, "overlay.hint.reset_draft");
      break;
    case kActionBgTunerRow_Header:
      break;
    default:
      HINT(change, "overlay.hint.adjust");
      HINT(reset, "overlay.hint.canonical");
      break;
    }
  } else {
    switch (help_row->source.diorama.kind) {
    case kDioramaEditorRow_Plane:
      HINT(change, "overlay.hint.shape");
      HINT(confirm, "overlay.hint.settings");
      HINT(reset, "overlay.hint.clear_plane");
      break;
    case kDioramaEditorRow_ResetRoom:
      HINT(confirm, "overlay.hint.reset_room");
      break;
    case kDioramaEditorRow_Header:
      break;
    default:
      HINT(change, "overlay.hint.adjust");
      HINT(reset, "overlay.hint.clear");
      break;
    }
  }
  if (multiple_tabs)
    HINT(tabs,
         help_row->owner == kLayerMenuRow_ActionBg ? "overlay.hint.tab" : "overlay.hint.level");
#undef HINT
}
