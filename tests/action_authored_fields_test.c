#include "action/action_effect_recipes.h"
#include "action/action_effect_receivers.h"
#include "action/action_effect_preview.h"
#include "action/action_environment_exposure.h"
#include "action/action_effect_projection.h"
#include "action/action_effect_source.h"
#include "diorama/diorama.h"
#include "support/test_assert.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static ActionEffectRecipes recipes;
static ActionSceneEffectFrame frame;
static ActionSceneEffectRenderBatch a,b;
static ActionReceiverLighting receiver;
static ActionEffectSourcePrimitive primitives[kActionSourceMaximumPrimitives];
static ActionEffectSourceLightJob light_jobs[kActionSourceMaxLightJobs];
static bool Project(void *context,const ActionEffectInstance *e,float x,float y,ArRenderPointF *p) {
  (void)context;*p=(ArRenderPointF){e->world_x+x,e->world_y+y};return true;
}
static bool Clip(void *context,const ActionEffectInstance *e,ActionEffectLocalRect *r) {
  const ActionEffectLocalRect *visible=context;
  *r=(ActionEffectLocalRect){fmaxf(e->geometry.data.rect.x0,visible->x0-e->world_x),fmaxf(e->geometry.data.rect.y0,visible->y0-e->world_y),
    fminf(e->geometry.data.rect.x1,visible->x1-e->world_x),fminf(e->geometry.data.rect.y1,visible->y1-e->world_y)};
  return r->x1>r->x0&&r->y1>r->y0;
}
static void Parse(const char *text) {unsigned line;assert(ActionEffectRecipes_Parse(&recipes,text,strlen(text),&line));ActionEffectRecipes_Apply(&recipes,1,2,0,37,NULL,&frame);}
static void Valid(const ActionSceneEffectRenderBatch *batch) {
  for(int i=0;i<batch->vertex_count;++i){const ArRenderVertex2D *v=&batch->vertices[i];assert(isfinite(v->position.x)&&isfinite(v->position.y)&&isfinite(v->color.a)&&v->color.a>=0&&v->color.a<=1);}
  for(int i=0;i<batch->index_count;++i)assert(batch->indices[i]>=0&&batch->indices[i]<batch->vertex_count);
}
static void Geometry(void) {
  const char *names[]={"particle-area","light-fan","water-surface","drips","waterfall-spray","cloud-bank","exposure","wet-contour","halo","light-gradient"};
  char text[512];ActionEffectLocalRect clip={-240,-100,240,350};
  for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i) {
    snprintf(text,sizeof(text),"[effects]\nversion=1\n[emitter:01:02:0:%s:123]\nx=0\ny=100\nwidth=256\nheight=128\n",names[i]);Parse(text);
    const unsigned layer=frame.authored[0].render_layer;
    assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,Clip,&clip,&a));assert(a.vertex_count&&a.index_count);Valid(&a);
    assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,Clip,&clip,&b));
    assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(a.vertices[0])));
    assert(ActionSceneDecorationRender_Build(&frame,layer,false,false,Project,Clip,&clip,&b));assert(!b.vertex_count);
    frame.authored[0].pulse_ticks=111;assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,Clip,&clip,&b));Valid(&b);
    frame.authored[0].pulse_ticks=37;assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,Clip,&clip,&b));
    assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(a.vertices[0])));
  }
  Parse("[effects]\nversion=1\n[emitter:01:02:0:particle-area:123]\nx=8000\ny=8000\nwidth=16000\nheight=16000\nparticles=4\n");
  frame.authored[0].world_x=frame.authored[0].world_y=8000;
  clip=(ActionEffectLocalRect){7000,7000,8000,7352};
  assert(ActionSceneDecorationRender_Build(&frame,frame.authored[0].render_layer,true,true,Project,Clip,&clip,&a));assert(a.vertex_count<=4096);
  assert(!ActionSceneDecorationRender_Build(&frame,frame.authored[0].render_layer,true,true,Project,NULL,NULL,&b));
  /* Shared-cell positions survive viewport translation. */
  const ArRenderVertex2D saved=a.vertices[0];clip.x0+=16;clip.x1+=16;
  assert(ActionSceneDecorationRender_Build(&frame,frame.authored[0].render_layer,true,true,Project,Clip,&clip,&b));
  bool found=false;for(int i=0;i<b.vertex_count;++i)if(!memcmp(&saved,&b.vertices[i],sizeof(saved)))found=true;assert(found);
}
/* CPU-only recipe tests cannot detect direct project() calls or whole-room
 * traversal that silently latches the game onto reference projection. */
