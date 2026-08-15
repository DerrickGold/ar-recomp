#include "sim_background_voxel_renderer.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "scene3d_math.h"
#include "sim_background_mountain_relief.h"
#include "sim_background_voxel_biome.h"
#include "sim_background_voxel_depth.h"
#include "sim_background_voxel_lighting.h"
#include "sim_background_voxel_lod.h"
#include "sim_background_voxel_model_cache.h"
#include "sim_background_voxel_models.h"
#include "sim_background_voxel_palette.h"
#include "sim_background_voxel_proportions.h"
#include "sim_background_voxels.h"

enum {
  /* Authored tree clusters remain under 100 faces at Ultra. This batch normally
   * covers an entire viewport in one call, while retaining a safe flush path
   * for unusually broad views. */
  kMaxGeometryQuads = 8192,
  kMaxVertices = kMaxGeometryQuads * 4,
  kMaxIndices = kMaxGeometryQuads * 6,
  /* A north-clipped range can gain two authentic cap rows. They add at most
   * 48 sparse front faces and no side bands, but reserve two complete rows so
   * the bound remains obvious if the cap pattern is expanded later. */
  kMaxMountainReliefCells = kSimBackgroundMountainCellCount +
      2 * kSimBackgroundMountainTownCells,
  kMaxMountainReliefFaces = kMaxMountainReliefCells *
      (1 + 4 * kSimBackgroundMountainReliefMaxSideBands),
};

typedef struct SimBackgroundGeometryBatch {
  SDL_Vertex vertices[kMaxVertices];
  int indices[kMaxIndices];
  int vertex_count, index_count;
} SimBackgroundGeometryBatch;

typedef struct ProjectedModelFace {
  Scene3DPoint points[4];
  float depth;
  uint8_t material;
  uint8_t brightness[4];
} ProjectedModelFace;

typedef struct ProjectedMountainReliefFace {
  Scene3DPoint points[4];
  SDL_FPoint uv[4];
  float depth;
  uint8_t brightness;
  uint8_t alpha[4];
} ProjectedMountainReliefFace;

typedef struct SimBackgroundModelLean {
  float x_per_height;
  float y_per_height;
} SimBackgroundModelLean;

static struct {
  SDL_Texture *ground;
  SDL_Texture *mountain_art;
  SDL_Texture *supersample;
  int supersample_w, supersample_h;
  uint32_t uploaded_serial;
  bool allocation_failed;
  bool mountain_art_allocation_failed;
  bool supersample_allocation_failed;
  uint32_t cache_stamp;
  SimBackgroundVoxelBiome biome;
  ProjectedMountainReliefFace
      mountain_projected[kMaxMountainReliefFaces];
  SimBackgroundVoxelPalette palettes[kSimBackgroundMaxObjects];
  SimBackgroundGeometryBatch batch;
} g_renderer_state;

static SDL_Texture *CreateCanvasTexture(
    SDL_Renderer *renderer, SDL_ScaleMode scale_mode) {
  SDL_Texture *texture = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kSimTownCanvasPixels, kSimTownCanvasPixels);
  if (!texture) return NULL;
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(texture, scale_mode);
  return texture;
}

void SimBackgroundVoxelRenderer_Upload(SDL_Renderer *renderer) {
  uint32_t serial = SimBackgroundVoxels_Serial();
  if (!renderer || !serial || serial == g_renderer_state.uploaded_serial ||
      g_renderer_state.allocation_failed)
    return;
  if (!g_renderer_state.ground)
    g_renderer_state.ground = CreateCanvasTexture(
        renderer, SDL_SCALEMODE_LINEAR);
  if (!g_renderer_state.ground) {
    g_renderer_state.allocation_failed = true;
    fprintf(stderr, "[sim-bg-voxels] texture allocation failed: %s\n",
            SDL_GetError());
    return;
  }
  int pitch = kSimTownCanvasPixels * (int)sizeof(uint32_t);
  if (!SDL_UpdateTexture(g_renderer_state.ground, NULL,
                         SimBackgroundVoxels_GroundPixels(), pitch)) {
    fprintf(stderr, "[sim-bg-voxels] texture upload failed: %s\n",
            SDL_GetError());
    return;
  }

  if (!g_renderer_state.mountain_art &&
      !g_renderer_state.mountain_art_allocation_failed)
    g_renderer_state.mountain_art = CreateCanvasTexture(
        renderer, SDL_SCALEMODE_NEAREST);
  if (g_renderer_state.mountain_art &&
      !SDL_UpdateTexture(g_renderer_state.mountain_art, NULL,
                         SimBackgroundVoxels_AtlasPixels(), pitch)) {
    fprintf(stderr, "[sim-bg-voxels] mountain atlas upload failed: %s\n",
            SDL_GetError());
    SDL_DestroyTexture(g_renderer_state.mountain_art);
    g_renderer_state.mountain_art = NULL;
    g_renderer_state.mountain_art_allocation_failed = true;
  } else if (!g_renderer_state.mountain_art &&
             !g_renderer_state.mountain_art_allocation_failed) {
    fprintf(stderr, "[sim-bg-voxels] mountain atlas allocation failed: %s\n",
            SDL_GetError());
    g_renderer_state.mountain_art_allocation_failed = true;
  }

  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  g_renderer_state.biome = SimBackgroundVoxelBiome_ForTown(scene->town);
  for (uint16_t i = 0; i < scene->object_count; i++)
    SimBackgroundVoxelPalette_Build(
        &scene->objects[i], g_renderer_state.biome,
        &g_renderer_state.palettes[i]);
  g_renderer_state.uploaded_serial = serial;
}

bool SimBackgroundVoxelRenderer_Ready(uint32_t serial) {
  return serial && serial == g_renderer_state.uploaded_serial &&
      g_renderer_state.ground;
}

SDL_Texture *SimBackgroundVoxelRenderer_GroundTexture(uint32_t serial) {
  return SimBackgroundVoxelRenderer_Ready(serial) ? g_renderer_state.ground
                                                   : NULL;
}

static void TexturePointToWorld(
    const SimBackgroundVoxelRenderParams *params,
    float texture_x, float texture_y, float height_pixels,
    float *world_x, float *world_y, float *world_z) {
  float fx = (texture_x - params->source.x) / params->source.w;
  float fy = (texture_y - params->source.y) / params->source.h;
  float aspect = (float)params->viewport.w / params->viewport.h;
  *world_x = (fx - 0.5f) * aspect;
  *world_y = 0.5f - fy;
  *world_z = height_pixels / params->source.h;
}

