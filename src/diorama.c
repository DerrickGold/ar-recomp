#include "diorama.h"
#include "settings.h"
#include "snes/ppu.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

// #define USE_UPRIGHT_OBJ

/* Codebase convention for boolean env flags (e.g. AR_MXCHECK, main.c):
 * non-null, non-empty, AND not "0" — plain getenv(x)?1:0 wrongly treats
 * FOO=0 the same as FOO=1, since getenv only returns NULL when unset. */
static bool EnvFlagOn(const char *name) {
  const char *v = getenv(name);
  return v && v[0] && v[0] != '0';
}

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
                    "disabled (is AR_GPU_SHADERS=1 set?)\n");
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

/* AR_GPU_FX_SHADOW=1, independent of any other AR_GPU_FX_* toggle (per the
 * session's request to test each effect in isolation). Both this AND
 * AR_GPU_SHADERS=1 (the backend switch, main.c) must be set. */
static bool ShadowBlurEnabled(SDL_Renderer *renderer) {
  static int enabled = -1;
  if (enabled < 0) enabled = EnvFlagOn("AR_GPU_FX_SHADOW") ? 1 : 0;
  if (!enabled) return false;
  EnsureBlurShader(renderer);
  return g_blur_available;
}

/* AR_GPU_FX_DOF=1 (§7.2 depth-of-field): reuses the SAME blur shader as the
 * shadow effect (identical 9-tap box blur), just applied to a layer's MAIN
 * draw with a radius scaled by that layer's distance from the focal plane,
 * instead of a fixed radius on the shadow copy. Independent toggle, same
 * AR_GPU_SHADERS=1 prerequisite. */
static bool DofBlurEnabled(SDL_Renderer *renderer) {
  static int enabled = -1;
  if (enabled < 0) enabled = EnvFlagOn("AR_GPU_FX_DOF") ? 1 : 0;
  if (!enabled) return false;
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
                    "disabled (is AR_GPU_SHADERS=1 set?)\n");
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

/* AR_GPU_FX_RIM=1, independent of AR_GPU_FX_SHADOW (each effect tested in
 * isolation per the session's request). Both this AND AR_GPU_SHADERS=1
 * (the backend switch) must be set. */
static bool RimLightEnabled(SDL_Renderer *renderer) {
  static int enabled = -1;
  if (enabled < 0) enabled = EnvFlagOn("AR_GPU_FX_RIM") ? 1 : 0;
  if (!enabled) return false;
  EnsureRimLightShader(renderer);
  return g_rim_light_available;
}

/* ── Parallax-aware edge AA (AR_GPU_FX_EDGEAA=1) ─────────────────────────
 * Doc §7.2's "parallax-aware anti-aliasing at layer edges." Each BG layer's
 * quad is the same size on the source texture but sits at a different Z, so
 * after perspective projection their outer boundaries (the diorama
 * "shadowbox" side walls) land at slightly different screen positions —
 * the hard rectangular UV cutoff at each layer's edge can look aliased/
 * jaggy at a tilt. This fades alpha to 0 over a few texels near the true
 * UV edge (u_min/u_max/v_min/v_max — NOT assumed to be 0/1, since the
 * widescreen-widened capture only fills [0, snes_width/kPpuBufWidth) of the
 * allocated texture), softening that boundary instead of a hard cut. */

typedef struct {
  float texel_w, texel_h, u_min, u_max, v_min, v_max, feather, _pad;
} EdgeAAUniforms;

static SDL_GPUShader *g_edge_aa_shader;
static SDL_GPURenderState *g_edge_aa_state;
static bool g_edge_aa_init_attempted;
static bool g_edge_aa_available;

static const char kEdgeAAMSL[] =
"#include <metal_stdlib>\n"
"#include <simd/simd.h>\n"
"using namespace metal;\n"
"struct type_Context {\n"
"  float texel_w; float texel_h; float u_min; float u_max;\n"
"  float v_min; float v_max; float feather; float pad0;\n"
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
"  float4 c = u_texture.sample(u_sampler, uv);\n"
"  float du = min(uv.x - Context.u_min, Context.u_max - uv.x);\n"
"  float dv = min(uv.y - Context.v_min, Context.v_max - uv.y);\n"
"  float d = min(du, dv);\n"
"  float texel_avg = (Context.texel_w + Context.texel_h) * 0.5;\n"
"  float fade = clamp(d / (texel_avg * Context.feather), 0.0, 1.0);\n"
"  float4 vc = in.in_var_COLOR0;\n"
"  out.out_var_SV_Target = float4(c.rgb * vc.rgb, c.a * fade * vc.a);\n"
"  return out;\n"
"}\n";

