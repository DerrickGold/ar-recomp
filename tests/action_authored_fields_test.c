#include "action/action_effect_recipes.h"
#include "action/action_heat.h"
#include "action/action_light_kinds.h"
#include "action/action_effect_receivers.h"
#include "action/action_effect_preview.h"
#include "action/action_environment_exposure.h"
#include "action/action_exposure.h"
#include "render/scenery_dimming.h"
#include "action/action_effect_projection.h"
#include "action/action_effect_source.h"
#include "action/action_effect_render_internal.h"
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
static void HeatAndParticlePaths(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:heat-haze:1]\nx=256\ny=128\nwidth=512\nheight=256\namplitude=1.5\n");
  const DioramaHeat heat=ActionHeat_Prepare(&frame,0,0,512,256);
  assert(DioramaHeat_Active(&heat));
  const ArRenderPointF centre={.5f,.5f},edge={0,.5f};
  const ArRenderPointF moved=DioramaHeat_Uv(&heat,centre),still=DioramaHeat_Uv(&heat,edge);
  assert(fabsf(moved.x-centre.x)>.00001f&&fabsf(moved.x-centre.x)<.01f);
  assert(still.x==edge.x&&still.y==edge.y);
  DioramaHeat overlap=heat;memcpy(overlap.bounds[1],heat.bounds[0],sizeof(heat.bounds[0]));memcpy(overlap.motion[1],heat.motion[0],sizeof(heat.motion[0]));
  const ArRenderPointF combined=DioramaHeat_Uv(&overlap,centre);
  assert(combined.x==moved.x&&combined.y==moved.y);
  const DioramaHeat disabled=ActionHeat_Prepare(NULL,0,0,512,256);assert(!DioramaHeat_Active(&disabled));
  unsigned line;const char *invalid="[effects]\nversion=1\n[emitter:01:02:0:heat-haze:1]\nanchor=bg1\n";
  assert(!ActionEffectRecipes_Parse(&recipes,invalid,strlen(invalid),&line));
  invalid="[effects]\nversion=1\n[emitter:01:02:0:particle-path:1]\npoints=0,0 0,0\n";
  assert(!ActionEffectRecipes_Parse(&recipes,invalid,strlen(invalid),&line));
  Parse("[effects]\nversion=1\n[emitter:01:02:0:particle-path:1]\nx=128\ny=128\nwidth=128\nheight=128\npoints=48,-48 0,0 -48,48\nparticles=32\nactivity=.25\nwander=0\nsize-min=.4\nsize-max=.4\n");
  for(unsigned ticks=0;ticks<1000;ticks+=29) {
    frame.authored[0].pulse_ticks=ticks;
    assert(ActionSceneDecorationRender_Build(&frame,frame.authored[0].render_layer,false,true,Project,NULL,NULL,&a));Valid(&a);
    assert(a.vertex_count<=128);
    for(int i=0;i<a.vertex_count;i+=4) {
      float x=0,y=0;for(int j=0;j<4;++j){x+=a.vertices[i+j].position.x*.25f;y+=a.vertices[i+j].position.y*.25f;}
      assert(fabsf(x+y-256)<.001f); /* right-to-left routes preserve their order */
    }
  }
}
static void GroundDust(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:cloud-bank:123]\nx=128\ny=160\n"
        "width=192\nheight=64\npoints=-96,24 -48,-8 0,0 48,-24 96,16\n"
        "amplitude=17\ndrift=110\nlifetime=320\nstrands=6\n");
  const unsigned layer=frame.authored[0].render_layer;
  const ActionEffectFieldStyle *s=&frame.authored[0].field_style;
  for(unsigned tick=0;tick<800;tick+=19) {
    frame.authored[0].pulse_ticks=tick;
    assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&a));
    Valid(&a);assert(a.vertex_count && a.vertex_count<=kActionContourMaxPoints*4);
    for(int i=0;i<a.vertex_count;++i) {
      const float x=a.vertices[i].position.x-128,y=a.vertices[i].position.y-160;
      unsigned col=1;while(col+1<s->point_count && x>s->points[col].x)++col;
      const float t=(x-s->points[col-1].x)/(s->points[col].x-s->points[col-1].x);
      const float floor=s->points[col-1].y+(s->points[col].y-s->points[col-1].y)*t;
      assert(y<=floor+.001f); /* No puff hangs below or cuts across its dune. */
    }
  }
  frame.authored[0].pulse_ticks=37;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&a));
  frame.authored[0].pulse_ticks=411;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(a.vertices[0])));
  frame.authored[0].pulse_ticks=37;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(a.vertices[0])));
  Parse("[effects]\nversion=1\n[emitter:01:02:0:cloud-bank:123]\npoints=-32,0 32,0\n");
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&a));
  unsigned visible=0;
  for(int i=0;i<a.vertex_count;++i)if(a.vertices[i].color.a>0)++visible;
  assert(visible); /* A straight two-point trace must not fade out entirely. */

  Parse("[effects]\nversion=1\n[emitter:01:02:0:cloud-bank:123]\nwidth=192\nheight=96\n"
        "points=-96,40 0,-24 96,40\namplitude=26\ndrift=110\ncarry=156\n");
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&a));
  frame.authored[0].field_style.carry=0;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count);
  bool carried=false;
  for(int i=0;i<a.vertex_count;++i) {
    assert(a.vertices[i].position.y<=b.vertices[i].position.y+.001f);
    if(a.vertices[i].position.x<0)assert(a.vertices[i].position.y==b.vertices[i].position.y);
    if(a.vertices[i].position.x>0&&a.vertices[i].position.y<b.vertices[i].position.y-10)carried=true;
  }
  assert(carried); /* Dust crosses the crest instead of falling down its lee. */
  frame.authored[0].field_style.carry=156;
  frame.authored[0].field_style.drift=-110;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&a));
  frame.authored[0].field_style.carry=0;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  carried=false;
  for(int i=0;i<a.vertex_count;++i) {
    if(a.vertices[i].position.x>0)assert(a.vertices[i].position.y==b.vertices[i].position.y);
    if(a.vertices[i].position.x<0&&a.vertices[i].position.y<b.vertices[i].position.y-10)carried=true;
  }
  assert(carried); /* The same rule supports wind blowing in either direction. */
  frame.authored[0].particle_count=40;
  frame.authored[0].particle_style.size_min=.28f;
  frame.authored[0].particle_style.size_max=.85f;
  unsigned max_grains=0;
  for(unsigned tick=0;tick<1200;tick+=23) {
    frame.authored[0].pulse_ticks=tick;
    assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&a));
    Valid(&a);assert(a.vertex_count<=kActionContourMaxPoints*4+40*4);
    if(a.vertex_count>max_grains)max_grains=a.vertex_count;
    for(int i=0;i<a.vertex_count;++i) {
      const float x=a.vertices[i].position.x,y=a.vertices[i].position.y;
      assert(y<=-24+fabsf(x)*64/96+1); /* No grain sinks into a dune. */
    }
  }
  assert(max_grains>(unsigned)b.vertex_count);
}
static void MovingGlints(void) {
  const char *kinds[]={"particle-path","cloud-bank","particle-area"};
  const char *extra[]={"points=-96,24 0,-16 96,24\npattern=sand\n",
    "points=-96,24 0,-16 96,24\namplitude=18\ndrift=110\n",
    "pattern=sand\ntravel-x=96\ntravel-y=-24\n"};
  char text[512];
  for(unsigned k=0;k<3;++k) {
    snprintf(text,sizeof(text),"[effects]\nversion=1\n[emitter:01:02:0:%s:1]\n"
        "width=192\nheight=96\nparticles=12\ncolor=c09a60\nintensity=.8\ntwinkle=.9\n%s",kinds[k],extra[k]);
    Parse(text);
    const unsigned layer=frame.authored[0].render_layer;
    unsigned reflections=0;ArRenderPointF first={0};bool moved=false;
    for(unsigned tick=0;tick<500;tick+=17) {
      frame.authored[0].pulse_ticks=tick;frame.authored[0].field_style.twinkle=0;
      assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&a));
      frame.authored[0].field_style.twinkle=.9f;
      assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));Valid(&b);
      assert(a.vertex_count==b.vertex_count&&a.index_count==b.index_count);
      for(int i=0;i<a.vertex_count;++i) {
        assert(a.vertices[i].position.x==b.vertices[i].position.x&&a.vertices[i].position.y==b.vertices[i].position.y);
        assert(b.vertices[i].color.a>=a.vertices[i].color.a);
        if(b.vertices[i].color.r>a.vertices[i].color.r+.05f)++reflections;
      }
      if(a.vertex_count){if(!tick)first=a.vertices[a.vertex_count-1].position;
        else if(first.x!=a.vertices[a.vertex_count-1].position.x)moved=true;}
    }
    assert(reflections&&moved); /* Highlights follow moving grains, without adding geometry. */
    frame.authored[0].pulse_ticks=37;
    assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&a));
    frame.authored[0].pulse_ticks=411;
    assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));
    frame.authored[0].pulse_ticks=37;
    assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));
    assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(a.vertices[0])));
    frame.authored[0].tuning.intensity=0;
    assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));
    for(int i=0;i<b.vertex_count;++i)assert(b.vertices[i].color.a==0);
  }
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

