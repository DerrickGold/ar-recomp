#pragma once

#include <stdbool.h>

#include "sim_render_metadata.h"

enum {
  kSceneInspectorBg1 = 1 << 0,
  kSceneInspectorBg2 = 1 << 1,
  kSceneInspectorBg3 = 1 << 2,
  kSceneInspectorBg4 = 1 << 3,
  kSceneInspectorBgAll = 0x0f,
};

/* Read-only inspection of the currently rendered PPU state. Coordinates use
 * SNES screen space: x may be negative or exceed 255 in widescreen margins;
 * y is the visible 0..223 output row. */
bool SceneInspector_Select(int screen_x, int screen_y);
/* Presentation overlays can target only one captured plane. Filtering keeps
 * the report tied to the pixels actually composited at the clicked point. */
bool SceneInspector_SelectFiltered(int screen_x, int screen_y,
                                   unsigned bg_mask, bool inspect_objects);
/* Game-thread snapshot supplied after scanout.  Passing NULL invalidates it;
 * selection code never reads the live SIM metadata producer. */
void SceneInspector_SetSimFrameData(const SimFrameData *frame);
void SceneInspector_Clear(void);
bool SceneInspector_HasSelection(void);
const char *SceneInspector_PanelText(void);

/* Point/rect used by the host to mark the inspected graphic. The rectangle is
 * the best visible tile or sprite candidate and uses exclusive endpoints. */
bool SceneInspector_GetPoint(int *screen_x, int *screen_y);
bool SceneInspector_GetHighlight(int *x0, int *y0, int *x1, int *y1);
