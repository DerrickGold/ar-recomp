#include "smaa_sdl.h"

#include "platform/sdl/render_sdl_internal.h"
#include "shaders/smaa_edges_frag.h"
#include "shaders/smaa_weights_frag.h"
#include "shaders/smaa_blend_frag.h"
#include "../../../third_party/smaa/AreaTex.h"
#include "../../../third_party/smaa/SearchTex.h"

static void ResetTargets(ArSdlSmaa *smaa) {
  if (smaa->blend_pass) SDL_DestroyGPURenderState(smaa->blend_pass);
  SDL_DestroyTexture(smaa->edges);
  SDL_DestroyTexture(smaa->weights);
  smaa->blend_pass = NULL;
  smaa->edges = smaa->weights = NULL;
}

void ArSdlSmaa_Reset(ArSdlRenderBackend *backend) {
  ArSdlSmaa *smaa = &backend->smaa;
  ResetTargets(smaa);
  if (smaa->edge_pass) SDL_DestroyGPURenderState(smaa->edge_pass);
  if (smaa->weight_pass) SDL_DestroyGPURenderState(smaa->weight_pass);
  SDL_DestroyTexture(smaa->area);
  SDL_DestroyTexture(smaa->search);
  if (smaa->sampler) SDL_ReleaseGPUSampler(backend->gpu_device, smaa->sampler);
  *smaa = (ArSdlSmaa){0};
}

static SDL_Texture *LookupTexture(SDL_Renderer *renderer, int width, int height,
    const Uint8 *values, int channels) {
  SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
      SDL_TEXTUREACCESS_STATIC, width, height);
  if (!texture) return NULL;
  Uint8 *pixels = SDL_malloc((size_t)width * height * 4);
  if (pixels) {
    for (int i = 0; i < width * height; ++i) {
      pixels[4 * i] = values[channels * i];
      pixels[4 * i + 1] = channels == 2 ? values[channels * i + 1] : 0;
      pixels[4 * i + 2] = 0;
      pixels[4 * i + 3] = 255;
    }
  }
  const bool uploaded = pixels && SDL_UpdateTexture(texture, NULL, pixels, width * 4);
  SDL_free(pixels);
  if (uploaded) return texture;
  SDL_DestroyTexture(texture);
  return NULL;
}

static SDL_GPURenderState *CreatePass(ArSdlRenderBackend *backend,
    const GpuShaderBlobs *blobs, const char *name, SDL_Texture *const *textures, int count) {
  // Slot zero is supplied by SDL_RenderTexture.
  SDL_GPUTextureSamplerBinding bindings[2];
  for (int i = 0; i < count; ++i) {
    SDL_GPUTexture *texture = SDL_GetPointerProperty(SDL_GetTextureProperties(textures[i]),
        SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, NULL);
    if (!texture) return NULL;
    bindings[i] = (SDL_GPUTextureSamplerBinding){texture, backend->smaa.sampler};
  }
  SDL_GPUShader *shader = ArSdlRenderBackend_GetFragmentShader(
      backend, blobs, name, 1 + count, 1);
  if (!shader) return NULL;
  const SDL_GPURenderStateCreateInfo info = {
    .fragment_shader = shader,
    .num_sampler_bindings = count, .sampler_bindings = count ? bindings : NULL,
  };
  return SDL_CreateGPURenderState(backend->renderer, &info);
}

static bool PreparePasses(ArSdlRenderBackend *backend) {
  ArSdlSmaa *smaa = &backend->smaa;
  if (smaa->unavailable) return false;
  if (smaa->weight_pass) return true;
  static const GpuShaderBlobs edge_shader = {
    kSmaaEdgesFragMSL, kSmaaEdgesFragMSLSize, kSmaaEdgesFragSPV, kSmaaEdgesFragSPVSize,
    kSmaaEdgesFragDXIL, kSmaaEdgesFragDXILSize,
  };
  static const GpuShaderBlobs weight_shader = {
    kSmaaWeightsFragMSL, kSmaaWeightsFragMSLSize, kSmaaWeightsFragSPV, kSmaaWeightsFragSPVSize,
    kSmaaWeightsFragDXIL, kSmaaWeightsFragDXILSize,
  };
  const SDL_GPUSamplerCreateInfo sampler = {
    .min_filter = SDL_GPU_FILTER_LINEAR, .mag_filter = SDL_GPU_FILTER_LINEAR,
    .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
    .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
  };
  smaa->sampler = SDL_CreateGPUSampler(backend->gpu_device, &sampler);
  smaa->area = LookupTexture(backend->renderer, AREATEX_WIDTH, AREATEX_HEIGHT, areaTexBytes, 2);
  smaa->search = LookupTexture(backend->renderer, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, searchTexBytes, 1);
  if (smaa->sampler && smaa->area && smaa->search) {
    SDL_Texture *lookups[] = {smaa->area, smaa->search};
    smaa->edge_pass = CreatePass(backend, &edge_shader, "SMAA edges", NULL, 0);
    if (smaa->edge_pass)
      smaa->weight_pass = CreatePass(backend, &weight_shader, "SMAA weights", lookups, 2);
  }
  if (smaa->weight_pass) return true;
  SDL_Log("[render] SMAA unavailable (%s)", SDL_GetError());
  ArSdlSmaa_Reset(backend);
  smaa->unavailable = true;
  return false;
}

