/* The world-map navigation renderer: the 3D sky/ground/weather treatment shown
 * while travelling between towns, split verbatim out of present_sim3d.c.
 *
 * Split out for isolation rather than for size. saves/sim-actions.rec crosses
 * this path during GPU smoke runs, but it does not yet have a dedicated,
 * pixel-stable checkpoint. Keeping the renderer in its own translation unit
 * makes that remaining regression-asset gap structural and visible.
 *
 * The D6 no-live-globals invariant holds here as everywhere in the present
 * family: no g_ppu, no g_settings, no Settings_Visible*(). State arrives via
 * the `const FrameSlot *`. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "present.h"
#include "action/action_effect_render.h"
#include "constants.h"
#include "snesrecomp/game/types.h"
#include "diorama/diorama.h"
#include "host/host_clock.h"
#include "presentation_outcome.h"
#include "render/render_device.h"
#include "render/render_output.h"
#include "scene3d_math.h"
#include "sim/sim_world_map.h"
#include "sim/sim_world_navigation_capture.h"
#include "sim/sim3d.h"

/* kPixelAspect_Crt43 and kDioramaCam_Free/kDioramaCam_Dynamic are plain enum
 * constants (not live state) — fine to pull in just for those. */
#include "settings.h"
extern ArRenderDevice g_render_device;
#include "present_sim3d_internal.h"


static ArRenderTexture s_world_navigation_palace_texture;
static ArRenderTexture s_world_navigation_ui_texture;
static bool s_world_navigation_composition_upload_valid;
static bool s_world_navigation_cloud_unavailable;
static bool s_world_navigation_weather_failure_reported;

