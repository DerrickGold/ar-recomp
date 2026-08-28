#include "crt_post.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "gpu_shader_blob.h"
#include "platform/sdl/render_sdl_internal.h"
#include "session_fatal.h"
#include "shaders/crt_frag.h"

/* Mirrors src/shaders/crt.frag.glsl field-for-field. All scalars on purpose:
 * packing a pair into a vec2 would shift every later GLSL member without a
 * compile error on either side. */
typedef struct CrtUniforms {
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

static const GpuShaderBlobs kCrtBlobs = {
  kCrtFragMSL, kCrtFragMSLSize, kCrtFragSPV, kCrtFragSPVSize,
  kCrtFragDXIL, kCrtFragDXILSize,
};

/* AR_CRT_PASSTHROUGH is a byte-identity plumbing hook: it exercises the scene
 * target and resolve while deliberately bypassing the shader. Player policy
 * and all authored effect values arrive through CrtPostConfig. */
typedef enum CrtMode {
  kCrtMode_Off,
  kCrtMode_Passthrough,
  kCrtMode_Full,
} CrtMode;

static bool s_passthrough_checked;
static bool s_passthrough;
static bool s_engaged;
static CrtMode s_engaged_mode;
static CrtPostConfig s_engaged_config;
static SDL_Renderer *s_renderer;
static SDL_Texture *s_scene;
static int s_scene_w, s_scene_h;
static SDL_GPUShader *s_shader;
static SDL_GPURenderState *s_state;
static SDL_GPUDevice *s_gpu_device;
static bool s_shader_attempted;

static const char *CrtSdlErrorOr(const char *fallback) {
  const char *error = SDL_GetError();
  return error && error[0] ? error : fallback;
}

static CrtMode Mode(const CrtPostConfig *config) {
  if (!s_passthrough_checked) {
    s_passthrough_checked = true;
    const char *value = getenv("AR_CRT_PASSTHROUGH");
    s_passthrough = value && value[0] && value[0] != '0';
    if (s_passthrough)
      fprintf(stderr, "[crt] passthrough - render target engaged, shader "
                      "bypassed (plumbing check)\n");
  }
  if (s_passthrough) return kCrtMode_Passthrough;
  return config && config->enabled ? kCrtMode_Full : kCrtMode_Off;
}

static void ReleaseScene(void) {
  if (!s_scene) return;
  SDL_DestroyTexture(s_scene);
  s_scene = NULL;
  s_scene_w = 0;
  s_scene_h = 0;
}

static bool AcceptRenderer(SDL_Renderer *renderer) {
  if (!renderer) return false;
  if (s_renderer && s_renderer != renderer) {
    SDL_SetError("CRT resources belong to another renderer");
    return false;
  }
  s_renderer = renderer;
  return true;
}

static bool EnsureScene(SDL_Renderer *renderer, int width, int height) {
  if (!AcceptRenderer(renderer)) return false;
  if (s_scene && s_scene_w == width && s_scene_h == height) return true;
  ReleaseScene();
  if (width <= 0 || height <= 0) return false;

  /* Use the renderer's preferred texture format so the fullscreen resolve does
   * not introduce a format conversion. This target remains backend-owned and
   * crosses the public boundary only as an opaque ArRenderTexture handle. */
  SDL_PixelFormat format = SDL_PIXELFORMAT_RGBA8888;
  const SDL_PixelFormat *formats = (const SDL_PixelFormat *)
      SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),
                             SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER, NULL);
  if (formats && formats[0] != SDL_PIXELFORMAT_UNKNOWN) format = formats[0];

  s_scene = SDL_CreateTexture(
      renderer, format, SDL_TEXTUREACCESS_TARGET, width, height);
  if (!s_scene) {
    fprintf(stderr, "[crt] scene target %dx%d failed: %s\n",
            width, height, SDL_GetError());
    return false;
  }
  if (!SDL_SetTextureScaleMode(s_scene, SDL_SCALEMODE_LINEAR) ||
      !SDL_SetTextureBlendMode(s_scene, SDL_BLENDMODE_NONE)) {
    fprintf(stderr, "[crt] scene target setup failed: %s\n", SDL_GetError());
    ReleaseScene();
    return false;
  }
  s_scene_w = width;
  s_scene_h = height;
  return true;
}

