#ifndef AR_ACTION_EFFECT_SOURCE_SDL_H
#define AR_ACTION_EFFECT_SOURCE_SDL_H
#include <SDL3/SDL.h>
#include "action/action_effect_source.h"
#include "action/action_effect_render.h"

typedef struct ArGpuEffectLight {
  SDL_GPUBuffer *data, *occluders;
  unsigned point_count, occluder_count, kind, occluder_index;
} ArGpuEffectLight;

typedef struct ArGpuEffectPacket {
  SDL_GPUBuffer *primitives, *occluders;
  ArGpuEffectLight lights[kActionSourceMaxLightJobs];
  uint64_t revision, input_revision;
  unsigned light_count;
  int origin_x, origin_y;
  unsigned capacity, shadow_count;
  float brightness;
} ArGpuEffectPacket;

typedef struct ArGpuEffectSource {
  SDL_GPUDevice *device;
  SDL_GPUComputePipeline *project, *shadow, *moon;
  SDL_GPUGraphicsPipeline *draw[2][3], *skybox[2][2];
  SDL_GPUBuffer *vertices, *coverage, *sky_vertices;
  SDL_GPUBuffer *moon_work, *moon_mask, *moon_light;
  SDL_GPUTransferBuffer *upload;
  ArGpuEffectPacket packets[kActionSourcePacketSlots], transient;
  SDL_GPUSampler *sampler, *nearest_sampler, *wrap_sampler, *wrap_nearest_sampler;
  SDL_GPUCommandBuffer *commands;
  bool batch_open, batch_failed, batching_enabled;
  uint64_t draws, submissions;
  unsigned pending_draws, largest_batch;
} ArGpuEffectSource;

bool ArGpuEffectSource_Init(ArGpuEffectSource *, SDL_GPUDevice *);
void ArGpuEffectSource_Destroy(ArGpuEffectSource *);
/* Owner-thread scope containing only this source's draws. End submits before
 * any SDL draw, target change, resource mutation or readback. No nested scopes.
 * Failed draws discard the scope and invalidate speculative input caches. */
bool ArGpuEffectSource_BeginBatch(ArGpuEffectSource *);
bool ArGpuEffectSource_EndBatch(ArGpuEffectSource *);
bool ArGpuEffectSource_Draw(ArGpuEffectSource *, ArRenderDevice *, SDL_GPUBuffer *motion,
                            uint32_t motion_slots, float phase, const ActionEffectSourceBatch *,
                            const DioramaProjection *, const ActionMoonlightOcclusion *,
                            ArRenderBlendMode, float brightness);
bool ArGpuEffectSource_Skybox(ArGpuEffectSource *, ArRenderDevice *, SDL_GPUBuffer *motion,
                              int motion_slot, float phase, ArRenderTexture,
                              const DioramaSkyboxSourceDraw *);
#endif
