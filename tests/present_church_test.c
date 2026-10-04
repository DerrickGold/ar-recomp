#include "sim/church/present_church.h"
#include "sim/church/church_scene.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>

static SimBackgroundVoxelScene s_live_scene;
static uint32_t s_ground[512 * 512], s_objects[256 * 224], s_panels[256 * 224];
static uint32_t s_actor_pixel, s_ui_pixel, s_removed_pixel;
static uint32_t s_column_pixel, s_column_corner;
static unsigned s_next_texture, s_draws;
static uint32_t s_columns[28 * 144];
const uint32_t *ChurchArt_ColumnPixels(void) { return s_columns; }
static ArRenderRectI s_actor_bounds[2];
static bool s_reject_upload, s_reference_altar, s_fail_scene, s_fail_restore;
static const char *Error(void *ctx) {
  (void)ctx;
  return "injected church failure";
}
static bool Capture(void *ctx, ArRenderTargetState *out) {
  (void)ctx;
  *out = (ArRenderTargetState){.valid = true};
  return true;
}
static bool Restore(void *ctx, const ArRenderTargetState *state) {
  (void)ctx;
  assert(state->valid);
  return !s_fail_restore;
}
const SimBackgroundVoxelScene *SimBackgroundVoxels_Scene(void) { return &s_live_scene; }
const uint32_t *SimBackgroundVoxels_GroundPixels(void) { return s_ground; }
bool ChurchLandscape_Capture(ChurchLandscape *out, const SimBackgroundVoxelScene *town) {
  assert(town == &s_live_scene);
  out->mountain_atlas[0] = 0xffabcdef;
  out->world_pixels[0] = 0xff1122ee;
  out->world_valid = true;
  return true;
}
void ChurchScene_Reset(ArRenderDevice *device) { (void)device; }
bool ChurchScene_Draw(ArRenderDevice *device, ArRenderRectI viewport,
                      const SimBackgroundVoxelScene *town, const uint32_t *ground,
                      const ChurchSceneOptions *options) {
  (void)device;
  (void)viewport;
  assert(town->town == 1 && town->object_count == 1);
  assert(ground[0] == 0xff123456);
  assert(options->landscape && options->landscape->world_valid);
  assert(options->landscape->mountain_atlas[0] == 0xffabcdef);
  assert(options->landscape->world_pixels[0] == 0xff1122ee);
  assert(ArRenderTexture_IsValid(options->people));
  assert(options->brightness == 1);
  assert(options->reference_altar == s_reference_altar);
  assert(options->light_azimuth_deg == 157 && options->light_elevation_deg == 42);
  assert(options->shading == 1 && options->style == 2 && options->town_serial == 7);
  assert(!memcmp(options->actor_bounds, s_actor_bounds, sizeof(s_actor_bounds)));
  s_draws++;
  return !s_fail_scene;
}
static bool Create(void *context, const ArRenderTextureDesc *desc, ArRenderTexture *out) {
  (void)context;
  assert(desc->width == 256 && (desc->height == 512 || desc->height == 224));
  *out = (ArRenderTexture){++s_next_texture};
  return true;
}
static void Destroy(void *context, ArRenderTexture texture) {
  (void)context;
  (void)texture;
}
static bool Update(void *context, ArRenderTexture texture, const ArRenderRectI *rect,
                   const void *pixels, int pitch) {
  (void)context;
  (void)texture;
  (void)rect;
  assert(pitch == 256 * 4);
  if (s_reject_upload) return false;
  const uint32_t *p = pixels;
  if (texture.value == 1) {
    s_actor_pixel = p[224 * 256];
    s_column_pixel = p[300 * 256 + 10];
    s_column_corner = p[272 * 256];
    s_removed_pixel = p[72 * 256 + 96];
    s_ui_pixel = p[112 * 256 + 160];
  }
  return true;
}
static bool Draw(void *context, ArRenderTexture texture, const ArRenderRectF *source,
                 const ArRenderRectF *destination, const ArRenderDrawState *state) {
  (void)context;
  (void)texture;
  (void)destination;
  (void)state;
  assert(source->w == 256 && source->h == 224);
  s_draws++;
  return true;
}
static SrPpuSurfaceView Surface(uint32_t *pixels) {
  return (SrPpuSurfaceView){.data = (uint8_t *)pixels,
                            .byte_size = sizeof(s_objects),
                            .pitch_bytes = 256 * 4,
                            .width_pixels = 256,
                            .height_pixels = 224,
                            .flags = SR_PPU_SURFACE_BOUND,
                            .pixel_format = SR_PPU_PIXEL_FORMAT_ARGB8888_U32};
}
int main(void) {
  const ArRenderBackendOps ops = {.struct_size = sizeof(ops),
                                  .create_texture = Create,
                                  .destroy_texture = Destroy,
                                  .update_texture = Update,
                                  .draw_texture = Draw,
                                  .capture_render_target_state = Capture,
                                  .restore_render_target_state = Restore,
                                  .last_error = Error};
  ArRenderDevice device = {.ops = &ops, .context = &s_next_texture};
  device.capabilities.flags = kArRenderCapability_ScopedRenderTargets;
  static FrameSlot slot;
  slot.church_enabled = true;
  slot.church_town = 1;
  slot.hud_split_height = 32;
  for (int width = 1194; width <= 2134; width += 200) {
    const ArRenderRectI viewport = {16, 24, width, 896};
    ArRenderRectF inset = {900, 560, 384, 288};
    PresentChurch_PlaceComparison(&slot, viewport, 26, 16, &inset);
    assert(inset.x >= viewport.x && inset.y - 16 >= viewport.y + 128);
    /* Native dialogue starts at row 152; the right menu starts at x=136. */
    assert(inset.y + inset.h + 16 < viewport.y + 152 * 4);
    assert(inset.x + inset.w + 16 < viewport.x + width / 2);
  }
  slot.church_town = 0;
  slot.hud_split_height = 0;

  slot.inidisp = 15;
  s_columns[28 * 28 + 10] = 0xffb5a584;
  slot.sim.light_azimuth_deg = 157;
  slot.sim.light_elevation_deg = 42;
  slot.sim.background_voxel_shading = 1;
  slot.sim.background_voxel_style = 2;
  slot.sim.view = kSimView_Enhanced;
  slot.sim.town = 1;
  slot.sim.background_voxel_serial = 7;
  s_live_scene.town = 1;
  s_live_scene.object_count = 1;
  s_live_scene.objects[0].kind = kSimBackgroundVoxel_Cathedral;
  s_ground[0] = 0xff123456;
  PresentChurch_Upload(&device, &slot);
  assert(!PresentChurch_Active(&slot));
  slot.sim.view = kSimView_None;
  slot.church_town = 1;
  slot.ppu_surfaces.overlays[SR_PPU_OVERLAY_OBJ][0] = Surface(s_objects);
  slot.ppu_surfaces.overlays[SR_PPU_OVERLAY_BG2][0] = Surface(s_panels);
  /* The 3D altar must not require a captured native church background. */
  s_objects[72 * 256 + 96] = 0xffeecc88;
  s_actor_bounds[0] = (ArRenderRectI){0, 0, 1, 1};
  s_objects[112 * 256 + 160] = 0xff2255aa;
  PresentChurch_Upload(&device, &slot);
  assert(PresentChurch_Active(&slot));
  assert(s_actor_pixel == 0xffeecc88 && s_removed_pixel == 0 && s_ui_pixel == 0xff2255aa);
  assert(s_column_pixel == 0xffb5a584 && s_column_corner == 0);
  /* The room has no SIM parameters; the exterior must retain those from town. */
  slot.sim.light_azimuth_deg = slot.sim.light_elevation_deg = 0;
  slot.sim.background_voxel_shading = slot.sim.background_voxel_style = 0;
  /* Native room setup may overwrite live town products after upload. */
  memset(&s_live_scene, 0, sizeof(s_live_scene));
  s_ground[0] = 0;
  assert(PresentChurch_Draw(&device, &slot, (ArRenderRectI){0, 0, 1280, 800}) ==
         kPresentationOutcome_Complete);
  assert(s_draws == 1);
  slot.church_reference_altar = s_reference_altar = true;
  assert(PresentChurch_Draw(&device, &slot, (ArRenderRectI){0, 0, 1280, 800}) ==
         kPresentationOutcome_Complete);
  assert(s_draws == 2);
  /* Animation and a one-person audience update the contact silhouette without
   * stale bounds for the person who disappeared. Transparent RGB is ignored. */
  s_objects[72 * 256 + 96] = 0x00ffffff;
  s_objects[80 * 256 + 132] = 0xff7788aa;
  s_objects[100 * 256 + 143] = 0xff99bbcc;
  s_actor_bounds[0] = (ArRenderRectI){0};
  s_actor_bounds[1] = (ArRenderRectI){4, 8, 12, 21};
  PresentChurch_Upload(&device, &slot);
  assert(PresentChurch_Draw(&device, &slot, (ArRenderRectI){0, 0, 1280, 800}) ==
         kPresentationOutcome_Complete);
  assert(s_ui_pixel == 0xff2255aa && s_draws == 3);
  /* Borrowed surfaces expire each emulated tick; the owned town survives. */
  slot.ppu_surfaces.lifetime_generation++;
  PresentChurch_Upload(&device, &slot);
  assert(PresentChurch_Active(&slot));
  PresentChurch_ResetResources(&device);
  assert(!PresentChurch_Active(&slot));
  PresentChurch_Upload(&device, &slot);
  assert(PresentChurch_Active(&slot));
  assert(PresentChurch_Draw(&device, &slot, (ArRenderRectI){0, 0, 1280, 800}) ==
         kPresentationOutcome_Complete);
  s_fail_restore = true;
  assert(PresentChurch_Draw(&device, &slot, (ArRenderRectI){0, 0, 1280, 800}) ==
         kPresentationOutcome_CoreFailure);
  s_fail_restore = false;
  s_fail_scene = true;
  assert(PresentChurch_Draw(&device, &slot, (ArRenderRectI){0, 0, 1280, 800}) ==
         kPresentationOutcome_OptionalOmitted);
  assert(!PresentChurch_Active(&slot));
  PresentChurch_Upload(&device, &slot);
  assert(!PresentChurch_Active(&slot)); /* No repeated failing bake every frame. */
  s_fail_scene = false;
  slot.sim.view = kSimView_Enhanced;
  slot.church_town = 0;
  s_live_scene.town = 1;
  s_live_scene.object_count = 1;
  s_live_scene.objects[0].kind = kSimBackgroundVoxel_Cathedral;
  s_ground[0] = 0xff123456;
  PresentChurch_Upload(&device, &slot);
  slot.sim.view = kSimView_None;
  slot.church_town = 1;
  PresentChurch_Upload(&device, &slot);
  assert(PresentChurch_Active(&slot)); /* Retry on the next visit. */
  PresentChurch_ResetResources(&device);
  PresentChurch_Upload(&device, &slot);
  assert(PresentChurch_Active(&slot));
  slot.church_load_generation++;
  PresentChurch_Upload(&device, &slot);
  assert(!PresentChurch_Active(&slot));
  slot.church_town = 2;
  assert(!PresentChurch_Active(&slot));
  slot.church_town = 1;
  s_reject_upload = true;
  PresentChurch_Upload(&device, &slot);
  assert(!PresentChurch_Active(&slot));
  s_reject_upload = false;
  slot.church_town = 0;
  PresentChurch_Upload(&device, &slot);
  slot.church_town = 1;
  PresentChurch_Upload(&device, &slot);
  assert(!PresentChurch_Active(&slot));
  PresentChurch_Reset(&device);
  puts("church snapshot, SIM settings, native art extraction, UI, and fallback: passed");
  return 0;
}
