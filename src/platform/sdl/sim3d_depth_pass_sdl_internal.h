#ifndef AR_SIM3D_DEPTH_PASS_SDL_INTERNAL_H
#define AR_SIM3D_DEPTH_PASS_SDL_INTERNAL_H
/* Sim3DDepthPass SDL internals: the GPU vertex and uniform layouts, mesh and
 * list types, the pass state and mesh table (defined in sim3d_depth_pass_sdl.c),
 * and the helpers one part calls in another. Not a public API.
 * Phase: present (render owner thread). */
#include "platform/sdl/gpu_texture_upload_layout.h"
#include "app/performance_metrics.h"
#include "sim/sim3d/sim3d_depth_pass.h"
#include "sim/sim3d/sim3d_performance.h"

#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdint.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/sdl/gpu_shader_blob.h"
#include "platform/sdl/render_sdl_internal.h"
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
#include "sim3d_depth_reference.h"
#endif

enum {
  kSim3DDepthRgbaBytesPerPixel = 4,
  kSim3DDepthVerticesPerQuad = 4,
  kSim3DDepthIndicesPerQuad = 6,
  kSim3DDepthInitialCpuVertexCapacity = 4096,
  kSim3DDepthInitialGpuVertexCapacity = 8192,
  kMaximumRetainedMeshes = 16,
  /* Two 16-chunk globe surfaces, up to seven live town-model chunks and
   * ordinary/radial caches can coexist across modes. These are handle limits,
   * not preallocated GPU storage. Each source still has its own small bound. */
  kMaximumGeometryMeshes = 64,
  kMaximumRetainedVertices = kSim3DDepthMaximumSourceQuads * 4,
  /* Optional opaque caching must not consume the existing weather budget.
   * Keep both domains bounded without making callers budget backend slots. */
  /* A split world surface repeats its nine shadow taps/two overlays per
   * chunk. Leave separate room for the atmosphere/cloud-body samples. */
  kMaximumEffectSamples = 256,
  kMaximumGeometrySamples = 64,
  kMaximumMeshSamples = kMaximumEffectSamples + kMaximumGeometrySamples,
  kMaximumSampleVertices = 2 * 1024 * 1024,
  kMaximumSurfaceRanges = 64,
  kSurfaceShadowBatchTaps = 3, /* Matched by the 352-byte shader uniform assertion. */
};

typedef struct Sim3DGpuVertex {
  float position[4];
  float color[4];
  float uv[2];
} Sim3DGpuVertex;

typedef struct Sim3DBillboardRimUniform {
  float offset[4];
  ArRenderColorF color;
} Sim3DBillboardRimUniform;
_Static_assert(sizeof(Sim3DBillboardRimUniform)==32 &&
    offsetof(Sim3DBillboardRimUniform,color)==16,
    "Directional rim uniform occupies two 16-byte registers");
typedef struct Sim3DBillboardRun {
  Uint32 first, count;
  Sim3DBillboardRimUniform rim;
} Sim3DBillboardRun;

typedef struct Sim3DSphericalGpuQuad {
  float positions[4][4], normals[4][4], weights[4][2];
} Sim3DSphericalGpuQuad;

typedef struct Sim3DSphericalUniform {
  float rotation[4], offset_extent[4], atlas[4], color[4];
} Sim3DSphericalUniform;

typedef struct Sim3DSurfaceGpuQuad {
  float points[4][4], shades[4][4], colors[4][4], uv[4][2], mask_uv[4][2];
} Sim3DSurfaceGpuQuad;

typedef enum Sim3DMeshKind {
  kMeshScreen, kMeshSpherical, kMeshGeometry, kMeshRadial, kMeshSurface,
  kMeshSphericalBody, kMeshLinear,
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
  kMeshModel, kMeshModelClipped,
#endif
} Sim3DMeshKind;
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
typedef struct Sim3DModelUniform { float matrix[16], viewport[4]; } Sim3DModelUniform;
#endif
typedef enum Sim3DModelPipeline {
  kModelPipeline_Radial,
  kModelPipeline_Linear,
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
  kModelPipeline_ReferenceAffine,
  kModelPipeline_ReferenceClipped,
#endif
  kModelPipelineCount,
} Sim3DModelPipeline;
typedef struct Sim3DLinearUniform {
  float matrix[16], offset[4], axes[kSim3DDepthLinearAxisCount][4], raster[4];
} Sim3DLinearUniform;
_Static_assert(sizeof(Sim3DLinearUniform) == 352 &&
    offsetof(Sim3DLinearUniform, raster) == 336, "std140 linear displacement view");
_Static_assert(sizeof(Sim3DDepthLinearVertex) == 40 &&
    offsetof(Sim3DDepthLinearVertex, color) == 16 &&
    offsetof(Sim3DDepthLinearVertex, axis) == 32, "packed linear displacement vertex");
