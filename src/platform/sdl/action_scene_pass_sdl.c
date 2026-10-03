#include "action_scene_pass_sdl.h"
#include "gpu_shader_blob.h"
#include "shaders/action_scene_vert.h"
#include "shaders/action_scene_frag.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

typedef struct ProjectionUniform {
  float matrix[16], viewport[4], target_capture[4], plane_uv[4], shape[4];
  float curve[4], fold[4], fold_end[4], sky_rect[4], sky_motion[4], offset[4];
} ProjectionUniform;
typedef struct MaterialUniform {
  float texture_info[4], atlas[4], clip[4];
} MaterialUniform;
_Static_assert(sizeof(ProjectionUniform) == 224 &&
    offsetof(ProjectionUniform, sky_motion) == 192, "action projection std140");
_Static_assert(sizeof(MaterialUniform) == 48, "action material std140");
_Static_assert(sizeof(DioramaSceneVertex) == 32 &&
    offsetof(DioramaSceneVertex, color) == 8 &&
    offsetof(DioramaSceneVertex, uv) == 24, "packed source-space vertex");

static int BlendIndex(ArRenderBlendMode blend) {
  switch (blend) {
    case kArRenderBlendMode_Opaque: return 0;
    case kArRenderBlendMode_Alpha: return 1;
    case kArRenderBlendMode_Add: return 2;
    default: return -1;
  }
}

void ArGpuActionScenePass_Destroy(ArGpuActionScenePass *p) {
  if (!p || !p->device) return;
  for (unsigned i = 0; i < 3; ++i)
    if (p->pipelines[i]) SDL_ReleaseGPUGraphicsPipeline(p->device, p->pipelines[i]);
  if (p->sampler) SDL_ReleaseGPUSampler(p->device, p->sampler);
  if (p->nearest_sampler) SDL_ReleaseGPUSampler(p->device, p->nearest_sampler);
  if (p->vertices) SDL_ReleaseGPUBuffer(p->device, p->vertices);
  if (p->indices) SDL_ReleaseGPUBuffer(p->device, p->indices);
  if (p->upload) SDL_ReleaseGPUTransferBuffer(p->device, p->upload);
  *p = (ArGpuActionScenePass){0};
}

bool ArGpuActionScenePass_Init(ArGpuActionScenePass *p, SDL_GPUDevice *device) {
  if (!p || !device) return false;
  *p = (ArGpuActionScenePass){.device = device};
  const GpuShaderBlobs vert = {kActionSceneVertMSL, kActionSceneVertMSLSize,
      kActionSceneVertSPV, kActionSceneVertSPVSize, kActionSceneVertDXIL, kActionSceneVertDXILSize};
  const GpuShaderBlobs frag = {kActionSceneFragMSL, kActionSceneFragMSLSize,
      kActionSceneFragSPV, kActionSceneFragSPVSize, kActionSceneFragDXIL, kActionSceneFragDXILSize};
  SDL_GPUShader *vs = GpuShaderBlob_CreateWithStorage(device, &vert,
      "action source projection", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 1);
  SDL_GPUShader *fs = GpuShaderBlob_CreateFragment(device, &frag, "action scene", 1, 1);
  const SDL_GPUVertexBufferDescription vb = {.slot = 0, .pitch = sizeof(DioramaSceneVertex),
      .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX};
  const SDL_GPUVertexAttribute attributes[] = {
      {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0},
      {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 8},
      {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 24}};
  for (unsigned i = 0; vs && fs && i < 3; ++i) {
    const SDL_GPUColorTargetDescription color = {
      .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
      .blend_state = {.enable_blend = i != 0,
        .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
        .dst_color_blendfactor = i == 2 ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        .color_blend_op = SDL_GPU_BLENDOP_ADD,
        .src_alpha_blendfactor = i == 2 ? SDL_GPU_BLENDFACTOR_ZERO : SDL_GPU_BLENDFACTOR_ONE,
        .dst_alpha_blendfactor = i == 2 ? SDL_GPU_BLENDFACTOR_ONE : SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        .alpha_blend_op = SDL_GPU_BLENDOP_ADD}};
    const SDL_GPUGraphicsPipelineCreateInfo info = {
      .vertex_shader = vs, .fragment_shader = fs,
      .vertex_input_state = {.vertex_buffer_descriptions = &vb, .num_vertex_buffers = 1,
          .vertex_attributes = attributes, .num_vertex_attributes = 3},
      .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
      .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE,
          .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE, .enable_depth_clip = true},
      .multisample_state = {.sample_count = SDL_GPU_SAMPLECOUNT_1},
      .target_info = {.color_target_descriptions = &color, .num_color_targets = 1}};
    p->pipelines[i] = SDL_CreateGPUGraphicsPipeline(device, &info);
  }
  if (vs) SDL_ReleaseGPUShader(device, vs);
  if (fs) SDL_ReleaseGPUShader(device, fs);
  SDL_GPUSamplerCreateInfo sampler = {.min_filter = SDL_GPU_FILTER_LINEAR,
      .mag_filter = SDL_GPU_FILTER_LINEAR, .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
      .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE};
  p->sampler = SDL_CreateGPUSampler(device, &sampler);
  sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
  p->nearest_sampler = SDL_CreateGPUSampler(device, &sampler);
  if (p->pipelines[0] && p->pipelines[1] && p->pipelines[2] && p->sampler && p->nearest_sampler) return true;
  ArGpuActionScenePass_Destroy(p);
  return false;
}

