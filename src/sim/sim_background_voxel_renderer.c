#include "sim_background_voxel_renderer.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "scene3d_math.h"
#include "sim3d_depth_pass.h"
#include "sim_background_mountain_objects.h"
#include "sim_background_mountain_relief.h"
#include "sim_background_voxel_biome.h"
#include "sim_background_voxel_lighting.h"
#include "sim_background_voxel_lod.h"
#include "sim_background_voxel_model_cache.h"
#include "sim_background_voxel_models.h"
#include "sim_background_voxel_palette.h"
#include "sim_background_voxel_proportions.h"
#include "sim_background_voxel_region.h"
#include "sim_background_voxels.h"

enum {
  /* Authored tree clusters remain under 100 faces at Ultra. This batch normally
   * covers an entire viewport in one call, while retaining a safe flush path
   * for unusually broad views. */
  kMaxGeometryQuads = 8192,
  kMaxVertices = kMaxGeometryQuads * 4,
  kMaxIndices = kMaxGeometryQuads * 6,
  /* Original cells plus the complete bounded set of reconstructed cap tiles. */
  kMaxMountainReliefCells = kSimBackgroundMountainCellCount +
      kSimBackgroundMountainMaxCapTiles,
  kMaxMountainReliefFaces = kMaxMountainReliefCells *
      kSimBackgroundMountainReliefMaxStackLayers,
};

/* Contact shading is a ground decal. It is lifted by a fraction of a source
 * pixel so it wins the depth test against the ground plane it sits on without
 * being separable from it at any supported zoom. */
static const float kContactLiftPixels = 0.06f;

typedef struct SimBackgroundGeometryBatch {
  SDL_Vertex vertices[kMaxVertices];
  int indices[kMaxIndices];
  int vertex_count, index_count;
} SimBackgroundGeometryBatch;

typedef struct ProjectedModelFace {
  Scene3DPoint points[4];
  float gpu_depth[4];
  uint8_t material;
  uint8_t brightness[4];
} ProjectedModelFace;

typedef struct ProjectedMountainReliefFace {
  Scene3DPoint points[4];
  float gpu_depth[4];
  SDL_FPoint uv[4];
  uint8_t brightness;
  uint8_t alpha[4];
} ProjectedMountainReliefFace;

typedef struct SimBackgroundModelLean {
  float x_per_height;
  float y_per_height;
} SimBackgroundModelLean;

typedef struct SimBackgroundProjectionAxis {
  float x_per_height;
  float y_per_height;
  float height_scale;
} SimBackgroundProjectionAxis;

static const SimBackgroundProjectionAxis kUprightProjectionAxis = {
  0.0f, 0.0f, 1.0f,
};

static struct {
  SDL_Texture *ground;
  uint32_t uploaded_serial;
  bool allocation_failed;
  uint32_t cache_stamp;
  SimBackgroundVoxelBiome biome;
  ProjectedMountainReliefFace
      mountain_projected[kMaxMountainReliefFaces];
  /* Exact per-column silhouette tops keep every repeated mountain copy
   * converged at its own local peak, even inside one connected range. */
  int16_t mountain_peak_y
      [kSimBackgroundMountainCellCount + 1]
      [kSimBackgroundMountainTownCells];
  /* A connected range may contain several overlapping peaks whose feet land
   * on different map rows. Per-column bases keep those local contacts on the
   * ground instead of lifting every peak to the component's lowest row. */
  int16_t mountain_base_y
      [kSimBackgroundMountainCellCount + 1]
      [kSimBackgroundMountainTownCells];
  SimBackgroundVoxelPalette palettes[kSimBackgroundMaxObjects];
  SimBackgroundGeometryBatch batch;
} g_renderer_state;

static SDL_Texture *CreateCanvasTexture(
    SDL_Renderer *renderer, SDL_ScaleMode scale_mode) {
  SDL_Texture *texture = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kSimTownCanvasPixels, kSimTownCanvasPixels);
  if (!texture) return NULL;
  if (!SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND) ||
      !SDL_SetTextureScaleMode(texture, scale_mode)) {
    SDL_DestroyTexture(texture);
    return NULL;
  }
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

  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  bool mountains_required = scene->mountains.cell_count != 0;
  if (mountains_required && !Sim3DDepthPass_UploadMountainAtlas(
          renderer, SimBackgroundVoxels_AtlasPixels(),
          kSimTownCanvasPixels, kSimTownCanvasPixels, pitch)) {
    fprintf(stderr, "[sim-bg-voxels] GPU mountain atlas upload failed: %s\n",
            SDL_GetError());
    g_renderer_state.allocation_failed = true;
    return;
  }

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
      [kSimBackgroundVoxel_Palm] = 0.32f,
      [kSimBackgroundVoxel_BroadTree] = 0.30f,
      [kSimBackgroundVoxel_Shrub] = 0.26f,
      [kSimBackgroundVoxel_StoryTree] = 0.28f,
      [kSimBackgroundVoxel_BloodpoolCastle] = 0.38f,
      [kSimBackgroundVoxel_MarahnaTemple] = 0.38f,
      /* A pyramid has no facade to hold upright; leaning it only shears the
       * one silhouette the player recognizes it by. */
      [kSimBackgroundVoxel_Pyramid] = 0.20f,
    };
    if (kind >= kSimBackgroundVoxel_House &&
        kind < kSimBackgroundVoxelKindCount)
      billboard_blend = blend[kind];
  }
  return CameraFacingLean(params, billboard_blend);
}

