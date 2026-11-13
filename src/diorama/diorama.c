#include "action/action_voxel.h"
#include "actraiser/actraiser_room_profiles.h"

#include "diorama.h"
#include "diorama_astral_rift.h"
#include "constants.h"
#include "diorama_effect_backend.h"
#include "diorama_aperture.h"
#include "diorama_edge_aa.h"
#include "diorama_layer_order.h"
#include "diorama_rom_backdrop.h"
#include "diorama_skybox_uv.h"
#include "diorama_stack_group.h"
#include "diorama_depth_shapes.h" /* rake/bow/thick/stack/voxel arithmetic */
#include "diorama_depth_pass.h"
#include "diorama_trunk_guard_upload.h"
#include "diorama_voxel_guard_upload.h"
#include "diorama_voxel_source_clip.h"
#include "diorama_performance.h"
#include "render/scene3d_math.h"
#include "render/scenery_dimming.h"
#include "render/scenery_shading.h"
#include "render/render_output.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Optional compositor polish is additive. The game owns its settings and
 * semantic parameters; the active backend owns native shaders and may decline
 * any effect without disabling the ordinary compositor path. */

/* 3x3 weighted-box blur (9 taps, center weighted x2) as a cheap Gaussian
 * approximation — softens the existing hard-edged silhouette shadow into a
 * soft drop shadow (doc §7.2: "each layer casts a soft shadow on the one
 * behind it"). Samples the SAME texture/UVs the CPU path already uses;
 * vertex color (the existing black+alpha tint) is preserved by the final
 * multiply, so this is purely additive over the existing effect. */
/* kSettingCat_Graphics "Soft shadow blur" row, independent of the other
 * GPU effect toggles. Read fresh every frame (same live-toggle pattern as
 * the diorama_layer_* visibility settings) — both this AND
 * gpu_shaders_enabled (the backend switch, host_video.c) must be on. */
static bool ShadowBlurEnabled(ArRenderDevice *device,
                              const DioramaRenderOptions *options) {
  return options->shadow_blur &&
      DioramaEffectBackend_IsAvailable(device, kDioramaEffect_Blur);
}

/* B5: skybox DoF reuses the same blur shader machinery, but unlike the
 * effects above it has no separate settings toggle — the skybox mode enum
 * itself is the opt-in, and blur is inherent to reading as "atmosphere, not
 * focus" (the doc's framing), not an independent knob. Falls back to a
 * crisp (unblurred) skybox if the shader is unavailable — still far better
 * than the void it replaces. */
static bool SkyboxBlurEnabled(ArRenderDevice *device) {
  return DioramaEffectBackend_IsAvailable(device, kDioramaEffect_Blur);
}

/* Focal plane: BG1's Z (kDioramaLayers) — the main playfield the player and
 * most of the action sit on. Layers farther from it blur proportionally;
 * kDofMaxRadiusTexels caps how soft the farthest layer (the backdrop) gets. */
static const float kDofFocalZ = 0.50f;
static const float kDofStrength = 3.0f;      /* texels of blur per unit Z distance */
static const float kDofMaxRadiusTexels = 2.0f;

static float DofRadiusForLayer(float layer_z) {
  float dist = fabsf(layer_z - kDofFocalZ);
  float radius = dist * kDofStrength;
  if (radius > kDofMaxRadiusTexels) radius = kDofMaxRadiusTexels;
  return radius;
}

/* KNOWN LIMITATION (confirmed live, not fixed): the shadow copy is a flat
 * semi-transparent quad drawn painter's-algorithm style over whatever was
 * drawn before it. It has no notion of whether the receiving pixels are
 * actually opaque — where an earlier layer has a transparent gap (sprite
 * silhouette edges, tile gaps), the shadow just darkens whatever shows
 * through underneath, sky included. Excluding BG2 (see kDioramaLayers) only
 * fixed the "backdrop is directly behind" case, not this general one. A
 * real fix needs depth/stencil-aware "only shadow opaque receivers"
 * compositing — bigger scope than this pass. Left OFF by default
 * (AR_GPU_FX_SHADOW=1 to experiment) rather than half-fixed. */

/* ── Rim lighting / edge glow (AR_GPU_FX_RIM=1) ──────────────────────────
 * Unlike the shadow effect above, this only reacts to a layer's OWN alpha
 * silhouette — it brightens pixels that are opaque but close to their own
 * edge, and never changes the alpha/footprint of the sprite. So it has
 * none of the "bleeds onto whatever's behind" problem: nothing behind the
 * layer is touched, only the layer's own already-opaque pixels are tinted. */

/* kSettingCat_Graphics "Rim lighting" row, independent of the other GPU
 * effect toggles. Both this AND gpu_shaders_enabled must be on. */
static bool RimLightEnabled(ArRenderDevice *device,
                              const DioramaRenderOptions *options) {
  return options->rim_light &&
      DioramaEffectBackend_IsAvailable(device, kDioramaEffect_RimLight);
}

/* ── Depth of field + screen-space edge coverage ─────────────────────────
 * DOF remains a fragment effect. Edge AA is deliberately geometry now: an
 * earlier shader faded two SOURCE texels inward from every rectangular edge.
 * Perspective could magnify that into a many-pixel translucent band which
 * exposed differently colored layers underneath. The coverage fringe keeps
 * the true layer opaque and fades only a one-output-pixel outward ring. It
 * also means an edge-only plane no longer binds a nine-tap blur shader whose
 * radius is zero. */

/* kSettingCat_Graphics "Depth of field" row (§7.2). */
static bool DofBlurEnabled(ArRenderDevice *device,
                              const DioramaRenderOptions *options) {
  return options->depth_of_field &&
      DioramaEffectBackend_IsAvailable(device, kDioramaEffect_DofEdge);
}

/* Which layers get edge AA: the BG planes whose rectangular boundary is the
 * visible "shadowbox wall" (BG1/BG2 and their priority-split halves). Not
 * the backdrop (full-screen, no meaningful edge), not sprites (small
 * billboards — rim light already treats their edges), not the HUD (BG3,
 * must stay crisp). */
static bool LayerGetsEdgeAA(int plane) {
  switch (plane) {
    case SR_PPU_OVERLAY_BG1:
    case SR_PPU_OVERLAY_BG2:
    case kDioramaPlane_Bg1Hi:
    case kDioramaPlane_Bg2Hi:
    case kDioramaPlane_Bg1Far:
    case kDioramaPlane_Bg2Far:
      return true;
    default:
      return false;
  }
}

/* Background planes authored in front of BG1 retain their internal parallax,
 * but they must not enlarge the rectangular scene aperture. Their transparent
 * pixels are priority holes that reveal the already/later composited stack;
 * exposing the auxiliary plane's own projected boundary turns its edge texels
 * into dark/light strips at oblique camera angles. */
static bool LayerUsesFocalAperture(int plane) {
  switch (plane) {
    case SR_PPU_OVERLAY_BG1:
    case SR_PPU_OVERLAY_BG2:
    case kDioramaPlane_Bg1Hi:
    case kDioramaPlane_Bg2Hi:
    case kDioramaPlane_Bg1Far:
    case kDioramaPlane_Bg2Far:
      return true;
    default:
      return false;
  }
}

/* ── B1b-crisp: ×4 supersample + premultiplied-LINEAR AA ─────────────────
 * The diorama layer textures use nearest filtering and
 * the tilted quads sample them through an arbitrary perspective warp, so
 * high-contrast pixel-art edges step/shimmer as the camera moves — even
 * with interpolation off, this is plain NEAREST minification/magnification
 * artifacting, not a scroll-smoothness issue. Fix: render each layer to a
 * ×4 integer-upscaled NEAREST intermediate first (matches the existing
 * the host's kHdMode7Scale=4 supersample scale, then sample
 * THAT with LINEAR for the actual tilt+shift draw — the intermediate is 4
 * whole texels per source texel, so LINEAR there interpolates smoothly
 * instead of stepping.
 *
 * The intermediate keeps straight alpha and uses ordinary alpha blending for
 * the final draw. Some APIs expose premultiplied blending, but it is not
 * implemented reliably by every renderer: a backend can accept
 * the mode yet draw the transparent part of a target texture as opaque black.
 * Keeping the widely supported blend path is more important than the small
 * fringe reduction premultiplication can provide on antialiased edge texels.
 *
 * Scoped to layers that DON'T have a rim-light or nonzero-DOF shader bound.
 * Edge coverage is ordinary geometry and can share this filtered source. A
 * layer gets one texture-filtering path or the other, never both. */
enum { kDioramaSupersample = 4 };

/* Guard-bearing captures and ordinary planes have two widths in one frame.
 * Retain both exact sizes instead of resizing one intermediate between layers.
 * Pixels are refreshed on every use; only allocation is cached. */
enum { kDioramaScratchSizeCount = 2 };
typedef struct DioramaScratchTarget {
  ArRenderTexture texture;
  int width, height;
} DioramaScratchTarget;
typedef struct DioramaScratchPool {
  DioramaScratchTarget targets[kDioramaScratchSizeCount];
  unsigned next;
} DioramaScratchPool;
static DioramaScratchPool s_diorama_ss;
static DioramaScratchPool s_diorama_trunk_guard_ss;
static DioramaScratchPool s_diorama_voxel_guard_ss;
static ArRenderTexture s_diorama_voxel_guard_priority;
static int s_diorama_voxel_guard_priority_width;
static bool s_diorama_trunk_guard_ss_unavailable;
static bool s_diorama_voxel_guard_ss_unavailable;
static bool s_diorama_ss_unavailable;
static ArRenderTexture s_diorama_priority_texture;
static int s_diorama_priority_w, s_diorama_priority_h;
static DioramaScratchPool s_diorama_dof_source;
static DioramaScratchPool s_diorama_sky_weather;
static DioramaScratchPool s_diorama_lightmap;
static DioramaScratchPool s_diorama_bloom[3];
static ArRenderTexture s_diorama_unlit_map;
static DioramaScratchPool s_diorama_actor_lightmaps[2];
static DioramaScratchPool s_diorama_voxel_subscreen;
static bool s_diorama_dof_source_unavailable;
static ArRenderTexture s_diorama_stack_group_texture;
static int s_diorama_stack_group_w, s_diorama_stack_group_h;
static bool s_diorama_stack_group_unavailable;
static ArRenderTexture s_diorama_skybox_prefilter_texture;
static int s_diorama_skybox_prefilter_w, s_diorama_skybox_prefilter_h;
static bool s_diorama_skybox_prefilter_unavailable;
static bool s_diorama_skybox_prefilter_valid;
static ArRenderTexture s_diorama_skybox_prefilter_source;
static uint64_t s_diorama_skybox_prefilter_revision;
static float s_diorama_skybox_prefilter_radius;
static ArRenderColorF s_diorama_skybox_prefilter_tint;
static DioramaSceneryGrade s_diorama_skybox_prefilter_grade;
static bool s_diorama_skybox_prefilter_rom_source;

static void ResetDioramaScratchPool(ArRenderDevice *device, DioramaScratchPool *pool) {
  for (unsigned i = 0; i < kDioramaScratchSizeCount; ++i)
    ArRenderDevice_DestroyTexture(device, pool->targets[i].texture);
  memset(pool, 0, sizeof(*pool));
}

static ArRenderTexture EnsureDioramaScratchTarget(
    ArRenderDevice *device, DioramaScratchPool *pool, int width, int height,
    ArRenderFilter filter) {
  for (unsigned i = 0; i < kDioramaScratchSizeCount; ++i) {
    const DioramaScratchTarget *target = &pool->targets[i];
    if (ArRenderTexture_IsValid(target->texture) &&
        target->width == width && target->height == height)
      return target->texture;
  }
  DioramaScratchTarget *target = &pool->targets[pool->next];
  pool->next = (pool->next + 1) % kDioramaScratchSizeCount;
  ArRenderDevice_DestroyTexture(device, target->texture);
  *target = (DioramaScratchTarget){0};
  const ArRenderTextureDesc desc = {
    .width = width, .height = height,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Target,
    .filter = filter, .blend = kArRenderBlendMode_Alpha,
  };
  if (!ArRenderDevice_CreateTexture(device, &desc, &target->texture)) {
    ArRenderDevice_DestroyTexture(device, target->texture);
    *target = (DioramaScratchTarget){0};
    return ArRenderTexture_Invalid();
  }
  target->width = width;
  target->height = height;
  return target->texture;
}

static ArRenderTexture UnlitSceneryMap(ArRenderDevice *device) {
  if (!ArRenderTexture_IsValid(s_diorama_unlit_map)) {
    const ArRenderTextureDesc desc={1,1,kArRenderPixelFormat_Argb8888,
        kArRenderTextureUsage_Static,kArRenderFilter_Nearest,kArRenderBlendMode_Alpha};
    const uint32_t black=0xff000000u;
    const ArRenderRectI pixel={0,0,1,1};
    if (!ArRenderDevice_CreateTexture(device,&desc,&s_diorama_unlit_map) ||
        !ArRenderDevice_UpdateTexture(device,s_diorama_unlit_map,&pixel,&black,sizeof(black))) {
      ArRenderDevice_DestroyTexture(device,s_diorama_unlit_map);
      s_diorama_unlit_map=ArRenderTexture_Invalid();
    }
  }
  return s_diorama_unlit_map;
}

static void ResetDioramaSupersample(ArRenderDevice *device) {
  ResetDioramaScratchPool(device, &s_diorama_ss);
  ResetDioramaScratchPool(device, &s_diorama_trunk_guard_ss);
  ResetDioramaScratchPool(device, &s_diorama_voxel_guard_ss);
  ArRenderDevice_DestroyTexture(device,s_diorama_voxel_guard_priority);
  s_diorama_voxel_guard_priority=ArRenderTexture_Invalid();
  s_diorama_voxel_guard_priority_width=0;
  s_diorama_ss_unavailable = false;
  s_diorama_trunk_guard_ss_unavailable = false;
  s_diorama_voxel_guard_ss_unavailable = false;
}

static void ResetDioramaPrioritySurface(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(device, s_diorama_priority_texture);
  s_diorama_priority_texture = ArRenderTexture_Invalid();
  s_diorama_priority_w = s_diorama_priority_h = 0;
}

static void DisableDioramaSupersample(ArRenderDevice *device) {
  ResetDioramaSupersample(device);
  s_diorama_ss_unavailable = true;
}

static void ResetDioramaDofSource(ArRenderDevice *device) {
  ResetDioramaScratchPool(device, &s_diorama_dof_source);
  s_diorama_dof_source_unavailable = false;
}

static void DisableDioramaDofSource(ArRenderDevice *device) {
  ResetDioramaDofSource(device);
  s_diorama_dof_source_unavailable = true;
}

static void ResetDioramaStackGroup(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(device, s_diorama_stack_group_texture);
  s_diorama_stack_group_texture = ArRenderTexture_Invalid();
  s_diorama_stack_group_w = 0;
  s_diorama_stack_group_h = 0;
  s_diorama_stack_group_unavailable = false;
}

static void DisableDioramaStackGroup(ArRenderDevice *device) {
  ResetDioramaStackGroup(device);
  s_diorama_stack_group_unavailable = true;
}

static void ResetDioramaSkyboxPrefilter(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(
      device, s_diorama_skybox_prefilter_texture);
  s_diorama_skybox_prefilter_texture = ArRenderTexture_Invalid();
  s_diorama_skybox_prefilter_w = 0;
  s_diorama_skybox_prefilter_h = 0;
  s_diorama_skybox_prefilter_unavailable = false;
  s_diorama_skybox_prefilter_valid = false;
  s_diorama_skybox_prefilter_source = ArRenderTexture_Invalid();
  s_diorama_skybox_prefilter_revision = 0;
  s_diorama_skybox_prefilter_radius = 0.0f;
  s_diorama_skybox_prefilter_tint = (ArRenderColorF){0};
  s_diorama_skybox_prefilter_rom_source = false;
}

static void DisableDioramaSkyboxPrefilter(ArRenderDevice *device) {
  ResetDioramaSkyboxPrefilter(device);
  s_diorama_skybox_prefilter_unavailable = true;
}

static ArRenderTexture EnsureDioramaSkyboxPrefilterTexture(
    ArRenderDevice *device, int width, int height) {
  if (!ArRenderDevice_IsReady(device) || width <= 0 || height <= 0 ||
      s_diorama_skybox_prefilter_unavailable)
    return ArRenderTexture_Invalid();
  if (ArRenderTexture_IsValid(s_diorama_skybox_prefilter_texture) &&
      s_diorama_skybox_prefilter_w == width &&
      s_diorama_skybox_prefilter_h == height)
    return s_diorama_skybox_prefilter_texture;
  ArRenderDevice_DestroyTexture(
      device, s_diorama_skybox_prefilter_texture);
  s_diorama_skybox_prefilter_texture = ArRenderTexture_Invalid();
  s_diorama_skybox_prefilter_valid = false;
  const ArRenderTextureDesc desc = {
    .width = width,
    .height = height,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Target,
    .filter = kArRenderFilter_Nearest,
    .blend = kArRenderBlendMode_Alpha,
  };
  if (!ArRenderDevice_CreateTexture(
          device, &desc, &s_diorama_skybox_prefilter_texture)) {
    fprintf(stderr,
            "[diorama] skybox prefilter target unavailable; using direct "
            "full-resolution blur: %s\n", ArRenderDevice_LastError(device));
    DisableDioramaSkyboxPrefilter(device);
    return ArRenderTexture_Invalid();
  }
  s_diorama_skybox_prefilter_w = width;
  s_diorama_skybox_prefilter_h = height;
  return s_diorama_skybox_prefilter_texture;
}

static ArRenderTexture EnsureDioramaStackGroupTexture(
    ArRenderDevice *device, int width, int height) {
  if (!ArRenderDevice_IsReady(device) || width <= 0 || height <= 0 ||
      s_diorama_stack_group_unavailable)
    return ArRenderTexture_Invalid();
  if (ArRenderTexture_IsValid(s_diorama_stack_group_texture) &&
      s_diorama_stack_group_w == width &&
      s_diorama_stack_group_h == height)
    return s_diorama_stack_group_texture;
  ArRenderDevice_DestroyTexture(device, s_diorama_stack_group_texture);
  s_diorama_stack_group_texture = ArRenderTexture_Invalid();
  const ArRenderTextureDesc desc = {
    .width = width,
    .height = height,
    .format = kArRenderPixelFormat_Argb8888,
    .usage = kArRenderTextureUsage_Target,
    .filter = kArRenderFilter_Linear,
    .blend = kArRenderBlendMode_AlphaPremultiplied,
  };
  if (!ArRenderDevice_CreateTexture(
          device, &desc, &s_diorama_stack_group_texture)) {
    fprintf(stderr,
            "[diorama] stack-group target unavailable; using direct stack "
            "draws: %s\n", ArRenderDevice_LastError(device));
    DisableDioramaStackGroup(device);
    return ArRenderTexture_Invalid();
  }
  s_diorama_stack_group_w = width;
  s_diorama_stack_group_h = height;
  return s_diorama_stack_group_texture;
}

/* Object occupancy is always culled. BG coverage has an explicit A/B gate. */
static bool DioramaSparseCoverageEnabledForPlane(
    int plane, const DioramaRenderOptions *options) {
  return DioramaPlaneIsObjectPriority(plane) ||
      (options->sparse_coverage && DioramaPlaneUsesSparseCoverage(plane));
}

static ArRenderTexture EnsureDioramaSupersampleTexture(
    ArRenderDevice *device, int w, int h) {
  if (!ArRenderDevice_IsReady(device) || w <= 0 || h <= 0 || s_diorama_ss_unavailable)
    return ArRenderTexture_Invalid();
  const ArRenderTexture texture = EnsureDioramaScratchTarget(
      device, &s_diorama_ss, w, h, kArRenderFilter_Linear);
  if (!ArRenderTexture_IsValid(texture)) {
    fprintf(stderr,
            "[diorama] supersample target unavailable; optional crisp AA "
            "disabled for this renderer: %s\n", ArRenderDevice_LastError(device));
    DisableDioramaSupersample(device);
  }
  return texture;
}

static ArRenderTexture EnsureDioramaDofSourceTexture(
    ArRenderDevice *device, int width, int height) {
  if (!ArRenderDevice_IsReady(device) || width <= 0 || height <= 0 ||
      s_diorama_dof_source_unavailable)
    return ArRenderTexture_Invalid();
  const ArRenderTexture texture = EnsureDioramaScratchTarget(
      device, &s_diorama_dof_source, width, height, kArRenderFilter_Nearest);
  if (!ArRenderTexture_IsValid(texture)) {
    fprintf(stderr,
            "[diorama] compact DOF source unavailable; using crisp layer "
            "fallback: %s\n", ArRenderDevice_LastError(device));
    DisableDioramaDofSource(device);
  }
  return texture;
}

/* Renders `source` (an ABI-max-width x snes_height layer texture, already
 * NEAREST-scaled) into the compact shared ×4 straight-alpha intermediate.
 * Returns an invalid handle (caller falls back to `source`) if the
 * intermediate couldn't be (re)created.
 *
 * Live report (2026-07-21): a thin magenta/garbage-colored line was visible
 * at the diorama's right edge whenever a layer used this path (most
 * noticeable on the near-fullscreen backdrop plane) — present even with NO
 * interpolation shift active, so it wasn't the B1b UV-window bug. Root
 * cause: this used to blit the whole source texture into the whole
 * intermediate, which faithfully copies
 * source's uninitialized tail (columns snes_width..surface-max-width-1 — see
 * B1b UV-window comment below for why that tail exists at all) into the
 * intermediate too. The final draw's LINEAR sample at the exact valid/
 * invalid boundary (u=uv_u1) then blends the last real texel against that
 * garbage, every frame, for every crisp-path layer — B1b-crisp switched
 * this path from NEAREST (no cross-texel blending, so this boundary was
 * never sampled softly) to LINEAR, which is what actually exposed it. Fixed
 * by blitting only the VALID `{0,0,snes_width,snes_height}` source sub-rect
 * into an intermediate sized to that exact active region. */
static void DisableDioramaSupersamplePool(ArRenderDevice *device,DioramaScratchPool *pool) {
  if(!pool){DisableDioramaSupersample(device);return;}
  ResetDioramaScratchPool(device,pool);
  if(pool==&s_diorama_voxel_guard_ss)s_diorama_voxel_guard_ss_unavailable=true;
  else s_diorama_trunk_guard_ss_unavailable=true;
}
static ArRenderTexture BuildDioramaSupersampleFromPool(
    ArRenderDevice *device, ArRenderTexture source, int obj_apron, int source_y,
    int snes_width, int snes_height, DioramaScratchPool *pool, PresentationOutcome *outcome) {
  if (outcome) *outcome = kPresentationOutcome_Complete;
  if (!ArRenderDevice_IsReady(device) ||
      !ArRenderTexture_IsValid(source) || obj_apron < 0 || source_y < 0 || snes_width <= 0 ||
      snes_height <= 0 || snes_width > INT_MAX / kDioramaSupersample ||
      snes_height > INT_MAX / kDioramaSupersample) {
    if (outcome) *outcome = kPresentationOutcome_CoreFailure;
    return ArRenderTexture_Invalid();
  }
  const bool pool_unavailable=pool==&s_diorama_voxel_guard_ss
      ?s_diorama_voxel_guard_ss_unavailable:s_diorama_trunk_guard_ss_unavailable;
  const ArRenderTexture ss = pool
      ? pool_unavailable?ArRenderTexture_Invalid():EnsureDioramaScratchTarget(device,pool,
          snes_width*kDioramaSupersample,snes_height*kDioramaSupersample,kArRenderFilter_Linear)
      : EnsureDioramaSupersampleTexture(device,snes_width*kDioramaSupersample,snes_height*kDioramaSupersample);
  if (!ArRenderTexture_IsValid(ss)) {
    if(pool&&!pool_unavailable)
      DisableDioramaSupersamplePool(device,pool);
    if (outcome) *outcome = kPresentationOutcome_OptionalOmitted;
    return ArRenderTexture_Invalid();
  }
  ArRenderTargetState target_state;
  const ArRenderTargetBeginResult begin = ArRenderDevice_BeginTarget(
      device, ss, &target_state);
  if (begin != kArRenderTargetBegin_Ready) {
    if (outcome) {
      *outcome = begin == kArRenderTargetBegin_StateLost
          ? kPresentationOutcome_CoreFailure
          : kPresentationOutcome_OptionalOmitted;
    }
    if (begin == kArRenderTargetBegin_Omitted)
      DisableDioramaSupersamplePool(device,pool);
    return ArRenderTexture_Invalid();
  }
  bool success = ArRenderDevice_Clear(
      device, (ArRenderColorF){0.0f, 0.0f, 0.0f, 0.0f});
  /* Starts at the APRON, not at column 0: the displayed span is the MIDDLE of
   * an apron-wide surface. Blitting from 0 copied the empty left apron in and
   * pushed the content right -- which on the backdrop (the one BLENDMODE_NONE
   * layer, so transparent reads as black) showed as a permanent black stripe
   * down the left edge, fixed in place regardless of level progression. */
  const ArRenderRectF src = {
    (float)obj_apron, (float)source_y, (float)snes_width, (float)snes_height,
  };
  const ArRenderRectF dst = {
    0.0f, 0.0f, (float)(snes_width * kDioramaSupersample),
    (float)(snes_height * kDioramaSupersample),
  };
  if (success) {
    const ArRenderDrawState draw_state = {
      .flags = kArRenderDrawState_Blend,
      .blend = kArRenderBlendMode_Opaque,
    };
    success = ArRenderDevice_DrawTextureWithState(
        device, source, &src, &dst, &draw_state);
  }
  /* Restore the caller's target, not an assumed CRT target. Presentation can
   * deliberately wrap the complete scene in another render pass (heat haze,
   * capture, accessibility filters); hard-coding the boot target silently
   * escaped all of those wrappers whenever this crisp path was active. */
  if (!ArRenderDevice_EndTarget(device, &target_state)) {
    if (outcome) *outcome = kPresentationOutcome_CoreFailure;
    return ArRenderTexture_Invalid();
  }
  if (!success && outcome)
    *outcome = kPresentationOutcome_OptionalOmitted;
  if (!success)
    DisableDioramaSupersamplePool(device,pool);
  return success ? ss : ArRenderTexture_Invalid();
}

static ArRenderTexture BuildDioramaSupersample(ArRenderDevice *device,ArRenderTexture source,
    int obj_apron,int width,int height,PresentationOutcome *outcome) {
  return BuildDioramaSupersampleFromPool(device,source,obj_apron,0,width,height,NULL,outcome);
}

/* Copies only the active capture into a source-resolution target. The custom
 * DOF sampler clamps to the ACTUAL texture edge there, rather than to the edge
 * of the persistent 640x352 allocation whose unused tail is transparent. That
 * gives every blur tap the correct border condition without cropping or
 * stretching the layer's visible texels. */
static ArRenderTexture BuildDioramaDofSource(
    ArRenderDevice *device, ArRenderTexture source, int obj_apron,
    int snes_width, int snes_height, PresentationOutcome *outcome) {
  if (outcome) *outcome = kPresentationOutcome_Complete;
  if (!ArRenderDevice_IsReady(device) ||
      !ArRenderTexture_IsValid(source) || obj_apron < 0 || snes_width <= 0 ||
      snes_height <= 0) {
    if (outcome) *outcome = kPresentationOutcome_CoreFailure;
    return ArRenderTexture_Invalid();
  }
  const ArRenderTexture compact = EnsureDioramaDofSourceTexture(
      device, snes_width, snes_height);
  if (!ArRenderTexture_IsValid(compact)) {
    if (outcome) *outcome = kPresentationOutcome_OptionalOmitted;
    return ArRenderTexture_Invalid();
  }
  ArRenderTargetState target_state;
  const ArRenderTargetBeginResult begin = ArRenderDevice_BeginTarget(
      device, compact, &target_state);
  if (begin != kArRenderTargetBegin_Ready) {
    if (outcome) {
      *outcome = begin == kArRenderTargetBegin_StateLost
          ? kPresentationOutcome_CoreFailure
          : kPresentationOutcome_OptionalOmitted;
    }
    if (begin == kArRenderTargetBegin_Omitted)
      DisableDioramaDofSource(device);
    return ArRenderTexture_Invalid();
  }
  bool success = ArRenderDevice_Clear(
      device, (ArRenderColorF){0.0f, 0.0f, 0.0f, 0.0f});
  const ArRenderRectF src = {
    (float)obj_apron, 0.0f, (float)snes_width, (float)snes_height,
  };
  const ArRenderRectF dst = {
    0.0f, 0.0f, (float)snes_width, (float)snes_height,
  };
  if (success) {
    const ArRenderDrawState draw_state = {
      .flags = kArRenderDrawState_Blend,
      .blend = kArRenderBlendMode_Opaque,
    };
    success = ArRenderDevice_DrawTextureWithState(
        device, source, &src, &dst, &draw_state);
  }
  if (!ArRenderDevice_EndTarget(device, &target_state)) {
    if (outcome) *outcome = kPresentationOutcome_CoreFailure;
    return ArRenderTexture_Invalid();
  }
  if (!success && outcome)
    *outcome = kPresentationOutcome_OptionalOmitted;
  if (!success)
    DisableDioramaDofSource(device);
  return success ? compact : ArRenderTexture_Invalid();
}

/* Camera input is supplied by the caller, including automatic framing. */
static const float kDioramaFovY = 0.4f;
static const float kDioramaDistMin = 2.0f;
typedef Scene3DCamera DioramaCamera;

static float Clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/* ── Layer table ─────────────────────────────────────────────────────── */

typedef struct DioramaLayerDesc {
  int plane;          /* kDioramaPlane_* / SR_PPU_OVERLAY_* index */
  float z;
  ArRenderColorF shade;
  bool is_figure;
  bool casts_shadow;  /* see the kDioramaLayers interaction policy below */
} DioramaLayerDesc;

/* Table order IS the draw order (painter's algorithm) and, ignoring the two
 * normally empty virtual-far slots immediately before their anchors, mirrors
 * the SNES Mode-1 priority stack exactly (the z-rank table in ppu.c
 * PpuDrawBackgrounds), so occlusion matches hardware: priority-1 tiles cover
 * priority-2 sprites, priority-0/1 sprites hide behind the playfield, and so
 * on. The ordinary/high pair stays nearly co-planar; only an explicitly
 * classified virtual-far surface separates art from its anchor in depth. All
 * four sprite bands share one depth. BG3 stays one plane: ActRaiser action
 * HUDs ride the $2105 quirk rank in front of everything.
 *
 * The BG1 and BG2 families do not cast drop shadows. The shadow pass has no
 * receiver mask: it paints an offset silhouette over everything already drawn,
 * so a BG1 shadow necessarily darkens BG2/the sky rather than a meaningful
 * receiving surface. Figure planes retain shadows against the background
 * stack; BG3 keeps its existing policy. */
static const DioramaLayerDesc kDioramaLayers[] = {
  { kDioramaPlane_Backdrop, 0.00f, { 0.70f, 0.70f, 0.80f, 1.0f },
    false, false },
  { SR_PPU_OVERLAY_OBJ,  0.51f, { 1.0f,  1.0f,  1.0f,  1.0f },   /* prio 0 */
    true, true },
  { kDioramaPlane_Obj1,     0.51f, { 1.0f,  1.0f,  1.0f,  1.0f },
    true, true },
  { kDioramaPlane_Bg2Far,   0.05f, { 0.82f, 0.82f, 0.88f, 1.0f },
    false, false },
  { SR_PPU_OVERLAY_BG2,  0.20f, { 0.82f, 0.82f, 0.88f, 1.0f },   /* prio 0 */
    false, false },
  { kDioramaPlane_Bg1Far,   0.35f, { 0.92f, 0.92f, 0.95f, 1.0f },
    false, false },
  { SR_PPU_OVERLAY_BG1,  0.50f, { 0.92f, 0.92f, 0.95f, 1.0f },   /* prio 0 */
    false, false },
  { kDioramaPlane_Obj2,     0.51f, { 1.0f,  1.0f,  1.0f,  1.0f },
    true, true },
  { kDioramaPlane_Bg2Hi,    0.21f, { 0.82f, 0.82f, 0.88f, 1.0f },
    false, false },
  { kDioramaPlane_Bg1Hi,    0.51f, { 0.92f, 0.92f, 0.95f, 1.0f },
    false, false },
  { kDioramaPlane_Obj3,     0.52f, { 1.0f,  1.0f,  1.0f,  1.0f },
    true, true },
  { SR_PPU_OVERLAY_BG3,  0.95f, { 1.0f,  1.0f,  1.0f,  1.0f },
    false, true },
};
/* An ENUM, not a `static const int`. In C a const object is not an integer
 * constant expression, so using one as an array extent silently produces a
 * VARIABLE-LENGTH ARRAY -- which is what the two resolved-layer arrays in the
 * per-frame draw path were, allocating on the stack every frame with no bound
 * the compiler could check. `-Wvla` reports it, but -Wall -Wextra do NOT imply
 * -Wvla, so the build was silent about it.
 *
 * The neighbouring vertex buffers in the same function avoided this by using
 * #define extents (DIORAMA_VERTS_PER_LAYER); an enum gets the same guarantee
 * while keeping the count derived from the table rather than restated. */
enum {
  kDioramaLayerCount = (int)(sizeof(kDioramaLayers) / sizeof(kDioramaLayers[0]))
};

static const DioramaLayerDesc *DioramaDescForPlane(int plane) {
  for (int i = 0; i < kDioramaLayerCount; i++)
    if (kDioramaLayers[i].plane == plane) return &kDioramaLayers[i];
  return NULL;
}

static bool DeathHeimFaceScene(const DioramaScene *scene) {
  return scene->map_group == kActRaiserMapGroup_DeathHeim &&
      scene->map_number == kActRaiserDeathHeimMap_Hub &&
      scene->layer_section == kDioramaLayerSection_Room;
}

static bool SkyboxReplacesPlane(const DioramaScene *scene, int plane) {
  /* Room 1 A's far band contains the faces and their captured eye sprites.
   * Ordinary BG2 is the independently scrolling water below those faces. */
  return scene->render->skybox == kDioramaSky_Only &&
      !(DeathHeimFaceScene(scene) && plane == SR_PPU_OVERLAY_BG2);
}

/* Projection publication and drawing must describe the same frame. Keeping
 * every visibility/resource gate here prevents a hidden or unuploaded plane
 * from remaining projectable to presentation effects. */
static bool DioramaLayerIsDrawable(
    const DioramaLayerDesc *layer, const ArRenderTexture textures[],
    const uint8_t *const pixels[], const DioramaScene *scene) {
  const bool guarded=layer&&scene->voxel_guard&&scene->voxel_guard->capture_generation&&
      ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number)&&
      scene->voxel_guard->source_revision==ActionVoxel_Revision(scene->voxel_geometry)&&
      (layer->plane==SR_PPU_OVERLAY_BG1||layer->plane==kDioramaPlane_Bg1Hi||layer->plane==kDioramaPlane_Bg1Far)&&
      ArRenderTexture_IsValid(scene->voxel_guard->textures[layer->plane==SR_PPU_OVERLAY_BG1?0:layer->plane==kDioramaPlane_Bg1Hi?1:2]);
  return layer && Diorama_PlaneEligible(
      layer->plane, (scene->render->visible_planes & (1u << layer->plane)) != 0,
      ArRenderTexture_IsValid(textures[layer->plane])||guarded,
      pixels[layer->plane] != NULL||guarded,
      scene->render->hud_flat,
      SkyboxReplacesPlane(scene, layer->plane),
      scene->additive_plane_mask);
}

/* A BG or OBJ plane may additionally be requested by a current captured
 * effect: its metadata is current content even if the isolated hardware band
 * contributed no final pixels. A content-bearing upload failure is filtered
 * by present.c before it reaches this predicate. */
static bool DioramaLayerIsProjectable(
    const DioramaLayerDesc *layer, const ArRenderTexture textures[],
    const uint8_t *const pixels[], const DioramaScene *scene) {
  if (!layer) return false;
  const int priority = DioramaPlaneObjectPriority(layer->plane);
  const bool has_obj_effect = priority >= 0 &&
      (scene->effect_obj_priority_mask & (1u << (unsigned)priority)) != 0;
  const bool has_bg_effect = layer->plane >= 0 && layer->plane < 32 &&
      (scene->effect_bg_plane_mask & (1u << (unsigned)layer->plane)) != 0;
  return Diorama_PlaneProjectable(
      layer->plane, (scene->render->visible_planes & (1u << layer->plane)) != 0,
      ArRenderTexture_IsValid(textures[layer->plane]),
      pixels[layer->plane] != NULL,
      has_obj_effect || has_bg_effect, scene->render->hud_flat,
      SkyboxReplacesPlane(scene, layer->plane),
      scene->additive_plane_mask);
}

/* ── 3D projection ───────────────────────────────────────────────────── */

