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
/* A one-shot diorama layer dump (Shift+D or AR_DIORAMA_DUMP_GF). Arming it
 * makes the next frame draw capture every diorama plane even with diorama
 * mode off; the service after that draw writes the layers and disarms. */
void HostDevTools_ArmDioramaDump(void);
bool HostDevTools_DioramaDumpArmed(void);
void HostDevTools_ServiceDioramaDump(void);
DevToolsCaptureResult HostDevTools_WriteFramebufferPpm(FILE *file, bool require_composite);

/* The inspector selection the renderer highlights; FrameSlot_Capture copies it
 * into each slot, and closing the inspector clears it. */
struct InspectorPresentationSelection;
const struct InspectorPresentationSelection *HostDevTools_InspectorPresentation(void);
void HostDevTools_ClearInspectorPresentation(void);

#endif /* AR_HOST_DEV_TOOLS_H */
