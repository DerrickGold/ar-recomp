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
  /* Select one render unit per physical pixel of the current output/target,
   * bypassing any host-side logical scaling left by an earlier path. */
  bool (*use_output_coordinates)(void *context);
  bool (*get_output_size)(void *context, int *width, int *height);
  bool (*set_viewport)(void *context, const ArRenderRectI *viewport);
  bool (*set_clip_rect)(void *context, const ArRenderRectI *clip);
  bool (*capture_render_target_state)(void *context,
                                      ArRenderTargetState *state);
  bool (*restore_render_target_state)(void *context,
                                      const ArRenderTargetState *state);
  bool (*clear)(void *context, ArRenderColorF color);
  bool (*draw_texture)(void *context, ArRenderTexture texture,
                       const ArRenderRectF *source,
                       const ArRenderRectF *destination,
                       const ArRenderDrawState *state);
  bool (*draw_geometry)(void *context, ArRenderTexture texture,
                        const ArRenderVertex2D *vertices, int vertex_count,
                        const int32_t *indices, int index_count,
                        const ArRenderDrawState *state);
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
/* Establish physical output-pixel coordinates before querying or drawing a
 * viewport. Backends without an implicit logical transform implement this as
 * a successful no-op. The reported size belongs to the current output/target. */
bool ArRenderDevice_UseOutputCoordinates(ArRenderDevice *device);
bool ArRenderDevice_GetOutputSize(ArRenderDevice *device,
                                  int *width, int *height);
bool ArRenderDevice_SetViewport(ArRenderDevice *device,
                                const ArRenderRectI *viewport);
bool ArRenderDevice_SetClipRect(ArRenderDevice *device,
                                const ArRenderRectI *clip);
/* Scoped target entry is an optional extension. Begin captures the caller's
 * target/viewport/clip, binds `target`, selects its full viewport, and disables
 * clipping. Omitted guarantees that the caller state is intact; StateLost is
 * fatal to the current frame because subsequent draw ownership is unknown.
 * A Ready begin must be paired with EndTarget exactly once. */
ArRenderTargetBeginResult ArRenderDevice_BeginTarget(
    ArRenderDevice *device, ArRenderTexture target,
    ArRenderTargetState *state);
bool ArRenderDevice_EndTarget(ArRenderDevice *device,
                              const ArRenderTargetState *state);
bool ArRenderDevice_Clear(ArRenderDevice *device, ArRenderColorF color);
bool ArRenderDevice_DrawTexture(ArRenderDevice *device,
                                ArRenderTexture texture,
                                const ArRenderRectF *source,
                                const ArRenderRectF *destination);
bool ArRenderDevice_DrawTextureWithState(
    ArRenderDevice *device, ArRenderTexture texture,
    const ArRenderRectF *source, const ArRenderRectF *destination,
    const ArRenderDrawState *state);
bool ArRenderDevice_DrawTextureTinted(ArRenderDevice *device,
                                      ArRenderTexture texture,
                                      const ArRenderRectF *source,
                                      const ArRenderRectF *destination,
                                      ArRenderColorF tint);
/* An invalid geometry texture selects untextured per-vertex colour. */
bool ArRenderDevice_DrawGeometry(ArRenderDevice *device,
                                 ArRenderTexture texture,
                                 const ArRenderVertex2D *vertices,
                                 int vertex_count,
                                 const int32_t *indices,
                                 int index_count);
/* `state` may be NULL; blend and texture-address overrides are scoped to this
 * submission. Geometry tint is rejected because vertex colours own its
 * modulation. Addressing is rejected for untextured geometry. */
bool ArRenderDevice_DrawGeometryWithState(
    ArRenderDevice *device, ArRenderTexture texture,
    const ArRenderVertex2D *vertices, int vertex_count,
    const int32_t *indices, int index_count,
    const ArRenderDrawState *state);
/* Convenience batch for a uniformly coloured, untextured rectangle. The
 * explicit blend keeps this draw independent of backend-global state. */
bool ArRenderDevice_DrawSolidRect(ArRenderDevice *device,
                                  const ArRenderRectF *rectangle,
                                  ArRenderColorF color,
                                  ArRenderBlendMode blend);
bool ArRenderDevice_Present(ArRenderDevice *device);
const char *ArRenderDevice_LastError(const ArRenderDevice *device);

#endif /* AR_RENDER_DEVICE_H */
