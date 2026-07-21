#include "diorama.h"
#include "settings.h"
#include "snes/ppu.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ── M8 (ar-recomp-threading-impl.md §7, optional GPU shader polish) ────
 *
 * Off by default; requires AR_GPU_SHADERS=1 (main.c, switches the renderer
 * to SDL's "gpu" backend) AND its own AR_GPU_FX_SHADOW=1 toggle, so each
 * effect can be tested in isolation per the session's own request. Any
 * failure (unsupported shader format, compile error, render-state creation
 * failure) logs once and falls back to the existing CPU-side hard shadow —
 * this must never be a hard failure, it's additive polish.
 *
 * The MSL below is hand-written, not the SDL_shadercross-generated kind the
 * doc's §7.1 sketch implied — no working example of SDL_CreateGPURenderState
 * existed anywhere I could find for this brand-new (SDL 3.4.0) API, so the
 * exact binding convention (vertex color at `[[user(locn0)]]`, texcoord at
 * `[[user(locn1)]]`, current draw texture auto-bound at `[[texture(0)]]`/
 * `[[sampler(0)]]`, a custom uniform buffer at `[[buffer(0)]]`) was reverse
 * engineered from SDL's own compiled test shaders
 * (test/testgpurender_effects_{grayscale,CRT}.frag.msl.h in the SDL repo,
 * decoded back from their xxd -i byte arrays) rather than from any
 * documentation. Verify against a newer SDL if this ever stops compiling. */

typedef struct { float texel_w, texel_h, radius, _pad; } BlurUniforms;

static SDL_GPUShader *g_blur_shader;
static SDL_GPURenderState *g_blur_state;
static bool g_blur_init_attempted;
static bool g_blur_available;

/* 3x3 weighted-box blur (9 taps, center weighted x2) as a cheap Gaussian
 * approximation — softens the existing hard-edged silhouette shadow into a
 * soft drop shadow (doc §7.2: "each layer casts a soft shadow on the one
 * behind it"). Samples the SAME texture/UVs the CPU path already uses;
 * vertex color (the existing black+alpha tint) is preserved by the final
 * multiply, so this is purely additive over the existing effect. */
static const char kBlurMSL[] =
"#include <metal_stdlib>\n"
"#include <simd/simd.h>\n"
"using namespace metal;\n"
"struct type_Context { float2 texel; float radius; float pad0; };\n"
"struct main0_out { float4 out_var_SV_Target [[color(0)]]; };\n"
"struct main0_in {\n"
"  float4 in_var_COLOR0 [[user(locn0)]];\n"
"  float2 in_var_TEXCOORD0 [[user(locn1)]];\n"
"};\n"
"fragment main0_out main0(main0_in in [[stage_in]],\n"
"    constant type_Context& Context [[buffer(0)]],\n"
"    texture2d<float> u_texture [[texture(0)]],\n"
"    sampler u_sampler [[sampler(0)]]) {\n"
"  main0_out out = {};\n"
"  float2 texel = Context.texel * Context.radius;\n"
"  float2 uv = in.in_var_TEXCOORD0;\n"
"  float4 sum = float4(0.0);\n"
"  sum += u_texture.sample(u_sampler, uv + float2(-texel.x, -texel.y));\n"
"  sum += u_texture.sample(u_sampler, uv + float2( 0.0,     -texel.y));\n"
"  sum += u_texture.sample(u_sampler, uv + float2( texel.x, -texel.y));\n"
"  sum += u_texture.sample(u_sampler, uv + float2(-texel.x,  0.0));\n"
"  sum += u_texture.sample(u_sampler, uv) * 2.0;\n"
"  sum += u_texture.sample(u_sampler, uv + float2( texel.x,  0.0));\n"
"  sum += u_texture.sample(u_sampler, uv + float2(-texel.x,  texel.y));\n"
"  sum += u_texture.sample(u_sampler, uv + float2( 0.0,      texel.y));\n"
"  sum += u_texture.sample(u_sampler, uv + float2( texel.x,  texel.y));\n"
"  out.out_var_SV_Target = (sum / 10.0) * in.in_var_COLOR0;\n"
"  return out;\n"
"}\n";

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
  SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
  if (!(formats & SDL_GPU_SHADERFORMAT_MSL)) {
    fprintf(stderr, "[gpu-fx] this GPU backend doesn't support MSL "
                    "(formats=0x%x) — blur effects disabled\n",
            (unsigned)formats);
    return;
  }

  SDL_GPUShaderCreateInfo info;
  SDL_zero(info);
  info.code = (const Uint8 *)kBlurMSL;
  info.code_size = sizeof(kBlurMSL) - 1;
  info.entrypoint = "main0";
  info.format = SDL_GPU_SHADERFORMAT_MSL;
  info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  info.num_samplers = 1;
  info.num_uniform_buffers = 1;

  g_blur_shader = SDL_CreateGPUShader(device, &info);
  if (!g_blur_shader) {
    fprintf(stderr, "[gpu-fx] blur shader compile failed: %s\n",
            SDL_GetError());
    return;
  }

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

static const char kRimLightMSL[] =
"#include <metal_stdlib>\n"
"#include <simd/simd.h>\n"
"using namespace metal;\n"
"struct type_Context { float2 texel; float strength; float pad0; };\n"
"struct main0_out { float4 out_var_SV_Target [[color(0)]]; };\n"
"struct main0_in {\n"
"  float4 in_var_COLOR0 [[user(locn0)]];\n"
"  float2 in_var_TEXCOORD0 [[user(locn1)]];\n"
"};\n"
"fragment main0_out main0(main0_in in [[stage_in]],\n"
"    constant type_Context& Context [[buffer(0)]],\n"
"    texture2d<float> u_texture [[texture(0)]],\n"
"    sampler u_sampler [[sampler(0)]]) {\n"
"  main0_out out = {};\n"
"  float2 uv = in.in_var_TEXCOORD0;\n"
"  float2 tx = Context.texel;\n"
"  float4 c = u_texture.sample(u_sampler, uv);\n"
"  float a_up    = u_texture.sample(u_sampler, uv + float2(0.0, -tx.y)).a;\n"
"  float a_down  = u_texture.sample(u_sampler, uv + float2(0.0,  tx.y)).a;\n"
"  float a_left  = u_texture.sample(u_sampler, uv + float2(-tx.x, 0.0)).a;\n"
"  float a_right = u_texture.sample(u_sampler, uv + float2( tx.x, 0.0)).a;\n"
"  float min_neighbor = min(min(a_up, a_down), min(a_left, a_right));\n"
"  float edge = c.a * max(0.0, c.a - min_neighbor);\n"
"  float3 rim_color = float3(1.0, 0.95, 0.7);\n"
"  float3 glow = c.rgb + rim_color * (edge * Context.strength);\n"
"  float4 vc = in.in_var_COLOR0;\n"
"  out.out_var_SV_Target = float4(glow * vc.rgb, c.a * vc.a);\n"
"  return out;\n"
"}\n";

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
  SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
  if (!(formats & SDL_GPU_SHADERFORMAT_MSL)) {
    fprintf(stderr, "[gpu-fx] this GPU backend doesn't support MSL "
                    "(formats=0x%x) — rim light disabled\n",
            (unsigned)formats);
    return;
  }

  SDL_GPUShaderCreateInfo info;
  SDL_zero(info);
  info.code = (const Uint8 *)kRimLightMSL;
  info.code_size = sizeof(kRimLightMSL) - 1;
  info.entrypoint = "main0";
  info.format = SDL_GPU_SHADERFORMAT_MSL;
  info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  info.num_samplers = 1;
  info.num_uniform_buffers = 1;

  g_rim_light_shader = SDL_CreateGPUShader(device, &info);
  if (!g_rim_light_shader) {
    fprintf(stderr, "[gpu-fx] rim light shader compile failed: %s\n",
            SDL_GetError());
    return;
  }

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
  float edge_feather, _pad;
} DofEdgeUniforms;

