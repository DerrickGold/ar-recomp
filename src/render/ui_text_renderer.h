#ifndef AR_RENDER_UI_TEXT_RENDERER_H
#define AR_RENDER_UI_TEXT_RENDERER_H

#include "localization/text_backend.h"
#include "render/text_surface_cache.h"

#define AR_UI_TEXT_RUN_ABI_VERSION UINT32_C(2)

/* Placement in the host's physical UI rectangle, independent of the text's
 * paragraph direction. Do not reuse logical leading/trailing raster alignment. */
typedef enum ArUiTextAlignment {
  kArUiTextAlignment_Left = 0,
  kArUiTextAlignment_Center,
  kArUiTextAlignment_Right,
} ArUiTextAlignment;

/* Host-interface text, independent of game text routes, saves and settings.
 * All coordinates are physical output pixels. A complete UTF-8 run is shaped
 * together; callers truncate at grapheme boundaries before submitting it. */
typedef struct ArUiTextRun {
  size_t struct_size;
  uint32_t abi_version;
  const char *utf8;
  size_t utf8_bytes;
  ArRenderRectI bounds;
  ArUiTextAlignment alignment;
  ArRenderColorF tint;
  uint32_t style_id;
  uint32_t band_rgb;
  uint32_t body_rgb;
  uint32_t shadow_rgb;
  bool shadow_enabled;
  const char *language_bcp47;
} ArUiTextRun;

/* Zero-initialize. Owns its font instance/cache, borrows the render device and
 * backend operations. Reinitialization is transactional. Host resolves all
 * font resources; no platform headers, ROM lookups or filesystem policy here. */
typedef struct ArUiTextRenderer { void *implementation; } ArUiTextRenderer;

bool ArUiTextRenderer_Init(ArUiTextRenderer *renderer, ArRenderDevice *device,
                           const ArTextBackend *backend,
                           const ArTextBackendConfig *fonts,
                           char *error, size_t error_capacity);
void ArUiTextRenderer_Destroy(ArUiTextRenderer *renderer);
bool ArUiTextRenderer_IsReady(const ArUiTextRenderer *renderer);
/* Call on render-device reset, before any subsequent draw. Fonts stay open. */
void ArUiTextRenderer_ClearTextures(ArUiTextRenderer *renderer);
/* Immediate draw, retaining no caller pointers/surface handles. Warm draws do
 * no shaping, allocation or upload; position and tint don't change the key. */
bool ArUiTextRenderer_Draw(ArUiTextRenderer *renderer, const ArUiTextRun *run);
/* Same bounded layout/cache as Draw, without submitting pixels. */
bool ArUiTextRenderer_Measure(ArUiTextRenderer *renderer, const ArUiTextRun *run,
                              int *width, int *height);
const ArTextSurfaceCacheStats *ArUiTextRenderer_GetStats(
    const ArUiTextRenderer *renderer);

#endif
