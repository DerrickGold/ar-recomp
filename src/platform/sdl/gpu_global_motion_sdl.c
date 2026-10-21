#include "platform/sdl/gpu_global_motion_sdl.h"
#include "platform/sdl/gpu_prototype_policy.h"
#include "platform/sdl/gpu_shader_blob.h"
#include "shaders/motion_global_bound_comp.h"
#include "shaders/motion_global_cost_comp.h"
#include "shaders/motion_global_refine_comp.h"
#include "shaders/motion_global_validate_comp.h"
#include "shaders/motion_global_warp_comp.h"
#include "shaders/motion_global_layers_comp.h"
#include "present/presentation_frame_generation.h"

_Static_assert(kPresentationFrameGenerationSearchRadius == 7 &&
    kPresentationFrameGenerationMaximumWidth == 640 &&
    kPresentationFrameGenerationMaximumHeight == 352,
    "Update motion compute kernels when changing the CPU search limits");

#define BLOBS(name) {k##name##MSL, k##name##MSLSize, k##name##SPV, k##name##SPVSize, k##name##DXIL, k##name##DXILSize}
bool ArGpuGlobalMotion_Init(ArGpuGlobalMotion *p, SDL_GPUDevice *device) {
  *p = (ArGpuGlobalMotion){.device = device, .plane_count = 6};
  p->camera_bound = ArGpuPrototype_Enabled(device, "AR_GPU_CAMERA_MOTION_BOUND");
  if (p->camera_bound) SDL_Log("[gpu-global-motion] camera-guided bound enabled");
  const GpuShaderBlobs bound = BLOBS(MotionGlobalBoundComp), cost = BLOBS(MotionGlobalCostComp), refine = BLOBS(MotionGlobalRefineComp),
      validate = BLOBS(MotionGlobalValidateComp), warp = BLOBS(MotionGlobalWarpComp);
  p->bound = GpuShaderBlob_CreateCompute(device, &bound, "global motion bound",
      2, 0, 1, 0, 1, 64, 1);
  p->cost = GpuShaderBlob_CreateCompute(device, &cost, "global motion cost", 2, 1, 1, 0, 1, 64, 1);
  p->refine = GpuShaderBlob_CreateCompute(device, &refine, "global motion refine",
      2, 1, 1, 0, 1, 64, 1);
  p->validate = GpuShaderBlob_CreateCompute(device, &validate, "global motion validate",
      0, 1, 1, 0, 0, 64, 1);
  p->warp = GpuShaderBlob_CreateCompute(device, &warp, "global motion warp", 2, 1, 0, 1, 1, 8, 8);
  const SDL_GPUBufferCreateInfo cost_buffer = {
    .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
    .size = 16 * 225 * sizeof(uint32_t)};
  SDL_GPUBufferCreateInfo buffer = cost_buffer;
  p->costs = SDL_CreateGPUBuffer(device, &buffer);
  buffer.size = 16 * 10 * sizeof(uint32_t); p->bounds = SDL_CreateGPUBuffer(device, &buffer);
  buffer.size = 16 * 4 * sizeof(int32_t); p->directions = SDL_CreateGPUBuffer(device, &buffer);
  buffer.size = 8 * sizeof(ArGpuGlobalMotionResult);
  buffer.usage |= SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
  p->motion = SDL_CreateGPUBuffer(device, &buffer);
  const SDL_GPUSamplerCreateInfo sampler = {.min_filter = SDL_GPU_FILTER_LINEAR,
    .mag_filter = SDL_GPU_FILTER_LINEAR, .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
    .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
    .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
    .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE};
  p->sampler = SDL_CreateGPUSampler(device, &sampler);
  return p->bound && p->cost && p->refine && p->validate && p->warp && p->bounds && p->costs && p->directions && p->motion && p->sampler;
}
bool ArGpuGlobalMotion_PrepareLayerWarp(ArGpuGlobalMotion *p) {
  if (!p->warp_layers) {
    const GpuShaderBlobs shader = BLOBS(MotionGlobalLayersComp);
    p->warp_layers = GpuShaderBlob_CreateCompute(p->device, &shader,
        "global motion direct layers", 2, 1, 0, 8, 1, 8, 8);
  }
  return p->warp_layers != NULL;
}
#undef BLOBS
void ArGpuGlobalMotion_Destroy(ArGpuGlobalMotion *p) {
  SDL_ReleaseGPUComputePipeline(p->device, p->bound); SDL_ReleaseGPUBuffer(p->device, p->bounds);
  SDL_ReleaseGPUComputePipeline(p->device, p->cost); SDL_ReleaseGPUComputePipeline(p->device, p->refine);
  SDL_ReleaseGPUComputePipeline(p->device, p->validate); SDL_ReleaseGPUComputePipeline(p->device, p->warp);
  if (p->warp_layers) SDL_ReleaseGPUComputePipeline(p->device, p->warp_layers);
  SDL_ReleaseGPUBuffer(p->device, p->costs); SDL_ReleaseGPUBuffer(p->device, p->directions);
  SDL_ReleaseGPUBuffer(p->device, p->motion); SDL_ReleaseGPUSampler(p->device, p->sampler);
  if (p->output) SDL_ReleaseGPUTexture(p->device, p->output);
  *p = (ArGpuGlobalMotion){0};
}
bool ArGpuGlobalMotion_Resize(ArGpuGlobalMotion *p, unsigned width, unsigned height) {
  if (!width || width > 640 || !height || height > 352 || !p->plane_count || p->plane_count > 8) return false;
  if (width == p->width && height == p->height && p->plane_count == p->configured_plane_count) return true;
  if (p->output) SDL_ReleaseGPUTexture(p->device, p->output);
  p->output = NULL; p->width = width; p->height = height;
  p->configured_plane_count = p->plane_count;
  for (unsigned i = 0; i < 8; ++i) {
    p->bound_hint[i] = 0;
    p->extents[i][2] = (float)height;
    p->extents[i][0] = (float)width;
    p->extents[i][1] = i < p->plane_count ? 1.0f : 0.0f;
  }
  return true;
}
void ArGpuGlobalMotion_SetHint(ArGpuGlobalMotion *p, unsigned plane, int dx, int dy) {
  if (plane >= 8) return;
  dx = SDL_clamp(dx, -7, 7);
  dy = SDL_clamp(dy, -7, 7);
  p->bound_hint[plane] = dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1
      ? 0u : (unsigned)(1 + (dy + 7) * 15 + dx + 7);
}
bool ArGpuGlobalMotion_EnsureOutput(ArGpuGlobalMotion *p) {
  if (p->output) return true;
  if (!p->width || !p->height || !p->configured_plane_count) return false;
  const SDL_GPUTextureCreateInfo info = {.type = SDL_GPU_TEXTURETYPE_2D,
    .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
    .usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER,
    .width = p->width, .height = p->height * p->configured_plane_count,
    .layer_count_or_depth = 1, .num_levels = 1};
  if (!SDL_GPUTextureSupportsFormat(p->device, info.format, info.type, info.usage)) return false;
  p->output = SDL_CreateGPUTexture(p->device, &info);
  return p->output != NULL;
}
static void BindPair(ArGpuGlobalMotion *p, SDL_GPUComputePass *pass, SDL_GPUTexture *previous, SDL_GPUTexture *current) {
  const SDL_GPUTextureSamplerBinding bindings[] = {{previous, p->sampler}, {current, p->sampler}};
  SDL_BindGPUComputeSamplers(pass, 0, bindings, 2);
}
bool ArGpuGlobalMotion_Analyze(ArGpuGlobalMotion *p, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *previous, SDL_GPUTexture *current) {
  float extents[8][4];
  SDL_memcpy(extents, p->extents, sizeof(extents));
  bool hinted = false;
  for (unsigned i = 0; i < 8; ++i) {
    extents[i][3] = p->camera_bound && extents[i][1] != 0.0f
        ? (float)p->bound_hint[i] : 0.0f;
    hinted |= extents[i][3] != 0.0f;
  }
  SDL_PushGPUComputeUniformData(cmd, 0, extents, sizeof(extents));
  const SDL_GPUStorageBufferReadWriteBinding bounds = {.buffer = p->bounds, .cycle = true};
  const SDL_GPUStorageBufferReadWriteBinding outputs[] = {{.buffer = p->costs, .cycle = true},
      {.buffer = p->directions, .cycle = true}, {.buffer = p->motion, .cycle = true}};
  SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(cmd, NULL, 0, &bounds, 1);
  if (!pass) return false;
  SDL_BindGPUComputePipeline(pass, p->bound); BindPair(p, pass, previous, current);
  SDL_DispatchGPUCompute(pass, hinted ? 10 : 9, 16, 1); SDL_EndGPUComputePass(pass);
  pass = SDL_BeginGPUComputePass(cmd, NULL, 0, &outputs[0], 1);
  if (!pass) return false;
  SDL_BindGPUComputePipeline(pass, p->cost); BindPair(p, pass, previous, current);
  SDL_BindGPUComputeStorageBuffers(pass, 0, &p->bounds, 1);
  SDL_DispatchGPUCompute(pass, 225, 16, 1); SDL_EndGPUComputePass(pass);
  // Separate passes are required for dependencies under SDL's compute contract.
  pass = SDL_BeginGPUComputePass(cmd, NULL, 0, &outputs[1], 1);
  if (!pass) return false;
  SDL_BindGPUComputePipeline(pass, p->refine); BindPair(p, pass, previous, current);
  SDL_BindGPUComputeStorageBuffers(pass, 0, &p->costs, 1);
  SDL_DispatchGPUCompute(pass, 16, 1, 1); SDL_EndGPUComputePass(pass);
  pass = SDL_BeginGPUComputePass(cmd, NULL, 0, &outputs[2], 1);
  if (!pass) return false;
  SDL_BindGPUComputePipeline(pass, p->validate);
  SDL_BindGPUComputeStorageBuffers(pass, 0, &p->directions, 1);
  SDL_DispatchGPUCompute(pass, 1, 1, 1); SDL_EndGPUComputePass(pass);
  return true;
}
bool ArGpuGlobalMotion_Warp(ArGpuGlobalMotion *p, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *previous, SDL_GPUTexture *current, float phase) {
  if (!(phase >= 0.0f && phase <= 1.0f) || !ArGpuGlobalMotion_EnsureOutput(p)) return false;
  const SDL_GPUStorageTextureReadWriteBinding output = {.texture = p->output, .cycle = true};
  struct { float phase[4], extents[8][4]; } uniform = {.phase = {phase, (float)(p->input_plane_count ? p->input_plane_count : p->plane_count), 0, 0}};
  SDL_memcpy(uniform.extents, p->extents, sizeof(p->extents));
  SDL_PushGPUComputeUniformData(cmd, 0, &uniform, sizeof(uniform));
  SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(cmd, &output, 1, NULL, 0);
  if (!pass) return false;
  SDL_BindGPUComputePipeline(pass, p->warp); BindPair(p, pass, previous, current);
  SDL_BindGPUComputeStorageBuffers(pass, 0, &p->motion, 1);
  SDL_DispatchGPUCompute(pass, (p->width + 7) / 8, (p->height + 7) / 8, p->plane_count);
  SDL_EndGPUComputePass(pass); return true;
}