static SDL_GPUShader *g_dofedge_shader;
static SDL_GPURenderState *g_dofedge_state;
static bool g_dofedge_init_attempted;
static bool g_dofedge_available;

static const char kDofEdgeMSL[] =
"#include <metal_stdlib>\n"
"#include <simd/simd.h>\n"
"using namespace metal;\n"
"struct type_Context {\n"
"  float texel_w; float texel_h; float blur_radius;\n"
"  float u_min; float u_max; float v_min; float v_max;\n"
"  float edge_feather; float pad0;\n"
"};\n"
"struct main0_out { float4 out_var_SV_Target [[color(0)]]; };\n"
"struct main0_in {\n"
"  float4 in_var_COLOR0 [[user(locn0)]];\n"
"  float2 in_var_TEXCOORD0 [[user(locn1)]];\n"
"};\n"
"fragment main0_out main0(main0_in in [[stage_in]],\n"
"    constant type_Context& Context [[buffer(0)]],\n"
"    texture2d<float> u_texture [[texture(0)]],\n"
"    sampler u_sampler [[sampler(0)]]) {\n"
"  main0_out out = {};\n"
"  float2 uv = in.in_var_TEXCOORD0;\n"
"  float2 texel = float2(Context.texel_w, Context.texel_h) * Context.blur_radius;\n"
"  float4 sum = float4(0.0);\n"
"  sum += u_texture.sample(u_sampler, uv + float2(-texel.x, -texel.y));\n"
"  sum += u_texture.sample(u_sampler, uv + float2( 0.0,     -texel.y));\n"
"  sum += u_texture.sample(u_sampler, uv + float2( texel.x, -texel.y));\n"
"  sum += u_texture.sample(u_sampler, uv + float2(-texel.x,  0.0));\n"
"  sum += u_texture.sample(u_sampler, uv) * 2.0;\n"
"  sum += u_texture.sample(u_sampler, uv + float2( texel.x,  0.0));\n"
"  sum += u_texture.sample(u_sampler, uv + float2(-texel.x,  texel.y));\n"
"  sum += u_texture.sample(u_sampler, uv + float2( 0.0,      texel.y));\n"
"  sum += u_texture.sample(u_sampler, uv + float2( texel.x,  texel.y));\n"
"  float4 c = sum / 10.0;\n"
"  float fade = 1.0;\n"
"  if (Context.edge_feather > 0.0) {\n"
"    float du = min(uv.x - Context.u_min, Context.u_max - uv.x);\n"
"    float dv = min(uv.y - Context.v_min, Context.v_max - uv.y);\n"
"    float d = min(du, dv);\n"
"    float texel_avg = (Context.texel_w + Context.texel_h) * 0.5;\n"
"    fade = clamp(d / (texel_avg * Context.edge_feather), 0.0, 1.0);\n"
"  }\n"
"  float4 vc = in.in_var_COLOR0;\n"
"  out.out_var_SV_Target = float4(c.rgb * vc.rgb, c.a * fade * vc.a);\n"
"  return out;\n"
"}\n";

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
  SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
  if (!(formats & SDL_GPU_SHADERFORMAT_MSL)) {
    fprintf(stderr, "[gpu-fx] this GPU backend doesn't support MSL "
                    "(formats=0x%x) — DOF/edge AA disabled\n",
            (unsigned)formats);
    return;
  }

  SDL_GPUShaderCreateInfo info;
  SDL_zero(info);
  info.code = (const Uint8 *)kDofEdgeMSL;
  info.code_size = sizeof(kDofEdgeMSL) - 1;
  info.entrypoint = "main0";
  info.format = SDL_GPU_SHADERFORMAT_MSL;
  info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  info.num_samplers = 1;
  info.num_uniform_buffers = 1;

  g_dofedge_shader = SDL_CreateGPUShader(device, &info);
  if (!g_dofedge_shader) {
    fprintf(stderr, "[gpu-fx] DOF/edge AA shader compile failed: %s\n",
            SDL_GetError());
    return;
  }

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
    case kPpuOverlaySource_Bg1:
    case kPpuOverlaySource_Bg2:
    case kDioramaPlane_Bg1Hi:
    case kDioramaPlane_Bg2Hi:
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
 * kHdMode7Scale=4 supersample scale, main.c:105/present.c:42), then sample
 * THAT with LINEAR for the actual tilt+shift draw — the intermediate is 4
 * whole texels per source texel, so LINEAR there interpolates smoothly
 * instead of stepping.
 *
 * Compositing the source (straight alpha) onto a transparent-black-cleared
 * intermediate with plain SDL_BLENDMODE_BLEND is a cheap, shader-free way to
 * premultiply: dstRGB = srcRGB*srcA + 0*(1-srcA) = srcRGB*srcA, dstA = srcA.
 * Premultiplying matters because the LINEAR sample blends across texel
 * boundaries — with straight alpha, blending a fully-transparent black texel
 * against an opaque colored one drags black into the result (a dark fringe);
 * premultiplied RGB is already zero wherever alpha is zero, so the blend
 * only ever mixes real color. The final draw then uses
 * SDL_BLENDMODE_BLEND_PREMULTIPLIED to composite that premultiplied result
 * onto the screen correctly.
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

