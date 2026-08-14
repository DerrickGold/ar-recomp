#include "sim_background_voxel_renderer.h"

#include <math.h>
#include <stdio.h>

#include "scene3d_math.h"
#include "sim_background_voxel_models.h"
#include "sim_background_voxel_proportions.h"
#include "sim_background_voxels.h"

enum {
  /* Authored tree clusters remain under 100 faces at Ultra. This batch normally
   * covers an entire viewport in one call, while retaining a safe flush path
   * for unusually broad views. */
  kMaxGeometryQuads = 8192,
  kMaxVertices = kMaxGeometryQuads * 4,
  kMaxIndices = kMaxGeometryQuads * 6,
};

typedef struct SimBackgroundGeometryBatch {
  SDL_Vertex vertices[kMaxVertices];
  int indices[kMaxIndices];
  int vertex_count, index_count;
} SimBackgroundGeometryBatch;

typedef struct SimBackgroundVoxelPalette {
  uint32_t material[kSimVoxelMaterial_Count];
} SimBackgroundVoxelPalette;

typedef struct ProjectedModelFace {
  Scene3DPoint points[4];
  float depth;
  uint8_t material;
  uint8_t brightness;
} ProjectedModelFace;

typedef struct SimBackgroundModelLean {
  float x_per_height;
  float y_per_height;
} SimBackgroundModelLean;

static struct {
  SDL_Texture *ground;
  uint32_t uploaded_serial;
  bool allocation_failed;
  SimBackgroundVoxelPalette palettes[kSimBackgroundMaxObjects];
  SimBackgroundGeometryBatch batch;
} g_renderer_state;

static uint32_t Argb(uint8_t red, uint8_t green, uint8_t blue) {
  return 0xFF000000u | (uint32_t)red << 16 |
      (uint32_t)green << 8 | blue;
}

static uint32_t VaryColour(uint32_t colour, int variation) {
  int red = (int)((colour >> 16) & 0xFF) + variation;
  int green = (int)((colour >> 8) & 0xFF) + variation;
  int blue = (int)(colour & 0xFF) + variation;
  if (red < 0) red = 0;
  if (green < 0) green = 0;
  if (blue < 0) blue = 0;
  if (red > 255) red = 255;
  if (green > 255) green = 255;
  if (blue > 255) blue = 255;
  return Argb((uint8_t)red, (uint8_t)green, (uint8_t)blue);
}

static void SetCommonPalette(SimBackgroundVoxelPalette *palette) {
  palette->material[kSimVoxelMaterial_Wall] = Argb(194, 155, 85);
  palette->material[kSimVoxelMaterial_WallLight] = Argb(230, 204, 139);
  palette->material[kSimVoxelMaterial_Roof] = Argb(100, 55, 125);
  palette->material[kSimVoxelMaterial_RoofLight] = Argb(132, 78, 153);
  palette->material[kSimVoxelMaterial_Trim] = Argb(218, 198, 145);
  palette->material[kSimVoxelMaterial_Dark] = Argb(39, 28, 34);
  palette->material[kSimVoxelMaterial_Wood] = Argb(121, 70, 31);
  palette->material[kSimVoxelMaterial_Metal] = Argb(110, 116, 113);
  palette->material[kSimVoxelMaterial_Blade] = Argb(239, 237, 220);
  palette->material[kSimVoxelMaterial_Trunk] = Argb(91, 55, 27);
  palette->material[kSimVoxelMaterial_Leaves] = Argb(22, 132, 35);
  palette->material[kSimVoxelMaterial_LeavesLight] = Argb(62, 177, 51);
  palette->material[kSimVoxelMaterial_LeavesDark] = Argb(7, 79, 24);
}

