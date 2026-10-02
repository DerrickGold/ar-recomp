/* Opt-in GPU background experiment. Compares every RGBA byte against the
 * production CPU PPU, then times completed work without readback in the timed
 * loop. It does not replace live scanout or claim whole-game frame savings. */
#include "packet.h"
#include "motion.h"
#include "action/action_scene_snapshot.h"
#include "action/action_bg_world.h"
#include "platform/sdl/gpu_shader_blob.h"
#include "shaders/ppu_bg_probe_frag.h"
#include "shaders/sim3d_depth_vert.h"
#include "snes/simd.h"
#include "present/presentation_frame_generation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { \
  fprintf(stderr, "probe failed: %s:%d: %s (%s)\n", __FILE__, __LINE__, #x, SDL_GetError()); \
  exit(1); } } while (0)

typedef struct Fixture Fixture;
typedef struct FixtureBinding { Fixture *fixture; unsigned bg; } FixtureBinding;
struct Fixture {
  SrSceneFrame frame;
  uint16_t vram[32768], cgram[256], hscroll[2][224], vscroll[2][224];
  uint8_t mosaic[224];
  SrSceneRowPolicy rows[2][224];
  ActionSceneSnapshot scene;
  ActionBgWorld *world[2];
  FixtureBinding binding[2];
  bool real;
};

typedef struct Gpu {
  SDL_GPUDevice *device;
  SDL_GPUGraphicsPipeline *pipeline;
  SDL_GPUTexture *input, *output;
  SDL_GPUSampler *sampler;
  SDL_GPUBuffer *vertices;
  SDL_GPUTransferBuffer *upload;
  SDL_GPUFence *fences[3];
  unsigned next_fence;
  unsigned width, height;
  SDL_GPUTexture *previous;
  MotionProbe *motion;
} Gpu;

