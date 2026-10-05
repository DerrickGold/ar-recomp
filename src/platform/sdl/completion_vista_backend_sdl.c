#include "sim/world_nav/completion_vista_backend.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>
#include "gpu_shader_blob.h"
#include "gpu_render_preparation.h"
#include "shaders/completion_vista_frag.h"

/* std140 vec4 order in completion_vista.frag.glsl. Keep preparation and
 * drawing on this same layout instead of duplicating numeric offsets. */
enum {
  kVistaUniform_EyeRadius,
  kVistaUniform_RightScale,
  kVistaUniform_UpScale,
  kVistaUniform_ForwardTime,
  kVistaUniform_SunHorizonStage,
  kVistaUniform_PlayerBrightnessWaves,
  kVistaUniform_OutputSize,
  kVistaUniform_LightSources,
  kVistaUniform_LightTargets = kVistaUniform_LightSources + kCompletionVistaLightCount,
  kVistaUniform_NativeContact = kVistaUniform_LightTargets + kCompletionVistaLightCount,
  kVistaUniform_Count
};
typedef float CompletionVistaUniforms[kVistaUniform_Count][4];
_Static_assert(sizeof(CompletionVistaUniforms) == 544,
               "Match the completion shader's 34 vec4 uniform block");

static const GpuShaderBlobs kVistaBlobs = {
    kCompletionVistaFragMSL,     kCompletionVistaFragMSLSize, kCompletionVistaFragSPV,
    kCompletionVistaFragSPVSize, kCompletionVistaFragDXIL,    kCompletionVistaFragDXILSize,
};
static struct {
  SDL_Renderer *renderer;
  SDL_GPURenderState *state;
  ArRenderTexture white;
  bool attempted;
} s_vista;

bool CompletionVistaBackend_IsAvailable(ArRenderDevice *device) {
  if (!ArRenderCapabilities_Has(ArRenderDevice_Capabilities(device),
                                kArRenderCapability_CustomShaders))
    return false;
  const char *enabled = SDL_getenv("AR_DEATH_HEIM_COMPLETION_FX");
  if (enabled && !strcmp(enabled, "0")) return false;
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (!renderer || (s_vista.renderer && s_vista.renderer != renderer)) return false;
  if (s_vista.attempted) return s_vista.state != NULL;
  s_vista.attempted = true;
  s_vista.renderer = renderer;
  SDL_GPUShader *shader =
      ArSdlRenderBackend_FragmentShader(device, &kVistaBlobs, "Death Heim completion ocean", 1, 1);
  if (!shader) return false;
  s_vista.state = SDL_CreateGPURenderState(
      renderer, &(SDL_GPURenderStateCreateInfo){.fragment_shader = shader});
  CompletionVistaUniforms warm = {
      [kVistaUniform_EyeRadius] = {0, 0, .42f, 32},
      [kVistaUniform_RightScale] = {1, 0, 0, 1},
      [kVistaUniform_UpScale] = {0, 0, 1, 1},
      [kVistaUniform_ForwardTime] = {0, 1, 0, 0},
      [kVistaUniform_SunHorizonStage] = {.5f, .035f, .53f, kCompletionVistaStage_Sky},
      [kVistaUniform_PlayerBrightnessWaves] = {.5f, .72f, 1, 1},
      [kVistaUniform_OutputSize] = {8, 8, 0, 0},
  };
  for (int i = 0; i < kCompletionVistaLightCount; i++) {
    warm[kVistaUniform_LightSources + i][1] = 2;
    warm[kVistaUniform_LightSources + i][2] = 34;
    warm[kVistaUniform_LightSources + i][3] = .10f;
    warm[kVistaUniform_LightTargets + i][1] = 1;
    warm[kVistaUniform_LightTargets + i][2] = 32;
    warm[kVistaUniform_LightTargets + i][3] = 1;
  }
  const ArRenderTextureDesc desc = {1,
                                    1,
                                    kArRenderPixelFormat_Argb8888,
                                    kArRenderTextureUsage_Static,
                                    kArRenderFilter_Linear,
                                    kArRenderBlendMode_Alpha};
  const uint32_t white = UINT32_C(0xffffffff);
  if (!s_vista.state ||
      !GpuRenderPreparation_Warm(device, s_vista.state, warm, sizeof(warm), false) ||
      !ArRenderDevice_CreateTexture(device, &desc, &s_vista.white) ||
      !ArRenderDevice_UpdateTexture(device, s_vista.white, NULL, &white, sizeof(white))) {
    SDL_DestroyGPURenderState(s_vista.state);
    s_vista.state = NULL;
    ArRenderDevice_DestroyTexture(device, s_vista.white);
    s_vista.white = ArRenderTexture_Invalid();
  }
  return s_vista.state != NULL;
}