static SimBackgroundModelLean CameraFacingLean(
    const SimBackgroundVoxelRenderParams *params,
    float billboard_blend) {
  /* Clip depth's ground gradient points away from the camera. In town-texture
   * coordinates Y has the opposite sign from world Y, so (m3,-m7) is that
   * direction in pixels. Moving a model top along it cancels some of the
   * depth introduced by world Z and rotates the vertical axis toward a true
   * billboard without flattening the model completely.
   *
   * A 0.35 blend preserves visible roof depth while making the facade read
   * like the original upright SIM art. The cap keeps a nearly pitchless
   * camera from asking for an infinitely tilted billboard. */
  const float kMaximumLeanPerHeight = 1.15f;
  /* Express the clip-depth gradient in authored texture pixels. Matrix X is
   * in aspect-scaled world units while Y/Z each span one source height; using
   * raw matrix coefficients made the lean change with window aspect ratio. */
  float aspect = (float)params->viewport.w / params->viewport.h;
  float direction_x = params->matrix[3] * aspect / params->source.w;
  float direction_y = -params->matrix[7] / params->source.h;
  float ground_depth = sqrtf(direction_x * direction_x +
                             direction_y * direction_y);
  if (ground_depth < 0.0001f)
    return (SimBackgroundModelLean){0.0f, 0.0f};
  float vertical_depth = params->matrix[11] / params->source.h;
  float full_billboard = -vertical_depth / ground_depth;
  if (full_billboard <= 0.0f)
    return (SimBackgroundModelLean){0.0f, 0.0f};
  float lean = full_billboard * billboard_blend;
  if (lean > kMaximumLeanPerHeight) lean = kMaximumLeanPerHeight;
  return (SimBackgroundModelLean){
    direction_x / ground_depth * lean,
    direction_y / ground_depth * lean,
  };
}

static SimBackgroundModelLean CameraFacingModelLean(
    const SimBackgroundVoxelRenderParams *params,
    SimBackgroundVoxelKind kind) {
  float billboard_blend = 0.35f;
  if (params->facing == kSimBackgroundVoxelFacing_PerModel) {
    /* Tall narrow facades need more correction than broad roofs and foliage.
     * These are presentation axes, not height scales, so proportions remain
     * stable as the camera moves. */
    static const float blend[kSimBackgroundVoxelKindCount] = {
      [kSimBackgroundVoxel_House] = 0.48f,
      [kSimBackgroundVoxel_Cathedral] = 0.44f,
      [kSimBackgroundVoxel_Windmill] = 0.50f,
      [kSimBackgroundVoxel_Factory] = 0.38f,
      [kSimBackgroundVoxel_Tree] = 0.30f,
    };
    if (kind >= kSimBackgroundVoxel_House &&
        kind < kSimBackgroundVoxelKindCount)
      billboard_blend = blend[kind];
  }
  return CameraFacingLean(params, billboard_blend);
}

static bool ProjectPoint(const SimBackgroundVoxelRenderParams *params,
                         const SimBackgroundModelLean *lean,
                         float texture_x, float texture_y, float height_pixels,
                         Scene3DPoint *out, float *clip_depth) {
  /* Lean is a rotation-like presentation axis, not an additive shear. Scale
   * its vertical and ground components together so facing the camera does not
   * make the model physically longer as the pitch changes. */
  float aspect = (float)params->viewport.w / params->viewport.h;
  float x_metric = aspect * params->source.h / params->source.w;
  float lean_x = lean->x_per_height * x_metric;
  float lean_length = sqrtf(lean_x * lean_x +
                            lean->y_per_height * lean->y_per_height);
  float vertical_height = height_pixels /
      sqrtf(1.0f + lean_length * lean_length);
  texture_x += vertical_height * lean->x_per_height;
  texture_y += vertical_height * lean->y_per_height;
  float world_x, world_y, world_z;
  TexturePointToWorld(params, texture_x, texture_y, vertical_height,
                      &world_x, &world_y, &world_z);
  Scene3DPoint projected;
  if (!Scene3D_ProjectWorldPoint(
          params->matrix, world_x, world_y, world_z,
          params->viewport.w, params->viewport.h, &projected))
    return false;
  projected.x += params->viewport.x;
  projected.y += params->viewport.y;
  if (params->render_scale == kSimBackgroundVoxelRenderScale_PixelClean) {
    projected.x = floorf(projected.x) + 0.5f;
    projected.y = floorf(projected.y) + 0.5f;
  }
  *out = projected;
  if (clip_depth)
    *clip_depth = Scene3D_ClipDepth(
        params->matrix, world_x, world_y, world_z);
  return true;
}

static SDL_FColor VertexColour(uint32_t argb, uint8_t brightness) {
  float shade = brightness / 255.0f;
  return (SDL_FColor){
    ((argb >> 16) & 0xFF) / 255.0f * shade,
    ((argb >> 8) & 0xFF) / 255.0f * shade,
    (argb & 0xFF) / 255.0f * shade,
    ((argb >> 24) & 0xFF) / 255.0f,
  };
}

static void FlushBatchTexture(SDL_Renderer *renderer,
                              SimBackgroundGeometryBatch *batch,
                              SDL_Texture *texture) {
  if (!batch->index_count) return;
  SDL_RenderGeometry(renderer, texture, batch->vertices, batch->vertex_count,
                     batch->indices, batch->index_count);
  batch->vertex_count = 0;
  batch->index_count = 0;
}

static void FlushBatch(SDL_Renderer *renderer,
                       SimBackgroundGeometryBatch *batch) {
  FlushBatchTexture(renderer, batch, NULL);
}

static void AppendProjectedFace(
    SDL_Renderer *renderer, SimBackgroundGeometryBatch *batch,
    const ProjectedModelFace *face,
    const SimBackgroundVoxelPalette *palette,
    SimBackgroundVoxelShading shading) {
  if (batch->vertex_count + 4 > kMaxVertices ||
      batch->index_count + 6 > kMaxIndices)
    FlushBatch(renderer, batch);
  int base = batch->vertex_count;
  for (int i = 0; i < 4; i++) {
    SimBackgroundVoxelMaterial material =
        (SimBackgroundVoxelMaterial)face->material;
    uint32_t argb = shading == kSimBackgroundVoxelShading_MaterialAware
        ? SimBackgroundVoxelPalette_Ramp(
              palette, material, face->brightness[i])
        : SimBackgroundVoxelPalette_Base(palette, material);
    uint8_t brightness =
        shading == kSimBackgroundVoxelShading_MaterialAware
            ? 255 : face->brightness[i];
    SDL_FColor colour = VertexColour(argb, brightness);
    batch->vertices[batch->vertex_count++] =
        (SDL_Vertex){{face->points[i].x, face->points[i].y}, colour, {0, 0}};
  }
  static const int indices[] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < 6; i++)
    batch->indices[batch->index_count++] = base + indices[i];
}

static bool IsDegenerate(const Scene3DPoint points[4]) {
  float twice_area = 0.0f;
  for (int i = 0; i < 4; i++) {
    const Scene3DPoint *a = &points[i];
    const Scene3DPoint *b = &points[(i + 1) & 3];
    twice_area += a->x * b->y - b->x * a->y;
  }
  return twice_area > -0.05f && twice_area < 0.05f;
}

static SimBackgroundVoxelDetail EffectiveMountainDetail(
    const SimBackgroundVoxelRenderParams *params) {
  SimBackgroundVoxelDetail requested =
      (SimBackgroundVoxelDetail)params->detail;
  if (params->lod != kSimBackgroundVoxelLod_Adaptive) return requested;
  const SimBackgroundModelLean no_lean = {0.0f, 0.0f};
  float origin_x = (float)params->town_screen_x0 - params->camera_x;
  float origin_y = -(float)params->camera_y;
  float center = kSimTownCanvasPixels * 0.5f;
  Scene3DPoint bottom, top;
  if (!ProjectPoint(params, &no_lean, origin_x + center, origin_y + center,
                    0.0f, &bottom, NULL) ||
      !ProjectPoint(params, &no_lean, origin_x + center, origin_y + center,
                    24.0f, &top, NULL))
    return requested;
  float dx = top.x - bottom.x, dy = top.y - bottom.y;
  float projected_height = sqrtf(dx * dx + dy * dy);
  if (params->render_scale == kSimBackgroundVoxelRenderScale_2x)
    projected_height *= 0.5f;
  return SimBackgroundVoxelLod_Resolve(
      requested, kSimBackgroundVoxelLod_Adaptive, projected_height);
}

