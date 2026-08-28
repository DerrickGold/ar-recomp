#include "sim/sim_shadow_effect_backend.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "gpu_shader_blob.h"
#include "platform/sdl/render_sdl.h"
#include "shaders/sim_shadow_blur_frag.h"

static const GpuShaderBlobs kSimShadowBlurBlobs = {
  kSimShadowBlurFragMSL, kSimShadowBlurFragMSLSize,
  kSimShadowBlurFragSPV, kSimShadowBlurFragSPVSize,
  kSimShadowBlurFragDXIL, kSimShadowBlurFragDXILSize,
};

static struct {
  SDL_Renderer *renderer;
  SDL_GPUDevice *device;
  SDL_GPUShader *shader;
  SDL_GPURenderState *state;
  bool init_attempted;
} s_blur;

static SDL_GPUDevice *RendererGpuDevice(SDL_Renderer *renderer) {
  if (!renderer) return NULL;
  SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
  return props ? (SDL_GPUDevice *)SDL_GetPointerProperty(
      props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL) : NULL;
}

static bool EnsureBlur(ArRenderDevice *device) {
  if (!ArRenderCapabilities_Has(
          ArRenderDevice_Capabilities(device),
          kArRenderCapability_CustomShaders))
    return false;
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (!renderer) return false;
  if (s_blur.renderer && s_blur.renderer != renderer) {
    SDL_SetError("SIM shadow blur belongs to another renderer");
    return false;
  }
  if (s_blur.init_attempted) return s_blur.state != NULL;
  s_blur.init_attempted = true;
  s_blur.renderer = renderer;

  SDL_GPUDevice *gpu = RendererGpuDevice(renderer);
  if (!gpu) return false;
  s_blur.device = gpu;
  s_blur.shader = GpuShaderBlob_CreateFragment(
      gpu, &kSimShadowBlurBlobs, "SIM shadow blur", 1, 1);
  if (!s_blur.shader) return false;

  SDL_GPURenderStateCreateInfo state_info;
  SDL_zero(state_info);
  state_info.fragment_shader = s_blur.shader;
  s_blur.state = SDL_CreateGPURenderState(renderer, &state_info);
  if (!s_blur.state) {
    fprintf(stderr, "[sim3d-d4] shadow blur render state failed: %s\n",
            SDL_GetError());
    SDL_ReleaseGPUShader(gpu, s_blur.shader);
    s_blur.shader = NULL;
    return false;
  }
  return true;
}

bool SimShadowEffectBackend_IsAvailable(ArRenderDevice *device) {
  return EnsureBlur(device);
}

bool SimShadowEffectBackend_BindBlur(
    ArRenderDevice *device, const SimShadowBlurEffectParams *params) {
  if (!params || !EnsureBlur(device)) return false;
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (!renderer) return false;
  const struct {
    float texel_x;
    float texel_y;
    float radius;
    float padding;
  } uniforms = {
    params->texel_x, params->texel_y, params->radius, 0.0f,
  };
  return SDL_SetGPURenderStateFragmentUniforms(
             s_blur.state, 0, &uniforms, sizeof(uniforms)) &&
      SDL_SetGPURenderState(renderer, s_blur.state);
}

bool SimShadowEffectBackend_Unbind(ArRenderDevice *device) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  return renderer && SDL_SetGPURenderState(renderer, NULL);
}

void SimShadowEffectBackend_Reset(ArRenderDevice *device) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  SDL_GPUDevice *current_device = RendererGpuDevice(renderer);
  const bool same_renderer = !s_blur.renderer ||
      renderer == s_blur.renderer;
  const bool same_device = !s_blur.device ||
      current_device == s_blur.device;
  if (renderer && same_renderer)
    (void)SDL_SetGPURenderState(renderer, NULL);
  if (same_device)
    SDL_DestroyGPURenderState(s_blur.state);
  if (same_device && current_device && s_blur.shader)
    SDL_ReleaseGPUShader(current_device, s_blur.shader);
  memset(&s_blur, 0, sizeof(s_blur));
}
