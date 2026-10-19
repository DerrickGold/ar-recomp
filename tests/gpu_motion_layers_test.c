#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/sdl/gpu_global_motion_sdl.h"
#include "platform/sdl/gpu_block_motion_sdl.h"
#include "platform/sdl/gpu_frame_handoff_sdl.h"
#include "support/test_assert.h"

enum { W = 640, H = 352, N = 16, Pixels = W * H, Bytes = Pixels * 4, Blocks = 880 };
static void Wait(SDL_GPUDevice *gpu, SDL_GPUCommandBuffer *cmd) {
  SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
  assert(fence && SDL_WaitForGPUFences(gpu, true, &fence, 1));
  SDL_ReleaseGPUFence(gpu, fence);
}
static SDL_GPUTexture *Texture(SDL_GPUDevice *gpu, unsigned height, SDL_GPUTextureUsageFlags usage) {
  const SDL_GPUTextureCreateInfo info = {.type = SDL_GPU_TEXTURETYPE_2D,
      .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, .usage = usage,
      .width = W, .height = height, .layer_count_or_depth = 1, .num_levels = 1};
  SDL_GPUTexture *texture = SDL_CreateGPUTexture(gpu, &info);
  assert(texture);
  return texture;
}
/* Optional isolated throughput probe: no swapchain, motion search or readback
 * in the measured interval. Timings include command encoding/submission and
 * completion waits, so they are not GPU timestamps or whole-game pacing. */
static void Benchmark(SDL_GPUDevice *gpu, ArGpuGlobalMotion *global, ArGpuBlockMotion *actors,
    SDL_GPUTexture *previous, SDL_GPUTexture *current, ArGpuGlobalMotionTarget bg[8],
    const ArGpuBlockMotionTarget obj[4], const ArGpuFrameUnpack all[N],
    unsigned bg_mask, unsigned obj_mask) {
  enum { Batch = 8, Samples = 128, Warmup = 8 };
  ArGpuFrameUnpack unpack[N];
  unsigned count = 0;
  for (unsigned i = 0; i < N; ++i) {
    unsigned mask = i < 8 ? bg_mask : obj_mask, plane = i < 8 ? i : (i - 8) % 4;
    if (mask & (1u << plane)) unpack[count++] = all[i];
  }
  for (unsigned i = 0; i < 8; ++i) bg[i].clear_padding = false;
  const bool modes[] = {false, true, true, false};
  for (unsigned round = 0; round < 4; ++round) {
    uint64_t total = 0, encode = 0;
    for (unsigned sample = 0; sample < Samples + Warmup; ++sample) {
      uint64_t start = SDL_GetTicksNS();
      SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
      assert(cmd);
      for (unsigned frame = 0; frame < Batch; ++frame) {
        float phase = (float)((sample + frame) % 3 + 1) / 4.0f;
        if (modes[round]) {
          assert(ArGpuGlobalMotion_WarpLayers(global, cmd, previous, current, phase, bg, bg_mask));
          assert(ArGpuBlockMotion_WarpLayers(actors, cmd, previous, current, phase, obj_mask, obj));
        } else {
          assert(ArGpuGlobalMotion_Warp(global, cmd, previous, current, phase));
          assert(ArGpuBlockMotion_Warp(actors, cmd, previous, current, phase, obj_mask));
          assert(ArGpuFrameHandoff_EncodeUnpack(cmd, unpack, count));
        }
      }
      SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
      uint64_t encoded = SDL_GetTicksNS();
      assert(fence && SDL_WaitForGPUFences(gpu, true, &fence, 1));
      SDL_ReleaseGPUFence(gpu, fence);
      uint64_t end = SDL_GetTicksNS();
      if (sample >= Warmup) { total += end - start; encode += encoded - start; }
    }
    printf("motion-benchmark direct=%u round=%u bg=%02x obj=%x frames=%u completion-ms=%.6f encode-submit-ms=%.6f\n",
        modes[round], round, bg_mask, obj_mask, Samples * Batch,
        (double)total / (Samples * Batch * 1e6), (double)encode / (Samples * Batch * 1e6));
    fflush(stdout);
  }
}

