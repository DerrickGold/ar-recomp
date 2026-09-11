#include "sim/sim3d_depth_pass.h"
#include "sim/sim3d_performance.h"

#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_shader_blob.h"
#include "platform/sdl/render_sdl_internal.h"
#include "shaders/sim3d_depth_frag.h"
#include "shaders/sim3d_depth_vert.h"
#include "shaders/sim3d_spherical_vert.h"

enum {
  kSim3DDepthRgbaBytesPerPixel = 4,
  kSim3DDepthVerticesPerQuad = 4,
  kSim3DDepthIndicesPerQuad = 6,
  kSim3DDepthInitialCpuVertexCapacity = 4096,
  kSim3DDepthInitialGpuVertexCapacity = 8192,
  kMaximumRetainedMeshes = 16,
  kMaximumRetainedVertices = 256 * 1024,
  kMaximumMeshSamples = 64,
  kMaximumSampleVertices = 2 * 1024 * 1024,
};

typedef struct Sim3DGpuVertex {
  float position[4];
  float color[4];
  float uv[2];
} Sim3DGpuVertex;

typedef struct Sim3DSphericalGpuQuad {
  float positions[4][4], normals[4][4], weights[4][2];
} Sim3DSphericalGpuQuad;

typedef struct Sim3DSphericalUniform {
  float rotation[4], offset_extent[4], atlas[4], color[4];
} Sim3DSphericalUniform;

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
  int width, height;
  bool dirty, queued;
  bool spherical;
};

typedef struct Sim3DMeshSample {
  Sim3DDepthMesh *mesh;
  Sim3DDepthPassLayer layer;
  Uint32 first;
  ArRenderColorF color;
  Sim3DSphericalUniform spherical;
} Sim3DMeshSample;

/* Handles outlive a renderer reset; only their platform payload is reset.
 * This registry is never published outside the owner thread. */
static Sim3DDepthMesh *s_meshes;

static struct {
  SDL_Renderer *renderer;
  SDL_GPUDevice *device;
  SDL_GPUShader *vertex_shader;
  SDL_GPUShader *fragment_shader;
  SDL_GPUGraphicsPipeline *pipeline;
  SDL_GPUGraphicsPipeline *depth_occluder_pipeline;
  SDL_GPUGraphicsPipeline *effect_pipeline;
  SDL_GPUGraphicsPipeline *mesh_pipeline;
  SDL_GPUShader *spherical_shader;
  SDL_GPUGraphicsPipeline *spherical_pipeline;
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
  SDL_GPUTexture *white_texture;
  SDL_Texture *output_texture;
  int width, height;
  float clip_x_scale, clip_y_scale;
  bool collecting;
  bool geometry_failed;
  bool failed;
  Sim3DDepthList lists[kSim3DDepthPassLayerCount];
  Sim3DMeshSample samples[kMaximumMeshSamples];
  Uint32 sample_count, sample_vertices, sample_capacity;
  ArRenderPointF *sample_uv;
  SDL_GPUBuffer *sample_buffer;
  SDL_GPUTransferBuffer *sample_transfer;
  Uint32 sample_gpu_bytes;
} g_depth_pass;

static const GpuShaderBlobs kVertexBlobs = {
  kSim3dDepthVertMSL, kSim3dDepthVertMSLSize,
  kSim3dDepthVertSPV, kSim3dDepthVertSPVSize,
  kSim3dDepthVertDXIL, kSim3dDepthVertDXILSize,
};
static const GpuShaderBlobs kFragmentBlobs = {
  kSim3dDepthFragMSL, kSim3dDepthFragMSLSize,
  kSim3dDepthFragSPV, kSim3dDepthFragSPVSize,
  kSim3dDepthFragDXIL, kSim3dDepthFragDXILSize,
};
static const GpuShaderBlobs kSphericalBlobs = {
  kSim3dSphericalVertMSL, kSim3dSphericalVertMSLSize,
  kSim3dSphericalVertSPV, kSim3dSphericalVertSPVSize,
  kSim3dSphericalVertDXIL, kSim3dSphericalVertDXILSize,
};

static SDL_GPUTexture *GpuTexture(SDL_Texture *texture) {
  if (!texture) return NULL;
  /* SDL_GPU resources are device-owned. Binding a texture created by a
   * different renderer can hand one backend a handle owned by another
   * device, which is an API contract violation rather than a recoverable
   * draw error. All current callers use this renderer, but keep the boundary
   * explicit so future material layers fail closed. */
  if (SDL_GetRendererFromTexture(texture) != g_depth_pass.renderer) {
    SDL_SetError("SIM3D depth texture belongs to another renderer");
    return NULL;
  }
  SDL_PropertiesID props = SDL_GetTextureProperties(texture);
  return props ? SDL_GetPointerProperty(
      props, SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, NULL) : NULL;
}

static void ReleaseTargets(void) {
  if (g_depth_pass.output_texture)
    SDL_DestroyTexture(g_depth_pass.output_texture);
  if (g_depth_pass.color_target)
    SDL_ReleaseGPUTexture(g_depth_pass.device, g_depth_pass.color_target);
  if (g_depth_pass.depth_target)
    SDL_ReleaseGPUTexture(g_depth_pass.device, g_depth_pass.depth_target);
  g_depth_pass.output_texture = NULL;
  g_depth_pass.color_target = NULL;
  g_depth_pass.depth_target = NULL;
  g_depth_pass.width = 0;
  g_depth_pass.height = 0;
}

static bool CreateTargets(SDL_Renderer *renderer, int width, int height,
                          SDL_ScaleMode scale_mode) {
  if (g_depth_pass.output_texture && g_depth_pass.width == width &&
      g_depth_pass.height == height) {
    return SDL_SetTextureScaleMode(g_depth_pass.output_texture, scale_mode);
  }
  ReleaseTargets();

  SDL_GPUTextureCreateInfo color_info;
  SDL_zero(color_info);
  color_info.type = SDL_GPU_TEXTURETYPE_2D;
  color_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  color_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
      SDL_GPU_TEXTUREUSAGE_SAMPLER;
  color_info.width = (Uint32)width;
  color_info.height = (Uint32)height;
  color_info.layer_count_or_depth = 1;
  color_info.num_levels = 1;
  color_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
  g_depth_pass.color_target = SDL_CreateGPUTexture(
      g_depth_pass.device, &color_info);

  SDL_GPUTextureCreateInfo depth_info = color_info;
  depth_info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  depth_info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
  /* D3D12 records an optimized clear value when the resource is created.
   * SDL still produces the correct result if this differs from the render
   * pass, but the mismatch triggers the D3D12 validation layer and can force
   * a slower clear. Other backends ignore this documented property. */
  SDL_PropertiesID depth_props = SDL_CreateProperties();
  if (depth_props) {
    SDL_SetFloatProperty(depth_props,
        SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_DEPTH_FLOAT, 1.0f);
    depth_info.props = depth_props;
  }
  g_depth_pass.depth_target = SDL_CreateGPUTexture(
      g_depth_pass.device, &depth_info);
  if (depth_props) SDL_DestroyProperties(depth_props);
  if (!g_depth_pass.color_target || !g_depth_pass.depth_target) {
    fprintf(stderr, "[sim3d-depth] render target creation failed: %s\n",
            SDL_GetError());
    ReleaseTargets();
    return false;
  }

  SDL_PropertiesID props = SDL_CreateProperties();
  if (props) {
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
        SDL_GetPixelFormatFromGPUTextureFormat(color_info.format));
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
        SDL_TEXTUREACCESS_TARGET);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
    SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER,
                           g_depth_pass.color_target);
    g_depth_pass.output_texture =
        SDL_CreateTextureWithProperties(renderer, props);
    SDL_DestroyProperties(props);
  }
  if (!g_depth_pass.output_texture ||
      !SDL_SetTextureBlendMode(g_depth_pass.output_texture,
                               SDL_BLENDMODE_BLEND) ||
      !SDL_SetTextureScaleMode(g_depth_pass.output_texture, scale_mode)) {
    fprintf(stderr, "[sim3d-depth] SDL target wrapper failed: %s\n",
            SDL_GetError());
    ReleaseTargets();
    return false;
  }
  g_depth_pass.width = width;
  g_depth_pass.height = height;
  return true;
}

