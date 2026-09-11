/* The SIM cloud shroud: deterministic tileable value noise, its texture, and
 * the shroud mesh that covers the permanently actor-free ground beyond OAM's
 * reach. Its camera-dependent mesh is retained; drift changes only sampling.
 *
 * The noise and the layer table are shared with the world-map sky through
 * present_sim3d_internal.h, so the two skies cannot drift apart in look. */

#include "present_sim3d_clouds.h"
#include "present_sim3d_internal.h"
#include "present_sim3d_project.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/host_clock.h"
#include "sim/sim_world_map.h"
#include "sim/sim3d.h"
#include "sim/sim3d_performance.h"
#include "sim/sim_cloud_effect_backend.h"
#include "render/render_device.h"

#ifndef AR_SIM3D_TERRAIN_ELEVATION
#define AR_SIM3D_TERRAIN_ELEVATION 0
#endif

extern ArRenderDevice g_render_device;

static ArRenderTexture s_sim_cloud_texture;
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

typedef struct SimCloudMeshKey {
  ArRenderRectI source, viewport;
  float matrix[16];
  float bounds[4], origin[2], altitude, clear[4], inset, falloff, opacity;
} SimCloudMeshKey;

/* Projection/coverage belong to the camera, not the drifting texture. Keep
 * the exact per-bank arithmetic but project each point once on a key change.
 * This is presentation-owned storage, never borrowed by asynchronous work. */
static struct {
  SimCloudMeshKey key;
  bool ready, indices_ready, visible;
  ArRenderVertex2D vertices[kSimCloudLayerCount][kSimCloudVertexCount];
  ArRenderPointF base_uv[kSimCloudLayerCount][kSimCloudVertexCount];
  ArRenderVertex2D gpu_vertices[kSimCloudVertexCount];
  int32_t indices[kSimCloudIndexCount];
} s_sim_cloud_mesh;

_Static_assert(sizeof(s_sim_cloud_mesh) <= 576 * 1024,
               "Cloud geometry cache must remain bounded below 576 KiB");