#define DIORAMA_SUBDIV_X 8
#define DIORAMA_SUBDIV_Y kDioramaPlaneSubdivY
/* Curved wall capacity; ordinary planes retain their original 8x6 grid. */
#define HOLLOW_COLUMNS 43
#define HOLLOW_ROWS 10
#define DIORAMA_VERTS_PER_LAYER (HOLLOW_COLUMNS * HOLLOW_ROWS)
#define DIORAMA_INDICES_PER_LAYER ((HOLLOW_COLUMNS - 1) * (HOLLOW_ROWS-1) * 6)
#define DIORAMA_EDGE_BOUNDARY_POINTS \
  (2 * (DIORAMA_SUBDIV_X + DIORAMA_SUBDIV_Y))
#define DIORAMA_EDGE_FRINGE_VERTS (2 * DIORAMA_EDGE_BOUNDARY_POINTS)
#define DIORAMA_EDGE_FRINGE_INDICES (6 * DIORAMA_EDGE_BOUNDARY_POINTS)
#define DIORAMA_AA_VERTS \
  (DIORAMA_VERTS_PER_LAYER + DIORAMA_EDGE_FRINGE_VERTS)
#define DIORAMA_AA_INDICES \
  (DIORAMA_INDICES_PER_LAYER + DIORAMA_EDGE_FRINGE_INDICES)
/* One extra interval gives the curved continuation a dedicated row at its
 * non-uniform handoff, while retaining six intervals for the visible bend. */
#define DIORAMA_OVERFLOW_SUBDIV_Y (DIORAMA_SUBDIV_Y + 1)
#define DIORAMA_OVERFLOW_VERTS \
  ((DIORAMA_SUBDIV_X + 1) * (DIORAMA_OVERFLOW_SUBDIV_Y + 1))
#define DIORAMA_OVERFLOW_INDICES \
  (DIORAMA_SUBDIV_X * DIORAMA_OVERFLOW_SUBDIV_Y * 6)
#define DIORAMA_ATTACHED_VERTS \
  (DIORAMA_OVERFLOW_VERTS + DIORAMA_VERTS_PER_LAYER)
#define DIORAMA_ATTACHED_INDICES \
  (DIORAMA_OVERFLOW_INDICES + DIORAMA_INDICES_PER_LAYER)
#define DIORAMA_ATTACHED_AA_VERTS \
  (DIORAMA_ATTACHED_VERTS + DIORAMA_EDGE_FRINGE_VERTS)
#define DIORAMA_ATTACHED_AA_INDICES \
  (DIORAMA_ATTACHED_INDICES + DIORAMA_EDGE_FRINGE_INDICES)

/* Presentation is synchronous on one render thread. Keep the largest voxel
 * batch out of automatic storage: 24 copies are roughly 75 KiB of vertices
 * and indices before the scaled target copy. */
static ArRenderVertex2D
    s_diorama_stack_vertices[kDioramaVoxelMax * DIORAMA_VERTS_PER_LAYER];
static ArRenderVertex2D
    s_diorama_stack_target_vertices[
        kDioramaVoxelMax * DIORAMA_VERTS_PER_LAYER];
static int32_t
    s_diorama_stack_indices[kDioramaVoxelMax * DIORAMA_INDICES_PER_LAYER];

static void BuildViewProjection(const DioramaCamera *cam, int out_w, int out_h,
                                float out_mat[16]) {
  Scene3D_BuildViewProjection(cam, out_w, out_h, out_mat);
}

/* GEO (followup doc, shared prereq for B5/B6): the projection kernel
 * (world xyz -> clip -> perspective divide -> viewport pixel), factored out
 * of what was BuildLayerMesh's inline per-vertex math so B5's skybox quad
 * and B6's floor/ceiling/wall quads can share it. Pure function of its
 * inputs — calling it with the same (mvp, x, y, z, screen_w, screen_h) as
 * the inlined version always produced is bit-for-bit identical to before;
 * only the CALLER'S world-coordinate formula matters for byte-identical
 * output, and BuildLayerMesh's is left untouched below (verified: an
 * algebraically-equivalent but differently-associated rewrite of its
 * `(s - 0.5f) * aspect_x` does NOT reproduce the same float32 rounding in
 * ~14% of cases — checked numerically before this refactor). */
static bool ProjectWorldPoint(const float mvp[16], float x, float y, float z,
                              int screen_w, int screen_h,
                              ArRenderPointF *out_point) {
  Scene3DPoint point;
  if (!Scene3D_ProjectWorldPoint(mvp, x, y, z, screen_w, screen_h,
                                 &point))
    return false;
  *out_point = (ArRenderPointF){ point.x, point.y };
  return true;
}

static bool ProjectWorldPointDepth(const float mvp[16],float x,float y,float z,
    int width,int height,ArRenderPointF *out,float *depth) {
  if(!depth)return ProjectWorldPoint(mvp,x,y,z,width,height,out);
  Scene3DPoint p;
  if(!Scene3D_ProjectWorldPointWithDepth(mvp,x,y,z,width,height,&p,depth))return false;
  *out=(ArRenderPointF){p.x,p.y};return true;
}

/* Split a regular vertex grid into two triangles per cell. */
static void TriangulateGrid(int subdiv_u, int subdiv_v, int32_t *out_indices,
                            int *num_indices) {
  int ii = 0, cols = subdiv_u + 1;
  for (int row = 0; row < subdiv_v; row++) {
    for (int col = 0; col < subdiv_u; col++) {
      int tl = row * cols + col;
      out_indices[ii++] = tl;
      out_indices[ii++] = tl + 1;
      out_indices[ii++] = tl + cols;
      out_indices[ii++] = tl + 1;
      out_indices[ii++] = tl + cols + 1;
      out_indices[ii++] = tl + cols;
    }
  }
  *num_indices = ii;
}

/* Assemble the pure rake/bow row depths into the projected layer mesh. */
static void BuildLayerMesh(const float mvp[16], float z_world, float z_rake,
                           float z_bow,
                           float u0, float v0, float u1, float v1,
                           float aspect_x, float height_scale,
                           int screen_w, int screen_h,
                           ArRenderColorF color,
                           ArRenderVertex2D *out_verts, float *out_depths, int32_t *out_indices,
                           int *num_verts, int *num_indices) {
  DioramaPerformanceScope performance =
      DioramaPerformance_Begin(kDioramaPerformance_Mesh);
  *num_verts = 0;
  *num_indices = 0;
  int vi = 0;
  for (int row = 0; row <= DIORAMA_SUBDIV_Y; row++) {
    for (int col = 0; col <= DIORAMA_SUBDIV_X; col++) {
      float s = (float)col / DIORAMA_SUBDIV_X;
      float t = (float)row / DIORAMA_SUBDIV_Y;
      float wx = (s - 0.5f) * aspect_x;
      float wy = (0.5f - t) * height_scale;
      /* Linear rake plus eased bow; returns z_world untouched when both are zero,
       * so an unauthored layer is bit-identical to what it always was. */
      float wz = DioramaTiltedRowDepth(z_world, z_rake, z_bow, t);
      if (!ProjectWorldPointDepth(mvp, wx, wy, wz, screen_w, screen_h,
                             &out_verts[vi].position,out_depths?out_depths+vi:NULL)) {
        DioramaPerformance_End(performance);
        return;
      }
      out_verts[vi].tex_coord = (ArRenderPointF){ u0 + s * (u1 - u0),
                                              v0 + t * (v1 - v0) };
      out_verts[vi].color = color;
      vi++;
    }
  }
  *num_verts = vi;
  TriangulateGrid(DIORAMA_SUBDIV_X, DIORAMA_SUBDIV_Y, out_indices, num_indices);
  DioramaPerformance_End(performance);
}

/* Assemble a shaded skirt from the pure thickness geometry. v_bottom follows
 * the live source edge so the fold remains attached during scrolling. */
static void BuildLayerSkirtMesh(const float mvp[16], float z_world,
                                float z_rake, float thickness,
                                float u0, float u1, float v_bottom,
                                float aspect_x, float height_scale,
                                int screen_w, int screen_h,
                                ArRenderColorF color,
                                ArRenderVertex2D *out_verts,
                                int32_t *out_indices,
                                int *num_verts, int *num_indices) {
  DioramaPerformanceScope performance =
      DioramaPerformance_Begin(kDioramaPerformance_Mesh);
  *num_verts = 0;
  *num_indices = 0;
  /* The plane's bottom edge is where the skirt starts, so it inherits the rake's
   * bottom depth — otherwise a room authoring BOTH would tear at the fold. */
  const float z_top = z_world + z_rake;
  const float y_top = -0.5f * height_scale;
  int vi = 0;
  for (int row = 0; row <= DIORAMA_SUBDIV_Y; row++) {
    for (int col = 0; col <= DIORAMA_SUBDIV_X; col++) {
      float s = (float)col / DIORAMA_SUBDIV_X;
      float t = (float)row / DIORAMA_SUBDIV_Y;
      float wx = (s - 0.5f) * aspect_x;
      /* Pure depth-shape arithmetic is unit-tested separately; this loop only
       * assembles and projects it. */
      float wy = 0.0f, wz = 0.0f, shade_mul = 1.0f;
      DioramaSkirtVertex(t, z_top, y_top, thickness, &wy, &wz, &shade_mul);
      if (!ProjectWorldPoint(mvp, wx, wy, wz, screen_w, screen_h,
                             &out_verts[vi].position)) {
        DioramaPerformance_End(performance);
        return;
      }
      /* Bottom source row, repeated down the whole skirt. */
      out_verts[vi].tex_coord = (ArRenderPointF){ u0 + s * (u1 - u0), v_bottom };
      ArRenderColorF c = color;
      c.r *= shade_mul;
      c.g *= shade_mul;
      c.b *= shade_mul;
      out_verts[vi].color = c;
      vi++;
    }
  }
  *num_verts = vi;
  TriangulateGrid(DIORAMA_SUBDIV_X, DIORAMA_SUBDIV_Y, out_indices, num_indices);
  DioramaPerformance_End(performance);
}

/* General world-space quad mesh builder, lerped from a corner + two edge
 * vectors. Used by DrawDioramaShoebox (gated by scene->render->shoebox)
 * to build the floor/ceiling/side-wall quads, which vary axis pairs that
 * BuildLayerMesh cannot (it hardcodes a constant z_world and only varies
 * X/Y). Kept deliberately separate from BuildLayerMesh's own formula rather
 * than routing BuildLayerMesh through it, since the two aren't bit-identical
 * (see ProjectWorldPoint's comment). */
static void BuildQuadMesh(const float mvp[16],
                          float origin_x, float origin_y, float origin_z,
                          float edge_u_x, float edge_u_y, float edge_u_z,
                          float edge_v_x, float edge_v_y, float edge_v_z,
                          float u0, float v0, float u1, float v1,
                          int subdiv_u, int subdiv_v,
                          int screen_w, int screen_h, ArRenderColorF color,
                          ArRenderVertex2D *out_verts,
                          int32_t *out_indices,
                          int *num_verts, int *num_indices) {
  DioramaPerformanceScope performance =
      DioramaPerformance_Begin(kDioramaPerformance_Mesh);
  *num_verts = 0;
  *num_indices = 0;
  int vi = 0;
  for (int row = 0; row <= subdiv_v; row++) {
    for (int col = 0; col <= subdiv_u; col++) {
      float s = (float)col / subdiv_u;
      float t = (float)row / subdiv_v;
      float wx = origin_x + s * edge_u_x + t * edge_v_x;
      float wy = origin_y + s * edge_u_y + t * edge_v_y;
      float wz = origin_z + s * edge_u_z + t * edge_v_z;
      if (!ProjectWorldPoint(mvp, wx, wy, wz, screen_w, screen_h,
                             &out_verts[vi].position)) {
        DioramaPerformance_End(performance);
        return;
      }
      out_verts[vi].tex_coord = (ArRenderPointF){ u0 + s * (u1 - u0),
                                              v0 + t * (v1 - v0) };
      out_verts[vi].color = color;
      vi++;
    }
  }
  *num_verts = vi;
  TriangulateGrid(subdiv_u, subdiv_v, out_indices, num_indices);
  DioramaPerformance_End(performance);
}

/* AR_AITOS_WATERFALL_LOG=1 draw-side twin of action_effects.c's capture line.
 * Together the two localise any dropout: a capture line reading veil=no with no
 * matching draw line means the section never reached the renderer, while
 * section=waterfall with ext=0 means it did and the geometry gate rejected it.
 * Logs on CHANGE so a full jump stays readable. */
static void DioramaAitosWaterfallLog(bool log_on, uint8_t map_group, uint8_t map_number,
                                     int layer_section, bool eligible,
                                     int extension_nv, int authentic_y0,
                                     int capture_height, int drawable_y1,
                                     float fold_t, float overlap_t) {
  if (!log_on) return;
  /* Quantise the floats: they carry a sub-tick interpolation shift that would
   * otherwise make every frame a "change" and defeat the whole point. */
  const int fold_key = (int)(fold_t * 1000.0f);
  const int overlap_key = (int)(overlap_t * 1000.0f);
  static int last_key[6] = {-2, -2, -2, -2, -2, -2};
  const int key[6] = {
    layer_section, extension_nv > 0, authentic_y0, drawable_y1,
    fold_key, overlap_key,
  };
  if (!memcmp(key, last_key, sizeof key)) return;
  memcpy(last_key, key, sizeof key);
  fprintf(stderr,
          "[aitos-wf] draw map=$%02X/$%02X section=%d eligible=%d ext_nv=%d "
          "top=%d cap_h=%d drawable_y1=%d fold_t=%.4f overlap_t=%.4f\n",
          map_group, map_number, layer_section, eligible ? 1 : 0, extension_nv,
          authentic_y0, capture_height, drawable_y1,
          (double)fold_t, (double)overlap_t);
}

/* Curved continuation for a finite captured plane. Unlike BuildQuadMesh, each
 * row follows DioramaOverflowFoldPoint, so the surface can stay coplanar under
 * the host's lower margin and then turn toward the camera. The same pure path
 * is consumed by diorama_projection.c for BG-local particles and lighting. */
static void BuildFoldedOverflowMesh(
    const float mvp[16],
    float y_top, float z_top, float z_handoff,
    float overflow_height, float overlap_t,
    float front_z, float front_drop,
    float u0, float v0, float u1, float v1,
    float aspect_x, int screen_w, int screen_h, ArRenderColorF color,
    ArRenderVertex2D *out_verts, int32_t *out_indices,
    int *num_verts, int *num_indices) {
  DioramaPerformanceScope performance =
      DioramaPerformance_Begin(kDioramaPerformance_Mesh);
  *num_verts = 0;
  *num_indices = 0;
  int vi = 0;
  for (int row = 0; row <= DIORAMA_OVERFLOW_SUBDIV_Y; row++) {
    const float t = DioramaOverflowFoldRowT(
        row, DIORAMA_OVERFLOW_SUBDIV_Y, overlap_t);
    float wy = 0.0f, wz = 0.0f;
    DioramaOverflowFoldPoint(
        t, y_top, z_top, z_handoff,
        overflow_height, overlap_t, front_z, front_drop,
        &wy, &wz);
    for (int col = 0; col <= DIORAMA_SUBDIV_X; col++) {
      const float s = (float)col / DIORAMA_SUBDIV_X;
      const float wx = (s - 0.5f) * aspect_x;
      if (!ProjectWorldPoint(mvp, wx, wy, wz, screen_w, screen_h,
                             &out_verts[vi].position)) {
        DioramaPerformance_End(performance);
        return;
      }
      out_verts[vi].tex_coord = (ArRenderPointF){
        u0 + s * (u1 - u0), v0 + t * (v1 - v0),
      };
      out_verts[vi].color = color;
      vi++;
    }
  }
  *num_verts = vi;
  TriangulateGrid(
      DIORAMA_SUBDIV_X, DIORAMA_OVERFLOW_SUBDIV_Y,
      out_indices, num_indices);
  DioramaPerformance_End(performance);
}

/* The supersample target contains only the active captured rectangle, while
 * ordinary layer geometry addresses the larger persistent layer texture.
 * Remap any mesh that will sample the compact target. Keeping this shared is
 * what lets auxiliary geometry such as the waterfall continuation use the
 * exact same resolved source as its host plane rather than silently falling
 * back to the raw texture. */
static void RemapMeshToCompactTexture(ArRenderVertex2D *vertices, int count,
                                      int obj_apron,
                                      int snes_width, int snes_height) {
  const float u_scale = (float)SR_PPU_SURFACE_MAX_WIDTH / (float)snes_width;
  const float u_bias = (float)obj_apron / (float)snes_width;
  const float v_scale = (float)SR_PPU_SURFACE_MAX_HEIGHT / (float)snes_height;
  for (int v = 0; v < count; v++) {
    vertices[v].tex_coord.x = vertices[v].tex_coord.x * u_scale - u_bias;
    vertices[v].tex_coord.y *= v_scale;
  }
}

static bool AppendDioramaEdgeFringe(
    const ArRenderVertex2D *grid, const float *grid_depths, float width_pixels,
    DioramaEdgeAaMask edge_mask,
    float u_min, float u_max, float v_min, float v_max,
    float texel_width, float texel_height,
    ArRenderVertex2D *vertices, float *depths, int vertex_capacity, int *vertex_count,
    int32_t *indices, int index_capacity, int *index_count) {
  if (!grid || !vertices || !vertex_count || !indices || !index_count ||
      *vertex_count < 0 || *index_count < 0 ||
      *vertex_count > vertex_capacity || *index_count > index_capacity)
    return false;
  const int vertex_base = *vertex_count;
  const int index_base = *index_count;
  int fringe_vertices = 0;
  int fringe_indices = 0;
  if (!DioramaEdgeAa_BuildFringeDepth(
          grid, grid_depths, DIORAMA_SUBDIV_X, DIORAMA_SUBDIV_Y,
          width_pixels, edge_mask,
          u_min, u_max, v_min, v_max, texel_width, texel_height,
          &vertices[vertex_base], depths?depths+vertex_base:NULL, vertex_capacity - vertex_base,
          &indices[index_base], index_capacity - index_base,
          &fringe_vertices, &fringe_indices))
    return false;
  for (int index = 0; index < fringe_indices; index++)
    indices[index_base + index] += vertex_base;
  *vertex_count += fringe_vertices;
  *index_count += fringe_indices;
  return true;
}

/* ── Render ───────────────────────────────────────────────────────────── */

/* M5 (D6/buffer-ownership split): upload remains separate from composite even
 * though presentation is now synchronous. The boundary keeps texture ownership
 * and the producer snapshot explicit. The caller supplies the frame-snapshotted
 * request/content intersection; this function neither reads live settings nor
 * rescans producer-owned pixels. */
static bool SubmitDioramaGeometry(
    ArRenderDevice *device, ArRenderTexture texture,
    const ArRenderVertex2D *vertices, int num_vertices,
    const int32_t *indices, int num_indices,
    const ArRenderDrawState *state) {
  DioramaPerformanceScope performance =
      DioramaPerformance_Begin(kDioramaPerformance_Submit);
  const bool succeeded = ArRenderDevice_DrawGeometryWithState(
      device, texture, vertices, num_vertices, indices, num_indices, state);
  DioramaPerformance_End(performance);
  DioramaPerformance_AddDraw(
      succeeded, vertices, num_vertices, indices, num_indices,
      state && (state->flags & kArRenderDrawState_Blend)
          ? state->blend : kArRenderBlendMode_Alpha);
  return succeeded;
}

static bool RenderDioramaGeometry(
    ArRenderDevice *device, ArRenderTexture texture,
    const ArRenderVertex2D *vertices, int num_vertices,
    const int32_t *indices, int num_indices, ArRenderBlendMode blend) {
  const ArRenderDrawState state = {
    .flags = kArRenderDrawState_Blend |
        (ArRenderTexture_IsValid(texture)
            ? kArRenderDrawState_Address : 0),
    .blend = blend,
    .address_u = kArRenderTextureAddressMode_Clamp,
    .address_v = kArRenderTextureAddressMode_Clamp,
  };
  return SubmitDioramaGeometry(
      device, texture, vertices, num_vertices, indices, num_indices, &state);
}

typedef struct DioramaShadingSubmit {
  ArRenderDevice *device;
  ArRenderTexture texture;
  ArRenderBlendMode blend;
  DioramaPerformanceScope performance;
} DioramaShadingSubmit;

static bool SubmitShadedDioramaGeometry(void *context,
    const ArRenderVertex2D *vertices,int nv,const int32_t *indices,int ni) {
  DioramaShadingSubmit *draw=context;
  DioramaPerformance_End(draw->performance);
  const bool ok=RenderDioramaGeometry(draw->device,draw->texture,vertices,nv,indices,ni,draw->blend);
  draw->performance=DioramaPerformance_Begin(kDioramaPerformance_Mesh);
  return ok;
}

static bool RenderShadedDioramaGeometry(
    ArRenderDevice *device, ArRenderTexture texture,
    const ArRenderVertex2D *vertices, int nv, const int32_t *indices, int ni,
    ArRenderBlendMode blend, const SceneryShadingField *field, ArRenderRectF source_uv,
    const DioramaSceneryLightEffectParams *lighting) {
  const bool lit = lighting && ArRenderTexture_IsValid(lighting->illumination);
  if (lit && !DioramaEffectBackend_BindSceneryLight(device, lighting)) {
    (void)DioramaEffectBackend_Unbind(device);
    return false;
  }
  DioramaShadingSubmit draw={device,texture,blend,DioramaPerformance_Begin(kDioramaPerformance_Mesh)};
  const bool ok=SceneryShading_Draw(field,source_uv,vertices,nv,indices,ni,
      SubmitShadedDioramaGeometry,&draw);
  DioramaPerformance_End(draw.performance);
  const bool restored = !lit || DioramaEffectBackend_Unbind(device);
  return ok && restored;
}

typedef struct DioramaDepthShadingSubmit {
  ArRenderDevice *device;
  const DioramaDepthSink *sink;
  DioramaDepthSubmission draw;
  DioramaPerformanceScope performance;
} DioramaDepthShadingSubmit;
static bool SubmitShadedDioramaDepth(void *context,const ArRenderVertex2D *v,
    const float *depths,int nv,const int32_t *indices,int ni) {
  DioramaDepthShadingSubmit *s=context;
  s->draw.vertices=v;s->draw.depths=depths;s->draw.vertex_count=nv;
  s->draw.indices=indices;s->draw.index_count=ni;
  DioramaPerformance_End(s->performance);
  const DioramaPerformanceScope submitted=DioramaPerformance_Begin(kDioramaPerformance_Submit);
  const bool ok=s->sink->submit(s->sink->userdata,s->device,&s->draw);
  DioramaPerformance_End(submitted);
  DioramaPerformance_AddDraw(ok,v,nv,indices,ni,s->draw.blend);
  s->performance=DioramaPerformance_Begin(kDioramaPerformance_Mesh);
  return ok;
}
static bool RenderShadedDioramaDepthGeometryWithReceiver(ArRenderDevice *device,ArRenderTexture texture,
    const ArRenderVertex2D *v,const float *depths,const float *clip_w,const ArRenderPointF *heat_world_xy,int nv,const int32_t *indices,int ni,
    ArRenderBlendMode blend,const SceneryShadingField *field,ArRenderRectF source_uv,
    const DioramaSceneryLightEffectParams *lighting,const DioramaDepthSink *sink,
    int plane,bool native_actor) {
  if(!sink)return RenderShadedDioramaGeometry(device,texture,v,nv,indices,ni,blend,field,source_uv,lighting);
  if(!sink->submit || !depths)return false;
  /* Generated caps are clipped in world/source space before projection and
   * already carry vertex lighting. SceneryShading may split affine meshes;
   * never pass an unmodified W sidecar through that separate clipping path. */
  if((clip_w && (field || native_actor)) || (heat_world_xy && !clip_w))return false;
  DioramaDepthShadingSubmit submit={.device=device,.sink=sink,
      .draw={.texture=texture,.blend=blend,.lighting=lighting,.plane=plane,
          .native_actor=native_actor,.clip_w=clip_w,.heat_world_xy=heat_world_xy},
      .performance=DioramaPerformance_Begin(kDioramaPerformance_Mesh)};
  const bool ok=clip_w?SubmitShadedDioramaDepth(&submit,v,depths,nv,indices,ni):
      SceneryShading_DrawDepth(field,source_uv,v,depths,nv,indices,ni,SubmitShadedDioramaDepth,&submit);
  DioramaPerformance_End(submit.performance);
  return ok;
}
static bool RenderShadedDioramaDepthGeometryWithW(ArRenderDevice *device,ArRenderTexture texture,
    const ArRenderVertex2D *v,const float *depths,const float *clip_w,int nv,const int32_t *indices,int ni,
    ArRenderBlendMode blend,const SceneryShadingField *field,ArRenderRectF source_uv,
    const DioramaSceneryLightEffectParams *lighting,const DioramaDepthSink *sink,
    int plane,bool native_actor) {
  return RenderShadedDioramaDepthGeometryWithReceiver(device,texture,v,depths,clip_w,NULL,nv,indices,ni,
      blend,field,source_uv,lighting,sink,plane,native_actor);
}
static bool RenderShadedDioramaDepthGeometry(ArRenderDevice *device,ArRenderTexture texture,
    const ArRenderVertex2D *v,const float *depths,int nv,const int32_t *indices,int ni,
    ArRenderBlendMode blend,const SceneryShadingField *field,ArRenderRectF source_uv,
    const DioramaSceneryLightEffectParams *lighting,const DioramaDepthSink *sink,
    int plane,bool native_actor) {
  return RenderShadedDioramaDepthGeometryWithW(device,texture,v,depths,NULL,nv,indices,ni,
      blend,field,source_uv,lighting,sink,plane,native_actor);
}

static void RecordOptionalDioramaDraw(
    PresentationOutcome *outcome, bool succeeded) {
  if (outcome && !succeeded)
    *outcome = PresentationOutcome_Combine(
        *outcome, kPresentationOutcome_OptionalOmitted);
}

/* Submit every non-redundant copy as one ordered geometry batch. Where the
 * projected cost justifies it, preserve that exact batch inside a reusable
 * half-output target and composite its premultiplied result once. The normal
 * front plane remains outside this group so its rim/DOF/supersample treatment
 * and crisp source resolution are unchanged. */
static PresentationOutcome RenderDioramaStackBatch(
    ArRenderDevice *device, ArRenderTexture source,
    const ArRenderVertex2D *vertices, int vertex_count,
    const int32_t *indices, int index_count,
    ArRenderBlendMode blend, const DioramaStackGroupPlan *plan,
    int output_width, int output_height, bool stack_grouping,
    const SceneryShadingField *exposure, const DioramaSceneryLightEffectParams *lighting,
    ArRenderRectF light_viewport) {
  if (!vertices || vertex_count <= 0 || !indices || index_count <= 0)
    return kPresentationOutcome_Complete;

  const bool use_group = stack_grouping && plan &&
      plan->use_intermediate;
  if (!use_group) {
    return RenderShadedDioramaGeometry(
               device, source, vertices, vertex_count, indices, index_count,
               blend, exposure, (ArRenderRectF){0,0,1,1}, lighting)
        ? kPresentationOutcome_Complete
        : kPresentationOutcome_OptionalOmitted;
  }

  const ArRenderTexture target = EnsureDioramaStackGroupTexture(
      device, plan->target_width, plan->target_height);
  if (!ArRenderTexture_IsValid(target)) {
    return RenderShadedDioramaGeometry(
               device, source, vertices, vertex_count, indices, index_count,
               blend, exposure, (ArRenderRectF){0,0,1,1}, lighting)
        ? kPresentationOutcome_Complete
        : kPresentationOutcome_OptionalOmitted;
  }

  ArRenderTargetState target_state;
  const ArRenderTargetBeginResult begin =
      ArRenderDevice_BeginTarget(device, target, &target_state);
  if (begin == kArRenderTargetBegin_StateLost)
    return kPresentationOutcome_CoreFailure;
  if (begin == kArRenderTargetBegin_Omitted) {
    DisableDioramaStackGroup(device);
    return RenderShadedDioramaGeometry(
               device, source, vertices, vertex_count, indices, index_count,
               blend, exposure, (ArRenderRectF){0,0,1,1}, lighting)
        ? kPresentationOutcome_Complete
        : kPresentationOutcome_OptionalOmitted;
  }

  memcpy(s_diorama_stack_target_vertices, vertices,
         (size_t)vertex_count * sizeof(vertices[0]));
  for (int i = 0; i < vertex_count; i++) {
    s_diorama_stack_target_vertices[i].position.x *= plan->scale_x;
    s_diorama_stack_target_vertices[i].position.y *= plan->scale_y;
  }
  DioramaPerformance_SetRasterViewport(
      plan->target_width, plan->target_height);
  bool target_ready = ArRenderDevice_Clear(
      device, (ArRenderColorF){0.0f, 0.0f, 0.0f, 0.0f});
  if (target_ready) {
    DioramaSceneryLightEffectParams target_lighting = *lighting;
    target_lighting.light_uv = light_viewport;
    target_ready = RenderShadedDioramaGeometry(
        device, source, s_diorama_stack_target_vertices, vertex_count,
        indices, index_count, blend, exposure, (ArRenderRectF){0,0,1,1}, &target_lighting);
  }
  const bool restored = ArRenderDevice_EndTarget(device, &target_state);
  DioramaPerformance_SetRasterViewport(output_width, output_height);
  if (!restored) return kPresentationOutcome_CoreFailure;
  if (!target_ready) {
    DisableDioramaStackGroup(device);
    return RenderShadedDioramaGeometry(
               device, source, vertices, vertex_count, indices, index_count,
               blend, exposure, (ArRenderRectF){0,0,1,1}, lighting)
        ? kPresentationOutcome_Complete
        : kPresentationOutcome_OptionalOmitted;
  }

  const ArRenderRectI bounds = plan->output_bounds;
  const float x0 = (float)bounds.x;
  const float y0 = (float)bounds.y;
  const float x1 = (float)(bounds.x + bounds.w);
  const float y1 = (float)(bounds.y + bounds.h);
  const float u0 = x0 / (float)output_width;
  const float v0 = y0 / (float)output_height;
  const float u1 = x1 / (float)output_width;
  const float v1 = y1 / (float)output_height;
  const ArRenderColorF white = {1.0f, 1.0f, 1.0f, 1.0f};
  const ArRenderVertex2D composite_vertices[] = {
    {{x0, y0}, white, {u0, v0}},
    {{x1, y0}, white, {u1, v0}},
    {{x1, y1}, white, {u1, v1}},
    {{x0, y1}, white, {u0, v1}},
  };
  const int32_t composite_indices[] = {0, 1, 2, 0, 2, 3};
  const ArRenderBlendMode composite_blend =
      blend == kArRenderBlendMode_Add
          ? kArRenderBlendMode_AddPremultiplied
          : kArRenderBlendMode_AlphaPremultiplied;
  return RenderDioramaGeometry(
             device, target, composite_vertices, 4,
             composite_indices, 6, composite_blend)
      ? kPresentationOutcome_Complete
      : kPresentationOutcome_OptionalOmitted;
}

/* Edge margin fix (live report 2026-07-21, fixed
 * 2026-07-26 behind the `diorama_margin_fix` setting / AR_DIORAMA_MARGIN_FIX).
 *
 * Symptom: near a level's start/end the captured BG2 content went black at the
 * world-bound edge instead of extending — a black wedge clipping the skybox.
 * Cause: ActRaiser_ApplyWidescreenPolicy narrows the LIVE per-side margin as the
 * camera reaches a finite world's bound, but every diorama consumer samples the
 * FIXED capture span, and the never-rendered columns are transparent — which
 * an opaque draw turns those samples into black.
 *
 * An earlier revision of this comment claimed there was "no cheap fix" and that
 * the only options were a per-layer numeric ceiling in PpuLayerExtra or a second
 * BG2-only scanout pass. Both were wrong. PpuLayerExtra returns 0 for any layer
 * carrying a clamp/mirror/repeat bit BEFORE it consults a numeric argument, so a
 * numeric ceiling is inert for the common case; and for most action maps BG2's
 * margins are not fetched from tilemap at all — they are SYNTHESIZED by
 * PpuMergePaddedBackground from the always-present authentic 256 columns, whose
 * two padding loops were simply bounded by the live margin instead of the
 * budget. Widening them for captured layers only (Fix A) costs no extra fetch
 * and no extra pass.
 *
 * Where synthesis does not apply — a genuinely wide BG2, or a clamped one — this
 * function crops its own U range to BG2's valid span instead (Fix B), trading a
 * slight sky stretch for the wedge. The framebuffer's own gap strips are filled
 * with the scene backdrop rather than black (Fix C, actraiser_rtl.c).
 *
 * Edge coverage no longer depends on this UV span: the compositor adds its
 * one-pixel fringe to the projected boundary after any crop is resolved. */

/* B5 (followup doc): draws BG2 as a viewport-FILLING screen-space quad —
 * deliberately NOT run through the camera MVP (BuildQuadMesh/
 * ProjectWorldPoint are for world-space geometry; a plain screen-rect quad
 * is the simplest of the doc's two suggested approaches and, unlike an
 * "oversized far-plane quad," mathematically cannot reveal an edge at any
 * tilt/yaw/zoom the free/dynamic cameras can reach). Dimmed via a FIXED
 * vertex color (not run through shade_mix/diorama_depth_shade — the doc's
 * explicit "independent of the depth-shade slider" call) and optionally
 * DoF'd with the existing blur shader. Must be called BEFORE the per-layer
 * loop (painter's algorithm: skybox is behind everything).
 *
 * `dim`: false in Skybox-only (live report, 2026-07-21) — there, BG2 is the
 * ENTIRE visible background (the caller also skips the backdrop layer in
 * that mode, see Diorama_Composite), so a dim/atmospheric tint just reads
 * as needlessly dark. Plane+skybox still wants it dim (atmosphere behind
 * the sharper in-box copy, not the focus) — but subtle (see kSkyboxDim).
 * `blur_radius`: caller-chosen per mode (live report, 2026-07-21) —
 * Skybox-only wants it barely soft (BG2 is the whole visible background
 * there, so heavy blur reads as "the picture is broken," not atmosphere);
 * Plane+skybox wants the fuller blur since the in-box copy stays sharp and
 * the skybox is deliberately meant to read as unfocused backdrop. */
static bool DioramaSkyboxPrefilterColorEquals(
    ArRenderColorF left, ArRenderColorF right) {
  return left.r == right.r && left.g == right.g &&
      left.b == right.b && left.a == right.a;
}

/* Evaluate the nine-tap skybox blur once per source texel, then let the final
 * viewport-filling draw sample that result with one ordinary texture lookup.
 * With a 256x256 ROM backdrop this moves the expensive shader from millions
 * of output fragments to 65K source fragments. Immutable/raw sources retain
 * the target until their revision changes; frame-generated interpolation is
 * explicitly dynamic and rebuilds it for each distinct host presentation. */
