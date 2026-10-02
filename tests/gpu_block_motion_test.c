#include <SDL3/SDL.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "platform/sdl/gpu_block_motion_sdl.h"
#include "present/presentation_frame_generation.h"
#include "support/test_assert.h"
enum { W = 640, B = 880, N = 4 };
static uint32_t Pattern(int x, int y) {
  uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
  h ^= h >> 13;
  return 0xff000000u | (h & 0xffffffu);
}
static void Fill(uint32_t *pixels, int h, unsigned seed, bool current) {
  memset(pixels, 0, (size_t)W * h * N * 4);
  for (unsigned plane = 0; plane < N; ++plane) {
    int dx = current ? (int)((seed + plane) % 7) - 3 : 0,
        dy = current ? (int)((seed * 3 + plane) % 5) - 2 : 0;
    if (!seed)
      dx = dy = 0;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < W; ++x) {
        int sx = x - dx, sy = y - dy;
        bool filled = plane == 0 || (sx >= 23 && sx < 63 && sy >= 5 && sy < h - 2) ||
                      (sx > 130 && sx < 158 && sy > h / 2 && sy < h / 2 + 19);
        if (plane == 3)
          filled = (sx % 123 < 16 && sy % 57 < 17);
        if (seed == 4 && current && plane == 2)
          filled = false;
        if (filled && sx >= 0 && sx < W && sy >= 0 && sy < h)
          pixels[((size_t)plane * h + y) * W + x] = Pattern(sx, sy);
      }
  }
}
static bool Wait(SDL_GPUDevice *gpu, SDL_GPUCommandBuffer *cmd) {
  SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  if (!f)
    return false;
  bool ok = SDL_WaitForGPUFences(gpu, true, &f, 1);
  SDL_ReleaseGPUFence(gpu, f);
  return ok;
}
int main(void) {
  if (!SDL_Init(SDL_INIT_VIDEO))
    return 77;
  SDL_GPUDevice *gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL |
                                               SDL_GPU_SHADERFORMAT_MSL,
                                           true, NULL);
  if (!gpu) {
    SDL_Quit();
    return 77;
  }
  ArGpuBlockMotion p;
  assert(ArGpuBlockMotion_Init(&p, gpu));
  const unsigned heights[] = {17, 64, 352}, widths[] = {33, 624, 640};
  for (unsigned geometry = 0; geometry < 3; ++geometry) {
    const unsigned width = widths[geometry], height = heights[geometry], bytes = W * height * N * 4;
    assert(ArGpuBlockMotion_Resize(&p, width, height));
    const SDL_GPUTextureCreateInfo info = {.type = SDL_GPU_TEXTURETYPE_2D,
                                           .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                           .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
                                           .width = W,
                                           .height = height * N,
                                           .layer_count_or_depth = 1,
                                           .num_levels = 1};
    SDL_GPUTexture *previous = SDL_CreateGPUTexture(gpu, &info),
                   *current = SDL_CreateGPUTexture(gpu, &info);
    assert(previous && current);
    const SDL_GPUTransferBufferCreateInfo ui = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                                .size = bytes * 2};
    const SDL_GPUTransferBufferCreateInfo di = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                                .size = bytes + (4 * B + 4) * 16};
    SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(gpu, &ui),
                          *download = SDL_CreateGPUTransferBuffer(gpu, &di);
    assert(upload && download);
    uint32_t *a = malloc(bytes), *b = malloc(bytes);
    assert(a && b);
    for (unsigned seed = 0; seed < 6; ++seed) {
      Fill(a, (int)height, seed, false);
      Fill(b, (int)height, seed, true);
      for (unsigned y = 0; y < height * N; ++y)
        for (unsigned x = width; x < W; ++x)
          a[y * W + x] = b[y * W + x] = 0;
      uint8_t *mapped = SDL_MapGPUTransferBuffer(gpu, upload, true);
      assert(mapped);
      memcpy(mapped, a, bytes);
      memcpy(mapped + bytes, b, bytes);
      SDL_UnmapGPUTransferBuffer(gpu, upload);
      SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
      assert(cmd);
      SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
      assert(copy);
      SDL_GPUTextureTransferInfo input = {
          .transfer_buffer = upload, .pixels_per_row = W, .rows_per_layer = height * N};
      SDL_GPUTextureRegion dest = {.texture = previous, .w = W, .h = height * N, .d = 1};
      SDL_UploadToGPUTexture(copy, &input, &dest, true);
      input.offset = bytes;
      dest.texture = current;
      SDL_UploadToGPUTexture(copy, &input, &dest, true);
      SDL_EndGPUCopyPass(copy);
      for (unsigned plane = 0; plane < 4; ++plane) {
        p.extents[plane][2] = (float)plane;
        p.extents[plane][3] = seed == 5 && plane == 3 ? 0 : 1;
      }
      assert(ArGpuBlockMotion_Analyze(&p, cmd, previous, current));
      copy = SDL_BeginGPUCopyPass(cmd);
      assert(copy);
      const SDL_GPUBufferRegion field = {.buffer = p.motion, .size = (4 * B + 4) * 16};
      const SDL_GPUTransferBufferLocation output = {.transfer_buffer = download};
      SDL_DownloadFromGPUBuffer(copy, &field, &output);
      SDL_EndGPUCopyPass(copy);
      assert(Wait(gpu, cmd));
      const int32_t *got = SDL_MapGPUTransferBuffer(gpu, download, false);
      assert(got);
      PresentationFrameGenerationMotionField fields[4] = {0};
      for (unsigned plane = 0; plane < 4; ++plane) {
        PresentationFrameGenerationMotionField expected = {0};
        if (p.extents[plane][3] != 0)
          PresentationFrameGeneration_Analyze(
              a + plane * W * height, b + plane * W * height, (int)width, (int)height, W, W,
              kPresentationFrameGenerationAnalysis_Blocks, &expected);
        for (unsigned block = 0; block < B; ++block) {
          int v[4] = {expected.forward_dx[block], expected.forward_dy[block],
                      expected.backward_dx[block], expected.backward_dy[block]};
          for (unsigned c = 0; c < 4; ++c) {
            const int actual = got[(plane * B + block) * 4 + c];
            if (actual != v[c])
              fprintf(stderr, "height=%u seed=%u plane=%u block=%u component=%u GPU=%d CPU=%d\n",
                      height, seed, plane, block, c, actual, v[c]);
            assert(actual == v[c]);
          }
        }
        assert(got[(4 * B + plane) * 4] == expected.valid);
        fields[plane] = expected;
      }
      SDL_UnmapGPUTransferBuffer(gpu, download);
      // Compare GPU-generated intermediate mesh vertices with CPU field
      // sampling in both endpoint directions, including a partial edge block.
      const float phases[] = {0.25f, 0.5f, 0.75f};
      const unsigned bx = (width + 15) / 16, by = (height + 15) / 16;
      const unsigned vertex_count = (bx + 1) * (by + 1);
      for (unsigned phase_index = 0; phase_index < 3; ++phase_index) {
        const float phase = phases[phase_index];
        for (unsigned plane = 0; plane < 4; ++plane) {
          cmd = SDL_AcquireGPUCommandBuffer(gpu);
          assert(cmd);
          assert(ArGpuBlockMotion_Warp(&p, cmd, previous, current, phase, 1u << plane));
          copy = SDL_BeginGPUCopyPass(cmd);
          assert(copy);
          const SDL_GPUBufferRegion vertices = {.buffer = p.vertices, .size = vertex_count * 48};
          SDL_DownloadFromGPUBuffer(copy, &vertices, &output);
          SDL_EndGPUCopyPass(copy);
          assert(Wait(gpu, cmd));
          const float *mesh = SDL_MapGPUTransferBuffer(gpu, download, false);
          assert(mesh);
          const bool forward = phase < 0.5f && fields[plane].valid;
          for (unsigned v = 0; v < vertex_count; ++v) {
            unsigned x = (v % (bx + 1)) * 16, y = (v / (bx + 1)) * 16;
            if (x > width)
              x = width;
            if (y > height)
              y = height;
            float dx = 0, dy = 0;
            if (fields[plane].valid)
              PresentationFrameGeneration_MotionAt(&fields[plane], forward,
                                                   (int)(x < width ? x : width - 1),
                                                   (int)(y < height ? y : height - 1), &dx, &dy);
            float amount = forward ? phase : 1.0f - phase;
            float px = ((float)x + dx * amount) / width * 2.0f - 1.0f;
            float py = 1.0f - ((float)y + dy * amount + plane * height) / (height * 4) * 2.0f;
            assert(fabsf(mesh[v * 12] - px) < 0.000001f);
            assert(fabsf(mesh[v * 12 + 1] - py) < 0.000001f);
            assert(mesh[v * 12 + 4] == (forward ? 1.0f : 0.0f));
            assert(mesh[v * 12 + 8] == x && mesh[v * 12 + 9] == y);
          }
          SDL_UnmapGPUTransferBuffer(gpu, download);
        }
      }
      // The current endpoint must survive the GPU-built warp mesh exactly.
      cmd = SDL_AcquireGPUCommandBuffer(gpu);
      assert(cmd);
      assert(ArGpuBlockMotion_Warp(&p, cmd, previous, current, 1.0f, 15));
      copy = SDL_BeginGPUCopyPass(cmd);
      assert(copy);
      const SDL_GPUTextureRegion image = {.texture = p.output, .w = width, .h = height * N, .d = 1};
      const SDL_GPUTextureTransferInfo io = {
          .transfer_buffer = download, .pixels_per_row = W, .rows_per_layer = height * N};
      SDL_DownloadFromGPUTexture(copy, &image, &io);
      SDL_EndGPUCopyPass(copy);
      assert(Wait(gpu, cmd));
      const uint32_t *result = SDL_MapGPUTransferBuffer(gpu, download, false);
      assert(result);
      for (unsigned i = 0; i < bytes / 4; ++i) {
        if (i % W >= width)
          continue;
        if (result[i] != b[i])
          fprintf(stderr, "warp h=%u seed=%u at=%u GPU=%08x CPU=%08x\n", height, seed, i, result[i],
                  b[i]);
        assert(result[i] == b[i]);
      }
      SDL_UnmapGPUTransferBuffer(gpu, download);
    }
    free(a);
    free(b);
    SDL_ReleaseGPUTexture(gpu, previous);
    SDL_ReleaseGPUTexture(gpu, current);
    SDL_ReleaseGPUTransferBuffer(gpu, upload);
    SDL_ReleaseGPUTransferBuffer(gpu, download);
  }
  assert(SDL_WaitForGPUIdle(gpu));
  ArGpuBlockMotion_Destroy(&p);
  SDL_DestroyGPUDevice(gpu);
  SDL_Quit();
  puts("GPU actor motion: CPU field parity, fractional mesh parity and endpoint parity passed");
  return 0;
}
