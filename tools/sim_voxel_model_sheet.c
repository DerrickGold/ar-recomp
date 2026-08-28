/*
 * Render the production SIM voxel models in isolation for visual auditing.
 *
 * This deliberately goes through the same model cache, regional builders,
 * palettes, proportions, camera-facing projection, material lighting and D32
 * depth pass as the in-game renderer.  The only omitted scene input is terrain
 * elevation: every audit model stands on a common flat datum so silhouettes
 * and proportions can be compared directly.
 */

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/sdl/render_sdl.h"
#include "scene3d_math.h"
#include "sim/sim3d_depth_pass.h"
#include "sim/sim_background_bridge.h"
#include "sim/sim_background_voxel_biome.h"
#include "sim/sim_background_voxel_model_cache.h"
#include "sim/sim_background_voxel_palette.h"
#include "sim/sim_background_voxel_project.h"
#include "sim/sim_background_voxel_proportions.h"

enum {
  kRenderWidth = 420,
  kRenderHeight = 360,
  kSourcePixels = 52,
};

static ArRenderDevice g_render_device;
static ArSdlRenderBackend g_render_backend;

static const float kContactLiftPixels = 0.06f;

typedef struct ContactBounds {
  float x0, y0, x1, y1;
} ContactBounds;

typedef struct AuditEntry {
  const char *section;
  const char *filename;
  const char *label;
  SimBackgroundVoxelObject object;
} AuditEntry;

/* The depth pass reports production work to this profiler hook. */
void Sim3DPerformance_AddDraw(uint64_t vertices, uint64_t indices) {
  (void)vertices;
  (void)indices;
}