static int CompareProjectedMountainDepth(
    const void *left, const void *right) {
  float a = ((const ProjectedMountainReliefFace *)left)->depth;
  float b = ((const ProjectedMountainReliefFace *)right)->depth;
  return a < b ? 1 : a > b ? -1 : 0;
}

static void AddProjectedMountainReliefFace(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundModelLean *lean,
    float origin_x, float origin_y,
    const float local_x[4], const float local_y[4],
    const float local_z[4], const SDL_FPoint uv[4],
    uint8_t brightness, const uint8_t alpha[4], int *count) {
  if (*count >= kMaxMountainReliefFaces) return;
  ProjectedMountainReliefFace face = {
    .brightness = brightness,
  };
  float depth_sum = 0.0f;
  for (int point = 0; point < 4; point++) {
    float depth;
    if (!ProjectPoint(params, lean,
                      origin_x + local_x[point],
                      origin_y + local_y[point], local_z[point],
                      &face.points[point], &depth))
      return;
    face.uv[point] = uv[point];
    face.alpha[point] = alpha[point];
    depth_sum += depth;
  }
  if (IsDegenerate(face.points)) return;
  face.depth = depth_sum * 0.25f;
  g_renderer_state.mountain_projected[(*count)++] = face;
}

static void MountainPlanePoint(
    float source_x, float source_y, float baseline,
    const SimBackgroundMountainRelief *relief,
    float offset_x, float offset_y,
    float *local_x, float *local_y, float *local_z) {
  float rise = baseline - source_y;
  *local_x = source_x + offset_x;
  *local_y = baseline - rise * relief->face_depth_scale + offset_y;
  *local_z = rise * relief->face_height_scale;
}

static bool FindMountainTileSource(
    const SimBackgroundMountainField *field, uint8_t tile,
    int *source_cell_x, int *source_cell_y) {
  for (int cell_y = 0; cell_y < kSimBackgroundMountainTownCells; cell_y++)
    for (int cell_x = 0; cell_x < kSimBackgroundMountainTownCells; cell_x++) {
      int cell = cell_y * kSimBackgroundMountainTownCells + cell_x;
      if (field->tile[cell] != tile ||
          !(field->flags[cell] & kSimBackgroundMountainCell_Occupied))
        continue;
      *source_cell_x = cell_x;
      *source_cell_y = cell_y;
      return true;
    }
  return false;
}

static void AddMountainFrontTile(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundModelLean *lean,
    const SimBackgroundMountainRelief *relief,
    float origin_x, float origin_y, float baseline,
    int destination_cell_x, int destination_cell_y,
    int source_cell_x, int source_cell_y, bool mirror_x,
    int *count) {
  float x0 = destination_cell_x * kSimBackgroundCellPixels;
  float y0 = destination_cell_y * kSimBackgroundCellPixels;
  float x1 = x0 + kSimBackgroundCellPixels;
  float y1 = y0 + kSimBackgroundCellPixels;
  float u0 = source_cell_x * kSimBackgroundCellPixels /
      (float)kSimTownCanvasPixels;
  float v0 = source_cell_y * kSimBackgroundCellPixels /
      (float)kSimTownCanvasPixels;
  float u1 = (source_cell_x + 1) * kSimBackgroundCellPixels /
      (float)kSimTownCanvasPixels;
  float v1 = (source_cell_y + 1) * kSimBackgroundCellPixels /
      (float)kSimTownCanvasPixels;
  const SDL_FPoint uv[4] = {
    {mirror_x ? u1 : u0, v0}, {mirror_x ? u0 : u1, v0},
    {mirror_x ? u0 : u1, v1}, {mirror_x ? u1 : u0, v1},
  };
  const float source_x[4] = {x0, x1, x1, x0};
  const float source_y[4] = {y0, y0, y1, y1};
  float local_x[4], local_y[4], local_z[4];
  for (int point = 0; point < 4; point++)
    MountainPlanePoint(
        source_x[point], source_y[point], baseline, relief,
        0.0f, 0.0f,
        &local_x[point], &local_y[point], &local_z[point]);
  const uint8_t opaque[4] = {255, 255, 255, 255};
  AddProjectedMountainReliefFace(
      params, lean, origin_x, origin_y,
      local_x, local_y, local_z, uv, 255, opaque, count);
}

static void AddNorthMountainCaps(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundModelLean *lean,
    const SimBackgroundMountainRelief *relief,
    const SimBackgroundMountainField *field,
    const float component_bottom[kSimBackgroundMountainCellCount + 1],
    float origin_x, float origin_y, int *count) {
  SimBackgroundMountainCaps caps;
  SimBackgroundMountains_BuildNorthCaps(field, &caps);
  for (uint8_t at = 0; at < caps.tile_count; at++) {
    const SimBackgroundMountainCapTile *tile = &caps.tiles[at];
    int source_cell_x, source_cell_y;
    if (!FindMountainTileSource(
            field, tile->source_tile, &source_cell_x, &source_cell_y))
      continue;
    AddMountainFrontTile(
        params, lean, relief, origin_x, origin_y,
        component_bottom[tile->component], tile->cell_x, tile->cell_y,
        source_cell_x, source_cell_y,
        (tile->flags & kSimBackgroundMountainCapTile_MirrorX) != 0,
        count);
  }
}

