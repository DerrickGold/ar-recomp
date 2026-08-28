#include "diorama.h"
#include "gpu_shader_blob.h"
#include "actraiser_game.h"
#include "constants.h"
#include "atomic_replace.h"
#include "diorama_layer_order.h"
#include "diorama_rom_backdrop.h"
#include "diorama_skybox_uv.h"
#include "camera_orbit.h"
#include "diorama_depth_shapes.h" /* rake/bow/thick/stack/voxel arithmetic */
#include "diorama_performance.h"
#include "scene3d_math.h"
#include "presentation_geometry.h"
#include "presentation_upload_mirror.h"
#include "snesrecomp/runner.h"
#include "settings.h"
#include "user_data_dir.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct DioramaRomSkyboxCache {
  uint32_t pixels[kDioramaRomBackdropPixels * kDioramaRomBackdropPixels];
  SDL_Texture *art_texture;
  SDL_Texture *texture;
  int source;
  uint32_t default_fill_argb;
  uint32_t transparent_fill_argb;
  bool transparent_fill_configured;
  bool composite_valid;
  bool resource_failed;
  bool available;
} DioramaRomSkyboxCache;

static const uint8_t *g_diorama_rom_data;
static size_t g_diorama_rom_size;
static DioramaRomSkyboxCache g_rom_skybox = {
  .source = -1,
};
static PresentationUploadMirror
    g_diorama_upload_mirrors[kDioramaPlane_Count];

static void FailRomSkyboxResource(const char *operation) {
  if (g_rom_skybox.resource_failed) return;
  g_rom_skybox.resource_failed = true;
  fprintf(stderr,
          "[diorama] ROM skybox %s failed (%s); using captured fallback\n",
          operation ? operation : "resource", SDL_GetError());
}

bool Diorama_InitRomBackdrops(const uint8_t *rom_data, size_t rom_size) {
  g_diorama_rom_data = rom_data;
  g_diorama_rom_size = rom_size;
  g_rom_skybox.available = false;
  g_rom_skybox.source = -1;
  return rom_data && rom_size > 0;
}

static SDL_Texture *RomSkyboxTexture(SDL_Renderer *renderer, int source,
                                     bool transparent_fill_configured,
                                     uint32_t transparent_fill_argb,
                                     bool *state_restore_failed) {
  if (state_restore_failed) *state_restore_failed = false;
  if (!renderer || !g_diorama_rom_data ||
      !DioramaLayerOrder_SourceIsValid(source) ||
      source == kDioramaLayerSource_Captured)
    return NULL;
  if (g_rom_skybox.source != source) {
    uint8_t group = 0, map = 0, bg = 0;
    g_rom_skybox.available =
        DioramaLayerOrder_DecodeActionBgSource(
            source, &group, &map, &bg) &&
        DioramaRomBackdrop_LoadActionBgSparse(
            g_diorama_rom_data, g_diorama_rom_size, group, map, bg,
            g_rom_skybox.pixels,
            sizeof(g_rom_skybox.pixels) /
                sizeof(g_rom_skybox.pixels[0]),
            &g_rom_skybox.default_fill_argb);
    g_rom_skybox.source = source;
    g_rom_skybox.composite_valid = false;
    g_rom_skybox.resource_failed = false;
    SDL_DestroyTexture(g_rom_skybox.art_texture);
    g_rom_skybox.art_texture = NULL;
    SDL_DestroyTexture(g_rom_skybox.texture);
    g_rom_skybox.texture = NULL;
    fprintf(stderr,
            "[diorama] ROM skybox source=%s decoded=%d\n",
            DioramaLayerOrder_SourceToken(source),
            g_rom_skybox.available ? 1 : 0);
  }
  if (!g_rom_skybox.available || g_rom_skybox.resource_failed) return NULL;
  if (!g_rom_skybox.art_texture) {
    g_rom_skybox.art_texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
        kDioramaRomBackdropPixels, kDioramaRomBackdropPixels);
    if (!g_rom_skybox.art_texture ||
        !SDL_UpdateTexture(g_rom_skybox.art_texture, NULL,
                           g_rom_skybox.pixels,
                           kDioramaRomBackdropPixels *
                               (int)sizeof(g_rom_skybox.pixels[0])) ||
        !SDL_SetTextureScaleMode(
            g_rom_skybox.art_texture, SDL_SCALEMODE_NEAREST) ||
        !SDL_SetTextureBlendMode(
            g_rom_skybox.art_texture, SDL_BLENDMODE_BLEND)) {
      SDL_DestroyTexture(g_rom_skybox.art_texture);
      g_rom_skybox.art_texture = NULL;
      FailRomSkyboxResource("art upload");
      return NULL;
    }
  }
  if (!g_rom_skybox.texture) {
    g_rom_skybox.texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
        kDioramaRomBackdropPixels, kDioramaRomBackdropPixels);
    if (!g_rom_skybox.texture ||
        !SDL_SetTextureScaleMode(
            g_rom_skybox.texture, SDL_SCALEMODE_NEAREST) ||
        !SDL_SetTextureBlendMode(
            g_rom_skybox.texture, SDL_BLENDMODE_BLEND)) {
      SDL_DestroyTexture(g_rom_skybox.texture);
      g_rom_skybox.texture = NULL;
      FailRomSkyboxResource("composite target creation");
      return NULL;
    }
    g_rom_skybox.composite_valid = false;
  }

  const uint32_t fill_argb = transparent_fill_configured
      ? transparent_fill_argb : g_rom_skybox.default_fill_argb;
  if (!g_rom_skybox.composite_valid ||
      g_rom_skybox.transparent_fill_configured !=
          transparent_fill_configured ||
      g_rom_skybox.transparent_fill_argb != fill_argb) {
    PresentationTargetState target_state;
    const PresentationTargetBeginResult begin =
        PresentationGeometry_BeginTarget(
            renderer, g_rom_skybox.texture, &target_state);
    if (begin != kPresentationTargetBegin_Ready) {
      FailRomSkyboxResource("composite target bind");
      if (state_restore_failed)
        *state_restore_failed =
            begin == kPresentationTargetBegin_StateLost;
      return NULL;
    }
    bool composed =
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE) &&
        SDL_SetRenderDrawColor(
            renderer, (Uint8)(fill_argb >> 16), (Uint8)(fill_argb >> 8),
            (Uint8)fill_argb, (Uint8)(fill_argb >> 24)) &&
        SDL_RenderClear(renderer) &&
        SDL_RenderTexture(renderer, g_rom_skybox.art_texture, NULL, NULL);
    const bool state_restored =
        PresentationGeometry_EndTarget(renderer, &target_state);
    if (!state_restored) {
      FailRomSkyboxResource("render-state restore");
      if (state_restore_failed) *state_restore_failed = true;
      return NULL;
    }
    if (!composed) {
      FailRomSkyboxResource("fill composition");
      return NULL;
    }
    g_rom_skybox.transparent_fill_configured =
        transparent_fill_configured;
    g_rom_skybox.transparent_fill_argb = fill_argb;
    g_rom_skybox.composite_valid = true;
  }
  return g_rom_skybox.texture;
}

/* ── Optional GPU shader polish ─────────────────────────────────────────
 *
 * Off by default; requires the "GPU shader effects" setting (main.c, switches
 * the renderer to SDL's "gpu" backend) AND each effect's own toggle, so every
 * effect can be tested in isolation. Any failure (no shader format in common,
 * compile error, render-state creation failure) logs once and falls back to
 * the pre-shader CPU path — this must never be a hard failure, it's additive
 * polish.
 *
 * ── Shader authoring and distribution ───────────────────────────────────
 *
 * These shaders were originally hand-written MSL, which meant they compiled
 * on Metal and were silently dead on every other backend: the old init path
 * bailed unless SDL_GPU_SHADERFORMAT_MSL was offered, so Linux, the Steam
 * Deck and Windows got no GPU effects at all.
 *
 * They are now authored ONCE as GLSL in src/shaders/ and cross-compiled by
 * tools/build_shaders.py into committed headers carrying SPIR-V (Vulkan),
 * DXIL (D3D12), and MSL (Metal). Nothing compiles shaders at build time — the
 * hermetic build (`snesbuild build --hermetic`) has a pinned `zig cc` and
 * nothing else, so a build-time shader toolchain would break the one-download bundle.
 * Regenerate with tools/build_shaders.py after editing any .frag.glsl; the
 * generator refuses to emit a blob whose bindings drifted.
 *
 * The binding convention is documented (SDL_gpu.h, "Shader Resources"):
 * fragment stage uses descriptor set 2 for sampled textures and set 3 for
 * uniform buffers; SDL's render pipeline supplies COLOR0 at location 0 and
 * TEXCOORD0 at location 1, arriving in Metal as [[user(locn0)]]/[[user(locn1)]]
 * with the draw texture at [[texture(0)]]/[[sampler(0)]] and uniforms at
 * [[buffer(0)]]. spirv-cross emits exactly those slots unprompted, which is
 * what makes this pipeline viable; tests/shader_blob_test.c pins it. */

typedef struct { float texel_w, texel_h, radius, _pad; } BlurUniforms;

static SDL_GPUShader *g_blur_shader;
static SDL_GPURenderState *g_blur_state;
static bool g_blur_init_attempted;
static bool g_blur_available;
static SDL_GPUDevice *g_diorama_shader_device;

/* Generated by tools/build_shaders.py from the GLSL under src/shaders. */
#include "shaders/blur_frag.h"
#include "shaders/dof_edge_frag.h"
#include "shaders/rim_frag.h"

static const GpuShaderBlobs kBlurBlobs = {
  kBlurFragMSL, kBlurFragMSLSize, kBlurFragSPV, kBlurFragSPVSize,
  kBlurFragDXIL, kBlurFragDXILSize
};
static const GpuShaderBlobs kDofEdgeBlobs = {
  kDofEdgeFragMSL, kDofEdgeFragMSLSize, kDofEdgeFragSPV, kDofEdgeFragSPVSize,
  kDofEdgeFragDXIL, kDofEdgeFragDXILSize
};
static const GpuShaderBlobs kRimLightBlobs = {
  kRimFragMSL, kRimFragMSLSize, kRimFragSPV, kRimFragSPVSize,
  kRimFragDXIL, kRimFragDXILSize
};


/* 3x3 weighted-box blur (9 taps, center weighted x2) as a cheap Gaussian
 * approximation — softens the existing hard-edged silhouette shadow into a
 * soft drop shadow (doc §7.2: "each layer casts a soft shadow on the one
 * behind it"). Samples the SAME texture/UVs the CPU path already uses;
 * vertex color (the existing black+alpha tint) is preserved by the final
 * multiply, so this is purely additive over the existing effect. */
static void EnsureBlurShader(SDL_Renderer *renderer) {
  if (g_blur_init_attempted) return;
  g_blur_init_attempted = true;

  SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
  SDL_GPUDevice *device = (SDL_GPUDevice *)SDL_GetPointerProperty(
      props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL);
  if (!device) {
    fprintf(stderr, "[gpu-fx] renderer has no GPU device — blur effects "
                    "disabled (enable \"GPU shader effects\" in Graphics settings?)\n");
    return;
  }
  g_diorama_shader_device = device;

  g_blur_shader = GpuShaderBlob_CreateFragment(
      device, &kBlurBlobs, "blur", 1, 1);
  if (!g_blur_shader) return;  /* already reported, with the format */

  SDL_GPURenderStateCreateInfo state_info;
  SDL_zero(state_info);
  state_info.fragment_shader = g_blur_shader;
  g_blur_state = SDL_CreateGPURenderState(renderer, &state_info);
  if (!g_blur_state) {
    fprintf(stderr, "[gpu-fx] blur render state creation failed: %s\n",
            SDL_GetError());
    SDL_ReleaseGPUShader(device, g_blur_shader);
    g_blur_shader = NULL;
    return;
  }

  g_blur_available = true;
  fprintf(stderr, "[gpu-fx] blur shader ready\n");
}

/* kSettingCat_Graphics "Soft shadow blur" row, independent of the other
 * GPU effect toggles. Read fresh every frame (same live-toggle pattern as
 * the diorama_layer_* visibility settings) — both this AND
 * gpu_shaders_enabled (the backend switch, main.c) must be on. */
static bool ShadowBlurEnabled(SDL_Renderer *renderer) {
  if (!g_settings.gpu_fx_shadow) return false;
  EnsureBlurShader(renderer);
  return g_blur_available;
}

/* B5: skybox DoF reuses the same blur shader machinery, but unlike the
 * effects above it has no separate settings toggle — the skybox mode enum
 * itself is the opt-in, and blur is inherent to reading as "atmosphere, not
 * focus" (the doc's framing), not an independent knob. Falls back to a
 * crisp (unblurred) skybox if the shader is unavailable — still far better
 * than the void it replaces. */
