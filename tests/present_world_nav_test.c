#include "present_internal.h"
#include "present_sim3d_internal.h"
#include "present_world_nav_geometry.h"
#include "actraiser/actraiser_localization_world_navigation.h"
#include "render/render_device.h"
#include "render/localized_text_presenter.h"
#include "settings.h"
#include "sim/sim_town_terrain.h"
#include "sim/sim_town_ground_art.h"
#include "sim/sim_world_map.h"
#include "sim/sim_world_navigation_capture.h"
#include "sim/sim_world_navigation_clouds.h"
#include "sim/sim_world_navigation_terrain.h"
#include "sim/sim3d_depth_pass.h"
#include "sim/sim3d_performance.h"
#include "sim/sim_background_voxel_model_cache.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
  int space_draws;
  int cloud_body_draws;
  int cloud_shadow_draws;
  int cloud_ocean_shadow_draws;
  int ground_uploads;
  uint64_t ground_upload_bytes;
  uint32_t *ground_upload_mirror;
  bool fail_ground_upload;
  int mountain_uploads;
  uint64_t mountain_upload_bytes;
  uint32_t *mountain_upload_mirror;
  bool fail_mountain_upload;
  bool viewport_set;
  ArRenderRectI viewport;
  int ground_vertex_count;
  int ground_index_count;
  ArRenderVertex2D ground_vertices[4];
  bool check_map_edge_opacity;
  unsigned opaque_edge_land, faded_edge_water;
  bool check_atmosphere_enclosure;
  bool atmosphere_seen;
  float atmosphere_support[64];
  bool track_markers;
  bool allow_far_clipped_depth;
  bool require_frustum_clipped;
  int sky_backdrop_draws;
  ArRenderVertex2D sky_backdrop_vertices[10];
  bool track_palace_focus;
  int palace_focus_vertices;
  ArRenderPointF palace_focus_uv, palace_focus_position;
  int palace_draws, ui_draws, label_draws;
  ArRenderRectF palace_rect, ui_rect, label_rect;
} FakeBackend;

ArRenderDevice g_render_device;
static FakeBackend *s_backend;

static int depth_solid_faces;
static int depth_terrain_faces;
static int depth_world_mountain_faces;
static int depth_width, depth_height;
static int depth_expected_shadow_terrain_faces;
static int depth_shadow_terrain_faces, depth_shadow_ocean_faces, depth_cloud_faces;
static uint64_t depth_world_mountain_hash;
static bool depth_begin_failure;
static int depth_fail_ocean_batch;
static int depth_ocean_batches, depth_ocean_quads;
static int depth_volume_faces, volume_uploads;
static bool volume_upload_failure;
static float volume_previous_depth;
static int depth_near_volume_faces;
static int depth_upper_volume_faces;
static float near_volume_max_depth, far_volume_min_depth;
static bool depth_collecting;
enum { kDepthSurfaceSlots = 131072 };
static struct { bool used, terrain; float point[3]; } depth_surface[kDepthSurfaceSlots];

static bool CheckDepthSurface(const Sim3DDepthVertex *v, bool insert) {
  const float point[3] = {v->x, v->y, v->depth};
  uint32_t bits[3];
  memcpy(bits, point, sizeof(bits));
  uint32_t at = (bits[0] * 16777619u ^ bits[1] * 2166136261u ^ bits[2]) % kDepthSurfaceSlots;
  while (depth_surface[at].used && memcmp(depth_surface[at].point, point, sizeof(point)))
    at = (at + 1) % kDepthSurfaceSlots;
  if (insert) {
    depth_surface[at].used = true;
    depth_surface[at].terrain = v->uv.x >= 0;
    memcpy(depth_surface[at].point, point, sizeof(point));
  } else assert(depth_surface[at].used); /* Weather uses exact opaque geometry. */
  return depth_surface[at].terrain;
}

static bool QuadTouchesViewport(const Sim3DDepthVertex vertices[4]) {
  float left = vertices[0].x, right = left, top = vertices[0].y, bottom = top;
  for (int p = 1; p < 4; p++) {
    left = fminf(left, vertices[p].x); right = fmaxf(right, vertices[p].x);
    top = fminf(top, vertices[p].y); bottom = fmaxf(bottom, vertices[p].y);
  }
  return right >= -1 && left <= depth_width + 1 && bottom >= -1 && top <= depth_height + 1;
}

bool Sim3DDepthPass_UploadAtlasRegions(
    ArRenderDevice *device, Sim3DDepthPassLayer layer,
    const uint32_t *pixels, int width, int height, int pitch,
    const ArRenderRectI *regions, int region_count) {
  (void)device;
  assert(layer == kSim3DDepthPass_Ground || layer == kSim3DDepthPass_GroundBlur ||
         layer == kSim3DDepthPass_Cloud || layer == kSim3DDepthPass_WorldMountain ||
         layer == kSim3DDepthPass_VolumeCloud);
  if (layer == kSim3DDepthPass_VolumeCloud) {
    volume_uploads++;
    if (volume_upload_failure) return false;
  }
  assert(pixels && width > 0 && height > 0 && pitch >= width * 4);
  assert(regions && region_count > 0);
  for (int i = 0; i < region_count; i++) {
    assert(regions[i].x >= 0 && regions[i].y >= 0 && regions[i].w > 0 && regions[i].h > 0);
    assert(regions[i].x + regions[i].w <= width && regions[i].y + regions[i].h <= height);
  }
  if (layer == kSim3DDepthPass_Ground) {
    FakeBackend *backend = device->context;
    backend->ground_uploads++;
    for (int i = 0; i < region_count; i++)
      backend->ground_upload_bytes += (uint64_t)regions[i].w * regions[i].h * 4;
    if (backend->fail_ground_upload) return false;
    if (backend->ground_upload_mirror) {
      assert(width == 2048 && height == 2048);
      for (int i = 0; i < region_count; i++)
        for (int y = regions[i].y; y < regions[i].y + regions[i].h; y++)
          memcpy(backend->ground_upload_mirror + y * width + regions[i].x,
              (const uint8_t *)pixels + y * pitch + regions[i].x * 4, regions[i].w * 4);
      for (int y = 0; y < height; y++)
        assert(!memcmp(backend->ground_upload_mirror + y * width,
            (const uint8_t *)pixels + y * pitch, width * 4));
    }
  }
  if (layer == kSim3DDepthPass_WorldMountain) {
    FakeBackend *backend = device->context;
    backend->mountain_uploads++;
    if (backend->fail_mountain_upload) return false;
    for (int i = 0; i < region_count; i++)
      backend->mountain_upload_bytes += (uint64_t)regions[i].w * regions[i].h * 4;
    if (backend->mountain_upload_mirror) {
      assert(width == 512 && height == 512);
      for (int i = 0; i < region_count; i++)
        for (int y = regions[i].y; y < regions[i].y + regions[i].h; y++)
          memcpy(backend->mountain_upload_mirror + y * width + regions[i].x,
              (const uint8_t *)pixels + y * pitch + regions[i].x * 4, regions[i].w * 4);
      for (int y = 0; y < height; y++)
        assert(!memcmp(backend->mountain_upload_mirror + y * width,
            (const uint8_t *)pixels + y * pitch, width * 4));
    }
  }
  if (layer == kSim3DDepthPass_Cloud) {
    const int period = kSimWorldNavigationCloudWidth - 1;
    assert(width == kSimWorldNavigationCloudWidth * 2);
    assert(height == kSimWorldNavigationCloudHeight * 3);
    /* Endpoint-inclusive baking has width-1 intervals per revolution.
     * Unwrapped quads must sample the same filtered texels on either copy. */
    for (int y = 0; y < height; y++) {
      const uint32_t *row = (const uint32_t *)((const uint8_t *)pixels + y * pitch);
      for (int x = period; x < width; x++) assert(row[x] == row[x - period]);
    }
  }
  return true;
}

bool Sim3DDepthPass_Begin(ArRenderDevice *device, int width, int height,
                         ArRenderFilter filter) {
  assert(device && device->context);
  /* Backdrops inherit output setup from their caller, so bind this pass's
   * device directly instead of relying on UseOutputCoordinates being called. */
  s_backend = device->context;
  (void)filter;
  assert(width > 0 && height > 0);
  depth_width = width; depth_height = height;
  depth_expected_shadow_terrain_faces = 0;
  depth_shadow_terrain_faces = depth_shadow_ocean_faces = depth_cloud_faces = 0;
  depth_solid_faces = depth_terrain_faces = 0;
  depth_world_mountain_faces = 0;
  depth_ocean_batches = depth_ocean_quads = 0;
  depth_volume_faces = 0;
  depth_near_volume_faces = 0;
  depth_upper_volume_faces = 0;
  s_backend->palace_focus_vertices = 0;
  s_backend->opaque_edge_land = s_backend->faded_edge_water = 0;
  near_volume_max_depth = 0;
  far_volume_min_depth = 1;
  volume_previous_depth = 1;
  depth_world_mountain_hash = UINT64_C(14695981039346656037);
  memset(depth_surface, 0, sizeof(depth_surface));
  depth_collecting = !depth_begin_failure;
  return depth_collecting;
}

