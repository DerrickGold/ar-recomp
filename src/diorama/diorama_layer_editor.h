#ifndef DIORAMA_LAYER_EDITOR_H
#define DIORAMA_LAYER_EDITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "diorama_layer_order.h"

/* Row model for the in-game diorama layer editor (System > Layers).
 *
 * WHY THIS FILE EXISTS. The editor's rows are not settings-registry
 * descriptors: g_setting_descs is a static const array, so a list whose shape
 * depends on the room the player is standing in and on which plane the cursor
 * is over cannot live there. But the overlay (settings_overlay.c, 3083 lines)
 * is the wrong place for the row arithmetic either -- it is an SDL file, and
 * while it IS in a test target, that target deliberately does not link
 * diorama.c. So the row list, the strategy cycling and every bound live here,
 * pure: no SDL, no globals, no file I/O. The overlay walks the list this
 * builds and draws it; the test drives this directly.
 *
 * That split is the same one the depth shapes use (diorama_depth_shapes.c) and
 * for the same reason: the arithmetic that decides what the renderer draws must
 * be reachable from a test.
 *
 * THE EDITOR IS A DEBUG TOOL. Every row here, and the whole section that hosts
 * it, is hidden unless show_debug_settings is on -- authoring a room means
 * knowing what a rake does to a parallax rate. The gate lives in the overlay
 * (it owns Settings_IsDebugOnly); this module is gate-agnostic and simply
 * builds rows when asked.
 *
 * SCOPE: the LIVE room only. A level's room set cannot be enumerated -- $19 is
 * not a uniform act selector (docs/ram-map.md records that act 2 starts at
 * $02/$02/$03/$04/$04/$05 depending on the region, Death Heim uses $01 for its
 * hub and $02-$08 for arenas) and no table of valid rooms exists in the ROM
 * notes. Listing a fixed $01..$08 would therefore offer rooms that do not
 * exist. So a level tab shows the room the player is standing in, and is empty
 * otherwise -- which also matches how the editor is used: walk to the room,
 * look at the screen, cycle the shape.
 */

enum {
  /* Level tabs are the ACTION map groups only ($01 Fillmore .. $07 Death Heim).
   * Diorama_IsActiveThisFrame gates on ActRaiser_IsActionMapGroup, so group $00
   * (towns, world map, title) or $08 (ending) could never show a live room and
   * would be a tab that is always empty. */
  kDioramaEditorLevelCount = 7,
  /* Worst case rows for one tab: a header, every plane, the expanded parameter
   * block of the one selected plane, and the reset action. Sized so a caller
   * can stack-allocate the array without reasoning about which shape is active.
   */
  kDioramaEditorRowMax = 32,
};

/* What a row DOES, which is what the overlay's key dispatch switches on. The
 * label/value columns come from the row itself, so the overlay never needs to
 * know what a rake is. */
typedef enum DioramaEditorRowKind {
  /* Not selectable: the "Room 02" caption, or the "no live room" notice. */
  kDioramaEditorRow_Header = 0,
  /* A plane. Left/Right cycles its depth strategy; that is the whole point of
   * the editor. Reset clears every override on the plane. */
  kDioramaEditorRow_Plane,
  /* A numeric parameter of the selected plane (depth, alpha, z, copies...).
   * Left/Right steps it. Reset clears just that key. */
  kDioramaEditorRow_Param,
  /* An enumerated parameter (stack direction). Left/Right cycles it. */
  kDioramaEditorRow_ParamEnum,
  /* Clear every override in the live room, restoring the built-in look. */
  kDioramaEditorRow_ResetRoom,
} DioramaEditorRowKind;

/* Which knob a Param/ParamEnum row edits. The editor needs a discriminator
 * that is independent of the shape, because several shapes share a key (stack
 * and voxel both resolve onto `stack`), and because Reset must clear exactly
 * one set_* flag. */
typedef enum DioramaEditorParam {
  kDioramaEditorParam_None = 0,
  kDioramaEditorParam_Depth,      /* the active shape's magnitude */
  kDioramaEditorParam_Copies,     /* stack copies / voxel slices */
  kDioramaEditorParam_Density,    /* stack slices per unit depth */
  kDioramaEditorParam_Direction,  /* stack fill side */
  kDioramaEditorParam_Z,
  kDioramaEditorParam_Alpha,
  kDioramaEditorParam_Source,
  kDioramaEditorParam_Order,
} DioramaEditorParam;

/* One row, fully resolved: everything the overlay draws or dispatches on.
 *
 * Text is carried as fixed buffers rather than pointers because a row's value
 * is computed (an authored float, a strategy name plus its magnitude), so there
 * is nothing stable to point at. Sized for the panel's value column. */
typedef struct DioramaEditorRow {
  DioramaEditorRowKind kind;
  char label[32];
  char value[24];
  /* Which plane this row belongs to, or -1 for a header / reset row. Plane
   * rows and the parameter rows beneath them share it. */
  int plane;
  DioramaEditorParam param;
  /* Exact live scope represented by this row. The settings overlay uses this
   * identity for writes instead of re-querying a camera-local provider after
   * the row was selected. */
  uint8_t map_group;
  uint8_t map_number;
  uint8_t section;
  /* Renderer-resolved value of an inherited skybox source. Scoped records keep
   * only their own authored fields, but the Source row must display and step
   * from what is actually being drawn. */
  uint8_t effective_source;
  /* True when the row is a parameter of the selected plane, so the overlay can
   * indent it and dim it distinctly from a plane row. */
  bool nested;
  bool selectable;
} DioramaEditorRow;