static bool SkyboxBlurEnabled(SDL_Renderer *renderer) {
  EnsureBlurShader(renderer);
  return g_blur_available;
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

typedef struct { float texel_w, texel_h, strength, _pad; } RimLightUniforms;

static SDL_GPUShader *g_rim_light_shader;
static SDL_GPURenderState *g_rim_light_state;
static bool g_rim_light_init_attempted;
static bool g_rim_light_available;

static void EnsureRimLightShader(SDL_Renderer *renderer) {
  if (g_rim_light_init_attempted) return;
  g_rim_light_init_attempted = true;

  SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
  SDL_GPUDevice *device = (SDL_GPUDevice *)SDL_GetPointerProperty(
      props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL);
  if (!device) {
    fprintf(stderr, "[gpu-fx] renderer has no GPU device — rim light "
                    "disabled (enable \"GPU shader effects\" in Graphics settings?)\n");
    return;
  }
  g_diorama_shader_device = device;

  g_rim_light_shader = GpuShaderBlob_CreateFragment(
      device, &kRimLightBlobs, "rim light", 1, 1);
  if (!g_rim_light_shader) return;  /* already reported, with the format */

  SDL_GPURenderStateCreateInfo state_info;
  SDL_zero(state_info);
  state_info.fragment_shader = g_rim_light_shader;
  g_rim_light_state = SDL_CreateGPURenderState(renderer, &state_info);
  if (!g_rim_light_state) {
    fprintf(stderr, "[gpu-fx] rim light render state creation failed: %s\n",
            SDL_GetError());
    SDL_ReleaseGPUShader(device, g_rim_light_shader);
    g_rim_light_shader = NULL;
    return;
  }

  g_rim_light_available = true;
  fprintf(stderr, "[gpu-fx] rim light shader ready\n");
}

/* kSettingCat_Graphics "Rim lighting" row, independent of the other GPU
 * effect toggles. Both this AND gpu_shaders_enabled must be on. */
static bool RimLightEnabled(SDL_Renderer *renderer) {
  if (!g_settings.gpu_fx_rim) return false;
  EnsureRimLightShader(renderer);
  return g_rim_light_available;
}

/* ── Depth of field + parallax-aware edge AA, COMBINED ───────────────────
 * Doc §7.2's DOF blur and "parallax-aware anti-aliasing at layer edges."
 * These two target the SAME layer set (BG1/BG2 + their priority-split
 * halves), and SDL only allows ONE custom fragment shader bound per draw
 * call — an earlier version of this code picked edge AA over DOF whenever
 * both were enabled for a layer, which (since both default on) meant DOF
 * silently never rendered at all (confirmed live). Fixed by doing both in
 * one shader pass: blur_radius=0 makes the box-blur a no-op (all 9 taps
 * land on the same texel), edge_feather<=0 skips the edge fade — either
 * knob independently zeroable, so this one shader correctly serves
 * DOF-only, edge-AA-only, both together, or (both zero) neither. */

typedef struct {
  float texel_w, texel_h, blur_radius;
  float u_min, u_max, v_min, v_max;
  float edge_feather, lower_content_v_max;
} DofEdgeUniforms;

static SDL_GPUShader *g_dofedge_shader;
static SDL_GPURenderState *g_dofedge_state;
static bool g_dofedge_init_attempted;
static bool g_dofedge_available;

static void EnsureDofEdgeShader(SDL_Renderer *renderer) {
  if (g_dofedge_init_attempted) return;
  g_dofedge_init_attempted = true;

  SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
  SDL_GPUDevice *device = (SDL_GPUDevice *)SDL_GetPointerProperty(
      props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL);
  if (!device) {
    fprintf(stderr, "[gpu-fx] renderer has no GPU device — DOF/edge AA "
                    "disabled (enable \"GPU shader effects\" in Graphics settings?)\n");
    return;
  }
  g_diorama_shader_device = device;

  g_dofedge_shader = GpuShaderBlob_CreateFragment(
      device, &kDofEdgeBlobs, "DOF/edge AA", 1, 1);
  if (!g_dofedge_shader) return;  /* already reported, with the format */

  SDL_GPURenderStateCreateInfo state_info;
  SDL_zero(state_info);
  state_info.fragment_shader = g_dofedge_shader;
  g_dofedge_state = SDL_CreateGPURenderState(renderer, &state_info);
  if (!g_dofedge_state) {
    fprintf(stderr, "[gpu-fx] DOF/edge AA render state creation failed: %s\n",
            SDL_GetError());
    SDL_ReleaseGPUShader(device, g_dofedge_shader);
    g_dofedge_shader = NULL;
    return;
  }

  g_dofedge_available = true;
  fprintf(stderr, "[gpu-fx] DOF/edge AA shader ready\n");
}

/* kSettingCat_Graphics "Depth of field" row (§7.2). */
static bool DofBlurEnabled(SDL_Renderer *renderer) {
  if (!g_settings.gpu_fx_dof) return false;
  EnsureDofEdgeShader(renderer);
  return g_dofedge_available;
}

/* kSettingCat_Graphics "Edge anti-aliasing" row. */
static bool EdgeAAEnabled(SDL_Renderer *renderer) {
  if (!g_settings.gpu_fx_edgeaa) return false;
  EnsureDofEdgeShader(renderer);
  return g_dofedge_available;
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

/* ── B1b-crisp: ×4 supersample + premultiplied-LINEAR AA ─────────────────
 * The diorama layer textures are SDL_SCALEMODE_NEAREST (main.c ~2528) and
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
 * The intermediate keeps straight alpha and uses ordinary
 * SDL_BLENDMODE_BLEND for the final draw. SDL exposes premultiplied blending,
 * but it is not implemented reliably by every renderer: a backend can accept
 * the mode yet draw the transparent part of a target texture as opaque black.
 * Keeping the widely supported blend path is more important than the small
 * fringe reduction premultiplication can provide on antialiased edge texels.
 *
 * Scoped to layers that DON'T have an M8 custom GPU shader bound (rim
 * light / DOF / edge-AA, all opt-in and off by default): those shaders do
 * their own straight-alpha math (box-blurring texel.rgb, edge-fading via
 * vertex alpha) that assumes a straight-alpha source and BLENDMODE_BLEND —
 * feeding them a premultiplied source would need reworking that math too,
 * out of scope for this AA-only pass. A layer gets one polish path or the
 * other, never both. */
enum { kDioramaSupersample = 4 };

static SDL_Texture *g_diorama_ss_texture;
static int g_diorama_ss_w, g_diorama_ss_h;
static bool g_diorama_ss_unavailable;

static void DisableDioramaSupersample(void) {
  SDL_DestroyTexture(g_diorama_ss_texture);
  g_diorama_ss_texture = NULL;
  g_diorama_ss_w = 0;
  g_diorama_ss_h = 0;
  g_diorama_ss_unavailable = true;
}

static SDL_Texture *EnsureDioramaSupersampleTexture(SDL_Renderer *renderer,
                                                     int w, int h) {
  if (!renderer || w <= 0 || h <= 0) return NULL;
  if (g_diorama_ss_texture && g_diorama_ss_w == w && g_diorama_ss_h == h)
    return g_diorama_ss_texture;
  if (g_diorama_ss_unavailable) return NULL;
  SDL_DestroyTexture(g_diorama_ss_texture);
  g_diorama_ss_texture = NULL;
  g_diorama_ss_texture = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
  if (!g_diorama_ss_texture ||
      !SDL_SetTextureScaleMode(g_diorama_ss_texture, SDL_SCALEMODE_LINEAR)) {
    fprintf(stderr,
            "[diorama] supersample target unavailable; optional crisp AA "
            "disabled for this renderer: %s\n",
            SDL_GetError());
    DisableDioramaSupersample();
    return NULL;
  }
  g_diorama_ss_w = w;
  g_diorama_ss_h = h;
  return g_diorama_ss_texture;
}

/* Renders `source` (an ABI-max-width x snes_height layer texture, already
 * NEAREST-scaled) into the compact shared ×4 straight-alpha intermediate.
 * Returns NULL (caller falls back to `source`) if the intermediate couldn't
 * be (re)created.
 *
 * Live report (2026-07-21): a thin magenta/garbage-colored line was visible
 * at the diorama's right edge whenever a layer used this path (most
 * noticeable on the near-fullscreen backdrop plane) — present even with NO
 * interpolation shift active, so it wasn't the B1b UV-window bug. Root
 * cause: this used to blit the WHOLE source texture (`SDL_RenderTexture(...,
 * NULL, NULL)`) into the WHOLE intermediate, which faithfully copies
 * source's uninitialized tail (columns snes_width..surface-max-width-1 — see
 * B1b UV-window comment below for why that tail exists at all) into the
 * intermediate too. The final draw's LINEAR sample at the exact valid/
 * invalid boundary (u=uv_u1) then blends the last real texel against that
 * garbage, every frame, for every crisp-path layer — B1b-crisp switched
 * this path from NEAREST (no cross-texel blending, so this boundary was
 * never sampled softly) to LINEAR, which is what actually exposed it. Fixed
 * by blitting only the VALID `{0,0,snes_width,snes_height}` source sub-rect
 * into an intermediate sized to that exact active region. */
static SDL_Texture *BuildDioramaSupersample(SDL_Renderer *renderer,
                                            SDL_Texture *source, int obj_apron,
                                            int snes_width, int snes_height,
                                            PresentationOutcome *outcome) {
  if (outcome) *outcome = kPresentationOutcome_Complete;
  if (!renderer || !source || obj_apron < 0 || snes_width <= 0 ||
      snes_height <= 0 || snes_width > INT_MAX / kDioramaSupersample ||
      snes_height > INT_MAX / kDioramaSupersample) {
    if (outcome) *outcome = kPresentationOutcome_CoreFailure;
    return NULL;
  }
  SDL_Texture *ss = EnsureDioramaSupersampleTexture(
      renderer, snes_width * kDioramaSupersample,
      snes_height * kDioramaSupersample);
  if (!ss) {
    if (outcome) *outcome = kPresentationOutcome_OptionalOmitted;
    return NULL;
  }
  PresentationTargetState target_state;
  const PresentationTargetBeginResult begin =
      PresentationGeometry_BeginTarget(renderer, ss, &target_state);
  if (begin != kPresentationTargetBegin_Ready) {
    if (outcome) {
      *outcome = begin == kPresentationTargetBegin_StateLost
          ? kPresentationOutcome_CoreFailure
          : kPresentationOutcome_OptionalOmitted;
    }
    if (begin == kPresentationTargetBegin_Omitted)
      DisableDioramaSupersample();
    return NULL;
  }
  bool success =
      SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE) &&
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0) &&
      SDL_RenderClear(renderer);
  /* Starts at the APRON, not at column 0: the displayed span is the MIDDLE of
   * an apron-wide surface. Blitting from 0 copied the empty left apron in and
   * pushed the content right -- which on the backdrop (the one BLENDMODE_NONE
   * layer, so transparent reads as black) showed as a permanent black stripe
   * down the left edge, fixed in place regardless of level progression. */
  SDL_FRect src = { (float)obj_apron, 0.0f, (float)snes_width,
                    (float)snes_height };
  SDL_FRect dst = { 0.0f, 0.0f, (float)(snes_width * kDioramaSupersample),
                    (float)(snes_height * kDioramaSupersample) };
  SDL_BlendMode old_blend = SDL_BLENDMODE_BLEND;
  bool have_old_blend = false;
  if (success) {
    have_old_blend = SDL_GetTextureBlendMode(source, &old_blend);
    success = have_old_blend &&
        SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE) &&
        SDL_RenderTexture(renderer, source, &src, &dst);
  }
  bool state_restore_failed = false;
  if (have_old_blend && !SDL_SetTextureBlendMode(source, old_blend)) {
    success = false;
    state_restore_failed = true;
  }
  /* Restore the caller's target, not an assumed CRT target. Presentation can
   * deliberately wrap the complete scene in another render pass (heat haze,
   * capture, accessibility filters); hard-coding the boot target silently
   * escaped all of those wrappers whenever this crisp path was active. */
  if (!PresentationGeometry_EndTarget(renderer, &target_state)) {
    state_restore_failed = true;
  }
  if (state_restore_failed) {
    if (outcome) *outcome = kPresentationOutcome_CoreFailure;
    return NULL;
  }
  if (!success && outcome)
    *outcome = kPresentationOutcome_OptionalOmitted;
  if (!success)
    DisableDioramaSupersample();
  return success ? ss : NULL;
}

/* ── Camera constants (§5.6) ─────────────────────────────────────────── */

static const float kDioramaFovY = 0.4f;
static const float kDioramaTiltMin = -0.7f, kDioramaTiltMax = 0.7f;
static const float kDioramaDistMin =  2.0f, kDioramaDistMax = 20.0f;
static const float kDioramaDragRadPerPx = 0.005f;
static const float kDioramaZoomStep     = 0.5f;

float Diorama_DragRadPerPx(void) { return kDioramaDragRadPerPx; }
float Diorama_ZoomStep(void)     { return kDioramaZoomStep; }

/* ── Camera state ────────────────────────────────────────────────────── */

typedef Scene3DCamera DioramaCamera;

/* A3 (followup doc): zero-init, not a hand-tuned literal — every field here
 * is unconditionally overwritten by Diorama_SeedCameraFromSettings (below)
 * before first render (boot, camera-row menu edits, and Reset Camera all
 * call it), so the settings descriptors are the single source of truth for
 * defaults. A literal here would look load-bearing despite never being used. */
static DioramaCamera g_diorama_cam;
static float g_diorama_auto_distance = 5.0f;
static bool g_diorama_settings_dirty;
static uint64_t g_diorama_settings_dirty_at;
static bool s_diorama_dragging;
static CameraOrbit s_diorama_dynamic_orbit;
static const float kDioramaOrbitReturnTimeSeconds = 0.35f;

bool Diorama_IsDragging(void)          { return s_diorama_dragging; }
void Diorama_SetDragging(bool dragging) { s_diorama_dragging = dragging; }

static float Clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/* ── Camera operations ───────────────────────────────────────────────── */

void Diorama_SeedCameraFromSettings(void) {
  g_diorama_cam.tilt_x =
      (float)g_settings.diorama_tilt_x_mrad / (float)kPermilleScale;
  g_diorama_cam.tilt_y =
      (float)g_settings.diorama_tilt_y_mrad / (float)kPermilleScale;
  g_diorama_cam.distance =
      (float)g_settings.diorama_distance_x100 / (float)kPercentScale;
  g_diorama_cam.fov_y = kDioramaFovY;
}

void Diorama_CaptureCameraPresentationState(
    DioramaCameraPresentationState *state) {
  if (!state) return;
  *state = (DioramaCameraPresentationState){
    .mode = g_settings.diorama_camera_mode,
    .free_pose = {
      .tilt_x =
          (float)g_settings.diorama_tilt_x_mrad / (float)kPermilleScale,
      .tilt_y =
          (float)g_settings.diorama_tilt_y_mrad / (float)kPermilleScale,
      .distance =
          (float)g_settings.diorama_distance_x100 / (float)kPercentScale,
    },
    .dynamic_baseline = {
      .tilt_x =
          (float)g_settings.diorama_dyncam_baseline_tilt_x_mrad /
              (float)kPermilleScale,
      .tilt_y =
          (float)g_settings.diorama_dyncam_baseline_tilt_y_mrad /
              (float)kPermilleScale,
      .distance =
          (float)g_settings.diorama_dyncam_baseline_distance_x100 /
              (float)kPercentScale,
    },
    .orbit_yaw = s_diorama_dynamic_orbit.yaw,
    .orbit_pitch = s_diorama_dynamic_orbit.pitch,
  };
}

void Diorama_AdjustCamera(float d_yaw, float d_pitch, float d_zoom) {
  if (g_settings.diorama_camera_mode == kDioramaCam_Dynamic) {
    const float baseline_yaw =
        (float)g_settings.diorama_dyncam_baseline_tilt_y_mrad /
        (float)kPermilleScale;
    const float baseline_pitch =
        (float)g_settings.diorama_dyncam_baseline_tilt_x_mrad /
        (float)kPermilleScale;
    CameraOrbit_Adjust(&s_diorama_dynamic_orbit, d_yaw, d_pitch,
                       baseline_yaw, baseline_pitch,
                       kDioramaTiltMin, kDioramaTiltMax,
                       kDioramaTiltMin, kDioramaTiltMax);

    if (d_zoom == 0.0f) return;
    float distance = g_settings.diorama_dyncam_baseline_distance_x100 > 0
        ? (float)g_settings.diorama_dyncam_baseline_distance_x100 /
              (float)kPercentScale
        : g_diorama_auto_distance;
    distance = Clampf(distance + d_zoom,
                      kDioramaDistMin, kDioramaDistMax);
    g_settings.diorama_dyncam_baseline_distance_x100 =
        (int)(distance * (float)kPercentScale);
    g_diorama_settings_dirty = true;
    g_diorama_settings_dirty_at = SDL_GetTicks();
    return;
  }

  g_diorama_cam.tilt_y = Clampf(g_diorama_cam.tilt_y + d_yaw,
                                kDioramaTiltMin, kDioramaTiltMax);
  g_diorama_cam.tilt_x = Clampf(g_diorama_cam.tilt_x + d_pitch,
                                kDioramaTiltMin, kDioramaTiltMax);
  if (d_zoom != 0.0f) {
    float base = (g_diorama_cam.distance > 0.0f) ? g_diorama_cam.distance
                                                 : g_diorama_auto_distance;
    g_diorama_cam.distance = Clampf(base + d_zoom,
                                    kDioramaDistMin, kDioramaDistMax);
  }
  g_settings.diorama_tilt_x_mrad =
      (int)(g_diorama_cam.tilt_x * (float)kPermilleScale);
  g_settings.diorama_tilt_y_mrad =
      (int)(g_diorama_cam.tilt_y * (float)kPermilleScale);
  g_settings.diorama_distance_x100 =
      (int)(g_diorama_cam.distance * (float)kPercentScale);
  g_diorama_settings_dirty = true;
  g_diorama_settings_dirty_at = SDL_GetTicks();
}