static bool EnsureShader(ArRenderDevice *device, SDL_Renderer *renderer) {
  if (!ArRenderCapabilities_Has(
          ArRenderDevice_Capabilities(device),
          kArRenderCapability_CustomShaders)) {
    SDL_SetError("active render backend does not support custom shaders");
    return false;
  }
  if (!AcceptRenderer(renderer)) return false;
  if (s_shader_attempted) return s_state != NULL;
  s_shader_attempted = true;

  SDL_PropertiesID properties = SDL_GetRendererProperties(renderer);
  s_gpu_device = (SDL_GPUDevice *)SDL_GetPointerProperty(
      properties, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL);
  if (!s_gpu_device) {
    fprintf(stderr, "[crt] renderer has no GPU device - CRT disabled "
                    "(enable GPU shader effects in Graphics settings?)\n");
    return false;
  }

  s_shader = GpuShaderBlob_CreateFragment(
      s_gpu_device, &kCrtBlobs, "CRT", 1, 1);
  if (!s_shader) return false;

  SDL_GPURenderStateCreateInfo info;
  SDL_zero(info);
  info.fragment_shader = s_shader;
  s_state = SDL_CreateGPURenderState(renderer, &info);
  if (!s_state) {
    fprintf(stderr, "[crt] render state creation failed: %s\n", SDL_GetError());
    SDL_ReleaseGPUShader(s_gpu_device, s_shader);
    s_shader = NULL;
    return false;
  }
  fprintf(stderr, "[crt] shader ready\n");
  return true;
}

bool CrtPost_Begin(ArRenderDevice *device, const CrtPostConfig *config) {
  const CrtMode mode = Mode(config);
  if (mode == kCrtMode_Off) {
    /* Do not retain a full-window target while the feature is disabled. */
    ReleaseScene();
    s_engaged = false;
    return false;
  }

  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (mode == kCrtMode_Full && !EnsureShader(device, renderer)) {
    SessionFatal_Request(
        "CRT processing was enabled, but the renderer could not create its "
        "GPU shader (%s). Restart with GPU shader effects available, update "
        "your graphics driver, or disable CRT processing.",
        CrtSdlErrorOr("no compatible GPU device"));
    return false;
  }

  int width = 0, height = 0;
  if (!renderer || !SDL_GetRenderOutputSize(renderer, &width, &height) ||
      !EnsureScene(renderer, width, height)) {
    SessionFatal_Request(
        "CRT processing was enabled, but its %dx%d render target could not "
        "be created (%s). Restart the game after checking graphics memory "
        "and driver availability, or disable CRT processing.",
        width, height, CrtSdlErrorOr("invalid output size"));
    return false;
  }

  if (!ArRenderDevice_SetRenderTarget(
          device, ArSdlRenderBackend_BorrowTexture(s_scene))) {
    SessionFatal_Request(
        "CRT processing could not bind its render target (%s). Restart the "
        "game; if this repeats, update your graphics driver or disable CRT "
        "processing.",
        ArRenderDevice_LastError(device));
    return false;
  }
  /* Logical presentation belongs to the active SDL render target. Start the
   * scene target from a known state; individual compositors may then select
   * their established presentation modes. */
  if (!SDL_SetRenderLogicalPresentation(
          renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED) ||
      !SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) ||
      !SDL_RenderClear(renderer)) {
    char error[256];
    snprintf(error, sizeof(error), "%s",
             CrtSdlErrorOr("render-target setup failed"));
    (void)ArRenderDevice_SetRenderTarget(
        device, ArRenderTexture_Invalid());
    SessionFatal_Request(
        "CRT processing could not prepare its render target (%s). Restart "
        "the game; if this repeats, update your graphics driver or disable "
        "CRT processing.", error);
    return false;
  }

  s_engaged = true;
  s_engaged_mode = mode;
  s_engaged_config = config ? *config : (CrtPostConfig){0};
  return true;
}

/* Must run while the scene target is bound because SDL logical presentation
 * state is target-local. Flat mode lets SDL letterbox; 3D callers supply the
 * fallback after disabling logical presentation and calculating a viewport. */
static ArRenderRectI ResolveImageRect(SDL_Renderer *renderer,
                                      ArRenderRectI fallback) {
  int logical_w = 0, logical_h = 0;
  SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
  SDL_FRect rectangle;
  if (SDL_GetRenderLogicalPresentation(
          renderer, &logical_w, &logical_h, &mode) &&
      mode != SDL_LOGICAL_PRESENTATION_DISABLED &&
      SDL_GetRenderLogicalPresentationRect(renderer, &rectangle) &&
      rectangle.w > 0.0f && rectangle.h > 0.0f) {
    return (ArRenderRectI){
      (int)rectangle.x, (int)rectangle.y,
      (int)rectangle.w, (int)rectangle.h,
    };
  }
  return fallback;
}