static SDL_Texture *EnsureDioramaSupersampleTexture(SDL_Renderer *renderer,
                                                     int w, int h) {
  if (g_diorama_ss_texture && g_diorama_ss_w == w && g_diorama_ss_h == h)
    return g_diorama_ss_texture;
  if (g_diorama_ss_texture) SDL_DestroyTexture(g_diorama_ss_texture);
  g_diorama_ss_texture = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
  g_diorama_ss_w = w;
  g_diorama_ss_h = h;
  if (g_diorama_ss_texture)
    SDL_SetTextureScaleMode(g_diorama_ss_texture, SDL_SCALEMODE_LINEAR);
  return g_diorama_ss_texture;
}

/* Renders `source` (a full kPpuBufWidth x snes_height layer texture, already
 * NEAREST-scaled) into the shared ×4 intermediate, premultiplied per the
 * comment above. Caller must have already set `source`'s blend mode to
 * SDL_BLENDMODE_BLEND. Returns NULL (caller falls back to `source`) if the
 * intermediate couldn't be (re)created.
 *
 * Live report (2026-07-21): a thin magenta/garbage-colored line was visible
 * at the diorama's right edge whenever a layer used this path (most
 * noticeable on the near-fullscreen backdrop plane) — present even with NO
 * interpolation shift active, so it wasn't the B1b UV-window bug. Root
 * cause: this used to blit the WHOLE source texture (`SDL_RenderTexture(...,
 * NULL, NULL)`) into the WHOLE intermediate, which faithfully copies
 * source's uninitialized tail (columns snes_width..kPpuBufWidth-1 — see the
 * B1b UV-window comment below for why that tail exists at all) into the
 * intermediate too. The final draw's LINEAR sample at the exact valid/
 * invalid boundary (u=uv_u1) then blends the last real texel against that
 * garbage, every frame, for every crisp-path layer — B1b-crisp switched
 * this path from NEAREST (no cross-texel blending, so this boundary was
 * never sampled softly) to LINEAR, which is what actually exposed it. Fixed
 * by blitting only the VALID `{0,0,snes_width,snes_height}` source sub-rect
 * into the correspondingly-scaled sub-rect of the intermediate — the
 * remainder stays the transparent-black clear from above, so the LINEAR
 * boundary blend fades toward transparent/black instead of garbage. */
