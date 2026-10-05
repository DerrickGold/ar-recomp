#include "present/render_preparation.h"

#include <stdio.h>
#include "render/crt_post.h"
#include "app/session_fatal.h"
#include "diorama/diorama_effect_backend.h"
#include "sim/sim3d/sim3d_depth_pass.h"
#include "sim/sim3d/sim_cloud_effect_backend.h"
#include "sim/sim3d/sim_shadow_effect_backend.h"
#include "sim/world_nav/completion_vista_backend.h"
#include "sim/world_nav/present_world_nav.h"

static bool ProbeBlend(ArRenderDevice *device, ArRenderTexture texture,
                       ArRenderBlendMode blend) {
  const ArRenderVertex2D vertices[] = {
    {{0, 0}, {1, 1, 1, 1}, {0, 0}},
    {{8, 0}, {1, 1, 1, 1}, {1, 0}},
    {{0, 8}, {1, 1, 1, 1}, {0, 1}},
  };
  const int32_t indices[] = {0, 1, 2};
  const ArRenderDrawState state = {.flags = kArRenderDrawState_Blend, .blend = blend};
  return ArRenderDevice_DrawGeometryWithState(device, texture,
      vertices, 3, indices, 3, &state);
}

bool RenderPreparation_Prepare(ArRenderDevice *device, RenderFeatureMask *supported) {
  if (!supported || !ArRenderDevice_IsReady(device) || SessionFatal_Requested()) return false;
  *supported = 0;
  const Sim3DPreparedPipelines depth = Sim3DDepthPass_PreparePipelines(device);
  if (SessionFatal_Requested()) return false;
  if (depth.depth) *supported |= kRenderFeature_Depth;
  if (depth.depth && depth.radial && depth.surfaces && depth.spherical_body)
    *supported |= kRenderFeature_ConnectedGlobe;
  if (DioramaEffectBackend_IsAvailable(device, kDioramaEffect_Blur))
    *supported |= kRenderFeature_DioramaBlur;
  if (SessionFatal_Requested()) return false;
  if (DioramaEffectBackend_IsAvailable(device, kDioramaEffect_RimLight))
    *supported |= kRenderFeature_DioramaRim;
  if (SessionFatal_Requested()) return false;
  if (DioramaEffectBackend_IsAvailable(device, kDioramaEffect_DofEdge))
    *supported |= kRenderFeature_DioramaDof;
  if (SessionFatal_Requested()) return false;
  if (CrtPost_Prepare(device)) *supported |= kRenderFeature_Crt;
  if (SessionFatal_Requested()) return false;
  /* These have equivalent ordinary draw implementations; failure to prepare
   * an acceleration shader does not mean the visual feature is unsupported. */
  const bool cloud_shader = SimCloudEffectBackend_IsAvailable(device);
  if (SessionFatal_Requested()) return false;
  const bool shadow_shader = SimShadowEffectBackend_IsAvailable(device);
  if (SessionFatal_Requested()) return false;
  /* Materialize the ending's custom blend pipelines at boot/reset. Lazy
   * preparation here otherwise stalls the native A-to-B reveal. The ordinary
   * globe remains usable when this optional material is unavailable. */
  const bool completion_shader = CompletionVistaBackend_IsAvailable(device);
  if (SessionFatal_Requested()) return false;
  const bool completion_clouds = completion_shader &&
      PresentWorldNav_PrepareCompletionResources();

  ArRenderTexture sample = ArRenderTexture_Invalid(), target = ArRenderTexture_Invalid();
  const ArRenderTextureDesc sample_desc = {
    1, 1, kArRenderPixelFormat_Argb8888, kArRenderTextureUsage_Static,
    kArRenderFilter_Nearest, kArRenderBlendMode_Alpha,
  };
  const ArRenderTextureDesc target_desc = {
    8, 8, kArRenderPixelFormat_Argb8888, kArRenderTextureUsage_Target,
    kArRenderFilter_Nearest, kArRenderBlendMode_Alpha,
  };
  const uint32_t white = UINT32_C(0xffffffff);
  ArRenderTargetState state = {0};
  bool ok = ArRenderDevice_CreateTexture(device, &sample_desc, &sample) &&
      ArRenderDevice_UpdateTexture(device, sample, NULL, &white, sizeof(white)) &&
      ArRenderDevice_CreateTexture(device, &target_desc, &target);
  if (ok) ok = ArRenderDevice_BeginTarget(device, target, &state) == kArRenderTargetBegin_Ready;
  if (ok) {
    ok = ArRenderDevice_Clear(device, (ArRenderColorF){0, 0, 0, 0});
    if (ok && ProbeBlend(device, sample, kArRenderBlendMode_DestinationAlphaMask))
      *supported |= kRenderFeature_SimRim;
    if (ok && ProbeBlend(device, ArRenderTexture_Invalid(), kArRenderBlendMode_Add))
      *supported |= kRenderFeature_Effects;
    if (ok && ProbeBlend(device, sample, kArRenderBlendMode_AlphaAccumulate) &&
        ProbeBlend(device, sample, kArRenderBlendMode_AddPremultiplied))
      *supported |= kRenderFeature_SimSoftShadows;
    if (!ArRenderDevice_EndTarget(device, &state)) ok = false;
  }
  ArRenderDevice_DestroyTexture(device, sample);
  ArRenderDevice_DestroyTexture(device, target);
  const ArRenderCapabilities *caps = ArRenderDevice_Capabilities(device);
  fprintf(stderr,
          "[graphics-prepare] features=$%x texture-limit=%dx%d "
          "depth=%d linear-models=%d radial=%d surface=%d body=%d cloud-shader=%d shadow-shader=%d "
          "completion-shader=%d completion-clouds=%d\n",
          *supported, caps->maximum_texture_width, caps->maximum_texture_height, depth.depth,
          depth.linear_models, depth.radial, depth.surfaces, depth.spherical_body, cloud_shader,
          shadow_shader, completion_shader, completion_clouds);
  return ok && !SessionFatal_Requested();
}
