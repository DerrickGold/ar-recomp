#include "actraiser/actraiser_room_profiles.h"

#include "diorama.h"
#include "constants.h"
#include "diorama_effect_backend.h"
#include "diorama_aperture.h"
#include "diorama_edge_aa.h"
#include "diorama_layer_order.h"
#include "diorama_rom_backdrop.h"
#include "diorama_skybox_uv.h"
#include "diorama_stack_group.h"
#include "diorama_depth_shapes.h" /* rake/bow/thick/stack/voxel arithmetic */
#include "diorama_performance.h"
#include "render/scene3d_math.h"
#include "render/scenery_dimming.h"
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

/* BG guard columns and ordinary OBJ captures have two widths in one frame.
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
static bool s_diorama_ss_unavailable;
static ArRenderTexture s_diorama_priority_texture;
static int s_diorama_priority_w, s_diorama_priority_h;
static DioramaScratchPool s_diorama_dof_source;
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

static void ResetDioramaSupersample(ArRenderDevice *device) {
  ResetDioramaScratchPool(device, &s_diorama_ss);
  s_diorama_ss_unavailable = false;
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
static ArRenderTexture BuildDioramaSupersample(
    ArRenderDevice *device, ArRenderTexture source, int obj_apron,
    int snes_width, int snes_height, PresentationOutcome *outcome) {
  if (outcome) *outcome = kPresentationOutcome_Complete;
  if (!ArRenderDevice_IsReady(device) ||
      !ArRenderTexture_IsValid(source) || obj_apron < 0 || snes_width <= 0 ||
      snes_height <= 0 || snes_width > INT_MAX / kDioramaSupersample ||
      snes_height > INT_MAX / kDioramaSupersample) {
    if (outcome) *outcome = kPresentationOutcome_CoreFailure;
    return ArRenderTexture_Invalid();
  }
  const ArRenderTexture ss = EnsureDioramaSupersampleTexture(
      device, snes_width * kDioramaSupersample,
      snes_height * kDioramaSupersample);
  if (!ArRenderTexture_IsValid(ss)) {
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
      DisableDioramaSupersample(device);
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
    (float)obj_apron, 0.0f, (float)snes_width, (float)snes_height,
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
    DisableDioramaSupersample(device);
  return success ? ss : ArRenderTexture_Invalid();
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

/* Projection publication and drawing must describe the same frame. Keeping
 * every visibility/resource gate here prevents a hidden or unuploaded plane
 * from remaining projectable to presentation effects. */
static bool DioramaLayerIsDrawable(
    const DioramaLayerDesc *layer, const ArRenderTexture textures[],
    const uint8_t *const pixels[], const DioramaScene *scene) {
  return layer && Diorama_PlaneEligible(
      layer->plane, (scene->render->visible_planes & (1u << layer->plane)) != 0,
      ArRenderTexture_IsValid(textures[layer->plane]),
      pixels[layer->plane] != NULL,
      scene->render->hud_flat,
      scene->render->skybox == kDioramaSky_Only,
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
      scene->render->skybox == kDioramaSky_Only,
      scene->additive_plane_mask);
}

/* ── 3D projection ───────────────────────────────────────────────────── */