static void EnsureEdgeAAShader(SDL_Renderer *renderer) {
  if (g_edge_aa_init_attempted) return;
  g_edge_aa_init_attempted = true;

  SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
  SDL_GPUDevice *device = (SDL_GPUDevice *)SDL_GetPointerProperty(
      props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL);
  if (!device) {
    fprintf(stderr, "[gpu-fx] renderer has no GPU device — edge AA "
                    "disabled (is AR_GPU_SHADERS=1 set?)\n");
    return;
  }
  SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
  if (!(formats & SDL_GPU_SHADERFORMAT_MSL)) {
    fprintf(stderr, "[gpu-fx] this GPU backend doesn't support MSL "
                    "(formats=0x%x) — edge AA disabled\n",
            (unsigned)formats);
    return;
  }

  SDL_GPUShaderCreateInfo info;
  SDL_zero(info);
  info.code = (const Uint8 *)kEdgeAAMSL;
  info.code_size = sizeof(kEdgeAAMSL) - 1;
  info.entrypoint = "main0";
  info.format = SDL_GPU_SHADERFORMAT_MSL;
  info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  info.num_samplers = 1;
  info.num_uniform_buffers = 1;

  g_edge_aa_shader = SDL_CreateGPUShader(device, &info);
  if (!g_edge_aa_shader) {
    fprintf(stderr, "[gpu-fx] edge AA shader compile failed: %s\n",
            SDL_GetError());
    return;
  }

  SDL_GPURenderStateCreateInfo state_info;
  SDL_zero(state_info);
  state_info.fragment_shader = g_edge_aa_shader;
  g_edge_aa_state = SDL_CreateGPURenderState(renderer, &state_info);
  if (!g_edge_aa_state) {
    fprintf(stderr, "[gpu-fx] edge AA render state creation failed: %s\n",
            SDL_GetError());
    SDL_ReleaseGPUShader(device, g_edge_aa_shader);
    g_edge_aa_shader = NULL;
    return;
  }

  g_edge_aa_available = true;
  fprintf(stderr, "[gpu-fx] edge AA shader ready\n");
}

/* AR_GPU_FX_EDGEAA=1, independent of the other AR_GPU_FX_* toggles. Both
 * this AND AR_GPU_SHADERS=1 (the backend switch) must be set. */
