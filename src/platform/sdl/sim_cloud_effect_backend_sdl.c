#include "sim/sim_cloud_effect_backend.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

#include "gpu_shader_blob.h"
#include "platform/sdl/render_sdl_internal.h"
#include "shaders/sim_cloud_frag.h"

static const GpuShaderBlobs kCloudBlobs = {
  kSimCloudFragMSL, kSimCloudFragMSLSize,
  kSimCloudFragSPV, kSimCloudFragSPVSize,
  kSimCloudFragDXIL, kSimCloudFragDXILSize,
};

static struct {
  SDL_Renderer *renderer;
  SDL_GPUDevice *device;
  SDL_GPUShader *shader;
  SDL_GPURenderState *state;
  bool attempted;
} s_cloud;

static SDL_GPUDevice *CloudDevice(SDL_Renderer *renderer) {
  return renderer ? SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),
      SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL) : NULL;
}

bool SimCloudEffectBackend_IsAvailable(ArRenderDevice *device) {
  if (!ArRenderCapabilities_Has(ArRenderDevice_Capabilities(device),
          kArRenderCapability_CustomShaders)) return false;
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (!renderer || (s_cloud.renderer && s_cloud.renderer != renderer)) return false;
  if (s_cloud.attempted) return s_cloud.state != NULL;
  s_cloud.attempted = true;
  s_cloud.renderer = renderer;
  /* A startup diagnostic for same-binary comparisons; not a quality setting. */
  const char *override = SDL_getenv("AR_SIM3D_CLOUD_GPU");
  if (override && !strcmp(override, "0")) return false;
  s_cloud.device = CloudDevice(renderer);
  if (!s_cloud.device) return false;
  s_cloud.shader = GpuShaderBlob_CreateFragment(s_cloud.device, &kCloudBlobs,
      "SIM cloud banks", 1, 1);
  if (!s_cloud.shader) return false;
  const SDL_GPURenderStateCreateInfo info = {.fragment_shader = s_cloud.shader};
  s_cloud.state = SDL_CreateGPURenderState(renderer, &info);
  if (!s_cloud.state) {
    SDL_ReleaseGPUShader(s_cloud.device, s_cloud.shader);
    s_cloud.shader = NULL;
  }
  return s_cloud.state != NULL;
}

bool SimCloudEffectBackend_Bind(ArRenderDevice *device, const SimCloudEffectParams *params) {
  if (!params || !SimCloudEffectBackend_IsAvailable(device)) return false;
  float uniforms[kSimCloudEffectSamples][4];
  for (int i = 0; i < kSimCloudEffectSamples; i++) {
    const SimCloudEffectSample *sample = &params->samples[i];
    if (!isfinite(sample->scale) || sample->scale <= 0 ||
        !isfinite(sample->offset_x) || !isfinite(sample->offset_y) ||
        !isfinite(sample->opacity) || sample->opacity < 0 || sample->opacity > 1)
      return false;
    uniforms[i][0] = sample->scale;
    uniforms[i][1] = sample->offset_x;
    uniforms[i][2] = sample->offset_y;
    uniforms[i][3] = sample->opacity;
  }
  _Static_assert(sizeof(uniforms) == 48, "Three std140 float4 samples");
  return SDL_SetGPURenderStateFragmentUniforms(s_cloud.state, 0, uniforms, sizeof(uniforms)) &&
      SDL_SetGPURenderState(s_cloud.renderer, s_cloud.state);
}

bool SimCloudEffectBackend_Unbind(ArRenderDevice *device) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  return renderer && (!s_cloud.renderer || renderer == s_cloud.renderer) &&
      SDL_SetGPURenderState(renderer, NULL);
}

void SimCloudEffectBackend_Reset(ArRenderDevice *device) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (s_cloud.renderer && renderer != s_cloud.renderer) return;
  if (renderer) (void)SDL_SetGPURenderState(renderer, NULL);
  if (!s_cloud.device || CloudDevice(renderer) == s_cloud.device) {
    SDL_DestroyGPURenderState(s_cloud.state);
    if (s_cloud.shader) SDL_ReleaseGPUShader(s_cloud.device, s_cloud.shader);
  }
  memset(&s_cloud, 0, sizeof(s_cloud));
}