static void BuildPalette(const SimBackgroundVoxelObject *object,
                         SimBackgroundVoxelPalette *palette) {
  SetCommonPalette(palette);
  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House:
      palette->material[kSimVoxelMaterial_Wall] = Argb(202, 158, 81);
      palette->material[kSimVoxelMaterial_WallLight] = Argb(238, 204, 125);
      palette->material[kSimVoxelMaterial_Roof] = Argb(123, 64, 34);
      palette->material[kSimVoxelMaterial_RoofLight] = Argb(163, 87, 42);
      break;
    case kSimBackgroundVoxel_Cathedral:
      palette->material[kSimVoxelMaterial_Wall] = Argb(194, 200, 190);
      palette->material[kSimVoxelMaterial_WallLight] = Argb(235, 236, 220);
      palette->material[kSimVoxelMaterial_Roof] = Argb(172, 181, 178);
      palette->material[kSimVoxelMaterial_RoofLight] = Argb(239, 239, 224);
      palette->material[kSimVoxelMaterial_Trim] = Argb(150, 160, 157);
      palette->material[kSimVoxelMaterial_Dark] = Argb(41, 43, 40);
      break;
    case kSimBackgroundVoxel_Windmill:
      palette->material[kSimVoxelMaterial_Wall] = Argb(196, 154, 82);
      palette->material[kSimVoxelMaterial_WallLight] = Argb(225, 190, 116);
      palette->material[kSimVoxelMaterial_Roof] = Argb(88, 47, 118);
      palette->material[kSimVoxelMaterial_RoofLight] = Argb(127, 74, 151);
      break;
    case kSimBackgroundVoxel_Factory:
      palette->material[kSimVoxelMaterial_Wall] = Argb(177, 144, 73);
      palette->material[kSimVoxelMaterial_WallLight] = Argb(220, 190, 112);
      palette->material[kSimVoxelMaterial_Roof] = Argb(80, 42, 105);
      palette->material[kSimVoxelMaterial_RoofLight] = Argb(116, 65, 139);
      palette->material[kSimVoxelMaterial_Trim] = Argb(111, 81, 115);
      break;
    case kSimBackgroundVoxel_Tree: {
      int seed = object->cell_x * 3 + object->cell_y * 5 + object->group;
      int variation = (seed % 3 - 1) * 7;
      palette->material[kSimVoxelMaterial_Leaves] =
          VaryColour(palette->material[kSimVoxelMaterial_Leaves], variation);
      palette->material[kSimVoxelMaterial_LeavesLight] =
          VaryColour(palette->material[kSimVoxelMaterial_LeavesLight],
                     variation);
      palette->material[kSimVoxelMaterial_LeavesDark] =
          VaryColour(palette->material[kSimVoxelMaterial_LeavesDark],
                     variation);
      break;
    }
  }
  if (object->kind != kSimBackgroundVoxel_Tree && object->record_slot != 0xFF) {
    int variation = ((int)object->record_slot % 3 - 1) * 4;
    palette->material[kSimVoxelMaterial_Wall] =
        VaryColour(palette->material[kSimVoxelMaterial_Wall], variation);
    palette->material[kSimVoxelMaterial_Roof] =
        VaryColour(palette->material[kSimVoxelMaterial_Roof], variation);
  }
}

static SDL_Texture *CreateGroundTexture(SDL_Renderer *renderer) {
  SDL_Texture *texture = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kSimTownCanvasPixels, kSimTownCanvasPixels);
  if (!texture) return NULL;
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
  return texture;
}

