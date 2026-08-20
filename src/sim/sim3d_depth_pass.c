#include "sim3d_depth_pass.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_shader_blob.h"
#include "shaders/sim3d_depth_frag.h"
#include "shaders/sim3d_depth_vert.h"

enum {
  kSim3DDepthRgbaBytesPerPixel = 4,
  kSim3DDepthVerticesPerQuad = 4,
  kSim3DDepthIndicesPerQuad = 6,
  kSim3DDepthInitialCpuVertexCapacity = 4096,
  kSim3DDepthInitialGpuVertexCapacity = 8192,
};

typedef struct Sim3DGpuVertex {
  float position[4];
  float color[4];
  float uv[2];
} Sim3DGpuVertex;

typedef struct Sim3DDepthList {
  Sim3DGpuVertex *vertices;
  Uint32 count;
  Uint32 capacity;
} Sim3DDepthList;

static struct {
  SDL_Renderer *renderer;
  SDL_GPUDevice *device;
  SDL_GPUShader *vertex_shader;
  SDL_GPUShader *fragment_shader;
  SDL_GPUGraphicsPipeline *pipeline;
  SDL_GPUGraphicsPipeline *depth_occluder_pipeline;
  SDL_GPUGraphicsPipeline *effect_pipeline;
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
  SDL_GPUTexture *mountain_atlas;
  SDL_GPUTransferBuffer *mountain_atlas_transfer;
  Uint32 mountain_atlas_transfer_size;
  int mountain_atlas_width, mountain_atlas_height;
  SDL_GPUTexture *white_texture;
  SDL_Texture *output_texture;
  int width, height;
  float clip_x_scale, clip_y_scale;
  bool collecting;
  bool geometry_failed;
  bool failed;
  Sim3DDepthList lists[kSim3DDepthPassLayerCount];
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
    Sim3DDepthPass_Reset();

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
  g_depth_pass.renderer = renderer;
  if (!CreatePipeline() || !CreateWhiteTexture(renderer)) {
    fprintf(stderr, "[sim3d-depth] required GPU resources failed: %s\n",
            SDL_GetError());
    g_depth_pass.failed = true;
    return false;
  }
  return true;
}

bool Sim3DDepthPass_Require(SDL_Renderer *renderer) {
  return renderer && EnsureInitialized(renderer);
}

const char *Sim3DDepthPass_LastError(void) {
  const char *error = SDL_GetError();
  return error && error[0] ? error : "required SDL_GPU depth pass unavailable";
}

