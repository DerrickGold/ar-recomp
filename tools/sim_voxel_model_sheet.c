/*
 * Render the production SIM voxel models in isolation for visual auditing.
 *
 * This deliberately goes through the same model cache, regional builders,
 * palettes, proportions, camera-facing projection, material lighting and D32
 * depth pass as the in-game renderer.  The only omitted scene input is terrain
 * elevation: every audit model stands on a common flat datum so silhouettes
 * and proportions can be compared directly.
 * Optional AR_AUDIT_DETAIL (0..3), AR_AUDIT_TILT_X/Y,
 * AR_AUDIT_LIGHT_ELEVATION and AR_AUDIT_REVERSE controls support repeatable
 * LOD, lighting and draw-order checks. Defaults retain the canonical sheet.
 */

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This SDL diagnostic needs native texture readback for its BMP output. */
#include "platform/sdl/render_sdl_internal.h"
#include "render/scene3d_math.h"
#include "sim/sim3d/sim3d_depth_pass.h"
#include "sim/sim3d/sim3d_performance.h"
#include "sim/voxels/sim_background_bridge.h"
#include "sim/voxels/sim_background_voxel_biome.h"
#include "sim/voxels/sim_background_voxel_model_cache.h"
#include "sim/voxels/sim_background_voxel_palette.h"
#include "sim/voxels/sim_background_voxel_project.h"
#include "sim/voxels/sim_background_voxel_proportions.h"

enum {
  kRenderWidth = 420,
  kRenderHeight = 360,
  kSourcePixels = 52,
};

static ArRenderDevice s_render_device;
/* Optional audit controls leave the default Ultra sheet unchanged. */
static int audit_detail = kSimBackgroundVoxelDetail_Ultra;
static int audit_reverse;

static const float kContactLiftPixels = 0.06f;

typedef struct AuditEntry {
  const char *section;
  const char *filename;
  const char *label;
  SimBackgroundVoxelObject object;
  int grid_size;
} AuditEntry;

/* The depth pass reports production work to this profiler hook. */
void Sim3DPerformance_AddDraw(uint64_t vertices, uint64_t indices) {
  (void)vertices;
  (void)indices;
}
void Sim3DPerformance_AddGeometryUpload(uint64_t bytes) { (void)bytes; }
void Sim3DPerformance_AddGeometryCopy(uint64_t bytes, uint64_t calls) { (void)bytes; (void)calls; }
void Sim3DPerformance_AddAtlasCopy(uint64_t bytes) { (void)bytes; }

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

