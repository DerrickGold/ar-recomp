#include "render/render_device.h"
#include "presentation_upload_mirror.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct FakeBackend {
  uintptr_t next_texture;
  int create_count;
  int destroy_count;
  int update_count;
  int draw_texture_count;
  int draw_geometry_count;
  int set_render_target_count;
  int capture_target_state_count;
  int restore_target_state_count;
  int present_count;
  ArRenderTextureDesc last_desc;
  ArRenderRectI last_update;
  ArRenderTexture last_texture;
  ArRenderDrawState last_draw_state;
  bool last_draw_state_present;
  int last_vertex_count;
  int last_index_count;
  ArRenderVertex2D last_vertices[4];
  ArRenderTargetState target_state;
  bool fail_capture_target_state;
  bool fail_set_viewport;
  bool fail_restore_target_state;
} FakeBackend;

static bool CreateTexture(void *context, const ArRenderTextureDesc *desc,
                          ArRenderTexture *out_texture) {
  FakeBackend *backend = context;
  assert(desc->format == kArRenderPixelFormat_Argb8888);
  backend->create_count++;
  backend->last_desc = *desc;
  *out_texture = (ArRenderTexture){++backend->next_texture};
  return true;
}

static void DestroyTexture(void *context, ArRenderTexture texture) {
  FakeBackend *backend = context;
  assert(ArRenderTexture_IsValid(texture));
  backend->destroy_count++;
  backend->last_texture = texture;
}

static bool UpdateTexture(void *context, ArRenderTexture texture,
                          const ArRenderRectI *destination,
                          const void *pixels, int pitch_bytes) {
  FakeBackend *backend = context;
  assert(destination && pixels && pitch_bytes > 0);
  backend->update_count++;
  backend->last_texture = texture;
  backend->last_update = *destination;
  return true;
}

static bool SetRenderTarget(void *context, ArRenderTexture target) {
  FakeBackend *backend = context;
  backend->set_render_target_count++;
  backend->last_texture = target;
  backend->target_state.target = target;
  return true;
}

static bool SetViewport(void *context, const ArRenderRectI *viewport) {
  FakeBackend *backend = context;
  if (backend->fail_set_viewport) return false;
  backend->target_state.viewport_set = viewport != NULL;
  if (viewport) backend->target_state.viewport = *viewport;
  return true;
}

static bool SetClipRect(void *context, const ArRenderRectI *clip) {
  FakeBackend *backend = context;
  backend->target_state.clip_enabled = clip != NULL;
  if (clip) backend->target_state.clip = *clip;
  return true;
}

static bool CaptureRenderTargetState(void *context,
                                     ArRenderTargetState *state) {
  FakeBackend *backend = context;
  backend->capture_target_state_count++;
  if (backend->fail_capture_target_state) return false;
  *state = backend->target_state;
  state->valid = true;
  return true;
}

static bool RestoreRenderTargetState(
    void *context, const ArRenderTargetState *state) {
  FakeBackend *backend = context;
  backend->restore_target_state_count++;
  if (backend->fail_restore_target_state) return false;
  backend->target_state = *state;
  return true;
}

static bool Clear(void *context, ArRenderColorF color) {
  (void)context;
  (void)color;
  return true;
}

static bool DrawTexture(void *context, ArRenderTexture texture,
                        const ArRenderRectF *source,
                        const ArRenderRectF *destination,
                        const ArRenderDrawState *state) {
  FakeBackend *backend = context;
  (void)source;
  (void)destination;
  backend->draw_texture_count++;
  backend->last_texture = texture;
  backend->last_draw_state_present = state != NULL;
  if (state) backend->last_draw_state = *state;
  return true;
}

static bool DrawGeometry(void *context, ArRenderTexture texture,
                         const ArRenderVertex2D *vertices, int vertex_count,
                         const int32_t *indices, int index_count,
                         const ArRenderDrawState *state) {
  FakeBackend *backend = context;
  assert(vertices && vertex_count > 0 && vertex_count <= 4);
  assert(indices && index_count > 0);
  backend->draw_geometry_count++;
  backend->last_texture = texture;
  backend->last_vertex_count = vertex_count;
  backend->last_index_count = index_count;
  memcpy(backend->last_vertices, vertices,
         (size_t)vertex_count * sizeof(vertices[0]));
  backend->last_draw_state_present = state != NULL;
  if (state) backend->last_draw_state = *state;
  return true;
}