static bool FiniteUniform(const void *data, unsigned bytes) {
  for (unsigned at = 0; at < bytes; at += sizeof(float)) {
    float value;
    memcpy(&value, (const unsigned char *)data + at, sizeof(value));
    if (!isfinite(value)) return false;
  }
  return true;
}

static bool Uniforms(const DioramaSceneDraw *d, const ArGpuActionSceneTexture *t,
    unsigned width, unsigned height, float phase, ProjectionUniform *u, MaterialUniform *m) {
  const DioramaProjection *v = d->view;
  const DioramaPlaneProjection *p = &d->plane;
  if (!v || !v->valid || v->output_width <= 0 || v->output_height <= 0 ||
      v->texture_width <= 0 || v->texture_height <= 0 ||
      !(v->aspect_x > 0) || !(v->height_scale > 0) ||
      d->motion_slot < -1 || d->motion_slot >= 8 || BlendIndex(d->blend) < 0 ||
      (d->motion_slot >= 0 && (p->capture_offset.x != 0 || p->capture_offset.y != 0)) ||
      !t->texture || !t->width || !t->height ||
      !(t->source.w >= 1) || !(t->source.h >= 1) ||
      t->source.x < 0 || t->source.y < 0 ||
      t->source.x + t->source.w > t->width || t->source.y + t->source.h > t->height)
    return false;
  if (d->use_skybox) {
    const DioramaSkyboxBandProjection *s = &d->skybox;
    if (!(s->x1 > s->x0) || !(s->y1 > s->y0) ||
        !(s->output_y1 > s->output_y0) || s->output_y0 < 0 || s->output_y1 > 1)
      return false;
  } else if (!p->valid || !(p->u1 > p->u0) || !(p->v1 > p->v0) ||
      (p->overflow_valid && !(p->overflow_height > 0))) return false;
  if (d->clip_enabled && (!(d->clip.w > 0) || !(d->clip.h > 0))) return false;
  *u = (ProjectionUniform){
    .viewport = {(float)v->output_x, (float)v->output_y, (float)v->output_width, (float)v->output_height},
    .target_capture = {(float)width, (float)height, (float)v->texture_width, (float)v->texture_height},
    .plane_uv = {p->u0, p->v0, p->u1, p->v1},
    .shape = {v->aspect_x, v->height_scale, p->z_world, p->rake},
    .curve = {p->bow, p->world_y_offset, (float)v->texture_x_origin, d->use_skybox ? 1 : 0},
    .fold = {p->overflow_valid ? 1 : 0, p->overflow_fold_t, p->overflow_height, p->overflow_overlap_t},
    .fold_end = {p->overflow_handoff_z, p->overflow_front_z, p->overflow_front_drop, 0},
    .sky_rect = {d->skybox.x0, d->skybox.y0, d->skybox.x1, d->skybox.y1},
    .sky_motion = {d->skybox.output_y0, d->skybox.output_y1, phase, (float)d->motion_slot},
    .offset = {p->capture_offset.x, p->capture_offset.y, 0, 0}};
  memcpy(u->matrix, v->matrix, sizeof(u->matrix));
  *m = (MaterialUniform){
    .texture_info = {(float)t->width, (float)t->height, d->textured ? 1 : 0, d->clip_enabled ? 1 : 0},
    .atlas = {t->source.x, t->source.y, t->source.w, t->source.h},
    .clip = {d->clip.x, d->clip.y, d->clip.x + d->clip.w, d->clip.y + d->clip.h}};
  return FiniteUniform(u, sizeof(*u)) && FiniteUniform(m, sizeof(*m));
}