static SDL_Texture *PassTarget(SDL_Renderer *renderer, int width, int height) {
  SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
      SDL_TEXTUREACCESS_TARGET, width, height);
  if (texture && SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE) &&
      SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR))
    return texture;
  SDL_DestroyTexture(texture);
  return NULL;
}

static bool PrepareTargets(ArSdlRenderBackend *backend, int width, int height) {
  ArSdlSmaa *smaa = &backend->smaa;
  if (smaa->width == width && smaa->height == height) return smaa->blend_pass != NULL;
  static const GpuShaderBlobs blend_shader = {
    kSmaaBlendFragMSL, kSmaaBlendFragMSLSize, kSmaaBlendFragSPV, kSmaaBlendFragSPVSize,
    kSmaaBlendFragDXIL, kSmaaBlendFragDXILSize,
  };
  ResetTargets(smaa);
  smaa->width = width;
  smaa->height = height;
  smaa->edges = PassTarget(backend->renderer, width, height);
  smaa->weights = PassTarget(backend->renderer, width, height);
  if (smaa->edges && smaa->weights) {
    SDL_Texture *weights[] = {smaa->weights};
    smaa->blend_pass = CreatePass(backend, &blend_shader, "SMAA blend", weights, 1);
  }
  if (!smaa->blend_pass) {
    SDL_Log("[render] SMAA targets unavailable at %dx%d, using FXAA (%s)",
        width, height, SDL_GetError());
    ResetTargets(smaa);
    return false;
  }
  return true;
}

static ArRenderTargetBeginResult DrawPass(SDL_Renderer *renderer, SDL_GPURenderState *pass,
    const float *uniforms, Uint32 size, SDL_Texture *texture,
    const SDL_FRect *source, const SDL_FRect *destination) {
  const bool bound = SDL_SetGPURenderStateFragmentUniforms(pass, 0, uniforms, size) &&
      SDL_SetGPURenderState(renderer, pass);
  const bool drawn = bound && SDL_RenderTexture(renderer, texture, source, destination);
  if (bound && !SDL_SetGPURenderState(renderer, NULL))
    return kArRenderTargetBegin_StateLost;
  return drawn ? kArRenderTargetBegin_Ready : kArRenderTargetBegin_Omitted;
}

ArRenderTargetBeginResult ArSdlSmaa_Draw(ArSdlRenderBackend *backend, SDL_Texture *scene,
    const SDL_FRect *source, const SDL_FRect *destination) {
  float scene_width, scene_height;
  if (!backend->gpu_device || !source || !destination ||
      !SDL_GetTextureSize(scene, &scene_width, &scene_height) ||
      !(source->x >= 0 && source->y >= 0 && source->w >= 1 && source->h >= 1 &&
        source->w <= scene_width - source->x && source->h <= scene_height - source->y &&
        source->x == SDL_floorf(source->x) && source->y == SDL_floorf(source->y) &&
        source->w == SDL_floorf(source->w) && source->h == SDL_floorf(source->h) &&
        destination->w > 0 && destination->h > 0) ||
      !PreparePasses(backend) || !PrepareTargets(backend, (int)source->w, (int)source->h))
    return kArRenderTargetBegin_Omitted;

  ArSdlSmaa *smaa = &backend->smaa;
  SDL_Renderer *renderer = backend->renderer;
  SDL_Texture *parent = SDL_GetRenderTarget(renderer);
  const float dx = 1.0f / scene_width, dy = 1.0f / scene_height;
  const float edges[] = {dx, dy, scene_width, scene_height,
      (source->x + 0.5f) * dx, (source->y + 0.5f) * dy,
      (source->x + source->w - 0.5f) * dx, (source->y + source->h - 0.5f) * dy};
  const float weights[] = {1.0f / source->w, 1.0f / source->h, source->w, source->h};
  const float blend[] = {weights[0], weights[1], weights[2], weights[3],
      source->x * dx, source->y * dy, source->w * dx, source->h * dy};
  const bool upscale = source->w != destination->w || source->h != destination->h;
  // Both intermediate passes write every pixel, including empty edge weights.
  ArRenderTargetBeginResult result = kArRenderTargetBegin_Omitted;
  if (SDL_SetRenderTarget(renderer, smaa->edges))
    result = DrawPass(renderer, smaa->edge_pass, edges, sizeof(edges), scene, source, NULL);
  if (result == kArRenderTargetBegin_Ready)
    result = SDL_SetRenderTarget(renderer, smaa->weights)
        ? DrawPass(renderer, smaa->weight_pass, weights, sizeof(weights), smaa->edges, NULL, NULL)
        : kArRenderTargetBegin_Omitted;

  // At a reduced resolution, complete neighborhood blending before upscaling.
  // The edge mask is dead after weight calculation, so reuse its target.
  if (result == kArRenderTargetBegin_Ready && upscale)
    result = SDL_SetRenderTarget(renderer, smaa->edges)
        ? DrawPass(renderer, smaa->blend_pass, blend, sizeof(blend), scene, source, NULL)
        : kArRenderTargetBegin_Omitted;
  if (!SDL_SetRenderTarget(renderer, parent)) return kArRenderTargetBegin_StateLost;
  if (result != kArRenderTargetBegin_Ready) return result;
  if (!upscale)
    return DrawPass(renderer, smaa->blend_pass, blend, sizeof(blend), scene, source, destination);
  return SDL_RenderTexture(renderer, smaa->edges, NULL, destination)
      ? kArRenderTargetBegin_Ready : kArRenderTargetBegin_Omitted;
}
