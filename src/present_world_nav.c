/* The world-map navigation renderer: the 3D sky/ground/weather treatment shown
 * while travelling between towns, split verbatim out of present_sim3d.c.
 *
 * Split out for isolation rather than for size. This is the one renderer in the
 * present family with NO automated render coverage -- no sim3d checkpoint sets
 * AR_SIM3D_WORLD_NAV and no staged replay in saves/ reaches world-map travel --
 * so keeping it in its own translation unit makes that gap structural and
 * visible instead of a footnote. Staging a world-map SRAM seed + replay and
 * adding a checkpoint is the missing regression asset; see docs/code-style.md.
 *
 * The D6 no-live-globals invariant holds here as everywhere in the present
 * family: no g_ppu, no g_settings, no Settings_Visible*(). State arrives via
 * the `const FrameSlot *`. */

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "present.h"
#include "action/action_effect_render.h"
#include "constants.h"
#include "types.h"
#include "diorama/diorama.h"
#include "scene3d_math.h"
#include "sim/sim_world_map.h"
#include "sim/sim_world_navigation_capture.h"
#include "sim/sim3d.h"

/* kPixelAspect_Crt43 and kDioramaCam_Free/kDioramaCam_Dynamic are plain enum
 * constants (not live state) — fine to pull in just for those. */
#include "settings.h"
#include "present_internal.h"


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
#include "present_sim3d_internal.h"


static SDL_Texture *s_world_navigation_palace_texture;
static SDL_Texture *s_world_navigation_ui_texture;
static bool s_world_navigation_composition_alloc_failed;
static bool s_world_navigation_composition_upload_valid;