static bool Present(void *context) {
  FakeBackend *backend = context;
  backend->present_count++;
  return true;
}

static const char *LastError(void *context) {
  (void)context;
  return "fake backend error";
}

static const ArRenderBackendOps kFakeOps = {
  .struct_size = sizeof(ArRenderBackendOps),
  .create_texture = CreateTexture,
  .destroy_texture = DestroyTexture,
  .update_texture = UpdateTexture,
  .set_render_target = SetRenderTarget,
  .set_viewport = SetViewport,
  .set_clip_rect = SetClipRect,
  .capture_render_target_state = CaptureRenderTargetState,
  .restore_render_target_state = RestoreRenderTargetState,
  .clear = Clear,
  .draw_texture = DrawTexture,
  .draw_geometry = DrawGeometry,
  .present = Present,
  .last_error = LastError,
};

static void TestDeviceDispatchAndCapabilities(void) {
  FakeBackend backend = {0};
  ArRenderDevice device = {0};
  const ArRenderCapabilities capabilities = {
    .flags = kArRenderCapability_StreamingTextures |
             kArRenderCapability_RenderTargets |
             kArRenderCapability_ScopedRenderTargets |
             kArRenderCapability_Geometry |
             kArRenderCapability_TextureWrap,
    .maximum_texture_width = 1024,
    .maximum_texture_height = 1024,
  };
  assert(ArRenderDevice_Init(
      &device, &kFakeOps, &backend, capabilities));
  assert(ArRenderCapabilities_Has(
      ArRenderDevice_Capabilities(&device),
      kArRenderCapability_StreamingTextures |
          kArRenderCapability_Geometry));

  const ArRenderTextureDesc desc = {
    .width = 256,
    .height = 224,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Opaque,
  };
  ArRenderTexture texture = ArRenderTexture_Invalid();
  assert(ArRenderDevice_CreateTexture(&device, &desc, &texture));
  assert(texture.value == 1 && backend.create_count == 1);

  uint32_t pixels[4] = {0};
  const ArRenderRectI update = {4, 5, 2, 2};
  assert(ArRenderDevice_UpdateTexture(
      &device, texture, &update, pixels, 2 * (int)sizeof(uint32_t)));
  assert(backend.update_count == 1);
  assert(memcmp(&backend.last_update, &update, sizeof(update)) == 0);

  const ArRenderRectF source = {0, 0, 2, 2};
  assert(ArRenderDevice_DrawTexture(
      &device, texture, &source, NULL));
  assert(backend.draw_texture_count == 1);
  assert(!backend.last_draw_state_present);

  const ArRenderColorF tint = {0.25f, 0.5f, 0.75f, 1.0f};
  assert(ArRenderDevice_DrawTextureTinted(
      &device, texture, &source, NULL, tint));
  assert(backend.draw_texture_count == 2);
  assert(backend.last_draw_state_present);
  assert(backend.last_draw_state.flags == kArRenderDrawState_Tint);
  assert(memcmp(&backend.last_draw_state.tint, &tint, sizeof(tint)) == 0);

  const ArRenderDrawState blend_state = {
    .flags = kArRenderDrawState_Blend,
    .blend = kArRenderBlendMode_Add,
  };
  assert(ArRenderDevice_DrawTextureWithState(
      &device, texture, &source, NULL, &blend_state));
  assert(backend.draw_texture_count == 3);
  assert(backend.last_draw_state_present);
  assert(backend.last_draw_state.flags == kArRenderDrawState_Blend);
  assert(backend.last_draw_state.blend == kArRenderBlendMode_Add);

  const ArRenderDrawState address_state = {
    .flags = kArRenderDrawState_Address,
    .address_u = kArRenderTextureAddressMode_Wrap,
    .address_v = kArRenderTextureAddressMode_Clamp,
  };
  assert(ArRenderDevice_DrawTextureWithState(
      &device, texture, &source, NULL, &address_state));
  assert(backend.draw_texture_count == 4);
  assert(backend.last_draw_state.address_u ==
         kArRenderTextureAddressMode_Wrap);
  assert(backend.last_draw_state.address_v ==
         kArRenderTextureAddressMode_Clamp);

  ArRenderDrawState invalid_state = {
    .flags = kArRenderDrawState_Tint,
    .tint = {1.01f, 1.0f, 1.0f, 1.0f},
  };
  assert(!ArRenderDevice_DrawTextureWithState(
      &device, texture, &source, NULL, &invalid_state));
  invalid_state = (ArRenderDrawState){.flags = UINT32_C(1) << 31};
  assert(!ArRenderDevice_DrawTextureWithState(
      &device, texture, &source, NULL, &invalid_state));
  assert(backend.draw_texture_count == 4);

  const ArRenderTextureDesc target_desc = {
    .width = 640,
    .height = 480,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Target,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  ArRenderTexture target = ArRenderTexture_Invalid();
  assert(ArRenderDevice_CreateTexture(&device, &target_desc, &target));
  assert(backend.last_desc.usage == kArRenderTextureUsage_Target);
  assert(ArRenderDevice_SetRenderTarget(&device, target));
  assert(backend.set_render_target_count == 1 &&
         ArRenderTexture_Equals(backend.last_texture, target));
  assert(ArRenderDevice_SetRenderTarget(
      &device, ArRenderTexture_Invalid()));
  assert(backend.set_render_target_count == 2 &&
         !ArRenderTexture_IsValid(backend.last_texture));

  const ArRenderRectI viewport = {7, 8, 320, 240};
  const ArRenderRectI clip = {9, 10, 100, 80};
  assert(ArRenderDevice_SetViewport(&device, &viewport));
  assert(ArRenderDevice_SetClipRect(&device, &clip));
  ArRenderTargetState target_state = {0};
  assert(ArRenderDevice_BeginTarget(&device, target, &target_state) ==
         kArRenderTargetBegin_Ready);
  assert(target_state.valid);
  assert(backend.capture_target_state_count == 1);
  assert(ArRenderTexture_Equals(backend.target_state.target, target));
  assert(!backend.target_state.viewport_set);
  assert(!backend.target_state.clip_enabled);
  assert(ArRenderDevice_EndTarget(&device, &target_state));
  assert(backend.restore_target_state_count == 1);
  assert(!ArRenderTexture_IsValid(backend.target_state.target));
  assert(backend.target_state.viewport_set);
  assert(memcmp(&backend.target_state.viewport, &viewport,
                sizeof(viewport)) == 0);
  assert(backend.target_state.clip_enabled);
  assert(memcmp(&backend.target_state.clip, &clip, sizeof(clip)) == 0);

  backend.fail_capture_target_state = true;
  assert(ArRenderDevice_BeginTarget(&device, target, &target_state) ==
         kArRenderTargetBegin_Omitted);
  assert(!target_state.valid);
  backend.fail_capture_target_state = false;

  backend.fail_set_viewport = true;
  assert(ArRenderDevice_BeginTarget(&device, target, &target_state) ==
         kArRenderTargetBegin_Omitted);
  assert(!target_state.valid);
  assert(backend.restore_target_state_count == 2);
  backend.fail_restore_target_state = true;
  assert(ArRenderDevice_BeginTarget(&device, target, &target_state) ==
         kArRenderTargetBegin_StateLost);
  assert(!target_state.valid);
  backend.fail_set_viewport = false;
  backend.fail_restore_target_state = false;
  ArRenderDevice_DestroyTexture(&device, target);

  const ArRenderVertex2D vertices[3] = {0};
  const int32_t indices[3] = {0, 1, 2};
  assert(ArRenderDevice_DrawGeometry(
      &device, texture, vertices, 3, indices, 3));
  assert(backend.draw_geometry_count == 1);
  assert(!backend.last_draw_state_present);
  assert(ArRenderDevice_DrawGeometryWithState(
      &device, texture, vertices, 3, indices, 3, &blend_state));
  assert(backend.draw_geometry_count == 2);
  assert(backend.last_draw_state_present);
  assert(backend.last_draw_state.flags == kArRenderDrawState_Blend);
  invalid_state = (ArRenderDrawState){
    .flags = kArRenderDrawState_Tint,
    .tint = {1.0f, 1.0f, 1.0f, 1.0f},
  };
  assert(!ArRenderDevice_DrawGeometryWithState(
      &device, texture, vertices, 3, indices, 3, &invalid_state));
  assert(backend.draw_geometry_count == 2);
  assert(!ArRenderDevice_DrawGeometryWithState(
      &device, ArRenderTexture_Invalid(), vertices, 3, indices, 3,
      &address_state));
  assert(backend.draw_geometry_count == 2);

  const ArRenderRectF rectangle = {10.0f, 20.0f, 30.0f, 40.0f};
  const ArRenderColorF color = {0.25f, 0.5f, 0.75f, 0.8f};
  assert(ArRenderDevice_DrawSolidRect(
      &device, &rectangle, color, kArRenderBlendMode_Alpha));
  assert(backend.draw_geometry_count == 3);
  assert(!ArRenderTexture_IsValid(backend.last_texture));
  assert(backend.last_vertex_count == 4 && backend.last_index_count == 6);
  assert(backend.last_draw_state_present);
  assert(backend.last_draw_state.flags == kArRenderDrawState_Blend);
  assert(backend.last_draw_state.blend == kArRenderBlendMode_Alpha);
  assert(backend.last_vertices[0].position.x == 10.0f &&
         backend.last_vertices[0].position.y == 20.0f);
  assert(backend.last_vertices[2].position.x == 40.0f &&
         backend.last_vertices[2].position.y == 60.0f);
  assert(memcmp(&backend.last_vertices[0].color, &color, sizeof(color)) == 0);
  assert(!ArRenderDevice_DrawSolidRect(
      &device, &(ArRenderRectF){0.0f, 0.0f, 0.0f, 1.0f}, color,
      kArRenderBlendMode_Alpha));
  assert(!ArRenderDevice_DrawSolidRect(
      &device, &rectangle, (ArRenderColorF){1.0f, 1.0f, 1.0f, 1.1f},
      kArRenderBlendMode_Alpha));
  assert(backend.draw_geometry_count == 3);
  assert(ArRenderDevice_Present(&device));
  assert(backend.present_count == 1);

  ArRenderDevice_DestroyTexture(&device, texture);
  assert(backend.destroy_count == 2);
  assert(strcmp(ArRenderDevice_LastError(&device), "fake backend error") == 0);
  ArRenderDevice_Reset(&device);
  assert(!ArRenderDevice_IsReady(&device));
}

static void TestIncompleteBackendIsRejected(void) {
  FakeBackend backend = {0};
  ArRenderDevice device = {0};
  ArRenderBackendOps incomplete = kFakeOps;
  incomplete.present = NULL;
  assert(!ArRenderDevice_Init(
      &device, &incomplete, &backend, (ArRenderCapabilities){0}));
  assert(!ArRenderDevice_IsReady(&device));
}

static void TestDirtyUploadWorksWithoutANativeRenderer(void) {
  FakeBackend backend = {0};
  ArRenderDevice device = {0};
  assert(ArRenderDevice_Init(
      &device, &kFakeOps, &backend, (ArRenderCapabilities){0}));

  PresentationUploadMirror mirror = {0};
  PresentationUploadResult result = {0};
  ArRenderTexture texture = {42};
  uint32_t pixels[4] = {1, 2, 3, 4};
  assert(PresentationUploadMirror_UploadArgb8888(
      &mirror, &device, texture, (const uint8_t *)pixels,
      2, 2, 2 * (int)sizeof(uint32_t), 10, 20, &result));
  assert(result.changed && result.uploaded_bytes == sizeof(pixels));
  assert(backend.update_count == 1);
  assert(backend.last_update.x == 10 && backend.last_update.y == 20 &&
         backend.last_update.w == 2 && backend.last_update.h == 2);

  memset(&result, 0xff, sizeof(result));
  assert(PresentationUploadMirror_UploadArgb8888(
      &mirror, &device, texture, (const uint8_t *)pixels,
      2, 2, 2 * (int)sizeof(uint32_t), 10, 20, &result));
  assert(!result.changed && result.uploaded_bytes == 0);
  assert(backend.update_count == 1);

  pixels[3] = 5;
  assert(PresentationUploadMirror_UploadArgb8888(
      &mirror, &device, texture, (const uint8_t *)pixels,
      2, 2, 2 * (int)sizeof(uint32_t), 10, 20, &result));
  assert(result.changed && result.uploaded_bytes == sizeof(uint32_t));
  assert(backend.update_count == 2);
  assert(backend.last_update.x == 11 && backend.last_update.y == 21 &&
         backend.last_update.w == 1 && backend.last_update.h == 1);
  PresentationUploadMirror_Reset(&mirror);
}

int main(void) {
  TestDeviceDispatchAndCapabilities();
  TestIncompleteBackendIsRejected();
  TestDirtyUploadWorksWithoutANativeRenderer();
  puts("render_device_test: ok");
  return 0;
}