static unsigned SourceBuild(unsigned layer, bool lighting, bool particles,
    const ActionEffectProjectionContext *context) {
  ActionEffectSourceBatch source={.context=*context,.primitives=primitives,
      .capacity=kActionSourceMaximumPrimitives,.lights=light_jobs,
      .light_capacity=kActionSourceMaxLightJobs};
  assert(ActionSceneDecorationRender_Build(&frame,layer,lighting,particles,
      ActionEffectSource_ProjectPoint,ActionEffectSource_ClipBounds,&source,&b));
  assert(!source.failed);
  return source.count;
}
static void DeferredAuthored(void) {
  static const struct {const char *kind,*extra;} cases[]={
    {"soft-light",""},{"motes",""},{"free-mist",""},{"ground-mist","mist-height=26\n"},
    {"particle-area","particles=8\npattern=snow\n"},{"light-fan",""},{"water-surface",""},
    {"drips",""},{"waterfall-spray",""},{"cloud-bank",""},
    {"exposure","intensity=.5\ndim-scenery=1\n"},{"wet-contour","points=0,0 40,10 80,0\n"},
    {"halo",""},{"light-gradient",""},{"flame",""},{"torch",""}};
  ActionEffectProjectionContext context={.visible_width=512,.snes_height=448,
      .capture_height=448,.viewport={0,0,1024,896}};
  bool covered[kActionEffect_KindCount]={0};
  for(unsigned k=0;k<sizeof(cases)/sizeof(cases[0]);++k) {
    frame=(ActionSceneEffectFrame){0};
    char text[512];snprintf(text,sizeof(text),
        "[effects]\nversion=1\n[emitter:01:02:0:%s:123]\nx=128\ny=160\nwidth=192\nheight=128\n%s",
        cases[k].kind,cases[k].extra);Parse(text);
    assert(frame.authored_count==1);
    covered[frame.authored[0].kind]=true;
    frame.authored_floor[0]=(ActionEffectFloorField){.count=2,.spans={{64,128,176,26},{128,192,192,26}}};
    frame.authored[0].flags|=kActionEffectFlag_Visible;
    unsigned total=0,cpu_total=0;
    for(unsigned layer=0;layer<kActionEffectRenderLayer_Count;++layer)for(unsigned pass=0;pass<4;++pass) {
      const bool lighting=(pass&1)!=0,particles=(pass&2)!=0;
      assert(ActionSceneDecorationRender_Build(&frame,layer,lighting,particles,
          ActionEffectProjection_ProjectPoint,ActionEffectProjection_ClipBounds,&context,&a));
      const unsigned count=SourceBuild(layer,lighting,particles,&context);
      if(a.index_count)assert(count);
      if(!pass)assert(!count);
      total+=count;
      cpu_total+=(unsigned)a.index_count;
    }
    if(!total)fprintf(stderr,"No deferred geometry for authored family %s\n",cases[k].kind);
    assert(total && cpu_total);
  }
  for(unsigned kind=0;kind<kActionEffect_KindCount;++kind) {
    if(ActionEffectRecipes_IsEmitter(kind) && !covered[kind]) {
      fprintf(stderr,"Missing deferred emitter fixture: %s\n",ActionEffectRecipes_KindName(kind));
      assert(covered[kind]);
    }
  }
}
static void DeferredParticleArea(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:particle-area:123]\nx=8000\ny=8000\nwidth=16000\nheight=16000\nparticles=8\npattern=snow\n");
  ActionEffectProjectionContext context={.bg1_camera_x=7000,.bg1_camera_y=7000,
      .visible_width=1000,.snes_height=352,.capture_height=352,.viewport={0,0,1000,352}};
  const unsigned layer=frame.authored[0].render_layer;
  const unsigned count=SourceBuild(layer,false,true,&context);
  assert(count && count<512); /* Work follows the view, not the saved region. */
  const ActionEffectSourcePrimitive saved=primitives[count/2];
  context.bg1_camera_x+=16;
  const unsigned moved=SourceBuild(layer,false,true,&context);
  bool found=false;
  for(unsigned i=0;i<moved;++i)if(!memcmp(saved.points,primitives[i].points,sizeof(saved.points)) &&
      !memcmp(saved.colors,primitives[i].colors,sizeof(saved.colors)))found=true;
  assert(found); /* World-cell seeds survive panning. */
  context.bg1_camera_x=-4000;assert(!SourceBuild(layer,false,true,&context));

  /* BG2 scrolls independently. A retained packet must cover every skybox
   * band, including static anchors stretched over multiple bands. */
  DioramaProjection sky={.valid=true,.bg2_skybox={.count=2,.active_band=0,
      .bands={{.x0=0,.x1=512,.y0=0,.y1=224,.output_y0=0,.output_y1=.5f},
              {.x0=0,.x1=512,.y0=224,.y1=448,.output_y0=.5f,.output_y1=1}}}};
  context.diorama_projection=&sky;context.bg2_camera_x=context.bg2_camera_y=7000;
  frame.authored[0].projection_plane=kActionEffectProjectionPlane_Bg2;
  ActionEffectSourceBatch source={.context=context};
  ActionEffectLocalRect first,second;
  assert(ActionEffectSource_ClipBounds(&source,&frame.authored[0],&first));
  sky.bg2_skybox.active_band=1;
  assert(ActionEffectSource_ClipBounds(&source,&frame.authored[0],&second));
  assert(!memcmp(&first,&second,sizeof(first)));
  assert(first.y1>=7000+448-frame.authored[0].world_y);
  frame.authored[0].flags|=kActionEffectFlag_StaticAnchor;
  assert(SourceBuild(layer,false,true,&context));
}
static void HaloAndGradient(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:halo:123]\nx=100\ny=100\nwidth=160\nheight=160\ncolor=ffffff\ncolor-end=445588\nsoftness=.5\nlight-player=1\n");
  assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));float multiply[3],add[3];
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,100,100,multiply,add);assert(add[0]==0); /* open centre */
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,166,100,multiply,add);assert(add[0]>.05f);
  Parse("[effects]\nversion=1\n[emitter:01:02:0:light-gradient:123]\nx=100\ny=100\nwidth=1024\nheight=1024\ncolor=ff0000\ncolor-end=0000ff\nsoftness=0\nangle=0\nlight-player=1\n");
  assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,100,0,multiply,add);assert(add[0]>add[2]);
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,100,200,multiply,add);assert(add[0]<add[2]);
  const ActionEffectLocalRect clip={0,0,256,224};
  assert(ActionSceneDecorationRender_Build(&frame,ActionEffectReceivers_Layer(&frame.authored[0]),true,false,Project,Clip,(void*)&clip,&a));
  assert(a.vertex_count>0&&a.vertex_count<=224);Valid(&a);
  for(int i=0;i<a.vertex_count;++i){assert(a.vertices[i].position.x>=-.001f&&a.vertices[i].position.x<=256.001f);assert(a.vertices[i].position.y>=-.001f&&a.vertices[i].position.y<=224.001f);}
}
static void Receivers(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:soft-light:123]\nx=100\ny=100\nwidth=96\nheight=64\ncolor=ffffff\nlight-scenery=0\nlight-player=1\nlight-enemies=0\n[emitter:01:02:0:exposure:234]\nx=100\ny=100\nwidth=512\nheight=512\nintensity=.5\ndim-scenery=0\ndim-player=0\ndim-enemies=1\n");
  assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));float multiply[3],add[3];
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,100,100,multiply,add);
  assert(multiply[0]==1 && fabsf(add[0]-.2f)<.001f); /* Centre belongs to ONE glow triangle. */
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Enemies,100,100,multiply,add);assert(multiply[0]==.5f&&add[0]==0);
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Enemies,1000,1000,multiply,add);assert(multiply[0]==1&&add[0]==0);
  assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,false,&receiver));
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,100,100,multiply,add);assert(add[0]==0);
  ActionEffectInstance e={.kind=kActionEffect_CaveAmbientLight,.render_layer=kActionEffectRenderLayer_ForegroundLight};
  assert(ActionEffectReceivers_Layer(&e)==kActionEffectRenderLayer_ForegroundLight);
  e.tuning.light_receivers_set=1;assert(ActionEffectReceivers_Layer(&e)==kActionEffectRenderLayer_Bg1Light);
  frame.decoration_count=255;assert(!ActionEnvironment_Bg1Dimming(&frame,1,2));
}
static void Events(void) {
  ActionEffectPreviewEvent event={.start=65530,.duration=96,.x=128,.y=128,.velocity_x=-1,.velocity_y=1,.seed=123};
  ActionEffectInstance e,repeat;
  for(unsigned i=0;i<ActionEffectPreview_Count();++i) {
    event.kind=ActionEffectPreview_Kind(i);assert(ActionEffectPreview_Build(&event,3,&e));
    frame=(ActionSceneEffectFrame){0};
    if(e.render_layer==kActionEffectRenderLayer_WorldDust)frame.decorations[frame.decoration_count++]=e;
    else frame.effects[frame.effect_count++]=e;
    if(frame.effect_count)assert(ActionSceneEffectRender_Build(&frame,true,true,Project,NULL,&a));
    else assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,true,true,Project,NULL,NULL,&a));
    assert(a.vertex_count&&a.index_count);Valid(&a);
    assert(ActionEffectPreview_Build(&event,3,&repeat));assert(!memcmp(&e,&repeat,sizeof(e)));
    assert(!ActionEffectPreview_Build(&event,65529,&repeat));assert(!ActionEffectPreview_Build(&event,200,&repeat));
  }
  event.kind=255;assert(!ActionEffectPreview_Build(&event,3,&e));
}
static void MovingLightReceivers(void) {
  ActionEffectPreviewEvent event={.kind=kActionEffect_EnemyFireball,.duration=96,.x=100,.y=100,.seed=123};
  frame=(ActionSceneEffectFrame){.effect_count=1,.visible_count=1};
  assert(ActionEffectPreview_Build(&event,0,&frame.effects[0]));
  Parse("[effects]\nversion=1\n[source:01:02:0:enemy-fireball:0]\nlight-scenery=1\nlight-player=1\nlight-enemies=0\n");
  assert(ActionSceneEffectRender_Build(&frame,true,false,Project,NULL,&a));assert(!a.index_count);
  assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_Bg1Plane,true,false,Project,NULL,NULL,&a));assert(a.index_count);
  assert(ActionEffectProjection_RequiredBgPlaneMask(NULL,&frame)&1u);
  assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));float multiply[3],add[3];
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,100,100,multiply,add);assert(add[0]>0);
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Enemies,100,100,multiply,add);assert(add[0]==0);
  frame.effects[0].flags|=kActionEffectFlag_LightingOff;
  assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_Bg1Plane,true,true,Project,NULL,NULL,&a));assert(!a.index_count);
  assert(ActionSceneEffectRender_Build(&frame,true,true,Project,NULL,&a));assert(a.index_count); /* Trails survive. */
}
static void NativeMembers(void) {
  unsigned line;
  const char *text = "[effects]\nversion=1\n"
                     "[source:01:01:0:forest-forward:1]\ncolor=ccddff\n"
                     "[member:01:01:0:forest-forward:1]\noffset-x=32\noffset-y=8\nwidth-scale=1."
                     "5\nlength-scale=.8\nangle=-12\ncolor=ff8040\n"
                     "[member:01:01:0:forest-forward:c]\nenabled=0\n";
  assert(ActionEffectRecipes_Parse(&recipes, text, strlen(text), &line));
  const ActionEffectRecipes saved = recipes;
  const char *bad[] = {"[member:01:01:0:forest-forward:0]\n",
                       "[member:01:01:0:forest-forward:d]\n",
                       "[member:01:02:0:forest-forward:1]\n",
                       "[member:01:02:0:cave-drips:1]\nwidth-scale=2\n",
                       "[member:01:02:0:cave-water:1]\nlength-scale=2\n",
                       "[member:02:03:0:castle-light:9]\nangle=15\n",
                       "[member:01:01:0:forest-forward:1]\nangle=31\n",
                       "[member:01:01:0:forest-forward:1]\noffset-x=513\n",
                       "[member:01:01:0:forest-forward:1]\nlight-player=1\n"};
  char invalid[512];
  for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    snprintf(invalid, sizeof(invalid), "[effects]\nversion=1\n%s", bad[i]);
    assert(!ActionEffectRecipes_Parse(&recipes, invalid, strlen(invalid), &line));
    assert(!memcmp(&recipes, &saved, sizeof(recipes)));
  }
  frame = (ActionSceneEffectFrame){.decoration_count = 1};
  frame.decorations[0] = (ActionEffectInstance){
      .kind = kActionEffect_ForestForwardLight,
      .generation = 1,
      .phase = kActionEffectPhase_ForestCanopyLight,
      .projection_plane = kActionEffectProjectionPlane_BetweenBackgrounds,
      .flags = kActionEffectFlag_Visible,
      .render_layer = kActionEffectRenderLayer_ForegroundLight,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {0, 0, 768, 544}}};
  assert(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_ForegroundLight, true,
                                           false, Project, NULL, NULL, &a));
  assert(a.vertex_count);
  ActionEffectRecipes_Apply(&recipes, 1, 1, 0, 37, NULL, &frame);
  assert(frame.members.count == 2 && frame.members.records[0].offset_x == 32 &&
         frame.members.records[0].angle == -12);
  assert(frame.effect_count == 0 && frame.authored_count == 0 && frame.decoration_count == 1);
  assert(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_ForegroundLight, true,
                                           false, Project, NULL, NULL, &b));
  Valid(&b);
  assert(a.vertex_count != b.vertex_count ||
         memcmp(a.vertices, b.vertices, a.vertex_count * sizeof(a.vertices[0])));
  assert(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_ForegroundLight, true,
                                           false, Project, NULL, NULL, &a));
  assert(a.vertex_count == b.vertex_count &&
         !memcmp(a.vertices, b.vertices, a.vertex_count * sizeof(a.vertices[0])));
  /* A recipe for another terrain cannot change this frame's member table. */
  ActionEffectRecipes_Apply(&recipes, 1, 1, 1, 37, NULL, &frame);
  assert(!frame.members.count);
  /* All catalogue members may overlap without enlarging any pool. */
  char stress[8192] = "[effects]\nversion=1\n";
  for (unsigned i = 1; i <= 12; ++i) {
    char record[256];
    snprintf(record, sizeof(record),
             "[member:01:01:0:forest-forward:%x]\noffset-x=-512\nwidth-scale=2\nlength-scale="
             "2\nangle=30\nintensity=4\n",
             i);
    strcat(stress, record);
  }
  assert(ActionEffectRecipes_Parse(&recipes, stress, strlen(stress), &line));
  ActionEffectRecipes_Apply(&recipes, 1, 1, 0, 37, NULL, &frame);
  for (int x = 0; x < 3600; x += 80) {
    frame.decorations[0].world_x = x;
    assert(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_ForegroundLight, true,
                                             false, Project, NULL, NULL, &b));
    Valid(&b);
    for (int i = 0; i < b.vertex_count; ++i) {
      const ArRenderColorF color = b.vertices[i].color;
      assert(color.r >= 0 && color.r <= 1 && color.g >= 0 && color.g <= 1 && color.b >= 0 &&
             color.b <= 1);
    }
  }
}
static ActionEnvironmentTileEdit test_tile;
static bool EditedTile(void *context, unsigned bg, int x, int y, ActionEnvironmentTileEdit *out) {
  (void)context;
  (void)bg;
  *out = test_tile;
  if (x != 0 || y != 0) out->blank = 1;
  return true;
}
static void EditedOcclusion(void) {
  static uint16_t vram[32768];
  vram[16] = 0x0080;
  vram[23] = 0x0001;
  ActionEnvironmentScene scene = {.vram = vram, .tile_edit = EditedTile};
  uint8_t bits;
  test_tile = (ActionEnvironmentTileEdit){.entry = 1, .replace = 1, .band = 1};
  assert(ActionEnvironmentScene_OpacityRow(&scene, 0, 0, 0, &bits) && bits == 1);
  test_tile.entry = 0x4001;
  assert(ActionEnvironmentScene_OpacityRow(&scene, 0, 0, 0, &bits) && bits == 128);
  test_tile.entry = 0x8001;
  assert(ActionEnvironmentScene_OpacityRow(&scene, 0, 0, 0, &bits) && bits == 128);
  test_tile.black[0] = 0x40;
  test_tile.transparent[0] = 1;
  assert(ActionEnvironmentScene_OpacityRow(&scene, 0, 0, 0, &bits) && bits == 2);
  test_tile.band = 0;
  assert(ActionEnvironmentScene_OpacityRow(&scene, 0, 0, 0, &bits) && !bits);
  test_tile.band = 1;
  test_tile.blank = 1;
  assert(ActionEnvironmentScene_OpacityRow(&scene, 0, 0, 0, &bits) && !bits);
  test_tile = (ActionEnvironmentTileEdit){.entry = 1, .replace = 1, .band = 1};
  memset(vram + 16, 255, 32); /* Full tile, independent of flips. */
  assert(ActionEnvironmentScene_CaptureScenery(&scene, &frame.scenery));
  assert(frame.scenery.valid && frame.scenery.count == 1);
  assert(frame.scenery.rectangles[0].x0 == 0 && frame.scenery.rectangles[0].x1 == 8 &&
         frame.scenery.rectangles[0].y0 == 0 && frame.scenery.rectangles[0].y1 == 8);
  memset(test_tile.transparent, 255, 8);
  assert(ActionEnvironmentScene_CaptureScenery(&scene, &frame.scenery));
  assert(!frame.scenery.count);
  assert(!ActionEnvironmentScene_OpacityRow(&scene, 0, 1, 0, &bits));
  static ActionSceneryCaptureCache cache;
  static ActionMoonlightOcclusion cached, uncached;
  for (unsigned step = 0; step < 48; ++step) {
    test_tile.band = step % 3;
    test_tile.entry = (uint16_t)(1 | ((step & 3) << 14));
    test_tile.blank = step % 7 == 0;
    test_tile.transparent[step % 8] ^= (uint8_t)(1u << (step % 8));
    test_tile.black[(step+1) % 8] ^= 0x41;
    vram[16 + step % 16] ^= 0x1248; /* Animated opacity at the same VRAM address. */
    scene.camera_x[0] = (int)step * 9 - 128;
    scene.camera_y[0] = (int)step - 16;
    scene.scenery_cache = NULL;
    assert(ActionEnvironmentScene_CaptureScenery(&scene, &uncached));
    scene.scenery_cache = &cache;
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
      assert(ActionEnvironmentScene_CaptureScenery(&scene, &cached));
      assert(cached.valid == uncached.valid && cached.count == uncached.count);
      assert(!memcmp(cached.rectangles, uncached.rectangles,
                     cached.count * sizeof(cached.rectangles[0])));
    }
  }

  /* Directional geometry changes with the edited caster, while collision does not. */
  const char *fan = "[effects]\nversion=1\n[emitter:01:02:0:light-fan:1]\nx=0\ny=0\nwidth="
                    "16\nheight=100\nstrands=8\n";
  frame = (ActionSceneEffectFrame){0};
  Parse(fan);
  const unsigned layer = frame.authored[0].render_layer;
  assert(ActionSceneDecorationRender_Build(&frame, layer, true, false, Project, NULL, NULL, &a));
  frame.scenery =
      (ActionMoonlightOcclusion){.valid = 1, .count = 1, .rectangles = {{-64, 10, 64, 72}}};
  assert(ActionSceneDecorationRender_Build(&frame, layer, true, false, Project, NULL, NULL, &b));
  assert(a.vertex_count == b.vertex_count && a.index_count == b.index_count);
  bool darker = false;
  for (int i = 0; i < a.vertex_count; ++i) {
    assert(a.vertices[i].position.x == b.vertices[i].position.x &&
           a.vertices[i].position.y == b.vertices[i].position.y);
    if (a.vertices[i].color.a > b.vertices[i].color.a) darker = true;
  }
  assert(darker);
  Valid(&b);
  frame.scenery.count = 0;
  assert(ActionSceneDecorationRender_Build(&frame, layer, true, false, Project, NULL, NULL, &b));
  assert(!memcmp(a.vertices, b.vertices, a.vertex_count * sizeof(a.vertices[0])));
}

