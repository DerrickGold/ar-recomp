#include "diorama_frame_generation.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "present.h"
#include "presentation_geometry.h"
#include "presentation_frame_generation.h"

enum {
  kFrameGenerationMaximumPairSpanMs = 50,
  kFrameGenerationTextureBytes =
      kFrameSlotLayerTextureWidth * kFrameSlotLayerTextureHeight *
      (int)sizeof(uint32_t),
  kFrameGenerationMaximumVertices =
      (kPresentationFrameGenerationMaximumBlocksX + 1) *
      (kPresentationFrameGenerationMaximumBlocksY + 1),
  kFrameGenerationMaximumIndices =
      kPresentationFrameGenerationMaximumBlocks * 6,
};

typedef struct DioramaFrameGenerationPlane {
  uint32_t *previous_pixels;
  uint32_t *current_pixels;
  SDL_Texture *previous_texture;
  SDL_Texture *current_texture;
  SDL_Texture *generated_texture;
  int output_x;
  int texture_width;
  int texture_height;
  PresentationFrameGenerationMotionField motion;
  bool current_valid;
  bool pair_valid;
} DioramaFrameGenerationPlane;

typedef struct DioramaFrameGenerationKey {
  uint64_t timestamp_ns;
  uint32_t plane_mask;
  uint32_t additive_plane_mask;
  int width;
  int height;
  uint8_t bg_mode;
  uint8_t map_group;
  uint8_t map_number;
  uint8_t layer_section;
  bool valid;
} DioramaFrameGenerationKey;

static DioramaFrameGenerationPlane s_planes[kDioramaPlane_Count];
static DioramaFrameGenerationKey s_last_key;
static uint64_t s_pair_timestamp_ns;
static uint32_t s_pair_mask;
static SDL_Vertex s_vertices[kFrameGenerationMaximumVertices];
static int s_indices[kFrameGenerationMaximumIndices];
static int s_index_blocks_x = -1;
static int s_index_blocks_y = -1;
static const int kQuadIndices[] = {0, 1, 2, 1, 3, 2};

_Static_assert(
    kFrameSlotLayerTextureWidth <=
        kPresentationFrameGenerationMaximumWidth,
    "capture texture width exceeds frame-generation capacity");
_Static_assert(
    kFrameSlotLayerTextureHeight <=
        kPresentationFrameGenerationMaximumHeight,
    "capture texture height exceeds frame-generation capacity");

static void DestroyPlaneTextures(DioramaFrameGenerationPlane *plane) {
  SDL_DestroyTexture(plane->previous_texture);
  SDL_DestroyTexture(plane->current_texture);
  SDL_DestroyTexture(plane->generated_texture);
  plane->previous_texture = NULL;
  plane->current_texture = NULL;
  plane->generated_texture = NULL;
  plane->output_x = 0;
  plane->texture_width = 0;
  plane->texture_height = 0;
}

void DioramaFrameGeneration_Reset(void) {
  for (int plane = 0; plane < kDioramaPlane_Count; plane++) {
    DestroyPlaneTextures(&s_planes[plane]);
    s_planes[plane].current_valid = false;
    s_planes[plane].pair_valid = false;
    memset(&s_planes[plane].motion, 0, sizeof(s_planes[plane].motion));
  }
  memset(&s_last_key, 0, sizeof(s_last_key));
  s_pair_timestamp_ns = 0;
  s_pair_mask = 0;
  s_index_blocks_x = -1;
  s_index_blocks_y = -1;
}

void DioramaFrameGeneration_Shutdown(void) {
  DioramaFrameGeneration_Reset();
  for (int plane = 0; plane < kDioramaPlane_Count; plane++) {
    free(s_planes[plane].previous_pixels);
    free(s_planes[plane].current_pixels);
    s_planes[plane].previous_pixels = NULL;
    s_planes[plane].current_pixels = NULL;
  }
}

static bool EnsurePlaneBuffers(DioramaFrameGenerationPlane *plane) {
  if (!plane->previous_pixels)
    plane->previous_pixels = malloc(kFrameGenerationTextureBytes);
  if (!plane->current_pixels)
    plane->current_pixels = malloc(kFrameGenerationTextureBytes);
  return plane->previous_pixels && plane->current_pixels;
}