ArRenderRectI CrtPost_End(ArRenderDevice *device,
                          int scan_columns, int scan_lines,
                          ArRenderRectI image) {
  if (!s_engaged) return image;
  s_engaged = false;

  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (!renderer || renderer != s_renderer) {
    SessionFatal_Request(
        "CRT processing lost its owning render device. Restart the game; if "
        "this repeats, report the graphics backend in use.");
    return image;
  }

  image = ResolveImageRect(renderer, image);
  if (!ArRenderDevice_SetRenderTarget(
          device, ArRenderTexture_Invalid())) {
    SessionFatal_Request(
        "CRT processing could not restore the window render target (%s). "
        "Restart the game; if this repeats, update your graphics driver or "
        "disable CRT processing.", ArRenderDevice_LastError(device));
    return image;
  }

  /* Resolve over the full default output, then restore its logical state so
   * screenshots and post-resolve host UI retain their established geometry. */
  int logical_w = 0, logical_h = 0;
  SDL_RendererLogicalPresentation logical_mode =
      SDL_LOGICAL_PRESENTATION_DISABLED;
  bool resolved = SDL_GetRenderLogicalPresentation(
      renderer, &logical_w, &logical_h, &logical_mode) &&
      SDL_SetRenderLogicalPresentation(
          renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED) &&
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) &&
      SDL_RenderClear(renderer);

  if (s_engaged_mode == kCrtMode_Full && s_state) {
    if (image.w <= 0 || image.h <= 0) {
      image = (ArRenderRectI){0, 0, s_scene_w, s_scene_h};
    }
    const CrtUniforms uniforms = {
      .output_w = (float)s_scene_w,
      .output_h = (float)s_scene_h,
      .image_x = (float)image.x,
      .image_y = (float)image.y,
      .image_w = (float)image.w,
      .image_h = (float)image.h,
      .scan_lines = (float)(scan_lines > 0
          ? scan_lines : kActRaiserAuthenticHeight),
      .scan_columns = (float)(scan_columns > 0
          ? scan_columns : kActRaiserAuthenticWidth),
      .curvature = s_engaged_config.curvature,
      .scanline_depth = s_engaged_config.scanline_depth,
      .mask_strength = s_engaged_config.mask_strength,
      .aberration = s_engaged_config.aberration,
      .bandwidth = s_engaged_config.bandwidth,
      .vignette = s_engaged_config.vignette,
      .brightness = s_engaged_config.brightness,
    };
    resolved = resolved && SDL_SetGPURenderStateFragmentUniforms(
        s_state, 0, &uniforms, (Uint32)sizeof uniforms);
    resolved = resolved && SDL_SetGPURenderState(renderer, s_state);
  }

  resolved = resolved && ArRenderDevice_DrawTexture(
      device, ArSdlRenderBackend_BorrowTexture(s_scene), NULL, NULL);
  if (s_engaged_mode == kCrtMode_Full && s_state)
    resolved = SDL_SetGPURenderState(renderer, NULL) && resolved;
  resolved = SDL_SetRenderLogicalPresentation(
      renderer, logical_w, logical_h, logical_mode) && resolved;
  if (!resolved) {
    SessionFatal_Request(
        "CRT processing could not resolve the completed frame (%s). Restart "
        "the game; if this repeats, update your graphics driver or disable "
        "CRT processing.", CrtSdlErrorOr("frame resolve failed"));
  }
  return image;
}

ArRenderTexture CrtPost_BaseTarget(void) {
  return s_engaged && s_scene
      ? ArSdlRenderBackend_BorrowTexture(s_scene)
      : ArRenderTexture_Invalid();
}

void CrtPost_Shutdown(ArRenderDevice *device) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (s_renderer && renderer != s_renderer) return;
  if (s_state) {
    SDL_DestroyGPURenderState(s_state);
    s_state = NULL;
  }
  if (s_shader && s_gpu_device) {
    SDL_ReleaseGPUShader(s_gpu_device, s_shader);
    s_shader = NULL;
  }
  ReleaseScene();
  s_renderer = NULL;
  s_gpu_device = NULL;
  s_engaged = false;
  s_engaged_mode = kCrtMode_Off;
  s_engaged_config = (CrtPostConfig){0};
  s_shader_attempted = false;
}