static ArRenderTexture EnsureWorldNavigationCompositionTexture(
    ArRenderTexture *texture) {
  if (ArRenderTexture_IsValid(*texture)) return *texture;
  const ArRenderTextureDesc desc = {
    .width = kSimWorldNavigationCompositionWidth,
    .height = kSimWorldNavigationCompositionHeight,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  if (!ArRenderDevice_CreateTexture(&g_render_device, &desc, texture)) {
    fprintf(stderr,
            "[world-navigation] composition texture unavailable: %s\n",
            ArRenderDevice_LastError(&g_render_device));
    return ArRenderTexture_Invalid();
  }
  return *texture;
}

void UploadWorldNavigationComposition(const FrameSlot *slot) {
  s_world_navigation_composition_upload_valid = false;
  if (!slot || slot->sim.view != kSimView_WorldNavigation) return;
  const SimWorldNavigationComposition *composition =
      &slot->sim.world_navigation_scene.composition;
  if (!composition->valid) return;
  if (composition->empty_animation) {
    s_world_navigation_composition_upload_valid = true;
    return;
  }

  ArRenderTexture palace = EnsureWorldNavigationCompositionTexture(
      &s_world_navigation_palace_texture);
  ArRenderTexture ui = EnsureWorldNavigationCompositionTexture(
      &s_world_navigation_ui_texture);
  if (!ArRenderTexture_IsValid(palace) ||
      !ArRenderTexture_IsValid(ui))
    return;
  ArRenderRectI palace_rect = {
    0, 0, composition->palace.width, composition->palace.height,
  };
  ArRenderRectI ui_rect = {
    0, 0, composition->ui.width, composition->ui.height,
  };
  if (!ArRenderDevice_UpdateTexture(
          &g_render_device, palace, &palace_rect,
          g_sim_world_navigation_palace_pixels,
          kSimWorldNavigationCompositionPitch) ||
      !ArRenderDevice_UpdateTexture(
          &g_render_device, ui, &ui_rect,
          g_sim_world_navigation_ui_pixels,
          kSimWorldNavigationCompositionPitch))
    return;
  s_world_navigation_composition_upload_valid = true;
}
static ArRenderTexture s_world_navigation_cloud_texture;

static ArRenderTexture EnsureWorldNavigationCloudTexture(void) {
  enum {
    kPaddedPixels = kSimCloudTexturePixels * 2,
  };
  if (ArRenderTexture_IsValid(s_world_navigation_cloud_texture))
    return s_world_navigation_cloud_texture;
  /* A texture-allocation or lock failure makes clouds unavailable only for
   * this renderer generation.  They are optional atmosphere, so keep the
   * complete ground/composition path and re-probe after a renderer reset. */
  if (s_world_navigation_cloud_unavailable)
    return ArRenderTexture_Invalid();
  const ArRenderTextureDesc desc = {
    .width = kPaddedPixels,
    .height = kPaddedPixels,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Streaming,
    .filter = kArRenderFilter_Linear,
    .blend = kArRenderBlendMode_Alpha,
  };
  if (!ArRenderDevice_CreateTexture(
          &g_render_device, &desc, &s_world_navigation_cloud_texture)) {
    s_world_navigation_cloud_unavailable = true;
    fprintf(stderr,
            "[world-navigation] cloud texture unavailable: %s\n",
            ArRenderDevice_LastError(&g_render_device));
    return ArRenderTexture_Invalid();
  }
  uint32_t *pixels = malloc(
      (size_t)kPaddedPixels * kPaddedPixels * sizeof(*pixels));
  if (!pixels) {
    ArRenderDevice_DestroyTexture(
        &g_render_device, s_world_navigation_cloud_texture);
    s_world_navigation_cloud_texture = ArRenderTexture_Invalid();
    s_world_navigation_cloud_unavailable = true;
    fprintf(stderr,
            "[world-navigation] cloud upload staging unavailable\n");
    return ArRenderTexture_Invalid();
  }
  for (int y = 0; y < kPaddedPixels; y++) {
    uint32_t *row = pixels + (size_t)y * kPaddedPixels;
    for (int x = 0; x < kPaddedPixels; x++)
      row[x] = SimCloudTexel(
          x % kSimCloudTexturePixels, y % kSimCloudTexturePixels);
  }
  const bool uploaded = ArRenderDevice_UpdateTexture(
      &g_render_device, s_world_navigation_cloud_texture, NULL, pixels,
      kPaddedPixels * (int)sizeof(*pixels));
  free(pixels);
  if (!uploaded) {
    fprintf(stderr, "[world-navigation] cloud upload failed: %s\n",
            ArRenderDevice_LastError(&g_render_device));
    ArRenderDevice_DestroyTexture(
        &g_render_device, s_world_navigation_cloud_texture);
    s_world_navigation_cloud_texture = ArRenderTexture_Invalid();
    s_world_navigation_cloud_unavailable = true;
  }
  return s_world_navigation_cloud_texture;
}

/* $09 full-world presentation.
 *
 * Navigation uses the same developed texture cache as the town underlay, but
 * none of the underlay geometry: the complete map is the primary plane and
 * its captured Mode-7 matrix is already an affine top-down camera. */
static ArRenderPointF WorldNavigationAuthenticToOutput(
    const FrameSlot *slot, ArRenderRectI viewport,
    float authentic_x, float authentic_y) {
  const float authentic_x0 =
      ((float)slot->snes_width -
       (float)kSimWorldNavigationCompositionWidth) * 0.5f;
  const float captured_x = authentic_x0 + authentic_x;
  return (ArRenderPointF){
    (float)viewport.x +
        (captured_x - (float)slot->visible_x0) *
            (float)viewport.w / (float)slot->visible_width,
    (float)viewport.y + authentic_y *
        (float)viewport.h / (float)slot->snes_height,
  };
}

static bool DrawWorldNavigationGround(
    const FrameSlot *slot, ArRenderRectI viewport, ArRenderTexture texture) {
  const SimWorldNavigationScene *scene =
      &slot->sim.world_navigation_scene;
  if (!ArRenderTexture_IsValid(texture) || !scene->valid ||
      slot->visible_width <= 0 ||
      slot->snes_height <= 0)
    return false;

  ArRenderVertex2D vertices[4];
  for (int i = 0; i < 4; i++) {
    float authentic_x = 0.0f, authentic_y = 0.0f;
    const float source_x =
        (float)scene->ground[i].tile_x * kSimWorldMapTilePixels;
    const float source_y =
        (float)scene->ground[i].tile_y * kSimWorldMapTilePixels;
    if (!SimWorldNavigationScene_ProjectSource(
            scene, source_x, source_y, &authentic_x, &authentic_y))
      return false;
    const ArRenderPointF output = WorldNavigationAuthenticToOutput(
        slot, viewport, authentic_x, authentic_y);
    vertices[i] = (ArRenderVertex2D){
      {output.x, output.y},
      {1.0f, 1.0f, 1.0f, 1.0f},
      {scene->ground[i].texture_u, scene->ground[i].texture_v},
    };
  }
  static const int32_t indices[6] = {0, 1, 2, 0, 2, 3};
  if (!ArRenderDevice_DrawGeometry(
          &g_render_device, texture, vertices, 4, indices, 6)) {
    fprintf(stderr, "[world-navigation] ground draw failed: %s\n",
            ArRenderDevice_LastError(&g_render_device));
    return false;
  }
  return true;
}

static bool DrawWorldNavigationLightTreatment(
    const FrameSlot *slot, ArRenderRectI viewport) {
  if (!slot->sim.world_navigation_lighting) return true;
  const float elevation =
      (float)slot->sim.light_elevation_deg * kPi / 180.0f;
  const float low_sun = 1.0f - sinf(elevation);
  if (low_sun <= 0.001f) return true;

  /* The ground is a flat, top-down texture with one normal, so its diffuse
   * term is necessarily uniform. A restrained warm dusk grade makes that
   * physically honest limitation visible without pretending the painted map
   * has per-pixel normals. Azimuth remains meaningful for cloud shadows. */
  /* Preserve the original byte quantization before crossing the portable
   * float-color boundary. Otherwise every non-integral value would subtly
   * change the dusk treatment. */
  const uint8_t alpha_byte = (uint8_t)(low_sun * 72.0f + 0.5f);
  const float alpha = alpha_byte / 255.0f;
  const ArRenderRectF area = {
    (float)viewport.x, (float)viewport.y,
    (float)viewport.w, (float)viewport.h,
  };
  return ArRenderDevice_DrawSolidRect(
      &g_render_device, &area,
      (ArRenderColorF){42.0f / 255.0f, 24.0f / 255.0f,
                       12.0f / 255.0f, alpha},
      kArRenderBlendMode_Alpha);
}

/* The original label selector owns the clear 256x256 region. Outside it, the
 * already-downsampled world texture supplies depth blur and the shared haze
 * setting mixes the terrain toward the live scene backdrop. Both passes use
 * one affine mesh, so the boundary follows the scripted zoom and rotation. */
static bool DrawWorldNavigationActiveRegionHaze(
    const FrameSlot *slot, ArRenderRectI viewport) {
  const SimWorldNavigationScene *scene =
      &slot->sim.world_navigation_scene;
  if (!slot->sim.world_navigation_haze ||
      (!slot->sim.underlay_defocus_pct && !slot->sim.underlay_haze_pct))
    return true;

  enum {
    /* Four samples through each fade band preserve the smoothstep's zero
     * slope at the clear edge. Sampling only its endpoints and letting SDL
     * interpolate linearly leaves a visible rectangular crease. */
    kMaxAxis = 12,
    kMaxVertices = kMaxAxis * kMaxAxis,
    kMaxIndices = (kMaxAxis - 1) * (kMaxAxis - 1) * 6,
  };
  /* The town control is in its 2048px high-fidelity source space; navigation
   * uses a 1024px texture, so halve it before applying the same visual width. */
  const float lead = fmaxf(
      1.0f, (float)slot->sim.cull_haze_lead_px * 0.5f);
  const float x0 = scene->active_region_x;
  const float y0 = scene->active_region_y;
  const float x1 = x0 + scene->active_region_width;
  const float y1 = y0 + scene->active_region_height;
  float xs[kMaxAxis] = {0.0f, (float)kSimWorldMapPixels};
  float ys[kMaxAxis] = {0.0f, (float)kSimWorldMapPixels};
  int nx = 2, ny = 2;
  if (scene->active_region_valid) {
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x0 - lead);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x0 - lead * 0.75f);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x0 - lead * 0.50f);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x0 - lead * 0.25f);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x0);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x1);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x1 + lead * 0.25f);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x1 + lead * 0.50f);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x1 + lead * 0.75f);
    nx = InsertSimGroundCoordinate(xs, nx, kMaxAxis, x1 + lead);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y0 - lead);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y0 - lead * 0.75f);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y0 - lead * 0.50f);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y0 - lead * 0.25f);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y0);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y1);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y1 + lead * 0.25f);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y1 + lead * 0.50f);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y1 + lead * 0.75f);
    ny = InsertSimGroundCoordinate(ys, ny, kMaxAxis, y1 + lead);
  }

  ArRenderVertex2D vertices[kMaxVertices];
  int32_t indices[kMaxIndices];
  int vertex_count = 0, index_count = 0;
  for (int row = 0; row < ny; row++) {
    for (int column = 0; column < nx; column++) {
      float authentic_x = 0.0f, authentic_y = 0.0f;
      if (!SimWorldNavigationScene_ProjectSource(
              scene, xs[column], ys[row], &authentic_x, &authentic_y))
        return false;
      const float haze =
          SimWorldNavigationScene_LocationHaze(
              scene, xs[column], ys[row], lead);
      const ArRenderPointF output = WorldNavigationAuthenticToOutput(
          slot, viewport, authentic_x, authentic_y);
      vertices[vertex_count++] = (ArRenderVertex2D){
        {output.x, output.y},
        {1.0f, 1.0f, 1.0f,
         haze * (float)slot->sim.underlay_defocus_pct / (float)kPercentScale},
        {xs[column] / (float)kSimWorldMapPixels,
         ys[row] / (float)kSimWorldMapPixels},
      };
    }
  }
  for (int row = 0; row + 1 < ny; row++) {
    for (int column = 0; column + 1 < nx; column++) {
      const int tl = row * nx + column;
      indices[index_count++] = tl;
      indices[index_count++] = tl + 1;
      indices[index_count++] = tl + nx + 1;
      indices[index_count++] = tl;
      indices[index_count++] = tl + nx + 1;
      indices[index_count++] = tl + nx;
    }
  }

  ArRenderTexture blur = SimUnderlayBlurTexture(
      slot->sim.underlay_serial);
  if (slot->sim.underlay_defocus_pct &&
      ArRenderTexture_IsValid(blur)) {
    if (!ArRenderDevice_DrawGeometry(
            &g_render_device, blur, vertices, vertex_count,
            indices, index_count))
      return false;
  }

  if (slot->sim.underlay_haze_pct) {
    const uint32_t backdrop = slot->sim.separated_backdrop_argb;
    const float r = (float)((backdrop >> 16) & 0xFF) / 255.0f;
    const float g = (float)((backdrop >> 8) & 0xFF) / 255.0f;
    const float b = (float)(backdrop & 0xFF) / 255.0f;
    for (int i = 0; i < vertex_count; i++) {
      const float haze = SimWorldNavigationScene_LocationHaze(
          scene, vertices[i].tex_coord.x * kSimWorldMapPixels,
          vertices[i].tex_coord.y * kSimWorldMapPixels, lead);
      vertices[i].color = (ArRenderColorF){
        r, g, b,
        haze * (float)slot->sim.underlay_haze_pct / (float)kPercentScale,
      };
    }
    const ArRenderDrawState state = {
      .flags = kArRenderDrawState_Blend,
      .blend = kArRenderBlendMode_Alpha,
    };
    const bool ok = ArRenderDevice_DrawGeometryWithState(
        &g_render_device, ArRenderTexture_Invalid(), vertices, vertex_count,
        indices, index_count, &state);
    if (!ok) return false;
  }
  return true;
}

