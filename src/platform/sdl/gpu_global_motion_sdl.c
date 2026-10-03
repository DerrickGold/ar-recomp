#include "platform/sdl/gpu_global_motion_sdl.h"
#include "platform/sdl/gpu_shader_blob.h"
#include "shaders/motion_global_bound_comp.h"
#include "shaders/motion_global_cost_comp.h"
#include "shaders/motion_global_refine_comp.h"
#include "shaders/motion_global_validate_comp.h"
#include "shaders/motion_global_warp_comp.h"
#include "present/presentation_frame_generation.h"

_Static_assert(kPresentationFrameGenerationSearchRadius == 7 &&
    kPresentationFrameGenerationMaximumWidth == 640 &&
    kPresentationFrameGenerationMaximumHeight == 352,
    "Update motion compute kernels when changing the CPU search limits");

#define BLOBS(name) {k##name##MSL, k##name##MSLSize, k##name##SPV, k##name##SPVSize, k##name##DXIL, k##name##DXILSize}
bool ArGpuGlobalMotion_Init(ArGpuGlobalMotion *p, SDL_GPUDevice *device) {
  *p = (ArGpuGlobalMotion){.device = device, .plane_count = 6};
  const GpuShaderBlobs bound = BLOBS(MotionGlobalBoundComp), cost = BLOBS(MotionGlobalCostComp), refine = BLOBS(MotionGlobalRefineComp),
      validate = BLOBS(MotionGlobalValidateComp), warp = BLOBS(MotionGlobalWarpComp);
  p->bound = GpuShaderBlob_CreateCompute(device, &bound, 2, 0, 1, 0, 1, 64, 1);
  p->cost = GpuShaderBlob_CreateCompute(device, &cost, 2, 1, 1, 0, 1, 64, 1);
  p->refine = GpuShaderBlob_CreateCompute(device, &refine, 2, 1, 1, 0, 1, 64, 1);
  p->validate = GpuShaderBlob_CreateCompute(device, &validate, 0, 1, 1, 0, 0, 64, 1);
  p->warp = GpuShaderBlob_CreateCompute(device, &warp, 2, 1, 0, 1, 1, 8, 8);
  const SDL_GPUBufferCreateInfo cost_buffer = {
    .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
    .size = 16 * 225 * sizeof(uint32_t)};
  SDL_GPUBufferCreateInfo buffer = cost_buffer;
  p->costs = SDL_CreateGPUBuffer(device, &buffer);
  buffer.size = 16 * 9 * sizeof(uint32_t); p->bounds = SDL_CreateGPUBuffer(device, &buffer);
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
#undef BLOBS
void ArGpuGlobalMotion_Destroy(ArGpuGlobalMotion *p) {
  SDL_ReleaseGPUComputePipeline(p->device, p->bound); SDL_ReleaseGPUBuffer(p->device, p->bounds);
  SDL_ReleaseGPUComputePipeline(p->device, p->cost); SDL_ReleaseGPUComputePipeline(p->device, p->refine);
  SDL_ReleaseGPUComputePipeline(p->device, p->validate); SDL_ReleaseGPUComputePipeline(p->device, p->warp);
  SDL_ReleaseGPUBuffer(p->device, p->costs); SDL_ReleaseGPUBuffer(p->device, p->directions);
  SDL_ReleaseGPUBuffer(p->device, p->motion); SDL_ReleaseGPUSampler(p->device, p->sampler);
  if (p->output) SDL_ReleaseGPUTexture(p->device, p->output);
  *p = (ArGpuGlobalMotion){0};
}
bool ArGpuGlobalMotion_Resize(ArGpuGlobalMotion *p, unsigned width, unsigned height) {
  if (!width || width > 640 || !height || height > 352 || !p->plane_count || p->plane_count > 8) return false;
  if (width == p->width && height == p->height && p->plane_count == p->allocated_plane_count) return true;
  const SDL_GPUTextureCreateInfo info = {.type = SDL_GPU_TEXTURETYPE_2D,
    .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
    .usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER,
    .width = width, .height = height * p->plane_count, .layer_count_or_depth = 1, .num_levels = 1};
  if (!SDL_GPUTextureSupportsFormat(p->device, info.format, info.type, info.usage)) return false;
  SDL_GPUTexture *output = SDL_CreateGPUTexture(p->device, &info);
  if (!output) return false;
  if (p->output) SDL_ReleaseGPUTexture(p->device, p->output);
  p->output = output; p->width = width; p->height = height;
  p->allocated_plane_count = p->plane_count;
  for (unsigned i = 0; i < 8; ++i) {
    p->extents[i][2] = (float)height;
    p->extents[i][0] = (float)width;
    p->extents[i][1] = i < p->plane_count ? 1.0f : 0.0f;
  }
  return true;
}
static void BindPair(ArGpuGlobalMotion *p, SDL_GPUComputePass *pass, SDL_GPUTexture *previous, SDL_GPUTexture *current) {
  const SDL_GPUTextureSamplerBinding bindings[] = {{previous, p->sampler}, {current, p->sampler}};
  SDL_BindGPUComputeSamplers(pass, 0, bindings, 2);
}
bool ArGpuGlobalMotion_Analyze(ArGpuGlobalMotion *p, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *previous, SDL_GPUTexture *current) {
  SDL_PushGPUComputeUniformData(cmd, 0, p->extents, sizeof(p->extents));
  const SDL_GPUStorageBufferReadWriteBinding bounds = {.buffer = p->bounds, .cycle = true};
  const SDL_GPUStorageBufferReadWriteBinding outputs[] = {{.buffer = p->costs, .cycle = true},
      {.buffer = p->directions, .cycle = true}, {.buffer = p->motion, .cycle = true}};
  SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(cmd, NULL, 0, &bounds, 1);
  if (!pass) return false;
  SDL_BindGPUComputePipeline(pass, p->bound); BindPair(p, pass, previous, current);
  SDL_DispatchGPUCompute(pass, 9, 16, 1); SDL_EndGPUComputePass(pass);
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
  if (!p->output || !(phase >= 0.0f && phase <= 1.0f)) return false;
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
