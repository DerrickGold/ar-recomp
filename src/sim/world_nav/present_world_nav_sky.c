/* Palace-only backdrop, mist and density banks. Owns its atlas publication
 * state, but neither the caller's output/depth pass nor native foreground. */
#include "sim/world_nav/present_world_nav_sky.h"
#include "sim/world_nav/completion_vista_backend.h"
#include "sim/sim3d/present_sim3d_environment.h"
#include "sim/world_nav/sim_world_navigation_sky_clouds.h"
#include "sim/sim3d/sim3d_performance.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static struct {
  bool ready, attempted, lighting;
  float light[3];
  SimSkyCloudBounds bounds[kSimSkyCloudBanks][kSimSkyCloudSlices + 1];
} s_clouds;

/* The ending has fixed illumination. Retain its CPU atlas independently of
 * the Palace's mutable GPU atlas so a prior Palace visit cannot force another
 * density bake during the A-to-B reveal. About 1.4 MiB, released on reset. */
static struct {
  uint32_t *pixels;
  SimSkyCloudBounds bounds[kSimSkyCloudBanks][kSimSkyCloudSlices + 1];
} s_completion_clouds;
static const float kCompletionCloudLight[3] = {0, -.90f, -.43589f};

bool PresentWorldNavSky_PrepareCompletion(void) {
  if (s_completion_clouds.pixels) return true;
  uint32_t *pixels = malloc((size_t)kSimSkyCloudAtlasWidth * kSimSkyCloudAtlasHeight *
                           sizeof(*pixels));
  if (!pixels) return false;
  bool ready = SimWorldNavigationSkyClouds_Bake(
      pixels, kSimSkyCloudAtlasWidth, kCompletionCloudLight, true);
  for (int bank = 0; ready && bank < kSimSkyCloudBanks; ++bank)
    for (int slice = 0; ready && slice <= kSimSkyCloudSlices; ++slice)
      ready = SimWorldNavigationSkyClouds_Bounds(pixels, kSimSkyCloudAtlasWidth,
          bank, slice, &s_completion_clouds.bounds[bank][slice]);
  if (!ready) {
    free(pixels);
    return false;
  }
  s_completion_clouds.pixels = pixels;
  return true;
}

static bool CloudBoundsEnabled(void) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *value = getenv("AR_SIM3D_SKY_CLOUD_BOUNDS");
    /* Explicit full-rectangle reference remains available for diagnostics. */
    enabled = !value || strcmp(value, "0") != 0;
  }
  return enabled != 0;
}

float PresentWorldNavSky_Horizon(ArRenderRectI viewport,
                             const WorldNavigationProjection *projection) {
  const float radius = projection->globe_radius_world;
  const float cosine = radius / (radius + projection->camera_world[2]);
  Scene3DPoint horizon;
  if (!Scene3D_ProjectWorldPoint(projection->matrix, 0,
          radius * sqrtf(fmaxf(0, 1 - cosine * cosine)), radius * (cosine - 1),
          viewport.w, viewport.h, &horizon)) return .53f;
  return fminf(.9f, fmaxf(.1f, horizon.y / viewport.h));
}

bool PresentWorldNavSky_DrawBackdrop(ArRenderDevice *device, ArRenderRectI viewport, bool gradient,
                                     float horizon) {
  /* The Palace is in the sky, not a spaceship. This inexpensive native-style
   * daylight gradient is independent of navigation's optional starfield. */
  const ArRenderColorF colors[5] = {
    {.09f, .16f, .78f, 1}, {.09f, .16f, .78f, 1},
    {.25f, .48f, .94f, 1}, {.73f, .87f, 1, 1}, {.73f, .87f, 1, 1},
  };
  /* Reach the pale band at the projected sea horizon, not at the bottom of
   * the framebuffer hidden behind the globe and Palace dialogue box. Hold
   * the rich upper blue through the roof/HUD so it reaches the windows.
   * These fractions describe Palace art direction, not UI ownership. */
  const float rows[5] = {0, horizon * .38f, horizon * .76f, horizon, 1};
  ArRenderVertex2D vertices[10];
  for (int y = 0; y < 5; y++)
    for (int x = 0; x < 2; x++)
      vertices[y * 2 + x] = (ArRenderVertex2D){
        {(float)(x * viewport.w), rows[y] * viewport.h},
        colors[gradient ? y : 2], {0, 0},
      };
  static const int32_t indices[24] = {0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4,
      4, 5, 7, 4, 7, 6, 6, 7, 9, 6, 9, 8};
  return ArRenderDevice_DrawGeometry(device, ArRenderTexture_Invalid(),
      vertices, 10, indices, 24);
}

