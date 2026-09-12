/* deckbench-gpu — texture-upload cost model for SDL's GPU renderer.
 *
 * Companion to deckbench-cpu. The CPU benchmark showed that CPU memory traffic
 * accounts for only a fraction of a measured upload stage; the remainder has to
 * be in the driver path. This measures that path directly and produces the two
 * constants a host-side cost model needs:
 *
 *     upload_ms  ~=  calls * ALPHA  +  bytes * BETA
 *
 * ALPHA (per-call overhead) and BETA (per-byte cost) differ enormously between
 * a unified-memory desktop API and a discrete-style Vulkan backend, and their
 * RATIO decides an architectural question: whether to prefer fewer large
 * transfers or many small precise ones. Run this on every target you ship.
 *
 * It uses SDL_CreateGPURenderer(device, NULL) — an OFFSCREEN renderer — so it
 * needs no window, no display and no compositor, and runs fine over SSH. That
 * is also the exact path a renderer that owns its own output target uses.
 *
 * Also measured, because it decides whether a producer can write straight into
 * GPU-visible memory: mapped transfer-buffer write AND read bandwidth. Upload
 * memory is typically write-combined — fast to write, pathologically slow to
 * read — and a single stray CPU read of it can cost more than everything this
 * benchmark is trying to save. Reading is measured so that hazard is a number
 * rather than a warning.
 *
 * CPU wall times. Not frame rates, not GPU-side timings.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

/* ---------------------------------------------------------------- timing -- */

static uint64_t NowNanos(void) { return SDL_GetTicksNS(); }

