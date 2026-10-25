#include "present/render_preparation.h"

#include <stdio.h>
#include "render/crt_post.h"
#include "render/gpu_compile_log.h"
#include "app/session_fatal.h"
#include "app/settings.h"
#include "present/preparation_indicator.h"
#include "diorama/diorama_bg_gpu.h"
#include "diorama/diorama_astral_rift.h"
#include "diorama/diorama_effect_backend.h"
#include "diorama/diorama_frame_generation.h"
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

static void TickIndicator(void *context) { PreparationIndicator_Tick(context); }

/* Between preparation steps: keep the indicator moving, and close the compile
 * log's preparation bracket if a fatal error ends preparation early. */
static bool PreparationContinues(ArRenderDevice *device) {
  PreparationIndicator_Tick(device);
  if (!SessionFatal_Requested()) return true;
  GpuCompileLog_EndPreparation();
  PreparationIndicator_End(device);
  return false;
}

/* Full boot/reset preparation builds both sets regardless of current settings.
 * These flags also support hosts that only prepare selected features. */
static bool s_action_stage_prepared;
static bool s_interpolation_prepared;

static void PrepareActionStage(ArRenderDevice *device, bool interpolation) {
  /* Astral Rift uses the built-in triangle pipelines warmed by FinishPreparation.
   * Decode/upload its art here too, so enabling it or entering 07:08 is cheap. */
  DioramaAstralRift_Prepare(device);
  DioramaBgGpu_Prepare(device);
  DioramaEffectBackend_IsAvailable(device, kDioramaEffect_PrioritySurface);
  DioramaEffectBackend_PrepareSceneryLight(device);
  DioramaFrameGeneration_PreparePipelines(device, interpolation);
  s_action_stage_prepared = true;
  s_interpolation_prepared = interpolation;
}

void RenderPreparation_PrepareSelectedFeatures(ArRenderDevice *device) {
  const bool diorama = Diorama_ModeIsOn();
  const bool interpolation = Diorama_InterpolationIsOn();
  if ((s_action_stage_prepared || !diorama) &&
      (s_interpolation_prepared || !interpolation))
    return;
  if (!ArRenderDevice_IsReady(device) || SessionFatal_Requested()) return;
  /* A newly enabled feature must finish preparation before revealing its
   * first frame, just like boot/device reset. Keep the indicator and compile
   * accounting active here too. */
  PreparationIndicator_Begin(device);
  GpuCompileLog_BeginPreparation(TickIndicator, device);
  PrepareActionStage(device, interpolation);
  GpuCompileLog_EndPreparation();
  PreparationIndicator_End(device);
}

bool RenderPreparation_Prepare(ArRenderDevice *device, RenderFeatureMask *supported) {
  if (!supported || !ArRenderDevice_IsReady(device) || SessionFatal_Requested()) return false;
  *supported = 0;
  PreparationIndicator_Begin(device);
  GpuCompileLog_BeginPreparation(TickIndicator, device);
  s_action_stage_prepared = s_interpolation_prepared = false;
  const Sim3DPreparedPipelines depth = Sim3DDepthPass_PreparePipelines(device);
  if (!PreparationContinues(device)) return false;
  if (depth.depth) *supported |= kRenderFeature_Depth;
  if (depth.depth && depth.radial && depth.surfaces && depth.spherical_body)
    *supported |= kRenderFeature_ConnectedGlobe;
  if (DioramaEffectBackend_IsAvailable(device, kDioramaEffect_Blur))
    *supported |= kRenderFeature_DioramaBlur;
  if (!PreparationContinues(device)) return false;
  if (DioramaEffectBackend_IsAvailable(device, kDioramaEffect_RimLight))
    *supported |= kRenderFeature_DioramaRim;
  if (!PreparationContinues(device)) return false;
  if (DioramaEffectBackend_IsAvailable(device, kDioramaEffect_DofEdge))
    *supported |= kRenderFeature_DioramaDof;
  if (!PreparationContinues(device)) return false;
  if (CrtPost_Prepare(device)) *supported |= kRenderFeature_Crt;
  if (!PreparationContinues(device)) return false;
  /* These have equivalent ordinary draw implementations; failure to prepare
   * an acceleration shader does not mean the visual feature is unsupported. */
  const bool cloud_shader = SimCloudEffectBackend_IsAvailable(device);
  if (!PreparationContinues(device)) return false;
  const bool shadow_shader = SimShadowEffectBackend_IsAvailable(device);
  if (!PreparationContinues(device)) return false;
  /* Materialize the ending's custom blend pipelines at boot/reset. Lazy
   * preparation here otherwise stalls the native A-to-B reveal. The ordinary
   * globe remains usable when this optional material is unavailable. */
  const bool completion_shader = CompletionVistaBackend_IsAvailable(device);
  if (!PreparationContinues(device)) return false;
  const bool completion_clouds = completion_shader &&
      PresentWorldNav_PrepareCompletionResources();
  /* Action entry can enable Diorama after the last settings-service pass.
   * Prepare its shared projection/lighting pipelines even when boot starts
   * in a flat menu. Otherwise the first upload compiles them during gameplay. */
  /* Prepare optional features even when their settings are currently off.
   * Toggling interpolation during play must reuse already compiled pipelines.
   * This creates pipelines, not the full-size motion/capture textures. */
  PrepareActionStage(device, true);
  if (!PreparationContinues(device)) return false;

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
  /* SDL owns additional pipelines outside the game shader wrappers. Build
   * those too and wait for submitted warmup draws before ending preparation. */
  if (ok) ok = ArRenderDevice_FinishPreparation(device);
  const ArRenderCapabilities *caps = ArRenderDevice_Capabilities(device);
  fprintf(stderr,
          "[graphics-prepare] features=$%x texture-limit=%dx%d "
          "depth=%d linear-models=%d radial=%d surface=%d body=%d cloud-shader=%d shadow-shader=%d "
          "completion-shader=%d completion-clouds=%d\n",
          *supported, caps->maximum_texture_width, caps->maximum_texture_height, depth.depth,
          depth.linear_models, depth.radial, depth.surfaces, depth.spherical_body, cloud_shader,
          shadow_shader, completion_shader, completion_clouds);
  GpuCompileLog_EndPreparation();
  PreparationIndicator_End(device);
  return ok && !SessionFatal_Requested();
}
