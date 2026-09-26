#include "sim/sim3d/sim3d_textures.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "app/settings.h"
#include "present/present.h"
#include "sim/sim3d/sim3d.h"
#include "sim/sim_render_atlas.h"

Settings g_settings;
uint32_t g_sim_obj_atlas_pixels[kSimObjAtlasWidth * kSimObjAtlasHeight];
uint32_t g_sim3d_flat_pixels[kSim3DMaxWidth * kSim3DMaxHeight];
static FrameSlot frame;
static uint32_t pixels[16 * 8];
static bool menu_active, canvas_needed;
static uint32_t upload_mask;
static int attempts, created, destroyed, fail_at, fatals, transfers;
static bool alive[64], fail_upload;
static uint64_t bytes;
static ArRenderRectI uploaded;
static ArRenderTexture last_texture;

/* Capture and rendering algorithms have their own tests. Here their policy
 * results are controlled to test resource availability and upload ownership. */
SimRenderFeatureMask Settings_Sim3DRequestedFeatures(void) { return 0; }
bool PresentSimMenu_Active(const FrameSlot *slot) { assert(slot == &frame); return menu_active; }
uint32_t Sim3D_PlaneTextureUploadMask(SimRenderFeatureMask features, uint32_t captured) {
  assert(features == frame.sim.effective_features && captured == frame.sim.separated_plane_mask);
  return upload_mask;
}
Sim3DGroundSource Sim3D_ResolveGroundSource(SimRenderFeatureMask features,
                                           bool enabled, bool ready) {
  (void)features;
  (void)enabled;
  (void)ready;
  return kSim3DGround_Canvas;
}
void SimBackgroundVoxelRenderer_Upload(ArRenderDevice *device) { assert(device); }
bool SimBackgroundVoxelRenderer_Ready(uint32_t serial) { (void)serial; return false; }
void PresentSim3DCanvas_Upload(ArRenderDevice *device, bool needed) {
  assert(device);
  canvas_needed = needed;
}
void Sim3DPerformance_AddUpload(uint64_t count) { assert(count); bytes += count; transfers++; }
void SessionFatal_Request(const char *format, ...) { assert(format); fatals++; }
void Die(const char *message) { (void)message; abort(); }

static bool Create(void *ctx, const ArRenderTextureDesc *desc, ArRenderTexture *out) {
  (void)ctx;
  assert(desc->usage == kArRenderTextureUsage_Streaming);
  if (++attempts == fail_at) return false;
  assert(created + 1 < 64);
  alive[++created] = true;
  *out = (ArRenderTexture){(uintptr_t)created};
  return true;
}
static void Destroy(void *ctx, ArRenderTexture texture) {
  (void)ctx;
  assert(texture.value < 64 && alive[texture.value]);
  alive[texture.value] = false;
  destroyed++;
}
static bool Update(void *ctx, ArRenderTexture texture, const ArRenderRectI *rect,
                   const void *source, int pitch) {
  (void)ctx;
  assert(source && pitch > 0 && alive[texture.value]);
  last_texture = texture;
  if (rect) uploaded = *rect;
  return !fail_upload;
}
static const char *Error(void *ctx) { (void)ctx; return "injected allocation failure"; }
static const ArRenderBackendOps ops = {
  .struct_size = sizeof(ops), .create_texture = Create, .destroy_texture = Destroy,
  .update_texture = Update, .last_error = Error,
};

int main(void) {
  ArRenderDevice device = {.ops = &ops, .context = &created,
      .capabilities = {.flags = kArRenderCapability_StreamingTextures}};
  /* An atlas allocation failure does not discard working core textures. */
  fail_at = 1;
  Sim3DTextures_Create(&device);
  assert(Sim3DTextures_Ready() && !Sim3DTextures_BillboardsReady());
  g_settings.sim3d_mode = g_settings.sim3d_object_billboards = true;
  SettingDesc setting = {.field = &g_settings.sim3d_object_billboards};
  assert(!Sim3DTextures_ValidateSetting(&setting) && fatals == 1);
  g_settings.sim3d_mode = false;
  Sim3DTextures_Destroy(&device);
  assert(created == destroyed && !Sim3DTextures_Ready());

  /* A partial core family must not remain available to scene/menu consumers. */
  fail_at = attempts + 4;
  Sim3DTextures_Create(&device);
  assert(!Sim3DTextures_Ready() && Sim3DTextures_BillboardsReady());
  for (int i = 0; i < kSim3DPlane_Count; i++) assert(!Sim3DTextures_Layer(i).value);
  assert(!Sim3DTextures_Flat().value && !Sim3DTextures_Layer(-1).value);
  Sim3DTextures_Destroy(&device);
  assert(created == destroyed);

  fail_at = 0;
  Sim3DTextures_Create(&device);
  assert(Sim3DTextures_Ready() && Sim3DTextures_BillboardsReady());
  frame.snes_width = 16;
  frame.snes_height = 8;
  frame.sim.view = kSimView_Enhanced;
  frame.sim.separated_valid = true;
  frame.sim.separated_plane_mask = 1u << kSim3DPlane_Bg1Low;
  frame.sim3d_output_surfaces.planes[kSim3DPlane_Bg1Low] = (SrPpuSurfaceView){
    .data = (uint8_t *)pixels, .byte_size = sizeof(pixels),
    .pitch_bytes = 16 * 4, .width_pixels = 16, .height_pixels = 8,
    .flags = SR_PPU_SURFACE_BOUND, .pixel_format = SR_PPU_PIXEL_FORMAT_ARGB8888_U32,
  };
  Sim3DTextures_Upload(&device, &frame);
  assert(canvas_needed && transfers == 1 && bytes == sizeof(pixels));
  assert(last_texture.value == Sim3DTextures_Flat().value);
  Sim3DTextures_Upload(&device, &frame);
  assert(transfers == 1); /* Unchanged pixels reuse the uploaded texture. */
  frame.sim.effective_features = kSimFeature_GroundProjection;
  Sim3DTextures_Upload(&device, &frame);
  assert(transfers == 1); /* Projected view omits its unused flat fallback. */
  menu_active = true;
  Sim3DTextures_Upload(&device, &frame);
  assert(transfers == 2); /* Menu still needs captured planes omitted by the scene policy. */
  assert(last_texture.value == Sim3DTextures_Layer(kSim3DPlane_Bg1Low).value);
  pixels[19]++;
  fail_upload = true;
  Sim3DTextures_Upload(&device, &frame);
  assert(transfers == 2 && uploaded.w == 1 && uploaded.h == 1);
  fail_upload = false;
  Sim3DTextures_Upload(&device, &frame);
  assert(transfers == 3 && uploaded.w == 16 && uploaded.h == 8);
  Sim3DTextures_ResetUploads();
  Sim3DTextures_Upload(&device, &frame);
  assert(transfers == 4);
  frame.sim3d_output_surfaces.planes[kSim3DPlane_Bg1Low].byte_size--;
  pixels[0]++;
  Sim3DTextures_Upload(&device, &frame);
  assert(transfers == 4); /* Invalid producer views do not reach the backend. */
  Sim3DTextures_Destroy(&device);
  Sim3DTextures_Destroy(&device);
  assert(created == destroyed && !Sim3DTextures_BillboardsReady());
  puts("SIM texture ownership: availability, upload selection, retries and resets passed");
  return 0;
}
