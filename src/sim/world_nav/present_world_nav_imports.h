#ifndef AR_PRESENT_WORLD_NAV_IMPORTS_H
#define AR_PRESENT_WORLD_NAV_IMPORTS_H
/* Optional continent surface on the production globe, with native chart
 * coordinates rotated by half a turn. The host owns destination selection. */
#include "sim/world_nav/present_world_nav_geometry.h"
struct FrameSlot;
bool WorldNavigationImports_Draw(const struct FrameSlot *slot, ArRenderRectI viewport,
                                 const WorldNavigationProjection *projection);
bool WorldNavigationImports_Normal(const WorldNavigationProjection *projection,
                                    float x, float y, float normal[3]);
bool WorldNavigationImports_DrawFlat(const struct FrameSlot *slot, ArRenderRectI viewport);
void WorldNavigationImports_DrawLabel(const struct FrameSlot *slot, ArRenderRectI viewport);
void WorldNavigationImports_Reset(void);
#endif
