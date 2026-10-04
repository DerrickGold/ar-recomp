#ifndef AR_PRESENT_WORLD_NAV_INTERNAL_H
#define AR_PRESENT_WORLD_NAV_INTERNAL_H
/* PresentWorldNav internals: what the world-navigation presenter's parts
 * share: the resource-owner state (defined in present_world_nav.c), the shared
 * types and constants, and the helpers one part calls in another. Not a public
 * API; only present_world_nav*.c and present_sim_globe.c include it.
 * Phase: present (FrameSlot only). */
#include "sim/world_nav/present_world_nav_geometry.h"
#include "sim/world_nav/present_world_nav_composition.h"
#include "sim/world_nav/present_world_nav_sky.h"
#include "sim/world_nav/present_world_nav_model_mesh.h"
#include "sim/world_nav/present_sim_globe_focus.h"
#include "sim/world_nav/present_world_nav_test.h"
#include "host/parallel_work.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "present/present.h"
#include "action/action_effect_render.h"
#include "actraiser/actraiser_localization_world_navigation.h"
#include "constants.h"
#include "deterministic_hash.h"
#include "snesrecomp/game/types.h"
#include "diorama/diorama.h"
#include "host/host_clock.h"
#include "app/performance_metrics.h"
#include "present/presentation_outcome.h"
#include "present/presentation_upload_mirror.h"
#include "render/render_device.h"
#include "render/render_output.h"
#include "render/localized_text_presenter.h"
#include "render/scene3d_math.h"
#include "sim/sim3d/sim3d_camera_limits.h"
#include "sim/sim3d/sim3d_depth_pass.h"
#include "sim/sim3d/sim3d_mesh_set.h"
#include "sim/sim3d/sim3d_performance.h"
#include "sim/voxels/sim_background_bridge.h"
#include "sim/voxels/sim_background_voxel_biome.h"
#include "sim/voxels/sim_background_voxel_model_cache.h"
#include "sim/voxels/sim_background_voxel_palette.h"
#include "sim/voxels/sim_background_voxel_proportions.h"
#include "sim/sim_world_map.h"
#include "sim/world_nav/sim_world_navigation_art.h"
#include "sim/world_nav/sim_world_navigation_capture.h"
#include "sim/world_nav/sim_world_navigation_globe.h"
#include "sim/world_nav/sim_world_navigation_terrain.h"
#include "sim/world_nav/sim_world_navigation_mountains.h"
#include "sim/world_nav/sim_world_navigation_mountain_transition.h"
#include "sim/world_nav/sim_world_navigation_clouds.h"
#include "sim/world_nav/sim_world_navigation_cliffs.h"
#include "sim/town/sim_town_ground_art.h"
#include "sim/sim3d/sim3d.h"
#include "sim/world_nav/present_sim_globe_mapping.h"

#ifndef AR_SIM3D_TERRAIN_ELEVATION
#define AR_SIM3D_TERRAIN_ELEVATION 0
#endif

/* kPixelAspect_Crt43 and kDioramaCam_Free/kDioramaCam_Dynamic are plain enum
 * constants (not live state) — fine to pull in just for those. */
#include "app/settings.h"
#include "host/host_video.h"
#include "sim/sim3d/present_sim3d_environment.h"
#include "sim/world_nav/present_sim_globe.h"
#include "sim/world_nav/present_sim_globe_mountains.h"
#include "sim/world_nav/present_sim_globe_terrain.h"
#include "sim/world_nav/present_sim_globe_water.h"
#include "sim/sim3d/present_sim3d_project.h"
#include "sim/voxels/sim_background_voxels.h"


typedef struct WorldNavigationModelBounds {
  float minimum_rise, maximum_rise, angular_radius;
} WorldNavigationModelBounds;
typedef struct WorldNavigationCliffProjection {
  Sim3DDepthVertex depth[4];
  Scene3DClipPoint clip[4];
  ArRenderColorF colour[4];
  float normal[4][3];
  ArRenderPointF cloud_uv[kSimCloudLayerCount][4];
  uint8_t outside[4];
} WorldNavigationCliffProjection;