typedef struct Sim3DRadialUniform { float matrix[16], basis[3][4], radial[4]; } Sim3DRadialUniform;
typedef struct Sim3DSurfaceUniform {
  Sim3DRadialUniform view;
  float light[4], material[4];
  Sim3DSphericalUniform spherical;
  float mask_rect[4], mask[4];
  float shadow_basis[3][4];
} Sim3DSurfaceUniform;
typedef struct Sim3DBodyView {
  float matrix[16], basis[3][4], centre_radius[4], texture_basis[3][4], rotation[4];
} Sim3DBodyView;
typedef struct Sim3DBodyUniform {
  Sim3DBodyView view;
  Sim3DSphericalUniform sample;
} Sim3DBodyUniform;
_Static_assert(sizeof(Sim3DBodyView) == 192 &&
    offsetof(Sim3DBodyView, rotation) == 176, "std140 spherical body view");
_Static_assert(sizeof(Sim3DDepthSphericalBodyVertex) == 16 &&
    offsetof(Sim3DDepthSphericalBodyVertex, opacity) == 12, "packed normal and opacity");
_Static_assert(sizeof(Sim3DSurfaceGpuQuad) == 256 && offsetof(Sim3DSurfaceGpuQuad, mask_uv) == 224,
    "sixteen packed float4 surface attributes");
_Static_assert(sizeof(Sim3DSurfaceUniform) == 304 &&
                   offsetof(Sim3DSurfaceUniform, spherical) == 160 &&
                   offsetof(Sim3DSurfaceUniform, shadow_basis) == 256,
               "std140 shared surface transform and material");
_Static_assert(sizeof(Sim3DRadialUniform) == 128, "std140 radial transform");
_Static_assert(sizeof(Sim3DDepthRadialVertex) == 40 &&
    offsetof(Sim3DDepthRadialVertex, elevation) == 12 &&
    offsetof(Sim3DDepthRadialVertex, color) == 20 &&
    offsetof(Sim3DDepthRadialVertex, variant) == 36, "packed radial vertex");

_Static_assert(sizeof(Sim3DSphericalGpuQuad) == 40 * sizeof(float), "Ten packed float4 attributes");
_Static_assert(sizeof(Sim3DSphericalUniform) == 64, "Four std140 float4 uniforms");

_Static_assert(sizeof(ArRenderPointF) == 2 * sizeof(float) &&
    offsetof(ArRenderPointF, y) == sizeof(float), "UV stream is packed float2");
_Static_assert(sizeof(ArRenderColorF) == 4 * sizeof(float) &&
    offsetof(ArRenderColorF, a) == 3 * sizeof(float), "Color stream is packed RGBA float4");

typedef struct Sim3DDepthList {
  Sim3DGpuVertex *vertices;
  Uint32 count;
  Uint32 capacity;
} Sim3DDepthList;

typedef struct Sim3DDepthAtlas {
  SDL_GPUTexture *texture;
  SDL_GPUTransferBuffer *transfer;
  Uint32 transfer_size;
  int width, height;
} Sim3DDepthAtlas;

struct Sim3DDepthMesh {
  struct Sim3DDepthMesh *next;
  SDL_GPUDevice *device;
  SDL_GPUBuffer *positions;
  SDL_GPUTransferBuffer *transfer;
  Uint32 count, capacity;
  Uint32 linear_update_first, linear_update_count;
  SDL_GPUBuffer *linear_spare;
  Uint32 linear_removed, linear_tail_count;
  bool linear_splice;
  int width, height;
  bool dirty, queued;
  Sim3DMeshKind kind;
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
  float bounds_min[3], bounds_max[3];
#endif
  float radial_extent[2];
  float linear_extent[5]; /* absolute position XYZ, displacement, depth offset */
  float surface_uv_extent;
  SDL_GPUBuffer *selection;
  SDL_GPUTransferBuffer *selection_transfer;
  Uint32 selection_count, selection_capacity;
  bool selection_dirty;
  Sim3DDepthMeshRange surface_ranges[kMaximumSurfaceRanges];
  Uint32 surface_range_count;
  bool surface_selected;
};

typedef struct Sim3DMeshSample {
  Sim3DDepthMesh *mesh;
  Sim3DDepthPassLayer layer;
  SDL_GPUTexture *texture; /* Optional, borrowed only through this submission. */
  Uint32 first, count, ordinary_before;
  ArRenderColorF color;
  /* Exactly one transform payload is used by a sample's mesh kind. */
  union {
    Sim3DSphericalUniform spherical;
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
    Sim3DModelUniform model;
#endif
    Sim3DLinearUniform linear;
    Sim3DRadialUniform radial;
    Sim3DSurfaceUniform surface;
    Sim3DBodyUniform body;
  };
} Sim3DMeshSample;

