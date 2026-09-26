#ifndef AR_PRESENT_SCENE_INSPECTOR_H
#define AR_PRESENT_SCENE_INSPECTOR_H
/* Scene inspector's crosshair, selected graphic outline and report panel.
 * Project the captured selection through the same HUD layout used to draw it;
 * retain the click anchor until a change in output size requires reprojection. */

#include "render/render_types.h"

typedef struct FrameSlot FrameSlot;

/* Draw after scene effects, in the host's full-output coordinate space. */
void PresentSceneInspector_Draw(const FrameSlot *slot, ArRenderRectI viewport);

#endif