static bool DrawWorldNavigationCloudLayer(
    const FrameSlot *slot, ArRenderRectI viewport, ArRenderTexture texture,
    const SimCloudLayer *layer, uint64_t elapsed_ms, float drift,
    float source_offset_x, float source_offset_y, ArRenderColorF colour) {
  if (!layer) return false;
  const SimWorldNavigationScene *scene =
      &slot->sim.world_navigation_scene;
  float phase_u = layer->offset_x +
      Scene3D_WrappedTextureOffset(elapsed_ms, layer->drift_x, drift);
  float phase_v = layer->offset_y +
      Scene3D_WrappedTextureOffset(elapsed_ms, layer->drift_y, drift);
  phase_u -= floorf(phase_u);
  phase_v -= floorf(phase_v);
  enum {
    kPaddedPixels = kSimCloudTexturePixels * 2,
  };
  /* The texture contains two exact copies of the proven town cloud field in
   * each axis. Sample one complete copy through a single world quad and slide
   * the window through the padded copy. This retains wraparound drift on
   * backends that cannot wrap geometry UVs, without introducing
   * any manually tiled geometry edges. */
  const float u0 =
      (phase_u * kSimCloudTexturePixels + 0.5f) / kPaddedPixels;
  const float v0 =
      (phase_v * kSimCloudTexturePixels + 0.5f) / kPaddedPixels;
  const float u1 =
      (phase_u * kSimCloudTexturePixels +
       kSimCloudTexturePixels - 0.5f) / kPaddedPixels;
  const float v1 =
      (phase_v * kSimCloudTexturePixels +
       kSimCloudTexturePixels - 0.5f) / kPaddedPixels;
  const float u[4] = {u0, u1, u1, u0};
  const float v[4] = {v0, v0, v1, v1};
  ArRenderVertex2D vertices[4];
  for (int i = 0; i < 4; i++) {
    const float source_x =
        (float)scene->ground[i].tile_x * kSimWorldMapTilePixels;
    const float source_y =
        (float)scene->ground[i].tile_y * kSimWorldMapTilePixels;
    float authentic_x = 0.0f, authentic_y = 0.0f;
    if (!SimWorldNavigationScene_ProjectSource(
            scene, source_x + source_offset_x,
            source_y + source_offset_y, &authentic_x, &authentic_y))
      return false;
    const ArRenderPointF output = WorldNavigationAuthenticToOutput(
        slot, viewport, authentic_x, authentic_y);
    vertices[i] = (ArRenderVertex2D){
      {output.x, output.y},
      colour,
      {u[i], v[i]},
    };
  }
  static const int32_t indices[6] = {0, 1, 2, 0, 2, 3};
  return ArRenderDevice_DrawGeometry(
      &g_render_device, texture, vertices, 4, indices, 6);
}

