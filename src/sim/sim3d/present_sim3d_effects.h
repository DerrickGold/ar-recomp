#ifndef AR_PRESENT_SIM3D_EFFECTS_H
#define AR_PRESENT_SIM3D_EFFECTS_H

#include <stdbool.h>

#include "present/present.h"
#include "render/render_types.h"
#include "render/scene3d_math.h"
#include "sim/sim3d/present_sim3d_project.h"
#include "sim/sim_render_metadata.h"

/* Frame-owned SIM effect stages. Each takes the composite's source/viewport
 * and camera and draws one band of the effect composite; ordering between
 * them is the composite's business, not theirs. */
void DrawSimEffectLocalLighting(
    const FrameSlot *slot, bool lighting, const SimSceneProjection *scene);
void DrawSimEffectSceneFlash(
    const FrameSlot *slot, bool lighting, ArRenderRectI viewport);
void DrawSimEffectParticles(
    const FrameSlot *slot, bool particles, const SimSceneProjection *scene);
/* Native Sun tint is mandatory, independent of enhancement switches. Apply
 * once to replacement scenery; captured BG1 already contains the PPU add. */
bool DrawSimSunTint(const FrameSlot *slot, ArRenderRectI viewport);
void DrawSimSunLight(
    const FrameSlot *slot, bool lighting, const SimSceneProjection *scene);
void DrawSimSunRays(const FrameSlot *slot, bool lighting, bool particles,
                    const SimSceneProjection *scene);
/* The volcanic arc's heads are drawn from the ROM's own art at the model's
 * published crater mouth, so they are a separate stage from the particles. */
void DrawSimEffectFireballHeads(
    const FrameSlot *slot, bool billboards, const SimSceneProjection *scene);

/* Atlas billboards bypass PPU scene planes, so they must honor the same
 * completed HUD transfer. Generic OBJ captures describe scene/menu ownership. */
static inline bool SimObjectIsPromotedHud(const FrameSlot *slot,
                                         const SimRenderObject *object) {
  const HudIconFrame *icon = &slot->hud_icon;
  return object->tier == kSimRecordTier_Fixed && object->oam_count &&
      icon->scene_removed && icon->count && object->oam_first >= icon->first &&
      object->oam_first + object->oam_count <= icon->first + icon->count;
}
void DrawSimMapPlaneObject(
    const FrameSlot *slot, const SimRenderObject *object, int screen_origin_x,
    int screen_origin_y, ArRenderRectI source, ArRenderRectI viewport,
    const float matrix[16]);

#endif /* AR_PRESENT_SIM3D_EFFECTS_H */
