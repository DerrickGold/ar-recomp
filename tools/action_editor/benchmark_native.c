/* Complete-room native GPU benchmark. No gameplay, swapchain presentation or
 * pixel readbacks in timed frames. Completion includes a deliberate GPU-idle
 * wait, so it measures serial work, not the game's overlapped frame pacing. */
#include <SDL3/SDL.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "benchmark_probe.h"
#include "room_preview_native.h"
#include "platform/sdl/action_effect_source_sdl.h"
#include "platform/sdl/render_sdl_interop.h"
#include "render/gpu_compile_log.h"
#include "host/frame_producer.h"

/* Room packets are small; authored scenery documents can exceed 1 MiB. */
static uint8_t s_input[8 * 1024 * 1024 + 1];
static ArRenderDevice s_device;
static ArGpuEffectSource s_source;
static SDL_GPUBuffer *s_motion;
static uint64_t s_capture, s_prepared;
static unsigned s_packet, s_primitives;
/* Optional diagnostics, sampled on the render owner. GPU completion remains a
 * wall-clock wait, not a GPU timestamp. Logging happens after that wait. */
static bool s_profile;
static uint64_t s_prepared_cpu;
static struct {
  uint64_t effects_ns, effects_cpu_ns, sky_ns, sky_cpu_ns;
  unsigned packets, light_jobs, shadow_rectangles, primitive_bytes;
} s_profile_frame;
static uint64_t CpuNow(void) { return s_profile ? HostFrameProducer_ThreadCpuTimeNs() : 0; }
uint8_t *DioramaPreview_Input(void) { return s_input; }
unsigned DioramaPreview_Capacity(void) { return sizeof(s_input); }
ArRenderDevice *DioramaPreview_Device(void) { return &s_device; }
void DioramaPreview_Reset(void) { Diorama_ResetCompositorResources(&s_device); }
bool DioramaPreview_Begin(int width, int height) {
  (void)width;
  (void)height;
  s_prepared = SDL_GetPerformanceCounter();
  s_prepared_cpu = CpuNow();
  return ArRenderDevice_SetRenderTarget(&s_device, ArRenderTexture_Invalid()) &&
         ArRenderDevice_SetViewport(&s_device, NULL) &&
         ArRenderDevice_SetClipRect(&s_device, NULL) &&
         ArRenderDevice_Clear(&s_device, (ArRenderColorF){0, 0, 0, 1});
}
static bool Effects(ArRenderDevice *device, const ActionEffectSourceBatch *batch,
                    const DioramaProjection *view, const ActionMoonlightOcclusion *scenery,
                    ArRenderBlendMode blend, float brightness) {
  s_primitives += batch->count;
  ActionEffectSourceBatch source = *batch;
  if (s_packet < kActionSourcePacketSlots) {
    source.packet_slot = s_packet;
    source.revision = s_capture * kActionSourcePacketSlots + s_packet + 1;
  }
  ++s_packet;
  const uint64_t cpu = CpuNow(), wall = s_profile ? SDL_GetTicksNS() : 0;
  const bool ok = ArGpuEffectSource_Draw(&s_source, device, s_motion, 0, 0, &source, view, scenery,
                                         blend, brightness);
  if (s_profile) {
    s_profile_frame.effects_ns += SDL_GetTicksNS() - wall;
    s_profile_frame.effects_cpu_ns += CpuNow() - cpu;
    s_profile_frame.packets += batch->count != 0;
    s_profile_frame.light_jobs += batch->light_count;
    s_profile_frame.primitive_bytes += batch->count * sizeof(*batch->primitives);
    for (unsigned i = 0; i < batch->count; ++i)
      if (batch->primitives[i].meta[3]) {
        s_profile_frame.shadow_rectangles += scenery && scenery->valid ? scenery->count : 0;
        break;
      }
  }
  return ok;
}
static bool Skybox(ArRenderDevice *device, ArRenderTexture texture,
                   const DioramaSkyboxSourceDraw *draw) {
  const uint64_t cpu = CpuNow(), wall = s_profile ? SDL_GetTicksNS() : 0;
  const bool ok = ArGpuEffectSource_Skybox(&s_source, device, s_motion, -1, 0, texture, draw);
  if (s_profile) {
    s_profile_frame.sky_ns += SDL_GetTicksNS() - wall;
    s_profile_frame.sky_cpu_ns += CpuNow() - cpu;
  }
  return ok;
}
static bool EffectBatch(ArRenderDevice *device, bool begin) {
  (void)device;
  return begin ? ArGpuEffectSource_BeginBatch(&s_source) : ArGpuEffectSource_EndBatch(&s_source);
}
static unsigned Read(const char *path) {
  size_t n = 0;
  void *data = SDL_LoadFile(path, &n);
  if (!data) return 0;
  if (!n || n >= sizeof(s_input)) {
    SDL_SetError("empty or oversized input: %s", path);
    SDL_free(data);
    return 0;
  }
  memcpy(s_input, data, n);
  s_input[n] = 0;
  SDL_free(data);
  return (unsigned)n;
}
static bool Integer(const char *name, const char *text, int low, int high, int *out) {
  char *end = NULL;
  errno = 0;
  const long value = strtol(text, &end, 10);
  if (errno == ERANGE || end == text || *end || value < low || value > high) {
    fprintf(stderr, "[benchmark] %s must be an integer in [%d, %d], got '%s'\n", name, low, high,
            text);
    return false;
  }
  *out = (int)value;
  return true;
}
static bool EnvInt(const char *key, int fallback, int low, int high, int *out) {
  const char *value = getenv(key);
  if (value) return Integer(key, value, low, high, out);
  *out = fallback;
  return fallback >= low && fallback <= high;
}
static const char *EnvPath(const char *key) {
  const char *value = getenv(key);
  return value && *value ? value : NULL;
}
static bool Load(const char *path, int (*load)(unsigned)) {
  const unsigned size = Read(path);
  if (!size) return false;
  if (load(size)) return true;
  return SDL_SetError("could not decode/configure '%s'", path);
}
static bool SavePng(const char *path) {
  SDL_Surface *image = SDL_RenderReadPixels(ArSdlRenderBackend_Renderer(&s_device), NULL);
  if (!image) return false;
  const bool ok = SDL_SavePNG(image, path);
  SDL_DestroySurface(image);
  return ok;
}
static bool CloseOutput(FILE **file, const char *path) {
  bool ok = !ferror(*file);
  if (fclose(*file) != 0) ok = false;
  *file = NULL;
  if (ok) return true;
  return SDL_SetError("could not write '%s'", path);
}
static int Coordinate(int from, int to, float t) {
  /* Preserve the sweep's float interpolation without overflowing signed
   * subtraction/addition for an extreme CLI coordinate. */
  int64_t value = (int64_t)from + (int64_t)((float)((int64_t)to - from) * t);
  const int low = from < to ? from : to, high = from > to ? from : to;
  if (value < low) value = low;
  if (value > high) value = high;
  return (int)value;
}
static bool Sibling(char *out, size_t capacity, const char *path, const char *name) {
  const char *slash = strrchr(path, '/'), *backslash = strrchr(path, '\\');
  if (backslash && (!slash || backslash > slash)) slash = backslash;
  int n = snprintf(out, capacity, "%.*s%s", slash ? (int)(slash - path + 1) : 0, path, name);
  if (n >= 0 && (size_t)n < capacity) return true;
  return SDL_SetError("asset path too long: %s", path);
}
static bool InitMotion(SDL_GPUDevice *gpu) {
  const SDL_GPUBufferCreateInfo info = {.size = 256,
                                        .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                                                 SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ};
  s_motion = SDL_CreateGPUBuffer(gpu, &info);
  if (!s_motion) return false;
  const SDL_GPUTransferBufferCreateInfo upload_info = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                                       .size = 256};
  SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(gpu, &upload_info);
  void *mapped = upload ? SDL_MapGPUTransferBuffer(gpu, upload, false) : NULL;
  if (!mapped) {
    if (upload) SDL_ReleaseGPUTransferBuffer(gpu, upload);
    return false;
  }
  memset(mapped, 0, 256);
  SDL_UnmapGPUTransferBuffer(gpu, upload);
  SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
  SDL_GPUCopyPass *copy = cmd ? SDL_BeginGPUCopyPass(cmd) : NULL;
  if (!copy) {
    if (cmd) SDL_CancelGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(gpu, upload);
    return false;
  }
  const SDL_GPUTransferBufferLocation from = {.transfer_buffer = upload};
  const SDL_GPUBufferRegion to = {.buffer = s_motion, .size = 256};
  SDL_UploadToGPUBuffer(copy, &from, &to, false);
  SDL_EndGPUCopyPass(copy);
  bool ok = SDL_SubmitGPUCommandBuffer(cmd);
  SDL_ReleaseGPUTransferBuffer(gpu, upload);
  return ok;
}
int main(int argc, char **argv) {
  if (argc != 12) {
    fprintf(stderr,
            "usage: %s output.csv layers.ini effects.ini room.arscene x y end-x end-y player-x "
            "player-y frames\n"
            "AR_BENCH_WIDTH/HEIGHT (1280/800), AR_BENCH_EFFECTS (1), AR_BENCH_SKY (1),\n"
            "AR_BENCH_WARMUP (one full route), AR_BENCH_CLOCK (0), AR_BENCH_STEP (2).\n"
            "AR_BENCH_FINAL_BATTLE=1 enables the optional native-nebula material in 07:08.\n"
            "AR_BENCH_SHOT saves a PNG after the last measured frame.\n"
            "AR_BENCH_PROFILE=path.csv adds owner-thread CPU and submission diagnostics.\n"
            "AR_BENCH_FRAME_PROBE=library enables the optional Vulkan timestamp probe.\n"
            "AR_BENCH_CAPTURE=directory captures every frame instead of timing.\n",
            argv[0]);
    return 2;
  }
  int width, height, frames, warmup, clock, step, sky, enabled, final_battle, x, y, ex, ey, px, py;
  if (!Integer("frames", argv[11], 2, 100000, &frames) ||
      !EnvInt("AR_BENCH_WIDTH", 1280, 1, 2048, &width) ||
      !EnvInt("AR_BENCH_HEIGHT", 800, 1, 2048, &height) ||
      !EnvInt("AR_BENCH_WARMUP", frames, 0, 100000, &warmup) ||
      !EnvInt("AR_BENCH_CLOCK", 0, 0, INT_MAX, &clock) ||
      !EnvInt("AR_BENCH_STEP", 2, 0, 60, &step) || !EnvInt("AR_BENCH_SKY", 1, 0, 2, &sky) ||
      !EnvInt("AR_BENCH_EFFECTS", 1, 0, 1, &enabled) ||
      !EnvInt("AR_BENCH_FINAL_BATTLE", 0, 0, 1, &final_battle) ||
      !Integer("x", argv[5], INT_MIN, INT_MAX, &x) ||
      !Integer("y", argv[6], INT_MIN, INT_MAX, &y) ||
      !Integer("end-x", argv[7], INT_MIN, INT_MAX, &ex) ||
      !Integer("end-y", argv[8], INT_MIN, INT_MAX, &ey) ||
      !Integer("player-x", argv[9], INT_MIN, INT_MAX, &px) ||
      !Integer("player-y", argv[10], INT_MIN, INT_MAX, &py))
    return 2;
  if (px >= 0 && ((int64_t)px + ex - x < INT_MIN || (int64_t)px + ex - x > INT_MAX ||
                  (int64_t)py + ey - y < INT_MIN || (int64_t)py + ey - y > INT_MAX)) {
    fprintf(stderr, "[benchmark] player route exceeds integer coordinates\n");
    return 2;
  }
  const char *capture_dir = EnvPath("AR_BENCH_CAPTURE");
  const char *profile_path = EnvPath("AR_BENCH_PROFILE");
  const char *probe_path = EnvPath("AR_BENCH_FRAME_PROBE");
  const char *shot = EnvPath("AR_BENCH_SHOT");
  if ((capture_dir && (probe_path || profile_path)) ||
      (profile_path && !strcmp(profile_path, argv[1])) ||
      (shot && (!strcmp(shot, argv[1]) || (profile_path && !strcmp(shot, profile_path))))) {
    fprintf(stderr, "[benchmark] capture cannot be profiled; output paths must be distinct\n");
    return 2;
  }
  int result = 4;
  const char *operation = "load frame probe";
  SDL_Window *window = NULL;
  SDL_GPUDevice *gpu = NULL;
  FILE *trace = NULL, *profile = NULL;
  bool preparing = false, probe_open = false;
  /* Diagnostic probes can tag existing work without linking a native GPU API
   * into the shared renderer. The optional library owns its measurements. */
  SDL_SharedObject *probe = NULL;
  ArRoomBenchmarkFrameProbe probe_frame = NULL;
  if (probe_path) {
    probe = SDL_LoadObject(probe_path);
    if (!probe ||
        !(probe_frame = (ArRoomBenchmarkFrameProbe)SDL_LoadFunction(probe, "ArVkProfileFrame")))
      goto cleanup;
  }
  GpuCompileLog_BeginPreparation(NULL, NULL);
  preparing = true;
  result = 77;
  operation = "initialize display/backend";
  if (!SDL_Init(SDL_INIT_VIDEO)) goto cleanup;
  window = SDL_CreateWindow("Action room renderer benchmark", width, height, SDL_WINDOW_HIDDEN);
  if (!window || !ArSdlRenderBackend_CreateForWindow(&s_device, window, getenv("SDL_GPU_DRIVER")))
    goto cleanup;
  gpu = ArSdlRenderBackend_GpuDevice(&s_device);
  result = 1;
  operation = "initialize GPU sources";
  if (!gpu || !ArGpuEffectSource_Init(&s_source, gpu) || !InitMotion(gpu)) goto cleanup;
  RoomPreview_SetSourceEffectBatch(EffectBatch);
  result = 3;
  operation = "load room/layers/effects";
  if (!Load(argv[4], RoomPreview_Load) || !Load(argv[2], RoomPreview_Configure) ||
      !Load(argv[3], RoomPreview_ConfigureEffects))
    goto cleanup;
  unsigned named = RoomPreview_SkyboxRoom();
  char path[1024], name[32];
  if (named) {
    snprintf(name, sizeof(name), "%02x%02x.arscene", named >> 16, (named >> 8) & 255);
    if (!Sibling(path, sizeof(path), argv[4], name) || !Load(path, RoomPreview_LoadSkybox))
      goto cleanup;
  }
  if (px >= 0) {
    if (!Sibling(path, sizeof(path), argv[4], "player.bin") ||
        !Load(path, RoomPreview_LoadPlayer) || !RoomPreview_ReceiverStyle(1, 1))
      goto cleanup;
  }
  RoomPreview_FinalBattle(final_battle);
  RoomPreview_EnableEffects(enabled);
  RoomPreview_SetSourceProjection(Effects, Skybox);
  if (!RoomPreview_SetView(3.25f, 0, 0, 1, 1)) goto cleanup;
  result = 4;
  operation = "open output trace";
  trace = fopen(argv[1], "w");
  if (!trace) {
    SDL_SetError("%s: %s", argv[1], strerror(errno));
    goto cleanup;
  }
  if (profile_path) {
    profile = fopen(profile_path, "w");
    if (!profile) {
      SDL_SetError("%s: %s", profile_path, strerror(errno));
      goto cleanup;
    }
    s_profile = true;
    if (!CpuNow()) {
      SDL_SetError("owner-thread CPU clock unavailable");
      goto cleanup;
    }
    fprintf(
        profile,
        "frame,prepare_cpu_ms,compose_cpu_ms,wait_cpu_ms,effects_wall_ms,effects_cpu_ms,sky_wall_"
        "ms,sky_cpu_ms,packets,light_jobs,shadow_rectangles,primitive_bytes,draws,submissions\n");
  }
  fprintf(trace, capture_dir ? "frame,requested_x,requested_y,clock\n"
                             : "frame,requested_x,requested_y,clock,primitives,vertices,prepare_ms,"
                               "compose_submit_ms,submit_elapsed_ms,gpu_wait_ms,complete_ms\n");
  const double scale = 1000.0 / SDL_GetPerformanceFrequency();
  double total = 0, prepare = 0, compose = 0, wait = 0;
  for (int i = -warmup; i < frames; ++i) {
    if (probe_frame) {
      probe_frame(i);
      probe_open = true;
    }
    if (i == 0) {
      GpuCompileLog_EndPreparation();
      preparing = false;
      fprintf(stderr, "[benchmark] %s\n", capture_dir ? "capture-only" : "timed-begin");
      fflush(stderr);
    }
    /* Warm the whole route and animation interval, then seek back. This
     * reaches pipelines hidden at the entrance before the timed traversal. */
    float t =
        i < 0 ? (warmup > 1 ? (i + warmup) / (float)(warmup - 1) : 0) : i / (float)(frames - 1);
    int cx = Coordinate(x, ex, t), cy = Coordinate(y, ey, t);
    uint32_t tick = (uint32_t)clock + (uint32_t)(t * (frames - 1)) * step;
    result = 5;
    operation = "render frame";
    if (px >= 0) {
      const int actor_x = (int)((int64_t)px + cx - x);
      const int actor_y = (int)((int64_t)py + cy - y);
      if (!RoomPreview_SetReceivers(1, actor_x, actor_y, 0, 0)) {
        SDL_SetError("player receiver position (%d, %d) is outside preview limits", actor_x, actor_y);
        goto cleanup;
      }
    }
    s_primitives = 0;
    s_packet = 0;
    ++s_capture;
    memset(&s_profile_frame, 0, sizeof(s_profile_frame));
    const uint64_t draws = s_source.draws, submissions = s_source.submissions, cpu_start = CpuNow();
    const uint64_t start = SDL_GetPerformanceCounter();
    if (!RoomPreview_Render(cx, cy, tick, 120, 64, width, height, 1, 0, 0, sky, 0) ||
        !ArSdlRenderBackend_SubmitPending(&s_device))
      goto cleanup;
    const uint64_t submitted = SDL_GetPerformanceCounter();
    const uint64_t cpu_submitted = CpuNow();
    if (!SDL_WaitForGPUIdle(gpu)) goto cleanup;
    const uint64_t end = SDL_GetPerformanceCounter();
    const uint64_t cpu_end = CpuNow();
    if (i < 0) continue;
    if (capture_dir) {
      char filename[2048];
      int length = snprintf(filename, sizeof(filename), "%s/frame-%06d.png", capture_dir, i);
      result = 6;
      operation = "write PNG capture";
      if (length < 0 || (size_t)length >= sizeof(filename)) {
        SDL_SetError("capture path too long");
        goto cleanup;
      }
      if (!SavePng(filename)) goto cleanup;
      fprintf(trace, "%d,%d,%d,%u\n", i, cx, cy, tick);
      continue;
    }
    double p = (s_prepared - start) * scale, c = (submitted - s_prepared) * scale;
    double w = (end - submitted) * scale, e = (end - start) * scale;
    prepare += p;
    compose += c;
    wait += w;
    total += e;
    fprintf(trace, "%d,%d,%d,%u,%u,%u,%.6f,%.6f,%.6f,%.6f,%.6f\n", i, cx, cy, tick, s_primitives,
            RoomPreview_EffectVertices(), p, c, p + c, w, e);
    if (profile)
      fprintf(profile, "%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%u,%u,%u,%u,%llu,%llu\n", i,
              (s_prepared_cpu - cpu_start) * 1e-6, (cpu_submitted - s_prepared_cpu) * 1e-6,
              (cpu_end - cpu_submitted) * 1e-6, s_profile_frame.effects_ns * 1e-6,
              s_profile_frame.effects_cpu_ns * 1e-6, s_profile_frame.sky_ns * 1e-6,
              s_profile_frame.sky_cpu_ns * 1e-6, s_profile_frame.packets,
              s_profile_frame.light_jobs, s_profile_frame.shadow_rectangles,
              s_profile_frame.primitive_bytes, (unsigned long long)(s_source.draws - draws),
              (unsigned long long)(s_source.submissions - submissions));
  }
  /* The last frame has completed and its query results have been collected.
   * Capture readback and teardown can use other fence paths; neither belongs
   * to the frame trace. */
  if (probe_frame) {
    probe_frame(kArRoomBenchmarkProbeClosed);
    probe_open = false;
  }
  result = 4;
  operation = "write output trace";
  if (!CloseOutput(&trace, argv[1]) || (profile && !CloseOutput(&profile, profile_path)))
    goto cleanup;
  result = 6;
  operation = "write final PNG";
  if (shot && !SavePng(shot)) goto cleanup;
  if (capture_dir)
    printf("capture-only: %d frames; timings intentionally omitted\n", frames);
  else
    printf("backend=%s size=%dx%d frames=%d effects=%u enabled=%d prepare_ms=%.4f "
           "compose_submit_ms=%.4f gpu_wait_ms=%.4f complete_ms=%.4f\n",
           SDL_GetGPUDeviceDriver(gpu), width, height, frames, RoomPreview_EffectCount(), enabled,
           prepare / frames, compose / frames, wait / frames, total / frames);
  result = 0;
cleanup:
  if (result) fprintf(stderr, "[benchmark] %s: %s\n", operation, SDL_GetError());
  if (preparing) GpuCompileLog_EndPreparation();
  /* An error can leave submitted work before the ordinary frame idle. Collect
   * it before closing the optional probe; unsubmitted source work is cancelled
   * by its owner during destruction. No extra wait is added to timed frames. */
  if (probe_open && gpu && SDL_WaitForGPUIdle(gpu)) probe_frame(kArRoomBenchmarkProbeClosed);
  if (trace) fclose(trace);
  if (profile) fclose(profile);
  if (ArRenderDevice_IsReady(&s_device)) RoomPreview_Reset();
  ArGpuEffectSource_Destroy(&s_source);
  if (s_motion) SDL_ReleaseGPUBuffer(gpu, s_motion);
  ArSdlRenderBackend_Destroy(&s_device);
  if (window) SDL_DestroyWindow(window);
  SDL_Quit();
  if (probe) SDL_UnloadObject(probe);
  return result;
}