static void SoftLightScroll(void) {
  frame=(ActionSceneEffectFrame){0};
  /* The lower light in Kassandora 2/5. Its centre enters the bottom of a
   * 64-row extended capture after its illuminated area is already visible. */
  Parse("[effects]\nversion=1\n[emitter:01:02:0:soft-light:1]\n"
      "x=520\ny=1424\nwidth=512\nheight=224\nintensity=1.3\ncolor=d6aa71\n"
      "light-scenery=1\nlight-player=1\nlight-enemies=1\n");
  ActionEffectInstance *e=&frame.authored[0];
  const unsigned layer=ActionEffectReceivers_Layer(e);
  DioramaProjection view={.valid=true,.output_width=1024,.output_height=704,
      .texture_width=512,.texture_height=352,.aspect_x=2,.height_scale=2,
      .matrix={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},
      .bg1_plane={.valid=true,.u1=1,.v1=1}};
  ActionEffectProjectionContext c={.diorama_projection=&view,.ws_extra=128,.ws_extra_top=64,
      .scenery_lighting_only=true};
  unsigned max_vertices=0;
  for(unsigned edge=0;edge<4;++edge) for(int offset=-16;offset<=16;++offset) {
    const float x=edge<2?(edge?512:0)+offset:256;
    const float y=edge>=2?(edge==3?352:0)+offset:176;
    c.bg1_camera_x=(int16_t)(e->world_x+c.ws_extra-x);
    c.bg1_camera_y=(int16_t)(e->world_y+c.ws_extra_top-y);
    assert(ActionSceneDecorationRender_Build(&frame,layer,true,false,
        ActionEffectProjection_ProjectPoint,ActionEffectProjection_ClipBounds,&c,&a));
    assert(a.vertex_count&&a.index_count);Valid(&a);
    if((unsigned)a.vertex_count>max_vertices)max_vertices=a.vertex_count;
    /* Compare illumination at a fixed world point, including when the
     * light's centre and its unit sizing probes are outside the capture. */
    const float px=edge<2?(edge?-64:64):17;
    const float py=edge>=2?(edge==3?-48:48):13;
    ArRenderPointF point;
    assert(ActionEffectProjection_ProjectPoint(&c,e,px,py,&point));
    receiver=(ActionReceiverLighting){.frame=&frame,.player=a};
    float measured[3],expected[3],add[3];
    ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,point.x,point.y,measured,add);
    assert(ActionEffectReceivers_Prepare(&frame,1,2,e->world_x-128,e->world_y-112,0,0,true,&receiver));
    ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,e->world_x+px,e->world_y+py,expected,add);
    for(unsigned channel=0;channel<3;++channel) {
      if(fabsf(measured[channel]-expected[channel])>=.0001f)
        fprintf(stderr,"soft edge=%u offset=%d channel=%u: %g != %g\n",edge,offset,channel,measured[channel],expected[channel]);
      assert(measured[channel]>1 && fabsf(measured[channel]-expected[channel])<.0001f);
    }
    /* Source packets keep the entire field. Per-presentation clipping uses
     * resident camera motion, without recapturing based on the centre. */
    const unsigned count=SourceBuild(layer,true,false,&c);
    assert(count==kActionEffectGlowIndices/3);
    for(unsigned i=0;i<count;++i)assert(primitives[i].meta[0]==kActionSourceGlowTriangle);
  }
  c.bg1_camera_x=e->world_x+1024;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,false,
      ActionEffectProjection_ProjectPoint,ActionEffectProjection_ClipBounds,&c,&a));
  assert(!a.vertex_count&&!a.index_count);
  printf("Soft-light scrolling: four capture edges, stable receiver brightness, full GPU packets; unchanged %u-vertex budget.\n",max_vertices);
}