/* The editor's live context: which room the player is in, and where the cursor
 * is. Passed in rather than read from globals so a test can place the cursor
 * anywhere without an emulator. */
typedef struct DioramaEditorContext {
  bool room_live;         /* false when the diorama is not running a room */
  uint8_t map_group;      /* $18 */
  uint8_t map_number;     /* $19 */
  uint8_t section;        /* DioramaLayerSection */
  /* The plane the cursor is on, or -1. Only this plane's parameters are listed
   * -- see the header comment on why the list expands rather than showing every
   * plane's parameters at once. */
  int selected_plane;
} DioramaEditorContext;

/* Map a level tab index (0..kDioramaEditorLevelCount-1) to its map group, and
 * its display name. The name is the kingdom, because that is how the player
 * thinks of a level; Death Heim is last and is not a kingdom, which is why the
 * table is explicit rather than derived. */
uint8_t DioramaLayerEditor_LevelGroup(int level_index);
const char *DioramaLayerEditor_LevelName(int level_index);
/* The tab index for a map group, or -1 when the group has no tab (i.e. is not
 * an action group, so can never host a diorama). */
int DioramaLayerEditor_LevelIndexOfGroup(uint8_t map_group);

/* Build the row list for one level tab.
 *
 * Writes at most `capacity` rows and returns the number written. A tab whose
 * level is not the one the player is in gets a single explanatory header row --
 * never zero rows, so the panel never renders as an unexplained blank.
 *
 * `table` may be NULL (nothing authored yet); the rows then show every plane at
 * its built-in flat state.
 */
int DioramaLayerEditor_BuildRows(const DioramaLayerOrderTable *table,
                                 const DioramaEditorContext *context,
                                 int level_index,
                                 DioramaEditorRow *out, int capacity);

/* Copy `text` uppercased into `out`, always NUL-terminating.
 *
 * The menu is drawn in the game's own all-caps face, but the shape and direction
 * names are lowercase because they are MANIFEST TOKENS -- renaming one would
 * invalidate authored files, so they cannot simply be spelled in capitals. Three
 * call sites needed the same conversion (two row values and the status line),
 * which is why it is one function rather than three loops. ASCII only, which is
 * all the token grammar allows. */
void DioramaLayerEditor_Upper(char *out, size_t size, const char *text);

/* Help text for the description panel: what the selected row does, and for a
 * plane row what its CURRENT shape costs. Never NULL.
 *
 * Here rather than in the overlay because the trade-offs are properties of the
 * shapes, not of the menu -- a rake's shear and a stack's inter-slice gaps are
 * the same facts diorama-layers.ini documents, and one wording keeps them from
 * drifting apart. `strategy` is ignored for rows that are not plane rows. */
const char *DioramaLayerEditor_RowHelp(DioramaEditorRowKind kind,
                                       DioramaEditorParam param,
                                       DioramaDepthStrategy strategy);

/* Cycle a plane's depth strategy by `direction` (+1/-1), wrapping.
 *
 * THIS IS THE LOAD-BEARING FUNCTION. The six strategies are NOT exclusive in
 * the data model: rake and bow SUM (BuildLayerSkirtMesh in diorama.c passes
 * rake+bow to the skirt), and DioramaLayerOrder_StrategyOf reports whichever key DOMINATES
 * rather than the only one set. So selecting a strategy means clearing every
 * key that would otherwise dominate or add to it, not merely setting one.
 * Getting that wrong silently draws a different shape than the row claims.
 *
 * Returns the strategy now authored. `set_*` flags for the chosen shape are
 * set with a sensible starting magnitude when the plane had none, so one
 * keypress produces something visible rather than a shape with depth zero.
 */
DioramaDepthStrategy DioramaLayerEditor_CycleStrategy(
    DioramaPlaneOverride *plane, int direction);

/* Author `strategy` on the plane outright, clearing the others. Same semantics
 * as CycleStrategy, exposed for a test and for a preset. */
void DioramaLayerEditor_SetStrategy(DioramaPlaneOverride *plane,
                                    DioramaDepthStrategy strategy);

/* Which strategy a plane's authored keys currently amount to. Distinct from
 * DioramaLayerOrder_StrategyOf, which reads a RESOLVED layer -- the editor has
 * only the override, before resolution. */
DioramaDepthStrategy DioramaLayerEditor_StrategyOfPlane(
    const DioramaPlaneOverride *plane);

/* Step one parameter of a plane by `direction` steps of its own natural
 * increment, clamped to the bound the manifest parser enforces for that key.
 * Returns true when the value changed, so the caller can report a bump at the
 * end of a range rather than silently doing nothing.
 *
 * The increments and bounds are deliberately here rather than in the overlay:
 * they are properties of the manifest grammar (diorama_layer_order.c's parser
 * rejects a rake outside -1..1, slices outside 2..24) and a UI that invented
 * its own would let the player author a value its own file cannot reload. */
bool DioramaLayerEditor_StepParam(DioramaPlaneOverride *plane,
                                  DioramaEditorParam param, int direction);

/* Clear one parameter (the Reset key on a Param row), or every override on the
 * plane (Reset on a Plane row). Both are true undos: a plane cleared this way
 * is indistinguishable from one never authored, so the room falls back to the
 * built-in table exactly. */
void DioramaLayerEditor_ClearParam(DioramaPlaneOverride *plane,
                                   DioramaEditorParam param);
void DioramaLayerEditor_ClearPlane(DioramaPlaneOverride *plane);

#endif /* DIORAMA_LAYER_EDITOR_H */
