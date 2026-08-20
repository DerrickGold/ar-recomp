/* The SIM cloud shroud: deterministic tileable value noise, its texture, and
 * the shroud mesh that covers the permanently actor-free ground beyond OAM's
 * reach. Split out of present_sim3d.c; the definitions are unchanged.
 *
 * The noise and the layer table are shared with the world-map sky through
 * present_sim3d_internal.h, so the two skies cannot drift apart in look. */

#include <SDL3/SDL.h>
#include "present_sim3d_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim/sim_world_map.h"
#include "sim/sim3d.h"

/* kPixelAspect_Crt43 and kDioramaCam_Free/kDioramaCam_Dynamic are plain enum
 * constants (not live state) — fine to pull in just for those. */

#ifndef AR_SIM3D_TERRAIN_ELEVATION
#define AR_SIM3D_TERRAIN_ELEVATION 0
#endif


extern SDL_Renderer *g_renderer;
extern SDL_Texture *g_texture;
extern SDL_Texture *g_hud_bg_texture;
extern SDL_Texture *g_hud_obj_texture;
extern uint8_t g_pixels[];
extern uint8_t g_hud_bg_pixels[];
extern uint8_t g_hud_obj_pixels[];
extern SDL_Texture *g_diorama_textures[kDioramaPlane_Count];
extern uint8_t *g_diorama_layer_pixels[kDioramaPlane_Count];
extern SDL_Texture *g_sim_obj_atlas_texture;

extern SDL_Texture *g_sim3d_layer_textures[kSim3DPlane_Count];
extern SDL_Texture *g_sim3d_flat_texture;

#include "present_sim3d_project.h"

static SDL_Texture *s_sim_cloud_texture;
static bool s_sim_cloud_alloc_failed;

/* Cloud shroud.
 *
 * The ground extension reaches the whole town and beyond, but OAM can only
 * place sprites inside the authentic window plus its live widescreen margins.
 * Everything past that is permanently actor-free, and an empty town reads as a
 * bug rather than as distance. The shroud covers exactly that region: it is
 * drawn last, over the objects, so what it hides is unresolvably distant
 * instead of missing.
 *
 * The field is anchored in town space, sampled through the same mapping the
 * underlay uses, so clouds sit over places rather than sliding with the
 * camera. What moves is the hole: coverage is computed against the
 * sprite-drawable rectangle, which follows the camera, so advancing thins the
 * cover ahead and thickens it behind. That is the "whisking aside" without any
 * animation at all. */
enum {
  kSimCloudOctaves = 5,
  /* Reuse the extension mesh density; the shroud covers the same trapezoid and
   * suffers the same affine-UV error if it is coarser. */
  kSimCloudColumns = kSimUnderlayColumns,
  kSimCloudRows = kSimUnderlayRows,
  kSimCloudVertexCount = (kSimCloudColumns + 1) * (kSimCloudRows + 1),
  kSimCloudIndexCount = kSimCloudColumns * kSimCloudRows * 6,
};

const SimCloudLayer kSimCloudLayers[] = {
  { 4.0f, 0.00f, 0.00f, 1.00f, 0.0060f, 0.0011f },
  { 2.7f, 0.37f, 0.61f, 0.85f, 0.0037f, 0.0008f },
  { 6.3f, 0.72f, 0.19f, 0.70f, 0.0094f, 0.0021f },
};
const int kSimCloudLayerCount =
    (int)(sizeof(kSimCloudLayers) / sizeof(kSimCloudLayers[0]));

/* Deterministic value noise. A hash rather than rand() so the field is
 * identical every run and a checkpoint image is reproducible. */
static float CloudHash(int x, int y) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

static float CloudSmooth(float t) { return t * t * (3.0f - 2.0f * t); }