static int BuildProjectedMountainReliefFaces(
    const SimBackgroundVoxelRenderParams *params) {
  if (!g_renderer_state.mountain_art) return 0;
  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  const SimBackgroundMountainField *field = &scene->mountains;
  if (!field->cell_count) return 0;
  SimBackgroundMountainRelief relief;
  SimBackgroundMountainRelief_Resolve(
      EffectiveMountainDetail(params),
      (SimBackgroundVoxelShading)params->shading, &relief);
  if (!relief.side_band_count || relief.depth_pixels <= 0.0f) return 0;

  float origin_x = (float)params->town_screen_x0 - params->camera_x;
  float origin_y = -(float)params->camera_y;
  SimBackgroundModelLean mountain_lean = CameraFacingLean(
      params, params->facing == kSimBackgroundVoxelFacing_PerModel
          ? 0.44f : 0.35f);

  /* Each connected range shares one baseline. Mapping source Y partly into
   * height and partly into ground depth turns the original pseudo-perspective
   * art into one continuous shallow facade instead of tilting every 16x16
   * tile independently. */
  float component_bottom[kSimBackgroundMountainCellCount + 1] = {0};
  for (int cell_y = 0; cell_y < kSimBackgroundMountainTownCells;
       cell_y++)
    for (int cell_x = 0; cell_x < kSimBackgroundMountainTownCells;
         cell_x++) {
      int cell = cell_y * kSimBackgroundMountainTownCells + cell_x;
      if (!(field->flags[cell] & kSimBackgroundMountainCell_Occupied))
        continue;
      uint16_t component = field->component[cell];
      if (!component) component = 1;
      float bottom = (cell_y + 1) * kSimBackgroundCellPixels;
      if (bottom > component_bottom[component])
        component_bottom[component] = bottom;
    }

  /* Side bands are displaced behind the front facade along the same
   * clip-depth gradient used by the building billboards. The original art is
   * emitted once; only component perimeter edges receive thickness. */
  float aspect = (float)params->viewport.w / params->viewport.h;
  float away_x = params->matrix[3] * aspect / params->source.w;
  float away_y = -params->matrix[7] / params->source.h;
  float away_length = sqrtf(away_x * away_x + away_y * away_y);
  if (away_length > 0.000001f) {
    away_x /= away_length;
    away_y /= away_length;
  } else {
    away_x = 0.0f;
    away_y = -1.0f;
  }
  int count = 0;
  for (int cell_y = 0; cell_y < kSimBackgroundMountainTownCells;
       cell_y++) {
    for (int cell_x = 0; cell_x < kSimBackgroundMountainTownCells;
         cell_x++) {
      if (!SimBackgroundMountains_CellOccupied(field, cell_x, cell_y))
        continue;
      int cell = cell_y * kSimBackgroundMountainTownCells + cell_x;
      uint16_t component = field->component[cell];
      if (!component) component = 1;
      float baseline = component_bottom[component];
      float x0 = cell_x * kSimBackgroundCellPixels;
      float y0 = cell_y * kSimBackgroundCellPixels;
      float x1 = x0 + kSimBackgroundCellPixels;
      float y1 = y0 + kSimBackgroundCellPixels;
      const SDL_FPoint uv[4] = {
        {x0 / kSimTownCanvasPixels, y0 / kSimTownCanvasPixels},
        {x1 / kSimTownCanvasPixels, y0 / kSimTownCanvasPixels},
        {x1 / kSimTownCanvasPixels, y1 / kSimTownCanvasPixels},
        {x0 / kSimTownCanvasPixels, y1 / kSimTownCanvasPixels},
      };
      const float source_x[4] = {x0, x1, x1, x0};
      const float source_y[4] = {y0, y0, y1, y1};
      float local_x[4], local_y[4], local_z[4];
      for (int point = 0; point < 4; point++)
        MountainPlanePoint(
            source_x[point], source_y[point], baseline, &relief,
            0.0f, 0.0f,
            &local_x[point], &local_y[point], &local_z[point]);
      const uint8_t front_alpha[4] = {255, 255, 255, 255};
      AddProjectedMountainReliefFace(
          params, &mountain_lean, origin_x, origin_y,
          local_x, local_y, local_z, uv,
          255, front_alpha, &count);

      static const int edge_dx[4] = {0, 1, 0, -1};
      static const int edge_dy[4] = {-1, 0, 1, 0};
      static const float edge_normal_x[4] = {0.0f, 1.0f, 0.0f, -1.0f};
      static const float edge_normal_y[4] = {-1.0f, 0.0f, 1.0f, 0.0f};
      for (int edge = 0; edge < 4; edge++) {
        /* A range touching the north map boundary continues beneath the
         * atmospheric shroud. It has no physical back edge to extrude. */
        if (edge == 0 && cell_y == 0) continue;
        if (SimBackgroundMountains_CellOccupied(
                field, cell_x + edge_dx[edge],
                cell_y + edge_dy[edge]))
          continue;
        float facing = -(edge_normal_x[edge] * away_x +
                         edge_normal_y[edge] * away_y);
        if (facing <= 0.0001f) continue;
        if (facing > 1.0f) facing = 1.0f;
        float ax, ay, bx, by, au, av, bu, bv;
        switch (edge) {
          case 0:
            ax = x0; ay = y0; bx = x1; by = y0;
            au = uv[0].x; av = uv[0].y;
            bu = uv[1].x; bv = uv[1].y;
            break;
          case 1:
            ax = x1; ay = y0; bx = x1; by = y1;
            au = uv[1].x; av = uv[1].y;
            bu = uv[2].x; bv = uv[2].y;
            break;
          case 2:
            ax = x1; ay = y1; bx = x0; by = y1;
            au = uv[2].x; av = uv[2].y;
            bu = uv[3].x; bv = uv[3].y;
            break;
          default:
            ax = x0; ay = y1; bx = x0; by = y0;
            au = uv[3].x; av = uv[3].y;
            bu = uv[0].x; bv = uv[0].y;
            break;
        }
        const SDL_FPoint side_uv[4] = {
          {au, av}, {bu, bv}, {bu, bv}, {au, av},
        };
        uint8_t side_alpha_value = (uint8_t)(relief.side_alpha * facing);
        const uint8_t side_alpha[4] = {
          side_alpha_value, side_alpha_value,
          side_alpha_value, side_alpha_value,
        };
        for (uint8_t band = 0; band < relief.side_band_count; band++) {
          float t0 = relief.depth_pixels * band /
              relief.side_band_count;
          float t1 = relief.depth_pixels * (band + 1) /
              relief.side_band_count;
          const float edge_source_x[4] = {ax, bx, bx, ax};
          const float edge_source_y[4] = {ay, by, by, ay};
          const float offset[4] = {t0, t0, t1, t1};
          float side_x[4], side_y[4], side_z[4];
          for (int point = 0; point < 4; point++)
            MountainPlanePoint(
                edge_source_x[point], edge_source_y[point], baseline,
                &relief, away_x * offset[point], away_y * offset[point],
                &side_x[point], &side_y[point], &side_z[point]);
          AddProjectedMountainReliefFace(
              params, &mountain_lean, origin_x, origin_y,
              side_x, side_y, side_z, side_uv,
              relief.side_brightness[band], side_alpha, &count);
        }
      }
    }
  }
  AddNorthMountainCaps(
      params, &mountain_lean, &relief, field, component_bottom,
      origin_x, origin_y, &count);
  qsort(g_renderer_state.mountain_projected, (size_t)count,
        sizeof(g_renderer_state.mountain_projected[0]),
        CompareProjectedMountainDepth);
  return count;
}

static void AppendProjectedMountainReliefFace(
    SDL_Renderer *renderer, SimBackgroundGeometryBatch *batch,
    const ProjectedMountainReliefFace *face) {
  if (batch->vertex_count + 4 > kMaxVertices ||
      batch->index_count + 6 > kMaxIndices)
    FlushBatchTexture(renderer, batch, g_renderer_state.mountain_art);
  int base = batch->vertex_count;
  float shade = face->brightness / 255.0f;
  for (int point = 0; point < 4; point++) {
    SDL_FColor tint = {
      shade, shade, shade, face->alpha[point] / 255.0f,
    };
    batch->vertices[batch->vertex_count++] = (SDL_Vertex){
      {face->points[point].x, face->points[point].y},
      tint, face->uv[point],
    };
  }
  static const int indices[] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < 6; i++)
    batch->indices[batch->index_count++] = base + indices[i];
}

static float ObjectOriginY(const SimBackgroundVoxelObject *object) {
  return (object->cell_y + object->source_cells_h -
          object->footprint_cells_d) * kSimBackgroundCellPixels;
}

typedef struct SimBackgroundContactBounds {
  float x0, y0, x1, y1;
} SimBackgroundContactBounds;

enum { kSimBackgroundMaxContactQuads = 3 };

