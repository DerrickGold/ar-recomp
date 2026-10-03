#include "diorama/diorama_bg_gpu.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include "app/performance_metrics.h"
#include "platform/sdl/render_sdl_internal.h"
#include "platform/sdl/presentation_geometry_sdl.h"
#include "shaders/ppu_bg_capture_frag.h"
#include "shaders/ppu_bg_capture_comp.h"

enum { kTextureWidth = 256,
    kTextureHeight = (SR_PPU_BG_PACKET_WORDS + kTextureWidth - 1) / kTextureWidth };
_Static_assert(SR_PPU_BG_PACKET_ARENA_BASE == 8456, "Update ppu_bg_capture shader layout");

static struct {
  SDL_Renderer *renderer;
  SDL_GPURenderState *state;
  SDL_Texture *input, *planes[3][3];
  SDL_GPUDevice *gpu;
  SDL_GPUComputePipeline *decode;
  SDL_GPUBuffer *packet;
  SDL_GPUTransferBuffer *upload;
  SDL_GPUTexture *native_planes[3][3];
  SrPpuBgPacket *previous;
  uint32_t valid_mask;
  bool attempted, ready;
} s_bg;

static uint32_t SourceMask(unsigned bg);

static void ReleaseNative(void) {
  if (!s_bg.gpu) return;
  for (unsigned bg = 0; bg < 3; ++bg) for (unsigned band = 0; band < 3; ++band)
    if (s_bg.native_planes[bg][band])
      SDL_ReleaseGPUTexture(s_bg.gpu, s_bg.native_planes[bg][band]);
  if (s_bg.decode) SDL_ReleaseGPUComputePipeline(s_bg.gpu, s_bg.decode);
  if (s_bg.packet) SDL_ReleaseGPUBuffer(s_bg.gpu, s_bg.packet);
  if (s_bg.upload) SDL_ReleaseGPUTransferBuffer(s_bg.gpu, s_bg.upload);
  s_bg.gpu = NULL; s_bg.decode = NULL; s_bg.packet = NULL; s_bg.upload = NULL;
  memset(s_bg.native_planes, 0, sizeof(s_bg.native_planes));
}

static bool EnsureNative(ArRenderDevice *device) {
  const char *mode = getenv("AR_GPU_BG_DECODE");
  const ArSdlRenderBackend *backend = device->context;
  // Native submissions must be ordered with all prior Renderer consumers.
  if ((mode && strcmp(mode, "native")) || !backend->output_window || !backend->gpu_device)
    return false;
  s_bg.gpu = backend->gpu_device;
  const SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
  if (!SDL_GPUTextureSupportsFormat(s_bg.gpu, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
          SDL_GPU_TEXTURETYPE_2D, usage)) { ReleaseNative(); return false; }
  static const GpuShaderBlobs blobs = {kPpuBgCaptureCompMSL, kPpuBgCaptureCompMSLSize,
      kPpuBgCaptureCompSPV, kPpuBgCaptureCompSPVSize,
      kPpuBgCaptureCompDXIL, kPpuBgCaptureCompDXILSize};
  s_bg.decode = GpuShaderBlob_CreateCompute(s_bg.gpu, &blobs, 0, 1, 0, 3, 1, 8, 8);
  const SDL_GPUBufferCreateInfo packet = {.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
      .size = sizeof(s_bg.previous->words)};
  const SDL_GPUTransferBufferCreateInfo upload = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
      .size = sizeof(s_bg.previous->words)};
  s_bg.packet = SDL_CreateGPUBuffer(s_bg.gpu, &packet);
  s_bg.upload = SDL_CreateGPUTransferBuffer(s_bg.gpu, &upload);
  if (!s_bg.decode || !s_bg.packet || !s_bg.upload) { ReleaseNative(); return false; }
  SDL_Log("[gpu-bg-decode] native storage-buffer compute active");
  return true;
}