static void BillboardExtents(void) {
  const DioramaProjection view={.valid=true,.output_width=1024,.output_height=704,
      .texture_width=512,.texture_height=352,.aspect_x=2,.height_scale=2,
      .matrix={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},
      .bg1_plane={.valid=true,.u1=1,.v1=1}};
  ActionEffectProjectionContext c={.diorama_projection=&view};
  ActionEffectInstance e={.world_x=256,.world_y=176,.flags=kActionEffectFlag_Visible,
      .projection_plane=kActionEffectProjectionPlane_Bg1,
      .render_layer=kActionEffectRenderLayer_Bg1Plane};
  const ActionEffectGlowStyle glow={.radius_x=32,.radius_y=24,
      .ring_scale={.16f,.58f,1},.centre={1,.5f,.2f,1},
      .ring={{1,.5f,.2f,.8f},{1,.5f,.2f,.2f},{1,.5f,.2f,0}},.axis_x=1};
  for(unsigned cloud=0;cloud<2;++cloud)for(unsigned edge=0;edge<4;++edge)
    for(unsigned outside=0;outside<2;++outside) {
      const int distance=outside?160:8;
      e.world_x=edge<2?(edge?512+distance:-distance):256;
      e.world_y=edge>=2?(edge==3?352+distance:-distance):176;
      ActionEffectGeometryWriter writer=GeometryWriter(a.vertices,kActionSceneEffectRenderMaxVertices,
          a.indices,kActionSceneEffectRenderMaxIndices);
      if(cloud)assert(AppendSceneSoftCloud(&writer,&e,0,0,32,24,
          (ArRenderColorF){.5f,.7f,1,1},.5f,7,ActionEffectProjection_ProjectPoint,&c));
      else assert(AppendGlow(&writer,&e,&glow,1,0,0,ActionEffectProjection_ProjectPoint,&c));
      if(writer.vertex_count!=(outside?0:cloud?kActionSceneEffectWaterfallMistCloudVertices:kActionEffectGlowVertices))
        fprintf(stderr,"billboard cloud=%u edge=%u outside=%u: %d vertices\n",cloud,edge,outside,writer.vertex_count);
      assert(writer.vertex_count==(outside?0:cloud?kActionSceneEffectWaterfallMistCloudVertices:kActionEffectGlowVertices));
      assert(writer.index_count==(outside?0:cloud?kActionSceneEffectWaterfallMistCloudIndices:kActionEffectGlowIndices));
      if(!outside) {
        ArRenderPointF anchor;
        assert(!ActionEffectProjection_ProjectPoint(&c,&e,0,0,&anchor));
        assert(ActionEffectProjection_ProjectBillboardPoint(&c,&e,0,0,&anchor));
        assert(a.vertices[0].position.x==anchor.x&&a.vertices[0].position.y==anchor.y);
      }
    }
  puts("Glow/cloud extents: visible spill survives offscreen centres at all four edges; fully outside volumes culled with unchanged geometry budgets.");
}
static void DeferredAuthored(void) {
  static const struct {const char *kind,*extra;} cases[]={
    {"soft-light",""},{"motes",""},{"free-mist",""},{"ground-mist","mist-height=26\n"},
    {"particle-area","particles=8\npattern=snow\n"},{"light-fan",""},{"water-surface",""},
    {"drips",""},{"waterfall-spray",""},{"cloud-bank",""},
    {"cloud-bank","points=-96,24 -48,-8 0,0 48,-24 96,16\namplitude=17\ndrift=110\n"},
    {"cloud-bank","points=-64,0 64,16\n"},
    {"cloud-bank","points=-96,24 0,-24 96,24\ncarry=156\nparticles=40\namplitude=26\ndrift=110\n"},
    {"exposure","brightness=.5\nfade-left=32\nfade-top=24\nfade-right=32\nfade-bottom=24\ndim-scenery=1\n"},
    {"exposure","brightness=1.6\nfade-top=40\ndim-scenery=1\n"},{"wet-contour","points=0,0 40,10 80,0\n"},
    {"halo",""},{"light-gradient",""},{"flame",""},{"torch","shadows=1\n"},{"particle-path","points=-60,-40 0,-24 60,40\npattern=sand\ngust=.8\nactivity=.65\n"},
    {"torch","particles=16\nlifetime=480\ntravel-x=12\ntravel-y=-16\nwander=8\ncolor-end=d9a35c\n"}};
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
    if(ActionEffectRecipes_IsEmitter(kind) && kind!=kActionEffect_AuthoredHeat && !covered[kind]) {
      fprintf(stderr,"Missing deferred emitter fixture: %s\n",ActionEffectRecipes_KindName(kind));
      assert(covered[kind]);
    }
  }
}
static void TorchShadows(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:torch:1]\nx=128\ny=160\nreach=3\nshadows=1\n");
  const unsigned layer=ActionEffectReceivers_Layer(&frame.authored[0]);
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,false,Project,NULL,NULL,&a));
  frame.scenery=(ActionMoonlightOcclusion){.valid=true,.count=1,
      .rectangles={{148,80,200,240}}};
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,false,Project,NULL,NULL,&b));
  assert(a.vertex_count>0&&a.vertex_count==b.vertex_count);Valid(&b);
  unsigned dimmed=0,unchanged=0;
  for(int i=0;i<a.vertex_count;++i) {
    assert(b.vertices[i].color.a<=a.vertices[i].color.a);
    assert(b.vertices[i].color.a>=a.vertices[i].color.a*.65f);
    if(b.vertices[i].color.a<a.vertices[i].color.a)++dimmed;
    else if(a.vertices[i].color.a>0)++unchanged;
  }
  assert(dimmed&&unchanged); /* Scenery interrupts the spill, not the flame body. */
  ActionEffectProjectionContext context={.visible_width=512,.snes_height=448,
      .capture_height=448,.viewport={0,0,1024,896}};
  const unsigned count=SourceBuild(layer,true,false,&context);
  unsigned shadowed=0;
  for(unsigned i=0;i<count;++i)if(primitives[i].meta[3])++shadowed;
  assert(shadowed&&shadowed<count); /* Resident rendering retains the same split. */
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
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,100,100,multiply,add);assert(multiply[0]==1&&add[0]==0); /* open centre */
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,166,100,multiply,add);assert(multiply[0]>1.05f&&add[0]==0);
  Parse("[effects]\nversion=1\n[emitter:01:02:0:light-gradient:123]\nx=100\ny=100\nwidth=1024\nheight=1024\ncolor=ff0000\ncolor-end=0000ff\nsoftness=0\nangle=0\nlight-player=1\n");
  assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,100,0,multiply,add);assert(multiply[0]>multiply[2]);
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,100,200,multiply,add);assert(multiply[0]<multiply[2]);
  const ActionEffectLocalRect clip={0,0,256,224};
  assert(ActionSceneDecorationRender_Build(&frame,ActionEffectReceivers_Layer(&frame.authored[0]),true,false,Project,Clip,(void*)&clip,&a));
  assert(a.vertex_count>0&&a.vertex_count<=224);Valid(&a);
  for(int i=0;i<a.vertex_count;++i){assert(a.vertices[i].position.x>=-.001f&&a.vertices[i].position.x<=256.001f);assert(a.vertices[i].position.y>=-.001f&&a.vertices[i].position.y<=224.001f);}
}
static void Receivers(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:soft-light:123]\nx=100\ny=100\nwidth=96\nheight=64\ncolor=ffffff\nillumination=1\nlight-scenery=0\nlight-player=1\nlight-enemies=0\n[emitter:01:02:0:exposure:234]\nx=100\ny=100\nwidth=512\nheight=512\nintensity=.5\ndim-scenery=0\ndim-player=0\ndim-enemies=1\n");
  assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));float multiply[3],add[3];
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,100,100,multiply,add);
  assert(fabsf(multiply[0]-1.2f)<.001f && add[0]==0); /* Centre belongs to ONE glow triangle. */
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Enemies,100,100,multiply,add);assert(multiply[0]==.5f&&add[0]==0);
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Enemies,1000,1000,multiply,add);assert(multiply[0]==1&&add[0]==0);
  assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,false,&receiver));
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,100,100,multiply,add);assert(multiply[0]==1&&add[0]==0);
  ActionEffectInstance e={.kind=kActionEffect_CaveAmbientLight,.render_layer=kActionEffectRenderLayer_ForegroundLight};
  assert(ActionEffectReceivers_Layer(&e)==kActionEffectRenderLayer_Bg1Light);
  e.tuning.light_receivers_set=1;assert(ActionEffectReceivers_Layer(&e)==kActionEffectRenderLayer_Bg1Light);
  frame.decoration_count=255;assert(!ActionEnvironment_Bg1Dimming(&frame,1,2));
}
static void TorchReceivers(void) {
  /* Exercise both placed torches and captured wall sconces, with the same
   * independent dimming/light choices used in the Kassandora temple. */
  for (unsigned native = 0; native < 2; ++native) {
    frame=(ActionSceneEffectFrame){0};
    Parse("[effects]\nversion=1\n"
        "[emitter:01:02:0:torch:1]\nx=136\ny=304\nplacement=playfield\n"
        "light-scenery=0\nlight-player=1\nlight-enemies=1\n"
        "[emitter:01:02:0:exposure:2]\nx=200\ny=304\nwidth=1024\nheight=512\n"
        "brightness=.46\ndim-player=1\ndim-enemies=1\n");
    ActionEffectInstance *torch=&frame.authored[0];
    if (native) {
      frame.decorations[0]=*torch;
      torch=&frame.decorations[0];
      torch->kind=kActionEffect_WallTorch;
      torch->phase=kActionEffectPhase_WallTorch;
      torch->geometry.data.rect=(ActionEffectLocalRect){-5,-9,5,2};
      frame.decoration_count=1;
      frame.authored[0].flags=0;
    }
    frame.glow_field=*ActionGlowField_Bundled(2,3);
    frame.glow_field.SpillRadius[0]=110;
    frame.glow_field.SpillRadius[1]=88;
    ActionGlowField_Prepare(&frame.glow_field);
    frame.glow_field_valid=true;
    assert(ActionEffectReceivers_Prepare(&frame,1,2,0,160,0,160,true,&receiver));
    float multiply[3],add[3],enemy_multiply[3],enemy_add[3];
    ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,160,310,multiply,add);
    ActionEffectReceivers_Sample(&receiver,kActionReceiver_Enemies,160,310,enemy_multiply,enemy_add);
    assert(multiply[0]>.51f && multiply[0]>multiply[1] && multiply[1]>multiply[2]);
    assert(add[0]==0 && add[1]==0 && add[2]==0);
    assert(!memcmp(multiply,enemy_multiply,sizeof(multiply)) && !memcmp(add,enemy_add,sizeof(add)));
    ActionEffectReceivers_Sample(&receiver,kActionReceiver_Enemies,600,310,multiply,add);
    assert(fabsf(multiply[0]-.46f)<.00001f && add[0]==0); /* Outside the light, still inside the dimming box. */
    torch->tuning.light_receivers=kActionReceiver_Player;
    assert(ActionEffectReceivers_Prepare(&frame,1,2,0,160,0,160,true,&receiver));
    ActionEffectReceivers_Sample(&receiver,kActionReceiver_Enemies,160,310,multiply,add);
    assert(fabsf(multiply[0]-.46f)<.00001f && add[0]==0);
    assert(ActionEffectReceivers_Prepare(&frame,1,2,0,160,0,160,false,&receiver));
    ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,160,310,multiply,add);
    assert(fabsf(multiply[0]-.46f)<.00001f && add[0]==0);
  }
}
static void ReceiverIllumination(void) {
  frame=(ActionSceneEffectFrame){0};
  Parse("[effects]\nversion=1\n"
      "[emitter:01:02:0:soft-light:1]\nx=100\ny=100\nwidth=96\nheight=64\n"
      "color=ffffff\nintensity=3.75\nillumination=1\nlight-player=1\nlight-enemies=1\n"
      "[emitter:01:02:0:exposure:2]\nx=100\ny=100\nwidth=512\nheight=512\n"
      "brightness=.25\ndim-player=1\ndim-enemies=1\n");
  float multiply[3],add[3];
  /* Scenery uses different blend passes for glows and surface light. Object
   * illumination must restore the palette identically from either pass. */
  const unsigned layers[]={kActionEffectRenderLayer_Bg1Plane,kActionEffectRenderLayer_Bg1Light};
  for(unsigned layer=0;layer<2;++layer) {
    frame.authored[0].render_layer=layers[layer];
    assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));
    for(unsigned role=0;role<2;++role) {
      const unsigned target=role?kActionReceiver_Enemies:kActionReceiver_Player;
      ActionEffectReceivers_Sample(&receiver,target,100,100,multiply,add);
      for(unsigned c=0;c<3;++c)assert(fabsf(multiply[c]-1)<.00001f&&add[c]==0);
      /* Light cannot disappear when ambient illumination reaches black. */
      frame.authored[1].field_style.brightness=0;
      ActionEffectReceivers_Sample(&receiver,target,100,100,multiply,add);
      for(unsigned c=0;c<3;++c)assert(fabsf(multiply[c]-.75f)<.00001f&&add[c]==0);
      frame.authored[1].field_style.brightness=.25f;
    }
  }
  /* A warm source both reveals detail and tints it: black stays black, while
   * every nonzero palette channel gains light independently of room dimming. */
  frame.authored[0].tuning.color=0xff8040;
  assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,100,100,multiply,add);
  assert(fabsf(multiply[0]-1)<.00001f);
  assert(multiply[0]>multiply[1]&&multiply[1]>multiply[2]&&multiply[2]>.25f);
  const float dark_blue[3]={.04f,.12f,.70f};
  float lit_luma=0,dim_luma=0;
  const float weights[3]={.2126f,.7152f,.0722f};
  for(unsigned c=0;c<3;++c) {
    assert(add[c]==0);
    lit_luma+=dark_blue[c]*multiply[c]*weights[c];
    dim_luma+=dark_blue[c]*.25f*weights[c];
  }
  assert(lit_luma>dim_luma);
  assert(dark_blue[2]*multiply[2]>dark_blue[0]*multiply[0]); /* still blue armor */
  /* Extended light intensity raises radiance without increasing alpha or
   * losing the source tint; both receiver roles can exceed the old 2x cap. */
  for(unsigned intensity=4;intensity<=16;intensity*=2) {
    frame.authored[0].tuning.intensity=(float)intensity;
    assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));
    for(unsigned role=0;role<2;++role) {
      ActionEffectReceivers_Sample(&receiver,role?kActionReceiver_Enemies:kActionReceiver_Player,100,100,multiply,add);
      const float tint[]={1,128/255.f,64/255.f};
      for(unsigned c=0;c<3;++c)assert(fabsf(multiply[c]-(.25f+.2f*intensity*tint[c]))<.00001f&&add[c]==0);
    }
  }
  frame.authored[2]=frame.authored[0];frame.authored_count=3;
  assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,100,100,multiply,add);
  assert(multiply[0]==kActionReceiverMaxIllumination); /* bounded overlapping sources */
  frame.authored_count=2;
  /* Weak/overlapping sources accumulate as illumination, not opaque washes. */
  frame.authored[0].tuning.intensity=1;
  frame.authored[0].tuning.color=0xffffff;
  frame.authored[2]=frame.authored[0];frame.authored_count=3;
  assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,100,100,multiply,add);
  assert(fabsf(multiply[0]-.65f)<.00001f&&add[0]==0);
  /* Native cave/castle ambient uses the same combination as authored boxes. */
  frame.authored[1].flags=0;receiver.dim_receivers=kActionReceiver_Player;
  receiver.dimming=.75f;receiver.ramp=(ArRenderRectF){0};
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,100,100,multiply,add);
  assert(fabsf(multiply[0]-.65f)<.00001f&&add[0]==0);
}
static void ExposureBoxes(void) {
  frame=(ActionSceneEffectFrame){0};
  Parse("[effects]\nversion=1\n[emitter:01:02:0:exposure:1]\nx=100\ny=100\nwidth=200\nheight=200\nbrightness=.5\nfade-left=40\nfade-top=40\ndim-player=1\n"
        "[emitter:01:02:0:exposure:2]\nx=180\ny=180\nwidth=200\nheight=200\nbrightness=.5\nfade-left=40\nfade-top=40\ndim-player=1\n");
  assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));float multiply[3],add[3];
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,150,150,multiply,add);
  assert(multiply[0]==.5f); /* overlap is .5, never .25 */
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,20,20,multiply,add);
  assert(fabsf(multiply[0]-.875f)<.00001f);
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Enemies,150,150,multiply,add);assert(multiply[0]==1);
  ActionEffectLocalRect clip={-50,-50,400,400};
  assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_Bg1Mist,true,true,Project,Clip,&clip,&a));Valid(&a);
  // Every interior sample belongs to at most one emitted triangle, even where
  // boxes and their feathered corners overlap. Verify actual alpha blending.
  for(float y=1.37f;y<280;y+=9.1f)for(float x=1.19f;x<280;x+=9.3f) {
    float transmission=1;unsigned hits=0;
    for(int i=0;i<a.index_count;i+=3) {
      const ArRenderVertex2D *v0=&a.vertices[a.indices[i]],*v1=&a.vertices[a.indices[i+1]],*v2=&a.vertices[a.indices[i+2]];
      const float ax=v1->position.x-v0->position.x,ay=v1->position.y-v0->position.y,bx=v2->position.x-v0->position.x,by=v2->position.y-v0->position.y;
      const float det=ax*by-ay*bx;if(fabsf(det)<.00001f)continue;
      const float u=((x-v0->position.x)*by-(y-v0->position.y)*bx)/det;
      const float v=(ax*(y-v0->position.y)-ay*(x-v0->position.x))/det;
      if(u<0||v<0||u+v>=1)continue;
      transmission*=1-(v0->color.a*(1-u-v)+v1->color.a*u+v2->color.a*v);++hits;
    }
    const ActionEffectInstance *boxes[]={&frame.authored[0],&frame.authored[1]};
    assert(hits<=1);assert(fabsf(transmission-ActionExposure_Union(boxes,2,x,y))<.02f);
  }
  const int vertices=a.vertex_count;
  ActionExposureLighting exposure={.origin_x=0,.origin_y=0,.width=400,.height=400};
  ActionExposure_Prepare(&exposure,&frame);
  assert(exposure.count==1&&exposure.counts[0]==2);
  assert(ActionExposure_Scenery(&exposure,.375f,.375f)==.5f);
  const ArRenderRectF ramp=SceneryDimming_ToUv((ArRenderRectF){800,640,320,448},500,600,640,512);
  assert(fabsf(SceneryDimming_Amount(.45f,ramp,(1500-500)/640.f,(864-600)/512.f)-.225f)<.00001f);
  ActionEffectProjectionContext context={.visible_width=512,.snes_height=448,.capture_height=448,.viewport={0,0,512,448}};
  assert(SourceBuild(kActionEffectRenderLayer_Bg1Mist,true,true,&context)>0);
  context.scenery_exposure_in_mesh=true;
  assert(SourceBuild(kActionEffectRenderLayer_Bg1Mist,true,true,&context)==0); /* no double dimming */
  frame.authored[1].field_style.brightness=1.5f;frame.authored[1].render_layer=kActionEffectRenderLayer_Bg1Light;
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,150,150,multiply,add);assert(multiply[0]==.75f);
  assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_Bg1Light,true,true,Project,Clip,&clip,&a));Valid(&a);
  const clock_t start=clock();
  for(unsigned i=0;i<1000;++i)assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_Bg1Mist,true,true,Project,Clip,&clip,&a));
  printf("Light boxes: non-stacking union, receiver masks, feathering and deferred GPU geometry passed (%d union vertices; %.3f ms/build).\n",vertices,1000.0*(clock()-start)/CLOCKS_PER_SEC/1000);
}
typedef struct ExposureMeshProbe {
  float x,y,brightness;
  unsigned hits;
} ExposureMeshProbe;
typedef struct ExposureMeshCheck {
  ExposureMeshProbe probes[8];
  unsigned count,draws,vertices,indices,fail_draw;
} ExposureMeshCheck;
static bool CheckExposureMesh(void *context,const ArRenderVertex2D *v,int nv,const int32_t *idx,int ni) {
  ExposureMeshCheck *check=context;
  ++check->draws;check->vertices+=(unsigned)nv;check->indices+=(unsigned)ni;
  assert(nv<=kSceneryShadingBatchVertices);
  for(int i=0;i<nv;++i) {
    assert(isfinite(v[i].position.x)&&isfinite(v[i].position.y));
    assert(isfinite(v[i].color.r)&&fabsf(v[i].color.a-.65f)<.00001f);
  }
  for(int i=0;i<ni;++i)assert(idx[i]>=0&&idx[i]<nv);
  for(unsigned p=0;p<check->count;++p)for(int i=0;i<ni;i+=3) {
    const ArRenderVertex2D *a=&v[idx[i]],*b=&v[idx[i+1]],*c=&v[idx[i+2]];
    const float ax=b->position.x-a->position.x,ay=b->position.y-a->position.y;
    const float bx=c->position.x-a->position.x,by=c->position.y-a->position.y;
    const float det=ax*by-ay*bx;if(fabsf(det)<.00001f)continue;
    const float px=check->probes[p].x-a->position.x,py=check->probes[p].y-a->position.y;
    const float u=(px*by-py*bx)/det,w=(ax*py-ay*px)/det;
    if(u<-.000001f||w<-.000001f||u+w>1.000001f)continue;
    const float value=(a->color.r*(1-u-w)+b->color.r*u+c->color.r*w)/.8f;
    if(check->probes[p].hits)assert(fabsf(check->probes[p].brightness-value)<.0001f);
    check->probes[p].brightness=value;++check->probes[p].hits;
  }
  return !check->fail_draw||check->draws<check->fail_draw;
}
static bool DrawExposureMesh(ActionExposureLighting *exposure,ArRenderRectF source_uv,ExposureMeshCheck *check) {
  ArRenderVertex2D vertices[81];int32_t indices[384];unsigned ni=0;
  for(unsigned y=0;y<=8;++y)for(unsigned x=0;x<=8;++x) {
    const float u=x/8.f,v=y/8.f;
    vertices[y*9+x]=(ArRenderVertex2D){
      .position={exposure->origin_x+u*624,exposure->origin_y+v*352},
      .tex_coord={(u*624/640-source_uv.x)/source_uv.w,(v-source_uv.y)/source_uv.h},
      .color={.8f,.6f,.4f,.65f}};
    if(x<8&&y<8){const int32_t base=(int32_t)(y*9+x);
      indices[ni++]=base;indices[ni++]=base+1;indices[ni++]=base+10;
      indices[ni++]=base;indices[ni++]=base+10;indices[ni++]=base+9;}
  }
  return SceneryShading_Draw(&exposure->field,source_uv,vertices,81,indices,(int)ni,CheckExposureMesh,check);
}
static void ExposureMeshScroll(void) {
  /* The reported room has a hard box edge at world x=0. A camera-relative
   * eight-column mesh used to change a fixed wall's brightness from .8 to .46
   * when a coarse vertex crossed that edge. Exercise the submitted triangles,
   * including compact source UVs used by supersampling and priority surfaces. */
  const char *scenes[]={
    "[effects]\nversion=1\n[emitter:01:02:0:exposure:1]\nx=1024\ny=256\nwidth=2048\nheight=512\nbrightness=.46\n",
    "[effects]\nversion=1\n[emitter:01:02:0:exposure:1]\nx=8\ny=200\nwidth=12\nheight=24\nbrightness=.46\n",
    "[effects]\nversion=1\n[emitter:01:02:0:exposure:1]\nx=80\ny=220\nwidth=160\nheight=200\nbrightness=.5\n"
    "[emitter:01:02:0:exposure:2]\nx=140\ny=240\nwidth=160\nheight=200\nbrightness=.5\n",
    "[effects]\nversion=1\n[emitter:01:02:0:exposure:1]\nx=80\ny=220\nwidth=160\nheight=200\nbrightness=.5\nfade-left=64\nfade-top=64\n"
    "[emitter:01:02:0:exposure:2]\nx=180\ny=240\nwidth=160\nheight=200\nbrightness=1.3\nfade-right=64\n"
  };
  for(unsigned s=0;s<sizeof(scenes)/sizeof(scenes[0]);++s) {
    Parse(scenes[s]);
    for(unsigned compact=0;compact<2;++compact)for(int camera=-64;camera<=144;camera+=4) {
      ActionExposureLighting exposure={.origin_x=camera-184,.origin_y=camera*.25f,
        .width=640,.height=352};
      ActionExposure_Prepare(&exposure,&frame);
      ExposureMeshCheck check={.count=8,.probes={
        {-1.37f,200.19f,0,0},{1.37f,200.19f,0,0},{8.31f,199.17f,0,0},{16.19f,201.37f,0,0},
        {37.19f,157.37f,0,0},{110.31f,203.37f,0,0},{151.37f,213.19f,0,0},{221.37f,200.19f,0,0}}};
      assert(DrawExposureMesh(&exposure,compact?(ArRenderRectF){.06f,0,.9f,.8f}:
          (ArRenderRectF){0,0,1,1},&check));
      for(unsigned p=0;p<check.count;++p) {
        const ExposureMeshProbe *probe=&check.probes[p];
        const float expected=ActionExposure_Scenery(&exposure,
            (probe->x-exposure.origin_x)/640,(probe->y-exposure.origin_y)/352);
        assert(probe->hits);
        assert(fabsf(probe->brightness-expected)<(s==3?.009f:.0001f));
      }
      assert(check.draws==1);
    }
  }
  /* Many overlapping feathered boxes must stream without silently dropping
   * triangles. Also propagate a failed submission rather than partial success. */
  ActionEffectInstance box=frame.authored[0];frame.authored_count=kActionAuthoredMaxInstances;
  for(unsigned i=0;i<frame.authored_count;++i) {
    frame.authored[i]=box;
    frame.authored[i].world_x=90+i*17;frame.authored[i].world_y=60+i*11;
  }
  ActionExposureLighting exposure={.width=640,.height=352};ActionExposure_Prepare(&exposure,&frame);
  ExposureMeshCheck check={0};assert(DrawExposureMesh(&exposure,(ArRenderRectF){0,0,1,1},&check));
  assert(check.draws>1);
  check=(ExposureMeshCheck){.fail_draw=2};
  assert(!DrawExposureMesh(&exposure,(ArRenderRectF){0,0,1,1},&check));assert(check.draws==2);
  puts("Scenery exposure: fixed world boundaries across 424 scrolling/compact-UV views, feathering, overlap and streamed batches passed.");
}
static void ExposureGroupCapacity(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:exposure:1]\nx=100\ny=100\nwidth=200\nheight=200\nbrightness=.99\n");
  const ActionEffectInstance box=frame.authored[0];
  frame.authored_count=kActionAuthoredMaxInstances;
  for(unsigned distinct=0;distinct<2;++distinct) {
    float expected=1;
    for(unsigned i=0;i<frame.authored_count;++i) {
      frame.authored[i]=box;
      frame.authored[i].field_style.brightness=.99f+(distinct?i:i%3)*.0001f;
      if(distinct||i<3)expected*=frame.authored[i].field_style.brightness;
    }
    ActionExposureLighting exposure={.width=200,.height=200};
    ActionExposure_Prepare(&exposure,&frame);
    assert(exposure.count==(distinct?kActionAuthoredMaxInstances:3));
    assert(fabsf(ActionExposure_Scenery(&exposure,.5f,.5f)-expected)<.00001f);
    assert(fabsf(ActionExposure_Cell(&exposure,.5f,.5f,.5f,.5f)-expected)<.00001f);
    float shade=0;
    assert(ActionExposure_Uniform(&exposure,.1f,.1f,.9f,.9f,&shade));
    assert(fabsf(shade-expected)<.00001f);
  }
}
static void BenchmarkExposureMesh(void) {
  printf("Effect frame: %zu bytes; exposure workspace: %zu bytes for %u authored slots.\n",
      sizeof(ActionSceneEffectFrame),sizeof(ActionExposureLighting),kActionAuthoredMaxInstances);
  for(unsigned count=1;count<=kActionAuthoredMaxInstances;count*=4) {
    Parse("[effects]\nversion=1\n[emitter:01:02:0:exposure:1]\nx=1024\ny=256\nwidth=2048\nheight=512\nbrightness=.46\n");
    const ActionEffectInstance box=frame.authored[0];frame.authored_count=count;
    for(unsigned i=1;i<count;++i) {
      frame.authored[i]=box;frame.authored[i].world_x=90+i*17;frame.authored[i].world_y=60+i*11;
      frame.authored[i].geometry.data.rect=(ActionEffectLocalRect){-80,-100,80,100};
      for(unsigned j=0;j<4;++j)frame.authored[i].field_style.fade[j]=64;
    }
    ActionExposureLighting exposure={.origin_x=-184,.origin_y=96,.width=640,.height=352};
    ActionExposure_Prepare(&exposure,&frame);
    ExposureMeshCheck check={0};const clock_t start=clock();
    for(unsigned i=0;i<100;++i) {
      check=(ExposureMeshCheck){0};assert(DrawExposureMesh(&exposure,(ArRenderRectF){0,0,1,1},&check));
    }
    printf("Scenery mesh: %u boxes, %u vertices, %u draws, %.3f ms/draw (including validation).\n",
        count,check.vertices,check.draws,1000.0*(clock()-start)/CLOCKS_PER_SEC/100);
  }
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
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,100,100,multiply,add);assert(multiply[0]>1&&add[0]==0);
  ActionEffectReceivers_Sample(&receiver,kActionReceiver_Enemies,100,100,multiply,add);assert(multiply[0]==1&&add[0]==0);
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
                       "[member:01:01:0:forest-forward:1]\nlight-player=2\n"};
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
  assert(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg1Light, true,
                                           false, Project, NULL, NULL, &a));
  assert(a.vertex_count);
  ActionEffectRecipes_Apply(&recipes, 1, 1, 0, 37, NULL, &frame);
  assert(frame.members.count == 2 && frame.members.records[0].offset_x == 32 &&
         frame.members.records[0].angle == -12);
  assert(frame.effect_count == 0 && frame.authored_count == 0 && frame.decoration_count == 1);
  assert(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg1Light, true,
                                           false, Project, NULL, NULL, &b));
  Valid(&b);
  assert(a.vertex_count != b.vertex_count ||
         memcmp(a.vertices, b.vertices, a.vertex_count * sizeof(a.vertices[0])));
  assert(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg1Light, true,
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
             "2\nangle=30\nintensity=16\n",
             i);
    strcat(stress, record);
  }
  assert(ActionEffectRecipes_Parse(&recipes, stress, strlen(stress), &line));
  ActionEffectRecipes_Apply(&recipes, 1, 1, 0, 37, NULL, &frame);
  for (int x = 0; x < 3600; x += 80) {
    frame.decorations[0].world_x = x;
    assert(ActionSceneDecorationRender_Build(&frame, kActionEffectRenderLayer_Bg1Light, true,
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
  /* A ceiling above the placed source is behind the emitted rays. The old
   * bounds-top shadow origin incorrectly dimmed their first segment. */
  frame.scenery =
      (ActionMoonlightOcclusion){.valid = 1, .count = 1, .rectangles = {{-64, -64, 64, -8}}};
  assert(ActionSceneDecorationRender_Build(&frame, layer, true, false, Project, NULL, NULL, &b));
  for (int i = 0; i < a.vertex_count; ++i)
    if (a.vertices[i].position.y >= 0) assert(a.vertices[i].color.a == b.vertices[i].color.a);
  ActionEffectProjectionContext context = {.visible_width=512,.snes_height=448,
      .capture_height=448,.viewport={0,0,1024,896}};
  const unsigned count = SourceBuild(layer, true, false, &context);
  assert(count);
  for (unsigned i = 0; i < count; ++i) {
    assert(primitives[i].meta[3]);
    assert(primitives[i].extra[0] == 0 && primitives[i].extra[1] == 0);
  }
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
static void SurfaceLightingSplit(void) {
  frame=(ActionSceneEffectFrame){0};
  Parse("[effects]\nversion=1\n"
      "[emitter:01:02:0:torch:1]\nx=128\ny=160\nwidth=96\nheight=128\nlight-scenery=1\n"
      "[emitter:01:02:0:exposure:2]\nx=128\ny=160\nwidth=256\nheight=256\nbrightness=.25\n");
  ActionEffectProjectionContext context={.visible_width=512,.snes_height=448,
      .capture_height=448,.viewport={0,0,1024,896},.scenery_exposure_in_mesh=true};
  assert(ActionEffectReceivers_HasSurfaceLights(&frame));
  unsigned combined=0,illumination=0,particles=0;
  for(unsigned layer=0;layer<kActionEffectRenderLayer_Count;++layer) {
    combined+=SourceBuild(layer,true,true,&context);
    context.scenery_lighting_only=true;
    illumination+=SourceBuild(layer,true,false,&context);
    context.scenery_lighting_only=false;
    context.scenery_lighting_in_material=true;
    particles+=SourceBuild(layer,true,true,&context);
    context.scenery_lighting_in_material=false;
  }
  assert(illumination && particles && combined==illumination+particles);
  frame.authored[0].tuning.light_receivers=kActionReceiver_Player;
  assert(!ActionEffectReceivers_HasSurfaceLights(&frame));
  context.scenery_lighting_only=true;
  for(unsigned layer=0;layer<kActionEffectRenderLayer_Count;++layer)
    assert(!SourceBuild(layer,true,false,&context));
  puts("Scenery illumination: lights separated once from particles/exposure; receiver masks respected.");
}
static void CastleSkyDepth(void) {
  frame=(ActionSceneEffectFrame){0};
  frame.castle_field=*ActionCastleField_Bundled(8);frame.castle_field_valid=true;
  frame.decoration_count=1;
  ActionEffectInstance *e=&frame.decorations[0];
  *e=(ActionEffectInstance){.kind=kActionEffect_CastleSky,
      .phase=kActionEffectPhase_CastleEnvironment,.environment_room=8,
      .world_x=128,.world_y=48,.phase_ticks=127,
      .flags=kActionEffectFlag_Visible|kActionEffectFlag_ClipToRect,
      .render_layer=kActionEffectRenderLayer_Bg2Plane,.projection_plane=kActionEffectProjectionPlane_Bg2,
      .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-384,-48,384,194}},
      .clip_rect={-384,-48,384,194}};
  /* The inherited field receiver mask must never bypass the back wall. */
  e->tuning.light_receivers_set=1;e->tuning.light_receivers=7;
  ActionEffectProjectionContext context={.visible_width=512,.snes_height=448,
      .capture_height=448,.viewport={0,0,1024,896}};
  const unsigned sky=SourceBuild(kActionEffectRenderLayer_Bg2Plane,true,false,&context);
  assert(sky && !ActionEffectReceivers_Mask(e));
  assert(!ActionEffectReceivers_HasSurfaceLights(&frame));
  context.scenery_lighting_in_material=true;
  assert(SourceBuild(kActionEffectRenderLayer_Bg2Plane,true,false,&context)==sky);
  context.scenery_lighting_only=true;
  for(unsigned layer=0;layer<kActionEffectRenderLayer_Count;++layer)
    assert(!SourceBuild(layer,true,false,&context));
  assert(ActionEffectReceivers_Prepare(&frame,2,8,0,0,0,0,true,&receiver));
  assert(!receiver.player.vertex_count && !receiver.enemies.vertex_count);
  /* Actual window spill continues to illuminate scenery and both roles. */
  e->kind=kActionEffect_CastleLight;e->render_layer=kActionEffectRenderLayer_Bg1Plane;
  e->projection_plane=kActionEffectProjectionPlane_Bg1;e->source_mask=1;
  e->world_y=-160;
  e->geometry.data.rect=e->clip_rect=(ActionEffectLocalRect){-384,0,384,544};
  assert(ActionEffectReceivers_HasSurfaceLights(&frame));
  assert(SourceBuild(kActionEffectRenderLayer_Bg1Plane,true,false,&context));
  assert(ActionEffectReceivers_Prepare(&frame,2,8,0,0,0,0,true,&receiver));
  assert(receiver.player.vertex_count && receiver.enemies.vertex_count);
  puts("Castle sky stays behind masonry; window spill retains scenery and actor receivers.");
}
static void HighIntensityGeometry(void) {
  const char *names[]={"soft-light","light-fan","light-gradient","halo","torch","flame"};
  static ActionEffectSourcePrimitive saved[kActionSourceMaximumPrimitives];
  const ActionEffectProjectionContext context={.visible_width=512,.snes_height=448,
      .capture_height=448,.viewport={0,0,1024,896}};
  for(unsigned family=0;family<sizeof(names)/sizeof(names[0]);++family) {
    frame=(ActionSceneEffectFrame){0};char text[256];
    snprintf(text,sizeof(text),"[effects]\nversion=1\n[emitter:01:02:0:%s:1]\n"
        "x=128\ny=160\nwidth=96\nheight=128\nintensity=4\ncolor=ff8040\n",names[family]);
    Parse(text);const unsigned layer=frame.authored[0].render_layer;
    assert(ActionSceneDecorationRender_Build(&frame,layer,true,false,Project,NULL,NULL,&a));
    assert(a.vertex_count>0);Valid(&a);
    const unsigned count=SourceBuild(layer,true,false,&context);assert(count);
    memcpy(saved,primitives,count*sizeof(*saved));
    frame.authored[0].tuning.intensity=16;
    assert(ActionSceneDecorationRender_Build(&frame,layer,true,false,Project,NULL,NULL,&b));
    assert(a.vertex_count==b.vertex_count&&a.index_count==b.index_count);Valid(&b);
    for(int i=0;i<a.vertex_count;++i) {
      const ArRenderVertex2D *x=&a.vertices[i],*y=&b.vertices[i];
      assert(x->position.x==y->position.x&&x->position.y==y->position.y);
      assert(x->color.a==y->color.a);
      assert(fabsf(y->color.r-4*x->color.r)<.00001f);
      assert(fabsf(y->color.g-4*x->color.g)<.00001f);
      assert(fabsf(y->color.b-4*x->color.b)<.00001f);
    }
    assert(SourceBuild(layer,true,false,&context)==count);
    for(unsigned i=0;i<count;++i) {
      for(unsigned c=0;c<(saved[i].meta[0]==kActionSourceQuad?4u:3u);++c) {
        float *color=c<3?saved[i].colors[c]:saved[i].extra;
        for(unsigned rgb=0;rgb<3;++rgb)color[rgb]*=4;
      }
      assert(!memcmp(&saved[i],&primitives[i],sizeof(saved[i])));
    }
  }
  puts("High intensity: six light families preserve falloff and CPU/GPU geometry counts at 16x.");
}
/* Compare actual material packets and object-receiver geometry. The same
 * calibration must never touch the normal additive/particle pass. */
static void CheckIlluminationResponse(ActionEffectInstance *effect) {
  static ActionEffectSourcePrimitive saved[kActionSourceMaximumPrimitives];
  ActionEffectProjectionContext context={.visible_width=512,.snes_height=448,
      .capture_height=448,.viewport={0,0,1024,896},.scenery_lighting_only=true};
  effect->tuning.light_receivers_set=1;
  effect->tuning.light_receivers=7;
  effect->tuning.illumination_set=1;
  effect->tuning.illumination=1;
  const unsigned layer=ActionEffectReceivers_SurfaceLayer(effect);
  const unsigned count=SourceBuild(layer,true,false,&context);assert(count);
  memcpy(saved,primitives,count*sizeof(*saved));
  assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));
  a=receiver.player;assert(a.vertex_count>0);
  /* Also exercise zero and an explicit non-default multiplier. */
  const float gains[]={ActionLightKind_Illumination(effect->kind),0,5};
  for(unsigned g=0;g<3;++g) {
    effect->tuning.illumination_set=g!=0;effect->tuning.illumination=gains[g];
    assert(SourceBuild(layer,true,false,&context)==count);
    for(unsigned i=0;i<count;++i) {
      ActionEffectSourcePrimitive expected=saved[i];
      for(unsigned c=0;c<(expected.meta[0]==kActionSourceQuad?4u:3u);++c) {
        float *color=c<3?expected.colors[c]:expected.extra;
        for(unsigned rgb=0;rgb<3;++rgb)color[rgb]*=gains[g];
      }
      assert(!memcmp(&expected,&primitives[i],sizeof(expected)));
    }
    assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));
    for(unsigned role=0;role<2;++role) {
      const ActionSceneEffectRenderBatch *result=role?&receiver.enemies:&receiver.player;
      assert(result->vertex_count==a.vertex_count&&result->index_count==a.index_count);
      assert(!memcmp(result->indices,a.indices,a.index_count*sizeof(a.indices[0])));
      for(int i=0;i<a.vertex_count;++i) {
        ArRenderVertex2D expected=a.vertices[i];
        expected.color.r*=gains[g];expected.color.g*=gains[g];expected.color.b*=gains[g];
        assert(!memcmp(&expected,&result->vertices[i],sizeof(expected)));
      }
    }
  }
  context.scenery_lighting_only=false;
  const unsigned volume_layer=ActionEffectReceivers_Layer(effect);
  effect->tuning.illumination=1;
  if(frame.effect_count)assert(ActionSceneEffectRender_Build(&frame,true,true,Project,NULL,&a));
  else assert(ActionSceneDecorationRender_Build(&frame,volume_layer,true,true,Project,NULL,NULL,&a));
  const unsigned normal_count=SourceBuild(volume_layer,true,true,&context);
  memcpy(saved,primitives,normal_count*sizeof(*saved));
  effect->tuning.illumination=16;
  if(frame.effect_count)assert(ActionSceneEffectRender_Build(&frame,true,true,Project,NULL,&b));
  else assert(ActionSceneDecorationRender_Build(&frame,volume_layer,true,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&a.index_count==b.index_count);
  assert(!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(a.vertices[0])));
  assert(SourceBuild(volume_layer,true,true,&context)==normal_count);
  assert(!memcmp(saved,primitives,normal_count*sizeof(*saved)));
}
static void NativeDaylightReceivers(void) {
  ActionEnvironmentScene scene={.group=1,.room=1,.clock=37};
  ActionRayField forest=*ActionRayField_Bundled();
  memset(forest.dimensions,0,sizeof(forest.dimensions));forest.witness_count=0;
  frame=(ActionSceneEffectFrame){0};ActionRayField_Capture(&scene,&frame,&forest,1);
  assert(frame.decoration_count==3&&ActionEffectReceivers_HasSurfaceLights(&frame));
  ActionEffectInstance *front=&frame.decorations[2];
  assert(front->kind==kActionEffect_ForestForwardLight&&ActionEffectReceivers_Mask(front)==7);
  assert(!ActionEffectReceivers_Mask(&frame.decorations[0])); /* Rear volume is the same sunlight. */
  ActionEffectProjectionContext context={.visible_width=512,.snes_height=448,
      .capture_height=448,.viewport={0,0,1024,896},.scenery_lighting_only=true};
  assert(SourceBuild(kActionEffectRenderLayer_Bg1Light,true,false,&context)>0);
  assert(!SourceBuild(kActionEffectRenderLayer_Bg2Plane,true,false,&context));
  assert(ActionEffectReceivers_Prepare(&frame,1,1,0,0,0,0,true,&receiver));
  assert(receiver.player.vertex_count>0&&receiver.player.vertex_count==receiver.enemies.vertex_count);
  context.scenery_lighting_only=false;context.scenery_lighting_in_material=true;
  assert(!SourceBuild(kActionEffectRenderLayer_Bg1Light,true,false,&context));
  assert(!SourceBuild(kActionEffectRenderLayer_ForegroundLight,true,false,&context));
  assert(SourceBuild(kActionEffectRenderLayer_Bg2Plane,true,false,&context)>0);
  CheckIlluminationResponse(front);

  ActionMoonField moon=*ActionMoonField_Bundled();
  memset(moon.Dimensions,0,sizeof(moon.Dimensions));moon.WitnessCount[0]=0;
  frame=(ActionSceneEffectFrame){0};scene.group=2;
  ActionMoonField_Capture(&scene,&frame,&moon,2);frame.moonlight.valid=true;
  assert(ActionEffectReceivers_HasSurfaceLights(&frame));
  assert(ActionEffectReceivers_Mask(&frame.decorations[0])==7);
  context.scenery_lighting_only=true;
  assert(SourceBuild(kActionEffectRenderLayer_Bg1Plane,true,false,&context)>0);
  assert(!SourceBuild(kActionEffectRenderLayer_Bg2Plane,true,false,&context));
  context.scenery_lighting_only=false;
  assert(SourceBuild(kActionEffectRenderLayer_Bg2Plane,true,false,&context)>0);
  assert(!SourceBuild(kActionEffectRenderLayer_Bg1Plane,true,false,&context));
  CheckIlluminationResponse(&frame.decorations[0]);

  /* Midpoint-camera lights must reach the same world point as their rays,
   * including odd/negative camera values and both scroll directions. */
  frame=(ActionSceneEffectFrame){0};
  Parse("[effects]\nversion=1\n[emitter:01:02:0:soft-light:1]\nx=128\ny=160\nwidth=64\nheight=64\nlight-player=1\nlight-enemies=1\n");
  frame.authored[0].projection_plane=kActionEffectProjectionPlane_BetweenBackgrounds;
  for(int c=-3;c<5;++c) {
    const int x=101*c,y=77*c,bx=25*c,by=19*c;
    assert(ActionEffectReceivers_Prepare(&frame,1,2,x,y,bx,by,true,&receiver));
    float multiply[3],add[3];
    ActionEffectReceivers_Sample(&receiver,kActionReceiver_Player,128+x-(x+bx)/2,160+y-(y+by)/2,multiply,add);
    assert(multiply[0]>1.1f);
  }
  puts("Native daylight: forest/moon material routing, default sprite receivers, stable camera anchors and independent volume calibration passed.");
}
static void LightResponseAudit(void) {
  const char *names[]={"soft-light","light-fan","light-gradient","halo","torch","flame"};
  for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i) {
    frame=(ActionSceneEffectFrame){0};char text[256];
    snprintf(text,sizeof(text),"[effects]\nversion=1\n[emitter:01:02:0:%s:1]\n"
        "x=128\ny=160\nwidth=96\nheight=128\nplacement=playfield\n",names[i]);
    Parse(text);CheckIlluminationResponse(&frame.authored[0]);
  }
  unsigned families=0;
  for(unsigned i=0;i<ActionEffectPreview_Count();++i) {
    const unsigned kind=ActionEffectPreview_Kind(i);
    if(!ActionLightKind_Supported(kind))continue;
    const ActionEffectPreviewEvent event={.kind=kind,.duration=240,.x=128,.y=160,.seed=37};
    for(unsigned tick=1;tick<32;tick+=7) {
      frame=(ActionSceneEffectFrame){.effect_count=1};
      assert(ActionEffectPreview_Build(&event,tick,&frame.effects[0]));
      CheckIlluminationResponse(&frame.effects[0]);
    }
    ++families;
  }
  assert(families==18);
  puts("Light response: 6 authored and 18 actor/attack families retain geometry, falloff and additive effects; CPU/GPU receiver gains agree.");
}
static void NativeSourceOverrides(void) {
  frame=(ActionSceneEffectFrame){.decoration_count=1};
  frame.decorations[0]=(ActionEffectInstance){.generation=1,.kind=kActionEffect_CaveAmbientLight,
      .flags=kActionEffectFlag_Visible,.tuning={.light_receivers=6,.light_receivers_set=1,
      .dim_receivers=5,.dim_receivers_set=1}};
  Parse("[effects]\nversion=1\n[source:01:02:0:cave-light:1]\nintensity=2\n");
  assert(frame.decorations[0].tuning.light_receivers==6 && frame.decorations[0].tuning.light_receivers_set);
  assert(frame.decorations[0].tuning.dim_receivers==5 && frame.decorations[0].tuning.dim_receivers_set);
  assert(ActionEffectReceivers_HasActors(&frame,true));
  assert(ActionEffectReceivers_HasActors(&frame,false)); /* Dimming is independent. */
  Parse("[effects]\nversion=1\n[source:01:02:0:cave-light:1]\nlight-scenery=1\nlight-player=0\nlight-enemies=0\n"
      "dim-scenery=1\ndim-player=0\ndim-enemies=0\n");
  assert(!ActionEffectReceivers_HasActors(&frame,true));

  const ActionSurfaceField *f=ActionSurfaceField_Bundled(4);
  frame=(ActionSceneEffectFrame){.decoration_count=1};
  frame.decorations[0]=(ActionEffectInstance){.generation=1,.kind=kActionEffect_AitosWaterfallMist,
      .phase=kActionEffectPhase_AitosWaterfallMist,.pulse_ticks=37,
      .flags=kActionEffectFlag_Visible,.render_layer=kActionEffectRenderLayer_Bg2Alpha,
      .projection_plane=kActionEffectProjectionPlane_Bg2,
      .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={f->Bounds[0],f->Bounds[1],f->Bounds[2],f->Bounds[3]}}};
  const unsigned layer=frame.decorations[0].render_layer;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,false,Project,NULL,NULL,&a));assert(a.vertex_count);
  Parse("[effects]\nversion=1\n[source:01:02:0:waterfall-mist:1]\nwidth-scale=1.5\nlength-scale=2\n");
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,false,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&a.index_count==b.index_count);
  for(int i=0;i<a.vertex_count;++i) {
    assert(fabsf(b.vertices[i].position.x-a.vertices[i].position.x*1.5f)<.001f);
    assert(fabsf(b.vertices[i].position.y-a.vertices[i].position.y*2)<.001f);
  }
  puts("Source overrides: inherited receivers survive tint edits; waterfall mist resizes real clouds with unchanged geometry counts.");
}
static void NativeMemberReceivers(void) {
  ActionEffectProjectionContext context={.visible_width=768,.snes_height=544,
      .capture_height=544,.viewport={0,0,768,544},.scenery_lighting_only=true};
  for(unsigned castle=0;castle<2;++castle)for(unsigned mask=0;mask<8;++mask) {
    frame=(ActionSceneEffectFrame){0};
    const unsigned group=castle?2:1,room=castle?8:1;
    const char *name=castle?"castle-light":"forest-forward";
    if(castle) {
      frame.castle_field=*ActionCastleField_Bundled(room);frame.castle_field_valid=true;
      frame.castle_field.SourceCount[0]=1;
      frame.decoration_count=1;
      frame.decorations[0]=(ActionEffectInstance){.generation=1,.kind=kActionEffect_CastleLight,
          .phase=kActionEffectPhase_CastleEnvironment,.environment_room=8,.source_mask=1,
          .flags=kActionEffectFlag_Visible,.render_layer=kActionEffectRenderLayer_Bg1Plane,
          .projection_plane=kActionEffectProjectionPlane_Bg1,
          .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={0,0,768,544}}};
    }else{
      ActionRayField forest=*ActionRayField_Bundled();
      memset(forest.dimensions,0,sizeof(forest.dimensions));forest.witness_count=0;
      forest.ray_count=1;forest.rays[0].x=240;forest.components=8;
      const ActionEnvironmentScene scene={.group=1,.room=1,.clock=37};
      ActionRayField_Capture(&scene,&frame,&forest,1);
    }
    char text[512];unsigned line;
    snprintf(text,sizeof(text),"[effects]\nversion=1\n[source:%02x:%02x:0:%s:1]\n"
        "light-scenery=0\nlight-player=0\nlight-enemies=0\n"
        "[member:%02x:%02x:0:%s:1]\nlight-scenery=%u\nlight-player=%u\nlight-enemies=%u\n",
        group,room,name,group,room,name,!!(mask&1),!!(mask&2),!!(mask&4));
    assert(ActionEffectRecipes_Parse(&recipes,text,strlen(text),&line));
    ActionEffectRecipes_Apply(&recipes,group,room,0,37,NULL,&frame);
    assert(frame.members.count==1&&frame.members.records[0].light_receivers==mask);
    assert(ActionEffectReceivers_Prepare(&frame,group,room,0,0,0,0,true,&receiver));
    assert(!!receiver.player.vertex_count==!!(mask&2));
    assert(!!receiver.enemies.vertex_count==!!(mask&4));
    assert(!!SourceBuild(castle?kActionEffectRenderLayer_Bg1Plane:kActionEffectRenderLayer_Bg1Light,
        true,false,&context)==!!(mask&1));
    if(mask==6)assert(receiver.player.vertex_count==receiver.enemies.vertex_count&&
        !memcmp(receiver.player.vertices,receiver.enemies.vertices,receiver.player.vertex_count*sizeof(ArRenderVertex2D)));
    /* Turning a member off still defeats an enabled parent for that member. */
    frame.decorations[0].tuning.light_receivers=7;
    assert(ActionEffectReceivers_Prepare(&frame,group,room,0,0,0,0,true,&receiver));
    assert(!!receiver.player.vertex_count==!!(mask&2));
    assert(!!receiver.enemies.vertex_count==!!(mask&4));
  }
  puts("Native rays/windows: all eight member receiver masks override/inherit parent routing on CPU and GPU.");
}
int main(int argc, char **argv) {
  NativeSourceOverrides();
  NativeMemberReceivers();
  BillboardExtents();
  SoftLightScroll();
  NativeDaylightReceivers();
  LightResponseAudit();
  HighIntensityGeometry();
  SurfaceLightingSplit();
  CastleSkyDepth();
  DeferredAuthored();TorchShadows();DeferredParticleArea();
  LayerAnchors();
  Geometry();GroundDust();MovingGlints();HeatAndParticlePaths();HaloAndGradient();
  Receivers();TorchReceivers();ReceiverIllumination();ExposureBoxes();ExposureMeshScroll();ExposureGroupCapacity();
  Events();
  MovingLightReceivers();
  NativeMembers();
  EditedOcclusion();
  puts("Authored fields: 16 CPU/source families, clipping, bounded areas, viewport-stable particles, reverse seeking, "
       "receiver separation, moving lights, glow centre and 19 preview families passed.");
  if (argc == 2 && !strcmp(argv[1], "--benchmark")) {BenchmarkReceivers();BenchmarkExposureMesh();}
  return 0;
}