static SDL_Texture *CreatePlaneTexture(SDL_Renderer *renderer,
                                       SDL_TextureAccess access,
                                       int width, int height, int plane) {
  SDL_Texture *texture = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888,
      access, width, height);
  if (!texture) return NULL;
  if (!SDL_SetTextureScaleMode(
          texture, access == SDL_TEXTUREACCESS_TARGET
              ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR) ||
      !SDL_SetTextureBlendMode(
          texture, plane == kDioramaPlane_Backdrop
              ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND)) {
    SDL_DestroyTexture(texture);
    return NULL;
  }
  return texture;
}

static bool EnsurePlaneTextures(SDL_Renderer *renderer, int plane_index,
                                const DioramaPlaneCaptureRegion *region) {
  DioramaFrameGenerationPlane *plane = &s_planes[plane_index];
  if (plane->output_x != region->x ||
      plane->texture_width != region->width ||
      plane->texture_height != region->height)
    DestroyPlaneTextures(plane);
  if (!plane->previous_texture)
    plane->previous_texture =
        CreatePlaneTexture(renderer, SDL_TEXTUREACCESS_STREAMING,
                           region->width, region->height, plane_index);
  if (!plane->current_texture)
    plane->current_texture =
        CreatePlaneTexture(renderer, SDL_TEXTUREACCESS_STREAMING,
                           region->width, region->height, plane_index);
  if (!plane->generated_texture)
    plane->generated_texture =
        CreatePlaneTexture(renderer, SDL_TEXTUREACCESS_TARGET,
                           kFrameSlotLayerTextureWidth,
                           kFrameSlotLayerTextureHeight, plane_index);
  const bool ready = plane->previous_texture && plane->current_texture &&
      plane->generated_texture;
  if (ready) {
    plane->output_x = region->x;
    plane->texture_width = region->width;
    plane->texture_height = region->height;
  }
  return ready;
}

static bool KeysAreContinuous(const DioramaFrameGenerationKey *previous,
                              const DioramaFrameGenerationKey *current,
                              uint8_t capture_ticks) {
  if (!previous->valid || !capture_ticks ||
      current->timestamp_ns <= previous->timestamp_ns)
    return false;
  if (current->timestamp_ns - previous->timestamp_ns >=
      (uint64_t)kFrameGenerationMaximumPairSpanMs *
          kNanosecondsPerMillisecond)
    return false;
  return previous->width == current->width &&
      previous->height == current->height &&
      previous->bg_mode == current->bg_mode &&
      previous->map_group == current->map_group &&
      previous->map_number == current->map_number &&
      previous->layer_section == current->layer_section &&
      previous->additive_plane_mask == current->additive_plane_mask;
}

static void CopySurfaceRegion(
    uint32_t *destination, const uint8_t *source, size_t source_pitch_bytes,
    const DioramaPlaneCaptureRegion *region) {
  const size_t row_bytes = (size_t)region->width * sizeof(uint32_t);
  const size_t source_x_bytes = (size_t)region->x * sizeof(uint32_t);
  for (int y = 0; y < region->height; y++) {
    memcpy(&destination[(size_t)y * kFrameSlotLayerTextureWidth],
           source + (size_t)y * source_pitch_bytes + source_x_bytes,
           row_bytes);
  }
}