static bool EnsureNativePlane(unsigned bg, unsigned band, int width, int height) {
  SDL_Texture **output = &s_bg.planes[bg][band];
  SDL_GPUTexture **texture = &s_bg.native_planes[bg][band];
  float old_width = 0, old_height = 0;
  if (*output && (!SDL_GetTextureSize(*output, &old_width, &old_height) ||
      old_width != width || old_height != height)) {
    SDL_DestroyTexture(*output); *output = NULL;
    SDL_ReleaseGPUTexture(s_bg.gpu, *texture); *texture = NULL;
    s_bg.valid_mask &= ~SourceMask(bg);
  }
  if (*texture) return true;
  const SDL_GPUTextureCreateInfo info = {.type = SDL_GPU_TEXTURETYPE_2D,
      .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
      .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
          SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
      .width = (Uint32)width, .height = (Uint32)height, .layer_count_or_depth = 1,
      .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1};
  *texture = SDL_CreateGPUTexture(s_bg.gpu, &info);
  if (!*texture) return false;
  SDL_PropertiesID props = SDL_CreateProperties();
  bool ok = props && SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, *texture) &&
      SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_RGBA32) &&
      SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_TARGET) &&
      SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width) &&
      SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
  if (ok) *output = SDL_CreateTextureWithProperties(s_bg.renderer, props);
  SDL_DestroyProperties(props);
  ok = *output && SDL_SetTextureBlendMode(*output, bg == 2 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND) &&
      SDL_SetTextureScaleMode(*output, bg == 2 ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
  if (!ok) {
    SDL_DestroyTexture(*output); *output = NULL;
    SDL_ReleaseGPUTexture(s_bg.gpu, *texture); *texture = NULL;
  }
  return ok;
}

static const unsigned kPlanes[3][3] = {
  {SR_PPU_OVERLAY_BG1, kDioramaPlane_Bg1Hi, kDioramaPlane_Bg1Far},
  {SR_PPU_OVERLAY_BG2, kDioramaPlane_Bg2Hi, kDioramaPlane_Bg2Far},
  {kDioramaPlane_Count, 0, 0},
};

static uint32_t SourceMask(unsigned bg) {
  if (bg == 2) return 1u << kDioramaPlane_Count;
  return (1u << kPlanes[bg][0]) | (1u << kPlanes[bg][1]) | (1u << kPlanes[bg][2]);
}

static bool PacketBoundsValid(const SrPpuBgPacket *p) {
  const unsigned used = p->words[3];
  if (used < SR_PPU_BG_PACKET_ARENA_BASE || used > SR_PPU_BG_PACKET_WORDS ||
      (p->words[2] & ~7u)) return false;
  for (unsigned source = 0; source < 3; ++source) {
    const unsigned height = p->words[source == 2 ? 5 : 1];
    const unsigned width = p->words[source == 2 ? 4 : 0];
    if (width > SR_PPU_BG_PACKET_WIDTH || height > SR_PPU_BG_PACKET_HEIGHT) return false;
    for (unsigned y = 0; y < height; ++y) {
      const uint32_t *row = p->words + SR_PPU_BG_PACKET_HEADER_WORDS +
          SrPpuBgPacket_Row(source, y) * SR_PPU_BG_PACKET_ROW_WORDS;
      if (!row[0]) continue;
      if (row[0] > 4 || (row[0] >= 3 && row[4] > 7) || (source == 2 && row[0] != 2) ||
          row[6] < SR_PPU_BG_PACKET_ARENA_BASE || row[6] > used - 256 ||
          (row[7] && (row[7] < SR_PPU_BG_PACKET_ARENA_BASE ||
              row[7] > used - SrPpuBgPacket_RowWords(row, source))) ||
          (row[0] == 4 && row[5] && (row[5] < SR_PPU_BG_PACKET_ARENA_BASE ||
              row[5] > used - SR_PPU_BG_PACKET_PIXEL_STRIDE))) return false;
      if (source == 2 && row[4]) {
        const unsigned first = row[4] & 65535u, end = row[4] >> 16;
        const int64_t start = (int32_t)row[5] + (int64_t)first;
        const int64_t last = (int32_t)row[5] + (int64_t)end - 1;
        if (p->words[6] > 1 || first >= end || end > width || start < 0 ||
            last >= (int64_t)p->words[1] * SR_PPU_BG_PACKET_WIDTH) return false;
        for (unsigned alias_y = (unsigned)start / SR_PPU_BG_PACKET_WIDTH;
             alias_y <= (unsigned)last / SR_PPU_BG_PACKET_WIDTH; ++alias_y)
          if (!p->words[SR_PPU_BG_PACKET_HEADER_WORDS +
              SrPpuBgPacket_Row(p->words[6], alias_y) * SR_PPU_BG_PACKET_ROW_WORDS]) return false;
      }
    }
  }
  return true;
}