static SDL_Renderer *CreateProductionRenderer(SDL_Window *window) {
  SDL_PropertiesID properties = SDL_CreateProperties();
  if (!properties) return NULL;
  SDL_SetStringProperty(properties,
      SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER);
  SDL_SetPointerProperty(properties,
      SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
  SDL_SetBooleanProperty(properties,
      SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true);
  SDL_SetBooleanProperty(properties,
      SDL_PROP_RENDERER_CREATE_GPU_SHADERS_DXIL_BOOLEAN, true);
  SDL_SetBooleanProperty(properties,
      SDL_PROP_RENDERER_CREATE_GPU_SHADERS_MSL_BOOLEAN, true);
  SDL_Renderer *renderer = SDL_CreateRendererWithProperties(properties);
  SDL_DestroyProperties(properties);
  return renderer;
}

static float FootprintWidth(const SimBackgroundVoxelObject *object) {
  if (object->kind == kSimBackgroundVoxel_Bridge)
    return SimBackgroundBridge_ResolveBounds(object).width;
  return object->footprint_cells_w * (float)kSimBackgroundCellPixels;
}

static float FootprintDepth(const SimBackgroundVoxelObject *object) {
  if (object->kind == kSimBackgroundVoxel_Bridge)
    return SimBackgroundBridge_ResolveBounds(object).depth;
  return object->footprint_cells_d * (float)kSimBackgroundCellPixels;
}

static int ResolveContactBounds(const SimBackgroundVoxelObject *object,
                                ContactBounds out[3]) {
  switch ((SimBackgroundVoxelKind)object->kind) {
    case kSimBackgroundVoxel_House:
      out[0] = (ContactBounds){1.8f, 2.5f, 14.2f, 15.2f};
      return 1;
    case kSimBackgroundVoxel_Cathedral:
      out[0] = (ContactBounds){0.8f, 8.8f, 31.2f, 31.4f};
      return 1;
    case kSimBackgroundVoxel_Windmill:
      out[0] = (ContactBounds){6.2f, 2.3f, 25.8f, 15.2f};
      return 1;
    case kSimBackgroundVoxel_Factory:
      out[0] = (ContactBounds){0.8f, 0.8f, 21.8f, 10.8f};
      out[1] = (ContactBounds){0.8f, 22.2f, 21.8f, 31.2f};
      out[2] = (ContactBounds){21.2f, 0.8f, 31.2f, 31.2f};
      return 3;
    case kSimBackgroundVoxel_Tree:
    case kSimBackgroundVoxel_BroadTree:
    case kSimBackgroundVoxel_Palm:
      out[0] = (ContactBounds){5.4f, 5.4f, 10.6f, 10.6f};
      return 1;
    case kSimBackgroundVoxel_Shrub:
      out[0] = (ContactBounds){5.8f, 5.8f, 10.2f, 10.2f};
      return 1;
    case kSimBackgroundVoxel_StoryTree:
      out[0] = (ContactBounds){10.0f, 11.0f, 22.0f, 23.0f};
      return 1;
    case kSimBackgroundVoxel_BloodpoolCastle:
    case kSimBackgroundVoxel_MarahnaTemple:
      out[0] = (ContactBounds){1.0f, 3.0f, 31.0f, 31.5f};
      return 1;
    case kSimBackgroundVoxel_Pyramid:
      out[0] = (ContactBounds){0.5f, 1.5f, 31.5f, 31.5f};
      return 1;
    case kSimBackgroundVoxel_Bridge:
      return 0;
  }
  return 0;
}

static ContactBounds ScaleContactBounds(
    ContactBounds bounds, const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelProportions *proportions) {
  const float center_x = FootprintWidth(object) * 0.5f;
  const float center_y = FootprintDepth(object) * 0.5f;
  return (ContactBounds){
    center_x + (bounds.x0 - center_x) * proportions->footprint_scale,
    center_y + (bounds.y0 - center_y) * proportions->footprint_scale,
    center_x + (bounds.x1 - center_x) * proportions->footprint_scale,
    center_y + (bounds.y1 - center_y) * proportions->footprint_scale,
  };
}

static void AppendContact(
    const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelPalette *palette,
    const SimBackgroundVoxelRenderParams *params,
    float origin_x, float origin_y,
    const SimBackgroundVoxelProportions *proportions) {
  ContactBounds authored[3];
  const int count = ResolveContactBounds(object, authored);
  for (int at = 0; at < count; at++) {
    const ContactBounds bounds = ScaleContactBounds(
        authored[at], object, proportions);
    const float local_x[4] = {
      bounds.x0, bounds.x1, bounds.x1, bounds.x0,
    };
    const float local_y[4] = {
      bounds.y0, bounds.y0, bounds.y1, bounds.y1,
    };
    SimBackgroundProjectedFace face = {
      .material = kSimVoxelMaterial_Contact,
      .brightness = {255, 255, 255, 255},
    };
    bool valid = true;
    for (int point = 0; point < 4; point++) {
      if (!SimBackgroundVoxelProject_GroundedVertex(
              params, &kSimBackgroundUprightProjectionAxis,
              origin_x + local_x[point], origin_y + local_y[point],
              kContactLiftPixels, 0.0f,
              &face.points[point], &face.gpu_depth[point])) {
        valid = false;
        break;
      }
    }
    if (valid && !SimBackgroundVoxelProject_IsDegenerate(face.points))
      SimBackgroundVoxelProject_AppendFace(
          &face, palette, kSimBackgroundVoxelShading_MaterialAware);
  }
}

static bool AppendModel(const SimBackgroundVoxelObject *object,
                        const SimBackgroundVoxelRenderParams *params,
                        uint32_t stamp) {
  const SimBackgroundVoxelBiome biome =
      SimBackgroundVoxelBiome_ForTown(object->town);
  const SimBackgroundVoxelModelShadingKey shading_key = {
    .light_azimuth_deg = params->light_azimuth_deg,
    .light_elevation_deg = params->light_elevation_deg,
    .shading = params->shading,
    .biome = (uint8_t)biome,
  };
  const SimBackgroundVoxelModelShading *shading = NULL;
  const SimBackgroundVoxelModel *model = SimBackgroundVoxelModelCache_Get(
      object, kSimBackgroundVoxelDetail_Ultra,
      kSimBackgroundVoxelStyle_Varied, stamp,
      &shading_key, &shading);
  if (!model || !model->face_count || model->overflow || !shading)
    return false;

  SimBackgroundVoxelPalette palette;
  SimBackgroundVoxelPalette_Build(object, biome, &palette);
  const SimBackgroundVoxelProportions *proportions =
      SimBackgroundVoxelProportions_Get(
          (SimBackgroundVoxelKind)object->kind);
  const float center_x = FootprintWidth(object) * 0.5f;
  const float center_y = FootprintDepth(object) * 0.5f;
  /* All models share one world scale and datum.  Larger plots therefore look
   * larger on the sheet, preserving the in-game family proportions. */
  const float origin_x = 26.0f - center_x;
  const float origin_y = 35.0f - center_y;

  SimBackgroundProjectionAxis axes[kSimBackgroundVoxelKindCount];
  SimBackgroundVoxelProject_ResolveAxes(params, axes);
  const SimBackgroundProjectionAxis *axis = &axes[object->kind];
  AppendContact(object, &palette, params, origin_x, origin_y, proportions);

  for (uint16_t face_index = 0;
       face_index < model->face_count; face_index++) {
    const SimBackgroundVoxelModelFace *source = &model->faces[face_index];
    SimBackgroundProjectedFace face = {
      .material = shading->material[face_index],
    };
    memcpy(face.brightness, shading->brightness[face_index],
           sizeof(face.brightness));
    bool valid = true;
    for (int point = 0; point < 4; point++) {
      const float local_x = center_x +
          (source->points[point].x - center_x) *
              proportions->footprint_scale;
      const float local_y = center_y +
          (source->points[point].y - center_y) *
              proportions->footprint_scale;
      const float local_z = source->points[point].z *
          proportions->height_scale;
      if (!SimBackgroundVoxelProject_GroundedVertex(
              params, axis,
              origin_x + local_x, origin_y + local_y,
              local_z, 0.0f,
              &face.points[point], &face.gpu_depth[point])) {
        valid = false;
        break;
      }
    }
    if (!valid || SimBackgroundVoxelProject_IsDegenerate(face.points))
      continue;
    SimBackgroundVoxelProject_AppendFace(
        &face, &palette, kSimBackgroundVoxelShading_MaterialAware);
  }
  return true;
}

static bool SaveCurrentPass(SDL_Renderer *renderer, const char *path) {
  ArRenderTexture output_handle = Sim3DDepthPass_Submit(
      &g_render_device, ArRenderTexture_Invalid());
  SDL_Texture *output = ArSdlRenderBackend_UnwrapTexture(output_handle);
  if (!output || !SDL_SetRenderTarget(renderer, output)) return false;
  SDL_Surface *readback = SDL_RenderReadPixels(renderer, NULL);
  if (!readback) return false;
  SDL_Surface *argb = SDL_ConvertSurface(readback, SDL_PIXELFORMAT_ARGB8888);
  SDL_DestroySurface(readback);
  if (!argb) return false;
  const bool saved = SDL_SaveBMP(argb, path);
  SDL_DestroySurface(argb);
  SDL_SetRenderTarget(renderer, NULL);
  return saved;
}

static SimBackgroundVoxelObject BaseObject(
    SimBackgroundVoxelKind kind, uint8_t town) {
  SimBackgroundVoxelObject object = {
    .kind = (uint8_t)kind,
    .town = town,
    .cell_x = 7,
    .cell_y = 11,
    .record_slot = 2,
    .source_cells_w = 1,
    .source_cells_h = 1,
    .footprint_cells_w = 1,
    .footprint_cells_d = 1,
  };
  switch (kind) {
    case kSimBackgroundVoxel_Cathedral:
    case kSimBackgroundVoxel_Factory:
    case kSimBackgroundVoxel_StoryTree:
    case kSimBackgroundVoxel_BloodpoolCastle:
    case kSimBackgroundVoxel_MarahnaTemple:
    case kSimBackgroundVoxel_Pyramid:
      object.source_cells_w = object.source_cells_h = 2;
      object.footprint_cells_w = object.footprint_cells_d = 2;
      break;
    case kSimBackgroundVoxel_Windmill:
      object.source_cells_w = object.source_cells_h = 2;
      object.footprint_cells_w = 2;
      object.footprint_cells_d = 1;
      break;
    case kSimBackgroundVoxel_Bridge:
      break;
    case kSimBackgroundVoxel_House:
    case kSimBackgroundVoxel_Tree:
    case kSimBackgroundVoxel_BroadTree:
    case kSimBackgroundVoxel_Palm:
    case kSimBackgroundVoxel_Shrub:
      break;
  }
  return object;
}

static bool RenderEntry(SDL_Renderer *renderer,
                        const SimBackgroundVoxelRenderParams *base_params,
                        const char *output_dir,
                        const AuditEntry *entry,
                        FILE *manifest,
                        uint32_t stamp) {
  SimBackgroundVoxelRenderParams params = *base_params;
  params.town = entry->object.town;
  SimBackgroundVoxelProject_Prepare(&params);
  if (!Sim3DDepthPass_Begin(
          &g_render_device, kRenderWidth, kRenderHeight,
          kArRenderFilter_Nearest)) {
    fprintf(stderr, "depth begin failed: %s\n",
            Sim3DDepthPass_LastError());
    return false;
  }
  if (!AppendModel(&entry->object, &params, stamp)) {
    fprintf(stderr, "model build failed: %s\n", entry->label);
    return false;
  }
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s.bmp", output_dir, entry->filename);
  if (!SaveCurrentPass(renderer, path)) {
    fprintf(stderr, "save failed for %s: %s\n", path, SDL_GetError());
    return false;
  }
  fprintf(manifest, "%s\t%s\t%s.bmp\n",
          entry->section, entry->label, entry->filename);
  return true;
}

static bool EmitEntry(SDL_Renderer *renderer,
                      const SimBackgroundVoxelRenderParams *params,
                      const char *output_dir, FILE *manifest,
                      uint32_t *stamp,
                      const char *section, const char *filename,
                      const char *label,
                      SimBackgroundVoxelObject object) {
  const AuditEntry entry = {section, filename, label, object};
  return RenderEntry(renderer, params, output_dir, &entry, manifest,
                     (*stamp)++);
}

static bool RenderAll(SDL_Renderer *renderer, const char *output_dir,
                      FILE *manifest) {
  const Scene3DCamera camera = {
    .tilt_x = -0.35f,
    .tilt_y = 0.0f,
    .distance = Scene3D_AutoFitDistance(0.4f),
    .fov_y = 0.4f,
  };
  float matrix[16];
  Scene3D_BuildViewProjection(
      &camera, kRenderWidth, kRenderHeight, matrix);
  const SimBackgroundVoxelRenderParams params = {
    .detail = kSimBackgroundVoxelDetail_Ultra,
    .lod = kSimBackgroundVoxelLod_Fixed,
    .shading = kSimBackgroundVoxelShading_MaterialAware,
    .style = kSimBackgroundVoxelStyle_Varied,
    .facing = kSimBackgroundVoxelFacing_PerModel,
    .render_scale = kSimBackgroundVoxelRenderScale_PixelClean,
    .landscape_height_pct = 0,
    .light_azimuth_deg = 0,
    .light_elevation_deg = 85,
    .source = {0, 0, kSourcePixels, kSourcePixels},
    .viewport = {0, 0, kRenderWidth, kRenderHeight},
    .matrix = matrix,
  };
  uint32_t stamp = 1;
  static const char *town_names[6] = {
    "Fillmore", "Bloodpool", "Kasandora",
    "Aitos", "Marahna", "Northwall",
  };
  static const char *stage_names[3] = {
    "early", "middle", "developed",
  };

  for (int town = 1; town <= 6; town++) {
    for (int level = 0; level < 3; level++) {
      for (int alternate = 0; alternate < 2; alternate++) {
        char filename[96], label[160];
        snprintf(filename, sizeof(filename),
                 "house-%d-%d-%s", town, level,
                 alternate ? "alternate" : "front");
        snprintf(label, sizeof(label), "%s %s house - %s",
                 town_names[town - 1], stage_names[level],
                 alternate ? "alternate" : "front");
        SimBackgroundVoxelObject object = BaseObject(
            kSimBackgroundVoxel_House, (uint8_t)town);
        object.development_level = (uint8_t)level;
        object.flags = alternate
            ? kSimBackgroundVoxel_AlternateFacing : 0;
        if (!EmitEntry(renderer, &params, output_dir, manifest, &stamp,
                       "Regional houses", filename, label, object))
          return false;
      }
    }
  }

#define EMIT(section_, filename_, label_, object_)                       \
  do {                                                                   \
    if (!EmitEntry(renderer, &params, output_dir, manifest, &stamp,      \
                   section_, filename_, label_, object_))                \
      return false;                                                      \
  } while (0)

  SimBackgroundVoxelObject object = BaseObject(
      kSimBackgroundVoxel_Cathedral, 1);
  EMIT("Landmarks and infrastructure", "cathedral-temperate",
       "Cathedral - temperate", object);
  object.town = 6;
  EMIT("Landmarks and infrastructure", "cathedral-snow",
       "Cathedral - Northwall snow", object);

  for (int phase = 0; phase < 3; phase++) {
    char filename[64], label[96];
    snprintf(filename, sizeof(filename), "windmill-phase-%d", phase);
    snprintf(label, sizeof(label), "Windmill - blade phase %d", phase + 1);
    object = BaseObject(kSimBackgroundVoxel_Windmill, 1);
    object.animation_phase = (uint8_t)phase;
    EMIT("Landmarks and infrastructure", filename, label, object);
  }
  object = BaseObject(kSimBackgroundVoxel_Windmill, 6);
  EMIT("Landmarks and infrastructure", "windmill-snow",
       "Windmill - Northwall snow", object);

  object = BaseObject(kSimBackgroundVoxel_Factory, 1);
  EMIT("Landmarks and infrastructure", "factory-temperate",
       "Factory - temperate", object);
  object.town = 6;
  EMIT("Landmarks and infrastructure", "factory-snow",
       "Factory - Northwall snow", object);

  object = BaseObject(kSimBackgroundVoxel_BloodpoolCastle, 2);
  EMIT("Landmarks and infrastructure", "bloodpool-castle",
       "Bloodpool castle", object);
  object = BaseObject(kSimBackgroundVoxel_Pyramid, 3);
  EMIT("Landmarks and infrastructure", "kasandora-pyramid",
       "Kasandora pyramid", object);
  object = BaseObject(kSimBackgroundVoxel_MarahnaTemple, 5);
  EMIT("Landmarks and infrastructure", "marahna-temple",
       "Marahna temple", object);

  for (int town = 1; town <= 6; town++) {
    char filename[64], label[96];
    snprintf(filename, sizeof(filename), "tree-town-%d", town);
    snprintf(label, sizeof(label), "%s permanent tree", town_names[town - 1]);
    object = BaseObject(kSimBackgroundVoxel_Tree, (uint8_t)town);
    object.flags = kSimBackgroundVoxel_IsolatedTree;
    object.record_slot = kSimBackgroundVoxelNoRecordSlot;
    EMIT("Vegetation", filename, label, object);
  }
  object = BaseObject(kSimBackgroundVoxel_BroadTree, 3);
  object.flags = kSimBackgroundVoxel_IsolatedTree;
  object.record_slot = kSimBackgroundVoxelNoRecordSlot;
  EMIT("Vegetation", "broad-tree-kasandora",
       "Kasandora broad tree", object);
  object.town = 5;
  EMIT("Vegetation", "broad-tree-marahna",
       "Marahna broad tree", object);
  object = BaseObject(kSimBackgroundVoxel_Palm, 5);
  object.flags = kSimBackgroundVoxel_IsolatedTree;
  object.record_slot = kSimBackgroundVoxelNoRecordSlot;
  EMIT("Vegetation", "marahna-palm", "Marahna palm", object);
  object = BaseObject(kSimBackgroundVoxel_Shrub, 1);
  object.flags = kSimBackgroundVoxel_IsolatedTree;
  object.record_slot = kSimBackgroundVoxelNoRecordSlot;
  EMIT("Vegetation", "clearable-shrub", "Clearable shrub", object);
  object = BaseObject(kSimBackgroundVoxel_StoryTree, 6);
  object.record_slot = kSimBackgroundVoxelNoRecordSlot;
  EMIT("Vegetation", "northwall-story-tree",
       "Northwall ancient story tree", object);

  object = BaseObject(kSimBackgroundVoxel_Bridge, 1);
  object.cell_x = 1;
  object.cell_y = 2;
  object.bridge_axis = kSimBackgroundBridgeAxis_EastWest;
  object.bridge_bank_a_x = 0;
  object.bridge_bank_a_y = 2;
  object.bridge_bank_b_x = 3;
  object.bridge_bank_b_y = 2;
  EMIT("Bridges", "bridge-temperate-ew",
       "Temperate bridge - east/west", object);
  object.bridge_axis = kSimBackgroundBridgeAxis_NorthSouth;
  object.cell_x = 2;
  object.cell_y = 1;
  object.bridge_bank_a_x = 2;
  object.bridge_bank_a_y = 0;
  object.bridge_bank_b_x = 2;
  object.bridge_bank_b_y = 3;
  EMIT("Bridges", "bridge-temperate-ns",
       "Temperate bridge - north/south", object);
  object.town = 6;
  object.bridge_axis = kSimBackgroundBridgeAxis_EastWest;
  object.cell_x = 1;
  object.cell_y = 2;
  object.bridge_bank_a_x = 0;
  object.bridge_bank_a_y = 2;
  object.bridge_bank_b_x = 3;
  object.bridge_bank_b_y = 2;
  EMIT("Bridges", "bridge-snow-ew",
       "Northwall bridge - east/west", object);
  object.bridge_axis = kSimBackgroundBridgeAxis_NorthSouth;
  object.cell_x = 2;
  object.cell_y = 1;
  object.bridge_bank_a_x = 2;
  object.bridge_bank_a_y = 0;
  object.bridge_bank_b_x = 2;
  object.bridge_bank_b_y = 3;
  EMIT("Bridges", "bridge-snow-ns",
       "Northwall bridge - north/south", object);

  object = BaseObject(kSimBackgroundVoxel_House, 1);
  object.development_level = 2;
  object.flags = kSimBackgroundVoxel_UnderConstruction;
  EMIT("Construction", "construction-house",
       "House construction frame", object);
  for (int phase = 0; phase < 3; phase++) {
    char filename[64], label[96];
    snprintf(filename, sizeof(filename), "construction-windmill-%d", phase);
    snprintf(label, sizeof(label), "Windmill construction - phase %d",
             phase + 1);
    object = BaseObject(kSimBackgroundVoxel_Windmill, 1);
    object.flags = kSimBackgroundVoxel_UnderConstruction;
    object.animation_phase = (uint8_t)phase;
    EMIT("Construction", filename, label, object);
  }
  object = BaseObject(kSimBackgroundVoxel_Factory, 1);
  object.flags = kSimBackgroundVoxel_UnderConstruction;
  EMIT("Construction", "construction-factory",
       "Factory construction frame", object);

#undef EMIT
  return true;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s OUTPUT_DIRECTORY\n", argv[0]);
    return 2;
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "SDL video init failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window *window = SDL_CreateWindow(
      "SIM voxel audit", kRenderWidth, kRenderHeight, SDL_WINDOW_HIDDEN);
  if (!window) {
    fprintf(stderr, "window creation failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_Renderer *renderer = CreateProductionRenderer(window);
  if (!renderer) {
    fprintf(stderr, "GPU renderer creation failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  if (!ArSdlRenderBackend_Bind(
          &g_render_device, &g_render_backend, renderer)) {
    fprintf(stderr, "render-device binding failed: %s\n", SDL_GetError());
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  if (!Sim3DDepthPass_Require(&g_render_device)) {
    fprintf(stderr, "D32 pass unavailable: %s\n",
            Sim3DDepthPass_LastError());
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  char manifest_path[1024];
  snprintf(manifest_path, sizeof(manifest_path),
           "%s/manifest.tsv", argv[1]);
  FILE *manifest = fopen(manifest_path, "w");
  if (!manifest) {
    perror(manifest_path);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  fprintf(manifest, "section\tlabel\tfile\n");
  const bool rendered = RenderAll(renderer, argv[1], manifest);
  fclose(manifest);

  SimBackgroundVoxelModelCache_Reset();
  Sim3DDepthPass_Reset(&g_render_device);
  ArRenderDevice_Reset(&g_render_device);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return rendered ? 0 : 1;
}
