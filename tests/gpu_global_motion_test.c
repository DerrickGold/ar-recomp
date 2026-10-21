#include <SDL3/SDL.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/sdl/gpu_global_motion_sdl.h"
#include "present/presentation_frame_generation.h"
#include "support/test_assert.h"

enum { W = 640, N = 8 };
static const int kDx[N] = {-7, -4, -2, 0, 2, 4, 6, 7};
static const int kDy[N] = {7, -2, 0, 0, 5, -4, 3, -7};

static uint32_t Pattern(int x, int y) {
  uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
  h ^= h >> 13;
  return 0xff000000u | (h & 0xffffffu);
}

static void Fill(uint32_t *pixels, unsigned height, unsigned seed, bool current) {
  for (unsigned band = 0; band < N; ++band)
    for (int y = 0; y < (int)height; ++y)
      for (int x = 0; x < W; ++x) {
        int dx = current && seed ? kDx[band] : 0;
        int dy = current && seed ? kDy[band] : 0;
        const int sx = x - dx, sy = y - dy;
        uint32_t pixel = Pattern(sx, sy);
        if (seed == 2 && x % 13 < 4 && current) pixel ^= 0x00371821u;
        if (seed == 3) pixel = current ? 0xff113355u : 0xff775511u;
        if (seed == 4) pixel = (sx + sy) % 4 ? 0xff775511u : 0xff113355u;
        if (seed == 5) pixel &= current && x % 3 == 0 ? 0xffffffu : 0xffffffffu;
        if (seed == 6 || (seed == 7 && (sx % 127 > 20 || sy % 71 > 20))) pixel = 0;
        pixels[((size_t)band * height + y) * W + x] = pixel;
      }
}

static void Hints(ArGpuGlobalMotion *p, unsigned mode, unsigned seed) {
  p->camera_bound = mode != 0;
  for (unsigned i = 0; i < N; ++i) {
    int dx = seed ? kDx[i] : 0, dy = seed ? kDy[i] : 0;
    if (mode == 2) { dx = -dx; dy = -dy; }
    if (mode == 3) { dx = i % 2 ? INT_MAX : INT_MIN; dy = i % 2 ? INT_MIN : INT_MAX; }
    if (mode == 4) { dx = (int)(i * 17 % 15) - 7; dy = (int)(i * 29 % 15) - 7; }
    ArGpuGlobalMotion_SetHint(p, i, dx, dy);
  }
}

static bool Wait(SDL_GPUDevice *gpu, SDL_GPUCommandBuffer *cmd) {
  SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  if (!fence) return false;
  bool ok = SDL_WaitForGPUFences(gpu, true, &fence, 1);
  SDL_ReleaseGPUFence(gpu, fence);
  return ok;
}

