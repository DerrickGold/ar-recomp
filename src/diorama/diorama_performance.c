#include "diorama_performance.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/host_clock.h"

enum { kDioramaPerformanceWindowNs = 1000000000 };

typedef struct DioramaPerformanceCounter {
  uint64_t elapsed_ns;
  uint64_t calls;
} DioramaPerformanceCounter;

typedef struct DioramaPerformanceData {
  DioramaPerformanceCounter counters[kDioramaPerformanceStage_Count];
  uint64_t window_started_ns;
  uint64_t presentations;
  uint64_t plane_syncs;
  uint64_t plane_sync_failures;
  uint64_t texture_uploads;
  uint64_t upload_bytes;
  uint64_t draws;
  uint64_t draw_failures;
  uint64_t vertices;
  uint64_t indices;
  uint64_t viewport_pixels;
  uint64_t fragment_pixels[kArRenderBlendMode_Multiply + 1];
  uint64_t plane_fragment_pixels[32];
  uint64_t plane_draws[32];
} DioramaPerformanceData;

static DioramaPerformanceData s_data;
static atomic_flag s_data_lock = ATOMIC_FLAG_INIT;
static atomic_int s_enabled_state;
static atomic_int s_viewport_width;
static atomic_int s_viewport_height;
static atomic_int s_current_plane;

static void LockPerformanceData(void) {
  while (atomic_flag_test_and_set_explicit(
      &s_data_lock, memory_order_acquire)) {}
}

static void UnlockPerformanceData(void) {
  atomic_flag_clear_explicit(&s_data_lock, memory_order_release);
}

static const char *const kDioramaStageNames[] = {
  "total", "upload", "frame-analysis", "producer-setup", "scanout",
  "producer-finish", "host-post", "frame-synthesis", "mesh",
  "supersample", "dof-source", "submit", "callback",
};
_Static_assert(
    sizeof(kDioramaStageNames) / sizeof(kDioramaStageNames[0]) ==
        kDioramaPerformanceStage_Count,
    "every Diorama performance stage needs a report label");

bool DioramaPerformance_Enabled(void) {
  enum { kUnknown, kDisabled, kEnabled };
  int state = atomic_load_explicit(&s_enabled_state, memory_order_acquire);
  if (state != kUnknown) return state == kEnabled;
  const int resolved = getenv("AR_PERF") || getenv("AR_ACTION_PERF")
      ? kEnabled : kDisabled;
  int expected = kUnknown;
  (void)atomic_compare_exchange_strong_explicit(
      &s_enabled_state, &expected, resolved,
      memory_order_release, memory_order_acquire);
  return atomic_load_explicit(
      &s_enabled_state, memory_order_acquire) == kEnabled;
}

static uint64_t DioramaPerformanceNow(void) {
  return HostClock_Nanoseconds();
}

DioramaPerformanceScope DioramaPerformance_Begin(
    DioramaPerformanceStage stage) {
  DioramaPerformanceScope scope = {.stage = stage};
  if (!DioramaPerformance_Enabled() || stage < 0 ||
      stage >= kDioramaPerformanceStage_Count)
    return scope;
  scope.started_ns = DioramaPerformanceNow();
  scope.active = true;
  return scope;
}

void DioramaPerformance_End(DioramaPerformanceScope scope) {
  if (!scope.active) return;
  const uint64_t elapsed_ns =
      HostClock_Nanoseconds() - scope.started_ns;
  LockPerformanceData();
  DioramaPerformanceCounter *counter = &s_data.counters[scope.stage];
  counter->elapsed_ns += elapsed_ns;
  counter->calls++;
  UnlockPerformanceData();
}

void DioramaPerformance_AddPlaneSync(bool succeeded, bool uploaded,
                                     uint64_t uploaded_bytes) {
  if (!DioramaPerformance_Enabled()) return;
  LockPerformanceData();
  s_data.plane_syncs++;
  if (!succeeded) {
    s_data.plane_sync_failures++;
  } else if (uploaded) {
    s_data.texture_uploads++;
    s_data.upload_bytes += uploaded_bytes;
  }
  UnlockPerformanceData();
}

void DioramaPerformance_SetRasterViewport(int width, int height) {
  if (!DioramaPerformance_Enabled()) return;
  if (width < 0) width = 0;
  if (height < 0) height = 0;
  atomic_store_explicit(&s_viewport_width, width, memory_order_release);
  atomic_store_explicit(&s_viewport_height, height, memory_order_release);
}

void DioramaPerformance_SetViewport(int width, int height) {
  if (!DioramaPerformance_Enabled()) return;
  DioramaPerformance_SetRasterViewport(width, height);
  LockPerformanceData();
  if (width > 0 && height > 0)
    s_data.viewport_pixels += (uint64_t)width * (uint64_t)height;
  UnlockPerformanceData();
}