static SDL_Texture *EnsureWorldNavigationCompositionTexture(
    SDL_Texture **texture) {
  if (*texture || s_world_navigation_composition_alloc_failed)
    return *texture;
  *texture = SDL_CreateTexture(
      g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kSimWorldNavigationCompositionWidth,
      kSimWorldNavigationCompositionHeight);
  if (!*texture) {
    s_world_navigation_composition_alloc_failed = true;
    fprintf(stderr,
            "[world-navigation] composition texture unavailable: %s\n",
            SDL_GetError());
    return NULL;
  }
  SDL_SetTextureBlendMode(*texture, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(*texture, SDL_SCALEMODE_NEAREST);
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

  SDL_Texture *palace = EnsureWorldNavigationCompositionTexture(
      &s_world_navigation_palace_texture);
  SDL_Texture *ui = EnsureWorldNavigationCompositionTexture(
      &s_world_navigation_ui_texture);
  if (!palace || !ui) return;
  SDL_Rect palace_rect = {
    0, 0, composition->palace.width, composition->palace.height,
  };
  SDL_Rect ui_rect = {
    0, 0, composition->ui.width, composition->ui.height,
  };
  if (!SDL_UpdateTexture(
          palace, &palace_rect, g_sim_world_navigation_palace_pixels,
          kSimWorldNavigationCompositionPitch) ||
      !SDL_UpdateTexture(
          ui, &ui_rect, g_sim_world_navigation_ui_pixels,
          kSimWorldNavigationCompositionPitch))
    return;
  s_world_navigation_composition_upload_valid = true;
}
static SDL_Texture *s_world_navigation_cloud_texture;
static bool s_world_navigation_cloud_alloc_failed;

static SDL_Texture *EnsureWorldNavigationCloudTexture(void) {
  enum {
    kPaddedPixels = kSimCloudTexturePixels * 2,
  };
  if (s_world_navigation_cloud_texture ||
      s_world_navigation_cloud_alloc_failed)
    return s_world_navigation_cloud_texture;
  s_world_navigation_cloud_texture = SDL_CreateTexture(
      g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kPaddedPixels, kPaddedPixels);
  if (!s_world_navigation_cloud_texture) {
    s_world_navigation_cloud_alloc_failed = true;
    fprintf(stderr,
            "[world-navigation] cloud texture unavailable: %s\n",
            SDL_GetError());
    return NULL;
  }
  SDL_SetTextureBlendMode(s_world_navigation_cloud_texture,
                          SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(s_world_navigation_cloud_texture,
                          SDL_SCALEMODE_LINEAR);
  void *pixels = NULL;
  int pitch = 0;
  if (!SDL_LockTexture(
          s_world_navigation_cloud_texture, NULL, &pixels, &pitch)) {
    SDL_DestroyTexture(s_world_navigation_cloud_texture);
    s_world_navigation_cloud_texture = NULL;
    s_world_navigation_cloud_alloc_failed = true;
    return NULL;
  }
  for (int y = 0; y < kPaddedPixels; y++) {
    uint32_t *row = (uint32_t *)((uint8_t *)pixels + (size_t)y * pitch);
    for (int x = 0; x < kPaddedPixels; x++)
      row[x] = SimCloudTexel(
          x % kSimCloudTexturePixels, y % kSimCloudTexturePixels);
  }
  SDL_UnlockTexture(s_world_navigation_cloud_texture);
  return s_world_navigation_cloud_texture;
}

/* $09 full-world presentation.
 *
 * Navigation uses the same developed texture cache as the town underlay, but
 * none of the underlay geometry: the complete map is the primary plane and
 * its captured Mode-7 matrix is already an affine top-down camera. */
static SDL_FPoint WorldNavigationAuthenticToOutput(
    const FrameSlot *slot, SDL_Rect viewport,
    float authentic_x, float authentic_y) {
  const float authentic_x0 =
      ((float)slot->snes_width -
       (float)kSimWorldNavigationCompositionWidth) * 0.5f;
  const float captured_x = authentic_x0 + authentic_x;
  return (SDL_FPoint){
    (float)viewport.x +
        (captured_x - (float)slot->visible_x0) *
            (float)viewport.w / (float)slot->visible_width,
    (float)viewport.y + authentic_y *
        (float)viewport.h / (float)slot->snes_height,
  };
}

static bool DrawWorldNavigationGround(
    const FrameSlot *slot, SDL_Rect viewport, SDL_Texture *texture) {
  const SimWorldNavigationScene *scene =
      &slot->sim.world_navigation_scene;
  if (!texture || !scene->valid || slot->visible_width <= 0 ||
      slot->snes_height <= 0)
    return false;

  SDL_Vertex vertices[4];
  for (int i = 0; i < 4; i++) {
    float authentic_x = 0.0f, authentic_y = 0.0f;
    const float source_x =
        (float)scene->ground[i].tile_x * kSimWorldMapTilePixels;
    const float source_y =
        (float)scene->ground[i].tile_y * kSimWorldMapTilePixels;
    if (!SimWorldNavigationScene_ProjectSource(
            scene, source_x, source_y, &authentic_x, &authentic_y))
      return false;
    vertices[i] = (SDL_Vertex){
      WorldNavigationAuthenticToOutput(
          slot, viewport, authentic_x, authentic_y),
      {1.0f, 1.0f, 1.0f, 1.0f},
      {scene->ground[i].texture_u, scene->ground[i].texture_v},
    };
  }
  static const int indices[6] = {0, 1, 2, 0, 2, 3};
  SDL_SetTextureColorMod(texture, 255, 255, 255);
  SDL_SetTextureAlphaMod(texture, 255);
  if (!SDL_RenderGeometry(g_renderer, texture, vertices, 4, indices, 6)) {
    fprintf(stderr, "[world-navigation] ground draw failed: %s\n",
            SDL_GetError());
    return false;
  }
  return true;
}

static bool DrawWorldNavigationLightTreatment(
    const FrameSlot *slot, SDL_Rect viewport) {
  if (!slot->sim.world_navigation_lighting) return true;
  const float elevation =
      (float)slot->sim.light_elevation_deg * kPi / 180.0f;
  const float low_sun = 1.0f - sinf(elevation);
  if (low_sun <= 0.001f) return true;

  /* The ground is a flat, top-down texture with one normal, so its diffuse
   * term is necessarily uniform. A restrained warm dusk grade makes that
   * physically honest limitation visible without pretending the painted map
   * has per-pixel normals. Azimuth remains meaningful for cloud shadows. */
  const Uint8 alpha = (Uint8)(low_sun * 72.0f + 0.5f);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(g_renderer, 42, 24, 12, alpha);
  const SDL_FRect area = ToFRect(viewport);
  const bool ok = SDL_RenderFillRect(g_renderer, &area);
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
  return ok;
}

/* The original label selector owns the clear 256x256 region. Outside it, the
 * already-downsampled world texture supplies depth blur and the shared haze
 * setting mixes the terrain toward the live scene backdrop. Both passes use
 * one affine mesh, so the boundary follows the scripted zoom and rotation. */
static bool DrawWorldNavigationActiveRegionHaze(
    const FrameSlot *slot, SDL_Rect viewport) {
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

  SDL_Vertex vertices[kMaxVertices];
  int indices[kMaxIndices];
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
      vertices[vertex_count++] = (SDL_Vertex){
        WorldNavigationAuthenticToOutput(
            slot, viewport, authentic_x, authentic_y),
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

  if (slot->sim.underlay_defocus_pct && s_sim_underlay_blur_texture) {
    SDL_SetTextureColorMod(s_sim_underlay_blur_texture, 255, 255, 255);
    SDL_SetTextureAlphaMod(s_sim_underlay_blur_texture, 255);
    if (!SDL_RenderGeometry(
            g_renderer, s_sim_underlay_blur_texture, vertices, vertex_count,
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
      vertices[i].color = (SDL_FColor){
        r, g, b,
        haze * (float)slot->sim.underlay_haze_pct / (float)kPercentScale,
      };
    }
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    const bool ok = SDL_RenderGeometry(
        g_renderer, NULL, vertices, vertex_count, indices, index_count);
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
    if (!ok) return false;
  }
  return true;
}

static bool DrawWorldNavigationCloudLayer(
    const FrameSlot *slot, SDL_Rect viewport, SDL_Texture *texture,
    const SimCloudLayer *layer, Uint64 elapsed_ms, float drift,
    float source_offset_x, float source_offset_y, SDL_FColor colour) {
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
   * backends that cannot wrap SDL_RenderGeometry UVs, without introducing
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
  SDL_Vertex vertices[4];
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
    vertices[i] = (SDL_Vertex){
      WorldNavigationAuthenticToOutput(
          slot, viewport, authentic_x, authentic_y),
      colour,
      {u[i], v[i]},
    };
  }
  static const int indices[6] = {0, 1, 2, 0, 2, 3};
  return SDL_RenderGeometry(g_renderer, texture, vertices, 4, indices, 6);
}

static bool DrawWorldNavigationWeather(
    const FrameSlot *slot, SDL_Rect viewport) {
  if (!slot->sim.world_navigation_clouds ||
      !slot->sim.cloud_opacity_pct)
    return true;
  SDL_Texture *texture = EnsureWorldNavigationCloudTexture();
  if (!texture) return false;

  const float opacity =
      (float)slot->sim.cloud_opacity_pct / (float)kPercentScale;
  const float body_visibility = SimWorldNavigationScene_CloudVisibility(
      slot->sim.world_navigation.zoom_current,
      slot->sim.cloud_altitude_px);
  const float drift =
      (float)slot->sim.cloud_drift_pct / (float)kPercentScale;
  const Uint64 elapsed_ms = SDL_GetTicks();
  SDL_SetTextureColorMod(texture, 255, 255, 255);
  SDL_SetTextureAlphaMod(texture, 255);

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
                (SDL_FColor){0.0f, 0.0f, 0.0f, alpha}))
          return false;
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
              (SDL_FColor){
                1.0f, 1.0f, 1.0f,
                opacity * layer->weight * body_visibility,
              })) {
        return false;
      }
    }
  }
  return true;
}

