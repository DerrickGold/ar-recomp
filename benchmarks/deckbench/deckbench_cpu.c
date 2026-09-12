/* deckbench-cpu — Steam Deck CPU/memory calibration for the action-mode
 * upload path.
 *
 * Purpose: derive the constants the Mac cannot supply. The Deck's upload stage
 * measured 4.74 ms/frame moving ~13 MB, against a 4 MiB shared L3; the same
 * work costs 0.29 ms on an M2. This benchmark isolates why, and answers three
 * questions that block the structural work:
 *
 *   1. Does the per-plane dirty scan parallelize on 4 Zen 2 cores, or does
 *      dispatch cost eat it? (decides Tier 1 threading)
 *   2. Does the upload mirror ever pay for itself IN MOTION, or does a
 *      scrolling camera dirty every plane anyway? (decides structural fix A)
 *   3. What is the effective scan/copy bandwidth at each cache tier?
 *      (supplies gamma for the Mac-side cost model)
 *
 * It links the REAL PresentationUploadMirror_FindDirtyRect rather than a copy,
 * so the calibration cannot drift from the shipping kernel.
 *
 * Builds and runs on both macOS and Linux: running the identical kernels on
 * both hosts is what makes the Mac cost model trustworthy. CPU pinning is
 * Linux-only and is skipped elsewhere.
 *
 * These are CPU wall times. They are not FPS, not GPU timings, and not a
 * prediction of any whole-frame result.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

#include "presentation_upload_mirror.h"

/* ---------------------------------------------------------------- timing -- */