static SimBackgroundVoxelModelContact ScaleContactBounds(
    SimBackgroundVoxelModelContact bounds, const SimBackgroundVoxelObject *object,
    const SimBackgroundVoxelProportions *proportions) {
  const float center_x = FootprintWidth(object) * 0.5f;
  const float center_y = FootprintDepth(object) * 0.5f;
  return (SimBackgroundVoxelModelContact){
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
  SimBackgroundVoxelModelContact authored[kSimBackgroundVoxelModelMaxContacts];
  const int count = SimBackgroundVoxelModel_Contacts(object, authored);
  for (int at = 0; at < count; at++) {
    const SimBackgroundVoxelModelContact bounds = ScaleContactBounds(
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
                        float offset_x, float offset_y) {
  const SimBackgroundVoxelBiome biome =
      SimBackgroundVoxelBiome_ForTown(object->town);
  const SimBackgroundVoxelModelShadingKey shading_key = {
    .light_azimuth_deg = params->light_azimuth_deg,
    .light_elevation_deg = params->light_elevation_deg,
    .shading = params->shading,
    .biome = (uint8_t)biome,
  };
  const SimBackgroundVoxelModelShading *shading = NULL;
  const SimBackgroundVoxelModelView *model = SimBackgroundVoxelModelCache_Get(
      object, audit_detail,
      kSimBackgroundVoxelStyle_Varied,
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
  const float origin_x = 26.0f - center_x + offset_x;
  const float origin_y = 35.0f - center_y + offset_y;

  SimBackgroundProjectionAxis axes[kSimBackgroundVoxelKindCount];
  SimBackgroundVoxelProject_ResolveAxes(params, axes);
  const SimBackgroundProjectionAxis *axis = &axes[object->kind];
  AppendContact(object, &palette, params, origin_x, origin_y, proportions);

  for (uint16_t face_index = 0;
       face_index < model->face_count; face_index++) {
    int idx = audit_reverse ? model->face_count - 1 - face_index : face_index;
    const SimBackgroundVoxelModelFace *source = &model->faces[idx];
    SimBackgroundProjectedFace face = {
      .material = shading->material[idx],
    };
    memcpy(face.brightness, shading->brightness[idx],
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
      &s_render_device, ArRenderTexture_Invalid());
  ArRenderTargetState previous;
  if (!ArRenderTexture_IsValid(output_handle) ||
      ArRenderDevice_BeginTarget(&s_render_device, output_handle, &previous) !=
          kArRenderTargetBegin_Ready)
    return false;
  SDL_Surface *readback = SDL_RenderReadPixels(renderer, NULL);
  if (!ArRenderDevice_EndTarget(&s_render_device, &previous)) {
    SDL_DestroySurface(readback);
    return false;
  }
  if (!readback) return false;
  SDL_Surface *argb = SDL_ConvertSurface(readback, SDL_PIXELFORMAT_ARGB8888);
  SDL_DestroySurface(readback);
  if (!argb) return false;
  const bool saved = SDL_SaveBMP(argb, path);
  SDL_DestroySurface(argb);
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
    case kSimBackgroundVoxel_AnimalPen:
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
    case kSimBackgroundVoxel_Boulder:
    case kSimBackgroundVoxel_Rocks:
    case kSimBackgroundVoxel_Shrub:
      break;
  }
  return object;
}

static bool RenderEntry(SDL_Renderer *renderer,
                        const SimBackgroundVoxelRenderParams *base_params,
                        const char *output_dir,
                        const AuditEntry *entry,
                        FILE *manifest) {
  SimBackgroundVoxelRenderParams params = *base_params;
  params.town = entry->object.town;
  SimBackgroundVoxelProject_Prepare(&params);
  if (!Sim3DDepthPass_Begin(
          &s_render_device, kRenderWidth, kRenderHeight,
          kArRenderFilter_Nearest)) {
    fprintf(stderr, "depth begin failed: %s\n",
            Sim3DDepthPass_LastError());
    return false;
  }
  int grid = entry->grid_size ? entry->grid_size : 1;
  for (int y = 0; y < grid; y++)
    for (int x = 0; x < grid; x++) {
      SimBackgroundVoxelObject object = entry->object;
      object.cell_x += x;
      object.cell_y += y;
      if (grid > 1)
        object.tree_edges = (y > 0 ? kSimBackgroundTreeEdge_North : 0) |
            (x + 1 < grid ? kSimBackgroundTreeEdge_East : 0) |
            (y + 1 < grid ? kSimBackgroundTreeEdge_South : 0) |
            (x > 0 ? kSimBackgroundTreeEdge_West : 0);
      if (!AppendModel(&object, &params, (x - (grid - 1) * .5f) * 16,
                       (y - (grid - 1) * .5f) * 16)) {
        fprintf(stderr, "model build failed: %s\n", entry->label);
        return false;
      }
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
                      const char *section, const char *filename,
                      const char *label,
                      SimBackgroundVoxelObject object) {
  const AuditEntry entry = {section, filename, label, object};
  return RenderEntry(renderer, params, output_dir, &entry, manifest);
}

static bool RenderAll(SDL_Renderer *renderer, const char *output_dir,
                      FILE *manifest) {
  const Scene3DCamera camera = {
    .tilt_x = getenv("AR_AUDIT_TILT_X") ? atof(getenv("AR_AUDIT_TILT_X")) : -0.35f,
    .tilt_y = getenv("AR_AUDIT_TILT_Y") ? atof(getenv("AR_AUDIT_TILT_Y")) : 0.0f,
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
    .light_elevation_deg = getenv("AR_AUDIT_LIGHT_ELEVATION") ? atoi(getenv("AR_AUDIT_LIGHT_ELEVATION")) : 85,
    .source = {0, 0, kSourcePixels, kSourcePixels},
    .viewport = {0, 0, kRenderWidth, kRenderHeight},
    .matrix = matrix,
  };
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
        if (!EmitEntry(renderer, &params, output_dir, manifest,
                       "Regional houses", filename, label, object))
          return false;
      }
    }
  }

#define EMIT(section_, filename_, label_, object_)                         \
  do {                                                                   \
    if (!EmitEntry(renderer, &params, output_dir, manifest,                \
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

  /* Append new families so the historical 67 model numbers remain stable. */
  object = BaseObject(kSimBackgroundVoxel_Boulder, 1);
  object.visual_metatile = 0x61;
  EMIT("Town environment", "fillmore-boulder", "Fillmore lightning boulder", object);
  static const uint8_t rock_tiles[] = {0x62, 0x63, 0x69, 0x6A, 0x6B};
  for (unsigned at = 0; at < sizeof(rock_tiles); at++) {
    char filename[64], label[96];
    object = BaseObject(kSimBackgroundVoxel_Rocks, 4);
    object.visual_metatile = rock_tiles[at];
    snprintf(filename, sizeof(filename), "rocky-ground-%02x", rock_tiles[at]);
    snprintf(label, sizeof(label), "Aitos scattered rocks - $%02X", rock_tiles[at]);
    EMIT("Town environment", filename, label, object);
  }

  object = BaseObject(kSimBackgroundVoxel_AnimalPen, 4);
  EMIT("Town environment", "aitos-animal-pen", "Aitos wooden animal pen", object);

#undef EMIT
  /* Keep the historical 67 baseline numbers stable. Regional geometry gets
   * an auxiliary manifest and is selected by the ROM-aware art index. */
  char regional_path[1024];
  snprintf(regional_path, sizeof(regional_path), "%s/regional-manifest.tsv", output_dir);
  FILE *regional = fopen(regional_path, "w");
  if (!regional) { perror(regional_path); return false; }
  fprintf(regional, "section\tlabel\tfile\n");
  object = BaseObject(kSimBackgroundVoxel_Pyramid, 3);
  object.flags = kSimBackgroundVoxel_PyramidEye;
  bool rendered = EmitEntry(renderer, &params, output_dir, regional,
      "Regional variants", "kasandora-pyramid-jp", "Kasandora pyramid - Japanese eye", object);
  for (int alternate = 0; rendered && alternate < 2; alternate++) {
    object = BaseObject(kSimBackgroundVoxel_House, 5);
    object.development_level = 2;
    object.visual_state = kSimStructureVisualState_Finished;
    object.visual_metatile = alternate ? 0x3B : 0x3A;
    object.flags = alternate ? kSimBackgroundVoxel_AlternateFacing : 0;
    rendered = EmitEntry(renderer, &params, output_dir, regional, "Regional variants",
        alternate ? "house-5-2-jp-alternate" : "house-5-2-jp-front",
        alternate ? "Marahna developed - Japanese stilt alternate"
                  : "Marahna developed - Japanese stilt front", object);
  }
  fclose(regional);
  if (!rendered) return false;
  /* Supplementary states stay outside the numbered/regional manifests. */
  char states_path[1024];
  snprintf(states_path, sizeof(states_path), "%s/state-manifest.tsv", output_dir);
  FILE *states = fopen(states_path, "w");
  if (!states) { perror(states_path); return false; }
  fprintf(states, "section\tlabel\tfile\n");
  object = BaseObject(kSimBackgroundVoxel_House, 1);
  object.flags = kSimBackgroundVoxel_UnderConstruction;
  object.animation_phase = 1;
  object.development_level = 2;
  rendered = EmitEntry(renderer, &params, output_dir, states, "Supplementary states",
      "construction-house-1", "House construction - second stage", object);
  for (int family = 0; rendered && family < 2; family++)
    for (int phase = 0; rendered && phase < 2; phase++) {
      char filename[64], label[96];
      object = BaseObject(kSimBackgroundVoxel_House, family ? 3 : 4);
      object.development_level = family ? 1 : 0;
      object.flags = kSimBackgroundVoxel_UnderConstruction;
      object.animation_phase = (uint8_t)phase;
      snprintf(filename, sizeof(filename), "construction-%s-%d", family ? "canvas" : "straw", phase);
      snprintf(label, sizeof(label), "%s shelter - %s", family ? "Canvas" : "Straw",
          phase ? "partial covering" : "lashed pole frame");
      rendered = EmitEntry(renderer, &params, output_dir, states, "Supplementary states",
          filename, label, object);
    }
  for (int phase = 0; rendered && phase < 2; phase++) {
    object = BaseObject(kSimBackgroundVoxel_House, 4);
    object.development_level = 0;
    object.flags = kSimBackgroundVoxel_UnderConstruction | kSimBackgroundVoxel_AlternateFacing;
    object.animation_phase = (uint8_t)phase;
    rendered = EmitEntry(renderer, &params, output_dir, states, "Supplementary states",
        phase ? "construction-straw-alternate-1" : "construction-straw-alternate-0",
        phase ? "Straw hut alternate - partial crest covering" : "Straw hut alternate - crest frame",
        object);
  }
  for (int axis = 0; rendered && axis < 2; axis++) {
    object = BaseObject(kSimBackgroundVoxel_Bridge, 1);
    object.flags = kSimBackgroundVoxel_UnderConstruction;
    object.bridge_axis = axis ? kSimBackgroundBridgeAxis_NorthSouth : kSimBackgroundBridgeAxis_EastWest;
    object.cell_x = axis ? 2 : 1; object.cell_y = axis ? 1 : 2;
    object.bridge_bank_a_x = axis ? 2 : 0; object.bridge_bank_a_y = axis ? 0 : 2;
    object.bridge_bank_b_x = axis ? 2 : 3; object.bridge_bank_b_y = axis ? 3 : 2;
    rendered = EmitEntry(renderer, &params, output_dir, states, "Supplementary states",
        axis ? "construction-bridge-ns" : "construction-bridge-ew",
        axis ? "Bridge construction - north/south" : "Bridge construction - east/west", object);
  }
  fclose(states);
  if (!rendered) return false;
  /* Forest previews have a separate manifest to preserve historical audit
   * numbering. Include joined patches at production spacing, not just a cell. */
  char forest_path[1024];
  snprintf(forest_path, sizeof(forest_path), "%s/forest-manifest.tsv", output_dir);
  FILE *forest = fopen(forest_path, "w");
  if (!forest) { perror(forest_path); return false; }
  fprintf(forest, "section\tlabel\tfile\n");
  for (int town = 1; rendered && town <= 6; town++) {
    char filename[64], label[96];
    object = BaseObject(kSimBackgroundVoxel_Tree, (uint8_t)town);
    object.tree_edges = 15;
    object.record_slot = kSimBackgroundVoxelNoRecordSlot;
    snprintf(filename, sizeof(filename), "forest-town-%d", town);
    snprintf(label, sizeof(label), "%s conifer forest cell", town_names[town - 1]);
    rendered = EmitEntry(renderer, &params, output_dir, forest,
        "Forest clusters", filename, label, object);
  }
  for (int town = 3; rendered && town <= 5; town += 2) {
    char filename[64], label[96];
    object = BaseObject(kSimBackgroundVoxel_BroadTree, (uint8_t)town);
    object.tree_edges = 15;
    object.record_slot = kSimBackgroundVoxelNoRecordSlot;
    snprintf(filename, sizeof(filename), "broad-forest-town-%d", town);
    snprintf(label, sizeof(label), "%s broadleaf forest cell", town_names[town - 1]);
    rendered = EmitEntry(renderer, &params, output_dir, forest,
        "Forest clusters", filename, label, object);
  }
  static const int patch_towns[] = {1, 5, 6};
  for (int patch = 0; rendered && patch < 3; patch++) {
    int town = patch_towns[patch];
    char filename[64], label[96];
    object = BaseObject(town == 5 ? kSimBackgroundVoxel_BroadTree : kSimBackgroundVoxel_Tree,
                        (uint8_t)town);
    object.tree_edges = 15;
    object.record_slot = kSimBackgroundVoxelNoRecordSlot;
    snprintf(filename, sizeof(filename), "forest-patch-town-%d", town);
    snprintf(label, sizeof(label), "%s joined forest patch", town_names[town - 1]);
    const AuditEntry entry = {"Forest clusters", filename, label, object, 2};
    rendered = RenderEntry(renderer, &params, output_dir, &entry, forest);
  }
  fclose(forest);
  return rendered;
}

int main(int argc, char **argv) {
  if (getenv("AR_AUDIT_DETAIL")) audit_detail = atoi(getenv("AR_AUDIT_DETAIL"));
  audit_reverse = getenv("AR_AUDIT_REVERSE") != NULL;
  if (audit_detail < 0 || audit_detail >= kSimBackgroundVoxelDetail_Count) {
    fprintf(stderr, "AR_AUDIT_DETAIL must be 0..3\n");
    return 2;
  }
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
  if (!ArSdlRenderBackend_CreateForWindow(&s_render_device, window, NULL)) {
    fprintf(stderr, "GPU renderer creation failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(&s_render_device);
  if (!Sim3DDepthPass_Require(&s_render_device)) {
    fprintf(stderr, "D32 pass unavailable: %s\n",
            Sim3DDepthPass_LastError());
    ArSdlRenderBackend_Destroy(&s_render_device);
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
    ArSdlRenderBackend_Destroy(&s_render_device);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  fprintf(manifest, "section\tlabel\tfile\n");
  const bool rendered = RenderAll(renderer, argv[1], manifest);
  fclose(manifest);

  SimBackgroundVoxelModelCache_Reset();
  Sim3DDepthPass_Reset(&s_render_device);
  ArSdlRenderBackend_Destroy(&s_render_device);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return rendered ? 0 : 1;
}
