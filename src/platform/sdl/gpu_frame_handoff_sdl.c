#include "gpu_frame_handoff_sdl.h"

static bool Inside(ArRenderRectI r, unsigned w, unsigned h) {
  return w > 0 && w <= 16384 && h > 0 && h <= 16384 && r.x >= 0 && r.y >= 0 &&
      r.w > 0 && r.h > 0 && (unsigned)r.x < w && (unsigned)r.y < h &&
      (unsigned)r.w <= w - (unsigned)r.x && (unsigned)r.h <= h - (unsigned)r.y;
}

bool ArGpuFrameHandoff_EncodePack(ArGpuActionScenePass *pass, SDL_GPUCommandBuffer *cmd,
    SDL_GPUTexture *atlas, unsigned width, unsigned height, SDL_GPUBuffer *motion,
    const ArGpuFramePack *planes, unsigned count) {
  if (!planes || !count || count > kArGpuFrameHandoffMaximumPlanes) return false;
  DioramaSceneVertex vertices[kArGpuFrameHandoffMaximumPlanes][4];
  DioramaSceneDraw draws[kArGpuFrameHandoffMaximumPlanes];
  ArGpuActionSceneTexture textures[kArGpuFrameHandoffMaximumPlanes];
  const int32_t indices[] = {0, 1, 2, 0, 2, 3};
  const DioramaProjection view = {.valid = true, .output_width = (int)width,
      .output_height = (int)height, .texture_width = (int)width, .texture_height = (int)height,
      .aspect_x = 1, .height_scale = 1};
  for (unsigned i = 0; i < count; ++i) {
    const ArRenderRectI r = planes[i].destination;
    if (!Inside(r, width, height) || planes[i].source.source.w != r.w ||
        planes[i].source.source.h != r.h) return false;
    for (unsigned j = 0; j < 4; ++j) {
      const float x = j == 1 || j == 2 ? 1 : 0, y = j >= 2 ? 1 : 0;
      vertices[i][j] = (DioramaSceneVertex){
          {(float)r.x + x * r.w, (float)r.y + y * r.h}, {1,1,1,1}, {x,y}};
    }
    textures[i] = planes[i].source;
    draws[i] = (DioramaSceneDraw){.vertices = vertices[i], .indices = indices,
        .vertex_count = 4, .index_count = 6, .view = &view, .use_skybox = true,
        .skybox = {0, 0, (float)width, (float)height, 0, 1},
        .textured = true, .nearest = true, .blend = kArRenderBlendMode_Opaque, .motion_slot = -1};
  }
  return ArGpuActionScenePass_Encode(pass, cmd, atlas, width, height, motion, 1,
      draws, textures, count, SDL_GPU_LOADOP_CLEAR, (ArRenderColorF){0});
}

bool ArGpuFrameHandoff_EncodeUnpack(SDL_GPUCommandBuffer *cmd,
    const ArGpuFrameUnpack *planes, unsigned count) {
  if (!cmd || !planes || !count || count > kArGpuFrameHandoffMaximumPlanes) return false;
  /* Validate before recording anything: no partial handoff may be exposed. */
  for (unsigned i = 0; i < count; ++i) {
    const ArGpuFrameUnpack *p = &planes[i];
    const ArRenderRectI destination = {p->destination_origin.x, p->destination_origin.y,
        p->source_region.w, p->source_region.h};
    if (!p->source || !p->destination || p->source == p->destination ||
        !Inside(p->source_region, p->source_width, p->source_height) ||
        !Inside(destination, p->destination_width, p->destination_height)) return false;
    for (unsigned j = 0; j < count; ++j)
      if (p->destination == planes[j].source ||
          (j < i && p->destination == planes[j].destination)) return false;
  }
  for (unsigned i = 0; i < count; ++i) if (planes[i].clear_destination) {
    const SDL_GPUColorTargetInfo target = {.texture = planes[i].destination,
        .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE};
    SDL_GPURenderPass *clear = SDL_BeginGPURenderPass(cmd, &target, 1, NULL);
    if (!clear) return false;
    SDL_EndGPURenderPass(clear);
  }
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
  if (!copy) return false;
  for (unsigned i = 0; i < count; ++i) {
    const ArGpuFrameUnpack *p = &planes[i];
    const SDL_GPUTextureLocation source = {.texture = p->source,
        .x = (Uint32)p->source_region.x, .y = (Uint32)p->source_region.y};
    const SDL_GPUTextureLocation destination = {.texture = p->destination,
        .x = (Uint32)p->destination_origin.x, .y = (Uint32)p->destination_origin.y};
    SDL_CopyGPUTextureToTexture(copy, &source, &destination,
        (Uint32)p->source_region.w, (Uint32)p->source_region.h, 1, false);
  }
  SDL_EndGPUCopyPass(copy);
  return true;
}
