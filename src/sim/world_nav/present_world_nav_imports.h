#ifndef AR_PRESENT_WORLD_NAV_IMPORTS_H
#define AR_PRESENT_WORLD_NAV_IMPORTS_H
/* Optional continent surface on the production globe. Native coordinates
 * remain owned by travel/selection; only the spherical chart is enlarged. */
#include "sim/world_nav/present_world_nav_geometry.h"
struct FrameSlot;
/* Blend from the AR1 chart to the enlarged imported chart during a turn.
 * Use the same transform for terrain, camera focus and the palace marker. */
ArRenderPointF WorldNavigationImports_ChartPoint(float x, float y, float turn);
bool WorldNavigationImports_Draw(const struct FrameSlot *slot, ArRenderRectI viewport,
                                 const WorldNavigationProjection *projection);
bool WorldNavigationImports_Normal(const WorldNavigationProjection *projection,
                                    float x, float y, float normal[3]);
bool WorldNavigationImports_DrawFlat(const struct FrameSlot *slot, ArRenderRectI viewport);
void WorldNavigationImports_DrawLabel(const struct FrameSlot *slot, ArRenderRectI viewport);
void WorldNavigationImports_Reset(void);
#endif
