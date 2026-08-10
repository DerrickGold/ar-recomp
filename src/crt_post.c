#include "crt_post.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_shader_blob.h"
#include "settings.h"

/* Mirrors the uniform block in src/shaders/crt.frag.glsl field-for-field. All
 * scalars on purpose: packing any pair into a vec2 would shift every later
 * member on the GLSL side without a compile error on either. */
typedef struct {
  float output_w, output_h;
  float image_x, image_y, image_w, image_h;
  float scan_lines;
  float scan_columns;
  float curvature;
  float scanline_depth;
  float mask_strength;
  float aberration;
  float bandwidth;
  float vignette;
  float brightness;
} CrtUniforms;

#include "shaders/crt_frag.h"

static const GpuShaderBlobs kCrtBlobs = {
  kCrtFragMSL, kCrtFragMSLSize, kCrtFragSPV, kCrtFragSPVSize
};

/* Control surface: the Video > CRT tab (kSettingCat_Crt). The master toggle is
 * a player row; the seven knobs behind it are developer-only, since their
 * defaults ARE the intended look rather than a performance trade-off. The
 * legacy AR_CRT* environment variables still work — they are declared as the
 * `env` field on each descriptor, so they seed through the normal settings
 * priority chain rather than being read here.
 *
 * Read live every frame (same pattern as the diorama_layer_* toggles) so the
 * knobs move the picture while the menu is open.
 *
 * One mode has no settings row: AR_CRT_PASSTHROUGH=1 redirects through the
 * render target but blits straight back with no shader. That is the Phase 0
 * proof — output must stay byte-identical to the effect being off — and it is
 * a test hook, not something to expose to players. */
typedef enum { kCrtMode_Off, kCrtMode_Passthrough, kCrtMode_Full } CrtMode;

static bool s_passthrough_checked;
static bool s_passthrough;
static bool s_engaged;
/* What Begin decided this frame — End must not re-read the setting, or a
 * toggle landing mid-frame would resolve with a different mode than it
 * engaged with. */
static CrtMode s_engaged_mode;
static SDL_Texture *s_scene;
static int s_scene_w, s_scene_h;
static SDL_GPUShader *s_shader;
static SDL_GPURenderState *s_state;
static SDL_GPUDevice *s_device;
static bool s_shader_attempted;

static CrtMode Mode(void) {
  if (!s_passthrough_checked) {
    s_passthrough_checked = true;
    const char *value = getenv("AR_CRT_PASSTHROUGH");
    s_passthrough = value && value[0] && value[0] != '0';
    if (s_passthrough)
      fprintf(stderr, "[crt] passthrough — render target engaged, shader "
                      "bypassed (plumbing check)\n");
  }
  if (s_passthrough) return kCrtMode_Passthrough;
  return g_settings.crt_enabled ? kCrtMode_Full : kCrtMode_Off;
}

static void ReleaseScene(void) {
  if (!s_scene) return;
  SDL_DestroyTexture(s_scene);
  s_scene = NULL;
  s_scene_w = 0;
  s_scene_h = 0;
}

static bool EnsureScene(SDL_Renderer *renderer, int width, int height) {
  if (s_scene && s_scene_w == width && s_scene_h == height) return true;
  ReleaseScene();
  if (width <= 0 || height <= 0) return false;

  /* Prefer the renderer's own first texture format: hardcoding one risks a
   * full-screen format conversion on every resolve. Falls back to RGBA8888
   * where the property is unavailable. */
  SDL_PixelFormat format = SDL_PIXELFORMAT_RGBA8888;
  const SDL_PixelFormat *formats = (const SDL_PixelFormat *)
      SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),
                             SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER, NULL);
  if (formats && formats[0] != SDL_PIXELFORMAT_UNKNOWN) format = formats[0];

  s_scene = SDL_CreateTexture(renderer, format,
                              SDL_TEXTUREACCESS_TARGET, width, height);
  if (!s_scene) {
    fprintf(stderr, "[crt] scene target %dx%d failed: %s\n", width, height,
            SDL_GetError());
    return false;
  }
  /* Linear: the curved resolve samples between texels, and nearest there
   * produces stair-stepping along the bow. The game's own art is still
   * nearest-sampled when it is drawn INTO this target, so pixels stay crisp
   * everywhere the warp is not actually bending them. */
  SDL_SetTextureScaleMode(s_scene, SDL_SCALEMODE_LINEAR);
  SDL_SetTextureBlendMode(s_scene, SDL_BLENDMODE_NONE);
  s_scene_w = width;
  s_scene_h = height;
  return true;
}