#define DIORAMA_SUBDIV_X 8
#define DIORAMA_SUBDIV_Y kDioramaPlaneSubdivY
#define DIORAMA_VERTS_PER_LAYER ((DIORAMA_SUBDIV_X + 1) * (DIORAMA_SUBDIV_Y + 1))
#define DIORAMA_INDICES_PER_LAYER (DIORAMA_SUBDIV_X * DIORAMA_SUBDIV_Y * 6)
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
                           ArRenderVertex2D *out_verts, int32_t *out_indices,
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
    const ArRenderVertex2D *grid, float width_pixels,
    DioramaEdgeAaMask edge_mask,
    float u_min, float u_max, float v_min, float v_max,
    float texel_width, float texel_height,
    ArRenderVertex2D *vertices, int vertex_capacity, int *vertex_count,
    int32_t *indices, int index_capacity, int *index_count) {
  if (!grid || !vertices || !vertex_count || !indices || !index_count ||
      *vertex_count < 0 || *index_count < 0 ||
      *vertex_count > vertex_capacity || *index_count > index_capacity)
    return false;
  const int vertex_base = *vertex_count;
  const int index_base = *index_count;
  int fringe_vertices = 0;
  int fringe_indices = 0;
  if (!DioramaEdgeAa_BuildFringe(
          grid, DIORAMA_SUBDIV_X, DIORAMA_SUBDIV_Y,
          width_pixels, edge_mask,
          u_min, u_max, v_min, v_max, texel_width, texel_height,
          &vertices[vertex_base], vertex_capacity - vertex_base,
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
    int output_width, int output_height, bool stack_grouping) {
  if (!vertices || vertex_count <= 0 || !indices || index_count <= 0)
    return kPresentationOutcome_Complete;

  const bool use_group = stack_grouping && plan &&
      plan->use_intermediate;
  if (!use_group) {
    return RenderDioramaGeometry(
               device, source, vertices, vertex_count, indices, index_count,
               blend)
        ? kPresentationOutcome_Complete
        : kPresentationOutcome_OptionalOmitted;
  }

  const ArRenderTexture target = EnsureDioramaStackGroupTexture(
      device, plan->target_width, plan->target_height);
  if (!ArRenderTexture_IsValid(target)) {
    return RenderDioramaGeometry(
               device, source, vertices, vertex_count, indices, index_count,
               blend)
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
    return RenderDioramaGeometry(
               device, source, vertices, vertex_count, indices, index_count,
               blend)
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
    target_ready = RenderDioramaGeometry(
        device, source, s_diorama_stack_target_vertices, vertex_count,
        indices, index_count, blend);
  }
  const bool restored = ArRenderDevice_EndTarget(device, &target_state);
  DioramaPerformance_SetRasterViewport(output_width, output_height);
  if (!restored) return kPresentationOutcome_CoreFailure;
  if (!target_ready) {
    DisableDioramaStackGroup(device);
    return RenderDioramaGeometry(
               device, source, vertices, vertex_count, indices, index_count,
               blend)
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
    PresentationOutcome *outcome) {
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
    shader_bound = DioramaEffectBackend_BindBlur(device, &params);
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
  s_diorama_skybox_prefilter_rom_source = rom_source;
  return target;
}

static PresentationOutcome DrawDioramaSkybox(
    ArRenderDevice *device, ArRenderTexture skybox_texture,
    int obj_apron, int snes_width, int snes_height,
    int out_w, int out_h, bool dim,
    float blur_radius, bool rom_source,
    uint64_t source_revision, bool source_dynamic,
    const DioramaBgValidSpanPlan *valid_spans,
    ArRenderPointF capture_offset, bool follow_camera,
    int authentic_y0, float camera_delta, float pixel_aspect,
    DioramaSkyboxProjection *projection, const DioramaRenderOptions *options) {
  if (!ArRenderTexture_IsValid(skybox_texture) || snes_height <= 0)
    return kPresentationOutcome_CoreFailure;
  PresentationOutcome outcome = kPresentationOutcome_Complete;
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
  if (blur_requested && options->skybox_prefilter) {
    PresentationOutcome prefilter_outcome;
    const ArRenderTexture prefiltered = BuildDioramaSkyboxPrefilter(
        device, skybox_texture, source_width, source_height,
        out_w, out_h, tint, blur_radius, rom_source,
        source_revision, source_dynamic, &prefilter_outcome);
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
  if (!rom_source && follow_camera && vertical_valid)
    DioramaSkyboxVerticalMapping_FollowCamera(
        &vertical, source_height, authentic_y0, camera_delta + capture_offset.y);
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
  const float fitted_width = DioramaSkyboxVerticalMapping_FitAspect(
      &vertical, source_height, available_width,
      out_h > 0 ? (float)out_w / out_h : 0.0f, pixel_aspect);
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
    const float centre_u = 0.5f * (band_u0[i] + band_u1[i]);
    const float half_width_u = 0.5f * fitted_width / source_width;
    const float u0 = centre_u - half_width_u;
    const float u1 = centre_u + half_width_u;
    const float vertical_t0 = DioramaSkyboxVerticalMapping_Fraction(
        &vertical, y0);
    const float vertical_t1 = DioramaSkyboxVerticalMapping_Fraction(
        &vertical, y1);
    const float draw_y0 = (float)out_h * vertical_t0;
    const float draw_y1 = (float)out_h * vertical_t1;
    const float v0 = vertical.texture_v0 +
        (vertical.texture_v1 - vertical.texture_v0) * vertical_t0;
    const float v1 = vertical.texture_v0 +
        (vertical.texture_v1 - vertical.texture_v0) * vertical_t1;
    ArRenderVertex2D verts[4] = {
      { { 0.0f, draw_y0 },         tint, { u0, v0 } },
      { { (float)out_w, draw_y0 }, tint, { u1, v0 } },
      { { (float)out_w, draw_y1 }, tint, { u1, v1 } },
      { { 0.0f, draw_y1 },         tint, { u0, v1 } },
    };
    if (!SubmitDioramaGeometry(
            device, skybox_texture, verts, 4, indices, 6, &draw_state)) {
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
    const DioramaProjection *projection) {
  if (!output_frame || !plane_effect || !projection || !projection->valid)
    return kPresentationOutcome_Complete;
  if (!ArRenderOutputFrame_EnterFullOutput(output_frame))
    return kPresentationOutcome_OptionalOmitted;
  DioramaPerformanceScope callback_performance =
      DioramaPerformance_Begin(kDioramaPerformance_Callback);
  plane_effect(plane_effect_userdata, plane, projection);
  DioramaPerformance_End(callback_performance);
  if (!ArRenderOutputFrame_RestoreViewport(output_frame))
    return kPresentationOutcome_CoreFailure;
  return kPresentationOutcome_Complete;
}

static PresentationOutcome DioramaCompositeCoreFailure(
    ArRenderOutputFrame *output_frame) {
  ArRenderOutputFrame_Abort(output_frame);
  return kPresentationOutcome_CoreFailure;
}

typedef struct DioramaViewGeometry {
  const DioramaRenderOptions *options;
  float auto_distance;
  float matrix[16];
  float world_y_offset, bg2_world_y_offset;
  DioramaCamera camera;
  float aspect_x, height_scale;
  float u0, v0, u1, v1;
  int width, height;
} DioramaViewGeometry;

typedef struct DioramaFocalAperture {
  ArRenderVertex2D vertices[DIORAMA_VERTS_PER_LAYER];
  float z;
  bool valid;
} DioramaFocalAperture;

int Diorama_ResolveSceneLayers(const DioramaScene *scene,
                               DioramaResolvedLayer *resolved) {
  if (!scene || !scene->render || !resolved) return 0;
  if (scene->render->resolved_layers) {
    const int count = scene->render->resolved_layer_count;
    if (count <= 0 || count > kDioramaLayerCount) return 0;
    memcpy(resolved, scene->render->resolved_layers,
           (size_t)count * sizeof(*resolved));
    return count;
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
    return DioramaLayerOrder_ResolveSection(
        scene->render->layers, scene->map_group, scene->map_number,
        scene->layer_section, defaults, kDioramaLayerCount, resolved,
        kDioramaLayerCount);
  }
}

/* A periodic native backdrop occupies the same world plane as the playfield,
 * but still paints behind every layer. Inverse projection covers the viewport
 * at every camera pose without scaling camera motion or exposing texture
 * allocation edges. One bounded mesh, one ordinary wrapped texture draw. */
static PresentationOutcome DrawPeriodicDioramaSkybox(
    ArRenderDevice *device, const DioramaCapture *capture,
    const DioramaViewGeometry *geometry, float reference_z,
    DioramaProjection *projection) {
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
  if (!SubmitDioramaGeometry(device, capture->skybox->texture,
          vertices, count, indices, index_count, &state))
    return kPresentationOutcome_CoreFailure;
  if (projection) {
    projection->bg2_skybox = (DioramaSkyboxProjection){
      .world_plane = {.valid = true, .capture_offset = motion,
        .u0 = geometry->u0, .v0 = geometry->v0,
        .u1 = geometry->u1, .v1 = geometry->v1, .z_world = reference_z},
      .count = 1, .active_band = -1,
      .bands = {{x0,y0,x1,y1,0,1}},
    };
  }
  return kPresentationOutcome_Complete;
}

/* The skybox draws first. Missing ROM art falls back to current captured
 * BG2; failed renderer restoration is a core failure, never a fallback. */
static PresentationOutcome DrawResolvedDioramaSkybox(
    ArRenderDevice *device, const DioramaCapture *capture,
    const DioramaViewGeometry *geometry, const ArRenderTexture *textures,
    const DioramaResolvedLayer *resolved, int resolved_count,
    DioramaProjection *projection) {
  PresentationOutcome outcome = kPresentationOutcome_Complete;
  static const float kSkyboxBlurRadiusOnly = 1.0f;
  static const float kSkyboxBlurRadiusBoth = 3.0f;
  if (geometry->options->skybox != kDioramaSky_Off) {
    DioramaPerformance_SetPlane(SR_PPU_OVERLAY_BG2);
    bool both = geometry->options->skybox == kDioramaSky_Both;
    ArRenderTexture skybox_texture = textures[SR_PPU_OVERLAY_BG2];
    bool rom_skybox = false;
    uint64_t skybox_revision = capture->bg2_revision;
    bool skybox_dynamic = capture->bg2_dynamic;
    int skybox_apron = capture->obj_apron;
    int skybox_width = capture->width;
    ArRenderPointF capture_offset = {capture->obj_apron,0};
    if (capture->plane_capture_offsets) {
      capture_offset.x += capture->plane_capture_offsets[SR_PPU_OVERLAY_BG2].x;
      capture_offset.y += capture->plane_capture_offsets[SR_PPU_OVERLAY_BG2].y;
    }
    DioramaBgValidSpanPlan skybox_spans;
    const DioramaBgValidSpanPlan *skybox_valid_spans = capture->bg2_valid_spans;
    const int skybox_source =
        DioramaLayerOrder_SkyboxSource(resolved, resolved_count);
    if (skybox_source == kDioramaLayerSource_Captured && capture->skybox &&
        capture->skybox->periodic && ArRenderTexture_IsValid(capture->skybox->texture)) {
      float reference_z = DioramaBg1ReferenceZ() - 0.5f;
      for (int i = 0; i < resolved_count; ++i)
        if (resolved[i].plane == SR_PPU_OVERLAY_BG1)
          reference_z = resolved[i].z - 0.5f;
      const PresentationOutcome periodic = DrawPeriodicDioramaSkybox(
          device, capture, geometry, reference_z, projection);
      if (periodic != kPresentationOutcome_OptionalOmitted) return periodic;
      /* A camera looking past the plane's horizon uses the ordinary captured
       * skybox. Never submit partially initialized inverse-projection data. */
    }
    if (skybox_source == kDioramaLayerSource_Captured && capture->skybox &&
        !capture->skybox->periodic &&
        ArRenderTexture_IsValid(capture->skybox->texture) &&
        capture->bg2_valid_spans) {
      skybox_texture = capture->skybox->texture;
      skybox_revision = capture->skybox->revision;
      skybox_dynamic = capture->skybox->dynamic;
      skybox_apron = 0;
      skybox_width = capture->skybox->width;
      capture_offset = capture->skybox->capture_offset;
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
      if (capture->bg_transparent_fill_configured &&
          capture->bg_transparent_fill_argb &&
          DioramaLayerOrder_DecodeActionBgSource(skybox_source, &source_group,
                                                 &source_map, &source_bg) &&
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
        skybox_dynamic = false;
        skybox_revision = ((uint64_t)(uint32_t)skybox_source << 33) ^
                          ((uint64_t)transparent_fill_configured << 32) ^
                          (uint64_t)transparent_fill_argb;
      }
    }

    if (rom_skybox || capture->pixels[SR_PPU_OVERLAY_BG2] ||
        (capture->skybox &&
         ArRenderTexture_IsValid(capture->skybox->texture))) {
      const PresentationOutcome skybox = DrawDioramaSkybox(
          device, skybox_texture, skybox_apron, skybox_width, capture->height,
          geometry->width, geometry->height, both,
          both ? kSkyboxBlurRadiusBoth : kSkyboxBlurRadiusOnly, rom_skybox,
          skybox_revision, skybox_dynamic, skybox_valid_spans, capture_offset,
          capture->bg2_scroll_valid, capture->authentic_y0,
          (geometry->world_y_offset + geometry->bg2_world_y_offset) *
              kActRaiserAuthenticHeight,
          geometry->aspect_x * kActRaiserAuthenticHeight / capture->width,
          projection && !projection->bg2_plane.valid
              ? &projection->bg2_skybox : NULL, geometry->options);
      outcome = PresentationOutcome_Combine(outcome, skybox);
      if (!PresentationOutcome_IsUsable(skybox)) {
        return kPresentationOutcome_CoreFailure;
      }
    }
  }
  return outcome;
}

/* UVs describe the displayed capture inside the allocation. World size
 * remains normalized to 224 native rows so margins extend the scene rather
 * than shrinking the playfield. Resolve auto-fit before applying zoom. */
static void PrepareDioramaView(const DioramaCapture *capture,
                               const DioramaView *view,
                               const DioramaScene *scene,
                               const ArRenderTexture *textures,
                               const DioramaResolvedLayer *resolved,
                               int resolved_count,
                               DioramaViewGeometry *geometry) {
  float tex_h = (float)capture->height;
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
  float fit_h = 0.5f / tan_half;
  float fit_w = vis_half_w / (tan_half * screen_aspect);
  static const float kDioramaZ_Hud = 0.95f;
  geometry->auto_distance =
      fmaxf(fit_h, fit_w) * 1.02f + (kDioramaZ_Hud - 0.5f);

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

  float framing_weight = Clampf(view->camera_framing_weight, 0.0f, 1.0f);
  if (framing_weight == 0.0f) {
    Diorama_OffsetCamera(geometry->matrix, capture->framing_x, capture->framing_y, par);
    return;
  }
  float free_matrix[16];
  memcpy(free_matrix, geometry->matrix, sizeof(free_matrix));

  {
    float focal_z = DioramaBg1ReferenceZ() - 0.5f;
    float focal_rake = 0.0f, focal_bow = 0.0f;
    for (int i = 0; i < resolved_count; i++) {
      if (resolved[i].plane != SR_PPU_OVERLAY_BG1) continue;
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
    const int high = primary == SR_PPU_OVERLAY_BG1
        ? kDioramaPlane_Bg1Hi : kDioramaPlane_Bg2Hi;
    const DioramaResolvedLayer *focal = NULL;
    for (int i = 0; i < resolved_count; i++) {
      if ((resolved[i].plane != primary && resolved[i].plane != high) ||
          !resolved[i].alpha)
        continue;
      const DioramaLayerDesc *layer = DioramaDescForPlane(resolved[i].plane);
      if (!DioramaLayerIsDrawable(layer, textures, capture->pixels, scene))
        continue;
      /* Priority splitting can leave the ordinary band entirely empty. */
      if (!focal || resolved[i].plane == primary) focal = &resolved[i];
    }
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

  /* Restore automatic framing and the camera stop alongside the manual pose.
   * Free Cam and active manual input use the untouched camera matrix. */
  if (framing_weight < 1.0f) {
    for (int i = 0; i < 16; i++)
      geometry->matrix[i] = free_matrix[i] + framing_weight *
          (geometry->matrix[i] - free_matrix[i]);
  }
  /* Designer framing is the baseline even during manual orbit. It does not
   * change when extra tiles alter the scenery bounds or capture margins. */
  Diorama_OffsetCamera(geometry->matrix, capture->framing_x, capture->framing_y, par);

  /* Capture width is a storage budget, not the perspective viewport. In
   * particular 16:9 can see past a native edge that still covers 16:10. Clamp
   * after vertical and authored framing so neither can undo the side stop. */
  if (capture->horizontal_bounds.valid) {
    const int primary = capture->horizontal_bounds.plane;
    const int high = primary == SR_PPU_OVERLAY_BG1
        ? kDioramaPlane_Bg1Hi : kDioramaPlane_Bg2Hi;
    const DioramaResolvedLayer *focal = NULL;
    for (int i = 0; i < resolved_count; ++i) {
      if ((resolved[i].plane != primary && resolved[i].plane != high) ||
          !resolved[i].alpha) continue;
      const DioramaLayerDesc *layer = DioramaDescForPlane(resolved[i].plane);
      if (!DioramaLayerIsDrawable(layer, textures, capture->pixels, scene)) continue;
      if (!focal || resolved[i].plane == primary) focal = &resolved[i];
    }
    if (focal) {
      float clamped[16];
      memcpy(clamped, geometry->matrix, sizeof(clamped));
      if (Diorama_ClampCameraHorizontally(
              clamped, geometry->aspect_x, geometry->height_scale,
              focal->z - 0.5f, focal->rake, focal->bow,
              &capture->horizontal_bounds))
        for (int c = 0; c < 16; ++c)
          geometry->matrix[c] += framing_weight * (clamped[c] - geometry->matrix[c]);
    }
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
                   (ArRenderColorF){1.0f, 1.0f, 1.0f, 1.0f}, aperture->vertices,
                   focal_aperture_indices, &focal_aperture_nv,
                   &focal_aperture_ni);
    aperture->valid = focal_aperture_nv == DIORAMA_VERTS_PER_LAYER &&
                      focal_aperture_ni == DIORAMA_INDICES_PER_LAYER;
    break;
  }
}

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
          .u0 = geometry->u0,
          .v0 = geometry->v0,
          .u1 = geometry->u1,
          .v1 = geometry->v1,
          .z_world = resolved[i].z - 0.5f,
          .rake = resolved[i].rake,
          .bow = resolved[i].bow,
      };
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
      if (resolved[i].plane == kDioramaPlane_Bg2Hi) {
        out_projection->bg2_high_plane = plane;
      }
      const int priority = DioramaPlaneObjectPriority(resolved[i].plane);
      if (priority < 0) continue;
      out_projection->object_planes[priority] = plane;
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

typedef struct DioramaLayerDraw {
  const DioramaResolvedLayer *authored;
  const DioramaLayerDesc *description;
  ArRenderTexture texture;
  ArRenderTexture priority_surface;
  ArRenderColorF shade;
  float dimming;
  ArRenderRectF dimming_ramp;
  ArRenderBlendMode blend;
} DioramaLayerDraw;

static void ShadeDioramaVertices(const DioramaLayerDraw *layer,
    ArRenderVertex2D *vertices, int count) {
  if (layer->dimming <= 0) return;
  for (int i = 0; i < count; i++) {
    const float light = 1-SceneryDimming_Amount(layer->dimming,layer->dimming_ramp,
        vertices[i].tex_coord.x,vertices[i].tex_coord.y);
    vertices[i].color.r *= light;
    vertices[i].color.g *= light;
    vertices[i].color.b *= light;
  }
}

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
    PresentationOutcome *outcome) {
  *outcome = kPresentationOutcome_Complete;
  if (!scene->render->priority_surface) return ArRenderTexture_Invalid();
  const DioramaResolvedLayer *low = NULL, *high = NULL;
  for (int i = 0; i < count; i++) {
    if (resolved[i].plane == SR_PPU_OVERLAY_BG1) low = &resolved[i];
    if (resolved[i].plane == kDioramaPlane_Bg1Hi) high = &resolved[i];
  }
  const uint32_t pair_mask = (1u << SR_PPU_OVERLAY_BG1) |
                             (1u << kDioramaPlane_Bg1Hi);
  const uint32_t additive_pair = scene->additive_plane_mask & pair_mask;
  if (!low || !high || low >= high || low->alpha != 255 || high->alpha != 255 ||
      low->z != high->z || low->rake != high->rake || low->bow != high->bow ||
      low->z != kDofFocalZ || high->stack > 0 || high->thickness > 0 ||
      (additive_pair && additive_pair != pair_mask) ||
      !DioramaLayerIsDrawable(DioramaDescForPlane(low->plane), textures,
                               capture->pixels, scene) ||
      !DioramaLayerIsDrawable(DioramaDescForPlane(high->plane), textures,
                               capture->pixels, scene))
    return ArRenderTexture_Invalid();
  if (!DioramaEffectBackend_IsAvailable(device, kDioramaEffect_PrioritySurface))
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
  int32_t indices[DIORAMA_INDICES_PER_LAYER];
  int vertex_count, index_count;
  bool constrained;
} DioramaLayerMesh;

typedef struct DioramaAttachedMesh {
  /* Enough room to append the host plane and its optional edge fringe. */
  ArRenderVertex2D vertices[DIORAMA_ATTACHED_AA_VERTS];
  int32_t indices[DIORAMA_ATTACHED_AA_INDICES];
  int vertex_count, index_count;
  float lower_content_v_max;
} DioramaAttachedMesh;

static void PrepareDioramaLayerMesh(const DioramaCapture *capture,
                                    const DioramaViewGeometry *geometry,
                                    const DioramaFocalAperture *aperture,
                                    const DioramaLayerDraw *layer,
                                    DioramaLayerMesh *mesh) {
  const float z_world = layer->authored->z - 0.5f;
  BuildLayerMesh(geometry->matrix, z_world, layer->authored->rake,
                 layer->authored->bow, geometry->u0, geometry->v0, geometry->u1,
                 geometry->v1, geometry->aspect_x, geometry->height_scale,
                 geometry->width, geometry->height, layer->shade,
                 mesh->vertices, mesh->indices, &mesh->vertex_count,
                 &mesh->index_count);
  ShadeDioramaVertices(layer,mesh->vertices,mesh->vertex_count);
  mesh->constrained = false;
  if (aperture->valid && layer->description->plane != SR_PPU_OVERLAY_BG1 &&
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
}

/* Fold at the authentic bottom; keep the continuation coplanar through
 * the last drawable texel plus tolerance. This makes the attachment stable
 * when authored capture extents change. Only low BG2 publishes the fold for
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
        log_fold_t = fold_t;
        log_overlap_t = overlap_t;
        BuildFoldedOverflowMesh(
            geometry->matrix, extension_y, extension_z, handoff_z,
            extension_height, overlap_t, kShoeboxZFront,
            kAitosWaterfallFrontDrop, geometry->u0, source_v0, geometry->u1,
            source_v1, geometry->aspect_x, geometry->width, geometry->height,
            layer->shade, attached->vertices, attached->indices,
            &attached->vertex_count, &attached->index_count);

        if (attached->vertex_count > 0 && out_projection &&
            layer->description->plane == SR_PPU_OVERLAY_BG2 &&
            out_projection->bg2_plane.valid) {
          DioramaPlaneProjection *plane = &out_projection->bg2_plane;
          plane->overflow_valid = true;
          plane->overflow_fold_t = fold_t;
          plane->overflow_height = extension_height;
          plane->overflow_overlap_t = overlap_t;
          plane->overflow_handoff_z = handoff_z;
          plane->overflow_front_z = kShoeboxZFront;
          plane->overflow_front_drop = kAitosWaterfallFrontDrop;
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
                     copy_color, copy_vertices, copy_indices, &copy_nv,
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
          geometry->width, geometry->height, geometry->options->stack_grouping);
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
          RenderDioramaGeometry(device, layer->texture, skirt_verts, skirt_nv,
                                skirt_indices, skirt_ni, layer->blend));
    }
  }

  return outcome;
}

/* Resolve compact sources before shadows, then bind the main effect for
 * exactly one submission. The attached waterfall precedes its host face in
 * that batch; the host owns their overlap. Every successful bind is unbound
 * before returning, including failures. */
static PresentationOutcome DrawDioramaLayerFace(
    ArRenderDevice *device, const DioramaCapture *capture,
    const DioramaViewGeometry *geometry, const DioramaLayerDraw *layer,
    const DioramaLayerMesh *mesh, DioramaAttachedMesh *attached) {
  PresentationOutcome outcome = kPresentationOutcome_Complete;
  const bool priority_surface = ArRenderTexture_IsValid(layer->priority_surface);
  bool rim_light = layer->description->is_figure && RimLightEnabled(device, geometry->options);
  bool want_dof = !rim_light &&
                  layer->description->plane != SR_PPU_OVERLAY_BG3 &&
                  DofBlurEnabled(device, geometry->options);
  float dof_radius = want_dof ? DofRadiusForLayer(layer->authored->z) : 0.0f;
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

  bool layer_shader_bound = false;
  if (priority_surface) {
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
  } else if (rim_light) {
    const DioramaRimLightEffectParams params = {
        .texel_width = 1.0f / (float)SR_PPU_SURFACE_MAX_WIDTH,
        .texel_height = 1.0f / (float)SR_PPU_SURFACE_MAX_HEIGHT,
        .strength = 0.33f,
    };
    layer_shader_bound = DioramaEffectBackend_BindRimLight(device, &params);
  } else if (use_dof_shader) {
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
  if ((rim_light || use_dof_shader) && !layer_shader_bound) {
    outcome = PresentationOutcome_Combine(outcome,
                                          kPresentationOutcome_OptionalOmitted);
    if (!DioramaEffectBackend_Unbind(device)) {
      return kPresentationOutcome_CoreFailure;
    }
  }

  bool main_submitted = false;
  if (attached->vertex_count > 0) {
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
              draw_verts, 1.0f, edge_mask, draw_u0, draw_u1, draw_v0, draw_v1,
              draw_texel_width, draw_texel_height, attached->vertices,
              DIORAMA_ATTACHED_AA_VERTS, &attached->vertex_count,
              attached->indices, DIORAMA_ATTACHED_AA_INDICES,
              &attached->index_count)) {
        outcome = PresentationOutcome_Combine(
            outcome, kPresentationOutcome_OptionalOmitted);
      }
    }
    main_submitted = RenderDioramaGeometry(
        device, draw_texture, attached->vertices, attached->vertex_count,
        attached->indices, attached->index_count, layer->blend);
  } else if (want_edge) {
    ArRenderVertex2D aa_vertices[DIORAMA_AA_VERTS];
    int32_t aa_indices[DIORAMA_AA_INDICES];
    int aa_vertex_count = mesh->vertex_count;
    int aa_index_count = mesh->index_count;
    memcpy(aa_vertices, draw_verts,
           (size_t)mesh->vertex_count * sizeof(aa_vertices[0]));
    memcpy(aa_indices, mesh->indices,
           (size_t)mesh->index_count * sizeof(aa_indices[0]));
    if (!AppendDioramaEdgeFringe(draw_verts, 1.0f, kDioramaEdgeAa_All, draw_u0,
                                 draw_u1, draw_v0, draw_v1, draw_texel_width,
                                 draw_texel_height, aa_vertices,
                                 DIORAMA_AA_VERTS, &aa_vertex_count, aa_indices,
                                 DIORAMA_AA_INDICES, &aa_index_count)) {
      outcome = PresentationOutcome_Combine(
          outcome, kPresentationOutcome_OptionalOmitted);
    }
    main_submitted = RenderDioramaGeometry(device, draw_texture, aa_vertices,
                                           aa_vertex_count, aa_indices,
                                           aa_index_count, layer->blend);
  } else {
    main_submitted = RenderDioramaGeometry(device, draw_texture, draw_verts,
                                           mesh->vertex_count, mesh->indices,
                                           mesh->index_count, layer->blend);
  }
  if (layer_shader_bound && !DioramaEffectBackend_Unbind(device)) {
    return kPresentationOutcome_CoreFailure;
  }
  if (!main_submitted) {
    return kPresentationOutcome_CoreFailure;
  }
  return outcome;
}

/* The guard columns occupy the existing apron allocation. Expand only BG
 * geometry and its texture crop; native/OBJ coordinates retain their anchor. */
static bool DioramaUsesBgApron(const DioramaCapture *capture, int plane) {
  return capture->obj_apron > 0 && DioramaPlaneUsesBgApron(plane, capture->bg_apron_mask);
}

static void DioramaExpandBgApron(DioramaCapture *capture, DioramaViewGeometry *geometry) {
  const int width = capture->width + 2 * capture->obj_apron;
  if (geometry) {
    geometry->aspect_x *= (float)width / capture->width;
    geometry->u0 = 0.0f;
    geometry->u1 = (float)width / SR_PPU_SURFACE_MAX_WIDTH;
  }
  capture->width = width;
  capture->obj_apron = 0;
  /* Coverage cells describe the ordinary window, not these wider meshes. */
  capture->coverage_masks = NULL;
}

static PresentationOutcome DrawResolvedDioramaLayer(
    ArRenderDevice *device, const DioramaCapture *capture,
    const DioramaScene *scene, const DioramaViewGeometry *geometry,
    const DioramaFocalAperture *aperture, const ArRenderTexture *textures,
    ArRenderTexture priority_surface, const DioramaResolvedLayer *resolved,
    ArRenderOutputFrame *output_frame,
    DioramaProjection *out_projection) {
  if (!resolved->alpha)
    return kPresentationOutcome_Complete;
  const DioramaLayerDesc *description = DioramaDescForPlane(resolved->plane);
  if (!description)
    return kPresentationOutcome_Complete;
  DioramaPerformance_SetPlane(description->plane);
  if (!DioramaLayerIsDrawable(description, textures, capture->pixels, scene)) {
    if (!DioramaLayerIsProjectable(description, textures, capture->pixels, scene))
      return kPresentationOutcome_Complete;
    return DioramaSubmitPlaneEffect(output_frame, scene->plane_effect,
                                    scene->plane_effect_userdata,
                                    description->plane, out_projection);
  }

  const float shade_mix =
      geometry->options->depth_shade;
  const bool scenery = description->plane == SR_PPU_OVERLAY_BG1 ||
      description->plane == kDioramaPlane_Bg1Hi || description->plane == kDioramaPlane_Bg1Far;
  const float dimming = scenery ? fminf(1, fmaxf(0, scene->bg1_dimming)) : 0;
  const bool additive =
      (scene->additive_plane_mask & (1u << (unsigned)description->plane)) != 0;
  const DioramaLayerDraw layer = {
      .authored = resolved,
      .description = description,
      .texture = textures[description->plane],
      .priority_surface = (description->plane == SR_PPU_OVERLAY_BG1 ||
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
      .dimming_ramp = scene->bg1_dimming_ramp,
      .blend = description->plane == kDioramaPlane_Backdrop
                   ? kArRenderBlendMode_Opaque
               : additive ? kArRenderBlendMode_Add
                          : kArRenderBlendMode_Alpha,
  };
  DioramaLayerMesh mesh;
  DioramaViewGeometry layer_geometry = *geometry;
  DioramaCapture layer_capture;
  if (DioramaUsesBgApron(capture, description->plane)) {
    layer_capture = *capture;
    DioramaExpandBgApron(&layer_capture, &layer_geometry);
    capture = &layer_capture;
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
  PrepareDioramaLayerMesh(capture, geometry, aperture, &layer, &mesh);
  DioramaAttachedMesh attached;
  PrepareDioramaWaterfall(capture, scene, geometry, &layer, &attached,
                          out_projection);
  PresentationOutcome outcome =
      DrawDioramaLayerDepth(device, capture, geometry, aperture, &layer);
  if (!PresentationOutcome_IsUsable(outcome))
    return outcome;
  const PresentationOutcome face =
      DrawDioramaLayerFace(device, capture, geometry, &layer, &mesh, &attached);
  outcome = PresentationOutcome_Combine(outcome, face);
  if (!PresentationOutcome_IsUsable(face))
    return outcome;
  const PresentationOutcome effect = DioramaSubmitPlaneEffect(
      output_frame, scene->plane_effect, scene->plane_effect_userdata,
      description->plane, out_projection);
  return PresentationOutcome_Combine(outcome, effect);
}

PresentationOutcome Diorama_Composite(ArRenderDevice *device,
                                      const DioramaCapture *capture,
                                      const DioramaView *view,
                                      const DioramaScene *scene,
                                      DioramaProjection *out_projection) {
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
  ArRenderTexture textures[kDioramaPlane_Count];
  for (int plane = 0; plane < kDioramaPlane_Count; ++plane)
    textures[plane] = capture->textures ? capture->textures[plane]
                                        : ArRenderTexture_Invalid();
  const ArRenderColorF black = {0.0f, 0.0f, 0.0f, 1.0f};
  const ArRenderColorF navy = {20.0f / 255.0f, 20.0f / 255.0f, 30.0f / 255.0f,
                               1.0f};
  ArRenderOutputFrame output_frame;
  if (!ArRenderOutputFrame_Begin(device, view->viewport, black, navy,
                                 &output_frame))
    return kPresentationOutcome_CoreFailure;

  DioramaViewGeometry geometry = {.options = scene->render, .width = view->viewport.w,
                                  .height = view->viewport.h};
  DioramaPerformance_SetViewport(geometry.width, geometry.height);
  DioramaResolvedLayer resolved[kDioramaLayerCount];
  const int resolved_count = Diorama_ResolveSceneLayers(scene, resolved);
  PrepareDioramaView(capture, view, scene, textures, resolved, resolved_count,
                     &geometry);
  PublishDioramaView(capture, view, &geometry, out_projection);
  PublishDioramaPlanes(capture, scene, &geometry, textures, resolved,
                       resolved_count, out_projection);
  PresentationOutcome outcome = DrawResolvedDioramaSkybox(
      device, capture, &geometry, textures, resolved, resolved_count, out_projection);
  if (!PresentationOutcome_IsUsable(outcome))
    goto failed;

  if (out_projection && out_projection->bg2_skybox.count &&
      (scene->effect_bg_plane_mask & (1u << SR_PPU_OVERLAY_BG2))) {
    /* The skybox replaces BG2-low. Draw its attached enhancements here once
     * per UV band, before enclosure, foreground scenery and actors. Keep the
     * mapping published for later moon-lit water and timber receivers. */
    for (unsigned i = 0; i < out_projection->bg2_skybox.count; i++) {
      out_projection->bg2_skybox.active_band = (int)i;
      const PresentationOutcome effect = DioramaSubmitPlaneEffect(
          &output_frame, scene->plane_effect, scene->plane_effect_userdata,
          SR_PPU_OVERLAY_BG2, out_projection);
      outcome = PresentationOutcome_Combine(outcome,effect);
      if (!PresentationOutcome_IsUsable(effect)) goto failed;
    }
    out_projection->bg2_skybox.active_band = -1;
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
  if (DioramaUsesBgApron(capture, SR_PPU_OVERLAY_BG1))
    DioramaExpandBgApron(&focal_capture, &focal_geometry);
  PrepareDioramaAperture(&focal_capture, scene, &focal_geometry, textures, resolved, resolved_count,
                         &aperture);
  int draw_order[kDioramaLayerCount];
  const int draw_count =
      ResolveDioramaDrawOrder(scene, resolved, resolved_count, draw_order);
  PresentationOutcome priority_outcome;
  const ArRenderTexture priority_surface = BuildDioramaPrioritySurface(
      device, &focal_capture, scene, textures, resolved, resolved_count, &priority_outcome);
  outcome = PresentationOutcome_Combine(outcome, priority_outcome);
  if (!PresentationOutcome_IsUsable(priority_outcome)) goto failed;
  for (int draw = 0; draw < draw_count; ++draw) {
    const PresentationOutcome layer = DrawResolvedDioramaLayer(
        device, capture, scene, &geometry, &aperture, textures,
        priority_surface, &resolved[draw_order[draw]], &output_frame, out_projection);
    outcome = PresentationOutcome_Combine(outcome, layer);
    if (!PresentationOutcome_IsUsable(layer))
      goto failed;
  }

  if (!ArRenderOutputFrame_Finish(&output_frame))
    goto failed;
  return outcome;

failed:
  if (out_projection) memset(out_projection, 0, sizeof(*out_projection));
  return DioramaCompositeCoreFailure(&output_frame);
}

void Diorama_ResetCompositorResources(ArRenderDevice *device) {
  ResetDioramaSupersample(device);
  ResetDioramaPrioritySurface(device);
  ResetDioramaDofSource(device);
  ResetDioramaStackGroup(device);
  ResetDioramaSkyboxPrefilter(device);
  DioramaEffectBackend_Reset(device);
}