void DioramaPerformance_SetPlane(int plane) {
  if (!DioramaPerformance_Enabled()) return;
  atomic_store_explicit(&s_current_plane, plane, memory_order_release);
}

typedef struct DioramaCoveragePoint {
  double x, y;
} DioramaCoveragePoint;

static int ClipCoverageEdge(
    const DioramaCoveragePoint *input, int input_count,
    DioramaCoveragePoint *output, int axis, double boundary,
    bool keep_greater) {
  if (input_count <= 0) return 0;
  int output_count = 0;
  DioramaCoveragePoint previous = input[input_count - 1];
  double previous_value = axis ? previous.y : previous.x;
  bool previous_inside = keep_greater
      ? previous_value >= boundary : previous_value <= boundary;
  for (int i = 0; i < input_count; i++) {
    const DioramaCoveragePoint current = input[i];
    const double current_value = axis ? current.y : current.x;
    const bool current_inside = keep_greater
        ? current_value >= boundary : current_value <= boundary;
    if (current_inside != previous_inside) {
      const double span = current_value - previous_value;
      const double t = span != 0.0
          ? (boundary - previous_value) / span : 0.0;
      output[output_count++] = (DioramaCoveragePoint){
        previous.x + (current.x - previous.x) * t,
        previous.y + (current.y - previous.y) * t,
      };
    }
    if (current_inside) output[output_count++] = current;
    previous = current;
    previous_value = current_value;
    previous_inside = current_inside;
  }
  return output_count;
}

static double ClippedTriangleArea(
    const ArRenderVertex2D *a, const ArRenderVertex2D *b,
    const ArRenderVertex2D *c, int width, int height) {
  DioramaCoveragePoint buffers[2][8] = {
    {
      {(double)a->position.x, (double)a->position.y},
      {(double)b->position.x, (double)b->position.y},
      {(double)c->position.x, (double)c->position.y},
    },
  };
  int count = 3;
  int source = 0;
  static const struct {
    int axis;
    bool keep_greater;
  } edges[] = {
    {0, true}, {0, false}, {1, true}, {1, false},
  };
  for (int edge = 0; edge < 4 && count > 0; edge++) {
    const double boundary = edge == 1 ? (double)width
        : edge == 3 ? (double)height : 0.0;
    count = ClipCoverageEdge(
        buffers[source], count, buffers[1 - source],
        edges[edge].axis, boundary, edges[edge].keep_greater);
    source = 1 - source;
  }
  double twice_area = 0.0;
  for (int i = 0; i < count; i++) {
    const DioramaCoveragePoint p = buffers[source][i];
    const DioramaCoveragePoint q = buffers[source][(i + 1) % count];
    twice_area += p.x * q.y - q.x * p.y;
  }
  return twice_area < 0.0 ? -0.5 * twice_area : 0.5 * twice_area;
}

static uint64_t DrawCoveragePixels(
    const ArRenderVertex2D *vertices, int vertex_count,
    const int32_t *indices, int index_count, int width, int height) {
  if (!vertices || vertex_count <= 0 || !indices || index_count < 3 ||
      width <= 0 || height <= 0)
    return 0;
  double area = 0.0;
  for (int i = 0; i + 2 < index_count; i += 3) {
    const int32_t ia = indices[i];
    const int32_t ib = indices[i + 1];
    const int32_t ic = indices[i + 2];
    if (ia < 0 || ib < 0 || ic < 0 ||
        ia >= vertex_count || ib >= vertex_count || ic >= vertex_count)
      continue;
    area += ClippedTriangleArea(
        &vertices[ia], &vertices[ib], &vertices[ic], width, height);
  }
  return area > 0.0 ? (uint64_t)(area + 0.5) : 0;
}

void DioramaPerformance_AddDraw(
    bool succeeded, const ArRenderVertex2D *vertices, int vertex_count,
    const int32_t *indices, int index_count, ArRenderBlendMode blend) {
  if (!DioramaPerformance_Enabled()) return;
  const int width = atomic_load_explicit(
      &s_viewport_width, memory_order_acquire);
  const int height = atomic_load_explicit(
      &s_viewport_height, memory_order_acquire);
  const int plane = atomic_load_explicit(
      &s_current_plane, memory_order_acquire);
  const uint64_t coverage = succeeded
      ? DrawCoveragePixels(
            vertices, vertex_count, indices, index_count, width, height)
      : 0;
  LockPerformanceData();
  s_data.draws++;
  if (!succeeded) {
    s_data.draw_failures++;
  } else {
    if (vertex_count > 0) s_data.vertices += (uint64_t)vertex_count;
    if (index_count > 0) s_data.indices += (uint64_t)index_count;
    if (blend >= kArRenderBlendMode_Opaque &&
        blend <= kArRenderBlendMode_Multiply)
      s_data.fragment_pixels[blend] += coverage;
    if (plane >= 0 && plane < 32) {
      s_data.plane_fragment_pixels[plane] += coverage;
      s_data.plane_draws[plane]++;
    }
  }
  UnlockPerformanceData();
}

