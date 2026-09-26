#ifndef AR_HD_REPLACEMENT_HOST_H
#define AR_HD_REPLACEMENT_HOST_H
/* HdReplacementHost: host resources for HD replacements. Loads and reloads
 * the replacement textures, owns the Mode-7 overlay surface present.c draws,
 * and binds only the capture surfaces needed by replacement art.
 * Phase: host (main thread, between frames). */

#include <stdbool.h>
#include <stdint.h>

#include "render/render_types.h"

enum { kHdMode7Scale = 4 };

/* Present-time Mode-7 resources. They are host-created and synchronously
 * consumed by present.c; emulated state never owns these allocations. */
extern uint8_t *g_m7_overlay_pixels;
extern ArRenderTexture g_m7_texture;

void HdReplacementHost_LoadTextures(void);
void HdReplacementHost_BindSurfaces(void);
void HdReplacementHost_ReloadTextures(void);
void HdReplacementHost_Shutdown(void);

/* Rebind existing art surfaces after the common PPU outputs are configured. */
struct HostPpuOutputControl;
void HdReplacementHost_RebindSurfaces(const struct HostPpuOutputControl *output);

#endif /* AR_HD_REPLACEMENT_HOST_H */