static SimBackgroundProjectionAxis ProjectionAxis(
    const SimBackgroundVoxelRenderParams *params,
    SimBackgroundModelLean lean) {
  /* Normalize a camera-facing axis once per object kind, not once for every
   * projected vertex. This preserves the rotation-like height correction
   * while removing thousands of square roots from a developed-town frame. */
  float aspect = (float)params->viewport.w / params->viewport.h;
  float x_metric = aspect * params->source.h / params->source.w;
  float lean_x = lean.x_per_height * x_metric;
  float lean_length = sqrtf(
      lean_x * lean_x + lean.y_per_height * lean.y_per_height);
  float height_scale = 1.0f / sqrtf(1.0f + lean_length * lean_length);
  return (SimBackgroundProjectionAxis){
    lean.x_per_height * height_scale,
    lean.y_per_height * height_scale,
    height_scale,
  };
}

static void ResolveProjectionAxes(
    const SimBackgroundVoxelRenderParams *params,
    SimBackgroundProjectionAxis axes[kSimBackgroundVoxelKindCount]) {
  for (int kind = 0; kind < kSimBackgroundVoxelKindCount; kind++)
    axes[kind] = ProjectionAxis(
        params, CameraFacingModelLean(
            params, (SimBackgroundVoxelKind)kind));
}

/* Leans a model-local point and maps it into world units. Kept separate so the
 * screen position and the GPU depth of one vertex are derived from a single
 * transform instead of repeating it per output. */
static void LeanedPointToWorld(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float texture_x, float texture_y, float height_pixels,
    float *world_x, float *world_y, float *world_z) {
  TexturePointToWorld(
      params,
      texture_x + height_pixels * axis->x_per_height,
      texture_y + height_pixels * axis->y_per_height,
      height_pixels * axis->height_scale,
      world_x, world_y, world_z);
}

static bool ProjectWorldToScreen(
    const SimBackgroundVoxelRenderParams *params,
    float world_x, float world_y, float world_z, Scene3DPoint *out) {
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
  return true;
}

static bool ProjectPoint(const SimBackgroundVoxelRenderParams *params,
                         const SimBackgroundProjectionAxis *axis,
                         float texture_x, float texture_y, float height_pixels,
                         Scene3DPoint *out, float *clip_depth) {
  float world_x, world_y, world_z;
  LeanedPointToWorld(params, axis, texture_x, texture_y, height_pixels,
                     &world_x, &world_y, &world_z);
  if (!ProjectWorldToScreen(params, world_x, world_y, world_z, out))
    return false;
  if (clip_depth)
    *clip_depth = Scene3D_ClipDepth(
        params->matrix, world_x, world_y, world_z);
  return true;
}

/* Screen position and GPU depth for one submitted vertex. Every face vertex
 * needs both; computing them from two entry points repeated the lean, the
 * world mapping and the clip-W row of the matrix for every vertex of every
 * face in the town. */
static bool ProjectVertex(const SimBackgroundVoxelRenderParams *params,
                          const SimBackgroundProjectionAxis *axis,
                          float texture_x, float texture_y,
                          float height_pixels,
                          Scene3DPoint *out, float *gpu_depth) {
  float world_x, world_y, world_z;
  LeanedPointToWorld(params, axis, texture_x, texture_y, height_pixels,
                     &world_x, &world_y, &world_z);
  return ProjectWorldToScreen(params, world_x, world_y, world_z, out) &&
      Scene3D_NormalizedDepth(
          params->matrix, world_x, world_y, world_z, gpu_depth);
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
    const ProjectedModelFace *face,
    const SimBackgroundVoxelPalette *palette,
    SimBackgroundVoxelShading shading) {
  if (!Sim3DDepthPass_IsCollecting()) return;
  Sim3DDepthVertex vertices[4];
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
    vertices[i] = (Sim3DDepthVertex){
      .x = face->points[i].x,
      .y = face->points[i].y,
      .depth = face->gpu_depth[i],
      .color = VertexColour(argb, brightness),
      .uv = {-1.0f, -1.0f},
    };
  }
  Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Solid, vertices);
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
  float origin_x = (float)params->town_screen_x0 - params->camera_x;
  float origin_y = -(float)params->camera_y;
  float center = kSimTownCanvasPixels * 0.5f;
  Scene3DPoint bottom, top;
  if (!ProjectPoint(params, &kUprightProjectionAxis,
                    origin_x + center, origin_y + center,
                    0.0f, &bottom, NULL) ||
      !ProjectPoint(params, &kUprightProjectionAxis,
                    origin_x + center, origin_y + center,
                    24.0f, &top, NULL))
    return requested;
  float dx = top.x - bottom.x, dy = top.y - bottom.y;
  float projected_height = sqrtf(dx * dx + dy * dy);
  if (params->render_scale == kSimBackgroundVoxelRenderScale_2x)
    projected_height *= 0.5f;
  return SimBackgroundVoxelLod_Resolve(
      requested, kSimBackgroundVoxelLod_Adaptive, projected_height);
}

static void AddProjectedMountainReliefFace(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float origin_x, float origin_y,
    const float local_x[4], const float local_y[4],
    const float local_z[4], const SDL_FPoint uv[4],
    uint8_t brightness, const uint8_t alpha[4],
    int *count) {
  if (*count >= kMaxMountainReliefFaces) return;
  ProjectedMountainReliefFace face = {.brightness = brightness};
  for (int point = 0; point < 4; point++) {
    if (!ProjectVertex(params, axis,
                       origin_x + local_x[point],
                       origin_y + local_y[point], local_z[point],
                       &face.points[point], &face.gpu_depth[point]))
      return;
    face.uv[point] = uv[point];
    face.alpha[point] = alpha[point];
  }
  if (IsDegenerate(face.points)) return;
  g_renderer_state.mountain_projected[(*count)++] = face;
}

