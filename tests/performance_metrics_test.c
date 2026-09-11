#include "performance_metrics.h"
#include "performance_overlay.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); abort(); \
} } while (0)
#define NEAR(a, b) CHECK(fabs((a) - (b)) < 0.000001)
static uint64_t s_now, s_clock_reads;
static unsigned s_text_calls;
uint64_t HostClock_Nanoseconds(void) { s_clock_reads++; return s_now; }
uint64_t HostClock_Milliseconds(void) { return s_now / 1000000; }
void SettingsOverlay_DrawGameText(int x, int y, int scale, uint8_t alpha, const char *text) {
  (void)x; (void)y; (void)scale; (void)alpha; (void)text;
  s_text_calls++;
}

typedef struct MockRender {
  bool fail_create, fail_capture, fail_bind, fail_viewport, fail_restore, fail_clear, fail_draw;
  unsigned creates, destroys, captures, restores, clears, draws, geometry;
  ArRenderTexture target;
} MockRender;
static bool Create(void *p, const ArRenderTextureDesc *desc, ArRenderTexture *out) {
  MockRender *mock = p;
  mock->creates++;
  CHECK(desc->usage == kArRenderTextureUsage_Target);
  CHECK(desc->blend == kArRenderBlendMode_AlphaPremultiplied);
  if (mock->fail_create) return false;
  *out = (ArRenderTexture){mock->creates}; return true;
}
static void Destroy(void *p, ArRenderTexture texture) { (void)texture; ((MockRender *)p)->destroys++; }
static bool Capture(void *p, ArRenderTargetState *out) {
  MockRender *mock = p; mock->captures++;
  *out = (ArRenderTargetState){.target = mock->target, .valid = true};
  return !mock->fail_capture;
}
static bool Bind(void *p, ArRenderTexture texture) {
  MockRender *mock = p;
  if (mock->fail_bind) return false;
  mock->target = texture; return true;
}
static bool Viewport(void *p, const ArRenderRectI *rect) { (void)rect; return !((MockRender *)p)->fail_viewport; }
static bool Clip(void *p, const ArRenderRectI *rect) { (void)p; (void)rect; return true; }
static bool Restore(void *p, const ArRenderTargetState *state) {
  MockRender *mock = p; mock->restores++;
  if (mock->fail_restore) return false;
  mock->target = state->target; return true;
}
static bool Clear(void *p, ArRenderColorF color) {
  (void)color; MockRender *mock = p; mock->clears++; return !mock->fail_clear;
}
static bool Draw(void *p, ArRenderTexture texture, const ArRenderRectF *source,
    const ArRenderRectF *destination, const ArRenderDrawState *state) {
  (void)texture; (void)source; (void)destination; (void)state;
  MockRender *mock = p; mock->draws++; return !mock->fail_draw;
}
static bool Geometry(void *p, ArRenderTexture texture, const ArRenderVertex2D *v,
    int count, const int32_t *indices, int index_count, const ArRenderDrawState *state) {
  (void)texture; (void)v; (void)count; (void)indices; (void)index_count; (void)state;
  ((MockRender *)p)->geometry++; return true;
}