void DioramaFrameGeneration_Capture(
    SDL_Renderer *renderer, const FrameSlot *slot,
    const uint8_t *const pixels[kDioramaPlane_Count],
    const size_t pitch_bytes[kDioramaPlane_Count],
    uint32_t changed_plane_mask) {
  s_pair_timestamp_ns = 0;
  s_pair_mask = 0;
  if (!renderer || !slot || !pixels || !pitch_bytes ||
      !slot->diorama_active ||
      !slot->interp_setting_enabled || !slot->capture_ticks ||
      slot->turbo_active || (slot->inidisp & 0x80u) != 0) {
    s_last_key.valid = false;
    return;
  }

  const int width = slot->snes_width + slot->obj_apron * 2;
  const int height = slot->snes_height +
      slot->ws_extra_top + slot->ws_extra_bottom;
  const uint32_t plane_mask = slot->diorama_plane_request_mask &
      slot->diorama_plane_content_mask;
  DioramaFrameGenerationKey current = {
    .timestamp_ns = slot->timestamp_ns,
    .plane_mask = plane_mask,
    .additive_plane_mask = slot->diorama_plane_additive_mask,
    .width = width,
    .height = height,
    .bg_mode = slot->bg_mode,
    .map_group = slot->diorama_map_group,
    .map_number = slot->diorama_map_number,
    .layer_section = slot->diorama_layer_section,
    .valid = width > 0 &&
        width <= kFrameSlotLayerTextureWidth &&
        width <= kPresentationFrameGenerationMaximumWidth &&
        height > 0 &&
        height <= kFrameSlotLayerTextureHeight &&
        height <= kPresentationFrameGenerationMaximumHeight,
  };
  if (!current.valid) {
    s_last_key.valid = false;
    return;
  }

  const bool continuous =
      KeysAreContinuous(&s_last_key, &current, slot->capture_ticks);
  for (int plane_index = 0; plane_index < kDioramaPlane_Count;
       plane_index++) {
    DioramaFrameGenerationPlane *plane = &s_planes[plane_index];
    plane->pair_valid = false;
    if (!(plane_mask & (1u << plane_index)) || !pixels[plane_index] ||
        pitch_bytes[plane_index] < (size_t)width * sizeof(uint32_t)) {
      plane->current_valid = false;
      continue;
    }
    DioramaPlaneCaptureRegion region;
    if (!DioramaPlaneCaptureRegion_Resolve(
            plane_index, width, height, slot->obj_apron, &region)) {
      plane->current_valid = false;
      continue;
    }
    const bool region_matches =
        plane->output_x == region.x &&
        plane->texture_width == region.width &&
        plane->texture_height == region.height;
    /* The synchronized raw texture is already the exact current endpoint.
     * A byte-identical private pair cannot add an intermediate image, so keep
     * the retained endpoint in place and do no CPU copy, private upload, motion
     * search, or synthesis for this plane. Invalid private state still retries
     * even when the raw upload mirror reported no change. */
    if (!(changed_plane_mask & (1u << plane_index)) &&
        plane->current_valid && region_matches)
      continue;
    if (!EnsurePlaneBuffers(plane)) {
      plane->current_valid = false;
      continue;
    }

    uint32_t *swap = plane->previous_pixels;
    plane->previous_pixels = plane->current_pixels;
    plane->current_pixels = swap;
    const bool had_previous = plane->current_valid && continuous &&
        region_matches && (s_last_key.plane_mask & (1u << plane_index));
    CopySurfaceRegion(
        plane->current_pixels, pixels[plane_index],
        pitch_bytes[plane_index], &region);
    if (!EnsurePlaneTextures(renderer, plane_index, &region)) {
      plane->current_valid = false;
      continue;
    }

    /* Keep both private endpoints capture-sized. Linear warping can then
     * sample their physical texture edge without touching the stale padding
     * carried by the fixed-size compositor textures. Swapping means this is
     * still one endpoint upload per captured plane, not two. */
    SDL_Texture *texture_swap = plane->previous_texture;
    plane->previous_texture = plane->current_texture;
    plane->current_texture = texture_swap;
    if (!SDL_UpdateTexture(
            plane->current_texture, NULL, plane->current_pixels,
            kFrameSlotLayerTextureWidth * (int)sizeof(uint32_t))) {
      plane->current_valid = false;
      continue;
    }
    plane->current_valid = true;
    if (!had_previous) continue;
    const PresentationFrameGenerationAnalysisMode mode =
        DioramaPlaneIsObjectPriority(plane_index)
            ? kPresentationFrameGenerationAnalysis_Blocks
            : kPresentationFrameGenerationAnalysis_Global;
    plane->pair_valid = PresentationFrameGeneration_Analyze(
        plane->previous_pixels, plane->current_pixels,
        region.width, region.height,
        kFrameSlotLayerTextureWidth, kFrameSlotLayerTextureWidth,
        mode, &plane->motion);
    if (plane->pair_valid) s_pair_mask |= 1u << plane_index;
  }
  s_pair_timestamp_ns = s_pair_mask ? slot->timestamp_ns : 0;
  s_last_key = current;
}