bool Sim3DDepthPass_AppendQuad(Sim3DDepthPassLayer layer,
                              const Sim3DDepthVertex vertices[4]) {
  assert(depth_collecting);
  for (int i = 0; i < 4; i++) {
    assert(isfinite(vertices[i].x) && isfinite(vertices[i].y));
    assert(isfinite(vertices[i].depth) && vertices[i].depth >= 0.0f);
    assert(vertices[i].depth <= 1.0f || s_backend->allow_far_clipped_depth);
    if (s_backend->require_frustum_clipped) {
      assert(vertices[i].x >= -.001f && vertices[i].x <= depth_width + .001f);
      assert(vertices[i].y >= -.001f && vertices[i].y <= depth_height + .001f);
    }
    if (layer == kSim3DDepthPass_Ground) CheckDepthSurface(&vertices[i], true);
    if (layer == kSim3DDepthPass_CloudShadow || layer == kSim3DDepthPass_GroundHaze ||
        layer == kSim3DDepthPass_GroundBlur) CheckDepthSurface(&vertices[i], false);
  }
  if (layer == kSim3DDepthPass_Solid) depth_solid_faces++;
  if (layer == kSim3DDepthPass_VolumeCloud) {
    assert(QuadTouchesViewport(vertices));
    for (int p = 0; p < 4; p++) {
      assert(vertices[p].depth <= volume_previous_depth + .000001f);
      assert(vertices[p].uv.x >= 0 && vertices[p].uv.x <= 1);
      assert(vertices[p].uv.y >= 0 && vertices[p].uv.y <= 1);
    }
    volume_previous_depth = vertices[0].depth;
    depth_volume_faces++;
    bool upper = true;
    for (int p = 0; p < 4; p++) upper &= vertices[p].y < depth_height * .34f;
    if (upper) depth_upper_volume_faces++;
    if (vertices[0].color.a < .699f) {
      depth_near_volume_faces++;
      near_volume_max_depth = fmaxf(near_volume_max_depth, vertices[0].depth);
    } else far_volume_min_depth = fminf(far_volume_min_depth, vertices[0].depth);
  }
  if (layer == kSim3DDepthPass_Ground && vertices[0].uv.x < 0) depth_ocean_quads++;
  if (layer == kSim3DDepthPass_CloudShadow || layer == kSim3DDepthPass_Cloud) {
    assert(QuadTouchesViewport(vertices));
    if (layer == kSim3DDepthPass_Cloud) depth_cloud_faces++;
    else if (CheckDepthSurface(vertices, false)) depth_shadow_terrain_faces++;
    else depth_shadow_ocean_faces++;
  }
  if (layer == kSim3DDepthPass_WorldMountain) {
    depth_world_mountain_faces++;
    for (int p = 0; p < 4; p++) {
      const Sim3DDepthVertex *v = &vertices[p];
      const float values[] = {v->x, v->y, v->depth, v->uv.x, v->uv.y,
          v->color.r, v->color.g, v->color.b, v->color.a};
      const uint8_t *bytes = (const uint8_t *)values;
      for (size_t i = 0; i < sizeof(values); i++)
        depth_world_mountain_hash = (depth_world_mountain_hash ^ bytes[i]) * UINT64_C(1099511628211);
    }
  }
  if (layer == kSim3DDepthPass_Ground && vertices[0].uv.x >= 0.0f) {
    if (s_backend->check_map_edge_opacity) {
      const int x = (int)lroundf(vertices[0].uv.x * 128);
      const int y = (int)lroundf(vertices[0].uv.y * 128);
      if (x < 10 || y < 10 || x >= 118 || y >= 118) {
        bool opaque = true;
        for (int p = 0; p < 4; p++) opaque &= vertices[p].color.a == 1;
        if (!SimWorldMap_CellIsOpenWater(x, y)) {
          assert(opaque); /* No corner of a mixed coast/land cell may fade. */
          s_backend->opaque_edge_land++;
        } else if (!opaque) s_backend->faded_edge_water++;
      }
    }
    depth_terrain_faces++;
    if (QuadTouchesViewport(vertices)) depth_expected_shadow_terrain_faces++;
    s_backend->ground_vertex_count = 129 * 129;
    s_backend->ground_index_count = 128 * 128 * 6;
    for (int i = 0; i < 4; i++) {
      const Sim3DDepthVertex *v = &vertices[i];
      const float u = v->uv.x, t = v->uv.y;
      if (s_backend->track_palace_focus &&
          fabsf(u - s_backend->palace_focus_uv.x) < .000001f &&
          fabsf(t - s_backend->palace_focus_uv.y) < .000001f) {
        s_backend->palace_focus_vertices++;
        s_backend->palace_focus_position = (ArRenderPointF){v->x, v->y};
      }
      int at = u == 0 && t == 0 ? 0 :
          u == 0.5f && t == 0.5f ? 1 :
          u == 1 && t == 1 ? 2 : u == 0.5f && t == 0 ? 3 : -1;
      if (at >= 0) s_backend->ground_vertices[at] =
          (ArRenderVertex2D){{v->x, v->y}, v->color, v->uv};
      if (s_backend->check_atmosphere_enclosure) {
        assert(s_backend->atmosphere_seen);
        for (int direction = 0; direction < 64; direction++) {
          const float angle = 6.28318530718f * direction / 64;
          assert(cosf(angle) * v->x + sinf(angle) * v->y <=
                 s_backend->atmosphere_support[direction] + 0.01f);
        }
      }
    }
  }
  return true;
}

bool Sim3DDepthPass_AppendQuads(Sim3DDepthPassLayer layer,
                               const Sim3DDepthVertex *vertices, size_t count) {
  assert(layer == kSim3DDepthPass_Solid || layer == kSim3DDepthPass_Cloud || layer == kSim3DDepthPass_CloudShadow ||
         layer == kSim3DDepthPass_VolumeCloud ||
         layer == kSim3DDepthPass_WorldMountain || layer == kSim3DDepthPass_Ground ||
         layer == kSim3DDepthPass_GroundHaze || layer == kSim3DDepthPass_GroundBlur);
  const bool ocean = layer == kSim3DDepthPass_Ground && count && vertices[0].uv.x < 0;
  if (ocean && ++depth_ocean_batches == depth_fail_ocean_batch) return false;
  for (size_t i = 0; i < count; i++) {
    for (int c = 0; c < 4; c++) {
      const Sim3DDepthVertex *v = &vertices[i * 4 + c];
      if (ocean || layer == kSim3DDepthPass_Solid) {
        assert(v->uv.x == -1 && v->uv.y == -1);
      } else if (layer != kSim3DDepthPass_GroundHaze) {
        assert(v->uv.x >= 0 && v->uv.x <= 1);
        assert(v->uv.y >= 0 && v->uv.y <= 1);
      }
      assert(v->color.a >= 0 && v->color.a <= 1);
    }
    assert(Sim3DDepthPass_AppendQuad(layer, vertices + i * 4));
  }
  if (layer == kSim3DDepthPass_Cloud) s_backend->cloud_body_draws++;
  else if (layer == kSim3DDepthPass_CloudShadow) {
    if (CheckDepthSurface(vertices, false)) s_backend->cloud_shadow_draws++;
    else s_backend->cloud_ocean_shadow_draws++;
  }
  return true;
}

ArRenderTexture Sim3DDepthPass_Submit(ArRenderDevice *device,
                                     ArRenderTexture shadow_texture) {
  (void)device;
  (void)shadow_texture;
  assert(depth_collecting);
  depth_collecting = false;
  return (ArRenderTexture){303};
}

uint32_t g_sim_world_navigation_palace_pixels[
    kSimWorldNavigationCompositionWidth *
    kSimWorldNavigationCompositionHeight];
uint32_t g_sim_world_navigation_label_pixels[
    kSimWorldNavigationCompositionWidth *
    kSimWorldNavigationCompositionHeight];
uint32_t g_sim_world_navigation_plaque_pixels[
    kSimWorldNavigationCompositionWidth *
    kSimWorldNavigationCompositionHeight];

static bool s_localized_record_available;
static bool s_localized_prepare_success;
static int s_localized_draws;
static float s_localized_brightness;
static ArRenderRectI s_localized_bounds;

const ArLocalizationScreenTextRecord *ArLocalizationFrame_FindScreenText(
    const ArLocalizationFrame *frame, uint32_t surface_id) {
  (void)frame;
  static const ArLocalizationScreenTextRecord record = {
      .surface_id = kActRaiserLocalizationWorldNavigationSurface,
      .x = 156, .y = 25, .width = 76, .height = 8};
  return s_localized_record_available && surface_id == record.surface_id
      ? &record : NULL;
}

bool ArLocalizedTextPresenter_PrepareScreenText(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    uint32_t surface_id, ArRenderRectI bounds,
    ArLocalizedPreparedFrame *prepared) {
  (void)device;
  (void)frame;
  (void)surface_id;
  s_localized_bounds = bounds;
  if (prepared) memset(prepared, 0, sizeof(*prepared));
  return s_localized_prepare_success;
}

bool ArLocalizedTextPresenter_DrawWithBrightness(
    ArRenderDevice *device, const ArLocalizedPreparedFrame *prepared,
    float brightness) {
  (void)device;
  (void)prepared;
  ++s_localized_draws;
  s_localized_brightness = brightness;
  return true;
}