static bool EnsureShader(SDL_Renderer *renderer) {
  if (s_shader_attempted) return s_state != NULL;
  s_shader_attempted = true;

  SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
  s_device = (SDL_GPUDevice *)SDL_GetPointerProperty(
      props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL);
  if (!s_device) {
    fprintf(stderr, "[crt] renderer has no GPU device — CRT disabled (enable "
                    "\"GPU shader effects\" in Graphics settings?)\n");
    return false;
  }

  s_shader = GpuShaderBlob_CreateFragment(s_device, &kCrtBlobs, "CRT", 1, 1);
  if (!s_shader) return false;

  SDL_GPURenderStateCreateInfo info;
  SDL_zero(info);
  info.fragment_shader = s_shader;
  s_state = SDL_CreateGPURenderState(renderer, &info);
  if (!s_state) {
    fprintf(stderr, "[crt] render state creation failed: %s\n", SDL_GetError());
    SDL_ReleaseGPUShader(s_device, s_shader);
    s_shader = NULL;
    return false;
  }

  fprintf(stderr, "[crt] shader ready\n");
  return true;
}

bool CrtPost_Begin(SDL_Renderer *renderer) {
  const CrtMode mode = renderer ? Mode() : kCrtMode_Off;
  if (mode == kCrtMode_Off) {
    /* Switched off at runtime: hand back the scene target rather than leaving
     * a full-window texture (~28MB at 4K) resident for a disabled feature. */
    ReleaseScene();
    return false;
  }

  /* Shader first: a machine without one gets the ORIGINAL path, and checking
   * before allocating avoids reserving the target only to bail. */
  if (mode == kCrtMode_Full && !EnsureShader(renderer)) return false;

  int width = 0, height = 0;
  SDL_GetRenderOutputSize(renderer, &width, &height);
  if (!EnsureScene(renderer, width, height)) return false;

  SDL_SetRenderTarget(renderer, s_scene);
  /* The target carries its own logical presentation state (SDL_render.h: "Each
   * render target has its own logical presentation state"), which is what lets
   * PresentCompositeScene's per-mode logical-presentation handling work
   * unchanged in here. Start from a known state each frame. */
  SDL_SetRenderLogicalPresentation(renderer, 0, 0,
                                   SDL_LOGICAL_PRESENTATION_DISABLED);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);
  s_engaged = true;
  s_engaged_mode = mode;
  return true;
}

/* Where the game picture actually sits inside the scene target.
 *
 * The two present paths letterbox by different mechanisms, so neither source
 * alone is right. Flat mode leaves SDL's logical presentation active and lets
 * SDL do the letterboxing, so SDL itself knows the rect. The 3D paths disable
 * logical presentation and compute their own viewport, which the caller passes
 * in. Ask SDL first, fall back to the caller.
 *
 * MUST be called while the scene target is still bound: logical presentation
 * state is per-target. */
static SDL_Rect ResolveImageRect(SDL_Renderer *renderer, SDL_Rect fallback) {
  int logical_w = 0, logical_h = 0;
  SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
  SDL_FRect rect;
  if (SDL_GetRenderLogicalPresentation(renderer, &logical_w, &logical_h, &mode) &&
      mode != SDL_LOGICAL_PRESENTATION_DISABLED &&
      SDL_GetRenderLogicalPresentationRect(renderer, &rect) &&
      rect.w > 0.0f && rect.h > 0.0f) {
    SDL_Rect resolved = { (int)rect.x, (int)rect.y, (int)rect.w, (int)rect.h };
    return resolved;
  }
  return fallback;
}