bool PresentWorldNavSky_DrawMist(ArRenderDevice *device, ArRenderRectI viewport, float horizon) {
  /* Artistic aerial-perspective veil, not a physical scattering solver.
   * The fixed horizon camera makes a small screen-space band useful here;
   * it is composed before the native Palace foreground and master fade. */
  const float rows[4] = {horizon - .07f, horizon, horizon + .07f, horizon + .16f};
  const float opacity[4] = {0, .14f, .045f, 0};
  ArRenderVertex2D vertices[12];
  int32_t indices[18];
  for (int band = 0; band < 3; band++) {
    for (int p = 0; p < 4; p++) {
      const int y = band + (p >= 2);
      vertices[band * 4 + p] = (ArRenderVertex2D){
        {(p == 1 || p == 2) ? (float)viewport.w : 0, rows[y] * viewport.h},
        {.72f, .87f, .98f, opacity[y]}, {0, 0}};
    }
    static const int corners[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) indices[band * 6 + i] = band * 4 + corners[i];
  }
  const ArRenderDrawState blend = {.flags = kArRenderDrawState_Blend,
      .blend = kArRenderBlendMode_Alpha};
  return ArRenderDevice_DrawGeometryWithState(device, ArRenderTexture_Invalid(),
      vertices, 12, indices, 18, &blend);
}

/* Camera-facing volume slices retain actual view depth. All banks are sorted
 * together: sorting bank centres alone fails where their depth ranges overlap.
 * The density/light atlas is baked on key changes, not in the frame loop. */
static bool EnsureSkyPalaceCloudAtlas(ArRenderDevice *device, const float light[3], bool lighting) {
  if (s_clouds.attempted &&
      s_clouds.lighting == lighting &&
      memcmp(s_clouds.light, light, sizeof(s_clouds.light)) == 0)
    return s_clouds.ready;
  s_clouds.attempted = true;
  s_clouds.ready = false;
  s_clouds.lighting = lighting;
  memcpy(s_clouds.light, light, sizeof(s_clouds.light));
  const ArRenderRectI region = {0, 0, kSimSkyCloudAtlasWidth, kSimSkyCloudAtlasHeight};
  if (lighting && !memcmp(light, kCompletionCloudLight, sizeof(kCompletionCloudLight)) &&
      PresentWorldNavSky_PrepareCompletion()) {
    memcpy(s_clouds.bounds, s_completion_clouds.bounds, sizeof(s_clouds.bounds));
    return s_clouds.ready = Sim3DDepthPass_UploadAtlasRegions(
        device, kSim3DDepthPass_VolumeCloud, s_completion_clouds.pixels,
        region.w, region.h, region.w * (int)sizeof(uint32_t), &region, 1);
  }
  uint32_t *pixels = malloc((size_t)kSimSkyCloudAtlasWidth * kSimSkyCloudAtlasHeight *
      sizeof(*pixels));
  if (!pixels) return false;
  bool ready = SimWorldNavigationSkyClouds_Bake(
      pixels, kSimSkyCloudAtlasWidth, light, lighting);
  for (int bank = 0; ready && bank < kSimSkyCloudBanks; ++bank)
    for (int slice = 0; ready && slice <= kSimSkyCloudSlices; ++slice)
      ready = SimWorldNavigationSkyClouds_Bounds(pixels, kSimSkyCloudAtlasWidth,
          bank, slice, &s_clouds.bounds[bank][slice]);
  ready = ready && Sim3DDepthPass_UploadAtlasRegions(device, kSim3DDepthPass_VolumeCloud,
          pixels, region.w, region.h, region.w * (int)sizeof(*pixels), &region, 1);
  free(pixels);
  return s_clouds.ready = ready;
}