uint64_t HostClock_Milliseconds(void) { return 0; }
uint64_t HostClock_Nanoseconds(void) { return 0; }

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
  s_backend = backend;
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
  FakeBackend *backend = context;
  (void)texture;
  (void)source;
  (void)destination;
  (void)state;
  if (backend->require_frustum_clipped && !source) {
    assert(state && (state->flags & kArRenderDrawState_Blend));
    assert(state->blend == kArRenderBlendMode_AlphaPremultiplied);
  }
  if (backend->track_markers && source) {
    if (source->w == 16 && source->h == 16) {
      backend->palace_draws++; backend->palace_rect = *destination;
    } else if (source->w == 32 && source->h == 8) {
      backend->ui_draws++; backend->ui_rect = *destination;
    } else if (source->w == 24 && source->h == 8) {
      backend->label_draws++; backend->label_rect = *destination;
    }
  }
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
  if (!ArRenderTexture_IsValid(texture) && vertex_count == 12 && index_count == 18) {
    assert(state && (state->flags & kArRenderDrawState_Blend));
    assert(state->blend == kArRenderBlendMode_Alpha);
  }
  if (!ArRenderTexture_IsValid(texture) && vertex_count == 10 && index_count == 24) {
    backend->sky_backdrop_draws++;
    memcpy(backend->sky_backdrop_vertices, vertices, sizeof(backend->sky_backdrop_vertices));
    assert(vertices[0].position.y == 0);
    for (int row = 1; row < 5; row++) {
      assert(vertices[row * 2].position.y > vertices[(row - 1) * 2].position.y);
      assert(vertices[row * 2].color.b >= vertices[(row - 1) * 2].color.b);
      assert(vertices[row * 2].color.r >= vertices[(row - 1) * 2].color.r);
    }
    assert(vertices[6].position.y > vertices[8].position.y * .51f);
    assert(vertices[6].position.y < vertices[8].position.y * .54f);
    /* Keep the rich upper blue in the visible windows, not behind the HUD. */
    assert(vertices[2].position.y > vertices[8].position.y * .19f);
    assert(!memcmp(&vertices[0].color, &vertices[2].color, sizeof(vertices[0].color)));
  }
  if (backend->draw_geometry_count == backend->fail_geometry_call)
    return false;
  if (backend->check_atmosphere_enclosure &&
      !ArRenderTexture_IsValid(texture) && vertex_count == 1 + 48 * 96 &&
      vertices[0].color.a < 1.0f) {
    backend->atmosphere_seen = true;
    float previous_alpha = vertices[0].color.a;
    for (int ring = 0; ring < 48; ring++) {
      const float alpha = vertices[1 + ring * 96].color.a;
      assert(alpha >= 0 && alpha <= previous_alpha + .000001f);
      for (int sector = 1; sector < 96; sector++)
        assert(vertices[1 + ring * 96 + sector].color.a == alpha);
      previous_alpha = alpha;
    }
    assert(previous_alpha < .000001f); /* No bright line at the outer tangent. */
    for (int direction = 0; direction < 64; direction++) {
      const float angle = 2.0f * kPi * direction / 64.0f;
      const float dx = cosf(angle), dy = sinf(angle);
      float support = -INFINITY;
      for (int i = 0; i < vertex_count; i++)
        support = fmaxf(support,
            dx * vertices[i].position.x + dy * vertices[i].position.y);
      backend->atmosphere_support[direction] = support;
    }
  }
  if (!ArRenderTexture_IsValid(texture) && vertex_count == 4 + 256 * 4) {
    backend->space_draws++;
    assert(vertices[0].color.r < 0.02f && vertices[0].color.b < 0.03f);
    assert(index_count == 6 + 256 * 6);
  } else if (ArRenderTexture_IsValid(texture) && vertex_count == 48 * 48 * 4) {
    assert(index_count == 48 * 48 * 6);
    if (vertices[0].color.r > 0.0f) backend->cloud_body_draws++;
    else backend->cloud_shadow_draws++;
    for (int i = 0; i < vertex_count; i++) {
      assert(isfinite(vertices[i].position.x) && isfinite(vertices[i].position.y));
      assert(vertices[i].tex_coord.x >= 0 && vertices[i].tex_coord.x <= 1);
      assert(vertices[i].tex_coord.y >= 0 && vertices[i].tex_coord.y <= 1);
      assert(vertices[i].color.a >= 0 && vertices[i].color.a <= 1);
    }
  } else {
    /* Textured ground must never regress to painter-ordered 2D geometry. */
    assert(!ArRenderTexture_IsValid(texture));
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
  slot.sim.world_navigation_atmosphere = true;
  slot.sim.world_navigation_models = true;
  slot.sim.world_navigation_relief = true;
  slot.sim.world_navigation_cloud_shadows = true;
  slot.sim.underlay_serial = 1;
  slot.sim.landscape_height_pct = 100;
  slot.sim.height_scale_x100 = 100;
  slot.sim.background_voxel_detail = kSimBackgroundVoxelDetail_Ultra;
  slot.sim.background_voxel_enabled = true;
  slot.sim.background_voxel_style = kSimBackgroundVoxelStyle_Varied;
  slot.sim.projection_pitch_mrad = -575;
  slot.sim.projection_distance_x100 = 300;
  slot.sim.world_navigation.focus_x = kSimWorldMapPixels / 2;
  slot.sim.world_navigation.focus_y = kSimWorldMapPixels / 2;
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
  /* Only atmosphere uses the 2D mesh path; ocean, land and models now share
   * the real depth pass, even when the town object list is empty. */
  assert(backend.draw_geometry_count == 1);
  assert(backend.ground_vertex_count == 129 * 129);
  assert(backend.ground_index_count == 128 * 128 * 6);
  assert(backend.set_viewport_count == 3);
  assert(!backend.viewport_set);
  assert(backend.viewport.x == 160 && backend.viewport.y == 0);
  assert(backend.viewport.w == 960 && backend.viewport.h == 720);
  /* The source point under the Palace remains the projection origin even
   * though the map around it is now oblique and curved. */
  assert(backend.ground_vertices[1].position.x > 479.9f &&
         backend.ground_vertices[1].position.x < 480.1f);
  assert(backend.ground_vertices[1].position.y > 359.9f &&
         backend.ground_vertices[1].position.y < 360.1f);
  /* A perspective globe no longer pins its texture corners to the viewport
   * corners. The back edge contracts more than the near edge. */
  assert(backend.ground_vertices[0].position.x > 0.0f);
  assert(backend.ground_vertices[2].position.x < 960.0f);
  assert(backend.ground_vertices[0].position.y > 0.0f);
  assert(isfinite(backend.ground_vertices[2].position.y));
  assert(fabsf(backend.ground_vertices[2].position.y - 720.0f) > 1.0f);
  assert(backend.ground_vertices[3].position.y <
         backend.ground_vertices[1].position.y);
}

static void TestTownReliefRegistration(void) {
  const float datum_offset[kSimTownCount] = {0, 3, 4, 4, 0, 4};
  for (uint8_t town = 1; town <= kSimTownCount; town++) {
    int origin_x = 0, origin_y = 0;
    assert(SimWorldMap_OriginForTown(town, &origin_x, &origin_y));
    const float local_x = 11.25f, local_y = 17.5f;
    SimWorldNavigationTerrainSample sample;
    assert(SimWorldNavigationTerrain_Sample(
        origin_x + local_x, origin_y + local_y, &sample));
    const float expected = SimTownTerrain_HeightUnitsAt(
        town, local_x * kSimTownCellPixels,
        local_y * kSimTownCellPixels) + datum_offset[town - 1];
    assert(sample.height_units > expected - 0.0001f &&
           sample.height_units < expected + 0.0001f);
    assert(sample.authored_weight == 1.0f);
  }
  assert(!SimWorldNavigationTerrain_Sample(0.0f, 0.0f, NULL));
}

static void AssertPerformanceScopeRestored(void) {
  const Sim3DPerformanceScope scope =
      Sim3DPerformance_Begin(kSim3DPerformance_HostUi);
  assert(scope.previous_stage == -1); /* No failed stage owns later work. */
  Sim3DPerformance_End(scope);
}

static void TestFailureRestoresFullOutput(void) {
  /* First draw fails in either the atmosphere or the optional space stage. */
  for (int backdrop = 0; backdrop <= 1; backdrop++) {
    FakeBackend backend = {
      .output_width = 1280,
      .output_height = 720,
      .fail_geometry_call = 1,
    };
    assert(ArRenderDevice_Init(
        &g_render_device, &kFakeOps, &backend,
        (ArRenderCapabilities){0}));
    FrameSlot slot = WorldNavigationSlot();
    slot.sim.world_navigation_backdrop = backdrop != 0;
    UploadWorldNavigationComposition(&slot);
    assert(PresentWorldNavigation3D(&slot) ==
           kPresentationOutcome_CoreFailure);
    assert(backend.set_viewport_count == 3);
    assert(!backend.viewport_set);
    AssertPerformanceScopeRestored();
  }
}

static void TestAuthoredTownModelsUseSharedCacheAndDepth(void) {
  FakeBackend backend = {.output_width = 1280, .output_height = 720};
  assert(ArRenderDevice_Init(&g_render_device, &kFakeOps, &backend,
                            (ArRenderCapabilities){0}));
  FrameSlot slot = WorldNavigationSlot();
  slot.sim.world_navigation.active_location = 2;
  slot.sim.world_navigation_towns.object_count = 1;
  SimBackgroundVoxelObject *object =
      &slot.sim.world_navigation_towns.objects[0];
  *object = (SimBackgroundVoxelObject){
    .town = 2, .kind = kSimBackgroundVoxel_Factory,
    .cell_x = 15, .cell_y = 15, .record_slot = 7,
    .source_cells_w = 2, .source_cells_h = 2,
    .footprint_cells_w = 2, .footprint_cells_d = 2,
    .visual_state = kSimStructureVisualState_Finished,
  };
  SimBackgroundVoxelModelCache_Reset();
  slot.sim.world_navigation_brightness = 0;
  UploadWorldNavigationComposition(&slot);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_terrain_faces == 128 * 128);
  /* The actual sideways-U factory has substantially more than the former
   * two-box proxy's ten faces and is fetched through the shared town cache. */
  assert(depth_solid_faces > 20);
  assert(SimBackgroundVoxelModelCache_Stats().misses == 1);
  /* Black is the loading opportunity: the first visible fade step must
   * reuse the authored models, not defer their compilation until then. */
  slot.sim.world_navigation_brightness = 1;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(SimBackgroundVoxelModelCache_Stats().hits == 1);
  /* The second held view reuses owned projected faces without consulting the
   * compiler cache again. The first repeat warmed that projection cache. */
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(SimBackgroundVoxelModelCache_Stats().hits == 1);
  /* Even with Ultra enabled globally, this overview-sized object must have
   * fetched the authored Low model. A lookup must hit the exact same entry. */
  assert(SimBackgroundVoxelModelCache_Get(
      object, kSimBackgroundVoxelDetail_Low, kSimBackgroundVoxelStyle_Varied, NULL, NULL));
  assert(SimBackgroundVoxelModelCache_Stats().hits == 2);
  assert(SimBackgroundVoxelModelCache_Stats().misses == 1);
  depth_begin_failure = true;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_CoreFailure);
  assert(!backend.viewport_set);
  AssertPerformanceScopeRestored();
  depth_begin_failure = false;
}

static void TestOceanBatchFailureRecovery(void) {
  FakeBackend backend = {.output_width = 1280, .output_height = 720};
  assert(ArRenderDevice_Init(&g_render_device, &kFakeOps, &backend,
                            (ArRenderCapabilities){0}));
  FrameSlot slot = WorldNavigationSlot();
  UploadWorldNavigationComposition(&slot);
  for (int failed_batch = 1; failed_batch <= 2; failed_batch++) {
    depth_fail_ocean_batch = failed_batch;
    assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_CoreFailure);
    assert(depth_ocean_batches == failed_batch);
    assert(!backend.viewport_set);
    AssertPerformanceScopeRestored();
    depth_fail_ocean_batch = 0;
    assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
    assert(depth_ocean_quads == 9120); /* Complete original closed shell. */
    assert(depth_ocean_batches > 1 && depth_ocean_batches <= 150);
    AssertPerformanceScopeRestored();
  }
}