static PresentationOutcome OmitWorldNavigationWeather(const char *reason) {
  if (!s_world_navigation_weather_failure_reported) {
    s_world_navigation_weather_failure_reported = true;
    fprintf(stderr, "[world-navigation] optional weather omitted: %s\n",
            reason && reason[0] ? reason : "renderer rejected the effect");
  }
  return kPresentationOutcome_OptionalOmitted;
}

static PresentationOutcome DrawWorldNavigationWeather(
    const FrameSlot *slot, ArRenderRectI viewport) {
  if (!slot->sim.world_navigation_clouds ||
      !slot->sim.cloud_opacity_pct)
    return kPresentationOutcome_Complete;
  ArRenderTexture texture = EnsureWorldNavigationCloudTexture();
  if (!ArRenderTexture_IsValid(texture))
    return OmitWorldNavigationWeather(
        ArRenderDevice_LastError(&g_render_device));

  const float opacity =
      (float)slot->sim.cloud_opacity_pct / (float)kPercentScale;
  const float body_visibility = SimWorldNavigationScene_CloudVisibility(
      slot->sim.world_navigation.zoom_current,
      slot->sim.cloud_altitude_px);
  const float drift =
      (float)slot->sim.cloud_drift_pct / (float)kPercentScale;
  const uint64_t elapsed_ms = HostClock_Milliseconds();

  /* A cloud's altitude is invisible to an orthographic top-down camera until
   * it casts a displaced shadow. Reuse the town light's world-space shear so
   * the shadow rotates and zooms with the scripted Mode-7 event. The
   * procedural alpha already supplies a soft edge; the softness dial spreads
   * three low-alpha samples across the light-perpendicular axis. */
  if (slot->sim.world_navigation_lighting &&
      slot->sim.shadow_opacity_pct) {
    float light_x = 0.0f, light_y = 0.0f;
    SimShadowLight(slot, &light_x, &light_y);
    const float shadow_x =
        light_x * (float)slot->sim.cloud_altitude_px;
    const float shadow_y =
        light_y * (float)slot->sim.cloud_altitude_px;
    const float blur =
        (float)slot->sim.shadow_softness_pct * 0.08f;
    const int sample_count = blur > 0.01f ? 3 : 1;
    const float perpendicular_x = -sinf(
        (float)slot->sim.light_azimuth_deg * kPi / 180.0f);
    const float perpendicular_y = cosf(
        (float)slot->sim.light_azimuth_deg * kPi / 180.0f);
    for (unsigned layer_index = 0;
         layer_index <
             (size_t)kSimCloudLayerCount;
         layer_index++) {
      const SimCloudLayer *layer = &kSimCloudLayers[layer_index];
      for (int sample = 0; sample < sample_count; sample++) {
        const float spread =
            sample_count == 1 ? 0.0f : (float)(sample - 1) * blur;
        const float sample_weight =
            sample_count == 1 ? 1.0f : sample == 1 ? 0.5f : 0.25f;
        const float alpha = opacity * layer->weight *
            ((float)slot->sim.shadow_opacity_pct / (float)kPercentScale) *
            0.35f * sample_weight;
        if (!DrawWorldNavigationCloudLayer(
                slot, viewport, texture, layer, elapsed_ms, drift,
                shadow_x + perpendicular_x * spread,
                shadow_y + perpendicular_y * spread,
                (ArRenderColorF){0.0f, 0.0f, 0.0f, alpha}))
          return OmitWorldNavigationWeather(
              ArRenderDevice_LastError(&g_render_device));
      }
    }
  }

  if (body_visibility > 0.001f) {
    for (unsigned layer_index = 0;
         layer_index < (size_t)kSimCloudLayerCount;
         layer_index++) {
      const SimCloudLayer *layer = &kSimCloudLayers[layer_index];
      if (!DrawWorldNavigationCloudLayer(
              slot, viewport, texture, layer, elapsed_ms, drift, 0.0f, 0.0f,
              (ArRenderColorF){
                1.0f, 1.0f, 1.0f,
                opacity * layer->weight * body_visibility,
              })) {
        return OmitWorldNavigationWeather(
            ArRenderDevice_LastError(&g_render_device));
      }
    }
  }
  return kPresentationOutcome_Complete;
}