static SDL_Texture *BuildDioramaSupersample(SDL_Renderer *renderer,
                                            SDL_Texture *source,
                                            int snes_width, int snes_height) {
  SDL_Texture *ss = EnsureDioramaSupersampleTexture(
      renderer, kPpuBufWidth * kDioramaSupersample,
      snes_height * kDioramaSupersample);
  if (!ss) return NULL;
  SDL_SetRenderTarget(renderer, ss);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
  SDL_RenderClear(renderer);
  SDL_FRect src = { 0.0f, 0.0f, (float)snes_width, (float)snes_height };
  SDL_FRect dst = { 0.0f, 0.0f, (float)(snes_width * kDioramaSupersample),
                    (float)(snes_height * kDioramaSupersample) };
  SDL_RenderTexture(renderer, source, &src, &dst);
  SDL_SetRenderTarget(renderer, NULL);
  return ss;
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

typedef struct DioramaCamera {
  float tilt_x;
  float tilt_y;
  float distance;
  float fov_y;
} DioramaCamera;

/* A3 (followup doc): zero-init, not a hand-tuned literal — every field here
 * is unconditionally overwritten by Diorama_SeedCameraFromSettings (below)
 * before first render (boot, camera-row menu edits, and Reset Camera all
 * call it), so the settings descriptors are the single source of truth for
 * the actual defaults. A literal here would be dead weight a future editor
 * could mistake for load-bearing. */
static DioramaCamera g_diorama_cam;
static float g_diorama_auto_distance = 5.0f;
static bool g_diorama_settings_dirty;
static uint64_t g_diorama_settings_dirty_at;
static bool s_diorama_dragging;

bool Diorama_IsDragging(void)          { return s_diorama_dragging; }
void Diorama_SetDragging(bool dragging) { s_diorama_dragging = dragging; }

static float Clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

/* ── Camera operations ───────────────────────────────────────────────── */

void Diorama_SeedCameraFromSettings(void) {
  g_diorama_cam.tilt_x = (float)g_settings.diorama_tilt_x_mrad / 1000.0f;
  g_diorama_cam.tilt_y = (float)g_settings.diorama_tilt_y_mrad / 1000.0f;
  g_diorama_cam.distance = (float)g_settings.diorama_distance_x100 / 100.0f;
  g_diorama_cam.fov_y = kDioramaFovY;
}

void Diorama_AdjustCamera(float d_yaw, float d_pitch, float d_zoom) {
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
  g_settings.diorama_tilt_x_mrad = (int)(g_diorama_cam.tilt_x * 1000.0f);
  g_settings.diorama_tilt_y_mrad = (int)(g_diorama_cam.tilt_y * 1000.0f);
  g_settings.diorama_distance_x100 = (int)(g_diorama_cam.distance * 100.0f);
  g_diorama_settings_dirty = true;
  g_diorama_settings_dirty_at = SDL_GetTicks();
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
  Diorama_SeedCameraFromSettings();
  g_diorama_settings_dirty = true;
  g_diorama_settings_dirty_at = SDL_GetTicks();
}

void Diorama_FlushSettingsIfDirty(void) {
  if (g_diorama_settings_dirty && !s_diorama_dragging &&
      SDL_GetTicks() - g_diorama_settings_dirty_at > 500) {
    g_diorama_settings_dirty = false;
    if (!Settings_Save("settings.ini"))
      fprintf(stderr, "[diorama] failed to persist camera settings\n");
  }
}

/* ── Layer table ─────────────────────────────────────────────────────── */

typedef struct DioramaLayerDesc {
  int plane;          /* kDioramaPlane_* / kPpuOverlaySource_* index */
  float z;
  SDL_FColor shade;
  bool *visible;
  bool is_figure;
  bool casts_shadow;  /* see kDioramaLayers comment: false for BG2/BG2Hi */
} DioramaLayerDesc;

/* Table order IS the draw order (painter's algorithm) and mirrors the SNES
 * Mode-1 priority stack exactly (the z-rank table in ppu.c
 * PpuDrawBackgrounds), so occlusion matches hardware: priority-1 tiles cover
 * priority-2 sprites, priority-0/1 sprites hide behind the playfield, and so
 * on. The world z carries only parallax — each priority band shares its
 * parent layer's depth (and all four sprite bands share one depth) so a
 * layer never parallax-splits against itself. BG3 stays one plane: ActRaiser
 * action HUDs ride the $2105 quirk rank in front of everything.
 *
 * casts_shadow=false for BG2/BG2Hi: BG2 is drawn right after the backdrop
 * (nothing but sky/distant scenery behind it), so its shadow would land
 * squarely on the sky — visually nonsensical (confirmed live: a large soft
 * blurred shadow next to the moon in a night scene, M8 GPU shadow-blur
 * testing). BG1/BG1Hi and sprites keep casting shadows onto whatever's
 * behind them (BG2, the ground, each other). */
static const DioramaLayerDesc kDioramaLayers[] = {
  { kDioramaPlane_Backdrop, 0.00f, { 0.70f, 0.70f, 0.80f, 1.0f },
    &g_settings.diorama_layer_backdrop, false, false },
  { kPpuOverlaySource_Obj,  0.51f, { 1.0f,  1.0f,  1.0f,  1.0f },   /* prio 0 */
    &g_settings.diorama_layer_obj, true, true },
  { kDioramaPlane_Obj1,     0.51f, { 1.0f,  1.0f,  1.0f,  1.0f },
    &g_settings.diorama_layer_obj, true, true },
  { kPpuOverlaySource_Bg2,  0.20f, { 0.82f, 0.82f, 0.88f, 1.0f },   /* prio 0 */
    &g_settings.diorama_layer_bg2, false, false },
  { kPpuOverlaySource_Bg1,  0.50f, { 0.92f, 0.92f, 0.95f, 1.0f },   /* prio 0 */
    &g_settings.diorama_layer_bg1, false, true },
  { kDioramaPlane_Obj2,     0.51f, { 1.0f,  1.0f,  1.0f,  1.0f },
    &g_settings.diorama_layer_obj, true, true },
  { kDioramaPlane_Bg2Hi,    0.21f, { 0.82f, 0.82f, 0.88f, 1.0f },
    &g_settings.diorama_layer_bg2, false, false },
  { kDioramaPlane_Bg1Hi,    0.51f, { 0.92f, 0.92f, 0.95f, 1.0f },
    &g_settings.diorama_layer_bg1, false, true },
  { kDioramaPlane_Obj3,     0.52f, { 1.0f,  1.0f,  1.0f,  1.0f },
    &g_settings.diorama_layer_obj, true, true },
  { kPpuOverlaySource_Bg3,  0.95f, { 1.0f,  1.0f,  1.0f,  1.0f },
    &g_settings.diorama_layer_bg3, false, true },
};
static const int kDioramaLayerCount =
    (int)(sizeof(kDioramaLayers) / sizeof(kDioramaLayers[0]));

/* ── 3D projection ───────────────────────────────────────────────────── */

#define DIORAMA_SUBDIV_X 8
#define DIORAMA_SUBDIV_Y 6
#define DIORAMA_VERTS_PER_LAYER ((DIORAMA_SUBDIV_X + 1) * (DIORAMA_SUBDIV_Y + 1))
#define DIORAMA_INDICES_PER_LAYER (DIORAMA_SUBDIV_X * DIORAMA_SUBDIV_Y * 6)

static void Mat4Mul(const float a[16], const float b[16], float out[16]) {
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++) {
      float s = 0.0f;
      for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = s;
    }
}

static void BuildViewProjection(const DioramaCamera *cam, int out_w, int out_h,
                                float out_mat[16]) {
  const float kNear = 0.1f, kFar = 100.0f;
  float aspect = (out_h > 0) ? (float)out_w / (float)out_h : 1.0f;
  float f = 1.0f / tanf(cam->fov_y * 0.5f);
  float proj[16] = {
    f / aspect, 0, 0,                                    0,
    0,          f, 0,                                    0,
    0,          0, (kFar + kNear) / (kNear - kFar),     -1,
    0,          0, (2 * kFar * kNear) / (kNear - kFar),  0,
  };
  float cx = cosf(cam->tilt_x), sx = sinf(cam->tilt_x);
  float cy = cosf(cam->tilt_y), sy = sinf(cam->tilt_y);
  float rotY[16]  = { cy,0,sy,0,  0,1,0,0,  -sy,0,cy,0,  0,0,0,1 };
  float rotX[16]  = { 1,0,0,0,  0,cx,sx,0,  0,-sx,cx,0,  0,0,0,1 };
  float trans[16] = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,-cam->distance,1 };
  float rot[16], view[16];
  Mat4Mul(rotX, rotY, rot);
  Mat4Mul(trans, rot, view);
  Mat4Mul(proj, view, out_mat);
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
static SDL_FPoint ProjectWorldPoint(const float mvp[16], float x, float y, float z,
                                    int screen_w, int screen_h) {
  float clip[4];
  clip[0] = mvp[0]*x + mvp[4]*y + mvp[8]*z  + mvp[12];
  clip[1] = mvp[1]*x + mvp[5]*y + mvp[9]*z  + mvp[13];
  clip[2] = mvp[2]*x + mvp[6]*y + mvp[10]*z + mvp[14];
  clip[3] = mvp[3]*x + mvp[7]*y + mvp[11]*z + mvp[15];
  float inv_w = (clip[3] != 0.0f) ? 1.0f / clip[3] : 1.0f;
  float sx = (clip[0] * inv_w * 0.5f + 0.5f) * screen_w;
  float sy = (1.0f - (clip[1] * inv_w * 0.5f + 0.5f)) * screen_h;
  return (SDL_FPoint){ sx, sy };
}