static void TestSpaceAndCloudCover(void) {
  FakeBackend backend = {.output_width = 1280, .output_height = 720};
  assert(ArRenderDevice_Init(&g_render_device, &kFakeOps, &backend,
                            (ArRenderCapabilities){0}));
  FrameSlot slot = WorldNavigationSlot();
  slot.sim.world_navigation_backdrop = true;
  slot.sim.world_navigation_clouds = true;
  slot.sim.cloud_opacity_pct = 35;
  slot.sim.cloud_altitude_px = 96;
  slot.sim.cloud_drift_pct = 100;
  slot.sim.world_navigation_lighting = true;
  slot.sim.shadow_opacity_pct = 35;
  slot.sim.shadow_softness_pct = 100;
  slot.sim.world_navigation.zoom_current = 0x040A;
  UploadWorldNavigationComposition(&slot);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.space_draws == 1);
  assert(backend.cloud_body_draws > 0 && backend.cloud_body_draws <= 3 * 48);
  /* Only longitude-chart crossings add faces; cap extra geometry at 25%.
   * Batching must still fit the original API-call budget above. */
  assert(depth_cloud_faces > 0 && depth_cloud_faces <= 3 * 48 * 96 * 5 / 4);
  assert(depth_expected_shadow_terrain_faces > 0 && depth_expected_shadow_terrain_faces <= 128 * 128);
  assert(depth_shadow_terrain_faces == 9 * depth_expected_shadow_terrain_faces);
  assert(depth_shadow_ocean_faces > 0);
  assert(backend.cloud_ocean_shadow_draws > 0 && backend.cloud_ocean_shadow_draws <= 9 * 48);
  const int body_draws = backend.cloud_body_draws;
  slot.sim.world_navigation.zoom_current = 0x0206;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.cloud_body_draws == body_draws && depth_cloud_faces == 0); /* Below the cloud deck. */
  assert(depth_shadow_terrain_faces == 9 * depth_expected_shadow_terrain_faces);
  slot.sim.world_navigation_backdrop = false;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.space_draws == 2);
  /* A wide overview can legitimately retain every tile. A closer affine
   * scale must actually reject offscreen work, but retain all nine samples
   * on every overlapping receiver. Repeat through projection/viewport changes
   * to catch stale cached outcodes. */
  slot.sim.world_navigation_scene.source_to_screen[0] = 1;
  slot.sim.world_navigation_scene.source_to_screen[4] = 1;
  for (int view = 0; view < 3; view++) {
    slot.sim.world_navigation.focus_x = (uint16_t)(208 + view * 256);
    slot.sim.world_navigation.focus_y = (uint16_t)(512 - view * 128);
    slot.sim.projection_pitch_mrad = -575 - view * 150;
    backend.output_width = 640 + view * 192;
    backend.output_height = 480 + view * 64;
    assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
    assert(depth_expected_shadow_terrain_faces > 0 && depth_expected_shadow_terrain_faces < 128 * 128);
    assert(depth_shadow_terrain_faces == 9 * depth_expected_shadow_terrain_faces);
  }
  slot.sim.world_navigation.zoom_current = 0x040A;
  slot.sim.cloud_opacity_pct = 100;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_cloud_faces > 0); /* Chart interpolation must keep alpha in [0,1]. */
}

static void TestAdventAuthoredModelClearance(void) {
  /* At the last top-down zoom the hidden far hemisphere extends past the
   * far plane. The real GPU clips it; the near-side models must all survive. */
  FakeBackend backend = {.output_width = 800, .output_height = 600,
      .allow_far_clipped_depth = true};
  assert(ArRenderDevice_Init(&g_render_device, &kFakeOps, &backend,
                            (ArRenderCapabilities){0}));
  FrameSlot slot = WorldNavigationSlot();
  slot.sim.world_navigation_relief = false;
  slot.sim.world_navigation_cloud_shadows = false;
  slot.sim.cloud_altitude_px = 0;
  slot.sim.projection_pitch_mrad = 0;
  slot.sim.world_navigation_towns.object_count = 1;
  SimBackgroundVoxelObject *object = &slot.sim.world_navigation_towns.objects[0];
  *object = (SimBackgroundVoxelObject){
    .town = 2, .kind = kSimBackgroundVoxel_BloodpoolCastle,
    .cell_x = 15, .cell_y = 15,
    .source_cells_w = 2, .source_cells_h = 2,
    .footprint_cells_w = 2, .footprint_cells_d = 2,
    .visual_state = kSimStructureVisualState_Finished,
  };
  const int heights[] = {100, 400, 200, 300, 100};
  const int zooms[] = {50, 30, 10, 30, 50};
  for (int detail = kSimBackgroundVoxelDetail_Low;
       detail < kSimBackgroundVoxelDetail_Count; detail++) {
    slot.sim.background_voxel_detail = detail;
    for (int style = kSimBackgroundVoxelStyle_Basic;
         style < kSimBackgroundVoxelStyle_Count; style++) {
      slot.sim.background_voxel_style = style;
      SimBackgroundVoxelModel model;
      SimBackgroundVoxelModel_BuildStyled(object, (SimBackgroundVoxelDetail)detail,
          (SimBackgroundVoxelStyle)style, &model);
      for (size_t h = 0; h < sizeof(heights) / sizeof(heights[0]); h++) {
        slot.sim.height_scale_x100 = heights[h];
        for (size_t z = 0; z < sizeof(zooms) / sizeof(zooms[0]); z++) {
          /* Native straight-down affine, still drawing at the last zoom to
           * detect disappearing geometry even though that frame is black. */
          const float scale = 512.0f / zooms[z];
          slot.sim.world_navigation_scene.source_to_screen[0] = scale;
          slot.sim.world_navigation_scene.source_to_screen[4] = scale;
          slot.sim.world_navigation_scene.source_to_screen[2] = 128 - 512 * scale;
          slot.sim.world_navigation_scene.source_to_screen[5] = 112 - 512 * scale;
          slot.sim.world_navigation_brightness = (zooms[z] - 10) / 4;
          UploadWorldNavigationComposition(&slot);
          assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
          /* Complete alone misses this regression: faces crossing the near
           * plane were silently dropped at 400% height before black. */
          assert(depth_solid_faces == model.face_count);
          assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
          assert(depth_solid_faces == model.face_count);
        }
      }
    }
  }
  /* Same count/settings but a taller captured object must invalidate the
   * cached bound, independently of the style/detail changes above. */
  slot.sim.height_scale_x100 = 400;
  slot.sim.world_navigation_scene.source_to_screen[0] = 51.2f;
  slot.sim.world_navigation_scene.source_to_screen[4] = 51.2f;
  slot.sim.world_navigation_scene.source_to_screen[2] = 128 - 512 * 51.2f;
  slot.sim.world_navigation_scene.source_to_screen[5] = 112 - 512 * 51.2f;
  object->kind = kSimBackgroundVoxel_House;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  object->kind = kSimBackgroundVoxel_BloodpoolCastle;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  SimBackgroundVoxelModel restored;
  SimBackgroundVoxelModel_BuildStyled(object, kSimBackgroundVoxelDetail_Ultra,
      kSimBackgroundVoxelStyle_Varied, &restored);
  assert(depth_solid_faces == restored.face_count);
  slot.sim.world_navigation_models = false;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_solid_faces == 0);
  slot.sim.world_navigation_models = true;
  slot.sim.world_navigation_towns.object_count = 0;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_solid_faces == 0);
  slot.sim.world_navigation_towns.object_count = 1;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_solid_faces > 0);
  PresentWorldNav_ResetResources();
  UploadWorldNavigationComposition(&slot);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_solid_faces > 0);
  PresentWorldNav_ResetResources();
}

static void TestGroundCacheInvalidation(void) {
  FakeBackend backend = {.output_width = 1280, .output_height = 720};
  assert(ArRenderDevice_Init(&g_render_device, &kFakeOps, &backend,
                            (ArRenderCapabilities){0}));
  FrameSlot slot = WorldNavigationSlot();
  UploadWorldNavigationComposition(&slot);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  const ArRenderVertex2D first = backend.ground_vertices[1];
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(memcmp(&first, &backend.ground_vertices[1], sizeof(first)) == 0);
  slot.sim.world_navigation.focus_x += 64;
  slot.sim.world_navigation_scene.source_to_screen[2] -= 16.0f;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(fabsf(first.position.x - backend.ground_vertices[1].position.x) > 1.0f);
  const float unlit = backend.ground_vertices[1].color.r;
  slot.sim.world_navigation_lighting = true;
  slot.sim.light_elevation_deg = 35;
  slot.sim.light_azimuth_deg = 315;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_vertices[1].color.r < unlit);
  const float old_y = backend.ground_vertices[1].position.y;
  slot.sim.projection_pitch_mrad = -300;
  slot.sim.projection_yaw_mrad = 600;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_vertices[1].position.y == old_y); /* Town pose never tilts navigation. */
}

static void TestMapEdgeLandOpacity(void) {
  FakeBackend backend = {.output_width = 800, .output_height = 600,
                         .check_map_edge_opacity = true};
  assert(ArRenderDevice_Init(&g_render_device, &kFakeOps, &backend,
                            (ArRenderCapabilities){0}));
  uint8_t *rom = calloc(1, 0x100000);
  assert(rom);
  memset(rom + 0x70000, 0x10, 64);
  memset(rom + 0x53000, 0x10, 4 * 64);
  memset(rom + 0x70000 + 64, 0x10, 64);
  rom[0x70000 + 127] = 0x20; /* One land texel in an otherwise blue shore. */
  const int cells[][2] = {{80, 124}, {50, 0}, {0, 64}, {127, 64}, {64, 127}};
  for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++)
    rom[0x33341 + cells[i][1] * 128 + cells[i][0]] = 1;
  assert(SimWorldMap_Init(rom, 0x100000));
  PresentWorldNav_ResetResources();
  FrameSlot slot = WorldNavigationSlot();
  slot.sim.world_navigation_cloud_shadows = false;
  UploadWorldNavigationComposition(&slot);
  for (int relief = 0; relief <= 1; relief++) {
    slot.sim.world_navigation_relief = relief != 0;
    for (int repeat = 0; repeat < 3; repeat++) {
      assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
      assert(backend.opaque_edge_land == 5 && backend.faded_edge_water > 0);
    }
  }
  uint8_t map[kSimWorldMapBytes];
  memcpy(map, SimWorldMap_Baseline(), sizeof(map));
  for (int relief = 0; relief <= 1; relief++) {
    slot.sim.world_navigation_relief = relief != 0;
    assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
    map[124 * 128 + 80] = 0;
    assert(SimWorldMap_PublishBuiltTilemap(map) == 1);
    slot.sim.underlay_serial = SimWorldMap_Serial();
    assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
    assert(backend.opaque_edge_land == 4 && backend.faded_edge_water > 0);
    map[124 * 128 + 80] = 1;
    SimWorldMap_PublishBuiltTilemap(map);
    SimWorldMap_SetWaterAnimationSource(0xB0C0);
    slot.sim.underlay_serial = SimWorldMap_Serial();
    assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
    assert(backend.opaque_edge_land == 5);
  }
  PresentWorldNav_ResetResources();
  UploadWorldNavigationComposition(&slot);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.opaque_edge_land == 5);
  free(rom);
}