static bool DrawWorldNavigationCompositionLayer(
    const FrameSlot *slot, SDL_Rect viewport,
    const SimWorldNavigationCompositionLayer *layer,
    SDL_Texture *texture) {
  if (!layer || !layer->visible) return true;
  if (!texture || !layer->width || !layer->height) return false;
  const SDL_FPoint top_left = WorldNavigationAuthenticToOutput(
      slot, viewport, layer->screen_x, layer->screen_y);
  const SDL_FPoint bottom_right = WorldNavigationAuthenticToOutput(
      slot, viewport, layer->screen_x + layer->width,
      layer->screen_y + layer->height);
  SDL_FRect source = {0.0f, 0.0f, layer->width, layer->height};
  SDL_FRect destination = {
    top_left.x, top_left.y,
    bottom_right.x - top_left.x,
    bottom_right.y - top_left.y,
  };
  return SDL_RenderTexture(g_renderer, texture, &source, &destination);
}

static bool DrawWorldNavigationMasterFade(
    const FrameSlot *slot, SDL_Rect viewport) {
  const uint8_t alpha = SimWorldNavigationScene_MasterFadeAlpha(
      slot->sim.world_navigation_brightness);
  if (!alpha) return true;
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, alpha);
  const bool ok = SDL_RenderFillRect(g_renderer, &(SDL_FRect){
      (float)viewport.x, (float)viewport.y,
      (float)viewport.w, (float)viewport.h});
  SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);
  return ok;
}

