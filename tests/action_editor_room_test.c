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
#include "diorama/diorama_camera_policy.h"

static void CameraPolicy(EditorRoomScene *room, const ActionSceneSnapshot *assets) {
  if (assets->scene.group != 1 || assets->scene.map != 1) return;
  assert(EditorRoomScene_Configure(room, ""));
  const int outputs[][2] = {{960,600},{960,540},{800,600},{1260,540}};
  const int positions[] = {0,1,960,1921,2880,3840};
  for (unsigned v = 0; v < sizeof(outputs)/sizeof(*outputs); ++v) {
    const DioramaView view = {.distance_scale = 1, .camera_framing_weight = 1,
      .visible_width = 496, .viewport = {0,0,outputs[v][0],outputs[v][1]}};
    for (unsigned i = 0; i < sizeof(positions)/sizeof(*positions); ++i) {
      EditorRoomCamera camera;
      assert(EditorRoomScene_ResolveCamera(room, positions[i], 300, 37, 120, 64,
          &view, kDioramaSky_Only, &camera));
      const bool wide = outputs[v][0] * 10 > outputs[v][1] * 16;
      /* Fillmore BG2 has an explicit 128px extent, so it must not grow an
       * apron even when the ordinary live-world BG1 does. */
      assert(camera.bg_apron_mask == (wide ? 1 : 0));
      assert(EditorRoomScene_Render(room, camera.x, 300, 37, 120, 64, &camera));
      const DioramaCapture *capture = EditorRoomScene_Capture(room);
      const SrSceneSurfaces *surface = EditorRoomScene_Surfaces(room);
      assert(surface->background_view && surface->view_width == 496);
      assert(capture->bg_apron_mask == camera.bg_apron_mask);
      /* Independently check the whole-room BG1 -> BG2 mapping, including the
       * fractional origin. A BG2-scroll-minus-margin implementation fails. */
      const float expected = (assets->scene.bg[1].pages_wide * 256 - 496) *
          (float)(camera.x - camera.travel.minimum) / (camera.travel.maximum - camera.travel.minimum);
      const float actual = capture->bg2_camera_x - 120 - capture->skybox->capture_offset.x +
          capture->skybox->subpixel_x;
      assert(fabsf(actual - expected) < .001f);
      if (!wide) {
        for (unsigned bg = 0; bg < 2; ++bg) for (unsigned band = 0; band < 3; ++band)
          for (int y = 0; y < surface->height; ++y) for (int x = 0; x < 64; ++x) {
            assert(!surface->bands[bg][band][y * surface->pitch_pixels + x]);
            assert(!surface->bands[bg][band][y * surface->pitch_pixels + 64 + surface->width + x]);
          }
      }
    }
  }
}

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
    assert(EditorRoomScene_Render(room, x, y, frame_clock + i, extra, vertical, NULL));
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
    if (source->effect.kind == kActionEffect_TempleGroundMist) {
      const ActionEffectInstance *e = &source->effect;
      ArRenderRectF guide;
      assert(EditorRoomScene_DefaultGuideCount(room, i) == 1);
      assert(EditorRoomScene_DefaultGuide(room, i, 0, &guide));
      assert(guide.x == e->world_x + e->geometry.data.rect.x0);
      assert(guide.y == e->world_y + e->geometry.data.rect.y0);
      assert(guide.x + guide.w == e->world_x + e->geometry.data.rect.x1);
      assert(guide.y + guide.h == e->world_y);
    }
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
    /* Match the browser oracle's default view, including its room-edge stop.
     * Benchmarks intentionally retain their explicitly requested capture. */
    EditorRoomCamera camera;
    if (!benchmark) {
      const DioramaView view = {.distance_scale = 1, .camera_framing_weight = 1,
          .visible_width = 256 + 2 * extra, .viewport = {0, 0, 960, 600}};
      assert(EditorRoomScene_ResolveCamera(room, x, y, frame, extra, vertical, &view, kDioramaSky_Only, &camera));
      x = camera.x;
    }
    assert(EditorRoomScene_Render(room, x, y, frame, extra, vertical, benchmark ? NULL : &camera));
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
    assert(!EditorRoomScene_Render(room, -1, y, frame, extra, vertical, NULL));
    assert(!EditorRoomScene_Configure(room, "[unterminated"));
    assert(EditorRoomScene_Hash(room) == hash);
    printf("%08x %08x\n", hash, EditorRoomScene_EffectHash(room));
  }
  CameraPolicy(room, &assets);
  if (assets.scene.group == 1 && assets.scene.map == 1 && !assets.terrain_profile) {
    assert(EditorRoomScene_Configure(room, ""));
    assert(EditorRoomScene_Render(room, 0, 400, 0, 0, 0, NULL));
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
    assert(EditorRoomScene_Render(room, 0, 400, 0, 0, 0, NULL));
    size_t at = (size_t)sy * surface->pitch_pixels + sx;
    assert(surface->bands[0][2][at] == pixel);
    assert(!surface->bands[0][0][at] && !surface->bands[0][1][at]);
    snprintf(edits, sizeof(edits), "[layers:01:01]\nbg1-pixels = cell:%d,%d transparent:"
        "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n", cx, cy);
    assert(EditorRoomScene_Configure(room, edits));
    assert(EditorRoomScene_Render(room, 0, 400, 0, 0, 0, NULL));
    for (int band = 0; band < 3; ++band) assert(!surface->bands[0][band][at]);
    assert(EditorRoomScene_Configure(room, ""));
    assert(EditorRoomScene_Render(room, 0, 400, 0, 0, 0, NULL));
    assert(EditorRoomScene_Hash(room) == original);
  }
  if (assets.scene.group == 2 && assets.scene.map == 1) {
    assert(EditorRoomScene_Configure(room, ""));
    assert(EditorRoomScene_Render(room, 1152, 240, 37, 120, 64, NULL));
    const SrSceneSurfaces *surface = EditorRoomScene_Surfaces(room);
    assert(EditorRoomScene_Capture(room)->authentic_y0 > 0);
    /* BG2 is at its own top even though the foreground can expose extra rows.
     * No water from the bottom of the native page may wrap above its sky. */
    for (int band = 0; band < 3; ++band)
      for (int col = 0; col < surface->pitch_pixels; ++col)
        assert(surface->bands[1][band][col] == 0);
  }
  if (assets.scene.group == 2 && assets.scene.map == 8) {
    /* Painted boss-room extensions must use their real map coordinates even
     * when the original room's mirror policy is retained. Compare each pixel
     * with the same two wall stamps under live-world fill, in every terrain. */
    const char *fills[] = {"world", "mirror", "repeat", "clamp", "transparent"};
    uint32_t expected[2][16][16];
    for (unsigned fill = 0; fill < sizeof(fills)/sizeof(fills[0]); ++fill) {
      char config[512];
      snprintf(config, sizeof(config), "[layers:02:08]\n"
          "bg1-policy = edge:%s\n"
          "bg1-stamp:us+jp+eu = cell:-3,8 metatile:34 words:58AB,58AA,58BB,58BA bands:1,1,1,1\n"
          "bg1-stamp:us+jp+eu = cell:18,8 metatile:33 words:18AA,18AB,18BA,18BB bands:1,1,1,1\n", fills[fill]);
      assert(EditorRoomScene_Configure(room, config));
      assert(EditorRoomScene_Render(room, 0, 0, 0, 120, 32, NULL));
      const SrSceneSurfaces *surface = EditorRoomScene_Surfaces(room);
      const DioramaCapture *capture = EditorRoomScene_Capture(room);
      unsigned opaque = 0;
      for (int side = 0; side < 2; ++side)
        for (int row = 0; row < 16; ++row)
          for (int col = 0; col < 16; ++col) {
            const int sx = 64 + 120 + (side ? 18 : -3) * 16 + col;
            const int sy = capture->authentic_y0 + 8 * 16 - 1 + row;
            const uint32_t pixel = surface->bands[0][0][sy * surface->pitch_pixels + sx];
            if (!fill) expected[side][row][col] = pixel;
            else assert(expected[side][row][col] == pixel);
            opaque += (pixel >> 24) != 0;
          }
      assert(opaque > 256);
    }
  }
  if (assets.scene.group == 7 && assets.scene.map == 5) {
    /* Death Heim's solid ceiling extension meets native map row 0. The PPU
     * samples scanline 1 at world row 1, so the last upper margin must retain
     * native row 0 instead of leaving a transparent line at the handoff. */
    assert(EditorRoomScene_Configure(room,
        "[layers:07:05]\n"
        "bg1-stamp:us+jp+eu = cell:0,-1 metatile:06 "
        "words:3072,3072,3072,3072 bands:2,2,2,2\n"));
    for (int camera_y = 0; camera_y <= 3; ++camera_y) {
      assert(EditorRoomScene_Render(room, 0, camera_y, 0, 0, 8, NULL));
      const SrSceneSurfaces *surface = EditorRoomScene_Surfaces(room);
      const int top = EditorRoomScene_Capture(room)->authentic_y0;
      const int handoff = top - camera_y - 1;
      const uint32_t *ceiling = surface->bands[0][1];
      assert(handoff > 0);
      const uint32_t solid = ceiling[(handoff - 1) * surface->pitch_pixels + 64];
      assert((solid >> 24) == 255);
      for (int row = handoff; row <= handoff + 1; ++row)
        for (int col = 64; col < 80; ++col)
          assert(ceiling[row * surface->pitch_pixels + col] == solid);
    }
  }
  EditorRoomScene_Destroy(room);
  return 0;
}
