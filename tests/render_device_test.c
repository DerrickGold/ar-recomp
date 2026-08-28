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
  int present_count;
  ArRenderRectI last_update;
  ArRenderTexture last_texture;
} FakeBackend;

static bool CreateTexture(void *context, const ArRenderTextureDesc *desc,
                          ArRenderTexture *out_texture) {
  FakeBackend *backend = context;
  assert(desc->format == kArRenderPixelFormat_Argb8888);
  backend->create_count++;
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
  backend->last_texture = target;
  return true;
}

static bool SetViewport(void *context, const ArRenderRectI *viewport) {
  (void)context;
  (void)viewport;
  return true;
}

static bool SetClipRect(void *context, const ArRenderRectI *clip) {
  (void)context;
  (void)clip;
  return true;
}

static bool Clear(void *context, ArRenderColorF color) {
  (void)context;
  (void)color;
  return true;
}

static bool DrawTexture(void *context, ArRenderTexture texture,
                        const ArRenderRectF *source,
                        const ArRenderRectF *destination) {
  FakeBackend *backend = context;
  (void)source;
  (void)destination;
  backend->draw_texture_count++;
  backend->last_texture = texture;
  return true;
}

static bool DrawGeometry(void *context, ArRenderTexture texture,
                         const ArRenderVertex2D *vertices, int vertex_count,
                         const int32_t *indices, int index_count) {
  FakeBackend *backend = context;
  assert(vertices && vertex_count == 3);
  assert(indices && index_count == 3);
  backend->draw_geometry_count++;
  backend->last_texture = texture;
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
             kArRenderCapability_Geometry,
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

  const ArRenderVertex2D vertices[3] = {0};
  const int32_t indices[3] = {0, 1, 2};
  assert(ArRenderDevice_DrawGeometry(
      &device, texture, vertices, 3, indices, 3));
  assert(backend.draw_geometry_count == 1);
  assert(ArRenderDevice_Present(&device));
  assert(backend.present_count == 1);

  ArRenderDevice_DestroyTexture(&device, texture);
  assert(backend.destroy_count == 1);
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
