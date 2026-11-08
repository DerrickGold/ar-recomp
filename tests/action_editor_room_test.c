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
#include "action/action_lava_bubbles.h"
#include "action/action_voxel_internal.h"
#include "diorama/diorama_camera_policy.h"
#include "render/presentation_layout.h"

static void CameraPolicy(EditorRoomScene *room, const ActionSceneSnapshot *assets) {
  if (assets->scene.group != 1 || assets->scene.map != 1) return;
  assert(EditorRoomScene_Configure(room, ""));
  assert(!*EditorRoomScene_SceneryError(room));
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
      const bool voxel=EditorRoomScene_3DMode(room)==kAction3D_Voxel &&
          EditorRoomScene_Scene(room)->voxel_geometry;
      /* Fillmore BG2 has an explicit 128px extent, so it must not grow an
       * apron even when the ordinary live-world BG1 does. */
      assert(camera.bg_apron_mask == (wide || voxel ? 1 : 0));
      /* Resolve travel with the original camera policy. The additional Voxel
       * pixels must be a capture-only change even at finite room edges. */
      ActionBgPlan plan;assert(EditorRoomScene_BgPolicy(room,positions[i],300,37,false,&plan,NULL));
      DioramaCapture old_capture=DioramaCameraPolicy_Capture(&plan,NULL,
          positions[i],300,120,64,wide?1:0);
      DioramaScene old_scene=*EditorRoomScene_Scene(room);
      DioramaRenderOptions options=*old_scene.render;options.skybox=kDioramaSky_Only;old_scene.render=&options;
      int lo,hi;
      if(Diorama_ResolveCaptureCameraRange(&old_capture,&view,&old_scene,positions[i],&lo,&hi)) {
        ActionCameraAxisBounds expected;
        assert(ActionCameraAxisBounds_Resolve(EditorRoomScene_Width(room),256,
            lo>0?lo:0,(int)EditorRoomScene_Width(room)-256-hi>0?(int)EditorRoomScene_Width(room)-256-hi:0,&expected));
        assert(camera.travel.minimum==expected.minimum && camera.travel.maximum==expected.maximum);
      }
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
        for (unsigned bg = voxel?1:0; bg < 2; ++bg) for (unsigned band = 0; band < 3; ++band)
          for (int y = 0; y < surface->height; ++y) for (int x = 0; x < 64; ++x) {
            assert(!surface->bands[bg][band][y * surface->pitch_pixels + x]);
            assert(!surface->bands[bg][band][y * surface->pitch_pixels + 64 + surface->width + x]);
          }
      }
    }
  }
  /* Toggle back to both shipped modes: the extra BG1 source exists only while
   * compiled Voxel geometry is active, with no new raster work on yaw alone. */
  for(unsigned mode=kAction3D_Off;mode<=kAction3D_Voxel;++mode) {
    EditorRoomScene_Set3DMode(room,(Action3DMode)mode);
    DioramaView view={.camera={.distance=3.25f},.distance_scale=1,.camera_framing_weight=0,
        .visible_width=496,.viewport={0,0,960,600}};
    EditorRoomCamera camera;assert(EditorRoomScene_ResolveCamera(room,960,300,37,120,64,&view,kDioramaSky_Only,&camera));
    assert(camera.bg_apron_mask==(mode==kAction3D_Voxel?1:0));
    assert(EditorRoomScene_Render(room,camera.x,300,37,120,64,&camera));
    const SrSceneSurfaces *surfaces=EditorRoomScene_Surfaces(room);unsigned guards=0;
    for(unsigned band=0;band<3;++band)for(int y=0;y<surfaces->height;++y)for(int x=0;x<64;++x)
      guards+=(surfaces->bands[0][band][y*surfaces->pitch_pixels+x]>>24)!=0;
    assert((guards!=0)==(mode==kAction3D_Voxel));
    const unsigned scans=EditorRoomScene_RasterCount(room);
    view.camera.tilt_y=.5f;
    assert(EditorRoomScene_ResolveCamera(room,960,300,37,120,64,&view,kDioramaSky_Only,&camera));
    assert(EditorRoomScene_Render(room,camera.x,300,37,120,64,&camera));
    assert(EditorRoomScene_RasterCount(room)==scans);
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

static uint32_t FlatHash(const EditorRoomScene *room) {
  const SrSceneSurfaces *surface=EditorRoomScene_Surfaces(room);
  uint32_t hash=2166136261u;
  for(int y=0;y<224;++y)for(int x=64;x<64+surface->width;++x)
    hash=(hash^surface->backdrop[(size_t)y*surface->pitch_pixels+x])*16777619u;
  return hash;
}
static void CastleAnimation(EditorRoomScene *room,const ActionSceneSnapshot *assets) {
  assert(assets->scene.group==2 && (assets->scene.map==3 || assets->scene.map==5));
  assert(ActionRoomScene_CharacterAnimationPhaseCount(&assets->scene)==4);
  assert(assets->frame.animation_phase<0);
  const unsigned cadence=ActionRoomScene_CharacterAnimationCadence(&assets->scene);
  /* The first 0205 hanging-platform hall uses static chain art. Its animated
   * chain family is in the later shaft at x=1600/1680. */
  const int x=assets->scene.map==3?658:1408,y=assets->scene.map==3?253:128;
  const ActionVoxelGeometry *saved[4]={0};uint64_t revisions[4]={0};
  uint32_t ordinary[4],flat[4];
  for(unsigned phase=0;phase<4;++phase) {
    EditorRoomScene_Set3DMode(room,kAction3D_Diorama);
    assert(EditorRoomScene_Render(room,x,y,phase*cadence,120,64,NULL));
    ordinary[phase]=EditorRoomScene_Hash(room);
    EditorRoomScene_Set3DMode(room,kAction3D_Off);
    assert(EditorRoomScene_RenderFlat(room,x,y,phase*cadence,120,NULL));
    flat[phase]=FlatHash(room);
  }
  EditorRoomScene_Set3DMode(room,kAction3D_Voxel);
  DioramaRoomOverride terrain={0};
  const DioramaRoomOverride *base=DioramaLayerOrder_Find(
      EditorRoomScene_Scene(room)->render->layers,assets->scene.group,assets->scene.map);
  if(base)assert(DioramaLayerOrder_ForTerrain(base,assets->terrain_profile,&terrain));
  ActionRoomScene *source=malloc(sizeof(*source));assert(source);
  unsigned total_old_misroutes=0;
  for(unsigned iteration=0;iteration<8;++iteration) {
    const unsigned phase=iteration%4,clock=iteration*cadence;
    assert(EditorRoomScene_Render(room,x,y,clock,120,64,NULL));
    const ActionVoxelGeometry *actual=EditorRoomScene_Scene(room)->voxel_geometry;
    assert(actual);
    if(iteration<4) {
      for(unsigned prior=0;prior<phase;++prior)assert(actual!=saved[prior]);
      saved[phase]=ActionVoxel_Retain(actual);revisions[phase]=ActionVoxel_Revision(actual);
    } else assert(actual==saved[phase] && ActionVoxel_Revision(actual)==revisions[phase]);
    *source=assets->scene;
    assert(ActionRoomScene_BuildCharacters(&assets->scene,clock,-1,source->characters,sizeof(source->characters)));
    ActionVoxelGeometry *fresh=ActionVoxel_Create();assert(fresh);
    assert(ActionVoxel_Build(fresh,source,&terrain,ActionVoxel_SettingsForRoom(source->group,source->map)));
    assert(actual->width==fresh->width && actual->height==fresh->height &&
        actual->origin_x==fresh->origin_x && actual->origin_y==fresh->origin_y);
    assert(!memcmp(actual->front,fresh->front,(size_t)fresh->width*fresh->height));
    const DioramaCapture *capture=EditorRoomScene_Capture(room);
    const SrSceneSurfaces *surface=EditorRoomScene_Surfaces(room);
    const size_t pixels=(size_t)surface->pitch_pixels*surface->height,bytes=pixels*sizeof(uint32_t);
    uint32_t *split=malloc(bytes*4);assert(split);
    ActionRoomSceneFrameRequest request=assets->frame;
    request.camera_x=x;request.camera_y=y;request.game_frame=clock;
    ActionRoomSceneFrameState frame;
    assert(ActionRoomScene_BuildFrameState(&assets->scene,&request,&frame));
    const int ox=x-120-64,oy=y-capture->authentic_y0+1;
    assert(ActionVoxel_Split(fresh,ox,oy,surface->pitch_pixels,surface->height,
        surface->pitch_pixels,&frame,surface->bands[0][0],surface->pitch_pixels,split,split+pixels));
    assert(ActionVoxel_Split(saved[0],ox,oy,surface->pitch_pixels,surface->height,
        surface->pitch_pixels,&frame,surface->bands[0][0],surface->pitch_pixels,split+pixels*2,split+pixels*3));
    const uint32_t *front=(const uint32_t *)capture->pixels[kDioramaPlane_Bg1Far];
    unsigned old_misroutes=0,new_misroutes=0;
    for(size_t i=0;i<pixels;++i)if((split[pixels+i]>>24) && (split[pixels+i]&0xffffff)) {
      old_misroutes+=split[pixels*3+i]!=split[pixels+i];
      new_misroutes+=front[i]!=split[pixels+i];
    }
    assert(!new_misroutes);
    assert(!memcmp(front,split+pixels,bytes));
    assert(!memcmp(capture->pixels[SR_PPU_OVERLAY_BG1],split,bytes));
    if(iteration<4) {
      printf("castle-phase room=%02x%02x terrain=%u phase=%u stale_mask_colored_misroutes=%u current_mask_colored_misroutes=%u capture_hash=%u\n",
          source->group,source->map,assets->terrain_profile,phase,old_misroutes,new_misroutes,EditorRoomScene_Hash(room));
      total_old_misroutes+=old_misroutes;
    }
    free(split);ActionVoxel_Release(fresh);
    assert(EditorRoomScene_Render(room,x,y,clock+1,120,64,NULL));
    assert(EditorRoomScene_Scene(room)->voxel_geometry==saved[phase]);
  }
  assert(total_old_misroutes>0);
  for(unsigned phase=0;phase<4;++phase) {
    EditorRoomScene_Set3DMode(room,kAction3D_Diorama);
    assert(EditorRoomScene_Render(room,x,y,phase*cadence,120,64,NULL));
    assert(!EditorRoomScene_Scene(room)->voxel_geometry && EditorRoomScene_Hash(room)==ordinary[phase]);
    EditorRoomScene_Set3DMode(room,kAction3D_Off);
    assert(EditorRoomScene_RenderFlat(room,x,y,phase*cadence,120,NULL));
    assert(!EditorRoomScene_Scene(room)->voxel_geometry && FlatHash(room)==flat[phase]);
  }
  EditorRoomScene_Set3DMode(room,kAction3D_Voxel);
  assert(!EditorRoomScene_Configure(room,"[layers:02:03]\nbg1-pixels = invalid\n"
      "[layers:02:05]\nbg1-pixels = invalid\n"));
  assert(EditorRoomScene_Render(room,x,y,2*cadence,120,64,NULL));
  assert(EditorRoomScene_Scene(room)->voxel_geometry==saved[2]);
  /* Applying an actual pixel edit invalidates all four retained variants.
   * The saved old frames remain immutable and safe until their final release. */
  char edit[512];
  snprintf(edit,sizeof(edit),"[layers:%02x:%02x]\nbg1-pixels:us+jp+eu = cell:%d,%d transparent:"
      "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n",
      assets->scene.group,assets->scene.map,x/16,y/16);
  assert(EditorRoomScene_Configure(room,edit));
  for(unsigned phase=0;phase<4;++phase) {
    assert(EditorRoomScene_Render(room,x,y,phase*cadence,120,64,NULL));
    const ActionVoxelGeometry *changed=EditorRoomScene_Scene(room)->voxel_geometry;
    assert(changed && changed!=saved[phase] && ActionVoxel_Revision(changed)!=revisions[phase]);
    assert(ActionVoxel_Revision(saved[phase])==revisions[phase]);
    for(int py=y/16*16;py<y/16*16+16;++py)for(int px=x/16*16;px<x/16*16+16;++px)
      assert(!changed->front[(size_t)(py-changed->origin_y)*changed->width+px-changed->origin_x]);
  }
  DioramaLayerOrder_ClearRoom(&terrain);free(source);
  EditorRoomScene_Destroy(room);
  for(unsigned phase=0;phase<4;++phase) {
    assert(ActionVoxel_Revision(saved[phase])==revisions[phase]);
    assert(atomic_load(&saved[phase]->references)==1);
    ActionVoxel_Release(saved[phase]);
  }
  /* An unreviewed phase window must compile its actual pixels, never index
   * past the bounded cache or reuse a previously known four-phase mask. */
  ActionSceneSnapshot *extended=malloc(sizeof(*extended));assert(extended);
  *extended=*assets;
  extended->scene.video_profile[23]=(extended->scene.video_profile[23]&0xf0u)|8;
  assert(ActionRoomScene_HasCharacterAnimation(&extended->scene));
  room=EditorRoomScene_Create(extended);assert(room);
  assert(EditorRoomScene_Render(room,x,y,7*cadence,120,64,NULL));
  const ActionVoxelGeometry *unknown=ActionVoxel_Retain(EditorRoomScene_Scene(room)->voxel_geometry);
  assert(unknown);
  assert(EditorRoomScene_Render(room,x,y,7*cadence,120,64,NULL));
  const ActionVoxelGeometry *again=EditorRoomScene_Scene(room)->voxel_geometry;
  assert(again && again!=unknown && ActionVoxel_Revision(again)!=ActionVoxel_Revision(unknown));
  source=malloc(sizeof(*source));assert(source);*source=extended->scene;
  assert(ActionRoomScene_BuildCharacters(&extended->scene,7*cadence,-1,source->characters,sizeof(source->characters)));
  ActionVoxelGeometry *fresh=ActionVoxel_Create();assert(fresh);
  assert(ActionVoxel_Build(fresh,source,NULL,ActionVoxel_SettingsForRoom(source->group,source->map)));
  assert(again->width==fresh->width && again->height==fresh->height);
  assert(!memcmp(again->front,fresh->front,(size_t)fresh->width*fresh->height));
  ActionVoxel_Release(fresh);free(source);free(extended);EditorRoomScene_Destroy(room);
  assert(atomic_load(&unknown->references)==1);ActionVoxel_Release(unknown);
}

int main(int argc, char **argv) {
  const bool diorama=argc>3 && !strcmp(argv[argc-1],"--diorama");
  if(diorama)--argc;
  const bool castle_animation=argc>3 && !strcmp(argv[argc-1],"--castle-animation");
  if(castle_animation)--argc;
  const bool benchmark = argc > 3 && !strcmp(argv[argc - 1], "--benchmark");
  if (benchmark) --argc;
  if (argc != 3 && argc != 4) return 2;
  size_t n;
  char *bytes = Read(argv[1], &n);
  static ActionSceneSnapshot assets;
  assert(bytes && ActionSceneSnapshot_Decode((const uint8_t *)bytes, n, &assets)); free(bytes);
  EditorRoomScene *room = EditorRoomScene_Create(&assets); assert(room);
  if(diorama)EditorRoomScene_Set3DMode(room,kAction3D_Diorama);
  char *ini = Read(argv[2], &n); assert(ini && EditorRoomScene_Configure(room, ini)); free(ini);
  if(castle_animation) {CastleAnimation(room,&assets);return 0;}
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
  const char *reuse = getenv("AR_EDITOR_RASTER_CACHE");
  const bool raster_reuse = !reuse || strcmp(reuse, "0") != 0;
  while (scanf("%d %d %u %d %d", &x, &y, &frame, &extra, &vertical) == 5) {
    /* Match the browser oracle's default view, including its room-edge stop.
     * Benchmarks intentionally retain their explicitly requested capture. */
    EditorRoomCamera camera;
    if (!benchmark) {
      const DioramaView view = {.camera = {.distance = 3.25f}, .distance_scale = 1, .camera_framing_weight = 1,
          .visible_width = 360, .visible_height = 224,
          .viewport = ArPresentationLayout_ResolveViewport(960,600,false,false,360,224)};
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
    if (raster_reuse) {
      const unsigned scans = EditorRoomScene_RasterCount(room);
      const uint32_t effects_hash = EditorRoomScene_EffectHash(room);
      assert(EditorRoomScene_Render(room, x, y, frame, extra, vertical, &camera));
      assert(EditorRoomScene_RasterCount(room) == scans);
      assert(EditorRoomScene_Hash(room) == hash);
      assert(EditorRoomScene_EffectHash(room) == effects_hash);
    }
    assert(!EditorRoomScene_Render(room, -1, y, frame, extra, vertical, NULL));
    assert(!EditorRoomScene_Configure(room, "[unterminated"));
    assert(strstr(EditorRoomScene_SceneryError(room),"line 1"));
    assert(EditorRoomScene_Hash(room) == hash);
    printf("%08x %08x\n", hash, EditorRoomScene_EffectHash(room));
  }
  if(assets.scene.group==4&&assets.scene.map>=4&&assets.scene.map<=6) {
    const unsigned id=assets.scene.map;
    assert(EditorRoomScene_Configure(room,""));
    unsigned line;const char defaults[]="[effects]\nversion=1\n";
    assert(EditorRoomScene_ConfigureEffects(room,defaults,strlen(defaults),&line));
    ActionLavaBubbles timing;ActionLavaBubbles_Prepare(&timing,&assets.scene,ActionSurfaceField_Bundled(1),0);
    unsigned events=0;
    for(unsigned tick=0;tick<240;tick+=7) {
      assert(EditorRoomScene_Render(room,id==4?440:id==5?128:240,id==4?410:id==5?64:332,tick,0,0,NULL));
      const ActionSceneEffectFrame *effects=EditorRoomScene_Effects(room);timing.clock=tick;
      assert(!effects->decoration_overflow);
      for(unsigned i=0;i<effects->native_bubble_count;++i) {
        const ActionNativeBubbleEvent *b=&effects->native_bubbles[i];
        assert(b->age==ActionLavaBubbles_Age(&timing,b->x/16,b->y/16));
        assert(b->cadence==timing.cadence);
        bool owner=false;for(unsigned j=0;j<effects->decoration_count;++j)owner|=effects->decorations[j].generation==b->owner;
        assert(owner);++events;
      }
    }
    assert(events>20); /* Real regional mound pixels, not fabricated event fixtures. */
    char erased[192];snprintf(erased,sizeof(erased),"[layers:04:%02x]\nbg1-pixels:us+jp+eu = metatile:02 transparent:FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n",id);
    assert(EditorRoomScene_Configure(room,erased));
    assert(EditorRoomScene_Render(room,id==4?440:id==5?128:240,id==4?410:id==5?64:332,180,0,0,NULL));
    assert(!EditorRoomScene_Effects(room)->native_bubble_count);
  }
  CameraPolicy(room, &assets);
  if (assets.scene.group == 1 && assets.scene.map == 1 && !assets.terrain_profile) {
    /* Legacy virtual bands remain editable in ordinary Diorama. Voxel owns
     * the BG1 material split and authentic sprite-priority relationship. */
    EditorRoomScene_Set3DMode(room,kAction3D_Diorama);
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
    EditorRoomScene_Set3DMode(room,kAction3D_Voxel);
    assert(EditorRoomScene_Render(room, 0, 400, 0, 0, 0, NULL));
    const uint32_t voxel_original=EditorRoomScene_Hash(room);
    snprintf(edits, sizeof(edits), "[layers:01:01]\nbg1-virtual = cells:%d,%d-%d,%d band:0\n", cx, cy, cx, cy);
    assert(EditorRoomScene_Configure(room, edits));
    assert(EditorRoomScene_Render(room, 0, 400, 0, 0, 0, NULL));
    assert(EditorRoomScene_Hash(room)==voxel_original);
    /* A nonstamp pixel edit must not reactivate legacy Far classification in
     * Voxel. Exercise the actual native capture callback and the environment
     * adapter together, including ordinary Diorama's intentional Far route. */
    for (unsigned mode = 0; mode < 2; ++mode) {
      EditorRoomScene_Set3DMode(room, mode ? kAction3D_Voxel : kAction3D_Diorama);
      for (unsigned erased = 0; erased < 2; ++erased) {
        char combined[384];
        snprintf(combined, sizeof(combined), "[layers:01:01]\n"
            "bg1-virtual = cells:%d,%d-%d,%d band:0\n"
            "bg1-pixels = cell:%d,%d %s:"
            "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n",
            cx, cy, cx, cy, cx, cy, erased ? "transparent" : "black");
        assert(EditorRoomScene_Configure(room, combined));
        assert(EditorRoomScene_Render(room, 0, 400, 0, 0, 0, NULL));
        const ActionEnvironmentScene *environment = EditorRoomScene_Environment(room);
        ActionEnvironmentTileEdit applied;
        assert(environment->tile_edit(environment->tile_edit_context, 0,
            (sx - 64) / 8, (400 + sy + 1) / 8, &applied));
        const unsigned native_band = (applied.entry & 0x2000) ? 2 : 1;
        if (applied.band != (mode ? native_band : 0))
          fprintf(stderr, "nonstamp pixel priority cell=%d,%d mode=%u erase=%u word=%04x band=%u expected=%u\n",
              cx, cy, mode, erased, applied.entry, applied.band, mode ? native_band : 0);
        assert(applied.band == (mode ? native_band : 0));
        assert(erased ? applied.transparent[0] == 255 : applied.black[0] == 255);
        for (unsigned band = 0; band < 3; ++band) {
          const unsigned kept = mode ? native_band - 1 : 2;
          assert((surface->bands[0][band][at] != 0) == (!erased && band == kept));
        }
      }
      /* The non-edit fallback used by effect/receiver queries follows the
       * same policy; an unrelated tile edit must not change its band. */
      snprintf(edits, sizeof(edits), "[layers:01:01]\nbg1-virtual = cells:%d,%d-%d,%d band:0\n",
          cx, cy, cx, cy);
      assert(EditorRoomScene_Configure(room, edits));
      assert(EditorRoomScene_Render(room, 0, 400, 0, 0, 0, NULL));
      const ActionEnvironmentScene *environment = EditorRoomScene_Environment(room);
      ActionEnvironmentTileEdit applied;
      assert(environment->tile_edit(environment->tile_edit_context, 0,
          (sx - 64) / 8, (400 + sy + 1) / 8, &applied));
      assert(applied.band == (mode ? ((applied.entry & 0x2000) ? 2 : 1) : 0));
    }
    assert(EditorRoomScene_Configure(room, ""));
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
  if (assets.scene.group == 7 && assets.scene.map == 1 &&
      !(assets.frame.bgsc_override_mask == 3 &&
        assets.frame.bgsc_override[0] == 0x64 && assets.frame.bgsc_override[1] == 0x74)) {
    /* The hub's faces and source padding belong to its far skybox band;
     * only BG1's skull island acquires hardware-priority Voxel geometry.
     * Include a real source stamp so the edit path cannot silently retag it. */
    unsigned metatile=assets.scene.bg[1].map[0];
    const uint8_t *native=assets.scene.bg[1].metatiles+metatile*8;
    char edits[512];
    snprintf(edits,sizeof(edits),"[layers:07:01]\n"
        "bg2-virtual:us+jp+eu = cells:0,0-15,15 band:0\n"
        "bg2-stamp:us+jp+eu = cell:0,0 metatile:%02X words:%04X,%04X,%04X,%04X bands:0,0,0,0\n",
        metatile,((((unsigned)native[0]<<8)|native[1])&0xecffu)|0x0100u,
        ((((unsigned)native[2]<<8)|native[3])&0xecffu)|0x0100u,
        ((((unsigned)native[4]<<8)|native[5])&0xecffu)|0x0100u,
        ((((unsigned)native[6]<<8)|native[7])&0xecffu)|0x0100u);
    assert(EditorRoomScene_Configure(room,edits));
    EditorRoomScene_Set3DMode(room,kAction3D_Diorama);
    assert(EditorRoomScene_Render(room,0,0,37,0,0,NULL));
    const SrSceneSurfaces *surface=EditorRoomScene_Surfaces(room);
    const size_t bytes=(size_t)surface->height*surface->pitch_pixels*sizeof(uint32_t);
    uint32_t *saved[3];unsigned far_pixels=0;
    for(unsigned band=0;band<3;++band) {
      saved[band]=malloc(bytes);assert(saved[band]);
      memcpy(saved[band],surface->bands[1][band],bytes);
    }
    /* Surface storage is low/high/far; authoring band zero routes to far. */
    for(size_t i=0;i<bytes/sizeof(uint32_t);++i)far_pixels+=saved[2][i]>>24!=0;
    assert(far_pixels);
    EditorRoomScene_Set3DMode(room,kAction3D_Voxel);
    assert(EditorRoomScene_Scene(room)->voxel_geometry);
    assert(EditorRoomScene_Render(room,0,0,37,0,0,NULL));
    for(unsigned band=0;band<3;++band) {
      assert(!memcmp(saved[band],surface->bands[1][band],bytes));
      free(saved[band]);
    }
    /* Authored rows beyond a resident page keep the row's edge semantics:
     * a finite island extension is not another periodic island, while the
     * hub's bottom water family still cycles. Exercise the editor's actual
     * capture callback after rendering/reconfiguring the room. */
    const char extension_edits[] = "[layers:07:01]\n"
        "bg1-stamp:us+jp+eu = cell:6,16 metatile:00 words:0123,0123,0123,0123 bands:1,1,1,1\n"
        "bg2-stamp:us+jp+eu = cell:6,16 metatile:00 words:0234,0234,0234,0234 bands:1,1,1,1\n"
        "bg1-pixels:us+jp+eu = cell:6,16 black:FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n"
        "bg2-pixels:us+jp+eu = cell:6,16 black:FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n";
    for (unsigned changed_policy = 0; changed_policy < 2; ++changed_policy) {
      char configured[2048];
      snprintf(configured, sizeof(configured), "%s%s", extension_edits,
          changed_policy ? "bg1-policy = edge:repeat\nbg2-policy = edge:clamp bands:0\n" : "");
      assert(EditorRoomScene_Configure(room, configured));
      EditorRoomScene_Set3DMode(room, kAction3D_Diorama);
      assert(EditorRoomScene_Render(room, 0, 0, 37, 120, 64, NULL));
      const ActionEnvironmentScene *environment = EditorRoomScene_Environment(room);
      for (unsigned bg = 0; bg < 2; ++bg) {
        ActionEnvironmentTileEdit edit;
        assert(environment->tile_edit(environment->tile_edit_context, bg, 12, 32, &edit));
        assert(edit.entry == (bg ? 0x234 : 0x123) && edit.black[0] == 255);
        const bool cycles = bg ? !changed_policy : changed_policy;
        for (int repeat = -1; repeat <= 1; repeat += 2) {
          assert(environment->tile_edit(environment->tile_edit_context, bg,
              12 + repeat * 32, 32, &edit));
          assert((edit.black[0] == 255) == cycles);
        }
      }
    }
  }
  if (assets.scene.group == 7 && assets.scene.map == 1 &&
      assets.frame.bgsc_override_mask == 3 &&
      assets.frame.bgsc_override[0] == 0x64 && assets.frame.bgsc_override[1] == 0x74) {
    /* The completion page owns a separate presentation. A Voxel mode toggle
     * or a scoped edit reload must not introduce the boss hub's skull deck. */
    const char edits[]="[layers:07:01:completion]\nbg1-policy = edge:transparent\n";
    assert(EditorRoomScene_Configure(room,edits));
    EditorRoomScene_Set3DMode(room,kAction3D_Diorama);
    assert(EditorRoomScene_Render(room,0,0,37,120,32,NULL));
    const uint32_t original=EditorRoomScene_Hash(room);
    EditorRoomScene_Set3DMode(room,kAction3D_Voxel);
    assert(!EditorRoomScene_Scene(room)->voxel_geometry);
    assert(EditorRoomScene_Configure(room,edits));
    assert(!EditorRoomScene_Scene(room)->voxel_geometry);
    assert(EditorRoomScene_Render(room,0,0,37,120,32,NULL));
    assert(EditorRoomScene_Hash(room)==original);
  }
  if (getenv("AR_EDITOR_RASTER_STATS"))
    fprintf(stderr, "scanouts=%u\n", EditorRoomScene_RasterCount(room));
  EditorRoomScene_Destroy(room);
  return 0;
}
