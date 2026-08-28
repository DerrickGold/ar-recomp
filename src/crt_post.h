/* Backend-neutral contract for the final fullscreen CRT post-process.
 *
 * Frame orchestration owns player policy and supplies semantic effect values.
 * The active platform adapter owns the scene target, shader representation,
 * native render state, and logical-presentation plumbing. */
#ifndef AR_CRT_POST_H
#define AR_CRT_POST_H

#include <stdbool.h>

#include "render/render_device.h"

typedef struct CrtPostConfig {
  bool enabled;
  float curvature;
  float scanline_depth;
  float mask_strength;
  float aberration;
  float bandwidth;
  float vignette;
  float brightness;
} CrtPostConfig;

/* Redirect subsequent scene rendering into the backend-owned offscreen target.
 * Returns false without side effects when disabled. A selected-mode setup
 * failure also returns false, but latches SessionFatal so the caller can end
 * the frame and session cleanly. */
bool CrtPost_Begin(ArRenderDevice *device, const CrtPostConfig *config);

/* Resolve the scene target to the platform's default output. Safe to call
 * unconditionally; returns `image` unchanged if Begin did not engage.
 *
 * scan_columns/scan_lines are source-signal dimensions, not output pixels.
 * image is the letterboxed game rectangle inside the target. The returned
 * rectangle is authoritative for post-resolve host UI: a backend may refine
 * the fallback using presentation state attached to its scene target. */
ArRenderRectI CrtPost_End(ArRenderDevice *device,
                          int scan_columns, int scan_lines,
                          ArRenderRectI image);

/* The target that "back to the base surface" means right now: invalid when
 * the effect is off, the opaque backend-owned scene target while engaged. */
ArRenderTexture CrtPost_BaseTarget(void);

/* Release backend resources before the render device is destroyed. */
void CrtPost_Shutdown(ArRenderDevice *device);

#endif /* AR_CRT_POST_H */