/* ---- state shared by the parts; defined in sim3d_depth_pass_sdl.c ---- */
extern Sim3DDepthMesh *g_depth_pass_meshes;
#if defined(AR_SIM3D_DEPTH_TEST_REFERENCE)
extern bool g_depth_pass_reject_linear_preparation;
#endif
typedef struct Sim3DDepthPassState {
  SDL_Renderer *renderer;
  SDL_GPUDevice *device;
  SDL_GPUShader *vertex_shader;
  SDL_GPUShader *fragment_shader;
  SDL_GPUGraphicsPipeline *pipeline;
  SDL_GPUGraphicsPipeline *depth_occluder_pipeline;
  SDL_GPUGraphicsPipeline *effect_pipeline;
  SDL_GPUShader *billboard_rim_fragment;
  SDL_GPUGraphicsPipeline *billboard_rim_pipeline;
  SDL_GPUGraphicsPipeline *mesh_pipeline;
  SDL_GPUShader *spherical_shader;
  SDL_GPUGraphicsPipeline *spherical_pipeline;
  SDL_GPUShader *model_shader[kModelPipelineCount], *model_clipped_fragment;
  SDL_GPUGraphicsPipeline *model_pipeline[kModelPipelineCount];
  bool model_pipeline_attempted[kModelPipelineCount];
  SDL_GPUShader *surface_vertex, *surface_fragment;
  SDL_GPUShader *shadow_batch_vertex, *shadow_batch_fragment;
  SDL_GPUGraphicsPipeline *surface_pipeline[3]; /* opaque, transparent, batched shadows */
  bool surface_pipeline_attempted;
  SDL_GPUShader *body_vertex, *body_fragment;
  SDL_GPUGraphicsPipeline *body_pipeline;
  bool body_pipeline_attempted;
  SDL_GPUSampler *nearest_sampler;
  SDL_GPUSampler *linear_sampler;
  SDL_GPUBuffer *vertex_buffer;
  SDL_GPUTransferBuffer *transfer_buffer;
  SDL_GPUBuffer *index_buffer;
  SDL_GPUTransferBuffer *index_transfer_buffer;
  Uint32 gpu_vertex_capacity;
  Uint32 gpu_index_capacity;
  bool index_upload_required;
  SDL_GPUTexture *color_target;
  SDL_GPUTexture *depth_target;
  Sim3DDepthAtlas atlases[kSim3DDepthPassLayerCount];
  SDL_GPUTexture *selected_ground;
  SDL_GPUTexture *billboard_texture;
  Sim3DBillboardRun billboard_runs[kSim3DDepthMaximumBillboardQuads];
  Uint32 billboard_run_count;
  SDL_GPUTexture *white_texture;
  SDL_Texture *output_texture;
  int width, height;
  float clip_x_scale, clip_y_scale;
  bool collecting;
  bool geometry_failed;
  bool failed;
  Sim3DDepthList lists[kSim3DDepthPassLayerCount];
  Sim3DMeshSample samples[kMaximumMeshSamples];
  Uint32 sample_count, geometry_sample_count, sample_vertices, sample_capacity;
  ArRenderPointF *sample_uv;
  SDL_GPUBuffer *sample_buffer;
  SDL_GPUTransferBuffer *sample_transfer;
  Uint32 sample_gpu_bytes, sample_upload_bytes;
} Sim3DDepthPassState;
extern Sim3DDepthPassState g_depth_pass;

/* ---- defined in sim3d_depth_pass_sdl.c ---- */
bool IsModelMesh(Sim3DMeshKind kind);
bool ReserveList(Sim3DDepthList *list, Uint32 additional);

/* ---- defined in sim3d_depth_pass_pipelines_sdl.c ---- */
bool CreateBodyPipeline(void);
bool CreateSurfacePipelines(void);
bool CreateModelPipeline(Sim3DModelPipeline variant);
SDL_GPUGraphicsPipeline *MeshPipeline(Sim3DMeshKind kind);
SDL_GPUTexture *GpuTexture(SDL_Texture *texture);
void ReleaseTargets(void);
bool CreateTargets(SDL_Renderer *renderer, int width, int height,
                          SDL_ScaleMode scale_mode);
bool TextureUploadsNeedAlignment(void);
bool EnsureInitialized(SDL_Renderer *renderer);

/* ---- defined in sim3d_depth_pass_meshes_sdl.c ---- */
Uint32 MeshVertexBytes(const Sim3DDepthMesh *mesh);
void ReleaseMeshStorage(Sim3DDepthMesh *mesh);
bool ValidPosition(Sim3DDepthPosition position);
bool ValidSampleColor(ArRenderColorF color);
bool GeometryLayerSupported(Sim3DDepthPassLayer layer);
bool OrderedLayerSupported(Sim3DDepthPassLayer layer);

#endif  /* AR_SIM3D_DEPTH_PASS_SDL_INTERNAL_H */
