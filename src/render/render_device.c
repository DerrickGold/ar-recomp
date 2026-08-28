#include "render_device.h"

#include <string.h>

static bool HasRequiredOps(const ArRenderBackendOps *ops) {
  return ops && ops->struct_size >= sizeof(*ops) &&
      ops->create_texture && ops->destroy_texture && ops->update_texture &&
      ops->set_render_target && ops->set_viewport && ops->set_clip_rect &&
      ops->clear && ops->draw_texture && ops->draw_geometry && ops->present &&
      ops->last_error;
}

bool ArRenderDevice_Init(ArRenderDevice *device,
                         const ArRenderBackendOps *ops,
                         void *context,
                         ArRenderCapabilities capabilities) {
  if (!device || !context || !HasRequiredOps(ops)) return false;
  *device = (ArRenderDevice){
    .ops = ops,
    .context = context,
    .capabilities = capabilities,
  };
  return true;
}

void ArRenderDevice_Reset(ArRenderDevice *device) {
  if (device) memset(device, 0, sizeof(*device));
}

bool ArRenderDevice_IsReady(const ArRenderDevice *device) {
  return device && device->ops && device->context;
}

const ArRenderCapabilities *ArRenderDevice_Capabilities(
    const ArRenderDevice *device) {
  return ArRenderDevice_IsReady(device) ? &device->capabilities : NULL;
}

bool ArRenderDevice_CreateTexture(ArRenderDevice *device,
                                  const ArRenderTextureDesc *desc,
                                  ArRenderTexture *out_texture) {
  if (out_texture) *out_texture = ArRenderTexture_Invalid();
  return ArRenderDevice_IsReady(device) && desc && out_texture &&
      desc->width > 0 && desc->height > 0 &&
      device->ops->create_texture(device->context, desc, out_texture) &&
      ArRenderTexture_IsValid(*out_texture);
}

void ArRenderDevice_DestroyTexture(ArRenderDevice *device,
                                   ArRenderTexture texture) {
  if (ArRenderDevice_IsReady(device) && ArRenderTexture_IsValid(texture))
    device->ops->destroy_texture(device->context, texture);
}

bool ArRenderDevice_UpdateTexture(ArRenderDevice *device,
                                  ArRenderTexture texture,
                                  const ArRenderRectI *destination,
                                  const void *pixels, int pitch_bytes) {
  return ArRenderDevice_IsReady(device) &&
      ArRenderTexture_IsValid(texture) && pixels && pitch_bytes > 0 &&
      device->ops->update_texture(
          device->context, texture, destination, pixels, pitch_bytes);
}

bool ArRenderDevice_SetRenderTarget(ArRenderDevice *device,
                                    ArRenderTexture target) {
  return ArRenderDevice_IsReady(device) &&
      device->ops->set_render_target(device->context, target);
}

bool ArRenderDevice_SetViewport(ArRenderDevice *device,
                                const ArRenderRectI *viewport) {
  return ArRenderDevice_IsReady(device) &&
      device->ops->set_viewport(device->context, viewport);
}

bool ArRenderDevice_SetClipRect(ArRenderDevice *device,
                                const ArRenderRectI *clip) {
  return ArRenderDevice_IsReady(device) &&
      device->ops->set_clip_rect(device->context, clip);
}

bool ArRenderDevice_Clear(ArRenderDevice *device, ArRenderColorF color) {
  return ArRenderDevice_IsReady(device) &&
      device->ops->clear(device->context, color);
}

bool ArRenderDevice_DrawTexture(ArRenderDevice *device,
                                ArRenderTexture texture,
                                const ArRenderRectF *source,
                                const ArRenderRectF *destination) {
  return ArRenderDevice_IsReady(device) &&
      ArRenderTexture_IsValid(texture) &&
      device->ops->draw_texture(
          device->context, texture, source, destination);
}

bool ArRenderDevice_DrawGeometry(ArRenderDevice *device,
                                 ArRenderTexture texture,
                                 const ArRenderVertex2D *vertices,
                                 int vertex_count,
                                 const int32_t *indices,
                                 int index_count) {
  return ArRenderDevice_IsReady(device) && vertices && vertex_count > 0 &&
      indices && index_count > 0 &&
      device->ops->draw_geometry(device->context, texture, vertices,
                                 vertex_count, indices, index_count);
}

bool ArRenderDevice_Present(ArRenderDevice *device) {
  return ArRenderDevice_IsReady(device) &&
      device->ops->present(device->context);
}

const char *ArRenderDevice_LastError(const ArRenderDevice *device) {
  if (!ArRenderDevice_IsReady(device)) return "render device is not ready";
  const char *error = device->ops->last_error(device->context);
  return error && error[0] ? error : "render backend operation failed";
}