PresentationOutcome CompletionVistaBackend_Draw(ArRenderDevice *device, ArRenderRectI viewport,
                                                const CompletionVistaParams *p) {
  if (!p || p->radius <= 0 || p->right_scale <= 0 || p->up_scale <= 0 || p->width <= 0 ||
      p->height <= 0 || (unsigned)p->stage >= kCompletionVistaStage_Count || p->brightness < 0 ||
      p->brightness > 1 || viewport.w <= 0 || viewport.h <= 0)
    return kPresentationOutcome_CoreFailure;
  if (!CompletionVistaBackend_IsAvailable(device)) return kPresentationOutcome_OptionalOmitted;
  CompletionVistaUniforms uniforms = {
      [kVistaUniform_EyeRadius] = {p->eye[0], p->eye[1], p->eye[2], p->radius},
      [kVistaUniform_RightScale] = {p->right[0], p->right[1], p->right[2], p->right_scale},
      [kVistaUniform_UpScale] = {p->up[0], p->up[1], p->up[2], p->up_scale},
      [kVistaUniform_ForwardTime] = {p->forward[0], p->forward[1], p->forward[2], p->time},
      [kVistaUniform_SunHorizonStage] = {p->sun.x, p->sun.y, p->horizon, (float)p->stage},
      [kVistaUniform_PlayerBrightnessWaves] = {p->player.x, p->player.y, p->brightness,
                                               p->waves ? 1 : 0},
      [kVistaUniform_OutputSize] = {(float)p->width, (float)p->height, p->pixel_water ? 1 : 0,
                                    p->stage == kCompletionVistaStage_Ocean &&
                                            ArRenderTexture_IsValid(p->water_pattern)
                                        ? 1
                                        : 0},
      [kVistaUniform_NativeContact] = {p->waterline.x, p->waterline.y,
                                       ArRenderTexture_IsValid(p->native_art) ? 1 : 0,
                                       (float)p->detail_flags},
  };
  for (int i = 0; i < kCompletionVistaLightCount; i++) {
    const CompletionVistaLight *light = &p->lights[i];
    if (light->cone_slope <= 0 || light->strength < 0) return kPresentationOutcome_CoreFailure;
    memcpy(uniforms[kVistaUniform_LightSources + i], light->source, 3 * sizeof(float));
    memcpy(uniforms[kVistaUniform_LightTargets + i], light->target, 3 * sizeof(float));
    uniforms[kVistaUniform_LightSources + i][3] = light->cone_slope;
    uniforms[kVistaUniform_LightTargets + i][3] = light->strength;
  }
  for (unsigned i = 0; i < kVistaUniform_Count; i++)
    for (unsigned j = 0; j < 4; j++)
      if (!isfinite(uniforms[i][j])) return kPresentationOutcome_CoreFailure;
  const bool bound =
      SDL_SetGPURenderStateFragmentUniforms(s_vista.state, 0, uniforms, sizeof(uniforms)) &&
      SDL_SetGPURenderState(s_vista.renderer, s_vista.state);
  const ArRenderDrawState draw = {.flags = kArRenderDrawState_Blend,
                                  .blend = p->stage == kCompletionVistaStage_Shafts ||
                                                   p->stage == kCompletionVistaStage_Rim
                                               ? kArRenderBlendMode_Add
                                               : kArRenderBlendMode_Alpha};
  const ArRenderRectF rect = {(float)viewport.x, (float)viewport.y, (float)viewport.w,
                              (float)viewport.h};
  const ArRenderTexture texture =
      (p->stage == kCompletionVistaStage_Reflection || p->stage == kCompletionVistaStage_Rim) &&
              ArRenderTexture_IsValid(p->native_art)
          ? p->native_art
      : p->stage == kCompletionVistaStage_Ocean && p->pixel_water &&
              ArRenderTexture_IsValid(p->water_pattern)
          ? p->water_pattern
          : s_vista.white;
  const bool drawn =
      bound && ArRenderDevice_DrawTextureWithState(device, texture, NULL, &rect, &draw);
  const bool restored = SDL_SetGPURenderState(s_vista.renderer, NULL);
  return drawn && restored ? kPresentationOutcome_Complete : kPresentationOutcome_CoreFailure;
}

void CompletionVistaBackend_Reset(ArRenderDevice *device) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (s_vista.renderer && renderer != s_vista.renderer) return;
  if (renderer) (void)SDL_SetGPURenderState(renderer, NULL);
  SDL_DestroyGPURenderState(s_vista.state);
  ArRenderDevice_DestroyTexture(device, s_vista.white);
  memset(&s_vista, 0, sizeof(s_vista));
}