static bool SourceChanged(const SrPpuBgPacket *p, unsigned bg) {
  const SrPpuBgPacket *old = s_bg.previous;
  if (!s_bg.valid_mask || p->words[0] != old->words[0] || p->words[1] != old->words[1] ||
      ((p->words[2] ^ old->words[2]) & (1u << bg))) return true;
  if (bg == 2 && memcmp(p->words + 4, old->words + 4, 3 * sizeof(uint32_t))) return true;
  const unsigned height = p->words[bg == 2 ? 5 : 1];
  for (unsigned y = 0; y < height; ++y) {
    const unsigned index = SR_PPU_BG_PACKET_HEADER_WORDS + SrPpuBgPacket_Row(bg, y) * SR_PPU_BG_PACKET_ROW_WORDS;
    const uint32_t *now = p->words + index, *before = old->words + index;
    if (memcmp(now, before, 6 * sizeof(uint32_t))) return true;
    if (!now[0]) continue;
    if (!now[6] || !before[6] || memcmp(p->words + now[6], old->words + before[6], 256 * sizeof(uint32_t))) return true;
    if (!!now[7] != !!before[7]) return true;
    if (now[7] && memcmp(p->words + now[7], old->words + before[7],
        SrPpuBgPacket_RowWords(now, bg) * sizeof(uint32_t))) return true;
    if (now[0] == 4 && now[5] && memcmp(p->words + now[5], old->words + before[5],
        SR_PPU_BG_PACKET_PIXEL_STRIDE * sizeof(uint32_t))) return true;
  }
  return false;
}

void DioramaBgGpu_Reset(ArRenderDevice *device) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (s_bg.gpu && renderer == s_bg.renderer) (void)ArSdlRenderBackend_SubmitPending(device);
  if (renderer && renderer == s_bg.renderer) (void)SDL_SetGPURenderState(renderer, NULL);
  SDL_DestroyGPURenderState(s_bg.state);
  SDL_DestroyTexture(s_bg.input);
  for (unsigned bg = 0; bg < 3; ++bg) for (unsigned band = 0; band < 3; ++band)
    SDL_DestroyTexture(s_bg.planes[bg][band]);
  ReleaseNative();
  free(s_bg.previous);
  memset(&s_bg, 0, sizeof(s_bg));
}

static bool Ensure(ArRenderDevice *device) {
  SDL_Renderer *renderer = ArSdlRenderBackend_Renderer(device);
  if (!renderer || (s_bg.renderer && s_bg.renderer != renderer)) return false;
  if (s_bg.attempted) return s_bg.ready;
  s_bg.attempted = true; s_bg.renderer = renderer;
  s_bg.previous = malloc(sizeof(*s_bg.previous));
  if (!s_bg.previous) return false;
  if (EnsureNative(device)) { s_bg.ready = true; return true; }
  static const GpuShaderBlobs blobs = {kPpuBgCaptureFragMSL, kPpuBgCaptureFragMSLSize,
      kPpuBgCaptureFragSPV, kPpuBgCaptureFragSPVSize,
      kPpuBgCaptureFragDXIL, kPpuBgCaptureFragDXILSize};
  SDL_GPUShader *shader = ArSdlRenderBackend_FragmentShader(device, &blobs,
      "PPU background capture", 1, 1);
  if (!shader) return false;
  const SDL_GPURenderStateCreateInfo state = {.fragment_shader = shader};
  s_bg.state = SDL_CreateGPURenderState(renderer, &state);
  s_bg.input = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
      SDL_TEXTUREACCESS_STREAMING, kTextureWidth, kTextureHeight);
  if (!s_bg.state || !s_bg.input || !s_bg.previous) return false;
  s_bg.ready = SDL_SetTextureBlendMode(s_bg.input, SDL_BLENDMODE_NONE) &&
      SDL_SetTextureScaleMode(s_bg.input, SDL_SCALEMODE_NEAREST);
  return s_bg.ready;
}