bool ArGpuGlobalMotion_WarpLayers(ArGpuGlobalMotion *p, SDL_GPUCommandBuffer *cmd,
    SDL_GPUTexture *previous, SDL_GPUTexture *current, float phase,
    const ArGpuGlobalMotionTarget targets[8], unsigned mask) {
  if (!p->warp_layers || !targets || p->plane_count != 8 || mask > 255 ||
      !(phase >= 0.0f && phase <= 1.0f)) return false;
  SDL_GPUStorageTextureReadWriteBinding outputs[8] = {0};
  struct { float phase[4], extents[8][4]; } uniform = {
      .phase = {phase, (float)(p->input_plane_count ? p->input_plane_count : p->plane_count)}};
  SDL_memcpy(uniform.extents, p->extents, sizeof(p->extents));
  /* Validate the complete set before recording anything. */
  for (unsigned i = 0; i < 8; ++i) {
    const ArGpuGlobalMotionTarget *t = &targets[i];
    if (!t->texture || t->texture == previous || t->texture == current) return false;
    for (unsigned j = 0; j < i; ++j)
      if (targets[j].texture == t->texture) return false;
    if ((mask & (1u << i)) && (t->x > t->width ||
        p->extents[i][0] > t->width - t->x || p->extents[i][2] > t->height)) return false;
    outputs[i].texture = t->texture;
    uniform.extents[i][1] = (mask & (1u << i)) ? 1.0f : 0.0f;
    uniform.extents[i][3] = (float)t->x;
  }
  for (unsigned i = 0; i < 8; ++i) {
    if (!(mask & (1u << i)) || !targets[i].clear_padding) continue;
    const SDL_GPUColorTargetInfo clear = {.texture = targets[i].texture,
        .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE};
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &clear, 1, NULL);
    if (!pass) return false;
    SDL_EndGPURenderPass(pass);
  }
  SDL_PushGPUComputeUniformData(cmd, 0, &uniform, sizeof(uniform));
  SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(cmd, outputs, 8, NULL, 0);
  if (!pass) return false;
  SDL_BindGPUComputePipeline(pass, p->warp_layers);
  BindPair(p, pass, previous, current);
  SDL_BindGPUComputeStorageBuffers(pass, 0, &p->motion, 1);
  SDL_DispatchGPUCompute(pass, (p->width + 7) / 8, (p->height + 7) / 8, 8);
  SDL_EndGPUComputePass(pass);
  return true;
}
