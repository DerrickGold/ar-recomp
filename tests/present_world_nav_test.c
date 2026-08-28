#include "present_internal.h"
#include "present_sim3d_internal.h"
#include "render/render_device.h"
#include "settings.h"
#include "sim/sim_world_map.h"
#include "sim/sim_world_navigation_capture.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct FakeBackend {
  int output_width;
  int output_height;
  int use_output_coordinates_count;
  int get_output_size_count;
  int set_viewport_count;
  int clear_count;
  int draw_geometry_count;
  int fail_geometry_call;
  bool viewport_set;
  ArRenderRectI viewport;
  ArRenderVertex2D ground_vertices[4];
} FakeBackend;

ArRenderDevice g_render_device;

uint32_t g_sim_world_navigation_palace_pixels[
    kSimWorldNavigationCompositionWidth *
    kSimWorldNavigationCompositionHeight];
uint32_t g_sim_world_navigation_ui_pixels[
    kSimWorldNavigationCompositionWidth *
    kSimWorldNavigationCompositionHeight];

const float kPi = 3.14159265358979323846f;
const SimCloudLayer kSimCloudLayers[1] = {{0}};
const int kSimCloudLayerCount = 0;

uint64_t HostClock_Milliseconds(void) { return 0; }
uint64_t HostClock_Nanoseconds(void) { return 0; }

uint32_t SimCloudTexel(int x, int y) {
  (void)x;
  (void)y;
  return UINT32_C(0xffffffff);
}

int InsertSimGroundCoordinate(float *coordinates, int count, int capacity,
                              float coordinate) {
  (void)coordinates;
  (void)capacity;
  (void)coordinate;
  return count;
}

void SimShadowLight(const FrameSlot *slot, float *light_x, float *light_y) {
  (void)slot;
  *light_x = 0.0f;
  *light_y = 0.0f;
}

ArRenderTexture EnsureSimUnderlayTexture(const FrameSlot *slot) {
  (void)slot;
  return (ArRenderTexture){101};
}

ArRenderTexture SimUnderlayBlurTexture(uint32_t serial) {
  (void)serial;
  return ArRenderTexture_Invalid();
}

void DrawSimBackdrop(const FrameSlot *slot, ArRenderRectI viewport,
                     const float matrix[16]) {
  (void)slot;
  (void)viewport;
  (void)matrix;
}

static bool CreateTexture(void *context, const ArRenderTextureDesc *desc,
                          ArRenderTexture *texture) {
  (void)context;
  (void)desc;
  *texture = (ArRenderTexture){202};
  return true;
}

static void DestroyTexture(void *context, ArRenderTexture texture) {
  (void)context;
  (void)texture;
}

static bool UpdateTexture(void *context, ArRenderTexture texture,
                          const ArRenderRectI *destination,
                          const void *pixels, int pitch_bytes) {
  (void)context;
  (void)texture;
  (void)destination;
  (void)pixels;
  (void)pitch_bytes;
  return true;
}

static bool SetRenderTarget(void *context, ArRenderTexture target) {
  (void)context;
  (void)target;
  return true;
}

static bool UseOutputCoordinates(void *context) {
  FakeBackend *backend = context;
  backend->use_output_coordinates_count++;
  return true;
}

static bool GetOutputSize(void *context, int *width, int *height) {
  FakeBackend *backend = context;
  backend->get_output_size_count++;
  *width = backend->output_width;
  *height = backend->output_height;
  return true;
}

static bool SetViewport(void *context, const ArRenderRectI *viewport) {
  FakeBackend *backend = context;
  backend->set_viewport_count++;
  backend->viewport_set = viewport != NULL;
  if (viewport) backend->viewport = *viewport;
  return true;
}

static bool SetClipRect(void *context, const ArRenderRectI *clip) {
  (void)context;
  (void)clip;
  return true;
}

static bool Clear(void *context, ArRenderColorF color) {
  FakeBackend *backend = context;
  (void)color;
  backend->clear_count++;
  return true;
}

static bool DrawTexture(void *context, ArRenderTexture texture,
                        const ArRenderRectF *source,
                        const ArRenderRectF *destination,
                        const ArRenderDrawState *state) {
  (void)context;
  (void)texture;
  (void)source;
  (void)destination;
  (void)state;
  return true;
}

