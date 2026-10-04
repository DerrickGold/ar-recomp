/* shader_blob_test.c — guards the generated-shader distribution path.
 *
 * The GPU effects were hand-written MSL, so they compiled on Metal and were
 * silently dead everywhere else. They are now authored once as GLSL in
 * src/shaders/ and compiled by tools/build_shaders.py into COMMITTED headers
 * carrying SPIR-V (Vulkan), DXIL (D3D12), and MSL (Metal). Nothing compiles
 * shaders at build time — the hermetic build has a pinned `zig cc` and
 * nothing else.
 *
 * That arrangement has two failure modes a compile cannot catch, and this test
 * pins both:
 *
 *   1. The blob must actually be accepted by the live backend's shader
 *      compiler. A byte array is valid C no matter how corrupt its contents.
 *
 *   2. The selectable entrypoint name differs for SPIR-V and MSL: glslc leaves
 *      SPIR-V at `main`, while spirv-cross emits Metal `main0`. Swapping them
 *      is checked in SPIR-V metadata; live pipelines validate linkage.
 *      Vulkan module creation need not resolve an entrypoint. DXIL is already a single compiled stage container and D3D12
 *      implementations may ignore this field, so only its positive case is
 *      meaningful.
 *
 * Needs a real GPU device, so it is skipped (exit 0) wherever one cannot be
 * created — CI containers, headless boxes without Vulkan. It does NOT need a
 * window: shader compilation is a device-level operation.
 */
#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  const unsigned char *msl;
  unsigned int msl_size;
  const unsigned char *spv;
  unsigned int spv_size;
  const unsigned char *dxil;
  unsigned int dxil_size;
} GpuShaderBlobs;

#include "shaders/blur_frag.h"
#include "shaders/crt_frag.h"
#include "shaders/dof_edge_frag.h"
#include "shaders/rim_frag.h"
#include "shaders/sim3d_billboard_rim_frag.h"
#include "shaders/sim3d_depth_frag.h"
#include "shaders/sim3d_depth_vert.h"
#include "shaders/sim3d_spherical_vert.h"
#include "shaders/sim3d_model_vert.h"
#include "shaders/sim3d_linear_vert.h"
#include "shaders/sim3d_shadow_batch_vert.h"
#include "shaders/sim3d_shadow_batch_frag.h"
#include "shaders/sim_shadow_blur_frag.h"
#include "shaders/sim_cloud_frag.h"
#include "shaders/sim3d_model_clipped_frag.h"

static int s_failures;
#define CHECK(expr)                                                                                \
  do {                                                                                             \
    if (!(expr)) {                                                                                 \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr);                     \
      s_failures++;                                                                                \
    }                                                                                              \
  } while (0)

/* Representative shaders from each graphics interface the game ships. Adding a .frag.glsl without adding it here
 * would leave it unguarded, so keep this list in step with the platform effect
 * implementations. */