bool Diorama_UpdateDynamicCamera(float elapsed_seconds, bool orbit_held) {
  if (g_settings.diorama_camera_mode != kDioramaCam_Dynamic) {
    bool changed = s_diorama_dynamic_orbit.yaw != 0.0f ||
                   s_diorama_dynamic_orbit.pitch != 0.0f;
    CameraOrbit_Reset(&s_diorama_dynamic_orbit);
    return changed;
  }
  return CameraOrbit_Update(
      &s_diorama_dynamic_orbit, elapsed_seconds, orbit_held,
      kDioramaOrbitReturnTimeSeconds);
}

void Diorama_ResetCamera(void) {
  static const char *const kResetKeys[] = {
    "diorama_tilt_x_mrad",
    "diorama_tilt_y_mrad",
    "diorama_distance_x100",
    /* B4-baseline (followup doc): Reset Camera also returns Dynamic Cam's
     * dedicated baseline pose to its defaults, so it's a true "return
     * everything camera-related to defaults" action regardless of which
     * mode is active. */
    "diorama_dyncam_baseline_tilt_x_mrad",
    "diorama_dyncam_baseline_tilt_y_mrad",
    "diorama_dyncam_baseline_distance_x100",
    "diorama_reactive_strength",
    "diorama_depth_shade",
    "diorama_layer_backdrop",
    "diorama_layer_bg2",
    "diorama_layer_bg1",
    "diorama_layer_obj",
    "diorama_layer_bg3",
    "diorama_skybox",
    "diorama_shoebox",
  };
  for (size_t i = 0; i < sizeof(kResetKeys) / sizeof(kResetKeys[0]); i++) {
    const SettingDesc *row = Settings_Find(kResetKeys[i]);
    if (row) Settings_Reset(row);
  }
  CameraOrbit_Reset(&s_diorama_dynamic_orbit);
  Diorama_SeedCameraFromSettings();
  g_diorama_settings_dirty = true;
  g_diorama_settings_dirty_at = SDL_GetTicks();
}

void Diorama_FlushSettingsIfDirty(void) {
  if (g_diorama_settings_dirty && !s_diorama_dragging &&
      SDL_GetTicks() - g_diorama_settings_dirty_at > 500) {
    g_diorama_settings_dirty = false;
    char settings_path[kHostPathCapacity];
    UserDataFile(settings_path, sizeof settings_path, "settings.ini");
    if (!Settings_Save(settings_path))
      fprintf(stderr, "[diorama] failed to persist camera settings\n");
  }
}

/* ── Layer table ─────────────────────────────────────────────────────── */

typedef struct DioramaLayerDesc {
  int plane;          /* kDioramaPlane_* / SR_PPU_OVERLAY_* index */
  float z;
  SDL_FColor shade;
  bool *visible;
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
    &g_settings.diorama_layer_backdrop, false, false },
  { SR_PPU_OVERLAY_OBJ,  0.51f, { 1.0f,  1.0f,  1.0f,  1.0f },   /* prio 0 */
    &g_settings.diorama_layer_obj, true, true },
  { kDioramaPlane_Obj1,     0.51f, { 1.0f,  1.0f,  1.0f,  1.0f },
    &g_settings.diorama_layer_obj, true, true },
  { kDioramaPlane_Bg2Far,   0.05f, { 0.82f, 0.82f, 0.88f, 1.0f },
    &g_settings.diorama_layer_bg2, false, false },
  { SR_PPU_OVERLAY_BG2,  0.20f, { 0.82f, 0.82f, 0.88f, 1.0f },   /* prio 0 */
    &g_settings.diorama_layer_bg2, false, false },
  { kDioramaPlane_Bg1Far,   0.35f, { 0.92f, 0.92f, 0.95f, 1.0f },
    &g_settings.diorama_layer_bg1, false, false },
  { SR_PPU_OVERLAY_BG1,  0.50f, { 0.92f, 0.92f, 0.95f, 1.0f },   /* prio 0 */
    &g_settings.diorama_layer_bg1, false, false },
  { kDioramaPlane_Obj2,     0.51f, { 1.0f,  1.0f,  1.0f,  1.0f },
    &g_settings.diorama_layer_obj, true, true },
  { kDioramaPlane_Bg2Hi,    0.21f, { 0.82f, 0.82f, 0.88f, 1.0f },
    &g_settings.diorama_layer_bg2, false, false },
  { kDioramaPlane_Bg1Hi,    0.51f, { 0.92f, 0.92f, 0.95f, 1.0f },
    &g_settings.diorama_layer_bg1, false, false },
  { kDioramaPlane_Obj3,     0.52f, { 1.0f,  1.0f,  1.0f,  1.0f },
    &g_settings.diorama_layer_obj, true, true },
  { SR_PPU_OVERLAY_BG3,  0.95f, { 1.0f,  1.0f,  1.0f,  1.0f },
    &g_settings.diorama_layer_bg3, false, true },
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

/* Per-room ($18,$19) layer overrides. The editor mutates this; the draw loop
 * reads it. Empty by default, and DioramaLayerOrder_Resolve on an empty table
 * returns the defaults verbatim in built-in order, so an unedited game is
 * bit-identical to before this existed. */
static DioramaLayerOrderTable g_layer_overrides;

DioramaLayerOrderTable *Diorama_LayerOverrides(void) {
  return &g_layer_overrides;
}

static const DioramaLayerDesc *DioramaDescForPlane(int plane) {
  for (int i = 0; i < kDioramaLayerCount; i++)
    if (kDioramaLayers[i].plane == plane) return &kDioramaLayers[i];
  return NULL;
}

/* Projection publication and drawing must describe the same frame. Keeping
 * every visibility/resource gate here prevents a hidden or unuploaded plane
 * from remaining projectable to presentation effects. */
static bool DioramaLayerIsDrawable(
    const DioramaLayerDesc *layer, SDL_Texture *textures[],
    const uint8_t *const pixels[]) {
  return layer && Diorama_PlaneEligible(
      layer->plane, !layer->visible || *layer->visible,
      textures[layer->plane] != NULL, pixels[layer->plane] != NULL,
      g_settings.diorama_hud_flat,
      g_settings.diorama_skybox == kDioramaSky_Only);
}

/* A BG or OBJ plane may additionally be requested by a current captured
 * effect: its metadata is current content even if the isolated hardware band
 * contributed no final pixels. A content-bearing upload failure is filtered
 * by present.c before it reaches this predicate. */
static bool DioramaLayerIsProjectable(
    const DioramaLayerDesc *layer, SDL_Texture *textures[],
    const uint8_t *const pixels[],
    uint8_t effect_obj_priority_mask, uint32_t effect_bg_plane_mask) {
  if (!layer) return false;
  const int priority = DioramaPlaneObjectPriority(layer->plane);
  const bool has_obj_effect = priority >= 0 &&
      (effect_obj_priority_mask & (1u << (unsigned)priority)) != 0;
  const bool has_bg_effect = layer->plane >= 0 && layer->plane < 32 &&
      (effect_bg_plane_mask & (1u << (unsigned)layer->plane)) != 0;
  return Diorama_PlaneProjectable(
      layer->plane, !layer->visible || *layer->visible,
      textures[layer->plane] != NULL, pixels[layer->plane] != NULL,
      has_obj_effect || has_bg_effect, g_settings.diorama_hud_flat,
      g_settings.diorama_skybox == kDioramaSky_Only);
}

/* ── layer manifest I/O ──────────────────────────────────────────────────
 *
 * `diorama-layers.ini` beside settings.ini. Sections are rooms, bodies are
 * planes:
 *
 *     [layers:01:02]          ; $18=01 $19=02 -- Fillmore act 2
 *     bg2hi = rake:0.29       ; flood the water forward to meet the rock path
 *     bg1   = thick:0.20      ; give the rock path a near face
 *     bg2hi = stack:0.29 copies:4  ; or fill the gap with parallel repeats
 *     bg2   = z:0.30 alpha:200
 *
 * The grammar and every bound live in diorama_layer_order.c, which is pure and
 * tested; this is only the file wrapper. A malformed line is reported and
 * SKIPPED rather than aborting the load, so one typo cannot cost every other
 * authored room. */
static const char kLayerManifestLeaf[] = "diorama-layers.ini";

void Diorama_LoadLayerManifest(void) {
  char path[kHostPathCapacity];
  UserDataFile(path, sizeof path, kLayerManifestLeaf);
  FILE *file = fopen(path, "r");
  if (!file) {
    /* Absent is legitimate -- an unauthored install renders stock geometry and
     * that is correct. Say so anyway, at one line: this file is CWD-relative,
     * and when a packaged build failed to ship it the only symptom was the 3D
     * scene quietly losing every authored room, with nothing anywhere to
     * distinguish "nothing authored" from "the manifest is not where the game
     * is looking". The path is printed for exactly that reason. */
    fprintf(stderr,
            "[diorama-layers] %s not found (looked in the working directory) "
            "-- no per-room layer overrides; the diorama renders stock "
            "geometry\n",
            path);
    return;
  }

  memset(&g_layer_overrides, 0, sizeof(g_layer_overrides));
  DioramaRoomOverride *room = NULL;
  char line[512];
  int rooms = 0, planes = 0, bad = 0, line_number = 0;
  while (fgets(line, sizeof line, file)) {
    line_number++;
    char *at = line;
    while (*at == ' ' || *at == '\t') at++;
    /* Strip a trailing comment BEFORE trimming, so `bg2hi = rake:0.29  ; why`
     * parses. The grammar in diorama_layer_order.c is whitespace-delimited
     * key:value only and would reject the `;` as a malformed pair -- inline
     * comments are a property of this file format, so they belong here rather
     * than in the pure parser. Section lines never needed this (ParseSection
     * only ever sees what is inside the brackets), which is exactly why the
     * first documented example looked fine and its second line did not. */
    for (char *scan = at; *scan; scan++) {
      if (*scan == ';' || *scan == '#') { *scan = '\0'; break; }
    }
    char *end = at + strlen(at);
    while (end > at && (end[-1] == '\n' || end[-1] == '\r' ||
                        end[-1] == ' ' || end[-1] == '\t'))
      *--end = '\0';
    if (!*at) continue;

    if (*at == '[') {
      /* EVERY failure below must clear `room`. A section line that does not
       * resolve cannot leave the PREVIOUS room selected, or the plane lines that
       * follow are silently applied to it -- so a one-character typo in a header
       * changes a different room's rendering while reporting success. That was
       * the behaviour until this comment: the unterminated-'[' arm incremented
       * `bad` and continued without touching `room`. */
      char *close = strchr(at, ']');
      if (!close) {
        fprintf(stderr,
                "[diorama-layers] %s:%d: section missing ']' -- ignored, and "
                "the plane lines under it are skipped\n",
                kLayerManifestLeaf, line_number);
        room = NULL;
        bad++;
        continue;
      }
      *close = '\0';
      uint8_t group = 0, map = 0, section = kDioramaLayerSection_Room;
      if (!DioramaLayerOrder_ParseScopedSection(
              at + 1, &group, &map, &section)) {
        /* Not one of ours -- a foreign section just ends the current room. That
         * is legitimate (the file may grow other sections), so it is not
         * counted as bad. But it IS worth a line: the grammar is strict
         * ("layers:GG:MM[:token]", hex), so `[layers:04:01 ]` with one
         * stray space lands here and would otherwise drop a whole authored room
         * with no output at all. Reported at a lower volume than an error. */
        if (!strncmp(at + 1, "layers", 6))
          fprintf(stderr,
                  "[diorama-layers] %s:%d: '[%s]' is not a valid room header "
                  "(expected [layers:GG:MM[:token]] in hex) -- its plane lines are "
                  "skipped\n",
                  kLayerManifestLeaf, line_number, at + 1);
        room = NULL;
        continue;
      }
      room = DioramaLayerOrder_FindOrAddSection(
          &g_layer_overrides, group, map, section);
      if (!room) {
        fprintf(stderr, "[diorama-layers] %s:%d: table full, room dropped\n",
                kLayerManifestLeaf, line_number);
        bad++;
      } else {
        rooms++;
      }
      continue;
    }
    if (!room) continue;
    const char *error = NULL;
    if (DioramaLayerOrder_ParseLine(room, at, &error)) {
      planes++;
    } else {
      bad++;
      fprintf(stderr, "[diorama-layers] %s:%d: %s -- line skipped\n",
              kLayerManifestLeaf, line_number, error ? error : "bad line");
    }
  }
  fclose(file);
  fprintf(stderr,
          "[diorama-layers] loaded %s: %d room(s), %d plane override(s)%s\n",
          path, rooms, planes, bad ? ", some lines skipped" : "");
}

/* The documentation a genuinely NEW manifest is seeded with. An existing file
 * keeps whatever preamble it already has -- the whole point of the merge is that
 * this text is never allowed to overwrite the user's. */
static const char kLayerManifestPreamble[] =
    "# Diorama per-room layer overrides.\n"
    "#\n"
    "# THERE IS AN IN-GAME EDITOR for this file: turn on \"Show developer\n"
    "# settings\" (System > Game) and a \"Layers\" section appears in the settings\n"
    "# menu. Left/Right cycles a plane through the shapes below and the result is\n"
    "# on screen immediately. Every edit is written back here, so the two are\n"
    "# interchangeable -- and your comments and layout are PRESERVED across a save.\n"
    "#\n"
    "# Section is [layers:GG:MM] with $18/$19 in hex. A camera-local refinement\n"
    "# may use [layers:GG:MM:waterfall] and inherits the base room first. Keys are\n"
    "#   order:<slot>  z:<-1..2>  alpha:<0-255>  rake:<-1..1>  bow:<-1..1>"
    "  thick:<0..1>\n"
    "#   stack:<0..1>  copies:<1..8>  density:<per unit>  dir:<forward|"
    "backward|both>\n"
    "#   voxel:<0..1>  slices:<2..24>\n"
    "# Base BG1/BG2 accept transparent:off, transparent:black, or\n"
    "# transparent:cgram-XX. Off suppresses an inherited room fill. A fill\n"
    "# fills the complete low plane before tiles paint, including untiled areas;\n"
    "# mirror/repeat/clamp remain tile policies and the high band stays sparse.\n"
    "# Action BG virtual depth bands share the same room section. Band 0 is the\n"
    "# new far plane, band 1 the ordinary BG plane, and band 2 its priority-1\n"
    "# plane. Cell rectangles override metatile rules; the ROM priority bit is\n"
    "# the fallback. The virtual plane itself accepts z/order/alpha:\n"
    "#   bg1-virtual = z:0.35 order:4 alpha:255\n"
    "#   bg1-virtual = metatile:23 band:0\n"
    "#   bg1-virtual = cells:4,5-12,5 band:2\n"
    "# Backdrop's source key selects the SKYBOX: captured uses current BG2;\n"
    "# rom-GG-MM-bgN (N=1/2) decodes a stock action BG. Backdrop alpha/z/order\n"
    "# control only the residual plane and do not disable that skybox source.\n"
    "# A named ROM BG source follows the same fill-then-paint rule.\n"
    "# rake tilts a plane in depth (top keeps z, bottom sits at z+rake); bow is\n"
    "# the same tilt EASED. thick extrudes the bottom edge forward. stack fills\n"
    "# the gap with PARALLEL repeats (no tilt, one parallax rate); dir picks which\n"
    "# side to fill and density sets slices per unit depth. voxel is a dense\n"
    "# unfaded stack -- one SOLID object that, unlike thick, respects the art's\n"
    "# silhouette. All compose.\n\n";

