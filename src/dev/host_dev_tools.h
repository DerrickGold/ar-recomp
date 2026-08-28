#ifndef HOST_DEV_TOOLS_H
#define HOST_DEV_TOOLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "render/render_types.h"

void HostDevTools_FormatInspectorInfo(char *buffer, size_t buffer_size);
bool HostDevTools_DumpSceneAssets(void);
void HostDevTools_TakeFullSnapshot(void);
void HostDevTools_AdjustHudOutputScale(int delta_percent);
bool HostDevTools_InspectWindowPoint(int window_x, int window_y);
void HostDevTools_DumpDioramaLayers(void);
ArRenderExtentI HostDevTools_WriteFramebufferPpm(FILE *file);

#endif /* HOST_DEV_TOOLS_H */