static void TestTallModelViewportClearance(void) {
  FakeBackend backend = {.output_width = 800, .output_height = 600};
  assert(ArRenderDevice_Init(&g_render_device, &kFakeOps, &backend,
                            (ArRenderCapabilities){0}));
  FrameSlot slot = WorldNavigationSlot();
  slot.sim.world_navigation_relief = false;
  slot.sim.world_navigation_cloud_shadows = false;
  slot.sim.height_scale_x100 = 400;
  /* Radial navigation no longer has an oblique offscreen-anchor/visible-roof
   * case. Keep that shared culling regression in the horizon camera. */
  slot.sim.view = kSimView_SkyPalace;
  slot.sim.world_navigation.focus_y = 136;
  slot.sim.world_navigation_towns.object_count = 1;
  SimBackgroundVoxelObject *object = &slot.sim.world_navigation_towns.objects[0];
  *object = (SimBackgroundVoxelObject){
    .town = 2, .kind = kSimBackgroundVoxel_BloodpoolCastle,
    .cell_x = 15, .cell_y = 15,
    .source_cells_w = 2, .source_cells_h = 2,
    .footprint_cells_w = 2, .footprint_cells_d = 2,
    .visual_state = kSimStructureVisualState_Finished,
  };
  const ArRenderRectI viewport = {0, 0, 800, 600};
  /* Its raised roof enters the viewport although the short castle remains
   * outside. Replayed and rebuilt caches must retain those clipped faces. */
  for (int repeat = 0; repeat < 3; repeat++) {
    assert(PresentWorldNavigationBackdrop(&slot, viewport) == kPresentationOutcome_Complete);
    assert(depth_solid_faces > 0);
  }
  slot.sim.height_scale_x100 = 100;
  assert(PresentWorldNavigationBackdrop(&slot, viewport) == kPresentationOutcome_Complete);
  assert(depth_solid_faces == 0); /* Truly offscreen at native model height. */
  slot.sim.height_scale_x100 = 400;
  assert(PresentWorldNavigationBackdrop(&slot, viewport) == kPresentationOutcome_Complete);
  assert(depth_solid_faces > 0);
  PresentWorldNav_ResetResources();
  assert(PresentWorldNavigationBackdrop(&slot, viewport) == kPresentationOutcome_Complete);
  assert(depth_solid_faces > 0);
}

static void TestOptionalStagesSkipWorkAndRestore(void) {
  FakeBackend backend = {.output_width = 1280, .output_height = 720};
  assert(ArRenderDevice_Init(&g_render_device, &kFakeOps, &backend,
                            (ArRenderCapabilities){0}));
  FrameSlot slot = WorldNavigationSlot();
  slot.sim.world_navigation_towns.object_count = 1;
  slot.sim.world_navigation_towns.objects[0] = (SimBackgroundVoxelObject){
    .town = 2, .kind = kSimBackgroundVoxel_Factory,
    .cell_x = 15, .cell_y = 15, .record_slot = 7,
    .source_cells_w = 2, .source_cells_h = 2,
    .footprint_cells_w = 2, .footprint_cells_d = 2,
    .visual_state = kSimStructureVisualState_Finished,
  };
  slot.sim.world_navigation.active_location = 2;
  slot.sim.world_navigation_atmosphere = false;
  slot.sim.world_navigation_models = false;
  slot.sim.world_navigation_relief = false;
  slot.sim.world_navigation_cloud_shadows = false;
  SimBackgroundVoxelModelCache_Reset();
  UploadWorldNavigationComposition(&slot);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_solid_faces == 0 && depth_terrain_faces == 128 * 128);
  assert(SimBackgroundVoxelModelCache_Stats().misses == 0);
  assert(backend.draw_geometry_count == 0);
  assert(backend.cloud_shadow_draws == 0 && backend.cloud_body_draws == 0);
  ArRenderVertex2D flat[4];
  memcpy(flat, backend.ground_vertices, sizeof(flat));
  slot.sim.world_navigation_relief = true;
  slot.sim.landscape_height_pct = 0;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(memcmp(flat, backend.ground_vertices, sizeof(flat)) == 0);
  slot.sim.landscape_height_pct = 100;
  slot.sim.world_navigation_models = true;
  slot.sim.world_navigation_atmosphere = true;
  slot.sim.world_navigation_clouds = true;
  slot.sim.world_navigation_lighting = true;
  slot.sim.light_elevation_deg = 90;
  slot.sim.world_navigation.zoom_current = 0x040A;
  slot.sim.cloud_opacity_pct = 35;
  slot.sim.cloud_altitude_px = 96;
  slot.sim.shadow_opacity_pct = 35;
  slot.sim.shadow_softness_pct = 50;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_solid_faces > 20);
  assert(SimBackgroundVoxelModelCache_Stats().misses == 1);
  assert(backend.draw_geometry_count == 1);
  assert(backend.cloud_body_draws > 0 && backend.cloud_body_draws <= 3 * 48);
  assert(backend.cloud_shadow_draws == 0);
  slot.sim.world_navigation_cloud_shadows = true;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_shadow_terrain_faces == 9 * depth_expected_shadow_terrain_faces);
  const int shadow_draws = backend.cloud_shadow_draws, body_draws = backend.cloud_body_draws;
  slot.sim.world_navigation_clouds = false;
  slot.sim.world_navigation_models = false;
  slot.sim.world_navigation_atmosphere = false;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_solid_faces == 0);
  assert(backend.draw_geometry_count == 2);
  assert(backend.cloud_shadow_draws == shadow_draws && depth_shadow_terrain_faces == 0);
  assert(backend.cloud_body_draws == body_draws && depth_cloud_faces == 0);
}

static void TestDetailedGroundLiveInvalidation(void) {
  FakeBackend backend = {.output_width = 1280, .output_height = 720};
  assert(ArRenderDevice_Init(&g_render_device, &kFakeOps, &backend,
                            (ArRenderCapabilities){0}));
  PresentWorldNav_ResetResources();
  uint8_t *rom = calloc(0x100000, 1);
  assert(rom && SimTownGroundArt_Init(rom, 0x100000));
  free(rom);
  FrameSlot slot = WorldNavigationSlot();
  SimWorldNavigationTownGround *ground = &slot.sim.world_navigation_towns.ground;
  ground->enabled_town_mask = 1;
  ground->development_tier[0] = 1;
  memset(ground->terrain[0], 8, sizeof(ground->terrain[0]));
  UploadWorldNavigationComposition(&slot);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_uploads == 1);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_uploads == 1);
  slot.sim.world_navigation_ground_detail = true;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_uploads == 2);
  ground->terrain[0][20 * 32 + 20] = 0x25;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_uploads == 3); /* changed native cell, unchanged Mode7 serial */
  slot.sim.world_navigation.focus_x += 10;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_uploads == 3); /* camera changes do not rebuild artwork */
  ground->development_tier[0] = 2;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_uploads == 4);
  slot.sim.world_navigation_models = false;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_uploads == 5);
  slot.sim.world_navigation_ground_detail = false;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_uploads == 6);
  ground->terrain[0][20 * 32 + 20] = 8;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_uploads == 6); /* disabled stage ignores native revisions */
  slot.sim.world_navigation_ground_detail = true;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_uploads == 7);
  /* All authored cliffs participate in the same opaque/depth, haze and
   * cloud-shadow surface. CheckDepthSurface verifies exact XYZ reuse. */
  ground->enabled_town_mask = 63;
  slot.sim.world_navigation_clouds = true;
  slot.sim.world_navigation_cloud_shadows = true;
  slot.sim.cloud_opacity_pct = 35;
  slot.sim.shadow_opacity_pct = 35;
  slot.sim.shadow_softness_pct = 50;
  slot.sim.cloud_altitude_px = 96;
  slot.sim.cloud_drift_pct = 0;
  slot.sim.world_navigation_haze = true;
  slot.sim.underlay_haze_pct = 25;
  slot.sim.underlay_defocus_pct = 20;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  const int cliff_faces = depth_terrain_faces;
  assert(cliff_faces > 128 * 128);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_terrain_faces == cliff_faces);
  slot.sim.world_navigation_ground_detail = false;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_terrain_faces == 128 * 128);
  slot.sim.world_navigation_ground_detail = true;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_terrain_faces == cliff_faces);
  slot.sim.landscape_height_pct = 0;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_terrain_faces == 128 * 128);
  slot.sim.landscape_height_pct = 100;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_terrain_faces == cliff_faces);
  PresentWorldNav_ResetResources();
  SimTownGroundArt_Shutdown();
}