bool Sim3DDepthPass_UploadMountainAtlas(SDL_Renderer *renderer,
                                        const uint32_t *argb_pixels,
                                        int width, int height, int pitch) {
  if (!renderer || !argb_pixels || width <= 0 || height <= 0 || pitch <= 0)
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

  const bool resources_match = g_depth_pass.mountain_atlas &&
      g_depth_pass.mountain_atlas_transfer &&
      g_depth_pass.mountain_atlas_width == width &&
      g_depth_pass.mountain_atlas_height == height &&
      g_depth_pass.mountain_atlas_transfer_size >= upload_size;
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
      fprintf(stderr, "[sim3d-depth] mountain upload resource creation "
                      "failed: %s\n", SDL_GetError());
      if (transfer)
        SDL_ReleaseGPUTransferBuffer(g_depth_pass.device, transfer);
      if (texture)
        SDL_ReleaseGPUTexture(g_depth_pass.device, texture);
      return false;
    }
    if (g_depth_pass.mountain_atlas_transfer)
      SDL_ReleaseGPUTransferBuffer(
          g_depth_pass.device, g_depth_pass.mountain_atlas_transfer);
    if (g_depth_pass.mountain_atlas)
      SDL_ReleaseGPUTexture(g_depth_pass.device, g_depth_pass.mountain_atlas);
    g_depth_pass.mountain_atlas = texture;
    g_depth_pass.mountain_atlas_transfer = transfer;
    g_depth_pass.mountain_atlas_transfer_size = upload_size;
    g_depth_pass.mountain_atlas_width = width;
    g_depth_pass.mountain_atlas_height = height;
  }

  uint8_t *mapped = SDL_MapGPUTransferBuffer(
      g_depth_pass.device, g_depth_pass.mountain_atlas_transfer, true);
  if (!mapped) {
    fprintf(stderr, "[sim3d-depth] mountain upload allocation failed: %s\n",
            SDL_GetError());
    return false;
  }
  /* Convert numeric ARGB words to an explicit byte format. Besides avoiding
   * backend-specific channel layouts, this keeps the upload correct on either
   * host byte order; it runs only when the immutable town atlas changes. */
  for (int y = 0; y < height; y++) {
    const uint8_t *source =
        (const uint8_t *)argb_pixels + (size_t)y * (size_t)pitch;
    uint8_t *destination = mapped + (size_t)y * row_bytes;
    for (int x = 0; x < width; x++) {
      /* A caller-provided pitch is not required to preserve uint32_t
       * alignment on every row. memcpy gives the numeric ARGB word without
       * an unaligned typed dereference (undefined on strict-alignment ARM). */
      uint32_t argb;
      memcpy(&argb, source + (size_t)x * sizeof(argb), sizeof(argb));
      destination[x * kSim3DDepthRgbaBytesPerPixel + 0] =
          (uint8_t)(argb >> 16);
      destination[x * kSim3DDepthRgbaBytesPerPixel + 1] =
          (uint8_t)(argb >> 8);
      destination[x * kSim3DDepthRgbaBytesPerPixel + 2] = (uint8_t)argb;
      destination[x * kSim3DDepthRgbaBytesPerPixel + 3] =
          (uint8_t)(argb >> 24);
    }
  }
  SDL_UnmapGPUTransferBuffer(
      g_depth_pass.device, g_depth_pass.mountain_atlas_transfer);

  if (!SDL_FlushRenderer(renderer)) return false;
  SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(
      g_depth_pass.device);
  SDL_GPUCopyPass *copy = commands ? SDL_BeginGPUCopyPass(commands) : NULL;
  if (!copy) {
    if (commands) SDL_CancelGPUCommandBuffer(commands);
    return false;
  }
  SDL_GPUTextureTransferInfo source_info = {
    .transfer_buffer = g_depth_pass.mountain_atlas_transfer,
    .offset = 0,
    .pixels_per_row = (Uint32)width,
    .rows_per_layer = (Uint32)height,
  };
  SDL_GPUTextureRegion destination = {
    .texture = g_depth_pass.mountain_atlas,
    .w = (Uint32)width,
    .h = (Uint32)height,
    .d = 1,
  };
  /* Both resources persist across animated canvas revisions. Cycling keeps a
   * new upload from waiting on the previous frame while avoiding per-frame
   * GPU allocation and deferred destruction. */
  SDL_UploadToGPUTexture(copy, &source_info, &destination, true);
  SDL_EndGPUCopyPass(copy);
  if (!SDL_SubmitGPUCommandBuffer(commands)) {
    fprintf(stderr, "[sim3d-depth] mountain upload submission failed: %s\n",
            SDL_GetError());
    return false;
  }
  return true;
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

bool Sim3DDepthPass_Begin(SDL_Renderer *renderer, int width, int height,
                          SDL_ScaleMode output_scale_mode) {
  if (!renderer || width <= 0 || height <= 0 ||
      !EnsureInitialized(renderer) ||
      !CreateTargets(renderer, width, height, output_scale_mode))
    return false;
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++)
    g_depth_pass.lists[i].count = 0;
  g_depth_pass.geometry_failed = false;
  g_depth_pass.collecting = true;
  g_depth_pass.clip_x_scale = 2.0f / (float)width;
  g_depth_pass.clip_y_scale = 2.0f / (float)height;
  return true;
}

bool Sim3DDepthPass_AppendQuad(Sim3DDepthPassLayer layer,
                               const Sim3DDepthVertex vertices[4]) {
  if (!g_depth_pass.collecting || !vertices || layer < 0 ||
      layer >= kSim3DDepthPassLayerCount)
    return false;
  Sim3DDepthList *list = &g_depth_pass.lists[layer];
  if (!ReserveList(list, kSim3DDepthVerticesPerQuad)) {
    g_depth_pass.geometry_failed = true;
    return false;
  }
  for (int i = 0; i < kSim3DDepthVerticesPerQuad; i++) {
    const Sim3DDepthVertex *source = &vertices[i];
    Sim3DGpuVertex *destination = &list->vertices[list->count++];
    destination->position[0] =
        source->x * g_depth_pass.clip_x_scale - 1.0f;
    destination->position[1] =
        1.0f - source->y * g_depth_pass.clip_y_scale;
    destination->position[2] = source->depth;
    destination->position[3] = 1.0f;
    destination->color[0] = source->color.r;
    destination->color[1] = source->color.g;
    destination->color[2] = source->color.b;
    destination->color[3] = source->color.a;
    destination->uv[0] = source->uv.x;
    destination->uv[1] = source->uv.y;
  }
  return true;
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
      return g_depth_pass.mountain_atlas;
    case kSim3DDepthPass_ShadowReceiver:
      return GpuTexture(shadow_texture);
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
    case kSim3DDepthPass_Effect:
    case kSim3DDepthPass_ShadowReceiver:
      return g_depth_pass.effect_pipeline;
    case kSim3DDepthPass_Solid:
    case kSim3DDepthPass_Mountain:
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
  return layer == kSim3DDepthPass_ShadowReceiver
      ? g_depth_pass.linear_sampler : g_depth_pass.nearest_sampler;
}