typedef struct ShaderCase {
  const char *name;
  GpuShaderBlobs blobs;
  SDL_GPUShaderStage stage;
  Uint32 samplers;
  Uint32 uniforms;
} ShaderCase;
static const ShaderCase kShaders[] = {
    {"sim3d_shadow_batch_vertex",
     {kSim3dShadowBatchVertMSL, kSim3dShadowBatchVertMSLSize, kSim3dShadowBatchVertSPV,
      kSim3dShadowBatchVertSPVSize, kSim3dShadowBatchVertDXIL, kSim3dShadowBatchVertDXILSize},
     SDL_GPU_SHADERSTAGE_VERTEX,
     0,
     1},
    {"sim3d_shadow_batch_fragment",
     {kSim3dShadowBatchFragMSL, kSim3dShadowBatchFragMSLSize, kSim3dShadowBatchFragSPV,
      kSim3dShadowBatchFragSPVSize, kSim3dShadowBatchFragDXIL, kSim3dShadowBatchFragDXILSize},
     SDL_GPU_SHADERSTAGE_FRAGMENT,
     1,
     0},
    {"sim3d_linear_vertex",
     {kSim3dLinearVertMSL, kSim3dLinearVertMSLSize, kSim3dLinearVertSPV, kSim3dLinearVertSPVSize,
      kSim3dLinearVertDXIL, kSim3dLinearVertDXILSize},
     SDL_GPU_SHADERSTAGE_VERTEX,
     0,
     1},
    {"blur",
     {kBlurFragMSL, kBlurFragMSLSize, kBlurFragSPV, kBlurFragSPVSize, kBlurFragDXIL,
      kBlurFragDXILSize},
     SDL_GPU_SHADERSTAGE_FRAGMENT,
     1,
     1},
    {"crt",
     {kCrtFragMSL, kCrtFragMSLSize, kCrtFragSPV, kCrtFragSPVSize, kCrtFragDXIL, kCrtFragDXILSize},
     SDL_GPU_SHADERSTAGE_FRAGMENT,
     1,
     1},
    {"dof_edge",
     {kDofEdgeFragMSL, kDofEdgeFragMSLSize, kDofEdgeFragSPV, kDofEdgeFragSPVSize, kDofEdgeFragDXIL,
      kDofEdgeFragDXILSize},
     SDL_GPU_SHADERSTAGE_FRAGMENT,
     1,
     1},
    {"rim",
     {kRimFragMSL, kRimFragMSLSize, kRimFragSPV, kRimFragSPVSize, kRimFragDXIL, kRimFragDXILSize},
     SDL_GPU_SHADERSTAGE_FRAGMENT,
     1,
     1},
    {"billboard rim",
     {kSim3dBillboardRimFragMSL, kSim3dBillboardRimFragMSLSize, kSim3dBillboardRimFragSPV,
      kSim3dBillboardRimFragSPVSize, kSim3dBillboardRimFragDXIL, kSim3dBillboardRimFragDXILSize},
     SDL_GPU_SHADERSTAGE_FRAGMENT,
     1,
     1},
    {"sim3d_depth_fragment",
     {kSim3dDepthFragMSL, kSim3dDepthFragMSLSize, kSim3dDepthFragSPV, kSim3dDepthFragSPVSize,
      kSim3dDepthFragDXIL, kSim3dDepthFragDXILSize},
     SDL_GPU_SHADERSTAGE_FRAGMENT,
     1,
     0},
    {"sim3d_depth_vertex",
     {kSim3dDepthVertMSL, kSim3dDepthVertMSLSize, kSim3dDepthVertSPV, kSim3dDepthVertSPVSize,
      kSim3dDepthVertDXIL, kSim3dDepthVertDXILSize},
     SDL_GPU_SHADERSTAGE_VERTEX,
     0,
     0},
    {"sim3d_spherical_vertex",
     {kSim3dSphericalVertMSL, kSim3dSphericalVertMSLSize, kSim3dSphericalVertSPV,
      kSim3dSphericalVertSPVSize, kSim3dSphericalVertDXIL, kSim3dSphericalVertDXILSize},
     SDL_GPU_SHADERSTAGE_VERTEX,
     0,
     1},
    {"sim3d_model_vertex",
     {kSim3dModelVertMSL, kSim3dModelVertMSLSize, kSim3dModelVertSPV, kSim3dModelVertSPVSize,
      kSim3dModelVertDXIL, kSim3dModelVertDXILSize},
     SDL_GPU_SHADERSTAGE_VERTEX,
     0,
     1},
    {"sim_shadow_blur",
     {kSimShadowBlurFragMSL, kSimShadowBlurFragMSLSize, kSimShadowBlurFragSPV,
      kSimShadowBlurFragSPVSize, kSimShadowBlurFragDXIL, kSimShadowBlurFragDXILSize},
     SDL_GPU_SHADERSTAGE_FRAGMENT,
     1,
     1},
    {"sim_cloud",
     {kSimCloudFragMSL, kSimCloudFragMSLSize, kSimCloudFragSPV, kSimCloudFragSPVSize,
      kSimCloudFragDXIL, kSimCloudFragDXILSize},
     SDL_GPU_SHADERSTAGE_FRAGMENT,
     1,
     1},
    {"sim3d_model_clipped_fragment",
     {kSim3dModelClippedFragMSL, kSim3dModelClippedFragMSLSize,
      kSim3dModelClippedFragSPV, kSim3dModelClippedFragSPVSize,
      kSim3dModelClippedFragDXIL, kSim3dModelClippedFragDXILSize},
     SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0},
};
static const int kShaderCount = (int)(sizeof(kShaders) / sizeof(kShaders[0]));