static void TestNativeMountainRestoration(void) {
  static const uint32_t rows[32] = {
    0xFFFFFFFFu, 0xFFFC3C3Fu, 0xFFF0000Fu, 0xFFC00003u, 0xF0000000u,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0x00000600u, 0x00000F00u, 0x00006F00u, 0x0000FF80u, 0x0000FFE0u, 0x0000FFF8u,
  };
  FakeBackend backend = {.output_width = 1280, .output_height = 720};
  backend.ground_upload_mirror = calloc(2048u * 2048, sizeof(uint32_t));
  assert(backend.ground_upload_mirror);
  assert(ArRenderDevice_Init(&g_render_device, &kFakeOps, &backend, (ArRenderCapabilities){0}));
  PresentWorldNav_ResetResources();
  uint8_t *rom = calloc(0x100000, 1);
  assert(rom);
  rom[0x1098D] = 0x24;
  rom[0x1098E] = 8;
  /* One animated water metatile, with distinguishable opaque/transparent
   * phases. The rest of the synthetic native material remains static. */
  rom[0xE3B95] = 31;
  for (int q = 0; q < 4; q++) {
    rom[0xC881B + 0x25 * 8 + q * 2] = 1;
    /* Static native ground must differ from the underlying world image so
     * a stale retained CPU atlas cannot hide behind matching black pixels. */
    rom[0xC881B + 8 * 8 + q * 2] = 32;
  }
  for (int bank = 0; bank < 2; bank++)
    for (int y = 0; y < 8; y++) {
      rom[0x60000 + bank * 0x4000 + 32 + y * 2] = 255;
      rom[0x60000 + bank * 0x4000 + 0x200 + 32 + y * 2] = 255;
      rom[0x60000 + bank * 0x4000 + 32 * 32 + y * 2] = 255;
    }
  assert(SimTownGroundArt_Init(rom, 0x100000));
  free(rom);
  FrameSlot slot = WorldNavigationSlot();
  slot.sim.world_navigation_mountains = true;
  SimWorldNavigationTownGround *ground = &slot.sim.world_navigation_towns.ground;
  ground->enabled_town_mask = 2;
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++)
      ground->terrain[1][y * 32 + x] = rows[y] & (1u << x) ? 0x89 : 8;
  ground->terrain[1][12 * 32 + 12] = 0x25;
  slot.sim.world_navigation_brightness = 0;
  UploadWorldNavigationComposition(&slot);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_world_mountain_faces > 200 && backend.mountain_uploads == 1);
  const uint64_t stationary_hash = depth_world_mountain_hash;
  slot.sim.world_navigation_brightness = 15;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_world_mountain_hash == stationary_hash);
  assert(backend.mountain_uploads == 1 && backend.ground_uploads == 1);
  slot.sim.world_navigation.focus_x += 10;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_world_mountain_hash != stationary_hash);
  const uint64_t moved_hash = depth_world_mountain_hash;
  slot.sim.world_navigation_lighting = !slot.sim.world_navigation_lighting;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_world_mountain_hash != moved_hash);
  slot.sim.world_navigation_lighting = !slot.sim.world_navigation_lighting;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_world_mountain_hash == moved_hash);
  slot.sim.landscape_height_pct = 50;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_world_mountain_hash != moved_hash);
  slot.sim.landscape_height_pct = 100;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_world_mountain_hash == moved_hash);
  backend.output_width = 960; backend.output_height = 540;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_world_mountain_hash != moved_hash);
  backend.output_width = 1280; backend.output_height = 720;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_world_mountain_hash == moved_hash);
  assert(backend.mountain_uploads == 1 && backend.ground_uploads == 1);
  slot.sim.world_navigation_mountains = false;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_world_mountain_faces == 0 && backend.ground_uploads == 2);
  slot.sim.world_navigation_mountains = true;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_world_mountain_hash == moved_hash);
  assert(depth_world_mountain_faces > 200 && backend.mountain_uploads == 2);
  ground->development_tier[1] = 2;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.mountain_uploads == 3);
  slot.sim.world_navigation_relief = false;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_world_mountain_faces == 0);
  backend.fail_mountain_upload = true;
  slot.sim.world_navigation_relief = true;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_world_mountain_faces == 0 && depth_terrain_faces > 0);
  const int failed_uploads = backend.mountain_uploads;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.mountain_uploads == failed_uploads); /* no per-frame retry churn */
  PresentWorldNav_ResetResources();
  backend.fail_mountain_upload = false;
  UploadWorldNavigationComposition(&slot);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_world_mountain_faces > 200);
  int native_uploads = backend.mountain_uploads;
  const int ground_uploads = backend.ground_uploads;
  uint8_t map[kSimWorldMapBytes];
  memcpy(map, SimWorldMap_Baseline(), sizeof(map));
  map[0] = 1;
  assert(SimWorldMap_PublishBuiltTilemap(map) == 1);
  /* Exterior continuations depend on surrounding rock. A geography edit
   * invalidates their retained scene even with an unchanged image serial. */
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.mountain_uploads == native_uploads + 1 && backend.ground_uploads == ground_uploads + 1);
  native_uploads = backend.mountain_uploads;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_uploads == ground_uploads + 1);
  slot.sim.world_navigation_ground_detail = true;
  slot.sim.underlay_serial = SimWorldMap_Serial();
  slot.sim.game_frame = 1;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  const int animated_uploads = backend.ground_uploads;
  const uint64_t animated_bytes = backend.ground_upload_bytes;
  const int terrain_faces = depth_terrain_faces;
  const int mountain_faces = depth_world_mountain_faces;
  slot.sim.game_frame = 8;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_uploads == animated_uploads);
  for (int phase = 1; phase <= 4; phase++) {
    slot.sim.game_frame = (uint16_t)(phase * 8 + 1);
    assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
    assert(backend.ground_uploads == animated_uploads + phase);
    assert(backend.ground_upload_bytes == animated_bytes + (uint64_t)phase * 16 * 16 * 4);
    assert(backend.mountain_uploads == native_uploads);
    assert(depth_terrain_faces == terrain_faces && depth_world_mountain_faces == mountain_faces);
    assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
    assert(backend.ground_uploads == animated_uploads + phase); /* frozen clock */
  }
  /* Rebuilding a different style mutates the retained CPU atlas. If its full
   * upload fails, returning to the last GPU style must repair that CPU image
   * before another animation patch. The upload mirror checks every pixel,
   * not only the animated region. */
  slot.sim.world_navigation_ground_detail = false;
  backend.fail_ground_upload = true;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_CoreFailure);
  backend.fail_ground_upload = false;
  const uint64_t full_failure_bytes = backend.ground_upload_bytes;
  slot.sim.world_navigation_ground_detail = true;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_upload_bytes ==
         full_failure_bytes + UINT64_C(2048) * 2048 * 4);
  const uint64_t repaired_bytes = backend.ground_upload_bytes;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_upload_bytes == repaired_bytes); /* frozen after repair */
  slot.sim.game_frame = 41;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_upload_bytes == repaired_bytes + 16 * 16 * 4);
  slot.sim.game_frame = 33;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_upload_bytes == repaired_bytes + 2 * 16 * 16 * 4);
  /* A failed incremental upload must force a full retry, including a clock
   * rewind to the last successfully published phase. */
  slot.sim.game_frame = 41;
  backend.fail_ground_upload = true;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_CoreFailure);
  backend.fail_ground_upload = false;
  const uint64_t failed_bytes = backend.ground_upload_bytes;
  slot.sim.game_frame = 33;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_upload_bytes == failed_bytes + UINT64_C(2048) * 2048 * 4);
  slot.sim.world_navigation_ground_detail = false;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  const int disabled_uploads = backend.ground_uploads;
  slot.sim.game_frame = 41;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_uploads == disabled_uploads);
  /* More than 256 tiny animation runs exercise bounded coarse upload
   * grouping. The retained-image oracle above still requires every pixel,
   * including intervening static ground, to equal the complete CPU atlas. */
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++)
      if (!(rows[y] & (1u << x))) ground->terrain[1][y * 32 + x] = x & 1 ? 0x25 : 8;
  slot.sim.world_navigation_ground_detail = true;
  slot.sim.game_frame = 1;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  const uint64_t coarse_bytes = backend.ground_upload_bytes;
  slot.sim.game_frame = 9;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.ground_upload_bytes > coarse_bytes);
  assert(backend.ground_upload_bytes < coarse_bytes + UINT64_C(2048) * 2048 * 4);
  map[0] = SimWorldMap_Baseline()[0];
  assert(SimWorldMap_PublishBuiltTilemap(map) == 1);
  uint8_t *world_rom = calloc(0x100000, 1);
  assert(world_rom && SimWorldMap_Init(world_rom, 0x100000));
  free(world_rom);
  assert(!SimWorldMap_DevelopedAvailable());
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  const int baseline_uploads = backend.mountain_uploads;
  const uint32_t baseline_geography = SimWorldMap_GeographySerial();
  /* Publishing an identical map changes authority, not pixels/serial. The
   * optional exterior geometry must get a new chance to build exactly once. */
  assert(SimWorldMap_PublishBuiltTilemap(SimWorldMap_Baseline()) == 0);
  assert(SimWorldMap_GeographySerial() == baseline_geography);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.mountain_uploads == baseline_uploads + 1);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.mountain_uploads == baseline_uploads + 1);
  PresentWorldNav_ResetResources();
  free(backend.ground_upload_mirror);
  SimTownGroundArt_Shutdown();
}

static void TestLavaUploadRecovery(void) {
  /* Same audited occupancy as the material test, with entirely synthetic
   * palette/CHR data. The overhead crater uses a single atlas rectangle. */
  static const uint32_t rows[32] = {
    0xFFFCFFFFu, 0xFFFCFFFFu, 0xFCFCFFFFu, 0xDE3C3C3Fu,
    0xFF003C0Fu, 0xFF003C03u, 0xFF000000u, 0xFF000000u,
    0xC0000300u, 0xC0000780u, 0x00000FC1u, 0x00000FC3u,
    0x80000FC3u, 0xE0006FC3u, 0xF000F003u, 0xF000F003u,
    0xF000F001u, 0xF0000003u, 0xF0000003u, 0xC0000003u,
    0x18000000u, 0x3C000006u, 0x3C00000Fu, 0x3C00000Fu,
    0x1800000Fu, 0x3E000007u, 0x3F00000Fu, 0x3F00000Fu,
    0x3F98001Fu, 0x3FFE667Fu, 0x0FFFFFFFu, 0x0FFFFFFFu,
  };
  FakeBackend backend = {.output_width = 800, .output_height = 600};
  backend.mountain_upload_mirror = calloc(512u * 512, sizeof(uint32_t));
  assert(backend.mountain_upload_mirror);
  assert(ArRenderDevice_Init(&g_render_device, &kFakeOps, &backend, (ArRenderCapabilities){0}));
  PresentWorldNav_ResetResources();
  uint8_t *rom = calloc(0x100000, 1);
  assert(rom);
  rom[0xE3B93 + 0x21 * 2] = 31;
  for (int tile = 0x70; tile <= 0x71; tile++)
    for (int q = 0; q < 4; q++) {
      rom[0xC881A + tile * 8 + q * 2] = 8;
      rom[0xC881B + tile * 8 + q * 2] = 1;
    }
  for (int bank = 0; bank < 2; bank++)
    for (int y = 0; y < 8; y++) rom[0x60000 + bank * 0x4000 + 32 + y * 2] = 255;
  assert(SimTownGroundArt_Init(rom, 0x100000));
  free(rom);
  FrameSlot slot = WorldNavigationSlot();
  slot.sim.world_navigation_mountains = true;
  slot.sim.game_frame = 1;
  SimWorldNavigationTownGround *ground = &slot.sim.world_navigation_towns.ground;
  ground->enabled_town_mask = 8;
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) ground->terrain[3][y * 32 + x] = rows[y] & (1u << x) ? 0x89 : 8;
  UploadWorldNavigationComposition(&slot);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.mountain_uploads == 1 && backend.mountain_upload_bytes == 512u * 512 * 4);
  assert(depth_world_mountain_faces > 200);
  const uint64_t geometry = depth_world_mountain_hash;
  const int ground_uploads = backend.ground_uploads;
  slot.sim.game_frame = 8;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.mountain_uploads == 2 && backend.mountain_upload_bytes == 512u * 512 * 4 + 1024);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.mountain_uploads == 2); /* Held clock performs no transfer. */
  backend.fail_mountain_upload = true;
  slot.sim.game_frame = 16;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.mountain_uploads == 3 && depth_world_mountain_hash == geometry);
  slot.sim.game_frame = 1; /* Rewind while the atlas is dirty. */
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.mountain_uploads == 4);
  backend.fail_mountain_upload = false;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.mountain_uploads == 5 && backend.mountain_upload_bytes == 512u * 512 * 4 + 2048);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.mountain_uploads == 5 && backend.ground_uploads == ground_uploads);
  assert(depth_world_mountain_hash == geometry);
  slot.sim.world_navigation_mountains = false;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  slot.sim.game_frame = 24;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.mountain_uploads == 5);
  slot.sim.world_navigation_mountains = true;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.mountain_uploads == 6 && depth_world_mountain_hash == geometry);
  PresentWorldNav_ResetResources();
  SimTownGroundArt_Shutdown();
  free(backend.mountain_upload_mirror);
}