static bool DrawWorldNavigationCompositionLayer(
    const FrameSlot *slot, ArRenderRectI viewport,
    const SimWorldNavigationCompositionLayer *layer,
    ArRenderTexture texture) {
  if (!layer || !layer->visible) return true;
  if (!ArRenderTexture_IsValid(texture) ||
      !layer->width || !layer->height)
    return false;
  const ArRenderPointF top_left = WorldNavigationAuthenticToOutput(
      slot, viewport, layer->screen_x, layer->screen_y);
  const ArRenderPointF bottom_right = WorldNavigationAuthenticToOutput(
      slot, viewport, layer->screen_x + layer->width,
      layer->screen_y + layer->height);
  ArRenderRectF source = {0.0f, 0.0f, layer->width, layer->height};
  ArRenderRectF destination = {
    top_left.x, top_left.y,
    bottom_right.x - top_left.x,
    bottom_right.y - top_left.y,
  };
  return ArRenderDevice_DrawTexture(
      &g_render_device, texture, &source, &destination);
}

static bool DrawWorldNavigationMasterFade(
    const FrameSlot *slot, ArRenderRectI viewport) {
  const uint8_t alpha = SimWorldNavigationScene_MasterFadeAlpha(
      slot->sim.world_navigation_brightness);
  if (!alpha) return true;
  const ArRenderRectF area = {
    (float)viewport.x, (float)viewport.y,
    (float)viewport.w, (float)viewport.h,
  };
  return ArRenderDevice_DrawSolidRect(
      &g_render_device, &area,
      (ArRenderColorF){0.0f, 0.0f, 0.0f, alpha / 255.0f},
      kArRenderBlendMode_Alpha);
}