static ArRenderTexture BuildDioramaSkyboxPrefilter(
    ArRenderDevice *device, ArRenderTexture source,
    int source_width, int source_height,
    int output_width, int output_height,
    ArRenderColorF tint, float blur_radius, bool rom_source,
    uint64_t source_revision, bool source_dynamic,
    const DioramaSceneryGrade *grade, PresentationOutcome *outcome) {
  if (outcome) *outcome = kPresentationOutcome_Complete;
  if (!ArRenderDevice_IsReady(device) ||
      !ArRenderTexture_IsValid(source) ||
      source_width <= 0 || source_height <= 0 ||
      output_width <= 0 || output_height <= 0) {
    if (outcome) *outcome = kPresentationOutcome_CoreFailure;
    return ArRenderTexture_Invalid();
  }
  const ArRenderTexture target = EnsureDioramaSkyboxPrefilterTexture(
      device, source_width, source_height);
  if (!ArRenderTexture_IsValid(target)) {
    if (outcome) *outcome = kPresentationOutcome_OptionalOmitted;
    return ArRenderTexture_Invalid();
  }
  if (!source_dynamic && s_diorama_skybox_prefilter_valid &&
      ArRenderTexture_Equals(source, s_diorama_skybox_prefilter_source) &&
      source_revision == s_diorama_skybox_prefilter_revision &&
      blur_radius == s_diorama_skybox_prefilter_radius &&
      rom_source == s_diorama_skybox_prefilter_rom_source &&
      grade->strength == s_diorama_skybox_prefilter_grade.strength &&
      grade->saturation == s_diorama_skybox_prefilter_grade.saturation &&
      grade->shadows == s_diorama_skybox_prefilter_grade.shadows &&
      grade->highlights == s_diorama_skybox_prefilter_grade.highlights &&
      DioramaSkyboxPrefilterColorEquals(
          tint, s_diorama_skybox_prefilter_tint))
    return target;

  ArRenderTargetState target_state;
  const ArRenderTargetBeginResult begin =
      ArRenderDevice_BeginTarget(device, target, &target_state);
  if (begin == kArRenderTargetBegin_StateLost) {
    if (outcome) *outcome = kPresentationOutcome_CoreFailure;
    return ArRenderTexture_Invalid();
  }
  if (begin == kArRenderTargetBegin_Omitted) {
    DisableDioramaSkyboxPrefilter(device);
    if (outcome) *outcome = kPresentationOutcome_OptionalOmitted;
    return ArRenderTexture_Invalid();
  }

  DioramaPerformance_SetRasterViewport(source_width, source_height);
  bool prefiltered = ArRenderDevice_Clear(
      device, (ArRenderColorF){0.0f, 0.0f, 0.0f, 0.0f});
  bool shader_bound = false;
  bool shader_restored = true;
  if (prefiltered) {
    const DioramaBlurEffectParams params = {
      .texel_width = 1.0f / (float)source_width,
      .texel_height = 1.0f / (float)source_height,
      .radius = blur_radius,
    };
    if (grade->strength > 0) {
      const DioramaSceneryLightEffectParams material={.illumination=UnlitSceneryMap(device),
          .texel_width=params.texel_width,.texel_height=params.texel_height,
          .blur_radius=blur_radius,.mode=blur_radius>0?1:0,.shade={1,1,1,1},.grade=*grade};
      shader_bound=ArRenderTexture_IsValid(material.illumination) &&
          DioramaEffectBackend_BindSceneryLight(device,&material);
    } else shader_bound = DioramaEffectBackend_BindBlur(device, &params);
    prefiltered = shader_bound;
    if (!shader_bound)
      shader_restored = DioramaEffectBackend_Unbind(device);
  }
  if (prefiltered) {
    const int32_t indices[] = {0, 1, 2, 0, 2, 3};
    const ArRenderVertex2D vertices[] = {
      {{0.0f, 0.0f}, tint, {0.0f, 0.0f}},
      {{(float)source_width, 0.0f}, tint, {1.0f, 0.0f}},
      {{(float)source_width, (float)source_height}, tint, {1.0f, 1.0f}},
      {{0.0f, (float)source_height}, tint, {0.0f, 1.0f}},
    };
    ArRenderDrawState draw_state = {
      .flags = kArRenderDrawState_Blend,
      .blend = kArRenderBlendMode_Opaque,
    };
    if (rom_source) {
      draw_state.flags |= kArRenderDrawState_Address;
      draw_state.address_u = kArRenderTextureAddressMode_Wrap;
      draw_state.address_v = kArRenderTextureAddressMode_Clamp;
    }
    prefiltered = SubmitDioramaGeometry(
        device, source, vertices, 4, indices, 6, &draw_state);
  }
  if (shader_bound)
    shader_restored = DioramaEffectBackend_Unbind(device);
  const bool target_restored =
      ArRenderDevice_EndTarget(device, &target_state);
  DioramaPerformance_SetRasterViewport(output_width, output_height);
  if (!shader_restored || !target_restored) {
    if (outcome) *outcome = kPresentationOutcome_CoreFailure;
    return ArRenderTexture_Invalid();
  }
  if (!prefiltered) {
    DisableDioramaSkyboxPrefilter(device);
    if (outcome) *outcome = kPresentationOutcome_OptionalOmitted;
    return ArRenderTexture_Invalid();
  }

  s_diorama_skybox_prefilter_valid = true;
  s_diorama_skybox_prefilter_source = source;
  s_diorama_skybox_prefilter_revision = source_revision;
  s_diorama_skybox_prefilter_radius = blur_radius;
  s_diorama_skybox_prefilter_tint = tint;
  s_diorama_skybox_prefilter_grade = *grade;
  s_diorama_skybox_prefilter_rom_source = rom_source;
  return target;
}

/* A fixed background grid displaces UVs, not foreground pixels. The resident
 * shader uses the same samples after resolving generated-frame camera motion. */
enum { kHeatVertices=(kDioramaHeatColumns+1)*(kDioramaHeatRows+1),
       kHeatIndices=kDioramaHeatColumns*kDioramaHeatRows*6 };
static void HeatQuad(ArRenderVertex2D *vertices,int32_t *indices,
    const ArRenderVertex2D quad[4],const DioramaHeat *heat) {
  for(unsigned y=0;y<=kDioramaHeatRows;++y)for(unsigned x=0;x<=kDioramaHeatColumns;++x) {
    const float u=(float)x/kDioramaHeatColumns,v=(float)y/kDioramaHeatRows;
    ArRenderVertex2D *out=&vertices[y*(kDioramaHeatColumns+1)+x];
    *out=quad[0];out->position.x+=u*(quad[1].position.x-quad[0].position.x);
    out->position.y+=v*(quad[3].position.y-quad[0].position.y);
    out->tex_coord.x+=u*(quad[1].tex_coord.x-quad[0].tex_coord.x);
    out->tex_coord.y+=v*(quad[3].tex_coord.y-quad[0].tex_coord.y);
    if(heat)out->tex_coord=DioramaHeat_Uv(heat,out->tex_coord);
    out->tex_coord.x=fmaxf(quad[0].tex_coord.x,fminf(quad[1].tex_coord.x,out->tex_coord.x));
    out->tex_coord.y=fmaxf(quad[0].tex_coord.y,fminf(quad[3].tex_coord.y,out->tex_coord.y));
  }
  int count;
  TriangulateGrid(kDioramaHeatColumns,kDioramaHeatRows,indices,&count);
}
static ArRenderTexture CompositeSkyWeather(ArRenderDevice *device, ArRenderTexture source,
    int width, int height, const DioramaScene *scene, DioramaProjection *projection,
    bool *composited, PresentationOutcome *outcome) {
  *composited=false;
  const unsigned bands=projection->bg2_skybox.count?projection->bg2_skybox.count:1;
  bool visible=false;
  for (unsigned i=0;!visible&&i<bands;++i) {
    projection->bg2_skybox.active_band=projection->bg2_skybox.count?(int)i:-1;
    visible=scene->sky_weather(scene->plane_effect_userdata,projection,false);
  }
  if (!visible)return source;
  const ArRenderTexture target=EnsureDioramaScratchTarget(device,&s_diorama_sky_weather,
      width,height,kArRenderFilter_Nearest);
  ArRenderTargetState saved;
  if (!ArRenderTexture_IsValid(target) ||
      ArRenderDevice_BeginTarget(device,target,&saved)!=kArRenderTargetBegin_Ready) {
    *outcome=kPresentationOutcome_CoreFailure;return ArRenderTexture_Invalid();
  }
  const ArRenderDrawState state={.flags=kArRenderDrawState_Blend,.blend=kArRenderBlendMode_Opaque};
  const ArRenderRectF rect={0,0,width,height};
  bool ok=ArRenderDevice_DrawTextureWithState(device,source,&rect,&rect,&state);
  const DioramaSourceEffectBatch batch=scene->render->source_effect_batch;
  const bool began=ok&&(!batch||batch(device,true));
  ok=began;
  for (unsigned i=0;ok&&i<bands;++i) {
    projection->bg2_skybox.active_band=projection->bg2_skybox.count?(int)i:-1;
    ok=scene->sky_weather(scene->plane_effect_userdata,projection,true);
  }
  if (began&&batch)ok=batch(device,false)&&ok;
  const bool restored=ArRenderDevice_EndTarget(device,&saved);
  if (!ok||!restored) {
    *outcome=kPresentationOutcome_CoreFailure;return ArRenderTexture_Invalid();
  }
  *composited=true;
  return target;
}

/* Bake source-attached effects into an unwarped copy of a finite BG2 plane.
 * The normal face draw then applies the wall's perspective, heat and DoF to
 * artwork and particles together. The identity projection maps capture pixels
 * directly to texels, including the generated-camera offset and OBJ apron. */
static ArRenderTexture CompositePlaneWeather(ArRenderDevice *device, ArRenderTexture source,
    const DioramaScene *scene, DioramaProjection *projection, PresentationOutcome *outcome) {
  DioramaProjection flat=*projection;
  memset(flat.matrix,0,sizeof(flat.matrix));
  flat.matrix[0]=flat.matrix[5]=flat.matrix[10]=flat.matrix[15]=1;
  flat.aspect_x=flat.height_scale=2;
  flat.output_x=flat.output_y=0;
  flat.output_width=flat.texture_width;
  flat.output_height=flat.texture_height;
  flat.bg2_skybox=(DioramaSkyboxProjection){0};
  flat.bg2_plane=(DioramaPlaneProjection){.valid=true,
      .capture_offset=projection->bg2_plane.capture_offset,.u1=1,.v1=1};
  bool composited;
  source=CompositeSkyWeather(device,source,flat.texture_width,flat.texture_height,
      scene,&flat,&composited,outcome);
  projection->bg2_skybox.weather_composited|=composited;
  return source;
}

static PresentationOutcome DrawDioramaSkybox(
    ArRenderDevice *device, ArRenderTexture skybox_texture,
    int obj_apron, int snes_width, int snes_height,
    int out_w, int out_h, bool dim,
    float blur_radius, bool rom_source,
    uint64_t source_revision, bool source_dynamic,
    const DioramaBgValidSpanPlan *valid_spans,
    ArRenderPointF capture_offset, float subpixel_x, const float *span_shift_texels,
    const DioramaRasterRowShift *row_shift,
    int motion_source, bool follow_camera,
    int authentic_y0, float camera_delta, float pixel_aspect,
    float face_bottom_y, float foreground_top_y,
    DioramaSkyboxProjection *projection, const DioramaRenderOptions *options, const DioramaHeat *heat,
    const DioramaCloudLight *cloud_light, const DioramaSceneryGrade *grade,
    const DioramaScene *scene, DioramaProjection *effect_projection) {
  if (!ArRenderTexture_IsValid(skybox_texture) || snes_height <= 0)
    return kPresentationOutcome_CoreFailure;
  PresentationOutcome outcome = kPresentationOutcome_Complete;
  const bool heated=!rom_source&&DioramaHeat_Active(heat);
  ArRenderVertex2D heat_vertices[kHeatVertices];int32_t heat_indices[kHeatIndices];
  /* Valid spans use surface columns, including the OBJ apron. Blur-safe UV
   * insets keep the kernel out of uncaptured texels; the aspect crop stays
   * inside those intervals. */
  /* Live report (2026-07-21): {0.30,0.30,0.40} read as jarringly dark for
   * Plane+skybox — the intent is a subtle cue that this is background, not
   * a heavy tint. Lightened substantially; still a touch cool/blue like the
   * rest of the per-layer shade table. */
  static const ArRenderColorF kSkyboxDim = { 0.78f, 0.78f, 0.85f, 1.0f };
  static const ArRenderColorF kSkyboxFull = { 1.0f, 1.0f, 1.0f, 1.0f };
  ArRenderColorF tint = dim ? kSkyboxDim : kSkyboxFull;
  const int32_t indices[6] = { 0, 1, 2, 0, 2, 3 };
  /* Captured skyboxes are opaque resolved surfaces. A ROM composite may carry
   * authored transparent gaps when a scoped fill is explicitly Off, so keep
   * its alpha meaningful; opaque default/colour fills render identically. */
  ArRenderDrawState draw_state = {
    .flags = kArRenderDrawState_Blend,
    .blend = rom_source
        ? kArRenderBlendMode_Alpha : kArRenderBlendMode_Opaque,
  };
  /* The captured sky is blurred and magnified several times over: sample it
   * linearly so sub-texel band and camera motion stays smooth instead of
   * stepping a whole source pixel at a time. */
  if (!rom_source) {
    draw_state.flags |= kArRenderDrawState_Filter;
    draw_state.filter = kArRenderFilter_Linear;
  }
  if (rom_source) {
    draw_state.flags |= kArRenderDrawState_Address;
    draw_state.address_u = kArRenderTextureAddressMode_Wrap;
    draw_state.address_v = kArRenderTextureAddressMode_Clamp;
  }
  const bool blur_requested = SkyboxBlurEnabled(device);
  bool blur_bound = false;
  bool prefiltered_skybox = false;
  const int source_width = rom_source
      ? kDioramaRomBackdropPixels : SR_PPU_SURFACE_MAX_WIDTH;
  const int source_height = rom_source
      ? kDioramaRomBackdropPixels : SR_PPU_SURFACE_MAX_HEIGHT;

  /* Fix B/BH6: render each row band over its own actually-valid U span. This
   * is the distinction the former scalar lost in Death Heim: the clamped upper
   * image stretches from the authentic 256, while the repeating fog below it
   * uses the fully padded capture. Equivalent adjacent spans are coalesced by
   * DioramaBgValidSpanPlan_Build, so ordinary rooms still issue one draw. The
   * margin-fix gate selects valid spans or the legacy full capture; both then
   * receive the same aspect-preserving crop. */
  DioramaBgValidSpan legacy = {
    .y0 = 0, .y1 = snes_height,
    .x0 = obj_apron, .x1 = obj_apron + snes_width,
  };
  const DioramaBgValidSpan *spans = &legacy;
  unsigned span_count = 1;
  const bool use_valid_spans = !rom_source &&
      options->margin_fix && valid_spans && valid_spans->count;
  if (use_valid_spans) {
    spans = valid_spans->spans;
    span_count = valid_spans->count;
    if (span_count > kDioramaBgMaxValidSpans)
      span_count = kDioramaBgMaxValidSpans;
  }
  DioramaSkyboxVerticalMapping vertical = {
    .capture_y0 = 0,
    .capture_y1 = snes_height,
    .texture_v0 = 0.0f,
    .texture_v1 = rom_source
        ? 1.0f : (float)snes_height / (float)source_height,
  };
  const bool vertical_valid = !use_valid_spans ||
      DioramaSkyboxVerticalMapping_Build(
          valid_spans, snes_height, source_height,
          blur_radius, &vertical);
  DioramaSkyboxVerticalMapping raw_vertical = vertical;
  float motion_follow[8] = {0};
  if (!rom_source && follow_camera && vertical_valid) {
    const float low = vertical.texture_v0 * source_height;
    const float high = vertical.texture_v1 * source_height;
    motion_follow[0] = low;
    motion_follow[1] = high - fminf(kActRaiserAuthenticHeight, high - low);
    motion_follow[2] = camera_delta;
    motion_follow[3] = authentic_y0;
    motion_follow[7] = fminf(kActRaiserAuthenticHeight, high - low);
    DioramaSkyboxVerticalMapping_FollowCamera(
        &vertical, source_height, authentic_y0, camera_delta + capture_offset.y);
  }
  float band_u0[kDioramaBgMaxValidSpans] = {0};
  float band_u1[kDioramaBgMaxValidSpans] = {0};
  float available_width = INFINITY;
  for (unsigned i = 0; i < span_count; i++) {
    if (!vertical_valid || spans[i].x1 <= spans[i].x0 ||
        spans[i].y1 <= vertical.capture_y0 || spans[i].y0 >= vertical.capture_y1)
      continue;
    if (rom_source)
      DioramaRomSkyboxUvRange(
          snes_width, source_width, &band_u0[i], &band_u1[i]);
    else
      DioramaSkyboxUvRange(source_width, spans[i].x0, spans[i].x1,
                            blur_radius, &band_u0[i], &band_u1[i]);
    available_width = fminf(available_width,
        (band_u1[i] - band_u0[i]) * source_width);
  }
  /* Preserve pixel shape with one vertical window across raster bands. A wide
   * capture usually needs a horizontal crop; a narrow finite source crops the
   * vertical window instead. Published effect bounds use these same UVs. */
  motion_follow[4] = available_width;
  motion_follow[5] = out_h > 0 ? (float)out_w / out_h : 0;
  motion_follow[6] = pixel_aspect;
  if (projection && options->draw_resident_skybox)
    memcpy(projection->motion_follow, motion_follow, sizeof(motion_follow));
  const float fitted_width = DioramaSkyboxVerticalMapping_FitAspect(
      &vertical, source_height, available_width,
      out_h > 0 ? (float)out_w / out_h : 0.0f, pixel_aspect);
  const float subpixel_u = rom_source ? 0.0f : subpixel_x / source_width;
  if (face_bottom_y > 0.0f && isfinite(foreground_top_y) && fitted_width > 0.0f) {
    /* The horizontal fit makes the faces larger than the water plane. Keep
     * their last source row above the projected cloud edge, with eight native
     * output rows of breathing room. Translate the sampling window without
     * changing its size, so faces and eye sprites retain the same scale. */
    const float texture_height = (vertical.texture_v1 - vertical.texture_v0) * source_height;
    const float target = fmaxf(0.0f, fminf(1.0f,
        foreground_top_y / out_h - 8.0f / kActRaiserAuthenticHeight));
    const float bottom = (face_bottom_y + capture_offset.y -
        vertical.texture_v0 * source_height) / texture_height;
    const float shift = fmaxf(0.0f, bottom - target) * texture_height;
    const float capture_shift = shift *
        (vertical.capture_y1 - vertical.capture_y0) / texture_height;
    vertical.capture_y0 += capture_shift;
    vertical.capture_y1 += capture_shift;
    vertical.texture_v0 += shift / source_height;
    vertical.texture_v1 += shift / source_height;
    raw_vertical.capture_y0 += capture_shift;
    raw_vertical.capture_y1 += capture_shift;
    raw_vertical.texture_v0 += shift / source_height;
    raw_vertical.texture_v1 += shift / source_height;
  }
  if (projection) {
    projection->motion_source = rom_source ? 1 : motion_source;
    projection->capture_offset = rom_source ? (ArRenderPointF){0} :
        (ArRenderPointF){capture_offset.x - obj_apron, capture_offset.y};
  }
  DioramaSkyboxSourceMapping resident_mapping={0};
  if (options->draw_resident_skybox && !rom_source) {
    resident_mapping=(DioramaSkyboxSourceMapping){
        .meta={span_count,source_width,source_height,pixel_aspect},
        .output={out_w,out_h,capture_offset.x,capture_offset.y},
        .vertical={raw_vertical.capture_y0,raw_vertical.capture_y1,
                   raw_vertical.texture_v0,raw_vertical.texture_v1},
        .follow={motion_follow[0],motion_follow[1],motion_follow[2],motion_follow[3]},
        .fit={motion_follow[7]}};
    for (unsigned i=0;i<span_count;++i) {
      float *band=resident_mapping.bands[i];
      DioramaSkyboxUvRange(source_width,spans[i].x0,spans[i].x1,blur_radius,&band[0],&band[1]);
      const float shift=subpixel_u+(span_shift_texels?span_shift_texels[i]/source_width:0);
      band[0]+=shift;band[1]+=shift;
      if (spans[i].x1<=spans[i].x0)band[0]=band[1]=0;
      band[2]=spans[i].y0;band[3]=spans[i].y1;
    }
  }
  /* Resolve the same sky window before filtering. The weather renderer
   * projects its rigid cloud anchor, then inverts each destination strip into
   * this source texture. Raster seams and generated motion are applied once. */
  if (!rom_source && scene->sky_weather && effect_projection &&
      !effect_projection->bg2_plane.valid && vertical_valid && fitted_width>0) {
    DioramaProjection weather=*effect_projection;
    weather.bg2_plane=(DioramaPlaneProjection){0};
    weather.bg2_skybox=(DioramaSkyboxProjection){.active_band=-1,.to_texture=true,
        .capture_offset={capture_offset.x-obj_apron,capture_offset.y},
        .texture_offset=capture_offset,.motion_source=motion_source};
    DioramaSkyboxProjection *sky=&weather.bg2_skybox;
    memcpy(sky->motion_follow,motion_follow,sizeof(motion_follow));
    for (unsigned i=0;i<span_count;++i) {
      const float y0=fmaxf(spans[i].y0,vertical.capture_y0);
      const float y1=fminf(spans[i].y1,vertical.capture_y1);
      if (y1<=y0 || spans[i].x1<=spans[i].x0)continue;
      const float centre=.5f*(band_u0[i]+band_u1[i])+subpixel_u+
          (span_shift_texels?span_shift_texels[i]/source_width:0);
      const float half=.5f*fitted_width/source_width;
      const float t0=DioramaSkyboxVerticalMapping_Fraction(&vertical,y0);
      const float t1=DioramaSkyboxVerticalMapping_Fraction(&vertical,y1);
      const float v0=vertical.texture_v0+(vertical.texture_v1-vertical.texture_v0)*t0;
      const float v1=vertical.texture_v0+(vertical.texture_v1-vertical.texture_v0)*t1;
      sky->bands[sky->count++]=(DioramaSkyboxBandProjection){
          (centre-half)*source_width-capture_offset.x,v0*source_height-capture_offset.y,
          (centre+half)*source_width-capture_offset.x,v1*source_height-capture_offset.y,t0,t1};
    }
    if (options->draw_resident_skybox) {
      sky->resident=resident_mapping;
      sky->count=span_count;
    }
    Diorama_MergeSkyWeatherBands(sky);
    if (sky->count) {
      bool composited;
      skybox_texture=CompositeSkyWeather(device,skybox_texture,source_width,source_height,scene,&weather,&composited,&outcome);
      if (!ArRenderTexture_IsValid(skybox_texture))return outcome;
      if (composited) {
        source_dynamic=true; /* Animated weather must never reuse a stale blur. */
        effect_projection->bg2_skybox.weather_composited=true;
      }
    }
  }
  if ((blur_requested && options->skybox_prefilter) || grade->strength > 0) {
    PresentationOutcome prefilter_outcome;
    const ArRenderTexture prefiltered = BuildDioramaSkyboxPrefilter(
        device, skybox_texture, source_width, source_height,
        out_w, out_h, tint, blur_requested ? blur_radius : 0, rom_source,
        source_revision, source_dynamic, grade, &prefilter_outcome);
    outcome = PresentationOutcome_Combine(outcome, prefilter_outcome);
    if (!PresentationOutcome_IsUsable(prefilter_outcome))
      return prefilter_outcome;
    if (ArRenderTexture_IsValid(prefiltered)) {
      skybox_texture = prefiltered;
      tint = kSkyboxFull;
      prefiltered_skybox = true;
    }
  }
  if (blur_requested && !prefiltered_skybox) {
    const DioramaBlurEffectParams params = {
      .texel_width = 1.0f / (float)source_width,
      .texel_height = 1.0f / (float)source_height,
      .radius = blur_radius,
    };
    blur_bound = DioramaEffectBackend_BindBlur(device, &params);
    if (!blur_bound) {
      outcome = kPresentationOutcome_OptionalOmitted;
      if (!DioramaEffectBackend_Unbind(device))
        return kPresentationOutcome_CoreFailure;
    }
  }

  if (options->draw_resident_skybox && !rom_source) {
    DioramaSkyboxSourceDraw draw = {
      .indices = indices, .vertex_count = 4, .index_count = 6, .blend = draw_state.blend,
      .texture_width = source_width, .texture_height = source_height,
      .radius = blur_bound ? blur_radius : 0, .linear = true,
      .cloud_light = *cloud_light,
      .motion_slot = motion_source == 4 ? 5 : motion_source == 3 ? 3 : 6,
      .mapping = resident_mapping};
    if (vertical_valid) {
      const ArRenderVertex2D verts[] = {
        {{0,0},tint,{0,0}}, {{out_w,0},tint,{1,0}},
        {{out_w,out_h},tint,{1,1}}, {{0,out_h},tint,{0,1}}};
      draw.vertices = verts;
      if(heated) {
        HeatQuad(heat_vertices,heat_indices,verts,NULL);
        draw.vertices=heat_vertices;draw.indices=heat_indices;
        draw.vertex_count=kHeatVertices;draw.index_count=kHeatIndices;draw.heat=*heat;
      }
      /* These resident draws share a target and have no ordinary renderer
       * work between them. Submit the ordered bands together. */
      const DioramaSourceEffectBatch batch = options->source_effect_batch;
      const bool began = !batch || batch(device, true);
      if (!began) outcome = kPresentationOutcome_CoreFailure;
      for (unsigned i = 0; began && i < span_count; ++i) {
        DioramaRasterRowRun runs[SR_PPU_SURFACE_MAX_HEIGHT];
        DioramaSkyboxRowShift row_runs[SR_PPU_SURFACE_MAX_HEIGHT];
        const unsigned run_count = span_count < kDioramaSkyboxSourceBands
            ? DioramaRasterRowShift_Runs(row_shift, authentic_y0, spans[i].y0, spans[i].y1,
                                         runs, SR_PPU_SURFACE_MAX_HEIGHT)
            : 0;
        for (unsigned r = 0; r < run_count; ++r)
          row_runs[r] = (DioramaSkyboxRowShift){runs[r].y0, runs[r].y1, runs[r].texels};
        draw.band = i;
        draw.row_shifts = run_count ? row_runs : NULL;
        draw.row_shift_count = run_count;
        if (!options->draw_resident_skybox(device, skybox_texture, &draw))
          outcome = kPresentationOutcome_CoreFailure;
      }
      if (began && batch && !batch(device, false)) outcome = kPresentationOutcome_CoreFailure;
      draw.row_shifts = NULL;
      draw.row_shift_count = 0;
      if (projection) {
        projection->resident = draw.mapping;
        projection->count = span_count;
        projection->active_band = -1;
        /* Retain the captured-frame projection for small CPU-authored details
         * too. Resident effects still apply generated motion on the GPU. The
         * fit above already resolved these values; no readback is required. */
        for(unsigned i=0;i<span_count;++i) {
          const float y0=fmaxf(spans[i].y0,vertical.capture_y0);
          const float y1=fminf(spans[i].y1,vertical.capture_y1);
          if(y1<=y0||spans[i].x1<=spans[i].x0||fitted_width<=0)continue;
          const float center=.5f*(band_u0[i]+band_u1[i])+subpixel_u+
              (span_shift_texels?span_shift_texels[i]/source_width:0);
          const float half=.5f*fitted_width/source_width;
          const float t0=DioramaSkyboxVerticalMapping_Fraction(&vertical,y0);
          const float t1=DioramaSkyboxVerticalMapping_Fraction(&vertical,y1);
          const float v0=vertical.texture_v0+(vertical.texture_v1-vertical.texture_v0)*t0;
          const float v1=vertical.texture_v0+(vertical.texture_v1-vertical.texture_v0)*t1;
          projection->bands[i]=(DioramaSkyboxBandProjection){
              (center-half)*source_width-capture_offset.x,v0*source_height-capture_offset.y,
              (center+half)*source_width-capture_offset.x,v1*source_height-capture_offset.y,t0,t1};
        }
      }
    }
    if (blur_bound && !DioramaEffectBackend_Unbind(device)) return kPresentationOutcome_CoreFailure;
    return outcome;
  }
  if(!rom_source&&DioramaCloudLight_Active(cloud_light)) {
    const DioramaSceneryLightEffectParams material={.illumination=UnlitSceneryMap(device),
      .texel_width=1.f/source_width,.texel_height=1.f/source_height,
      .blur_radius=blur_bound?blur_radius:0,.mode=blur_bound?1:0,.shade={1,1,1,1},
      .cloud_light=*cloud_light};
    if(!DioramaEffectBackend_BindSceneryLight(device,&material)) {
      (void)DioramaEffectBackend_Unbind(device);return kPresentationOutcome_CoreFailure;
    }
    blur_bound=true; /* The final material now owns blur and cloud illumination. */
  }
  for (unsigned i = 0; i < span_count; i++) {
    /* The layer capture may contain unavailable top/bottom rows when another
     * primary plane owns a taller world. A skybox is an enveloping backdrop:
     * discard those rows, then normalize the remaining BG over the complete
     * output rather than preserving a black band in world space. */
    if (!vertical_valid || fitted_width <= 0.0f ||
        spans[i].x1 <= spans[i].x0) continue;
    float y0 = spans[i].y0 < vertical.capture_y0
        ? vertical.capture_y0 : spans[i].y0;
    float y1 = spans[i].y1 > vertical.capture_y1
        ? vertical.capture_y1 : spans[i].y1;
    if (y1 <= y0) continue;
    const float centre_u = 0.5f * (band_u0[i] + band_u1[i]) + subpixel_u +
        (span_shift_texels ? span_shift_texels[i] / source_width : 0.0f);
    const float half_width_u = 0.5f * fitted_width / source_width;
    const float u0 = centre_u - half_width_u;
    const float u1 = centre_u + half_width_u;
    const float vertical_t0 = DioramaSkyboxVerticalMapping_Fraction(
        &vertical, y0);
    const float vertical_t1 = DioramaSkyboxVerticalMapping_Fraction(
        &vertical, y1);
    const float v0 = vertical.texture_v0 +
        (vertical.texture_v1 - vertical.texture_v0) * vertical_t0;
    const float v1 = vertical.texture_v0 +
        (vertical.texture_v1 - vertical.texture_v0) * vertical_t1;
    /* Rows the band's shift cannot follow draw as their own quads, moved by
     * their remainder; the band publishes one projection either way. */
    DioramaRasterRowRun runs[SR_PPU_SURFACE_MAX_HEIGHT];
    unsigned run_count = DioramaRasterRowShift_Runs(
        row_shift, authentic_y0, (int)floorf(y0), (int)ceilf(y1),
        runs, SR_PPU_SURFACE_MAX_HEIGHT);
    if (!run_count) runs[run_count++] = (DioramaRasterRowRun){0, 0, 0.0f};
    static ArRenderVertex2D verts[4 * SR_PPU_SURFACE_MAX_HEIGHT];
    static int32_t piece_indices[6 * SR_PPU_SURFACE_MAX_HEIGHT];
    int pieces = 0;
    for (unsigned r = 0; r < run_count; ++r) {
      const bool whole = run_count == 1 && runs[r].y1 <= runs[r].y0;
      const float piece_y0 = whole ? y0 : fmaxf(y0, (float)runs[r].y0);
      const float piece_y1 = whole ? y1 : fminf(y1, (float)runs[r].y1);
      if (piece_y1 <= piece_y0) continue;
      const float t0 = whole ? vertical_t0
          : DioramaSkyboxVerticalMapping_Fraction(&vertical, piece_y0);
      const float t1 = whole ? vertical_t1
          : DioramaSkyboxVerticalMapping_Fraction(&vertical, piece_y1);
      const float draw_y0 = (float)out_h * t0;
      const float draw_y1 = (float)out_h * t1;
      const float piece_v0 = vertical.texture_v0 +
          (vertical.texture_v1 - vertical.texture_v0) * t0;
      const float piece_v1 = vertical.texture_v0 +
          (vertical.texture_v1 - vertical.texture_v0) * t1;
      const float du = runs[r].texels / source_width;
      ArRenderVertex2D *quad = &verts[4 * pieces];
      quad[0] = (ArRenderVertex2D){ { 0.0f, draw_y0 },         tint, { u0 + du, piece_v0 } };
      quad[1] = (ArRenderVertex2D){ { (float)out_w, draw_y0 }, tint, { u1 + du, piece_v0 } };
      quad[2] = (ArRenderVertex2D){ { (float)out_w, draw_y1 }, tint, { u1 + du, piece_v1 } };
      quad[3] = (ArRenderVertex2D){ { 0.0f, draw_y1 },         tint, { u0 + du, piece_v1 } };
      for (int k = 0; k < 6; ++k) piece_indices[6 * pieces + k] = 4 * pieces + indices[k];
      if(heated) {
        HeatQuad(heat_vertices,heat_indices,quad,heat);
        if(!SubmitDioramaGeometry(device,skybox_texture,heat_vertices,kHeatVertices,heat_indices,kHeatIndices,&draw_state))
          outcome=kPresentationOutcome_CoreFailure;
      }
      ++pieces;
    }
    if (!pieces) continue;
    const bool submitted = heated || SubmitDioramaGeometry(
        device, skybox_texture, verts, 4 * pieces, piece_indices, 6 * pieces, &draw_state);
    if (!submitted) {
      outcome = kPresentationOutcome_CoreFailure;
    } else if (projection && u1 > u0 && v1 > v0 &&
               projection->count < kDioramaBgMaxValidSpans) {
      /* Named ROM pages replace the captured art entirely. Their ambient
       * fields span the displayed capture, without inheriting the texture's
       * wrap count or a scrolling capture's interpolation offset. Captured
       * sources instead publish exact sampled pixels for art-bound lights. */
      projection->bands[projection->count++] = rom_source
          ? (DioramaSkyboxBandProjection){
              0.0f, 0.0f, (float)snes_width, (float)snes_height, 0.0f, 1.0f}
          : (DioramaSkyboxBandProjection){
              u0 * source_width - capture_offset.x,
              v0 * source_height - capture_offset.y,
              u1 * source_width - capture_offset.x,
              v1 * source_height - capture_offset.y,
              vertical_t0, vertical_t1};
      projection->active_band = -1;
    }
  }
  const bool shader_restored =
      !blur_bound || DioramaEffectBackend_Unbind(device);
  if (!shader_restored)
    return kPresentationOutcome_CoreFailure;
  return outcome;
}

/* B6 (followup doc): floor/ceiling/side-wall enclosure. z_back/z_front match
 * the backdrop's and HUD's z_world exactly (kDioramaLayers' z=0.00/0.95,
 * minus the 0.5 offset every layer's z_world applies) so the box lines up
 * with the layer stack's own depth range. Flat-shaded and untextured —
 * The render-device contract accepts an invalid texture for vertex-colour-only
 * geometry (doc's "start flat"; no wall art yet). Built via BuildQuadMesh —
 * floor/ceiling/walls each vary a different world-axis pair, which
 * BuildLayerMesh's hardcoded-z formula can't do (see GEO's comment). Must
 * be called AFTER the skybox (if any) and BEFORE the per-layer loop —
 * painter's algorithm, the box surrounds the stack. */
static const float kShoeboxZBack = -0.50f;
static const float kShoeboxZFront = 0.45f;
/* Once the waterfall reaches the host plane's real lower edge, retain only a
 * shallow vertical drop while it travels to the box's front depth. Enough Y
 * change preserves a cascading silhouette; most of the repeated tile budget
 * becomes forward travel, which is what makes the overflow leave the screen
 * plane and read as water folding over the diorama lip. */
static const float kAitosWaterfallFrontDrop = 0.18f;
/* Keep two native 8 px tile rows hidden beneath the authentic BG2.  The
 * overlap is opaque and coplanar, so it costs no visible source area while
 * leaving enough coverage for camera pitch, landing shake, and filtering. */
static const int kAitosWaterfallSeamTolerancePixels = 2 * 8;
/* Crossfade band (rad) around tilt_y=0 — see the near-wall comment below. */
static const float kShoeboxWallFadeRange = 0.15f;