static int ContactBounds(const SimBackgroundVoxelObject *object,
                         SimBackgroundContactBounds *out) {
  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House:
      out[0] = (SimBackgroundContactBounds){1.8f, 2.5f, 14.2f, 15.2f};
      return 1;
    case kSimBackgroundVoxel_Cathedral:
      out[0] = (SimBackgroundContactBounds){0.8f, 8.8f, 31.2f, 31.4f};
      return 1;
    case kSimBackgroundVoxel_Windmill:
      out[0] = (SimBackgroundContactBounds){6.2f, 2.3f, 25.8f, 15.2f};
      return 1;
    case kSimBackgroundVoxel_Factory:
      out[0] = (SimBackgroundContactBounds){0.8f, 0.8f, 21.8f, 10.8f};
      out[1] = (SimBackgroundContactBounds){0.8f, 22.2f, 21.8f, 31.2f};
      out[2] = (SimBackgroundContactBounds){21.2f, 0.8f, 31.2f, 31.2f};
      return 3;
    case kSimBackgroundVoxel_Tree:
      /* Contact belongs to the trunk, not the foliage footprint. This avoids
       * turning dense forests into one continuous dark carpet. */
      out[0] = (SimBackgroundContactBounds){5.4f, 5.4f, 10.6f, 10.6f};
      return 1;
  }
  return 0;
}

static void AppendGroundContact(
    SDL_Renderer *renderer, SimBackgroundGeometryBatch *batch,
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelPalette *palette,
    const SimBackgroundVoxelRenderParams *params,
    float origin_x, float origin_y,
    const SimBackgroundVoxelProportions *proportions) {
  if (params->shading < kSimBackgroundVoxelShading_AmbientOcclusion)
    return;
  SimBackgroundContactBounds bounds[kSimBackgroundMaxContactQuads];
  int count = ContactBounds(object, bounds);
  float center_x = object->footprint_cells_w *
      kSimBackgroundCellPixels * 0.5f;
  float center_y = object->footprint_cells_d *
      kSimBackgroundCellPixels * 0.5f;
  const SimBackgroundModelLean no_lean = {0.0f, 0.0f};
  for (int at = 0; at < count; at++) {
    float x0 = center_x + (bounds[at].x0 - center_x) *
        proportions->footprint_scale;
    float x1 = center_x + (bounds[at].x1 - center_x) *
        proportions->footprint_scale;
    float y0 = center_y + (bounds[at].y0 - center_y) *
        proportions->footprint_scale;
    float y1 = center_y + (bounds[at].y1 - center_y) *
        proportions->footprint_scale;
    const float local_x[4] = {x0, x1, x1, x0};
    const float local_y[4] = {y0, y0, y1, y1};
    ProjectedModelFace face = {
      .material = kSimVoxelMaterial_Contact,
      .brightness = {255, 255, 255, 255},
    };
    bool valid = true;
    for (int point = 0; point < 4; point++) {
      if (!ProjectPoint(params, &no_lean,
                        origin_x + local_x[point],
                        origin_y + local_y[point], 0.06f,
                        &face.points[point], NULL)) {
        valid = false;
        break;
      }
    }
    if (valid && !IsDegenerate(face.points))
      AppendProjectedFace(
          renderer, batch, &face, palette,
          (SimBackgroundVoxelShading)params->shading);
  }
}

static float ModelAuthoredHeight(const SimBackgroundVoxelObject *object) {
  bool construction =
      (object->flags & kSimBackgroundVoxel_UnderConstruction) != 0;
  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House: return construction ? 12.0f : 15.6f;
    case kSimBackgroundVoxel_Cathedral: return 24.0f;
    case kSimBackgroundVoxel_Windmill: return construction ? 24.0f : 31.0f;
    case kSimBackgroundVoxel_Factory: return construction ? 10.0f : 17.0f;
    case kSimBackgroundVoxel_Tree: return 15.0f;
  }
  return 16.0f;
}

static float ObjectGroundDepth(
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelRenderParams *params) {
  float origin_x = (float)params->town_screen_x0 - params->camera_x +
      object->cell_x * kSimBackgroundCellPixels;
  float origin_y = -(float)params->camera_y + ObjectOriginY(object);
  float center_x = object->footprint_cells_w *
      kSimBackgroundCellPixels * 0.5f;
  float center_y = object->footprint_cells_d *
      kSimBackgroundCellPixels * 0.5f;
  float world_x, world_y, world_z;
  TexturePointToWorld(params, origin_x + center_x, origin_y + center_y,
                      0.0f, &world_x, &world_y, &world_z);
  return Scene3D_ClipDepth(params->matrix, world_x, world_y, world_z);
}

static bool ObjectMayBeVisible(
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundModelLean *lean) {
  const SimBackgroundVoxelProportions *proportions =
      SimBackgroundVoxelProportions_Get(
          (SimBackgroundVoxelKind)object->kind);
  float origin_x = (float)params->town_screen_x0 - params->camera_x +
      object->cell_x * kSimBackgroundCellPixels;
  float origin_y = -(float)params->camera_y + ObjectOriginY(object);
  float center_x = object->footprint_cells_w *
      kSimBackgroundCellPixels * 0.5f;
  float center_y = object->footprint_cells_d *
      kSimBackgroundCellPixels * 0.5f;
  float half_x = center_x * proportions->footprint_scale;
  float half_y = center_y * proportions->footprint_scale;
  float x[2] = {origin_x + center_x - half_x,
                origin_x + center_x + half_x};
  float y[2] = {origin_y + center_y - half_y,
                origin_y + center_y + half_y};
  float z[2] = {0.0f,
                ModelAuthoredHeight(object) * proportions->height_scale};
  float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
  bool any = false;
  for (int zi = 0; zi < 2; zi++)
    for (int yi = 0; yi < 2; yi++)
      for (int xi = 0; xi < 2; xi++) {
        Scene3DPoint point;
        if (!ProjectPoint(params, lean, x[xi], y[yi], z[zi],
                          &point, NULL))
          continue;
        if (!any) {
          min_x = max_x = point.x;
          min_y = max_y = point.y;
          any = true;
        } else {
          if (point.x < min_x) min_x = point.x;
          if (point.x > max_x) max_x = point.x;
          if (point.y < min_y) min_y = point.y;
          if (point.y > max_y) max_y = point.y;
        }
      }
  if (!any) return false;
  /* Output-space padding covers projecting edges whose corners straddle the
   * near plane, while remaining independent of source zoom and aspect. */
  const float margin =
      params->render_scale == kSimBackgroundVoxelRenderScale_2x
      ? 48.0f : 24.0f;
  return max_x >= params->viewport.x - margin &&
      min_x <= params->viewport.x + params->viewport.w + margin &&
      max_y >= params->viewport.y - margin &&
      min_y <= params->viewport.y + params->viewport.h + margin;
}

static SimBackgroundVoxelDetail EffectiveDetail(
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundModelLean *lean,
    float origin_x, float origin_y,
    const SimBackgroundVoxelProportions *proportions,
    float center_x, float center_y) {
  SimBackgroundVoxelDetail requested =
      (SimBackgroundVoxelDetail)params->detail;
  if (params->lod != kSimBackgroundVoxelLod_Adaptive) return requested;
  Scene3DPoint bottom, top;
  float height = ModelAuthoredHeight(object) * proportions->height_scale;
  if (!ProjectPoint(params, lean, origin_x + center_x, origin_y + center_y,
                    0.0f, &bottom, NULL) ||
      !ProjectPoint(params, lean, origin_x + center_x, origin_y + center_y,
                    height, &top, NULL))
    return requested;
  float dx = top.x - bottom.x;
  float dy = top.y - bottom.y;
  float projected_height = sqrtf(dx * dx + dy * dy);
  if (params->render_scale == kSimBackgroundVoxelRenderScale_2x)
    projected_height *= 0.5f;
  return SimBackgroundVoxelLod_Resolve(
      requested, kSimBackgroundVoxelLod_Adaptive,
      projected_height);
}

