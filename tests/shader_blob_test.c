/* shader_blob_test.c — guards the generated-shader distribution path.
 *
 * The GPU effects were hand-written MSL, so they compiled on Metal and were
 * silently dead everywhere else. They are now authored once as GLSL in
 * src/shaders/ and compiled by tools/build_shaders.py into COMMITTED headers
 * carrying both SPIR-V (Vulkan: Linux, Steam Deck, Windows) and MSL (Metal:
 * macOS). Nothing compiles shaders at build time — the hermetic build has a
 * pinned `zig cc` and nothing else.
 *
 * That arrangement has two failure modes a compile cannot catch, and this test
 * pins both:
 *
 *   1. The blob must actually be accepted by the live backend's shader
 *      compiler. A byte array is valid C no matter how corrupt its contents.
 *
 *   2. The entrypoint name differs by format and is not cosmetic: glslc leaves
 *      the SPIR-V entry point named `main`, while spirv-cross renames it to
 *      `main0` when emitting Metal. Swapping them fails shader creation — so
 *      this test asserts the wrong name is REJECTED, which is what proves the
 *      right name is load-bearing rather than coincidental.
 *
 * Needs a real GPU device, so it is skipped (exit 0) wherever one cannot be
 * created — CI containers, headless boxes without Vulkan. It does NOT need a
 * window: shader compilation is a device-level operation.
 */
#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
  const unsigned char *msl;
  unsigned int msl_size;
  const unsigned char *spv;
  unsigned int spv_size;
} GpuShaderBlobs;

#include "shaders/blur_frag.h"
#include "shaders/crt_frag.h"
#include "shaders/dof_edge_frag.h"
#include "shaders/rim_frag.h"
#include "shaders/sim3d_depth_frag.h"
#include "shaders/sim3d_depth_vert.h"

static int s_failures;
#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

/* Every shader the game ships. Adding a .frag.glsl without adding it here
 * would leave it unguarded, so keep this list in step with diorama.c's. */
static const struct {
  const char *name;
  GpuShaderBlobs blobs;
  SDL_GPUShaderStage stage;
  Uint32 samplers;
  Uint32 uniforms;
} kShaders[] = {
  { "blur",        { kBlurFragMSL, kBlurFragMSLSize,
                     kBlurFragSPV, kBlurFragSPVSize },
                     SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1 },
  { "crt",         { kCrtFragMSL, kCrtFragMSLSize,
                     kCrtFragSPV, kCrtFragSPVSize },
                     SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1 },
  { "dof_edge",    { kDofEdgeFragMSL, kDofEdgeFragMSLSize,
                     kDofEdgeFragSPV, kDofEdgeFragSPVSize },
                     SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1 },
  { "rim",         { kRimFragMSL, kRimFragMSLSize,
                     kRimFragSPV, kRimFragSPVSize },
                     SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1 },
  { "sim3d_depth_fragment",
                    { kSim3dDepthFragMSL, kSim3dDepthFragMSLSize,
                      kSim3dDepthFragSPV, kSim3dDepthFragSPVSize },
                      SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0 },
  { "sim3d_depth_vertex",
                    { kSim3dDepthVertMSL, kSim3dDepthVertMSLSize,
                      kSim3dDepthVertSPV, kSim3dDepthVertSPVSize },
                      SDL_GPU_SHADERSTAGE_VERTEX, 0, 0 },
};
static const int kShaderCount = (int)(sizeof(kShaders) / sizeof(kShaders[0]));

/* Mirrors diorama.c's CreateFragmentShaderFromBlobs. `entrypoint` is a
 * parameter here only so the test can feed it a deliberately wrong name. */
static SDL_GPUShader *CreateFrom(SDL_GPUDevice *device,
                                 const GpuShaderBlobs *blobs,
                                 const char *entrypoint,
                                 SDL_GPUShaderStage stage,
                                 Uint32 samplers, Uint32 uniforms) {
  const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
  SDL_GPUShaderCreateInfo info;
  SDL_zero(info);
  if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
    info.code = blobs->spv;
    info.code_size = blobs->spv_size;
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  } else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
    info.code = blobs->msl;
    info.code_size = blobs->msl_size;
    info.format = SDL_GPU_SHADERFORMAT_MSL;
  } else {
    return NULL;
  }
  info.entrypoint = entrypoint;
  info.stage = stage;
  info.num_samplers = samplers;
  info.num_uniform_buffers = uniforms;
  return SDL_CreateGPUShader(device, &info);
}

int main(void) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  /* Advertising both formats is what lets SDL pick a backend it can actually
   * feed — the same call shape main.c needs at renderer creation. */
  SDL_GPUDevice *device = SDL_CreateGPUDevice(
      SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, false, NULL);
  if (!device) {
    fprintf(stderr,
            "shader_blob_test: SKIP — no GPU device supporting SPIR-V or MSL "
            "(%s)\n", SDL_GetError());
    SDL_Quit();
    return 0;
  }

  const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
  const bool spirv = (formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0;
  const char *good = spirv ? "main" : "main0";
  const char *bad = spirv ? "main0" : "main";
  printf("shader_blob_test: driver=%s formats=0x%x using=%s entrypoint=%s "
         "shaders=%d\n",
         SDL_GetGPUDeviceDriver(device), (unsigned)formats,
         spirv ? "SPIR-V" : "MSL", good, kShaderCount);

  /* 1. Every committed blob compiles on this backend. */
  for (int i = 0; i < kShaderCount; i++) {
    SDL_GPUShader *shader = CreateFrom(
        device, &kShaders[i].blobs, good, kShaders[i].stage,
        kShaders[i].samplers, kShaders[i].uniforms);
    CHECK(shader != NULL);
    if (!shader)
      fprintf(stderr, "  %s blob rejected: %s\n",
              kShaders[i].name, SDL_GetError());
    else
      SDL_ReleaseGPUShader(device, shader);
  }

  /* 2. The other format's entrypoint name is rejected, proving the per-format
   *    name in CreateFragmentShaderFromBlobs is doing real work. One shader is
   *    enough — the name comes from the format, not the shader. The backend
   *    logs its own compile error here; that output is EXPECTED. */
  printf("shader_blob_test: negative case follows; one backend shader-compile "
         "error below is expected\n");
  fflush(stdout);
  SDL_GPUShader *wrong = CreateFrom(
      device, &kShaders[0].blobs, bad, kShaders[0].stage,
      kShaders[0].samplers, kShaders[0].uniforms);
  CHECK(wrong == NULL);
  if (wrong) {
    fprintf(stderr,
            "  entrypoint \"%s\" was ACCEPTED on a %s blob — the per-format "
            "entrypoint selection may no longer be needed, or is being "
            "ignored; re-check before trusting it.\n",
            bad, spirv ? "SPIR-V" : "MSL");
    SDL_ReleaseGPUShader(device, wrong);
  }

  SDL_DestroyGPUDevice(device);
  SDL_Quit();

  if (s_failures) {
    fprintf(stderr, "shader_blob_test: %d failure(s)\n", s_failures);
    return 1;
  }
  printf("shader_blob_test: OK\n");
  return 0;
}