static bool EdgeAAEnabled(SDL_Renderer *renderer) {
  static int enabled = -1;
  if (enabled < 0) enabled = EnvFlagOn("AR_GPU_FX_EDGEAA") ? 1 : 0;
  if (!enabled) return false;
  EnsureEdgeAAShader(renderer);
  return g_edge_aa_available;
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

static DioramaCamera g_diorama_cam = {
  .tilt_x = 0.0f, .tilt_y = -0.18f, .distance = 0.0f, .fov_y = 0.4f
};
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
    "diorama_sprite_upright",
    "diorama_depth_shade",
    "diorama_layer_backdrop",
    "diorama_layer_bg2",
    "diorama_layer_bg1",
    "diorama_layer_obj",
    "diorama_layer_bg3",
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

static void BuildUprightModel(float pitch_undo, float pivot_y, float out[16]) {
  float c = cosf(pitch_undo), s = sinf(pitch_undo);
  float rot[16]  = { 1,0,0,0,  0,c,s,0,  0,-s,c,0,  0,0,0,1 };
  float to[16]   = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,-pivot_y,0,1 };
  float back[16] = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0, pivot_y,0,1 };
  float tmp[16];
  Mat4Mul(back, rot, tmp);
  Mat4Mul(tmp, to, out);
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
      float clip[4];
      clip[0] = mvp[0]*wx + mvp[4]*wy + mvp[8]*z_world  + mvp[12];
      clip[1] = mvp[1]*wx + mvp[5]*wy + mvp[9]*z_world  + mvp[13];
      clip[2] = mvp[2]*wx + mvp[6]*wy + mvp[10]*z_world + mvp[14];
      clip[3] = mvp[3]*wx + mvp[7]*wy + mvp[11]*z_world + mvp[15];
      float inv_w = (clip[3] != 0.0f) ? 1.0f / clip[3] : 1.0f;
      float sx = (clip[0] * inv_w * 0.5f + 0.5f) * screen_w;
      float sy = (1.0f - (clip[1] * inv_w * 0.5f + 0.5f)) * screen_h;
      out_verts[vi].position = (SDL_FPoint){ sx, sy };
      out_verts[vi].tex_coord = (SDL_FPoint){ u0 + s * (u1 - u0),
                                              v0 + t * (v1 - v0) };
      out_verts[vi].color = color;
      vi++;
    }
  }
  *num_verts = vi;
  int ii = 0, cols = DIORAMA_SUBDIV_X + 1;
  for (int row = 0; row < DIORAMA_SUBDIV_Y; row++) {
    for (int col = 0; col < DIORAMA_SUBDIV_X; col++) {
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

/* M7 (§6.1): which SNES BG scroll register (0=BG1..3=BG4) a diorama plane's
 * content follows, or -1 if it isn't scroll-shiftable (the backdrop's
 * meaning is ambiguous outside a single BG, and sprites move via per-OAM
 * position, not a layer scroll register — §6.4 explicitly defers sprite
 * interpolation). Priority-band splits (Bg1Hi/Bg2Hi) follow their parent's
 * scroll, same as they share its Z/shade. */
static int DioramaLayerBgIndex(int plane) {
  switch (plane) {
    case kPpuOverlaySource_Bg1:
    case kDioramaPlane_Bg1Hi: return 0;
    case kPpuOverlaySource_Bg2:
    case kDioramaPlane_Bg2Hi: return 1;
    case kPpuOverlaySource_Bg3: return 2;
    default: return -1;
  }
}

bool Diorama_Composite(SDL_Renderer *renderer, int snes_width, int snes_height,
                       int active_pixel_aspect, bool ignore_aspect_ratio,
                       int visible_width, SDL_Texture *textures[],
                       uint8_t *pixels[],
                       const DioramaScrollDelta *scroll_delta) {
  if (!renderer) return false;

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

  float tex_h = (float)snes_height;
  float uv_u0 = 0.0f;
  float uv_u1 = (float)snes_width / (float)kPpuBufWidth;

  float par = 1.0f;
  if (active_pixel_aspect == kPixelAspect_Crt43 && !ignore_aspect_ratio)
    par = 7.0f / 6.0f;
  float aspect_x = (float)snes_width / tex_h * par;
  float vis_half_w = 0.5f * (float)visible_width / tex_h * par;

  float screen_aspect = (float)out_w / (float)out_h;
  float tan_half = tanf(g_diorama_cam.fov_y * 0.5f);
  float fit_h = 0.5f / tan_half;
  float fit_w = vis_half_w / (tan_half * screen_aspect);
  static const float kDioramaZ_Hud = 0.95f;
  g_diorama_auto_distance =
      fmaxf(fit_h, fit_w) * 1.02f + (kDioramaZ_Hud - 0.5f);

  DioramaCamera cam = g_diorama_cam;
  if (cam.distance <= 0.0f) cam.distance = g_diorama_auto_distance;

  float mvp[16];
  BuildViewProjection(&cam, out_w, out_h, mvp);

  float upright = (float)g_settings.diorama_sprite_upright / 100.0f;
  float shade_mix = (float)g_settings.diorama_depth_shade / 100.0f;
  float upright_mvp[16];
  int disable_fig_adjust = 1;

  #ifdef USE_UPRIGHT_OBJ
  disable_fig_adjust = 0;
  if (upright > 0.0f && cam.tilt_x != 0.0f) {
    float model[16];
    BuildUprightModel(-cam.tilt_x * upright, -0.5f, model);
    Mat4Mul(mvp, model, upright_mvp);
  } else {
    memcpy(upright_mvp, mvp, sizeof(upright_mvp));
  }
  #endif

  SDL_Vertex verts[DIORAMA_VERTS_PER_LAYER];
  int indices[DIORAMA_INDICES_PER_LAYER];
  int nv, ni;

  for (int i = 0; i < kDioramaLayerCount; i++) {
    const DioramaLayerDesc *layer = &kDioramaLayers[i];
    if (layer->visible && !*layer->visible) continue;
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
    int adjust_figures = layer->is_figure && !disable_fig_adjust;
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
    BuildLayerMesh(adjust_figures? upright_mvp : mvp,
                   z_world, uv_u0 + layer_du, 0.0f + layer_dv,
                   uv_u1 + layer_du, 1.0f + layer_dv,
                   aspect_x, out_w, out_h, shade,
                   verts, indices, &nv, &ni);

    SDL_SetTextureBlendMode(texture,
        is_backdrop ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);

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
      SDL_RenderGeometry(renderer, texture, shadow, nv, indices, ni);
      if (shadow_blur)
        SDL_SetGPURenderState(renderer, NULL);
    }

    /* M8/AR_GPU_FX_RIM: edge glow on sprite silhouettes only (doc §7.2).
     * Bound only for this one draw call, sprites only — everything else
     * (backdrop, BG tiles, HUD) renders exactly as before. */
    bool rim_light = layer->is_figure && RimLightEnabled(renderer);

    /* M8/AR_GPU_FX_DOF: depth-of-field, distance-from-focal-plane blur.
     * Never on the HUD (BG3) — UI text must stay legible regardless of
     * camera "focus". Mutually exclusive with rim light per draw call (SDL's
     * custom render state is one shader at a time); in practice this rarely
     * matters since sprites sit near the focal plane already (DofRadiusForLayer
     * returns ~0 for them), so the two effects don't compete for the same
     * layers in the first place. Skips the bind entirely when the computed
     * radius is negligible, to avoid paying for a shader pass with no
     * visible effect. */
    /* M8/AR_GPU_FX_EDGEAA: feather the true UV edge (not assumed 0/1 — see
     * LayerGetsEdgeAA comment) on BG1/BG2/their Hi splits. Takes priority
     * over DOF when both would apply to the same layer (only one custom
     * shader can be bound per draw call); in practice this mostly matters
     * for BG2, where DOF's blur is already small (BG2 sits close to the
     * focal plane), so preferring the edge fade there is a reasonable
     * trade-off rather than a real loss. */
    bool edge_aa = !rim_light && LayerGetsEdgeAA(layer->plane) &&
        EdgeAAEnabled(renderer);

    bool dof_blur = !rim_light && !edge_aa &&
        layer->plane != kPpuOverlaySource_Bg3 && DofBlurEnabled(renderer);
    float dof_radius = dof_blur ? DofRadiusForLayer(layer->z) : 0.0f;
    if (dof_radius < 0.05f) dof_blur = false;

    if (rim_light) {
      RimLightUniforms u = {
        1.0f / (float)kPpuBufWidth, 1.0f / (float)snes_height, 0.33f, 0.0f,
      };
      SDL_SetGPURenderStateFragmentUniforms(g_rim_light_state, 0, &u, sizeof(u));
      SDL_SetGPURenderState(renderer, g_rim_light_state);
    } else if (edge_aa) {
      EdgeAAUniforms u = {
        1.0f / (float)kPpuBufWidth, 1.0f / (float)snes_height,
        uv_u0 + layer_du, uv_u1 + layer_du,
        0.0f + layer_dv, 1.0f + layer_dv,
        2.0f, 0.0f,
      };
      SDL_SetGPURenderStateFragmentUniforms(g_edge_aa_state, 0, &u, sizeof(u));
      SDL_SetGPURenderState(renderer, g_edge_aa_state);
    } else if (dof_blur) {
      BlurUniforms u = {
        1.0f / (float)kPpuBufWidth, 1.0f / (float)snes_height, dof_radius, 0.0f,
      };
      SDL_SetGPURenderStateFragmentUniforms(g_blur_state, 0, &u, sizeof(u));
      SDL_SetGPURenderState(renderer, g_blur_state);
    }
    SDL_RenderGeometry(renderer, texture, verts, nv, indices, ni);
    if (rim_light || edge_aa || dof_blur)
      SDL_SetGPURenderState(renderer, NULL);
  }

  if (interpolating)
    SDL_SetRenderTextureAddressMode(renderer, SDL_TEXTURE_ADDRESS_AUTO,
                                    SDL_TEXTURE_ADDRESS_AUTO);

  return true;
}