static void TestAtmosphereEnclosesRaisedTerrain(void) {
  FakeBackend backend = {
    .output_width = 1280, .output_height = 720,
    .check_atmosphere_enclosure = true,
  };
  assert(ArRenderDevice_Init(&g_render_device, &kFakeOps, &backend,
                            (ArRenderCapabilities){0}));
  FrameSlot slot = WorldNavigationSlot();
  const int heights[] = {0, 50, 100, 150};
  const int focus[][2] = {
    {768, 512}, {590, 512}, {208, 512},
    {208, 414}, {466, 192}, {710, 792},
  };
  for (unsigned h = 0; h < sizeof(heights) / sizeof(heights[0]); h++) {
    slot.sim.landscape_height_pct = heights[h];
    for (unsigned f = 0; f < sizeof(focus) / sizeof(focus[0]); f++) {
      slot.sim.world_navigation.focus_x = focus[f][0];
      slot.sim.world_navigation.focus_y = focus[f][1];
      slot.sim.world_navigation_scene.source_to_screen[2] =
          kActRaiserAuthenticWidth * 0.5f -
          focus[f][0] * slot.sim.world_navigation_scene.source_to_screen[0];
      slot.sim.world_navigation_scene.source_to_screen[5] =
          kActRaiserAuthenticHeight * 0.5f -
          focus[f][1] * slot.sim.world_navigation_scene.source_to_screen[4];
      backend.atmosphere_seen = false;
      UploadWorldNavigationComposition(&slot);
      assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
    }
  }
}

static void TestGlobeInspection(void) {
  FakeBackend backend = {.output_width = 1280, .output_height = 720, .track_markers = true};
  assert(ArRenderDevice_Init(&g_render_device, &kFakeOps, &backend, (ArRenderCapabilities){0}));
  PresentWorldNav_ResetResources();
  FrameSlot slot = WorldNavigationSlot();
  SimWorldNavigationComposition *composition = &slot.sim.world_navigation_scene.composition;
  composition->empty_animation = false;
  composition->palace = (SimWorldNavigationCompositionLayer){
      .visible = true, .screen_x = 120, .screen_y = 104, .width = 16, .height = 16};
  composition->plaque = (SimWorldNavigationCompositionLayer){
      .visible = true, .screen_x = 8, .screen_y = 8, .width = 32, .height = 8};
  const SimWorldNavigationFrame navigation = slot.sim.world_navigation;
  UploadWorldNavigationComposition(&slot);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  const ArRenderRectF palace = backend.palace_rect, ui = backend.ui_rect;
  assert(fabsf(palace.w - 16 * 960.0f / 256 * .75f) < .0001f);
  assert(fabsf(palace.x + palace.w * .5f - 480) < .0001f);
  assert(fabsf(palace.y + palace.h * .5f - 360) < .0001f);
  const SimWorldNavigationComposition original_composition = *composition;
  ArRenderVertex2D rest[4];
  memcpy(rest, backend.ground_vertices, sizeof(rest));
  assert(backend.palace_draws == 1 && backend.ui_draws == 1);
  slot.sim_manual_orbit_yaw = .6f;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.palace_draws == 2 && backend.ui_draws == 2);
  assert(backend.palace_rect.x != palace.x);
  assert(!memcmp(&ui, &backend.ui_rect, sizeof(ui)));
  assert(memcmp(rest, backend.ground_vertices, sizeof(rest)));
  slot.sim_manual_orbit_yaw = 3.14159265359f;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.palace_draws == 2 && backend.ui_draws == 3); /* Behind planet. */
  slot.sim_manual_orbit_pitch = 1.57079632679f;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  slot.sim_manual_orbit_pitch = -1.57079632679f;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  slot.sim_manual_orbit_yaw = slot.sim_manual_orbit_pitch = 0;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(!memcmp(rest, backend.ground_vertices, sizeof(rest)));
  assert(!memcmp(&palace, &backend.palace_rect, sizeof(palace)));
  assert(!memcmp(&slot.sim.world_navigation, &navigation, sizeof(navigation)));
  assert(backend.ground_uploads == 1); /* Camera never rebuilds native artwork. */
  const struct { uint16_t distance; float scale; } zooms[] = {
    {100, 1}, {200, 1}, {225, 1}, {300, .75f}, {500, .45f},
    {900, .35f}, {2000, .35f}, {300, .75f},
  };
  for (size_t i = 0; i < sizeof(zooms) / sizeof(zooms[0]); i++) {
    slot.sim.projection_distance_x100 = zooms[i].distance;
    assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
    const ArRenderRectF actual = backend.palace_rect;
    assert(fabsf(actual.w / palace.w - zooms[i].scale / .75f) < .00001f);
    assert(fabsf(actual.h / palace.h - zooms[i].scale / .75f) < .00001f);
    assert(fabsf(actual.x + actual.w * .5f - 480) < .0001f);
    assert(fabsf(actual.y + actual.h * .5f - 360) < .0001f);
    assert(!memcmp(&ui, &backend.ui_rect, sizeof(ui)));
    assert(!memcmp(composition, &original_composition, sizeof(*composition)));
    assert(backend.ground_uploads == 1);
  }
  slot.sim.world_navigation.focus_x = slot.sim.world_navigation.focus_y = 0;
  slot.sim.world_navigation_towns.object_count = 1;
  slot.sim.world_navigation_towns.objects[0] = (SimBackgroundVoxelObject){
    .town = 2, .kind = kSimBackgroundVoxel_Factory, .cell_x = 15, .cell_y = 15,
    .source_cells_w = 2, .source_cells_h = 2, .footprint_cells_w = 2, .footprint_cells_d = 2,
    .visual_state = kSimStructureVisualState_Finished,
  };
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  const int visible_faces = depth_solid_faces;
  assert(visible_faces > 0); /* Visible despite the former native-focus cutoff. */
  for (int sign = -1; sign <= 1; sign++) {
    slot.sim_manual_orbit_yaw = sign * .00001f;
    for (int town = 0; town < 6; town++) {
      slot.sim.world_navigation.active_location = town;
      assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
      assert(depth_solid_faces == visible_faces); /* No orbit/label selection pop. */
    }
  }
  slot.sim_manual_orbit_yaw = .1f;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_solid_faces > 0); /* Inspection selects by projected view/LOD. */
  int town_x, town_y;
  assert(SimWorldMap_OriginForTown(2, &town_x, &town_y));
  slot.sim.world_navigation.focus_x = (town_x + 16) * kSimWorldMapTilePixels;
  slot.sim.world_navigation.focus_y = (town_y + 16) * kSimWorldMapTilePixels;
  slot.sim_manual_orbit_yaw = 3.14159265359f;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_solid_faces == 0); /* Hidden models never reach the depth batch. */
  slot.sim_manual_orbit_yaw = 0;
  slot.sim.world_navigation.focus_x = slot.sim.world_navigation.focus_y = 0;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(depth_solid_faces == visible_faces);
  PresentWorldNav_ResetResources();
}

static void TestLocalizedNavigationLabelHandoff(void) {
  FakeBackend backend = {
      .output_width = 1280, .output_height = 720, .track_markers = true};
  assert(ArRenderDevice_Init(
      &g_render_device, &kFakeOps, &backend,
      (ArRenderCapabilities){0}));
  PresentWorldNav_ResetResources();
  FrameSlot slot = WorldNavigationSlot();
  SimWorldNavigationComposition *composition =
      &slot.sim.world_navigation_scene.composition;
  composition->empty_animation = false;
  composition->palace = (SimWorldNavigationCompositionLayer){
      .visible = true, .screen_x = 120, .screen_y = 104,
      .width = 16, .height = 16};
  composition->plaque = (SimWorldNavigationCompositionLayer){
      .visible = true, .screen_x = 8, .screen_y = 8,
      .width = 32, .height = 8};
  composition->label = (SimWorldNavigationCompositionLayer){
      .visible = true, .screen_x = 156, .screen_y = 25,
      .width = 24, .height = 8};
  UploadWorldNavigationComposition(&slot);

  s_localized_record_available = false;
  s_localized_prepare_success = false;
  s_localized_draws = 0;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.label_draws == 1 && s_localized_draws == 0);

  s_localized_record_available = true;
  s_localized_prepare_success = true;
  slot.sim.world_navigation_brightness = 7;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.label_draws == 1 && s_localized_draws == 1);
  assert(fabsf(s_localized_brightness - 7.0f / 15.0f) < .0001f);
  assert(s_localized_bounds.x == 585 && s_localized_bounds.y == 80);
  assert(s_localized_bounds.w == 285 && s_localized_bounds.h == 26);

  /* Preparation failure restores the exact captured native glyph layer. */
  s_localized_prepare_success = false;
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.label_draws == 2 && s_localized_draws == 1);
  s_localized_record_available = false;
}

static void TestPalaceMarkerAutoFitAndRasterBounds(void) {
  FakeBackend backend = {.track_markers = true};
  assert(ArRenderDevice_Init(&g_render_device, &kFakeOps, &backend, (ArRenderCapabilities){0}));
  PresentWorldNav_ResetResources();
  FrameSlot slot = WorldNavigationSlot();
  slot.sim.projection_distance_x100 = 0;
  SimWorldNavigationComposition *composition = &slot.sim.world_navigation_scene.composition;
  composition->empty_animation = false;
  /* Deliberately asymmetric around the travel focus, as the native cloud
   * crop is. Resizing must scale that offset, not recenter the raster. */
  composition->palace = (SimWorldNavigationCompositionLayer){
      .visible = true, .screen_x = 123, .screen_y = 97, .width = 16, .height = 16};
  const SimWorldNavigationComposition original = *composition;
  UploadWorldNavigationComposition(&slot);
  const float scale = fminf(1, fmaxf(.35f, 2.25f / Scene3D_AutoFitDistance(.4f)));
  const int sizes[][3] = {{800, 600, 800}, {1280, 720, 960}, {1920, 1080, 1440}};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    backend.output_width = sizes[i][0]; backend.output_height = sizes[i][1];
    assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
    const float x_scale = sizes[i][2] / 256.0f, y_scale = sizes[i][1] / 224.0f;
    const ArRenderRectF actual = backend.palace_rect;
    assert(fabsf(actual.w - 16 * x_scale * scale) < .0001f);
    assert(fabsf(actual.h - 16 * y_scale * scale) < .0001f);
    assert(fabsf(actual.x + actual.w * .5f - (sizes[i][2] * .5f + 3 * x_scale * scale)) < .0001f);
    assert(fabsf(actual.y + actual.h * .5f - (sizes[i][1] * .5f - 7 * y_scale * scale)) < .0001f);
    assert(!memcmp(composition, &original, sizeof(original)));
  }
  const int draws = backend.palace_draws;
  composition->empty_animation = true; /* Native Advent has no marker. */
  UploadWorldNavigationComposition(&slot);
  assert(PresentWorldNavigation3D(&slot) == kPresentationOutcome_Complete);
  assert(backend.palace_draws == draws);
  PresentWorldNav_ResetResources();
}