/* Shared triangulation: a (subdiv_u+1)x(subdiv_v+1) vertex grid into
 * subdiv_u*subdiv_v quads, each split into 2 triangles. Identical to what
 * BuildLayerMesh always did (diorama.c, pre-GEO) — factored out verbatim so
 * BuildQuadMesh doesn't duplicate it. */
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

static void BuildLayerMesh(const float mvp[16], float z_world,
                           float u0, float v0, float u1, float v1,
                           float aspect_x, int screen_w, int screen_h,
                           SDL_FColor color,
                           SDL_Vertex *out_verts, int *out_indices,
                           int *num_verts, int *num_indices) {
  int vi = 0;
  for (int row = 0; row <= DIORAMA_SUBDIV_Y; row++) {
    for (int col = 0; col <= DIORAMA_SUBDIV_X; col++) {
      float s = (float)col / DIORAMA_SUBDIV_X;
      float t = (float)row / DIORAMA_SUBDIV_Y;
      float wx = (s - 0.5f) * aspect_x;
      float wy = 0.5f - t;
      out_verts[vi].position =
          ProjectWorldPoint(mvp, wx, wy, z_world, screen_w, screen_h);
      out_verts[vi].tex_coord = (SDL_FPoint){ u0 + s * (u1 - u0),
                                              v0 + t * (v1 - v0) };
      out_verts[vi].color = color;
      vi++;
    }
  }
  *num_verts = vi;
  TriangulateGrid(DIORAMA_SUBDIV_X, DIORAMA_SUBDIV_Y, out_indices, num_indices);
}

/* GEO (followup doc): general world-space quad mesh builder, lerped from a
 * corner + two edge vectors — B5's viewport-fill skybox quad and B6's
 * floor/ceiling/side-wall quads (which vary axis pairs BuildLayerMesh can't:
 * it hardcodes a constant z_world and only varies X/Y) will call this. DEAD
 * CODE until then (GEO is a pure factor-out checkpoint; near-wall culling
 * and the actual wall geometry are B6's job, not this one) — kept
 * deliberately separate from BuildLayerMesh's own formula rather than
 * routing BuildLayerMesh through it, since the two aren't bit-identical
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
  int vi = 0;
  for (int row = 0; row <= subdiv_v; row++) {
    for (int col = 0; col <= subdiv_u; col++) {
      float s = (float)col / subdiv_u;
      float t = (float)row / subdiv_v;
      float wx = origin_x + s * edge_u_x + t * edge_v_x;
      float wy = origin_y + s * edge_u_y + t * edge_v_y;
      float wz = origin_z + s * edge_u_z + t * edge_v_z;
      out_verts[vi].position =
          ProjectWorldPoint(mvp, wx, wy, wz, screen_w, screen_h);
      out_verts[vi].tex_coord = (SDL_FPoint){ u0 + s * (u1 - u0),
                                              v0 + t * (v1 - v0) };
      out_verts[vi].color = color;
      vi++;
    }
  }
  *num_verts = vi;
  TriangulateGrid(subdiv_u, subdiv_v, out_indices, num_indices);
}

/* ── Render ───────────────────────────────────────────────────────────── */

/* M5 (D6/buffer-ownership split): upload is separated from composite so the
 * present thread can release the game thread (safe to redraw pixels[]) right
 * after this returns, instead of after the full composite+vsync-present. */
void Diorama_Upload(SDL_Texture *textures[], uint8_t *pixels[],
                    int snes_width, int snes_height) {
  SDL_Rect upload = { 0, 0, snes_width, snes_height };
  for (int i = 0; i < kDioramaLayerCount; i++) {
    int plane = kDioramaLayers[i].plane;
    if (textures[plane] && pixels[plane])
      SDL_UpdateTexture(textures[plane], &upload, pixels[plane],
                        snes_width * 4);
  }
}

/* M7 (§6.1)/B1b (followup doc): which base-camera delta (0=BG1, 1=BG2) a
 * diorama plane's content follows, or -1 if it isn't scroll-shiftable (the
 * backdrop's meaning is ambiguous outside a single BG). Priority-band splits
 * (Bg1Hi/Bg2Hi) follow their parent's scroll, same as they share its
 * Z/shade. BG3 has no WRAM camera (index 2 stays zero in DioramaScrollDelta
 * — it's UI, not world content), so it isn't listed here at all; it simply
 * never interpolates.
 *
 * B1b rule: OBJ planes ride the BG1 base-camera delta. §6.4 originally
 * deferred sprite interpolation entirely (returned -1) — left that way,
 * sprites (including the player standing on BG1's platforms) would step at
 * 60fps while the world glides at >60fps, the exact relative-judder artifact
 * B1's rejected "exclude HDMA layers" non-fix was ruled out for, but now on
 * the most eye-tracked object on screen. Sprite screen positions already
 * embed the camera (screen = world − camera), so shifting the OBJ plane by
 * the interpolated BG1 camera delta keeps sprites rigidly attached to the
 * gliding world; their own world-space animation still refreshes at 60fps —
 * the same acceptable residual as HDMA raster detail (see B1's ceiling
 * note). */
static int DioramaLayerBgIndex(int plane) {
  switch (plane) {
    case kPpuOverlaySource_Bg1:
    case kDioramaPlane_Bg1Hi: return 0;
    case kPpuOverlaySource_Bg2:
    case kDioramaPlane_Bg2Hi: return 1;
    case kPpuOverlaySource_Obj:
    case kDioramaPlane_Obj1:
    case kDioramaPlane_Obj2:
    case kDioramaPlane_Obj3: return 0;
    default: return -1;
  }
}