enum {
  kWorldNavigationTerrainCells = kSimWorldMapTiles,
  kWorldNavigationTerrainAxis = kWorldNavigationTerrainCells + 1,
  kWorldNavigationTerrainVertexCount =
      kWorldNavigationTerrainAxis * kWorldNavigationTerrainAxis,
  kWorldNavigationOceanRings = 48,
  kWorldNavigationOceanSectors = 96,
  kWorldNavigationOceanQuads = kWorldNavigationOceanRings*kWorldNavigationOceanSectors,
  kWorldNavigationOceanVertexCount =
      1 + kWorldNavigationOceanRings * kWorldNavigationOceanSectors,
  kWorldNavigationOceanIndexCount =
      kWorldNavigationOceanSectors * 3 +
      (kWorldNavigationOceanRings - 1) *
          kWorldNavigationOceanSectors * 6,
};

_Static_assert(kWorldNavigationTerrainVertexCount <= UINT16_MAX &&
    kWorldNavigationOceanVertexCount <= UINT16_MAX,
    "shadow receiver corner indices must cover both surface grids");

typedef struct WorldNavigationShellGeometry {
  WorldNavigationProjection projection;
  ArRenderRectI viewport;
  bool ready;
  Sim3DDepthVertex points[kWorldNavigationOceanVertexCount];
  bool occlusion;
  int first_index, first_vertex;
  Scene3DClipPoint clip[kWorldNavigationOceanVertexCount];
  float normal[kWorldNavigationOceanVertexCount][3];
  float alpha[kWorldNavigationOceanVertexCount];
  bool front[kWorldNavigationOceanVertexCount];
  uint8_t outside[kWorldNavigationOceanVertexCount];
} WorldNavigationShellGeometry;

typedef enum WorldNavigationShell {
  kWorldNavigationShell_Ocean,
  kWorldNavigationShell_Atmosphere,
  kWorldNavigationShell_Cloud,
} WorldNavigationShell;

typedef struct WorldNavigationAtmosphereDrawCache {
  ArRenderVertex2D *vertices;
  int32_t *indices;
  size_t quad_count, capacity;
  bool ready, repeated, unavailable;
} WorldNavigationAtmosphereDrawCache;

static const float kWorldNavigationTerrainAmbient = 0.68f;

typedef struct WorldNavigationGroundKey {
  WorldNavigationProjection projection;
  float source_to_screen[6];
  ArRenderRectI viewport;
  uint32_t geography_serial;
  int snes_width, snes_height, visible_width, visible_x0;
  int visible_height, visible_top;
  int light_azimuth, light_elevation, lighting;
  Sim3DDepthSurfaceFocus focus;
} WorldNavigationGroundKey;

typedef struct WorldNavigationGroundSample {
  float normal[3][3]; /* centre, half-cell east, half-cell south */
  float height[3];
  float edge_alpha;
} WorldNavigationGroundSample;
typedef struct WorldNavigationGroundSampleKey {
  float chart_radius_tiles;
  uint32_t geography_serial, cliff_serial;
  bool heights;
} WorldNavigationGroundSampleKey;
typedef struct WorldNavigationMountainProjection {
  float normal[4][3], floor[4], rise[4];
  Sim3DDepthVertex points[4];
  Scene3DClipPoint clip[4];
  bool visible;
} WorldNavigationMountainProjection;
typedef struct WorldNavigationMountainProjectionKey {
  WorldNavigationProjection projection;
  int width, height, lighting;
  Sim3DDepthSurfaceFocus focus;
} WorldNavigationMountainProjectionKey;

typedef struct WorldNavigationModelProjectionKey {
  WorldNavigationProjection projection;
  ArRenderRectI viewport;
  uint32_t model_revision, surface_revision;
  int height_scale, light_azimuth, light_elevation, lighting;
  Sim3DDepthSurfaceFocus focus;
} WorldNavigationModelProjectionKey;

/* Static spans retain their original position around animated objects.
 * End indexes the existing 8 MiB-bounded vertex cache; object indexes the
 * current immutable capture, never a retained FrameSlot/model-cache pointer. */