static void BenchmarkReceivers(void) {
  char text[2048]="[effects]\nversion=1\n";
  for(unsigned i=0;i<3;++i) {
    char record[384];snprintf(record,sizeof(record),"[emitter:01:02:0:light-fan:%x]\nx=%u\ny=0\nwidth=160\nheight=512\nstrands=8\nlight-scenery=1\nlight-player=1\nlight-enemies=1\n",i+1,i*128);
    strcat(text,record);
  }
  strcat(text,"[emitter:01:02:0:particle-area:4]\nx=1024\ny=640\nwidth=2048\nheight=1280\nparticles=4\n");
  frame=(ActionSceneEffectFrame){0};Parse(text);
  enum {iterations=1000};const clock_t start=clock();
  float multiply[3],add[3];
  for(unsigned i=0;i<iterations;++i) {
    assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));
    for(unsigned object=0;object<16;++object)
      ActionEffectReceivers_Sample(&receiver,object&1?kActionReceiver_Player:kActionReceiver_Enemies,object*24,160,multiply,add);
  }
  const double ms=(double)(clock()-start)*1000/CLOCKS_PER_SEC/iterations;
  printf("Receiver CPU stress: 3 eight-strand fans, 16 object samples, %d + %d light vertices, %.3f ms/frame (%u iterations; no GPU).\n",
      receiver.player.vertex_count,receiver.enemies.vertex_count,ms,iterations);
}
static void LayerAnchors(void) {
  frame=(ActionSceneEffectFrame){0};
  Parse("[effects]\nversion=1\n[emitter:01:02:0:light-fan:123]\nx=112\ny=62\nwidth=128\nheight=256\nanchor=bg2-point\nplacement=background\n");
  ActionEffectInstance *e=&frame.authored[0];
  assert(ActionEffectProjection_RequiredBgPlaneMask(NULL,&frame)==(1u<<SR_PPU_OVERLAY_BG2));
  for(unsigned size=0;size<2;++size)for(unsigned extra=32;extra<=64;extra+=32)
    for(unsigned mode=0;mode<4;++mode) {
      DioramaProjection p={.valid=true,.output_width=size?1280:800,.output_height=size?720:600,
        .matrix={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},.aspect_x=2,.height_scale=1,
        .texture_width=512,.texture_height=352,
        .bg2_plane={.valid=(mode&1)!=0,.u1=1,.v1=1}};
      if(mode>=2)p.bg2_skybox=(DioramaSkyboxProjection){.count=2,.active_band=-1,
        .bands={{0,0,512,192,0,192.f/352},{24,192,488,352,192.f/352,1}}};
      ActionEffectProjectionContext c={.ws_extra=128,.ws_extra_top=extra,
        .viewport={0,0,p.output_width,p.output_height},.visible_width=512,.snes_height=224,
        .diorama_projection=mode?&p:NULL};
      ArRenderPointF first,second;
      assert(ActionEffectProjection_ProjectPoint(&c,e,0,74,&first));
      assert(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
          ActionEffectProjection_ProjectPoint,ActionEffectProjection_ClipBounds,&c,&a));
      assert(a.vertex_count&&a.index_count);Valid(&a);
      c.bg1_camera_x=900;c.bg1_camera_y=200;
      assert(ActionEffectProjection_ProjectPoint(&c,e,0,74,&second));
      assert(first.x==second.x&&first.y==second.y);
      assert(ActionSceneDecorationRender_Build(&frame,e->render_layer,true,true,
          ActionEffectProjection_ProjectPoint,ActionEffectProjection_ClipBounds,&c,&b));
      assert(a.vertex_count==b.vertex_count&&a.index_count==b.index_count);
      assert(!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(a.vertices[0])));
      assert(!memcmp(a.indices,b.indices,a.index_count*sizeof(a.indices[0])));
      c.bg2_camera_x=16;c.bg2_camera_y=8;
      assert(ActionEffectProjection_ProjectPoint(&c,e,0,74,&second));
      assert(first.x!=second.x&&first.y!=second.y);
      if(mode>=2) {
        /* Any authored kind uses one transform on both sides of a raster
         * seam. Surface-raster mode deliberately follows the shifted bands. */
        e->flags|=kActionEffectFlag_ClippedMesh;
        p.bg2_skybox.active_band=0;
        assert(ActionEffectProjection_ProjectPoint(&c,e,17,130,&first));
        p.bg2_skybox.active_band=1;
        assert(ActionEffectProjection_ProjectPoint(&c,e,17,130,&second));
        assert(first.x==second.x&&first.y==second.y);
        e->flags&=(uint8_t)~kActionEffectFlag_StaticAnchor;
        assert(ActionEffectProjection_ProjectPoint(&c,e,17,130,&second));
        assert(fabsf(first.x-second.x)>.1f);
        e->flags=kActionEffectFlag_Visible|kActionEffectFlag_StaticAnchor;
      }
    }
  /* A source attached to BG2 can be inserted after BG1 surfaces. Both
   * callbacks must be requested even though its camera remains BG2. */
  e->render_layer=kActionEffectRenderLayer_Bg1Plane;
  assert(ActionEffectProjection_RequiredBgPlaneMask(NULL,&frame)==3);
}
int main(int argc, char **argv) {
  DeferredAuthored();DeferredParticleArea();
  LayerAnchors();
  Geometry();HaloAndGradient();
  Receivers();
  Events();
  MovingLightReceivers();
  NativeMembers();
  EditedOcclusion();
  puts("Authored fields: 16 CPU/source families, clipping, bounded areas, viewport-stable particles, reverse seeking, "
       "receiver separation, moving lights, glow centre and 19 preview families passed.");
  if (argc == 2 && !strcmp(argv[1], "--benchmark")) BenchmarkReceivers();
  return 0;
}
