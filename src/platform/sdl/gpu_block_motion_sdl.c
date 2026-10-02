#include "platform/sdl/gpu_block_motion_sdl.h"
#include "platform/sdl/gpu_shader_blob.h"
#include "present/presentation_frame_generation.h"
#include "shaders/motion_block_cost_comp.h"
#include "shaders/motion_block_search_comp.h"
#include "shaders/motion_block_validate_comp.h"
#include "shaders/motion_block_vertices_comp.h"
#include "shaders/motion_block_warp_frag.h"
#include "shaders/sim3d_depth_vert.h"

enum { kBlocks = 880, kVertices = 41 * 23, kIndices = 880 * 6 };
_Static_assert(kPresentationFrameGenerationMaximumBlocks == kBlocks &&
                   kPresentationFrameGenerationBlockSize == 16 &&
                   kPresentationFrameGenerationSearchRadius == 7,
               "Update GPU block motion limits with the CPU oracle");
#define BLOBS(n) {k##n##MSL, k##n##MSLSize, k##n##SPV, k##n##SPVSize, k##n##DXIL, k##n##DXILSize}
bool ArGpuBlockMotion_Init(ArGpuBlockMotion *p, SDL_GPUDevice *device) {
  *p = (ArGpuBlockMotion){.device = device};
  const GpuShaderBlobs cost = BLOBS(MotionBlockCostComp), search = BLOBS(MotionBlockSearchComp),
                       validate = BLOBS(MotionBlockValidateComp),
                       vertices = BLOBS(MotionBlockVerticesComp), vert = BLOBS(Sim3dDepthVert),
                       frag = BLOBS(MotionBlockWarpFrag);
  p->cost = GpuShaderBlob_CreateCompute(device, &cost, 2, 0, 1, 0, 1, 64, 1);
  p->search = GpuShaderBlob_CreateCompute(device, &search, 0, 1, 1, 0, 1, 64, 1);
  p->validate = GpuShaderBlob_CreateCompute(device, &validate, 0, 1, 1, 0, 0, 64, 1);
  p->build_vertices = GpuShaderBlob_CreateCompute(device, &vertices, 0, 1, 1, 0, 1, 64, 1);
  SDL_GPUShader *vs =
      GpuShaderBlob_Create(device, &vert, "motion mesh", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
  SDL_GPUShader *fs = GpuShaderBlob_CreateFragment(device, &frag, "motion warp", 2, 1);
  const SDL_GPUVertexBufferDescription buffer = {
      .slot = 0, .pitch = 48, .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX};
  const SDL_GPUVertexAttribute attributes[] = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
                                               {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 16},
                                               {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 32}};
  const SDL_GPUColorTargetDescription color = {.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM};
  const SDL_GPUGraphicsPipelineCreateInfo pipeline = {
      .vertex_shader = vs,
      .fragment_shader = fs,
      .vertex_input_state = {.vertex_buffer_descriptions = &buffer,
                             .num_vertex_buffers = 1,
                             .vertex_attributes = attributes,
                             .num_vertex_attributes = 3},
      .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
      .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL,
                           .cull_mode = SDL_GPU_CULLMODE_NONE,
                           .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
                           .enable_depth_clip = true},
      .multisample_state = {.sample_count = SDL_GPU_SAMPLECOUNT_1},
      .target_info = {.color_target_descriptions = &color, .num_color_targets = 1}};
  if (vs && fs)
    p->warp = SDL_CreateGPUGraphicsPipeline(device, &pipeline);
  if (vs)
    SDL_ReleaseGPUShader(device, vs);
  if (fs)
    SDL_ReleaseGPUShader(device, fs);
  const SDL_GPUBufferCreateInfo directions = {.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                                                       SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                              .size = 8 * kBlocks * 16};
  SDL_GPUBufferCreateInfo costs = directions;
  costs.size = 8 * kBlocks * 226 * sizeof(uint32_t);
  p->costs = SDL_CreateGPUBuffer(device, &costs);
  SDL_GPUBufferCreateInfo motion = directions;
  motion.size = (4 * kBlocks + 4) * 16;
  const SDL_GPUBufferCreateInfo mesh = {.usage = SDL_GPU_BUFFERUSAGE_VERTEX |
                                                 SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                                        .size = kVertices * 48};
  const SDL_GPUBufferCreateInfo indices = {.usage = SDL_GPU_BUFFERUSAGE_INDEX,
                                           .size = kIndices * 2};
  p->directions = SDL_CreateGPUBuffer(device, &directions);
  p->motion = SDL_CreateGPUBuffer(device, &motion);
  p->vertices = SDL_CreateGPUBuffer(device, &mesh);
  p->indices = SDL_CreateGPUBuffer(device, &indices);
  const SDL_GPUSamplerCreateInfo sampler = {
      .min_filter = SDL_GPU_FILTER_LINEAR,
      .mag_filter = SDL_GPU_FILTER_LINEAR,
      .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
      .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE};
  p->sampler = SDL_CreateGPUSampler(device, &sampler);
  return p->cost && p->costs && p->search && p->validate && p->build_vertices && p->warp &&
         p->directions && p->motion && p->vertices && p->indices && p->sampler;
}
#undef BLOBS
void ArGpuBlockMotion_Destroy(ArGpuBlockMotion *p) {
  if (!p->device)
    return;
  SDL_ReleaseGPUComputePipeline(p->device, p->cost);
  SDL_ReleaseGPUBuffer(p->device, p->costs);
  SDL_ReleaseGPUComputePipeline(p->device, p->search);
  SDL_ReleaseGPUComputePipeline(p->device, p->validate);
  SDL_ReleaseGPUComputePipeline(p->device, p->build_vertices);
  SDL_ReleaseGPUGraphicsPipeline(p->device, p->warp);
  SDL_ReleaseGPUBuffer(p->device, p->directions);
  SDL_ReleaseGPUBuffer(p->device, p->motion);
  SDL_ReleaseGPUBuffer(p->device, p->vertices);
  SDL_ReleaseGPUBuffer(p->device, p->indices);
  SDL_ReleaseGPUSampler(p->device, p->sampler);
  if (p->output)
    SDL_ReleaseGPUTexture(p->device, p->output);
  *p = (ArGpuBlockMotion){0};
}
bool ArGpuBlockMotion_Resize(ArGpuBlockMotion *p, unsigned width, unsigned height) {
  if (!width || width > 640 || !height || height > 352)
    return false;
  if (width == p->width && height == p->height)
    return true;
  const SDL_GPUTextureCreateInfo texture = {.type = SDL_GPU_TEXTURETYPE_2D,
                                            .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                            .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                                                     SDL_GPU_TEXTUREUSAGE_SAMPLER,
                                            .width = width,
                                            .height = height * 4,
                                            .layer_count_or_depth = 1,
                                            .num_levels = 1};
  SDL_GPUTexture *output = SDL_CreateGPUTexture(p->device, &texture);
  if (!output)
    return false;
  if (p->output)
    SDL_ReleaseGPUTexture(p->device, p->output);
  p->output = output;
  // A failed index upload must not make a later retry look initialized.
  p->width = p->height = 0;
  for (unsigned i = 0; i < 4; ++i) {
    p->extents[i][0] = (float)width;
    p->extents[i][1] = (float)height;
    p->extents[i][2] = (float)(8 + i);
  }
  // The mesh uses the same two triangle order and row-major vertices as SDL.
  const unsigned bx = (width + 15) / 16, by = (height + 15) / 16;
  const SDL_GPUTransferBufferCreateInfo upload_info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                                       .size = kIndices * 2};
  SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(p->device, &upload_info);
  if (!upload)
    return false;
  uint16_t *data = SDL_MapGPUTransferBuffer(p->device, upload, false);
  if (!data) {
    SDL_ReleaseGPUTransferBuffer(p->device, upload);
    return false;
  }
  unsigned at = 0;
  for (unsigned y = 0; y < by; ++y)
    for (unsigned x = 0; x < bx; ++x) {
      uint16_t a = (uint16_t)(y * (bx + 1) + x), b = (uint16_t)(a + bx + 1);
      data[at++] = a;
      data[at++] = a + 1;
      data[at++] = b;
      data[at++] = a + 1;
      data[at++] = b + 1;
      data[at++] = b;
    }
  SDL_UnmapGPUTransferBuffer(p->device, upload);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(p->device);
  SDL_GPUCopyPass *copy = cmd ? SDL_BeginGPUCopyPass(cmd) : NULL;
  if (!copy) {
    if (cmd)
      SDL_CancelGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(p->device, upload);
    return false;
  }
  const SDL_GPUTransferBufferLocation source = {.transfer_buffer = upload};
  const SDL_GPUBufferRegion dest = {.buffer = p->indices, .size = at * 2};
  SDL_UploadToGPUBuffer(copy, &source, &dest, true);
  SDL_EndGPUCopyPass(copy);
  const bool ok = SDL_SubmitGPUCommandBuffer(cmd);
  SDL_ReleaseGPUTransferBuffer(p->device, upload);
  if (ok) {
    p->width = width;
    p->height = height;
  }
  return ok;
}
bool ArGpuBlockMotion_Analyze(ArGpuBlockMotion *p, SDL_GPUCommandBuffer *cmd,
                              SDL_GPUTexture *previous, SDL_GPUTexture *current) {
  const SDL_GPUTextureSamplerBinding pair[] = {{previous, p->sampler}, {current, p->sampler}};
  const SDL_GPUStorageBufferReadWriteBinding directions = {.buffer = p->directions, .cycle = true};
  const SDL_GPUStorageBufferReadWriteBinding motion = {.buffer = p->motion, .cycle = true};
  SDL_PushGPUComputeUniformData(cmd, 0, p->extents, sizeof(p->extents));
  const SDL_GPUStorageBufferReadWriteBinding costs = {.buffer = p->costs, .cycle = true};
  SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(cmd, NULL, 0, &costs, 1);
  if (!pass)
    return false;
  SDL_BindGPUComputePipeline(pass, p->cost);
  SDL_BindGPUComputeSamplers(pass, 0, pair, 2);
  SDL_DispatchGPUCompute(pass, kBlocks, 8, 1);
  SDL_EndGPUComputePass(pass);
  pass = SDL_BeginGPUComputePass(cmd, NULL, 0, &directions, 1);
  if (!pass)
    return false;
  SDL_BindGPUComputePipeline(pass, p->search);
  SDL_BindGPUComputeStorageBuffers(pass, 0, &p->costs, 1);
  SDL_DispatchGPUCompute(pass, 4, 2, 1);
  SDL_EndGPUComputePass(pass);
  pass = SDL_BeginGPUComputePass(cmd, NULL, 0, &motion, 1);
  if (!pass)
    return false;
  SDL_BindGPUComputePipeline(pass, p->validate);
  SDL_BindGPUComputeStorageBuffers(pass, 0, &p->directions, 1);
  SDL_DispatchGPUCompute(pass, 1, 4, 1);
  SDL_EndGPUComputePass(pass);
  return true;
}
bool ArGpuBlockMotion_Warp(ArGpuBlockMotion *p, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *previous,
                           SDL_GPUTexture *current, float phase, unsigned mask) {
  const SDL_GPUTextureSamplerBinding pair[] = {{previous, p->sampler}, {current, p->sampler}};
  bool cleared = false;
  for (unsigned plane = 0; plane < 4; ++plane) {
    if (!(mask & (1u << plane)))
      continue;
    // Live OBJ planes share the full capture including both resolve aprons.
    const unsigned bx = (p->width + 15) / 16, by = (p->height + 15) / 16;
    const float uniform[8] = {(float)p->width,
                              (float)p->height,
                              (float)plane,
                              phase,
                              (float)p->width,
                              (float)(p->height * 4),
                              0,
                              0};
    SDL_PushGPUComputeUniformData(cmd, 0, uniform, sizeof(uniform));
    const SDL_GPUStorageBufferReadWriteBinding vertices = {.buffer = p->vertices, .cycle = true};
    SDL_GPUComputePass *compute = SDL_BeginGPUComputePass(cmd, NULL, 0, &vertices, 1);
    if (!compute)
      return false;
    SDL_BindGPUComputePipeline(compute, p->build_vertices);
    SDL_BindGPUComputeStorageBuffers(compute, 0, &p->motion, 1);
    SDL_DispatchGPUCompute(compute, ((bx + 1) * (by + 1) + 63) / 64, 1, 1);
    SDL_EndGPUComputePass(compute);
    const SDL_GPUColorTargetInfo target = {.texture = p->output,
                                           .load_op =
                                               cleared ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR,
                                           .store_op = SDL_GPU_STOREOP_STORE,
                                           .cycle = !cleared,
                                           .clear_color = {0, 0, 0, 0}};
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &target, 1, NULL);
    if (!pass)
      return false;
    cleared = true;
    const SDL_GPUBufferBinding vb = {p->vertices, 0}, ib = {p->indices, 0};
    const SDL_Rect clip = {0, (int)(plane * p->height), (int)p->width, (int)p->height};
    const float sampling[4] = {(float)p->width, (float)p->height, p->extents[plane][2], 0};
    SDL_PushGPUFragmentUniformData(cmd, 0, sampling, sizeof(sampling));
    SDL_BindGPUGraphicsPipeline(pass, p->warp);
    SDL_SetGPUScissor(pass, &clip);
    SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
    SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
    SDL_BindGPUFragmentSamplers(pass, 0, pair, 2);
    SDL_DrawGPUIndexedPrimitives(pass, bx * by * 6, 1, 0, 0, 0);
    SDL_EndGPURenderPass(pass);
  }
  return true;
}