typedef struct WorldNavigationAnimatedModel {
  uint32_t static_end;
  uint16_t object;
  uint8_t detail;
} WorldNavigationAnimatedModel;

typedef struct WorldNavigationCloudShellKey {
  WorldNavigationProjection projection;
  ArRenderRectI viewport;
  SimWorldNavigationCloudRotation rotation;
} WorldNavigationCloudShellKey;

typedef struct WorldNavigationCloudUVCache {
  ArRenderPointF ground[kWorldNavigationTerrainVertexCount];
  ArRenderPointF ocean[kWorldNavigationOceanVertexCount];
  ArRenderPointF body[kWorldNavigationOceanVertexCount];
  float body_direction[kWorldNavigationOceanVertexCount][3];
  bool ground_ready, ocean_ready, body_ready;
  SimWorldNavigationCloudRotation ground_rotation;
  WorldNavigationCloudShellKey ocean_key, body_key;
  uint32_t cliff_serial;
  SimWorldNavigationCloudRotation cliff_rotation;
} WorldNavigationCloudUVCache;

typedef enum WorldNavigationReceiverSurface {
  kWorldNavigationReceiver_Ground,
  kWorldNavigationReceiver_Cliff,
  kWorldNavigationReceiver_Ocean,
} WorldNavigationReceiverSurface;
typedef struct WorldNavigationShadowReceiver {
  WorldNavigationClipPlan geometry;
  uint16_t corners[4];
  uint32_t cliff;
  WorldNavigationReceiverSurface surface;
} WorldNavigationShadowReceiver;
typedef struct WorldNavigationReceiverKey {
  WorldNavigationProjection projection;
  ArRenderRectI viewport;
  uint32_t terrain_serial, cliff_serial;
} WorldNavigationReceiverKey;

/* Shared ocean/land/cliff/mountain source replaces (never adds to) the world's
 * opaque retention slot. Each material keeps its own transform and atlas. */
enum {
  kWorldNavigationGridChunkCells = 16,
  kWorldNavigationGridChunkAxis = kWorldNavigationTerrainCells / kWorldNavigationGridChunkCells,
  kWorldNavigationGridChunks = kWorldNavigationGridChunkAxis * kWorldNavigationGridChunkAxis,
  kWorldNavigationSurfaceChunks =
      kWorldNavigationGridChunks + 3, /* ocean, land, cliffs, mountains */
  kWorldNavigationSurfaceMaximumQuads = kSim3DMeshSetMaximumQuads,
};
_Static_assert(kWorldNavigationTerrainCells % kWorldNavigationGridChunkCells == 0 &&
    (kWorldNavigationSurfaceChunks + 1) / 2 <= 64,
    "Adjacent source chunks merge into at most 64 selected runs");
_Static_assert(kWorldNavigationOceanQuads +
                       kWorldNavigationTerrainCells * kWorldNavigationTerrainCells +
                       kSimWorldNavigationCliffMaximumFaces +
                       kSimWorldNavigationMountainMaximumFaces <=
                   kWorldNavigationSurfaceMaximumQuads,
               "all authored world surfaces must fit the partitioned source");

typedef struct WorldNavigationGridSourceWork {
  const WorldNavigationGroundSample *samples;
  Sim3DDepthSurfaceVertex *vertices;
  float height_ratio;
} WorldNavigationGridSourceWork;

typedef struct WorldNavigationCliffSourceWork {
  const SimWorldNavigationCliffFace *faces;
  Sim3DDepthSurfaceVertex *vertices;
  ArRenderPointF *mask_uv;
  bool *valid;
  float chart_radius, height_ratio;
} WorldNavigationCliffSourceWork;

typedef struct WorldNavigationMountainSourceWork {
  const SimWorldNavigationMountainFace *faces;
  Sim3DDepthSurfaceVertex *vertices;
  ArRenderPointF *mask_uv;
  bool *valid;
  float chart_radius;
} WorldNavigationMountainSourceWork;