static int CompareU64(const void *a, const void *b) {
  const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

typedef struct { uint64_t median, min, max; } Stats;

static Stats Summarize(uint64_t *samples, size_t count) {
  Stats s = {0};
  if (!count) return s;
  qsort(samples, count, sizeof(*samples), CompareU64);
  s.median = samples[count / 2];
  s.min = samples[0];
  s.max = samples[count - 1];
  return s;
}

/* ----------------------------------------------------------------- output -- */

static FILE *g_json;
static bool g_first = true;

static void Emit(const char *test, const char *variant, int calls,
                 uint64_t bytes_per_call, Stats submit, Stats total) {
  if (!g_json) return;
  if (!g_first) fprintf(g_json, ",\n");
  g_first = false;
  const uint64_t bytes = (uint64_t)calls * bytes_per_call;
  fprintf(g_json,
      "    {\"test\":\"%s\",\"variant\":\"%s\",\"calls\":%d,"
      "\"bytes_per_call\":%" PRIu64 ",\"total_bytes\":%" PRIu64 ","
      "\"update_ns_median\":%" PRIu64 ",\"update_ns_min\":%" PRIu64 ","
      "\"frame_ns_median\":%" PRIu64 ",\"frame_ns_min\":%" PRIu64 ","
      "\"update_us_per_call\":%.4f,\"frame_gbps\":%.3f}",
      test, variant, calls, bytes_per_call, bytes,
      submit.median, submit.min, total.median, total.min,
      calls ? (double)submit.median / 1000.0 / calls : 0.0,
      total.median ? (double)bytes / ((double)total.median / 1e9) / 1e9 : 0.0);
  fflush(g_json);
}

/* ------------------------------------------------------------------ state -- */

typedef struct {
  SDL_GPUDevice *device;
  SDL_Renderer *renderer;
  int iterations;
  uint8_t *staging;          /* source pixels, CPU side */
  size_t staging_bytes;
  uint64_t *samples_update;
  uint64_t *samples_frame;
} Bench;

/* One frame's worth of work: `calls` texture updates, then a draw per texture
 * so the data is genuinely consumed, then present to force submission.
 *
 * Update time and whole-frame time are reported separately on purpose: on a
 * deferred backend SDL_UpdateTexture can look nearly free while the real cost
 * lands at submission. Reporting only the first would flatter the API. */
static void RunUploadFrame(Bench *b, SDL_Texture **textures, int texture_count,
                           int calls, int rect_w, int rect_h, int texture_h,
                           int pitch,
                           uint64_t *out_update_ns, uint64_t *out_frame_ns) {
  const uint64_t frame_start = NowNanos();

  const uint64_t update_start = NowNanos();
  for (int i = 0; i < calls; i++) {
    SDL_Texture *texture = textures[i % texture_count];
    /* Stagger the destination row so repeated calls on one texture do not all
     * target identical bytes, which a driver could plausibly coalesce. Wrap
     * rather than run past the texture: an out-of-bounds rect is REJECTED and
     * costs nothing, which silently turns extra calls into free ones. */
    int y = (i / texture_count) * rect_h;
    if (y + rect_h > texture_h) y = 0;
    const SDL_Rect rect = {0, y, rect_w, rect_h};
    SDL_UpdateTexture(texture, &rect, b->staging, pitch);
  }
  const uint64_t update_end = NowNanos();

  for (int i = 0; i < texture_count; i++) {
    const SDL_FRect dst = {0.0f, 0.0f, (float)rect_w, (float)rect_h};
    SDL_RenderTexture(b->renderer, textures[i], NULL, &dst);
  }
  SDL_RenderPresent(b->renderer);

  *out_update_ns = update_end - update_start;
  *out_frame_ns = NowNanos() - frame_start;
}

static void MeasureUpload(Bench *b, const char *test, const char *variant,
                          SDL_Texture **textures, int texture_count,
                          int calls, int rect_w, int rect_h, int texture_h,
                          int pitch) {
  for (int w = 0; w < 3; w++) {
    uint64_t a = 0, c = 0;
    RunUploadFrame(b, textures, texture_count, calls, rect_w, rect_h,
                   texture_h, pitch, &a, &c);
  }
  for (int i = 0; i < b->iterations; i++)
    RunUploadFrame(b, textures, texture_count, calls, rect_w, rect_h,
                   texture_h, pitch,
                   &b->samples_update[i], &b->samples_frame[i]);

  uint64_t *update_copy = malloc((size_t)b->iterations * sizeof(uint64_t));
  uint64_t *frame_copy = malloc((size_t)b->iterations * sizeof(uint64_t));
  memcpy(update_copy, b->samples_update, (size_t)b->iterations * sizeof(uint64_t));
  memcpy(frame_copy, b->samples_frame, (size_t)b->iterations * sizeof(uint64_t));
  const Stats update = Summarize(update_copy, (size_t)b->iterations);
  const Stats frame = Summarize(frame_copy, (size_t)b->iterations);
  free(update_copy);
  free(frame_copy);

  const uint64_t bytes_per_call = (uint64_t)rect_w * (uint64_t)rect_h * 4u;
  printf("%-14s %-16s %5d %10" PRIu64 " %11.1f %11.1f %10.2f\n",
         test, variant, calls, bytes_per_call,
         (double)update.median / 1000.0, (double)frame.median / 1000.0,
         frame.median ? (double)calls * bytes_per_call /
                            ((double)frame.median / 1e9) / 1e9 : 0.0);
  Emit(test, variant, calls, bytes_per_call, update, frame);
}

/* Mapped transfer-buffer bandwidth. The read direction is the one that decides
 * whether a producer may write straight into GPU-visible memory: if reads come
 * back an order of magnitude below writes, that memory is write-combined and
 * every CPU reader of it must be moved elsewhere first. */
static void MeasureTransferBuffer(Bench *b, SDL_GPUTransferBufferUsage usage,
                                  const char *label, size_t bytes) {
  SDL_GPUTransferBufferCreateInfo info = {
    .usage = usage, .size = (Uint32)bytes, .props = 0,
  };
  SDL_GPUTransferBuffer *buffer = SDL_CreateGPUTransferBuffer(b->device, &info);
  if (!buffer) {
    printf("  %-22s unavailable (%s)\n", label, SDL_GetError());
    return;
  }

  uint64_t *write_samples = malloc((size_t)b->iterations * sizeof(uint64_t));
  uint64_t *read_samples = malloc((size_t)b->iterations * sizeof(uint64_t));
  volatile uint64_t sink = 0;

  for (int i = -2; i < b->iterations; i++) {
    void *mapped = SDL_MapGPUTransferBuffer(b->device, buffer, false);
    if (!mapped) break;

    const uint64_t write_start = NowNanos();
    memcpy(mapped, b->staging, bytes);
    const uint64_t write_ns = NowNanos() - write_start;

    /* Deliberately sequential and deliberately simple: this is the shape a
     * stray readback takes in real code, not a worst case constructed to
     * exaggerate. */
    const uint64_t read_start = NowNanos();
    const uint64_t *words = mapped;
    uint64_t accumulator = 0;
    for (size_t w = 0; w < bytes / sizeof(uint64_t); w++) accumulator += words[w];
    const uint64_t read_ns = NowNanos() - read_start;
    sink += accumulator;

    SDL_UnmapGPUTransferBuffer(b->device, buffer);
    if (i >= 0) {
      write_samples[i] = write_ns;
      read_samples[i] = read_ns;
    }
  }
  (void)sink;

  const Stats write = Summarize(write_samples, (size_t)b->iterations);
  const Stats read = Summarize(read_samples, (size_t)b->iterations);
  const double write_gbps = write.median ? (double)bytes / ((double)write.median / 1e9) / 1e9 : 0.0;
  const double read_gbps = read.median ? (double)bytes / ((double)read.median / 1e9) / 1e9 : 0.0;

  printf("  %-22s write %7.2f GB/s   read %7.2f GB/s   ratio %5.1fx\n",
         label, write_gbps, read_gbps,
         read_gbps > 0.0 ? write_gbps / read_gbps : 0.0);

  if (g_json) {
    if (!g_first) fprintf(g_json, ",\n");
    g_first = false;
    fprintf(g_json,
        "    {\"test\":\"transfer_buffer\",\"variant\":\"%s\","
        "\"bytes\":%zu,\"write_ns_median\":%" PRIu64 ",\"read_ns_median\":%" PRIu64 ","
        "\"write_gbps\":%.3f,\"read_gbps\":%.3f}",
        label, bytes, write.median, read.median, write_gbps, read_gbps);
    fflush(g_json);
  }
  free(write_samples);
  free(read_samples);
  SDL_ReleaseGPUTransferBuffer(b->device, buffer);
}

/* The manual alternative to SDL_UpdateTexture: one persistent transfer buffer,
 * a mapped write, and an explicit copy pass into the texture SDL already owns.
 * This is the END-TO-END path, not just the CPU write -- measuring only the
 * memcpy into mapped memory flatters it, because the upload and copy pass that
 * must follow are exactly what SDL_UpdateTexture is also doing internally.
 *
 * The texture is not recreated: SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER hands
 * back the SDL_GPUTexture behind an ordinary SDL_Texture, so the compositor
 * keeps drawing the same object. */
static void MeasureManualUpload(Bench *b, SDL_Texture **textures,
                                int texture_count, int width, int height) {
  SDL_GPUTransferBuffer *transfer = NULL;
  SDL_GPUTexture **gpu_textures = calloc((size_t)texture_count, sizeof(*gpu_textures));
  if (!gpu_textures) return;
  bool usable = true;
  for (int i = 0; i < texture_count && usable; i++) {
    SDL_PropertiesID props = SDL_GetTextureProperties(textures[i]);
    gpu_textures[i] = props ? SDL_GetPointerProperty(
        props, SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, NULL) : NULL;
    usable = gpu_textures[i] != NULL;
  }
  if (!usable) {
    printf("%-14s %-16s  unavailable (no GPU texture behind SDL_Texture)\n",
           "manual", "transfer-buf");
    free(gpu_textures);
    return;
  }
  SDL_GPUTransferBufferCreateInfo info = {
    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
    .size = (Uint32)((size_t)width * (size_t)height * 4u),
    .props = 0,
  };
  transfer = SDL_CreateGPUTransferBuffer(b->device, &info);
  if (!transfer) {
    printf("%-14s %-16s  unavailable (%s)\n", "manual", "transfer-buf",
           SDL_GetError());
    free(gpu_textures);
    return;
  }

  for (int iteration = -3; iteration < b->iterations; iteration++) {
    const uint64_t started = NowNanos();
    for (int i = 0; i < texture_count; i++) {
      /* cycle=true lets the driver hand back storage not still in flight,
       * which is what a real double-buffered producer would rely on. */
      void *mapped = SDL_MapGPUTransferBuffer(b->device, transfer, true);
      if (!mapped) break;
      memcpy(mapped, b->staging, (size_t)width * (size_t)height * 4u);
      SDL_UnmapGPUTransferBuffer(b->device, transfer);

      SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(b->device);
      if (!commands) break;
      SDL_GPUCopyPass *pass = SDL_BeginGPUCopyPass(commands);
      if (pass) {
        const SDL_GPUTextureTransferInfo source = {
          .transfer_buffer = transfer, .offset = 0,
          .pixels_per_row = (Uint32)width, .rows_per_layer = (Uint32)height,
        };
        const SDL_GPUTextureRegion destination = {
          .texture = gpu_textures[i], .mip_level = 0, .layer = 0,
          .x = 0, .y = 0, .z = 0,
          .w = (Uint32)width, .h = (Uint32)height, .d = 1,
        };
        SDL_UploadToGPUTexture(pass, &source, &destination, false);
        SDL_EndGPUCopyPass(pass);
      }
      SDL_SubmitGPUCommandBuffer(commands);
    }
    if (iteration >= 0) b->samples_frame[iteration] = NowNanos() - started;
  }
  uint64_t *copy = malloc((size_t)b->iterations * sizeof(uint64_t));
  memcpy(copy, b->samples_frame, (size_t)b->iterations * sizeof(uint64_t));
  const Stats frame = Summarize(copy, (size_t)b->iterations);
  free(copy);
  const uint64_t bytes_per_call = (uint64_t)width * (uint64_t)height * 4u;
  printf("%-14s %-16s %5d %10" PRIu64 " %11s %11.1f %10.2f\n",
         "manual", "transfer-buf", texture_count, bytes_per_call, "-",
         (double)frame.median / 1000.0,
         frame.median ? (double)texture_count * bytes_per_call /
             ((double)frame.median / 1e9) / 1e9 : 0.0);
  Emit("manual", "transfer-buf", texture_count, bytes_per_call, frame, frame);
  SDL_ReleaseGPUTransferBuffer(b->device, transfer);
  free(gpu_textures);
}

/* ------------------------------------------------------------------- main -- */

int main(int argc, char **argv) {
  int width = 486, height = 224, planes = 12, iterations = 60;
  const char *json_path = NULL;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (!strcmp(a, "--width") && v) { width = atoi(v); i++; }
    else if (!strcmp(a, "--height") && v) { height = atoi(v); i++; }
    else if (!strcmp(a, "--planes") && v) { planes = atoi(v); i++; }
    else if (!strcmp(a, "--iterations") && v) { iterations = atoi(v); i++; }
    else if (!strcmp(a, "--json") && v) { json_path = v; i++; }
    else {
      fprintf(stderr, "usage: %s [--width N] [--height N] [--planes N]"
                      " [--iterations N] [--json PATH]\n", argv[0]);
      return 2;
    }
  }
  if (width <= 0 || height <= 0 || planes <= 0 || iterations <= 0) return 2;

  /* Video init is attempted but not required: an offscreen GPU renderer needs
   * no display. Over SSH there is usually no session to attach to, so fall
   * back rather than refusing to run. */
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_Init(SDL_INIT_VIDEO) && !SDL_Init(0)) {
      fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
      return 1;
    }
  }

  SDL_GPUDevice *device = SDL_CreateGPUDevice(
      SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL |
      SDL_GPU_SHADERFORMAT_DXIL, false, NULL);
  if (!device) {
    fprintf(stderr, "SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Renderer *renderer = SDL_CreateGPURenderer(device, NULL);
  if (!renderer) {
    fprintf(stderr, "SDL_CreateGPURenderer failed: %s\n", SDL_GetError());
    SDL_DestroyGPUDevice(device);
    return 1;
  }

  printf("deckbench-gpu: SDL %d.%d.%d, GPU driver \"%s\", renderer \"%s\"\n",
         SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
         SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
         SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
         SDL_GetGPUDeviceDriver(device), SDL_GetRendererName(renderer));
  printf("geometry: %d planes of %dx%d ARGB8888 (%.2f MiB total), %d iterations\n\n",
         planes, width, height,
         (double)planes * width * height * 4.0 / 1048576.0, iterations);

  Bench bench = {
    .device = device, .renderer = renderer, .iterations = iterations,
    .staging_bytes = (size_t)width * (size_t)height * 4u,
  };
  bench.staging = malloc(bench.staging_bytes);
  bench.samples_update = malloc((size_t)iterations * sizeof(uint64_t));
  bench.samples_frame = malloc((size_t)iterations * sizeof(uint64_t));
  if (!bench.staging || !bench.samples_update || !bench.samples_frame) return 1;
  for (size_t i = 0; i < bench.staging_bytes; i++)
    bench.staging[i] = (uint8_t)(i * 31u + 7u);

  if (json_path) {
    g_json = fopen(json_path, "w");
    if (g_json)
      fprintf(g_json,
          "{\n  \"tool\": \"deckbench-gpu\",\n  \"version\": 1,\n"
          "  \"gpu_driver\": \"%s\",\n  \"renderer\": \"%s\",\n"
          "  \"config\": {\"width\": %d, \"height\": %d, \"planes\": %d,"
          " \"iterations\": %d},\n  \"results\": [\n",
          SDL_GetGPUDeviceDriver(device), SDL_GetRendererName(renderer),
          width, height, planes, iterations);
  }

  SDL_Texture **textures = calloc((size_t)planes, sizeof(*textures));
  for (int i = 0; i < planes; i++) {
    textures[i] = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!textures[i]) {
      fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
      return 1;
    }
    SDL_SetTextureBlendMode(textures[i], SDL_BLENDMODE_NONE);
  }

  printf("%-14s %-16s %5s %10s %11s %11s %10s\n",
         "test", "variant", "calls", "bytes/call", "update_us", "frame_us", "GB/s");

  /* --- Per-call cost: hold bytes-per-call at a full plane, vary the count.
   * A flat frame time across counts means ALPHA dominates; linear growth in
   * proportion to total bytes means BETA does. */
  const int call_counts[] = {1, 2, 4, 8, 12, 24};
  for (size_t i = 0; i < sizeof(call_counts) / sizeof(call_counts[0]); i++) {
    const int calls = call_counts[i];
    const int used = calls < planes ? calls : planes;
    MeasureUpload(&bench, "call-sweep", "full-plane", textures, used, calls,
                  width, height, height, width * 4);
  }

  /* --- Per-byte cost: hold the call count at one per plane, shrink the region.
   * Falling frame time tracks BETA; a floor that will not fall exposes ALPHA. */
  const int row_divisors[] = {1, 2, 4, 8, 16};
  for (size_t i = 0; i < sizeof(row_divisors) / sizeof(row_divisors[0]); i++) {
    const int rows = height / row_divisors[i];
    if (rows <= 0) continue;
    char variant[32];
    snprintf(variant, sizeof(variant), "%d-rows", rows);
    MeasureUpload(&bench, "byte-sweep", variant, textures, planes, planes,
                  width, rows, height, width * 4);
  }

  /* --- One atlas versus many textures, at equal total bytes: the practical
   * question behind consolidating resources. */
  SDL_Texture *atlas = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      width, height * planes);
  if (atlas) {
    SDL_SetTextureBlendMode(atlas, SDL_BLENDMODE_NONE);
    MeasureUpload(&bench, "consolidate", "atlas-subrects", &atlas, 1, planes,
                  width, height, height * planes, width * 4);
    SDL_DestroyTexture(atlas);
  }

  /* Head-to-head with the call-sweep row at the same call count and bytes. */
  MeasureManualUpload(&bench, textures, planes, width, height);

  printf("\ntransfer buffers (%.2f MiB):\n",
         (double)bench.staging_bytes / 1048576.0);
  MeasureTransferBuffer(&bench, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                        "upload (CPU->GPU)", bench.staging_bytes);
  MeasureTransferBuffer(&bench, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                        "download (GPU->CPU)", bench.staging_bytes);

  if (g_json) {
    fprintf(g_json, "\n  ]\n}\n");
    fclose(g_json);
  }
  for (int i = 0; i < planes; i++) SDL_DestroyTexture(textures[i]);
  free(textures);
  free(bench.staging);
  free(bench.samples_update);
  free(bench.samples_frame);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyGPUDevice(device);
  SDL_Quit();
  return 0;
}
