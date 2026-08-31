#include "sim_background_voxel_project.h"

#include <math.h>

#include "sim3d_depth_pass.h"
#include "sim_background_voxel_models.h"

static const float kGroundProjectionEpsilonPixels = 0.000001f;
static const float kMinimumProjectedTwiceArea = 0.05f;

const SimBackgroundProjectionAxis kSimBackgroundUprightProjectionAxis = {
  0.0f, 0.0f, 1.0f,
};

void SimBackgroundVoxelProject_Prepare(
    SimBackgroundVoxelRenderParams *params) {
  if (!params) return;
  params->texture_to_clip_valid = false;
  if (!params->matrix || params->source.w <= 0 || params->source.h <= 0 ||
      params->viewport.w <= 0 || params->viewport.h <= 0)
    return;

  const float inverse_width = 1.0f / (float)params->source.w;
  const float inverse_height = 1.0f / (float)params->source.h;
  const float aspect = (float)params->viewport.w /
      (float)params->viewport.h;
  const float world_x_per_texture_x = aspect * inverse_width;
  const float world_y_per_texture_y = -inverse_height;
  const float world_z_per_height = inverse_height;
  const float world_x_constant =
      (-(float)params->source.x * inverse_width - 0.5f) * aspect;
  const float world_y_constant =
      0.5f + (float)params->source.y * inverse_height;

  for (int component = 0; component < 4; component++) {
    float *row = &params->texture_to_clip[component * 4];
    row[0] = params->matrix[component] * world_x_per_texture_x;
    row[1] = params->matrix[4 + component] * world_y_per_texture_y;
    row[2] = params->matrix[8 + component] * world_z_per_height;
    row[3] = params->matrix[component] * world_x_constant +
        params->matrix[4 + component] * world_y_constant +
        params->matrix[12 + component];
  }
  params->texture_to_clip_valid = true;
}

static float PreparedClipComponent(
    const SimBackgroundVoxelRenderParams *params, int component,
    float texture_x, float texture_y, float height_pixels) {
  const float *row = &params->texture_to_clip[component * 4];
  return row[0] * texture_x + row[1] * texture_y +
      row[2] * height_pixels + row[3];
}

static bool PreparedNormalizedDepth(
    const SimBackgroundVoxelRenderParams *params,
    float texture_x, float texture_y, float height_pixels,
    float *gpu_depth) {
  if (!params->texture_to_clip_valid || !gpu_depth) return false;
  const float clip_w = PreparedClipComponent(
      params, 3, texture_x, texture_y, height_pixels);
  if (clip_w <= kScene3DMinimumProjectionDepth) return false;
  const float clip_z = PreparedClipComponent(
      params, 2, texture_x, texture_y, height_pixels);
  const float depth = clip_z / clip_w * 0.5f + 0.5f;
  if (!isfinite(depth)) return false;
  *gpu_depth = depth;
  return true;
}

static bool ProjectPreparedTexturePoint(
    const SimBackgroundVoxelRenderParams *params,
    float texture_x, float texture_y, float height_pixels,
    Scene3DPoint *out, float *gpu_depth, float *clip_depth) {
  if (!params->texture_to_clip_valid || !out) return false;
  const float clip_x = PreparedClipComponent(
      params, 0, texture_x, texture_y, height_pixels);
  const float clip_y = PreparedClipComponent(
      params, 1, texture_x, texture_y, height_pixels);
  const float clip_w = PreparedClipComponent(
      params, 3, texture_x, texture_y, height_pixels);
  if (clip_w <= kScene3DMinimumProjectionDepth) return false;
  const float inverse_w = 1.0f / clip_w;
  Scene3DPoint projected = {
    (clip_x * inverse_w * 0.5f + 0.5f) * params->viewport.w +
        params->viewport.x,
    (1.0f - (clip_y * inverse_w * 0.5f + 0.5f)) * params->viewport.h +
        params->viewport.y,
  };
  if (params->render_scale == kSimBackgroundVoxelRenderScale_PixelClean) {
    projected.x = floorf(projected.x) + 0.5f;
    projected.y = floorf(projected.y) + 0.5f;
  }
  if (!isfinite(projected.x) || !isfinite(projected.y)) return false;
  if (gpu_depth) {
    const float clip_z = PreparedClipComponent(
        params, 2, texture_x, texture_y, height_pixels);
    const float depth = clip_z * inverse_w * 0.5f + 0.5f;
    if (!isfinite(depth)) return false;
    *gpu_depth = depth;
  }
  if (clip_depth) *clip_depth = clip_w;
  *out = projected;
  return true;
}