static bool CreatePipeline(void) {
  g_depth_pass.vertex_shader = GpuShaderBlob_Create(
      g_depth_pass.device, &kVertexBlobs, "SIM3D depth vertex",
      SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
  g_depth_pass.fragment_shader = GpuShaderBlob_CreateFragment(
      g_depth_pass.device, &kFragmentBlobs, "SIM3D depth fragment", 1, 0);
  if (!g_depth_pass.vertex_shader || !g_depth_pass.fragment_shader)
    return false;

  const SDL_GPUVertexBufferDescription buffer = {
    .slot = 0,
    .pitch = sizeof(Sim3DGpuVertex),
    .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
  };
  const SDL_GPUVertexAttribute attributes[] = {
    {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
     offsetof(Sim3DGpuVertex, position)},
    {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
     offsetof(Sim3DGpuVertex, color)},
    {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
     offsetof(Sim3DGpuVertex, uv)},
  };
  SDL_GPUColorTargetDescription color_target;
  SDL_zero(color_target);
  color_target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  color_target.blend_state.src_color_blendfactor =
      SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  color_target.blend_state.dst_color_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  color_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
  color_target.blend_state.src_alpha_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE;
  color_target.blend_state.dst_alpha_blendfactor =
      SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  color_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
  color_target.blend_state.enable_blend = true;

  SDL_GPUGraphicsPipelineCreateInfo info;
  SDL_zero(info);
  info.vertex_shader = g_depth_pass.vertex_shader;
  info.fragment_shader = g_depth_pass.fragment_shader;
  info.vertex_input_state.vertex_buffer_descriptions = &buffer;
  info.vertex_input_state.num_vertex_buffers = 1;
  info.vertex_input_state.vertex_attributes = attributes;
  info.vertex_input_state.num_vertex_attributes =
      (Uint32)(sizeof(attributes) / sizeof(attributes[0]));
  info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
  info.rasterizer_state.enable_depth_clip = true;
  info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
  info.depth_stencil_state.enable_depth_test = true;
  info.depth_stencil_state.enable_depth_write = true;
  info.target_info.color_target_descriptions = &color_target;
  info.target_info.num_color_targets = 1;
  info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  info.target_info.has_depth_stencil_target = true;
  g_depth_pass.pipeline = SDL_CreateGPUGraphicsPipeline(
      g_depth_pass.device, &info);
  if (!g_depth_pass.pipeline) {
    fprintf(stderr, "[sim3d-depth] pipeline creation failed: %s\n",
            SDL_GetError());
    return false;
  }
  /* The town ground is already rendered with its native texture through
   * SDL_RenderGeometry.  This pipeline contributes the identical surface to
   * D32 without touching the transparent composite's color target. */
  color_target.blend_state.color_write_mask = 0;
  color_target.blend_state.enable_color_write_mask = true;
  g_depth_pass.depth_occluder_pipeline = SDL_CreateGPUGraphicsPipeline(
      g_depth_pass.device, &info);
  if (!g_depth_pass.depth_occluder_pipeline) {
    fprintf(stderr, "[sim3d-depth] depth-only pipeline creation failed: %s\n",
            SDL_GetError());
    return false;
  }
  color_target.blend_state.enable_color_write_mask = false;
  info.depth_stencil_state.enable_depth_write = false;
  g_depth_pass.effect_pipeline = SDL_CreateGPUGraphicsPipeline(
      g_depth_pass.device, &info);
  if (!g_depth_pass.effect_pipeline) {
    fprintf(stderr, "[sim3d-depth] effect pipeline creation failed: %s\n",
            SDL_GetError());
    return false;
  }

  /* Same shaders and blend/depth rules, split inputs: retained positions,
   * streamed UVs, and one constant color per sample (instance-rate input). */
  const SDL_GPUVertexBufferDescription mesh_buffers[] = {
    {0, 4 * sizeof(float), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
    {1, sizeof(ArRenderPointF), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
    {2, sizeof(ArRenderColorF), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0},
  };
  const SDL_GPUVertexAttribute mesh_attributes[] = {
    {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
    {1, 2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
    {2, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0},
  };
  info.vertex_input_state.vertex_buffer_descriptions = mesh_buffers;
  info.vertex_input_state.num_vertex_buffers = 3;
  info.vertex_input_state.vertex_attributes = mesh_attributes;
  g_depth_pass.mesh_pipeline = SDL_CreateGPUGraphicsPipeline(g_depth_pass.device, &info);
  /* Optional optimization: unavailable split-input pipelines keep the
   * ordinary append path, without making the whole renderer unavailable. */

  g_depth_pass.spherical_shader = GpuShaderBlob_Create(g_depth_pass.device,
      &kSphericalBlobs, "SIM3D spherical mapping", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
  if (g_depth_pass.spherical_shader) {
    const SDL_GPUVertexBufferDescription spherical_buffer = {
      0, sizeof(Sim3DSphericalGpuQuad), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0,
    };
    SDL_GPUVertexAttribute spherical_attributes[10];
    for (Uint32 i = 0; i < 10; ++i)
      spherical_attributes[i] = (SDL_GPUVertexAttribute){
        i, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, i * 4 * (Uint32)sizeof(float),
      };
    info.vertex_shader = g_depth_pass.spherical_shader;
    info.vertex_input_state.vertex_buffer_descriptions = &spherical_buffer;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = spherical_attributes;
    info.vertex_input_state.num_vertex_attributes = 10;
    g_depth_pass.spherical_pipeline = SDL_CreateGPUGraphicsPipeline(g_depth_pass.device, &info);
  }

  SDL_GPUSamplerCreateInfo sampler;
  SDL_zero(sampler);
  sampler.min_filter = SDL_GPU_FILTER_NEAREST;
  sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
  sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  g_depth_pass.nearest_sampler = SDL_CreateGPUSampler(
      g_depth_pass.device, &sampler);
  if (!g_depth_pass.nearest_sampler) {
    fprintf(stderr, "[sim3d-depth] nearest sampler creation failed: %s\n",
            SDL_GetError());
    return false;
  }
  sampler.min_filter = SDL_GPU_FILTER_LINEAR;
  sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
  g_depth_pass.linear_sampler = SDL_CreateGPUSampler(
      g_depth_pass.device, &sampler);
  if (!g_depth_pass.linear_sampler) {
    fprintf(stderr, "[sim3d-depth] linear sampler creation failed: %s\n",
            SDL_GetError());
    return false;
  }
  return true;
}

static bool CreateWhiteTexture(SDL_Renderer *renderer) {
  SDL_GPUTextureCreateInfo texture_info;
  SDL_zero(texture_info);
  texture_info.type = SDL_GPU_TEXTURETYPE_2D;
  texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  texture_info.width = 1;
  texture_info.height = 1;
  texture_info.layer_count_or_depth = 1;
  texture_info.num_levels = 1;
  texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
  SDL_GPUTexture *texture = SDL_CreateGPUTexture(
      g_depth_pass.device, &texture_info);
  SDL_GPUTransferBufferCreateInfo transfer_info = {
    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
    .size = kSim3DDepthRgbaBytesPerPixel,
  };
  SDL_GPUTransferBuffer *transfer = texture ? SDL_CreateGPUTransferBuffer(
      g_depth_pass.device, &transfer_info) : NULL;
  uint8_t *mapped = transfer ? SDL_MapGPUTransferBuffer(
      g_depth_pass.device, transfer, false) : NULL;
  if (!mapped) {
    if (transfer)
      SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
    if (texture) SDL_ReleaseGPUTexture(g_depth_pass.device, texture);
    return false;
  }
  memset(mapped, 0xFF, kSim3DDepthRgbaBytesPerPixel);
  SDL_UnmapGPUTransferBuffer(g_depth_pass.device, transfer);
  if (!SDL_FlushRenderer(renderer)) {
    SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
    SDL_ReleaseGPUTexture(g_depth_pass.device, texture);
    return false;
  }
  SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(
      g_depth_pass.device);
  SDL_GPUCopyPass *copy = commands ? SDL_BeginGPUCopyPass(commands) : NULL;
  if (!copy) {
    if (commands) SDL_CancelGPUCommandBuffer(commands);
    SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
    SDL_ReleaseGPUTexture(g_depth_pass.device, texture);
    return false;
  }
  SDL_GPUTextureTransferInfo source = {
    .transfer_buffer = transfer,
    .pixels_per_row = 1,
    .rows_per_layer = 1,
  };
  SDL_GPUTextureRegion destination = {
    .texture = texture,
    .w = 1,
    .h = 1,
    .d = 1,
  };
  SDL_UploadToGPUTexture(copy, &source, &destination, false);
  SDL_EndGPUCopyPass(copy);
  if (!SDL_SubmitGPUCommandBuffer(commands)) {
    SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
    SDL_ReleaseGPUTexture(g_depth_pass.device, texture);
    return false;
  }
  SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
  g_depth_pass.white_texture = texture;
  return true;
}

static bool EnsureInitialized(SDL_Renderer *renderer) {
  if (g_depth_pass.failed) return false;
  if (g_depth_pass.renderer == renderer && g_depth_pass.pipeline) return true;
  if (g_depth_pass.renderer && g_depth_pass.renderer != renderer)
    Sim3DDepthPass_Reset(NULL);

  SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
  g_depth_pass.device = props ? SDL_GetPointerProperty(
      props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL) : NULL;
  if (!g_depth_pass.device) {
    fprintf(stderr, "[sim3d-depth] renderer has no SDL_GPU device\n");
    g_depth_pass.failed = true;
    return false;
  }
  if (!SDL_GPUTextureSupportsFormat(
          g_depth_pass.device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
          SDL_GPU_TEXTURETYPE_2D,
          SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
    fprintf(stderr, "[sim3d-depth] D32 depth targets are unsupported\n");
    g_depth_pass.failed = true;
    return false;
  }
  if (!SDL_GPUTextureSupportsFormat(
          g_depth_pass.device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
          SDL_GPU_TEXTURETYPE_2D,
          SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
              SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
    fprintf(stderr, "[sim3d-depth] RGBA8 sampled color targets are "
                    "unsupported\n");
    g_depth_pass.failed = true;
    return false;
  }
  g_depth_pass.renderer = renderer;
  if (!CreatePipeline() || !CreateWhiteTexture(renderer)) {
    fprintf(stderr, "[sim3d-depth] required GPU resources failed: %s\n",
            SDL_GetError());
    g_depth_pass.failed = true;
    return false;
  }
  return true;
}

bool Sim3DDepthPass_Require(ArRenderDevice *device) {
  const ArRenderCapabilities *capabilities =
      ArRenderDevice_Capabilities(device);
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  return renderer && ArRenderCapabilities_Has(
      capabilities, kArRenderCapability_Depth |
                        kArRenderCapability_CustomShaders) &&
      EnsureInitialized(renderer);
}

const char *Sim3DDepthPass_LastError(void) {
  const char *error = SDL_GetError();
  return error && error[0] ? error : "required SDL_GPU depth pass unavailable";
}

bool Sim3DDepthPass_UploadAtlasRegions(
    ArRenderDevice *device, Sim3DDepthPassLayer layer,
    const uint32_t *argb_pixels,
    int width, int height, int pitch,
    const ArRenderRectI *regions, int region_count) {
  if (layer != kSim3DDepthPass_Mountain && layer != kSim3DDepthPass_Ground &&
      layer != kSim3DDepthPass_GroundBlur && layer != kSim3DDepthPass_Cloud &&
      layer != kSim3DDepthPass_WorldMountain && layer != kSim3DDepthPass_VolumeCloud)
    return false;
  Sim3DDepthAtlas *atlas = &g_depth_pass.atlases[layer];
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (!renderer || !argb_pixels || width <= 0 || height <= 0 || pitch <= 0 ||
      !regions || region_count <= 0)
    return false;
  if ((size_t)width > SIZE_MAX / sizeof(uint32_t)) return false;
  const size_t row_bytes = (size_t)width * sizeof(uint32_t);
  if ((size_t)pitch < row_bytes ||
      row_bytes > UINT32_MAX ||
      (size_t)height > UINT32_MAX / row_bytes ||
      (size_t)height > SIZE_MAX / (size_t)pitch ||
      !EnsureInitialized(renderer))
    return false;
  const Uint32 upload_size = (Uint32)(row_bytes * (size_t)height);
  for (int region = 0; region < region_count; region++) {
    const ArRenderRectI *dirty = &regions[region];
    if (dirty->x < 0 || dirty->y < 0 || dirty->w <= 0 || dirty->h <= 0 ||
        dirty->x > width - dirty->w || dirty->y > height - dirty->h)
      return false;
  }

  const bool resources_match = atlas->texture &&
      atlas->transfer &&
      atlas->width == width &&
      atlas->height == height &&
      atlas->transfer_size >= upload_size;
  if (!resources_match) {
    SDL_GPUTextureCreateInfo texture_info;
    SDL_zero(texture_info);
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture_info.width = (Uint32)width;
    texture_info.height = (Uint32)height;
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels = 1;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(
        g_depth_pass.device, &texture_info);
    SDL_GPUTransferBufferCreateInfo transfer_info = {
      .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
      .size = upload_size,
    };
    SDL_GPUTransferBuffer *transfer = texture ? SDL_CreateGPUTransferBuffer(
        g_depth_pass.device, &transfer_info) : NULL;
    if (!texture || !transfer) {
      fprintf(stderr, "[sim3d-depth] material atlas upload resource creation "
                      "failed: %s\n", SDL_GetError());
      if (transfer)
        SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
      if (texture)
        SDL_ReleaseGPUTexture(g_depth_pass.device, texture);
      return false;
    }
    if (atlas->transfer)
      SDL_ReleaseGPUTransferBuffer(
          g_depth_pass.device, atlas->transfer);
    if (atlas->texture)
      SDL_ReleaseGPUTexture(g_depth_pass.device, atlas->texture);
    atlas->texture = texture;
    atlas->transfer = transfer;
    atlas->transfer_size = upload_size;
    atlas->width = width;
    atlas->height = height;
  }

  uint8_t *mapped = SDL_MapGPUTransferBuffer(
      g_depth_pass.device, atlas->transfer, true);
  if (!mapped) {
    fprintf(stderr, "[sim3d-depth] material atlas upload allocation failed: %s\n",
            SDL_GetError());
    return false;
  }
  /* Pack each rectangle contiguously. SDL GPU backends are free to cycle the
   * transfer storage on map, so callers cannot depend on untouched full-atlas
   * rows remaining addressable through a strided subwindow. Packing also
   * keeps backend row-layout constraints behind this depth-pass seam. */
  size_t packed_at = 0;
  for (int region = 0; region < region_count; region++) {
    const ArRenderRectI *dirty = &regions[region];
    const size_t region_row_bytes =
        (size_t)dirty->w * kSim3DDepthRgbaBytesPerPixel;
    const size_t region_bytes = region_row_bytes * (size_t)dirty->h;
    if (packed_at > (size_t)upload_size ||
        region_bytes > (size_t)upload_size - packed_at) {
      SDL_UnmapGPUTransferBuffer(
          g_depth_pass.device, atlas->transfer);
      return false;
    }
    const uint8_t *source = (const uint8_t *)argb_pixels +
        (size_t)dirty->y * (size_t)pitch +
        (size_t)dirty->x * sizeof(uint32_t);
    /* ARGB8888 describes native-endian integer values; RGBA32 describes the
     * GPU's byte order on every host. SDL owns optimized conversion, while
     * the portable caller retains its existing ARGB/pitch/region contract.
     * Validation above bounds row_bytes by the positive int source pitch. */
    if (!SDL_ConvertPixels(dirty->w, dirty->h, SDL_PIXELFORMAT_ARGB8888,
            source, pitch, SDL_PIXELFORMAT_RGBA32,
            mapped + packed_at, (int)region_row_bytes)) {
      SDL_UnmapGPUTransferBuffer(g_depth_pass.device, atlas->transfer);
      return false;
    }
    packed_at += region_bytes;
  }
  SDL_UnmapGPUTransferBuffer(
      g_depth_pass.device, atlas->transfer);

  if (!SDL_FlushRenderer(renderer)) return false;
  SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(
      g_depth_pass.device);
  SDL_GPUCopyPass *copy = commands ? SDL_BeginGPUCopyPass(commands) : NULL;
  if (!copy) {
    if (commands) SDL_CancelGPUCommandBuffer(commands);
    return false;
  }
  packed_at = 0;
  for (int region = 0; region < region_count; region++) {
    const ArRenderRectI *dirty = &regions[region];
    SDL_GPUTextureTransferInfo source_info = {
      .transfer_buffer = atlas->transfer,
      .offset = (Uint32)packed_at,
      .pixels_per_row = (Uint32)dirty->w,
      .rows_per_layer = (Uint32)dirty->h,
    };
    SDL_GPUTextureRegion destination = {
      .texture = atlas->texture,
      .x = (Uint32)dirty->x,
      .y = (Uint32)dirty->y,
      .w = (Uint32)dirty->w,
      .h = (Uint32)dirty->h,
      .d = 1,
    };
    /* Mapping with `cycle=true` selected writable storage for this frame.
     * Every region in this transaction must reference that same generation. */
    SDL_UploadToGPUTexture(copy, &source_info, &destination, false);
    packed_at += (size_t)dirty->w * (size_t)dirty->h *
        kSim3DDepthRgbaBytesPerPixel;
  }
  SDL_EndGPUCopyPass(copy);
  if (!SDL_SubmitGPUCommandBuffer(commands)) {
    fprintf(stderr, "[sim3d-depth] material atlas upload submission failed: %s\n",
            SDL_GetError());
    return false;
  }
  return true;
}

bool Sim3DDepthPass_UploadMountainAtlasRegions(
    ArRenderDevice *device, const uint32_t *argb_pixels,
    int width, int height, int pitch,
    const ArRenderRectI *regions, int region_count) {
  return Sim3DDepthPass_UploadAtlasRegions(
      device, kSim3DDepthPass_Mountain, argb_pixels,
      width, height, pitch, regions, region_count);
}

static bool ReserveList(Sim3DDepthList *list, Uint32 additional) {
  if (additional > UINT32_MAX - list->count) return false;
  Uint32 required = list->count + additional;
  if (required <= list->capacity) return true;
  Uint32 capacity = list->capacity ? list->capacity
      : kSim3DDepthInitialCpuVertexCapacity;
  while (capacity < required) {
    if (capacity > UINT32_MAX / 2) {
      capacity = required;
      break;
    }
    capacity *= 2;
  }
  if ((size_t)capacity > SIZE_MAX / sizeof(*list->vertices)) return false;
  Sim3DGpuVertex *vertices = realloc(
      list->vertices, (size_t)capacity * sizeof(*vertices));
  if (!vertices) return false;
  list->vertices = vertices;
  list->capacity = capacity;
  return true;
}

bool Sim3DDepthPass_Begin(ArRenderDevice *device, int width, int height,
                          ArRenderFilter output_filter) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  const SDL_ScaleMode output_scale_mode =
      output_filter == kArRenderFilter_Linear
          ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST;
  if (!renderer || width <= 0 || height <= 0 ||
      !EnsureInitialized(renderer) ||
      !CreateTargets(renderer, width, height, output_scale_mode))
    return false;
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++)
    g_depth_pass.lists[i].count = 0;
  g_depth_pass.sample_count = g_depth_pass.sample_vertices = 0;
  for (Sim3DDepthMesh *mesh = s_meshes; mesh; mesh = mesh->next) mesh->queued = false;
  g_depth_pass.geometry_failed = false;
  g_depth_pass.collecting = true;
  g_depth_pass.clip_x_scale = 2.0f / (float)width;
  g_depth_pass.clip_y_scale = 2.0f / (float)height;
  return true;
}

static Sim3DDepthMesh *CreateMesh(bool spherical) {
  if (!g_depth_pass.collecting ||
      !(spherical ? g_depth_pass.spherical_pipeline : g_depth_pass.mesh_pipeline)) return NULL;
  unsigned count = 0;
  for (Sim3DDepthMesh *mesh = s_meshes; mesh; mesh = mesh->next) count++;
  if (count >= kMaximumRetainedMeshes) return NULL;
  Sim3DDepthMesh *mesh = calloc(1, sizeof(*mesh));
  if (!mesh) return NULL;
  mesh->spherical = spherical;
  mesh->next = s_meshes;
  s_meshes = mesh;
  return mesh;
}

Sim3DDepthMesh *Sim3DDepthPass_CreateMesh(void) { return CreateMesh(false); }
Sim3DDepthMesh *Sim3DDepthPass_CreateSphericalMesh(void) { return CreateMesh(true); }

static Uint32 MeshVertexBytes(const Sim3DDepthMesh *mesh) {
  return mesh->spherical ? (Uint32)sizeof(Sim3DSphericalGpuQuad) / 4 : 4 * (Uint32)sizeof(float);
}

static void ReleaseMeshStorage(Sim3DDepthMesh *mesh) {
  if (mesh->positions) SDL_ReleaseGPUBuffer(mesh->device, mesh->positions);
  if (mesh->transfer) SDL_ReleaseGPUTransferBuffer(mesh->device, mesh->transfer);
  mesh->positions = NULL; mesh->transfer = NULL; mesh->device = NULL;
  mesh->count = mesh->capacity = 0;
  mesh->width = mesh->height = 0;
  mesh->dirty = mesh->queued = false;
}

void Sim3DDepthPass_DestroyMesh(Sim3DDepthMesh *mesh) {
  if (!mesh) return;
  if (g_depth_pass.collecting && mesh->queued) g_depth_pass.geometry_failed = true;
  Sim3DDepthMesh **link = &s_meshes;
  while (*link && *link != mesh) link = &(*link)->next;
  if (*link != mesh) return;
  *link = mesh->next;
  ReleaseMeshStorage(mesh);
  free(mesh);
}

bool Sim3DDepthPass_MeshReady(const Sim3DDepthMesh *mesh) {
  return g_depth_pass.collecting && mesh &&
      (mesh->spherical ? g_depth_pass.spherical_pipeline : g_depth_pass.mesh_pipeline) &&
      mesh->device == g_depth_pass.device && mesh->positions && mesh->count &&
      mesh->width == g_depth_pass.width && mesh->height == g_depth_pass.height;
}

static bool ReserveMesh(Sim3DDepthMesh *mesh, Uint32 count) {
  if (count > mesh->capacity) {
    Uint32 capacity = mesh->capacity ? mesh->capacity : 4096;
    while (capacity < count) capacity *= 2;
    const SDL_GPUBufferCreateInfo info = {
      .usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = capacity * MeshVertexBytes(mesh),
    };
    const SDL_GPUTransferBufferCreateInfo transfer_info = {
      .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = info.size,
    };
    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(g_depth_pass.device, &info);
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(g_depth_pass.device, &transfer_info);
    if (!buffer || !transfer) {
      if (buffer) SDL_ReleaseGPUBuffer(g_depth_pass.device, buffer);
      if (transfer) SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
      return false;
    }
    ReleaseMeshStorage(mesh);
    mesh->device = g_depth_pass.device;
    mesh->positions = buffer; mesh->transfer = transfer; mesh->capacity = capacity;
  }
  return true;
}

static bool CanUpdateMesh(const Sim3DDepthMesh *mesh, size_t quad_count, bool spherical) {
  return g_depth_pass.collecting && mesh && mesh->spherical == spherical &&
      (spherical ? g_depth_pass.spherical_pipeline : g_depth_pass.mesh_pipeline) &&
      !mesh->queued && (!mesh->device || mesh->device == g_depth_pass.device) &&
      quad_count && quad_count <= kMaximumRetainedVertices / 4;
}

static bool ValidPosition(Sim3DDepthPosition position) {
  return isfinite(position.x) && isfinite(position.y) && isfinite(position.depth);
}

static void MapPosition(Sim3DDepthPosition position, float out[4]) {
  out[0] = position.x * g_depth_pass.clip_x_scale - 1.0f;
  out[1] = 1.0f - position.y * g_depth_pass.clip_y_scale;
  out[2] = position.depth; out[3] = 1.0f;
}

static void PublishMesh(Sim3DDepthMesh *mesh, Uint32 count) {
  SDL_UnmapGPUTransferBuffer(g_depth_pass.device, mesh->transfer);
  mesh->count = count; mesh->width = g_depth_pass.width; mesh->height = g_depth_pass.height;
  mesh->dirty = true;
}

bool Sim3DDepthPass_UpdateMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthPosition *positions, size_t quad_count) {
  if (!CanUpdateMesh(mesh, quad_count, false) || !positions) return false;
  const Uint32 count = (Uint32)quad_count * 4;
  for (Uint32 i = 0; i < count; ++i) if (!ValidPosition(positions[i])) return false;
  if (!ReserveMesh(mesh, count)) return false;
  float (*mapped)[4] = SDL_MapGPUTransferBuffer(g_depth_pass.device, mesh->transfer, true);
  if (!mapped) return false;
  for (Uint32 i = 0; i < count; ++i) MapPosition(positions[i], mapped[i]);
  PublishMesh(mesh, count);
  return true;
}

bool Sim3DDepthPass_UpdateSphericalMesh(Sim3DDepthMesh *mesh,
    const Sim3DDepthSphericalQuad *quads, size_t quad_count) {
  if (!CanUpdateMesh(mesh, quad_count, true) || !quads) return false;
  for (size_t i = 0; i < quad_count; ++i) {
    if (quads[i].triangle > 2) return false;
    for (int p = 0; p < 4; ++p) {
      if (!ValidPosition(quads[i].positions[p])) return false;
      for (int j = 0; j < 3; ++j) if (!isfinite(quads[i].normals[p][j])) return false;
      for (int j = 0; j < 2; ++j) if (!isfinite(quads[i].weights[p][j])) return false;
    }
  }
  const Uint32 count = (Uint32)quad_count * 4;
  if (!ReserveMesh(mesh, count)) return false;
  Sim3DSphericalGpuQuad *mapped = SDL_MapGPUTransferBuffer(g_depth_pass.device, mesh->transfer, true);
  if (!mapped) return false;
  for (size_t i = 0; i < quad_count; ++i)
    for (int p = 0; p < 4; ++p) {
      MapPosition(quads[i].positions[p], mapped[i].positions[p]);
      memcpy(mapped[i].normals[p], quads[i].normals[p], 3 * sizeof(float));
      mapped[i].normals[p][3] = p ? 0 : (float)quads[i].triangle;
      memcpy(mapped[i].weights[p], quads[i].weights[p], 2 * sizeof(float));
    }
  PublishMesh(mesh, count);
  return true;
}

static bool MeshLayerSupported(Sim3DDepthPassLayer layer) {
  return layer == kSim3DDepthPass_CloudShadow || layer == kSim3DDepthPass_Cloud ||
      layer == kSim3DDepthPass_GroundBlur || layer == kSim3DDepthPass_GroundHaze ||
      layer == kSim3DDepthPass_Effect || layer == kSim3DDepthPass_VolumeCloud;
}

static bool ValidSampleColor(ArRenderColorF color) {
  return isfinite(color.r) && color.r >= 0 && color.r <= 1 &&
      isfinite(color.g) && color.g >= 0 && color.g <= 1 &&
      isfinite(color.b) && color.b >= 0 && color.b <= 1 &&
      isfinite(color.a) && color.a >= 0 && color.a <= 1;
}

bool Sim3DDepthPass_AppendMeshSample(Sim3DDepthPassLayer layer,
    Sim3DDepthMesh *mesh, const ArRenderPointF *uv, size_t quad_count,
    ArRenderColorF color) {
  if (!Sim3DDepthPass_MeshReady(mesh) || mesh->spherical || !MeshLayerSupported(layer) || !uv ||
      !ValidSampleColor(color) || quad_count != mesh->count / 4 || g_depth_pass.lists[layer].count ||
      g_depth_pass.sample_count == kMaximumMeshSamples ||
      mesh->count > kMaximumSampleVertices - g_depth_pass.sample_vertices)
    return false;
  const Uint32 count = g_depth_pass.sample_vertices + mesh->count;
  if (count > g_depth_pass.sample_capacity) {
    Uint32 capacity = g_depth_pass.sample_capacity ? g_depth_pass.sample_capacity : 4096;
    while (capacity < count) capacity *= 2;
    void *storage = realloc(g_depth_pass.sample_uv, (size_t)capacity * sizeof(*uv));
    if (!storage) return false;
    g_depth_pass.sample_uv = storage; g_depth_pass.sample_capacity = capacity;
  }
  memcpy(g_depth_pass.sample_uv + g_depth_pass.sample_vertices, uv, mesh->count * sizeof(*uv));
  g_depth_pass.samples[g_depth_pass.sample_count++] = (Sim3DMeshSample){
    .mesh = mesh, .layer = layer, .first = g_depth_pass.sample_vertices, .color = color,
  };
  g_depth_pass.sample_vertices = count;
  mesh->queued = true;
  return true;
}

bool Sim3DDepthPass_AppendSphericalSample(Sim3DDepthPassLayer layer,
    Sim3DDepthMesh *mesh, const Sim3DDepthSphericalSample *sample) {
  if (!Sim3DDepthPass_MeshReady(mesh) || !mesh->spherical || !MeshLayerSupported(layer) ||
      !sample || !ValidSampleColor(sample->color) || g_depth_pass.lists[layer].count ||
      g_depth_pass.sample_count == kMaximumMeshSamples)
    return false;
  for (int i = 0; i < 4; ++i) if (!isfinite(sample->rotation[i])) return false;
  if (!isfinite(sample->offset.x) || !isfinite(sample->offset.y) ||
      !isfinite(sample->texture_size.x) || !isfinite(sample->texture_size.y) ||
      sample->texture_size.x <= 0 || sample->texture_size.y <= 0 ||
      sample->atlas.x < 0 || sample->atlas.y < 0 || sample->atlas.w < 2 || sample->atlas.h < 2 ||
      (double)sample->atlas.x + 2.0 * sample->atlas.w > sample->texture_size.x ||
      (double)sample->atlas.y + sample->atlas.h > sample->texture_size.y) return false;
  Sim3DMeshSample *command = &g_depth_pass.samples[g_depth_pass.sample_count++];
  *command = (Sim3DMeshSample){.mesh = mesh, .layer = layer, .color = sample->color};
  Sim3DSphericalUniform *uniform = &command->spherical;
  memcpy(uniform->rotation, sample->rotation, sizeof(uniform->rotation));
  uniform->offset_extent[0] = sample->offset.x; uniform->offset_extent[1] = sample->offset.y;
  uniform->offset_extent[2] = sample->texture_size.x; uniform->offset_extent[3] = sample->texture_size.y;
  uniform->atlas[0] = (float)sample->atlas.x; uniform->atlas[1] = (float)sample->atlas.y;
  uniform->atlas[2] = (float)sample->atlas.w; uniform->atlas[3] = (float)sample->atlas.h;
  memcpy(uniform->color, &sample->color, sizeof(uniform->color));
  mesh->queued = true;
  return true;
}

bool Sim3DDepthPass_AppendQuads(Sim3DDepthPassLayer layer,
                               const Sim3DDepthVertex *vertices,
                               size_t quad_count) {
  if (!g_depth_pass.collecting || !vertices || layer < 0 ||
      layer >= kSim3DDepthPassLayerCount)
    return false;
  if (!quad_count) return true;
  for (Uint32 i = 0; i < g_depth_pass.sample_count; ++i)
    if (g_depth_pass.samples[i].layer == layer) return false;
  if (quad_count > UINT32_MAX / kSim3DDepthVerticesPerQuad) return false;
  const Uint32 vertex_count =
      (Uint32)quad_count * kSim3DDepthVerticesPerQuad;
  Sim3DDepthList *list = &g_depth_pass.lists[layer];
  if (!ReserveList(list, vertex_count)) {
    g_depth_pass.geometry_failed = true;
    return false;
  }
  for (Uint32 i = 0; i < vertex_count; i++) {
    /* Read a complete value before writing backend storage. Besides keeping
     * the conversion independent of caller storage, this lets the compiler
     * group unchanged color/UV transfers without alias checks per field. */
    const Sim3DDepthVertex source = vertices[i];
    Sim3DGpuVertex *destination = &list->vertices[list->count++];
    destination->position[0] =
        source.x * g_depth_pass.clip_x_scale - 1.0f;
    destination->position[1] =
        1.0f - source.y * g_depth_pass.clip_y_scale;
    destination->position[2] = source.depth;
    destination->position[3] = 1.0f;
    destination->color[0] = source.color.r;
    destination->color[1] = source.color.g;
    destination->color[2] = source.color.b;
    destination->color[3] = source.color.a;
    destination->uv[0] = source.uv.x;
    destination->uv[1] = source.uv.y;
  }
  return true;
}

bool Sim3DDepthPass_AppendQuad(Sim3DDepthPassLayer layer,
                               const Sim3DDepthVertex vertices[4]) {
  return Sim3DDepthPass_AppendQuads(layer, vertices, 1);
}

static bool EnsureGpuBuffers(Uint32 vertex_count) {
  if (vertex_count <= g_depth_pass.gpu_vertex_capacity) return true;
  const Uint32 maximum_vertices =
      UINT32_MAX / (Uint32)sizeof(Sim3DGpuVertex);
  if (vertex_count > maximum_vertices) {
    fprintf(stderr, "[sim3d-depth] geometry buffer exceeds GPU size limit\n");
    return false;
  }
  Uint32 capacity = g_depth_pass.gpu_vertex_capacity
      ? g_depth_pass.gpu_vertex_capacity
      : kSim3DDepthInitialGpuVertexCapacity;
  while (capacity < vertex_count) {
    if (capacity > maximum_vertices / 2) {
      capacity = vertex_count;
      break;
    }
    capacity *= 2;
  }
  SDL_GPUBufferCreateInfo buffer_info = {
    .usage = SDL_GPU_BUFFERUSAGE_VERTEX,
    .size = capacity * (Uint32)sizeof(Sim3DGpuVertex),
  };
  SDL_GPUTransferBufferCreateInfo transfer_info = {
    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
    .size = buffer_info.size,
  };
  SDL_GPUBuffer *vertex_buffer = SDL_CreateGPUBuffer(
      g_depth_pass.device, &buffer_info);
  SDL_GPUTransferBuffer *transfer_buffer = SDL_CreateGPUTransferBuffer(
      g_depth_pass.device, &transfer_info);
  const Uint32 index_capacity =
      capacity / kSim3DDepthVerticesPerQuad * kSim3DDepthIndicesPerQuad;
  SDL_GPUBufferCreateInfo index_buffer_info = {
    .usage = SDL_GPU_BUFFERUSAGE_INDEX,
    .size = index_capacity * (Uint32)sizeof(Uint32),
  };
  SDL_GPUTransferBufferCreateInfo index_transfer_info = {
    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
    .size = index_buffer_info.size,
  };
  SDL_GPUBuffer *index_buffer = SDL_CreateGPUBuffer(
      g_depth_pass.device, &index_buffer_info);
  SDL_GPUTransferBuffer *index_transfer_buffer =
      SDL_CreateGPUTransferBuffer(
          g_depth_pass.device, &index_transfer_info);
  if (!vertex_buffer || !transfer_buffer || !index_buffer ||
      !index_transfer_buffer) {
    fprintf(stderr, "[sim3d-depth] geometry buffer creation failed: %s\n",
            SDL_GetError());
    if (vertex_buffer)
      SDL_ReleaseGPUBuffer(g_depth_pass.device, vertex_buffer);
    if (transfer_buffer)
      SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer_buffer);
    if (index_buffer)
      SDL_ReleaseGPUBuffer(g_depth_pass.device, index_buffer);
    if (index_transfer_buffer)
      SDL_ReleaseGPUTransferBuffer(
          g_depth_pass.device, index_transfer_buffer);
    return false;
  }
  Uint32 *indices = SDL_MapGPUTransferBuffer(
      g_depth_pass.device, index_transfer_buffer, false);
  if (!indices) {
    fprintf(stderr, "[sim3d-depth] index staging map failed: %s\n",
            SDL_GetError());
    SDL_ReleaseGPUBuffer(g_depth_pass.device, vertex_buffer);
    SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer_buffer);
    SDL_ReleaseGPUBuffer(g_depth_pass.device, index_buffer);
    SDL_ReleaseGPUTransferBuffer(
        g_depth_pass.device, index_transfer_buffer);
    return false;
  }
  static const Uint32 order[kSim3DDepthIndicesPerQuad] = {
    0, 1, 2, 0, 2, 3,
  };
  for (Uint32 quad = 0;
       quad < capacity / kSim3DDepthVerticesPerQuad; quad++) {
    const Uint32 base = quad * kSim3DDepthVerticesPerQuad;
    for (int i = 0; i < kSim3DDepthIndicesPerQuad; i++)
      indices[quad * kSim3DDepthIndicesPerQuad + (Uint32)i] =
          base + order[i];
  }
  SDL_UnmapGPUTransferBuffer(
      g_depth_pass.device, index_transfer_buffer);
  if (g_depth_pass.vertex_buffer)
    SDL_ReleaseGPUBuffer(g_depth_pass.device, g_depth_pass.vertex_buffer);
  if (g_depth_pass.transfer_buffer)
    SDL_ReleaseGPUTransferBuffer(
        g_depth_pass.device, g_depth_pass.transfer_buffer);
  if (g_depth_pass.index_buffer)
    SDL_ReleaseGPUBuffer(g_depth_pass.device, g_depth_pass.index_buffer);
  if (g_depth_pass.index_transfer_buffer)
    SDL_ReleaseGPUTransferBuffer(
        g_depth_pass.device, g_depth_pass.index_transfer_buffer);
  g_depth_pass.vertex_buffer = vertex_buffer;
  g_depth_pass.transfer_buffer = transfer_buffer;
  g_depth_pass.index_buffer = index_buffer;
  g_depth_pass.index_transfer_buffer = index_transfer_buffer;
  g_depth_pass.gpu_vertex_capacity = capacity;
  g_depth_pass.gpu_index_capacity = index_capacity;
  g_depth_pass.index_upload_required = true;
  return true;
}

static SDL_GPUTexture *TextureForLayer(
    Sim3DDepthPassLayer layer, SDL_Texture *shadow_texture) {
  switch (layer) {
    case kSim3DDepthPass_Mountain:
    case kSim3DDepthPass_WorldMountain:
    case kSim3DDepthPass_Ground:
    case kSim3DDepthPass_GroundBlur:
    case kSim3DDepthPass_Cloud:
    case kSim3DDepthPass_VolumeCloud:
      return g_depth_pass.atlases[layer].texture;
    case kSim3DDepthPass_CloudShadow:
      return g_depth_pass.atlases[kSim3DDepthPass_Cloud].texture;
    case kSim3DDepthPass_ShadowReceiver:
      return GpuTexture(shadow_texture);
    case kSim3DDepthPass_GroundHaze:
    case kSim3DDepthPass_Effect:
    case kSim3DDepthPass_DepthOccluder:
    case kSim3DDepthPass_Solid:
      return g_depth_pass.white_texture;
    case kSim3DDepthPassLayerCount:
      break;
  }
  return NULL;
}

/* Opaque geometry writes depth; transparent effects only test against it.
 * Selecting per layer rather than switching once part-way through the loop
 * keeps that a property of the layer instead of a property of where the layer
 * happens to sit in the enum. */
static SDL_GPUGraphicsPipeline *PipelineForLayer(Sim3DDepthPassLayer layer) {
  switch (layer) {
    case kSim3DDepthPass_DepthOccluder:
      return g_depth_pass.depth_occluder_pipeline;
    case kSim3DDepthPass_GroundBlur:
    case kSim3DDepthPass_GroundHaze:
    case kSim3DDepthPass_CloudShadow:
    case kSim3DDepthPass_Cloud:
    case kSim3DDepthPass_VolumeCloud:
    case kSim3DDepthPass_Effect:
    case kSim3DDepthPass_ShadowReceiver:
      return g_depth_pass.effect_pipeline;
    case kSim3DDepthPass_Ground:
    case kSim3DDepthPass_Solid:
    case kSim3DDepthPass_Mountain:
    case kSim3DDepthPass_WorldMountain:
      return g_depth_pass.pipeline;
    case kSim3DDepthPassLayerCount:
      break;
  }
  return NULL;
}

static SDL_GPUSampler *SamplerForLayer(Sim3DDepthPassLayer layer) {
  /* Pixel-art mountain cutouts remain nearest-neighbour. The shadow receiver
   * is a filtered screen-space mask and may intentionally use a smaller
   * working target, so linear sampling is part of that layer's contract. */
  return layer == kSim3DDepthPass_ShadowReceiver ||
      layer == kSim3DDepthPass_Cloud || layer == kSim3DDepthPass_CloudShadow ||
      layer == kSim3DDepthPass_VolumeCloud ||
      layer == kSim3DDepthPass_Ground || layer == kSim3DDepthPass_GroundBlur
      ? g_depth_pass.linear_sampler : g_depth_pass.nearest_sampler;
}

static bool PrepareMeshSamples(void) {
  if (!g_depth_pass.sample_count) return true;
  const Uint32 uv_bytes = g_depth_pass.sample_vertices * (Uint32)sizeof(ArRenderPointF);
  const Uint32 bytes = uv_bytes + g_depth_pass.sample_count * (Uint32)sizeof(ArRenderColorF);
  if (bytes > g_depth_pass.sample_gpu_bytes) {
    Uint32 capacity = g_depth_pass.sample_gpu_bytes ? g_depth_pass.sample_gpu_bytes : 32768;
    while (capacity < bytes) capacity *= 2;
    const SDL_GPUBufferCreateInfo info = {.usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = capacity};
    const SDL_GPUTransferBufferCreateInfo transfer_info = {
      .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = capacity,
    };
    SDL_GPUBuffer *buffer = SDL_CreateGPUBuffer(g_depth_pass.device, &info);
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(g_depth_pass.device, &transfer_info);
    if (!buffer || !transfer) {
      if (buffer) SDL_ReleaseGPUBuffer(g_depth_pass.device, buffer);
      if (transfer) SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
      return false;
    }
    if (g_depth_pass.sample_buffer) SDL_ReleaseGPUBuffer(g_depth_pass.device, g_depth_pass.sample_buffer);
    if (g_depth_pass.sample_transfer) SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, g_depth_pass.sample_transfer);
    g_depth_pass.sample_buffer = buffer; g_depth_pass.sample_transfer = transfer;
    g_depth_pass.sample_gpu_bytes = capacity;
  }
  uint8_t *mapped = SDL_MapGPUTransferBuffer(g_depth_pass.device, g_depth_pass.sample_transfer, true);
  if (!mapped) return false;
  if (uv_bytes) memcpy(mapped, g_depth_pass.sample_uv, uv_bytes);
  for (Uint32 i = 0; i < g_depth_pass.sample_count; ++i)
    memcpy(mapped + uv_bytes + i * sizeof(ArRenderColorF), &g_depth_pass.samples[i].color,
        sizeof(ArRenderColorF));
  SDL_UnmapGPUTransferBuffer(g_depth_pass.device, g_depth_pass.sample_transfer);
  return true;
}

ArRenderTexture Sim3DDepthPass_Submit(
    ArRenderDevice *device, ArRenderTexture shadow_texture) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  SDL_Texture *native_shadow =
      ArSdlRenderBackend_UnwrapTexture(shadow_texture);
  if (!g_depth_pass.collecting || renderer != g_depth_pass.renderer)
    return ArRenderTexture_Invalid();
  g_depth_pass.collecting = false;
  if (g_depth_pass.geometry_failed) {
    fprintf(stderr, "[sim3d-depth] geometry collection failed or a queued mesh was invalidated\n");
    return ArRenderTexture_Invalid();
  }
  Uint32 total = 0;
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++) {
    if (g_depth_pass.lists[i].count > UINT32_MAX - total) {
      fprintf(stderr, "[sim3d-depth] geometry vertex count overflow\n");
      return ArRenderTexture_Invalid();
    }
    total += g_depth_pass.lists[i].count;
  }
  Uint32 needed = total;
  for (Uint32 i = 0; i < g_depth_pass.sample_count; ++i) {
    const Sim3DMeshSample *sample = &g_depth_pass.samples[i];
    if (!TextureForLayer(sample->layer, native_shadow)) return ArRenderTexture_Invalid();
    if (sample->mesh->count > needed) needed = sample->mesh->count;
  }
  if (!needed || total % kSim3DDepthVerticesPerQuad != 0 ||
      !EnsureGpuBuffers(needed) || !PrepareMeshSamples())
    return ArRenderTexture_Invalid();
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++) {
    if (!g_depth_pass.lists[i].count) continue;
    if (!TextureForLayer((Sim3DDepthPassLayer)i, native_shadow)) {
      fprintf(stderr, "[sim3d-depth] material layer %d has no GPU texture\n",
              i);
      return ArRenderTexture_Invalid();
    }
  }

  Sim3DGpuVertex *mapped = SDL_MapGPUTransferBuffer(
      g_depth_pass.device, g_depth_pass.transfer_buffer, true);
  if (!mapped) {
    fprintf(stderr, "[sim3d-depth] geometry upload map failed: %s\n",
            SDL_GetError());
    return ArRenderTexture_Invalid();
  }
  Uint32 first[kSim3DDepthPassLayerCount];
  Uint32 at = 0;
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++) {
    first[i] = at;
    size_t bytes = (size_t)g_depth_pass.lists[i].count * sizeof(*mapped);
    memcpy(mapped + at, g_depth_pass.lists[i].vertices, bytes);
    at += g_depth_pass.lists[i].count;
  }
  SDL_UnmapGPUTransferBuffer(
      g_depth_pass.device, g_depth_pass.transfer_buffer);

  /* SDL_Renderer and SDL_GPU share this device. Flush is the explicit API
   * boundary that submits queued 2D work and invalidates SDL's cached GPU
   * state before this command buffer writes the shared target. */
  if (!SDL_FlushRenderer(renderer)) return ArRenderTexture_Invalid();
  SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(
      g_depth_pass.device);
  if (!commands) return ArRenderTexture_Invalid();
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
  if (!copy) {
    SDL_CancelGPUCommandBuffer(commands);
    return ArRenderTexture_Invalid();
  }
  SDL_GPUTransferBufferLocation source = {
    .transfer_buffer = g_depth_pass.transfer_buffer,
    .offset = 0,
  };
  SDL_GPUBufferRegion destination = {
    .buffer = g_depth_pass.vertex_buffer,
    .offset = 0,
    .size = total * (Uint32)sizeof(Sim3DGpuVertex),
  };
  uint64_t vertex_upload_bytes = destination.size;
  if (total) SDL_UploadToGPUBuffer(copy, &source, &destination, true);
  if (g_depth_pass.sample_count) {
    const SDL_GPUTransferBufferLocation sample_source = {
      .transfer_buffer = g_depth_pass.sample_transfer,
    };
    const SDL_GPUBufferRegion sample_destination = {
      .buffer = g_depth_pass.sample_buffer,
      .size = g_depth_pass.sample_vertices * (Uint32)sizeof(ArRenderPointF) +
          g_depth_pass.sample_count * (Uint32)sizeof(ArRenderColorF),
    };
    SDL_UploadToGPUBuffer(copy, &sample_source, &sample_destination, true);
    vertex_upload_bytes += sample_destination.size;
    for (Sim3DDepthMesh *mesh = s_meshes; mesh; mesh = mesh->next) {
      if (!mesh->queued || !mesh->dirty) continue;
      const SDL_GPUTransferBufferLocation mesh_source = {.transfer_buffer = mesh->transfer};
      const SDL_GPUBufferRegion mesh_destination = {
        .buffer = mesh->positions, .size = mesh->count * MeshVertexBytes(mesh),
      };
      SDL_UploadToGPUBuffer(copy, &mesh_source, &mesh_destination, true);
      vertex_upload_bytes += mesh_destination.size;
    }
  }
  if (g_depth_pass.index_upload_required) {
    SDL_GPUTransferBufferLocation index_source = {
      .transfer_buffer = g_depth_pass.index_transfer_buffer,
      .offset = 0,
    };
    SDL_GPUBufferRegion index_destination = {
      .buffer = g_depth_pass.index_buffer,
      .offset = 0,
      .size = g_depth_pass.gpu_index_capacity * (Uint32)sizeof(Uint32),
    };
    SDL_UploadToGPUBuffer(
        copy, &index_source, &index_destination, false);
  }
  SDL_EndGPUCopyPass(copy);

  SDL_GPUColorTargetInfo color;
  SDL_zero(color);
  color.texture = g_depth_pass.color_target;
  color.clear_color = (SDL_FColor){0, 0, 0, 0};
  color.load_op = SDL_GPU_LOADOP_CLEAR;
  color.store_op = SDL_GPU_STOREOP_STORE;
  color.cycle = true;
  SDL_GPUDepthStencilTargetInfo depth;
  SDL_zero(depth);
  depth.texture = g_depth_pass.depth_target;
  depth.clear_depth = 1.0f;
  depth.load_op = SDL_GPU_LOADOP_CLEAR;
  depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
  depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
  depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
  depth.cycle = true;
  SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
      commands, &color, 1, &depth);
  if (!pass) {
    SDL_CancelGPUCommandBuffer(commands);
    return ArRenderTexture_Invalid();
  }
  SDL_GPUBufferBinding vertex_binding = {
    .buffer = g_depth_pass.vertex_buffer,
    .offset = 0,
  };
  SDL_GPUBufferBinding index_binding = {
    .buffer = g_depth_pass.index_buffer,
    .offset = 0,
  };
  SDL_BindGPUIndexBuffer(
      pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
  SDL_GPUGraphicsPipeline *bound = NULL;
  static const Sim3DDepthPassLayer order[] = {
    kSim3DDepthPass_DepthOccluder, kSim3DDepthPass_Ground,
    kSim3DDepthPass_GroundBlur, kSim3DDepthPass_GroundHaze,
    kSim3DDepthPass_CloudShadow,
    kSim3DDepthPass_Solid, kSim3DDepthPass_Mountain, kSim3DDepthPass_WorldMountain,
    kSim3DDepthPass_ShadowReceiver, kSim3DDepthPass_Effect, kSim3DDepthPass_Cloud,
    kSim3DDepthPass_VolumeCloud,
  };
  _Static_assert(sizeof(order) / sizeof(order[0]) == kSim3DDepthPassLayerCount,
                 "Every material layer needs a depth draw order");
  for (int draw = 0; draw < kSim3DDepthPassLayerCount; draw++) {
    const Sim3DDepthPassLayer i = order[draw];
    bool samples = false;
    for (Uint32 j = 0; j < g_depth_pass.sample_count; ++j)
      samples |= g_depth_pass.samples[j].layer == i;
    if (!g_depth_pass.lists[i].count && !samples) continue;
    SDL_GPUGraphicsPipeline *pipeline = PipelineForLayer((Sim3DDepthPassLayer)i);
    if (pipeline != bound) {
      SDL_BindGPUGraphicsPipeline(pass, pipeline);
      bound = pipeline;
    }
    SDL_GPUTexture *texture = TextureForLayer(
        (Sim3DDepthPassLayer)i, native_shadow);
    SDL_GPUTextureSamplerBinding texture_binding = {
      .texture = texture,
      .sampler = SamplerForLayer((Sim3DDepthPassLayer)i),
    };
    SDL_BindGPUFragmentSamplers(pass, 0, &texture_binding, 1);
    if (samples) {
      for (Uint32 j = 0; j < g_depth_pass.sample_count; ++j) {
        const Sim3DMeshSample *sample = &g_depth_pass.samples[j];
        if (sample->layer != i) continue;
        pipeline = sample->mesh->spherical ? g_depth_pass.spherical_pipeline : g_depth_pass.mesh_pipeline;
        if (pipeline != bound) {
          SDL_BindGPUGraphicsPipeline(pass, pipeline);
          bound = pipeline;
        }
        if (sample->mesh->spherical) {
          const SDL_GPUBufferBinding binding = {sample->mesh->positions, 0};
          SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
          SDL_PushGPUVertexUniformData(commands, 0, &sample->spherical, sizeof(sample->spherical));
          SDL_DrawGPUIndexedPrimitives(pass, 6, sample->mesh->count / 4, 0, 0, 0);
          continue;
        }
        const SDL_GPUBufferBinding bindings[] = {
          {sample->mesh->positions, 0},
          {g_depth_pass.sample_buffer, sample->first * (Uint32)sizeof(ArRenderPointF)},
          {g_depth_pass.sample_buffer, g_depth_pass.sample_vertices * (Uint32)sizeof(ArRenderPointF) +
              j * (Uint32)sizeof(ArRenderColorF)},
        };
        SDL_BindGPUVertexBuffers(pass, 0, bindings, 3);
        SDL_DrawGPUIndexedPrimitives(pass, sample->mesh->count / 4 * 6, 1, 0, 0, 0);
      }
    } else {
      vertex_binding.offset = first[i] * (Uint32)sizeof(Sim3DGpuVertex);
      SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
      SDL_DrawGPUIndexedPrimitives(pass,
          g_depth_pass.lists[i].count / kSim3DDepthVerticesPerQuad * kSim3DDepthIndicesPerQuad,
          1, 0, 0, 0);
    }
  }
  SDL_EndGPURenderPass(pass);
  if (!SDL_SubmitGPUCommandBuffer(commands)) {
    fprintf(stderr, "[sim3d-depth] command submission failed: %s\n",
            SDL_GetError());
    return ArRenderTexture_Invalid();
  }
  Sim3DPerformance_AddGeometryUpload(vertex_upload_bytes);
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++) {
    if (!g_depth_pass.lists[i].count) continue;
    Sim3DPerformance_AddDraw(
        g_depth_pass.lists[i].count,
        g_depth_pass.lists[i].count / kSim3DDepthVerticesPerQuad *
            kSim3DDepthIndicesPerQuad);
  }
  for (Uint32 i = 0; i < g_depth_pass.sample_count; ++i) {
    const Uint32 count = g_depth_pass.samples[i].mesh->count;
    Sim3DPerformance_AddDraw(count, count / 4 * 6);
  }
  for (Sim3DDepthMesh *mesh = s_meshes; mesh; mesh = mesh->next)
    if (mesh->queued) mesh->dirty = false;
  g_depth_pass.index_upload_required = false;
  return ArSdlRenderBackend_BorrowTexture(g_depth_pass.output_texture);
}