bool Diorama_SaveLayerManifest(void) {
  char path[kHostPathCapacity];
  UserDataFile(path, sizeof path, kLayerManifestLeaf);

  /* Read the current file first, so the merge can preserve everything it does
   * not own -- the preamble, hand-written comments, blank lines, and any section
   * the editor has never touched. Absent is the normal first-write case. */
  /* Cap on the manifest we will read back before merging. Generous next to a real
   * file (the shipped one is ~4 KB) but bounded, so a wrong path or a corrupt
   * file cannot make the save allocate without limit. */
  enum { kManifestReadMax = 1 << 20 };
  char *existing = NULL;
  long existing_len = 0;
  FILE *in = fopen(path, "rb");
  if (in) {
    if (fseek(in, 0, SEEK_END) == 0) {
      existing_len = ftell(in);
      if (existing_len < 0) existing_len = 0;
      if (existing_len > kManifestReadMax) existing_len = kManifestReadMax;
      rewind(in);
      existing = (char *)malloc((size_t)existing_len + 1);
      if (existing) {
        size_t got = fread(existing, 1, (size_t)existing_len, in);
        existing[got] = '\0';
      }
    }
    fclose(in);
  }

  /* Size the merged output, then render it. Two passes over a pure function is
   * cheaper and safer than guessing a bound -- and the merge preserves the whole
   * input, so its size is roughly the file's size plus a room or two. */
  size_t need = DioramaLayerOrder_MergeManifest(
      &g_layer_overrides, existing, kLayerManifestPreamble, NULL, 0);
  char *out = (char *)malloc(need + 1);
  if (!out) {
    free(existing);
    fprintf(stderr, "[diorama-layers] out of memory writing %s\n", path);
    return false;
  }
  size_t wrote = DioramaLayerOrder_MergeManifest(
      &g_layer_overrides, existing, kLayerManifestPreamble, out, need + 1);
  free(existing);

  /* Write to a temp file and rename, so a crash mid-write cannot leave the
   * user's manifest truncated -- this file may hold hand-authored content that
   * is not reproducible from the table. */
  /* Room for the manifest path plus the ".tmp" suffix. */
  char tmp[sizeof(path) + 64];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *file = fopen(tmp, "wb");
  if (!file) {
    free(out);
    fprintf(stderr, "[diorama-layers] cannot write %s\n", tmp);
    return false;
  }
  size_t put = fwrite(out, 1, wrote, file);
  free(out);
  if (put != wrote || fflush(file) != 0) {
    fclose(file);
    remove(tmp);
    fprintf(stderr, "[diorama-layers] short write to %s -- original kept\n", tmp);
    return false;
  }
  fclose(file);
  /* One atomic replace on both platforms. A bare rename() FAILS on Windows when
   * the destination exists (packaging builds windows-x86_64/arm64), so every save
   * after the first would silently stop persisting the user's edits -- the same
   * trap handled by buildgui's storeROM. See atomic_replace.h; this file may
   * contain hand-authored rooms that cannot be reproduced from the table, so
   * "original kept" below must be TRUE. */
  if (!AtomicReplaceFile(tmp, path)) {
    remove(tmp);
    fprintf(stderr, "[diorama-layers] could not replace %s -- original kept\n",
            path);
    return false;
  }

  int active = 0;
  for (int i = 0; i < g_layer_overrides.count; i++)
    if (DioramaLayerOrder_RoomIsActive(&g_layer_overrides.rooms[i])) active++;
  fprintf(stderr, "[diorama-layers] wrote %s (%d room(s), comments preserved)\n",
          path, active);
  return true;
}

/* ── 3D projection ───────────────────────────────────────────────────── */

#define DIORAMA_SUBDIV_X 8
#define DIORAMA_SUBDIV_Y 6
#define DIORAMA_VERTS_PER_LAYER ((DIORAMA_SUBDIV_X + 1) * (DIORAMA_SUBDIV_Y + 1))
#define DIORAMA_INDICES_PER_LAYER (DIORAMA_SUBDIV_X * DIORAMA_SUBDIV_Y * 6)
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
                              SDL_FPoint *out_point) {
  Scene3DPoint point;
  if (!Scene3D_ProjectWorldPoint(mvp, x, y, z, screen_w, screen_h,
                                 &point))
    return false;
  *out_point = (SDL_FPoint){ point.x, point.y };
  return true;
}