PresentationOutcome PresentWorldNavigation3D(const FrameSlot *slot) {
  PresentationOutcome outcome = kPresentationOutcome_Complete;
  const SimWorldNavigationScene *scene =
      &slot->sim.world_navigation_scene;
  const SimWorldNavigationComposition *composition = &scene->composition;
  if (!scene->valid || !composition->valid ||
      !s_world_navigation_composition_upload_valid)
    return kPresentationOutcome_CoreFailure;
  ArRenderTexture world = EnsureSimUnderlayTexture(slot);
  if (!ArRenderTexture_IsValid(world))
    return kPresentationOutcome_CoreFailure;
  if (!composition->empty_animation &&
      (!ArRenderTexture_IsValid(s_world_navigation_palace_texture) ||
       !ArRenderTexture_IsValid(s_world_navigation_ui_texture)))
    return kPresentationOutcome_CoreFailure;

  const int aspect_width = slot->visible_width *
      (slot->pixel_aspect == kPixelAspect_Crt43 ? 7 : 1);
  const int aspect_height = slot->snes_height *
      (slot->pixel_aspect == kPixelAspect_Crt43 ? 6 : 1);
  const ArRenderColorF black = {0.0f, 0.0f, 0.0f, 1.0f};
  ArRenderOutputFrame output_frame;
  if (!ArRenderOutputFrame_BeginAspectFit(
          &g_render_device, slot->ignore_aspect_ratio,
          aspect_width, aspect_height, black, black, &output_frame))
    return kPresentationOutcome_CoreFailure;
  const ArRenderRectI viewport = {
    0, 0, output_frame.viewport.w, output_frame.viewport.h,
  };
  if (slot->sim.world_navigation_backdrop)
    DrawSimBackdrop(slot, viewport, NULL);
  if (!DrawWorldNavigationGround(slot, viewport, world)) {
    ArRenderOutputFrame_Abort(&output_frame);
    return kPresentationOutcome_CoreFailure;
  }
  if (!DrawWorldNavigationLightTreatment(slot, viewport)) {
    ArRenderOutputFrame_Abort(&output_frame);
    return kPresentationOutcome_CoreFailure;
  }
  if (!DrawWorldNavigationActiveRegionHaze(slot, viewport)) {
    ArRenderOutputFrame_Abort(&output_frame);
    return kPresentationOutcome_CoreFailure;
  }

  /* Whole-world weather uses the same affine map as the ground. It has no
   * town sprite-window hole or cull boundary: every part of this world is
   * intentional content. The authentic top-down Palace stays over it, and
   * the location label/frame stays screen-space and last. */
  outcome = PresentationOutcome_Combine(
      outcome, DrawWorldNavigationWeather(slot, viewport));
  /* INIDISP is a master brightness applied after the PPU has composed every
   * layer. Do the same for the host-owned world and all its effects. The
   * Palace/UI captures are drawn afterward because PpuRasterizeObjRange has
   * already applied this frame's brightness to their pixels. */
  if (!DrawWorldNavigationMasterFade(slot, viewport)) {
    ArRenderOutputFrame_Abort(&output_frame);
    return kPresentationOutcome_CoreFailure;
  }
  if (!composition->empty_animation &&
      (!DrawWorldNavigationCompositionLayer(
           slot, viewport, &composition->palace,
           s_world_navigation_palace_texture) ||
       !DrawWorldNavigationCompositionLayer(
           slot, viewport, &composition->ui,
           s_world_navigation_ui_texture))) {
    ArRenderOutputFrame_Abort(&output_frame);
    return kPresentationOutcome_CoreFailure;
  }
  if (!ArRenderOutputFrame_Finish(&output_frame))
    return kPresentationOutcome_CoreFailure;
  return outcome;
}

/* The world-map half of the presentation-resource reset. PresentSim3D_ResetResources
 * keeps the town half and calls this; see the comment on
 * PresentRendererResources_Reset in present.c for why any of it exists. */
void PresentWorldNav_ResetResources(void) {
  ArRenderDevice_DestroyTexture(
      &g_render_device, s_world_navigation_cloud_texture);
  s_world_navigation_cloud_texture = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(
      &g_render_device, s_world_navigation_palace_texture);
  s_world_navigation_palace_texture = ArRenderTexture_Invalid();
  ArRenderDevice_DestroyTexture(
      &g_render_device, s_world_navigation_ui_texture);
  s_world_navigation_ui_texture = ArRenderTexture_Invalid();
  s_world_navigation_composition_upload_valid = false;
  s_world_navigation_cloud_unavailable = false;
  s_world_navigation_weather_failure_reported = false;
}