/* Tileable value noise at `period` cells across the texture. */
static float CloudNoise(float x, float y, int period) {
  int x0 = (int)floorf(x), y0 = (int)floorf(y);
  float fx = CloudSmooth(x - (float)x0), fy = CloudSmooth(y - (float)y0);
  int xa = ((x0 % period) + period) % period;
  int ya = ((y0 % period) + period) % period;
  int xb = (xa + 1) % period, yb = (ya + 1) % period;
  float n00 = CloudHash(xa, ya), n10 = CloudHash(xb, ya);
  float n01 = CloudHash(xa, yb), n11 = CloudHash(xb, yb);
  float top = n00 + (n10 - n00) * fx;
  float bottom = n01 + (n11 - n01) * fx;
  return top + (bottom - top) * fy;
}

uint32_t SimCloudTexel(int x, int y) {
  float amplitude = 0.5f, total = 0.0f, sum = 0.0f;
  int period = 4;
  for (int octave = 0; octave < kSimCloudOctaves; octave++) {
    /* Include both periodic endpoints. Navigation's padded texture contains
     * exact copies of this base field, so the first and last texels must
     * match under linear filtering. */
    const float scale =
        (float)period / (float)(kSimCloudTexturePixels - 1);
    /* Offset every octave off its lattice axes. Sampling every octave at
     * y=0/period made the periodic boundary a coherent straight feature even
     * though its endpoint values matched. */
    const float octave_x = 0.37f + (float)octave * 0.53f;
    const float octave_y = 0.61f + (float)octave * 0.29f;
    total += CloudNoise((float)x * scale + octave_x,
                        (float)y * scale + octave_y, period) *
        amplitude;
    sum += amplitude;
    amplitude *= 0.5f;
    period *= 2;
  }
  float density = (total / sum - 0.42f) / 0.38f;
  if (density < 0.0f) density = 0.0f;
  if (density > 1.0f) density = 1.0f;
  density = CloudSmooth(density);
  unsigned alpha = (unsigned)(density * 255.0f + 0.5f);
  unsigned tint = 236 + (unsigned)(density * 19.0f);
  if (tint > 255) tint = 255;
  return (alpha << 24) | (tint << 16) | (tint << 8) | 255u;
}