/* KNOWN LIMITATION (live report + investigated, 2026-07-21, not fixed): near
 * a level's start/end, the captured BG2 content this draws goes black at
 * the world-bound edge instead of extending — visible as a black wedge
 * clipping the skybox. Root cause: the widescreen margin ceiling
 * (extraLeftCur/extraRightCur, set once per frame in
 * ActRaiser_ApplyWidescreenPolicy from BG1's world position,
 * actraiser_rtl.c ~908-925) is a single GLOBAL PPU value, not per-layer —
 * every layer's scanline rendering respects the same ceiling. The existing
 * per-layer knobs (wsLayerClamp/wsLayerMirror/wsLayerRepeat, consumed by
 * PpuLayerExtra, ppu.c ~419) can only SHRINK a layer's margin down to 0 from
 * that ceiling; nothing lets one layer draw further than it. So there is no
 * cheap fix here — BG2 can't be given a wider margin than BG1's world bound
 * without either (a) a new per-layer NUMERIC margin ceiling in
 * PpuLayerExtra (touches a hot per-scanline path in core PPU rendering), or
 * (b) a second BG2-only scanout pass per frame just for this capture, run
 * with the ceiling forced to the full budget + mirror/repeat, separate from
 * the main frame. Author's call: (a) is the preferred direction (more
 * performant — no extra scanout pass) but deferred as its own follow-up,
 * not part of B5. */

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
static void DrawDioramaSkybox(SDL_Renderer *renderer, SDL_Texture *bg2_texture,
                              int snes_width, int snes_height,
                              int out_w, int out_h, bool dim,
                              float blur_radius) {
  if (!bg2_texture) return;
  float uv_u1 = (float)snes_width / (float)kPpuBufWidth;
  /* Same live report: a visible lighter/garbage-colored strip appeared at
   * the screen's right edge. Root cause: the blur shader samples texels up
   * to `radius` away from each fragment (kBlurMSL, this file's top-of-file
   * comment) — for fragments right at u=uv_u1 (this quad's edge, since
   * uv_u1 < 1.0 is the true boundary of what Diorama_Upload ever wrote,
   * kPpuBufWidth vs the widescreen capture's max width — the same class of
   * bug B1b's UV-window clamp fixed for the tilted layers), the rightward
   * samples reach past uv_u1 into that same uninitialized texture memory.
   * Unlike B1b's interpolation shift (which the tilted layers' own address
   * mode could clamp), the blur shader has no knowledge of uv_u1 to clamp
   * against, so the fix here is simpler: never SAMPLE that close to either
   * edge in the first place — inset the mapped UV range by a texel margin
   * comfortably larger than the blur's reach (this also keeps the LEFT
   * edge's leftward samples at u>0, so no explicit CLAMP addressing is
   * needed here — deliberately not touched, since the caller,
   * Diorama_Composite, is mid-sequence managing that mode itself for its
   * own interpolation clamp around the per-layer loop that runs after this
   * returns). Costs an imperceptible crop of the sky content, not a
   * rendering defect. */
  float margin_u = (blur_radius + 1.0f) / (float)kPpuBufWidth;
  float u0 = margin_u, u1 = uv_u1 - margin_u;
  if (u1 < u0) u1 = u0;  /* degenerate guard for a pathologically narrow capture */
  /* Live report (2026-07-21): {0.30,0.30,0.40} read as jarringly dark for
   * Plane+skybox — the intent is a subtle cue that this is background, not
   * a heavy tint. Lightened substantially; still a touch cool/blue like the
   * rest of the per-layer shade table. */
  static const SDL_FColor kSkyboxDim = { 0.78f, 0.78f, 0.85f, 1.0f };
  static const SDL_FColor kSkyboxFull = { 1.0f, 1.0f, 1.0f, 1.0f };
  SDL_FColor tint = dim ? kSkyboxDim : kSkyboxFull;
  SDL_Vertex verts[4] = {
    { { 0.0f, 0.0f },                   tint, { u0, 0.0f } },
    { { (float)out_w, 0.0f },           tint, { u1, 0.0f } },
    { { (float)out_w, (float)out_h },   tint, { u1, 1.0f } },
    { { 0.0f, (float)out_h },           tint, { u0, 1.0f } },
  };
  int indices[6] = { 0, 1, 2, 0, 2, 3 };
  SDL_SetTextureBlendMode(bg2_texture, SDL_BLENDMODE_NONE);
  bool blur = SkyboxBlurEnabled(renderer);
  if (blur) {
    BlurUniforms u = {
      1.0f / (float)kPpuBufWidth, 1.0f / (float)snes_height,
      blur_radius, 0.0f,
    };
    SDL_SetGPURenderStateFragmentUniforms(g_blur_state, 0, &u, sizeof(u));
    SDL_SetGPURenderState(renderer, g_blur_state);
  }
  SDL_RenderGeometry(renderer, bg2_texture, verts, 4, indices, 6);
  if (blur) SDL_SetGPURenderState(renderer, NULL);
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
/* Crossfade band (rad) around tilt_y=0 — see the near-wall comment below. */
static const float kShoeboxWallFadeRange = 0.15f;

static void DrawDioramaShoebox(SDL_Renderer *renderer, const float mvp[16],
                               float aspect_x, float tilt_y,
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
   * (±0.7 rad) without needing per-angle math. */
  static const float kShoeboxOverscan = 2.0f;
  float hx = 0.5f * aspect_x * kShoeboxOverscan;
  float half_y = 0.5f * kShoeboxOverscan;
  float z_span = kShoeboxZFront - kShoeboxZBack;
  SDL_Vertex verts[4];
  int indices[6];
  int nv, ni;

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

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
  SDL_RenderGeometry(renderer, NULL, verts, nv, indices, ni);

  BuildQuadMesh(mvp, -hx, half_y, kShoeboxZBack,
               2.0f * hx, 0.0f, 0.0f,
               0.0f, 0.0f, z_span,
               0.0f, 0.0f, 1.0f, 1.0f, 1, 1, out_w, out_h, kShoeboxColor,
               verts, indices, &nv, &ni);
  SDL_RenderGeometry(renderer, NULL, verts, nv, indices, ni);

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
    SDL_RenderGeometry(renderer, NULL, verts, nv, indices, ni);
  }
  if (alpha_pos_x > 0.01f) {
    SDL_FColor c = kShoeboxColor;
    c.a *= alpha_pos_x;
    BuildQuadMesh(mvp, hx, -half_y, kShoeboxZBack,
                 0.0f, 0.0f, z_span,
                 0.0f, 2.0f * half_y, 0.0f,
                 0.0f, 0.0f, 1.0f, 1.0f, 1, 1, out_w, out_h, c,
                 verts, indices, &nv, &ni);
    SDL_RenderGeometry(renderer, NULL, verts, nv, indices, ni);
  }
}