static unsigned Capacity(unsigned requested, unsigned limit) {
  unsigned size = 256;
  while (size < requested && size < limit) size *= 2;
  return size < limit ? size : limit;
}

static bool Reserve(ArGpuActionScenePass *p, unsigned vertices, unsigned indices) {
  if (vertices <= p->vertex_capacity && indices <= p->index_capacity) return true;
  unsigned vc = Capacity(vertices > p->vertex_capacity ? vertices : p->vertex_capacity,
      kDioramaSceneMaximumVertices);
  unsigned ic = Capacity(indices > p->index_capacity ? indices : p->index_capacity,
      kDioramaSceneMaximumIndices);
  const SDL_GPUBufferCreateInfo v = {.usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = vc * sizeof(DioramaSceneVertex)};
  const SDL_GPUBufferCreateInfo i = {.usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = ic * sizeof(int32_t)};
  const SDL_GPUTransferBufferCreateInfo upload = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = v.size + i.size};
  SDL_GPUBuffer *vb = SDL_CreateGPUBuffer(p->device, &v), *ib = SDL_CreateGPUBuffer(p->device, &i);
  SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(p->device, &upload);
  if (!vb || !ib || !transfer) {
    if (vb) SDL_ReleaseGPUBuffer(p->device, vb);
    if (ib) SDL_ReleaseGPUBuffer(p->device, ib);
    if (transfer) SDL_ReleaseGPUTransferBuffer(p->device, transfer);
    return false;
  }
  if (p->vertices) SDL_ReleaseGPUBuffer(p->device, p->vertices);
  if (p->indices) SDL_ReleaseGPUBuffer(p->device, p->indices);
  if (p->upload) SDL_ReleaseGPUTransferBuffer(p->device, p->upload);
  p->vertices = vb; p->indices = ib; p->upload = transfer;
  p->vertex_capacity = vc; p->index_capacity = ic;
  return true;
}

