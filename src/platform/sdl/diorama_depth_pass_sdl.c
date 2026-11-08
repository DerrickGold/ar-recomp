#include "diorama_depth_pass_sdl.h"
#include "diorama_depth_alpha_sdl.h"
#include "gpu_shader_blob.h"
#include "shaders/sim3d_depth_vert.h"
#include "shaders/diorama_depth_vert.h"
#include "shaders/sim3d_depth_frag.h"
#include "shaders/scenery_depth_frag.h"
#include "shaders/actor_depth_frag.h"
#include "diorama/diorama_scenery_uniforms.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(DioramaDepthVertex) == 48 &&
    offsetof(DioramaDepthVertex, color) == 16 &&
    offsetof(DioramaDepthVertex, uv) == 32 &&
    offsetof(DioramaDepthVertex, heat_world_xy) == 40, "depth shader vertex layout");

static int BlendIndex(ArRenderBlendMode blend) {
  switch (blend) {
    case kArRenderBlendMode_Opaque: return 0;
    case kArRenderBlendMode_Alpha: return 1;
    case kArRenderBlendMode_Add: return 2;
    default: return -1;
  }
}

void ArGpuDioramaDepthPass_Destroy(ArGpuDioramaDepthPass *p) {
  if (!p || !p->device) return;
  ArGpuDioramaDepthAlpha_Destroy(p);
  for (unsigned m = 0; m < 3; ++m)
    for (unsigned d = 0; d < kDioramaDepth_Count; ++d)
      for (unsigned b = 0; b < 3; ++b)
        if (p->pipelines[m][d][b]) SDL_ReleaseGPUGraphicsPipeline(p->device, p->pipelines[m][d][b]);
  if (p->sampler) SDL_ReleaseGPUSampler(p->device, p->sampler);
  if (p->nearest_sampler) SDL_ReleaseGPUSampler(p->device, p->nearest_sampler);
  if (p->depth) SDL_ReleaseGPUTexture(p->device, p->depth);
  if (p->white) SDL_ReleaseGPUTexture(p->device, p->white);
  if (p->black) SDL_ReleaseGPUTexture(p->device, p->black);
  if (p->neutral_attributes) SDL_ReleaseGPUTexture(p->device, p->neutral_attributes);
  if (p->vertices) SDL_ReleaseGPUBuffer(p->device, p->vertices);
  if (p->indices) SDL_ReleaseGPUBuffer(p->device, p->indices);
  if (p->upload) SDL_ReleaseGPUTransferBuffer(p->device, p->upload);
  *p = (ArGpuDioramaDepthPass){0};
}

static bool CreateSolid(ArGpuDioramaDepthPass *p, SDL_GPUTexture **texture, uint32_t color) {
  const SDL_GPUTextureCreateInfo desc = {.type = SDL_GPU_TEXTURETYPE_2D,
      .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, .width = 1, .height = 1,
      .layer_count_or_depth = 1, .num_levels = 1, .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER};
  *texture = SDL_CreateGPUTexture(p->device, &desc);
  const SDL_GPUTransferBufferCreateInfo info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 4};
  SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(p->device, &info);
  uint32_t *data = transfer ? SDL_MapGPUTransferBuffer(p->device, transfer, false) : NULL;
  if (!*texture || !data) {
    if (data) SDL_UnmapGPUTransferBuffer(p->device, transfer);
    if (transfer) SDL_ReleaseGPUTransferBuffer(p->device, transfer);
    return false;
  }
  *data = color;
  SDL_UnmapGPUTransferBuffer(p->device, transfer);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(p->device);
  SDL_GPUCopyPass *copy = cmd ? SDL_BeginGPUCopyPass(cmd) : NULL;
  if (!copy) {
    if (cmd) SDL_CancelGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(p->device, transfer);
    return false;
  }
  const SDL_GPUTextureTransferInfo src = {.transfer_buffer = transfer};
  const SDL_GPUTextureRegion dst = {.texture = *texture, .w = 1, .h = 1, .d = 1};
  SDL_UploadToGPUTexture(copy, &src, &dst, false);
  SDL_EndGPUCopyPass(copy);
  const bool ok = SDL_SubmitGPUCommandBuffer(cmd);
  SDL_ReleaseGPUTransferBuffer(p->device, transfer);
  return ok;
}