static void AppendProjectedSolidEffectFace(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float origin_x, float origin_y,
    const float local_x[4], const float local_y[4],
    const float local_z[4], SDL_FColor color) {
  if (!Sim3DDepthPass_IsCollecting()) return;
  Scene3DPoint points[4];
  Sim3DDepthVertex vertices[4];
  static const SDL_FPoint uv[4] = {
    {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f},
  };
  for (int point = 0; point < 4; point++) {
    float depth;
    if (!ProjectVertex(params, axis,
                       origin_x + local_x[point],
                       origin_y + local_y[point], local_z[point],
                       &points[point], &depth))
      return;
    vertices[point] = (Sim3DDepthVertex){
      .x = points[point].x,
      .y = points[point].y,
      .depth = depth,
      .color = color,
      .uv = uv[point],
    };
  }
  if (!IsDegenerate(points))
    Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Effect, vertices);
}

static void AppendProjectedSolidEffectBox(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float origin_x, float origin_y,
    float x0, float y0, float z0,
    float x1, float y1, float z1,
    SDL_FColor color) {
  static const uint8_t face[5][4] = {
    {4, 5, 6, 7},  /* top */
    {1, 0, 4, 5},  /* north */
    {2, 1, 5, 6},  /* east */
    {3, 2, 6, 7},  /* south */
    {0, 3, 7, 4},  /* west */
  };
  const float x[8] = {x0, x1, x1, x0, x0, x1, x1, x0};
  const float y[8] = {y0, y0, y1, y1, y0, y0, y1, y1};
  const float z[8] = {z0, z0, z0, z0, z1, z1, z1, z1};
  for (int side = 0; side < 5; side++) {
    float face_x[4], face_y[4], face_z[4];
    SDL_FColor shaded = color;
    if (side) {
      float shade = side == 3 ? 0.92f : side == 2 ? 0.84f : 0.76f;
      shaded.r *= shade;
      shaded.g *= shade;
      shaded.b *= shade;
    }
    for (int point = 0; point < 4; point++) {
      int corner = face[side][point];
      face_x[point] = x[corner];
      face_y[point] = y[corner];
      face_z[point] = z[corner];
    }
    AppendProjectedSolidEffectFace(
        params, axis, origin_x, origin_y,
        face_x, face_y, face_z, shaded);
  }
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

static bool MountainCapSource(
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainCapTile *tile,
    int *source_cell_x, int *source_cell_y) {
  *source_cell_x = tile->source_cell_x;
  *source_cell_y = tile->source_cell_y;
  return (*source_cell_x >= 0 && *source_cell_y >= 0) ||
      SimBackgroundMountains_TileSource(
          field, tile->source_tile, source_cell_x, source_cell_y);
}

enum {
  /* The lava blob authored into the $70/$71 crown occupies source pixels
   * x 138-149, y 136-141. The stamp's own origin is cell (6,8), so the blob's
   * centre is exactly the crown pair's shared edge, eleven pixels down. */
  kCraterCentreOffsetY = 11,
  kCraterSourceRadiusX = 6,
  kCraterSourceRadiusY = 3,
};

/* One flat elliptical fan on the crater plane. Each quad spans two octagon
 * segments so no submitted face collapses to a triangle, and the octagon
 * matches the stepped-but-round language the voxel models already use. */
static void AppendCraterGlowRing(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float origin_x, float origin_y,
    float centre_x, float centre_y, float z,
    float radius_x, float radius_y, SDL_FColor colour) {
  static const float unit[8][2] = {
    {0.0f, -1.0f}, {0.7071f, -0.7071f}, {1.0f, 0.0f},
    {0.7071f, 0.7071f}, {0.0f, 1.0f}, {-0.7071f, 0.7071f},
    {-1.0f, 0.0f}, {-0.7071f, -0.7071f},
  };
  for (int segment = 0; segment < 8; segment += 2) {
    float local_x[4] = {centre_x};
    float local_y[4] = {centre_y};
    float local_z[4] = {z, z, z, z};
    for (int step = 0; step < 3; step++) {
      const float *point = unit[(segment + step) & 7];
      local_x[step + 1] = centre_x + point[0] * radius_x;
      local_y[step + 1] = centre_y + point[1] * radius_y;
    }
    AppendProjectedSolidEffectFace(
        params, axis, origin_x, origin_y,
        local_x, local_y, local_z, colour);
  }
}

static void AppendVolcanoEffects(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    const SimBackgroundMountainRelief *relief,
    const SimBackgroundMountainObject *object,
    float origin_x, float origin_y, float baseline, float height_scale) {
  if (!(object->flags & kSimBackgroundMountainObject_Volcano) ||
      params->detail < kSimBackgroundVoxelDetail_Balanced ||
      params->style < kSimBackgroundVoxelStyle_Trim)
    return;

  /* The glow belongs on the authored blob's centre, not on the crown row's
   * top edge: the old placement pushed it a full three pixels past the peak
   * and left it hanging over the grass behind the mountain. */
  float crater_x =
      (object->cell_x + object->width_cells * 0.5f) *
      kSimBackgroundCellPixels;
  float crater_source_y =
      object->cell_y * kSimBackgroundCellPixels + kCraterCentreOffsetY;
  float crater_y, crater_z, ignored_x, rim_y, rim_z;
  MountainPlanePoint(
      crater_x, crater_source_y, baseline, relief, 0.0f, 0.0f,
      &ignored_x, &crater_y, &crater_z);
  /* Take the depth radius from the same plane mapping as the face it sits on,
   * so the ellipse keeps hugging the blob whenever relief is retuned. */
  MountainPlanePoint(
      crater_x, crater_source_y - kCraterSourceRadiusY, baseline, relief,
      0.0f, 0.0f, &ignored_x, &rim_y, &rim_z);
  float radius_y = crater_y - rim_y;
  if (radius_y < 0.5f) radius_y = 0.5f;
  crater_z *= height_scale;

  /* The source crater flashes on an eight-frame cadence. Match that cadence
   * with two shallow depth-tested glow rings rather than a screen-space
   * bloom, so nearby peaks still occlude the light correctly. Rings, not
   * rectangles: a squared-off slab of lava is the one shape the 12x6 pixel
   * blob never has. */
  bool flash_on = ((params->game_frame >> 3) & 1u) == 0;
  if (flash_on) {
    AppendCraterGlowRing(
        params, axis, origin_x, origin_y, crater_x, crater_y,
        crater_z + 0.35f, (float)kCraterSourceRadiusX, radius_y,
        (SDL_FColor){1.0f, 0.20f, 0.02f, 0.36f});
    AppendCraterGlowRing(
        params, axis, origin_x, origin_y, crater_x, crater_y,
        crater_z + 0.55f, kCraterSourceRadiusX * 0.55f, radius_y * 0.55f,
        (SDL_FColor){1.0f, 0.56f, 0.08f, 0.84f});
  }

  if (params->detail < kSimBackgroundVoxelDetail_High ||
      params->style < kSimBackgroundVoxelStyle_Architectural)
    return;
  int puff_count = params->detail == kSimBackgroundVoxelDetail_Ultra ? 4 : 2;
  for (int puff = 0; puff < puff_count; puff++) {
    unsigned age = (params->game_frame + (unsigned)puff * 17u) % 48u;
    if (age > 34u) continue;
    float age_fraction = age / 34.0f;
    float size = 2.4f + age_fraction * 2.8f;
    float drift_x = ((puff & 1) ? 1.0f : -1.0f) * age_fraction * 5.0f;
    float drift_y = -age_fraction * 4.0f;
    float z0 = crater_z + 2.0f + age_fraction * 17.0f;
    float opacity = 0.72f - age_fraction * 0.34f;
    SDL_FColor smoke = {
      0.66f + age_fraction * 0.10f,
      0.63f + age_fraction * 0.10f,
      0.58f + age_fraction * 0.10f,
      opacity,
    };
    AppendProjectedSolidEffectBox(
        params, axis, origin_x, origin_y,
        crater_x + drift_x - size * 0.5f,
        crater_y + drift_y - size * 0.5f, z0,
        crater_x + drift_x + size * 0.5f,
        crater_y + drift_y + size * 0.5f, z0 + size,
        smoke);
  }
}

static void RecordMountainPeakColumn(
    uint16_t component, int destination_cell_x, int destination_cell_y,
    int source_cell_x, int source_cell_y) {
  if (!component || component > kSimBackgroundMountainCellCount ||
      destination_cell_x < 0 ||
      destination_cell_x >= kSimBackgroundMountainTownCells ||
      source_cell_x < 0 ||
      source_cell_x >= kSimBackgroundMountainTownCells ||
      source_cell_y < 0 ||
      source_cell_y >= kSimBackgroundMountainTownCells)
    return;
  const uint32_t *atlas = SimBackgroundVoxels_AtlasPixels();
  int peak_y = INT_MAX;
  int source_x0 = source_cell_x * kSimBackgroundCellPixels;
  int source_y0 = source_cell_y * kSimBackgroundCellPixels;
  for (int y = 0; y < kSimBackgroundCellPixels; y++)
    for (int x = 0; x < kSimBackgroundCellPixels; x++) {
      size_t at = (size_t)(source_y0 + y) * kSimTownCanvasPixels +
          (size_t)(source_x0 + x);
      if ((atlas[at] >> 24) == 0) continue;
      int destination_y =
          destination_cell_y * kSimBackgroundCellPixels + y;
      if (destination_y < peak_y) peak_y = destination_y;
    }
  if (peak_y <
      g_renderer_state.mountain_peak_y[component][destination_cell_x])
    g_renderer_state.mountain_peak_y[component][destination_cell_x] =
        (int16_t)peak_y;
}

static void BuildMountainPeakColumns(
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainCaps *caps) {
  for (int component = 0;
       component <= kSimBackgroundMountainCellCount; component++)
    for (int x = 0; x < kSimBackgroundMountainTownCells; x++)
      g_renderer_state.mountain_peak_y[component][x] = INT16_MAX;
  for (int y = 0; y < kSimBackgroundMountainTownCells; y++)
    for (int x = 0; x < kSimBackgroundMountainTownCells; x++) {
      if (!SimBackgroundMountains_CellOccupied(field, x, y)) continue;
      int cell = y * kSimBackgroundMountainTownCells + x;
      uint16_t component = field->component[cell];
      if (!component) component = 1;
      RecordMountainPeakColumn(component, x, y, x, y);
    }
  for (uint8_t at = 0; at < caps->tile_count; at++) {
    const SimBackgroundMountainCapTile *tile = &caps->tiles[at];
    int source_cell_x, source_cell_y;
    if (!MountainCapSource(
            field, tile, &source_cell_x, &source_cell_y))
      continue;
    uint16_t component = tile->component ? tile->component : 1;
    RecordMountainPeakColumn(
        component, tile->cell_x, tile->cell_y,
        source_cell_x, source_cell_y);
  }
}

static void BuildMountainBaseColumns(
    const SimBackgroundMountainField *field) {
  for (int component = 0;
       component <= kSimBackgroundMountainCellCount; component++)
    for (int x = 0; x < kSimBackgroundMountainTownCells; x++)
      g_renderer_state.mountain_base_y[component][x] = INT16_MIN;
  for (int y = 0; y < kSimBackgroundMountainTownCells; y++)
    for (int x = 0; x < kSimBackgroundMountainTownCells; x++) {
      if (!SimBackgroundMountains_CellOccupied(field, x, y)) continue;
      int cell = y * kSimBackgroundMountainTownCells + x;
      uint16_t component = field->component[cell];
      if (!component) component = 1;
      int bottom = (y + 1) * kSimBackgroundCellPixels;
      if (bottom > g_renderer_state.mountain_base_y[component][x])
        g_renderer_state.mountain_base_y[component][x] = (int16_t)bottom;
    }
}

static float MountainEdgeBaseline(
    uint16_t component, int edge_x, float fallback) {
  int baseline = INT_MIN;
  if (component <= kSimBackgroundMountainCellCount)
    for (int side = -1; side <= 0; side++) {
      int cell_x = edge_x + side;
      if (cell_x < 0 || cell_x >= kSimBackgroundMountainTownCells)
        continue;
      int candidate =
          g_renderer_state.mountain_base_y[component][cell_x];
      if (candidate > baseline) baseline = candidate;
    }
  return baseline == INT_MIN ? fallback : (float)baseline;
}

static float MountainEdgeMaximumRise(
    uint16_t component, int edge_x, float baseline, float fallback_top) {
  int peak_y = INT16_MAX;
  if (component <= kSimBackgroundMountainCellCount)
    for (int side = -1; side <= 0; side++) {
      int cell_x = edge_x + side;
      if (cell_x < 0 || cell_x >= kSimBackgroundMountainTownCells)
        continue;
      int candidate =
          g_renderer_state.mountain_peak_y[component][cell_x];
      if (candidate < peak_y) peak_y = candidate;
    }
  float top = peak_y == INT16_MAX ? fallback_top : (float)peak_y;
  float maximum_rise = baseline - top;
  return maximum_rise > 0.0f ? maximum_rise : 1.0f;
}

static void AddMountainStackTile(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    const SimBackgroundMountainRelief *relief,
    float height_scale,
    float origin_x, float origin_y,
    float baseline_left, float baseline_right,
    float maximum_rise_left, float maximum_rise_right,
    int destination_cell_x, int destination_cell_y,
    int source_cell_x, int source_cell_y, uint8_t flags,
    int *count) {
  bool mirror_x =
      (flags & kSimBackgroundMountainCapTile_MirrorX) != 0;
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
  float source_x[4] = {x0, x1, x1, x0};
  float source_y[4] = {y0, y0, y1, y1};
  const uint8_t opaque[4] = {255, 255, 255, 255};
  /* Emit rear copies first for coherent material batching. Visibility is
   * resolved per fragment by the shared D32 target, so this loop order is not
   * a correctness dependency. */
  for (int layer = relief->stack_layer_count - 1; layer >= 0; layer--) {
    /* Every layer retains the exact front silhouette orientation. Flipping
     * only the rear copy moves off-centre tip pixels across their tile and
     * turns one peak into two visible horns. */
    bool layer_mirror_x = mirror_x;
    SDL_FPoint uv[4] = {
      {layer_mirror_x ? u1 : u0, v0},
      {layer_mirror_x ? u0 : u1, v0},
      {layer_mirror_x ? u0 : u1, v1},
      {layer_mirror_x ? u1 : u0, v1},
    };
    float local_x[4], local_y[4], local_z[4];
    for (int point = 0; point < 4; point++) {
      float baseline = point == 0 || point == 3
          ? baseline_left : baseline_right;
      float rise = baseline - source_y[point];
      float maximum_rise = point == 0 || point == 3
          ? maximum_rise_left : maximum_rise_right;
      float offset_y = SimBackgroundMountainRelief_StackOffsetY(
          relief, layer, rise, maximum_rise);
      MountainPlanePoint(
          source_x[point], source_y[point], baseline, relief,
          0.0f, offset_y,
          &local_x[point], &local_y[point], &local_z[point]);
      local_z[point] *= height_scale;
    }
    AddProjectedMountainReliefFace(
        params, axis, origin_x, origin_y,
        local_x, local_y, local_z, uv, 255, opaque,
        count);
  }
}

static int BuildProjectedMountainObjectFaces(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    const SimBackgroundMountainRelief *relief,
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainObjectList *objects,
    float origin_x, float origin_y) {
  int count = 0;
  for (uint8_t at = 0; at < objects->count; at++) {
    const SimBackgroundMountainObject *object = &objects->objects[at];
    float height_scale =
        object->flags & kSimBackgroundMountainObject_Volcano
            ? 1.12f : 1.0f;
    float baseline =
        (object->cell_y + object->height_cells) *
        (float)kSimBackgroundCellPixels;
    float maximum_rise =
        object->height_cells * (float)kSimBackgroundCellPixels;
    for (int row = 0; row < object->height_cells; row++)
      for (int column = 0; column < object->width_cells; column++) {
        if (!(object->row_occupied_mask[row] & (1u << column))) continue;
        int destination_x = object->cell_x + column;
        int destination_y = object->cell_y + row;
        /* Horizontal level edges retain their authentic half-peaks. At the
         * south edge, however, keep the rest of the independently reconstructed
         * object: clipping every row at y=32 reduced full mountains to shallow
         * disconnected caps whenever their bases extended beyond the map. */
        if (destination_x < 0 ||
            destination_x >= kSimBackgroundMountainTownCells ||
            destination_y < -kSimBackgroundMountainObjectMaxRows ||
            destination_y >= kSimBackgroundMountainTownCells +
                kSimBackgroundMountainObjectMaxRows)
          continue;
        int source_x, source_y;
        uint8_t source_tile = object->source_tile[row][column];
        if (!source_tile ||
            (!SimBackgroundVoxels_MountainTileSource(
                 source_tile, &source_x, &source_y) &&
             !SimBackgroundMountains_TileSource(
                 field, source_tile, &source_x, &source_y)))
          continue;
        AddMountainStackTile(
            params, axis, relief, height_scale, origin_x, origin_y,
            baseline, baseline, maximum_rise, maximum_rise,
            destination_x, destination_y,
            source_x, source_y, 0,
            &count);
      }
    AppendVolcanoEffects(
        params, axis, relief, object,
        origin_x, origin_y, baseline, height_scale);
  }
  return count;
}

static void AddNorthMountainCaps(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    const SimBackgroundMountainRelief *relief,
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainCaps *caps,
    const float component_bottom[kSimBackgroundMountainCellCount + 1],
    const float component_top[kSimBackgroundMountainCellCount + 1],
    float origin_x, float origin_y, int *count) {
  for (uint8_t at = 0; at < caps->tile_count; at++) {
    const SimBackgroundMountainCapTile *tile = &caps->tiles[at];
    int source_cell_x, source_cell_y;
    if (!MountainCapSource(
            field, tile, &source_cell_x, &source_cell_y))
      continue;
    uint16_t component = tile->component ? tile->component : 1;
    float maximum_rise_left = MountainEdgeMaximumRise(
        component, tile->cell_x,
        MountainEdgeBaseline(
            component, tile->cell_x, component_bottom[component]),
        component_top[component]);
    float maximum_rise_right = MountainEdgeMaximumRise(
        component, tile->cell_x + 1,
        MountainEdgeBaseline(
            component, tile->cell_x + 1, component_bottom[component]),
        component_top[component]);
    float baseline_left = MountainEdgeBaseline(
        component, tile->cell_x, component_bottom[component]);
    float baseline_right = MountainEdgeBaseline(
        component, tile->cell_x + 1, component_bottom[component]);
    AddMountainStackTile(
        params, axis, relief, 1.0f, origin_x, origin_y,
        baseline_left, baseline_right,
        maximum_rise_left, maximum_rise_right,
        tile->cell_x, tile->cell_y,
        source_cell_x, source_cell_y, tile->flags,
        count);
  }
}

static int BuildProjectedMountainReliefFaces(
    const SimBackgroundVoxelRenderParams *params) {
  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  const SimBackgroundMountainField *field = &scene->mountains;
  if (!field->cell_count) return 0;
  SimBackgroundMountainRelief relief;
  SimBackgroundMountainRelief_Resolve(
      EffectiveMountainDetail(params), &relief);
  if (!relief.stack_layer_count) return 0;

  float origin_x = (float)params->town_screen_x0 - params->camera_x;
  float origin_y = -(float)params->camera_y;
  SimBackgroundModelLean mountain_lean = CameraFacingLean(
      params, params->facing == kSimBackgroundVoxelFacing_PerModel
          ? 0.44f : 0.35f);
  SimBackgroundProjectionAxis mountain_axis =
      ProjectionAxis(params, mountain_lean);
  SimBackgroundMountainObjectList mountain_objects;
  if (SimBackgroundMountainObjects_Build(
          field, &scene->mountain_caps, &mountain_objects)) {
    return BuildProjectedMountainObjectFaces(
        params, &mountain_axis, &relief, field, &mountain_objects,
        origin_x, origin_y);
  }
  /* Each connected range shares one baseline. Mapping source Y partly into
   * height and partly into ground depth turns the original pseudo-perspective
   * art into one continuous shallow facade. */
  float component_bottom[kSimBackgroundMountainCellCount + 1] = {0};
  float component_top[kSimBackgroundMountainCellCount + 1];
  for (int component = 0;
       component <= kSimBackgroundMountainCellCount; component++) {
    component_top[component] = kSimTownCanvasPixels;
  }
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
      float top = cell_y * kSimBackgroundCellPixels;
      if (bottom > component_bottom[component])
        component_bottom[component] = bottom;
      if (top < component_top[component])
        component_top[component] = top;
    }
  for (uint8_t at = 0; at < scene->mountain_caps.tile_count; at++) {
    const SimBackgroundMountainCapTile *tile =
        &scene->mountain_caps.tiles[at];
    uint16_t component = tile->component ? tile->component : 1;
    float top = tile->cell_y * kSimBackgroundCellPixels;
    if (top < component_top[component])
      component_top[component] = top;
  }
  BuildMountainPeakColumns(field, &scene->mountain_caps);
  BuildMountainBaseColumns(field);
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
      float baseline_left = MountainEdgeBaseline(
          component, cell_x, component_bottom[component]);
      float baseline_right = MountainEdgeBaseline(
          component, cell_x + 1, component_bottom[component]);
      float maximum_rise_left = MountainEdgeMaximumRise(
          component, cell_x, baseline_left, component_top[component]);
      float maximum_rise_right = MountainEdgeMaximumRise(
          component, cell_x + 1, baseline_right, component_top[component]);
      AddMountainStackTile(
          params, &mountain_axis, &relief, 1.0f, origin_x, origin_y,
          baseline_left, baseline_right,
          maximum_rise_left, maximum_rise_right,
          cell_x, cell_y, cell_x, cell_y, 0,
          &count);
    }
  }
  AddNorthMountainCaps(
      params, &mountain_axis, &relief, field, &scene->mountain_caps,
      component_bottom, component_top,
      origin_x, origin_y, &count);
  return count;
}

