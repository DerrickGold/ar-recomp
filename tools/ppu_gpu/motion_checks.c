#include "motion.h"
#include "present/presentation_frame_generation.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "motion probe: %s:%d: %s (%s)\n", __FILE__, __LINE__, #x, SDL_GetError()); exit(1); } } while (0)

static SDL_GPUTransferBuffer *ReadTexture(MotionProbe *p, unsigned *pitch) {
  *pitch = (p->width * 4 + 255) & ~255u;
  const SDL_GPUTransferBufferCreateInfo info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = *pitch * p->height * 6};
  SDL_GPUTransferBuffer *buffer = SDL_CreateGPUTransferBuffer(p->device, &info); REQUIRE(buffer);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(p->device); REQUIRE(cmd);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd); REQUIRE(copy);
  const SDL_GPUTextureRegion source = {.texture = p->output, .w = p->width, .h = p->height * 6, .d = 1};
  const SDL_GPUTextureTransferInfo dest = {.transfer_buffer = buffer, .pixels_per_row = *pitch / 4, .rows_per_layer = p->height * 6};
  SDL_DownloadFromGPUTexture(copy, &source, &dest); SDL_EndGPUCopyPass(copy);
  REQUIRE(SDL_SubmitGPUCommandBuffer(cmd)); REQUIRE(SDL_WaitForGPUIdle(p->device)); return buffer;
}
static uint32_t Sample(const uint32_t *pixels, unsigned width, unsigned height, unsigned stride, float x, float y) {
  if (x < 0 || y < 0 || x >= width || y >= height) return 0;
  x = fmaxf(0, fminf((float)width - 1, x - .5f)); y = fmaxf(0, fminf((float)height - 1, y - .5f));
  unsigned x0 = (unsigned)x, y0 = (unsigned)y, x1 = x0 + 1 < width ? x0 + 1 : x0, y1 = y0 + 1 < height ? y0 + 1 : y0;
  float fx = x - x0, fy = y - y0; uint32_t out = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    float a = (float)((pixels[y0 * stride + x0] >> shift) & 255), b = (float)((pixels[y0 * stride + x1] >> shift) & 255);
    float c = (float)((pixels[y1 * stride + x0] >> shift) & 255), d = (float)((pixels[y1 * stride + x1] >> shift) & 255);
    out |= (uint32_t)lroundf((a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy) << shift;
  }
  return out;
}
unsigned MotionProbe_Check(MotionProbe *p, SDL_GPUTexture *previous, SDL_GPUTexture *current,
    const uint32_t *const previous_pixels[6], const uint32_t *const current_pixels[6], unsigned label) {
  PresentationFrameGenerationMotionField field[6] = {0}; unsigned accepted = 0;
  for (unsigned band = 0; band < 6; ++band) {
    if (p->extents[band][1] == 0.0f) continue;
    bool valid = PresentationFrameGeneration_Analyze(previous_pixels[band], current_pixels[band],
        (unsigned)p->extents[band][0], p->height, p->width, p->width, kPresentationFrameGenerationAnalysis_Global, &field[band]);
    accepted += valid;
  }
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(p->device); REQUIRE(cmd);
  const SDL_GPUTransferBufferCreateInfo info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = 6 * sizeof(MotionProbeResult)};
  SDL_GPUTransferBuffer *buffer = SDL_CreateGPUTransferBuffer(p->device, &info); REQUIRE(buffer);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd); REQUIRE(copy);
  const SDL_GPUBufferRegion source = {.buffer = p->motion, .size = info.size};
  const SDL_GPUTransferBufferLocation dest = {.transfer_buffer = buffer};
  SDL_DownloadFromGPUBuffer(copy, &source, &dest); SDL_EndGPUCopyPass(copy);
  REQUIRE(SDL_SubmitGPUCommandBuffer(cmd)); REQUIRE(SDL_WaitForGPUIdle(p->device));
  const MotionProbeResult *results = SDL_MapGPUTransferBuffer(p->device, buffer, false); REQUIRE(results);
  unsigned errors = 0;
  for (unsigned band = 0; band < 6; ++band) {
    const MotionProbeResult expected = {field[band].forward_dx[0], field[band].forward_dy[0],
      field[band].backward_dx[0], field[band].backward_dy[0], field[band].valid, {0}};
    if (memcmp(&results[band], &expected, sizeof(expected))) {
      fprintf(stderr, "motion label=%u band=%u GPU=(%d,%d,%d,%d,%d) CPU=(%d,%d,%d,%d,%d)\n", label, band,
          results[band].forward_x, results[band].forward_y, results[band].backward_x, results[band].backward_y, results[band].valid,
          expected.forward_x, expected.forward_y, expected.backward_x, expected.backward_y, expected.valid); ++errors;
    }
  }
  SDL_UnmapGPUTransferBuffer(p->device, buffer); SDL_ReleaseGPUTransferBuffer(p->device, buffer); REQUIRE(!errors);
  unsigned maximum_error = 0;
  const float phases[] = {0, .25f, .5f, .75f, 1};
  for (unsigned test = 0; test < sizeof(phases) / sizeof(phases[0]); ++test) {
    float phase = phases[test]; cmd = SDL_AcquireGPUCommandBuffer(p->device); REQUIRE(cmd);
    REQUIRE(MotionProbe_Warp(p, cmd, previous, current, phase)); REQUIRE(SDL_SubmitGPUCommandBuffer(cmd));
    unsigned pitch; buffer = ReadTexture(p, &pitch);
    const uint8_t *image = SDL_MapGPUTransferBuffer(p->device, buffer, false); REQUIRE(image);
    for (unsigned band = 0; band < 6; ++band) {
      const unsigned band_width = (unsigned)p->extents[band][0];
      const bool forward = phase < .5f;
      const float scale = forward ? phase : 1 - phase;
      const float dx = (forward ? field[band].forward_dx[0] : field[band].backward_dx[0]) * scale;
      const float dy = (forward ? field[band].forward_dy[0] : field[band].backward_dy[0]) * scale;
      for (unsigned y = 0; y < p->height; ++y) for (unsigned x = 0; x < p->width; ++x) {
        uint32_t expected = field[band].valid ? Sample(forward ? previous_pixels[band] : current_pixels[band],
            band_width, p->height, p->width, x + .5f - dx, y + .5f - dy) : current_pixels[band][y * p->width + x];
        if (x >= band_width) expected = 0;
        const uint8_t *actual = image + ((band * p->height + y) * pitch + x * 4);
        const unsigned shifts[] = {16, 8, 0, 24};
        for (unsigned c = 0; c < 4; ++c) {
          unsigned difference = (unsigned)abs((int)((expected >> shifts[c]) & 255) - actual[c]);
          if (difference > maximum_error) maximum_error = difference;
          // Endpoints and confidence rejection must be exact; fractional filtering
          // can differ by one byte due to device UNORM rounding.
          unsigned tolerance = field[band].valid && phase != 0 && phase != 1 ? 1 : 0;
          if (difference > tolerance && errors++ < 3)
            fprintf(stderr, "warp label=%u phase=%.2f band=%u (%u,%u) channel=%u expected=%u actual=%u\n",
                label, phase, band, x, y, c, (expected >> shifts[c]) & 255, actual[c]);
        }
      }
    }
    SDL_UnmapGPUTransferBuffer(p->device, buffer); SDL_ReleaseGPUTransferBuffer(p->device, buffer);
  }
  printf("motion-parity label=%u accepted=%u/6 vector_errors=0 warp_errors=%u max_filter_error=%u\n", label, accepted, errors, maximum_error);
  REQUIRE(!errors); return accepted;
}