static void TestOverlayResources(void) {
  static const ArRenderBackendOps ops = {.struct_size = sizeof(ops), .create_texture = Create,
      .destroy_texture = Destroy, .capture_render_target_state = Capture, .set_render_target = Bind,
      .set_viewport = Viewport, .set_clip_rect = Clip, .restore_render_target_state = Restore,
      .clear = Clear, .draw_texture = Draw, .draw_geometry = Geometry};
  MockRender mock = {.target = {123}};
  ArRenderDevice device = {.ops = &ops, .context = &mock, .capabilities = {
      .flags = kArRenderCapability_RenderTargets | kArRenderCapability_ScopedRenderTargets}};
  PerformanceSnapshot snapshot = {.revision = 1, .ready = true};
  ArRenderExtentI output = {1280, 800};
  CHECK(PerformanceOverlay_Render(&device, &snapshot, 2, output));
  CHECK(mock.creates == 1 && mock.captures == 1 && mock.restores == 1 && mock.target.value == 123);
  const unsigned cold_glyphs = s_text_calls;
  CHECK(cold_glyphs > 0);
  for (int i = 0; i < 120; i++) CHECK(PerformanceOverlay_Render(&device, &snapshot, 2, output));
  CHECK(s_text_calls == cold_glyphs && mock.draws == 121 && mock.clears == 1);
  snapshot.revision++;
  CHECK(PerformanceOverlay_Render(&device, &snapshot, 2, output));
  CHECK(mock.creates == 1 && mock.clears == 2 && s_text_calls == cold_glyphs * 2);
  CHECK(PerformanceOverlay_Render(&device, &snapshot, 2, (ArRenderExtentI){640,480}));
  CHECK(mock.creates == 2 && mock.destroys == 1);
  CHECK(PerformanceOverlay_Render(&device, &snapshot, 0, output));
  CHECK(mock.destroys == 2);
  PerformanceOverlay_Reset(&device); CHECK(mock.destroys == 2);
  for (int failure = 0; failure < 8; failure++) {
    mock = (MockRender){.target = {123}, .fail_create = failure == 0,
        .fail_capture = failure == 1, .fail_bind = failure == 2,
        .fail_clear = failure == 3, .fail_draw = failure == 4,
        .fail_restore = failure == 5 || failure == 6, .fail_viewport = failure == 6};
    if (failure == 7) device.capabilities.flags = 0;
    const bool state_lost = failure == 5 || failure == 6;
    CHECK(PerformanceOverlay_Render(&device, &snapshot, 2, output) == !state_lost);
    if (!state_lost) {
      CHECK(mock.target.value == 123);
      const unsigned creates = mock.creates, captures = mock.captures, draws = mock.draws;
      const unsigned glyphs = s_text_calls;
      CHECK(PerformanceOverlay_Render(&device, &snapshot, 2, output));
      CHECK(s_text_calls > glyphs); /* direct fallback, no failed allocation/bind storm */
      CHECK(mock.creates == creates && mock.captures == captures && mock.draws == draws);
    }
    PerformanceOverlay_Reset(&device);
  }
}

static void Fresh(void) {
  PerformanceMetrics_Configure(false, false);
  PerformanceMetrics_Configure(true, false);
}

static void TestToggleAndWindow(void) {
  PerformanceMetrics_Configure(false, false);
  PerformanceScope disabled = PerformanceMetrics_Begin(kPerformance_Ppu);
  PerformanceMetrics_End(disabled);
  CHECK(!s_clock_reads);
  Fresh();
  const uint32_t epoch = PerformanceMetrics_Epoch();
  PerformanceScope stale = PerformanceMetrics_Begin(kPerformance_Ppu);
  Fresh();
  CHECK(epoch != PerformanceMetrics_Epoch());
  s_now += 1000000;
  PerformanceMetrics_End(stale);
  PerformanceMetrics_Record(epoch, kPerformance_Ppu, 123);
  PerformanceMetrics_Record(PerformanceMetrics_Epoch(), (PerformanceStage)-1, 123);
  PerformanceMetrics_Record(PerformanceMetrics_Epoch(), kPerformanceStage_Count, 123);
  PerformanceMetrics_Add(kPerformanceCount_Count, 123);
  PerformanceSnapshot snapshot;
  for (int i = 0; i <= 100; i++) {
    PerformanceMetrics_Record(PerformanceMetrics_Epoch(), kPerformance_Emulation, 2000000);
    PerformanceMetrics_Add(kPerformanceCount_Ticks, 2);
    PerformanceMetrics_PresentCompleted((uint64_t)i * 10000000);
    PerformanceMetrics_Snapshot(&snapshot);
    CHECK(snapshot.ready == (i == 100));
  }
  CHECK(snapshot.presents == 101);
  NEAR(snapshot.fps, 100); NEAR(snapshot.frame_mean_ms, 10);
  NEAR(snapshot.frame_p95_ms, 10); NEAR(snapshot.frame_max_ms, 10);
  NEAR(snapshot.stages[kPerformance_Emulation].mean_ms, 2);
  CHECK(snapshot.stages[kPerformance_Emulation].calls == 101);
  CHECK(snapshot.stages[kPerformance_Ppu].calls == 0);
  NEAR(snapshot.counts[kPerformanceCount_Ticks], 2);
  const uint64_t revision = snapshot.revision;
  PerformanceMetrics_Configure(true, false);
  PerformanceMetrics_Snapshot(&snapshot);
  CHECK(snapshot.revision == revision && snapshot.ready);
  PerformanceMetrics_Record(PerformanceMetrics_Epoch(), kPerformance_Upload, 6000000);
  PerformanceContext context = {.scene = kPerformanceScene_World, .width = 1280, .height = 800};
  PerformanceMetrics_SetContext(&context);
  PerformanceMetrics_Snapshot(&snapshot);
  CHECK(!snapshot.ready && snapshot.context.scene == kPerformanceScene_World);
  PerformanceMetrics_PresentCompleted(2000000000);
  PerformanceMetrics_SetContext(&context); /* Same fields must not reset the window. */
  PerformanceMetrics_PresentCompleted(3000000000);
  PerformanceMetrics_Snapshot(&snapshot);
  CHECK(snapshot.ready && snapshot.presents == 2);
  NEAR(snapshot.stages[kPerformance_Upload].mean_ms, 3);
  NEAR(snapshot.stages[kPerformance_Upload].maximum_ms, 6);
  CHECK(!snapshot.stages[kPerformance_Emulation].calls);
}