bool ArGpuActionScenePass_Encode(ArGpuActionScenePass *p, SDL_GPUCommandBuffer *cmd,
    SDL_GPUTexture *target, unsigned width, unsigned height, SDL_GPUBuffer *motion, float phase,
    const DioramaSceneDraw *draws, const ArGpuActionSceneTexture *textures, unsigned count,
    SDL_GPULoadOp load, ArRenderColorF clear) {
  if (!p || !p->device || !p->sampler || !cmd || !target || !motion ||
      !width || !height || width > 16384 || height > 16384 ||
      !(phase >= 0 && phase <= 1) || !draws || !textures || !count || count > kDioramaSceneMaximumDraws ||
      (load != SDL_GPU_LOADOP_CLEAR && load != SDL_GPU_LOADOP_LOAD) || !FiniteUniform(&clear, sizeof(clear)))
    return false;
  ProjectionUniform projection[kDioramaSceneMaximumDraws];
  MaterialUniform material[kDioramaSceneMaximumDraws];
  unsigned nv = 0, ni = 0;
  /* Preflight the complete pass before allocation or command recording. */
  for (unsigned i = 0; i < count; ++i) {
    const DioramaSceneDraw *d = &draws[i];
    if (!d->vertices || !d->indices || !d->vertex_count || !d->index_count || d->index_count % 3 ||
        textures[i].texture == target ||
        d->vertex_count > kDioramaSceneMaximumVertices - nv || d->index_count > kDioramaSceneMaximumIndices - ni ||
        !Uniforms(d, &textures[i], width, height, phase, &projection[i], &material[i])) return false;
    for (unsigned j = 0; j < d->index_count; ++j)
      if (d->indices[j] < 0 || (unsigned)d->indices[j] >= d->vertex_count) return false;
    nv += d->vertex_count; ni += d->index_count;
  }
  if (!Reserve(p, nv, ni)) return false;
  const unsigned vertex_bytes = nv * sizeof(DioramaSceneVertex), index_bytes = ni * sizeof(int32_t);
  unsigned char *mapped = SDL_MapGPUTransferBuffer(p->device, p->upload, true);
  if (!mapped) return false;
  unsigned va = 0, ia = 0;
  for (unsigned i = 0; i < count; ++i) {
    memcpy(mapped + va * sizeof(DioramaSceneVertex), draws[i].vertices, draws[i].vertex_count * sizeof(DioramaSceneVertex));
    memcpy(mapped + vertex_bytes + ia * sizeof(int32_t), draws[i].indices, draws[i].index_count * sizeof(int32_t));
    va += draws[i].vertex_count; ia += draws[i].index_count;
  }
  SDL_UnmapGPUTransferBuffer(p->device, p->upload);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  if (!copy) return false;
  const SDL_GPUTransferBufferLocation vsrc = {.transfer_buffer = p->upload};
  const SDL_GPUTransferBufferLocation isrc = {.transfer_buffer = p->upload, .offset = vertex_bytes};
  const SDL_GPUBufferRegion vdst = {.buffer = p->vertices, .size = vertex_bytes};
  const SDL_GPUBufferRegion idst = {.buffer = p->indices, .size = index_bytes};
  SDL_UploadToGPUBuffer(copy, &vsrc, &vdst, true);
  SDL_UploadToGPUBuffer(copy, &isrc, &idst, true);
  SDL_EndGPUCopyPass(copy);
  const SDL_GPUColorTargetInfo output = {.texture = target, .load_op = load, .store_op = SDL_GPU_STOREOP_STORE,
      .clear_color = {clear.r, clear.g, clear.b, clear.a}};
  SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &output, 1, NULL);
  if (!pass) return false;
  const SDL_GPUBufferBinding vb = {.buffer = p->vertices}, ib = {.buffer = p->indices};
  SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
  SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
  SDL_BindGPUVertexStorageBuffers(pass, 0, &motion, 1);
  va = ia = 0;
  int previous_blend = -1;
  for (unsigned i = 0; i < count; ++i) {
    const int blend = BlendIndex(draws[i].blend);
    if (blend != previous_blend) SDL_BindGPUGraphicsPipeline(pass, p->pipelines[blend]);
    previous_blend = blend;
    const SDL_GPUTextureSamplerBinding sample = {textures[i].texture,
        draws[i].nearest ? p->nearest_sampler : p->sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &sample, 1);
    SDL_PushGPUVertexUniformData(cmd, 0, &projection[i], sizeof(projection[i]));
    SDL_PushGPUFragmentUniformData(cmd, 0, &material[i], sizeof(material[i]));
    SDL_DrawGPUIndexedPrimitives(pass, draws[i].index_count, 1, ia, (Sint32)va, 0);
    va += draws[i].vertex_count; ia += draws[i].index_count;
  }
  SDL_EndGPURenderPass(pass);
  return true;
}