static uint32_t Lookup(void *context, int32_t x, int32_t y, uint16_t *entry) {
  const Fixture *f = ((const FixtureBinding *)context)->fixture;
  const unsigned bg = ((const FixtureBinding *)context)->bg;
  if (x < 0 || y < 0) return 0;
  if (f->real)
    return ActionBgWorld_Lookup(f->world[bg], x, y, entry) == kActionBgLookup_Tile;
  if (x >= 160 || y >= 128 || (x + y) % 11 == 0) return 0;
  *entry = (uint16_t)((x * 7 + y * 13 + bg * 5) % 1024 |
      ((x + y) % 8 << 10) | ((x & 1) << 13) | ((y & 1) << 14) | ((x & 2) << 14));
  return 1;
}
static uint32_t Span(void *context, int32_t x, int32_t y, int32_t step,
                     uint32_t capacity, const uint16_t **entries, int64_t *stride) {
  const FixtureBinding *binding = context;
  ptrdiff_t words = 0;
  const size_t count = ActionBgWorld_LookupSpan(binding->fixture->world[binding->bg],
      x, y, step, capacity, entries, &words);
  *stride = words;
  return (uint32_t)count;
}
static uint32_t Band(void *context, int32_t x, int32_t y, uint16_t entry, uint8_t *band) {
  (void)context; (void)entry;
  *band = (uint8_t)((x + y) % 4); return true;
}
static void Setup(Fixture *f, unsigned phase) {
  memset(&f->frame, 0, sizeof(f->frame));
  f->frame = (SrSceneFrame){.struct_size = sizeof(f->frame), .vram = f->vram,
    .cgram = f->cgram, .mosaic = f->mosaic, .extra_x = phase % 3 == 0 ? 0 : phase % 3 == 1 ? 112 : 128,
    .top = phase % 4 == 0 ? 0 : 64, .bottom = phase % 4 == 0 ? 128 : 64,
    .main_screen = phase % 5 == 0 ? 1 : 3, .sub_screen = phase % 5 == 0 ? 2 : 0,
    .fixed_color = 0x18c3};
  for (unsigned bg = 0; bg < 2; ++bg) {
    f->binding[bg].fixture = f; f->binding[bg].bg = bg;
    SrSceneBackground *b = &f->frame.backgrounds[bg];
    *b = (SrSceneBackground){.tiles = {.lookup = Lookup, .lookup_span = f->real ? Span : NULL,
      .user_data = &f->binding[bg],
      .band_lookup = phase & 1 ? Band : NULL, .camera_x = 51 + (int)phase * 19,
      .camera_y = 128 + (int)phase * 3, .flags = SR_PPU_VIRTUAL_TILEMAP_INCLUDE_AUTHENTIC},
      .rows = f->rows[bg], .hscroll = f->hscroll[bg], .vscroll = f->vscroll[bg],
      .top = UINT16_MAX, .bottom = UINT16_MAX,
      .clip_vertical = phase % 3 == 2, .clip_top = 19, .clip_bottom = 43,
      .capture_flags = SR_PPU_OVERLAY_REMOVE_FROM_GAME |
          (phase & 2 ? SR_PPU_OVERLAY_MARK_BG_HALF_ADD : 0) |
          (phase & 4 ? SR_PPU_OVERLAY_APPLY_BG_FIXED_COLOR_SUBTRACT : 0),
      .fill_configured = true, .fill_mode = phase % 3, .fill_cgram = 17};
    for (unsigned y = 0; y < 224; ++y) {
      f->rows[bg][y] = (SrSceneRowPolicy){.fill = SR_PPU_BACKGROUND_FILL_LIVE_WORLD,
        .motion = SR_PPU_BACKGROUND_MOTION_NORMAL_SCROLL,
        .left = y % 7 ? UINT16_MAX : 23, .right = y % 9 ? UINT16_MAX : 31};
      f->hscroll[bg][y] = (uint16_t)((y / 9 + phase * 11) & 1023);
      f->vscroll[bg][y] = (uint16_t)((y / 29 - (int)phase * 3) & 1023);
    }
  }
}
static bool Load(Fixture *f, const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) return false;
  uint8_t *bytes = malloc(kActionSceneSnapshotMaxBytes + 1);
  if (!bytes) { fclose(file); return false; }
  size_t size = fread(bytes, 1, kActionSceneSnapshotMaxBytes + 1, file);
  bool ok = !ferror(file) && ActionSceneSnapshot_Decode(bytes, size, &f->scene);
  fclose(file); free(bytes);
  for (unsigned bg = 0; ok && bg < 2; ++bg) {
    const ActionRoomSceneBg *b = &f->scene.scene.bg[bg];
    const ActionBgImmutableInput input = {.map = b->map, .map_size = b->map_size,
      .metatiles = b->metatiles, .metatile_size = sizeof(b->metatiles),
      .world_width = b->pages_wide * 256, .world_height = b->pages_high * 256,
      .word_mask = kActionRoomSceneTileWordMask,
      .attributes = (uint8_t)(ActionRoomScene_BgAttributes(&f->scene.scene, bg + 1) >> 8),
      .metatile_words_big_endian = true};
    f->world[bg] = ActionBgWorld_Create();
    ok = f->world[bg] && ActionBgWorld_UpdateImmutable(f->world[bg], &input);
  }
  f->real = ok; return ok;
}
static bool RealFrame(Fixture *f, int x, int y, unsigned clock) {
  ActionRoomSceneFrameRequest request = f->scene.frame;
  request.camera_x = x; request.camera_y = y; request.game_frame = clock;
  ActionRoomSceneFrameState state;
  if (!ActionRoomScene_BuildFrameState(&f->scene.scene, &request, &state)) return false;
  uint8_t chars[kActionRoomSceneCharacterBytes];
  if (!ActionRoomScene_BuildCharacters(&f->scene.scene, clock, state.animation_phase, chars, sizeof(chars))) return false;
  memset(f->vram, 0, sizeof(f->vram)); memset(f->cgram, 0, sizeof(f->cgram));
  for (unsigned i = 0; i < sizeof(chars) / 2; ++i) f->vram[i] = chars[i * 2] | (uint16_t)chars[i * 2 + 1] << 8;
  for (unsigned i = 0; i < sizeof(f->scene.scene.extra_characters) / 2; ++i)
    f->vram[sizeof(chars) / 2 + i] = f->scene.scene.extra_characters[i * 2] |
        (uint16_t)f->scene.scene.extra_characters[i * 2 + 1] << 8;
  for (unsigned i = 0; i < sizeof(f->scene.scene.palette) / 2; ++i)
    f->cgram[i] = f->scene.scene.palette[i * 2] | (uint16_t)f->scene.scene.palette[i * 2 + 1] << 8;
  f->frame.extra_x = 112; f->frame.top = f->frame.bottom = 64;
  f->frame.main_screen = state.screen_enabled[0] & 3; f->frame.sub_screen = state.screen_enabled[1] & 3;
  f->frame.cgwsel = state.cgwsel; f->frame.cgadsub = state.cgadsub; f->frame.fixed_color = state.fixed_color;
  memcpy(f->mosaic, state.mosaic, sizeof(f->mosaic));
  for (unsigned bg = 0; bg < 2; ++bg) {
    SrSceneBackground *b = &f->frame.backgrounds[bg];
    b->tiles.camera_x = state.layer_camera_x[bg]; b->tiles.camera_y = state.layer_camera_y[bg];
    b->tiles.hscroll_anchor = state.layer_camera_x[bg] & 1023;
    b->tiles.vscroll_anchor = state.layer_camera_y[bg] & 1023;
    b->tiles.band_lookup = NULL; b->clip_vertical = false;
    b->capture_flags = SR_PPU_OVERLAY_REMOVE_FROM_GAME; b->fill_mode = 0;
    memcpy(f->hscroll[bg], state.bg_hscroll[bg], sizeof(f->hscroll[bg]));
    memcpy(f->vscroll[bg], state.bg_vscroll[bg], sizeof(f->vscroll[bg]));
    for (unsigned row = 0; row < 224; ++row)
      f->rows[bg][row].left = f->rows[bg][row].right = UINT16_MAX;
  }
  return true;
}