static PresentationOutcome DrawDioramaShoebox(
    ArRenderDevice *device, const float mvp[16],
    float aspect_x, float height_scale, float tilt_y,
    int out_w, int out_h) {
  /* Live report (2026-07-21): opaque walls read as a plain gray box,
   * disconnected from the skybox (drawn before everything, including these
   * walls) — since the walls are the same painter's-algorithm layer as the
   * skybox's "far opening," letting them stay translucent lets the sky page
   * straight through instead of needing to texture the walls separately.
   * 0.35 then read as too faint to actually define an edge at low tilt —
   * split the difference. */
  static const ArRenderColorF kShoeboxColor = { 0.15f, 0.15f, 0.20f, 0.55f };
  /* Live report (2026-07-21): a box sized to match the layer stack exactly
   * (hx = 0.5*aspect_x, y=[-0.5,0.5]) can rotate its own corners into view
   * at extreme tilt/pan, revealing void past ITS edges — the same class of
   * problem the skybox fixes for the backdrop, just one level out.
   * Oversized X/Y (not Z — that still has to line up with the layer
   * stack's own depth range) gives headroom across the whole tilt clamp
   * (±0.7 rad) without needing per-angle math.
   *
   * Live report (2026-07-26): overscan 2.0 put the walls at exactly TWICE the
   * layer extent, so they no longer converged on the plane edges — a visible
   * gap between the rendered planes and the walls, which defeats the box's
   * other job (masking the original screen edges). It also ignored C1's slack
   * inset on Y, making the vertical mismatch (2.07x) worse than the
   * horizontal (2.00x).
   *
   * Reduced to a small margin, and half_y now tracks height_scale so both
   * axes are inset consistently with the layers. The corner-into-view hazard
   * the 2.0 was guarding is now handled properly upstream: a9599e6 gave
   * Scene3D_ProjectWorldPoint a near-plane rejection, so a corner that
   * rotates behind the camera makes BuildQuadMesh drop the quad instead of
   * projecting it to a huge finite coordinate. That guard did not exist when
   * 2.0 was chosen, which is why padding was the only available fix then. If
   * the extremes still misbehave, the next step is a per-frame clamp via
   * Scene3D_DepthBoundaryY (what the sim underlay already does) rather than
   * re-inflating this constant. */
  static const float kShoeboxOverscan = 1.05f;
  float hx = 0.5f * aspect_x * kShoeboxOverscan;
  float half_y = 0.5f * height_scale * kShoeboxOverscan;
  float z_span = kShoeboxZFront - kShoeboxZBack;
  ArRenderVertex2D verts[4];
  int32_t indices[6];
  int nv, ni;
  PresentationOutcome outcome = kPresentationOutcome_Complete;

  /* Floor (y=-0.5) and ceiling (y=+0.5): always drawn — yaw doesn't bring
   * them toward the camera the way a side wall does (the doc's own note:
   * revisit only if pitch range grows past the existing ±0.7 clamp). Both
   * span the full x/z extent, single quad each (no subdivision needed for
   * a flat, untextured surface). */
  BuildQuadMesh(mvp, -hx, -half_y, kShoeboxZBack,
               2.0f * hx, 0.0f, 0.0f,
               0.0f, 0.0f, z_span,
               0.0f, 0.0f, 1.0f, 1.0f, 1, 1, out_w, out_h, kShoeboxColor,
               verts, indices, &nv, &ni);
  RecordOptionalDioramaDraw(
      &outcome,
      RenderDioramaGeometry(
          device, ArRenderTexture_Invalid(), verts, nv, indices, ni,
          kArRenderBlendMode_Alpha));

  BuildQuadMesh(mvp, -hx, half_y, kShoeboxZBack,
               2.0f * hx, 0.0f, 0.0f,
               0.0f, 0.0f, z_span,
               0.0f, 0.0f, 1.0f, 1.0f, 1, 1, out_w, out_h, kShoeboxColor,
               verts, indices, &nv, &ni);
  RecordOptionalDioramaDraw(
      &outcome,
      RenderDioramaGeometry(
          device, ArRenderTexture_Invalid(), verts, nv, indices, ni,
          kArRenderBlendMode_Alpha));

  /* Side walls (x=±hx): the baseline 2D geometry contract has no depth test,
   * so a wall on the camera's near side would occlude the view straight into
   * the box —
   * the doc's rule is to draw only the FAR wall, using tilt_y's sign (no
   * dot product needed for a simple box). FIRST-PASS SIGN GUESS, same as
   * B4-vellean's pitch lean: positive tilt_y is assumed to put the +X wall
   * near camera — flip if it reads backwards in play. Crossfades both
   * walls over a small band around tilt_y=0 (rather than a hard cull) so
   * the transition isn't a pop. */
  float t = tilt_y / kShoeboxWallFadeRange;
  if (t > 1.0f) t = 1.0f;
  if (t < -1.0f) t = -1.0f;
  float alpha_pos_x = 0.5f - 0.5f * t;  /* fades out as the +X wall nears */
  float alpha_neg_x = 0.5f + 0.5f * t;  /* fades in as the -X wall goes far */

  if (alpha_neg_x > 0.01f) {
    ArRenderColorF c = kShoeboxColor;
    c.a *= alpha_neg_x;
    BuildQuadMesh(mvp, -hx, -half_y, kShoeboxZBack,
                 0.0f, 0.0f, z_span,
                 0.0f, 2.0f * half_y, 0.0f,
                 0.0f, 0.0f, 1.0f, 1.0f, 1, 1, out_w, out_h, c,
                 verts, indices, &nv, &ni);
    RecordOptionalDioramaDraw(
        &outcome,
        RenderDioramaGeometry(
            device, ArRenderTexture_Invalid(), verts, nv, indices, ni,
            kArRenderBlendMode_Alpha));
  }
  if (alpha_pos_x > 0.01f) {
    ArRenderColorF c = kShoeboxColor;
    c.a *= alpha_pos_x;
    BuildQuadMesh(mvp, hx, -half_y, kShoeboxZBack,
                 0.0f, 0.0f, z_span,
                 0.0f, 2.0f * half_y, 0.0f,
                 0.0f, 0.0f, 1.0f, 1.0f, 1, 1, out_w, out_h, c,
                 verts, indices, &nv, &ni);
    RecordOptionalDioramaDraw(
        &outcome,
        RenderDioramaGeometry(
            device, ArRenderTexture_Invalid(), verts, nv, indices, ni,
            kArRenderBlendMode_Alpha));
  }
  return outcome;
}

/* Dynamic framing follows the playfield depth from the layer table. */
static float DioramaBg1ReferenceZ(void) {
  for (int i = 0; i < kDioramaLayerCount; i++)
    if (kDioramaLayers[i].plane == SR_PPU_OVERLAY_BG1)
      return kDioramaLayers[i].z;
  return 0.50f;
}

static PresentationOutcome DioramaSubmitPlaneEffect(
    ArRenderOutputFrame *output_frame,
    DioramaPlaneEffectFn plane_effect,
    void *plane_effect_userdata, int plane,
    const DioramaProjection *projection, DioramaSourceEffectBatch batch) {
  if (!output_frame || !plane_effect || !projection || !projection->valid)
    return kPresentationOutcome_Complete;
  if (!ArRenderOutputFrame_EnterFullOutput(output_frame))
    return kPresentationOutcome_OptionalOmitted;
  DioramaPerformanceScope callback_performance =
      DioramaPerformance_Begin(kDioramaPerformance_Callback);
  bool submitted = !batch || batch(output_frame->device, true);
  if (submitted) {
    plane_effect(plane_effect_userdata, plane, projection);
    submitted = !batch || batch(output_frame->device, false);
  }
  DioramaPerformance_End(callback_performance);
  if (!ArRenderOutputFrame_RestoreViewport(output_frame))
    return kPresentationOutcome_CoreFailure;
  return submitted ? kPresentationOutcome_Complete : kPresentationOutcome_CoreFailure;
}

static PresentationOutcome DioramaSubmitSkyboxEffects(
    ArRenderOutputFrame *output_frame, const DioramaScene *scene,
    DioramaProjection *projection) {
  projection->bg2_skybox.active_band = -1;
  if (!scene->plane_effect || !projection->valid)
    return kPresentationOutcome_Complete;
  const DioramaSourceEffectBatch batch = scene->render->source_effect_batch;
  if (!batch) {
    PresentationOutcome outcome = kPresentationOutcome_Complete;
    for (unsigned i = 0; i < projection->bg2_skybox.count; ++i) {
      projection->bg2_skybox.active_band = (int)i;
      const PresentationOutcome effect = DioramaSubmitPlaneEffect(output_frame, scene->plane_effect,
          scene->plane_effect_userdata, SR_PPU_OVERLAY_BG2, projection, NULL);
      outcome = PresentationOutcome_Combine(outcome, effect);
      if (!PresentationOutcome_IsUsable(effect)) break;
    }
    projection->bg2_skybox.active_band = -1;
    return outcome;
  }
  if (!ArRenderOutputFrame_EnterFullOutput(output_frame))
    return kPresentationOutcome_OptionalOmitted;
  DioramaPerformanceScope performance = DioramaPerformance_Begin(kDioramaPerformance_Callback);
  bool submitted = batch(output_frame->device, true);
  if (submitted) {
    /* Keep band-major callback order. Only resident source draws can opt into
     * this batch; target/viewport changes remain outside its lifetime. */
    for (unsigned i = 0; i < projection->bg2_skybox.count; ++i) {
      projection->bg2_skybox.active_band = (int)i;
      scene->plane_effect(scene->plane_effect_userdata, SR_PPU_OVERLAY_BG2, projection);
    }
    submitted = batch(output_frame->device, false);
  }
  projection->bg2_skybox.active_band = -1;
  DioramaPerformance_End(performance);
  if (!ArRenderOutputFrame_RestoreViewport(output_frame))
    return kPresentationOutcome_CoreFailure;
  return submitted ? kPresentationOutcome_Complete : kPresentationOutcome_CoreFailure;
}

static PresentationOutcome DioramaCompositeCoreFailure(
    ArRenderOutputFrame *output_frame) {
  ArRenderOutputFrame_Abort(output_frame);
  return kPresentationOutcome_CoreFailure;
}

#include "diorama_death_heim_nebula.h"

typedef struct DioramaViewGeometry {
  const DioramaRenderOptions *options;
  const DioramaDepthSink *depth_sink;
  float auto_distance;
  float matrix[16];
  float world_y_offset, bg2_world_y_offset;
  DioramaCamera camera;
  float aspect_x, height_scale;
  float u0, v0, u1, v1;
  int width, height, visible_width;
  /* Native pixels the horizontal edge clamp moved the playfield right on
   * screen: the presented camera is the native camera minus this. */
  float clamp_native_x;
} DioramaViewGeometry;

static bool DeathHeimNebulaWindow(const DioramaCapture *capture,
    const DioramaScene *scene, const DioramaViewGeometry *geometry,
    const DioramaResolvedLayer *layer, DioramaDeathHeimNebulaWindow *window) {
  if (!scene->voxel_mode || scene->map_group != 7 || scene->map_number != 8 ||
      scene->layer_section != kDioramaLayerSection_Room ||
      (layer->plane != SR_PPU_OVERLAY_BG1 && layer->plane != kDioramaPlane_Bg1Hi &&
       layer->plane != kDioramaPlane_Bg1Far) ||
      layer->source != kDioramaLayerSource_Captured || layer->rake || layer->bow)
    return false;
  const ArRenderPointF motion = capture->plane_capture_offsets
      ? capture->plane_capture_offsets[layer->plane] : (ArRenderPointF){0};
  if (!DioramaDeathHeimNebula_Window(capture->width, capture->height,
      capture->authentic_y0, capture->obj_apron, geometry->visible_width,
      motion.x, motion.y, window)) return false;
  /* Rendering and effect publication make the same decision before either
   * changes. Projected depth on a flat plane is affine in output coordinates;
   * four valid corners cover the rectangle without a later inverse failure. */
  const float z=layer->z-.5f;
  for (unsigned i=0;i<4;++i) {
    const float s=(float)(i&1),t=(float)(i>>1);
    float x,y;ArRenderPointF projected;
    if (!DioramaSkyboxWorldPoint(geometry->matrix,z,geometry->aspect_x,
            geometry->height_scale,s,t,&x,&y) ||
        !ProjectWorldPointDepth(geometry->matrix,(x-.5f)*geometry->aspect_x,
            (.5f-y)*geometry->height_scale,z,geometry->width,geometry->height,
            &projected,&window->depths[i]) ||
        window->depths[i]<0 || window->depths[i]>1)
      return false;
  }
  return true;
}

typedef struct DioramaFocalAperture {
  ArRenderVertex2D vertices[DIORAMA_VERTS_PER_LAYER];
  float z;
  bool valid;
} DioramaFocalAperture;

static float VoxelPlaneScale(const DioramaViewGeometry *geometry,const ActionVoxelGeometry *castle,int plane,float z) {
  const float offset=plane==SR_PPU_OVERLAY_BG1?ActionVoxel_Depth(castle):
      DioramaPlaneIsObjectPriority(plane)?ActionVoxel_PlayRecess(castle):0;
  const float distance=fmaxf(1,geometry->camera.distance),world_z=z-.5f;
  return (distance-world_z)/(distance-world_z-offset);
}
static float TrunkRecess(void);
static bool HollowPlaySpace(const DioramaScene *scene) {
  return scene->trunk && scene->trunk_settings.enabled;
}
static int HollowResolveLayers(const DioramaScene *scene,DioramaResolvedLayer *layers,int count) {
  if(ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number)) {
    DioramaResolvedLayer original[kDioramaLayerCount];
    memcpy(original,layers,(size_t)count*sizeof(*layers));
    /* The enclosure owns BG order, source and geometry. Saved layer policy
     * stays immutable and becomes effective again when Voxel 3D is off. */
    for(int i=0;i<kDioramaLayerCount;++i){
      const DioramaLayerDesc *d=&kDioramaLayers[i];
      layers[i]=(DioramaResolvedLayer){.plane=d->plane,.z=d->z,.alpha=255,
          .source=kDioramaLayerSource_Captured,.stack_copies=1,
          .stack_direction=kDioramaStack_Forward};
      if(DioramaPlaneIsObjectPriority(d->plane)||d->plane==SR_PPU_OVERLAY_BG3||
          d->plane==kDioramaPlane_Backdrop)
        for(int j=0;j<count;++j)if(original[j].plane==d->plane){layers[i]=original[j];break;}
      /* Native high water draws over OBJ2, beneath high masonry. One
       * translucent face sits at the castle lip instead of spanning the
       * walking space with the ordinary room's repeated water sheets. */
      if(((ActionVoxel_Profile(scene->voxel_geometry)==kActionVoxelProfile_Castle && scene->map_number==5) ||
          ActionVoxel_BackgroundWater(ActionVoxel_Profile(scene->voxel_geometry))) &&
          d->plane==kDioramaPlane_Bg2Hi){
        layers[i].z=.5f;layers[i].alpha=160;
      }
      /* The jungle's main-screen water is added to BG1 and actors. Keep
       * its animated colour field at the shoreline instead of moving it
       * behind the recessed jungle with the generic BG2 policy. */
      if(ActionVoxel_Profile(scene->voxel_geometry)==kActionVoxelProfile_MarahnaJungle &&
          (d->plane==SR_PPU_OVERLAY_BG2 || d->plane==kDioramaPlane_Bg2Hi))
        layers[i].z=.5f;
      /* The temple deliberately replaces its native red water with the
       * tower sky. This belongs to its Voxel profile, independent of saved
       * BG order, stacking or transparency preferences. */
      if(ActionVoxel_Profile(scene->voxel_geometry)==kActionVoxelProfile_FillmoreTemple) {
        if(d->plane==SR_PPU_OVERLAY_BG2 || d->plane==kDioramaPlane_Bg2Hi ||
            d->plane==kDioramaPlane_Bg2Far)layers[i].alpha=0;
        if(d->plane==kDioramaPlane_Backdrop)
          layers[i].source=DioramaLayerOrder_ActionBgSource(1,4,2);
      }
    }
    count=kDioramaLayerCount;
    int rear=-1,front=-1;
    for(int i=0;i<count;++i){
      if(layers[i].plane==SR_PPU_OVERLAY_BG1){rear=i;layers[i].z=.5f-ActionVoxel_Depth(scene->voxel_geometry);}
      if(layers[i].plane==kDioramaPlane_Bg1Far){front=i;layers[i].z=.5f;}
      if(layers[i].plane==kDioramaPlane_Bg1Hi)layers[i].z=.5f;
      if(DioramaPlaneIsObjectPriority(layers[i].plane))layers[i].z-=ActionVoxel_PlayRecess(scene->voxel_geometry);
    }
    if(rear>=0&&front>=0){DioramaResolvedLayer t=layers[rear];layers[rear]=layers[front];layers[front]=t;}
    return count;
  }
  if(!HollowPlaySpace(scene))return count;
  DioramaResolvedLayer original[kDioramaLayerCount];
  memcpy(original,layers,(size_t)count*sizeof(*layers));
  /* The enclosure owns ordering, material sources and shape. Resolve ordinary
   * overrides first for other rooms, but never let a BG policy cut this mesh. */
  for(int i=0;i<kDioramaLayerCount;++i) {
    const DioramaLayerDesc *d=&kDioramaLayers[i];
    layers[i]=(DioramaResolvedLayer){.plane=d->plane,.z=d->z,.alpha=255,.stack_copies=1,
        .source=kDioramaLayerSource_Captured,.stack_direction=kDioramaStack_Forward};
    if(DioramaPlaneIsObjectPriority(d->plane)||d->plane==SR_PPU_OVERLAY_BG3)
      for(int j=0;j<count;++j)if(original[j].plane==d->plane){layers[i]=original[j];break;}
    if(d->plane==SR_PPU_OVERLAY_BG1||DioramaPlaneIsObjectPriority(d->plane))
      layers[i].z-=scene->trunk_settings.recess;
    if(d->plane==kDioramaPlane_Bg1Hi||d->plane==kDioramaPlane_Bg1Far)layers[i].z=.5f;
  }
  return kDioramaLayerCount;
}

int Diorama_ResolveSceneLayers(const DioramaScene *scene,
                               DioramaResolvedLayer *resolved) {
  if (!scene || !scene->render || !resolved) return 0;
  if (scene->render->resolved_layers) {
    const int count = scene->render->resolved_layer_count;
    if (count <= 0 || count > kDioramaLayerCount) return 0;
    memcpy(resolved, scene->render->resolved_layers,
           (size_t)count * sizeof(*resolved));
    return HollowResolveLayers(scene,resolved,count);
  }
  {
    DioramaResolvedLayer defaults[kDioramaLayerCount];
    for (int i = 0; i < kDioramaLayerCount; i++) {
      defaults[i].plane = kDioramaLayers[i].plane;
      defaults[i].z = kDioramaLayers[i].z;
      defaults[i].alpha = kDioramaLayerAlphaOpaque;
      defaults[i].source = kDioramaLayerSource_Captured;
      defaults[i].rake = 0.0f;
      defaults[i].bow = 0.0f;
      defaults[i].thickness = 0.0f;
      defaults[i].stack = 0.0f;
      defaults[i].stack_copies = 0;
      defaults[i].stack_direction = kDioramaStack_Forward;
      defaults[i].stack_solid = false;
    }
    const int count=DioramaLayerOrder_ResolveSection(
        scene->render->layers, scene->map_group, scene->map_number,
        scene->layer_section, defaults, kDioramaLayerCount, resolved,
        kDioramaLayerCount);
    return HollowResolveLayers(scene,resolved,count);
  }
}

/* A periodic native backdrop occupies the same world plane as the playfield,
 * but still paints behind every layer. Inverse projection covers the viewport
 * at every camera pose without scaling camera motion or exposing texture
 * allocation edges. One bounded mesh, one ordinary wrapped texture draw. */
static PresentationOutcome DrawPeriodicDioramaSkybox(
    ArRenderDevice *device, const DioramaCapture *capture,
    const DioramaViewGeometry *geometry, float reference_z,
    const DioramaCloudLight *cloud_light, DioramaProjection *projection) {
  ArRenderVertex2D vertices[DIORAMA_VERTS_PER_LAYER];
  int32_t indices[DIORAMA_INDICES_PER_LAYER];
  ArRenderPointF motion = {0};
  if (capture->plane_capture_offsets)
    motion = capture->plane_capture_offsets[SR_PPU_OVERLAY_BG1];
  float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
  int count = 0;
  for (int row = 0; row <= DIORAMA_SUBDIV_Y; ++row) {
    for (int col = 0; col <= DIORAMA_SUBDIV_X; ++col) {
      const float sx = (float)col / DIORAMA_SUBDIV_X;
      const float sy = (float)row / DIORAMA_SUBDIV_Y;
      float x, y;
      if (!DioramaSkyboxWorldPoint(geometry->matrix, reference_z,
              geometry->aspect_x, geometry->height_scale, sx, sy, &x, &y))
        return kPresentationOutcome_OptionalOmitted;
      x *= capture->width;
      y *= capture->height;
      x0 = fminf(x0, x - motion.x);
      x1 = fmaxf(x1, x - motion.x);
      y0 = fminf(y0, y - motion.y);
      y1 = fmaxf(y1, y - motion.y);
      vertices[count++] = (ArRenderVertex2D){
        .position = {sx * geometry->width, sy * geometry->height},
        .color = {1,1,1,1},
        .tex_coord = {
          (x - 0.5f * (capture->width - kActRaiserAuthenticWidth) +
              capture->bg2_camera_x - motion.x) / 256.0f,
          (y - capture->authentic_y0 + capture->bg2_camera_y + 1 - motion.y) / 256.0f},
      };
    }
  }
  int index_count;
  TriangulateGrid(DIORAMA_SUBDIV_X, DIORAMA_SUBDIV_Y, indices, &index_count);
  const ArRenderDrawState state = {
    .flags = kArRenderDrawState_Blend | kArRenderDrawState_Address,
    .blend = kArRenderBlendMode_Opaque,
    .address_u = kArRenderTextureAddressMode_Wrap,
    .address_v = kArRenderTextureAddressMode_Wrap,
  };
  /* UVs already contain unwrapped world coordinates. Only the artwork
   * repeats every page; an authored light must not repeat with it. Undo the
   * native one-row sampling offset when evaluating the lighting field. */
  DioramaCloudLight light = *cloud_light;
  light.source[0] = 0;
  light.source[1] = -1;
  light.source[2] = light.source[3] = 256;
  const DioramaSkyboxSourceDraw source = {.vertices = vertices, .indices = indices,
      .vertex_count = count, .index_count = index_count, .blend = state.blend,
      .texture_width = 256, .texture_height = 256, .motion_slot = 0, .periodic = true,
      .cloud_light = light};
  const bool material = !geometry->options->draw_resident_skybox &&
      DioramaCloudLight_Active(&light);
  if (material) {
    const DioramaSceneryLightEffectParams params = {
        .illumination = UnlitSceneryMap(device), .shade = {1,1,1,1},
        .texel_width = 1.f / 256, .texel_height = 1.f / 256, .cloud_light = light};
    if (!DioramaEffectBackend_BindSceneryLight(device, &params)) {
      (void)DioramaEffectBackend_Unbind(device);
      return kPresentationOutcome_CoreFailure;
    }
  }
  const bool submitted = geometry->options->draw_resident_skybox
      ? geometry->options->draw_resident_skybox(device, capture->skybox->texture, &source)
      : SubmitDioramaGeometry(device, capture->skybox->texture,
          vertices, count, indices, index_count, &state);
  const bool restored = !material || DioramaEffectBackend_Unbind(device);
  if (!submitted || !restored)
    return kPresentationOutcome_CoreFailure;
  if (projection) {
    projection->bg2_skybox = (DioramaSkyboxProjection){
      .world_plane = {.valid = true, .capture_offset = motion,
        .u0 = geometry->u0, .v0 = geometry->v0,
        .u1 = geometry->u1, .v1 = geometry->v1, .z_world = reference_z},
      .count = 1, .active_band = -1, .motion_source = 2,
      .bands = {{x0,y0,x1,y1,0,1}},
    };
  }
  return kPresentationOutcome_Complete;
}

enum { kDeathHeimFaceRows = 9 * 16 };

/* The cloud band's top is one line across the BG2 mesh. Clip its projected
 * endpoints to the output before choosing the highest visible point; a yawed
 * off-screen corner must not pull the faces out of view. */
static float DeathHeimCloudTop(const DioramaCapture *capture,
    const DioramaViewGeometry *geometry, const DioramaResolvedLayer *resolved,
    int resolved_count) {
  for (int i = 0; i < resolved_count; i++) {
    const DioramaResolvedLayer *layer = &resolved[i];
    if (layer->plane != SR_PPU_OVERLAY_BG2 || !layer->alpha) continue;
    const float offset_y = capture->plane_capture_offsets
        ? capture->plane_capture_offsets[SR_PPU_OVERLAY_BG2].y : 0.0f;
    const float t = (capture->authentic_y0 + kDeathHeimFaceRows + offset_y) /
        capture->height;
    const float y = (0.5f - t) * geometry->height_scale + geometry->bg2_world_y_offset;
    const float z = DioramaTiltedRowDepth(layer->z - 0.5f, layer->rake, layer->bow, t);
    ArRenderPointF left, right;
    if (!ProjectWorldPoint(geometry->matrix, -0.5f * geometry->aspect_x, y, z,
            geometry->width, geometry->height, &left) ||
        !ProjectWorldPoint(geometry->matrix, 0.5f * geometry->aspect_x, y, z,
            geometry->width, geometry->height, &right)) return NAN;
    if (left.x > right.x) {
      const ArRenderPointF swap = left;
      left = right;
      right = swap;
    }
    const float x0 = fmaxf(0.0f, left.x), x1 = fminf(geometry->width, right.x);
    if (x1 < x0) return NAN;
    const float slope = right.x > left.x ? (right.y - left.y) / (right.x - left.x) : 0;
    return fminf(left.y + (x0 - left.x) * slope, left.y + (x1 - left.x) * slope);
  }
  return NAN;
}

/* The skybox draws first. Missing ROM art falls back to current captured
 * BG2; failed renderer restoration is a core failure, never a fallback. */
static PresentationOutcome DrawResolvedDioramaSkybox(
    ArRenderDevice *device, const DioramaCapture *capture,
    const DioramaScene *scene, const DioramaViewGeometry *geometry,
    const ArRenderTexture *textures,
    const DioramaResolvedLayer *resolved, int resolved_count,
    DioramaProjection *projection,bool subscreen_backdrop) {
  PresentationOutcome outcome = kPresentationOutcome_Complete;
  static const float kSkyboxBlurRadiusOnly = 1.0f;
  static const float kSkyboxBlurRadiusBoth = 3.0f;
  if (geometry->options->skybox != kDioramaSky_Off) {
    DioramaPerformance_SetPlane(SR_PPU_OVERLAY_BG2);
    bool both = geometry->options->skybox == kDioramaSky_Both;
    ArRenderTexture skybox_texture = textures[SR_PPU_OVERLAY_BG2];
    bool rom_skybox = false;
    DioramaSceneryGrade skybox_grade={0};
    uint64_t skybox_revision = capture->bg2_revision;
    bool skybox_dynamic = capture->bg2_dynamic;
    int skybox_apron = capture->obj_apron;
    int skybox_width = capture->width;
    int skybox_motion_source = 3;
    float skybox_subpixel_x = 0.0f;
    ArRenderPointF capture_offset = {capture->obj_apron,0};
    if (capture->plane_capture_offsets) {
      capture_offset.x += capture->plane_capture_offsets[SR_PPU_OVERLAY_BG2].x;
      capture_offset.y += capture->plane_capture_offsets[SR_PPU_OVERLAY_BG2].y;
    }
    DioramaBgValidSpanPlan skybox_spans;
    const DioramaBgValidSpanPlan *skybox_valid_spans = capture->bg2_valid_spans;
    const int skybox_source =
        DioramaLayerOrder_SkyboxSource(resolved, resolved_count);
    const bool face_skybox = DeathHeimFaceScene(scene) &&
        skybox_source == kDioramaLayerSource_Captured;
    if (face_skybox) {
      skybox_texture = textures[kDioramaPlane_Bg2Far];
      skybox_motion_source = 4;
      capture_offset = (ArRenderPointF){capture->obj_apron, 0};
      if (capture->plane_capture_offsets) {
        capture_offset.x += capture->plane_capture_offsets[kDioramaPlane_Bg2Far].x;
        capture_offset.y += capture->plane_capture_offsets[kDioramaPlane_Bg2Far].y;
      }
      /* The water's repeating row policy must not widen or shrink the faces.
       * Fit the authentic face image to the enveloping skybox; both the eye
       * sprites and authored tile pixels already share this capture space. */
      const int x0 = capture->obj_apron +
          (capture->width - kActRaiserAuthenticWidth) / 2;
      skybox_spans = (DioramaBgValidSpanPlan){.count = 1, .spans = {{
          capture->authentic_y0, capture->authentic_y0 + kActRaiserAuthenticHeight,
          x0, x0 + kActRaiserAuthenticWidth}}};
      skybox_valid_spans = &skybox_spans;
    }
    if (!face_skybox && skybox_source == kDioramaLayerSource_Captured && capture->skybox &&
        capture->skybox->periodic && ArRenderTexture_IsValid(capture->skybox->texture)) {
      float reference_z = DioramaBg1ReferenceZ() - 0.5f;
      for (int i = 0; i < resolved_count; ++i)
        if (resolved[i].plane == SR_PPU_OVERLAY_BG1)
          reference_z = resolved[i].z - 0.5f;
      const PresentationOutcome periodic = DrawPeriodicDioramaSkybox(
          device, capture, geometry, reference_z, &scene->cloud_light, projection);
      if (periodic != kPresentationOutcome_OptionalOmitted) return periodic;
      /* A camera looking past the plane's horizon uses the ordinary captured
       * skybox. Never submit partially initialized inverse-projection data. */
    }
    if (!face_skybox && skybox_source == kDioramaLayerSource_Captured && capture->skybox &&
        !capture->skybox->periodic &&
        ArRenderTexture_IsValid(capture->skybox->texture) &&
        capture->bg2_valid_spans) {
      skybox_texture = capture->skybox->texture;
      skybox_revision = capture->skybox->revision;
      skybox_dynamic = capture->skybox->dynamic;
      skybox_apron = 0;
      skybox_width = capture->skybox->width;
      skybox_motion_source = 0;
      capture_offset = capture->skybox->capture_offset;
      skybox_subpixel_x = capture->skybox->subpixel_x;
      skybox_spans = *capture->bg2_valid_spans;
      for (unsigned i = 0; i < skybox_spans.count; ++i) {
        if (skybox_spans.spans[i].x1 <= skybox_spans.spans[i].x0) continue;
        skybox_spans.spans[i].x0 = 0;
        skybox_spans.spans[i].x1 = skybox_width;
      }
      skybox_valid_spans = &skybox_spans;
    }
    if (skybox_source != kDioramaLayerSource_Captured) {
      uint8_t source_group = 0, source_map = 0, source_bg = 0;
      bool transparent_fill_configured = false;
      uint32_t transparent_fill_argb = 0;
      const bool decoded=DioramaLayerOrder_DecodeActionBgSource(skybox_source, &source_group,
          &source_map, &source_bg);
      if (capture->bg_transparent_fill_configured &&
          capture->bg_transparent_fill_argb &&
          decoded &&
          (source_bg == 1 || source_bg == 2)) {
        transparent_fill_configured =
            capture->bg_transparent_fill_configured[source_bg - 1];
        transparent_fill_argb =
            capture->bg_transparent_fill_argb[source_bg - 1];
      }
      bool skybox_state_restore_failed = false;
      const DioramaRenderOptions *options = geometry->options;
      const ArRenderTexture named_handle = options->resolve_skybox
          ? options->resolve_skybox(options->skybox_userdata, device, skybox_source,
              transparent_fill_configured, transparent_fill_argb, &skybox_state_restore_failed)
          : ArRenderTexture_Invalid();

      if (skybox_state_restore_failed) {
        return kPresentationOutcome_CoreFailure;
      }
      if (ArRenderTexture_IsValid(named_handle)) {
        skybox_texture = named_handle;
        rom_skybox = true;
        if(decoded && source_bg==1)skybox_grade=scene->scenery_grade;
        skybox_dynamic = false;
        skybox_revision = ((uint64_t)(uint32_t)skybox_source << 33) ^
                          ((uint64_t)transparent_fill_configured << 32) ^
                          (uint64_t)transparent_fill_argb;
      } else if(subscreen_backdrop) {
        /* Captured main-screen BG2 is already drawn in its own pass. It
         * cannot stand in for a missing subscreen backdrop without adding
         * the water twice. Preserve the native planes and omit this fill. */
        return kPresentationOutcome_OptionalOmitted;
      }
    }

    if (rom_skybox || capture->pixels[face_skybox
            ? kDioramaPlane_Bg2Far : SR_PPU_OVERLAY_BG2] ||
        (!face_skybox && capture->skybox &&
         ArRenderTexture_IsValid(capture->skybox->texture))) {
      DioramaRenderOptions skybox_options = *geometry->options;
      if (face_skybox) skybox_options.margin_fix = true;
      /* Hold HDMA parallax bands with the edge-clamped playfield. */
      DioramaBgValidSpanPlan held_spans;
      float held_shift[kDioramaBgMaxValidSpans];
      static DioramaRasterRowShift held_rows;
      const float *span_shift = NULL;
      const DioramaRasterRowShift *row_shift = NULL;
      if (!rom_skybox && !face_skybox && skybox_valid_spans &&
          capture->bg2_raster_camera_sensitivity &&
          DioramaBgValidSpanPlan_HoldRaster(skybox_valid_spans, capture->authentic_y0,
              capture->bg2_raster_camera_sensitivity, capture->bg2_raster_subtexel,
              capture->bg1_camera_x,
              capture->bg2_raster_camera_x, geometry->clamp_native_x,
              &held_spans, held_shift, &held_rows)) {
        skybox_valid_spans = &held_spans;
        span_shift = held_shift;
        if (held_rows.active) row_shift = &held_rows;
      }
      DioramaCloudLight sky_light=scene->cloud_light;
      /* The private sky capture can have a different origin from BG2's plane.
       * Use the same capture mapping as fixed BG2 effect anchors, including
       * the OBJ apron and interpolated private-source offset. */
      sky_light.source[0]+=capture->obj_apron-capture_offset.x;
      sky_light.source[1]-=capture_offset.y;
      const PresentationOutcome skybox = DrawDioramaSkybox(
          device, skybox_texture, skybox_apron, skybox_width, capture->height,
          geometry->width, geometry->height, both,
          both ? kSkyboxBlurRadiusBoth : kSkyboxBlurRadiusOnly, rom_skybox,
          skybox_revision, skybox_dynamic, skybox_valid_spans, capture_offset,
          skybox_subpixel_x, span_shift, row_shift,
          skybox_motion_source, !face_skybox && capture->bg2_scroll_valid,
          capture->authentic_y0,
          (geometry->world_y_offset + geometry->bg2_world_y_offset) *
              kActRaiserAuthenticHeight,
          geometry->aspect_x * kActRaiserAuthenticHeight / capture->width,
          face_skybox ? capture->authentic_y0 + kDeathHeimFaceRows : 0,
          face_skybox ? DeathHeimCloudTop(capture, geometry, resolved, resolved_count) : NAN,
          projection && (face_skybox || !projection->bg2_plane.valid)
              ? &projection->bg2_skybox : NULL, &skybox_options, &scene->background_heat, &sky_light, &skybox_grade,
          scene,projection);
      outcome = PresentationOutcome_Combine(outcome, skybox);
      if (!PresentationOutcome_IsUsable(skybox)) {
        return kPresentationOutcome_CoreFailure;
      }
    }
  }
  return outcome;
}

static const DioramaResolvedLayer *CameraFocalLayer(
    int primary, const DioramaScene *scene,
    const DioramaResolvedLayer *resolved, int count) {
  if(primary==SR_PPU_OVERLAY_BG1&&ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number))
    for(int i=0;i<count;++i)if(resolved[i].plane==kDioramaPlane_Bg1Far&&resolved[i].alpha)return &resolved[i];
  const int high = primary == SR_PPU_OVERLAY_BG1
      ? kDioramaPlane_Bg1Hi : kDioramaPlane_Bg2Hi;
  const DioramaResolvedLayer *focal = NULL;
  for (int i = 0; i < count; ++i) {
    const DioramaResolvedLayer *layer = &resolved[i];
    if ((layer->plane != primary && layer->plane != high) || !layer->alpha ||
        !Diorama_PlaneEligible(layer->plane,
            (scene->render->visible_planes & (1u << layer->plane)) != 0,
            true, true, scene->render->hud_flat,
            SkyboxReplacesPlane(scene, layer->plane), scene->additive_plane_mask)) continue;
    if (!focal || layer->plane == primary) focal = layer;
  }
  /* Camera limits must not change when an otherwise enabled priority band
   * happens to have no pixels in this capture. Also usable before scanout. */
  return focal;
}

/* UVs describe the displayed capture inside the allocation. World size
 * remains normalized to 224 native rows so margins extend the scene rather
 * than shrinking the playfield. Resolve auto-fit before applying zoom. */
