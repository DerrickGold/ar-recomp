#ifndef AR_RENDER_DEVICE_H
#define AR_RENDER_DEVICE_H

#include "render_types.h"

/* One dispatch occurs per resource operation or draw batch, never per pixel or
 * per vertex. Backends own resource storage; portable code sees only typed
 * opaque handles. */

typedef struct ArRenderBackendOps {
  size_t struct_size;

  bool (*create_texture)(void *context, const ArRenderTextureDesc *desc,
                         ArRenderTexture *out_texture);
  void (*destroy_texture)(void *context, ArRenderTexture texture);
  bool (*update_texture)(void *context, ArRenderTexture texture,
                         const ArRenderRectI *destination,
                         const void *pixels, int pitch_bytes);

  bool (*set_render_target)(void *context, ArRenderTexture target);
  bool (*set_viewport)(void *context, const ArRenderRectI *viewport);
  bool (*set_clip_rect)(void *context, const ArRenderRectI *clip);
  bool (*clear)(void *context, ArRenderColorF color);
  bool (*draw_texture)(void *context, ArRenderTexture texture,
                       const ArRenderRectF *source,
                       const ArRenderRectF *destination);
  bool (*draw_texture_tinted)(void *context, ArRenderTexture texture,
                              const ArRenderRectF *source,
                              const ArRenderRectF *destination,
                              ArRenderColorF tint);
  bool (*draw_geometry)(void *context, ArRenderTexture texture,
                        const ArRenderVertex2D *vertices, int vertex_count,
                        const int32_t *indices, int index_count);
  bool (*present)(void *context);
  const char *(*last_error)(void *context);
} ArRenderBackendOps;

typedef struct ArRenderDevice {
  const ArRenderBackendOps *ops;
  void *context;
  ArRenderCapabilities capabilities;
} ArRenderDevice;

/* Bind a backend-owned context and immutable operation table. All operations
 * needed by the portable baseline renderer are mandatory; optional enhanced
 * behavior is advertised through capabilities. */
bool ArRenderDevice_Init(ArRenderDevice *device,
                         const ArRenderBackendOps *ops,
                         void *context,
                         ArRenderCapabilities capabilities);
void ArRenderDevice_Reset(ArRenderDevice *device);
bool ArRenderDevice_IsReady(const ArRenderDevice *device);
const ArRenderCapabilities *ArRenderDevice_Capabilities(
    const ArRenderDevice *device);

bool ArRenderDevice_CreateTexture(ArRenderDevice *device,
                                  const ArRenderTextureDesc *desc,
                                  ArRenderTexture *out_texture);
void ArRenderDevice_DestroyTexture(ArRenderDevice *device,
                                   ArRenderTexture texture);
bool ArRenderDevice_UpdateTexture(ArRenderDevice *device,
                                  ArRenderTexture texture,
                                  const ArRenderRectI *destination,
                                  const void *pixels, int pitch_bytes);
/* An invalid target selects the platform's default output. NULL viewport or
 * clip rectangles restore the complete target extent / disable clipping. */
bool ArRenderDevice_SetRenderTarget(ArRenderDevice *device,
                                    ArRenderTexture target);
bool ArRenderDevice_SetViewport(ArRenderDevice *device,
                                const ArRenderRectI *viewport);
bool ArRenderDevice_SetClipRect(ArRenderDevice *device,
                                const ArRenderRectI *clip);
bool ArRenderDevice_Clear(ArRenderDevice *device, ArRenderColorF color);
bool ArRenderDevice_DrawTexture(ArRenderDevice *device,
                                ArRenderTexture texture,
                                const ArRenderRectF *source,
                                const ArRenderRectF *destination);
bool ArRenderDevice_DrawTextureTinted(ArRenderDevice *device,
                                      ArRenderTexture texture,
                                      const ArRenderRectF *source,
                                      const ArRenderRectF *destination,
                                      ArRenderColorF tint);
bool ArRenderDevice_DrawGeometry(ArRenderDevice *device,
                                 ArRenderTexture texture,
                                 const ArRenderVertex2D *vertices,
                                 int vertex_count,
                                 const int32_t *indices,
                                 int index_count);
bool ArRenderDevice_Present(ArRenderDevice *device);
const char *ArRenderDevice_LastError(const ArRenderDevice *device);

#endif /* AR_RENDER_DEVICE_H */
