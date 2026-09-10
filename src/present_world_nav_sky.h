/* Private Palace art direction, consuming an already prepared globe view. */
#ifndef PRESENT_WORLD_NAV_SKY_H
#define PRESENT_WORLD_NAV_SKY_H
#include "present.h"
#include "present_world_nav_geometry.h"

float PresentWorldNavSky_Horizon(ArRenderRectI viewport, const WorldNavigationProjection *projection);
bool PresentWorldNavSky_DrawBackdrop(ArRenderDevice *device, ArRenderRectI viewport, bool gradient, float horizon);
bool PresentWorldNavSky_DrawMist(ArRenderDevice *device, ArRenderRectI viewport, float horizon);
bool PresentWorldNavSky_DrawClouds(ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection, uint64_t elapsed_ms, float drift, float opacity);
void PresentWorldNavSky_Reset(void);
#endif
