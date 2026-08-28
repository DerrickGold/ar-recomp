/* Shared handling for the generated shader blobs.
 *
 * Shaders are authored once as GLSL in src/shaders/ and cross-compiled by
 * tools/build_shaders.py into committed headers carrying SPIR-V (Vulkan),
 * MSL (Metal), and DXIL (D3D12). The checked-in blobs keep shader compilers
 * out of ordinary game builds.
 *
 * This header exists so the format-selection rule lives in exactly one place:
 * it is used by SDL effect backends and tests/shader_blob_test.c, and the
 * entrypoint detail below is the kind that fails loudly but confusingly if a
 * copy drifts. */
#ifndef AR_GPU_SHADER_BLOB_H
#define AR_GPU_SHADER_BLOB_H

#include <SDL3/SDL.h>
#include <stdio.h>

typedef struct {
  const unsigned char *msl;
  unsigned int msl_size;
  const unsigned char *spv;
  unsigned int spv_size;
  const unsigned char *dxil;
  unsigned int dxil_size;
} GpuShaderBlobs;

static inline const char *GpuShaderBlob_FormatName(
    SDL_GPUShaderFormat format) {
  switch (format) {
    case SDL_GPU_SHADERFORMAT_SPIRV: return "SPIR-V";
    case SDL_GPU_SHADERFORMAT_DXIL: return "DXIL";
    case SDL_GPU_SHADERFORMAT_MSL: return "MSL";
    default: return "unknown";
  }
}

/* Pick the blob matching whatever the live GPU backend speaks.
 *
 * The entrypoint name differs by format and is NOT cosmetic: glslc emits
 * SPIR-V whose entry point keeps the GLSL name `main`, while spirv-cross
 * renames it to `main0` when it emits Metal. Passing the wrong one fails
 * shader creation outright — tests/shader_blob_test.c asserts exactly that,
 * so the rule stays load-bearing rather than accidental.
 *
 * Returns NULL on any failure, having already reported it. Callers are
 * expected to fall back to their pre-shader path; a missing shader must never
 * be fatal. */
static inline SDL_GPUShader *GpuShaderBlob_Create(
    SDL_GPUDevice *device, const GpuShaderBlobs *blobs, const char *label,
    SDL_GPUShaderStage stage, Uint32 num_samplers,
    Uint32 num_uniform_buffers) {
  const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);

  SDL_GPUShaderCreateInfo info;
  SDL_zero(info);
  if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
    info.code = blobs->spv;
    info.code_size = blobs->spv_size;
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.entrypoint = "main";
  } else if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
    info.code = blobs->dxil;
    info.code_size = blobs->dxil_size;
    info.format = SDL_GPU_SHADERFORMAT_DXIL;
    info.entrypoint = "main";
  } else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
    info.code = blobs->msl;
    info.code_size = blobs->msl_size;
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.entrypoint = "main0";
  } else {
    fprintf(stderr, "[gpu-fx] no shader format in common with this backend "
                    "(it offers 0x%x, we ship SPIR-V + DXIL + MSL) — %s disabled\n",
            (unsigned)formats, label);
    return NULL;
  }
  info.stage = stage;
  info.num_samplers = num_samplers;
  info.num_uniform_buffers = num_uniform_buffers;

  SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);
  if (!shader)
    fprintf(stderr, "[gpu-fx] %s shader compile failed (%s): %s\n", label,
            GpuShaderBlob_FormatName(info.format), SDL_GetError());
  return shader;
}

static inline SDL_GPUShader *GpuShaderBlob_CreateFragment(
    SDL_GPUDevice *device, const GpuShaderBlobs *blobs, const char *label,
    Uint32 num_samplers, Uint32 num_uniform_buffers) {
  return GpuShaderBlob_Create(
      device, blobs, label, SDL_GPU_SHADERSTAGE_FRAGMENT,
      num_samplers, num_uniform_buffers);
}

#endif /* AR_GPU_SHADER_BLOB_H */