static bool EnsureBlockIndices(int blocks_x, int blocks_y) {
  if (blocks_x == s_index_blocks_x && blocks_y == s_index_blocks_y)
    return true;
  const int index_count = blocks_x * blocks_y * 6;
  if (blocks_x <= 0 || blocks_y <= 0 ||
      index_count > kFrameGenerationMaximumIndices)
    return false;
  const int columns = blocks_x + 1;
  int index = 0;
  for (int row = 0; row < blocks_y; row++) {
    for (int column = 0; column < blocks_x; column++) {
      const int top_left = row * columns + column;
      s_indices[index++] = top_left;
      s_indices[index++] = top_left + 1;
      s_indices[index++] = top_left + columns;
      s_indices[index++] = top_left + 1;
      s_indices[index++] = top_left + columns + 1;
      s_indices[index++] = top_left + columns;
    }
  }
  s_index_blocks_x = blocks_x;
  s_index_blocks_y = blocks_y;
  return true;
}

static bool BuildMesh(
    const PresentationFrameGenerationMotionField *motion,
    int output_x, bool forward, float amount, int *out_vertices,
    const int **out_indices, int *out_index_count) {
  const int columns = motion->uniform ? 2 : motion->blocks_x + 1;
  const int rows = motion->uniform ? 2 : motion->blocks_y + 1;
  const int vertex_count = columns * rows;
  if (vertex_count > kFrameGenerationMaximumVertices)
    return false;

  int vertex = 0;
  for (int row = 0; row < rows; row++) {
    const int source_y = motion->uniform
        ? row * motion->height
        : row == motion->blocks_y
            ? motion->height
            : row * kPresentationFrameGenerationBlockSize;
    for (int column = 0; column < columns; column++) {
      const int source_x = motion->uniform
          ? column * motion->width
          : column == motion->blocks_x
              ? motion->width
              : column * kPresentationFrameGenerationBlockSize;
      float dx = 0.0f, dy = 0.0f;
      PresentationFrameGeneration_MotionAt(
          motion, forward,
          source_x < motion->width ? source_x : motion->width - 1,
          source_y < motion->height ? source_y : motion->height - 1,
          &dx, &dy);
      s_vertices[vertex++] = (SDL_Vertex){
        .position = {
          (float)(output_x + source_x) + dx * amount,
          (float)source_y + dy * amount,
        },
        .color = {1.0f, 1.0f, 1.0f, 1.0f},
        .tex_coord = {
          (float)source_x / (float)motion->width,
          (float)source_y / (float)motion->height,
        },
      };
    }
  }
  if (motion->uniform) {
    *out_indices = kQuadIndices;
    *out_index_count = (int)(sizeof(kQuadIndices) / sizeof(kQuadIndices[0]));
  } else {
    if (!EnsureBlockIndices(motion->blocks_x, motion->blocks_y)) return false;
    *out_indices = s_indices;
    *out_index_count = motion->blocks_x * motion->blocks_y * 6;
  }
  *out_vertices = vertex_count;
  return true;
}

static bool DrawEndpoint(SDL_Renderer *renderer, SDL_Texture *texture,
                         const PresentationFrameGenerationMotionField *motion,
                         int output_x, bool forward, float amount) {
  int vertex_count = 0, index_count = 0;
  const int *indices = NULL;
  if (!BuildMesh(
          motion, output_x, forward, amount,
          &vertex_count, &indices, &index_count))
    return false;
  SDL_BlendMode old_blend = SDL_BLENDMODE_NONE;
  SDL_ScaleMode old_scale = SDL_SCALEMODE_NEAREST;
  if (!SDL_GetTextureBlendMode(texture, &old_blend) ||
      !SDL_GetTextureScaleMode(texture, &old_scale))
    return false;
  /* SDL_RenderGeometry takes alpha from SDL_Vertex.color and explicitly
   * ignores texture alpha modulation. Requiring SetTextureAlphaModFloat here
   * would reject otherwise-capable backends where that optional texture state
   * is unsupported, without changing a single generated pixel. */
  const bool configured =
      SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE) &&
      SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
  const bool drawn = configured && SDL_RenderGeometry(
      renderer, texture, s_vertices, vertex_count,
      indices, index_count);
  /* Cleanup is deliberately non-short-circuiting. A backend failure restoring
   * one property must not prevent attempts to restore every other property. */
  bool restored = true;
  if (!SDL_SetTextureScaleMode(texture, old_scale)) restored = false;
  if (!SDL_SetTextureBlendMode(texture, old_blend)) restored = false;
  return drawn && restored;
}

