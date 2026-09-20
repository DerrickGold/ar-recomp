#ifndef PRESENT_WORLD_NAV_COMPOSITION_H
#define PRESENT_WORLD_NAV_COMPOSITION_H

#include "frame_slot.h"

/* Borrowed capture textures. Upload and reset own their lifetime; scene
 * composition chooses where the Palace, plaque and native label are drawn. */
typedef struct WorldNavigationCompositionTextures {
  ArRenderTexture palace, label, plaque;
  bool uploaded;
} WorldNavigationCompositionTextures;
const WorldNavigationCompositionTextures *WorldNavigationComposition_Get(void);
void WorldNavigationComposition_Reset(void);
ArRenderPointF WorldNavigationComposition_ProjectPoint(const FrameSlot *slot,
                                                       ArRenderRectI viewport, float x, float y);
bool WorldNavigationComposition_DrawLayer(const FrameSlot *slot, ArRenderRectI viewport,
                                          const SimWorldNavigationCompositionLayer *layer,
                                          ArRenderTexture texture, ArRenderPointF offset,
                                          float scale);

#endif