static void AppendProjectedMountainReliefFace(
    const ProjectedMountainReliefFace *face) {
  if (!Sim3DDepthPass_IsCollecting()) return;
  Sim3DDepthVertex vertices[4];
  float shade = face->brightness / 255.0f;
  for (int point = 0; point < 4; point++)
    vertices[point] = (Sim3DDepthVertex){
      .x = face->points[point].x,
      .y = face->points[point].y,
      .depth = face->gpu_depth[point],
      .color = {shade, shade, shade, face->alpha[point] / 255.0f},
      .uv = face->uv[point],
    };
  Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Mountain, vertices);
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
    case kSimBackgroundVoxel_Palm:
    case kSimBackgroundVoxel_BroadTree:
      /* Contact belongs to the trunk, not the foliage footprint. This avoids
       * turning dense forests into one continuous dark carpet. */
      out[0] = (SimBackgroundContactBounds){5.4f, 5.4f, 10.6f, 10.6f};
      return 1;
    case kSimBackgroundVoxel_Shrub:
      out[0] = (SimBackgroundContactBounds){5.8f, 5.8f, 10.2f, 10.2f};
      return 1;
    case kSimBackgroundVoxel_StoryTree:
      out[0] = (SimBackgroundContactBounds){10.0f, 11.0f, 22.0f, 23.0f};
      return 1;
    case kSimBackgroundVoxel_BloodpoolCastle:
    case kSimBackgroundVoxel_MarahnaTemple:
      out[0] = (SimBackgroundContactBounds){1.0f, 3.0f, 31.0f, 31.5f};
      return 1;
    case kSimBackgroundVoxel_Pyramid:
      out[0] = (SimBackgroundContactBounds){0.5f, 1.5f, 31.5f, 31.5f};
      return 1;
  }
  return 0;
}