/* Mirrors GpuShaderBlob_Create. `entrypoint` is a parameter here only so the
 * test can feed it a deliberately wrong name. */
static SDL_GPUShader *CreateFrom(SDL_GPUDevice *device, const GpuShaderBlobs *blobs,
                                 const char *entrypoint, SDL_GPUShaderStage stage, Uint32 samplers,
                                 Uint32 uniforms) {
  const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
  SDL_GPUShaderCreateInfo info;
  SDL_zero(info);
  if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
    info.code = blobs->spv;
    info.code_size = blobs->spv_size;
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  } else if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
    info.code = blobs->dxil;
    info.code_size = blobs->dxil_size;
    info.format = SDL_GPU_SHADERFORMAT_DXIL;
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

/* Decode little-endian words without alignment/aliasing assumptions. Opcode
 * 15 is OpEntryPoint; its execution model is Vertex=0 or Fragment=4. Check
 * the whole instruction stream so a truncated string cannot pass by prefix. */
static uint32_t SpvWord(const unsigned char *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool HasSpirvEntryPoint(const GpuShaderBlobs *blob, SDL_GPUShaderStage stage,
                               const char *name) {
  if (blob->spv_size < 20 || blob->spv_size % 4 || SpvWord(blob->spv) != 0x07230203)
    return false;
  bool found = false;
  for (size_t at = 20; at < blob->spv_size;) {
    uint32_t instruction = SpvWord(blob->spv + at);
    size_t bytes = (instruction >> 16) * 4;
    if (!bytes || bytes > blob->spv_size - at) return false;
    if ((instruction & 0xffff) == 15) {
      if (bytes < 16) return false;
      const char *entry = (const char *)blob->spv + at + 12;
      const char *end = memchr(entry, 0, bytes - 12);
      if (!end) return false;
      if (SpvWord(blob->spv + at + 4) == (stage == SDL_GPU_SHADERSTAGE_VERTEX ? 0u : 4u) &&
          (size_t)(end - entry) == strlen(name) && !memcmp(entry, name, strlen(name)))
        found = true;
    }
    at += bytes;
  }
  return found;
}

static const ShaderCase *FindShader(const char *name) {
  for (int i = 0; i < kShaderCount; ++i)
    if (!strcmp(kShaders[i].name, name)) return &kShaders[i];
  return NULL;
}

/* Pipeline creation, not Vulkan shader-module creation, resolves names and
 * links stage interfaces. Pair every tested module with its matching stage. */
static void CheckPipeline(SDL_GPUDevice *device, const ShaderCase *test, const char *entry) {
  const bool shadow = strstr(test->name, "shadow_batch") != NULL;
  const bool linear = !strcmp(test->name, "sim3d_linear_vertex") ||
                      !strcmp(test->name, "sim3d_model_clipped_fragment");
  const ShaderCase *vertex = test->stage == SDL_GPU_SHADERSTAGE_VERTEX ? test :
      FindShader(shadow ? "sim3d_shadow_batch_vertex" :
                 linear ? "sim3d_linear_vertex" : "sim3d_depth_vertex");
  const ShaderCase *fragment = test->stage == SDL_GPU_SHADERSTAGE_FRAGMENT ? test :
      FindShader(shadow ? "sim3d_shadow_batch_fragment" :
                 linear ? "sim3d_model_clipped_fragment" : "sim3d_depth_fragment");
  SDL_GPUShader *vs = CreateFrom(device, &vertex->blobs, entry, vertex->stage,
                                 vertex->samplers, vertex->uniforms);
  SDL_GPUShader *fs = CreateFrom(device, &fragment->blobs, entry, fragment->stage,
                                 fragment->samplers, fragment->uniforms);
  CHECK(vs && fs);
  if (vs && fs) {
    const bool spherical = !strcmp(vertex->name, "sim3d_spherical_vertex");
    const bool model = !strcmp(vertex->name, "sim3d_model_vertex");
    const unsigned count = shadow ? 8 : spherical ? 10 : model ? 2 : 3;
    SDL_GPUVertexAttribute attributes[10];
    for (unsigned i = 0; i < count; ++i)
      attributes[i] = (SDL_GPUVertexAttribute){i, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, i * 16};
    if (model) attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    if (count == 3) attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    const SDL_GPUVertexBufferDescription buffer = {
      .slot = 0, .pitch = count * 16,
      .input_rate = shadow || spherical ? SDL_GPU_VERTEXINPUTRATE_INSTANCE : SDL_GPU_VERTEXINPUTRATE_VERTEX,
    };
    const SDL_GPUColorTargetDescription color = {.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM};
    const SDL_GPUGraphicsPipelineCreateInfo info = {
      .vertex_shader = vs, .fragment_shader = fs,
      .vertex_input_state = {&buffer, 1, attributes, count},
      .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
      .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE,
                          .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE, .enable_depth_clip = true},
      .multisample_state = {.sample_count = SDL_GPU_SAMPLECOUNT_1},
      .target_info = {.color_target_descriptions = &color, .num_color_targets = 1},
    };
    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    CHECK(pipeline);
    if (!pipeline) fprintf(stderr, "  %s pipeline rejected: %s\n", test->name, SDL_GetError());
    else SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
  }
  if (vs) SDL_ReleaseGPUShader(device, vs);
  if (fs) SDL_ReleaseGPUShader(device, fs);
}