static bool DrawGeometry(void *context, ArRenderTexture texture,
                         const ArRenderVertex2D *vertices, int vertex_count,
                         const int32_t *indices, int index_count,
                         const ArRenderDrawState *state) {
  FakeBackend *backend = context;
  (void)indices;
  (void)index_count;
  (void)state;
  backend->draw_geometry_count++;
  if (backend->draw_geometry_count == backend->fail_geometry_call)
    return false;
  if (ArRenderTexture_IsValid(texture)) {
    assert(vertex_count == 4);
    memcpy(backend->ground_vertices, vertices,
           sizeof(backend->ground_vertices));
  }
  return true;
}

static bool Present(void *context) {
  (void)context;
  return true;
}

static const char *LastError(void *context) {
  (void)context;
  return "fake failure";
}

static const ArRenderBackendOps kFakeOps = {
  .struct_size = sizeof(ArRenderBackendOps),
  .create_texture = CreateTexture,
  .destroy_texture = DestroyTexture,
  .update_texture = UpdateTexture,
  .set_render_target = SetRenderTarget,
  .use_output_coordinates = UseOutputCoordinates,
  .get_output_size = GetOutputSize,
  .set_viewport = SetViewport,
  .set_clip_rect = SetClipRect,
  .clear = Clear,
  .draw_texture = DrawTexture,
  .draw_geometry = DrawGeometry,
  .present = Present,
  .last_error = LastError,
};

static FrameSlot WorldNavigationSlot(void) {
  FrameSlot slot = {0};
  slot.pixel_aspect = kPixelAspect_Crt43;
  slot.snes_width = kActRaiserAuthenticWidth;
  slot.snes_height = kActRaiserAuthenticHeight;
  slot.visible_width = kActRaiserAuthenticWidth;
  slot.sim.view = kSimView_WorldNavigation;
  slot.sim.world_navigation_brightness = 15;
  slot.sim.underlay_serial = 1;
  SimWorldNavigationScene *scene = &slot.sim.world_navigation_scene;
  scene->valid = true;
  scene->composition.valid = true;
  scene->composition.empty_animation = true;
  scene->source_to_screen[0] =
      (float)kActRaiserAuthenticWidth / kSimWorldMapPixels;
  scene->source_to_screen[4] =
      (float)kActRaiserAuthenticHeight / kSimWorldMapPixels;
  scene->ground[0] = (SimWorldNavigationGroundVertex){0, 0, 0.0f, 0.0f};
  scene->ground[1] = (SimWorldNavigationGroundVertex){128, 0, 1.0f, 0.0f};
  scene->ground[2] =
      (SimWorldNavigationGroundVertex){128, 128, 1.0f, 1.0f};
  scene->ground[3] = (SimWorldNavigationGroundVertex){0, 128, 0.0f, 1.0f};
  return slot;
}

static void TestAspectFitAndLocalGeometry(void) {
  FakeBackend backend = {
    .output_width = 1280,
    .output_height = 720,
  };
  assert(ArRenderDevice_Init(
      &g_render_device, &kFakeOps, &backend,
      (ArRenderCapabilities){0}));
  FrameSlot slot = WorldNavigationSlot();
  UploadWorldNavigationComposition(&slot);
  assert(PresentWorldNavigation3D(&slot) ==
         kPresentationOutcome_Complete);
  assert(backend.use_output_coordinates_count == 1);
  assert(backend.get_output_size_count == 1);
  assert(backend.clear_count == 1);
  /* Equal black margin/scene colours require only the ground draw. */
  assert(backend.draw_geometry_count == 1);
  assert(backend.set_viewport_count == 3);
  assert(!backend.viewport_set);
  assert(backend.viewport.x == 160 && backend.viewport.y == 0);
  assert(backend.viewport.w == 960 && backend.viewport.h == 720);
  assert(backend.ground_vertices[0].position.x == 0.0f);
  assert(backend.ground_vertices[0].position.y == 0.0f);
  assert(backend.ground_vertices[2].position.x == 960.0f);
  assert(backend.ground_vertices[2].position.y == 720.0f);
}

static void TestFailureRestoresFullOutput(void) {
  FakeBackend backend = {
    .output_width = 1280,
    .output_height = 720,
    .fail_geometry_call = 1,
  };
  assert(ArRenderDevice_Init(
      &g_render_device, &kFakeOps, &backend,
      (ArRenderCapabilities){0}));
  FrameSlot slot = WorldNavigationSlot();
  UploadWorldNavigationComposition(&slot);
  assert(PresentWorldNavigation3D(&slot) ==
         kPresentationOutcome_CoreFailure);
  assert(backend.set_viewport_count == 3);
  assert(!backend.viewport_set);
}

int main(void) {
  TestAspectFitAndLocalGeometry();
  TestFailureRestoresFullOutput();
  puts("present_world_nav_test: PASS");
  return 0;
}