static void DrawModel(
    SDL_Renderer *renderer, SimBackgroundGeometryBatch *batch,
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelPalette *palette,
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundModelLean *lean) {
  float origin_x = (float)params->town_screen_x0 - params->camera_x +
      object->cell_x * kSimBackgroundCellPixels;
  float origin_y = -(float)params->camera_y + ObjectOriginY(object);
  const SimBackgroundVoxelProportions *proportions =
      SimBackgroundVoxelProportions_Get(
          (SimBackgroundVoxelKind)object->kind);
  float center_x = object->footprint_cells_w *
      kSimBackgroundCellPixels * 0.5f;
  float center_y = object->footprint_cells_d *
      kSimBackgroundCellPixels * 0.5f;
  SimBackgroundVoxelDetail detail = EffectiveDetail(
      object, params, lean, origin_x, origin_y, proportions,
      center_x, center_y);
  const SimBackgroundVoxelModel *model = SimBackgroundVoxelModelCache_Get(
      object, detail, (SimBackgroundVoxelStyle)params->style,
      g_renderer_state.cache_stamp);
  if (!model || !model->face_count || model->overflow) return;
  AppendGroundContact(renderer, batch, object, palette, params,
                      origin_x, origin_y, proportions);
  ProjectedModelFace projected[kSimBackgroundVoxelModelMaxFaces];
  int projected_count = 0;
  for (uint16_t face_index = 0; face_index < model->face_count; face_index++) {
    const SimBackgroundVoxelModelFace *source = &model->faces[face_index];
    ProjectedModelFace face = {
      .material = SimBackgroundVoxelBiome_SurfaceMaterial(
          g_renderer_state.biome, detail,
          (SimBackgroundVoxelMaterial)source->material, source),
    };
    uint8_t directional = SimBackgroundVoxelLighting_FaceBrightness(
        source, (SimBackgroundVoxelShading)params->shading,
        params->light_azimuth_deg, params->light_elevation_deg);
    for (int point = 0; point < 4; point++)
      face.brightness[point] =
          SimBackgroundVoxelLighting_VertexBrightness(
              source, model, point, directional,
              (SimBackgroundVoxelShading)params->shading);
    float depth_sum = 0.0f;
    bool valid = true;
    for (int point = 0; point < 4; point++) {
      float depth;
      float local_x = center_x +
          (source->points[point].x - center_x) *
              proportions->footprint_scale;
      float local_y = center_y +
          (source->points[point].y - center_y) *
              proportions->footprint_scale;
      float local_z = source->points[point].z *
          proportions->height_scale;
      if (!ProjectPoint(params, lean,
                        origin_x + local_x,
                        origin_y + local_y,
                        local_z,
                        &face.points[point], &depth)) {
        valid = false;
        break;
      }
      depth_sum += depth;
    }
    if (!valid || IsDegenerate(face.points)) continue;
    face.depth = depth_sum * 0.25f;
    int insert = projected_count;
    while (insert > 0 && projected[insert - 1].depth < face.depth) {
      projected[insert] = projected[insert - 1];
      insert--;
    }
    projected[insert] = face;
    projected_count++;
  }
  for (int i = 0; i < projected_count; i++)
    AppendProjectedFace(
        renderer, batch, &projected[i], palette,
        (SimBackgroundVoxelShading)params->shading);
}

typedef struct SimBackgroundVisibleModel {
  uint16_t index;
  float depth;
  SimBackgroundModelLean lean;
} SimBackgroundVisibleModel;

typedef struct SimBackgroundVisibleModelList {
  uint16_t count;
  SimBackgroundVisibleModel entries[kSimBackgroundMaxObjects];
} SimBackgroundVisibleModelList;

static bool RenderParamsValid(
    SDL_Renderer *renderer, const SimBackgroundVoxelRenderParams *params) {
  if (!renderer || !params || !params->matrix || params->source.w <= 0 ||
      params->source.h <= 0 || params->viewport.w <= 0 ||
      params->viewport.h <= 0 ||
      !SimBackgroundVoxelRenderer_Ready(params->serial))
    return false;
  return true;
}

static void BuildVisibleModelList(
    const SimBackgroundVoxelRenderParams *params,
    SimBackgroundVisibleModelList *list) {
  list->count = 0;
  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  for (uint16_t i = 0; i < scene->object_count; i++) {
    const SimBackgroundVoxelObject *object = &scene->objects[i];
    SimBackgroundVisibleModel entry = {
      .index = i,
      .depth = ObjectGroundDepth(object, params),
      .lean = CameraFacingModelLean(
          params, (SimBackgroundVoxelKind)object->kind),
    };
    if (!ObjectMayBeVisible(object, params, &entry.lean)) continue;
    int insert = list->count;
    while (insert > 0 &&
           (list->entries[insert - 1].depth < entry.depth ||
            (list->entries[insert - 1].depth == entry.depth &&
             scene->objects[list->entries[insert - 1].index].cell_x >
                 object->cell_x))) {
      list->entries[insert] = list->entries[insert - 1];
      insert--;
    }
    list->entries[insert] = entry;
    list->count++;
  }
}

static void DrawModelDepthRange(
    SDL_Renderer *renderer, const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundVisibleModelList *list,
    int mountain_relief_count,
    float minimum_depth, float maximum_depth) {
  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  SimBackgroundGeometryBatch *batch = &g_renderer_state.batch;
  batch->vertex_count = 0;
  batch->index_count = 0;
  for (int at = 0; at < mountain_relief_count; at++) {
    const ProjectedMountainReliefFace *face =
        &g_renderer_state.mountain_projected[at];
    if (!SimBackgroundVoxelDepth_Contains(
            face->depth, minimum_depth, maximum_depth))
      continue;
    AppendProjectedMountainReliefFace(renderer, batch, face);
  }
  /* Textured relief and palette geometry cannot share one SDL batch. Flush
   * the authentic mountain copies before switching to solid model faces. */
  FlushBatchTexture(renderer, batch, g_renderer_state.mountain_art);
  for (uint16_t at = 0; at < list->count; at++) {
    const SimBackgroundVisibleModel *entry = &list->entries[at];
    if (!SimBackgroundVoxelDepth_Contains(
            entry->depth, minimum_depth, maximum_depth))
      continue;
    uint16_t index = entry->index;
    const SimBackgroundVoxelObject *object = &scene->objects[index];
    DrawModel(renderer, batch, object, &g_renderer_state.palettes[index],
              params, &entry->lean);
  }
  FlushBatch(renderer, batch);
}

