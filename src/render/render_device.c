#include "render_device.h"

#include <string.h>

static bool HasRequiredOps(const ArRenderBackendOps *ops) {
  return ops && ops->struct_size >= sizeof(*ops) &&
      ops->create_texture && ops->destroy_texture && ops->update_texture &&
      ops->set_render_target && ops->set_viewport && ops->set_clip_rect &&
      ops->clear && ops->draw_texture && ops->draw_geometry &&
      ops->present && ops->last_error;
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
  return ArRenderDevice_DrawTextureWithState(
      device, texture, source, destination, NULL);
}

static bool NormalizedColor(ArRenderColorF color) {
  return color.r >= 0.0f && color.r <= 1.0f &&
      color.g >= 0.0f && color.g <= 1.0f &&
      color.b >= 0.0f && color.b <= 1.0f &&
      color.a >= 0.0f && color.a <= 1.0f;
}

static bool ValidBlend(ArRenderBlendMode blend) {
  return blend >= kArRenderBlendMode_Opaque &&
      blend <= kArRenderBlendMode_Multiply;
}

static bool ValidAddress(ArRenderTextureAddressMode address) {
  return address >= kArRenderTextureAddressMode_Auto &&
      address <= kArRenderTextureAddressMode_Wrap;
}

static bool ValidDrawState(const ArRenderDrawState *state,
                           bool geometry, bool textured,
                           const ArRenderCapabilities *capabilities) {
  if (!state) return true;
  const ArRenderDrawStateFlags known =
      kArRenderDrawState_Tint | kArRenderDrawState_Blend |
      kArRenderDrawState_Address;
  if (state->flags & ~known) return false;
  if (geometry && (state->flags & kArRenderDrawState_Tint)) return false;
  if ((state->flags & kArRenderDrawState_Tint) &&
      !NormalizedColor(state->tint))
    return false;
  if ((state->flags & kArRenderDrawState_Blend) &&
      !ValidBlend(state->blend))
    return false;
  if (state->flags & kArRenderDrawState_Address) {
    if (!textured || !ValidAddress(state->address_u) ||
        !ValidAddress(state->address_v))
      return false;
    const bool needs_wrap =
        state->address_u == kArRenderTextureAddressMode_Wrap ||
        state->address_v == kArRenderTextureAddressMode_Wrap;
    if (needs_wrap && !ArRenderCapabilities_Has(
            capabilities, kArRenderCapability_TextureWrap))
      return false;
  }
  return true;
}

bool ArRenderDevice_DrawTextureWithState(
    ArRenderDevice *device, ArRenderTexture texture,
    const ArRenderRectF *source, const ArRenderRectF *destination,
    const ArRenderDrawState *state) {
  return ArRenderDevice_IsReady(device) &&
      ArRenderTexture_IsValid(texture) &&
      ValidDrawState(state, false, true, &device->capabilities) &&
      device->ops->draw_texture(
          device->context, texture, source, destination, state);
}

bool ArRenderDevice_DrawTextureTinted(ArRenderDevice *device,
                                      ArRenderTexture texture,
                                      const ArRenderRectF *source,
                                      const ArRenderRectF *destination,
                                      ArRenderColorF tint) {
  const ArRenderDrawState state = {
    .flags = kArRenderDrawState_Tint,
    .tint = tint,
  };
  return ArRenderDevice_DrawTextureWithState(
      device, texture, source, destination, &state);
}

bool ArRenderDevice_DrawGeometry(ArRenderDevice *device,
                                 ArRenderTexture texture,
                                 const ArRenderVertex2D *vertices,
                                 int vertex_count,
                                 const int32_t *indices,
                                 int index_count) {
  return ArRenderDevice_DrawGeometryWithState(
      device, texture, vertices, vertex_count, indices, index_count, NULL);
}

bool ArRenderDevice_DrawGeometryWithState(
    ArRenderDevice *device, ArRenderTexture texture,
    const ArRenderVertex2D *vertices, int vertex_count,
    const int32_t *indices, int index_count,
    const ArRenderDrawState *state) {
  return ArRenderDevice_IsReady(device) && vertices && vertex_count > 0 &&
      indices && index_count > 0 &&
      ValidDrawState(state, true, ArRenderTexture_IsValid(texture),
                     &device->capabilities) &&
      device->ops->draw_geometry(device->context, texture, vertices,
                                 vertex_count, indices, index_count, state);
}

bool ArRenderDevice_DrawSolidRect(ArRenderDevice *device,
                                  const ArRenderRectF *rectangle,
                                  ArRenderColorF color,
                                  ArRenderBlendMode blend) {
  if (!rectangle || rectangle->w <= 0.0f || rectangle->h <= 0.0f ||
      !NormalizedColor(color) || !ValidBlend(blend))
    return false;
  const float x1 = rectangle->x + rectangle->w;
  const float y1 = rectangle->y + rectangle->h;
  const ArRenderVertex2D vertices[] = {
    {{rectangle->x, rectangle->y}, color, {0.0f, 0.0f}},
    {{x1, rectangle->y}, color, {0.0f, 0.0f}},
    {{x1, y1}, color, {0.0f, 0.0f}},
    {{rectangle->x, y1}, color, {0.0f, 0.0f}},
  };
  const int32_t indices[] = {0, 1, 2, 0, 2, 3};
  const ArRenderDrawState state = {
    .flags = kArRenderDrawState_Blend,
    .blend = blend,
  };
  return ArRenderDevice_DrawGeometryWithState(
      device, ArRenderTexture_Invalid(), vertices, 4, indices, 6, &state);
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