static uint32_t Random(uint32_t *seed) { *seed = *seed * 1664525u + 1013904223u; return *seed; }
static void CheckPatternsAtSize(MotionProbe *p, unsigned width, unsigned height, unsigned count, unsigned label) {
  REQUIRE(SDL_WaitForGPUIdle(p->device)); REQUIRE(MotionProbe_Resize(p, width, height));
  const SDL_GPUTextureCreateInfo info = {.type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
    .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = width, .height = height * 6, .layer_count_or_depth = 1, .num_levels = 1};
  SDL_GPUTexture *textures[2] = {SDL_CreateGPUTexture(p->device, &info), SDL_CreateGPUTexture(p->device, &info)};
  REQUIRE(textures[0] && textures[1]);
  // SDL 3.4.12 Metal uploads use destination.w rather than pixels_per_row.
  // Keep those rows packed. Align other backends to avoid D3D12's repacking
  // fallback, including the deliberately tiny/odd-sized validation textures.
  const bool metal = !strcmp(SDL_GetGPUDeviceDriver(p->device), "metal");
  const unsigned upload_pitch = metal ? width * 4 : (width * 4 + 255) & ~255u;
  const unsigned pixel_count = width * height * 6;
  const SDL_GPUTransferBufferCreateInfo upload_info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = upload_pitch * height * 6};
  SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(p->device, &upload_info); REQUIRE(upload);
  uint32_t *pixels[2] = {malloc(pixel_count * sizeof(uint32_t)), malloc(pixel_count * sizeof(uint32_t))}; REQUIRE(pixels[0] && pixels[1]);
  const int shifts[][2] = {{0,0}, {1,0}, {-1,2}, {7,-7}, {-7,7}, {3,4}, {6,-2}, {-5,-3}};
  unsigned accepted = 0;
  REQUIRE(count <= sizeof(shifts) / sizeof(shifts[0]));
  for (unsigned test = 0; test < count; ++test) {
    for (unsigned band = 0; band < 6; ++band) {
      p->extents[band][0] = (float)(test & 1 ? (width > band * 11 ? width - band * 11 : 1) : width);
      /* Drop a previously moving band, then restore it on the following pair.
       * Unchanged/absent planes must not reuse old GPU confidence or vectors. */
      p->extents[band][1] = test == 2 && band >= 4 ? 0.0f : 1.0f;
    }
    uint32_t seed = 0x12abc987u;
    for (unsigned b = 0; b < 6; ++b) for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
      uint32_t color = Random(&seed);
      if (b == 0) color = 0; // empty / no reliable motion
      else if (b == 1) color = 0xff775599; // uniform / ties
      else if (b == 2) color = 0xff000000u | (((x / 16) * 0x3070u) + ((y / 16) * 0x11u));
      else if (b == 3) color = x % 4 < 2 ? 0xff777777u : 0; // repeating alpha edges
      pixels[0][(b * height + y) * width + x] = color;
    }
    for (unsigned b = 0; b < 6; ++b) for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
      int sx = (int)x - shifts[test][0], sy = (int)y - shifts[test][1];
      uint32_t color = sx >= 0 && sy >= 0 && sx < (int)width && sy < (int)height ? pixels[0][(b * height + sy) * width + sx] : 0;
      if (b == 5 && test == 6) color = Random(&seed); // decorrelated animation / reject
      pixels[1][(b * height + y) * width + x] = color;
    }
    for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
      uint8_t *mapped = SDL_MapGPUTransferBuffer(p->device, upload, true); REQUIRE(mapped);
      memset(mapped, 0, upload_info.size);
      for (unsigned i = 0; i < pixel_count; ++i) {
        unsigned at = (i / width) * upload_pitch + (i % width) * 4;
        uint32_t argb = pixels[endpoint][i]; mapped[at] = (uint8_t)(argb >> 16);
        mapped[at + 1] = (uint8_t)(argb >> 8); mapped[at + 2] = (uint8_t)argb; mapped[at + 3] = (uint8_t)(argb >> 24);
      }
      SDL_UnmapGPUTransferBuffer(p->device, upload);
      SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(p->device); REQUIRE(cmd);
      SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd); REQUIRE(copy);
      const SDL_GPUTextureTransferInfo source = {.transfer_buffer = upload, .pixels_per_row = upload_pitch / 4, .rows_per_layer = height * 6};
      const SDL_GPUTextureRegion dest = {.texture = textures[endpoint], .w = width, .h = height * 6, .d = 1};
      SDL_UploadToGPUTexture(copy, &source, &dest, true); SDL_EndGPUCopyPass(copy); REQUIRE(SDL_SubmitGPUCommandBuffer(cmd));
    }
    const uint32_t *a[6], *b[6]; for (unsigned i = 0; i < 6; ++i) { a[i] = pixels[0] + i * width * height; b[i] = pixels[1] + i * width * height; }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(p->device); REQUIRE(cmd);
    REQUIRE(MotionProbe_Analyze(p, cmd, textures[0], textures[1])); REQUIRE(SDL_SubmitGPUCommandBuffer(cmd));
    accepted += MotionProbe_Check(p, textures[0], textures[1], a, b, label + test);
  }
  if (width > 14 && height > 14) REQUIRE(accepted > 0);
  else REQUIRE(accepted == 0);
  SDL_ReleaseGPUTexture(p->device, textures[0]); SDL_ReleaseGPUTexture(p->device, textures[1]);
  SDL_ReleaseGPUTransferBuffer(p->device, upload); free(pixels[0]); free(pixels[1]);
}
void MotionProbe_CheckPatterns(MotionProbe *p) {
  CheckPatternsAtSize(p, 96, 64, 8, 2000);
  CheckPatternsAtSize(p, 640, 352, 5, 2100);
  CheckPatternsAtSize(p, 13, 9, 2, 2200);
  CheckPatternsAtSize(p, 1, 1, 2, 2300);
}