static void PrepareDioramaView(const DioramaCapture *capture,
                               const DioramaView *view,
                               const DioramaScene *scene,
                               const DioramaResolvedLayer *resolved,
                               int resolved_count,
                               DioramaViewGeometry *geometry) {
  float tex_h = (float)capture->height;
  geometry->visible_width = view->visible_width;
  geometry->world_y_offset = 0.0f;
  geometry->bg2_world_y_offset = 0.0f;

  geometry->u0 = (float)capture->obj_apron / (float)SR_PPU_SURFACE_MAX_WIDTH;
  geometry->u1 = (float)(capture->obj_apron + capture->width) /
                 (float)SR_PPU_SURFACE_MAX_WIDTH;

  geometry->v0 = 0.0f;
  geometry->v1 = tex_h / (float)SR_PPU_SURFACE_MAX_HEIGHT;

  geometry->height_scale = tex_h / (float)kActRaiserAuthenticHeight;

  float par = 1.0f;
  if (view->pixel_aspect == kPixelAspect_Crt43 && !view->ignore_aspect_ratio)
    par = 7.0f / 6.0f;

  geometry->aspect_x =
      (float)capture->width / (float)kActRaiserAuthenticHeight * par;
  float vis_half_w = 0.5f * (float)view->visible_width /
                     (float)kActRaiserAuthenticHeight * par;

  float screen_aspect = (float)geometry->width / (float)geometry->height;
  float tan_half = tanf(kDioramaFovY * 0.5f);
  const float visible_height = view->visible_height > 0
      ? (float)view->visible_height : (float)kActRaiserAuthenticHeight;
  float fit_h = 0.5f * visible_height /
      (float)kActRaiserAuthenticHeight / tan_half;
  float fit_w = vis_half_w / (tan_half * screen_aspect);
  static const float kDioramaZ_Hud = 0.95f;
  geometry->auto_distance =
      fmaxf(fit_h, fit_w) * 1.02f + (kDioramaZ_Hud - 0.5f);

  /* Width fitting (especially with CRT pixels) can back the camera beyond
   * the captured rows even when more painted scenery exists in the world.
   * Limit the automatic baseline by the fixed vertical storage budget, not
   * the remaining distance to a room edge: redistributing upper/lower rows
   * must not zoom the scene as it scrolls. Fully captured short rooms retain
   * their borders. User zoom/orbit is applied afterwards. */
  if (capture->vertical_bounds.valid &&
      !(capture->vertical_bounds.top_reached && capture->vertical_bounds.bottom_reached)) {
    const DioramaResolvedLayer *focal = CameraFocalLayer(
        capture->vertical_bounds.plane, scene, resolved, resolved_count);
    if (focal) {
      /* One native row at either end keeps the filtered texture boundary
       * outside the view instead of exposing a subpixel seam. */
      const float fit_capture = 0.5f * (tex_h - 2.0f) /
          ((float)kActRaiserAuthenticHeight * tan_half) + focal->z - 0.5f;
      geometry->auto_distance = fminf(geometry->auto_distance, fit_capture);
    }
  }

  geometry->camera = (DioramaCamera){view->camera.tilt_x, view->camera.tilt_y,
                                     view->camera.distance, kDioramaFovY};
  if (geometry->camera.distance <= 0.0f)
    geometry->camera.distance = geometry->auto_distance;

  else if (geometry->camera.distance < kDioramaDistMin)
    geometry->camera.distance = kDioramaDistMin;

  geometry->camera.distance += view->distance_offset;
  geometry->camera.distance *= view->distance_scale;
  if (geometry->camera.distance < kDioramaDistMin)
    geometry->camera.distance = kDioramaDistMin;

  BuildViewProjection(&geometry->camera, geometry->width, geometry->height,
                      geometry->matrix);
  Diorama_AlignCaptureToNativeCamera(
      geometry->matrix, capture->height, capture->authentic_y0);
  Diorama_TranslateCameraWorldX(geometry->matrix,
      -(float)capture->camera_offset_x * par / kActRaiserAuthenticHeight);

  /* The modeled tower follows the native arena vertically. Its old flat-layer
   * offset favors the tall facade and crops the walking deck at tilted views.
   * Keep that saved offset for ordinary Diorama, and keep horizontal framing
   * and user orbit/zoom in both modes. Resolve here so drawing and capture
   * footprint/camera queries use exactly the same policy. */
  const ActionVoxelProfile framing_profile =
      ActionVoxel_Active(scene->voxel_geometry, scene->map_group, scene->map_number)
          ? ActionVoxel_Profile(scene->voxel_geometry) : kActionVoxelProfile_None;
  const int framing_y = ActionVoxel_VerticalFraming(framing_profile, capture->framing_y);
  float framing_weight = Clampf(view->camera_framing_weight, 0.0f, 1.0f);
  if (framing_weight == 0.0f) {
    Diorama_OffsetCamera(geometry->matrix, capture->framing_x, framing_y, par);
    if (view->fit_free_camera) {
      const int primary = capture->vertical_bounds.valid ? capture->vertical_bounds.plane
          : capture->horizontal_bounds.valid ? capture->horizontal_bounds.plane
          : SR_PPU_OVERLAY_BG1;
      const DioramaResolvedLayer *focal = CameraFocalLayer(primary, scene, resolved, resolved_count);
      if (focal) {
        const float apron = DioramaPlaneUsesBgApron(primary, capture->bg_apron_mask)
            ? (float)capture->obj_apron / (float)capture->width : 0.0f;
        Diorama_FrameFreeCameraHorizontally(geometry->matrix, geometry->aspect_x,
            geometry->height_scale, focal->z - 0.5f, focal->rake, focal->bow,
            &capture->horizontal_bounds, -apron, 1.0f + apron);
        Diorama_FrameFreeCameraVertically(geometry->matrix, geometry->aspect_x,
            geometry->height_scale, focal->z - 0.5f, focal->rake, focal->bow);
      }
    }
    return;
  }
  float free_matrix[16];
  memcpy(free_matrix, geometry->matrix, sizeof(free_matrix));

  {
    float focal_z = DioramaBg1ReferenceZ() - 0.5f;
    float focal_rake = 0.0f, focal_bow = 0.0f;
    for (int i = 0; i < resolved_count; i++) {
      if (resolved[i].plane != (ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number)?kDioramaPlane_Bg1Far:SR_PPU_OVERLAY_BG1)) continue;
      focal_z = resolved[i].z - 0.5f;
      focal_rake = resolved[i].rake;
      focal_bow = resolved[i].bow;
      break;
    }
    Diorama_CenterCameraVertically(
        geometry->matrix, geometry->aspect_x, geometry->height_scale, focal_z,
        focal_rake, focal_bow, (float)capture->authentic_y0 / tex_h,
        (float)(capture->authentic_y0 + kActRaiserAuthenticHeight) / tex_h);
  }

  /* Dynamic framing follows the level, but must stop at captured world edges.
   * Free-camera orbit/zoom remains an explicit view chosen by the player. */
  if (capture->vertical_bounds.valid) {
    const int primary = capture->vertical_bounds.plane;
    const DioramaResolvedLayer *focal =
        CameraFocalLayer(primary, scene, resolved, resolved_count);
    if (focal)
      Diorama_ClampCameraVertically(
          geometry->matrix, geometry->aspect_x, geometry->height_scale,
          focal->z - 0.5f, focal->rake, focal->bow,
          capture->vertical_bounds.top_reached,
          capture->vertical_bounds.bottom_reached, &geometry->world_y_offset);
  }

  if (capture->bg2_scroll_valid)
    geometry->bg2_world_y_offset = framing_weight * Diorama_BackgroundClampOffset(
        capture->camera_y, capture->bg2_camera_y, capture->bg2_vertical_ratio,
        capture->bg2_world_height, geometry->world_y_offset);
  geometry->world_y_offset *= framing_weight;

  /* Restore automatic framing and the camera stop alongside the manual pose
   * during the Dynamic Cam override's release. */
  if (framing_weight < 1.0f) {
    for (int i = 0; i < 16; i++)
      geometry->matrix[i] = free_matrix[i] + framing_weight *
          (geometry->matrix[i] - free_matrix[i]);
  }
  /* Designer framing is the baseline even during manual orbit. It does not
   * change when extra tiles alter the scenery bounds or capture margins. */
  Diorama_OffsetCamera(geometry->matrix, capture->framing_x, framing_y, par);

  /* Capture width is a storage budget, not the perspective viewport. In
   * particular 16:9 can see past a native edge that still covers 16:10. Clamp
   * after vertical and authored framing so neither can undo the side stop.
   * The same holds for the captured columns themselves: a wide phone or a
   * room's authored framing can look past the stream budget while the world
   * goes on, so the capture is a second, harder stop. */
  {
    const int primary = capture->horizontal_bounds.valid
        ? capture->horizontal_bounds.plane
        : capture->vertical_bounds.valid ? capture->vertical_bounds.plane
                                         : SR_PPU_OVERLAY_BG1;
    const DioramaResolvedLayer *focal =
        CameraFocalLayer(primary, scene, resolved, resolved_count);
    if (focal && capture->width > 0) {
      float clamped[16];
      memcpy(clamped, geometry->matrix, sizeof(clamped));
      const float apron =
          DioramaPlaneUsesBgApron(primary, capture->bg_apron_mask)
              ? (float)capture->obj_apron / (float)capture->width : 0.0f;
      bool held = Diorama_FitCameraToCapture(
          clamped, geometry->aspect_x, geometry->height_scale,
          focal->z - 0.5f, focal->rake, focal->bow,
          &capture->horizontal_bounds, -apron, 1.0f + apron);
      held |= capture->horizontal_bounds.valid &&
          Diorama_ClampCameraHorizontally(
              clamped, geometry->aspect_x, geometry->height_scale,
              focal->z - 0.5f, focal->rake, focal->bow,
              &capture->horizontal_bounds);
      if (held) {
        float unclamped[16];
        memcpy(unclamped, geometry->matrix, sizeof(unclamped));
        for (int c = 0; c < 16; ++c)
          geometry->matrix[c] += framing_weight * (clamped[c] - geometry->matrix[c]);
        /* Measure the hold at the focal plane's centre row, in that plane's
         * native pixels, so camera-driven raster scroll can hold with it. */
        const float z = DioramaTiltedRowDepth(focal->z - 0.5f, focal->rake, focal->bow, 0.5f);
        const float half = 0.5f * geometry->aspect_x;
        ArRenderPointF before, after, left, right;
        if (ProjectWorldPoint(unclamped, 0.0f, 0.0f, z, geometry->width, geometry->height,
                              &before) &&
            ProjectWorldPoint(geometry->matrix, 0.0f, 0.0f, z, geometry->width,
                              geometry->height, &after) &&
            ProjectWorldPoint(geometry->matrix, -half, 0.0f, z, geometry->width,
                              geometry->height, &left) &&
            ProjectWorldPoint(geometry->matrix, half, 0.0f, z, geometry->width,
                              geometry->height, &right) &&
            fabsf(right.x - left.x) > 0.0f)
          geometry->clamp_native_x = (after.x - before.x) * capture->width /
                                     fabsf(right.x - left.x);
      }
    }
  }
}

bool Diorama_ResolveCaptureCameraRange(const DioramaCapture *capture,
    const DioramaView *view, const DioramaScene *scene, int camera_x,
    int *minimum, int *maximum) {
  if (!capture || !view || !scene || !scene->render || !minimum || !maximum ||
      !capture->horizontal_bounds.valid || capture->width <= 0 ||
      capture->height < kActRaiserAuthenticHeight || view->viewport.w <= 0 ||
      view->viewport.h <= 0 || view->camera_framing_weight <= 0.0f) return false;
  DioramaResolvedLayer resolved[kDioramaLayerCount];
  const int count = Diorama_ResolveSceneLayers(scene, resolved);
  const DioramaResolvedLayer *focal =
      CameraFocalLayer(capture->horizontal_bounds.plane, scene, resolved, count);
  if (!focal) return false;
  /* Resolve the projection before either finite edge is reached. Its scale
   * and source footprint then stay independent of horizontal camera travel. */
  DioramaCapture open = *capture;
  open.horizontal_bounds.left_reached = open.horizontal_bounds.right_reached = false;
  DioramaViewGeometry g = {.width = view->viewport.w, .height = view->viewport.h};
  PrepareDioramaView(&open, view, scene, resolved, count, &g);
  float left = INFINITY, right = -INFINITY;
  for (int row = 0; row <= kDioramaPlaneSubdivY; ++row) {
    const float t = (float)row / kDioramaPlaneSubdivY;
    const float y = (0.5f - t) * g.height_scale;
    const float z = DioramaTiltedRowDepth(focal->z - 0.5f, focal->rake, focal->bow, t);
    for (int side = 0; side < 2; ++side) {
      const float ndc = side ? 1.0f : -1.0f;
      const float denominator = g.matrix[0] - ndc * g.matrix[3];
      if (!(denominator > 0.0f)) return false;
      const float x = ((ndc * g.matrix[7] - g.matrix[4]) * y +
          (ndc * g.matrix[11] - g.matrix[8]) * z +
          ndc * g.matrix[15] - g.matrix[12]) / denominator;
      const float pixel = (x / g.aspect_x + 0.5f) * capture->width;
      left = fminf(left, pixel); right = fmaxf(right, pixel);
    }
  }
  const float margin = 0.5f * (capture->width - kActRaiserAuthenticWidth);
  /* World boundaries are integral native pixels. Recover them before taking
   * the difference, avoiding one-pixel ceil/floor jitter from normalized
   * bounds at different camera positions. */
  const float world_left = roundf(camera_x - margin + capture->horizontal_bounds.left * capture->width);
  const float world_right = roundf(camera_x - margin + capture->horizontal_bounds.right * capture->width);
  const float lo = world_left + margin - left;
  const float hi = world_right + margin - right;
  /* A room narrower than this view retains its ordinary small-room framing.
   * Round inward so fractional perspective never exposes a missing column. */
  if (!isfinite(lo) || !isfinite(hi) || lo < -65535 || hi > 65535 ||
      ceilf(lo) > floorf(hi)) return false;
  *minimum = (int)ceilf(lo); *maximum = (int)floorf(hi);
  /* A nearly screen-sized room has no useful scenery to reveal in one or
   * two native pixels of travel. Keep a stable centre instead of crawling
   * between fractional edge fits. This is layout-dependent, not tied to
   * the current camera position, and is shared with the editor. */
  if (*maximum - *minimum <= 2)
    *minimum = *maximum = *minimum + (*maximum - *minimum) / 2;
  return true;
}

bool Diorama_ResolveCaptureFootprint(const DioramaCapture *capture,
    const DioramaView *view, const DioramaScene *scene, int plane,
    ArRenderRectF *out) {
  if (!capture || !view || !scene || !scene->render || !out ||
      capture->width <= 0 || capture->height < kActRaiserAuthenticHeight ||
      view->viewport.w <= 0 || view->viewport.h <= 0 || view->visible_width <= 0 ||
      !isfinite(view->camera.tilt_x) || !isfinite(view->camera.tilt_y) ||
      !isfinite(view->camera.distance) || !isfinite(view->distance_offset) ||
      !isfinite(view->distance_scale) || view->distance_scale <= 0 ||
      !isfinite(view->camera_framing_weight)) return false;
  DioramaResolvedLayer resolved[kDioramaLayerCount];
  const int count = Diorama_ResolveSceneLayers(scene, resolved);
  const DioramaResolvedLayer *layer = NULL;
  for (int i = 0; i < count; ++i)
    if (resolved[i].plane == plane && resolved[i].alpha > 0) layer = &resolved[i];
  if (!layer) return false;
  DioramaViewGeometry g = {.width = view->viewport.w, .height = view->viewport.h};
  PrepareDioramaView(capture, view, scene, resolved, count, &g);
  double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
  const double offset = plane == SR_PPU_OVERLAY_BG2 || plane == kDioramaPlane_Bg2Hi
      ? g.bg2_world_y_offset : 0;
  /* Invert the viewport onto each linear mesh strip, then clip the resulting
   * quadrilateral to that strip. This includes bow extrema and tilted edges,
   * which a four-corner inversion of one flat plane would miss. */
  for (int row = 0; row < kDioramaPlaneSubdivY; ++row) {
    const float ta = (float)row / kDioramaPlaneSubdivY;
    const float tb = (float)(row + 1) / kDioramaPlaneSubdivY;
    const double ya = (0.5 - ta) * g.height_scale;
    const double yb = (0.5 - tb) * g.height_scale;
    const double za = DioramaTiltedRowDepth(layer->z - 0.5f, layer->rake, layer->bow, ta);
    const double zb = DioramaTiltedRowDepth(layer->z - 0.5f, layer->rake, layer->bow, tb);
    const double slope = (zb - za) / (yb - ya), intercept = za - slope * ya;
    typedef struct { double x, y; } Point;
    Point polygon[8], scratch[8];
    int n = 4;
    for (int corner = 0; corner < 4; ++corner) {
      const double nx = corner == 1 || corner == 2 ? 1 : -1;
      const double ny = corner < 2 ? 1 : -1;
      const float *m = g.matrix;
      const double a = m[0] - nx * m[3], d = m[1] - ny * m[3];
      const double b = m[4] + slope * m[8] - nx * (m[7] + slope * m[11]);
      const double e = m[5] + slope * m[9] - ny * (m[7] + slope * m[11]);
      const double c = nx * (m[15] + intercept * m[11] + offset * m[7]) -
          (m[12] + intercept * m[8] + offset * m[4]);
      const double f = ny * (m[15] + intercept * m[11] + offset * m[7]) -
          (m[13] + intercept * m[9] + offset * m[5]);
      const double determinant = a * e - b * d;
      if (fabs(determinant) < 1e-12) return false;
      Point p = {(c * e - b * f) / determinant, (a * f - c * d) / determinant};
      const double w = m[3] * p.x + m[7] * (p.y + offset) +
          m[11] * (intercept + slope * p.y) + m[15];
      if (!isfinite(p.x) || !isfinite(p.y) || w <= 0) return false;
      polygon[corner] = p;
    }
    for (int side = 0; side < 2 && n; ++side) {
      if ((!side && !row) || (side && row == kDioramaPlaneSubdivY - 1)) continue;
      const double edge = side ? yb : ya;
      int next = 0;
      for (int i = 0; i < n; ++i) {
        const Point a = polygon[i], b = polygon[(i + 1) % n];
        const bool in_a = side ? a.y >= edge : a.y <= edge;
        const bool in_b = side ? b.y >= edge : b.y <= edge;
        if (in_a) scratch[next++] = a;
        if (in_a != in_b) {
          const double t = (edge - a.y) / (b.y - a.y);
          scratch[next++] = (Point){a.x + t * (b.x - a.x), edge};
        }
      }
      n = next;
      memcpy(polygon, scratch, (size_t)n * sizeof(*polygon));
    }
    for (int i = 0; i < n; ++i) {
      const double x = (polygon[i].x / g.aspect_x + 0.5) * capture->width;
      const double y = (0.5 - polygon[i].y / g.height_scale) * capture->height;
      x0 = fmin(x0, x); x1 = fmax(x1, x); y0 = fmin(y0, y); y1 = fmax(y1, y);
    }
  }
  if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1)) return false;
  *out = (ArRenderRectF){(float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0)};
  return true;
}

/* A's water is a finite moving plane. Lower it to the viewport's bottom;
 * the enlarged faces occupy the space above it. Four native rows of guard
 * keep the captured bottom border and its filter footprint below the output. */
static void PlaceDeathHeimWater(const DioramaCapture *capture,
    const DioramaScene *scene, const ArRenderTexture *textures,
    const DioramaResolvedLayer *resolved, int resolved_count,
    DioramaViewGeometry *geometry) {
  if (!DeathHeimFaceScene(scene) || scene->render->skybox != kDioramaSky_Only)
    return;
  const float slope = geometry->matrix[5] + geometry->matrix[7];
  if (slope <= 0.0f) return;
  for (int i = 0; i < resolved_count; i++) {
    const DioramaResolvedLayer *layer = &resolved[i];
    if (layer->plane != SR_PPU_OVERLAY_BG2 || !layer->alpha ||
        !DioramaLayerIsDrawable(DioramaDescForPlane(layer->plane),
            textures, capture->pixels, scene)) continue;
    const ArRenderPointF offset = capture->plane_capture_offsets
        ? capture->plane_capture_offsets[layer->plane] : (ArRenderPointF){0};
    const float t = (capture->authentic_y0 + kActRaiserAuthenticHeight - 4 + offset.y) /
        capture->height;
    const float y = (0.5f - t) * geometry->height_scale + geometry->bg2_world_y_offset;
    const float z = DioramaTiltedRowDepth(layer->z - 0.5f, layer->rake, layer->bow, t);
    float shift = 0.0f;
    for (int side = 0; side < 2; side++) {
      Scene3DClipPoint point;
      if (!Scene3D_TransformToClip(geometry->matrix,
              (side - 0.5f + offset.x / capture->width) * geometry->aspect_x,
              y, z, &point) ||
          point.w <= kScene3DMinimumProjectionDepth) return;
      shift = fminf(shift, (-point.w - point.y) / slope);
    }
    geometry->bg2_world_y_offset += shift;
    return;
  }
}

static void PublishDioramaView(const DioramaCapture *capture,
                               const DioramaView *view,
                               const DioramaViewGeometry *geometry,
                               DioramaProjection *out_projection) {
  if (out_projection) {
    memcpy(out_projection->matrix, geometry->matrix, sizeof(geometry->matrix));
    out_projection->aspect_x = geometry->aspect_x;
    out_projection->height_scale = geometry->height_scale;
    out_projection->auto_distance = geometry->auto_distance;
    out_projection->camera_distance = geometry->camera.distance;
    out_projection->between_backgrounds_world_y_offset =
        -0.5f * geometry->bg2_world_y_offset;
    out_projection->between_backgrounds_capture_offset = (ArRenderPointF){0};
    if (capture->plane_capture_offsets) {
      const ArRenderPointF front = capture->plane_capture_offsets[SR_PPU_OVERLAY_BG1];
      const ArRenderPointF back = capture->plane_capture_offsets[SR_PPU_OVERLAY_BG2];
      out_projection->between_backgrounds_capture_offset = (ArRenderPointF){
          0.5f * (front.x - back.x), 0.5f * (front.y - back.y)};
    }

    out_projection->texture_x_origin = capture->obj_apron;
    out_projection->texture_width = SR_PPU_SURFACE_MAX_WIDTH;

    out_projection->texture_height = SR_PPU_SURFACE_MAX_HEIGHT;
    out_projection->output_x = view->viewport.x;
    out_projection->output_y = view->viewport.y;
    out_projection->output_width = geometry->width;
    out_projection->output_height = geometry->height;
  }
}

/* Forward BG planes converge to BG1 only at their outer rings. Empty
 * captures cannot move the camera; only drawable BG1 defines an aperture. */
static void PrepareDioramaAperture(const DioramaCapture *capture,
                                   const DioramaScene *scene,
                                   const DioramaViewGeometry *geometry,
                                   const ArRenderTexture *textures,
                                   const DioramaResolvedLayer *resolved,
                                   int resolved_count,
                                   DioramaFocalAperture *aperture) {
  int32_t focal_aperture_indices[DIORAMA_INDICES_PER_LAYER];
  int focal_aperture_nv = 0, focal_aperture_ni = 0;
  aperture->z = kDofFocalZ;
  aperture->valid = false;
  for (int i = 0; i < resolved_count; i++) {
    if (resolved[i].plane != SR_PPU_OVERLAY_BG1 || resolved[i].alpha == 0)
      continue;
    const DioramaLayerDesc *aperture_layer =
        DioramaDescForPlane(SR_PPU_OVERLAY_BG1);
    if (!DioramaLayerIsDrawable(aperture_layer, textures, capture->pixels, scene))
      break;
    aperture->z = resolved[i].z;
    BuildLayerMesh(geometry->matrix, aperture->z - 0.5f, resolved[i].rake,
                   resolved[i].bow, geometry->u0, geometry->v0, geometry->u1,
                   geometry->v1, geometry->aspect_x, geometry->height_scale,
                   geometry->width, geometry->height,
                   (ArRenderColorF){1.0f, 1.0f, 1.0f, 1.0f}, aperture->vertices, NULL,
                   focal_aperture_indices, &focal_aperture_nv,
                   &focal_aperture_ni);
    aperture->valid = focal_aperture_nv == (DIORAMA_SUBDIV_X+1)*(DIORAMA_SUBDIV_Y+1) &&
                      focal_aperture_ni == DIORAMA_SUBDIV_X*DIORAMA_SUBDIV_Y*6;
    break;
  }
}

/* Draw and effect culling share the same captured guard columns. */
static bool DioramaUsesCaptureApron(const DioramaCapture *capture, int plane) {
  return capture->obj_apron > 0 && (DioramaPlaneIsObjectPriority(plane) ||
      DioramaPlaneUsesBgApron(plane, capture->bg_apron_mask));
}

static int VoxelGuardEffectRows(const DioramaCapture *, const DioramaScene *);

/* An attached effect may retain a transform on an intentionally empty
 * source band. Publish the exact authored geometry used by layer drawing. */
static void PublishDioramaPlanes(const DioramaCapture *capture,
                                 const DioramaScene *scene,
                                 const DioramaViewGeometry *geometry,
                                 const ArRenderTexture *textures,
                                 const DioramaResolvedLayer *resolved,
                                 int resolved_count,
                                 DioramaProjection *out_projection) {
  if (out_projection) {
    for (int i = 0; i < resolved_count; i++) {
      if (resolved[i].alpha == 0) continue;
      const DioramaLayerDesc *layer = DioramaDescForPlane(resolved[i].plane);
      if (!DioramaLayerIsProjectable(layer, textures, capture->pixels, scene))
        continue;
      DioramaPlaneProjection plane = {
          .valid = true,
          .capture_apron = DioramaUsesCaptureApron(capture, resolved[i].plane)
              ? capture->obj_apron : 0,
          .u0 = geometry->u0,
          .v0 = geometry->v0,
          .u1 = geometry->u1,
          .v1 = geometry->v1,
          .z_world = resolved[i].z - 0.5f,
          .rake = resolved[i].rake,
          .bow = resolved[i].bow,
      };
      if(ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number)&&
          (resolved[i].plane==SR_PPU_OVERLAY_BG1||
           (DioramaPlaneIsObjectPriority(resolved[i].plane)&&ActionVoxel_PlayRecess(scene->voxel_geometry)>0))){
        if(resolved[i].plane==SR_PPU_OVERLAY_BG1) {
          plane.source_bounds_valid=true;
          plane.source_u0=plane.u0;plane.source_v0=plane.v0;
          plane.source_u1=plane.u1;plane.source_v1=plane.v1;
          /* Native source guards already draw below the primary image. The
           * procedural mist/spray/particle clip must cover that same finite
           * world area without changing image UVs or its physical plane. */
          plane.source_v1 += (float)VoxelGuardEffectRows(capture,scene) /
              (float)SR_PPU_SURFACE_MAX_HEIGHT;
        }
        const float scale=VoxelPlaneScale(geometry,scene->voxel_geometry,resolved[i].plane,resolved[i].z);
        const float u=.5f*(plane.u0+plane.u1),v=.5f*(plane.v0+plane.v1);
        const float du=.5f*(plane.u1-plane.u0)/scale,dv=.5f*(plane.v1-plane.v0)/scale;
        plane.u0=u-du;plane.u1=u+du;plane.v0=v-dv;plane.v1=v+dv;
      }
      if (resolved[i].plane == SR_PPU_OVERLAY_BG2 ||
          resolved[i].plane == kDioramaPlane_Bg2Hi)
        plane.world_y_offset = geometry->bg2_world_y_offset;
      if (capture->plane_capture_offsets)
        plane.capture_offset = capture->plane_capture_offsets[resolved[i].plane];
      if (resolved[i].plane == SR_PPU_OVERLAY_BG1) {
        out_projection->bg1_plane = plane;
      }
      if (resolved[i].plane == SR_PPU_OVERLAY_BG2) {
        out_projection->bg2_plane = plane;
      }
      if (resolved[i].plane == kDioramaPlane_Bg1Hi) {
        out_projection->bg1_high_plane = plane;
      }
      if (resolved[i].plane == kDioramaPlane_Bg1Far) {
        out_projection->bg1_far_plane = plane;
      }
      if (resolved[i].plane == kDioramaPlane_Bg2Hi) {
        out_projection->bg2_high_plane = plane;
      }
      const int priority = DioramaPlaneObjectPriority(resolved[i].plane);
      if (priority < 0) continue;
      out_projection->object_planes[priority] = plane;
    }
    /* Publish front-rock and walking contacts even if their native priority
     * layers are empty. Forest plants use the walking contact for all BG1
     * responses; cave drips and landing dust resolve their contacts by kind. */
    out_projection->voxel_scenery=ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number);
    if(out_projection->voxel_scenery &&
        out_projection->bg1_far_plane.valid) {
      if(!out_projection->bg1_high_plane.valid)
        out_projection->bg1_high_plane=out_projection->bg1_far_plane;
      DioramaPlaneProjection surface=out_projection->bg1_far_plane;
      const float recess=ActionVoxel_PlayRecess(scene->voxel_geometry);
      const float scale=1+recess/fmaxf(1,geometry->camera.distance);
      const float u=.5f*(surface.u0+surface.u1),v=.5f*(surface.v0+surface.v1);
      const float du=.5f*(surface.u1-surface.u0)/scale,dv=.5f*(surface.v1-surface.v0)/scale;
      surface.u0=u-du;surface.u1=u+du;surface.v0=v-dv;surface.v1=v+dv;
      surface.z_world=-recess;
      if(!out_projection->object_planes[2].valid)out_projection->object_planes[2]=surface;
      if(ActionVoxel_Profile(scene->voxel_geometry)==kActionVoxelProfile_FillmoreForest)
        out_projection->bg1_plane=surface;
      if(ActionVoxel_Profile(scene->voxel_geometry)==kActionVoxelProfile_BloodpoolMarsh)
        out_projection->bg1_plane=out_projection->bg1_far_plane;
    }
    /* Apply after the existing actor/contact fallback publication: an empty
     * actor band must never inherit the atmospheric texture mapping. */
    for (int i=0;i<resolved_count;++i) {
      DioramaDeathHeimNebulaWindow nebula;
      if (!DeathHeimNebulaWindow(capture,scene,geometry,&resolved[i],&nebula)) continue;
      DioramaPlaneProjection *plane=resolved[i].plane==SR_PPU_OVERLAY_BG1
          ? &out_projection->bg1_plane : resolved[i].plane==kDioramaPlane_Bg1Hi
          ? &out_projection->bg1_high_plane : &out_projection->bg1_far_plane;
      if (!plane->valid) continue;
      plane->native_atmosphere=true;
      plane->capture_offset=(ArRenderPointF){0}; /* Already in the window. */
      plane->capture_apron=0;
      plane->u0=nebula.u0;plane->v0=nebula.v0;
      plane->u1=nebula.u1;plane->v1=nebula.v1;
      plane->source_bounds_valid=true;
      plane->source_u0=plane->u0;plane->source_v0=plane->v0;
      plane->source_u1=plane->u1;plane->source_v1=plane->v1;
    }
    out_projection->valid = true;
  }
}

/* Normal layers retain authored painter order. Full-add scenes draw main
 * world, then sparse subscreen addends, then the non-math HUD. */
static int ResolveDioramaDrawOrder(const DioramaScene *scene,
                                   const DioramaResolvedLayer *resolved,
                                   int resolved_count,
                                   int draw_order[kDioramaLayerCount]) {
  int draw_count = 0;
  const int blend_pass_count = scene->additive_plane_mask ? 3 : 1;
  for (int blend_pass = 0; blend_pass < blend_pass_count; blend_pass++) {
    for (int i = 0; i < resolved_count; i++) {
      const int plane = resolved[i].plane;
      const bool additive =
          (scene->additive_plane_mask & (1u << (unsigned)plane)) != 0;
      int plane_pass = 0;
      if (scene->additive_plane_mask)
        plane_pass = additive ? 1 : plane == SR_PPU_OVERLAY_BG3 ? 2 : 0;
      if (plane_pass == blend_pass)
        draw_order[draw_count++] = i;
    }
  }
  return draw_count;
}

typedef struct DioramaVoxelGuardFilter {
  ArRenderTexture texture;
  bool ready,compact,priority;
} DioramaVoxelGuardFilter;
typedef struct DioramaDesertColumnMask DioramaDesertColumnMask;
typedef struct DioramaLayerDraw {
  const DioramaDepthSink *depth_sink;
  const DioramaResolvedLayer *authored;
  const DioramaLayerDesc *description;
  ArRenderTexture texture;
  ArRenderTexture priority_surface;
  ArRenderTexture voxel_rear_texture; /* Native art for folded rear ground. */
  ArRenderColorF shade;
  float dimming;
  float hollow_recess;
  float voxel_depth,voxel_recess;
  const ActionVoxelGeometry *voxel_geometry;
  ArRenderRectF dimming_ramp;
  const SceneryShadingField *exposure;
  DioramaSceneryLightEffectParams lighting;
  ArRenderRectF light_viewport;
  ArRenderBlendMode blend;
  const DioramaVoxelGuardView *voxel_guard;
  DioramaVoxelGuardFilter *voxel_guard_filter;
  bool voxel_guard_priority;
  const DioramaDesertColumnMask *desert_columns;
  bool desert_columns_only;
  int voxel_source_plane; /* -1 follows face ownership; helpers name their native source. */
  ArRenderRectF voxel_source_rect; /* Optional primary-relative native domain. */
} DioramaLayerDraw;

static void ShadeDioramaVertices(const DioramaLayerDraw *layer,
    ArRenderVertex2D *vertices, int count) {
  if (layer->dimming <= 0) return;
  for (int i = 0; i < count; i++) {
    float light = 1-SceneryDimming_Amount(layer->dimming,layer->dimming_ramp,
        vertices[i].tex_coord.x,vertices[i].tex_coord.y);
    vertices[i].color.r *= light;
    vertices[i].color.g *= light;
    vertices[i].color.b *= light;
  }
}

#include "diorama_voxel_guard_render.inc"
#include "diorama_desert_columns_render.inc"

/* Joined BG1 faces need complementary filter coverage, not two independently
 * softened alpha silhouettes. Keep their draw slots (and intervening sprites)
 * intact. Only pair opaque, coplanar, in-focus faces with matching projection;
 * an intentionally separated/deformed/translucent high plane keeps its own
 * treatment. Depth copies still use the authored source and shape. Attached
 * effects do not disqualify the pair: the shader is unbound before each
 * callback, and callbacks retain their original textures, projection and draw
 * slots. Bloodpool's mist precedes high water; reflections follow it. Disjoint
 * full-add captures (Marahna) use the same premultiplied sampling, but sum
 * coverage without the source-over correction. Never pair mixed blend modes. */
static ArRenderTexture BuildDioramaPrioritySurface(
    ArRenderDevice *device, const DioramaCapture *capture,
    const DioramaScene *scene, const ArRenderTexture *textures,
    const DioramaResolvedLayer *resolved, int count,
    PresentationOutcome *outcome,bool *guard_pair) {
  *outcome = kPresentationOutcome_Complete;
  *guard_pair=false;
  if (!scene->render->priority_surface) return ArRenderTexture_Invalid();
  const int low_plane=ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number)
      ?kDioramaPlane_Bg1Far:SR_PPU_OVERLAY_BG1;
  const DioramaResolvedLayer *low = NULL, *high = NULL;
  for (int i = 0; i < count; i++) {
    if (resolved[i].plane == low_plane) low = &resolved[i];
    if (resolved[i].plane == kDioramaPlane_Bg1Hi) high = &resolved[i];
  }
  const uint32_t pair_mask = (1u << low_plane) |
                             (1u << kDioramaPlane_Bg1Hi);
  const uint32_t additive_pair = scene->additive_plane_mask & pair_mask;
  if (!low || !high || low >= high || low->alpha != 255 || high->alpha != 255 ||
      low->z != high->z || low->rake != high->rake || low->bow != high->bow ||
      low->z != kDofFocalZ || high->stack > 0 || high->thickness > 0 ||
      (additive_pair && additive_pair != pair_mask))
    return ArRenderTexture_Invalid();
  if (!DioramaEffectBackend_IsAvailable(device, kDioramaEffect_PrioritySurface))
    return ArRenderTexture_Invalid();
  *guard_pair=VoxelGuardMatches(capture,scene->voxel_geometry,scene->voxel_guard)&&
      (scene->render->visible_planes&pair_mask)==pair_mask;
  if(!ArRenderTexture_IsValid(textures[low->plane])||!ArRenderTexture_IsValid(textures[high->plane])||
      !DioramaLayerIsDrawable(DioramaDescForPlane(low->plane), textures,
                               capture->pixels, scene) ||
      !DioramaLayerIsDrawable(DioramaDescForPlane(high->plane), textures,
                               capture->pixels, scene))
    return ArRenderTexture_Invalid();

  if (s_diorama_priority_w != capture->width ||
      s_diorama_priority_h != capture->height) {
    ResetDioramaPrioritySurface(device);
    const ArRenderTextureDesc desc = {
      .width = capture->width, .height = capture->height * 2,
      .format = kArRenderPixelFormat_Argb8888,
      .usage = kArRenderTextureUsage_Target,
      .filter = kArRenderFilter_Nearest, .blend = kArRenderBlendMode_Alpha,
    };
    if (!ArRenderDevice_CreateTexture(device, &desc, &s_diorama_priority_texture)) {
      *outcome = kPresentationOutcome_OptionalOmitted;
      return ArRenderTexture_Invalid();
    }
    s_diorama_priority_w = capture->width;
    s_diorama_priority_h = capture->height;
  }
  ArRenderTargetState saved;
  const ArRenderTargetBeginResult begin = ArRenderDevice_BeginTarget(
      device, s_diorama_priority_texture, &saved);
  if (begin != kArRenderTargetBegin_Ready) {
    *outcome = begin == kArRenderTargetBegin_StateLost
        ? kPresentationOutcome_CoreFailure : kPresentationOutcome_OptionalOmitted;
    return ArRenderTexture_Invalid();
  }
  const ArRenderRectF src = {
    (float)capture->obj_apron, 0, (float)capture->width, (float)capture->height,
  };
  const ArRenderDrawState state = {
    .flags = kArRenderDrawState_Blend, .blend = kArRenderBlendMode_Opaque,
  };
  bool success = true;
  for (int band = 0; band < 2; band++) {
    const ArRenderRectF dst = {
      0, (float)(band * capture->height), (float)capture->width, (float)capture->height,
    };
    success = ArRenderDevice_DrawTextureWithState(device,
        textures[band ? high->plane : low->plane], &src, &dst, &state) && success;
  }
  if (!ArRenderDevice_EndTarget(device, &saved)) {
    *outcome = kPresentationOutcome_CoreFailure;
    return ArRenderTexture_Invalid();
  }
  if (!success) {
    *outcome = kPresentationOutcome_OptionalOmitted;
    return ArRenderTexture_Invalid();
  }
  // Check both bindings before either face reaches the scene. A backend may
  // decline the optional effect without leaving a half-corrected pair.
  for (int band = 0; band < 2; band++) {
    const DioramaPrioritySurfaceEffectParams params = {
      (float)capture->width, (float)capture->height, band != 0,
      additive_pair != 0,
    };
    const bool bound = DioramaEffectBackend_BindPrioritySurface(device, &params);
    if (!DioramaEffectBackend_Unbind(device)) {
      *outcome = kPresentationOutcome_CoreFailure;
      return ArRenderTexture_Invalid();
    }
    if (!bound) {
      *outcome = kPresentationOutcome_OptionalOmitted;
      return ArRenderTexture_Invalid();
    }
  }
  return s_diorama_priority_texture;
}