bool Sim3DDepthPass_IsCollecting(void) {
  return g_depth_pass.collecting;
}

void Sim3DDepthPass_Reset(ArRenderDevice *device) {
  (void)device;
  g_depth_pass.collecting = false;
  ReleaseTargets();
  for (Sim3DDepthMesh *mesh = s_meshes; mesh; mesh = mesh->next) ReleaseMeshStorage(mesh);
  if (g_depth_pass.mesh_pipeline)
    SDL_ReleaseGPUGraphicsPipeline(g_depth_pass.device, g_depth_pass.mesh_pipeline);
  if (g_depth_pass.spherical_pipeline)
    SDL_ReleaseGPUGraphicsPipeline(g_depth_pass.device, g_depth_pass.spherical_pipeline);
  if (g_depth_pass.spherical_shader)
    SDL_ReleaseGPUShader(g_depth_pass.device, g_depth_pass.spherical_shader);
  if (g_depth_pass.sample_buffer)
    SDL_ReleaseGPUBuffer(g_depth_pass.device, g_depth_pass.sample_buffer);
  if (g_depth_pass.sample_transfer)
    SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, g_depth_pass.sample_transfer);
  free(g_depth_pass.sample_uv);
  if (g_depth_pass.pipeline)
    SDL_ReleaseGPUGraphicsPipeline(
        g_depth_pass.device, g_depth_pass.pipeline);
  if (g_depth_pass.depth_occluder_pipeline)
    SDL_ReleaseGPUGraphicsPipeline(
        g_depth_pass.device, g_depth_pass.depth_occluder_pipeline);
  if (g_depth_pass.effect_pipeline)
    SDL_ReleaseGPUGraphicsPipeline(
        g_depth_pass.device, g_depth_pass.effect_pipeline);
  if (g_depth_pass.vertex_shader)
    SDL_ReleaseGPUShader(g_depth_pass.device, g_depth_pass.vertex_shader);
  if (g_depth_pass.fragment_shader)
    SDL_ReleaseGPUShader(g_depth_pass.device, g_depth_pass.fragment_shader);
  if (g_depth_pass.nearest_sampler)
    SDL_ReleaseGPUSampler(g_depth_pass.device, g_depth_pass.nearest_sampler);
  if (g_depth_pass.linear_sampler)
    SDL_ReleaseGPUSampler(g_depth_pass.device, g_depth_pass.linear_sampler);
  if (g_depth_pass.vertex_buffer)
    SDL_ReleaseGPUBuffer(g_depth_pass.device, g_depth_pass.vertex_buffer);
  if (g_depth_pass.transfer_buffer)
    SDL_ReleaseGPUTransferBuffer(
        g_depth_pass.device, g_depth_pass.transfer_buffer);
  if (g_depth_pass.index_buffer)
    SDL_ReleaseGPUBuffer(g_depth_pass.device, g_depth_pass.index_buffer);
  if (g_depth_pass.index_transfer_buffer)
    SDL_ReleaseGPUTransferBuffer(
        g_depth_pass.device, g_depth_pass.index_transfer_buffer);
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++) {
    Sim3DDepthAtlas *atlas = &g_depth_pass.atlases[i];
    if (atlas->texture) SDL_ReleaseGPUTexture(g_depth_pass.device, atlas->texture);
    if (atlas->transfer)
      SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, atlas->transfer);
  }
  if (g_depth_pass.white_texture)
    SDL_ReleaseGPUTexture(g_depth_pass.device, g_depth_pass.white_texture);
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++) {
    free(g_depth_pass.lists[i].vertices);
    g_depth_pass.lists[i].vertices = NULL;
  }
  memset(&g_depth_pass, 0, sizeof(g_depth_pass));
}