static void DioramaPerformanceReport(const DioramaPerformanceData *data,
                                     uint64_t window_ns) {
  const double presentations =
      data->presentations ? (double)data->presentations : 1.0;
  uint64_t child_ns = 0;
  /* Upload and frame analysis run before presentation ownership transfers to
   * the compositor, so they are reported beside total but are not children. */
  for (int stage = kDioramaPerformance_FrameSynthesis;
       stage < kDioramaPerformanceStage_Count; stage++)
    child_ns += data->counters[stage].elapsed_ns;
  const uint64_t total_ns =
      data->counters[kDioramaPerformance_Total].elapsed_ns;
  const uint64_t other_ns = total_ns > child_ns ? total_ns - child_ns : 0;

  fprintf(stderr, "[diorama-perf] presents=%" PRIu64 " window=%.2fs",
          data->presentations, (double)window_ns / 1000000000.0);
  for (int stage = 0; stage < kDioramaPerformanceStage_Count; stage++) {
    const DioramaPerformanceCounter *counter = &data->counters[stage];
    if (!counter->calls) continue;
    fprintf(stderr, " %s=%.3fms", kDioramaStageNames[stage],
            (double)counter->elapsed_ns / 1000000.0 / presentations);
  }
  fprintf(stderr, " other=%.3fms\n",
          (double)other_ns / 1000000.0 / presentations);

  const uint64_t alpha_fragments =
      data->fragment_pixels[kArRenderBlendMode_Alpha] +
      data->fragment_pixels[kArRenderBlendMode_AlphaPremultiplied];
  const uint64_t add_fragments =
      data->fragment_pixels[kArRenderBlendMode_Add] +
      data->fragment_pixels[kArRenderBlendMode_AddPremultiplied];
  fprintf(stderr,
          "[diorama-work] sync/present=%.1f uploads/present=%.1f "
          "upload-KiB/present=%.1f draws/present=%.1f "
          "vertices/present=%.1f indices/present=%.1f "
          "failures={sync:%" PRIu64 ",draw:%" PRIu64 "}\n",
          (double)data->plane_syncs / presentations,
          (double)data->texture_uploads / presentations,
          (double)data->upload_bytes / 1024.0 / presentations,
          (double)data->draws / presentations,
          (double)data->vertices / presentations,
          (double)data->indices / presentations,
          data->plane_sync_failures, data->draw_failures);

  const double viewport_pixels = data->viewport_pixels
      ? (double)data->viewport_pixels : 1.0;
  uint64_t total_fragments = 0;
  for (int blend = kArRenderBlendMode_Opaque;
       blend <= kArRenderBlendMode_Multiply; blend++)
    total_fragments += data->fragment_pixels[blend];
  fprintf(stderr,
          "[diorama-fill] screen-layers=%.2fx fragments/present=%.2fM "
          "opaque=%.2fM alpha=%.2fM add=%.2fM\n",
          (double)total_fragments / viewport_pixels,
          (double)total_fragments / presentations / 1000000.0,
          (double)data->fragment_pixels[kArRenderBlendMode_Opaque] /
              presentations / 1000000.0,
          (double)alpha_fragments / presentations / 1000000.0,
          (double)add_fragments / presentations / 1000000.0);
  for (int plane = 0; plane < 32; plane++) {
    if (!data->plane_draws[plane]) continue;
    fprintf(stderr,
            "  plane %2d: draws/present=%.1f screen-layers=%.2fx "
            "fragments/present=%.2fM\n",
            plane, (double)data->plane_draws[plane] / presentations,
            (double)data->plane_fragment_pixels[plane] / viewport_pixels,
            (double)data->plane_fragment_pixels[plane] /
                presentations / 1000000.0);
  }
}

void DioramaPerformance_PresentCompleted(void) {
  if (!DioramaPerformance_Enabled()) return;
  const uint64_t now = DioramaPerformanceNow();
  DioramaPerformanceData snapshot;
  bool report = false;

  LockPerformanceData();
  if (!s_data.window_started_ns) s_data.window_started_ns = now;
  s_data.presentations++;
  const uint64_t window_ns = now - s_data.window_started_ns;
  if (window_ns >= (uint64_t)kDioramaPerformanceWindowNs) {
    snapshot = s_data;
    memset(&s_data, 0, sizeof(s_data));
    s_data.window_started_ns = now;
    report = true;
  }
  UnlockPerformanceData();

  if (report) DioramaPerformanceReport(&snapshot, window_ns);
}