void SimBackgroundVoxelRenderer_Upload(SDL_Renderer *renderer) {
  uint32_t serial = SimBackgroundVoxels_Serial();
  if (!renderer || !serial || serial == g_renderer_state.uploaded_serial ||
      g_renderer_state.allocation_failed)
    return;
  if (!g_renderer_state.ground)
    g_renderer_state.ground = CreateGroundTexture(renderer);
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
  for (uint16_t i = 0; i < scene->object_count; i++)
    BuildPalette(&scene->objects[i], &g_renderer_state.palettes[i]);
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

static SimBackgroundModelLean CameraFacingModelLean(
    const SimBackgroundVoxelRenderParams *params) {
  /* Clip depth's ground gradient points away from the camera. In town-texture
   * coordinates Y has the opposite sign from world Y, so (m3,-m7) is that
   * direction in pixels. Moving a model top along it cancels some of the
   * depth introduced by world Z and rotates the vertical axis toward a true
   * billboard without flattening the model completely.
   *
   * A 0.35 blend preserves visible roof depth while making the facade read
   * like the original upright SIM art. The cap keeps a nearly pitchless
   * camera from asking for an infinitely tilted billboard. */
  const float kBillboardBlend = 0.35f;
  const float kMaximumLeanPerHeight = 1.15f;
  float direction_x = params->matrix[3];
  float direction_y = -params->matrix[7];
  float ground_depth = sqrtf(direction_x * direction_x +
                             direction_y * direction_y);
  if (ground_depth < 0.0001f)
    return (SimBackgroundModelLean){0.0f, 0.0f};
  float full_billboard = -params->matrix[11] / ground_depth;
  if (full_billboard <= 0.0f)
    return (SimBackgroundModelLean){0.0f, 0.0f};
  float lean = full_billboard * kBillboardBlend;
  if (lean > kMaximumLeanPerHeight) lean = kMaximumLeanPerHeight;
  return (SimBackgroundModelLean){
    direction_x / ground_depth * lean,
    direction_y / ground_depth * lean,
  };
}

static bool ProjectPoint(const SimBackgroundVoxelRenderParams *params,
                         const SimBackgroundModelLean *lean,
                         float texture_x, float texture_y, float height_pixels,
                         Scene3DPoint *out, float *clip_depth) {
  /* Lean is a rotation-like presentation axis, not an additive shear. Scale
   * its vertical and ground components together so facing the camera does not
   * make the model physically longer as the pitch changes. */
  float lean_length = sqrtf(lean->x_per_height * lean->x_per_height +
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
    1.0f,
  };
}

static uint8_t DirectionalFaceBrightness(
    const SimBackgroundVoxelModelFace *face,
    const SimBackgroundVoxelRenderParams *params) {
  const SimBackgroundVoxelModelPoint *a = &face->points[0];
  const SimBackgroundVoxelModelPoint *b = &face->points[1];
  const SimBackgroundVoxelModelPoint *d = &face->points[3];
  float ux = b->x - a->x, uy = b->y - a->y, uz = b->z - a->z;
  float vx = d->x - a->x, vy = d->y - a->y, vz = d->z - a->z;
  float nx = uy * vz - uz * vy;
  float ny = uz * vx - ux * vz;
  float nz = ux * vy - uy * vx;
  float length = sqrtf(nx * nx + ny * ny + nz * nz);
  if (length < 0.0001f) return face->brightness;
  nx /= length;
  ny /= length;
  nz /= length;
  /* AddBox authors side faces from the inside winding and top faces from the
   * outside winding. Correct that convention before applying world light. */
  if (fabsf(nz) < 0.5f) {
    nx = -nx;
    ny = -ny;
    nz = -nz;
  } else if (nz < 0.0f) {
    nx = -nx;
    ny = -ny;
    nz = -nz;
  }

  const float kPi = 3.14159265f;
  float azimuth = params->light_azimuth_deg * kPi / 180.0f;
  float elevation = params->light_elevation_deg * kPi / 180.0f;
  float horizontal = cosf(elevation);
  /* The setting names the direction a shadow is thrown, so the light itself
   * comes from the opposite horizontal direction. */
  float light_x = -cosf(azimuth) * horizontal;
  float light_y = -sinf(azimuth) * horizontal;
  float light_z = sinf(elevation);
  float diffuse = nx * light_x + ny * light_y + nz * light_z;
  if (diffuse < 0.0f) diffuse = 0.0f;
  if (diffuse > 1.0f) diffuse = 1.0f;
  float authored = 0.86f + 0.14f * face->brightness / 255.0f;
  float lit = authored * (0.63f + 0.37f * diffuse);
  if (lit > 1.0f) lit = 1.0f;
  return (uint8_t)(lit * 255.0f + 0.5f);
}

static void FlushBatch(SDL_Renderer *renderer,
                       SimBackgroundGeometryBatch *batch) {
  if (!batch->index_count) return;
  SDL_RenderGeometry(renderer, NULL, batch->vertices, batch->vertex_count,
                     batch->indices, batch->index_count);
  batch->vertex_count = 0;
  batch->index_count = 0;
}

static void AppendProjectedFace(
    SDL_Renderer *renderer, SimBackgroundGeometryBatch *batch,
    const ProjectedModelFace *face,
    const SimBackgroundVoxelPalette *palette) {
  if (batch->vertex_count + 4 > kMaxVertices ||
      batch->index_count + 6 > kMaxIndices)
    FlushBatch(renderer, batch);
  int base = batch->vertex_count;
  uint32_t argb = face->material < kSimVoxelMaterial_Count
      ? palette->material[face->material] : Argb(255, 0, 255);
  SDL_FColor colour = VertexColour(argb, face->brightness);
  for (int i = 0; i < 4; i++)
    batch->vertices[batch->vertex_count++] =
        (SDL_Vertex){{face->points[i].x, face->points[i].y}, colour, {0, 0}};
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

static int ObjectSortKey(const SimBackgroundVoxelObject *object) {
  return (object->cell_y + object->source_cells_h) * 64 + object->cell_x;
}

static float ObjectOriginY(const SimBackgroundVoxelObject *object) {
  return (object->cell_y + object->source_cells_h -
          object->footprint_cells_d) * kSimBackgroundCellPixels;
}

static bool ObjectMayBeVisible(
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelRenderParams *params) {
  const float margin = 48.0f;
  float offset_x = (float)params->town_screen_x0 - params->camera_x;
  float offset_y = -(float)params->camera_y;
  float x0 = offset_x + object->cell_x * kSimBackgroundCellPixels;
  float y0 = offset_y + ObjectOriginY(object);
  float x1 = x0 + object->footprint_cells_w * kSimBackgroundCellPixels;
  float y1 = y0 + object->footprint_cells_d * kSimBackgroundCellPixels;
  return x1 >= params->source.x - margin &&
      x0 <= params->source.x + params->source.w + margin &&
      y1 >= params->source.y - margin &&
      y0 <= params->source.y + params->source.h + margin;
}

static void DrawModel(
    SDL_Renderer *renderer, SimBackgroundGeometryBatch *batch,
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelPalette *palette,
    const SimBackgroundVoxelRenderParams *params,
    const SimBackgroundModelLean *lean) {
  SimBackgroundVoxelModel model;
  SimBackgroundVoxelModel_Build(
      object, (SimBackgroundVoxelDetail)params->detail, &model);
  if (!model.face_count || model.overflow) return;

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
  ProjectedModelFace projected[kSimBackgroundVoxelModelMaxFaces];
  int projected_count = 0;
  for (uint16_t face_index = 0; face_index < model.face_count; face_index++) {
    const SimBackgroundVoxelModelFace *source = &model.faces[face_index];
    ProjectedModelFace face = {
      .material = source->material,
      .brightness = DirectionalFaceBrightness(source, params),
    };
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
    AppendProjectedFace(renderer, batch, &projected[i], palette);
}

void SimBackgroundVoxelRenderer_Draw(
    SDL_Renderer *renderer, const SimBackgroundVoxelRenderParams *params) {
  if (!renderer || !params || !params->matrix || params->source.w <= 0 ||
      params->source.h <= 0 || params->viewport.w <= 0 ||
      params->viewport.h <= 0 ||
      !SimBackgroundVoxelRenderer_Ready(params->serial))
    return;
  const SimBackgroundVoxelScene *scene = SimBackgroundVoxels_Scene();
  uint16_t order[kSimBackgroundMaxObjects];
  for (uint16_t i = 0; i < scene->object_count; i++) {
    int insert = i;
    while (insert > 0 &&
           ObjectSortKey(&scene->objects[order[insert - 1]]) >
               ObjectSortKey(&scene->objects[i])) {
      order[insert] = order[insert - 1];
      insert--;
    }
    order[insert] = i;
  }

  SimBackgroundGeometryBatch *batch = &g_renderer_state.batch;
  batch->vertex_count = 0;
  batch->index_count = 0;
  SimBackgroundModelLean lean = CameraFacingModelLean(params);
  for (uint16_t at = 0; at < scene->object_count; at++) {
    uint16_t index = order[at];
    const SimBackgroundVoxelObject *object = &scene->objects[index];
    if (!ObjectMayBeVisible(object, params)) continue;
    DrawModel(renderer, batch, object, &g_renderer_state.palettes[index],
              params, &lean);
  }
  FlushBatch(renderer, batch);
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
  SimBackgroundModelLean lean = CameraFacingModelLean(params);
  float lean_length = sqrtf(lean.x_per_height * lean.x_per_height +
                            lean.y_per_height * lean.y_per_height);
  float axis_z_scale = 1.0f / sqrtf(1.0f + lean_length * lean_length);
  for (uint16_t i = 0; i < scene->object_count; i++) {
    const SimBackgroundVoxelObject *object = &scene->objects[i];
    if (!ObjectMayBeVisible(object, params)) continue;
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
  g_renderer_state.ground = NULL;
  g_renderer_state.uploaded_serial = 0;
  g_renderer_state.allocation_failed = false;
  g_renderer_state.batch.vertex_count = 0;
  g_renderer_state.batch.index_count = 0;
}