int main(int argc, char **argv) {
  bool benchmark = argc == 2 && strcmp(argv[1], "--benchmark") == 0;
  if (!SDL_Init(SDL_INIT_VIDEO)) return 77;
  SDL_GPUDevice *gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV |
      SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL, !benchmark, NULL);
  if (!gpu) { SDL_Quit(); return 77; }
  ArGpuGlobalMotion global;
  ArGpuBlockMotion actors;
  assert(ArGpuGlobalMotion_Init(&global, gpu));
  assert(ArGpuGlobalMotion_PrepareLayerWarp(&global));
  assert(ArGpuBlockMotion_Init(&actors, gpu));
  global.plane_count = 8;
  global.input_plane_count = 16;
  SDL_GPUTexture *reference[N], *direct[N];
  for (unsigned i = 0; i < N; ++i) {
    reference[i] = Texture(gpu, H, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    direct[i] = Texture(gpu, H, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
        (i < 8 ? SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE : 0));
  }
  const unsigned field_bytes = (4 * Blocks + 4) * 16;
  const SDL_GPUTransferBufferCreateInfo upload_info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
      .size = Bytes * N * 2 + field_bytes + 8 * sizeof(ArGpuGlobalMotionResult)};
  const SDL_GPUTransferBufferCreateInfo download_info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
      .size = Bytes * N * 2};
  SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(gpu, &upload_info);
  SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(gpu, &download_info);
  assert(upload && download);
  const unsigned heights[] = {17, 352, 224}; /* Grow, then shrink: stale padding must clear. */
  const unsigned widths[] = {33, 624, 512};
  const float phases[] = {0, .25f, .5f, .75f, 1};
  unsigned cases = 0, rounded_pixels = 0, maximum_delta = 0;
  for (unsigned geometry = 0; geometry < 3; ++geometry) {
    const unsigned height = heights[geometry], width = widths[geometry];
    assert(ArGpuGlobalMotion_Resize(&global, W, height));
    assert(ArGpuBlockMotion_Resize(&actors, width, height));
    assert(!global.output && !actors.output && !actors.attributes);
    SDL_GPUTexture *previous = Texture(gpu, height * N, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    SDL_GPUTexture *current = Texture(gpu, height * N, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    uint32_t *data = SDL_MapGPUTransferBuffer(gpu, upload, true);
    assert(data);
    for (unsigned endpoint = 0; endpoint < 2; ++endpoint)
      for (unsigned plane = 0; plane < N; ++plane)
        for (unsigned y = 0; y < height; ++y)
          for (unsigned x = 0; x < W; ++x) {
            uint32_t hash = (x + endpoint * 3) * 73856093u ^ (y + endpoint) * 19349663u ^ plane * 83492791u;
            /* Different material metadata, translucent edges, transparent RGB
             * and disconnected sprites all need to survive the same warp. */
            unsigned alpha = plane < 8 || (x % 37 < 23 && y % 29 < 17) ? (hash >> 24) : 0;
            data[((endpoint * N + plane) * height + y) * W + x] = (hash & 0xffffffu) | (alpha << 24);
          }
    const unsigned image_bytes = W * height * N * 4;
    ArGpuGlobalMotionResult motion[8] = {0};
    for (unsigned i = 0; i < 8; ++i) {
      motion[i] = (ArGpuGlobalMotionResult){.forward_x = (int)i - 4, .forward_y = 3 - (int)i,
          .backward_x = 4 - (int)i, .backward_y = (int)i - 3, .valid = i % 3 != 0};
      global.extents[i][0] = (float)(width - i);
    }
    memcpy((uint8_t *)data + image_bytes * 2, motion, sizeof(motion));
    int32_t *field = (int32_t *)((uint8_t *)data + image_bytes * 2 + sizeof(motion));
    memset(field, 0, field_bytes);
    for (unsigned i = 0; i < 4; ++i) {
      field[(4 * Blocks + i) * 4] = i != 3;
      for (unsigned b = 0; b < Blocks; ++b) {
        int dx = i != 3 ? (int)((b + i) % 7) - 3 : 0, dy = i != 3 ? (int)(b % 5) - 2 : 0;
        field[(i * Blocks + b) * 4] = dx;
        field[(i * Blocks + b) * 4 + 1] = dy;
        field[(i * Blocks + b) * 4 + 2] = -dx;
        field[(i * Blocks + b) * 4 + 3] = -dy;
      }
    }
    SDL_UnmapGPUTransferBuffer(gpu, upload);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
    assert(cmd);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    assert(copy);
    SDL_GPUTextureTransferInfo input = {.transfer_buffer = upload, .pixels_per_row = W,
        .rows_per_layer = height * N};
    SDL_GPUTextureRegion region = {.texture = previous, .w = W, .h = height * N, .d = 1};
    SDL_UploadToGPUTexture(copy, &input, &region, true);
    input.offset = image_bytes; region.texture = current;
    SDL_UploadToGPUTexture(copy, &input, &region, true);
    SDL_GPUTransferBufferLocation from = {.transfer_buffer = upload, .offset = image_bytes * 2};
    SDL_GPUBufferRegion to = {.buffer = global.motion, .size = sizeof(motion)};
    SDL_UploadToGPUBuffer(copy, &from, &to, true);
    from.offset += sizeof(motion); to.buffer = actors.motion; to.size = field_bytes;
    SDL_UploadToGPUBuffer(copy, &from, &to, true);
    SDL_EndGPUCopyPass(copy);
    Wait(gpu, cmd);
    for (unsigned phase_index = 0; phase_index < 5; ++phase_index) {
      /* All, sparse, none, re-entry: absent bands must retain their prior
       * contents rather than alias another binding or be accidentally cleared. */
      unsigned bg_mask = phase_index == 1 ? 0xa5 : phase_index == 2 ? 0 : 255;
      unsigned actor_mask = phase_index == 1 ? 5 : phase_index == 2 ? 0 : 15;
      ArGpuGlobalMotionTarget bg[8] = {0};
      ArGpuBlockMotionTarget obj[4] = {0};
      ArGpuFrameUnpack unpack[N] = {0};
      unsigned count = 0;
      for (unsigned i = 0; i < 8; ++i)
        bg[i] = (ArGpuGlobalMotionTarget){direct[i], W, H, geometry == 0 ? i * 9 : i,
            phase_index == 0};
      for (unsigned i = 0; i < 4; ++i)
        obj[i] = (ArGpuBlockMotionTarget){direct[8+i], direct[12+i], W, H,
            geometry == 0 ? i * 9 : i};
      cmd = SDL_AcquireGPUCommandBuffer(gpu);
      assert(cmd);
      /* Exercise direct synthesis before allocating any fallback atlas, then
       * create the reference outputs lazily in the same geometry. Repeating
       * after grow/shrink checks both allocation and padding lifetimes. */
      assert(ArGpuGlobalMotion_WarpLayers(&global, cmd, previous, current, phases[phase_index], bg, bg_mask));
      assert(ArGpuBlockMotion_WarpLayers(&actors, cmd, previous, current, phases[phase_index], actor_mask, obj));
      if (!phase_index) assert(!global.output && !actors.output && !actors.attributes);
      assert(ArGpuGlobalMotion_Warp(&global, cmd, previous, current, phases[phase_index]));
      assert(ArGpuBlockMotion_Warp(&actors, cmd, previous, current, phases[phase_index], actor_mask));
      for (unsigned i = 0; i < N; ++i) {
        bool is_global = i < 8;
        unsigned plane = is_global ? i : (i - 8) % 4;
        if (!((is_global ? bg_mask : actor_mask) & (1u << plane))) continue;
        unpack[count++] = (ArGpuFrameUnpack){
            .source = is_global ? global.output : i < 12 ? actors.output : actors.attributes,
            .destination = reference[i], .source_width = is_global ? W : width,
            .source_height = height * (is_global ? 8 : 4),
            .destination_width = W, .destination_height = H,
            .source_region = {0, (int)(plane * height), (int)(is_global ? width - i : width), (int)height},
            .destination_origin = {(int)(is_global ? bg[plane].x : obj[plane].x), 0},
            .clear_destination = phase_index == 0};
      }
      assert(!count || ArGpuFrameHandoff_EncodeUnpack(cmd, unpack, count));
      copy = SDL_BeginGPUCopyPass(cmd);
      assert(copy);
      for (unsigned side = 0; side < 2; ++side)
        for (unsigned i = 0; i < N; ++i) {
          const SDL_GPUTextureRegion src = {.texture = side ? direct[i] : reference[i], .w = W, .h = H, .d = 1};
          const SDL_GPUTextureTransferInfo dst = {.transfer_buffer = download,
              .offset = (side * N + i) * Bytes, .pixels_per_row = W, .rows_per_layer = H};
          SDL_DownloadFromGPUTexture(copy, &src, &dst);
        }
      SDL_EndGPUCopyPass(copy);
      Wait(gpu, cmd);
      const uint32_t *pixels = SDL_MapGPUTransferBuffer(gpu, download, false);
      assert(pixels);
      for (unsigned i = 0; i < N * Pixels; ++i) {
        if (pixels[i] != pixels[N * Pixels + i]) {
          unsigned delta = 0;
          for (unsigned c = 0; c < 4; ++c) {
            unsigned d = (unsigned)abs((int)((pixels[i] >> (c * 8)) & 255) -
                (int)((pixels[N * Pixels + i] >> (c * 8)) & 255));
            if (d > delta) delta = d;
          }
          /* Reprojecting the actor mesh into a different-sized render target
           * can round bilinear UNORM channels by one step. Global compute and
           * both integer endpoints must remain bit-exact. */
          unsigned tolerance = i / Pixels >= 8 && phase_index > 0 && phase_index < 4 ? 1 : 0;
          if (delta > tolerance)
            fprintf(stderr, "direct mismatch geometry=%u phase=%.2f plane=%u xy=%u,%u ref=%08x direct=%08x\n",
                geometry, phases[phase_index], i / Pixels, i % W, (i % Pixels) / W, pixels[i], pixels[N * Pixels + i]);
          assert(delta <= tolerance);
          ++rounded_pixels;
          if (delta > maximum_delta) maximum_delta = delta;
        }
      }
      SDL_UnmapGPUTransferBuffer(gpu, download);
      ++cases;
      if (benchmark && geometry == 1 && phase_index == 4) {
        Benchmark(gpu, &global, &actors, previous, current, bg, obj, unpack, 0xc9, 5);
        Benchmark(gpu, &global, &actors, previous, current, bg, obj, unpack, 255, 15);
      }
    }
    SDL_ReleaseGPUTexture(gpu, previous); SDL_ReleaseGPUTexture(gpu, current);
  }
  for (unsigned i = 0; i < N; ++i) {
    SDL_ReleaseGPUTexture(gpu, reference[i]); SDL_ReleaseGPUTexture(gpu, direct[i]);
  }
  SDL_ReleaseGPUTransferBuffer(gpu, upload); SDL_ReleaseGPUTransferBuffer(gpu, download);
  ArGpuGlobalMotion_Destroy(&global); ArGpuBlockMotion_Destroy(&actors);
  SDL_DestroyGPUDevice(gpu); SDL_Quit();
  printf("GPU direct layers: %u atlas/direct cases, all 16 planes including padding; %u rounded actor pixels, max delta %u/255\n",
      cases, rounded_pixels, maximum_delta);
  return 0;
}