static int Worker(void *context) {
  const uint32_t epoch = *(const uint32_t *)context;
  for (int i = 0; i < 10000; i++) {
    PerformanceMetrics_Record(epoch, kPerformance_WorkHelpers, 1000);
    PerformanceMetrics_Add(kPerformanceCount_HelperJobs, 1);
  }
  return 0;
}
static void TestParallelAndCapacity(void) {
  Fresh();
  uint32_t epoch = PerformanceMetrics_Epoch();
  SDL_Thread *threads[3];
  for (int i = 0; i < 3; i++) {
    threads[i] = SDL_CreateThread(Worker, "metric-test", &epoch);
    CHECK(threads[i]);
  }
  Worker(&epoch);
  for (int i = 0; i < 3; i++) SDL_WaitThread(threads[i], NULL);
  PerformanceMetrics_PresentCompleted(0);
  PerformanceMetrics_PresentCompleted(1000000000);
  PerformanceSnapshot snapshot;
  PerformanceMetrics_Snapshot(&snapshot);
  CHECK(snapshot.stages[kPerformance_WorkHelpers].calls == 40000);
  NEAR(snapshot.stages[kPerformance_WorkHelpers].mean_ms, 20);
  NEAR(snapshot.stages[kPerformance_WorkHelpers].maximum_ms, .001);
  NEAR(snapshot.counts[kPerformanceCount_HelperJobs], 20000);
  Fresh();
  PerformanceMetrics_PresentCompleted(0);
  PerformanceMetrics_PresentCompleted(200000000); /* Maximum survives ring eviction. */
  for (int i = 1; i <= 800; i++)
    PerformanceMetrics_PresentCompleted(200000000 + (uint64_t)i * 1000000);
  PerformanceMetrics_Snapshot(&snapshot);
  CHECK(snapshot.ready);
  NEAR(snapshot.frame_max_ms, 200);
  NEAR(snapshot.frame_p95_ms, 1);
}

static void TestOverlayLayout(void) {
  PerformanceSnapshot snapshot = {.ready = true, .fps = 40, .frame_mean_ms = 25};
  const ArRenderExtentI sizes[] = {{1280,800}, {800,1280}, {640,480}, {560,390}, {320,240}, {240,120}, {0,0}};
  for (int i = 0; i < kPerformanceStage_Count; i++) {
    const char *name = PerformanceMetrics_StageName((PerformanceStage)i);
    CHECK(name && *name);
    snapshot.stages[i] = (PerformanceValue){1.25, 4.5, 5};
  }
  CHECK(!strcmp(PerformanceMetrics_StageName((PerformanceStage)-1), "unknown"));
  for (int scene = 0; scene < kPerformanceScene_Count; scene++)
    for (int host = 0; host < 4; host++)
      for (int level = 0; level <= 2; level++)
        for (size_t s = 0; s < sizeof(sizes) / sizeof(*sizes); s++) {
          snapshot.context = (PerformanceContext){.scene = (PerformanceScene)scene, .host_mode = host};
          PerformanceOverlayModel model;
          PerformanceOverlay_Build(&snapshot, level, sizes[s], &model);
          if (!level || !sizes[s].width) { CHECK(!model.line_count); continue; }
          CHECK(model.panel.x >= 0 && model.panel.y >= 0);
          CHECK(model.panel.x + model.panel.w <= sizes[s].width);
          CHECK(model.panel.y + model.panel.h <= sizes[s].height);
          CHECK(model.line_count > 0 && model.line_count <= kPerformanceOverlay_MaxLines);
          for (int line = 0; line < model.line_count; line++) {
            const PerformanceOverlayLine *text = &model.lines[line];
            CHECK(text->x >= model.panel.x && text->y >= model.panel.y);
            CHECK(text->x + (int)strlen(text->text) * 8 * model.scale <= model.panel.x + model.panel.w);
            CHECK(text->y + 8 * model.scale <= model.panel.y + model.panel.h);
          }
        }
}

int main(void) {
  TestToggleAndWindow();
  TestParallelAndCapacity();
  TestOverlayLayout();
  TestOverlayResources();
  PerformanceMetrics_Configure(false, false);
  puts("performance_metrics_test: PASS");
  return 0;
}