static SDL_Texture *EnsureSimCloudTexture(void) {
  if (s_sim_cloud_texture || s_sim_cloud_alloc_failed)
    return s_sim_cloud_texture;
  s_sim_cloud_texture = SDL_CreateTexture(
      g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kSimCloudTexturePixels, kSimCloudTexturePixels);
  if (!s_sim_cloud_texture) {
    s_sim_cloud_alloc_failed = true;
    fprintf(stderr, "[sim3d-cloud] shroud texture unavailable: %s\n",
            SDL_GetError());
    return NULL;
  }
  SDL_SetTextureBlendMode(s_sim_cloud_texture, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(s_sim_cloud_texture, SDL_SCALEMODE_LINEAR);

  void *pixels = NULL;
  int pitch = 0;
  if (!SDL_LockTexture(s_sim_cloud_texture, NULL, &pixels, &pitch)) {
    SDL_DestroyTexture(s_sim_cloud_texture);
    s_sim_cloud_texture = NULL;
    s_sim_cloud_alloc_failed = true;
    return NULL;
  }
  if (pitch <= 0) {
    SDL_UnlockTexture(s_sim_cloud_texture);
    SDL_DestroyTexture(s_sim_cloud_texture);
    s_sim_cloud_texture = NULL;
    s_sim_cloud_alloc_failed = true;
    return NULL;
  }
  for (int y = 0; y < kSimCloudTexturePixels; y++) {
    uint32_t *row = (uint32_t *)((uint8_t *)pixels +
        (size_t)y * (size_t)pitch);
    for (int x = 0; x < kSimCloudTexturePixels; x++)
      row[x] = SimCloudTexel(x, y);
  }
  SDL_UnlockTexture(s_sim_cloud_texture);
  return s_sim_cloud_texture;
}

void DrawSimCloudShroud(const FrameSlot *slot, SDL_Rect source,
                               SDL_Rect viewport, const float matrix[16]) {
  if (!slot->sim.underlay_serial || !slot->sim.cloud_opacity_pct ||
      source.w <= 0 || source.h <= 0)
    return;
  SDL_Texture *texture = EnsureSimCloudTexture();
  if (!texture) return;

  /* Same town-space mapping as the underlay, so a cloud stays over the ground
   * it covers when the camera moves. */
  float origin_x = (float)slot->sim.underlay_origin_tile_x *
      (float)kSimWorldMapTilePixels * (float)kSimWorldMapTownScale;
  float origin_y = (float)slot->sim.underlay_origin_tile_y *
      (float)kSimWorldMapTilePixels * (float)kSimWorldMapTownScale;
  float texture_x_at_zero =
      (float)slot->sim.underlay_screen_x0 - (float)slot->sim.camera_x - origin_x;
  float texture_y_at_zero = -(float)slot->sim.camera_y - origin_y;
  float span = (float)(kSimWorldMapPixels * kSimWorldMapTownScale);

  /* Lifted off the ground plane, using the same pixels-to-world-units scale
   * D3c virtual heights use, so "72 pixels up" means the same thing to a
   * cloud as it does to a flying actor. This must be resolved before the
   * near-plane bound: a constant-height cloud has a different camera-plane
   * intersection than the ground below it. */
  float altitude = SimTerrainMaximumHeightWorld(slot, source) +
      SimHeightWorldUnits(source, slot->sim.cloud_altitude_px,
                          slot->sim.height_scale_x100);

  float x0 = texture_x_at_zero, x1 = texture_x_at_zero + span;
  float y0 = texture_y_at_zero, y1 = texture_y_at_zero + span;
  float margin = (float)kSimUnderlayMarginPixels;
  if (x0 < source.x - margin) x0 = (float)source.x - margin;
  if (x1 > source.x + source.w + margin)
    x1 = (float)(source.x + source.w) + margin;
  if (y0 < source.y - margin) y0 = (float)source.y - margin;
  if (y1 > source.y + source.h + margin)
    y1 = (float)(source.y + source.h) + margin;
  if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) return;

  float aspect = (float)viewport.w / (float)viewport.h;
  float world_y0 = 0.5f - (y0 - source.y) / source.h;
  float world_y1 = 0.5f - (y1 - source.y) / source.h;
  for (int corner = 0; corner < 2; corner++) {
    float texture_x = corner ? x1 : x0;
    float world_x = ((texture_x - source.x) / source.w - 0.5f) * aspect;
    float boundary = 0.0f;
    bool increasing = false;
    if (!Scene3D_DepthBoundaryY(matrix, world_x, altitude,
                                kSimUnderlayMinClipDepth, &boundary,
                                &increasing))
      continue;
    if (increasing) {
      if (world_y1 < boundary) world_y1 = boundary;
    } else if (world_y0 > boundary) {
      world_y0 = boundary;
    }
  }
  if (world_y0 - world_y1 < 1.0f / source.h) return;
  y0 = source.y + (0.5f - world_y0) * source.h;
  y1 = source.y + (0.5f - world_y1) * source.h;

  float clear_x0 = (float)slot->sim.cloud_clear_x0;
  float clear_x1 = (float)slot->sim.cloud_clear_x1;
  float clear_y0 = (float)(source.y + slot->sim.cloud_clear_y0);
  float clear_y1 = (float)(source.y + slot->sim.cloud_clear_y1);
  float falloff = (float)slot->sim.cloud_falloff_px;
  float inset = (float)slot->sim.cloud_inset_px;
  float opacity =
      (float)slot->sim.cloud_opacity_pct / (float)kPercentScale;
  /* Overlapping banks at different scales and offsets, so each layer's gaps
   * sit over another layer's body and alpha compounds as `1 - prod(1 - a)`.
   *
   * There is deliberately no untextured floor pass. One used to sit at the
   * end, weighted by cover^3, to force the far field opaque where the banks
   * failed to meet -- an SDL_RenderGeometry call with a NULL texture, which
   * is solid white modulated only by vertex alpha. It did what it said and
   * whited out the view, and the premise was wrong anyway: guaranteeing
   * opacity is no longer this pass's job. Per-record cover hides what the
   * sprite window takes away, and the cull haze marks the boundary
   * continuously, so the banks here are free to be thin and gappy. */
  /* Drift, in texture widths per second.
   *
   * Each bank moves at its own rate, and the coarse layer moves slowest: that
   * difference is the whole effect. Three layers sliding together read as one
   * translating image no matter how the noise is shaped, whereas differing
   * rates make the banks pass through each other and the field appears to
   * churn -- gaps opening and closing on their own rather than sweeping by.
   *
   * Wall time rather than the game frame. Weather does not owe the simulation
   * anything, it keeps moving through a pause, and game_frame is a 16-bit
   * counter that would jump the whole sky every eighteen minutes when it
   * wrapped. */
  Uint64 elapsed_ms = SDL_GetTicks();
  float drift = (float)slot->sim.cloud_drift_pct / (float)kPercentScale;

  static SDL_Vertex vertices[kSimCloudVertexCount];
  static int indices[kSimCloudIndexCount];
  int index_count = 0;
  for (int row = 0; row < kSimCloudRows; row++) {
    for (int column = 0; column < kSimCloudColumns; column++) {
      int top_left = row * (kSimCloudColumns + 1) + column;
      int bottom_left = top_left + kSimCloudColumns + 1;
      indices[index_count++] = top_left;
      indices[index_count++] = top_left + 1;
      indices[index_count++] = bottom_left + 1;
      indices[index_count++] = top_left;
      indices[index_count++] = bottom_left + 1;
      indices[index_count++] = bottom_left;
    }
  }

  SDL_SetRenderTextureAddressMode(g_renderer, SDL_TEXTURE_ADDRESS_WRAP,
                                  SDL_TEXTURE_ADDRESS_WRAP);
  for (unsigned layer = 0;
       layer < sizeof(kSimCloudLayers) / sizeof(kSimCloudLayers[0]); layer++) {
    float scale = kSimCloudLayers[layer].scale;
    float weight = kSimCloudLayers[layer].weight;
    int vertex_count = 0;
    bool any_cover = false;
    for (int row = 0; row <= kSimCloudRows; row++) {
      float texture_y = y0 + (y1 - y0) * (float)row / (float)kSimCloudRows;
      for (int column = 0; column <= kSimCloudColumns; column++) {
        float texture_x =
            x0 + (x1 - x0) * (float)column / (float)kSimCloudColumns;
        float cover = Sim3D_CloudCoverage(texture_x, texture_y, clear_x0,
                                          clear_x1, clear_y0, clear_y1,
                                          inset, falloff);
        if (cover > 0.0f) any_cover = true;
        Scene3DPoint projected;
        if (!ProjectSimTexturePoint(matrix, source, viewport, texture_x,
                                    texture_y, altitude, &projected)) {
          any_cover = false;
          vertex_count = 0;
          break;
        }
        float u = ((texture_x - texture_x_at_zero) / span) * scale +
            kSimCloudLayers[layer].offset_x +
            Scene3D_WrappedTextureOffset(
                elapsed_ms, kSimCloudLayers[layer].drift_x, drift);
        float v = ((texture_y - texture_y_at_zero) / span) * scale +
            kSimCloudLayers[layer].offset_y +
            Scene3D_WrappedTextureOffset(
                elapsed_ms, kSimCloudLayers[layer].drift_y, drift);
        vertices[vertex_count++] = (SDL_Vertex){
          { projected.x, projected.y },
          { 1.0f, 1.0f, 1.0f, cover * opacity * weight },
          { u, v },
        };
      }
      if (!vertex_count) break;
    }
    if (!any_cover || !vertex_count) continue;
    SDL_RenderGeometry(g_renderer, texture, vertices, vertex_count, indices,
                       index_count);
  }
  SDL_SetRenderTextureAddressMode(g_renderer, SDL_TEXTURE_ADDRESS_AUTO,
                                  SDL_TEXTURE_ADDRESS_AUTO);
}

void PresentSim3DClouds_ResetResources(void) {
  if (s_sim_cloud_texture) SDL_DestroyTexture(s_sim_cloud_texture);
  s_sim_cloud_texture = NULL;
  s_sim_cloud_alloc_failed = false;
}