static uint32_t ResolveNative(ArRenderDevice *device, const SrPpuBgPacket *p,
    uint32_t requested, ArRenderTexture textures[kDioramaPlane_Count],
    ArRenderTexture *skybox, uint32_t *changed) {
  unsigned widths[3], heights[3], bands[3] = {0};
  for (unsigned bg = 0; bg < 3; ++bg) {
    if (!(requested & SourceMask(bg))) continue;
    const bool periodic = bg == 2 && p->words[4] == 256 && p->words[5] == 256;
    widths[bg] = periodic ? 256 : SR_PPU_BG_PACKET_WIDTH;
    heights[bg] = periodic ? 256 : SR_PPU_BG_PACKET_HEIGHT;
    for (unsigned band = 0; band < 3; ++band) {
      // The sky dispatch writes only band zero; its other bindings are distinct
      // one-pixel placeholders, never aliases of a texture being written.
      const bool dummy = bg == 2 && band != 0;
      if (!EnsureNativePlane(bg, band, dummy ? 1 : (int)widths[bg],
              dummy ? 1 : (int)heights[bg])) return 0;
    }
    for (unsigned band = 0; band < (bg == 2 ? 1u : 3u); ++band)
      if ((requested & ~s_bg.valid_mask) & (1u << kPlanes[bg][band])) bands[bg] |= 1u << band;
  }
  if (bands[0] || bands[1] || bands[2]) {
    const Uint32 size = p->words[3] * (Uint32)sizeof(uint32_t);
    uint32_t *data = SDL_MapGPUTransferBuffer(s_bg.gpu, s_bg.upload, true);
    if (!data) return 0;
    /* The packet is already in GPU byte order on little-endian hosts. A
     * word-at-a-time loop cannot assume this mapped pointer doesn't alias p,
     * and compiled to scalar stores plus a size reload on every word on Deck.
     * The transfer allocation is separate; use the platform's bulk copy. */
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
    memcpy(data, p->words, size);
#else
    const unsigned words = size / sizeof(uint32_t);
    for (unsigned i = 0; i < words; ++i) data[i] = SDL_Swap32LE(p->words[i]);
#endif
    SDL_UnmapGPUTransferBuffer(s_bg.gpu, s_bg.upload);
    if (!ArSdlRenderBackend_SubmitPending(device)) return 0;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(s_bg.gpu);
    if (!cmd) return 0;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) { SDL_CancelGPUCommandBuffer(cmd); return 0; }
    const SDL_GPUTransferBufferLocation source = {.transfer_buffer = s_bg.upload};
    const SDL_GPUBufferRegion destination = {.buffer = s_bg.packet, .size = size};
    SDL_UploadToGPUBuffer(copy, &source, &destination, true);
    SDL_EndGPUCopyPass(copy);
    for (unsigned bg = 0; bg < 3; ++bg) {
      if (!bands[bg]) continue;
      SDL_GPUStorageTextureReadWriteBinding outputs[3] = {0};
      for (unsigned band = 0; band < 3; ++band) outputs[band].texture = s_bg.native_planes[bg][band];
      SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(cmd, outputs, 3, NULL, 0);
      if (!pass) { SDL_CancelGPUCommandBuffer(cmd); return 0; }
      const Uint32 uniform[4] = {bg, bands[bg], widths[bg], heights[bg]};
      SDL_PushGPUComputeUniformData(cmd, 0, uniform, sizeof(uniform));
      SDL_BindGPUComputePipeline(pass, s_bg.decode);
      SDL_BindGPUComputeStorageBuffers(pass, 0, &s_bg.packet, 1);
      SDL_DispatchGPUCompute(pass, (widths[bg] + 7) / 8, (heights[bg] + 7) / 8, 1);
      SDL_EndGPUComputePass(pass);
    }
    if (!SDL_SubmitGPUCommandBuffer(cmd)) return 0;
    PerformanceMetrics_AddTextureUpload(1, size);
  }
  for (unsigned bg = 0; bg < 3; ++bg) {
    for (unsigned band = 0; band < (bg == 2 ? 1u : 3u); ++band) {
      const unsigned plane = kPlanes[bg][band], bit = 1u << plane;
      if (!(requested & bit)) continue;
      if ((bands[bg] & (1u << band)) && changed) *changed |= bit;
      if (bg == 2) *skybox = ArSdlRenderBackend_BorrowTexture(s_bg.planes[bg][band]);
      else textures[plane] = ArSdlRenderBackend_BorrowTexture(s_bg.planes[bg][band]);
    }
  }
  s_bg.valid_mask |= requested;
  memcpy(s_bg.previous, p, SrPpuBgPacket_Size(p));
  return requested;
}