bool PresentWorldNavSky_DrawClouds(ArRenderDevice *device, const FrameSlot *slot,
                                   ArRenderRectI viewport,
                                   const WorldNavigationProjection *projection, uint64_t elapsed_ms,
                                   float drift, float opacity) {
  typedef struct SkyCloudBank {
    float x, y, depth, width, height, thickness, speed, opacity;
    int bank;
  } SkyCloudBank;
  static const SkyCloudBank palace_banks[] = {
    {.12f, -.035f, 7.5f, .60f, .24f, 1.1f, .012f, 1, 0},
    {.54f, -.065f, 8.0f, .62f, .23f, 1.2f, .009f, 1, 1},
    {.93f, -.020f, 7.0f, .56f, .27f, 1.2f, .014f, 1, 2},
    /* A staggered upper deck reuses the same baked shapes. Three repeating
     * banks keep clouds passing through the upper windows, with blue gaps. */
    {.08f, -.28f, 6.5f, .48f, .19f, .7f, .011f, 1, 2},
    {.85f, -.32f, 6.8f, .50f, .20f, .7f, .011f, 1, 0},
    {1.62f, -.25f, 6.3f, .46f, .18f, .65f, .011f, 1, 1},
    /* Two staggered, repeating decks cross the globe instead of merely
     * skirting its silhouette. The lower deck travels faster for parallax;
     * softer middle banks leave glimpses of towns between the billows.
     * Native angel/pillars/UI are composed above the entire scene. */
    {.14f, .11f, 1.9f, .86f, .38f, .25f, .010f, .80f, 3},
    {.91f, .14f, 2.0f, .90f, .40f, .25f, .010f, .80f, 1},
    {1.68f, .12f, 1.8f, .88f, .39f, .25f, .010f, .80f, 2},
    {.03f, .34f, 1.2f, .98f, .48f, .18f, .015f, .90f, 0},
    {.80f, .36f, 1.3f, 1.02f, .49f, .18f, .015f, .90f, 3},
    {1.57f, .33f, 1.1f, .96f, .47f, .18f, .015f, .90f, 1},
  };
  /* Reuse the Palace's density volumes as a luminous cloud arch around the
   * completion sky's luminous opening, with low wisps framing the ocean. */
  static const SkyCloudBank heaven_banks[] = {
    {.03f,-.025f,8.0f,.62f,.38f,1.0f,.0010f,1,0},
    {.97f,-.020f,8.1f,.62f,.39f,1.0f,.0012f,1,2},
    {.13f,.115f,7.5f,.46f,.29f,.9f,.0008f,1,1},
    {.87f,.130f,7.6f,.45f,.28f,.9f,.0009f,1,3},
    {.27f,.225f,7.2f,.44f,.22f,.8f,.0007f,1,2},
    {.73f,.225f,7.3f,.44f,.22f,.8f,.0008f,1,0},
    {.47f,.285f,7.0f,.43f,.17f,.7f,.0005f,.9f,1},
    {.65f,.260f,7.1f,.32f,.16f,.7f,.0006f,.9f,3},
    {.04f,.540f,5.0f,.42f,.13f,.4f,.0016f,.65f,0},
    {.96f,.525f,5.1f,.44f,.12f,.4f,.0014f,.65f,2},
    {.21f,.630f,4.8f,.40f,.11f,.3f,.0019f,.60f,3},
    {.80f,.610f,4.9f,.41f,.10f,.3f,.0017f,.60f,1},
    /* Small, nearer translucent wisps cross the distant angel silhouettes.
     * Their independent speed and depth keep the opening spacious. */
    {.38f,.095f,4.3f,.18f,.065f,.16f,.0030f,.28f,3},
    {.60f,.055f,4.5f,.16f,.055f,.14f,.0020f,.22f,1},
    {.53f,.155f,4.1f,.20f,.050f,.12f,.0026f,.24f,0},
  };
  enum { kBanks = sizeof(heaven_banks) / sizeof(heaven_banks[0]), kSlices = kSimSkyCloudSlices };
  const bool completion = slot->sim.death_heim_completion_world;
  const bool heavens=completion && CompletionVistaBackend_IsAvailable(device);
  const SkyCloudBank *banks=heavens ? heaven_banks : palace_banks;
  const int bank_count=heavens ? kBanks : sizeof(palace_banks)/sizeof(palace_banks[0]);
  float right[3], down[3], forward[3];
  float scale_x = 0, scale_y = 0;
  for (int c = 0; c < 3; c++) {
    right[c] = projection->matrix[c * 4];
    down[c] = -projection->matrix[c * 4 + 1];
    forward[c] = projection->matrix[c * 4 + 3];
    scale_x += right[c] * right[c];
    scale_y += down[c] * down[c];
  }
  scale_x = sqrtf(scale_x);
  scale_y = sqrtf(scale_y);
  if (scale_x < .0001f || scale_y < .0001f) return false;
  for (int c = 0; c < 3; c++) { right[c] /= scale_x; down[c] /= scale_y; }
  const float azimuth = slot->sim.light_azimuth_deg * kPi / 180;
  const float elevation = slot->sim.light_elevation_deg * kPi / 180;
  const float sun[3] = {-cosf(azimuth) * cosf(elevation),
      -sinf(azimuth) * cosf(elevation), sinf(elevation)};
  float local_light[3] = {0};
  for (int c = 0; c < 3; c++) {
    local_light[0] += sun[c] * right[c];
    local_light[1] += sun[c] * down[c];
    local_light[2] += sun[c] * forward[c];
  }
  if(heavens) {
    memcpy(local_light, kCompletionCloudLight, sizeof(local_light));
  }
  if (!EnsureSkyPalaceCloudAtlas(device, local_light, slot->sim.world_navigation_lighting))
    return false;
  const bool volume = slot->sim.sky_palace_volumetric_clouds;
  const int slices = volume ? kSlices : 1;
  struct Slice { float depth; int bank, tile; };
  /* Depth and bank layout are immutable. Wind translates the banks laterally
   * but never changes this global back-to-front ordering. */
  static struct { struct Slice items[kBanks * kSlices]; int count; } orders[2][2];
  struct Slice *ordered = orders[heavens][volume].items;
  const bool prepare_order = orders[heavens][volume].count == 0;
  float centre_x[kBanks];
  int count = orders[heavens][volume].count;
  for (int b = 0; b < bank_count; b++) {
    const float shift = Scene3D_WrappedTextureOffset(elapsed_ms, banks[b].speed, drift);
    /* A .65 viewport margin encloses even the widest nearest slice.
     * Advection is per bank, not redundantly recomputed for every slice. */
    centre_x[b] = fmodf(banks[b].x + .65f + shift * 2.3f, 2.3f) - .65f;
    if (!prepare_order) continue;
    for (int z = 0; z < slices; z++) {
      const struct Slice slice = {banks[b].depth +
          (volume ? ((z + .5f) / kSlices - .5f) * banks[b].thickness : 0),
          b, volume ? z : kSlices};
      int at = count++;
      while (at && ordered[at - 1].depth < slice.depth) {
        ordered[at] = ordered[at - 1];
        at--;
      }
      ordered[at] = slice;
    }
  }
  orders[heavens][volume].count = count;
  const float horizon = PresentWorldNavSky_Horizon(viewport, projection);
  Sim3DPerformance_AddPath(kSim3DPath_CpuProject);
  Sim3DDepthVertex vertices[kBanks * kSlices * 4];
  Scene3DClipPoint clips[kBanks * kSlices * 4];
  size_t used = 0;
  const bool trim = CloudBoundsEnabled();
  for (int i = 0; i < count; i++) {
    const int b = ordered[i].bank, tile = ordered[i].tile;
    if (completion && !heavens && b >= 6) continue;
    /* Crop positions AND UVs in the same original slice coordinates. Keep
     * its centre, depth, density samples and global ordering unchanged. */
    const SimSkyCloudBounds bounds = trim ? s_clouds.bounds[banks[b].bank][tile]
        : (SimSkyCloudBounds){0, 0, kSimSkyCloudWidth - 1, kSimSkyCloudHeight - 1};
    if (bounds.x0 == bounds.x1 || bounds.y0 == bounds.y1) continue;
    const float x = centre_x[b];
    const float y = heavens ? banks[b].y : horizon + banks[b].y - (completion ? .14f : 0);
    for (int p = 0; p < 4; p++) {
      const float texel_x = p == 1 || p == 2 ? bounds.x1 : bounds.x0;
      const float texel_y = p >= 2 ? bounds.y1 : bounds.y0;
      const float dx = texel_x / (kSimSkyCloudWidth - 1);
      const float dy = texel_y / (kSimSkyCloudHeight - 1);
      const float lateral = ((2 * x - 1) + (dx - .5f) * banks[b].width * 2) *
          banks[b].depth / scale_x;
      const float vertical = ((2 * y - 1) + (dy - .5f) * banks[b].height * 2) *
          banks[b].depth / scale_y;
      float world[3];
      for (int c = 0; c < 3; c++)
        world[c] = projection->camera_world[c] + forward[c] * ordered[i].depth +
            right[c] * lateral + down[c] * vertical;
      Scene3DPoint screen;
      Sim3DDepthVertex *v = &vertices[used * 4 + p];
      if (!WorldNavigationProjectPoint(projection, viewport, world, &screen,
              &v->depth, &clips[used * 4 + p])) return false;
      v->x = screen.x;
      v->y = screen.y;
      v->color = (ArRenderColorF){1, 1, 1, fminf(1, opacity * 2) * banks[b].opacity};
      if(heavens) {
        const float cloud_x=x+(dx-.5f)*banks[b].width;
        const float cloud_y=y+(dy-.5f)*banks[b].height;
        const float sx=(cloud_x-.5f)/.36f, sy=(cloud_y+.055f)/.30f;
        const float radius=sqrtf(sx*sx+sy*sy);
        const float rim=expf(-powf((radius-.95f)*1.8f,2));
        /* Lift the ambient fill outside the opening's bright rim so the
         * arch keeps its volume without becoming a dark storm ceiling. */
        v->color.r=.60f+.40f*rim;
        v->color.g=.66f+.31f*rim;
        v->color.b=.74f+.26f*rim;
      }
      v->uv = (ArRenderPointF){
        (tile % kSimSkyCloudColumns * kSimSkyCloudWidth + .5f +
            texel_x) / kSimSkyCloudAtlasWidth,
        (banks[b].bank * kSimSkyCloudRows * kSimSkyCloudHeight +
            tile / kSimSkyCloudColumns * kSimSkyCloudHeight + .5f +
            texel_y) / kSimSkyCloudAtlasHeight};
    }
    ++used;
  }
  return WorldNavigationAppendProjectedQuads(kSim3DDepthPass_VolumeCloud,
      vertices, clips, used, viewport);
}


void PresentWorldNavSky_Reset(void) {
  s_clouds.ready = s_clouds.attempted = false;
  free(s_completion_clouds.pixels);
  s_completion_clouds.pixels = NULL;
}
