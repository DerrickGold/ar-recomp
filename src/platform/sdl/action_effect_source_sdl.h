#ifndef AR_ACTION_EFFECT_SOURCE_SDL_H
#define AR_ACTION_EFFECT_SOURCE_SDL_H
#include <SDL3/SDL.h>
#include "action/action_effect_source.h"
#include "action/action_effect_render.h"

typedef struct ArGpuEffectPacket {
  SDL_GPUBuffer *primitives;
  uint64_t revision;
  unsigned capacity, shadow_count;
  float brightness;
} ArGpuEffectPacket;

typedef struct ArGpuEffectSource {
  SDL_GPUDevice *device;
  SDL_GPUComputePipeline *project, *shadow, *moon;
  SDL_GPUGraphicsPipeline *draw[2][3], *skybox[2][2];
  SDL_GPUBuffer *primitives, *vertices, *occluders, *coverage, *sky_vertices;
  SDL_GPUBuffer *moon_job, *moon_work, *moon_mask, *moon_light;
  SDL_GPUTransferBuffer *upload;
  ArGpuEffectPacket packets[kActionSourcePacketSlots];
  SDL_GPUSampler *sampler, *nearest_sampler, *wrap_sampler, *wrap_nearest_sampler;
} ArGpuEffectSource;

bool ArGpuEffectSource_Init(ArGpuEffectSource *, SDL_GPUDevice *);
void ArGpuEffectSource_Destroy(ArGpuEffectSource *);
bool ArGpuEffectSource_Draw(ArGpuEffectSource *, ArRenderDevice *, SDL_GPUBuffer *motion,
                            uint32_t motion_slots, float phase, const ActionEffectSourceBatch *,
                            const DioramaProjection *, const ActionMoonlightOcclusion *,
                            ArRenderBlendMode, float brightness);
bool ArGpuEffectSource_Skybox(ArGpuEffectSource *, ArRenderDevice *, SDL_GPUBuffer *motion,
                              int motion_slot, float phase, ArRenderTexture,
                              const DioramaSkyboxSourceDraw *);
#endif