/* ---- state shared by the parts; defined in present_world_nav.c ---- */
typedef struct WorldNavigationArtState {
  uint8_t tilemap[kSimWorldMapBytes];
  uint32_t serial;
  uint64_t image_revision;
  uint32_t geography;
  SimWorldNavigationTownGround sources;
  bool detailed;
  bool models;
  uint8_t phase;
  uint32_t blur_serial;
  bool unavailable;
  uint32_t *pixels;
  SimWorldNavigationArtAnimation *animation;
  bool animation_unavailable;
  bool cliffs;
  Sim3DDepthAtlasCache *atlas_cache;
  bool atlas_cache_unavailable;
  int displayed_version; /* Per-frame selection only; never a CPU publication key. */
} WorldNavigationArtState;
extern WorldNavigationArtState g_world_nav_art;

typedef struct WorldNavigationMountainState {
  SimWorldNavigationMountainScene scene;
  SimWorldNavigationTownGround sources;
  bool ready;
  SimWorldNavigationMountainAtlasUpdate lava_upload;
  uint64_t atlas_revision;
  bool active;
  bool developed;
  SimWorldNavigationMountainTransition transition;
  bool transition_ready;
  uint32_t transition_serial;
  float chart_radius_tiles;
  int relief_pct;
  WorldNavigationMountainProjection *projection;
  size_t projection_capacity;
  bool samples_ready;
  bool projection_ready;
  bool projection_unavailable;
  WorldNavigationMountainProjectionKey projection_key;
  uint32_t geometry_revision;
} WorldNavigationMountainState;
extern WorldNavigationMountainState g_world_nav_mountains;

typedef struct WorldNavigationModelState {
  SimWorldNavigationTownObject
      objects[kSimWorldNavigationTownObjectCapacity];
  WorldNavigationModelBounds
      bounds[kSimWorldNavigationTownObjectCapacity];
  uint16_t object_count;
  int detail;
  int style;
  float chart_radius_tiles;
  float maximum_rise;
  uint32_t revision;
  WorldNavigationModelSource *gpu_sources;
  size_t gpu_source_capacity;
  bool gpu_sources_unavailable;
  bool gpu_current_ready;
  bool gpu_current_rejected;
  WorldNavigationModelProjectionKey projection_key;
  Sim3DDepthVertex *projected;
  size_t projected_count, projected_capacity;
  bool projected_valid, projection_key_ready, capturing, projection_unavailable;
  bool capture_static;
  Sim3DDepthMesh *solid_mesh;
  bool solid_mesh_published, solid_mesh_unavailable, solid_mesh_attempted;
  bool solid_mesh_opt_out;
  uint16_t animated_count;
  WorldNavigationAnimatedModel animated[kSimWorldNavigationTownObjectCapacity];
} WorldNavigationModelState;
extern WorldNavigationModelState g_world_nav_models;

typedef struct WorldNavigationTerrainState {
  SimWorldNavigationCliffScene cliffs;
  WorldNavigationCliffProjection *cliff_projection;
  uint32_t cliff_serial;
  uint32_t cliff_geography;
  float cliff_chart_radius_tiles;
  uint8_t cliff_mask;
  bool cliffs_ready;
  bool ready;
  uint32_t serial;
  float maximum_height;
  float height[
      kWorldNavigationTerrainVertexCount];
  float floor[
      kWorldNavigationTerrainVertexCount];
  float authored[
      kWorldNavigationTerrainVertexCount];
  ArRenderVertex2D vertices[
      kWorldNavigationTerrainVertexCount];
  Sim3DDepthVertex depth[
      kWorldNavigationTerrainVertexCount];
  Scene3DClipPoint clip[
      kWorldNavigationTerrainVertexCount];
  uint8_t outside[kWorldNavigationTerrainVertexCount];
  WorldNavigationGroundKey projection_key;
  bool projection_ready;
  WorldNavigationGroundSample samples[kWorldNavigationTerrainVertexCount];
  WorldNavigationGroundSampleKey sample_key;
  bool samples_ready;
} WorldNavigationTerrainState;
extern WorldNavigationTerrainState g_world_nav_terrain;