static uint64_t NowNanos(void) {
  struct timespec ts;
#if defined(CLOCK_MONOTONIC_RAW)
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
  clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static int CompareU64(const void *a, const void *b) {
  const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

/* Median plus the halves, so thermal drift under the 15 W cap is visible in
 * the output instead of silently biasing a single median. */
typedef struct {
  uint64_t median, min, max, first_half, second_half;
} Stats;

static Stats Summarize(uint64_t *samples, size_t count) {
  Stats s = {0};
  if (!count) return s;
  uint64_t *sorted = malloc(count * sizeof(*sorted));
  if (!sorted) return s;
  memcpy(sorted, samples, count * sizeof(*sorted));
  qsort(sorted, count, sizeof(*sorted), CompareU64);
  s.median = sorted[count / 2];
  s.min = sorted[0];
  s.max = sorted[count - 1];
  free(sorted);

  const size_t half = count / 2;
  if (half) {
    uint64_t *scratch = malloc(half * sizeof(*scratch));
    if (scratch) {
      memcpy(scratch, samples, half * sizeof(*scratch));
      qsort(scratch, half, sizeof(*scratch), CompareU64);
      s.first_half = scratch[half / 2];
      memcpy(scratch, samples + count - half, half * sizeof(*scratch));
      qsort(scratch, half, sizeof(*scratch), CompareU64);
      s.second_half = scratch[half / 2];
      free(scratch);
    }
  }
  return s;
}

/* ------------------------------------------------------------ thread pool -- */

/* Mirrors HostParallelWork's shape: persistent helpers that SLEEP between
 * jobs, woken per dispatch, joined before Run returns. Measuring a pool that
 * creates threads per job would measure the wrong thing entirely.
 *
 * Uses mutex+condvar rather than SDL semaphores so one binary runs on both
 * hosts; wakeup latency is in the same class, and the forkjoin test reports
 * it explicitly rather than leaving it assumed. */

typedef void (*RangeFn)(void *context, size_t first, size_t end);

typedef struct BenchPool BenchPool;

typedef struct {
  BenchPool *pool;
  int index;
  size_t first, end;
  bool has_work;
} BenchHelper;

struct BenchPool {
  pthread_t *threads;
  BenchHelper *helpers;
  int helper_count;
  pthread_mutex_t lock;
  pthread_cond_t work_ready, work_done;
  RangeFn range;
  void *context;
  int outstanding;
  bool shutting_down;
  bool pin;
  int pin_stride;
};

static void PinToCpu(int cpu) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
  (void)cpu;
#endif
}

static void *HelperMain(void *argument) {
  BenchHelper *helper = argument;
  BenchPool *pool = helper->pool;
  if (pool->pin) PinToCpu((helper->index + 1) * pool->pin_stride);
  pthread_mutex_lock(&pool->lock);
  for (;;) {
    while (!helper->has_work && !pool->shutting_down)
      pthread_cond_wait(&pool->work_ready, &pool->lock);
    if (pool->shutting_down) break;
    const size_t first = helper->first, end = helper->end;
    RangeFn range = pool->range;
    void *context = pool->context;
    pthread_mutex_unlock(&pool->lock);

    if (range && end > first) range(context, first, end);

    pthread_mutex_lock(&pool->lock);
    helper->has_work = false;
    if (--pool->outstanding == 0) pthread_cond_signal(&pool->work_done);
  }
  pthread_mutex_unlock(&pool->lock);
  return NULL;
}

static BenchPool *PoolCreate(int helper_count, bool pin, int pin_stride) {
  BenchPool *pool = calloc(1, sizeof(*pool));
  if (!pool) return NULL;
  pool->helper_count = helper_count;
  pool->pin = pin;
  pool->pin_stride = pin_stride > 0 ? pin_stride : 1;
  pthread_mutex_init(&pool->lock, NULL);
  pthread_cond_init(&pool->work_ready, NULL);
  pthread_cond_init(&pool->work_done, NULL);
  if (helper_count > 0) {
    pool->threads = calloc((size_t)helper_count, sizeof(*pool->threads));
    pool->helpers = calloc((size_t)helper_count, sizeof(*pool->helpers));
    if (!pool->threads || !pool->helpers) {
      free(pool->threads);
      free(pool->helpers);
      free(pool);
      return NULL;
    }
    for (int i = 0; i < helper_count; i++) {
      pool->helpers[i].pool = pool;
      pool->helpers[i].index = i;
      if (pthread_create(&pool->threads[i], NULL, HelperMain,
                         &pool->helpers[i]) != 0) {
        pool->helper_count = i;
        break;
      }
    }
  }
  return pool;
}

static void PoolDestroy(BenchPool *pool) {
  if (!pool) return;
  pthread_mutex_lock(&pool->lock);
  pool->shutting_down = true;
  pthread_cond_broadcast(&pool->work_ready);
  pthread_mutex_unlock(&pool->lock);
  for (int i = 0; i < pool->helper_count; i++)
    pthread_join(pool->threads[i], NULL);
  pthread_mutex_destroy(&pool->lock);
  pthread_cond_destroy(&pool->work_ready);
  pthread_cond_destroy(&pool->work_done);
  free(pool->threads);
  free(pool->helpers);
  free(pool);
}

/* Owner takes the first part and participates, exactly as HostParallelWork
 * does, so the measured parallelism includes the owner's share. */
static void PoolRun(BenchPool *pool, size_t count, RangeFn range,
                    void *context) {
  const int helpers = pool ? pool->helper_count : 0;
  const size_t parts = (size_t)helpers + 1;
  if (!count || !range) return;
  if (parts < 2) {
    range(context, 0, count);
    return;
  }
  const size_t quotient = count / parts, remainder = count % parts;

  pthread_mutex_lock(&pool->lock);
  pool->range = range;
  pool->context = context;
  pool->outstanding = 0;
  size_t first = quotient + (remainder > 0);
  const size_t owner_end = first;
  for (int i = 1; i < (int)parts; i++) {
    BenchHelper *helper = &pool->helpers[i - 1];
    helper->first = first;
    first += quotient + ((size_t)i < remainder);
    helper->end = first;
    helper->has_work = true;
    pool->outstanding++;
  }
  pthread_cond_broadcast(&pool->work_ready);
  pthread_mutex_unlock(&pool->lock);

  range(context, 0, owner_end);

  pthread_mutex_lock(&pool->lock);
  while (pool->outstanding > 0)
    pthread_cond_wait(&pool->work_done, &pool->lock);
  pthread_mutex_unlock(&pool->lock);
}

/* ------------------------------------------------------------- plane model -- */

/* One diorama plane: the live pixels scanout writes, plus the upload mirror
 * the dirty scan compares against. Geometry mirrors the real capture surface
 * (display columns plus the resolve apron), so cache behaviour matches. */
typedef struct {
  uint8_t *current;
  uint8_t *mirror;
  int width, height, pitch;
} BenchPlane;

typedef enum {
  kDirtyStatic,  /* nothing changed: the mirror's best case */
  kDirtyRegion,  /* one contiguous block: bounding rect is tight and correct */
  kDirtySparse,  /* scattered sprite-sized blocks: bounding rect is near-full */
  kDirtyScroll,  /* every row differs: camera panning, the action-mode case */
  kDirtyModeCount,
} DirtyMode;

static const char *DirtyModeName(DirtyMode mode) {
  switch (mode) {
    case kDirtyStatic: return "static";
    case kDirtyRegion: return "region";
    case kDirtySparse: return "sparse";
    case kDirtyScroll: return "scroll";
    default: return "unknown";
  }
}

static uint32_t NextRandom(uint32_t *state) {
  /* xorshift32: deterministic across hosts so both machines generate the
   * identical dirty pattern and their numbers stay comparable. */
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return (*state = x);
}

typedef struct {
  BenchPlane *planes;
  int count;
  size_t bytes_per_plane;
} PlaneSet;

static void PlaneSetFree(PlaneSet *set) {
  if (!set || !set->planes) return;
  for (int i = 0; i < set->count; i++) {
    free(set->planes[i].current);
    free(set->planes[i].mirror);
  }
  free(set->planes);
  set->planes = NULL;
  set->count = 0;
}

static bool PlaneSetCreate(PlaneSet *set, int count, int width, int height) {
  memset(set, 0, sizeof(*set));
  set->planes = calloc((size_t)count, sizeof(*set->planes));
  if (!set->planes) return false;
  set->count = count;
  const int pitch = width * 4;
  set->bytes_per_plane = (size_t)pitch * (size_t)height;
  for (int i = 0; i < count; i++) {
    BenchPlane *plane = &set->planes[i];
    plane->width = width;
    plane->height = height;
    plane->pitch = pitch;
    plane->current = malloc(set->bytes_per_plane);
    plane->mirror = malloc(set->bytes_per_plane);
    if (!plane->current || !plane->mirror) {
      PlaneSetFree(set);
      return false;
    }
    /* Content, not zeros: a zero-filled pair would let a future memcmp
     * short-circuit on pages the allocator shares. */
    uint32_t seed = 0x9E3779B9u ^ (uint32_t)(i + 1);
    for (size_t offset = 0; offset < set->bytes_per_plane; offset += 4) {
      const uint32_t value = NextRandom(&seed);
      memcpy(plane->current + offset, &value, sizeof(value));
    }
    memcpy(plane->mirror, plane->current, set->bytes_per_plane);
  }
  return true;
}

/* Applies the frame-to-frame difference to `current`, leaving `mirror` as the
 * previous frame. Deterministic per (mode, plane, dirty_pct). */
static void PlaneSetApplyDirty(PlaneSet *set, DirtyMode mode, int dirty_pct) {
  for (int i = 0; i < set->count; i++) {
    BenchPlane *plane = &set->planes[i];
    memcpy(plane->current, plane->mirror, set->bytes_per_plane);
    uint32_t seed = 0x85EBCA6Bu ^ (uint32_t)(i + 1) ^ ((uint32_t)mode << 16);
    switch (mode) {
      case kDirtyStatic:
        break;
      case kDirtyScroll: {
        /* A panning camera shifts every row, so every row differs across the
         * full width. This is the situation the bounding-rect mirror cannot
         * help with, and it is the normal case for a moving action screen. */
        for (int y = 0; y < plane->height; y++) {
          uint32_t *row = (uint32_t *)(plane->current + (size_t)y * plane->pitch);
          for (int x = 0; x < plane->width; x++) row[x] += 1u;
        }
        break;
      }
      case kDirtyRegion: {
        const int rows = plane->height * dirty_pct / 100;
        const int y0 = (plane->height - rows) / 2;
        for (int y = y0; y < y0 + rows; y++) {
          uint32_t *row = (uint32_t *)(plane->current + (size_t)y * plane->pitch);
          for (int x = 0; x < plane->width; x++) row[x] += 1u;
        }
        break;
      }
      case kDirtySparse: {
        /* Sprite-sized blocks scattered across the plane. Few changed pixels,
         * but their bounding rectangle covers most of the surface -- the
         * pathology that makes a single dirty rect the wrong granularity. */
        const int block = 16;
        const long total_blocks =
            ((long)plane->width / block) * ((long)plane->height / block);
        long touched = total_blocks * dirty_pct / 100;
        if (touched < 1) touched = 1;
        for (long b = 0; b < touched; b++) {
          const int bx = (int)(NextRandom(&seed) % (uint32_t)(plane->width / block)) * block;
          const int by = (int)(NextRandom(&seed) % (uint32_t)(plane->height / block)) * block;
          for (int y = by; y < by + block && y < plane->height; y++) {
            uint32_t *row = (uint32_t *)(plane->current + (size_t)y * plane->pitch);
            for (int x = bx; x < bx + block && x < plane->width; x++) row[x] += 1u;
          }
        }
        break;
      }
      default:
        break;
    }
  }
}

/* ------------------------------------------------------------------ tests -- */

typedef enum { kPartitionPlane, kPartitionRowBand } Partition;

typedef struct {
  PlaneSet *set;
  Partition partition;
  int bands_per_plane;
  /* A sink only, to stop the optimizer discarding the scan. Dirty AREA is
   * measured separately and untimed: accumulating it here would both perturb
   * the measurement and sum across every repeat. */
  volatile uint64_t checksum;
} ScanJob;

static void ScanRange(void *context, size_t first, size_t end) {
  ScanJob *job = context;
  uint64_t local = 0;
  for (size_t index = first; index < end; index++) {
    ArRenderRectI dirty = {0};
    if (job->partition == kPartitionPlane) {
      BenchPlane *plane = &job->set->planes[index];
      if (PresentationUploadMirror_FindDirtyRect(
              plane->current, plane->pitch, plane->mirror, plane->pitch,
              plane->width, plane->height, &dirty))
        local += (uint64_t)dirty.w * (uint64_t)dirty.h;
    } else {
      /* Row bands span the whole plane set so each part gets equal BYTES --
       * per-plane splitting is unbalanced once apron-carrying planes are
       * wider than the rest. The real kernel is still what runs: a band is
       * just an offset pointer and a smaller height. */
      const int bands = job->bands_per_plane;
      BenchPlane *plane = &job->set->planes[index / (size_t)bands];
      const int band = (int)(index % (size_t)bands);
      const int rows = plane->height / bands;
      const int y0 = band * rows;
      const int height =
          (band == bands - 1) ? (plane->height - y0) : rows;
      if (height <= 0) continue;
      if (PresentationUploadMirror_FindDirtyRect(
              plane->current + (size_t)y0 * plane->pitch, plane->pitch,
              plane->mirror + (size_t)y0 * plane->pitch, plane->pitch,
              plane->width, height, &dirty))
        local += (uint64_t)dirty.w * (uint64_t)dirty.h;
    }
  }
  job->checksum += local;
}

/* Total pixel area the bounding rectangles would upload, measured once and
 * untimed. This is the number that decides whether a single dirty rect is the
 * right granularity: when a sparse sprite plane reports nearly its whole area,
 * the scan has spent full read bandwidth to save nothing. */
static uint64_t ComputeDirtyArea(PlaneSet *set) {
  uint64_t area = 0;
  for (int i = 0; i < set->count; i++) {
    BenchPlane *plane = &set->planes[i];
    ArRenderRectI dirty = {0};
    if (PresentationUploadMirror_FindDirtyRect(
            plane->current, plane->pitch, plane->mirror, plane->pitch,
            plane->width, plane->height, &dirty))
      area += (uint64_t)dirty.w * (uint64_t)dirty.h;
  }
  return area;
}

typedef struct {
  PlaneSet *set;
  volatile uint64_t sink;
} CopyJob;

static void CopyRange(void *context, size_t first, size_t end) {
  CopyJob *job = context;
  for (size_t index = first; index < end; index++) {
    BenchPlane *plane = &job->set->planes[index];
    memcpy(plane->mirror, plane->current, (size_t)plane->pitch * (size_t)plane->height);
    job->sink += plane->mirror[0];
  }
}

typedef struct { volatile uint64_t sink; } EmptyJob;

static void EmptyRange(void *context, size_t first, size_t end) {
  EmptyJob *job = context;
  job->sink += end - first;
}

/* ----------------------------------------------------------------- output -- */

static FILE *g_json;
static bool g_first_result = true;

/* `bytes_exact` says whether bytes_touched is the real traffic. The dirty scan
 * short-circuits: memcmp stops at a row's first difference, and the edge scans
 * are skipped once the horizontal bounds saturate. So only the static case
 * actually reads both buffers in full, and a GB/s figure is meaningful only
 * there. Every other mode reports null rather than an invented rate. */
static void EmitResult(const char *test, const char *mode, int dirty_pct,
                       int planes, int width, int height, int threads,
                       bool pinned, const char *partition,
                       uint64_t bytes_touched, bool bytes_exact, Stats stats,
                       uint64_t dirty_area_px, uint64_t full_area_px) {
  if (!g_json) return;
  if (!g_first_result) fprintf(g_json, ",\n");
  g_first_result = false;
  const double seconds = (double)stats.median / 1e9;
  const double drift =
      stats.first_half ? ((double)stats.second_half - (double)stats.first_half) *
                             100.0 / (double)stats.first_half
                       : 0.0;
  fprintf(g_json,
          "    {\"test\":\"%s\",\"mode\":\"%s\",\"dirty_pct\":%d,"
          "\"planes\":%d,\"width\":%d,\"height\":%d,"
          "\"working_set_bytes\":%" PRIu64 ",\"threads\":%d,\"pinned\":%s,"
          "\"partition\":\"%s\",\"bytes_touched\":%" PRIu64 ","
          "\"ns_median\":%" PRIu64 ",\"ns_min\":%" PRIu64 ",\"ns_max\":%" PRIu64 ",",
          test, mode, dirty_pct, planes, width, height,
          (uint64_t)planes * (uint64_t)width * 4u * (uint64_t)height * 2u,
          threads, pinned ? "true" : "false", partition, bytes_touched,
          stats.median, stats.min, stats.max);
  if (bytes_exact && seconds > 0.0)
    fprintf(g_json, "\"gbps\":%.3f,", (double)bytes_touched / seconds / 1e9);
  else
    fprintf(g_json, "\"gbps\":null,");
  fprintf(g_json, "\"drift_pct\":%.2f,\"dirty_area_px\":%" PRIu64
          ",\"full_area_px\":%" PRIu64 ",\"dirty_frac\":%.4f}",
          drift, dirty_area_px, full_area_px,
          full_area_px ? (double)dirty_area_px / (double)full_area_px : 0.0);
  fflush(g_json);
}

/* ------------------------------------------------------------------- main -- */

typedef struct {
  int planes, width, height;
  int iterations, warmup;
  int thread_counts[8];
  int thread_count_count;
  bool pin;
  const char *preset;
  const char *json_path;
} Options;

static void Usage(const char *program) {
  fprintf(stderr,
      "usage: %s [options]\n"
      "  --preset NAME     l2 | l3 | real | large   (default: real)\n"
      "  --planes N        plane count (default from preset)\n"
      "  --width N         surface width in pixels\n"
      "  --height N        surface height in pixels\n"
      "  --iterations N    timed repeats per configuration (default 30)\n"
      "  --warmup N        untimed repeats first, to fault pages in (default 3)\n"
      "  --threads a,b,c   total workers incl. owner (default 1,2,3,4)\n"
      "  --no-pin          do not pin threads to physical cores (Linux only)\n"
      "  --json PATH       write results as JSON (default stdout summary only)\n",
      program);
}

/* Presets target the Deck's actual cache tiers: 512 KiB L2 per core and a
 * 4 MiB L3 shared by all four. "real" is the measured action geometry --
 * 256 display columns + widescreen margins + a 64-column apron per side. */
static void ApplyPreset(Options *options, const char *preset) {
  if (!strcmp(preset, "l2")) {
    options->planes = 2; options->width = 128; options->height = 112;
  } else if (!strcmp(preset, "l3")) {
    options->planes = 4; options->width = 256; options->height = 224;
  } else if (!strcmp(preset, "real")) {
    options->planes = 12; options->width = 486; options->height = 224;
  } else if (!strcmp(preset, "large")) {
    options->planes = 12; options->width = 640; options->height = 352;
  }
}

static void ParseThreadList(Options *options, const char *list) {
  options->thread_count_count = 0;
  const char *cursor = list;
  while (*cursor && options->thread_count_count < 8) {
    char *end = NULL;
    const long value = strtol(cursor, &end, 10);
    if (end == cursor) break;
    if (value > 0 && value <= 64)
      options->thread_counts[options->thread_count_count++] = (int)value;
    cursor = (*end == ',') ? end + 1 : end;
  }
}

int main(int argc, char **argv) {
  Options options = {
    .iterations = 30,
    .warmup = 3,
    .pin = true,
    .preset = "real",
    .thread_counts = {1, 2, 3, 4},
    .thread_count_count = 4,
  };
  ApplyPreset(&options, options.preset);

  for (int i = 1; i < argc; i++) {
    const char *argument = argv[i];
    const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (!strcmp(argument, "--preset") && value) {
      options.preset = value; ApplyPreset(&options, value); i++;
    } else if (!strcmp(argument, "--planes") && value) {
      options.planes = atoi(value); i++;
    } else if (!strcmp(argument, "--width") && value) {
      options.width = atoi(value); i++;
    } else if (!strcmp(argument, "--height") && value) {
      options.height = atoi(value); i++;
    } else if (!strcmp(argument, "--iterations") && value) {
      options.iterations = atoi(value); i++;
    } else if (!strcmp(argument, "--warmup") && value) {
      options.warmup = atoi(value); i++;
    } else if (!strcmp(argument, "--threads") && value) {
      ParseThreadList(&options, value); i++;
    } else if (!strcmp(argument, "--no-pin")) {
      options.pin = false;
    } else if (!strcmp(argument, "--json") && value) {
      options.json_path = value; i++;
    } else {
      Usage(argv[0]);
      return 2;
    }
  }
  if (options.planes <= 0 || options.width <= 0 || options.height <= 0 ||
      options.iterations <= 0 || options.warmup < 0 ||
      options.thread_count_count <= 0) {
    Usage(argv[0]);
    return 2;
  }

  PlaneSet set;
  if (!PlaneSetCreate(&set, options.planes, options.width, options.height)) {
    fprintf(stderr, "deckbench-cpu: out of memory allocating planes\n");
    return 1;
  }
  const uint64_t working_set =
      (uint64_t)set.bytes_per_plane * (uint64_t)options.planes * 2u;

  if (options.json_path) {
    g_json = fopen(options.json_path, "w");
    if (!g_json) {
      fprintf(stderr, "deckbench-cpu: cannot write %s: %s\n",
              options.json_path, strerror(errno));
      PlaneSetFree(&set);
      return 1;
    }
  }

  long online_cpus = 0;
#if defined(__linux__)
  online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
#endif

  if (g_json) {
    fprintf(g_json,
        "{\n  \"tool\": \"deckbench-cpu\",\n  \"version\": 1,\n"
        "  \"host\": {\"os\": \"%s\", \"online_cpus\": %ld},\n"
        "  \"config\": {\"preset\": \"%s\", \"planes\": %d, \"width\": %d,"
        " \"height\": %d, \"iterations\": %d, \"warmup\": %d, \"pin\": %s,"
        " \"working_set_bytes\": %" PRIu64 "},\n  \"results\": [\n",
#if defined(__linux__)
        "linux",
#elif defined(__APPLE__)
        "macos",
#else
        "other",
#endif
        online_cpus, options.preset, options.planes, options.width,
        options.height, options.iterations, options.warmup,
        options.pin ? "true" : "false", working_set);
  }

  printf("deckbench-cpu: %d planes %dx%d, working set %.2f MiB, %d iterations\n",
         options.planes, options.width, options.height,
         (double)working_set / (1024.0 * 1024.0), options.iterations);
  printf("%-10s %-8s %4s %3s %-8s %10s %10s %8s %8s\n",
         "test", "mode", "dpct", "thr", "part", "median_us", "GB/s",
         "dirty%", "drift%");

  uint64_t *samples = malloc((size_t)options.iterations * sizeof(*samples));
  if (!samples) { PlaneSetFree(&set); return 1; }

  const DirtyMode modes[] = {kDirtyStatic, kDirtyRegion, kDirtySparse, kDirtyScroll};
  const int sparse_pct = 5;   /* a few dozen sprite blocks */
  const int region_pct = 25;  /* a quarter of the surface */

  for (int t = 0; t < options.thread_count_count; t++) {
    const int threads = options.thread_counts[t];
    /* <=4 workers: one per PHYSICAL core (0,2,4,6). Above that we deliberately
     * land on SMT siblings, which share the L2 and line-fill buffers this
     * workload is starved of -- the point is to show that it hurts. */
    const int stride = (threads <= 4) ? 2 : 1;
    BenchPool *pool = PoolCreate(threads - 1, options.pin, stride);
    if (options.pin) PinToCpu(0);

    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
      const DirtyMode mode = modes[m];
      const int dirty_pct = (mode == kDirtySparse) ? sparse_pct
                          : (mode == kDirtyRegion) ? region_pct : 0;

      for (int p = 0; p < 2; p++) {
        const Partition partition = p ? kPartitionRowBand : kPartitionPlane;
        /* Enough bands that four workers can be fed evenly from one plane set
         * without splitting rows below a useful size. */
        const int bands = 8;
        ScanJob job = {.set = &set, .partition = partition,
                       .bands_per_plane = bands};
        const size_t count = (partition == kPartitionPlane)
            ? (size_t)set.count : (size_t)set.count * (size_t)bands;

        PlaneSetApplyDirty(&set, mode, dirty_pct);
        const uint64_t full_area =
            (uint64_t)options.width * (uint64_t)options.height *
            (uint64_t)set.count;
        const uint64_t dirty_area = ComputeDirtyArea(&set);

        /* Untimed first: the initial pass over a freshly malloc'd working
         * set faults every page in, which otherwise lands entirely in the
         * first half and shows up as huge negative "drift". */
        for (int w = 0; w < options.warmup; w++)
          PoolRun(pool, count, ScanRange, &job);
        for (int iteration = 0; iteration < options.iterations; iteration++) {
          const uint64_t started = NowNanos();
          PoolRun(pool, count, ScanRange, &job);
          samples[iteration] = NowNanos() - started;
        }
        const Stats stats = Summarize(samples, (size_t)options.iterations);
        /* Only the static case reads both buffers end to end; see EmitResult. */
        const uint64_t touched =
            (uint64_t)set.bytes_per_plane * (uint64_t)set.count * 2u;
        const bool exact = (mode == kDirtyStatic);
        printf("%-10s %-8s %4d %3d %-8s %10.1f %10s %8.1f %8.1f\n",
               "scan", DirtyModeName(mode), dirty_pct, threads,
               partition == kPartitionPlane ? "plane" : "rowband",
               (double)stats.median / 1000.0,
               exact ? "exact" : "n/a",
               100.0 * (double)dirty_area / (double)full_area,
               stats.first_half ? ((double)stats.second_half -
                                   (double)stats.first_half) * 100.0 /
                                      (double)stats.first_half : 0.0);
        EmitResult("scan", DirtyModeName(mode), dirty_pct, options.planes,
                   options.width, options.height, threads, options.pin,
                   partition == kPartitionPlane ? "plane" : "rowband",
                   touched, exact, stats, dirty_area, full_area);
      }
    }

    /* Mirror update bandwidth: the memcpy the scan pays for on every changed
     * region, and the copy structural fix C would delete outright. */
    {
      CopyJob job = {.set = &set};
      for (int w = 0; w < options.warmup; w++)
        PoolRun(pool, (size_t)set.count, CopyRange, &job);
      for (int iteration = 0; iteration < options.iterations; iteration++) {
        const uint64_t started = NowNanos();
        PoolRun(pool, (size_t)set.count, CopyRange, &job);
        samples[iteration] = NowNanos() - started;
      }
      const Stats stats = Summarize(samples, (size_t)options.iterations);
      const uint64_t touched =
          (uint64_t)set.bytes_per_plane * (uint64_t)set.count * 2u;
      char rate[16];
      snprintf(rate, sizeof(rate), "%.2f",
               (double)touched / ((double)stats.median / 1e9) / 1e9);
      printf("%-10s %-8s %4d %3d %-8s %10.1f %10s %8s %8.1f\n",
             "copy", "full", 100, threads, "plane",
             (double)stats.median / 1000.0, rate, "-",
             stats.first_half ? ((double)stats.second_half -
                                 (double)stats.first_half) * 100.0 /
                                    (double)stats.first_half : 0.0);
      EmitResult("copy", "full", 100, options.planes, options.width,
                 options.height, threads, options.pin, "plane", touched,
                 true, stats, 0, 0);
    }

    /* Dispatch + join with no work, which is what decides the smallest job
     * worth handing to a helper (HostParallelWork's minimum_per_part). */
    {
      EmptyJob job = {0};
      for (int w = 0; w < options.warmup; w++)
        PoolRun(pool, 1024, EmptyRange, &job);
      for (int iteration = 0; iteration < options.iterations; iteration++) {
        const uint64_t started = NowNanos();
        for (int repeat = 0; repeat < 100; repeat++)
          PoolRun(pool, 1024, EmptyRange, &job);
        samples[iteration] = (NowNanos() - started) / 100;
      }
      const Stats stats = Summarize(samples, (size_t)options.iterations);
      printf("%-10s %-8s %4d %3d %-8s %10.3f %10s %8s %8.1f\n",
             "forkjoin", "empty", 0, threads, "-",
             (double)stats.median / 1000.0, "-", "-",
             stats.first_half ? ((double)stats.second_half -
                                 (double)stats.first_half) * 100.0 /
                                    (double)stats.first_half : 0.0);
      EmitResult("forkjoin", "empty", 0, options.planes, options.width,
                 options.height, threads, options.pin, "-", 0, false,
                 stats, 0, 0);
    }

    PoolDestroy(pool);
  }

  if (g_json) {
    fprintf(g_json, "\n  ]\n}\n");
    fclose(g_json);
  }
  free(samples);
  PlaneSetFree(&set);
  return 0;
}