static void AppendGroundContact(
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
      if (!ProjectVertex(params, &kUprightProjectionAxis,
                         origin_x + local_x[point],
                         origin_y + local_y[point], kContactLiftPixels,
                         &face.points[point], &face.gpu_depth[point])) {
        valid = false;
        break;
      }
    }
    if (valid && !IsDegenerate(face.points))
      AppendProjectedFace(&face, palette,
                          (SimBackgroundVoxelShading)params->shading);
  }
}

static float ModelAuthoredHeight(const SimBackgroundVoxelObject *object) {
  return SimBackgroundVoxelRegion_AuthoredHeight(object);
}

static bool ObjectMayBeVisible(
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis) {
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
        if (!ProjectPoint(params, axis, x[xi], y[yi], z[zi],
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
    const SimBackgroundProjectionAxis *axis,
    float origin_x, float origin_y,
    const SimBackgroundVoxelProportions *proportions,
    float center_x, float center_y) {
  SimBackgroundVoxelDetail requested =
      (SimBackgroundVoxelDetail)params->detail;
  if (params->lod != kSimBackgroundVoxelLod_Adaptive) return requested;
  Scene3DPoint bottom, top;
  float height = ModelAuthoredHeight(object) * proportions->height_scale;
  if (!ProjectPoint(params, axis, origin_x + center_x, origin_y + center_y,
                    0.0f, &bottom, NULL) ||
      !ProjectPoint(params, axis, origin_x + center_x, origin_y + center_y,
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
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelPalette *palette,
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    const SimBackgroundVoxelLightDirection *light) {
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
      object, params, axis, origin_x, origin_y, proportions,
      center_x, center_y);
  const SimBackgroundVoxelModel *model = SimBackgroundVoxelModelCache_Get(
      object, detail, (SimBackgroundVoxelStyle)params->style,
      g_renderer_state.cache_stamp);
  if (!model || !model->face_count || model->overflow) return;
  AppendGroundContact(object, palette, params, origin_x, origin_y,
                      proportions);
  /* Faces are submitted as they are projected. The depth pass resolves
   * visibility per pixel, so there is nothing for an intermediate array to
   * order, and staging a whole model's worth of projected faces only cost a
   * second pass over 21KB of stack. */
  for (uint16_t face_index = 0; face_index < model->face_count; face_index++) {
    const SimBackgroundVoxelModelFace *source = &model->faces[face_index];
    ProjectedModelFace face = {
      .material = (uint8_t)SimBackgroundVoxelBiome_SurfaceMaterial(
          g_renderer_state.biome, detail,
          (SimBackgroundVoxelMaterial)source->material, source),
    };
    uint8_t directional =
        SimBackgroundVoxelLighting_FaceBrightnessWithDirection(
            source, (SimBackgroundVoxelShading)params->shading, light);
    SimBackgroundVoxelLighting_VertexBrightnesses(
        source, model, directional,
        (SimBackgroundVoxelShading)params->shading, face.brightness);
    bool valid = true;
    for (int point = 0; point < 4; point++) {
      float local_x = center_x +
          (source->points[point].x - center_x) *
              proportions->footprint_scale;
      float local_y = center_y +
          (source->points[point].y - center_y) *
              proportions->footprint_scale;
      float local_z = source->points[point].z *
          proportions->height_scale;
      if (!ProjectVertex(params, axis,
                         origin_x + local_x, origin_y + local_y, local_z,
                         &face.points[point], &face.gpu_depth[point])) {
        valid = false;
        break;
      }
    }
    if (!valid || IsDegenerate(face.points)) continue;
    AppendProjectedFace(&face, palette,
                        (SimBackgroundVoxelShading)params->shading);
  }
}

typedef struct SimBackgroundVisibleModel {
  uint16_t index;
  SimBackgroundProjectionAxis axis;
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
  SimBackgroundProjectionAxis axes[kSimBackgroundVoxelKindCount];
  ResolveProjectionAxes(params, axes);
  for (uint16_t i = 0; i < scene->object_count; i++) {
    const SimBackgroundVoxelObject *object = &scene->objects[i];
    if (object->kind >= kSimBackgroundVoxelKindCount) continue;
    SimBackgroundVisibleModel entry = {
      .index = i,
      .axis = axes[object->kind],
    };
    if (!ObjectMayBeVisible(object, params, &entry.axis)) continue;
    list->entries[list->count++] = entry;
  }
}

static void CollectDepthGeometry(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundVisibleModelList *list,
    int mountain_relief_count,
    const SimBackgroundVoxelLightDirection *light) {
  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  /* Submission order is intentionally immaterial. One opaque mountain draw
   * and one solid-model draw share the same D32 attachment; the GPU resolves
   * visibility per pixel instead of relying on CPU object/face ordering. */
  for (int at = 0; at < mountain_relief_count; at++)
    AppendProjectedMountainReliefFace(
        &g_renderer_state.mountain_projected[at]);
  for (uint16_t at = 0; at < list->count; at++) {
    const SimBackgroundVisibleModel *entry = &list->entries[at];
    uint16_t index = entry->index;
    const SimBackgroundVoxelObject *object = &scene->objects[index];
    DrawModel(object, &g_renderer_state.palettes[index],
              params, &entry->axis, light);
  }
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
  SimBackgroundVoxelRenderParams draw_params = *params;
  int output_scale = params->render_scale ==
          kSimBackgroundVoxelRenderScale_2x &&
      params->detail >= kSimBackgroundVoxelDetail_High ? 2 : 1;
  if (params->viewport.w > INT_MAX / output_scale ||
      params->viewport.h > INT_MAX / output_scale)
    return;
  draw_params.viewport = (SDL_Rect){
    0, 0,
    params->viewport.w * output_scale,
    params->viewport.h * output_scale,
  };
  SDL_ScaleMode scale_mode =
      params->render_scale == kSimBackgroundVoxelRenderScale_PixelClean
          ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR;
  if (!Sim3DDepthPass_Begin(
          renderer, draw_params.viewport.w, draw_params.viewport.h,
          scale_mode))
    return;

  g_renderer_state.cache_stamp++;
  if (!g_renderer_state.cache_stamp) g_renderer_state.cache_stamp = 1;
  SimBackgroundVisibleModelList list;
  BuildVisibleModelList(&draw_params, &list);
  int mountain_relief_count =
      BuildProjectedMountainReliefFaces(&draw_params);
  float visible_minimum, visible_maximum;
  GroundDepthRange(&draw_params, &visible_minimum, &visible_maximum);
  SimBackgroundVoxelLightDirection light;
  SimBackgroundVoxelLighting_ResolveDirection(
      params->light_azimuth_deg, params->light_elevation_deg, &light);
  CollectDepthGeometry(
      &draw_params, &list, mountain_relief_count, &light);
  SDL_Texture *depth_composite = Sim3DDepthPass_Submit(renderer, NULL);
  if (depth_composite) {
    SDL_FRect destination = {
      (float)params->viewport.x, (float)params->viewport.y,
      (float)params->viewport.w, (float)params->viewport.h,
    };
    SDL_RenderTexture(renderer, depth_composite, NULL, &destination);
  }
  /* Ground actors remain a separate authored priority band for now; the GPU
   * pass replaces the face/range painter sorter that caused roof leakage.
   * The callback no longer slices geometry, so camera motion cannot move a
   * model across an arbitrary CPU bucket boundary. */
  if (callback && interleaved) {
    callback(userdata, params, visible_minimum, visible_maximum, false);
    callback(userdata, params, 0.0f, 0.0f, true);
  }
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
        1.0f, 2.0f, 15.0f, 15.5f,
        SimBackgroundVoxelRegion_AuthoredHeight(object)};
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
    case kSimBackgroundVoxel_Palm:
    case kSimBackgroundVoxel_BroadTree:
    case kSimBackgroundVoxel_Shrub:
      out[0] = (SimBackgroundShadowBounds){
        0.0f, 0.0f, 16.0f, 16.0f,
        SimBackgroundVoxelRegion_AuthoredHeight(object)};
      return 1;
    case kSimBackgroundVoxel_StoryTree:
      out[0] = (SimBackgroundShadowBounds){
        4.5f, 4.5f, 27.5f, 27.5f,
        SimBackgroundVoxelRegion_AuthoredHeight(object)};
      return 1;
    case kSimBackgroundVoxel_BloodpoolCastle:
    case kSimBackgroundVoxel_MarahnaTemple:
      out[0] = (SimBackgroundShadowBounds){
        1.0f, 3.0f, 31.0f, 31.5f,
        SimBackgroundVoxelRegion_AuthoredHeight(object)};
      return 1;
    case kSimBackgroundVoxel_Pyramid:
      out[0] = (SimBackgroundShadowBounds){
        0.5f, 1.5f, 31.5f, 31.5f,
        SimBackgroundVoxelRegion_AuthoredHeight(object)};
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
  if (!RenderParamsValid(renderer, params)) return;
  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  SimBackgroundGeometryBatch *batch = &g_renderer_state.batch;
  batch->vertex_count = 0;
  batch->index_count = 0;
  SimBackgroundProjectionAxis axes[kSimBackgroundVoxelKindCount];
  ResolveProjectionAxes(params, axes);
  for (uint16_t i = 0; i < scene->object_count; i++) {
    const SimBackgroundVoxelObject *object = &scene->objects[i];
    if (object->kind >= kSimBackgroundVoxelKindCount) continue;
    const SimBackgroundProjectionAxis *axis = &axes[object->kind];
    if (!ObjectMayBeVisible(object, params, axis)) continue;
    SimBackgroundShadowBounds bounds[kSimBackgroundMaxShadowVolumes];
    int volume_count = ShadowBounds(object, bounds);
    for (int volume = 0; volume < volume_count; volume++)
      AppendShadowVolume(batch, params, object, bounds[volume],
                         axis->height_scale,
                         light_x, light_y);
  }
  FlushBatch(renderer, batch);
}

void SimBackgroundVoxelRenderer_Reset(void) {
  if (g_renderer_state.ground) SDL_DestroyTexture(g_renderer_state.ground);
  Sim3DDepthPass_Reset();
  g_renderer_state.ground = NULL;
  g_renderer_state.uploaded_serial = 0;
  g_renderer_state.allocation_failed = false;
  g_renderer_state.cache_stamp = 0;
  g_renderer_state.biome = kSimBackgroundVoxelBiome_Temperate;
  SimBackgroundVoxelModelCache_Reset();
  g_renderer_state.batch.vertex_count = 0;
  g_renderer_state.batch.index_count = 0;
}