uint32_t DioramaBgGpu_Resolve(ArRenderDevice *device, const SrPpuBgPacket *p,
    uint32_t requested, ArRenderTexture textures[kDioramaPlane_Count], ArRenderTexture *skybox, uint32_t *changed) {
  if (changed) *changed = 0;
  if (!p || !textures || !p->words[2] || p->words[0] > SR_PPU_BG_PACKET_WIDTH ||
      !p->words[0] || !p->words[1] || p->words[1] > SR_PPU_BG_PACKET_HEIGHT ||
      !PacketBoundsValid(p)) return 0;
  if (skybox) { *skybox = ArRenderTexture_Invalid(); requested |= SourceMask(2); }
  uint32_t eligible = 0;
  for (unsigned bg = 0; bg < 3; ++bg) {
    if (!(p->words[2] & (1u << bg))) continue;
    const unsigned width = p->words[bg == 2 ? 4 : 0], height = p->words[bg == 2 ? 5 : 1];
    bool complete = width && width <= SR_PPU_BG_PACKET_WIDTH && height && height <= SR_PPU_BG_PACKET_HEIGHT;
    for (unsigned y = 0; complete && y < height; ++y)
      complete &= p->words[SR_PPU_BG_PACKET_HEADER_WORDS +
          SrPpuBgPacket_Row(bg, y) * SR_PPU_BG_PACKET_ROW_WORDS] != 0;
    if (complete) eligible |= SourceMask(bg);
  }
  requested &= eligible;
  if (!requested || !Ensure(device)) return 0;
  uint32_t dirty_mask = 0;
  for (unsigned bg = 0; bg < 3; ++bg)
    if (SourceChanged(p, bg)) dirty_mask |= SourceMask(bg);
  if ((p->words[2] & 4u) && p->words[6] < 2 && (dirty_mask & SourceMask(p->words[6])))
    dirty_mask |= SourceMask(2);
  const bool dirty = (dirty_mask & requested) != 0 || (requested & ~s_bg.valid_mask) != 0;
  if (s_bg.gpu) {
    s_bg.valid_mask &= ~dirty_mask;
    const uint32_t resolved = ResolveNative(device, p, requested, textures, skybox, changed);
    if (!resolved) {
      SDL_Log("[gpu-bg-decode] native resolve failed: %s", SDL_GetError());
      s_bg.valid_mask = 0;
      s_bg.ready = false; // Use the caller's CPU fallback until renderer reset.
    }
    return resolved;
  }
  if (dirty) {
    void *data = NULL; int pitch = 0;
    const unsigned words = p->words[3];
    if (words < SR_PPU_BG_PACKET_ARENA_BASE || words > SR_PPU_BG_PACKET_WORDS) return 0;
    const SDL_Rect upload = {0, 0, kTextureWidth, (int)((words + kTextureWidth - 1) / kTextureWidth)};
    if (!SDL_LockTexture(s_bg.input, &upload, &data, &pitch)) return 0;
    for (unsigned y = 0; y < (unsigned)upload.h; ++y) {
      uint32_t *out = (uint32_t *)((uint8_t *)data + y * pitch);
      for (unsigned x = 0; x < kTextureWidth; ++x) {
        const unsigned i = y * kTextureWidth + x;
        out[x] = SDL_Swap32LE(i < words ? p->words[i] : 0);
      }
    }
    SDL_UnlockTexture(s_bg.input);
    PerformanceMetrics_AddTextureUpload(1, (uint64_t)kTextureWidth * (unsigned)upload.h * 4);
  }
  s_bg.valid_mask &= ~dirty_mask;
  SDL_Renderer *renderer = s_bg.renderer;
  SDL_Texture *target = SDL_GetRenderTarget(renderer);
  ArSdlPresentationOutputState state;
  if (!ArSdlPresentation_PushFullOutput(renderer, &state)) return 0;
  uint32_t resolved = 0;
  for (unsigned bg = 0; bg < 3; ++bg) {
    if (!(p->words[2] & (1u << bg))) continue;
    for (unsigned band = 0; band < (bg == 2 ? 1u : 3u); ++band) {
      const unsigned plane = kPlanes[bg][band], bit = 1u << plane;
      if (!(requested & bit)) continue;
      SDL_Texture **output = &s_bg.planes[bg][band];
      const bool periodic = bg == 2 && p->words[4] == 256 && p->words[5] == 256;
      const int output_width = periodic ? 256 : SR_PPU_BG_PACKET_WIDTH;
      const int output_height = periodic ? 256 : SR_PPU_BG_PACKET_HEIGHT;
      float old_width = 0, old_height = 0;
      if (*output && (!SDL_GetTextureSize(*output, &old_width, &old_height) ||
          old_width != output_width || old_height != output_height)) {
        SDL_DestroyTexture(*output); *output = NULL; s_bg.valid_mask &= ~bit;
      }
      if (!*output) {
        *output = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_TARGET, output_width, output_height);
        if (!*output || !SDL_SetTextureBlendMode(*output, bg == 2 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND) ||
            !SDL_SetTextureScaleMode(*output, bg == 2 ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST)) {
          SDL_DestroyTexture(*output);
          *output = NULL;
          continue;
        }
      }
      if (!(s_bg.valid_mask & bit)) {
        const float uniform[4] = {(float)bg, (float)band, (float)output_width, (float)output_height};
        bool ok = SDL_SetRenderTarget(renderer, *output) &&
            SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED) &&
            SDL_SetRenderViewport(renderer, NULL) && SDL_SetRenderClipRect(renderer, NULL) &&
            SDL_SetGPURenderStateFragmentUniforms(s_bg.state, 0, uniform, sizeof(uniform)) &&
            SDL_SetGPURenderState(renderer, s_bg.state) &&
            SDL_RenderTexture(renderer, s_bg.input, NULL, NULL);
        if (!ok) continue;
        s_bg.valid_mask |= bit;
        if (changed) *changed |= bit;
      }
      if (bg == 2) *skybox = ArSdlRenderBackend_BorrowTexture(*output);
      else textures[plane] = ArSdlRenderBackend_BorrowTexture(*output);
      resolved |= bit;
    }
  }
  const bool unbound = SDL_SetGPURenderState(renderer, NULL);
  const bool restored = SDL_SetRenderTarget(renderer, target);
  ArSdlPresentation_PopFullOutput(renderer, &state);
  if (!unbound || !restored) { s_bg.valid_mask = 0; return 0; }
  memcpy(s_bg.previous, p, SrPpuBgPacket_Size(p));
  return resolved;
}