SDL_Texture *Sim3DDepthPass_Submit(SDL_Renderer *renderer,
                                   SDL_Texture *shadow_texture) {
  if (!g_depth_pass.collecting || renderer != g_depth_pass.renderer)
    return NULL;
  g_depth_pass.collecting = false;
  if (g_depth_pass.geometry_failed) {
    fprintf(stderr, "[sim3d-depth] CPU geometry staging allocation failed\n");
    return NULL;
  }
  Uint32 total = 0;
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++) {
    if (g_depth_pass.lists[i].count > UINT32_MAX - total) {
      fprintf(stderr, "[sim3d-depth] geometry vertex count overflow\n");
      return NULL;
    }
    total += g_depth_pass.lists[i].count;
  }
  if (!total || total % kSim3DDepthVerticesPerQuad != 0 ||
      !EnsureGpuBuffers(total))
    return NULL;
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++) {
    if (!g_depth_pass.lists[i].count) continue;
    if (!TextureForLayer((Sim3DDepthPassLayer)i, shadow_texture)) {
      fprintf(stderr, "[sim3d-depth] material layer %d has no GPU texture\n",
              i);
      return NULL;
    }
  }

  Sim3DGpuVertex *mapped = SDL_MapGPUTransferBuffer(
      g_depth_pass.device, g_depth_pass.transfer_buffer, true);
  if (!mapped) {
    fprintf(stderr, "[sim3d-depth] geometry upload map failed: %s\n",
            SDL_GetError());
    return NULL;
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
  if (!SDL_FlushRenderer(renderer)) return NULL;
  SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(
      g_depth_pass.device);
  if (!commands) return NULL;
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
  if (!copy) {
    SDL_CancelGPUCommandBuffer(commands);
    return NULL;
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
  SDL_UploadToGPUBuffer(copy, &source, &destination, true);
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
    return NULL;
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
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++) {
    if (!g_depth_pass.lists[i].count) continue;
    vertex_binding.offset = first[i] * (Uint32)sizeof(Sim3DGpuVertex);
    SDL_BindGPUVertexBuffers(pass, 0, &vertex_binding, 1);
    SDL_GPUGraphicsPipeline *pipeline =
        PipelineForLayer((Sim3DDepthPassLayer)i);
    if (pipeline != bound) {
      SDL_BindGPUGraphicsPipeline(pass, pipeline);
      bound = pipeline;
    }
    SDL_GPUTexture *texture = TextureForLayer(
        (Sim3DDepthPassLayer)i, shadow_texture);
    SDL_GPUTextureSamplerBinding texture_binding = {
      .texture = texture,
      .sampler = SamplerForLayer((Sim3DDepthPassLayer)i),
    };
    SDL_BindGPUFragmentSamplers(pass, 0, &texture_binding, 1);
    SDL_DrawGPUIndexedPrimitives(
        pass,
        g_depth_pass.lists[i].count / kSim3DDepthVerticesPerQuad *
            kSim3DDepthIndicesPerQuad,
        1, 0, 0, 0);
  }
  SDL_EndGPURenderPass(pass);
  if (!SDL_SubmitGPUCommandBuffer(commands)) {
    fprintf(stderr, "[sim3d-depth] command submission failed: %s\n",
            SDL_GetError());
    return NULL;
  }
  g_depth_pass.index_upload_required = false;
  return g_depth_pass.output_texture;
}

bool Sim3DDepthPass_IsCollecting(void) {
  return g_depth_pass.collecting;
}

void Sim3DDepthPass_Reset(void) {
  g_depth_pass.collecting = false;
  ReleaseTargets();
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
  if (g_depth_pass.mountain_atlas)
    SDL_ReleaseGPUTexture(g_depth_pass.device, g_depth_pass.mountain_atlas);
  if (g_depth_pass.mountain_atlas_transfer)
    SDL_ReleaseGPUTransferBuffer(
        g_depth_pass.device, g_depth_pass.mountain_atlas_transfer);
  if (g_depth_pass.white_texture)
    SDL_ReleaseGPUTexture(g_depth_pass.device, g_depth_pass.white_texture);
  for (int i = 0; i < kSim3DDepthPassLayerCount; i++) {
    free(g_depth_pass.lists[i].vertices);
    g_depth_pass.lists[i].vertices = NULL;
  }
  memset(&g_depth_pass, 0, sizeof(g_depth_pass));
}