static bool GeneratePlane(
    SDL_Renderer *renderer, int plane_index,
    SDL_Texture *old_target, float phase) {
  DioramaFrameGenerationPlane *plane = &s_planes[plane_index];
  if (!plane->pair_valid || !plane->previous_texture ||
      !plane->current_texture || !plane->generated_texture)
    return false;

  if (!SDL_SetRenderTarget(renderer, plane->generated_texture)) return false;
  /* Logical presentation, viewport, and clip state are target-specific in
   * SDL. Disabling them on the old window/scene target before this switch does
   * not configure a newly selected private target; inheriting its creation-time
   * logical transform scales and clips native plane coordinates into garbage.
   * These generated targets are internal-only, so pin their state explicitly. */
  const bool configured = SDL_SetRenderLogicalPresentation(
          renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED) &&
      SDL_SetRenderViewport(renderer, NULL) &&
      SDL_SetRenderClipRect(renderer, NULL) &&
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
  /* Render only the nearer endpoint. With a trustworthy bidirectional field,
   * both warps meet at the same intermediate position; drawing the farther
   * endpoint again cannot add motion information. It does keep old transparent
   * sprite pixels alive and cross-fades animation poses, which looks like
   * latched input/ghosting on pixel art. Nearest-endpoint ownership preserves
   * the selected capture's exact alpha and changes ownership only at midpoint. */
  bool generated = configured && SDL_RenderClear(renderer);
  if (generated && phase < 0.5f) {
    generated = DrawEndpoint(
        renderer, plane->previous_texture, &plane->motion,
        plane->output_x, true, phase);
  } else if (generated) {
    generated = DrawEndpoint(
        renderer, plane->current_texture, &plane->motion,
        plane->output_x, false, 1.0f - phase);
  }
  return SDL_SetRenderTarget(renderer, old_target) && generated;
}

uint32_t DioramaFrameGeneration_Prepare(
    SDL_Renderer *renderer, const FrameSlot *slot, float alpha,
    SDL_Texture *const current_textures[kDioramaPlane_Count],
    uint32_t current_plane_mask,
    SDL_Texture *resolved_textures[kDioramaPlane_Count]) {
  if (!resolved_textures || !current_textures) return 0;
  memcpy(resolved_textures, current_textures,
         sizeof(SDL_Texture *) * kDioramaPlane_Count);
  if (!renderer || !slot || !slot->diorama_active ||
      !slot->interp_setting_enabled ||
      alpha < 0.0f ||
      s_pair_timestamp_ns != slot->timestamp_ns || !s_pair_mask)
    return 0;

  const float phase = PresentationFrameGeneration_PairPhase(
      alpha, slot->capture_ticks);
  PresentationOutputState output_state;
  if (!PresentationGeometry_PushFullOutput(renderer, &output_state)) return 0;
  SDL_Texture *old_target = SDL_GetRenderTarget(renderer);
  Uint8 old_r = 0, old_g = 0, old_b = 0, old_a = 0;
  if (!SDL_GetRenderDrawColor(renderer, &old_r, &old_g, &old_b, &old_a)) {
    PresentationGeometry_PopFullOutput(renderer, &output_state);
    return 0;
  }
  uint32_t generated_mask = 0;
  for (int plane = 0; plane < kDioramaPlane_Count; plane++) {
    if (!((s_pair_mask & current_plane_mask) & (1u << plane)) ||
        !current_textures[plane])
      continue;
    const bool generated = GeneratePlane(
        renderer, plane, old_target, phase);
    if (SDL_GetRenderTarget(renderer) != old_target) {
      (void)SDL_SetRenderTarget(renderer, old_target);
      break;
    }
    if (generated) {
      resolved_textures[plane] = s_planes[plane].generated_texture;
      generated_mask |= 1u << plane;
    }
  }
  SDL_SetRenderDrawColor(renderer, old_r, old_g, old_b, old_a);
  PresentationGeometry_PopFullOutput(renderer, &output_state);
  return generated_mask;
}
