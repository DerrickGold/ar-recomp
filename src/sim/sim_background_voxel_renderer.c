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
#include "sim_background_mountain_silhouette.h"
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
  /* Staging for the shadow mask, the one path that still batches through
   * SDL_RenderGeometry rather than the depth pass. Sized to hold a real
   * town's casters in a single draw - Bloodpool's 168 objects come to about
   * 1200 quads - and to flush cleanly rather than truncate beyond that. */
  kMaxGeometryQuads = 2048,
  kMaxVertices = kMaxGeometryQuads * 4,
  kMaxIndices = kMaxGeometryQuads * 6,
  /* Original cells plus the complete bounded set of reconstructed cap tiles. */
  kMaxMountainReliefCells = kSimBackgroundMountainCellCount +
      kSimBackgroundMountainMaxCapTiles,
  /* Every stack layer of every cell, plus the two silhouette skirt faces a
   * cell can contribute. The skirt is emitted once per cell rather than once
   * per layer, but the budget has to cover a mountain whose every cell is on
   * its own silhouette. */
  kMountainSkirtFacesPerCell = 2,
  kMaxMountainReliefFaces = kMaxMountainReliefCells *
      (kSimBackgroundMountainReliefMaxStackLayers +
       kMountainSkirtFacesPerCell),
};

/* Contact shading is a ground decal. It is lifted by a fraction of a source
 * pixel so it wins the depth test against the ground plane it sits on without
 * being separable from it at any supported zoom. */
static const float kContactLiftPixels = 0.06f;