static bool CreatePipelines(ArGpuDioramaDepthPass *p, unsigned material) {
  SDL_GPUDevice *device = p->device;
  const GpuShaderBlobs vb = material==1
      ? (GpuShaderBlobs){kDioramaDepthVertMSL,kDioramaDepthVertMSLSize,
          kDioramaDepthVertSPV,kDioramaDepthVertSPVSize,kDioramaDepthVertDXIL,kDioramaDepthVertDXILSize}
      : (GpuShaderBlobs){kSim3dDepthVertMSL, kSim3dDepthVertMSLSize,
          kSim3dDepthVertSPV, kSim3dDepthVertSPVSize, kSim3dDepthVertDXIL, kSim3dDepthVertDXILSize};
  const GpuShaderBlobs fb = material == 2 ? (GpuShaderBlobs){
      kActorDepthFragMSL, kActorDepthFragMSLSize, kActorDepthFragSPV,
      kActorDepthFragSPVSize, kActorDepthFragDXIL, kActorDepthFragDXILSize}
      : material == 1 ? (GpuShaderBlobs){
      kSceneryDepthFragMSL, kSceneryDepthFragMSLSize, kSceneryDepthFragSPV,
      kSceneryDepthFragSPVSize, kSceneryDepthFragDXIL, kSceneryDepthFragDXILSize}
      : (GpuShaderBlobs){kSim3dDepthFragMSL, kSim3dDepthFragMSLSize,
          kSim3dDepthFragSPV, kSim3dDepthFragSPVSize, kSim3dDepthFragDXIL, kSim3dDepthFragDXILSize};
  SDL_GPUShader *vs = GpuShaderBlob_Create(device, &vb, "action depth", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
  SDL_GPUShader *fs = GpuShaderBlob_CreateFragment(device, &fb, "action cutout", material == 2 ? 4 : material == 1 ? 2 : 1, material ? 1 : 0);
  const SDL_GPUVertexBufferDescription buffer = {.slot = 0, .pitch = sizeof(DioramaDepthVertex),
      .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX};
  const SDL_GPUVertexAttribute attributes[] = {
      {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
      {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16},
      {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 32},
      {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 40}};
  bool ok = vs && fs;
  for (unsigned d = 0; ok && d < kDioramaDepth_Count; ++d) for (unsigned b = 0; ok && b < 3; ++b) {
    if (d == kDioramaDepth_Write && b == 2) continue;
    const SDL_GPUColorTargetDescription color = {.format = p->color_format,
      .blend_state = {.enable_blend = b != 0,
        .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
        .dst_color_blendfactor = b == 2 ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        .color_blend_op = SDL_GPU_BLENDOP_ADD,
        .src_alpha_blendfactor = b == 2 ? SDL_GPU_BLENDFACTOR_ZERO : SDL_GPU_BLENDFACTOR_ONE,
        .dst_alpha_blendfactor = b == 2 ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        .alpha_blend_op = SDL_GPU_BLENDOP_ADD}};
    const SDL_GPUGraphicsPipelineCreateInfo info = {
      .vertex_shader = vs, .fragment_shader = fs,
      .vertex_input_state = {&buffer, 1, attributes, material==1?4:3},
      .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
      .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE,
          .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE, .enable_depth_clip = true},
      .multisample_state = {.sample_count = SDL_GPU_SAMPLECOUNT_1},
      .depth_stencil_state = {.compare_op = d == kDioramaDepth_NativeActor ? SDL_GPU_COMPAREOP_ALWAYS : SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
          .enable_depth_test = d != kDioramaDepth_Disabled,
          .enable_depth_write = d == kDioramaDepth_Write || d == kDioramaDepth_NativeActor},
      .target_info = {.color_target_descriptions = &color, .num_color_targets = 1,
          .depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT, .has_depth_stencil_target = true}};
    p->pipelines[material][d][b] = GpuShaderBlob_CreateGraphicsPipeline(device, &info, "action shared depth");
    ok = p->pipelines[material][d][b] != NULL;
  }
  if (vs) SDL_ReleaseGPUShader(device, vs);
  if (fs) SDL_ReleaseGPUShader(device, fs);
  if (!ok) for (unsigned d = 0; d < kDioramaDepth_Count; ++d) for (unsigned b = 0; b < 3; ++b) {
    if (p->pipelines[material][d][b]) SDL_ReleaseGPUGraphicsPipeline(device, p->pipelines[material][d][b]);
    p->pipelines[material][d][b] = NULL;
  }
  return ok;
}

bool ArGpuDioramaDepthPass_InitFormat(ArGpuDioramaDepthPass *p, SDL_GPUDevice *device,
    SDL_GPUTextureFormat format) {
  if (!p || !device || (format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM &&
      format != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM)) return false;
  *p = (ArGpuDioramaDepthPass){.device = device, .color_format = format};
  if (!SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
      SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
    ArGpuDioramaDepthPass_Destroy(p);
    return false;
  }
  const bool ok = CreatePipelines(p, 0);
  const SDL_GPUSamplerCreateInfo linear = {.min_filter = SDL_GPU_FILTER_LINEAR,
      .mag_filter = SDL_GPU_FILTER_LINEAR, .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
      .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE};
  SDL_GPUSamplerCreateInfo nearest = linear;
  nearest.min_filter = nearest.mag_filter = SDL_GPU_FILTER_NEAREST;
  p->sampler = SDL_CreateGPUSampler(device, &linear);
  p->nearest_sampler = SDL_CreateGPUSampler(device, &nearest);
  if (ok && p->sampler && p->nearest_sampler &&
      CreateSolid(p, &p->white, UINT32_MAX) && CreateSolid(p, &p->black, 0) &&
      CreateSolid(p, &p->neutral_attributes, SDL_Swap32LE(0xff000040u))) return true;
  ArGpuDioramaDepthPass_Destroy(p);
  return false;
}

bool ArGpuDioramaDepthPass_Init(ArGpuDioramaDepthPass *p, SDL_GPUDevice *device) {
  return ArGpuDioramaDepthPass_InitFormat(p, device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
}

static unsigned Capacity(unsigned needed, unsigned limit) {
  unsigned n = 256;
  while (n < needed && n < limit) n *= 2;
  return n < limit ? n : limit;
}

static bool Reserve(ArGpuDioramaDepthPass *p, unsigned nv, unsigned ni) {
  if (nv <= p->vertex_capacity && ni <= p->index_capacity) return true;
  const unsigned vc = Capacity(nv > p->vertex_capacity ? nv : p->vertex_capacity, kDioramaDepthMaximumVertices);
  const unsigned ic = Capacity(ni > p->index_capacity ? ni : p->index_capacity, kDioramaDepthMaximumIndices);
  const SDL_GPUBufferCreateInfo v = {.usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = vc * sizeof(DioramaDepthVertex)};
  const SDL_GPUBufferCreateInfo i = {.usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = ic * sizeof(int32_t)};
  const SDL_GPUTransferBufferCreateInfo t = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = v.size + i.size};
  SDL_GPUBuffer *vertices = SDL_CreateGPUBuffer(p->device, &v), *indices = SDL_CreateGPUBuffer(p->device, &i);
  SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(p->device, &t);
  if (!vertices || !indices || !upload) {
    if (vertices) SDL_ReleaseGPUBuffer(p->device, vertices);
    if (indices) SDL_ReleaseGPUBuffer(p->device, indices);
    if (upload) SDL_ReleaseGPUTransferBuffer(p->device, upload);
    return false;
  }
  if (p->vertices) SDL_ReleaseGPUBuffer(p->device, p->vertices);
  if (p->indices) SDL_ReleaseGPUBuffer(p->device, p->indices);
  if (p->upload) SDL_ReleaseGPUTransferBuffer(p->device, p->upload);
  p->vertices = vertices; p->indices = indices; p->upload = upload;
  p->vertex_capacity = vc; p->index_capacity = ic;
  return true;
}

static bool Finite(const void *data, size_t size) {
  for (size_t i = 0; i < size; i += sizeof(float)) {
    float v; memcpy(&v, (const uint8_t *)data + i, sizeof(v));
    if (!isfinite(v)) return false;
  }
  return true;
}

static bool EnsureDepth(ArGpuDioramaDepthPass *p, unsigned width, unsigned height) {
  if (p->depth && p->width == width && p->height == height) return true;
  const SDL_GPUTextureCreateInfo info = {.type = SDL_GPU_TEXTURETYPE_2D,
      .format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT, .width = width, .height = height,
      .layer_count_or_depth = 1, .num_levels = 1,
      .usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER};
  SDL_GPUTexture *depth = SDL_CreateGPUTexture(p->device, &info);
  if (!depth) return false;
  if (p->depth) SDL_ReleaseGPUTexture(p->device, p->depth);
  p->depth = depth; p->width = width; p->height = height;
  p->depth_initialized = false;
  return true;
}

bool ArGpuDioramaDepthPass_Prepare(ArGpuDioramaDepthPass *p, unsigned width, unsigned height,
    unsigned vertices, unsigned indices) {
  if (!p || !p->device || !p->white || !p->black || !p->neutral_attributes ||
      !width || !height || width > 16384 || height > 16384 ||
      vertices > kDioramaDepthMaximumVertices || indices > kDioramaDepthMaximumIndices) return false;
  for (unsigned m = 1; m < 3; ++m)
    if (!p->pipelines[m][kDioramaDepth_Write][0] && !CreatePipelines(p, m)) return false;
  return Reserve(p, vertices, indices) && EnsureDepth(p, width, height);
}

bool ArGpuDioramaDepthPass_EncodeMaterials(ArGpuDioramaDepthPass *p, SDL_GPUCommandBuffer *cmd,
    SDL_GPUTexture *target, unsigned width, unsigned height,
    const DioramaDepthDraw *draws, SDL_GPUTexture *const *sources,
    const ArGpuDioramaDepthMaterial *materials, unsigned count,
    SDL_GPULoadOp color_load, ArRenderColorF clear_color, bool clear_depth) {
  if (!p || !p->device || !p->white || !p->black || !p->neutral_attributes || !cmd || !target || target == p->depth ||
      !width || !height || width > 16384 || height > 16384 ||
      (color_load != SDL_GPU_LOADOP_CLEAR && color_load != SDL_GPU_LOADOP_LOAD) ||
      !Finite(&clear_color, sizeof(clear_color)) || count > kDioramaDepthMaximumDraws ||
      (count && (!draws || !sources)) ||
      (!clear_depth && (!p->depth_initialized || p->width != width || p->height != height))) return false;
  unsigned nv = 0, ni = 0;
  bool material_needed[3] = {0};
  /* Validate the complete batch before allocating or recording partial work. */
  for (unsigned i = 0; i < count; ++i) {
    const DioramaDepthDraw *d = &draws[i];
    const int blend = BlendIndex(d->blend);
    if (!d->vertices || !d->indices || !d->vertex_count || !d->index_count || d->index_count % 3 ||
        (unsigned)d->depth_mode >= kDioramaDepth_Count || blend < 0 ||
        !p->pipelines[0][d->depth_mode][blend] ||
        (d->textured && !sources[i]) || sources[i] == target || (sources[i] && sources[i] == p->depth) ||
        d->vertex_count > kDioramaDepthMaximumVertices - nv ||
        d->index_count > kDioramaDepthMaximumIndices - ni ||
        (d->clip_enabled && (d->clip.x < 0 || d->clip.y < 0 || d->clip.w <= 0 || d->clip.h <= 0 ||
          (int64_t)d->clip.x + d->clip.w > width || (int64_t)d->clip.y + d->clip.h > height))) return false;
    if (materials && materials[i].scenery) {
      const ArGpuDioramaDepthMaterial *binding = &materials[i];
      const DioramaSceneryLightEffectParams *m = binding->scenery;
      if (!d->textured || m->mode > 2 || (!binding->actor && ArRenderTexture_IsValid(m->attributes)) ||
          (!binding->actor && m->mode == 2 && (m->priority.width <= 0 || m->priority.height <= 0))) return false;
      SDL_GPUTexture *samples[] = {binding->illumination, binding->attributes, binding->enemy_illumination};
      for (unsigned sample = 0; sample < 3; ++sample)
        if (samples[sample] && (samples[sample] == target || samples[sample] == p->depth)) return false;
      if (binding->actor) {
        DioramaActorUniforms uniforms;
        DioramaActorUniforms_Init(&uniforms, m, (int)width, (int)height);
        if (!Finite(&uniforms, sizeof(uniforms))) return false;
      } else {
        if (binding->attributes || binding->enemy_illumination) return false;
        DioramaSceneryUniforms uniforms;
        DioramaSceneryUniforms_Init(&uniforms, m, (int)width, (int)height);
        if (!Finite(&uniforms, sizeof(uniforms))) return false;
      }
      material_needed[binding->actor ? 2 : 1] = true;
    } else if (materials && (materials[i].illumination || materials[i].attributes ||
        materials[i].enemy_illumination || materials[i].actor)) return false;
    for (unsigned j = 0; j < d->vertex_count; ++j)
      if (!Finite(&d->vertices[j], sizeof(d->vertices[j])) || d->vertices[j].clip[3] <= 0 ||
          /* Only the basic SIM shader reserves negative U for untextured
           * vertices. Scenery/actor materials use clamp addressing, including
           * real capture-edge and AA samples just outside the texture. */
          (d->textured && d->vertices[j].uv.x < 0 &&
            !(materials && materials[i].scenery))) return false;
    for (unsigned j = 0; j < d->index_count; ++j)
      if (d->indices[j] < 0 || (unsigned)d->indices[j] >= d->vertex_count) return false;
    nv += d->vertex_count; ni += d->index_count;
  }
  for (unsigned m = 1; m < 3; ++m)
    if (material_needed[m] && !p->pipelines[m][kDioramaDepth_Write][0] && !CreatePipelines(p, m)) return false;
  if (!Reserve(p, nv, ni) || !EnsureDepth(p, width, height)) return false;
  if (nv) {
    uint8_t *mapped = SDL_MapGPUTransferBuffer(p->device, p->upload, true);
    if (!mapped) return false;
    unsigned va = 0, ia = 0;
    const unsigned vb = nv * sizeof(DioramaDepthVertex), ib = ni * sizeof(int32_t);
    for (unsigned i = 0; i < count; ++i) {
      DioramaDepthVertex *dst = (DioramaDepthVertex *)mapped + va;
      memcpy(dst, draws[i].vertices, draws[i].vertex_count * sizeof(*dst));
      if (draws[i].depth_mode == kDioramaDepth_NativeActor)
        for (unsigned j = 0; j < draws[i].vertex_count; ++j) dst[j].clip[2] = dst[j].clip[3];
      if (!draws[i].textured) for (unsigned j = 0; j < draws[i].vertex_count; ++j) dst[j].uv.x = -1;
      memcpy(mapped + vb + ia * sizeof(int32_t), draws[i].indices, draws[i].index_count * sizeof(int32_t));
      va += draws[i].vertex_count; ia += draws[i].index_count;
    }
    SDL_UnmapGPUTransferBuffer(p->device, p->upload);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) return false;
    const SDL_GPUTransferBufferLocation vs = {.transfer_buffer = p->upload}, is = {.transfer_buffer = p->upload, .offset = vb};
    const SDL_GPUBufferRegion vd = {.buffer = p->vertices, .size = vb}, id = {.buffer = p->indices, .size = ib};
    SDL_UploadToGPUBuffer(copy, &vs, &vd, true); SDL_UploadToGPUBuffer(copy, &is, &id, true);
    SDL_EndGPUCopyPass(copy);
  }
  const SDL_GPUColorTargetInfo color = {.texture = target, .load_op = color_load, .store_op = SDL_GPU_STOREOP_STORE,
      .clear_color = {clear_color.r, clear_color.g, clear_color.b, clear_color.a}};
  const SDL_GPUDepthStencilTargetInfo depth = {.texture = p->depth, .clear_depth = 1,
      .load_op = clear_depth ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD, .store_op = SDL_GPU_STOREOP_STORE,
      .stencil_load_op = SDL_GPU_LOADOP_DONT_CARE, .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE};
  SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
  if (!pass) return false;
  if (count) {
    const SDL_GPUBufferBinding v = {.buffer = p->vertices}, i = {.buffer = p->indices};
    SDL_BindGPUVertexBuffers(pass, 0, &v, 1); SDL_BindGPUIndexBuffer(pass, &i, SDL_GPU_INDEXELEMENTSIZE_32BIT);
  }
  unsigned va = 0, ia = 0;
  for (unsigned i = 0; i < count; ++i) {
    const DioramaDepthDraw *d = &draws[i];
    const unsigned material = materials && materials[i].scenery ? (materials[i].actor ? 2 : 1) : 0;
    SDL_BindGPUGraphicsPipeline(pass, p->pipelines[material][d->depth_mode][BlendIndex(d->blend)]);
    if (material == 2) {
      DioramaActorUniforms uniforms;
      DioramaActorUniforms_Init(&uniforms, materials[i].scenery, (int)width, (int)height);
      SDL_PushGPUFragmentUniformData(cmd, 0, &uniforms, sizeof(uniforms));
      const SDL_GPUTextureSamplerBinding samples[] = {
          {materials[i].attributes ? materials[i].attributes : p->neutral_attributes, p->sampler},
          {materials[i].illumination ? materials[i].illumination : p->black, p->sampler},
          {materials[i].enemy_illumination ? materials[i].enemy_illumination : p->black, p->sampler}};
      SDL_BindGPUFragmentSamplers(pass, 1, samples, 3);
    } else if (material == 1) {
      DioramaSceneryUniforms uniforms;
      DioramaSceneryUniforms_Init(&uniforms, materials[i].scenery, (int)width, (int)height);
      SDL_PushGPUFragmentUniformData(cmd, 0, &uniforms, sizeof(uniforms));
      const SDL_GPUTextureSamplerBinding light = {
          materials[i].illumination ? materials[i].illumination : p->black, p->sampler};
      SDL_BindGPUFragmentSamplers(pass, 1, &light, 1);
    }
    const SDL_GPUTextureSamplerBinding sample = {d->textured ? sources[i] : p->white,
        d->nearest ? p->nearest_sampler : p->sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &sample, 1);
    const SDL_Rect clip = d->clip_enabled ? (SDL_Rect){d->clip.x, d->clip.y, d->clip.w, d->clip.h}
        : (SDL_Rect){0, 0, (int)width, (int)height};
    SDL_SetGPUScissor(pass, &clip);
    SDL_DrawGPUIndexedPrimitives(pass, d->index_count, 1, ia, (Sint32)va, 0);
    va += d->vertex_count; ia += d->index_count;
  }
  SDL_EndGPURenderPass(pass);
  p->depth_initialized = true;
  return true;
}

bool ArGpuDioramaDepthPass_Encode(ArGpuDioramaDepthPass *p, SDL_GPUCommandBuffer *cmd,
    SDL_GPUTexture *target, unsigned width, unsigned height,
    const DioramaDepthDraw *draws, SDL_GPUTexture *const *sources, unsigned count,
    SDL_GPULoadOp color_load, ArRenderColorF clear_color, bool clear_depth) {
  return ArGpuDioramaDepthPass_EncodeMaterials(p, cmd, target, width, height, draws,
      sources, NULL, count, color_load, clear_color, clear_depth);
}