static SDL_Texture *EnsureSupersampleTarget(SDL_Renderer *renderer,
                                            int width, int height) {
  if (g_renderer_state.supersample &&
      g_renderer_state.supersample_w == width &&
      g_renderer_state.supersample_h == height)
    return g_renderer_state.supersample;
  if (g_renderer_state.supersample) {
    SDL_DestroyTexture(g_renderer_state.supersample);
    g_renderer_state.supersample = NULL;
  }
  g_renderer_state.supersample_w = width;
  g_renderer_state.supersample_h = height;
  if (g_renderer_state.supersample_allocation_failed) return NULL;
  g_renderer_state.supersample = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
      width, height);
  if (!g_renderer_state.supersample) {
    g_renderer_state.supersample_allocation_failed = true;
    fprintf(stderr, "[sim-bg-voxels] 2x target allocation failed: %s\n",
            SDL_GetError());
    return NULL;
  }
  SDL_SetTextureBlendMode(g_renderer_state.supersample, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(g_renderer_state.supersample, SDL_SCALEMODE_LINEAR);
  return g_renderer_state.supersample;
}

typedef struct SimBackgroundDrawTargetState {
  bool supersampled;
  SDL_Texture *saved_target;
  bool clipped;
  SDL_Rect saved_clip;
  uint8_t red, green, blue, alpha;
} SimBackgroundDrawTargetState;

static void BeginDrawTarget(
    SDL_Renderer *renderer, const SimBackgroundVoxelRenderParams *params,
    SimBackgroundDrawTargetState *state,
    SimBackgroundVoxelRenderParams *draw_params) {
  *draw_params = *params;
  *state = (SimBackgroundDrawTargetState){0};
  bool supersample = renderer && params &&
      params->render_scale == kSimBackgroundVoxelRenderScale_2x &&
      params->detail >= kSimBackgroundVoxelDetail_High &&
      params->viewport.w > 0 && params->viewport.h > 0;
  if (!supersample) return;

  int width = params->viewport.w * 2;
  int height = params->viewport.h * 2;
  SDL_Texture *target = EnsureSupersampleTarget(renderer, width, height);
  if (!target) return;

  state->saved_target = SDL_GetRenderTarget(renderer);
  state->clipped = SDL_RenderClipEnabled(renderer);
  if (state->clipped) SDL_GetRenderClipRect(renderer, &state->saved_clip);
  state->red = state->green = state->blue = state->alpha = 255;
  SDL_GetRenderDrawColor(renderer, &state->red, &state->green,
                         &state->blue, &state->alpha);

  if (!SDL_SetRenderTarget(renderer, target)) {
    fprintf(stderr, "[sim-bg-voxels] 2x target unavailable: %s\n",
            SDL_GetError());
    SDL_DestroyTexture(g_renderer_state.supersample);
    g_renderer_state.supersample = NULL;
    g_renderer_state.supersample_allocation_failed = true;
    return;
  }
  state->supersampled = true;
  SDL_SetRenderClipRect(renderer, NULL);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
  SDL_RenderClear(renderer);
  draw_params->viewport = (SDL_Rect){0, 0, width, height};
  draw_params->render_scale = kSimBackgroundVoxelRenderScale_2x;
}

static void EndDrawTarget(
    SDL_Renderer *renderer, const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundDrawTargetState *state) {
  if (!state->supersampled) return;
  SDL_SetRenderTarget(renderer, state->saved_target);
  SDL_SetRenderClipRect(renderer,
                        state->clipped ? &state->saved_clip : NULL);
  SDL_SetRenderDrawColor(renderer, state->red, state->green,
                         state->blue, state->alpha);
  SDL_FRect destination = {
    (float)params->viewport.x, (float)params->viewport.y,
    (float)params->viewport.w, (float)params->viewport.h,
  };
  SDL_RenderTexture(renderer, g_renderer_state.supersample,
                    NULL, &destination);
}

static void GroundDepthRange(
    const SimBackgroundVoxelRenderParams *params,
    float *minimum, float *maximum) {
  *minimum = FLT_MAX;
  *maximum = -FLT_MAX;
  const float x[2] = {(float)params->source.x,
                      (float)(params->source.x + params->source.w)};
  const float y[2] = {(float)params->source.y,
                      (float)(params->source.y + params->source.h)};
  for (int yi = 0; yi < 2; yi++)
    for (int xi = 0; xi < 2; xi++) {
      float world_x, world_y, world_z;
      TexturePointToWorld(params, x[xi], y[yi], 0.0f,
                          &world_x, &world_y, &world_z);
      float depth = Scene3D_ClipDepth(
          params->matrix, world_x, world_y, world_z);
      if (depth < *minimum) *minimum = depth;
      if (depth > *maximum) *maximum = depth;
    }
}

static void DrawDepthLayers(
    SDL_Renderer *renderer, const SimBackgroundVoxelRenderParams *params,
    SimBackgroundVoxelDepthLayerCallback callback, void *userdata,
    bool interleaved) {
  if (!RenderParamsValid(renderer, params)) return;
  SimBackgroundDrawTargetState target_state;
  SimBackgroundVoxelRenderParams draw_params;
  BeginDrawTarget(renderer, params, &target_state, &draw_params);

  g_renderer_state.cache_stamp++;
  if (!g_renderer_state.cache_stamp) g_renderer_state.cache_stamp = 1;
  SimBackgroundVisibleModelList list;
  BuildVisibleModelList(&draw_params, &list);
  int mountain_relief_count =
      BuildProjectedMountainReliefFaces(&draw_params);
  float visible_minimum, visible_maximum;
  GroundDepthRange(&draw_params, &visible_minimum, &visible_maximum);
  uint8_t slice_count = interleaved
      ? SimBackgroundVoxelDepth_SliceCount(
            (SimBackgroundVoxelDetail)params->detail)
      : 1;
  if (visible_maximum - visible_minimum < 0.0001f) slice_count = 1;
  for (uint8_t slice = 0; slice < slice_count; slice++) {
    float minimum, maximum;
    SimBackgroundVoxelDepth_SliceRange(
        visible_minimum, visible_maximum, slice_count, slice,
        &minimum, &maximum);
    DrawModelDepthRange(
        renderer, &draw_params, &list, mountain_relief_count,
        minimum, maximum);
    if (callback)
      callback(userdata, &draw_params, minimum, maximum, false);
  }
  if (callback)
    callback(userdata, &draw_params, 0.0f, 0.0f, true);
  EndDrawTarget(renderer, params, &target_state);
}

void SimBackgroundVoxelRenderer_Draw(
    SDL_Renderer *renderer, const SimBackgroundVoxelRenderParams *params) {
  DrawDepthLayers(renderer, params, NULL, NULL, false);
}

void SimBackgroundVoxelRenderer_DrawInterleaved(
    SDL_Renderer *renderer, const SimBackgroundVoxelRenderParams *params,
    SimBackgroundVoxelDepthLayerCallback callback, void *userdata) {
  DrawDepthLayers(renderer, params, callback, userdata, true);
}

static void AppendSolidQuad(SimBackgroundGeometryBatch *batch,
                            const Scene3DPoint points[4]) {
  if (batch->vertex_count + 4 > kMaxVertices ||
      batch->index_count + 6 > kMaxIndices)
    return;
  const SDL_FColor black = {0.0f, 0.0f, 0.0f, 1.0f};
  int base = batch->vertex_count;
  for (int i = 0; i < 4; i++)
    batch->vertices[batch->vertex_count++] =
        (SDL_Vertex){{points[i].x, points[i].y}, black, {0, 0}};
  static const int indices[] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < 6; i++)
    batch->indices[batch->index_count++] = base + indices[i];
}

typedef struct SimBackgroundShadowBounds {
  float x0, y0, x1, y1, height;
} SimBackgroundShadowBounds;