typedef struct DioramaLayerMesh {
  ArRenderVertex2D vertices[DIORAMA_VERTS_PER_LAYER];
  float depths[DIORAMA_VERTS_PER_LAYER];
  int32_t indices[DIORAMA_INDICES_PER_LAYER];
  int vertex_count, index_count;
  bool constrained;
  bool has_depth;
  bool flat_native_grid;
  float repeat_y;
} DioramaLayerMesh;

/* Reuse the existing skybox inverse-plane mapping for depth only. Native
 * source pixels map linearly across the viewport, while this layer retains
 * its actual world-plane depth and native paint slot relative to actors. */
static void PrepareDeathHeimNebulaMesh(const DioramaViewGeometry *g,
    const DioramaLayerDraw *layer, const DioramaDeathHeimNebulaWindow *window,
    DioramaLayerMesh *mesh) {
  memset(mesh,0,sizeof(*mesh));
  mesh->constrained=true; /* The viewport owns the edge; no in-view AA fringe. */
  mesh->has_depth=layer->depth_sink!=NULL;
  for (int row=0;row<=DIORAMA_SUBDIV_Y;++row)
    for (int col=0;col<=DIORAMA_SUBDIV_X;++col) {
      const float s=(float)col/DIORAMA_SUBDIV_X,t=(float)row/DIORAMA_SUBDIV_Y;
      const unsigned index=(unsigned)mesh->vertex_count++;
      ArRenderVertex2D *v=mesh->vertices+index;
      v->position=(ArRenderPointF){s*g->width,t*g->height};
      v->tex_coord=(ArRenderPointF){window->u0+s*(window->u1-window->u0),
          window->v0+t*(window->v1-window->v0)};
      v->color=layer->shade;
      if (mesh->has_depth)
        mesh->depths[index]=(1-t)*((1-s)*window->depths[0]+s*window->depths[1])+
            t*((1-s)*window->depths[2]+s*window->depths[3]);
    }
  TriangulateGrid(DIORAMA_SUBDIV_X,DIORAMA_SUBDIV_Y,mesh->indices,&mesh->index_count);
  ShadeDioramaVertices(layer,mesh->vertices,mesh->vertex_count);
}

typedef struct DioramaAttachedMesh {
  /* Enough room to append the host plane and its optional edge fringe. */
  ArRenderVertex2D vertices[DIORAMA_ATTACHED_AA_VERTS];
  int32_t indices[DIORAMA_ATTACHED_AA_INDICES];
  int vertex_count, index_count;
  float lower_content_v_max;
  float cloud_source_v_offset; /* Repeated art UV -> native receiver UV. */
} DioramaAttachedMesh;

#include "action/action_trunk_life.h"
#include "diorama_trunk_side.h"
static ArRenderColorF LifeWallColor(float,float,ArRenderColorF);
static bool LifeLightingActive(void);
static ArRenderColorF LifeBranchColor(float,float);
/* Draw-local inputs; only derived contour/material caches persist. Landing
 * events are immutable producer snapshots and never advance here. */
static const ActionTrunkAssets *s_trunk_assets;
static DioramaTrunkSettings s_trunk_settings;
static float s_trunk_fade;
static float TrunkRecess(void) {return s_trunk_settings.enabled?s_trunk_settings.recess:0;}
static void PrepareHollowMesh(const DioramaCapture *capture,
    const DioramaViewGeometry *g, const DioramaLayerDraw *layer,
    float first_y,float last_y,float repeat_y,DioramaLayerMesh *mesh) {
  const float origin=capture->bg1_camera_x-(capture->width-256)*.5f;
  const float left=fmaxf(352,origin),right=fminf(672,origin+capture->width);
  mesh->vertex_count=mesh->index_count=0;
  mesh->repeat_y=repeat_y;
  mesh->constrained=true; /* Rims are hidden under bark; no rectangular-grid fringe. */
  mesh->has_depth=layer->depth_sink!=NULL;
  if(right<=left)return;
  float columns[HOLLOW_COLUMNS];int count=0;
  columns[count++]=left;
  for(int x=360;x<672;x+=8)if(x>left&&x<right)columns[count++]=(float)x;
  columns[count++]=right;
  float rows[HOLLOW_ROWS];int row_count=0;
  if(LifeLightingActive()) {
    const float oy=capture->camera_y-capture->authentic_y0+1;
    rows[row_count++]=first_y;
    for(float y=floorf((oy+first_y)/32)*32+32;y<oy+last_y;y+=32)rows[row_count++]=y-oy;
    rows[row_count++]=last_y;
  } else for(int row=0;row<=DIORAMA_SUBDIV_Y;++row)rows[row_count++]=first_y+(last_y-first_y)*row/DIORAMA_SUBDIV_Y;
  for(int row=0;row<row_count;++row)for(int col=0;col<count;++col) {
    const float world_x=columns[col],s=(world_x-origin)/capture->width;
    const float local_y=rows[row];
    const float t=local_y/capture->height,q=(world_x-512)/160;
    const float z=layer->authored->z-.5f-TrunkRecess()-s_trunk_settings.depth*fmaxf(0,1-q*q);
    const int index=mesh->vertex_count++;
    ArRenderVertex2D *v=&mesh->vertices[index];
    if(!ProjectWorldPointDepth(g->matrix,(s-.5f)*g->aspect_x,(.5f-t)*g->height_scale,
        z,g->width,g->height,&v->position,mesh->has_depth?mesh->depths+index:NULL)) {mesh->vertex_count=0;return;}
    v->tex_coord=(ArRenderPointF){g->u0+s*(g->u1-g->u0),local_y/SR_PPU_SURFACE_MAX_HEIGHT};
    v->color=LifeWallColor(world_x,capture->camera_y-capture->authentic_y0+1+local_y,layer->shade);
  }
  TriangulateGrid(count-1,row_count-1,mesh->indices,&mesh->index_count);
  ShadeDioramaVertices(layer,mesh->vertices,mesh->vertex_count);
  for(int i=0;i<mesh->vertex_count;++i)mesh->vertices[i].tex_coord.y-=repeat_y/SR_PPU_SURFACE_MAX_HEIGHT;
}

static void PrepareHollowRegionMesh(const DioramaCapture *capture,
    const DioramaViewGeometry *g,const DioramaLayerDraw *layer,
    float region_left,float region_right,float front_z,bool rear,DioramaLayerMesh *mesh) {
  memset(mesh,0,sizeof(*mesh));mesh->constrained=true;mesh->has_depth=layer->depth_sink!=NULL;
  const float ox=capture->bg1_camera_x-(capture->width-256)*.5f-capture->obj_apron;
  const float oy=capture->camera_y-capture->authentic_y0+1;
  const float left=fmaxf(region_left,ox+g->u0*SR_PPU_SURFACE_MAX_WIDTH);
  const float right=fminf(region_right,ox+g->u1*SR_PPU_SURFACE_MAX_WIDTH);
  const float top=fmaxf(0,oy+g->v0*SR_PPU_SURFACE_MAX_HEIGHT);
  const float bottom=fminf(s_trunk_assets->height,oy+g->v1*SR_PPU_SURFACE_MAX_HEIGHT);
  if(right<=left||bottom<=top)return;
  float columns[4];int n=0;columns[n++]=left;
  const float joints[]={288,736};
  for(unsigned i=0;i<2;++i)if(joints[i]>left&&joints[i]<right)columns[n++]=joints[i];
  columns[n++]=right;
  for(unsigned row=0;row<2;++row)for(int col=0;col<n;++col) {
    const float world_x=columns[col],world_y=row?bottom:top;
    const float u=(world_x-ox)/SR_PPU_SURFACE_MAX_WIDTH,v=(world_y-oy)/SR_PPU_SURFACE_MAX_HEIGHT;
    const float x=((u-g->u0)/(g->u1-g->u0)-.5f)*g->aspect_x;
    const float y=(.5f-(v-g->v0)/(g->v1-g->v0))*g->height_scale;
    const float z=front_z-(rear?DioramaTrunk_SideDepth(layer->hollow_recess,world_x):0);
    const unsigned index=(unsigned)mesh->vertex_count++;ArRenderVertex2D *vertex=&mesh->vertices[index];
    if(!ProjectWorldPointDepth(g->matrix,x,y,z,g->width,g->height,&vertex->position,
        mesh->has_depth?mesh->depths+index:NULL)){mesh->vertex_count=0;return;}
    vertex->tex_coord=(ArRenderPointF){u,v};vertex->color=layer->shade;
  }
  TriangulateGrid(n-1,1,mesh->indices,&mesh->index_count);
  ShadeDioramaVertices(layer,mesh->vertices,mesh->vertex_count);
}

#include "diorama_ground_cover_render.inc"
#include "diorama_trunk_edges.inc"
#include "diorama_voxel_edges.inc"
#include "diorama_voxel_authored.inc"
#include "diorama_voxel_sprites.inc"
#include "diorama_trunk_life.inc"
#include "diorama_voxel_observe.inc"
#include "diorama_castle_matte.h"
#include "diorama_castle_matte.inc"
#include "diorama_jungle_terminal.h"
#include "diorama_jungle_terminal.inc"

static void PrepareDioramaLayerMesh(const DioramaCapture *capture,
                                    const DioramaViewGeometry *geometry,
                                    const DioramaFocalAperture *aperture,
                                    const DioramaLayerDraw *layer,
                                    DioramaLayerMesh *mesh) {
  mesh->has_depth=layer->depth_sink!=NULL;mesh->flat_native_grid=false;
  if(layer->voxel_depth>0 && layer->description->plane==SR_PPU_OVERLAY_BG1 &&
      ActionVoxel_HasRearRelief(layer->voxel_geometry)) {
    PrepareVoxelRearMesh(capture,geometry,layer,mesh);
    return;
  }
  const float z_world = layer->authored->z - 0.5f;
  DioramaViewGeometry voxel_geometry;
  if(layer->voxel_depth>0&&(layer->description->plane==SR_PPU_OVERLAY_BG1||
      (DioramaPlaneIsObjectPriority(layer->description->plane)&&layer->voxel_recess>0))){
    voxel_geometry=*geometry;const float scale=VoxelPlaneScale(geometry,layer->voxel_geometry,layer->description->plane,layer->authored->z);
    voxel_geometry.aspect_x*=scale;voxel_geometry.height_scale*=scale;geometry=&voxel_geometry;
  }
  BuildLayerMesh(geometry->matrix, z_world, layer->authored->rake,
                 layer->authored->bow, geometry->u0, geometry->v0, geometry->u1,
                 geometry->v1, geometry->aspect_x, geometry->height_scale,
                 geometry->width, geometry->height, layer->shade,
                 mesh->vertices, mesh->has_depth?mesh->depths:NULL, mesh->indices, &mesh->vertex_count,
                 &mesh->index_count);
  ShadeDioramaVertices(layer,mesh->vertices,mesh->vertex_count);
  mesh->constrained = false;
  mesh->repeat_y = 0;
  mesh->flat_native_grid=layer->voxel_guard&&layer->authored->rake==0&&layer->authored->bow==0;
  if (layer->voxel_depth<=0 && layer->hollow_recess<=0 && aperture->valid && layer->description->plane != SR_PPU_OVERLAY_BG1 &&
      LayerUsesFocalAperture(layer->description->plane) &&
      layer->authored->z > aperture->z + 0.0001f) {
    mesh->constrained =
        DioramaAperture_ConstrainGrid(mesh->vertices, aperture->vertices,
                                      DIORAMA_SUBDIV_X, DIORAMA_SUBDIV_Y, 2.0f);
  }
  if (capture->coverage_masks &&
      DioramaSparseCoverageEnabledForPlane(layer->description->plane, geometry->options)) {
    const DioramaCoverageMask coverage =
        capture->coverage_masks[layer->description->plane];
    if (coverage && coverage != DioramaCoverage_FullMask())
      mesh->index_count = DioramaCoverage_FilterGridIndices(
          mesh->indices, mesh->index_count, coverage);
  }
  ObserveVoxelLayerMesh(capture, geometry, layer, mesh);
}

/* Continue from the authentic bottom through the last drawable texel plus
 * tolerance. Voxel keeps the waterfall behind the terrain; ordinary Diorama
 * retains its forward fold. Low BG2 publishes the same continuation for
 * atmosphere projection; the higher bands use the same geometry. */
static void PrepareDioramaWaterfall(const DioramaCapture *capture,
                                    const DioramaScene *scene,
                                    const DioramaViewGeometry *geometry,
                                    const DioramaLayerDraw *layer,
                                    DioramaAttachedMesh *attached,
                                    DioramaProjection *out_projection) {
  const float z_world = layer->authored->z - 0.5f;
  attached->vertex_count = attached->index_count = 0;

  const bool aitos_waterfall_extension =
      ActRaiserRoom_ProfileFor(scene->map_group, scene->map_number) ==
          kActRaiserRoomProfile_AitosWaterfall &&
      scene->layer_section == kDioramaLayerSection_AitosWaterfall &&
      (layer->description->plane == SR_PPU_OVERLAY_BG2 ||
       layer->description->plane == kDioramaPlane_Bg2Hi ||
       layer->description->plane == kDioramaPlane_Bg2Far);
  attached->lower_content_v_max = 0.0f;
  attached->cloud_source_v_offset = 0.0f;

  float log_fold_t = -1.0f, log_overlap_t = -1.0f;
  int log_drawable_y1 = -1;
  if (aitos_waterfall_extension) {
    DioramaVerticalRepeatPlan repeat;
    const float layer_v_span = geometry->v1 - geometry->v0;
    if (DioramaVerticalRepeatPlan_Build(
            capture->authentic_y0, kActRaiserAuthenticHeight, capture->height,
            SR_PPU_SURFACE_MAX_HEIGHT, &repeat) &&
        layer_v_span > 0.0f) {
      const float layer_v_shift = geometry->v0 - geometry->v0;
      int drawable_y1 = 0;
      if (!DioramaBgValidSpanPlan_DrawableRowBounds(capture->bg2_valid_spans,
                                                    NULL, &drawable_y1) ||
          drawable_y1 <= 0 || drawable_y1 > capture->height) {
        drawable_y1 = capture->height;
      }

      attached->lower_content_v_max =
          (float)drawable_y1 / (float)SR_PPU_SURFACE_MAX_HEIGHT + layer_v_shift;
      log_drawable_y1 = drawable_y1;
      const float fold_v =
          (float)repeat.fold_y / (float)SR_PPU_SURFACE_MAX_HEIGHT +
          layer_v_shift;
      const float source_v0 =
          (float)repeat.source_y0 / (float)SR_PPU_SURFACE_MAX_HEIGHT +
          layer_v_shift;
      const float source_v1 =
          (float)repeat.source_y1 / (float)SR_PPU_SURFACE_MAX_HEIGHT +
          layer_v_shift;
      const float fold_t = (fold_v - geometry->v0) / layer_v_span;
      const float extension_height =
          (float)repeat.repeat_height / (float)kActRaiserAuthenticHeight;
      if (fold_t >= 0.0f && fold_t <= 1.0f && source_v0 >= 0.0f &&
          source_v1 <= 1.0f) {
        const float extension_y = (0.5f - fold_t) * geometry->height_scale;
        const float extension_z = DioramaTiltedRowDepth(
            z_world, layer->authored->rake, layer->authored->bow, fold_t);

        float overlap_t = (float)(drawable_y1 - repeat.fold_y +
                                  kAitosWaterfallSeamTolerancePixels) /
                          (float)repeat.repeat_height;
        if (overlap_t < 0.0f)
          overlap_t = 0.0f;
        if (overlap_t > 1.0f)
          overlap_t = 1.0f;
        float handoff_t =
            fold_t + overlap_t * extension_height / geometry->height_scale;
        if (handoff_t > 1.0f)
          handoff_t = 1.0f;
        const float handoff_z = DioramaTiltedRowDepth(
            z_world, layer->authored->rake, layer->authored->bow, handoff_t);
        const bool coplanar = ActionVoxel_Active(
            scene->voxel_geometry, scene->map_group, scene->map_number);
        const float front_z = coplanar ? handoff_z : kShoeboxZFront;
        const float front_drop = coplanar
            ? extension_height * (1.0f - overlap_t) : kAitosWaterfallFrontDrop;
        log_fold_t = fold_t;
        log_overlap_t = overlap_t;
        BuildFoldedOverflowMesh(
            geometry->matrix, extension_y, extension_z, handoff_z,
            extension_height, overlap_t, front_z,
            front_drop, geometry->u0, source_v0, geometry->u1,
            source_v1, geometry->aspect_x, geometry->width, geometry->height,
            layer->shade, attached->vertices, attached->indices,
            &attached->vertex_count, &attached->index_count);

        if (attached->vertex_count > 0 && coplanar &&
            DioramaCloudLight_Active(&layer->lighting.cloud_light))
          attached->cloud_source_v_offset =
              (float)(repeat.fold_y - repeat.source_y0) / SR_PPU_SURFACE_MAX_HEIGHT;

        if (attached->vertex_count > 0 && out_projection &&
            layer->description->plane == SR_PPU_OVERLAY_BG2 &&
            out_projection->bg2_plane.valid) {
          DioramaPlaneProjection *plane = &out_projection->bg2_plane;
          plane->overflow_valid = true;
          plane->overflow_fold_t = fold_t;
          plane->overflow_height = extension_height;
          plane->overflow_overlap_t = overlap_t;
          plane->overflow_handoff_z = handoff_z;
          plane->overflow_front_z = front_z;
          plane->overflow_front_drop = front_drop;
        }
      }
    }
  }
  if (layer->description->plane == SR_PPU_OVERLAY_BG2)
    DioramaAitosWaterfallLog(scene->render->waterfall_diagnostics,
                             scene->map_group, scene->map_number,
                             scene->layer_section, aitos_waterfall_extension,
                             attached->vertex_count, capture->authentic_y0,
                             capture->height, log_drawable_y1, log_fold_t,
                             log_overlap_t);
}

#include "diorama_death_heim_water.inc"

/* Depth copies and the underside draw before the host face. They retain
 * the source texture, omit shadows/DOF, and never apply to the backdrop.
 * Stack groups preserve authored back-to-front order and sparse coverage. */
static PresentationOutcome
DrawDioramaLayerDepth(ArRenderDevice *device, const DioramaCapture *capture,
                      const DioramaViewGeometry *geometry,
                      const DioramaFocalAperture *aperture,
                      const DioramaLayerDraw *layer) {
  const float z_world = layer->authored->z - 0.5f;
  PresentationOutcome outcome = kPresentationOutcome_Complete;
  if (layer->authored->stack > 0.0f && layer->authored->stack_copies > 1 &&
      !(layer->description->plane == kDioramaPlane_Backdrop)) {
    int stack_batch_nv = 0, stack_batch_ni = 0;
    DioramaStackGroupBounds copy_bounds[kDioramaVoxelMax];
    int copy_bound_count = 0;
    for (int c = layer->authored->stack_copies - 1; c >= 0; c--) {
      if (DioramaStackCopyIsRedundant(c, layer->authored->stack_copies,
                                      layer->authored->stack_direction))
        continue;
      float copy_z = z_world, copy_shade = 1.0f, copy_alpha = 1.0f;
      DioramaStackCopyShaped(
          c, layer->authored->stack_copies, z_world, layer->authored->stack,
          layer->authored->stack_direction, layer->authored->stack_solid,
          &copy_z, &copy_shade, &copy_alpha);
      ArRenderColorF copy_color = layer->shade;
      copy_color.r *= copy_shade;
      copy_color.g *= copy_shade;
      copy_color.b *= copy_shade;
      copy_color.a *= copy_alpha;

      ArRenderVertex2D *copy_vertices =
          &s_diorama_stack_vertices[stack_batch_nv];
      int32_t *copy_indices = &s_diorama_stack_indices[stack_batch_ni];
      int copy_nv = 0, copy_ni = 0;
      BuildLayerMesh(geometry->matrix, copy_z, layer->authored->rake,
                     layer->authored->bow, geometry->u0, geometry->v0,
                     geometry->u1, geometry->v1, geometry->aspect_x,
                     geometry->height_scale, geometry->width, geometry->height,
                     copy_color, copy_vertices, NULL, copy_indices, &copy_nv,
                     &copy_ni);
      ShadeDioramaVertices(layer,copy_vertices,copy_nv);
      if (aperture->valid &&
          LayerUsesFocalAperture(layer->description->plane) &&
          copy_z + 0.5f > aperture->z + 0.0001f) {
        (void)DioramaAperture_ConstrainGrid(copy_vertices, aperture->vertices,
                                            DIORAMA_SUBDIV_X, DIORAMA_SUBDIV_Y,
                                            2.0f);
      }
      if (capture->coverage_masks &&
          DioramaSparseCoverageEnabledForPlane(layer->description->plane, geometry->options)) {
        const DioramaCoverageMask coverage =
            capture->coverage_masks[layer->description->plane];
        if (coverage && coverage != DioramaCoverage_FullMask())
          copy_ni = DioramaCoverage_FilterGridIndices(copy_indices, copy_ni,
                                                      coverage);
      }
      if (copy_nv <= 0 || copy_ni <= 0)
        continue;
      if (copy_bound_count < kDioramaVoxelMax &&
          DioramaStackGroupBounds_FromGeometry(copy_vertices, copy_nv,
                                               copy_indices, copy_ni,
                                               &copy_bounds[copy_bound_count]))
        copy_bound_count++;
      for (int index = 0; index < copy_ni; index++)
        copy_indices[index] += stack_batch_nv;
      stack_batch_nv += copy_nv;
      stack_batch_ni += copy_ni;
    }
    if (stack_batch_nv > 0 && stack_batch_ni > 0) {
      const DioramaStackGroupPlan stack_plan = DioramaStackGroupPlan_Build(
          geometry->width, geometry->height, capture->width, capture->height,
          copy_bounds, copy_bound_count);
      const PresentationOutcome stack_outcome = RenderDioramaStackBatch(
          device, layer->texture, s_diorama_stack_vertices, stack_batch_nv,
          s_diorama_stack_indices, stack_batch_ni, layer->blend, &stack_plan,
          geometry->width, geometry->height, geometry->options->stack_grouping, layer->exposure,
          &layer->lighting, layer->light_viewport);
      outcome = PresentationOutcome_Combine(outcome, stack_outcome);
      if (!PresentationOutcome_IsUsable(stack_outcome))
        return kPresentationOutcome_CoreFailure;
    }
  }

  if (layer->authored->thickness > 0.0f &&
      !(layer->description->plane == kDioramaPlane_Backdrop)) {
    int skirt_nv = 0, skirt_ni = 0;
    ArRenderVertex2D skirt_verts[DIORAMA_VERTS_PER_LAYER];
    int32_t skirt_indices[DIORAMA_INDICES_PER_LAYER];
    BuildLayerSkirtMesh(
        geometry->matrix, z_world, layer->authored->rake + layer->authored->bow,
        layer->authored->thickness, geometry->u0, geometry->u1, geometry->v1,
        geometry->aspect_x, geometry->height_scale, geometry->width,
        geometry->height, layer->shade, skirt_verts, skirt_indices, &skirt_nv,
        &skirt_ni);
    ShadeDioramaVertices(layer,skirt_verts,skirt_nv);
    if (skirt_nv > 0) {
      RecordOptionalDioramaDraw(
          &outcome,
          RenderShadedDioramaGeometry(device, layer->texture, skirt_verts, skirt_nv,
              skirt_indices, skirt_ni, layer->blend, layer->exposure, (ArRenderRectF){0,0,1,1}, &layer->lighting));
    }
  }

  return outcome;
}

/* Resolve compact sources before shadows, then bind the main effect for
 * exactly one submission. The attached waterfall precedes its host face in
 * that batch; the host owns their overlap. Every successful bind is unbound
 * before returning, including failures. */
/* Reuse a filtered source only within one uninterrupted wall draw. No
 * cross-frame key or lifetime: ordinary planes pass NULL. */
typedef struct DioramaFaceSource {
  bool ready,use_dof_shader;
  ArRenderTexture texture;
  int compact_scale;
  float dof_radius;
  PresentationOutcome outcome;
} DioramaFaceSource;

static PresentationOutcome DrawDioramaLayerFace(
    ArRenderDevice *device, const DioramaCapture *capture,
    const DioramaViewGeometry *geometry, const DioramaLayerDraw *layer,
    const DioramaLayerMesh *mesh, DioramaAttachedMesh *attached, DioramaFaceSource *source) {
  PresentationOutcome outcome = kPresentationOutcome_Complete;
  const bool depth_draw=mesh->has_depth && layer->depth_sink;
  if(depth_draw && attached->vertex_count)return kPresentationOutcome_CoreFailure;
  if(layer->voxel_guard&&!ArRenderTexture_IsValid(layer->texture)) {
    DioramaSceneryLightEffectParams light=layer->lighting;
    light.texel_width=1.f/SR_PPU_SURFACE_MAX_WIDTH;light.texel_height=1.f/SR_PPU_SURFACE_MAX_HEIGHT;
    light.attributes_uv=(ArRenderRectF){0,0,1,1};
    return RenderDesertColumnSource(device,layer,ArRenderTexture_Invalid(),mesh->vertices,
        depth_draw?mesh->depths:NULL,mesh->vertex_count,mesh->indices,mesh->index_count,
        (ArRenderRectF){0,0,1,1},&light,depth_draw?layer->depth_sink:NULL,mesh->flat_native_grid)?kPresentationOutcome_Complete:kPresentationOutcome_CoreFailure;
  }
  const bool priority_surface = ArRenderTexture_IsValid(layer->priority_surface);
  bool rim_light = layer->description->is_figure && RimLightEnabled(device, geometry->options);
  bool want_dof = !rim_light &&
                  layer->description->plane != SR_PPU_OVERLAY_BG3 &&
                  DofBlurEnabled(device, geometry->options);
  const float focal_bias=layer->description->plane==SR_PPU_OVERLAY_BG1||
      DioramaPlaneIsObjectPriority(layer->description->plane)?layer->hollow_recess:0;
  const float actor_focus=DioramaPlaneIsObjectPriority(layer->description->plane)?layer->voxel_recess:0;
  float dof_radius = want_dof ? DofRadiusForLayer(layer->authored->z+focal_bias+actor_focus) : 0.0f;
  if (dof_radius < 0.05f)
    dof_radius = 0.0f;
  bool want_edge = !rim_light && !mesh->constrained &&
                   LayerGetsEdgeAA(layer->description->plane) &&
                   geometry->options->edge_aa;
  bool use_dof_shader = !rim_light && dof_radius > 0.0f;
  bool use_shader = rim_light || use_dof_shader || priority_surface;

  ArRenderTexture draw_texture = priority_surface
      ? layer->priority_surface : layer->texture;
  int compact_scale = priority_surface ? 1 : 0;
  if(source&&source->ready) {
    draw_texture=source->texture;compact_scale=source->compact_scale;
    dof_radius=source->dof_radius;use_dof_shader=source->use_dof_shader;
    outcome=source->outcome;
  } else {
  if (use_dof_shader) {
    PresentationOutcome dof_source_outcome = kPresentationOutcome_Complete;
    DioramaPerformanceScope dof_source_performance =
        DioramaPerformance_Begin(kDioramaPerformance_DofSource);
    const ArRenderTexture compact = BuildDioramaDofSource(
        device, layer->texture, capture->obj_apron, capture->width,
        capture->height, &dof_source_outcome);
    DioramaPerformance_End(dof_source_performance);
    outcome = PresentationOutcome_Combine(outcome, dof_source_outcome);
    if (!PresentationOutcome_IsUsable(dof_source_outcome))
      return kPresentationOutcome_CoreFailure;
    if (ArRenderTexture_IsValid(compact)) {
      draw_texture = compact;
      compact_scale = 1;
    } else {
      dof_radius = 0.0f;
      use_dof_shader = false;
      use_shader = rim_light;
    }
  }
  if (!use_shader) {
    PresentationOutcome supersample_outcome = kPresentationOutcome_Complete;
    DioramaPerformanceScope supersample_performance =
        DioramaPerformance_Begin(kDioramaPerformance_Supersample);
    const ArRenderTexture ss = BuildDioramaSupersample(
        device, layer->texture, capture->obj_apron, capture->width,
        capture->height, &supersample_outcome);
    DioramaPerformance_End(supersample_performance);
    outcome = PresentationOutcome_Combine(outcome, supersample_outcome);
    if (!PresentationOutcome_IsUsable(supersample_outcome)) {
      return kPresentationOutcome_CoreFailure;
    }
    if (ArRenderTexture_IsValid(ss)) {
      draw_texture = ss;
      compact_scale = kDioramaSupersample;
    }
  }

    if(source)*source=(DioramaFaceSource){.ready=true,.use_dof_shader=use_dof_shader,
        .texture=draw_texture,.compact_scale=compact_scale,.dof_radius=dof_radius,.outcome=outcome};
  }

  ArRenderVertex2D compact_verts[DIORAMA_VERTS_PER_LAYER];
  const ArRenderVertex2D *draw_verts = mesh->vertices;
  float draw_u0 = geometry->u0;
  float draw_u1 = geometry->u1;
  float draw_v0 = geometry->v0;
  float draw_v1 = geometry->v1;
  float draw_lower_content_v_max = attached->lower_content_v_max;
  float draw_texel_width = 1.0f / (float)SR_PPU_SURFACE_MAX_WIDTH;
  float draw_texel_height = 1.0f / (float)SR_PPU_SURFACE_MAX_HEIGHT;
  if (compact_scale > 0) {
    memcpy(compact_verts, mesh->vertices,
           (size_t)mesh->vertex_count * sizeof(compact_verts[0]));
    RemapMeshToCompactTexture(compact_verts, mesh->vertex_count,
                              capture->obj_apron, capture->width,
                              capture->height);
    if (attached->vertex_count > 0) {
      RemapMeshToCompactTexture(attached->vertices, attached->vertex_count,
                                capture->obj_apron, capture->width,
                                capture->height);
    }
    draw_verts = compact_verts;
    draw_u0 = 0.0f;
    draw_u1 = 1.0f;
    draw_v0 = 0.0f;
    draw_v1 = 1.0f;
    if (draw_lower_content_v_max > 0.0f) {
      draw_lower_content_v_max *=
          (float)SR_PPU_SURFACE_MAX_HEIGHT / (float)capture->height;
    }
    draw_texel_width = 1.0f / (float)(capture->width * compact_scale);
    draw_texel_height = 1.0f / (float)(capture->height * compact_scale);
    if (priority_surface) {
      // Manual crisp sampling has the same footprint as the x4 intermediate.
      draw_texel_width /= kDioramaSupersample;
      draw_texel_height /= kDioramaSupersample;
    }
  }

  if (!(layer->description->plane == kDioramaPlane_Backdrop) &&
      layer->description->casts_shadow) {
    float off = (float)geometry->height * 0.004f;
    ArRenderVertex2D shadow[DIORAMA_VERTS_PER_LAYER];
    memcpy(shadow, draw_verts, (size_t)mesh->vertex_count * sizeof(shadow[0]));
    for (int v = 0; v < mesh->vertex_count; v++) {
      shadow[v].position.x += off;
      shadow[v].position.y += off;
      shadow[v].color = (ArRenderColorF){0.0f, 0.0f, 0.0f, 0.35f};
    }

    const bool shadow_blur_requested = ShadowBlurEnabled(device, geometry->options);
    bool shadow_blur_bound = false;
    if (shadow_blur_requested) {
      const DioramaBlurEffectParams params = {
          .texel_width = draw_texel_width,
          .texel_height = draw_texel_height,
          .radius = 3.0f,
      };
      shadow_blur_bound = DioramaEffectBackend_BindBlur(device, &params);
      if (!shadow_blur_bound) {
        outcome = PresentationOutcome_Combine(
            outcome, kPresentationOutcome_OptionalOmitted);
        if (!DioramaEffectBackend_Unbind(device)) {
          return kPresentationOutcome_CoreFailure;
        }
      }
    }
    RecordOptionalDioramaDraw(
        &outcome,
        RenderDioramaGeometry(device, draw_texture, shadow, mesh->vertex_count,
                              mesh->indices, mesh->index_count, layer->blend));
    if (shadow_blur_bound && !DioramaEffectBackend_Unbind(device)) {
      return kPresentationOutcome_CoreFailure;
    }
  }

  DioramaSceneryLightEffectParams lighting = layer->lighting;
  lighting.texel_width = draw_texel_width;
  lighting.texel_height = draw_texel_height;
  lighting.blur_radius = dof_radius;
  lighting.lower_content_v_max = draw_lower_content_v_max;
  lighting.priority = (DioramaPrioritySurfaceEffectParams){(float)capture->width, (float)capture->height,
      layer->description->plane == kDioramaPlane_Bg1Hi, layer->blend == kArRenderBlendMode_Add};
  lighting.rim_strength = rim_light ? .33f : 0;
  lighting.mode = priority_surface ? 2 : use_dof_shader ? 1 : 0;
  const bool material_lighting = ArRenderTexture_IsValid(lighting.illumination);
  bool layer_shader_bound = false;
  if (!depth_draw && !material_lighting && priority_surface) {
    const DioramaPrioritySurfaceEffectParams params = {
      (float)capture->width, (float)capture->height,
      layer->description->plane == kDioramaPlane_Bg1Hi,
      layer->blend == kArRenderBlendMode_Add,
    };
    layer_shader_bound = DioramaEffectBackend_BindPrioritySurface(device, &params);
    // A corrected low face cannot be followed by an uncorrected high face.
    // After the pair's successful preflight, losing state mid-scene is fatal.
    if (!layer_shader_bound) {
      (void)DioramaEffectBackend_Unbind(device);
      return kPresentationOutcome_CoreFailure;
    }
  } else if (!depth_draw && !material_lighting && rim_light) {
    const DioramaRimLightEffectParams params = {
        .texel_width = 1.0f / (float)SR_PPU_SURFACE_MAX_WIDTH,
        .texel_height = 1.0f / (float)SR_PPU_SURFACE_MAX_HEIGHT,
        .strength = 0.33f,
    };
    layer_shader_bound = DioramaEffectBackend_BindRimLight(device, &params);
  } else if (!depth_draw && !material_lighting && use_dof_shader) {
    const DioramaDofEdgeEffectParams params = {
        .texel_width = draw_texel_width,
        .texel_height = draw_texel_height,
        .blur_radius = dof_radius,
        .u_min = draw_u0,
        .u_max = draw_u1,
        .v_min = draw_v0,
        .v_max = draw_v1,

        .edge_feather = 0.0f,
        .lower_content_v_max = draw_lower_content_v_max,
    };
    layer_shader_bound = DioramaEffectBackend_BindDofEdge(device, &params);
  }
  if (!depth_draw && !material_lighting && (rim_light || use_dof_shader) && !layer_shader_bound) {
    outcome = PresentationOutcome_Combine(outcome,
                                          kPresentationOutcome_OptionalOmitted);
    if (!DioramaEffectBackend_Unbind(device)) {
      return kPresentationOutcome_CoreFailure;
    }
  }

  ArRenderRectF source_uv = compact_scale > 0
      ? (ArRenderRectF){(float)capture->obj_apron/SR_PPU_SURFACE_MAX_WIDTH,0,
          (float)capture->width/SR_PPU_SURFACE_MAX_WIDTH,
          (float)capture->height/SR_PPU_SURFACE_MAX_HEIGHT}
      : (ArRenderRectF){0,0,1,1};
  source_uv.y += mesh->repeat_y/SR_PPU_SURFACE_MAX_HEIGHT;
  lighting.attributes_uv = source_uv;
  bool main_submitted = false;
  if (attached->vertex_count > 0) {
    /* Art intentionally repeats, but a cloud field belongs to the receiving
     * world position. Give only this attached batch its receiver origin;
     * preserve the host's material and their established overlap ordering. */
    if (attached->cloud_source_v_offset != 0 && material_lighting &&
        DioramaCloudLight_Active(&lighting.cloud_light)) {
      DioramaSceneryLightEffectParams continuation_lighting = lighting;
      continuation_lighting.cloud_light.source[1] +=
          attached->cloud_source_v_offset * lighting.cloud_light.source[3];
      if (!RenderShadedDioramaGeometry(device, draw_texture, attached->vertices,
              attached->vertex_count, attached->indices, attached->index_count,
              layer->blend, layer->exposure, source_uv, &continuation_lighting))
        return kPresentationOutcome_CoreFailure;
      attached->vertex_count = attached->index_count = 0;
    }
    const int host_vertex_base = attached->vertex_count;
    memcpy(&attached->vertices[attached->vertex_count], draw_verts,
           (size_t)mesh->vertex_count * sizeof(attached->vertices[0]));
    for (int index = 0; index < mesh->index_count; index++)
      attached->indices[attached->index_count + index] =
          host_vertex_base + mesh->indices[index];
    attached->vertex_count += mesh->vertex_count;
    attached->index_count += mesh->index_count;
    if (want_edge) {
      DioramaEdgeAaMask edge_mask = kDioramaEdgeAa_All;
      if (draw_lower_content_v_max > 0.0f)
        edge_mask &= ~kDioramaEdgeAa_Bottom;
      if (!AppendDioramaEdgeFringe(
              draw_verts, NULL, 1.0f, edge_mask, draw_u0, draw_u1, draw_v0, draw_v1,
              draw_texel_width, draw_texel_height, attached->vertices, NULL,
              DIORAMA_ATTACHED_AA_VERTS, &attached->vertex_count,
              attached->indices, DIORAMA_ATTACHED_AA_INDICES,
              &attached->index_count)) {
        outcome = PresentationOutcome_Combine(
            outcome, kPresentationOutcome_OptionalOmitted);
      }
    }
    main_submitted = RenderShadedDioramaGeometry(
        device, draw_texture, attached->vertices, attached->vertex_count,
        attached->indices, attached->index_count, layer->blend, layer->exposure, source_uv, &lighting);
  } else if (want_edge) {
    ArRenderVertex2D aa_vertices[DIORAMA_AA_VERTS];
    float aa_depths[DIORAMA_AA_VERTS];
    int32_t aa_indices[DIORAMA_AA_INDICES];
    int aa_vertex_count = mesh->vertex_count;
    int aa_index_count = mesh->index_count;
    memcpy(aa_vertices, draw_verts,
           (size_t)mesh->vertex_count * sizeof(aa_vertices[0]));
    memcpy(aa_indices, mesh->indices,
           (size_t)mesh->index_count * sizeof(aa_indices[0]));
    if(depth_draw)memcpy(aa_depths,mesh->depths,(size_t)mesh->vertex_count*sizeof(*aa_depths));
    if (!AppendDioramaEdgeFringe(draw_verts, depth_draw?mesh->depths:NULL, 1.0f, kDioramaEdgeAa_All, draw_u0,
                                 draw_u1, draw_v0, draw_v1, draw_texel_width,
                                 draw_texel_height, aa_vertices, depth_draw?aa_depths:NULL,
                                 DIORAMA_AA_VERTS, &aa_vertex_count, aa_indices,
                                 DIORAMA_AA_INDICES, &aa_index_count)) {
      outcome = PresentationOutcome_Combine(
          outcome, kPresentationOutcome_OptionalOmitted);
    }
    if(layer->voxel_guard&&mesh->flat_native_grid) {
      main_submitted=RenderDesertColumnSource(device,layer,draw_texture,aa_vertices,
          depth_draw?aa_depths:NULL,mesh->vertex_count,aa_indices,mesh->index_count,
          source_uv,&lighting,depth_draw?layer->depth_sink:NULL,true);
      if(main_submitted&&aa_index_count>mesh->index_count)
        main_submitted=RenderDesertColumnSource(device,layer,draw_texture,aa_vertices,
            depth_draw?aa_depths:NULL,aa_vertex_count,aa_indices+mesh->index_count,aa_index_count-mesh->index_count,
            source_uv,&lighting,depth_draw?layer->depth_sink:NULL,false);
    } else main_submitted = layer->desert_columns ? RenderDesertColumnSource(device,layer,draw_texture,
        aa_vertices,depth_draw?aa_depths:NULL,aa_vertex_count,aa_indices,aa_index_count,
        source_uv,&lighting,depth_draw?layer->depth_sink:NULL,false) : layer->voxel_guard ? RenderVoxelNativeSources(device,layer,draw_texture,aa_vertices,
        depth_draw?aa_depths:NULL,NULL,aa_vertex_count,aa_indices,aa_index_count,
        layer->blend,layer->exposure,source_uv,&lighting,depth_draw?layer->depth_sink:NULL,true,false,false) :
      RenderShadedDioramaDepthGeometry(device, draw_texture, aa_vertices,
                                           depth_draw?aa_depths:NULL,
                                           aa_vertex_count, aa_indices,
                                           aa_index_count, layer->blend, layer->exposure, source_uv, &lighting,
                                           depth_draw?layer->depth_sink:NULL,layer->description->plane,
                                           DioramaPlaneIsObjectPriority(layer->description->plane));
  } else {
    main_submitted = layer->desert_columns ? RenderDesertColumnSource(device,layer,draw_texture,
        draw_verts,depth_draw?mesh->depths:NULL,mesh->vertex_count,mesh->indices,mesh->index_count,
        source_uv,&lighting,depth_draw?layer->depth_sink:NULL,mesh->flat_native_grid) : layer->voxel_guard ? RenderVoxelNativeSources(device,layer,draw_texture,draw_verts,
        depth_draw?mesh->depths:NULL,NULL,mesh->vertex_count,mesh->indices,mesh->index_count,
        layer->blend,layer->exposure,source_uv,&lighting,depth_draw?layer->depth_sink:NULL,true,false,mesh->flat_native_grid) :
      RenderShadedDioramaDepthGeometry(device, draw_texture, draw_verts,
                                           depth_draw?mesh->depths:NULL,
                                           mesh->vertex_count, mesh->indices,
                                           mesh->index_count, layer->blend, layer->exposure, source_uv, &lighting,
                                           depth_draw?layer->depth_sink:NULL,layer->description->plane,
                                           DioramaPlaneIsObjectPriority(layer->description->plane));
  }
  if (layer_shader_bound && !DioramaEffectBackend_Unbind(device)) {
    return kPresentationOutcome_CoreFailure;
  }
  if (!main_submitted) {
    return kPresentationOutcome_CoreFailure;
  }
  return outcome;
}