void SimBackgroundVoxelProject_TexturePointToWorld(
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

SimBackgroundModelLean SimBackgroundVoxelProject_CameraFacingLean(
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
  /* The approved bridge is an actual horizontal span, not facade art. Any
   * billboard correction would curl its deck as the camera moved. */
  if (kind == kSimBackgroundVoxel_Bridge)
    return (SimBackgroundModelLean){0.0f, 0.0f};
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
  return SimBackgroundVoxelProject_CameraFacingLean(params, billboard_blend);
}

SimBackgroundProjectionAxis SimBackgroundVoxelProject_Axis(
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

void SimBackgroundVoxelProject_ResolveAxes(
    const SimBackgroundVoxelRenderParams *params,
    SimBackgroundProjectionAxis axes[kSimBackgroundVoxelKindCount]) {
  for (int kind = 0; kind < kSimBackgroundVoxelKindCount; kind++)
    axes[kind] = SimBackgroundVoxelProject_Axis(
        params, CameraFacingModelLean(
            params, (SimBackgroundVoxelKind)kind));
}

/* Leans a model-local point and maps it into world units. Kept separate so the
 * screen position and the GPU depth of one vertex are derived from a single
 * transform instead of repeating it per output. */
void SimBackgroundVoxelProject_LeanedPointToWorld(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float texture_x, float texture_y, float height_pixels,
    float *world_x, float *world_y, float *world_z) {
  SimBackgroundVoxelProject_TexturePointToWorld(
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

static bool ProjectWorldToScreenWithDepth(
    const SimBackgroundVoxelRenderParams *params,
    float world_x, float world_y, float world_z,
    Scene3DPoint *out, float *gpu_depth) {
  Scene3DPoint projected;
  if (!Scene3D_ProjectWorldPointWithDepth(
          params->matrix, world_x, world_y, world_z,
          params->viewport.w, params->viewport.h,
          &projected, gpu_depth))
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

bool SimBackgroundVoxelProject_Point(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float texture_x, float texture_y, float height_pixels,
    Scene3DPoint *out, float *clip_depth) {
  const float leaned_x = texture_x + height_pixels * axis->x_per_height;
  const float leaned_y = texture_y + height_pixels * axis->y_per_height;
  const float leaned_height = height_pixels * axis->height_scale;
  if (params->texture_to_clip_valid)
    return ProjectPreparedTexturePoint(
        params, leaned_x, leaned_y, leaned_height,
        out, NULL, clip_depth);
  float world_x, world_y, world_z;
  SimBackgroundVoxelProject_LeanedPointToWorld(
      params, axis, texture_x, texture_y, height_pixels,
      &world_x, &world_y, &world_z);
  if (!ProjectWorldToScreen(params, world_x, world_y, world_z, out))
    return false;
  if (clip_depth)
    *clip_depth = Scene3D_ClipDepth(
        params->matrix, world_x, world_y, world_z);
  return true;
}

/* Terrain altitude is a world translation; only the object's authored height
 * follows its camera-facing presentation axis. Feeding their sum through
 * ProjectVertex would lean the ground contact itself and reopen a seam between
 * the model and the terrain mesh. */
bool SimBackgroundVoxelProject_GroundedVertexWithDepthGround(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float texture_x, float texture_y,
    float height_pixels, float visual_ground_pixels,
    float depth_ground_pixels,
    Scene3DPoint *out, float *gpu_depth) {
  if (!out || !gpu_depth) return false;
  const float leaned_x = texture_x + height_pixels * axis->x_per_height;
  const float leaned_y = texture_y + height_pixels * axis->y_per_height;
  const float leaned_height = height_pixels * axis->height_scale;
  if (params->texture_to_clip_valid) {
    float visual_depth;
    if (!ProjectPreparedTexturePoint(
            params, leaned_x, leaned_y,
            visual_ground_pixels + leaned_height,
            out, &visual_depth, NULL))
      return false;
    if (fabsf(depth_ground_pixels - visual_ground_pixels) <
        kGroundProjectionEpsilonPixels) {
      *gpu_depth = visual_depth;
      return true;
    }
    float safety_depth;
    if (!PreparedNormalizedDepth(
            params, leaned_x, leaned_y,
            depth_ground_pixels + leaned_height,
            &safety_depth))
      return false;
    *gpu_depth = fminf(visual_depth, safety_depth);
    return true;
  }
  float world_x, world_y, world_z;
  SimBackgroundVoxelProject_TexturePointToWorld(
      params,
      texture_x + height_pixels * axis->x_per_height,
      texture_y + height_pixels * axis->y_per_height,
      visual_ground_pixels + height_pixels * axis->height_scale,
      &world_x, &world_y, &world_z);
  float visual_depth;
  if (!ProjectWorldToScreenWithDepth(
          params, world_x, world_y, world_z, out, &visual_depth))
    return false;
  if (fabsf(depth_ground_pixels - visual_ground_pixels) <
      kGroundProjectionEpsilonPixels) {
    *gpu_depth = visual_depth;
    return true;
  }
  SimBackgroundVoxelProject_TexturePointToWorld(
      params,
      texture_x + height_pixels * axis->x_per_height,
      texture_y + height_pixels * axis->y_per_height,
      depth_ground_pixels + height_pixels * axis->height_scale,
      &world_x, &world_y, &world_z);
  float safety_depth;
  if (!Scene3D_NormalizedDepth(
          params->matrix, world_x, world_y, world_z, &safety_depth))
    return false;
  /* Orbit controls can reverse which world-Z point is nearer. A safety
   * envelope may only pull the bridge toward the camera in depth; it must
   * never make an otherwise visible bridge easier for terrain to reject. */
  *gpu_depth = fminf(visual_depth, safety_depth);
  return true;
}

bool SimBackgroundVoxelProject_GroundedVertex(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float texture_x, float texture_y,
    float height_pixels, float ground_pixels,
    Scene3DPoint *out, float *gpu_depth) {
  return SimBackgroundVoxelProject_GroundedVertexWithDepthGround(
      params, axis, texture_x, texture_y, height_pixels,
      ground_pixels, ground_pixels, out, gpu_depth);
}

bool SimBackgroundVoxelProject_GroundedPoint(
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundProjectionAxis *axis,
    float texture_x, float texture_y,
    float height_pixels, float ground_pixels,
    Scene3DPoint *out, float *clip_depth) {
  const float leaned_x = texture_x + height_pixels * axis->x_per_height;
  const float leaned_y = texture_y + height_pixels * axis->y_per_height;
  const float leaned_height =
      ground_pixels + height_pixels * axis->height_scale;
  if (params->texture_to_clip_valid)
    return ProjectPreparedTexturePoint(
        params, leaned_x, leaned_y, leaned_height,
        out, NULL, clip_depth);
  float world_x, world_y, world_z;
  SimBackgroundVoxelProject_TexturePointToWorld(
      params,
      texture_x + height_pixels * axis->x_per_height,
      texture_y + height_pixels * axis->y_per_height,
      ground_pixels + height_pixels * axis->height_scale,
      &world_x, &world_y, &world_z);
  if (!ProjectWorldToScreen(params, world_x, world_y, world_z, out))
    return false;
  if (clip_depth)
    *clip_depth = Scene3D_ClipDepth(
        params->matrix, world_x, world_y, world_z);
  return true;
}

static ArRenderColorF VertexColour(uint32_t argb, uint8_t brightness) {
  float shade = brightness / 255.0f;
  return (ArRenderColorF){
    ((argb >> 16) & 0xFF) / 255.0f * shade,
    ((argb >> 8) & 0xFF) / 255.0f * shade,
    (argb & 0xFF) / 255.0f * shade,
    ((argb >> 24) & 0xFF) / 255.0f,
  };
}

static void FlushBatchTexture(ArRenderDevice *device,
                              SimBackgroundGeometryBatch *batch,
                              ArRenderTexture texture) {
  if (!batch->index_count) return;
  ArRenderDevice_DrawGeometry(
      device, texture, batch->vertices, batch->vertex_count,
      batch->indices, batch->index_count);
  batch->vertex_count = 0;
  batch->index_count = 0;
}

void SimBackgroundVoxelProject_FlushBatch(ArRenderDevice *device,
                       SimBackgroundGeometryBatch *batch) {
  FlushBatchTexture(device, batch, ArRenderTexture_Invalid());
}

bool SimBackgroundVoxelProject_ResolveFace(
    const SimBackgroundProjectedFace *face,
    const SimBackgroundVoxelPalette *palette,
    SimBackgroundVoxelShading shading,
    Sim3DDepthVertex vertices[4]) {
  if (!face || !vertices) return false;
  const SimBackgroundVoxelMaterial material =
      (SimBackgroundVoxelMaterial)face->material;
  const bool valid_material = palette && material >= 0 &&
      material < kSimVoxelMaterial_Count;
  for (int i = 0; i < 4; i++) {
    int level = shading == kSimBackgroundVoxelShading_MaterialAware
        ? SimBackgroundVoxelPalette_LevelForBrightness(face->brightness[i])
        : kSimBackgroundVoxelPaletteBaseLevel;
    uint32_t argb = valid_material
        ? palette->material[material][level] : 0xFFFF00FFu;
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
  return true;
}

void SimBackgroundVoxelProject_AppendFace(
    const SimBackgroundProjectedFace *face,
    const SimBackgroundVoxelPalette *palette,
    SimBackgroundVoxelShading shading) {
  Sim3DDepthVertex vertices[4];
  if (SimBackgroundVoxelProject_ResolveFace(
          face, palette, shading, vertices))
    Sim3DDepthPass_AppendQuad(kSim3DDepthPass_Solid, vertices);
}

bool SimBackgroundVoxelProject_IsDegenerate(const Scene3DPoint points[4]) {
  float twice_area = 0.0f;
  for (int i = 0; i < 4; i++) {
    const Scene3DPoint *a = &points[i];
    const Scene3DPoint *b = &points[(i + 1) & 3];
    twice_area += a->x * b->y - b->x * a->y;
  }
  return twice_area > -kMinimumProjectedTwiceArea &&
      twice_area < kMinimumProjectedTwiceArea;
}