static void TestSkyPalaceClippingAndOwnership(void) {
  PresentWorldNav_ResetResources();
  FakeBackend backend = {.output_width = 1280, .output_height = 720,
      .require_frustum_clipped = true};
  assert(ArRenderDevice_Init(&g_render_device, &kFakeOps, &backend,
                            (ArRenderCapabilities){0}));
  FrameSlot slot = WorldNavigationSlot();
  slot.sim.view = kSimView_SkyPalace;
  slot.sim.sky_palace_volumetric_clouds = true;
  slot.sim.world_navigation_clouds = true;
  slot.sim.world_navigation_lighting = true;
  slot.sim.world_navigation_backdrop = true;
  slot.sim.cloud_opacity_pct = slot.sim.shadow_opacity_pct = 35;
  slot.sim.shadow_softness_pct = 50;
  slot.sim.light_elevation_deg = 70;
  slot.sim.light_azimuth_deg = 225;
  const ArRenderRectI viewport = {0, 0, 960, 720};
  for (int focus = 0; focus < 6; focus++) {
    int town_x, town_y;
    assert(SimWorldMap_OriginForTown(focus + 1, &town_x, &town_y));
    SimWorldNavigationScene *scene = &slot.sim.world_navigation_scene;
    scene->active_region_valid = true;
    scene->active_region_x = town_x * kSimWorldMapTilePixels;
    scene->active_region_y = town_y * kSimWorldMapTilePixels;
    scene->active_region_width = scene->active_region_height = 32 * kSimWorldMapTilePixels;
    backend.track_palace_focus = true;
    backend.palace_focus_uv = (ArRenderPointF){(town_x + 16) / 128.0f, (town_y + 16) / 128.0f};
    /* Selection metadata, not a stale travel focus, owns Palace framing. */
    slot.sim.world_navigation.focus_x = focus * 200;
    slot.sim.world_navigation.focus_y = 1023 - focus * 200;
    for (int altitude = 0; altitude < 3; altitude++) {
      slot.sim.cloud_altitude_px = altitude * 96;
      slot.sim.landscape_height_pct = altitude * 100;
      assert(PresentWorldNavigationBackdrop(&slot, viewport) == kPresentationOutcome_Complete);
      assert(depth_ocean_quads > 0 && depth_cloud_faces > 0);
      /* A clipped slice can produce up to fourteen triangle-quads. */
      assert(depth_volume_faces > 30 && depth_volume_faces <= 12 * 16 * 14);
      assert(depth_near_volume_faces > 30);
      assert(depth_upper_volume_faces > 20);
      assert(near_volume_max_depth < far_volume_min_depth);
      assert(backend.palace_focus_vertices > 0);
      assert(fabsf(backend.palace_focus_position.x / viewport.w - .5f) < .001f);
      assert(backend.palace_focus_position.y > viewport.h * .54f);
      assert(backend.palace_focus_position.y < viewport.h * .59f);
      assert(depth_shadow_ocean_faces > 0);
      assert(backend.set_viewport_count == 0 && backend.clear_count == 0);
      assert(backend.get_output_size_count == 0 && backend.use_output_coordinates_count == 0);
      assert(backend.palace_draws == 0 && backend.ui_draws == 0);
      assert(backend.sky_backdrop_draws == focus * 3 + altitude + 1);
      const ArRenderColorF upper = backend.sky_backdrop_vertices[2].color;
      assert(upper.b > .7f && upper.r < .15f && upper.g < .2f);
    }
  }
  backend.track_palace_focus = false;
  slot.sim.world_navigation_scene.active_region_valid = false;
  slot.sim.landscape_height_pct = 100;
  for (int edge = 0; edge < 2; edge++) {
    slot.sim.world_navigation.focus_x = slot.sim.world_navigation.focus_y = edge * 1023;
    assert(PresentWorldNavigationBackdrop(&slot, viewport) == kPresentationOutcome_Complete);
  }
  slot.sim.world_navigation_backdrop = false;
  assert(PresentWorldNavigationBackdrop(&slot, viewport) == kPresentationOutcome_Complete);
  for (int i = 1; i < 10; i++)
    assert(!memcmp(&backend.sky_backdrop_vertices[0].color,
        &backend.sky_backdrop_vertices[i].color, sizeof(ArRenderColorF)));
  slot.sim.world_navigation_backdrop = true;
  const int uploads = volume_uploads;
  slot.sim.sky_palace_volumetric_clouds = false;
  assert(PresentWorldNavigationBackdrop(&slot, viewport) == kPresentationOutcome_Complete);
  assert(depth_volume_faces > 0 && depth_volume_faces <= 12 * 14);
  assert(depth_upper_volume_faces > 0);
  assert(depth_near_volume_faces > 0 && near_volume_max_depth < far_volume_min_depth);
  assert(volume_uploads == uploads); /* No rebake for quality toggle. */
  slot.sim.world_navigation_clouds = false;
  assert(PresentWorldNavigationBackdrop(&slot, viewport) == kPresentationOutcome_Complete);
  assert(depth_volume_faces == 0 && volume_uploads == uploads);
  slot.sim.world_navigation_clouds = true;
  slot.sim.light_azimuth_deg++;
  volume_upload_failure = true;
  assert(PresentWorldNavigationBackdrop(&slot, viewport) == kPresentationOutcome_OptionalOmitted);
  assert(depth_volume_faces == 0 && volume_uploads == uploads + 1);
  assert(PresentWorldNavigationBackdrop(&slot, viewport) == kPresentationOutcome_OptionalOmitted);
  assert(volume_uploads == uploads + 1); /* Failed key does not retry each frame. */
  volume_upload_failure = false;
  slot.sim.light_azimuth_deg++;
  assert(PresentWorldNavigationBackdrop(&slot, viewport) == kPresentationOutcome_Complete);
  assert(depth_volume_faces > 0 && volume_uploads == uploads + 2);
  depth_begin_failure = true;
  assert(PresentWorldNavigationBackdrop(&slot, viewport) == kPresentationOutcome_CoreFailure);
  depth_begin_failure = false;
  assert(backend.set_viewport_count == 0);
  AssertPerformanceScopeRestored();
  slot.sim.view = kSimView_WorldNavigation;
  assert(PresentWorldNavigationBackdrop(&slot, viewport) == kPresentationOutcome_CoreFailure);
  PresentWorldNav_ResetResources();
}

static void TestShadowClipPlanParity(void) {
  const ArRenderRectI viewport = {0, 0, 640, 480};
  const ArRenderColorF color = {0, 0, 0, .1275f};
  for (int axis = 0; axis < 6; axis++)
    for (int phase = 0; phase < 13; phase++) {
      Scene3DClipPoint clip[4] = {{-.5f, -.4f, 0, 1}, {.5f, -.4f, 0, 1},
          {.5f, .4f, 0, 1}, {-.5f, .4f, 0, 1}};
      float *component = axis / 2 == 0 ? &clip[0].x : axis / 2 == 1 ? &clip[0].y : &clip[0].z;
      *component = (axis & 1 ? -1 : 1) * (phase + 1) * .25f;
      Sim3DDepthVertex input[4];
      ArRenderPointF uv[4];
      for (int p = 0; p < 4; p++) {
        uv[p] = (ArRenderPointF){(p + phase) * .023f, (3 - p + phase) * .017f};
        input[p] = (Sim3DDepthVertex){(clip[p].x * .5f + .5f) * viewport.w,
            (1 - (clip[p].y * .5f + .5f)) * viewport.h, .5f, color, uv[p]};
      }
      Sim3DDepthVertex expected[kWorldNavigationClippedQuads * 4];
      WorldNavigationClipPlan plans[kWorldNavigationClippedQuads];
      size_t count, expected_count;
      assert(WorldNavigationClipQuad(input, clip, viewport, expected, &expected_count));
      assert(WorldNavigationPrepareClipPlan(input, clip, viewport, plans, &count));
      assert(count == expected_count);
      for (size_t q = 0; q < count; q++) {
        Sim3DDepthVertex actual[4];
        WorldNavigationApplyShadowPlan(&plans[q], uv, color, actual);
        assert(!memcmp(actual, expected + q * 4, sizeof(actual)));
      }
      assert(WorldNavigationPrepareClipPlan(input, NULL, viewport, plans, &count) && count == 1);
      Sim3DDepthVertex uncut[4];
      WorldNavigationApplyShadowPlan(plans, uv, color, uncut);
      assert(!memcmp(uncut, input, sizeof(uncut)));
    }
}

int main(void) {
  TestShadowClipPlanParity();
  TestTownReliefRegistration();
  uint8_t *rom = calloc(1, 0x100000);
  assert(rom && SimWorldMap_Init(rom, 0x100000));
  free(rom);
  TestAspectFitAndLocalGeometry();
  TestFailureRestoresFullOutput();
  TestAuthoredTownModelsUseSharedCacheAndDepth();
  TestOceanBatchFailureRecovery();
  TestSpaceAndCloudCover();
  TestGroundCacheInvalidation();
  TestOptionalStagesSkipWorkAndRestore();
  TestDetailedGroundLiveInvalidation();
  TestNativeMountainRestoration();
  TestLavaUploadRecovery();
  TestAtmosphereEnclosesRaisedTerrain();
  TestGlobeInspection();
  TestLocalizedNavigationLabelHandoff();
  TestPalaceMarkerAutoFitAndRasterBounds();
  TestAdventAuthoredModelClearance();
  TestTallModelViewportClearance();
  TestSkyPalaceClippingAndOwnership();
  TestMapEdgeLandOpacity();
  AssertPerformanceScopeRestored();
  SimWorldMap_Shutdown();
  puts("present_world_nav_test: PASS");
  return 0;
}