/* Side captures have finite native bounds. They never repeat the central
 * lattice or replace transparent sky with a rectangular distant backstop. */
static PresentationOutcome DrawHollowSideFaces(ArRenderDevice *device,
    const DioramaCapture *capture,const DioramaViewGeometry *geometry,
    const DioramaLayerDraw *layer,bool rear,bool include_core) {
  const float regions[3][2]={{0,320},{704,(float)s_trunk_assets->side_width},{320,704}};
  PresentationOutcome outcome=kPresentationOutcome_Complete;DioramaFaceSource source={0};
  for(unsigned i=0;i<(include_core?3u:2u);++i) {
    DioramaLayerMesh mesh;DioramaAttachedMesh attached={0};
    const float front_z=i==2?layer->authored->z-.5f:-layer->hollow_recess;
    PrepareHollowRegionMesh(capture,geometry,layer,regions[i][0],regions[i][1],front_z,rear,&mesh);
    if(!mesh.vertex_count||!mesh.index_count)continue;
    outcome=PresentationOutcome_Combine(outcome,
        DrawDioramaLayerFace(device,capture,geometry,layer,&mesh,&attached,&source));
    if(!PresentationOutcome_IsUsable(outcome))break;
  }
  return outcome;
}

#include "diorama_trunk_guard_render.inc"

/* Water curtains and material relief share the ordinary face path, including
 * filtering, exposure and alpha. Cache the source across bounded chunks. */
typedef enum VoxelFaceKind {
  kVoxelFaceRelief, kVoxelFaceWaterNative, kVoxelFaceWaterRear
} VoxelFaceKind;
static PresentationOutcome DrawVoxelPartitionedFace(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *geometry,const DioramaLayerDraw *layer,VoxelFaceKind kind) {
  unsigned count=kind!=kVoxelFaceRelief
      ? BuildVoxelWaterfallFaces(capture,geometry,layer,kind==kVoxelFaceWaterRear)
      : BuildVoxelFrontFaces(capture,geometry,layer);
  if(!count)return kind==kVoxelFaceWaterRear ||
      (kind==kVoxelFaceRelief && ActionVoxel_HasHiddenNativeFronts(layer->voxel_geometry))
      ?kPresentationOutcome_Complete:kPresentationOutcome_CoreFailure;
  /* Build/observe the complete captured faces first so editor picks and
   * geometry ownership retain their source extent. An entirely offscreen
   * native face is a successful empty draw, not a missing-source fallback. */
  count=SortVoxelSides(geometry,count);
  if(!count)return kPresentationOutcome_Complete;
  DioramaFaceSource source={0};PresentationOutcome outcome=kPresentationOutcome_Complete;
  DioramaLayerDraw depth_layer=*layer;depth_layer.depth_sink=geometry->depth_sink;
  layer=&depth_layer;
  for(unsigned first=0;first<count;) {
    DioramaLayerMesh mesh={.constrained=true,.has_depth=layer->depth_sink!=NULL};
    while(first<count && mesh.vertex_count+4<=DIORAMA_VERTS_PER_LAYER && mesh.index_count+6<=DIORAMA_INDICES_PER_LAYER) {
      const VoxelSide *side=&s_voxel_sides[s_voxel_order[first]];
      const int base=mesh.vertex_count;memcpy(mesh.vertices+base,side->v,4*sizeof(*mesh.vertices));
      if(mesh.has_depth)memcpy(mesh.depths+base,side->depths,4*sizeof(*mesh.depths));
      ++first;
      const int tri[]={0,1,2,0,2,3};for(unsigned j=0;j<6;++j)mesh.indices[mesh.index_count++]=base+tri[j];
      mesh.vertex_count+=4;
    }
    ShadeDioramaVertices(layer,mesh.vertices,mesh.vertex_count);
    DioramaAttachedMesh attached={0};
    outcome=PresentationOutcome_Combine(outcome,DrawDioramaLayerFace(device,capture,geometry,layer,&mesh,&attached,&source));
    if(!PresentationOutcome_IsUsable(outcome))break;
  }
  return outcome;
}

static PresentationOutcome DrawVoxelGuardBody(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *g,const DioramaLayerDraw *layer,bool relief) {
  if(!layer->voxel_guard)return kPresentationOutcome_Complete;
  const DioramaTrunkGuardStrip *strip=&layer->voxel_guard->layout.lower;
  const DioramaTrunkGuardBounds *valid=&strip->valid;
  if(valid->width<=0||valid->height<=0)return kPresentationOutcome_Complete;
  DioramaLayerDraw bounded=*layer;
  bounded.desert_columns=NULL;bounded.desert_columns_only=false;
  bounded.voxel_source_rect=(ArRenderRectF){valid->x,
    layer->voxel_guard->layout.primary_height+valid->y,valid->width,valid->height};
  const bool desert=relief&&ActionVoxel_Profile(layer->voxel_geometry)==kActionVoxelProfile_KassandoraDesert&&
      ActionVoxel_SceneryRecess(layer->voxel_geometry)==0&&
      !ActionVoxel_HasHiddenNativeFronts(layer->voxel_geometry);
  if(relief&&!desert)return DrawVoxelPartitionedFace(device,capture,g,&bounded,kVoxelFaceRelief);
  DioramaLayerMesh mesh={.constrained=true,.has_depth=layer->depth_sink!=NULL};
  if(layer->description->plane==SR_PPU_OVERLAY_BG1&&ActionVoxel_HasRearRelief(layer->voxel_geometry))
    PrepareVoxelRearMesh(capture,g,&bounded,&mesh);
  else {
    const ArRenderRectF r=bounded.voxel_source_rect;
    const float scale=layer->description->plane==SR_PPU_OVERLAY_BG1?
      VoxelPlaneScale(g,layer->voxel_geometry,layer->description->plane,layer->authored->z):1;
    const unsigned rows=(unsigned)ceilf(r.h/16.f);
    for(unsigned row=0;row<=rows;++row)for(unsigned col=0;col<=DIORAMA_SUBDIV_X;++col) {
      const float u=(r.x+r.w*col/DIORAMA_SUBDIV_X)/SR_PPU_SURFACE_MAX_WIDTH;
      const float v=(r.y+r.h*row/rows)/SR_PPU_SURFACE_MAX_HEIGHT;
      const float s=(u-g->u0)/(g->u1-g->u0),t=(v-g->v0)/(g->v1-g->v0);
      const float z=DioramaTiltedRowDepth(layer->authored->z-.5f,layer->authored->rake,layer->authored->bow,t);
      const int at=mesh.vertex_count++;
      if(!ProjectWorldPointDepth(g->matrix,(s-.5f)*g->aspect_x*scale,(.5f-t)*g->height_scale*scale,z,
          g->width,g->height,&mesh.vertices[at].position,mesh.has_depth?mesh.depths+at:NULL))
        return kPresentationOutcome_CoreFailure;
      mesh.vertices[at].tex_coord=(ArRenderPointF){u,v};mesh.vertices[at].color=layer->shade;
    }
    TriangulateGrid(DIORAMA_SUBDIV_X,(int)rows,mesh.indices,&mesh.index_count);
    ShadeDioramaVertices(layer,mesh.vertices,mesh.vertex_count);
    ObserveVoxelLayerMesh(capture,g,layer,&mesh);
  }
  if(!mesh.vertex_count)return kPresentationOutcome_Complete;
  DioramaAttachedMesh attached={0};DioramaDesertColumnMask mask;
  bounded.desert_columns=NULL;bounded.desert_columns_only=false;
  if(desert) {
    if(!BuildDesertColumnMask(capture,g,&bounded,&mask))
      return DrawVoxelPartitionedFace(device,capture,g,&bounded,kVoxelFaceRelief);
    if(mask.count)bounded.desert_columns=&mask;
  }
  PresentationOutcome outcome=DrawDioramaLayerFace(device,capture,g,&bounded,&mesh,&attached,NULL);
  if(desert&&mask.count&&PresentationOutcome_IsUsable(outcome)) {
    bounded.desert_columns=NULL;bounded.desert_columns_only=true;
    outcome=PresentationOutcome_Combine(outcome,
        DrawVoxelPartitionedFace(device,capture,g,&bounded,kVoxelFaceRelief));
  }
  return outcome;
}

/* The perspective viewport can see beyond the ordinary scanout window.
 * Draw captured OBJ guards as well as verified BG guards, scaling both UVs
 * and world width so every existing pixel retains its position and size. */
static void DioramaExpandCaptureApron(DioramaCapture *capture,
                                     DioramaViewGeometry *geometry, int plane) {
  const int width = capture->width + 2 * capture->obj_apron;
  if (geometry) {
    geometry->aspect_x *= (float)width / capture->width;
    geometry->u0 = 0.0f;
    geometry->u1 = (float)width / SR_PPU_SURFACE_MAX_WIDTH;
  }
  capture->width = width;
  capture->obj_apron = 0;
  /* OBJ coverage describes its complete capture. BG masks still describe
   * the ordinary window, so cannot cull a wider guard mesh. */
  if (!DioramaPlaneIsObjectPriority(plane)) capture->coverage_masks = NULL;
}

static int VoxelGuardEffectRows(const DioramaCapture *capture,const DioramaScene *scene) {
  if(!ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number)||
      ActionVoxel_Depth(scene->voxel_geometry)<=0||
      ActRaiserRoom_ProfileFor(scene->map_group,scene->map_number)!=kActRaiserRoomProfile_AitosWaterfall)
    return 0;
  DioramaCapture expanded=*capture;
  if(DioramaUsesCaptureApron(capture,SR_PPU_OVERLAY_BG1))
    DioramaExpandCaptureApron(&expanded,NULL,SR_PPU_OVERLAY_BG1);
  if(!VoxelGuardMatches(&expanded,scene->voxel_geometry,scene->voxel_guard))return 0;
  const DioramaTrunkGuardBounds *valid=&scene->voxel_guard->layout.lower.valid;
  /* One rectangle cannot describe a full-width primary image followed by a
   * narrower or disconnected strip. Retain the old finite clip in that case. */
  return valid->x==0&&valid->y==0&&valid->width==expanded.width ? valid->height : 0;
}

typedef enum VoxelDrawPhase {
  kVoxelDrawNormal, kVoxelDrawRearWater
} VoxelDrawPhase;
/* The contact split keeps the existing surface sampling. Both halves
 * belong in the low BG1 slot, regardless of artwork or physical depth.
 * Submit the near half after the native face to retain the rim's coverage. */
static bool DrawVoxelLowContact(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *geometry,const DioramaLayerDraw *layer) {
  if(layer->description->plane!=kDioramaPlane_Bg1Far || layer->voxel_depth<=0 ||
      layer->voxel_recess<=0 || geometry->options->voxel_inspection==kDioramaVoxelInspect_Backing)
    return true;
  const float split=fmaxf(0,layer->voxel_recess-.01f);
  return DrawVoxelAuthored(device,capture,geometry,layer,true) &&
      DrawVoxelEdges(device,capture,geometry,layer,0,split);
}
static PresentationOutcome DrawResolvedDioramaLayer(
    ArRenderDevice *device, const DioramaCapture *capture,
    const DioramaScene *scene, const DioramaViewGeometry *geometry,
    const DioramaFocalAperture *aperture, const ArRenderTexture *textures,
    ArRenderTexture priority_surface,bool guard_priority,const DioramaResolvedLayer *resolved,
    ArRenderOutputFrame *output_frame,
    DioramaProjection *out_projection,VoxelDrawPhase voxel_phase,bool terminal_subscreen) {
  if (!resolved->alpha)
    return kPresentationOutcome_Complete;
  const DioramaLayerDesc *description = DioramaDescForPlane(resolved->plane);
  if (!description)
    return kPresentationOutcome_Complete;
  const DioramaVoxelInspection inspection=ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number)
      ? scene->render->voxel_inspection : kDioramaVoxelInspect_All;
  if(inspection==kDioramaVoxelInspect_Backing && DioramaPlaneIsObjectPriority(description->plane))
    return kPresentationOutcome_Complete;
  /* Rooted forest effects sit over the completed walking surface and before
   * actors. Submitting them with the recessed backdrop would let the front
   * grass face erase the plants; their projection remains the path surface. */
  const bool forest=ActionVoxel_Profile(scene->voxel_geometry)==kActionVoxelProfile_FillmoreForest;
  const bool front_effects=forest || ActionVoxel_Profile(scene->voxel_geometry)==kActionVoxelProfile_BloodpoolMarsh ||
      ActionVoxel_Profile(scene->voxel_geometry)==kActionVoxelProfile_KassandoraDesert;
  const int effect_plane=front_effects && description->plane==SR_PPU_OVERLAY_BG1 ? -1
      : front_effects && description->plane==kDioramaPlane_Bg1Far ? SR_PPU_OVERLAY_BG1
      : description->plane;
  DioramaPerformance_SetPlane(description->plane);
  if (!DioramaLayerIsDrawable(description, textures, capture->pixels, scene)) {
    if(voxel_phase==kVoxelDrawRearWater)return kPresentationOutcome_Complete;
    if (effect_plane<0) return kPresentationOutcome_Complete;
    if (!DioramaLayerIsProjectable(description, textures, capture->pixels, scene))
      return kPresentationOutcome_Complete;
    return DioramaSubmitPlaneEffect(output_frame, scene->plane_effect,
                                    scene->plane_effect_userdata,
                                    effect_plane, out_projection, scene->render->source_effect_batch);
  }

  const float shade_mix =
      geometry->options->depth_shade;
  const bool scenery = description->plane == SR_PPU_OVERLAY_BG1 ||
      description->plane == kDioramaPlane_Bg1Hi || description->plane == kDioramaPlane_Bg1Far;
  const bool cloud_background=DioramaCloudLight_Active(&scene->cloud_light)&&
      (description->plane==SR_PPU_OVERLAY_BG2||description->plane==kDioramaPlane_Bg2Hi||description->plane==kDioramaPlane_Bg2Far);
  const int priority=DioramaPlaneObjectPriority(description->plane);
  const bool actor=priority>=0 && (scene->actor_light || scene->native_bloom.set) &&
      capture->pixels[kDioramaPlane_ObjAttributes0+priority] &&
      ArRenderTexture_IsValid(textures[kDioramaPlane_ObjAttributes0+priority]);
  const float dimming = scenery ? fminf(1, fmaxf(0, scene->bg1_dimming)) : 0;
  const bool additive =
      (scene->additive_plane_mask & (1u << (unsigned)description->plane)) != 0;
  DioramaLayerDraw layer = {
      .depth_sink = scenery || priority>=0 ? geometry->depth_sink : NULL,
      .authored = resolved,
      .description = description,
      .voxel_source_plane = -1,
      .texture = textures[description->plane],
      .voxel_rear_texture = textures[SR_PPU_OVERLAY_BG1],
      .priority_surface = (description->plane ==
                           (ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number)
                               ?kDioramaPlane_Bg1Far:SR_PPU_OVERLAY_BG1) ||
                           description->plane == kDioramaPlane_Bg1Hi)
          ? priority_surface : ArRenderTexture_Invalid(),
      .shade =
          {
              1.0f + (description->shade.r - 1.0f) * shade_mix,
              1.0f + (description->shade.g - 1.0f) * shade_mix,
              1.0f + (description->shade.b - 1.0f) * shade_mix,
              description->shade.a * ((float)resolved->alpha / 255.0f),
          },
      .dimming = dimming,
      .hollow_recess = HollowPlaySpace(scene)?TrunkRecess():0,
      .voxel_geometry = scene->voxel_geometry,
      .voxel_depth = ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number)?ActionVoxel_Depth(scene->voxel_geometry):0,
      .voxel_recess = ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number)?ActionVoxel_PlayRecess(scene->voxel_geometry):0,
      .dimming_ramp = scene->bg1_dimming_ramp,
      .exposure = scenery ? scene->scenery_exposure : NULL,
      .lighting = {.illumination = cloud_background ? UnlitSceneryMap(device) : scenery ? scene->scenery_lightmap : actor ? scene->actor_lightmaps[0] : ArRenderTexture_Invalid(),
          .cloud_light = cloud_background ? scene->cloud_light : (DioramaCloudLight){0},
          .heat = scenery && DioramaHeat_Active(&scene->scenery_heat) ? scene->scenery_heat : (DioramaHeat){0},
          .grade = scenery ? scene->scenery_grade : (DioramaSceneryGrade){0},
          .attributes = actor ? textures[kDioramaPlane_ObjAttributes0+priority] : ArRenderTexture_Invalid(),
          .enemy_illumination = scene->actor_lightmaps[1],
          .shade = {1 + (description->shade.r - 1) * shade_mix,
              1 + (description->shade.g - 1) * shade_mix,
              1 + (description->shade.b - 1) * shade_mix, 1}},
      .light_viewport = {(float)output_frame->viewport.x/output_frame->output_width,
          (float)output_frame->viewport.y/output_frame->output_height,
          (float)output_frame->viewport.w/output_frame->output_width,
          (float)output_frame->viewport.h/output_frame->output_height},
      .blend = description->plane == kDioramaPlane_Backdrop
                   ? kArRenderBlendMode_Opaque
               : additive ? kArRenderBlendMode_Add
                          : kArRenderBlendMode_Alpha,
  };
  if (description->plane==SR_PPU_OVERLAY_BG2 && scene->sky_weather &&
      out_projection && out_projection->bg2_plane.valid) {
    PresentationOutcome source_outcome=kPresentationOutcome_Complete;
    layer.texture=CompositePlaneWeather(device,layer.texture,scene,out_projection,&source_outcome);
    if (!PresentationOutcome_IsUsable(source_outcome)) return source_outcome;
  }
  DioramaLayerMesh mesh;
  DioramaDeathHeimNebulaWindow nebula;
  if (DeathHeimNebulaWindow(capture,scene,geometry,resolved,&nebula)) {
    if (voxel_phase!=kVoxelDrawNormal) return kPresentationOutcome_Complete;
    PrepareDeathHeimNebulaMesh(geometry,&layer,&nebula,&mesh);
    DioramaAttachedMesh attached={0};
    const PresentationOutcome face=DrawDioramaLayerFace(device,capture,geometry,
        &layer,&mesh,&attached,NULL);
    if (!PresentationOutcome_IsUsable(face)) return face;
    return PresentationOutcome_Combine(face,DioramaSubmitPlaneEffect(output_frame,
        scene->plane_effect,scene->plane_effect_userdata,effect_plane,out_projection,
        scene->render->source_effect_batch));
  }
  DioramaViewGeometry layer_geometry = *geometry;
  DioramaCapture layer_capture;
  if (DioramaUsesCaptureApron(capture, description->plane)) {
    layer_capture = *capture;
    DioramaExpandCaptureApron(&layer_capture, &layer_geometry, description->plane);
    capture = &layer_capture;
    geometry = &layer_geometry;
  }
  DioramaVoxelGuardFilter guard_filter={0};
  DioramaCapture voxel_capture=*capture;
  if(layer.voxel_depth>0&&scenery&&VoxelGuardMatches(capture,layer.voxel_geometry,scene->voxel_guard)&&
      scene->voxel_guard->layout.lower.valid.width>0&&scene->voxel_guard->layout.lower.valid.height>0) {
    layer.voxel_guard=scene->voxel_guard;layer.voxel_guard_filter=&guard_filter;
    layer.voxel_guard_priority=guard_priority;
    voxel_capture.height=scene->voxel_guard->layout.primary_height+
        scene->voxel_guard->layout.lower.valid.y+scene->voxel_guard->layout.lower.valid.height;
  }
  /* A stale optional descriptor may have admitted an otherwise empty plane
   * to the draw order. It must not become an invalid ordinary texture draw. */
  if(scene->voxel_guard&&scenery&&!layer.voxel_guard&&!ArRenderTexture_IsValid(layer.texture))
    return kPresentationOutcome_Complete;
  const bool guarded_trunk=layer.hollow_recess>0&&scene->trunk_guard&&scenery;
  if ((layer.voxel_depth > 0 || guarded_trunk) && scenery && capture->plane_capture_offsets &&
      (capture->voxel_map_plane_mask & (1u << description->plane))) {
    /* Native map texels were rebased to the current capture without filtering.
     * Move their geometry continuously before projection, keeping the UVs and
     * every relief/cap join fixed in the same native world. The depth shear is
     * the existing 1 + depth/distance compensation expressed as one transform. */
    const ArRenderPointF offset = capture->plane_capture_offsets[description->plane];
    const float du = geometry->u1 - geometry->u0, dv = geometry->v1 - geometry->v0;
    if (!isfinite(offset.x) || !isfinite(offset.y) || du <= 0 || dv <= 0)
      return kPresentationOutcome_CoreFailure;
    layer_geometry = *geometry;
    const float tx = offset.x * geometry->aspect_x / (du * SR_PPU_SURFACE_MAX_WIDTH);
    const float ty = -offset.y * geometry->height_scale / (dv * SR_PPU_SURFACE_MAX_HEIGHT);
    const float inverse_distance = 1 / fmaxf(1, geometry->camera.distance);
    for (unsigned row = 0; row < 4; ++row) {
      const float shift = tx * layer_geometry.matrix[row] + ty * layer_geometry.matrix[4 + row];
      layer_geometry.matrix[12 + row] += shift;
      if(!guarded_trunk)layer_geometry.matrix[8 + row] -= shift * inverse_distance;
    }
    geometry = &layer_geometry;
  }
  if (description->plane == SR_PPU_OVERLAY_BG2 ||
      description->plane == kDioramaPlane_Bg2Hi ||
      description->plane == kDioramaPlane_Bg2Far) {
    layer_geometry = *geometry;
    Diorama_TranslateCameraWorldY(
        layer_geometry.matrix, geometry->bg2_world_y_offset);
    geometry = &layer_geometry;
  }
  if(voxel_phase==kVoxelDrawRearWater)
    return DrawVoxelPartitionedFace(device,capture,geometry,&layer,kVoxelFaceWaterRear);
  if(HollowPlaySpace(scene) &&
      description->plane==kDioramaPlane_Bg1Far) {
    /* Repeat a native 256-row wall segment beyond the finite capture. Each
     * patch shares identical boundary positions; only its texture UV resets.
     * The surface is clipped to real room bounds, never the moving capture. */
    const float origin_y=capture->camera_y-capture->authentic_y0+1;
    const float room_height=scene->trunk->height;
    const float first=fmaxf(-256,-origin_y);
    const float last=fminf(capture->height+256,room_height-origin_y);
    PresentationOutcome result=kPresentationOutcome_Complete;
    DioramaFaceSource source={0};
    for(int offset=-256;offset<last;offset+=256) {
      const float a=fmaxf(first,offset),b=fminf(last,offset+256);
      if(b<=a)continue;
      PrepareHollowMesh(capture,geometry,&layer,a,b,(float)offset,&mesh);
      if(!mesh.vertex_count||!mesh.index_count)continue;
      DioramaAttachedMesh empty={0};
      const PresentationOutcome face=DrawDioramaLayerFace(device,capture,geometry,&layer,&mesh,&empty,&source);
      result=PresentationOutcome_Combine(result,face);
      if(!PresentationOutcome_IsUsable(face))return result;
    }
    if(scene->trunk->side_width) {
      const PresentationOutcome sides=DrawHollowSideFaces(device,capture,geometry,&layer,true,false);
      result=PresentationOutcome_Combine(result,sides);
      if(!PresentationOutcome_IsUsable(sides))return result;
    }
    const PresentationOutcome guards=DrawHollowGuard(device,capture,geometry,&layer,scene->trunk_guard,scene->map_number);
    result=PresentationOutcome_Combine(result,guards);
    if(!PresentationOutcome_IsUsable(guards))return result;
    return PresentationOutcome_Combine(result,DioramaSubmitPlaneEffect(
        output_frame,scene->plane_effect,scene->plane_effect_userdata,
        description->plane,out_projection,scene->render->source_effect_batch));
  }
  if(layer.hollow_recess>0 &&
      (description->plane==SR_PPU_OVERLAY_BG1||description->plane==kDioramaPlane_Bg1Hi) &&
      !DrawHollowSolidEdges(device,capture,geometry,&layer,scene->map_number,
          description->plane==SR_PPU_OVERLAY_BG1?1:0,NULL,0))return kPresentationOutcome_CoreFailure;
  if(layer.hollow_recess>0 &&
      (description->plane==SR_PPU_OVERLAY_BG1||description->plane==kDioramaPlane_Bg1Hi) &&
      !DrawHollowPassageEdges(device,capture,geometry,&layer,scene->map_number,
          description->plane==kDioramaPlane_Bg1Hi,NULL,0))return kPresentationOutcome_CoreFailure;
  if(guarded_trunk&&(description->plane==SR_PPU_OVERLAY_BG1||description->plane==kDioramaPlane_Bg1Hi)) {
    const PresentationOutcome guards=DrawHollowGuard(device,capture,geometry,&layer,scene->trunk_guard,scene->map_number);
    if(!PresentationOutcome_IsUsable(guards))return guards;
  }
  if(HollowPlaySpace(scene)&&scene->trunk->side_high_pixels&&description->plane==kDioramaPlane_Bg1Hi) {
    /* Native high side details keep their actor-plane depth and high slot;
     * only the existing shaft rim belongs on the forward plane at zero. */
    const PresentationOutcome faces=DrawHollowSideFaces(device,capture,geometry,&layer,false,true);
    if(!PresentationOutcome_IsUsable(faces))return faces;
    return PresentationOutcome_Combine(faces,DioramaSubmitPlaneEffect(output_frame,
        scene->plane_effect,scene->plane_effect_userdata,description->plane,
        out_projection,scene->render->source_effect_batch));
  }
  if(layer.voxel_depth>0&&description->plane==kDioramaPlane_Bg1Far){
    if(inspection!=kDioramaVoxelInspect_Backing&&!DrawVoxelRearTrims(device,&voxel_capture,geometry,&layer,textures[kDioramaPlane_Bg1Hi]))
      return kPresentationOutcome_CoreFailure;
    if(inspection!=kDioramaVoxelInspect_Backing&&!DrawVoxelHighDecks(device,&voxel_capture,geometry,&layer,textures[kDioramaPlane_Bg1Hi]))
      return kPresentationOutcome_CoreFailure;
    const float split=fmaxf(0,layer.voxel_recess-.01f);
    if(!DrawVoxelAuthored(device,&voxel_capture,geometry,&layer,false))
      return kPresentationOutcome_CoreFailure;
    if(inspection!=kDioramaVoxelInspect_Backing&&
        !DrawVoxelEdges(device,&voxel_capture,geometry,&layer,split,layer.voxel_depth))
      return kPresentationOutcome_CoreFailure;
  }
  if(inspection!=kDioramaVoxelInspect_Backing&&layer.voxel_depth>0&&description->plane==kDioramaPlane_Bg1Hi&&
      !DrawVoxelEdges(device,&voxel_capture,geometry,&layer,0,layer.voxel_depth))
    return kPresentationOutcome_CoreFailure;
  if(layer.voxel_depth>0&&description->plane==kDioramaPlane_Bg1Hi&&
      !DrawVoxelAuthored(device,&voxel_capture,geometry,&layer,false))return kPresentationOutcome_CoreFailure;
  /* Inspection removes the forward artwork, not its native priority slots.
   * Cutaway keeps contact caps; backing isolation retains only the rear
   * surface (including authored repairs), with no invented replacement art. */
  if(inspection!=kDioramaVoxelInspect_All &&
      (description->plane==kDioramaPlane_Bg1Far || description->plane==kDioramaPlane_Bg1Hi))
    return DrawVoxelLowContact(device,&voxel_capture,geometry,&layer)
        ?kPresentationOutcome_Complete:kPresentationOutcome_CoreFailure;
  PrepareDioramaLayerMesh(capture, geometry, aperture, &layer, &mesh);
  if((description->plane==SR_PPU_OVERLAY_BG2||description->plane==kDioramaPlane_Bg2Hi||description->plane==kDioramaPlane_Bg2Far)&&
      DioramaHeat_Active(&scene->background_heat))
    for(int i=0;i<mesh.vertex_count;++i)mesh.vertices[i].tex_coord=DioramaHeat_Uv(&scene->background_heat,mesh.vertices[i].tex_coord);
  DioramaAttachedMesh attached;
  PrepareDioramaWaterfall(capture, scene, geometry, &layer, &attached,
                          out_projection);
  PrepareDeathHeimWaterExtension(capture, scene, geometry, &layer, &attached);
  PresentationOutcome outcome =
      DrawDioramaLayerDepth(device, capture, geometry, aperture, &layer);
  if (!PresentationOutcome_IsUsable(outcome))
    return outcome;
  unsigned waterfall_count=0;
  if(layer.voxel_depth>0 && description->plane==kDioramaPlane_Bg2Hi)
    ActionVoxel_Waterfalls(layer.voxel_geometry,&waterfall_count);
  bool gate_front=false;
  if(layer.voxel_depth>0) {
    unsigned count;const VoxelGate *gates=ActionVoxel_Gates(layer.voxel_geometry,&count);
    for(unsigned i=0;i<count;++i)
      gate_front |= gates[i].high_priority==(description->plane==kDioramaPlane_Bg1Hi);
  }
  const bool relief_front=layer.voxel_depth>0 &&
      (ActionVoxel_SceneryRecess(layer.voxel_geometry)>0 || ActionVoxel_HasColumns(layer.voxel_geometry) || gate_front ||
       ActionVoxel_HasHiddenNativeFronts(layer.voxel_geometry)) &&
      (description->plane==kDioramaPlane_Bg1Far || description->plane==kDioramaPlane_Bg1Hi);
  /* Grounded Desert columns and mountain ramp tapers replace only their
   * own native front. Preserve the ordinary capture grid, diagonal, shade
   * and sampling everywhere else. */
  bool desert_front=relief_front&&!waterfall_count&&!gate_front&&
      (ActionVoxel_Profile(layer.voxel_geometry)==kActionVoxelProfile_KassandoraDesert ||
       ActionVoxel_Profile(layer.voxel_geometry)==kActionVoxelProfile_AitosMountains)&&
      ActionVoxel_SceneryRecess(layer.voxel_geometry)==0&&
      !ActionVoxel_HasHiddenNativeFronts(layer.voxel_geometry);
  DioramaDesertColumnMask column_mask;
  if(desert_front) {
    desert_front=BuildDesertColumnMask(capture,geometry,&layer,&column_mask);
    if(desert_front&&column_mask.count)layer.desert_columns=&column_mask;
  }
  PresentationOutcome face = desert_front
      ? DrawDioramaLayerFace(device,capture,geometry,&layer,&mesh,&attached,NULL)
      : waterfall_count || relief_front
      ? DrawVoxelPartitionedFace(device,capture,geometry,&layer,
          waterfall_count?kVoxelFaceWaterNative:kVoxelFaceRelief)
      : DrawDioramaLayerFace(device,capture,geometry,&layer,&mesh,&attached,NULL);
  if(desert_front&&column_mask.count&&PresentationOutcome_IsUsable(face)) {
    layer.desert_columns=NULL;layer.desert_columns_only=true;
    face=PresentationOutcome_Combine(face,
        DrawVoxelPartitionedFace(device,capture,geometry,&layer,kVoxelFaceRelief));
  }
  outcome = PresentationOutcome_Combine(outcome, face);
  if (!PresentationOutcome_IsUsable(face))
    return outcome;
  if(layer.voxel_guard) {
    const PresentationOutcome extension=DrawVoxelGuardBody(device,capture,geometry,&layer,relief_front);
    outcome=PresentationOutcome_Combine(outcome,extension);
    if(!PresentationOutcome_IsUsable(extension))return extension;
  }
  if(!DrawCastleTerminalMatte(device,capture,scene,geometry,&layer))return kPresentationOutcome_CoreFailure;
  if(!DrawJungleTerminal(device,capture,scene,geometry,&layer,terminal_subscreen))return kPresentationOutcome_CoreFailure;
  if(layer.voxel_depth>0 && description->plane==SR_PPU_OVERLAY_BG1 &&
      !DrawVoxelNativeTransformedBacking(device,&voxel_capture,geometry,&layer))return kPresentationOutcome_CoreFailure;
  if(inspection!=kDioramaVoxelInspect_Backing&&layer.voxel_depth>0 && forest && description->plane==SR_PPU_OVERLAY_BG1 &&
      !DrawVoxelEdges(device,&voxel_capture,geometry,&layer,0,layer.voxel_depth))
    return kPresentationOutcome_CoreFailure;
  if(layer.voxel_depth>0 && (description->plane==SR_PPU_OVERLAY_BG2 ||
      description->plane==kDioramaPlane_Bg2Hi || description->plane==kDioramaPlane_Bg1Far ||
      description->plane==kDioramaPlane_Bg1Hi) && !DrawVoxelWaterSurfaces(device,capture,geometry,&layer))
    return kPresentationOutcome_CoreFailure;
  if(layer.hollow_recess>0 && !DrawHollowLife(device,capture,geometry,&layer,1-scene->native_bloom_dimming))return kPresentationOutcome_CoreFailure;
  if(!DrawVoxelLowContact(device,&voxel_capture,geometry,&layer))return kPresentationOutcome_CoreFailure;
  if(effect_plane<0)return outcome;
  const PresentationOutcome effect = DioramaSubmitPlaneEffect(
      output_frame, scene->plane_effect, scene->plane_effect_userdata,
      effect_plane, out_projection, scene->render->source_effect_batch);
  return PresentationOutcome_Combine(outcome, effect);
}