static int CompareTimes(const void *a, const void *b) {
  const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

/* Batched encoding through completion, not per-frame latency. No uploads,
 * downloads or compilation inside timing. Opposite order exposes drift. */
static void Benchmark(SDL_GPUDevice *gpu, ArGpuGlobalMotion *p,
    SDL_GPUTexture *previous, SDL_GPUTexture *current, unsigned seed) {
  enum { Batch = 8, Samples = 64, Warmup = 8 };
  const unsigned modes[] = {0, 1, 1, 0, 0, 2, 2, 0};
  for (unsigned round = 0; round < SDL_arraysize(modes); ++round) {
    Hints(p, modes[round], seed);
    uint64_t total = 0, encode = 0, elapsed[Samples];
    for (unsigned sample = 0; sample < Samples + Warmup; ++sample) {
      uint64_t start = SDL_GetTicksNS();
      SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
      assert(cmd);
      for (unsigned frame = 0; frame < Batch; ++frame)
        assert(ArGpuGlobalMotion_Analyze(p, cmd, previous, current));
      SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
      uint64_t submitted = SDL_GetTicksNS();
      assert(fence && SDL_WaitForGPUFences(gpu, true, &fence, 1));
      SDL_ReleaseGPUFence(gpu, fence);
      uint64_t end = SDL_GetTicksNS();
      if (sample >= Warmup) {
        elapsed[sample - Warmup] = end - start;
        total += end - start;
        encode += submitted - start;
      }
    }
    qsort(elapsed, Samples, sizeof(elapsed[0]), CompareTimes);
    printf("global-analysis-benchmark hint=%u round=%u seed=%u size=%ux%u frames=%u "
        "completion-ms=%.6f encode-submit-ms=%.6f batch-p50-ms=%.6f batch-p95-ms=%.6f\n",
        modes[round], round, seed, p->width, p->height, Samples * Batch,
        (double)total / (Samples * Batch * 1e6), (double)encode / (Samples * Batch * 1e6),
        (double)elapsed[Samples / 2] / (Batch * 1e6),
        (double)elapsed[(Samples - 1) * 95 / 100] / (Batch * 1e6));
    fflush(stdout);
  }
}

int main(int argc, char **argv) {
  const bool benchmark = argc == 2 && strcmp(argv[1], "--benchmark") == 0;
  if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
  SDL_GPUDevice *gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV |
      SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL, !benchmark, NULL);
  if (!gpu) { SDL_Quit(); return 77; }
  ArGpuGlobalMotion p;
  assert(ArGpuGlobalMotion_Init(&p, gpu));
  p.plane_count = N;
  const unsigned widths[] = {33, 256, 640}, heights[] = {17, 224, 352};
  unsigned checks = 0;
  for (unsigned geometry = 0; geometry < SDL_arraysize(widths); ++geometry) {
    if (benchmark && geometry != 2) continue;
    const unsigned width = widths[geometry], height = heights[geometry];
    const unsigned bytes = W * height * N * 4;
    assert(ArGpuGlobalMotion_Resize(&p, width, height));
    const SDL_GPUTextureCreateInfo ti = {.type = SDL_GPU_TEXTURETYPE_2D,
        .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = W, .height = height * N,
        .layer_count_or_depth = 1, .num_levels = 1};
    SDL_GPUTexture *previous = SDL_CreateGPUTexture(gpu, &ti);
    SDL_GPUTexture *current = SDL_CreateGPUTexture(gpu, &ti);
    assert(previous && current);
    const SDL_GPUTransferBufferCreateInfo ui = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = bytes * 2};
    const SDL_GPUTransferBufferCreateInfo di = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        .size = N * sizeof(ArGpuGlobalMotionResult)};
    SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(gpu, &ui);
    SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(gpu, &di);
    uint32_t *a = malloc(bytes), *b = malloc(bytes);
    assert(upload && download && a && b);
    for (unsigned seed = 0; seed < 8; ++seed) {
      if (benchmark && seed != 0 && seed != 1 && seed != 2 && seed != 6) continue;
      Fill(a, height, seed, false);
      Fill(b, height, seed, true);
      void *mapped = SDL_MapGPUTransferBuffer(gpu, upload, true);
      assert(mapped);
      memcpy(mapped, a, bytes);
      memcpy((uint8_t *)mapped + bytes, b, bytes);
      SDL_UnmapGPUTransferBuffer(gpu, upload);
      SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
      assert(cmd);
      SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
      assert(copy);
      for (unsigned i = 0; i < 2; ++i) {
        const SDL_GPUTextureTransferInfo src = {.transfer_buffer = upload,
            .offset = i * bytes, .pixels_per_row = W, .rows_per_layer = height * N};
        const SDL_GPUTextureRegion dst = {
            .texture = i ? current : previous, .w = W, .h = height * N, .d = 1};
        SDL_UploadToGPUTexture(copy, &src, &dst, true);
      }
      SDL_EndGPUCopyPass(copy);
      assert(Wait(gpu, cmd));
      if (benchmark) { Benchmark(gpu, &p, previous, current, seed); continue; }
      ArGpuGlobalMotionResult expected[N] = {0};
      for (unsigned i = 0; i < N; ++i) {
        PresentationFrameGenerationMotionField field;
        const unsigned w = i % 2 ? width - 3 : width;
        p.extents[i][0] = (float)w;
        if (PresentationFrameGeneration_Analyze(a + i * W * height, b + i * W * height,
            (int)w, (int)height, W, W, kPresentationFrameGenerationAnalysis_Global, &field)) {
          expected[i] = (ArGpuGlobalMotionResult){field.forward_dx[0], field.forward_dy[0],
              field.backward_dx[0], field.backward_dy[0], 1, {0}};
        }
      }
      const unsigned modes[] = {0, 1, 2, 3, 4, 0};
      for (unsigned step = 0; step < SDL_arraysize(modes); ++step) {
        Hints(&p, modes[step], seed);
        for (unsigned i = 0; i < N; ++i)
          p.extents[i][1] = step % 2 && i % 3 == 0 ? 0.0f : 1.0f;
        cmd = SDL_AcquireGPUCommandBuffer(gpu);
        assert(cmd && ArGpuGlobalMotion_Analyze(&p, cmd, previous, current));
        copy = SDL_BeginGPUCopyPass(cmd);
        assert(copy);
        const SDL_GPUBufferRegion src = {.buffer = p.motion, .size = di.size};
        const SDL_GPUTransferBufferLocation dst = {.transfer_buffer = download};
        SDL_DownloadFromGPUBuffer(copy, &src, &dst);
        SDL_EndGPUCopyPass(copy);
        assert(Wait(gpu, cmd));
        const ArGpuGlobalMotionResult *actual = SDL_MapGPUTransferBuffer(gpu, download, false);
        assert(actual);
        for (unsigned i = 0; i < N; ++i) {
          const ArGpuGlobalMotionResult want = p.extents[i][1]
              ? expected[i] : (ArGpuGlobalMotionResult){0};
          if (memcmp(&want, &actual[i], sizeof(want)))
            fprintf(stderr, "global motion mismatch geometry=%u seed=%u mode=%u plane=%u\n",
                geometry, seed, modes[step], i);
          assert(memcmp(&want, &actual[i], sizeof(want)) == 0);
          ++checks;
        }
        SDL_UnmapGPUTransferBuffer(gpu, download);
      }
    }
    free(a);
    free(b);
    SDL_ReleaseGPUTransferBuffer(gpu, upload);
    SDL_ReleaseGPUTransferBuffer(gpu, download);
    SDL_ReleaseGPUTexture(gpu, previous);
    SDL_ReleaseGPUTexture(gpu, current);
  }
  printf("global motion: %u exact CPU-oracle checks, backend=%s\n",
      checks, SDL_GetGPUDeviceDriver(gpu));
  ArGpuGlobalMotion_Destroy(&p);
  SDL_DestroyGPUDevice(gpu);
  SDL_Quit();
  return 0;
}