static bool SameCloudRect(ArRenderRectI a, ArRenderRectI b) {
  return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static bool SameCloudMesh(const SimCloudMeshKey *a, const SimCloudMeshKey *b) {
  return SameCloudRect(a->source, b->source) && SameCloudRect(a->viewport, b->viewport) &&
      !memcmp(a->matrix, b->matrix, sizeof(a->matrix)) &&
      !memcmp(a->bounds, b->bounds, sizeof(a->bounds)) &&
      !memcmp(a->origin, b->origin, sizeof(a->origin)) &&
      a->altitude == b->altitude && !memcmp(a->clear, b->clear, sizeof(a->clear)) &&
      a->inset == b->inset && a->falloff == b->falloff && a->opacity == b->opacity;
}

static void PrepareSimCloudMesh(const SimCloudMeshKey *key) {
  if (!s_sim_cloud_mesh.indices_ready) {
    int at = 0;
    for (int row = 0; row < kSimCloudRows; row++)
      for (int column = 0; column < kSimCloudColumns; column++) {
        const int top_left = row * (kSimCloudColumns + 1) + column;
        const int bottom_left = top_left + kSimCloudColumns + 1;
        const int32_t cell[6] = {top_left, top_left + 1, bottom_left + 1,
                                top_left, bottom_left + 1, bottom_left};
        memcpy(&s_sim_cloud_mesh.indices[at], cell, sizeof(cell));
        at += 6;
      }
    s_sim_cloud_mesh.indices_ready = true;
  }
  if (s_sim_cloud_mesh.ready && SameCloudMesh(&s_sim_cloud_mesh.key, key)) return;
  s_sim_cloud_mesh.key = *key;
  s_sim_cloud_mesh.ready = true;
  s_sim_cloud_mesh.visible = false;
  const float span = (float)(kSimWorldMapPixels * kSimWorldMapTownScale);
  for (int row = 0; row <= kSimCloudRows; row++) {
    const float y = key->bounds[2] + (key->bounds[3] - key->bounds[2]) *
        (float)row / (float)kSimCloudRows;
    for (int column = 0; column <= kSimCloudColumns; column++) {
      const float x = key->bounds[0] + (key->bounds[1] - key->bounds[0]) *
          (float)column / (float)kSimCloudColumns;
      const float cover = Sim3D_CloudCoverage(x, y, key->clear[0], key->clear[1],
          key->clear[2], key->clear[3], key->inset, key->falloff);
      Scene3DPoint projected;
      if (!ProjectSimTexturePoint(key->matrix, key->source, key->viewport,
              x, y, key->altitude, &projected)) {
        s_sim_cloud_mesh.visible = false;
        return;
      }
      if (cover > 0.0f) s_sim_cloud_mesh.visible = true;
      const int at = row * (kSimCloudColumns + 1) + column;
      s_sim_cloud_mesh.gpu_vertices[at] = (ArRenderVertex2D){
        {projected.x, projected.y}, {1, 1, 1, cover * key->opacity},
        {(x - key->origin[0]) / span, (y - key->origin[1]) / span},
      };
      for (int layer = 0; layer < kSimCloudLayerCount; layer++) {
        const ArRenderPointF uv = {
          ((x - key->origin[0]) / span) * kSimCloudLayers[layer].scale +
              kSimCloudLayers[layer].offset_x,
          ((y - key->origin[1]) / span) * kSimCloudLayers[layer].scale +
              kSimCloudLayers[layer].offset_y,
        };
        s_sim_cloud_mesh.base_uv[layer][at] = uv;
        s_sim_cloud_mesh.vertices[layer][at] = (ArRenderVertex2D){
          {projected.x, projected.y},
          {1.0f, 1.0f, 1.0f, cover * key->opacity * kSimCloudLayers[layer].weight}, uv,
        };
      }
    }
  }
}

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

static ArRenderTexture EnsureSimCloudTexture(void) {
  if (ArRenderTexture_IsValid(s_sim_cloud_texture) ||
      s_sim_cloud_alloc_failed)
    return s_sim_cloud_texture;
  const ArRenderTextureDesc desc = {
    .width = kSimCloudTexturePixels,
    .height = kSimCloudTexturePixels,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Linear,
    .blend = kArRenderBlendMode_Alpha,
  };
  if (!ArRenderDevice_CreateTexture(
          &g_render_device, &desc, &s_sim_cloud_texture)) {
    s_sim_cloud_alloc_failed = true;
    fprintf(stderr, "[sim3d-cloud] shroud texture unavailable: %s\n",
            ArRenderDevice_LastError(&g_render_device));
    return ArRenderTexture_Invalid();
  }

  uint32_t *pixels = malloc(
      (size_t)kSimCloudTexturePixels * kSimCloudTexturePixels *
      sizeof(*pixels));
  if (!pixels) {
    ArRenderDevice_DestroyTexture(&g_render_device, s_sim_cloud_texture);
    s_sim_cloud_texture = ArRenderTexture_Invalid();
    s_sim_cloud_alloc_failed = true;
    return ArRenderTexture_Invalid();
  }
  for (int y = 0; y < kSimCloudTexturePixels; y++) {
    uint32_t *row = pixels + (size_t)y * kSimCloudTexturePixels;
    for (int x = 0; x < kSimCloudTexturePixels; x++) {
      row[x] = SimCloudTexel(x, y);
    }
  }
  const bool uploaded = ArRenderDevice_UpdateTexture(
      &g_render_device, s_sim_cloud_texture, NULL, pixels,
      kSimCloudTexturePixels * (int)sizeof(*pixels));
  free(pixels);
  if (!uploaded) {
    fprintf(stderr, "[sim3d-cloud] shroud upload failed: %s\n",
            ArRenderDevice_LastError(&g_render_device));
    ArRenderDevice_DestroyTexture(&g_render_device, s_sim_cloud_texture);
    s_sim_cloud_texture = ArRenderTexture_Invalid();
    s_sim_cloud_alloc_failed = true;
  }
  return s_sim_cloud_texture;
}

PresentationOutcome DrawSimCloudShroud(const FrameSlot *slot, ArRenderRectI source,
                        ArRenderRectI viewport, const float matrix[16]) {
  if (!slot->sim.underlay_serial || !slot->sim.cloud_opacity_pct ||
      source.w <= 0 || source.h <= 0)
    return kPresentationOutcome_Complete;
  ArRenderTexture texture = EnsureSimCloudTexture();
  if (!ArRenderTexture_IsValid(texture)) return kPresentationOutcome_OptionalOmitted;

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
  if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) return kPresentationOutcome_Complete;

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
  if (world_y0 - world_y1 < 1.0f / source.h) return kPresentationOutcome_Complete;
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
   * failed to meet -- an untextured solid-white geometry pass modulated only
   * by vertex alpha. It did what it said and whited out the view, and the
   * premise was wrong anyway: guaranteeing
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
  uint64_t elapsed_ms = HostClock_Milliseconds();
  float drift = (float)slot->sim.cloud_drift_pct / (float)kPercentScale;

  SimCloudMeshKey key = {
    .source = source, .viewport = viewport,
    .bounds = {x0, x1, y0, y1}, .origin = {texture_x_at_zero, texture_y_at_zero},
    .altitude = altitude, .clear = {clear_x0, clear_x1, clear_y0, clear_y1},
    .inset = inset, .falloff = falloff, .opacity = opacity,
  };
  memcpy(key.matrix, matrix, sizeof(key.matrix));
  PrepareSimCloudMesh(&key);
  if (!s_sim_cloud_mesh.visible) return kPresentationOutcome_Complete;

  const ArRenderDrawState draw_state = {
    .flags = kArRenderDrawState_Address,
    .address_u = kArRenderTextureAddressMode_Wrap,
    .address_v = kArRenderTextureAddressMode_Wrap,
  };
  if (SimCloudEffectBackend_IsAvailable(&g_render_device)) {
    _Static_assert(kSimCloudEffectSamples == kSimCloudLayerCount, "All cloud banks are sampled");
    SimCloudEffectParams params;
    for (int layer = 0; layer < kSimCloudLayerCount; layer++) {
      params.samples[layer] = (SimCloudEffectSample){
        .scale = kSimCloudLayers[layer].scale,
        .offset_x = kSimCloudLayers[layer].offset_x + Scene3D_WrappedTextureOffset(
            elapsed_ms, kSimCloudLayers[layer].drift_x, drift),
        .offset_y = kSimCloudLayers[layer].offset_y + Scene3D_WrappedTextureOffset(
            elapsed_ms, kSimCloudLayers[layer].drift_y, drift),
        .opacity = kSimCloudLayers[layer].weight,
      };
    }
    const bool bound = SimCloudEffectBackend_Bind(&g_render_device, &params);
    bool drawn = false;
    if (bound) drawn = ArRenderDevice_DrawGeometryWithState(&g_render_device, texture,
        s_sim_cloud_mesh.gpu_vertices, kSimCloudVertexCount,
        s_sim_cloud_mesh.indices, kSimCloudIndexCount, &draw_state);
    if (!SimCloudEffectBackend_Unbind(&g_render_device)) return kPresentationOutcome_CoreFailure;
    if (bound) {
      if (drawn) Sim3DPerformance_AddDraw(kSimCloudVertexCount, kSimCloudIndexCount);
      return drawn ? kPresentationOutcome_Complete : kPresentationOutcome_OptionalOmitted;
    }
  }
  PresentationOutcome outcome = kPresentationOutcome_Complete;
  for (unsigned layer = 0;
       layer < (size_t)kSimCloudLayerCount; layer++) {
    ArRenderVertex2D *vertices = s_sim_cloud_mesh.vertices[layer];
    const float offset_x = Scene3D_WrappedTextureOffset(
        elapsed_ms, kSimCloudLayers[layer].drift_x, drift);
    const float offset_y = Scene3D_WrappedTextureOffset(
        elapsed_ms, kSimCloudLayers[layer].drift_y, drift);
    for (int at = 0; at < kSimCloudVertexCount; at++) {
      vertices[at].tex_coord.x = s_sim_cloud_mesh.base_uv[layer][at].x + offset_x;
      vertices[at].tex_coord.y = s_sim_cloud_mesh.base_uv[layer][at].y + offset_y;
    }
    if (ArRenderDevice_DrawGeometryWithState(
            &g_render_device, texture, vertices, kSimCloudVertexCount,
            s_sim_cloud_mesh.indices, kSimCloudIndexCount, &draw_state)) {
      Sim3DPerformance_AddDraw(
          kSimCloudVertexCount, kSimCloudIndexCount);
    } else outcome = kPresentationOutcome_OptionalOmitted;
  }
  return outcome;
}

void PresentSim3DClouds_ResetResources(void) {
  SimCloudEffectBackend_Reset(&g_render_device);
  ArRenderDevice_DestroyTexture(&g_render_device, s_sim_cloud_texture);
  s_sim_cloud_texture = ArRenderTexture_Invalid();
  s_sim_cloud_alloc_failed = false;
  memset(&s_sim_cloud_mesh, 0, sizeof(s_sim_cloud_mesh));
}