/* Fully lit / fully opaque, for the relief faces that want neither shaped. */
static const uint8_t kMountainFullIntensity[4] = {255, 255, 255, 255};
/* The volcano stands taller than the ordinary peaks it shares a stamp with. */
static const float kVolcanoHeightScale = 1.12f;

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
  uint8_t brightness[4];
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
  float aspect = (float)params->viewport.w / params->viewport.h;
  float direction_x, direction_y, ground_depth;
  if (!Scene3D_GroundDepthDirection(
          params->matrix, aspect, params->source.w, params->source.h,
          &direction_x, &direction_y, &ground_depth))
    return (SimBackgroundModelLean){0.0f, 0.0f};
  float vertical_depth = params->matrix[11] / params->source.h;
  float full_billboard = -vertical_depth / ground_depth;
  if (full_billboard <= 0.0f)
    return (SimBackgroundModelLean){0.0f, 0.0f};
  float lean = full_billboard * billboard_blend;
  if (lean > kMaximumLeanPerHeight) lean = kMaximumLeanPerHeight;
  return (SimBackgroundModelLean){direction_x * lean, direction_y * lean};
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
    const uint8_t brightness[4], const uint8_t alpha[4],
    int *count) {
  if (*count >= kMaxMountainReliefFaces) return;
  ProjectedMountainReliefFace face;
  for (int point = 0; point < 4; point++) {
    if (!ProjectVertex(params, axis,
                       origin_x + local_x[point],
                       origin_y + local_y[point], local_z[point],
                       &face.points[point], &face.gpu_depth[point]))
      return;
    face.uv[point] = uv[point];
    face.brightness[point] = brightness[point];
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

/* Unit direction, in town-texture pixels, that leads away from the camera
 * across the ground. */
typedef struct SimBackgroundStackDirection {
  float x, y;
} SimBackgroundStackDirection;

/* The relief stack fakes a mountain's thickness with parallel copies of its
 * art displaced behind the front one. "Behind" has to mean behind the CAMERA,
 * not map north: the sim camera has a yaw axis, a reactive lean and a manual
 * orbit, and a fixed northward displacement fans the copies out sideways as
 * soon as any of them is non-zero, which reads as the rear copies sliding off
 * the ground while the front one stays put. At yaw zero this resolves to
 * (0,-1) and reproduces the original northward offset exactly. */
static SimBackgroundStackDirection MountainStackDirection(
    const SimBackgroundVoxelRenderParams *params) {
  float aspect = (float)params->viewport.w / params->viewport.h;
  SimBackgroundStackDirection direction;
  if (!Scene3D_GroundDepthDirection(
          params->matrix, aspect, params->source.w, params->source.h,
          &direction.x, &direction.y, NULL))
    return (SimBackgroundStackDirection){0.0f, -1.0f};
  return direction;
}

/* What every tile of one mountain shares. Threading these seven values through
 * each emitter individually pushed both of them past fifteen parameters, which
 * is where the cell/source/baseline arguments that actually differ per tile
 * stopped being visible at the call site. */
typedef struct MountainTileContext {
  const SimBackgroundVoxelRenderParams *params;
  const SimBackgroundProjectionAxis *axis;
  const SimBackgroundMountainRelief *relief;
  SimBackgroundStackDirection stack_direction;
  float height_scale;
  float origin_x, origin_y;
} MountainTileContext;

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

/* The crater mouth of the volcano drawn this frame. See the header: this is
 * published so the eruption's fireball arcs can launch from the point the
 * player sees smoking rather than from a constant kept beside the model. */
static SimBackgroundCraterAnchor g_crater_anchor;

bool SimBackgroundVoxelRenderer_CraterAnchor(SimBackgroundCraterAnchor *out) {
  if (!out) return false;
  *out = g_crater_anchor;
  return g_crater_anchor.valid;
}

static void AppendVolcanoEffects(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    const SimBackgroundMountainRelief *relief,
    const SimBackgroundMountainObject *object,
    float origin_x, float origin_y, float baseline, float height_scale) {
  if (!(object->flags & kSimBackgroundMountainObject_Volcano)) return;

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

  /* Publish the mouth before anything is drawn from it, and publish it LEANED
   * -- the same transform LeanedPointToWorld applies to every model vertex,
   * including the glow ring below. An unleaned anchor sits a few pixels off
   * the glow at any pitch that leans the models at all, which is every pitch
   * the player uses. */
  g_crater_anchor = (SimBackgroundCraterAnchor){
    .valid = true,
    .local_x = crater_x + crater_z * axis->x_per_height,
    .local_y = crater_y + crater_z * axis->y_per_height,
    .height_pixels = crater_z * axis->height_scale,
  };

  if (params->detail < kSimBackgroundVoxelDetail_Balanced ||
      params->style < kSimBackgroundVoxelStyle_Trim)
    return;

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
    const MountainTileContext *context,
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
  /* Every layer retains the exact front silhouette orientation. Flipping only
   * the rear copy moves off-centre tip pixels across their tile and turns one
   * peak into two visible horns, so the mapping does not vary by layer. */
  const SDL_FPoint uv[4] = {
    {mirror_x ? u1 : u0, v0},
    {mirror_x ? u0 : u1, v0},
    {mirror_x ? u0 : u1, v1},
    {mirror_x ? u1 : u0, v1},
  };
  /* The displacement of the rearmost copy. Every nearer layer is a fixed
   * fraction of it, so the taper is resolved once per corner instead of once
   * per corner per layer. */
  const SimBackgroundMountainRelief *relief = context->relief;
  float corner_baseline[4], rearmost_depth[4];
  for (int point = 0; point < 4; point++) {
    bool left = point == 0 || point == 3;
    corner_baseline[point] = left ? baseline_left : baseline_right;
    float rise = corner_baseline[point] - source_y[point];
    /* The relief module states the displacement as a signed northward offset,
     * which is its magnitude along whichever way the camera says is back.
     * Negating recovers that magnitude; the direction supplies the sign, so a
     * zero-yaw camera lands on exactly the old northward value. */
    rearmost_depth[point] = -SimBackgroundMountainRelief_StackOffsetY(
        relief, (uint8_t)(relief->stack_layer_count - 1), rise,
        left ? maximum_rise_left : maximum_rise_right);
  }
  float layers = (float)(relief->stack_layer_count - 1);
  /* Emit rear copies first for coherent material batching. Visibility is
   * resolved per fragment by the shared D32 target, so this loop order is not
   * a correctness dependency. */
  for (int layer = relief->stack_layer_count - 1; layer >= 0; layer--) {
    float fraction = layers > 0.0f ? (float)layer / layers : 0.0f;
    float local_x[4], local_y[4], local_z[4];
    for (int point = 0; point < 4; point++) {
      float depth = rearmost_depth[point] * fraction;
      MountainPlanePoint(
          source_x[point], source_y[point], corner_baseline[point], relief,
          depth * context->stack_direction.x,
          depth * context->stack_direction.y,
          &local_x[point], &local_y[point], &local_z[point]);
      local_z[point] *= context->height_scale;
    }
    AddProjectedMountainReliefFace(
        context->params, context->axis, context->origin_x, context->origin_y,
        local_x, local_y, local_z, uv, kMountainFullIntensity,
        kMountainFullIntensity, count);
  }
}

/* Each relief layer is a flat inclined plane with the authentic art painted on
 * it: its cross-section is a straight line from the ground at the front up and
 * back to the ridge. That reads as a mountain only from the canonical camera.
 * The wedge between the plane and the ground is empty and open at the sides,
 * so any other angle shows a tilted board whose upper half hangs in the air -
 * and no amount of extra stack layers changes that, because the layers are
 * parallel copies of the same plane and thicken it along the ground rather
 * than filling underneath it.
 *
 * This closes the silhouette: wherever a mountain cell has no neighbour, a
 * quad drops from that cell's edge of the plane straight down to z=0, giving
 * the mass a visible side and a footprint. It follows the outline in both
 * axes, stretches the tile's own art down the wall, and ramps its shading
 * from the face's own value to a shaded base. */
enum {
  /* How far into the tile the wall's ground edge samples. The mountain tiles
   * carry a one-to-two pixel dither margin along their outline that is both
   * grass-coloured and partly transparent, so a wall sampled from the outline
   * alone shows green see-through streaks. Stretching the tile's own interior
   * across the quad is what makes it read as rock. */
  kMountainSkirtStretchPixels = 8,
  /* A wall needs more than a couple of rows of outline to be worth standing. */
  kMountainSkirtMinimumRows = 2,
};
/* Shading down the wall. The mountain face is drawn fully lit, so a wall at a
 * single darker value meets it in a hard tonal step, and any pixel of
 * misalignment at that junction then reads as a shelf rather than as an edge.
 * Ramping from almost the face's own value at the top to a shaded base makes
 * the join continuous and lets the wall still read as a side. */
static const uint8_t kMountainSkirtTopBrightness = 240;
static const uint8_t kMountainSkirtBaseBrightness = 150;
/* The wall stands a fraction of a pixel proud of the art and starts a hair
 * above the face it meets. Both are below one source pixel, and they close
 * the sub-pixel seam the fitted line cannot follow exactly - the outline is
 * jagged row to row and a single quad can only be straight. */
static const float kMountainSkirtOutwardMargin = 0.5f;
static const float kMountainSkirtOverlapPixels = 0.35f;
/* How far a row's outline may sit inside the fitted line before that row is
 * treated as dither fringe rather than part of the wall. */
static const float kMountainSkirtStraightTolerance = 1.5f;
/* Where a tile's art meets the open side of its cell. Derived from the
 * immutable silhouette table, so it is resolved once per tile/side and reused
 * for every frame, object and town. */
typedef struct MountainSkirtProfile {
  bool resolved;
  bool present;
  uint8_t first_row, last_row;
  /* Where the wall stands, fitted to the tile's whole outline and then pushed
   * outward until no row of art lies outside it. Taking the two end rows
   * alone slants the line inward at a base tile, whose last row is the
   * dithered fringe several pixels in, and the mountain then overhangs its
   * own wall by three or four pixels. */
  float wall_first, wall_last;
  /* Columns the quad samples: the outline for the top edge so it joins the
   * mountain without a seam, the interior for the ground edge. */
  uint8_t outer_first, outer_last;
  uint8_t inner_first, inner_last;
} MountainSkirtProfile;

/* Resolved from the compile-time silhouette table, so an entry is the same
 * value whenever it is computed and never needs invalidating - not on a town
 * change, not on Reset. It is filled lazily from the present thread, which is
 * the only thread that renders; a second renderer thread would need this
 * built up front instead. */
static MountainSkirtProfile g_skirt_profiles[256][2];

static bool MountainSilhouetteOpaque(uint8_t tile, int column, int row) {
  bool opaque = false;
  if (!SimBackgroundMountainSilhouette_Lookup(tile, column, row, &opaque))
    return true;  /* Unknown tiles are treated as solid elsewhere too. */
  return opaque;
}

/* Outermost and innermost opaque columns of one row, scanning from `edge`
 * toward the far side of the tile. */
static bool MountainRowRun(uint8_t tile, int row, int edge, int step,
                           int *outer, int *inner) {
  int first = -1, last = -1;
  for (int at = 0; at < kSimBackgroundCellPixels; at++) {
    int column = edge + at * step;
    if (!MountainSilhouetteOpaque(tile, column, row)) continue;
    if (first < 0) first = column;
    last = column;
  }
  if (first < 0) return false;
  int reach = first + step * kMountainSkirtStretchPixels;
  /* Never sample past the run: beyond it is the tile's other outline. */
  if ((step > 0 && reach > last) || (step < 0 && reach < last)) reach = last;
  /* The fringe rows where a mountain's foot meets grass are dithered, not
   * solid runs, so the column that far in can still be a hole. Back off to
   * the nearest opaque one rather than punching the wall through. */
  while (reach != first && !MountainSilhouetteOpaque(tile, reach, row))
    reach -= step;
  *outer = first;
  *inner = reach;
  return true;
}

/* Least-squares fit of the outline over a row span, as column = intercept +
 * slope * (row - first_row). */
static void MountainFitOutline(uint8_t tile, int edge, int step,
                               int first_row, int last_row,
                               float *slope, float *intercept) {
  float rows = 0.0f, sum_row = 0.0f, sum_column = 0.0f;
  float sum_row_column = 0.0f, sum_row_row = 0.0f;
  for (int row = first_row; row <= last_row; row++) {
    int outer, inner;
    if (!MountainRowRun(tile, row, edge, step, &outer, &inner)) continue;
    float offset = (float)(row - first_row);
    rows += 1.0f;
    sum_row += offset;
    sum_column += (float)outer;
    sum_row_column += offset * (float)outer;
    sum_row_row += offset * offset;
  }
  if (rows <= 0.0f) {
    *slope = 0.0f;
    *intercept = 0.0f;
    return;
  }
  float denominator = rows * sum_row_row - sum_row * sum_row;
  *slope = denominator > 0.0001f
      ? (rows * sum_row_column - sum_row * sum_column) / denominator : 0.0f;
  *intercept = (sum_column - *slope * sum_row) / rows;
}

static const MountainSkirtProfile *MountainSkirtProfileFor(uint8_t tile,
                                                           bool right_edge) {
  MountainSkirtProfile *profile = &g_skirt_profiles[tile][right_edge ? 1 : 0];
  if (profile->resolved) return profile;
  profile->resolved = true;
  int step = right_edge ? -1 : 1;
  int edge = right_edge ? kSimBackgroundCellPixels - 1 : 0;

  int first_row = -1, last_row = -1;
  for (int row = 0; row < kSimBackgroundCellPixels; row++) {
    int outer, inner;
    if (!MountainRowRun(tile, row, edge, step, &outer, &inner)) continue;
    if (first_row < 0) first_row = row;
    last_row = row;
  }
  if (first_row < 0 || last_row - first_row < kMountainSkirtMinimumRows)
    return profile;

  /* A mountain's foot dissolves into the lawn over two or three dithered
   * rows whose outline sits several pixels inside the rest of the wall.
   * Following them would either drag the wall into the mountain, leaving the
   * art overhanging it, or leave the wall protruding past the art once it is
   * biased back out. Ending the wall where the outline stops being straight
   * avoids both, and costs nothing: those rows are within a pixel of the
   * ground already. A genuine diagonal deviates from its own fit by nothing,
   * so it is never trimmed. */
  float slope, intercept;
  while (last_row - first_row > kMountainSkirtMinimumRows) {
    MountainFitOutline(tile, edge, step, first_row, last_row,
                       &slope, &intercept);
    int outer, inner;
    if (!MountainRowRun(tile, last_row, edge, step, &outer, &inner)) break;
    float line = intercept + slope * (float)(last_row - first_row);
    float deviation = (float)step * ((float)outer - line);
    if (deviation <= kMountainSkirtStraightTolerance) break;
    last_row--;
  }
  MountainFitOutline(tile, edge, step, first_row, last_row,
                     &slope, &intercept);

  /* Push the fitted line outward until no row of art lies outside it. */
  float bias = 0.0f;
  for (int row = first_row; row <= last_row; row++) {
    int outer, inner;
    if (!MountainRowRun(tile, row, edge, step, &outer, &inner)) continue;
    float line = intercept + slope * (float)(row - first_row);
    float outside = (float)step * (line - (float)outer);
    if (outside > bias) bias = outside;
  }

  int outer_first, inner_first, outer_last, inner_last;
  if (!MountainRowRun(tile, first_row, edge, step,
                      &outer_first, &inner_first) ||
      !MountainRowRun(tile, last_row, edge, step, &outer_last, &inner_last))
    return profile;

  float span = (float)(last_row - first_row);
  profile->present = true;
  profile->first_row = (uint8_t)first_row;
  profile->last_row = (uint8_t)last_row;
  profile->wall_first = intercept - (float)step * bias;
  profile->wall_last = intercept + slope * span - (float)step * bias;
  profile->outer_first = (uint8_t)outer_first;
  profile->outer_last = (uint8_t)outer_last;
  profile->inner_first = (uint8_t)inner_first;
  profile->inner_last = (uint8_t)inner_last;
  return profile;
}

/* Displacement of the outermost stack copy on one side of the mountain. The
 * relief stack recedes along the camera's away axis, which under yaw has a
 * sideways component - up to 2.8px at Ultra - so a wall standing at the front
 * copy is overhung by the rear ones. Zero when the stack fans the other way,
 * because then the front copy already is the outermost. */
static void MountainStackOutwardOffset(
    const SimBackgroundMountainRelief *relief,
    SimBackgroundStackDirection direction, float rise, float maximum_rise,
    bool right_edge, float *offset_x, float *offset_y) {
  *offset_x = 0.0f;
  *offset_y = 0.0f;
  if (relief->stack_layer_count < 2) return;
  float magnitude = -SimBackgroundMountainRelief_StackOffsetY(
      relief, (uint8_t)(relief->stack_layer_count - 1), rise, maximum_rise);
  float sideways = magnitude * direction.x;
  if ((right_edge ? sideways : -sideways) <= 0.0f) return;
  *offset_x = sideways;
  *offset_y = magnitude * direction.y;
}

static void AddMountainSkirtTile(
    const MountainTileContext *context,
    float baseline, float maximum_rise,
    int destination_cell_x, int destination_cell_y,
    int source_cell_x, int source_cell_y, uint8_t source_tile,
    bool right_edge, int *count) {
  const MountainSkirtProfile *profile =
      MountainSkirtProfileFor(source_tile, right_edge);
  if (!profile->present) return;

  float outward = right_edge ? kMountainSkirtOutwardMargin
                             : -kMountainSkirtOutwardMargin;
  float cell_x = destination_cell_x * (float)kSimBackgroundCellPixels +
      outward;
  float cell_top = destination_cell_y * (float)kSimBackgroundCellPixels;
  /* The wall's top edge is the outline itself, so it slants with a shoulder
   * tile's diagonal instead of standing at the cell boundary beside it. */
  float north_x = cell_x + profile->wall_first;
  float south_x = cell_x + profile->wall_last;
  float y0 = cell_top + (float)profile->first_row;
  float y1 = cell_top + (float)profile->last_row + 1.0f;
  float north_offset_x, north_offset_y, south_offset_x, south_offset_y;
  MountainStackOutwardOffset(context->relief, context->stack_direction,
                             baseline - y0, maximum_rise, right_edge,
                             &north_offset_x, &north_offset_y);
  MountainStackOutwardOffset(context->relief, context->stack_direction,
                             baseline - y1, maximum_rise, right_edge,
                             &south_offset_x, &south_offset_y);
  float north_wall_x, north_y, north_z, south_wall_x, south_y, south_z;
  MountainPlanePoint(north_x, y0, baseline, context->relief,
                     north_offset_x, north_offset_y,
                     &north_wall_x, &north_y, &north_z);
  MountainPlanePoint(south_x, y1, baseline, context->relief,
                     south_offset_x, south_offset_y,
                     &south_wall_x, &south_y, &south_z);
  north_z = north_z * context->height_scale + kMountainSkirtOverlapPixels;
  south_z = south_z * context->height_scale + kMountainSkirtOverlapPixels;
  /* Art already sitting on the ground has no wedge to close. */
  if (north_z <= kMountainSkirtOverlapPixels) return;

  float local_x[4] = {
    north_wall_x, south_wall_x, south_wall_x, north_wall_x,
  };
  float local_y[4] = {north_y, south_y, south_y, north_y};
  float local_z[4] = {north_z, south_z, 0.0f, 0.0f};
  float cell_u = source_cell_x * (float)kSimBackgroundCellPixels;
  float cell_v = source_cell_y * (float)kSimBackgroundCellPixels;
  /* Stretched down the wall rather than smeared along it: the top edge takes
   * the outline so it joins the mountain without a seam, and the ground edge
   * takes the interior, so the quad is the tile's own rock in the town's own
   * palette. */
  float v0 = (cell_v + profile->first_row + 0.5f) /
      (float)kSimTownCanvasPixels;
  float v1 = (cell_v + profile->last_row + 0.5f) /
      (float)kSimTownCanvasPixels;
  SDL_FPoint uv[4] = {
    {(cell_u + profile->outer_first + 0.5f) / kSimTownCanvasPixels, v0},
    {(cell_u + profile->outer_last + 0.5f) / kSimTownCanvasPixels, v1},
    {(cell_u + profile->inner_last + 0.5f) / kSimTownCanvasPixels, v1},
    {(cell_u + profile->inner_first + 0.5f) / kSimTownCanvasPixels, v0},
  };
  /* Points 0 and 1 are the top edge against the mountain; 2 and 3 stand on
   * the ground. */
  const uint8_t brightness[4] = {
    kMountainSkirtTopBrightness, kMountainSkirtTopBrightness,
    kMountainSkirtBaseBrightness, kMountainSkirtBaseBrightness,
  };
  AddProjectedMountainReliefFace(
      context->params, context->axis, context->origin_x, context->origin_y,
      local_x, local_y, local_z, uv, brightness, kMountainFullIntensity,
      count);
}

static bool MountainCellOccupied(const SimBackgroundMountainObject *object,
                                 int row, int column) {
  return column >= 0 && column < object->width_cells &&
      (object->row_occupied_mask[row] & (1u << column)) != 0;
}

static int BuildProjectedMountainObjectFaces(
    const MountainTileContext *shared,
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainObjectList *objects) {
  int count = 0;
  /* Cleared every frame so a town with no volcano cannot hand the eruption
   * arcs the last town's crater. */
  g_crater_anchor.valid = false;
  for (uint8_t at = 0; at < objects->count; at++) {
    const SimBackgroundMountainObject *object = &objects->objects[at];
    /* Only the volcano's extra height varies between objects. */
    MountainTileContext context = *shared;
    context.height_scale =
        object->flags & kSimBackgroundMountainObject_Volcano
            ? kVolcanoHeightScale : 1.0f;
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
            &context, baseline, baseline, maximum_rise, maximum_rise,
            destination_x, destination_y, source_x, source_y, 0, &count);
        /* Close the silhouette wherever the mass ends, so the raised part of
         * the plane has a visible side reaching the ground instead of an open
         * edge with the ground showing under it. */
        for (int side = 0; side < 2; side++) {
          bool right_edge = side != 0;
          if (MountainCellOccupied(object, row,
                                   column + (right_edge ? 1 : -1)))
            continue;
          AddMountainSkirtTile(
              &context, baseline, maximum_rise,
              destination_x, destination_y,
              source_x, source_y, source_tile, right_edge, &count);
        }
      }
    AppendVolcanoEffects(
        context.params, context.axis, context.relief, object,
        context.origin_x, context.origin_y, baseline, context.height_scale);
  }
  return count;
}