enum { kSimBackgroundMaxShadowVolumes = 3 };

static int ShadowBounds(const SimBackgroundVoxelObject *object,
                        SimBackgroundShadowBounds *out) {
  bool construction =
      (object->flags & kSimBackgroundVoxel_UnderConstruction) != 0;
  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House:
      out[0] = (SimBackgroundShadowBounds){
        1.0f, 2.0f, 15.0f, 15.5f, construction ? 12.0f : 15.5f};
      return 1;
    case kSimBackgroundVoxel_Cathedral:
      out[0] = (SimBackgroundShadowBounds){
        0.0f, 8.5f, 32.0f, 32.0f, 24.0f};
      return 1;
    case kSimBackgroundVoxel_Windmill:
      out[0] = (SimBackgroundShadowBounds){
        5.0f, 2.0f, 27.0f, 16.0f, construction ? 24.0f : 32.0f};
      return 1;
    case kSimBackgroundVoxel_Factory:
      if (construction) {
        out[0] = (SimBackgroundShadowBounds){
          0.5f, 0.5f, 31.5f, 32.0f, 10.0f};
        return 1;
      }
      /* Match the authored sideways-U mass instead of darkening its grass
       * courtyard with the old full-footprint approximation. */
      out[0] = (SimBackgroundShadowBounds){
        0.5f, 0.5f, 21.5f, 11.5f, 17.0f};
      out[1] = (SimBackgroundShadowBounds){
        0.5f, 20.5f, 21.5f, 31.5f, 17.0f};
      out[2] = (SimBackgroundShadowBounds){
        21.5f, 0.5f, 31.5f, 31.5f, 17.0f};
      return 3;
    case kSimBackgroundVoxel_Tree:
      out[0] = (SimBackgroundShadowBounds){
        0.0f, 0.0f, 16.0f, 16.0f, 15.0f};
      return 1;
  }
  return 0;
}

static void AppendShadowVolume(
    SimBackgroundGeometryBatch *batch,
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundVoxelObject *object,
    SimBackgroundShadowBounds bounds, float axis_z_scale,
    float light_x, float light_y) {
  if (bounds.x1 <= bounds.x0 || bounds.y1 <= bounds.y0 ||
      bounds.height <= 0.0f)
    return;
  const SimBackgroundVoxelProportions *proportions =
      SimBackgroundVoxelProportions_Get(
          (SimBackgroundVoxelKind)object->kind);
  float center_x = object->footprint_cells_w *
      kSimBackgroundCellPixels * 0.5f;
  float center_y = object->footprint_cells_d *
      kSimBackgroundCellPixels * 0.5f;
  float local_x[2] = {
    center_x + (bounds.x0 - center_x) *
        proportions->footprint_scale,
    center_x + (bounds.x1 - center_x) *
        proportions->footprint_scale,
  };
  float local_y[2] = {
    center_y + (bounds.y0 - center_y) *
        proportions->footprint_scale,
    center_y + (bounds.y1 - center_y) *
        proportions->footprint_scale,
  };
  float origin_x = (float)params->town_screen_x0 - params->camera_x +
      object->cell_x * kSimBackgroundCellPixels;
  float origin_y = -(float)params->camera_y + ObjectOriginY(object);
  float height_world = bounds.height * proportions->height_scale *
      axis_z_scale / params->source.h;
  Scene3DPoint base[4], offset[4];
  for (int corner = 0; corner < 4; corner++) {
    float world_x, world_y, world_z;
    TexturePointToWorld(
        params, origin_x + local_x[corner == 1 || corner == 2],
        origin_y + local_y[corner >= 2], 0.0f,
        &world_x, &world_y, &world_z);
    if (!Scene3D_ProjectWorldPoint(
            params->matrix, world_x, world_y, 0.0f,
            params->viewport.w, params->viewport.h, &base[corner]) ||
        !Scene3D_ProjectShadowPoint(
            params->matrix, world_x, world_y, height_world,
            light_x, light_y, params->viewport.w, params->viewport.h,
            &offset[corner]))
      return;
    base[corner].x += params->viewport.x;
    base[corner].y += params->viewport.y;
    offset[corner].x += params->viewport.x;
    offset[corner].y += params->viewport.y;
  }
  AppendSolidQuad(batch, base);
  AppendSolidQuad(batch, offset);
  for (int edge = 0; edge < 4; edge++) {
    int next = (edge + 1) & 3;
    Scene3DPoint bridge[4] = {
      base[edge], base[next], offset[next], offset[edge],
    };
    if (!IsDegenerate(bridge)) AppendSolidQuad(batch, bridge);
  }
}

void SimBackgroundVoxelRenderer_DrawShadowMask(
    SDL_Renderer *renderer, const SimBackgroundVoxelRenderParams *params,
    float light_x, float light_y) {
  if (!renderer || !params || !params->matrix || params->source.w <= 0 ||
      params->source.h <= 0 || params->viewport.w <= 0 ||
      params->viewport.h <= 0 ||
      !SimBackgroundVoxelRenderer_Ready(params->serial))
    return;
  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  SimBackgroundGeometryBatch *batch = &g_renderer_state.batch;
  batch->vertex_count = 0;
  batch->index_count = 0;
  for (uint16_t i = 0; i < scene->object_count; i++) {
    const SimBackgroundVoxelObject *object = &scene->objects[i];
    SimBackgroundModelLean lean = CameraFacingModelLean(
        params, (SimBackgroundVoxelKind)object->kind);
    if (!ObjectMayBeVisible(object, params, &lean)) continue;
    float aspect = (float)params->viewport.w / params->viewport.h;
    float x_metric = aspect * params->source.h / params->source.w;
    float lean_x = lean.x_per_height * x_metric;
    float lean_length = sqrtf(lean_x * lean_x +
                              lean.y_per_height * lean.y_per_height);
    float axis_z_scale = 1.0f / sqrtf(1.0f + lean_length * lean_length);
    SimBackgroundShadowBounds bounds[kSimBackgroundMaxShadowVolumes];
    int volume_count = ShadowBounds(object, bounds);
    for (int volume = 0; volume < volume_count; volume++)
      AppendShadowVolume(batch, params, object, bounds[volume], axis_z_scale,
                         light_x, light_y);
  }
  FlushBatch(renderer, batch);
}

void SimBackgroundVoxelRenderer_Reset(void) {
  if (g_renderer_state.ground) SDL_DestroyTexture(g_renderer_state.ground);
  if (g_renderer_state.mountain_art)
    SDL_DestroyTexture(g_renderer_state.mountain_art);
  if (g_renderer_state.supersample)
    SDL_DestroyTexture(g_renderer_state.supersample);
  g_renderer_state.ground = NULL;
  g_renderer_state.mountain_art = NULL;
  g_renderer_state.supersample = NULL;
  g_renderer_state.supersample_w = 0;
  g_renderer_state.supersample_h = 0;
  g_renderer_state.uploaded_serial = 0;
  g_renderer_state.allocation_failed = false;
  g_renderer_state.mountain_art_allocation_failed = false;
  g_renderer_state.supersample_allocation_failed = false;
  g_renderer_state.cache_stamp = 0;
  g_renderer_state.biome = kSimBackgroundVoxelBiome_Temperate;
  SimBackgroundVoxelModelCache_Reset();
  g_renderer_state.batch.vertex_count = 0;
  g_renderer_state.batch.index_count = 0;
}