static bool PrepareSceneryLightmap(ArRenderDevice *device, DioramaScene *scene,
    const DioramaProjection *projection) {
  if (ArRenderTexture_IsValid(scene->scenery_lightmap)) return true;
  if (scene->scenery_light && projection &&
      ArRenderCapabilities_Has(ArRenderDevice_Capabilities(device),
          kArRenderCapability_CustomShaders | kArRenderCapability_ScopedRenderTargets)) {
    int width, height;
    if (!ArRenderDevice_GetOutputSize(device, &width, &height))
      return false;
    const ArRenderTexture lightmap = EnsureDioramaScratchTarget(
        device, &s_diorama_lightmap, width, height, kArRenderFilter_Linear);
    ArRenderTargetState saved;
    if (!ArRenderTexture_IsValid(lightmap) ||
        ArRenderDevice_BeginTarget(device, lightmap, &saved) != kArRenderTargetBegin_Ready)
      return false;
    const DioramaSourceEffectBatch batch = scene->render->source_effect_batch;
    bool drawn = ArRenderDevice_Clear(device, (ArRenderColorF){0,0,0,1}) &&
        (!batch || batch(device, true));
    if (drawn) {
      drawn = scene->scenery_light(scene->plane_effect_userdata, projection);
      if (batch) drawn = batch(device, false) && drawn;
    }
    const bool restored = ArRenderDevice_EndTarget(device, &saved);
    if (!drawn || !restored) return false;
    scene->scenery_lightmap = lightmap;
  }
  return true;
}

/* The original projected backgrounds are drawn once. Filtering only their
 * small radiance map preserves native texels, raster animation and depth.
 * Foreground priority bands, actors and HUD retain their normal draw slots. */
static bool NativeBloomBackgroundPlane(int plane) {
  return plane == kDioramaPlane_Backdrop || LayerGetsEdgeAA(plane);
}

static bool NativeBloomDraw(ArRenderDevice *device, ArRenderTexture texture,
    const ArRenderRectF *source, const ArRenderRectF *destination,
    float strength, ArRenderBlendMode blend) {
  const ArRenderDrawState state = {.flags=kArRenderDrawState_Blend|kArRenderDrawState_Tint,
      .blend=blend, .tint={strength,strength,strength,1}};
  return ArRenderDevice_DrawTextureWithState(device,texture,source,destination,&state);
}

static bool PrepareNativeBloom(ArRenderDevice *device, const DioramaCapture *capture,
    DioramaScene *scene, DioramaViewGeometry *geometry,
    ArRenderTexture *textures, const DioramaResolvedLayer *resolved, int resolved_count,
    ArRenderOutputFrame *output, DioramaProjection *projection,
    uint32_t *consumed, ArRenderTexture *radiance) {
  *consumed=0; *radiance=ArRenderTexture_Invalid();
  if (!scene->native_bloom.set ||
      !ArRenderCapabilities_Has(ArRenderDevice_Capabilities(device),
          kArRenderCapability_CustomShaders|kArRenderCapability_ScopedRenderTargets) ||
      !DioramaEffectBackend_IsAvailable(device,kDioramaEffect_NativeBloom)) return false;
  const DioramaNativeBloom *style=&scene->native_bloom;
  const int w=output->output_width,h=output->output_height;
  const float reduction=fminf(1,fminf(640.0f/w,360.0f/h));
  const int bw=(int)ceilf(w*reduction),bh=(int)ceilf(h*reduction);
  const ArRenderTexture art=EnsureDioramaScratchTarget(device,&s_diorama_bloom[0],w,h,kArRenderFilter_Linear);
  const ArRenderTexture horizontal=EnsureDioramaScratchTarget(device,&s_diorama_bloom[1],bw,bh,kArRenderFilter_Linear);
  const ArRenderTexture glow=EnsureDioramaScratchTarget(device,&s_diorama_bloom[2],bw,bh,kArRenderFilter_Linear);
  if (!ArRenderTexture_IsValid(art)||!ArRenderTexture_IsValid(horizontal)||!ArRenderTexture_IsValid(glow)) return false;
  ArRenderTargetState saved;
  if (ArRenderDevice_BeginTarget(device,art,&saved)!=kArRenderTargetBegin_Ready) return false;
  DioramaScene background=*scene;
  background.plane_effect=NULL; background.scenery_light=NULL; background.actor_light=NULL;
  background.native_bloom=(DioramaNativeBloom){0};
  ArRenderOutputFrame frame;
  bool ok=ArRenderOutputFrame_Begin(device,output->viewport,
      (ArRenderColorF){0,0,0,1},(ArRenderColorF){0,0,0,1},&frame);
  if (ok) ok=PresentationOutcome_IsUsable(DrawResolvedDioramaSkybox(device,capture,
      &background,geometry,textures,resolved,resolved_count,projection,false));
  /* Build authored illumination after the sky publishes its UV mapping,
   * before extracting radiance. User-added lights and dimming still work. */
  if (ok) ok=PrepareSceneryLightmap(device,scene,projection);
  background.scenery_lightmap=scene->scenery_lightmap;
  DioramaFocalAperture aperture;
  DioramaCapture focal_capture=*capture;
  DioramaViewGeometry focal_geometry=*geometry;
  if (DioramaUsesCaptureApron(capture,SR_PPU_OVERLAY_BG1))
    DioramaExpandCaptureApron(&focal_capture,&focal_geometry,SR_PPU_OVERLAY_BG1);
  PrepareDioramaAperture(&focal_capture,&background,&focal_geometry,textures,resolved,resolved_count,&aperture);
  PresentationOutcome priority_outcome;bool guard_priority=false;
  const ArRenderTexture priority=BuildDioramaPrioritySurface(device,&focal_capture,&background,
      textures,resolved,resolved_count,&priority_outcome,&guard_priority);
  ok=ok && PresentationOutcome_IsUsable(priority_outcome);
  int order[kDioramaLayerCount];
  const int count=ResolveDioramaDrawOrder(&background,resolved,resolved_count,order);
  uint32_t mask=0;
  for (int i=0;ok && i<count;++i) {
    const DioramaResolvedLayer *layer=&resolved[order[i]];
    const DioramaLayerDesc *description=DioramaDescForPlane(layer->plane);
    if (!description) continue;
    if (!NativeBloomBackgroundPlane(layer->plane)) {
      if (DioramaLayerIsDrawable(description,textures,capture->pixels,&background)) break;
      continue;
    }
    ok=PresentationOutcome_IsUsable(DrawResolvedDioramaLayer(device,capture,&background,
        geometry,&aperture,textures,priority,guard_priority,layer,&frame,projection,kVoxelDrawNormal,false));
    mask|=1u<<layer->plane;
  }
  if(ok)ok=DioramaAstralRift_DrawDust(device,scene,capture,projection);
  const bool restored=ArRenderDevice_EndTarget(device,&saved);
  if (!ok||!restored) return false;
  /* Radius follows native image pixels, not monitor pixels. Five bilinear
   * taps per axis approximate a nine-tap Gaussian, with no CPU readback. */
  const float radius=style->radius*geometry->height/capture->height/3.230769f;
  for (unsigned pass=0;pass<2;++pass) {
    if (ArRenderDevice_BeginTarget(device,pass?glow:horizontal,&saved)!=kArRenderTargetBegin_Ready) return false;
    const DioramaNativeBloomEffectParams params={
        .step_x=pass?0:radius/w,.step_y=pass?radius/h:0,
        .threshold=style->threshold,.mode=(float)pass,
        .saturation=style->saturation,.contrast=style->contrast};
    const ArRenderRectF dst={0,0,bw,bh};
    ok=DioramaEffectBackend_BindNativeBloom(device,&params) &&
        NativeBloomDraw(device,pass?horizontal:art,NULL,&dst,1,kArRenderBlendMode_Opaque);
    ok=DioramaEffectBackend_Unbind(device)&&ok;
    const bool ended=ArRenderDevice_EndTarget(device,&saved);
    if (!ok||!ended) return false;
  }
  if (!ArRenderOutputFrame_RestoreViewport(output)) return false;
  const ArRenderRectF source={output->viewport.x,output->viewport.y,geometry->width,geometry->height};
  const ArRenderRectF dest={0,0,geometry->width,geometry->height};
  const DioramaNativeBloomEffectParams grade={.mode=2,.saturation=style->saturation,.contrast=style->contrast};
  ok=DioramaEffectBackend_BindNativeBloom(device,&grade)&&
      NativeBloomDraw(device,art,&source,&dest,1,kArRenderBlendMode_Opaque);
  ok=DioramaEffectBackend_Unbind(device)&&ok;
  const ArRenderRectF glow_source={source.x*bw/w,source.y*bh/h,source.w*bw/w,source.h*bh/h};
  const DioramaNativeBloomEffectParams gain={.mode=3,.gain=style->strength};
  ok=ok&&DioramaEffectBackend_BindNativeBloom(device,&gain)&&
      NativeBloomDraw(device,glow,&glow_source,&dest,1,kArRenderBlendMode_Add);
  ok=DioramaEffectBackend_Unbind(device)&&ok;
  if (!ok) return false;
  *consumed=mask; *radiance=glow;
  return true;
}

/* A native full-add scene adds one subscreen winner to the main screen.
 * Voxel faces overlap within that subscreen: alpha-compose them first, then
 * add the completed result once. Adding every face separately makes opaque
 * masonry luminous and lets its own rear face show through. */
static bool CompositeVoxelSubscreen(ArRenderDevice *device,ArRenderTexture texture,
                                    const ArRenderOutputFrame *frame) {
  const ArRenderRectI p=frame->viewport;
  const float u0=(float)p.x/frame->output_width,v0=(float)p.y/frame->output_height;
  const float u1=(float)(p.x+p.w)/frame->output_width,v1=(float)(p.y+p.h)/frame->output_height;
  const ArRenderColorF white={1,1,1,1};
  const ArRenderVertex2D vertices[]={
    {{0,0},white,{u0,v0}},{{p.w,0},white,{u1,v0}},
    {{p.w,p.h},white,{u1,v1}},{{0,p.h},white,{u0,v1}}};
  const int32_t indices[]={0,1,2,0,2,3};
  return RenderDioramaGeometry(device,texture,vertices,4,indices,6,kArRenderBlendMode_AddPremultiplied);
}
static bool BeginVoxelSubscreen(ArRenderDevice *device,ArRenderOutputFrame *frame,
    ArRenderTexture *texture,ArRenderTargetState *state,bool clear,bool *active,
    const DioramaDepthSink *sink) {
  *texture=EnsureDioramaScratchTarget(device,&s_diorama_voxel_subscreen,
      frame->output_width,frame->output_height,kArRenderFilter_Nearest);
  if(!ArRenderTexture_IsValid(*texture) ||
      ArRenderDevice_BeginTarget(device,*texture,state)!=kArRenderTargetBegin_Ready)return false;
  *active=true;
  return (!clear || ArRenderDevice_Clear(device,(ArRenderColorF){0,0,0,0})) &&
      ArRenderOutputFrame_RestoreViewport(frame) &&
      (!clear || !sink || !sink->clear || sink->clear(sink->userdata,device));
}

static PresentationOutcome DioramaCompositeBody(ArRenderDevice *device,
                                      const DioramaCapture *capture,
                                      const DioramaView *view,
                                      const DioramaScene *scene,
                                      DioramaProjection *out_projection) {
  s_ground_cover.active_count=0;s_ground_cover.options=NULL;
  if (out_projection)
    memset(out_projection, 0, sizeof(*out_projection));
  if (!ArRenderDevice_IsReady(device) || !capture || !view || !scene ||
      !scene->render || !capture->pixels ||
      capture->width <= 0 || capture->width > (int)SR_PPU_SURFACE_MAX_WIDTH ||
      capture->height < kActRaiserAuthenticHeight ||
      capture->height > (int)SR_PPU_SURFACE_MAX_HEIGHT ||
      capture->obj_apron < 0 ||
      capture->obj_apron > ((int)SR_PPU_SURFACE_MAX_WIDTH - capture->width) / 2 ||
      capture->authentic_y0 < 0 ||
      capture->authentic_y0 > capture->height - kActRaiserAuthenticHeight ||
      view->viewport.w <= 0 || view->viewport.h <= 0 || view->visible_width <= 0 ||
      !isfinite(view->camera.tilt_x) || !isfinite(view->camera.tilt_y) ||
      !isfinite(view->camera.distance) || !isfinite(view->distance_offset) ||
      !isfinite(view->distance_scale) || view->distance_scale <= 0 ||
      !isfinite(view->camera_framing_weight) ||
      !isfinite(scene->render->depth_shade) ||
      scene->render->skybox < kDioramaSky_Off || scene->render->skybox >= kDioramaSky_Count)
    return kPresentationOutcome_CoreFailure;
  DioramaScene trunk_scene;
  DioramaRenderOptions trunk_render;
  if (ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number)) {
    trunk_scene=*scene;trunk_render=*scene->render;
    trunk_render.visible_planes |= (1u<<SR_PPU_OVERLAY_BG1)|(1u<<kDioramaPlane_Bg1Hi)|
        (1u<<kDioramaPlane_Bg1Far)|(1u<<SR_PPU_OVERLAY_BG2)|(1u<<kDioramaPlane_Bg2Hi)|(1u<<kDioramaPlane_Bg2Far);
    trunk_scene.render=&trunk_render;scene=&trunk_scene;
  }
  if(HollowPlaySpace(scene)) {
    trunk_scene=*scene;trunk_render=*scene->render;
    trunk_render.skybox=kDioramaSky_Only;trunk_render.shoebox=false;
    trunk_render.visible_planes |= (1u<<SR_PPU_OVERLAY_BG1)|(1u<<kDioramaPlane_Bg1Hi)|
        (1u<<kDioramaPlane_Bg1Far)|(1u<<SR_PPU_OVERLAY_BG2)|(1u<<kDioramaPlane_Bg2Hi)|(1u<<kDioramaPlane_Bg2Far);
    trunk_render.visible_planes &= ~(1u<<kDioramaPlane_Backdrop);
    trunk_scene.render=&trunk_render;scene=&trunk_scene;
  }
  s_trunk_assets=scene->trunk;s_trunk_settings=scene->trunk_settings;
  s_trunk_fade=1-scene->native_bloom_dimming;
  TrunkLifePrepare(scene->trunk_effects,scene->native_bloom_clock,
      scene->trunk_lighting,scene->trunk_particles);
  ArRenderTexture textures[kDioramaPlane_Count];
  for (int plane = 0; plane < kDioramaPlane_Count; ++plane)
    textures[plane] = capture->textures ? capture->textures[plane]
                                        : ArRenderTexture_Invalid();
  /* Uploaders retain allocations when a native band becomes empty. A guard
   * may still admit that plane, but it must not expose the previous frame's
   * primary pixels. Presence tokens remain authoritative for GPU-owned bands. */
  if(scene->voxel_guard&&ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number)) {
    const int planes[]={SR_PPU_OVERLAY_BG1,kDioramaPlane_Bg1Hi,kDioramaPlane_Bg1Far};
    for(unsigned i=0;i<3;++i)if(!capture->pixels[planes[i]])
      textures[planes[i]]=ArRenderTexture_Invalid();
  }
  /* Room-owned geometry can enter a high-priority band whose original
   * captured pixels are empty. Supply transparent source storage for that
   * band; its immutable owned artwork still draws in the ordinary slot.
   * A content-bearing upload failure is never replaced by this fallback. */
  DioramaCapture owned_capture;
  const uint8_t *owned_pixels[kDioramaPlane_Count];
  if(ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number) &&
      (scene->render->visible_planes&(1u<<kDioramaPlane_Bg1Hi)) &&
      !capture->pixels[kDioramaPlane_Bg1Hi] && VoxelOwnsHighArt(scene->voxel_geometry)) {
    if(!EnsureVoxelEmptySource(device))return kPresentationOutcome_CoreFailure;
    memcpy(owned_pixels,capture->pixels,sizeof(owned_pixels));
    owned_pixels[kDioramaPlane_Bg1Hi]=(const uint8_t *)s_voxel_empty_pixels;
    textures[kDioramaPlane_Bg1Hi]=s_voxel_empty_source;
    owned_capture=*capture;owned_capture.pixels=owned_pixels;owned_capture.textures=textures;capture=&owned_capture;
  }
  const ArRenderColorF black = {0.0f, 0.0f, 0.0f, 1.0f};
  const ArRenderColorF navy = {20.0f / 255.0f, 20.0f / 255.0f, 30.0f / 255.0f,
                               1.0f};
  const uint32_t subscreen_planes=ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number)
      ? scene->additive_plane_mask : 0;
  ArRenderOutputFrame output_frame;
  if (!ArRenderOutputFrame_Begin(device, view->viewport, black, subscreen_planes?black:navy,
                                 &output_frame))
    return kPresentationOutcome_CoreFailure;
  ArRenderTargetState subscreen_state={0};
  ArRenderTexture subscreen_texture=ArRenderTexture_Invalid();
  bool subscreen_active=false;
  bool subscreen_background=false;

  DioramaViewGeometry geometry = {.options = scene->render, .width = view->viewport.w,
                                  .height = view->viewport.h,
      .depth_sink=HollowPlaySpace(scene)||ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number)
          ?scene->depth_sink:NULL};
  if (geometry.depth_sink && geometry.depth_sink->clear &&
      !geometry.depth_sink->clear(geometry.depth_sink->userdata,device)) goto failed;
  DioramaPerformance_SetViewport(geometry.width, geometry.height);
  DioramaResolvedLayer resolved[kDioramaLayerCount];
  const int resolved_count = Diorama_ResolveSceneLayers(scene, resolved);
  PrepareDioramaView(capture, view, scene, resolved, resolved_count,
                     &geometry);
  PlaceDeathHeimWater(capture, scene, textures, resolved, resolved_count, &geometry);
  PublishDioramaView(capture, view, &geometry, out_projection);
  PublishDioramaPlanes(capture, scene, &geometry, textures, resolved,
                       resolved_count, out_projection);
  if(!GroundCoverPrepare(scene,out_projection))goto failed;
  DioramaProjection cover_projection;
  if(!out_projection&&s_ground_cover.active_count) {
    /* Even a caller that does not retain projection output needs per-bank
     * ownership during its effect callbacks, or the old flat top duplicates
     * the replacement. Other NULL-projection paths remain unchanged. */
    memset(&cover_projection,0,sizeof(cover_projection));out_projection=&cover_projection;
    PublishDioramaView(capture,view,&geometry,out_projection);
    PublishDioramaPlanes(capture,scene,&geometry,textures,resolved,resolved_count,out_projection);
    for(unsigned i=0;i<s_ground_cover.active_count;++i) {
      const unsigned bank=s_ground_cover.active[i];
      out_projection->voxel_ground_cover_claims[bank/64]|=UINT64_C(1)<<(bank%64);
    }
  }
  PresentationOutcome outcome = scene->backdrop
      ? scene->backdrop(scene->backdrop_userdata,
          (ArRenderRectI){0, 0, geometry.width, geometry.height})
      : kPresentationOutcome_Complete;
  /* Background passes can bind their own depth targets. Re-establish the
   * output frame before any native platform or actor geometry is submitted. */
  if (!PresentationOutcome_IsUsable(outcome) ||
      (scene->backdrop && !ArRenderOutputFrame_RestoreViewport(&output_frame)))
    goto failed;
  DioramaScene illuminated = *scene;
  illuminated.scenery_lightmap = ArRenderTexture_Invalid();
  uint32_t bloom_planes=0;
  ArRenderTexture bloom_light=ArRenderTexture_Invalid();
  const bool bloom=scene->render->voxel_inspection==kDioramaVoxelInspect_All && PrepareNativeBloom(device,capture,&illuminated,&geometry,textures,resolved,
      resolved_count,&output_frame,out_projection,&bloom_planes,&bloom_light);
  uint8_t sky_group,sky_room,sky_bg;
  /* A named native BG backdrop belongs to that screen too. Seed the
   * subscreen with it before lighting preparation, then keep it beneath the
   * opaque world faces. Drawing it on the main screen doubles its colour
   * wherever an angled subscreen extends beyond the main capture. */
  if(!bloom && subscreen_planes && scene->render->skybox!=kDioramaSky_Off &&
      DioramaLayerOrder_DecodeActionBgSource(DioramaLayerOrder_SkyboxSource(resolved,resolved_count),
          &sky_group,&sky_room,&sky_bg) && sky_bg>=1 && sky_bg<=2 &&
      (subscreen_planes & (1u<<(sky_bg-1)))) {
    if(!BeginVoxelSubscreen(device,&output_frame,&subscreen_texture,&subscreen_state,true,&subscreen_active,geometry.depth_sink))goto failed;
    subscreen_background=true;
  }
  const PresentationOutcome skybox = bloom ? kPresentationOutcome_Complete : DrawResolvedDioramaSkybox(
      device, capture, scene, &geometry, textures, resolved, resolved_count, out_projection,subscreen_background);
  outcome = PresentationOutcome_Combine(outcome, skybox);
  if (!PresentationOutcome_IsUsable(outcome))
    goto failed;
  if(subscreen_active) {
    subscreen_active=false;
    if(!ArRenderDevice_EndTarget(device,&subscreen_state))goto failed;
  }

  /* Skybox-anchored lights need the exact UV-band projection published by
   * the skybox draw, including aspect fit and raster scroll. Build their
   * material map now, before any foreground surfaces or volume callbacks.
   * PrepareSceneryLightmap reuses the map if native bloom already built it. */
  if (!PrepareSceneryLightmap(device,&illuminated,out_projection)) goto failed;
  /* Palette/heat-only rooms need no full-screen light-map pass. A retained black
   * texel lets the same material shade their existing geometry in one draw. */
  if (!ArRenderTexture_IsValid(illuminated.scenery_lightmap) &&
      (scene->scenery_grade.strength > 0 || DioramaHeat_Active(&scene->scenery_heat)) &&
      ArRenderCapabilities_Has(ArRenderDevice_Capabilities(device),kArRenderCapability_CustomShaders)) {
    illuminated.scenery_lightmap=UnlitSceneryMap(device);
    if (!ArRenderTexture_IsValid(illuminated.scenery_lightmap)) goto failed;
  }
  memset(illuminated.actor_lightmaps,0,sizeof(illuminated.actor_lightmaps));
  bool actor_attributes=false;
  for(unsigned p=0;p<4;++p)actor_attributes |= capture->pixels[kDioramaPlane_ObjAttributes0+p]!=NULL;
  if((scene->actor_light || bloom) && actor_attributes && out_projection &&
      ArRenderCapabilities_Has(ArRenderDevice_Capabilities(device),
          kArRenderCapability_CustomShaders | kArRenderCapability_ScopedRenderTargets)) {
    int width,height;
    if(!ArRenderDevice_GetOutputSize(device,&width,&height))goto failed;
    for(unsigned role=0;role<2;++role) {
      const bool receives_bloom=bloom && (scene->native_bloom.receivers & (2u<<role));
      const bool same_bloom=!bloom || ((scene->native_bloom.receivers&6u)==0 || (scene->native_bloom.receivers&6u)==6);
      ArRenderTexture reuse=!receives_bloom && scene->actor_light_reuse[role]==1 ? illuminated.scenery_lightmap :
          role && same_bloom && (scene->actor_light_reuse[role]==2 ||
              (scene->actor_light_reuse[role]==1 && scene->actor_light_reuse[0]==1) ||
              !scene->actor_light) ? illuminated.actor_lightmaps[0] : ArRenderTexture_Invalid();
      if(ArRenderTexture_IsValid(reuse)) { illuminated.actor_lightmaps[role]=reuse;continue; }
      const ArRenderTexture map=EnsureDioramaScratchTarget(device,&s_diorama_actor_lightmaps[role],width,height,kArRenderFilter_Linear);
      ArRenderTargetState saved;
      if(!ArRenderTexture_IsValid(map)||ArRenderDevice_BeginTarget(device,map,&saved)!=kArRenderTargetBegin_Ready)goto failed;
      const DioramaSourceEffectBatch batch = scene->render->source_effect_batch;
      bool drawn=ArRenderDevice_Clear(device,(ArRenderColorF){0,0,0,1})&&
          (!batch || batch(device, true));
      if (drawn) {
        drawn=!scene->actor_light || scene->actor_light(scene->plane_effect_userdata,out_projection,2u<<role);
        if (batch) drawn = batch(device, false) && drawn;
      }
      if (drawn && receives_bloom) {
        const ArRenderRectF dst={0,0,width,height};
        drawn=NativeBloomDraw(device,bloom_light,NULL,&dst,scene->native_bloom.light*.25f,kArRenderBlendMode_Add);
      }
      const bool restored=ArRenderDevice_EndTarget(device,&saved);
      if(!drawn||!restored)goto failed;
      illuminated.actor_lightmaps[role]=map;
    }
  }
  scene = &illuminated;

  if (out_projection && !out_projection->bg2_plane.valid &&
      out_projection->bg2_skybox.count &&
      (scene->effect_bg_plane_mask & (1u << SR_PPU_OVERLAY_BG2))) {
    /* The skybox replaces BG2-low. Draw its attached enhancements here once
     * per UV band, before enclosure, foreground scenery and actors. Keep the
     * mapping published for later moon-lit water and timber receivers. */
    const PresentationOutcome effect = DioramaSubmitSkyboxEffects(&output_frame, scene, out_projection);
    outcome = PresentationOutcome_Combine(outcome, effect);
    if (!PresentationOutcome_IsUsable(effect)) goto failed;
  }
  if (scene->render->shoebox) {
    DioramaPerformance_SetPlane(-1);
    const PresentationOutcome shoebox = DrawDioramaShoebox(
        device, geometry.matrix, geometry.aspect_x, geometry.height_scale,
        geometry.camera.tilt_y, geometry.width, geometry.height);
    outcome = PresentationOutcome_Combine(outcome, shoebox);
    if (!PresentationOutcome_IsUsable(shoebox))
      goto failed;
  }

  DioramaFocalAperture aperture;
  DioramaCapture focal_capture = *capture;
  DioramaViewGeometry focal_geometry = geometry;
  if (DioramaUsesCaptureApron(capture, SR_PPU_OVERLAY_BG1))
    DioramaExpandCaptureApron(&focal_capture, &focal_geometry, SR_PPU_OVERLAY_BG1);
  PrepareDioramaAperture(&focal_capture, scene, &focal_geometry, textures, resolved, resolved_count,
                         &aperture);
  int draw_order[kDioramaLayerCount];
  const int draw_count =
      ResolveDioramaDrawOrder(scene, resolved, resolved_count, draw_order);
  DioramaScene subscreen_scene=*scene;
  subscreen_scene.additive_plane_mask=0;
  PresentationOutcome priority_outcome;bool guard_priority=false;
  const ArRenderTexture priority_surface = BuildDioramaPrioritySurface(
      device, &focal_capture, subscreen_planes?&subscreen_scene:scene, textures, resolved, resolved_count, &priority_outcome,&guard_priority);
  outcome = PresentationOutcome_Combine(outcome, priority_outcome);
  if (!PresentationOutcome_IsUsable(priority_outcome)) goto failed;
  const DioramaResolvedLayer *rear_water=NULL;
  unsigned waterfall_count=0;
  if(ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number))
    ActionVoxel_Waterfalls(scene->voxel_geometry,&waterfall_count);
  if(waterfall_count)
    for(int i=0;i<resolved_count;++i)
      if(resolved[i].plane==kDioramaPlane_Bg2Hi){rear_water=&resolved[i];break;}
  /* Terminal BG1 ink supplies only the missing subscreen contribution. It
   * cannot invent a main term when the native BG2 background is unavailable. */
  const bool terminal_main_coverage=scene->render->skybox!=kDioramaSky_Off &&
      DioramaLayerOrder_SkyboxSource(resolved,resolved_count)==kDioramaLayerSource_Captured &&
      capture->pixels[SR_PPU_OVERLAY_BG2] && ArRenderTexture_IsValid(textures[SR_PPU_OVERLAY_BG2]);
  for (int draw = 0; draw < draw_count; ++draw) {
    const DioramaResolvedLayer *resolved_layer = &resolved[draw_order[draw]];
    const bool subscreen=(subscreen_planes & (1u<<resolved_layer->plane))!=0;
    if(subscreen_active && !subscreen) {
      subscreen_active=false;
      if(!ArRenderDevice_EndTarget(device,&subscreen_state) ||
          !CompositeVoxelSubscreen(device,subscreen_texture,&output_frame))goto failed;
    }
    if(subscreen && !subscreen_active) {
      if(!BeginVoxelSubscreen(device,&output_frame,&subscreen_texture,&subscreen_state,
          !subscreen_background,&subscreen_active,geometry.depth_sink))goto failed;
      subscreen_background=false;
    }
    const DioramaScene *draw_scene=subscreen_active?&subscreen_scene:scene;
    const int prop_priority=DioramaPlaneObjectPriority(resolved_layer->plane);
    if (scene->render->voxel_inspection!=kDioramaVoxelInspect_Backing && prop_priority>=0 && resolved_layer->alpha &&
        (scene->render->visible_planes & (1u<<resolved_layer->plane)) &&
        !DrawVoxelSprites(device,capture,&geometry,draw_scene,&output_frame,(unsigned)prop_priority,false)) goto failed;
    if(rear_water && resolved_layer->plane==kDioramaPlane_Bg1Far) {
      /* Rear water sits over the recessed wall, behind front rock and actors.
       * The native high-priority curtain keeps its own foreground slot. */
      const PresentationOutcome water=DrawResolvedDioramaLayer(device,capture,draw_scene,
          &geometry,&aperture,textures,priority_surface,guard_priority,rear_water,&output_frame,
          out_projection,kVoxelDrawRearWater,false);
      outcome=PresentationOutcome_Combine(outcome,water);
      if(!PresentationOutcome_IsUsable(water))goto failed;
    }
    if (bloom_planes & (1u << resolved_layer->plane)) {
      /* The native face is already composited. Authored atmosphere still
       * belongs in its normal painter slot, outside the bloom extraction. */
      const DioramaLayerDesc *description = DioramaDescForPlane(resolved_layer->plane);
      if (resolved_layer->alpha && description &&
          DioramaLayerIsProjectable(description, textures, capture->pixels, scene)) {
        const PresentationOutcome effect = DioramaSubmitPlaneEffect(
            &output_frame, scene->plane_effect, scene->plane_effect_userdata,
            description->plane, out_projection, scene->render->source_effect_batch);
        outcome = PresentationOutcome_Combine(outcome, effect);
        if (!PresentationOutcome_IsUsable(effect)) goto failed;
      }
      continue;
    }
    const PresentationOutcome layer = DrawResolvedDioramaLayer(
        device, capture, draw_scene, &geometry, &aperture, textures,
        priority_surface,guard_priority, &resolved[draw_order[draw]], &output_frame, out_projection,kVoxelDrawNormal,subscreen_active&&terminal_main_coverage);
    outcome = PresentationOutcome_Combine(outcome, layer);
    if (!PresentationOutcome_IsUsable(layer))
      goto failed;
    if (scene->render->voxel_inspection!=kDioramaVoxelInspect_Backing && prop_priority>=0 && resolved_layer->alpha &&
        (scene->render->visible_planes & (1u<<resolved_layer->plane)) &&
        !DrawVoxelSprites(device,capture,&geometry,draw_scene,&output_frame,(unsigned)prop_priority,true)) goto failed;
  }

  if(subscreen_active) {
    subscreen_active=false;
    if(!ArRenderDevice_EndTarget(device,&subscreen_state) ||
        !CompositeVoxelSubscreen(device,subscreen_texture,&output_frame))goto failed;
  }
  if (!ArRenderOutputFrame_Finish(&output_frame))
    goto failed;
  GroundCoverObserveEnd(scene,true);
  return outcome;

failed:
  GroundCoverObserveEnd(scene,false);
  if(subscreen_active)(void)ArRenderDevice_EndTarget(device,&subscreen_state);
  if (out_projection) memset(out_projection, 0, sizeof(*out_projection));
  return DioramaCompositeCoreFailure(&output_frame);
}

PresentationOutcome Diorama_Composite(ArRenderDevice *device,
    const DioramaCapture *capture, const DioramaView *view,
    const DioramaScene *scene, DioramaProjection *out_projection) {
  const PresentationOutcome outcome = DioramaCompositeBody(device,capture,view,scene,out_projection);
  const DioramaDepthSink *sink = scene ? scene->depth_sink : NULL;
  if (sink && sink->finish &&
      !sink->finish(sink->userdata,device,PresentationOutcome_IsUsable(outcome))) {
    if (out_projection) memset(out_projection,0,sizeof(*out_projection));
    return kPresentationOutcome_CoreFailure;
  }
  return outcome;
}

void Diorama_ResetCompositorResources(ArRenderDevice *device) {
  ResetDesertColumnScratch();
  ResetVoxelAuthoredAtlas(device);
  ResetVoxelWaterColors(device);
  ResetJungleTerminalColors(device);
  DioramaAstralRift_Reset(device);
  /* Materials borrow the map's native handle, so release their states first. */
  DioramaEffectBackend_Reset(device);
  HollowResetMaterial(device);
  ArRenderDevice_DestroyTexture(device,s_diorama_unlit_map);
  s_diorama_unlit_map=ArRenderTexture_Invalid();
  ResetDioramaScratchPool(device, &s_diorama_lightmap);
  for (unsigned i=0;i<3;++i) ResetDioramaScratchPool(device,&s_diorama_bloom[i]);
  for(unsigned role=0;role<2;++role)ResetDioramaScratchPool(device, &s_diorama_actor_lightmaps[role]);
  ResetDioramaScratchPool(device,&s_diorama_voxel_subscreen);
  ResetDioramaSupersample(device);
  ResetDioramaPrioritySurface(device);
  ResetDioramaDofSource(device);
  ResetDioramaScratchPool(device,&s_diorama_sky_weather);
  ResetDioramaStackGroup(device);
  ResetDioramaSkyboxPrefilter(device);
}
