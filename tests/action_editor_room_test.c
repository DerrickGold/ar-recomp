/* Native oracle for full-room scanout. The browser test generates its assets
 * from the embedded catalogue; the renderer itself never needs a capture. */
#include "room_scene.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "action/action_effect_render.h"

static char *Read(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb"); if (!f) return NULL;
  if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
  long n = ftell(f); rewind(f);
  if (n < 0 || n > 1024 * 1024) { fclose(f); return NULL; }
  char *p = malloc((size_t)n + 1);
  if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
  fclose(f); p[n] = 0; *size = n; return p;
}
static ActionSceneEffectRenderBatch benchmark_batch;
static bool BenchmarkProject(void *context, const ActionEffectInstance *e, float x, float y,
                             ArRenderPointF *p) {
  const ActionEnvironmentScene *scene = context;
  const bool bg2 = e->projection_plane == kActionEffectProjectionPlane_Bg2 ||
                   e->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds;
  *p = (ArRenderPointF){e->world_x + x + (bg2 ? scene->camera_x[0] - scene->camera_x[1] : 0),
                        e->world_y + y + (bg2 ? scene->camera_y[0] - scene->camera_y[1] : 0)};
  return true;
}
static bool BenchmarkClip(void *context, const ActionEffectInstance *e, ActionEffectLocalRect *r) {
  const ActionEnvironmentScene *scene = context;
  ArRenderPointF origin;
  BenchmarkProject(context, e, 0, 0, &origin);
  *r = e->geometry.data.rect;
  r->x0 = fmaxf(r->x0, scene->camera_x[0] - 256 - origin.x);
  r->x1 = fminf(r->x1, scene->camera_x[0] + 512 - origin.x);
  r->y0 = fmaxf(r->y0, scene->camera_y[0] - 64 - origin.y);
  r->y1 = fminf(r->y1, scene->camera_y[0] + 288 - origin.y);
  return r->x1 > r->x0 && r->y1 > r->y0;
}
static void Benchmark(EditorRoomScene *room, int x, int y, unsigned frame_clock, int extra,
                      int vertical) {
  enum { iterations = 500 };
  unsigned vertices = 0, indices = 0;
  const clock_t start = clock();
  double geometry = 0;
  clock_t capture_ticks = 0;
  static ActionMoonlightOcclusion capture_scratch;
  for (unsigned i = 0; i < iterations; ++i) {
    assert(EditorRoomScene_Render(room, x, y, frame_clock + i, extra, vertical));
    const clock_t before = clock();
    const ActionSceneEffectFrame *frame = EditorRoomScene_Effects(room);
    const ActionEnvironmentScene *scene = EditorRoomScene_Environment(room);
    unsigned v = 0, n = 0;
    for (unsigned layer = 0; layer < kActionEffectRenderLayer_Count; ++layer) {
      assert(ActionSceneDecorationRender_Build(frame, layer, true, true, BenchmarkProject,
                                               BenchmarkClip, (void *)scene, &benchmark_batch));
      v += benchmark_batch.vertex_count;
      n += benchmark_batch.index_count;
    }
    if (v > vertices) vertices = v;
    if (n > indices) indices = n;
    geometry += (double)(clock() - before) / CLOCKS_PER_SEC;
    const clock_t capture_start = clock();
    assert(ActionEnvironmentScene_CaptureScenery(scene, &capture_scratch));
    capture_ticks += clock() - capture_start;
  }
  const ActionEnvironmentScene *scene = EditorRoomScene_Environment(room);
  const ActionSceneEffectFrame *frame = EditorRoomScene_Effects(room);
  printf("Room CPU %u:%u, extension %d/%d: PPU/capture + effect meshes %.3f ms/frame, effect "
         "meshes %.3f ms, opacity capture %.3f ms, max %u vertices/%u indices, %u opacity runs (%u "
         "frames, no GPU).\n",
         scene->group, scene->room, extra, vertical,
         (double)(clock() - start - capture_ticks) * 1000 / CLOCKS_PER_SEC / iterations,
         geometry * 1000 / iterations, (double)capture_ticks * 1000 / CLOCKS_PER_SEC / iterations,
         vertices, indices, frame->scenery.count, iterations);
}