typedef struct WorldNavigationSurfaceState {
  Sim3DDepthMesh *mesh;
  Sim3DDepthGeometryRange ranges[2];
  WorldNavigationGroundKey retained_key;
  uint32_t cliff_serial, mountain_revision;
  bool key_ready, published, unavailable, attempted, opt_out, mountains_retained;
} WorldNavigationSurfaceState;
extern WorldNavigationSurfaceState g_world_nav_surfaces;

typedef struct WorldNavigationGpuGridState {
  Sim3DMeshSet meshes;
  WorldNavigationGroundSampleKey key;
  WorldNavigationProjection rejected_projection;
  uint32_t rejected_geography, rejected_cliffs, rejected_mountains;
  float height_ratio;
  size_t source_quads, selected_quads, mountain_quads;
  uint32_t mountain_revision;
  struct {
    WorldNavigationRadialBounds bounds;
    Sim3DDepthMeshRange range;
  } chunks[kWorldNavigationSurfaceChunks];
  Sim3DDepthRadialTransform selection_transform;
  bool cull, cull_unavailable, selection_ready;
  bool attempted, enabled, unavailable, ready, drawn;
} WorldNavigationGpuGridState;
extern WorldNavigationGpuGridState g_world_nav_gpu_grid;

typedef struct WorldNavigationShellState {
  WorldNavigationShellGeometry cloud;
  WorldNavigationShellGeometry ocean;
  WorldNavigationShellGeometry atmosphere;
  WorldNavigationAtmosphereDrawCache atmosphere_draw;
  bool indices_ready;
  float longitude_cos[kWorldNavigationOceanSectors];
  float longitude_sin[kWorldNavigationOceanSectors];
  int32_t indices[
      kWorldNavigationOceanIndexCount];
  /* Navigation submits only the retained atmosphere vertex suffix. Keep
   * its rebased indices separate from the ocean/cloud topology. */
  int32_t atmosphere_indices[kWorldNavigationOceanIndexCount];
  ArRenderVertex2D vertices[
      kWorldNavigationOceanVertexCount];
} WorldNavigationShellState;
extern WorldNavigationShellState g_world_nav_shells;

typedef struct WorldNavigationWeatherState {
  bool unavailable;
  bool failure_reported;
  bool ready;
  float ground_normals[kWorldNavigationTerrainVertexCount][3];
  bool normals_ready;
  float chart_radius_tiles;
  WorldNavigationCloudUVCache uv[kSimCloudLayerCount];
  WorldNavigationShadowReceiver *receivers;
  size_t receiver_count, receiver_capacity;
  WorldNavigationReceiverKey receiver_key;
  bool receivers_ready, receivers_unavailable;
  Sim3DDepthMesh *receiver_mesh;
  Sim3DDepthPosition *receiver_positions;
  ArRenderPointF *receiver_uv;
  size_t receiver_mesh_capacity;
  bool receiver_mesh_ready, receiver_mesh_unavailable;
  Sim3DDepthMesh *spherical_mesh;
  Sim3DDepthSphericalQuad *spherical_quads;
  size_t spherical_capacity;
  bool spherical_ready, spherical_unavailable;
  Sim3DDepthMesh *body_mesh;
  Sim3DDepthSphericalBodyVertex *body_vertices;
  float body_radius, body_distance;
  bool body_unavailable;
} WorldNavigationWeatherState;
extern WorldNavigationWeatherState g_world_nav_weather;
#if AR_WORLD_NAV_CACHE_TESTING
extern size_t g_world_nav_model_test_bytes, g_world_nav_receiver_test_bytes;
#endif

/* ---- defined in present_world_nav.c ---- */
float WorldNavigationChartRadius(const FrameSlot *slot);
HostParallelWork *WorldNavigationWorkers(void);
bool EnsureWorldNavigationBlur(const FrameSlot *slot);
bool EnsureWorldNavigationResourcesAtRadius(const FrameSlot *slot, float radius_tiles);