bool Diorama_Composite(SDL_Renderer *renderer, int snes_width, int snes_height,
                       int active_pixel_aspect, bool ignore_aspect_ratio,
                       int visible_width, SDL_Texture *textures[],
                       uint8_t *pixels[],
                       const DioramaScrollDelta *scroll_delta,
                       const DioramaCameraPose *cam_pose,
                       float distance_scale) {
  if (!renderer || !cam_pose) return false;

  SDL_SetRenderLogicalPresentation(renderer, 0, 0,
                                   SDL_LOGICAL_PRESENTATION_DISABLED);
  int out_w = 0, out_h = 0;
  SDL_GetRenderOutputSize(renderer, &out_w, &out_h);
  if (out_w <= 0 || out_h <= 0) return false;

  bool interpolating = scroll_delta && scroll_delta->active;
  /* §6.4: SDL_RenderGeometry's default SDL_TEXTURE_ADDRESS_AUTO wraps UVs
   * outside [0,1] for power-of-two textures — shifting UVs to fake sub-frame
   * scroll would wrap the opposite (possibly opaque) edge into view. Clamp
   * instead, scoped to just this composite pass (restored below before
   * returning). */
  if (interpolating)
    SDL_SetRenderTextureAddressMode(renderer, SDL_TEXTURE_ADDRESS_CLAMP,
                                    SDL_TEXTURE_ADDRESS_CLAMP);

  SDL_SetRenderDrawColor(renderer, 20, 20, 30, 255);
  SDL_RenderClear(renderer);

  /* B5 (followup doc): drawn before the per-layer loop below — painter's
   * algorithm, skybox is the farthest thing in the scene. Same
   * pixels[]-populated guard the per-layer loop uses below, so a stale
   * texture from a prior session doesn't draw when BG2 isn't actually
   * captured this frame.
   *
   * Live report (2026-07-21): Skybox-only wants noticeably LESS blur than
   * Plane+skybox — it's the entire visible background there (no sharper
   * in-box copy to contrast against), so the same heavy blur just reads as
   * "broken," not atmospheric. */
  static const float kSkyboxBlurRadiusOnly = 1.0f;
  static const float kSkyboxBlurRadiusBoth = 3.0f;
  if (g_settings.diorama_skybox != kDioramaSky_Off &&
      pixels[kPpuOverlaySource_Bg2]) {
    bool both = g_settings.diorama_skybox == kDioramaSky_Both;
    DrawDioramaSkybox(renderer, textures[kPpuOverlaySource_Bg2],
                      snes_width, snes_height, out_w, out_h, both,
                      both ? kSkyboxBlurRadiusBoth : kSkyboxBlurRadiusOnly);
  }

  float tex_h = (float)snes_height;
  float uv_u0 = 0.0f;
  float uv_u1 = (float)snes_width / (float)kPpuBufWidth;

  float par = 1.0f;
  if (active_pixel_aspect == kPixelAspect_Crt43 && !ignore_aspect_ratio)
    par = 7.0f / 6.0f;
  float aspect_x = (float)snes_width / tex_h * par;
  float vis_half_w = 0.5f * (float)visible_width / tex_h * par;

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

  /* B6 (followup doc): drawn before the per-layer loop below — painter's
   * algorithm, the box surrounds the stack. */
  if (g_settings.diorama_shoebox)
    DrawDioramaShoebox(renderer, mvp, aspect_x, cam.tilt_y, out_w, out_h);

  float shade_mix = (float)g_settings.diorama_depth_shade / 100.0f;

  SDL_Vertex verts[DIORAMA_VERTS_PER_LAYER];
  int indices[DIORAMA_INDICES_PER_LAYER];
  int nv, ni;

  for (int i = 0; i < kDioramaLayerCount; i++) {
    const DioramaLayerDesc *layer = &kDioramaLayers[i];
    if (layer->visible && !*layer->visible) continue;
    /* A5 (followup doc): with diorama_hud_flat on, BG3 is deliberately not
     * captured as a diorama layer (actraiser_rtl.c) and the anchored flat
     * HUD draws separately (present.c). Skip this entry outright rather
     * than relying on its pixel buffer staying unpopulated — once the
     * buffer has been written at least once (tilted mode was used this
     * session), the pointer stays non-NULL and its last frame's content
     * would otherwise keep drawing as a stale ghost plane. */
    if (layer->plane == kPpuOverlaySource_Bg3 && g_settings.diorama_hud_flat)
      continue;
    /* B5 (followup doc): "Skybox only" promotes BG2 OUT of the box entirely
     * (drawn above as the enveloping skybox instead) — both priority bands
     * share the same underlying capture/visibility toggle, so both are
     * excluded together. "Plane + skybox" and "Off" leave this loop
     * untouched: BG2 still draws in-box exactly as before. */
    if ((layer->plane == kPpuOverlaySource_Bg2 ||
         layer->plane == kDioramaPlane_Bg2Hi) &&
        g_settings.diorama_skybox == kDioramaSky_Only)
      continue;
    /* B5 follow-up (live report, 2026-07-21): the pre-existing backdrop
     * plane (kDioramaPlane_Backdrop, the full flat-scene residual) sits
     * opaque at z=-0.50, in front of the skybox — at low tilt its projected
     * quad fills nearly the whole frustum, leaving only a thin sliver for
     * the skybox to show through at all. In Skybox-only, BG2 is meant to
     * REPLACE that role entirely (it's now the ENTIRE background, not a
     * margin-filler), so skip backdrop too. Plane+skybox keeps it — there
     * BG2's in-box copy is the main visual and backdrop still backstops any
     * gaps the way it always has. */
    if (layer->plane == kDioramaPlane_Backdrop &&
        g_settings.diorama_skybox == kDioramaSky_Only)
      continue;
    bool is_backdrop = (layer->plane == kDioramaPlane_Backdrop);
    SDL_Texture *texture = textures[layer->plane];
    if (!texture || !pixels[layer->plane]) continue;

    SDL_FColor shade = {
      1.0f + (layer->shade.r - 1.0f) * shade_mix,
      1.0f + (layer->shade.g - 1.0f) * shade_mix,
      1.0f + (layer->shade.b - 1.0f) * shade_mix,
      layer->shade.a,
    };

    float z_world = layer->z - 0.5f;
    /* M7/§6.2-6.3: shift this layer's UV window by its BG's interpolated
     * sub-tick scroll delta. Each layer uses its OWN BG's delta (parallax:
     * BG2 typically scrolls slower than BG1), so the differing per-layer
     * motion stays visible at >60Hz instead of being flattened to one
     * whole-frame shift. */
    float layer_du = 0.0f, layer_dv = 0.0f;
    if (interpolating) {
      int bg = DioramaLayerBgIndex(layer->plane);
      if (bg >= 0) {
        layer_du = scroll_delta->bg_du[bg];
        layer_dv = scroll_delta->bg_dv[bg];
      }
    }
    float layer_u0 = uv_u0 + layer_du, layer_u1 = uv_u1 + layer_du;
    /* B1b (followup doc) follow-up: the diorama capture only ever fills
     * [uv_u0, uv_u1] of the texture — the sliver beyond it, up to the
     * texture's true width (kPpuBufWidth vs the diorama capture's max width,
     * capped at kWsExtraMax=95 per side by the SNES OAM-wrap hard limit,
     * one short of kPpuExtraLeftRight=96 — widescreen.h), is genuinely never
     * written (SDL streaming textures have undefined initial content, not
     * zeroed). SDL_TEXTURE_ADDRESS_CLAMP (above) only guards against going
     * outside [0,1] of the TEXTURE — it does nothing for a coordinate that's
     * inside [0,1] but past the CAPTURED sub-region, so an interpolation
     * shift large enough to push u1 past uv_u1 sampled that uninitialized
     * memory directly: a garbage-colored strip at the tilted plane's edge,
     * only visible once real camera panning started moving the window (B1b's
     * source fix made that motion real for the first time — the old
     * HDMA-residue source rarely produced a clean directional shift).
     * Clamp the WINDOW POSITION (both edges together, preserving width — no
     * visual squish) rather than the shift itself, so smoothing is
     * untouched except for a large single-tick delta at the exact screen
     * position where the buffer's true edge is in view. */
    if (layer_u1 > uv_u1) {
      float excess = layer_u1 - uv_u1;
      layer_u1 -= excess; layer_u0 -= excess;
    } else if (layer_u0 < uv_u0) {
      float excess = uv_u0 - layer_u0;
      layer_u0 += excess; layer_u1 += excess;
    }
    BuildLayerMesh(mvp,
                   z_world, layer_u0, 0.0f + layer_dv,
                   layer_u1, 1.0f + layer_dv,
                   aspect_x, out_w, out_h, shade,
                   verts, indices, &nv, &ni);

    /* Determined up front (before the shadow/main draws) so B1b-crisp knows
     * whether this layer is eligible for the premultiplied supersample path
     * — see the section comment above kDioramaSupersample. */
    bool rim_light = layer->is_figure && RimLightEnabled(renderer);
    bool want_dof = !rim_light && layer->plane != kPpuOverlaySource_Bg3 &&
        DofBlurEnabled(renderer);
    float dof_radius = want_dof ? DofRadiusForLayer(layer->z) : 0.0f;
    if (dof_radius < 0.05f) dof_radius = 0.0f;
    bool want_edge = !rim_light && LayerGetsEdgeAA(layer->plane) &&
        EdgeAAEnabled(renderer);
    bool dof_or_edge = !rim_light && (dof_radius > 0.0f || want_edge);
    bool use_shader = rim_light || dof_or_edge;

    SDL_SetTextureBlendMode(texture,
        is_backdrop ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);

    SDL_Texture *draw_texture = texture;
    bool used_ss = false;
    if (!use_shader) {
      SDL_Texture *ss =
          BuildDioramaSupersample(renderer, texture, snes_width, snes_height);
      if (ss) { draw_texture = ss; used_ss = true; }
    }
    if (used_ss) {
      SDL_SetTextureBlendMode(draw_texture,
          is_backdrop ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    }

    if (!is_backdrop && layer->casts_shadow) {
      float off = (float)out_h * 0.004f;
      SDL_Vertex shadow[DIORAMA_VERTS_PER_LAYER];
      memcpy(shadow, verts, (size_t)nv * sizeof(shadow[0]));
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
      bool shadow_blur = ShadowBlurEnabled(renderer);
      if (shadow_blur) {
        BlurUniforms u = {
          1.0f / (float)kPpuBufWidth, 1.0f / (float)snes_height, 3.0f, 0.0f,
        };
        SDL_SetGPURenderStateFragmentUniforms(g_blur_state, 0, &u, sizeof(u));
        SDL_SetGPURenderState(renderer, g_blur_state);
      }
      SDL_RenderGeometry(renderer, draw_texture, shadow, nv, indices, ni);
      if (shadow_blur)
        SDL_SetGPURenderState(renderer, NULL);
    }

    /* M8/AR_GPU_FX_RIM, AR_GPU_FX_DOF, AR_GPU_FX_EDGEAA: rim_light/want_dof/
     * dof_radius/want_edge/dof_or_edge were already computed above (before
     * the shadow draw) so B1b-crisp's supersample gate could see them. Both
     * DOF and edge-AA are applied TOGETHER in the combined DOF/edge-AA
     * shader (see the section comment above kDofEdgeMSL) — neither silently
     * loses to the other. */
    if (rim_light) {
      RimLightUniforms u = {
        1.0f / (float)kPpuBufWidth, 1.0f / (float)snes_height, 0.33f, 0.0f,
      };
      SDL_SetGPURenderStateFragmentUniforms(g_rim_light_state, 0, &u, sizeof(u));
      SDL_SetGPURenderState(renderer, g_rim_light_state);
    } else if (dof_or_edge) {
      DofEdgeUniforms u = {
        1.0f / (float)kPpuBufWidth, 1.0f / (float)snes_height, dof_radius,
        uv_u0 + layer_du, uv_u1 + layer_du,
        0.0f + layer_dv, 1.0f + layer_dv,
        want_edge ? 2.0f : 0.0f, 0.0f,
      };
      SDL_SetGPURenderStateFragmentUniforms(g_dofedge_state, 0, &u, sizeof(u));
      SDL_SetGPURenderState(renderer, g_dofedge_state);
    }
    SDL_RenderGeometry(renderer, draw_texture, verts, nv, indices, ni);
    if (rim_light || dof_or_edge)
      SDL_SetGPURenderState(renderer, NULL);
  }

  if (interpolating)
    SDL_SetRenderTextureAddressMode(renderer, SDL_TEXTURE_ADDRESS_AUTO,
                                    SDL_TEXTURE_ADDRESS_AUTO);

  return true;
}