static bool InitGpu(Gpu *g) {
  if (!SDL_Init(SDL_INIT_VIDEO)) return false;
  g->device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL |
      SDL_GPU_SHADERFORMAT_MSL, false, NULL);
  if (!g->device) return false;
  const GpuShaderBlobs blobs = {kPpuBgProbeFragMSL, kPpuBgProbeFragMSLSize,
    kPpuBgProbeFragSPV, kPpuBgProbeFragSPVSize, kPpuBgProbeFragDXIL, kPpuBgProbeFragDXILSize};
  const GpuShaderBlobs vertex_blobs = {kSim3dDepthVertMSL, kSim3dDepthVertMSLSize,
    kSim3dDepthVertSPV, kSim3dDepthVertSPVSize, kSim3dDepthVertDXIL, kSim3dDepthVertDXILSize};
  SDL_GPUShader *frag = GpuShaderBlob_CreateFragment(g->device, &blobs, "PPU BG probe", 1, 0);
  SDL_GPUShader *vert = GpuShaderBlob_Create(g->device, &vertex_blobs, "PPU probe vertex",
      SDL_GPU_SHADERSTAGE_VERTEX, 0, 0); REQUIRE(frag && vert);
  const SDL_GPUVertexBufferDescription vb = {.slot = 0, .pitch = 10 * sizeof(float),
      .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX};
  const SDL_GPUVertexAttribute attributes[] = {
    {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
    {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 4 * sizeof(float)},
    {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 8 * sizeof(float)},
  };
  const SDL_GPUColorTargetDescription color = {.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM};
  const SDL_GPUGraphicsPipelineCreateInfo pipeline = {.vertex_shader = vert, .fragment_shader = frag,
    .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
    .vertex_input_state = {.vertex_buffer_descriptions = &vb, .num_vertex_buffers = 1,
      .vertex_attributes = attributes, .num_vertex_attributes = 3},
    .target_info = {.color_target_descriptions = &color, .num_color_targets = 1}};
  g->pipeline = SDL_CreateGPUGraphicsPipeline(g->device, &pipeline); REQUIRE(g->pipeline);
  SDL_ReleaseGPUShader(g->device, frag); SDL_ReleaseGPUShader(g->device, vert);
  const SDL_GPUTextureCreateInfo input = {.type = SDL_GPU_TEXTURETYPE_2D,
    .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
    .width = kProbeTextureWidth, .height = kProbeTextureHeight, .layer_count_or_depth = 1, .num_levels = 1};
  g->input = SDL_CreateGPUTexture(g->device, &input); REQUIRE(g->input);
  const SDL_GPUSamplerCreateInfo sampler = {.min_filter = SDL_GPU_FILTER_NEAREST,
    .mag_filter = SDL_GPU_FILTER_NEAREST, .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
    .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
    .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
    .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE};
  g->sampler = SDL_CreateGPUSampler(g->device, &sampler); REQUIRE(g->sampler);
  const SDL_GPUTransferBufferCreateInfo transfer = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
      .size = sizeof(((PpuGpuProbePacket *)0)->rgba)};
  g->upload = SDL_CreateGPUTransferBuffer(g->device, &transfer); REQUIRE(g->upload);
  const float triangle[3][10] = {{-1, 1, 0, 1, 1, 1, 1, 1, 0, 0},
      {3, 1, 0, 1, 1, 1, 1, 1, 2, 0}, {-1, -3, 0, 1, 1, 1, 1, 1, 0, 2}};
  const SDL_GPUBufferCreateInfo buffer = {.usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = sizeof(triangle)};
  g->vertices = SDL_CreateGPUBuffer(g->device, &buffer); REQUIRE(g->vertices);
  void *mapped = SDL_MapGPUTransferBuffer(g->device, g->upload, true); REQUIRE(mapped);
  memcpy(mapped, triangle, sizeof(triangle)); SDL_UnmapGPUTransferBuffer(g->device, g->upload);
  SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(g->device); REQUIRE(commands);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands); REQUIRE(copy);
  const SDL_GPUTransferBufferLocation source = {.transfer_buffer = g->upload};
  const SDL_GPUBufferRegion dest = {.buffer = g->vertices, .size = sizeof(triangle)};
  SDL_UploadToGPUBuffer(copy, &source, &dest, false); SDL_EndGPUCopyPass(copy);
  REQUIRE(SDL_SubmitGPUCommandBuffer(commands)); REQUIRE(SDL_WaitForGPUIdle(g->device));
  printf("backend=%s SDL=%d cpu_simd=%d packet_bytes=%zu\n", SDL_GetGPUDeviceDriver(g->device), SDL_GetVersion(), SR_SIMD_AVAILABLE, sizeof(((PpuGpuProbePacket *)0)->rgba));
  return true;
}
static void Prepare(Gpu *g, const PpuGpuProbePacket *p) {
  REQUIRE(p->width && p->height);
  if (g->width != p->width || g->height != p->height) {
    REQUIRE(SDL_WaitForGPUIdle(g->device));
    if (g->output) SDL_ReleaseGPUTexture(g->device, g->output);
    const SDL_GPUTextureCreateInfo output = {.type = SDL_GPU_TEXTURETYPE_2D,
      .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
      .width = p->width, .height = p->height * 6, .layer_count_or_depth = 1, .num_levels = 1};
    g->output = SDL_CreateGPUTexture(g->device, &output); REQUIRE(g->output);
    g->width = p->width; g->height = p->height;
  }
}
static void Draw(Gpu *g, const PpuGpuProbePacket *p, bool upload) {
  /* Bound resource cycling to three submissions. Never benchmark an unbounded
   * driver queue or mistake CPU submission time for completed throughput. */
  SDL_GPUFence **fence = &g->fences[g->next_fence];
  if (*fence) {
    REQUIRE(SDL_WaitForGPUFences(g->device, true, fence, 1));
    SDL_ReleaseGPUFence(g->device, *fence); *fence = NULL;
  }
  if (g->motion) {
    // Render directly into the older endpoint. No full-image copy is needed;
    // SDL resource cycling preserves uses still in flight on the bounded queue.
    SDL_GPUTexture *swap = g->previous; g->previous = g->output; g->output = swap;
  }
  SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(g->device); REQUIRE(commands);
  if (upload) {
    void *mapped = SDL_MapGPUTransferBuffer(g->device, g->upload, true); REQUIRE(mapped);
    memcpy(mapped, p->rgba, sizeof(p->rgba)); SDL_UnmapGPUTransferBuffer(g->device, g->upload);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands); REQUIRE(copy);
    const SDL_GPUTextureTransferInfo source = {.transfer_buffer = g->upload,
      .pixels_per_row = kProbeTextureWidth, .rows_per_layer = kProbeTextureHeight};
    const SDL_GPUTextureRegion dest = {.texture = g->input,
      .w = kProbeTextureWidth, .h = kProbeTextureHeight, .d = 1};
    SDL_UploadToGPUTexture(copy, &source, &dest, true); SDL_EndGPUCopyPass(copy);
  }
  const SDL_GPUColorTargetInfo color = {.texture = g->output, .cycle = true,
    .load_op = SDL_GPU_LOADOP_DONT_CARE, .store_op = SDL_GPU_STOREOP_STORE};
  SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &color, 1, NULL); REQUIRE(pass);
  SDL_BindGPUGraphicsPipeline(pass, g->pipeline);
  const SDL_GPUBufferBinding vertices = {.buffer = g->vertices};
  const SDL_GPUTextureSamplerBinding sampler = {.texture = g->input, .sampler = g->sampler};
  SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
  SDL_BindGPUFragmentSamplers(pass, 0, &sampler, 1);
  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0); SDL_EndGPURenderPass(pass);
  if (g->motion) {
    REQUIRE(MotionProbe_Analyze(g->motion, commands, g->previous, g->output));
    REQUIRE(MotionProbe_Warp(g->motion, commands, g->previous, g->output, .5f));
  }
  *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands); REQUIRE(*fence);
  g->next_fence = (g->next_fence + 1) % 3;
}
static void Compare(Gpu *g, const SrSceneSurfaces *cpu, unsigned phase) {
  REQUIRE(SDL_WaitForGPUIdle(g->device));
  /* D3D12 requires rows aligned to 256 bytes. Readback is validation-only. */
  const unsigned pitch = (g->width * 4 + 255) & ~255u;
  const SDL_GPUTransferBufferCreateInfo info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
    .size = pitch * g->height * 6};
  SDL_GPUTransferBuffer *readback = SDL_CreateGPUTransferBuffer(g->device, &info); REQUIRE(readback);
  SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(g->device); REQUIRE(commands);
  SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands); REQUIRE(copy);
  const SDL_GPUTextureRegion source = {.texture = g->output, .w = g->width, .h = g->height * 6, .d = 1};
  const SDL_GPUTextureTransferInfo dest = {.transfer_buffer = readback,
    .pixels_per_row = pitch / 4, .rows_per_layer = g->height * 6};
  SDL_DownloadFromGPUTexture(copy, &source, &dest); SDL_EndGPUCopyPass(copy);
  REQUIRE(SDL_SubmitGPUCommandBuffer(commands)); REQUIRE(SDL_WaitForGPUIdle(g->device));
  const uint8_t *image = SDL_MapGPUTransferBuffer(g->device, readback, false); REQUIRE(image);
  unsigned errors = 0, opaque = 0;
  REQUIRE(g->width == (unsigned)cpu->pitch_pixels && g->height == (unsigned)cpu->height);
  for (unsigned layer = 0; layer < 2; ++layer) for (unsigned band = 0; band < 3; ++band)
    for (int y = 0; y < cpu->height; ++y) for (int x = 0; x < cpu->pitch_pixels; ++x) {
      uint32_t argb = cpu->bands[layer][band][y * cpu->pitch_pixels + x];
      if (argb >> 24) ++opaque;
      const uint8_t expected[4] = {(uint8_t)(argb >> 16), (uint8_t)(argb >> 8), (uint8_t)argb, (uint8_t)(argb >> 24)};
      const uint8_t *actual = image + ((layer * 3 + band) * cpu->height + y) * pitch + x * 4;
      if (memcmp(expected, actual, 4)) {
        if (errors < 3) fprintf(stderr, "phase=%u bg=%u band=%u x=%d y=%d CPU=%08x GPU=%02x%02x%02x%02x\n",
            phase, layer, band, x, y, argb, actual[3], actual[0], actual[1], actual[2]);
        ++errors;
      }
    }
  SDL_UnmapGPUTransferBuffer(g->device, readback); SDL_ReleaseGPUTransferBuffer(g->device, readback);
  printf("parity phase=%u pixels=%u nontransparent=%u mismatches=%u\n",
      phase, g->width * g->height * 6, opaque, errors);
  REQUIRE(!errors);
}
static double Elapsed(Uint64 start, unsigned count) {
  return (double)(SDL_GetTicksNS() - start) / (1000000.0 * count);
}
static void Benchmark(Gpu *g, Fixture *f, SrSceneRenderer *cpu, PpuGpuProbePacket *p, unsigned n) {
  SrSceneSurfaces surfaces;
  for (unsigned i = 0; i < 16; ++i) Draw(g, p, true);
  REQUIRE(SDL_WaitForGPUIdle(g->device));
  Uint64 start = SDL_GetTicksNS();
  for (unsigned i = 0; i < n; ++i) REQUIRE(sr_scene_renderer_render(cpu, &f->frame, &surfaces));
  double cpu_ms = Elapsed(start, n);
  start = SDL_GetTicksNS();
  for (unsigned i = 0; i < n; ++i) REQUIRE(PpuGpuProbe_Build(&f->frame, p));
  double packet_ms = Elapsed(start, n);
  for (unsigned i = 0; i < 32; ++i) Draw(g, p, false);
  REQUIRE(SDL_WaitForGPUIdle(g->device));
  start = SDL_GetTicksNS();
  for (unsigned i = 0; i < n; ++i) Draw(g, p, false);
  REQUIRE(SDL_WaitForGPUIdle(g->device));
  double resident_ms = Elapsed(start, n);
  for (unsigned i = 0; i < 32; ++i) { REQUIRE(PpuGpuProbe_Build(&f->frame, p)); Draw(g, p, true); }
  REQUIRE(SDL_WaitForGPUIdle(g->device));
  start = SDL_GetTicksNS();
  for (unsigned i = 0; i < n; ++i) {
    REQUIRE(PpuGpuProbe_Build(&f->frame, p)); Draw(g, p, true);
  }
  REQUIRE(SDL_WaitForGPUIdle(g->device));
  double streaming_ms = Elapsed(start, n);
  printf("benchmark iterations=%u cpu_scene_ms=%.6f packet_ms=%.6f resident_completed_ms=%.6f streaming_completed_ms=%.6f\n",
      n, cpu_ms, packet_ms, resident_ms, streaming_ms);
  /* Readback happens only after the timed loops and verifies queued work ran. */
  Compare(g, &surfaces, 999);
}
static void MotionExperiment(Gpu *g, Fixture *f, SrSceneRenderer *cpu, PpuGpuProbePacket *p, unsigned n) {
  MotionProbe motion; REQUIRE(MotionProbe_Init(&motion, g->device));
  MotionProbe_CheckPatterns(&motion);
  Setup(f, 2); f->frame.extra_x = 112;
  if (f->real) REQUIRE(RealFrame(f, 350, 300, 37));
  REQUIRE(PpuGpuProbe_Build(&f->frame, p)); Prepare(g, p);
  REQUIRE(MotionProbe_Resize(&motion, p->width, p->height));
  const SDL_GPUTextureCreateInfo info = {.type = SDL_GPU_TEXTURETYPE_2D,
    .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
    .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
    .width = p->width, .height = p->height * 6, .layer_count_or_depth = 1, .num_levels = 1};
  g->previous = SDL_CreateGPUTexture(g->device, &info); REQUIRE(g->previous);
  const size_t band_bytes = (size_t)p->width * p->height * sizeof(uint32_t);
  uint32_t *previous_cpu = malloc(band_bytes * 6); REQUIRE(previous_cpu);
  const uint32_t *a[6], *b[6];
  const int shifts[][2] = {{0,0},{1,0},{-1,2},{7,-7},{-7,7},{3,4},{6,-2},{-5,-3}};
  unsigned accepted = 0;
  for (unsigned test = 0; test < sizeof(shifts) / sizeof(shifts[0]); ++test) {
    Setup(f, 2); f->frame.extra_x = 112;
    if (f->real) REQUIRE(RealFrame(f, 350, 300, 37));
    REQUIRE(PpuGpuProbe_Build(&f->frame, p));
    SrSceneSurfaces surfaces; REQUIRE(sr_scene_renderer_render(cpu, &f->frame, &surfaces));
    for (unsigned band = 0; band < 6; ++band) {
      a[band] = previous_cpu + band * band_bytes / sizeof(uint32_t);
      memcpy((void *)a[band], surfaces.bands[band / 3][band % 3], band_bytes);
    }
    g->motion = NULL; Draw(g, p, true);
    if (f->real) REQUIRE(RealFrame(f, 350 + shifts[test][0], 300 + shifts[test][1], 37 + (test & 1)));
    else for (unsigned bg = 0; bg < 2; ++bg) {
      f->frame.backgrounds[bg].tiles.camera_x += shifts[test][0];
      f->frame.backgrounds[bg].tiles.camera_y += shifts[test][1];
    }
    REQUIRE(PpuGpuProbe_Build(&f->frame, p)); REQUIRE(sr_scene_renderer_render(cpu, &f->frame, &surfaces));
    for (unsigned band = 0; band < 6; ++band) b[band] = surfaces.bands[band / 3][band % 3];
    g->motion = &motion; Draw(g, p, true);
    accepted += MotionProbe_Check(&motion, g->previous, g->output, a, b, 3000 + test);
  }
  printf("motion-room accepted=%u\n", accepted);
  // The resident pair is fixed here. It has measured motion, unlike an identical
  // pair; this benchmark dispatches the same full search regardless of confidence.
  for (unsigned i = 0; i < 32; ++i) {
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g->device); REQUIRE(cmd);
    REQUIRE(MotionProbe_Analyze(&motion, cmd, g->previous, g->output));
    REQUIRE(MotionProbe_Warp(&motion, cmd, g->previous, g->output, .5f));
    REQUIRE(SDL_SubmitGPUCommandBuffer(cmd)); REQUIRE(SDL_WaitForGPUIdle(g->device));
  }
  PresentationFrameGenerationMotionField field;
  Uint64 start = SDL_GetTicksNS();
  for (unsigned i = 0; i < n; ++i) for (unsigned band = 0; band < 6; ++band)
    (void)PresentationFrameGeneration_Analyze(a[band], b[band], p->width, p->height, p->width, p->width,
        kPresentationFrameGenerationAnalysis_Global, &field);
  double cpu_ms = Elapsed(start, n);
  start = SDL_GetTicksNS();
  for (unsigned i = 0; i < n; ++i) {
    SDL_GPUFence **fence = &g->fences[g->next_fence];
    if (*fence) { REQUIRE(SDL_WaitForGPUFences(g->device, true, fence, 1)); SDL_ReleaseGPUFence(g->device, *fence); }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g->device); REQUIRE(cmd);
    REQUIRE(MotionProbe_Analyze(&motion, cmd, g->previous, g->output));
    REQUIRE(MotionProbe_Warp(&motion, cmd, g->previous, g->output, .5f));
    *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); REQUIRE(*fence); g->next_fence = (g->next_fence + 1) % 3;
  }
  REQUIRE(SDL_WaitForGPUIdle(g->device)); double motion_ms = Elapsed(start, n);
  start = SDL_GetTicksNS();
  for (unsigned i = 0; i < n; ++i) {
    SDL_GPUFence **fence = &g->fences[g->next_fence];
    if (*fence) { REQUIRE(SDL_WaitForGPUFences(g->device, true, fence, 1)); SDL_ReleaseGPUFence(g->device, *fence); }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g->device); REQUIRE(cmd);
    REQUIRE(MotionProbe_Warp(&motion, cmd, g->previous, g->output, i & 1 ? .75f : .25f));
    *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); REQUIRE(*fence); g->next_fence = (g->next_fence + 1) % 3;
  }
  REQUIRE(SDL_WaitForGPUIdle(g->device)); double warp_ms = Elapsed(start, n);
  (void)MotionProbe_Check(&motion, g->previous, g->output, a, b, 3500);
  start = SDL_GetTicksNS();
  for (unsigned i = 0; i < n; ++i) {
    // Alternate neighboring cameras so this is a changing pair, not an idle frame.
    f->frame.backgrounds[0].tiles.camera_x += i & 1 ? -1 : 1;
    REQUIRE(PpuGpuProbe_Build(&f->frame, p)); Draw(g, p, true);
  }
  REQUIRE(SDL_WaitForGPUIdle(g->device)); double combined_ms = Elapsed(start, n);
  // Validate final changing pair after all timed work, without any timed readback.
  const int last_delta = (n - 1) & 1 ? -1 : 1;
  f->frame.backgrounds[0].tiles.camera_x -= last_delta;
  SrSceneSurfaces surfaces; REQUIRE(sr_scene_renderer_render(cpu, &f->frame, &surfaces));
  for (unsigned band = 0; band < 6; ++band)
    memcpy((void *)a[band], surfaces.bands[band / 3][band % 3], band_bytes);
  f->frame.backgrounds[0].tiles.camera_x += last_delta;
  REQUIRE(sr_scene_renderer_render(cpu, &f->frame, &surfaces));
  for (unsigned band = 0; band < 6; ++band) b[band] = surfaces.bands[band / 3][band % 3];
  (void)MotionProbe_Check(&motion, g->previous, g->output, a, b, 4000);
  if (n & 1) --f->frame.backgrounds[0].tiles.camera_x;
  // Matched alternating-camera CPU workload, including its pixel retention.
  // Backdrop/clearing still makes this a scene comparison, not live scanout time.
  REQUIRE(sr_scene_renderer_render(cpu, &f->frame, &surfaces));
  start = SDL_GetTicksNS();
  for (unsigned i = 0; i < n; ++i) {
    for (unsigned band = 0; band < 6; ++band)
      memcpy((void *)a[band], surfaces.bands[band / 3][band % 3], band_bytes);
    f->frame.backgrounds[0].tiles.camera_x += i & 1 ? -1 : 1;
    REQUIRE(sr_scene_renderer_render(cpu, &f->frame, &surfaces));
    for (unsigned band = 0; band < 6; ++band)
      (void)PresentationFrameGeneration_Analyze(a[band], surfaces.bands[band / 3][band % 3],
          p->width, p->height, p->width, p->width, kPresentationFrameGenerationAnalysis_Global, &field);
  }
  double cpu_combined_ms = Elapsed(start, n);
  if (n & 1) --f->frame.backgrounds[0].tiles.camera_x;
  g->motion = NULL; REQUIRE(PpuGpuProbe_Build(&f->frame, p)); Draw(g, p, true); g->motion = &motion;
  start = SDL_GetTicksNS();
  for (unsigned i = 0; i < n; ++i) {
    f->frame.backgrounds[0].tiles.camera_x += i & 1 ? -1 : 1;
    REQUIRE(PpuGpuProbe_Build(&f->frame, p)); Draw(g, p, true);
    if (i & 1) {
      SDL_GPUFence **fence = &g->fences[g->next_fence];
      if (*fence) { REQUIRE(SDL_WaitForGPUFences(g->device, true, fence, 1)); SDL_ReleaseGPUFence(g->device, *fence); }
      SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g->device); REQUIRE(cmd);
      REQUIRE(MotionProbe_Warp(&motion, cmd, g->previous, g->output, .75f));
      *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd); REQUIRE(*fence); g->next_fence = (g->next_fence + 1) % 3;
    }
  }
  REQUIRE(SDL_WaitForGPUIdle(g->device));
  printf("motion-benchmark iterations=%u cpu_global_analysis_ms=%.6f cpu_bg_analysis_ms=%.6f gpu_analysis_warp_completed_ms=%.6f gpu_warp_reuse_completed_ms=%.6f gpu_bg_analysis_warp_completed_ms=%.6f gpu_90hz_equivalent_ms_per_source=%.6f\n",
      n, cpu_ms, cpu_combined_ms, motion_ms, warp_ms, combined_ms, Elapsed(start, n));
  (void)MotionProbe_Check(&motion, g->previous, g->output, a, b, 4500);
  if (n & 1) --f->frame.backgrounds[0].tiles.camera_x;
  g->motion = NULL; free(previous_cpu); MotionProbe_Destroy(&motion);
  SDL_ReleaseGPUTexture(g->device, g->previous); g->previous = NULL;
}
int main(int argc, char **argv) {
  const char *path = NULL;
  bool motion = false;
  unsigned iterations = 160;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--motion")) motion = true;
    else if (!strcmp(argv[i], "--scene") && i + 1 < argc) path = argv[++i];
    else if (!strcmp(argv[i], "--iterations") && i + 1 < argc) {
      char *end; unsigned long n = strtoul(argv[++i], &end, 10);
      if (*end || !n || n > 10000) return 2;
      iterations = (unsigned)n;
    } else { fprintf(stderr, "usage: %s [--scene file.arscene] [--iterations N] [--motion]\n", argv[0]); return 2; }
  }
  Fixture *f = calloc(1, sizeof(*f)); PpuGpuProbePacket *p = malloc(sizeof(*p));
  SrPpuBgPacket *live = malloc(sizeof(*live)); REQUIRE(live);
  SrSceneRenderer *cpu = sr_scene_renderer_create(); REQUIRE(f && p && cpu);
  if (path) REQUIRE(Load(f, path));
  else {
    uint32_t seed = 1234567;
    for (unsigned i = 0; i < 32768; ++i) { seed = seed * 1664525u + 1013904223u; f->vram[i] = (uint16_t)(seed >> 16); }
    for (unsigned i = 0; i < 256; ++i) f->cgram[i] = (uint16_t)(i * 313 + 39) & 32767;
  }
  Gpu g = {0};
  if (!InitGpu(&g)) { fprintf(stderr, "GPU unavailable: %s\n", SDL_GetError()); return 77; }
  for (unsigned phase = 0; phase < 12; ++phase) {
    Setup(f, phase);
    if (path) REQUIRE(RealFrame(f, (int)phase * 71, 256 + (int)phase * 13, phase * 37));
    REQUIRE(PpuGpuProbe_Build(&f->frame, p));
    f->frame.background_packet = live;
    SrSceneSurfaces surfaces; REQUIRE(sr_scene_renderer_render(cpu, &f->frame, &surfaces));
    unsigned checked = 0;
    for (unsigned bg = 0; bg < 2; ++bg) for (unsigned y = 0; y < (unsigned)surfaces.height; ++y) {
      const unsigned row = SrPpuBgPacket_Row(bg, y);
      if (!live->words[SR_PPU_BG_PACKET_HEADER_WORDS + row * SR_PPU_BG_PACKET_ROW_WORDS]) continue;
      for (unsigned band = 0; band < 3; ++band) for (unsigned x = 0; x < (unsigned)surfaces.pitch_pixels; ++x) {
        const uint32_t expected = surfaces.bands[bg][band][y * surfaces.pitch_pixels + x];
        const uint32_t got = SrPpuBgPacket_Color(live, bg, band, x, y);
        if (got != expected) fprintf(stderr, "live packet phase=%u bg=%u band=%u (%u,%u) got=%08x expected=%08x\n", phase, bg, band, x, y, got, expected);
        REQUIRE(got == expected); ++checked;
      }
    }
    REQUIRE(checked); printf("live packet phase=%u checked=%u\n", phase, checked);
    f->frame.background_packet = NULL;
    Prepare(&g, p); Draw(&g, p, true); Compare(&g, &surfaces, phase);
    if (phase == 11) {
      /* The slow pixel sampler is a second oracle, outside the timed baseline. */
      f->frame.reference_renderer = true;
      REQUIRE(sr_scene_renderer_render(cpu, &f->frame, &surfaces));
      Compare(&g, &surfaces, 100);
      f->frame.reference_renderer = false;
      /* Packet replay must not observe later changes to borrowed source data. */
      f->vram[0] ^= 0xffff; f->cgram[1] ^= 0x7fff;
      f->frame.backgrounds[0].tiles.camera_x += 317;
      Draw(&g, p, true); Compare(&g, &surfaces, 101);
      f->vram[0] ^= 0xffff; f->cgram[1] ^= 0x7fff;
      f->frame.backgrounds[0].tiles.camera_x -= 317;
    }
  }
  /* Unsupported policies must never masquerade as a valid GPU job. */
  f->mosaic[100] = 0x11; REQUIRE(!PpuGpuProbe_Build(&f->frame, p)); REQUIRE(!p->width && !p->height);
  f->mosaic[100] = 0;
  f->rows[0][0].fill = SR_PPU_BACKGROUND_FILL_MIRROR; REQUIRE(!PpuGpuProbe_Build(&f->frame, p));
  f->rows[0][0].fill = SR_PPU_BACKGROUND_FILL_LIVE_WORLD;
  REQUIRE(!PpuGpuProbe_Build(NULL, p)); REQUIRE(!p->width && !p->height);
  f->frame.extra_x = 129; REQUIRE(!PpuGpuProbe_Build(&f->frame, p));
  f->frame.extra_x = path ? 112 : 128;
  f->frame.backgrounds[0].capture_flags |= SR_PPU_OVERLAY_MARK_MAIN_SCREEN_WINNER;
  REQUIRE(!PpuGpuProbe_Build(&f->frame, p));
  f->frame.backgrounds[0].capture_flags &= ~SR_PPU_OVERLAY_MARK_MAIN_SCREEN_WINNER;
  REQUIRE(PpuGpuProbe_Build(&f->frame, p)); Prepare(&g, p);
  Benchmark(&g, f, cpu, p, iterations);
  if (motion) MotionExperiment(&g, f, cpu, p, iterations);
  REQUIRE(SDL_WaitForGPUIdle(g.device));
  for (unsigned i = 0; i < 3; ++i) if (g.fences[i]) SDL_ReleaseGPUFence(g.device, g.fences[i]);
  SDL_ReleaseGPUGraphicsPipeline(g.device, g.pipeline); SDL_ReleaseGPUSampler(g.device, g.sampler);
  SDL_ReleaseGPUBuffer(g.device, g.vertices); SDL_ReleaseGPUTransferBuffer(g.device, g.upload);
  SDL_ReleaseGPUTexture(g.device, g.input); SDL_ReleaseGPUTexture(g.device, g.output);
  SDL_DestroyGPUDevice(g.device); SDL_Quit();
  sr_scene_renderer_destroy(cpu);
  for (unsigned bg = 0; bg < 2; ++bg) ActionBgWorld_Destroy(f->world[bg]);
  free(live); free(p); free(f);
  return 0;
}