/* ---- defined in present_world_nav_terrain.c ---- */
int WorldNavigationTerrainVertexIndex(int tile_x, int tile_y);
void EnsureWorldNavigationCliffs(const FrameSlot *slot, float radius_tiles);
void PrepareWorldNavigationTerrain(void);
void ResetWorldNavigationTerrain(void);
float WorldNavigationTerrainHeightAtPrepared(float source_x, float source_y,
                                                float *authored_weight, bool floor_only);
float WorldNavigationTerrainHeightAtImpl(float source_x, float source_y,
    float *authored_weight, bool floor_only);
float WorldNavigationTerrainHeightAt(float source_x, float source_y,
                                            float *authored_weight);
bool WorldNavigationSurfaceNormal(
    const WorldNavigationProjection *projection,
    float source_x, float source_y, float normal[3]);
void WorldNavigationRadialPoint(
    const WorldNavigationProjection *projection, const float normal[3],
    float radial_height, float out[3]);
bool WorldNavigationProjectSurface(
    ArRenderRectI viewport,
    const WorldNavigationProjection *projection,
    float source_x, float source_y, bool terrain,
    float height_offset_world, ArRenderPointF *out);
float WorldNavigationShadeFromPoints(const float light[3],
    const float centre[3], const float east[3], const float south[3]);
float WorldNavigationSurfaceShade(
    bool lighting,
    const WorldNavigationProjection *projection,
    const float light[3], float source_x, float source_y);
bool PrepareWorldNavigationGroundSamples(const WorldNavigationProjection *projection);
void BuildWorldNavigationGridSourceRange(void *context, size_t first, size_t end);
void BuildWorldNavigationCliffSourceRange(void *context, size_t first, size_t end);
void BuildWorldNavigationMountainSourceRange(void *context, size_t first, size_t end);
WorldNavigationRadialBounds WorldNavigationSourceBounds(
    const Sim3DDepthSurfaceVertex *vertices, size_t quads);
bool BuildWorldNavigationOceanSource(Sim3DDepthSurfaceVertex *quads, ArRenderPointF *mask);
bool WorldNavigationShellFrame(const WorldNavigationProjection *projection,
    float radius, float centre_z, float outward[3], float right[3], float up[3], float *distance);
bool WorldNavigationOceanTransform(const WorldNavigationProjection *projection,
    Sim3DDepthSurfaceTransform *out);

/* ---- defined in present_world_nav_ground.c ---- */
bool WorldNavigationGpuGridEnabled(void);
void PrepareWorldNavigationOceanIndices(void);
bool DrawWorldNavigationSphereShell(
    ArRenderRectI viewport,
    const WorldNavigationProjection *projection, WorldNavigationShell kind, float ocean_gain);
bool DrawWorldNavigationSpaceBackdrop(ArRenderRectI viewport);
bool DrawWorldNavigationSurfaceLayers(const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection, uint64_t elapsed_ms);
bool DrawWorldNavigationLightTreatment(
    const FrameSlot *slot, ArRenderRectI viewport);
bool DrawWorldNavigationActiveRegionHaze(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection);

/* ---- defined in present_world_nav_models.c ---- */
SimBackgroundBridgeBounds WorldNavigationObjectBounds(
    const SimWorldNavigationTownObject *object);
bool DrawWorldNavigationMountains(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection);
bool DrawWorldNavigationTowns(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection);
void ResetWorldNavigationModels(void);

/* ---- defined in present_world_nav_weather.c ---- */
bool EnsureWorldNavigationCloudTexture(void);
size_t WorldNavigationShadowSamples(const FrameSlot *slot, uint64_t elapsed_ms,
    Sim3DDepthSphericalSample samples[kSimCloudLayerCount * 3]);
PresentationOutcome DrawWorldNavigationWeather(
    const FrameSlot *slot, ArRenderRectI viewport,
    const WorldNavigationProjection *projection, uint64_t elapsed_ms);
void ResetWorldNavigationWeather(void);

/* ---- defined in present_sim_globe.c ---- */
void ResetWorldNavigationGlobeSurfaces(void);

#endif  /* AR_PRESENT_WORLD_NAV_INTERNAL_H */