int main(int argc, char **argv) {
  const bool benchmark = argc > 3 && !strcmp(argv[argc - 1], "--benchmark");
  if (benchmark) --argc;
  if (argc != 3 && argc != 4) return 2;
  size_t n;
  char *bytes = Read(argv[1], &n);
  static ActionSceneSnapshot assets;
  assert(bytes && ActionSceneSnapshot_Decode((const uint8_t *)bytes, n, &assets)); free(bytes);
  EditorRoomScene *room = EditorRoomScene_Create(&assets); assert(room);
  char *ini = Read(argv[2], &n); assert(ini && EditorRoomScene_Configure(room, ini)); free(ini);
  const uint32_t catalogue_before = EditorRoomScene_EffectHash(room);
  const clock_t catalogue_start = clock();
  const unsigned catalogue_count = EditorRoomScene_DefaultSourceCount(room);
  const double catalogue_ms = (double)(clock() - catalogue_start) * 1000 / CLOCKS_PER_SEC;
  assert(catalogue_count < 1024);
  assert(EditorRoomScene_DefaultSourceCount(room) == catalogue_count);
  assert(EditorRoomScene_EffectHash(room) == catalogue_before);
  for (unsigned i = 0; i < catalogue_count; ++i) {
    const EditorRoomEffectSource *source = EditorRoomScene_DefaultSource(room, i);
    assert(source && isfinite(source->map_scale_x) && source->map_scale_x > 0);
    assert(isfinite(source->map_scale_y) && source->map_scale_y > 0);
    for (unsigned j = 0; j < i; ++j) {
      const ActionEffectInstance *prior = &EditorRoomScene_DefaultSource(room, j)->effect;
      assert(prior->kind != source->effect.kind || prior->generation != source->effect.generation);
    }
  }
  if (benchmark)
    printf("Default source inventory: %u sources, %.3f ms once per room.\n", catalogue_count,
           catalogue_ms);
  if (argc == 4) {
    char *effects = Read(argv[3],&n); unsigned line;
    assert(effects && EditorRoomScene_ConfigureEffects(room,effects,n,&line));free(effects);
  }
  int x, y, extra, vertical; unsigned frame;
  while (scanf("%d %d %u %d %d", &x, &y, &frame, &extra, &vertical) == 5) {
    assert(EditorRoomScene_Render(room, x, y, frame, extra, vertical));
    const ActionSceneEffectFrame *effects = EditorRoomScene_Effects(room);
    bool directional = false;
    for (unsigned i = 0; i < effects->decoration_count; ++i)
      directional |= effects->decorations[i].kind == kActionEffect_BloodpoolMoonlight ||
                     effects->decorations[i].kind == kActionEffect_ForestForwardLight;
    if (directional) {
      if (!effects->scenery.valid)
        fprintf(stderr, "occlusion capture overflow %u:%u at %d,%d\n", assets.scene.group,
                assets.scene.map, x, y);
      assert(effects->scenery.valid);
    }
    if (benchmark) {
      Benchmark(room, x, y, frame, extra, vertical);
      break;
    }
    const uint32_t hash = EditorRoomScene_Hash(room);
    assert(!EditorRoomScene_Render(room, -1, y, frame, extra, vertical));
    assert(!EditorRoomScene_Configure(room, "[unterminated"));
    assert(EditorRoomScene_Hash(room) == hash);
    printf("%08x %08x\n", hash, EditorRoomScene_EffectHash(room));
  }
  if (assets.scene.group == 1 && assets.scene.map == 1 && !assets.terrain_profile) {
    assert(EditorRoomScene_Configure(room, ""));
    assert(EditorRoomScene_Render(room, 0, 400, 0, 0, 0));
    uint32_t original = EditorRoomScene_Hash(room), pixel = 0;
    const SrSceneSurfaces *surface = EditorRoomScene_Surfaces(room);
    int sx = 0, sy = 0;
    for (int row = 0; row < 224 && !pixel; ++row)
      for (int col = 64; col < 320 && !pixel; ++col)
        for (int band = 0; band < 3 && !pixel; ++band)
          if ((pixel = surface->bands[0][band][row * surface->pitch_pixels + col])) {
            sx = col; sy = row;
          }
    assert(pixel);
    const int cx = (sx - 64) / 16, cy = (400 + sy + 1) / 16;
    char edits[256];
    snprintf(edits, sizeof(edits), "[layers:01:01]\nbg1-virtual = cells:%d,%d-%d,%d band:0\n", cx, cy, cx, cy);
    assert(EditorRoomScene_Configure(room, edits));
    assert(EditorRoomScene_Render(room, 0, 400, 0, 0, 0));
    size_t at = (size_t)sy * surface->pitch_pixels + sx;
    assert(surface->bands[0][2][at] == pixel);
    assert(!surface->bands[0][0][at] && !surface->bands[0][1][at]);
    snprintf(edits, sizeof(edits), "[layers:01:01]\nbg1-pixels = cell:%d,%d transparent:"
        "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n", cx, cy);
    assert(EditorRoomScene_Configure(room, edits));
    assert(EditorRoomScene_Render(room, 0, 400, 0, 0, 0));
    for (int band = 0; band < 3; ++band) assert(!surface->bands[0][band][at]);
    assert(EditorRoomScene_Configure(room, ""));
    assert(EditorRoomScene_Render(room, 0, 400, 0, 0, 0));
    assert(EditorRoomScene_Hash(room) == original);
  }
  if (assets.scene.group == 2 && assets.scene.map == 1) {
    assert(EditorRoomScene_Configure(room, ""));
    assert(EditorRoomScene_Render(room, 1152, 240, 37, 120, 64));
    const SrSceneSurfaces *surface = EditorRoomScene_Surfaces(room);
    assert(EditorRoomScene_Capture(room)->authentic_y0 > 0);
    /* BG2 is at its own top even though the foreground can expose extra rows.
     * No water from the bottom of the native page may wrap above its sky. */
    for (int band = 0; band < 3; ++band)
      for (int col = 0; col < surface->pitch_pixels; ++col)
        assert(surface->bands[1][band][col] == 0);
  }
  EditorRoomScene_Destroy(room);
  return 0;
}