/* Split a regular vertex grid into two triangles per cell. */
static void TriangulateGrid(int subdiv_u, int subdiv_v, int *out_indices,
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
                           SDL_FColor color,
                           SDL_Vertex *out_verts, int *out_indices,
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
      out_verts[vi].tex_coord = (SDL_FPoint){ u0 + s * (u1 - u0),
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
                                SDL_FColor color,
                                SDL_Vertex *out_verts, int *out_indices,
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
      out_verts[vi].tex_coord = (SDL_FPoint){ u0 + s * (u1 - u0), v_bottom };
      SDL_FColor c = color;
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
 * vectors. Used by DrawDioramaShoebox (gated by g_settings.diorama_shoebox)
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
                          int screen_w, int screen_h, SDL_FColor color,
                          SDL_Vertex *out_verts, int *out_indices,
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
      out_verts[vi].tex_coord = (SDL_FPoint){ u0 + s * (u1 - u0),
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
static void DioramaAitosWaterfallLog(uint8_t map_group, uint8_t map_number,
                                     int layer_section, bool eligible,
                                     int extension_nv, int authentic_y0,
                                     int capture_height, int drawable_y1,
                                     float fold_t, float overlap_t) {
  static int log_on = -1;
  if (log_on < 0) {
    const char *value = getenv("AR_AITOS_WATERFALL_LOG");
    log_on = (value && value[0] && value[0] != '0') ? 1 : 0;
  }
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
    float aspect_x, int screen_w, int screen_h, SDL_FColor color,
    SDL_Vertex *out_verts, int *out_indices,
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
      out_verts[vi].tex_coord = (SDL_FPoint){
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
static void RemapMeshToSupersampleTexture(SDL_Vertex *vertices, int count,
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

/* ── Render ───────────────────────────────────────────────────────────── */

/* M5 (D6/buffer-ownership split): upload remains separate from composite even
 * though presentation is now synchronous. The boundary keeps texture ownership
 * and the producer snapshot explicit. The caller supplies the frame-snapshotted
 * request/content intersection; this function neither reads live settings nor
 * rescans producer-owned pixels. */
DioramaUploadResult Diorama_Upload(
    SDL_Texture *textures[], const uint8_t *const pixels[],
    const size_t pitch_bytes[],
    int snes_width, int snes_height, int obj_apron, uint32_t plane_mask) {
  DioramaUploadResult upload = {0};
  DioramaPerformanceScope performance =
      DioramaPerformance_Begin(kDioramaPerformance_Upload);
  /* `snes_width` is the FULL surface width (display + both aprons); the pitch
   * is always that, because that is how the buffers are laid out. What varies
   * is the destination RECT: a plane that can never hold apron content gets
   * only its display columns uploaded, because the apron columns are known
   * zeros and the textures were zero-filled at creation, so nothing ever
   * changes there. Skipping them is ~47 MB/s at 60fps -- most of the apron's
   * steady-state cost. */
  if (!textures || !pixels || !pitch_bytes || snes_width <= 0 ||
      snes_height <= 0 || obj_apron < 0 || obj_apron > snes_width / 2) {
    DioramaPerformance_End(performance);
    return upload;
  }
  for (int i = 0; i < kDioramaLayerCount; i++) {
    int plane = kDioramaLayers[i].plane;
    if (!(plane_mask & (1u << plane)) ||
        !textures[plane] || !pixels[plane] ||
        pitch_bytes[plane] > INT_MAX)
      continue;
    DioramaPlaneCaptureRegion region;
    if (!DioramaPlaneCaptureRegion_Resolve(
            plane, snes_width, snes_height, obj_apron, &region))
      continue;
    const uint8_t *src = pixels[plane] +
        (size_t)region.x * sizeof(uint32_t);
    PresentationUploadResult plane_upload = {0};
    const bool synchronized = PresentationUploadMirror_UploadArgb8888(
        &g_diorama_upload_mirrors[plane], textures[plane], src,
        region.width, region.height, (int)pitch_bytes[plane],
        region.x, 0, &plane_upload);
    DioramaPerformance_AddPlaneSync(
        synchronized, synchronized && plane_upload.changed,
        plane_upload.uploaded_bytes);
    if (!synchronized) continue;
    upload.synchronized_plane_mask |= 1u << plane;
    if (plane_upload.changed) upload.changed_plane_mask |= 1u << plane;
  }
  DioramaPerformance_End(performance);
  return upload;
}

static bool RenderDioramaGeometry(SDL_Renderer *renderer,
                                  SDL_Texture *texture,
                                  const SDL_Vertex *vertices,
                                  int num_vertices,
                                  const int *indices,
                                  int num_indices) {
  DioramaPerformanceScope performance =
      DioramaPerformance_Begin(kDioramaPerformance_Submit);
  const bool succeeded = SDL_RenderGeometry(
      renderer, texture, vertices, num_vertices, indices, num_indices);
  DioramaPerformance_End(performance);
  DioramaPerformance_AddDraw(succeeded, num_vertices, num_indices);
  return succeeded;
}

static void RecordOptionalDioramaDraw(
    PresentationOutcome *outcome, bool succeeded) {
  if (outcome && !succeeded)
    *outcome = PresentationOutcome_Combine(
        *outcome, kPresentationOutcome_OptionalOmitted);
}

/* Edge margin fix (live report 2026-07-21, fixed
 * 2026-07-26 behind the `diorama_margin_fix` setting / AR_DIORAMA_MARGIN_FIX).
 *
 * Symptom: near a level's start/end the captured BG2 content went black at the
 * world-bound edge instead of extending — a black wedge clipping the skybox.
 * Cause: ActRaiser_ApplyWidescreenPolicy narrows the LIVE per-side margin as the
 * camera reaches a finite world's bound, but every diorama consumer samples the
 * FIXED capture span, and the never-rendered columns are transparent — which
 * this quad's SDL_BLENDMODE_NONE turns into opaque black.
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
 * Still open: the DOF/edge-AA feather below anchors to the fixed span, so the
 * real content edge gets no feather when the span is cropped. Cosmetic, and only
 * on the GPU-shader path. */

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
static PresentationOutcome DrawDioramaSkybox(SDL_Renderer *renderer,
                              SDL_Texture *skybox_texture,
                              int obj_apron, int snes_width, int snes_height,
                              int out_w, int out_h, bool dim,
                              float blur_radius, bool rom_source,
                              const DioramaBgValidSpanPlan *valid_spans) {
  if (!skybox_texture || snes_height <= 0)
    return kPresentationOutcome_CoreFailure;
  PresentationOutcome outcome = kPresentationOutcome_Complete;
  /* [obj_apron, obj_apron+snes_width) -- the DISPLAYED span, which sits in the
   * middle of an apron-wide surface. The valid spans arrive already in the same
   * surface-column space, so the margin-fix branch needs no apron term. */
  float uv_u0_base = (float)obj_apron / (float)SR_PPU_SURFACE_MAX_WIDTH;
  float uv_u1 =
      (float)(obj_apron + snes_width) / (float)SR_PPU_SURFACE_MAX_WIDTH;
  /* Same live report: a visible lighter/garbage-colored strip appeared at
   * the screen's right edge. Root cause: the blur shader samples texels up
   * to `radius` away from each fragment (src/shaders/blur.frag.glsl) —
   * for fragments right at u=uv_u1 (this quad's edge, since
   * uv_u1 < 1.0 is the true boundary of what Diorama_Upload ever wrote,
   * allocated width vs the widescreen capture's max width — the same class of
   * bug B1b's former UV-window clamp exposed for the tilted layers), the rightward
   * samples reach past uv_u1 into that same uninitialized texture memory.
   * Unlike B1b's interpolation shift (which the tilted layers' own address
   * mode could clamp), the blur shader has no knowledge of uv_u1 to clamp
   * against, so the fix here is simpler: never SAMPLE that close to either
   * edge in the first place — inset the mapped UV range by a texel margin
   * comfortably larger than the blur's reach (this also keeps the LEFT
   * edge's leftward samples at u>0, so no explicit CLAMP addressing is
   * needed here). Costs an imperceptible crop of the sky content, not a
   * rendering defect. */
  /* Live report (2026-07-21): {0.30,0.30,0.40} read as jarringly dark for
   * Plane+skybox — the intent is a subtle cue that this is background, not
   * a heavy tint. Lightened substantially; still a touch cool/blue like the
   * rest of the per-layer shade table. */
  static const SDL_FColor kSkyboxDim = { 0.78f, 0.78f, 0.85f, 1.0f };
  static const SDL_FColor kSkyboxFull = { 1.0f, 1.0f, 1.0f, 1.0f };
  SDL_FColor tint = dim ? kSkyboxDim : kSkyboxFull;
  int indices[6] = { 0, 1, 2, 0, 2, 3 };
  /* Captured skyboxes are opaque resolved surfaces. A ROM composite may carry
   * authored transparent gaps when a scoped fill is explicitly Off, so keep
   * its alpha meaningful; opaque default/colour fills render identically. */
  SDL_BlendMode saved_blend = SDL_BLENDMODE_INVALID;
  if (!SDL_GetTextureBlendMode(skybox_texture, &saved_blend))
    return kPresentationOutcome_CoreFailure;
  if (!SDL_SetTextureBlendMode(
          skybox_texture,
          rom_source ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE)) {
    (void)SDL_SetTextureBlendMode(skybox_texture, saved_blend);
    return kPresentationOutcome_CoreFailure;
  }
  const bool blur_requested = SkyboxBlurEnabled(renderer);
  bool blur_bound = false;
  if (blur_requested) {
    const float source_width = rom_source
        ? (float)kDioramaRomBackdropPixels
        : (float)SR_PPU_SURFACE_MAX_WIDTH;
    const float source_height = rom_source
        ? (float)kDioramaRomBackdropPixels
        : (float)SR_PPU_SURFACE_MAX_HEIGHT;
    BlurUniforms u = {
      1.0f / source_width, 1.0f / source_height,
      blur_radius, 0.0f,
    };
    blur_bound = SDL_SetGPURenderStateFragmentUniforms(
                     g_blur_state, 0, &u, sizeof(u)) &&
        SDL_SetGPURenderState(renderer, g_blur_state);
    if (!blur_bound) {
      outcome = kPresentationOutcome_OptionalOmitted;
      if (!SDL_SetGPURenderState(renderer, NULL)) {
        (void)SDL_SetTextureBlendMode(skybox_texture, saved_blend);
        return kPresentationOutcome_CoreFailure;
      }
    }
  }

  /* Fix B/BH6: render each row band over its own actually-valid U span. This
   * is the distinction the former scalar lost in Death Heim: the clamped upper
   * image stretches from the authentic 256, while the repeating fog below it
   * uses the fully padded capture. Equivalent adjacent spans are coalesced by
   * DioramaBgValidSpanPlan_Build, so ordinary rooms still issue one draw with
   * the exact legacy coordinates. The disabled A/B gate likewise forces one
   * legacy full-capture draw. */
  DioramaBgValidSpan legacy = {
    .y0 = 0, .y1 = snes_height,
    .x0 = obj_apron, .x1 = obj_apron + snes_width,
  };
  const DioramaBgValidSpan *spans = &legacy;
  unsigned span_count = 1;
  if (!rom_source && g_settings.diorama_margin_fix &&
      valid_spans && valid_spans->count) {
    spans = valid_spans->spans;
    span_count = valid_spans->count;
    if (span_count > kDioramaBgMaxValidSpans)
      span_count = kDioramaBgMaxValidSpans;
  }
  for (unsigned i = 0; i < span_count; i++) {
    /* A fixed vertical extent is represented by an empty horizontal span for
     * those capture rows. Do not stretch a single boundary texel across it. */
    if (spans[i].x1 <= spans[i].x0) continue;
    int y0 = spans[i].y0 < 0 ? 0 : spans[i].y0;
    int y1 = spans[i].y1 > snes_height ? snes_height : spans[i].y1;
    if (y1 <= y0) continue;
    float u0, u1;
    if (rom_source) {
      DioramaRomSkyboxUvRange(
          snes_width, kDioramaRomBackdropPixels, &u0, &u1);
    } else if (g_settings.diorama_margin_fix) {
      DioramaSkyboxUvRange(SR_PPU_SURFACE_MAX_WIDTH,
                           spans[i].x0, spans[i].x1,
                           blur_radius, &u0, &u1);
    } else {
      float margin_u =
          (blur_radius + 1.0f) / (float)SR_PPU_SURFACE_MAX_WIDTH;
      u0 = uv_u0_base + margin_u;
      u1 = uv_u1 - margin_u;
      if (u1 < u0) u1 = u0;
    }
    const float draw_y0 =
        (float)out_h * ((float)y0 / (float)snes_height);
    const float draw_y1 =
        (float)out_h * ((float)y1 / (float)snes_height);
    const float v0 = rom_source
        ? (float)y0 / (float)snes_height
        : (float)y0 / (float)SR_PPU_SURFACE_MAX_HEIGHT;
    const float v1 = rom_source
        ? (float)y1 / (float)snes_height
        : (float)y1 / (float)SR_PPU_SURFACE_MAX_HEIGHT;
    SDL_Vertex verts[4] = {
      { { 0.0f, draw_y0 },         tint, { u0, v0 } },
      { { (float)out_w, draw_y0 }, tint, { u1, v0 } },
      { { (float)out_w, draw_y1 }, tint, { u1, v1 } },
      { { 0.0f, draw_y1 },         tint, { u0, v1 } },
    };
    if (!RenderDioramaGeometry(
            renderer, skybox_texture, verts, 4, indices, 6))
      outcome = kPresentationOutcome_CoreFailure;
  }
  const bool shader_restored =
      !blur_bound || SDL_SetGPURenderState(renderer, NULL);
  const bool blend_restored =
      SDL_SetTextureBlendMode(skybox_texture, saved_blend);
  if (!shader_restored || !blend_restored)
    return kPresentationOutcome_CoreFailure;
  return outcome;
}

/* B6 (followup doc): floor/ceiling/side-wall enclosure. z_back/z_front match
 * the backdrop's and HUD's z_world exactly (kDioramaLayers' z=0.00/0.95,
 * minus the 0.5 offset every layer's z_world applies) so the box lines up
 * with the layer stack's own depth range. Flat-shaded and untextured —
 * SDL_RenderGeometry accepts a NULL texture for vertex-color-only rendering
 * (doc's "start flat"; no wall art yet). Built via BuildQuadMesh (GEO) —
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
    SDL_Renderer *renderer, const float mvp[16],
    float aspect_x, float height_scale, float tilt_y,
    int out_w, int out_h) {
  /* Live report (2026-07-21): opaque walls read as a plain gray box,
   * disconnected from the skybox (drawn before everything, including these
   * walls) — since the walls are the same painter's-algorithm layer as the
   * skybox's "far opening," letting them stay translucent lets the sky page
   * straight through instead of needing to texture the walls separately.
   * 0.35 then read as too faint to actually define an edge at low tilt —
   * split the difference. */
  static const SDL_FColor kShoeboxColor = { 0.15f, 0.15f, 0.20f, 0.55f };
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
  SDL_Vertex verts[4];
  int indices[6];
  int nv, ni;

  SDL_BlendMode saved_blend = SDL_BLENDMODE_INVALID;
  if (!SDL_GetRenderDrawBlendMode(renderer, &saved_blend))
    return kPresentationOutcome_OptionalOmitted;
  if (!SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND)) {
    return SDL_SetRenderDrawBlendMode(renderer, saved_blend)
        ? kPresentationOutcome_OptionalOmitted
        : kPresentationOutcome_CoreFailure;
  }
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
      RenderDioramaGeometry(renderer, NULL, verts, nv, indices, ni));

  BuildQuadMesh(mvp, -hx, half_y, kShoeboxZBack,
               2.0f * hx, 0.0f, 0.0f,
               0.0f, 0.0f, z_span,
               0.0f, 0.0f, 1.0f, 1.0f, 1, 1, out_w, out_h, kShoeboxColor,
               verts, indices, &nv, &ni);
  RecordOptionalDioramaDraw(
      &outcome,
      RenderDioramaGeometry(renderer, NULL, verts, nv, indices, ni));

  /* Side walls (x=±hx): SDL_RenderGeometry has no depth test, so a wall on
   * the camera's near side would occlude the view straight into the box —
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
    SDL_FColor c = kShoeboxColor;
    c.a *= alpha_neg_x;
    BuildQuadMesh(mvp, -hx, -half_y, kShoeboxZBack,
                 0.0f, 0.0f, z_span,
                 0.0f, 2.0f * half_y, 0.0f,
                 0.0f, 0.0f, 1.0f, 1.0f, 1, 1, out_w, out_h, c,
                 verts, indices, &nv, &ni);
    RecordOptionalDioramaDraw(
        &outcome,
        RenderDioramaGeometry(renderer, NULL, verts, nv, indices, ni));
  }
  if (alpha_pos_x > 0.01f) {
    SDL_FColor c = kShoeboxColor;
    c.a *= alpha_pos_x;
    BuildQuadMesh(mvp, hx, -half_y, kShoeboxZBack,
                 0.0f, 0.0f, z_span,
                 0.0f, 2.0f * half_y, 0.0f,
                 0.0f, 0.0f, 1.0f, 1.0f, 1, 1, out_w, out_h, c,
                 verts, indices, &nv, &ni);
    RecordOptionalDioramaDraw(
        &outcome,
        RenderDioramaGeometry(renderer, NULL, verts, nv, indices, ni));
  }
  if (!SDL_SetRenderDrawBlendMode(renderer, saved_blend))
    return kPresentationOutcome_CoreFailure;
  return outcome;
}

/* The BG1 gameplay plane's depth, matching kDioramaLayers' entry for it. The
 * vertical-shift solve needs a reference depth and this is the layer the eye
 * reads as "the picture". */
/* Depth the vertical-shift solve measures against: the BG1 gameplay plane, the
 * layer the eye reads as "the picture". Looked up from kDioramaLayers rather
 * than restated as a constant -- a second copy of 0.50f would silently stop
 * tracking the table the moment a room override or a table edit moved BG1. */
static float DioramaBg1ReferenceZ(void) {
  for (int i = 0; i < kDioramaLayerCount; i++)
    if (kDioramaLayers[i].plane == SR_PPU_OVERLAY_BG1)
      return kDioramaLayers[i].z;
  return 0.50f;
}

/* How far to lift the world so the vertical band reads correctly.
 *
 * `pin` is half the added height -- the shift that keeps the AUTHENTIC band
 * exactly where it was and lets every new row bleed off the top. That is the
 * right answer only while the composition has empty space up there to spend.
 * It is a fixed WORLD-space offset, but its SCREEN effect depends on pitch:
 * pitching down tilts the plane so its projected centre sits low, leaving slack
 * above, and `pin` happens to consume exactly that. At a flat camera there is
 * no such slack -- the content already sat centred -- so `pin` pushes the whole
 * box against the top edge and opens a large gap along the bottom. Reported
 * from a flat free-cam run and measured there at 144px of top bias.
 *
 * So: lift by `pin`, but never past the point where the drawn content is
 * vertically centred in the viewport. Increasing d moves content up, so the
 * projected centre falls monotonically and a bisection is exact enough.
 *
 *   flat camera   -> centring shift is 0            -> d = 0, content centred
 *   pitched down  -> centring shift exceeds `pin`   -> d = pin, band pinned
 *
 * Both endpoints are what those cases already wanted, there is no jump between
 * them, and `pin == 0` (no band) short-circuits to the untouched matrix. */
/* Projected top and bottom of the drawn content for a candidate lift, or false
 * if either endpoint is unprojectable. One helper instead of two near-identical
 * bodies, and a plain function instead of macros -- the previous macro pair hid
 * a `return` from the caller's control flow, which is exactly the kind of thing
 * that made the "always apply the bottom floor" branch easy to get wrong. */
static bool DioramaContentExtent(const float mvp[16], float half, float lift,
                                 float z_ref, int out_w, int out_h,
                                 float *out_top, float *out_bottom) {
  float m[16];
  memcpy(m, mvp, sizeof(m));
  for (int r = 0; r < 4; r++) m[12 + r] += lift * mvp[4 + r];
  Scene3DPoint top, bottom;
  if (!Scene3D_ProjectWorldPoint(m, 0.0f, half, z_ref,
                                 out_w, out_h, &top) ||
      !Scene3D_ProjectWorldPoint(m, 0.0f, -half, z_ref,
                                 out_w, out_h, &bottom))
    return false;
  if (out_top) *out_top = top.y;
  if (out_bottom) *out_bottom = bottom.y;
  return true;
}

/* Bisect `lift` in [lo,hi] for the largest value still satisfying `too_low`.
 * Both solves below are the same monotone search: a bigger lift moves content
 * up, so each measured quantity falls as the lift grows. */
static float DioramaBisectLift(const float mvp[16], float half, float z_ref,
                               int out_w, int out_h, float lo, float hi,
                               float target, bool use_centre) {
  for (int i = 0; i < 24; i++) {
    float mid = 0.5f * (lo + hi), top, bottom;
    if (!DioramaContentExtent(mvp, half, mid, z_ref, out_w, out_h, &top, &bottom))
      return hi;
    float value = use_centre ? 0.5f * (top + bottom) : bottom;
    if (value > target) lo = mid; else hi = mid;
  }
  return 0.5f * (lo + hi);
}

static float DioramaPositiveVerticalShift(const float mvp[16],
                                          float height_scale,
                                          float pin, int out_w, int out_h) {
  if (pin <= 0.0f)
    return 0.0f;
  const float half = 0.5f * height_scale;
  const float target = 0.5f * (float)out_h;
  const float z_ref = DioramaBg1ReferenceZ();
  float top, bottom, centre_0, centre_pin;

  if (!DioramaContentExtent(mvp, half, 0.0f, z_ref, out_w, out_h, &top, &bottom))
    return pin;               /* unprojectable: keep the pinned default */
  centre_0 = 0.5f * (top + bottom);

  float d;
  if (centre_0 <= target) {
    d = 0.0f;                 /* already at or above centre; lifting worsens it */
  } else {
    if (!DioramaContentExtent(mvp, half, pin, z_ref, out_w, out_h, &top, &bottom))
      return pin;
    centre_pin = 0.5f * (top + bottom);
    d = (centre_pin >= target)
        ? pin                 /* even a full pin does not reach centre */
        : DioramaBisectLift(mvp, half, z_ref, out_w, out_h, 0.0f, pin,
                            target, true);
  }

  /* Floor the lift so centring never drops the content's BOTTOM further past
   * the viewport than a full pin would. Centring spends slack; when the content
   * is already taller than the window there is none, and a smaller lift only
   * buys losing rows off the bottom of the PLAYFIELD -- strictly worse than
   * losing band rows off the top, which is what the pin gives up.
   *
   * Applies in EVERY branch above, including d = 0: that is precisely the flat
   * camera at a tight fit, where centring would otherwise crop the playfield. */
  float bottom_now, bottom_pin;
  if (!DioramaContentExtent(mvp, half, d, z_ref, out_w, out_h, NULL, &bottom_now) ||
      !DioramaContentExtent(mvp, half, pin, z_ref, out_w, out_h, NULL, &bottom_pin))
    return pin;
  if (bottom_now > (float)out_h && bottom_pin < bottom_now)
    d = DioramaBisectLift(mvp, half, z_ref, out_w, out_h, d, pin,
                          (float)out_h, false);
  return d > pin ? pin : d;
}

/* The established solver is expressed as an upward lift. Mirror both world Y
 * and projected screen Y to reuse it exactly when a bottom-heavy capture asks
 * for a downward shift; mirroring twice preserves the source orientation while
 * turning original lift -d into mirrored lift +d. */
static float DioramaVerticalShift(const float mvp[16], float height_scale,
                                  float pin, int out_w, int out_h) {
  if (pin >= 0.0f)
    return DioramaPositiveVerticalShift(
        mvp, height_scale, pin, out_w, out_h);
  float mirrored[16];
  memcpy(mirrored, mvp, sizeof(mirrored));
  for (int r = 0; r < 4; r++) mirrored[4 + r] = -mirrored[4 + r];
  for (int c = 0; c < 4; c++) mirrored[c * 4 + 1] = -mirrored[c * 4 + 1];
  return -DioramaPositiveVerticalShift(
      mirrored, height_scale, -pin, out_w, out_h);
}

static PresentationOutcome DioramaSubmitPlaneEffect(
    SDL_Renderer *renderer, DioramaPlaneEffectFn plane_effect,
    void *plane_effect_userdata, int plane,
    const DioramaProjection *projection, bool viewport_is_output,
    const SDL_Rect *viewport) {
  if (!renderer || !plane_effect || !projection || !projection->valid ||
      !viewport)
    return kPresentationOutcome_Complete;
  if (!viewport_is_output && !SDL_SetRenderViewport(renderer, NULL))
    return kPresentationOutcome_OptionalOmitted;
  DioramaPerformanceScope callback_performance =
      DioramaPerformance_Begin(kDioramaPerformance_Callback);
  plane_effect(plane_effect_userdata, plane, projection);
  DioramaPerformance_End(callback_performance);
  if (!viewport_is_output &&
      !SDL_SetRenderViewport(renderer, viewport))
    return kPresentationOutcome_CoreFailure;
  return kPresentationOutcome_Complete;
}

static PresentationOutcome DioramaCompositeCoreFailure(
    SDL_Renderer *renderer, bool viewport_is_output) {
  if (!viewport_is_output)
    (void)SDL_SetRenderViewport(renderer, NULL);
  return kPresentationOutcome_CoreFailure;
}

PresentationOutcome Diorama_Composite(
    SDL_Renderer *renderer, int snes_width, int snes_height,
    int authentic_y0, int obj_apron,
    int active_pixel_aspect, bool ignore_aspect_ratio,
    int visible_width, SDL_Rect viewport,
    SDL_Texture *textures[], const uint8_t *const pixels[],
    const bool bg_transparent_fill_configured[2],
    const uint32_t bg_transparent_fill_argb[2],
    const DioramaCameraPose *cam_pose, float distance_scale,
    uint32_t additive_plane_mask,
    uint8_t effect_obj_priority_mask, uint32_t effect_bg_plane_mask,
    uint8_t map_group, uint8_t map_number, uint8_t layer_section,
    const DioramaBgValidSpanPlan *bg2_valid_spans,
    DioramaPlaneEffectFn plane_effect, void *plane_effect_userdata,
    DioramaProjection *out_projection) {
  if (out_projection) memset(out_projection, 0, sizeof(*out_projection));
  if (!renderer || !cam_pose || authentic_y0 < 0 ||
      authentic_y0 + kActRaiserAuthenticHeight > snes_height)
    return kPresentationOutcome_CoreFailure;

  if (!SDL_SetRenderLogicalPresentation(
          renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED))
    return kPresentationOutcome_CoreFailure;
  int out_w = 0, out_h = 0;
  if (!SDL_GetRenderOutputSize(renderer, &out_w, &out_h) ||
      out_w <= 0 || out_h <= 0 || viewport.x < 0 || viewport.y < 0 ||
      viewport.w <= 0 || viewport.h <= 0 ||
      viewport.x + viewport.w > out_w || viewport.y + viewport.h > out_h)
    return kPresentationOutcome_CoreFailure;

  PresentationOutcome outcome = kPresentationOutcome_Complete;

  /* The 3D compositor works in coordinates local to the game viewport. This
   * keeps the same projection math at any window size while SDL offsets and
   * clips the result into the aspect-fit rectangle. RenderClear deliberately
   * ignores SDL's viewport, so clear the full target to black first when bars
   * are needed, then fill only the game area with Diorama's navy backdrop. */
  const bool viewport_is_output =
      viewport.x == 0 && viewport.y == 0 &&
      viewport.w == out_w && viewport.h == out_h;
  if (!viewport_is_output) {
    if (!SDL_SetRenderViewport(renderer, NULL) ||
        !SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) ||
        !SDL_RenderClear(renderer) ||
        !SDL_SetRenderViewport(renderer, &viewport) ||
        !SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE) ||
        !SDL_SetRenderDrawColor(renderer, 20, 20, 30, 255) ||
        !SDL_RenderFillRect(renderer, &(SDL_FRect){
            0.0f, 0.0f, (float)viewport.w, (float)viewport.h}))
      return kPresentationOutcome_CoreFailure;
  }
  out_w = viewport.w;
  out_h = viewport.h;

  /* Resolve this room's authored overrides before the far-background pass.
   * The Backdrop record carries the skybox source as room-scoped metadata, so
   * waiting until the in-box layer loop would leave the skybox stuck on live
   * BG2 even though the editor and decoder had selected a ROM source. */
  DioramaResolvedLayer resolved[kDioramaLayerCount];
  int resolved_count;
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
    resolved_count = DioramaLayerOrder_ResolveSection(
        &g_layer_overrides, map_group, map_number, layer_section,
        defaults, kDioramaLayerCount, resolved, kDioramaLayerCount);
  }

  if (viewport_is_output) {
    if (!SDL_SetRenderDrawColor(renderer, 20, 20, 30, 255) ||
        !SDL_RenderClear(renderer))
      return kPresentationOutcome_CoreFailure;
  }

  /* B5 (followup doc): drawn before the per-layer loop below — painter's
   * algorithm, skybox is the farthest thing in the scene. Captured mode uses
   * the same pixels[]-populated guard as the layer loop so stale BG2 cannot
   * draw; an authored ROM source is immutable and does not need that guard.
   *
   * Live report (2026-07-21): Skybox-only wants noticeably LESS blur than
   * Plane+skybox — it's the entire visible background there (no sharper
   * in-box copy to contrast against), so the same heavy blur just reads as
   * "broken," not atmospheric. */
  static const float kSkyboxBlurRadiusOnly = 1.0f;
  static const float kSkyboxBlurRadiusBoth = 3.0f;
  if (g_settings.diorama_skybox != kDioramaSky_Off) {
    bool both = g_settings.diorama_skybox == kDioramaSky_Both;
    SDL_Texture *skybox_texture = textures[SR_PPU_OVERLAY_BG2];
    bool rom_skybox = false;
    const int skybox_source =
        DioramaLayerOrder_SkyboxSource(resolved, resolved_count);
    if (skybox_source != kDioramaLayerSource_Captured) {
      uint8_t source_group = 0, source_map = 0, source_bg = 0;
      bool transparent_fill_configured = false;
      uint32_t transparent_fill_argb = 0;
      if (bg_transparent_fill_configured && bg_transparent_fill_argb &&
          DioramaLayerOrder_DecodeActionBgSource(
              skybox_source, &source_group, &source_map, &source_bg) &&
          (source_bg == 1 || source_bg == 2)) {
        transparent_fill_configured =
            bg_transparent_fill_configured[source_bg - 1];
        transparent_fill_argb =
            bg_transparent_fill_argb[source_bg - 1];
      }
      bool skybox_state_restore_failed = false;
      SDL_Texture *named = RomSkyboxTexture(
          renderer, skybox_source, transparent_fill_configured,
          transparent_fill_argb,
          &skybox_state_restore_failed);
      /* A missing/invalid ROM backdrop can safely fall back to captured BG2.
       * A failed renderer-state restore cannot: continuing could submit the
       * rest of the frame to the cache target or through stale clip/viewport
       * state, producing backend-specific corruption. */
      if (skybox_state_restore_failed) {
        return DioramaCompositeCoreFailure(renderer, viewport_is_output);
      }
      if (named) {
        skybox_texture = named;
        rom_skybox = true;
      }
    }
    /* Named ROM art is immutable and supplies its own current pixels. A decode
     * or upload failure falls back to captured BG2, retaining the established
     * current-frame guard so a stale live texture cannot leak into a scene. */
    if (rom_skybox || pixels[SR_PPU_OVERLAY_BG2]) {
      if (rom_skybox &&
          !SDL_SetRenderTextureAddressMode(
              renderer, SDL_TEXTURE_ADDRESS_WRAP,
              SDL_TEXTURE_ADDRESS_CLAMP)) {
        return DioramaCompositeCoreFailure(renderer, viewport_is_output);
      }
      const PresentationOutcome skybox = DrawDioramaSkybox(
          renderer, skybox_texture,
          obj_apron, snes_width, snes_height, out_w, out_h, both,
          both ? kSkyboxBlurRadiusBoth : kSkyboxBlurRadiusOnly,
          rom_skybox, bg2_valid_spans);
      const bool address_restored = !rom_skybox ||
          SDL_SetRenderTextureAddressMode(
              renderer, SDL_TEXTURE_ADDRESS_AUTO,
              SDL_TEXTURE_ADDRESS_AUTO);
      outcome = PresentationOutcome_Combine(outcome, skybox);
      if (!address_restored || !PresentationOutcome_IsUsable(skybox)) {
        return DioramaCompositeCoreFailure(renderer, viewport_is_output);
      }
    }
  }

  float tex_h = (float)snes_height;
  /* The UV window is the DISPLAYED span [obj_apron, obj_apron+snes_width), not
   * the whole surface: the apron carries resolve headroom, never extra world to
   * show. Sampling from column 0 dragged both empty apron bands into every
   * plane and widened the picture by 2*apron. */
  float uv_u0 = (float)obj_apron / (float)SR_PPU_SURFACE_MAX_WIDTH;
  float uv_u1 =
      (float)(obj_apron + snes_width) / (float)SR_PPU_SURFACE_MAX_WIDTH;
  /* V now divides by the TEXTURE height the way U always divided by the
   * texture width. The old form (`1 - slack/tex_h`) silently assumed the
   * texture was exactly as tall as its content, which stopped being true once
   * the planes use the ABI maximum height to hold the vertical margin. */
  float uv_v0 = 0.0f;
  float uv_v1 = tex_h / (float)SR_PPU_SURFACE_MAX_HEIGHT;
  /* World height is normalized against the AUTHENTIC 224 lines, not against
   * the captured height -- this is the whole point of the vertical extend.
   * Dividing by tex_h would make the taller capture span the same 1.0 world
   * unit, so the auto-fit below would frame the bigger plane to the same
   * screen height and the only visible effect would be that everything got
   * ~14% SMALLER. Normalizing against 224 instead keeps the authentic band
   * exactly where it was and lets the extra scanlines project past the top
   * edge, into screen space the tilt was previously wasting.
   *
   */
  float height_scale = tex_h / (float)kActRaiserAuthenticHeight;

  float par = 1.0f;
  if (active_pixel_aspect == kPixelAspect_Crt43 && !ignore_aspect_ratio)
    par = 7.0f / 6.0f;
  /* Width is normalized against the same authentic-height reference used by
   * every layer and projection consumer. */
  float aspect_x = (float)snes_width /
      (float)kActRaiserAuthenticHeight * par;
  float vis_half_w =
      0.5f * (float)visible_width / (float)kActRaiserAuthenticHeight * par;

  float screen_aspect = (float)out_w / (float)out_h;
  float tan_half = tanf(kDioramaFovY * 0.5f);
  float fit_h = 0.5f / tan_half;
  float fit_w = vis_half_w / (tan_half * screen_aspect);
  static const float kDioramaZ_Hud = 0.95f;
  g_diorama_auto_distance =
      fmaxf(fit_h, fit_w) * 1.02f + (kDioramaZ_Hud - 0.5f);

  /* B4-split (followup doc): the camera comes from the caller's snapshot
   * (Free Cam: the authored/persisted pose via FrameSlot; Dynamic Cam:
   * present.c's own render camera) instead of reading the game-thread-owned
   * g_diorama_cam directly — see the DioramaCameraPose comment (diorama.h)
   * for why. fov_y stays the fixed camera constant; it was never authored
   * per-mode. */
  DioramaCamera cam = { cam_pose->tilt_x, cam_pose->tilt_y,
                        cam_pose->distance, kDioramaFovY };
  if (cam.distance <= 0.0f) cam.distance = g_diorama_auto_distance;
  /* M5 (followup doc): the descriptor range (0..2000, settings.c) must stay
   * contiguous to cover both the 0 auto-fit sentinel and the real
   * kDioramaDistMin..kDioramaDistMax range, so 1..199 (0.01x..1.99x) is a
   * reachable "dead zone" the range alone can't exclude — a single
   * right-arrow off the default 0 lands at distance=1, inside the near
   * plane (kNear=0.1), clipping the whole scene. Enforce the floor here,
   * at consume time. */
  else if (cam.distance < kDioramaDistMin) cam.distance = kDioramaDistMin;
  /* B4-kick: boost's zoom-punch, applied AFTER the auto-fit/dead-zone
   * resolution above so it composes correctly with the 0 sentinel (see the
   * distance_scale parameter comment, diorama.h). 1.0 = no change. */
  cam.distance *= distance_scale;
  if (cam.distance < kDioramaDistMin) cam.distance = kDioramaDistMin;

  float mvp[16];
  BuildViewProjection(&cam, out_w, out_h, mvp);

  /* Re-center around the AUTHENTIC band. Meshes are symmetric around wy=0,
   * while the captured source may have different numbers of rows above and
   * below the authentic 224. Their difference, not their sum, is the required
   * pin: equal margins naturally remain centred; a top-heavy capture shifts up
   * and a bottom-heavy capture shifts down.
   *
   * Folded into the MVP rather than passed to each mesh builder so that the
   * layers, thickness skirts, depth shapes, shoebox AND
   * Diorama_ProjectCapturedPoint (which projects through this same matrix)
   * cannot disagree about it. Column-major post-multiply by translate(0,d,0):
   * only the last column changes, by d times the second. d is 0 when there is
   * no vertical margin, leaving the matrix untouched. */
  {
    float bottom_rows = tex_h - (float)authentic_y0 -
        (float)kActRaiserAuthenticHeight;
    float pin = 0.5f * ((float)authentic_y0 - bottom_rows) /
        (float)kActRaiserAuthenticHeight;
    float d = DioramaVerticalShift(mvp, height_scale, pin, out_w, out_h);
    if (d != 0.0f)
      for (int r = 0; r < 4; r++)
        mvp[12 + r] += d * mvp[4 + r];
  }

  if (out_projection) {
    memcpy(out_projection->matrix, mvp, sizeof(mvp));
    out_projection->aspect_x = aspect_x;
    out_projection->height_scale = height_scale;
    /* The public point-projection contract takes coordinates relative to the
     * displayed capture. Layer textures carry a hidden resolve apron before
     * that capture, so publish its origin once instead of making every
     * presentation overlay know the surface layout. */
    out_projection->texture_x_origin = obj_apron;
    out_projection->texture_width = SR_PPU_SURFACE_MAX_WIDTH;
    /* The ALLOCATED height, matching texture_width's allocated width, because
     * Diorama_ProjectCapturedPoint divides a texture row by this to get V and
     * the uv_v window above is now expressed in the same allocated space. */
    out_projection->texture_height = SR_PPU_SURFACE_MAX_HEIGHT;
    out_projection->output_x = viewport.x;
    out_projection->output_y = viewport.y;
    out_projection->output_width = out_w;
    out_projection->output_height = out_h;
  }

  /* B6 (followup doc): drawn before the per-layer loop below — painter's
   * algorithm, the box surrounds the stack. */
  if (g_settings.diorama_shoebox) {
    const PresentationOutcome shoebox = DrawDioramaShoebox(
        renderer, mvp, aspect_x, height_scale, cam.tilt_y, out_w, out_h);
    outcome = PresentationOutcome_Combine(outcome, shoebox);
    if (!PresentationOutcome_IsUsable(shoebox)) {
      return DioramaCompositeCoreFailure(renderer, viewport_is_output);
    }
  }

  float shade_mix =
      (float)g_settings.diorama_depth_shade / (float)kPercentScale;

  SDL_Vertex verts[DIORAMA_VERTS_PER_LAYER];
  int indices[DIORAMA_INDICES_PER_LAYER];
  int nv, ni;

  /* Publish the exact authored shape/window of the effect-addressable BG
   * planes and each OBJ priority plane. A current attached effect can retain
   * its transform when the isolated source band has no winning pixels. */
  if (out_projection) {
    for (int i = 0; i < resolved_count; i++) {
      if (resolved[i].alpha == 0) continue;
      const DioramaLayerDesc *layer =
          DioramaDescForPlane(resolved[i].plane);
      if (!DioramaLayerIsProjectable(
              layer, textures, pixels, effect_obj_priority_mask,
              effect_bg_plane_mask))
        continue;
      DioramaPlaneProjection plane = {
        .valid = true,
        .u0 = uv_u0,
        .v0 = uv_v0,
        .u1 = uv_u1,
        .v1 = uv_v1,
        .z_world = resolved[i].z - 0.5f,
        .rake = resolved[i].rake,
        .bow = resolved[i].bow,
      };
      if (resolved[i].plane == SR_PPU_OVERLAY_BG1) {
        out_projection->bg1_plane = plane;
      }
      if (resolved[i].plane == SR_PPU_OVERLAY_BG2) {
        out_projection->bg2_plane = plane;
      }
      if (resolved[i].plane == kDioramaPlane_Bg1Hi) {
        out_projection->bg1_high_plane = plane;
      }
      const int priority = DioramaPlaneObjectPriority(resolved[i].plane);
      if (priority < 0) continue;
      out_projection->object_planes[priority] = plane;
    }
    out_projection->valid = true;
  }

  /* Ordinary scenes retain the single historical painter pass byte-for-byte.
   * A disjoint full-add scene needs three passes: main world, resolved TS
   * addends, then BG3. The PPU capture has already made every additive plane
   * sparse at non-winning pixels, so drawing all addends after the main world
   * reproduces main+sub without letting a low-priority addend be overwritten by
   * a later main plane. BG3 remains last because a non-math HUD winner must
   * occlude (rather than receive) the subscreen contribution. */
  int draw_order[kDioramaLayerCount];
  int draw_count = 0;
  const int blend_pass_count = additive_plane_mask ? 3 : 1;
  for (int blend_pass = 0; blend_pass < blend_pass_count; blend_pass++) {
    for (int i = 0; i < resolved_count; i++) {
      const int plane = resolved[i].plane;
      const bool additive =
          (additive_plane_mask & (1u << (unsigned)plane)) != 0;
      int plane_pass = 0;
      if (additive_plane_mask)
        plane_pass = additive ? 1
            : plane == SR_PPU_OVERLAY_BG3 ? 2 : 0;
      if (plane_pass == blend_pass)
        draw_order[draw_count++] = i;
    }
  }
  for (int draw_index = 0; draw_index < draw_count; draw_index++) {
    const int i = draw_order[draw_index];
    if (resolved[i].alpha == 0) continue;
    const DioramaLayerDesc *layer = DioramaDescForPlane(resolved[i].plane);
    if (!layer) continue;
    const bool is_additive =
        (additive_plane_mask & (1u << (unsigned)layer->plane)) != 0;
    const float layer_z = resolved[i].z;
    const float layer_rake = resolved[i].rake;
    const float layer_bow = resolved[i].bow;
    const float layer_thickness = resolved[i].thickness;
    const float layer_stack = resolved[i].stack;
    const int layer_stack_copies = resolved[i].stack_copies;
    const int layer_stack_dir = resolved[i].stack_direction;
    const bool layer_stack_solid = resolved[i].stack_solid;
    /* A5 (followup doc): with diorama_hud_flat on, BG3 is deliberately not
     * captured as a diorama layer (actraiser_rtl.c) and the anchored flat
     * HUD draws separately (present.c). Skip this entry outright rather
     * than relying on its pixel buffer staying unpopulated — once the
     * buffer has been written at least once (tilted mode was used this
     * session), the pointer stays non-NULL and its last frame's content
     * would otherwise keep drawing as a stale ghost plane. */
    /* B5 (followup doc): "Skybox only" promotes BG2 OUT of the box entirely
     * (drawn above as the enveloping skybox instead) — both priority bands
     * share the same underlying capture/visibility toggle, so both are
     * excluded together. "Plane + skybox" and "Off" leave this loop
     * untouched: BG2 still draws in-box exactly as before. */
    /* B5 follow-up (live report, 2026-07-21): the pre-existing backdrop
     * plane (kDioramaPlane_Backdrop, the full flat-scene residual) sits
     * opaque at z=-0.50, in front of the skybox — at low tilt its projected
     * quad fills nearly the whole frustum, leaving only a thin sliver for
     * the skybox to show through at all. In Skybox-only, BG2 is meant to
     * REPLACE that role entirely (it's now the ENTIRE background, not a
     * margin-filler), so skip backdrop too. Plane+skybox keeps it — there
     * BG2's in-box copy is the main visual and backdrop still backstops any
     * gaps the way it always has. */
    bool is_backdrop = (layer->plane == kDioramaPlane_Backdrop);
    SDL_Texture *texture = textures[layer->plane];
    if (!DioramaLayerIsDrawable(layer, textures, pixels)) {
      /* An isolated BG band can legitimately have no winning pixels at a
       * particular scroll position while an environmental effect remains
       * anchored to that plane. Preserve its painter-order callback and
       * transform without drawing a stale/empty texture. */
      if (DioramaLayerIsProjectable(
              layer, textures, pixels, effect_obj_priority_mask,
              effect_bg_plane_mask)) {
        const PresentationOutcome effect = DioramaSubmitPlaneEffect(
            renderer, plane_effect, plane_effect_userdata, layer->plane,
            out_projection, viewport_is_output, &viewport);
        outcome = PresentationOutcome_Combine(outcome, effect);
        if (!PresentationOutcome_IsUsable(effect)) {
          return DioramaCompositeCoreFailure(renderer, viewport_is_output);
        }
      }
      continue;
    }

    SDL_FColor shade = {
      1.0f + (layer->shade.r - 1.0f) * shade_mix,
      1.0f + (layer->shade.g - 1.0f) * shade_mix,
      1.0f + (layer->shade.b - 1.0f) * shade_mix,
      /* Authored alpha multiplies the layer's built-in shade alpha, so an
       * un-authored plane (255) preserves the built-in value exactly. */
      layer->shade.a * ((float)resolved[i].alpha / 255.0f),
    };

    float z_world = layer_z - 0.5f;
    const float layer_u0 = uv_u0;
    const float layer_u1 = uv_u1;
    const float layer_v0 = uv_v0;
    const float layer_v1 = uv_v1;

    BuildLayerMesh(mvp,
                   z_world, layer_rake, layer_bow, layer_u0, layer_v0,
                   layer_u1, layer_v1,
                   aspect_x, height_scale, out_w, out_h, shade,
                   verts, indices, &nv, &ni);

    /* Sized for the extension plus the host: when the extension is present,
     * both are appended into this one ordered geometry submission below.
     * Extension primitives remain first so the authentic BG2 owns the hidden
     * coplanar overlap without relying on a second draw call. */
    SDL_Vertex extension_verts[DIORAMA_ATTACHED_VERTS];
    int extension_indices[DIORAMA_ATTACHED_INDICES];
    int extension_nv = 0, extension_ni = 0;

    /* The validated `$04/$02-$03:waterfall` token is published only when the
     * exact three-row splash-platform signature is camera-local. Extend both
     * BG2 priority bands, and no other layer/section, by repeating the
     * authentic 224-row waterfall interval.
     *
     * The authentic interval is the only source guaranteed to stay populated
     * under every BG Extents edit. The previous PoC derived both its source and
     * fold from `bg2_valid_spans`; changing a top/bottom extent could therefore
     * move the fold outside the captured 256-row cycle and make the overflow
     * disappear. Folding at the authentic bottom is invariant; the opaque
     * continuation is tucked beneath the host before curling over its edge. */
    const bool aitos_waterfall_extension =
        map_group == kActRaiserMapGroup_Aitos &&
        map_number >= 2 && map_number <= 3 &&
        layer_section == kDioramaLayerSection_AitosWaterfall &&
        (layer->plane == SR_PPU_OVERLAY_BG2 ||
         layer->plane == kDioramaPlane_Bg2Hi ||
         layer->plane == kDioramaPlane_Bg2Far);
    float attached_lower_content_v_max = 0.0f;
    /* AR_AITOS_WATERFALL_LOG=1 draw-side counterpart. Seeded to sentinels so a
     * frame that took an early-out inside the block is distinguishable from one
     * that never entered it at all. */
    float log_fold_t = -1.0f, log_overlap_t = -1.0f;
    int log_drawable_y1 = -1;
    if (aitos_waterfall_extension) {
      DioramaVerticalRepeatPlan repeat;
      const float layer_v_span = layer_v1 - layer_v0;
      if (DioramaVerticalRepeatPlan_Build(
              authentic_y0, kActRaiserAuthenticHeight,
              snes_height, SR_PPU_SURFACE_MAX_HEIGHT, &repeat) &&
          layer_v_span > 0.0f) {
        /* Apply the same bounded sub-tick V shift as the host layer. The shift
         * cancels out of fold_t (geometry stays fixed) but keeps the repeated
         * pixels flowing with the captured waterfall rather than drifting by
         * one emulated tick. */
        const float layer_v_shift = layer_v0 - uv_v0;
        int drawable_y1 = 0;
        if (!DioramaBgValidSpanPlan_DrawableRowBounds(
                bg2_valid_spans, NULL, &drawable_y1) ||
            drawable_y1 <= 0 || drawable_y1 > snes_height) {
          drawable_y1 = snes_height;
        }
        /* The capture can deliberately end in transparent policy rows. The
         * attachment belongs at the final drawable texel, not at the texture
         * rectangle's geometric end; otherwise vertical interpolation samples
         * across that hidden ownership boundary and exposes a colored line. */
        attached_lower_content_v_max =
            (float)drawable_y1 / (float)SR_PPU_SURFACE_MAX_HEIGHT +
            layer_v_shift;
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
        const float fold_t = (fold_v - layer_v0) / layer_v_span;
        const float extension_height =
            (float)repeat.repeat_height / (float)kActRaiserAuthenticHeight;
        if (fold_t >= 0.0f && fold_t <= 1.0f && source_v0 >= 0.0f &&
            source_v1 <= 1.0f) {
          const float extension_y = (0.5f - fold_t) * height_scale;
          const float extension_z = DioramaTiltedRowDepth(
              z_world, layer_rake, layer_bow, fold_t);
          /* Remain coplanar through the host's last DRAWABLE row, then add the
           * tolerance underlap before bending. The dedicated row in
           * BuildFoldedOverflowMesh prevents the first curved triangle from
           * intruding into this interval under camera pitch.
           *
           * Measured in texture rows against `drawable_y1` — the same row the
           * shader hands ownership over at (attached_lower_content_v_max) —
           * rather than as `(1 - fold_t)` of the host mesh. Those are not the
           * same edge: the source crop and mesh geometry can otherwise put the
           * host handoff on different texture rows. Row units are
           * exact for both meshes: each spans 1/224 world units per texture row,
           * so no height_scale factor belongs in this expression at all. */
          float overlap_t =
              (float)(drawable_y1 - repeat.fold_y +
                      kAitosWaterfallSeamTolerancePixels) /
              (float)repeat.repeat_height;
          if (overlap_t < 0.0f) overlap_t = 0.0f;
          if (overlap_t > 1.0f) overlap_t = 1.0f;
          float handoff_t = fold_t +
              overlap_t * extension_height / height_scale;
          if (handoff_t > 1.0f) handoff_t = 1.0f;
          const float handoff_z = DioramaTiltedRowDepth(
              z_world, layer_rake, layer_bow, handoff_t);
          log_fold_t = fold_t;
          log_overlap_t = overlap_t;
          BuildFoldedOverflowMesh(
              mvp, extension_y, extension_z, handoff_z,
              extension_height, overlap_t,
              kShoeboxZFront, kAitosWaterfallFrontDrop,
              layer_u0, source_v0, layer_u1, source_v1,
              aspect_x, out_w, out_h, shade,
              extension_verts, extension_indices,
              &extension_nv, &extension_ni);

          /* Only the low BG2 plane owns the public waterfall projection. The
           * high-priority band uses identical geometry but must not overwrite
           * the atmosphere's established low-plane host. */
          if (extension_nv > 0 && out_projection &&
              layer->plane == SR_PPU_OVERLAY_BG2 &&
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
    if (layer->plane == SR_PPU_OVERLAY_BG2)
      DioramaAitosWaterfallLog(map_group, map_number, layer_section,
                               aitos_waterfall_extension, extension_nv,
                               authentic_y0, snes_height, log_drawable_y1,
                               log_fold_t, log_overlap_t);

    /* STACK: fill the depth gap with PARALLEL copies of the layer, drawn behind
     * the plane itself (back to front, so the painter's algorithm layers them
     * correctly without a depth test).
     *
     * This is the alternative to a rake for the same void, and the reason it
     * exists is that a rake tilts the plane: its rows end up at different
     * depths, so the perspective divide gives one layer two different parallax
     * rates and it shears as the camera moves, over-exaggerating that layer's
     * parallax. Every copy here stays exactly parallel at ONE depth, so each has
     * a single parallax rate and the layer keeps the flat poster-like motion the
     * whole diorama is built on.
     *
     * `dir` chooses which side of the plane gets filled: forward (toward the
     * camera, the default and what Fillmore act 2 needs, since its water sits
     * BEHIND the rock path), backward, or both. Copies are drawn in descending
     * index, which is far-to-near for forward and near-to-far for backward -- for
     * a backward stack that is still correct, because those copies are all behind
     * the plane and the plane is drawn last over them either way. Only `both`
     * genuinely interleaves, and its far half is drawn before its near half.
     *
     * Copy 0 coincides with the plane's own depth for a forward or backward
     * stack, and is skipped -- the plane's own draw below IS that copy, so drawing
     * it twice would double-darken the front face. A `both` stack has no copy at
     * the plane unless the count is odd, so nothing is skipped wrongly; the index-0
     * copy there is the far edge, which must be drawn.
     *
     * Same deliberate exclusions as the skirt: no shadow pass, no DOF/rim shader
     * (they key off a single depth and would be recomputed per copy for no
     * visual gain), and not the backdrop plane. */
    if (layer_stack > 0.0f && layer_stack_copies > 1 && !is_backdrop) {
      int stack_nv = 0, stack_ni = 0;
      SDL_Vertex stack_verts[DIORAMA_VERTS_PER_LAYER];
      int stack_indices[DIORAMA_INDICES_PER_LAYER];
      for (int c = layer_stack_copies - 1; c >= 0; c--) {
        /* Skip whichever copy coincides with the plane's own depth -- index 0 for
         * a one-sided fill, the middle one for an odd-count centred fill. The
         * rule lives in the pure module so it cannot drift from the geometry. */
        if (DioramaStackCopyIsRedundant(c, layer_stack_copies, layer_stack_dir))
          continue;
        float copy_z = z_world, copy_shade = 1.0f, copy_alpha = 1.0f;
        DioramaStackCopyShaped(c, layer_stack_copies, z_world, layer_stack,
                               layer_stack_dir, layer_stack_solid, &copy_z,
                               &copy_shade, &copy_alpha);
        SDL_FColor copy_color = shade;
        copy_color.r *= copy_shade;
        copy_color.g *= copy_shade;
        copy_color.b *= copy_shade;
        copy_color.a *= copy_alpha;
        /* Rake is passed through so a room authoring both keeps every copy on
         * the same tilt rather than mixing tilted and flat slices. */
        BuildLayerMesh(mvp, copy_z, layer_rake, layer_bow, layer_u0, layer_v0,
                       layer_u1, layer_v1, aspect_x, height_scale,
                       out_w, out_h, copy_color,
                       stack_verts, stack_indices, &stack_nv, &stack_ni);
        if (stack_nv > 0) {
          const bool configured = SDL_SetTextureBlendMode(
              texture, is_additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
          RecordOptionalDioramaDraw(
              &outcome,
              configured && RenderDioramaGeometry(
                  renderer, texture, stack_verts, stack_nv,
                  stack_indices, stack_ni));
        }
      }
    }

    /* THICKNESS: the extruded near face, drawn BEFORE the plane itself.
     *
     * Order matters and is not arbitrary. SDL_RenderGeometry has no depth test
     * (see the shoebox comment), so this is painter's algorithm: drawing the
     * skirt first lets the plane's own bottom edge land on top of it, which
     * keeps the fold crisp. Drawn the other way the skirt's top row would
     * overwrite the plane's last scanline and the seam would shimmer as the
     * camera moves.
     *
     * Deliberately plain: no shadow pass (a skirt is the underside of a layer
     * that already cast one, so a second offset copy would double-darken), no
     * DOF/rim shader (both are keyed to a single plane depth and the skirt spans
     * a depth RANGE, so the radius would be wrong along it), and never the
     * supersample path (that is for flat parallel art). Those are the honest
     * limits of extruding a 2D capture, not oversights.
     *
     * `is_backdrop` layers are excluded: the backdrop is the infinite behind-
     * everything fill drawn with BLENDMODE_NONE, so giving it a near face would
     * paint an opaque band across the scene. */
    if (layer_thickness > 0.0f && !is_backdrop) {
      int skirt_nv = 0, skirt_ni = 0;
      SDL_Vertex skirt_verts[DIORAMA_VERTS_PER_LAYER];
      int skirt_indices[DIORAMA_INDICES_PER_LAYER];
      BuildLayerSkirtMesh(mvp, z_world, layer_rake + layer_bow, layer_thickness,
                          layer_u0, layer_u1, layer_v1,
                          aspect_x, height_scale, out_w, out_h, shade,
                          skirt_verts, skirt_indices, &skirt_nv, &skirt_ni);
      if (skirt_nv > 0) {
        const bool configured = SDL_SetTextureBlendMode(
            texture, is_additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
        RecordOptionalDioramaDraw(
            &outcome,
            configured && RenderDioramaGeometry(
                renderer, texture, skirt_verts, skirt_nv,
                skirt_indices, skirt_ni));
      }
    }

    /* Determined up front (before the shadow/main draws) so B1b-crisp knows
     * whether this layer is eligible for the premultiplied supersample path
     * — see the section comment above kDioramaSupersample. */
    bool rim_light = layer->is_figure && RimLightEnabled(renderer);
    bool want_dof = !rim_light &&
        layer->plane != SR_PPU_OVERLAY_BG3 &&
        DofBlurEnabled(renderer);
    float dof_radius = want_dof ? DofRadiusForLayer(layer_z) : 0.0f;
    if (dof_radius < 0.05f) dof_radius = 0.0f;
    bool want_edge = !rim_light &&
        LayerGetsEdgeAA(layer->plane) &&
        EdgeAAEnabled(renderer);
    bool dof_or_edge = !rim_light && (dof_radius > 0.0f || want_edge);
    bool use_shader = rim_light || dof_or_edge;

    const SDL_BlendMode layer_blend = is_backdrop
        ? SDL_BLENDMODE_NONE
        : is_additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND;
    if (!SDL_SetTextureBlendMode(texture, layer_blend)) {
      return DioramaCompositeCoreFailure(renderer, viewport_is_output);
    }

    SDL_Texture *draw_texture = texture;
    bool used_ss = false;
    if (!use_shader) {
      PresentationOutcome supersample_outcome =
          kPresentationOutcome_Complete;
      DioramaPerformanceScope supersample_performance =
          DioramaPerformance_Begin(kDioramaPerformance_Supersample);
      SDL_Texture *ss = BuildDioramaSupersample(
          renderer, texture, obj_apron, snes_width, snes_height,
          &supersample_outcome);
      DioramaPerformance_End(supersample_performance);
      outcome = PresentationOutcome_Combine(outcome, supersample_outcome);
      if (!PresentationOutcome_IsUsable(supersample_outcome)) {
        return DioramaCompositeCoreFailure(renderer, viewport_is_output);
      }
      if (ss) {
        draw_texture = ss;
        used_ss = true;
      }
    }
    if (used_ss) {
      if (!SDL_SetTextureBlendMode(draw_texture, layer_blend)) {
        return DioramaCompositeCoreFailure(renderer, viewport_is_output);
      }
    }

    /* Supersample targets contain only the active capture region, unlike the
     * source textures whose allocation uses the ABI surface maxima. Remap
     * BOTH axes into the compact target so its right/bottom edges are 1.0
     * instead of the live width/height ratios. V used
     * to need no remap because the texture was exactly as tall as its content;
     * the vertical margin ended that. Stack/skirt draws above still use the
     * original source texture and therefore keep the original coordinates. */
    SDL_Vertex ss_verts[DIORAMA_VERTS_PER_LAYER];
    SDL_Vertex *draw_verts = verts;
    if (used_ss) {
      memcpy(ss_verts, verts, (size_t)nv * sizeof(ss_verts[0]));
      /* U is a scale AND an offset now: the supersample target holds the
       * displayed span alone, which begins at surface column obj_apron rather
       * than at 0. A pure scale would map the apron into the target and shift
       * every crisp-path layer right by apron*kDioramaSupersample texels. V
       * needs no offset -- the vertical band's row 0 IS the surface's row 0. */
      RemapMeshToSupersampleTexture(
          ss_verts, nv, obj_apron, snes_width, snes_height);
      if (extension_nv > 0) {
        RemapMeshToSupersampleTexture(
            extension_verts, extension_nv,
            obj_apron, snes_width, snes_height);
      }
      draw_verts = ss_verts;
    }

    if (!is_backdrop && layer->casts_shadow) {
      float off = (float)out_h * 0.004f;
      SDL_Vertex shadow[DIORAMA_VERTS_PER_LAYER];
      memcpy(shadow, draw_verts, (size_t)nv * sizeof(shadow[0]));
      for (int v = 0; v < nv; v++) {
        shadow[v].position.x += off;
        shadow[v].position.y += off;
        shadow[v].color = (SDL_FColor){ 0.0f, 0.0f, 0.0f, 0.35f };
      }
      /* M8/AR_GPU_FX_SHADOW: soften the hard silhouette shadow with a GPU
       * blur. Independently toggleable — bind only for this one draw call,
       * clear immediately after, so nothing else in the frame is affected
       * (falls back silently to the existing hard-edged shadow above if
       * unavailable/disabled/failed to compile). */
      const bool shadow_blur_requested = ShadowBlurEnabled(renderer);
      bool shadow_blur_bound = false;
      if (shadow_blur_requested) {
        BlurUniforms u = {
          1.0f / (float)SR_PPU_SURFACE_MAX_WIDTH,
          1.0f / (float)SR_PPU_SURFACE_MAX_HEIGHT, 3.0f, 0.0f,
        };
        shadow_blur_bound = SDL_SetGPURenderStateFragmentUniforms(
                                g_blur_state, 0, &u, sizeof(u)) &&
            SDL_SetGPURenderState(renderer, g_blur_state);
        if (!shadow_blur_bound) {
          outcome = PresentationOutcome_Combine(
              outcome, kPresentationOutcome_OptionalOmitted);
          if (!SDL_SetGPURenderState(renderer, NULL)) {
            return DioramaCompositeCoreFailure(
                renderer, viewport_is_output);
          }
        }
      }
      RecordOptionalDioramaDraw(
          &outcome,
          RenderDioramaGeometry(
              renderer, draw_texture, shadow, nv, indices, ni));
      if (shadow_blur_bound && !SDL_SetGPURenderState(renderer, NULL)) {
        return DioramaCompositeCoreFailure(renderer, viewport_is_output);
      }
    }

    /* M8/AR_GPU_FX_RIM, AR_GPU_FX_DOF, AR_GPU_FX_EDGEAA: rim_light/want_dof/
     * dof_radius/want_edge/dof_or_edge were already computed above (before
     * the shadow draw) so B1b-crisp's supersample gate could see them. Both
     * DOF and edge-AA are applied TOGETHER in the combined DOF/edge-AA
     * shader (src/shaders/dof_edge.frag.glsl) — neither silently
     * loses to the other. */
    bool layer_shader_bound = false;
    if (rim_light) {
      RimLightUniforms u = {
        1.0f / (float)SR_PPU_SURFACE_MAX_WIDTH,
        1.0f / (float)SR_PPU_SURFACE_MAX_HEIGHT, 0.33f, 0.0f,
      };
      layer_shader_bound = SDL_SetGPURenderStateFragmentUniforms(
                               g_rim_light_state, 0, &u, sizeof(u)) &&
          SDL_SetGPURenderState(renderer, g_rim_light_state);
    } else if (dof_or_edge) {
      /* Feed the shader the exact source window used by this mesh so edge
       * fading and geometry cannot disagree. */
      DofEdgeUniforms u = {
        1.0f / (float)SR_PPU_SURFACE_MAX_WIDTH,
        1.0f / (float)SR_PPU_SURFACE_MAX_HEIGHT, dof_radius,
        layer_u0, layer_u1,
        layer_v0, layer_v1,
        want_edge ? 2.0f : 0.0f,
        /* The opaque waterfall continuation owns this lower edge. Keep its
         * host samples inside captured BG2 and suppress only the already-
         * covered bottom feather; top/left/right AA remains active. */
        attached_lower_content_v_max,
      };
      layer_shader_bound = SDL_SetGPURenderStateFragmentUniforms(
                               g_dofedge_state, 0, &u, sizeof(u)) &&
          SDL_SetGPURenderState(renderer, g_dofedge_state);
    }
    if ((rim_light || dof_or_edge) && !layer_shader_bound) {
      outcome = PresentationOutcome_Combine(
          outcome, kPresentationOutcome_OptionalOmitted);
      if (!SDL_SetGPURenderState(renderer, NULL)) {
        return DioramaCompositeCoreFailure(renderer, viewport_is_output);
      }
    }
    /* Submit the attached waterfall and BG2 as one ordered geometry batch.
     * Auxiliary water remains first and coplanar for two hidden native tile
     * rows; the following host primitives cover it in the same draw. This
     * removes an otherwise unnecessary submission boundary from the seam
     * without pretending the folded 3D mesh can be baked into a 2D texture. */
    bool main_submitted = false;
    if (extension_nv > 0) {
      const int host_vertex_base = extension_nv;
      memcpy(&extension_verts[extension_nv], draw_verts,
             (size_t)nv * sizeof(extension_verts[0]));
      for (int index = 0; index < ni; index++)
        extension_indices[extension_ni + index] =
            host_vertex_base + indices[index];
      extension_nv += nv;
      extension_ni += ni;
      main_submitted = RenderDioramaGeometry(
          renderer, draw_texture, extension_verts,
          extension_nv, extension_indices, extension_ni);
    } else {
      main_submitted = RenderDioramaGeometry(
          renderer, draw_texture, draw_verts, nv, indices, ni);
    }
    if (layer_shader_bound && !SDL_SetGPURenderState(renderer, NULL)) {
      return DioramaCompositeCoreFailure(renderer, viewport_is_output);
    }
    if (!main_submitted) {
      return DioramaCompositeCoreFailure(renderer, viewport_is_output);
    }
    const PresentationOutcome effect = DioramaSubmitPlaneEffect(
        renderer, plane_effect, plane_effect_userdata, layer->plane,
        out_projection, viewport_is_output, &viewport);
    outcome = PresentationOutcome_Combine(outcome, effect);
    if (!PresentationOutcome_IsUsable(effect)) {
      return DioramaCompositeCoreFailure(renderer, viewport_is_output);
    }
  }

  if (!viewport_is_output && !SDL_SetRenderViewport(renderer, NULL))
    return kPresentationOutcome_CoreFailure;

  return outcome;
}

static SDL_GPUDevice *DioramaRendererGpuDevice(SDL_Renderer *renderer) {
  if (!renderer)
    return NULL;
  SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
  return (SDL_GPUDevice *)SDL_GetPointerProperty(
      props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL);
}

static void DioramaReleaseRendererResources(SDL_Renderer *renderer) {
  if (renderer)
    SDL_SetGPURenderState(renderer, NULL);

  SDL_DestroyTexture(g_diorama_ss_texture);
  g_diorama_ss_texture = NULL;
  g_diorama_ss_w = 0;
  g_diorama_ss_h = 0;
  g_diorama_ss_unavailable = false;

  for (int plane = 0; plane < kDioramaPlane_Count; plane++)
    PresentationUploadMirror_Reset(&g_diorama_upload_mirrors[plane]);

  SDL_DestroyTexture(g_rom_skybox.texture);
  g_rom_skybox.texture = NULL;
  SDL_DestroyTexture(g_rom_skybox.art_texture);
  g_rom_skybox.art_texture = NULL;
  g_rom_skybox.composite_valid = false;
  g_rom_skybox.resource_failed = false;

  SDL_GPUDevice *current_device = DioramaRendererGpuDevice(renderer);
  bool same_device = !g_diorama_shader_device ||
      current_device == g_diorama_shader_device;
  /* After a real device replacement the old device owns—and has already
   * invalidated—its states and shaders. Do not feed those stale handles to the
   * new device. An orderly shutdown, and reset backends that retain the same
   * SDL_GPUDevice object, take the explicit release path. */
  if (same_device) {
    SDL_DestroyGPURenderState(g_blur_state);
    SDL_DestroyGPURenderState(g_rim_light_state);
    SDL_DestroyGPURenderState(g_dofedge_state);
  }
  g_blur_state = NULL;
  g_rim_light_state = NULL;
  g_dofedge_state = NULL;

  if (same_device && current_device) {
    if (g_blur_shader)
      SDL_ReleaseGPUShader(current_device, g_blur_shader);
    if (g_rim_light_shader)
      SDL_ReleaseGPUShader(current_device, g_rim_light_shader);
    if (g_dofedge_shader)
      SDL_ReleaseGPUShader(current_device, g_dofedge_shader);
  }
  g_blur_shader = NULL;
  g_rim_light_shader = NULL;
  g_dofedge_shader = NULL;
  g_diorama_shader_device = NULL;
  g_blur_available = false;
  g_rim_light_available = false;
  g_dofedge_available = false;
  g_blur_init_attempted = false;
  g_rim_light_init_attempted = false;
  g_dofedge_init_attempted = false;
}

void Diorama_ResetRendererResources(SDL_Renderer *renderer) {
  DioramaReleaseRendererResources(renderer);
}

void Diorama_Shutdown(SDL_Renderer *renderer) {
  DioramaReleaseRendererResources(renderer);
}