static void AddNorthMountainCaps(
    const MountainTileContext *context,
    const SimBackgroundMountainField *field,
    const SimBackgroundMountainCaps *caps,
    const float component_bottom[kSimBackgroundMountainCellCount + 1],
    const float component_top[kSimBackgroundMountainCellCount + 1],
    int *count) {
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
        context, baseline_left, baseline_right,
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
  MountainTileContext context = {
    .params = params,
    .axis = &mountain_axis,
    .relief = &relief,
    .stack_direction = MountainStackDirection(params),
    .height_scale = 1.0f,
    .origin_x = origin_x,
    .origin_y = origin_y,
  };
  SimBackgroundMountainObjectList mountain_objects;
  if (SimBackgroundMountainObjects_Build(
          field, &scene->mountain_caps, &mountain_objects)) {
    return BuildProjectedMountainObjectFaces(
        &context, field, &mountain_objects);
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
          &context, baseline_left, baseline_right,
          maximum_rise_left, maximum_rise_right,
          cell_x, cell_y, cell_x, cell_y, 0,
          &count);
    }
  }
  AddNorthMountainCaps(
      &context, field, &scene->mountain_caps,
      component_bottom, component_top, &count);
  return count;
}

static void AppendProjectedMountainReliefFace(
    const ProjectedMountainReliefFace *face) {
  if (!Sim3DDepthPass_IsCollecting()) return;
  Sim3DDepthVertex vertices[4];
  for (int point = 0; point < 4; point++) {
    float shade = face->brightness[point] / 255.0f;
    vertices[point] = (Sim3DDepthVertex){
      .x = face->points[point].x,
      .y = face->points[point].y,
      .depth = face->gpu_depth[point],
      .color = {shade, shade, shade, face->alpha[point] / 255.0f},
      .uv = face->uv[point],
    };
  }
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
  /* Shading rides along with the geometry: none of its inputs move with the
   * camera, so the cache resolves it once per model per lighting state. */
  const SimBackgroundVoxelModelShadingKey shading_key = {
    .light_azimuth_deg = params->light_azimuth_deg,
    .light_elevation_deg = params->light_elevation_deg,
    .shading = params->shading,
    .biome = (uint8_t)g_renderer_state.biome,
  };
  const SimBackgroundVoxelModelShading *shading = NULL;
  const SimBackgroundVoxelModel *model = SimBackgroundVoxelModelCache_Get(
      object, detail, (SimBackgroundVoxelStyle)params->style,
      g_renderer_state.cache_stamp, &shading_key, &shading);
  if (!model || !model->face_count || model->overflow || !shading) return;
  AppendGroundContact(object, palette, params, origin_x, origin_y,
                      proportions);
  /* Faces are submitted as they are projected. The depth pass resolves
   * visibility per pixel, so there is nothing for an intermediate array to
   * order, and staging a whole model's worth of projected faces only cost a
   * second pass over 21KB of stack. */
  for (uint16_t face_index = 0; face_index < model->face_count; face_index++) {
    const SimBackgroundVoxelModelFace *source = &model->faces[face_index];
    ProjectedModelFace face = {.material = shading->material[face_index]};
    memcpy(face.brightness, shading->brightness[face_index],
           sizeof(face.brightness));
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
  /* Actors standing on ordinary ground go UNDER the composite, so the town's
   * own geometry hides them: a villager behind a peak or behind a house is
   * covered by it. The overhead camera means anything merely beside a building
   * still shows, because a model only occludes what is behind it on screen.
   *
   * Actors standing on a mountain cell go over it instead, lifted onto the
   * surface -- that is the town event that sends a villager up a peak, and the
   * authentic 2D map already says which case an actor is in. */
  if (callback && interleaved)
    callback(userdata, params, visible_minimum, visible_maximum,
             kSimBackgroundVoxelActorBand_Ground);
  if (depth_composite) {
    SDL_FRect destination = {
      (float)params->viewport.x, (float)params->viewport.y,
      (float)params->viewport.w, (float)params->viewport.h,
    };
    SDL_RenderTexture(renderer, depth_composite, NULL, &destination);
  }
  if (callback && interleaved) {
    callback(userdata, params, visible_minimum, visible_maximum,
             kSimBackgroundVoxelActorBand_Mountain);
    callback(userdata, params, 0.0f, 0.0f,
             kSimBackgroundVoxelActorBand_Overhead);
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

enum {
  kSimBackgroundMaxShadowVolumes = 3,
  /* A volume emits its base and offset caps plus one bridge per edge. */
  kShadowQuadsPerVolume = 6,
  kMaxShadowQuadsPerObject =
      kSimBackgroundMaxShadowVolumes * kShadowQuadsPerVolume,
};

_Static_assert(kMaxGeometryQuads >= kMaxShadowQuadsPerObject,
               "the shadow batch must hold one caster's volumes outright");

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
    /* Flush ahead of the caster that would not fit. AppendSolidQuad drops
     * silently when full, and a developed town can ask for more than this
     * batch holds - 1160 objects at three volumes is 20880 quads - so
     * without this the shadows nearest the end of the scene simply stop
     * appearing. Splitting the mask across draws is free: it is opaque black
     * into an offscreen target, so overlap is idempotent and order does not
     * matter, and the OBJ caster pass already issues one draw apiece. */
    if (batch->vertex_count + kMaxShadowQuadsPerObject * 4 > kMaxVertices ||
        batch->index_count + kMaxShadowQuadsPerObject * 6 > kMaxIndices)
      FlushBatch(renderer, batch);
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
  g_crater_anchor = (SimBackgroundCraterAnchor){0};
  SimBackgroundVoxelModelCache_Reset();
  g_renderer_state.batch.vertex_count = 0;
  g_renderer_state.batch.index_count = 0;
}
