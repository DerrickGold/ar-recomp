#ifndef AR_PRESENT_SIM3D_EFFECTS_H
#define AR_PRESENT_SIM3D_EFFECTS_H

#include <stdbool.h>

#include "present.h"
#include "render/render_types.h"
#include "scene3d_math.h"
#include "sim/sim_render_metadata.h"

/* Frame-owned SIM effect stages. Each takes the composite's source/viewport
 * and camera and draws one band of the effect composite; ordering between
 * them is the composite's business, not theirs. */
void DrawSimEffectLocalLighting(
    const FrameSlot *slot, bool lighting, ArRenderRectI source,
    ArRenderRectI viewport,
    const Scene3DCamera *camera, const float matrix[16]);
void DrawSimEffectSceneFlash(
    const FrameSlot *slot, bool lighting, ArRenderRectI viewport);
void DrawSimEffectParticles(
    const FrameSlot *slot, bool particles, ArRenderRectI source,
    ArRenderRectI viewport,
    const Scene3DCamera *camera, const float matrix[16]);
/* The volcanic arc's heads are drawn from the ROM's own art at the model's
 * published crater mouth, so they are a separate stage from the particles. */
void DrawSimEffectFireballHeads(
    const FrameSlot *slot, bool billboards, ArRenderRectI source,
    ArRenderRectI viewport, const Scene3DCamera *camera,
    const float matrix[16]);

/* An object the HUD has promoted out of the world tier; drawn on the map
 * plane rather than as a billboard. */
bool SimObjectIsPromotedHud(
    const FrameSlot *slot, const SimRenderObject *object);
void DrawSimMapPlaneObject(
    const FrameSlot *slot, const SimRenderObject *object, int screen_origin_x,
    int screen_origin_y, ArRenderRectI source, ArRenderRectI viewport,
    const float matrix[16]);

#endif /* AR_PRESENT_SIM3D_EFFECTS_H */
