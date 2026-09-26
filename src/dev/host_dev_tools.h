#ifndef AR_HOST_DEV_TOOLS_H
#define AR_HOST_DEV_TOOLS_H
/* HostDevTools: developer actions the host offers the settings overlay and
 * hotkeys: inspector info, scene asset dumps, full snapshots, HUD scale,
 * point inspection, diorama layer dumps and framebuffer PPM capture.
 * Phase: host (developer tools only). */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "render/render_types.h"
#include "dev/dev_tools_readback.h"

void HostDevTools_FormatInspectorInfo(char *buffer, size_t buffer_size);
bool HostDevTools_DumpSceneAssets(void);
void HostDevTools_TakeFullSnapshot(void);
void HostDevTools_AdjustHudOutputScale(int delta_percent);
bool HostDevTools_InspectWindowPoint(int window_x, int window_y);
void HostDevTools_DumpDioramaLayers(void);
/* A one-shot diorama layer dump is armed (Shift+D or AR_DIORAMA_DUMP_GF):
 * the next frame draw captures every diorama plane even with diorama mode
 * off, and the host dumps and clears it after that frame. */
extern bool g_diorama_dump_pending;
DevToolsCaptureResult HostDevTools_WriteFramebufferPpm(FILE *file, bool require_composite);

/* The inspector selection the renderer highlights; FrameSlot_Capture copies it
 * into each slot, and closing the inspector clears it. */
struct InspectorPresentationSelection;
const struct InspectorPresentationSelection *HostDevTools_InspectorPresentation(void);
void HostDevTools_ClearInspectorPresentation(void);

#endif /* AR_HOST_DEV_TOOLS_H */