bool PresentWorldNavigation3D(const FrameSlot *slot) {
  const SimWorldNavigationScene *scene =
      &slot->sim.world_navigation_scene;
  const SimWorldNavigationComposition *composition = &scene->composition;
  if (!scene->valid || !composition->valid ||
      !s_world_navigation_composition_upload_valid)
    return false;
  SDL_Texture *world = EnsureSimUnderlayTexture(slot);
  if (!world) return false;
  if (!composition->empty_animation &&
      (!s_world_navigation_palace_texture ||
       !s_world_navigation_ui_texture))
    return false;

  SDL_Rect viewport = ComputePresentationViewport(
      g_renderer, slot->ignore_aspect_ratio,
      slot->pixel_aspect, slot->visible_width, slot->snes_height);
  SDL_SetRenderLogicalPresentation(g_renderer, 0, 0,
                                   SDL_LOGICAL_PRESENTATION_DISABLED);
  SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
  SDL_RenderClear(g_renderer);
  SDL_SetRenderClipRect(g_renderer, &viewport);
  if (slot->sim.world_navigation_backdrop)
    DrawSimBackdrop(slot, viewport, NULL);
  if (!DrawWorldNavigationGround(slot, viewport, world)) {
    SDL_SetRenderClipRect(g_renderer, NULL);
    ApplyLogicalPresentation(slot);
    return false;
  }
  if (!DrawWorldNavigationLightTreatment(slot, viewport)) {
    SDL_SetRenderClipRect(g_renderer, NULL);
    ApplyLogicalPresentation(slot);
    return false;
  }
  if (!DrawWorldNavigationActiveRegionHaze(slot, viewport)) {
    SDL_SetRenderClipRect(g_renderer, NULL);
    ApplyLogicalPresentation(slot);
    return false;
  }

  /* Whole-world weather uses the same affine map as the ground. It has no
   * town sprite-window hole or cull boundary: every part of this world is
   * intentional content. The authentic top-down Palace stays over it, and
   * the location label/frame stays screen-space and last. */
  if (!DrawWorldNavigationWeather(slot, viewport)) {
    SDL_SetRenderClipRect(g_renderer, NULL);
    ApplyLogicalPresentation(slot);
    return false;
  }
  /* INIDISP is a master brightness applied after the PPU has composed every
   * layer. Do the same for the host-owned world and all its effects. The
   * Palace/UI captures are drawn afterward because PpuRasterizeObjRange has
   * already applied this frame's brightness to their pixels. */
  if (!DrawWorldNavigationMasterFade(slot, viewport)) {
    SDL_SetRenderClipRect(g_renderer, NULL);
    ApplyLogicalPresentation(slot);
    return false;
  }
  if (!composition->empty_animation &&
      (!DrawWorldNavigationCompositionLayer(
           slot, viewport, &composition->palace,
           s_world_navigation_palace_texture) ||
       !DrawWorldNavigationCompositionLayer(
           slot, viewport, &composition->ui,
           s_world_navigation_ui_texture))) {
    SDL_SetRenderClipRect(g_renderer, NULL);
    ApplyLogicalPresentation(slot);
    return false;
  }
  SDL_SetRenderClipRect(g_renderer, NULL);
  ApplyLogicalPresentation(slot);
  return true;
}

/* The world-map half of the presentation-resource reset. PresentSim3D_ResetResources
 * keeps the town half and calls this; see the comment on
 * PresentRendererResources_Reset in present.c for why any of it exists. */
void PresentWorldNav_ResetResources(void) {
  if (s_world_navigation_cloud_texture)
    SDL_DestroyTexture(s_world_navigation_cloud_texture);
  s_world_navigation_cloud_texture = NULL;
  s_world_navigation_cloud_alloc_failed = false;
  if (s_world_navigation_palace_texture)
    SDL_DestroyTexture(s_world_navigation_palace_texture);
  s_world_navigation_palace_texture = NULL;
  if (s_world_navigation_ui_texture)
    SDL_DestroyTexture(s_world_navigation_ui_texture);
  s_world_navigation_ui_texture = NULL;
  s_world_navigation_composition_alloc_failed = false;
  s_world_navigation_composition_upload_valid = false;
}