int main(void) {
  for (int i = 0; i < kShaderCount; ++i) {
    CHECK(HasSpirvEntryPoint(&kShaders[i].blobs, kShaders[i].stage, "main"));
    CHECK(!HasSpirvEntryPoint(&kShaders[i].blobs, kShaders[i].stage, "main0"));
    CHECK(!HasSpirvEntryPoint(&kShaders[i].blobs,
        kShaders[i].stage == SDL_GPU_SHADERSTAGE_VERTEX ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX,
        "main"));
  }
  if (s_failures) return 1;
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  /* Advertising both formats is what lets SDL pick a backend it can actually
   * feed — the call shape render_preparation.c uses at renderer creation. */
  SDL_GPUDevice *device = SDL_CreateGPUDevice(
      SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL, false,
      NULL);
  if (!device) {
    fprintf(stderr,
            "shader_blob_test: SKIP — no GPU device supporting SPIR-V, DXIL, "
            "or MSL "
            "(%s)\n",
            SDL_GetError());
    SDL_Quit();
    return 0;
  }

  const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
  const bool spirv = (formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0;
  const bool dxil = (formats & SDL_GPU_SHADERFORMAT_DXIL) != 0;
  const char *good = (spirv || dxil) ? "main" : "main0";
  const char *bad = (spirv || dxil) ? "main0" : "main";
  printf("shader_blob_test: driver=%s formats=0x%x using=%s entrypoint=%s "
         "shaders=%d\n",
         SDL_GetGPUDeviceDriver(device), (unsigned)formats,
         spirv ? "SPIR-V" : (dxil ? "DXIL" : "MSL"), good, kShaderCount);

  for (int i = 0; i < kShaderCount; i++) CheckPipeline(device, &kShaders[i], good);

  if (!spirv && !dxil) {
    /* The other source module's entrypoint name must be rejected. One shader
     * is enough: the name comes from the module format, not the effect. */
    printf("shader_blob_test: negative case follows; one backend "
           "shader-compile error below is expected\n");
    fflush(stdout);
    SDL_GPUShader *wrong = CreateFrom(device, &kShaders[0].blobs, bad, kShaders[0].stage,
                                      kShaders[0].samplers, kShaders[0].uniforms);
    CHECK(wrong == NULL);
    if (wrong) {
      fprintf(stderr,
              "  entrypoint \"%s\" was ACCEPTED on a %s source module — "
              "re-check the generated entrypoint contract.\n",
              bad, spirv ? "SPIR-V" : "MSL");
      SDL_ReleaseGPUShader(device, wrong);
    }
  } else {
    printf("shader_blob_test: SPIR-V metadata checked; DXIL has no alternate entrypoint "
           "negative case\n");
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