SDL_Rect CrtPost_End(SDL_Renderer *renderer,
                     int scan_columns, int scan_lines, SDL_Rect image) {
  if (!s_engaged) return image;
  s_engaged = false;

  /* Before unbinding — this reads the scene target's own presentation state. */
  image = ResolveImageRect(renderer, image);

  SDL_SetRenderTarget(renderer, NULL);

  /* The resolve must cover the whole backbuffer, so logical presentation has to
   * come off for the duration — but it gets PUT BACK.
   *
   * Leaving it disabled is not cosmetic: SDL_RenderReadPixels honours logical
   * presentation, so a screenshot taken with the effect on came back a
   * different SIZE than the same frame with it off (2128 vs 2140 rows here).
   * That silently breaks both the passthrough byte-identical check and the
   * visual A/B harness, neither of which would report anything more useful
   * than "the images differ". More generally, a present-time helper has no
   * business leaving renderer state changed behind it. */
  int logical_w = 0, logical_h = 0;
  SDL_RendererLogicalPresentation logical_mode =
      SDL_LOGICAL_PRESENTATION_DISABLED;
  SDL_GetRenderLogicalPresentation(renderer, &logical_w, &logical_h,
                                   &logical_mode);

  SDL_SetRenderLogicalPresentation(renderer, 0, 0,
                                   SDL_LOGICAL_PRESENTATION_DISABLED);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);

  if (s_engaged_mode == kCrtMode_Full && s_state) {
    /* An empty rect would divide by zero in the shader; the whole target is
     * what an unletterboxed window looks like anyway. */
    if (image.w <= 0 || image.h <= 0) {
      image.x = 0;
      image.y = 0;
      image.w = s_scene_w;
      image.h = s_scene_h;
    }
    CrtUniforms uniforms;
    uniforms.output_w = (float)s_scene_w;
    uniforms.output_h = (float)s_scene_h;
    uniforms.image_x = (float)image.x;
    uniforms.image_y = (float)image.y;
    uniforms.image_w = (float)image.w;
    uniforms.image_h = (float)image.h;
    uniforms.scan_lines = (float)(scan_lines > 0 ? scan_lines : 224);
    uniforms.scan_columns = (float)(scan_columns > 0 ? scan_columns : 256);
    uniforms.curvature = (float)g_settings.crt_curvature_x100 / 100.0f;
    uniforms.scanline_depth = (float)g_settings.crt_scanline_x100 / 100.0f;
    uniforms.mask_strength = (float)g_settings.crt_mask_x100 / 100.0f;
    uniforms.aberration = (float)g_settings.crt_aberration_x100 / 100.0f;
    uniforms.bandwidth = (float)g_settings.crt_bandwidth_x100 / 100.0f;
    uniforms.vignette = (float)g_settings.crt_vignette_x100 / 100.0f;
    uniforms.brightness = (float)g_settings.crt_brightness_x100 / 100.0f;

    SDL_SetGPURenderStateFragmentUniforms(s_state, 0, &uniforms,
                                          (Uint32)sizeof uniforms);
    SDL_SetGPURenderState(renderer, s_state);
  }

  SDL_RenderTexture(renderer, s_scene, NULL, NULL);

  if (s_engaged_mode == kCrtMode_Full && s_state)
    SDL_SetGPURenderState(renderer, NULL);

  SDL_SetRenderLogicalPresentation(renderer, logical_w, logical_h,
                                   logical_mode);
  return image;
}

SDL_Texture *CrtPost_BaseTarget(void) {
  return s_engaged ? s_scene : NULL;
}

/* Called from the host shutdown path. Not strictly required — the OS reclaims
 * everything — but leaving a declared teardown uncalled reads as though cleanup
 * happens when it does not, and it makes leak checkers useful here. */
void CrtPost_Shutdown(void) {
  if (s_state) {
    SDL_DestroyGPURenderState(s_state);
    s_state = NULL;
  }
  if (s_shader && s_device) {
    SDL_ReleaseGPUShader(s_device, s_shader);
    s_shader = NULL;
  }
  ReleaseScene();
  s_engaged = false;
  s_shader_attempted = false;
}
