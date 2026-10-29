#include "action/action_effect_recipes.h"
#include "action/action_effect_masks.h"
#include "action/action_heat.h"
#include "action/action_light_kinds.h"
#include "action/action_effect_receivers.h"
#include "action/action_effect_preview.h"
#include "action/action_environment_exposure.h"
#include "action/action_exposure.h"
#include "action/action_storm.h"
#include "action/action_storm_ground.h"
#include "render/scenery_dimming.h"
#include "render/mist_rainbow.h"
#include "render/scene3d_math.h"
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
  const DioramaHeat heat=ActionHeat_Prepare(&frame,kActionEffectProjectionPlane_Bg2,0,0,512,256);
  assert(DioramaHeat_Active(&heat));
  const ArRenderPointF centre={.5f,.5f},edge={0,.5f};
  const ArRenderPointF moved=DioramaHeat_Uv(&heat,centre),still=DioramaHeat_Uv(&heat,edge);
  assert(fabsf(moved.x-centre.x)>.00001f&&fabsf(moved.x-centre.x)<.01f);
  assert(still.x==edge.x&&still.y==edge.y);
  DioramaHeat overlap=heat;memcpy(overlap.bounds[1],heat.bounds[0],sizeof(heat.bounds[0]));memcpy(overlap.motion[1],heat.motion[0],sizeof(heat.motion[0]));
  const ArRenderPointF combined=DioramaHeat_Uv(&overlap,centre);
  assert(combined.x==moved.x&&combined.y==moved.y);
  const DioramaHeat disabled=ActionHeat_Prepare(NULL,kActionEffectProjectionPlane_Bg2,0,0,512,256);assert(!DioramaHeat_Active(&disabled));
  /* BG1 regions must not leak into BG2; scrolling changes only the capture
   * coordinates, not the phase or the world-space displacement. */
  Parse("[effects]\nversion=1\n[emitter:01:02:0:heat-haze:1]\nanchor=bg1\nx=256\ny=128\nwidth=512\nheight=256\namplitude=1.5\n");
  const DioramaHeat terrain=ActionHeat_Prepare(&frame,kActionEffectProjectionPlane_Bg1,0,0,512,256);
  const DioramaHeat background=ActionHeat_Prepare(&frame,kActionEffectProjectionPlane_Bg2,0,0,512,256);
  assert(DioramaHeat_Active(&terrain)&&!DioramaHeat_Active(&background));
  assert(!ActionEffectRecipes_NeedsBgMask(&recipes,1,2,0,0));
  assert(!ActionEffectRecipes_NeedsBgMask(&recipes,1,2,0,1));
  const DioramaHeat scrolled=ActionHeat_Prepare(&frame,kActionEffectProjectionPlane_Bg1,64,32,512,256);
  const ArRenderPointF shifted=DioramaHeat_Uv(&scrolled,(ArRenderPointF){.375f,.375f});
  assert(fabsf(shifted.x*512+64-moved.x*512)<.0001f);
  assert(fabsf(shifted.y*256+32-moved.y*256)<.0001f);
  const DioramaHeat outside=ActionHeat_Prepare(&frame,kActionEffectProjectionPlane_Bg1,512,0,512,256);
  assert(!DioramaHeat_Active(&outside));
  /* The source centre may be offscreen while its softened edge is visible. */
  const DioramaHeat edge_visible=ActionHeat_Prepare(&frame,kActionEffectProjectionPlane_Bg1,384,0,512,256);
  assert(DioramaHeat_Active(&edge_visible));
  unsigned line;const char *invalid="[effects]\nversion=1\n[emitter:01:02:0:heat-haze:1]\nanchor=bg2-raster\n";
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
    {"storm-lightning","lifetime=120\nactivity=.2\nseed=7\n"},{"soft-light",""},{"motes",""},{"free-mist",""},{"ground-mist","mist-height=26\n"},
    {"particle-area","particles=8\npattern=snow\n"},{"light-fan",""},{"water-surface",""},
    {"fireflies","particles=6\n"},{"drips",""},{"waterfall-spray",""},{"cloud-bank",""},{"foliage-shadow","angle=23\nseed=1234\n"},{"canopy-light","angle=23\nseed=1234\n"},
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
    if(frame.authored[0].kind==kActionEffect_AuthoredStorm) {
      for(unsigned tick=0;tick<120;++tick) {
        frame.authored[0].pulse_ticks=tick;
        assert(ActionSceneDecorationRender_Build(&frame,frame.authored[0].render_layer,true,true,Project,NULL,NULL,&a));
        if(a.vertex_count)break;
      }
    }
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
    if(ActionEffectRecipes_IsEmitter(kind) && kind!=kActionEffect_AuthoredHeat && kind!=kActionEffect_AuthoredCloudLight && !covered[kind]) {
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
static void CanopyFields(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:foliage-shadow:123]\n"
        "x=128\ny=96\nwidth=320\nheight=192\nstrands=4\namplitude=4\nseed=99\n");
  const unsigned layer=frame.authored[0].render_layer;
  assert(frame.authored[0].tuning.intensity==.5f);
  const ActionEffectLocalRect full={-512,-512,1024,1024};
  for (unsigned angle=0;angle<2;++angle) {
    const float radians=angle*.7f,cs=cosf(radians),sn=sinf(radians);
    frame.authored[0].field_style.angle=radians*57.2957795f;
    for(unsigned tick=0;tick<960;tick+=37) {
      frame.authored[0].pulse_ticks=tick;
      assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,Clip,(void *)&full,&a));
      assert(a.vertex_count>0&&a.vertex_count<=4*336);Valid(&a);
      /* Sway/rotation may not push a stencil into the neighbouring ruin.
       * Compare in the emitter's own axes, independent of camera clipping. */
      for(int i=0;i<a.vertex_count;++i) {
        const float x=a.vertices[i].position.x-128,y=a.vertices[i].position.y-96;
        assert(fabsf(cs*x-sn*y)<=160.001f);
        assert(fabsf(sn*x+cs*y)<=96.001f);
      }
    }
  }
  frame.authored[0].field_style.angle=0;
  frame.authored[0].pulse_ticks=37;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,Clip,(void *)&full,&a));
  const ActionEffectLocalRect edge={-32,0,64,192}; /* Centre is outside. */
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,Clip,(void *)&edge,&b));
  assert(b.vertex_count>0);Valid(&b);
  unsigned shared=0;
  for(int i=0;i<b.vertex_count;++i)for(int j=0;j<a.vertex_count;++j)
    if(!memcmp(&b.vertices[i],&a.vertices[j],sizeof(a.vertices[j])))++shared;
  assert(shared); /* Cropping never reseeds/repositions the leaves. */
  frame.authored[0].particle_style.seed=100;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,Clip,(void *)&full,&b));
  assert(a.vertex_count==b.vertex_count&&memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(a.vertices[0])));

  Parse("[effects]\nversion=1\n[emitter:01:02:0:light-fan:1]\nx=128\ny=32\n");
  assert(frame.authored[0].field_style.shadows==1); /* Preserve existing fans. */
  frame.scenery=(ActionMoonlightOcclusion){.valid=true,.count=1,.rectangles={{0,48,256,256}}};
  ActionEffectProjectionContext context={.visible_width=512,.snes_height=448,
      .capture_height=448,.viewport={0,0,512,448}};
  const unsigned light_layer=ActionEffectReceivers_Layer(&frame.authored[0]);
  const unsigned count=SourceBuild(light_layer,true,false,&context);
  unsigned shadowed=0;for(unsigned i=0;i<count;++i)shadowed+=primitives[i].meta[3]!=0;
  assert(shadowed);
  Parse("[effects]\nversion=1\n[emitter:01:02:0:light-fan:1]\nx=128\ny=32\nshadows=0\n");
  assert(!frame.authored[0].field_style.shadows);
  assert(ActionSceneDecorationRender_Build(&frame,light_layer,true,false,Project,NULL,NULL,&a));
  frame.scenery=(ActionMoonlightOcclusion){0};
  assert(ActionSceneDecorationRender_Build(&frame,light_layer,true,false,Project,NULL,NULL,&b));
  assert(a.vertex_count>0&&a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(a.vertices[0])));
  assert(SourceBuild(light_layer,true,false,&context)==count);
  for(unsigned i=0;i<count;++i)assert(!primitives[i].meta[3]);
  puts("Canopy fields: bounded swaying silhouettes, stable offscreen crops, editable seeds and CPU/GPU fan occlusion controls passed.");
}
static void TexturedCanopy(void) {
  uint8_t map[256]={0};
  ActionEnvironmentScene scene={.maps={{map,sizeof(map),256,256,1}},.collision={[1]=15}};
  Parse("[effects]\nversion=1\n[emitter:01:02:0:canopy-light:1]\nx=64\ny=96\nwidth=128\nheight=192\nangle=0\nseed=93\n");
  map[8*16+4]=1; /* Solid 16px roof at x=64, y=128. Other art has no collision. */
  ActionEffectRecipes_Apply(&recipes,1,2,0,37,&scene,&frame);
  const ActionEffectFloorField hits=frame.authored_floor[0];
  assert(hits.count==8);
  for(unsigned i=0;i<8;++i) {
    assert(fabsf(hits.spans[i].y-(i==4?128:192))<.01f);
    assert(hits.spans[i].height==(i==4?1:0));
  }
  scene.camera_x[0]=700;scene.camera_y[0]=300;
  ActionEffectRecipes_Apply(&recipes,1,2,0,37,&scene,&frame);
  assert(!memcmp(&hits,&frame.authored_floor[0],sizeof(hits))); /* No view-driven shadow change. */
  frame.authored[0].field_style.bounce=0;
  const unsigned layer=ActionEffectReceivers_Layer(&frame.authored[0]);
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,false,Project,NULL,NULL,&a));
  bool mapped=false;
  for(int i=0;i<a.vertex_count;++i) {
    mapped|=a.vertices[i].tex_coord.x>0;
  }
  assert(mapped);
  unsigned blocked=0;
  for(int i=0;i<a.index_count;i+=3) {
    const ArRenderPointF p=a.vertices[a.indices[i]].position,
        q=a.vertices[a.indices[i+1]].position,r=a.vertices[a.indices[i+2]].position;
    const float x=(p.x+q.x+r.x)/3;
    if(x>64&&x<80) {
      assert(fmaxf(p.y,fmaxf(q.y,r.y))<=128.01f);
      ++blocked;
    }
  }
  assert(blocked); /* A roof must stop illumination, not merely darken it. */
  ActionEffectProjectionContext projection={.visible_width=256,.snes_height=224,
      .capture_height=224,.viewport={0,0,256,224}};
  assert(SourceBuild(layer,true,false,&projection));
  bool source_uv=false;
  for(unsigned i=0;i<frame.authored_floor[0].count*2;++i)source_uv|=primitives[i].points[0][2]>0;
  assert(source_uv);
  frame.authored[0].field_style.shadows=0;
  ActionEnvironmentScene_CaptureCanopy(&scene,&frame.authored[0],&frame.authored_floor[0]);
  for(unsigned i=0;i<8;++i)assert(frame.authored_floor[0].spans[i].y==192);
  /* Angled fields must fade at their own edges even where those edges cut
   * across the middle of the atlas. Leaf proportions use one world scale. */
  for(int angle=-40;angle<=40;angle+=40) {
    frame.authored[0].field_style.angle=(float)angle;
    ActionEnvironmentScene_CaptureCanopy(&scene,&frame.authored[0],&frame.authored_floor[0]);
    assert(ActionSceneDecorationRender_Build(&frame,layer,true,false,Project,NULL,NULL,&a));
    const float lean=tanf(angle*.01745329252f);
    const ArRenderVertex2D *origin=&a.vertices[0],*p=&a.vertices[1],*q=&a.vertices[2];
    const float px=p->position.x-origin->position.x,py=p->position.y-origin->position.y;
    const float qx=q->position.x-origin->position.x,qy=q->position.y-origin->position.y;
    const float pu=(p->tex_coord.x-origin->tex_coord.x)*kActionMaskAtlasWidth,pv=(p->tex_coord.y-origin->tex_coord.y)*kActionMaskAtlasHeight;
    const float qu=(q->tex_coord.x-origin->tex_coord.x)*kActionMaskAtlasWidth,qv=(q->tex_coord.y-origin->tex_coord.y)*kActionMaskAtlasHeight;
    const float det=px*qy-py*qx;assert(fabsf(det)>.001f);
    const float ux=(pu*qy-qu*py)/det,uy=(qu*px-pu*qx)/det;
    const float vx=(pv*qy-qv*py)/det,vy=(qv*px-pv*qx)/det;
    const float scale2=ux*ux+vx*vx;
    assert(scale2>0&&fabsf(scale2-(uy*uy+vy*vy))<scale2*.001f);
    assert(fabsf(ux*uy+vx*vy)<scale2*.001f); /* A square stencil, even when turned. */
    unsigned edges=0,interior=0;
    for(int i=0;i<a.vertex_count;++i) {
      const ArRenderVertex2D *v=&a.vertices[i];
      const float opening_x=v->position.x-lean*v->position.y;
      if(fabsf(opening_x)<.001f||fabsf(opening_x-128)<.001f) {
        assert(v->color.a<.00001f);++edges;
      } else if(v->color.a>.01f)++interior;
      const float dx=v->position.x-origin->position.x,dy=v->position.y-origin->position.y;
      const float du=(v->tex_coord.x-origin->tex_coord.x)*kActionMaskAtlasWidth,dv=(v->tex_coord.y-origin->tex_coord.y)*kActionMaskAtlasHeight;
      assert(fabsf(du-ux*dx-uy*dy)<.00512f);
      assert(fabsf(dv-vx*dx-vy*dy)<.00512f);
    }
    assert(edges&&interior);
  }
  /* Seeds differing by eight used to select the exact same stencil. Their
   * mappings must now vary without changing transport geometry or its budget. */
  uint64_t patterns[64];frame.authored[0].field_style.amplitude=0;
  for(unsigned seed=0;seed<64;++seed) {
    frame.authored[0].particle_style.seed=93+seed*8;
    assert(ActionSceneDecorationRender_Build(&frame,layer,true,false,Project,NULL,NULL,&a));
    if(!seed)b=a;
    assert(a.vertex_count==b.vertex_count&&a.index_count==b.index_count);
    uint64_t hash=UINT64_C(14695981039346656037);
    const int column=(int)(a.vertices[0].tex_coord.x*kActionMaskAtlasColumns),row=(int)(a.vertices[0].tex_coord.y*kActionMaskAtlasHeight/kActionMaskCellSize);
    for(int i=0;i<a.vertex_count;++i) {
      assert(!memcmp(&a.vertices[i].position,&b.vertices[i].position,sizeof(ArRenderPointF)));
      const ArRenderPointF uv=a.vertices[i].tex_coord;
      assert((int)(uv.x*kActionMaskAtlasColumns)==column&&
          (int)(uv.y*kActionMaskAtlasHeight/kActionMaskCellSize)==row&&row==1);
      const uint8_t *bytes=(const uint8_t *)&a.vertices[i].tex_coord;
      for(size_t j=0;j<sizeof(ArRenderPointF);++j){hash^=bytes[j];hash*=UINT64_C(1099511628211);}
    }
    for(unsigned i=0;i<seed;++i)assert(hash!=patterns[i]);
    patterns[seed]=hash;
  }
  assert(ActionEffectMasks_Sample(0,0)==1);
  /* Tapered canopy contacts can have angled ends. Their general clipping
   * bound and the tighter rectangular fog bound must fit the room budget. */
  const ActionEffectLocalRect crop={0,0,64,64};
  for(int lean=-2;lean<=2;++lean)for(int y=-180;y<80;y+=20)for(int x=-300;x<120;x+=20) {
    const ArRenderVertex2D quad[4]={
      {.position={x,y}}, {.position={x+80,y}},
      {.position={x+80+lean*160,y+160}}, {.position={x+lean*160,y+160}}};
    ActionEffectGeometryWriter writer={.vertices=a.vertices,.indices=a.indices,
      .vertex_capacity=kActionSceneEffectRenderMaxVertices,.index_capacity=kActionSceneEffectRenderMaxIndices};
    assert(AppendSceneClippedQuad(&writer,&frame.authored[0],quad,&crop,Project,NULL));
    assert(writer.vertex_count<=(lean?10:8));
    ArRenderVertex2D tapered[4];memcpy(tapered,quad,sizeof(tapered));
    tapered[2].position.x-=lean*53;tapered[2].position.y-=53;
    writer.vertex_count=writer.index_count=0;
    assert(AppendSceneClippedQuad(&writer,&frame.authored[0],tapered,&crop,Project,NULL));
    assert(writer.vertex_count<=14);
  }
  for(unsigned cell=1;cell<16;++cell) {
    const ArRenderPointF corner=ActionEffectMasks_Uv(cell,0,0);
    assert(ActionEffectMasks_Sample(corner.x,corner.y)==0);
    float total=0;
    for(unsigned y=16;y<112;y+=16)for(unsigned x=16;x<112;x+=16) {
      const ArRenderPointF uv=ActionEffectMasks_Uv(cell,x/127.f,y/127.f);
      const float expected=(ActionEffectMasks_Pixel((cell%kActionMaskAtlasColumns)*128+x,(cell/kActionMaskAtlasColumns)*128+y)>>24)/255.f;
      assert(fabsf(ActionEffectMasks_Sample(uv.x,uv.y)-expected)<.0001f);total+=expected;
    }
    assert(total>0);
  }
  puts("Textured canopy: 64 distinct seed mappings, square leaf proportions, atlas containment, collision stops and camera-stable shadows passed.");
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
  unsigned index;
  test_tile = (ActionEnvironmentTileEdit){.entry=0x0401,.replace=1,.band=2};
  assert(ActionEnvironmentScene_PaletteIndex(&scene,0,0,0,&index) && index==17);
  assert(!ActionEnvironmentScene_PaletteIndex(&scene,0,7,0,&index));
  test_tile.entry |= 0x4000;
  assert(ActionEnvironmentScene_PaletteIndex(&scene,0,7,0,&index) && index==17);
  test_tile.entry = 0x8401;
  assert(ActionEnvironmentScene_PaletteIndex(&scene,0,7,0,&index) && index==17);
  test_tile.transparent[0] = 1;
  assert(!ActionEnvironmentScene_PaletteIndex(&scene,0,7,0,&index));
  test_tile.transparent[0] = 0; test_tile.black[0] = 1;
  assert(!ActionEnvironmentScene_PaletteIndex(&scene,0,7,0,&index));
  test_tile.black[0] = 0; test_tile.band = 0;
  assert(!ActionEnvironmentScene_PaletteIndex(&scene,0,7,0,&index));
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
static void SwampLightingFixture(void) {
  char text[8192]="[effects]\nversion=1\n";
  for(unsigned bank=0;bank<3;++bank) {
    char record[640];
    snprintf(record,sizeof(record),"[emitter:01:02:0:cloud-bank:%x]\nx=%u\ny=128\n"
        "width=256\nheight=96\ntextured=1\nrolling=.8\ncanopy-response=.8\nstrands=12\n"
        "color=819b92\ncolor-end=425a60\nseed=%u\n",bank*5+1,128+bank*256,bank+31);
    strcat(text,record);
    for(unsigned source=0;source<4;++source) {
      snprintf(record,sizeof(record),"[emitter:01:02:0:fireflies:%x]\nx=%u\ny=128\n"
          "width=192\nheight=56\nparticles=%u\nglow-style=%s\nchorus=.7\ntrail=.8\n"
          "intensity=1.3\nsize-min=1\nsize-max=1.6\nwander=16\ntravel-x=24\ntravel-y=8\n"
          "seed=%u\n",bank*5+2+source,128+bank*256,source<2?8:source==2?2:3,
          source<2?"insects":source==2?"wisps":"moss",bank*71+source+93);
      strcat(text,record);
    }
  }
  frame=(ActionSceneEffectFrame){0};Parse(text);
  assert(frame.authored_count==15);
}
static void BenchmarkSwampLighting(void) {
  SwampLightingFixture();
  const unsigned mist_layer=frame.authored[0].render_layer;
  const unsigned glow_layer=frame.authored[1].render_layer;
  enum {iterations=1000};const clock_t start=clock();
  for(unsigned tick=0;tick<iterations;++tick) {
    for(unsigned i=0;i<frame.authored_count;++i)frame.authored[i].pulse_ticks=tick;
    assert(ActionSceneDecorationRender_Build(&frame,mist_layer,true,true,Project,NULL,NULL,&a));
    assert(ActionSceneDecorationRender_Build(&frame,glow_layer,true,true,Project,NULL,NULL,&b));
  }
  const double ms=(double)(clock()-start)*1000/CLOCKS_PER_SEC/iterations;
  printf("Swamp CPU stress: 3 rolling banks, 63 glow points, %d + %d vertices, %.3f ms/frame (%u iterations; no GPU).\n",
      a.vertex_count,b.vertex_count,ms,iterations);
}
static void MistLightingBounds(void) {
  SwampLightingFixture();
  const unsigned layer=frame.authored[0].render_layer;
  ActionEffectLocalRect crop={90,100,210,170};
  for(unsigned tick=37;tick<2000;tick+=391) {
    for(unsigned i=0;i<frame.authored_count;++i)frame.authored[i].pulse_ticks=tick;
    assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&a));
    assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,Clip,&crop,&b));
    unsigned matched=0;
    for(int i=0;i<a.vertex_count;++i) {
      const ArRenderVertex2D *v=&a.vertices[i];
      if(v->position.x<=crop.x0||v->position.x>=crop.x1||v->position.y<=crop.y0||v->position.y>=crop.y1)continue;
      bool found=false;for(int j=0;j<b.vertex_count;++j)if(!memcmp(v,&b.vertices[j],sizeof(*v)))found=true;
      assert(found);matched++;
    }
    assert(matched>8); /* Skipping offscreen curls cannot change shared lighting. */
  }
  /* A wide excursion leaves the emitter box while its visible glow still
   * lights the fog. The old fixed 64px influence cutoff rejected this. */
  Parse("[effects]\nversion=1\n"
      "[emitter:01:02:0:cloud-bank:1]\nx=0\ny=0\nwidth=16\nheight=16\nstrands=1\n"
      "textured=1\nrolling=.8\ncanopy-response=1\ncolor=304040\ncolor-end=304040\n"
      "[emitter:01:02:0:fireflies:2]\nx=0\ny=0\nwidth=4\nheight=4\nparticles=1\n"
      "glow-style=wisps\ntravel-x=512\ntravel-y=0\nwander=0\nspread=0\nsize-min=4\nsize-max=4\nlifetime=1000\n");
  ActionEffectInstance *mist=&frame.authored[0],*glow=&frame.authored[1];
  bool outside=false;
  for(unsigned tick=0;tick<1000;tick+=37) {
    glow->pulse_ticks=tick;
    assert(ActionSceneDecorationRender_Build(&frame,glow->render_layer,false,true,Project,NULL,NULL,&a));
    assert(a.vertex_count==4);
    const float x=(a.vertices[0].position.x+a.vertices[2].position.x)*.5f;
    if(fabsf(x)<=100)continue;
    mist->world_x=x;mist->world_y=(a.vertices[0].position.y+a.vertices[2].position.y)*.5f;
    outside=true;break;
  }
  assert(outside);
  assert(ActionSceneDecorationRender_Build(&frame,mist->render_layer,false,true,Project,NULL,NULL,&a));
  glow->flags|=kActionEffectFlag_LightingOff;
  assert(ActionSceneDecorationRender_Build(&frame,mist->render_layer,false,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&a.vertex_count);
  bool lit=false;for(int i=0;i<a.vertex_count;++i)if(a.vertices[i].color.r>b.vertices[i].color.r+.001f)lit=true;
  assert(lit);
  const ArRenderColorF lit_color=a.vertices[0].color;
  glow->flags&=(uint8_t)~kActionEffectFlag_LightingOff;
  glow->projection_plane=kActionEffectProjectionPlane_Obj;
  assert(ActionSceneDecorationRender_Build(&frame,mist->render_layer,false,true,Project,NULL,NULL,&a));
  assert(!memcmp(&a.vertices[0].color,&lit_color,sizeof(lit_color))); /* Actor-bound lights share BG1's world. */
  /* BG2 coordinates belong to a different moving plane, not this BG1 mist. */
  glow->projection_plane=kActionEffectProjectionPlane_Bg2;
  assert(ActionSceneDecorationRender_Build(&frame,mist->render_layer,false,true,Project,NULL,NULL,&a));
  assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));

  mist->geometry.data.rect=(ActionEffectLocalRect){-256,-96,256,96};
  mist->field_style.strands=24;mist->field_style.wake=1;frame.actor_count=2;
  for(unsigned i=0;i<2;++i)frame.actors[i]=(ActionEffectActor){.generation=i+1,.body_radius=8,.visible=1,.player=i==0,
      .wake={.x=mist->world_x,.y=mist->world_y+32,.dx=i?-2:2,.strength=1}};
  assert(ActionSceneDecorationRender_Build(&frame,mist->render_layer,false,true,Project,NULL,NULL,&a));Valid(&a);
  assert(a.vertex_count>=200&&a.vertex_count<=50*14); /* Maximum lobes and both wakes fit. */
  mist->field_style.strands=25;
  assert(!ActionSceneDecorationRender_Build(&frame,mist->render_layer,false,true,Project,NULL,NULL,&b));
  puts("Mist lighting: world-stable crops, wide glow travel, separate BG anchors and maximum bank/wake capacity passed.");
}
static void DropletEdges(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:drips:1]\nx=100\ny=100\nwidth=4\nheight=100\n"
      "particles=1\nsize-min=2\nsize-max=2\n");
  const unsigned layer=frame.authored[0].render_layer;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&a));
  assert(a.vertex_count==4);
  const float x=(a.vertices[0].position.x+a.vertices[2].position.x)*.5f;
  const float y=(a.vertices[0].position.y+a.vertices[2].position.y)*.5f;
  const ActionEffectLocalRect crops[]={{x+.5f,y-8,x+8,y+8},{x-8,y-8,x-.5f,y+8},
      {x-8,y+1.5f,x+8,y+8},{x-8,y-8,x+8,y-1.5f}};
  for(unsigned edge=0;edge<4;++edge) {
    assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,Clip,(void*)&crops[edge],&b));
    assert(b.vertex_count==4&&!memcmp(a.vertices,b.vertices,sizeof(*a.vertices)*4));
  }
  ActionEffectLocalRect outside={x+3,y-8,x+8,y+8};
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,Clip,&outside,&b));
  assert(!b.vertex_count);
  puts("Droplets: partial footprints survive all four viewport edges; fully hidden drops are culled.");
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
        if(mode==2) {
          /* Weather is rasterized into each destination source strip before
           * sky blur. Sampling that strip must restore the same rigid ray. */
          p.bg2_skybox.texture_offset=(ArRenderPointF){137,64};
          for(unsigned band=0;band<p.bg2_skybox.count;++band) {
            p.bg2_skybox.active_band=(int)band;
            p.bg2_skybox.to_texture=true;
            assert(ActionEffectProjection_ProjectPoint(&c,e,17,130,&second));
            const DioramaSkyboxBandProjection *b=&p.bg2_skybox.bands[band];
            const float x=(second.x-137-b->x0)/(b->x1-b->x0)*p.output_width+p.output_x;
            const float y=((second.y-64-b->y0)/(b->y1-b->y0)*(b->output_y1-b->output_y0)+b->output_y0)*p.output_height+p.output_y;
            assert(fabsf(x-first.x)<.001f&&fabsf(y-first.y)<.001f);
          }
          p.bg2_skybox.to_texture=false;
        }
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
static void CloudVolumes(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:cloud-bank:123]\nx=128\ny=120\n"
        "width=256\nheight=96\ntextured=1\nrolling=.8\nvolume=1\nstrands=4\n"
        "color=f4e7d1\ncolor-end=68748c\ndrift=-32\namplitude=6\nlifetime=1200\n");
  const unsigned layer=frame.authored[0].render_layer;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&a));
  assert(a.vertex_count==32); /* Same two masks per lobe as ordinary rolling mist. */
  for(unsigned clock=0;clock<2400;clock+=37) {
    frame.authored[0].pulse_ticks=clock;
    assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));
    Valid(&b);
    assert(a.vertex_count==b.vertex_count);
    for(int i=0;i<b.vertex_count;i+=8) {
      const ArRenderPointF *origin=&b.vertices[i].position;
      for(int j=0;j<4;++j) {
        const ArRenderVertex2D *v=&b.vertices[i+j];
        assert(v->position.x>=0&&v->position.x<=256&&v->position.y>=72&&v->position.y<=168);
        assert(!memcmp(&v->position,&b.vertices[i+j+4].position,sizeof(v->position)));
        assert(fabsf((v->position.x-origin->x)-
            (a.vertices[i+j].position.x-a.vertices[i].position.x))<.0001f);
        assert(fabsf((v->position.y-origin->y)-
            (a.vertices[i+j].position.y-a.vertices[i].position.y))<.0001f);
        assert(ActionEffectMasks_Sample(v->tex_coord.x,v->tex_coord.y)<.0001f);
      }
    }
  }
  frame.authored[0].pulse_ticks=37+1200;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));
  assert(!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  frame.authored[0].pulse_ticks=37;
  const ActionEffectLocalRect crop={0,72,110,168}; /* The emitter centre is outside. */
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,Clip,(void*)&crop,&b));
  assert(b.vertex_count);
  for(int i=0;i<a.vertex_count;++i) {
    const ArRenderVertex2D *v=&a.vertices[i];
    if(v->position.x>=crop.x1)continue;
    bool found=false;
    for(int j=0;j<b.vertex_count;++j)if(!memcmp(v,&b.vertices[j],sizeof(*v)))found=true;
    assert(found); /* A crop cannot restart motion or change the density mapping. */
  }
  for(unsigned variant=0;variant<kActionMaskCloudCount;++variant) {
    float upper=0,lower=0;unsigned occupied=0;
    for(unsigned y=0;y<128;++y)for(unsigned x=0;x<128;++x) {
      const unsigned column=variant*kActionMaskCellSize+x;
      const unsigned body=ActionEffectMasks_Pixel(column,
          kActionMaskCloudBodyFirst/kActionMaskAtlasColumns*kActionMaskCellSize+y)>>24;
      const unsigned light=ActionEffectMasks_Pixel(column,
          kActionMaskCloudLightFirst/kActionMaskAtlasColumns*kActionMaskCellSize+y)>>24;
      if(x==0||y==0||x==127||y==127)assert(!body&&!light);
      if(body+light-body*light/255>192) {++occupied;if(y<64)upper+=light;else lower+=light;}
    }
    assert(occupied>1500&&upper>lower); /* Solid volume and a distinct shaded underside. */
  }
  Parse("[effects]\nversion=1\n[emitter:01:02:0:cloud-bank:1]\nx=128\ny=120\n"
      "width=256\nheight=96\ntextured=1\nrolling=.8\nvolume=1\nstrands=4\n"
      "color=f4e7d1\ncolor-end=68748c\nanchor=bg2-point\nplacement=background\n"
      "[emitter:01:02:0:cloud-light:2]\nx=128\ny=120\nwidth=512\nheight=512\nintensity=4\n");
  assert(ActionSceneDecorationRender_Build(&frame,frame.authored[0].render_layer,
      false,true,Project,NULL,NULL,&a));
  assert(a.vertex_count==32);
  for(int i=0;i<a.vertex_count;i+=8)for(int j=0;j<4;++j)
    assert(a.vertices[i+j].color.r<.5f&&
        a.vertices[i+j+4].color.r>a.vertices[i+j].color.r*1.5f);
  puts("Cloud volumes: immutable shapes, aligned shading, bounded drift, repeat/seek and offscreen-centre crops passed.");
}
static void MistRainbows(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:cloud-bank:1]\nx=128\ny=128\n"
      "width=256\nheight=128\ntextured=1\nrolling=.8\nvolume=1\nrainbow=.8\nstrands=3\n"
      "[emitter:01:02:0:cloud-light:2]\nx=128\ny=200\nwidth=512\nheight=600\n"
      "angle=0\nstrands=5\nintensity=4\nfalloff=1\ncloud-cover=0\nseed=123\n");
  ActionEffectInstance *mist=&frame.authored[0],*sun=&frame.authored[1];
  const unsigned layer=mist->render_layer;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&a));Valid(&a);
  assert(a.vertex_count>24&&a.vertex_count<=24+(kActionMistRainbowColumns+1)*(kActionMistRainbowRows+1));
  float strongest=0;
  for(int i=24;i<a.vertex_count;++i)strongest=fmaxf(strongest,a.vertices[i].color.a);
  assert(strongest>.01f&&strongest<.45f);
  /* Different foreground/background scroll must still sample the same light
   * when their relative world coordinates are translated together. */
  const ActionEnvironmentScene cameras={.camera_x={128,192},.camera_y={64,96}};
  ActionEffectRecipes_Apply(&recipes,1,2,0,37,&cameras,&frame);
  assert(frame.bg1_to_bg2_x==64&&frame.bg1_to_bg2_y==32);
  sun->world_x+=64;sun->world_y+=32;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  sun->tuning.intensity=0;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(b.vertex_count==24); /* No sunlight, no spectrum or extra geometry. */
  sun->tuning.intensity=.5f;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  float weaker=0;
  for(int i=24;i<b.vertex_count;++i) {
    weaker=fmaxf(weaker,b.vertices[i].color.a);
    assert(b.vertices[i].position.x==a.vertices[i].position.x&&b.vertices[i].position.y==a.vertices[i].position.y);
  }
  assert(weaker>0&&weaker<strongest);
  sun->tuning.intensity=4;
  sun->field_style.cloud.sun_depth=-48;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  for(int i=24;i<b.vertex_count;++i)assert(b.vertices[i].color.a==0); /* Sun facing viewer: wrong scattering angle. */
  sun->field_style.cloud.sun_depth=48;
  const float streak=sun->field_style.cloud.streak;
  sun->field_style.cloud.streak=0;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(b.vertex_count==24); /* Ambient halo/reflected light cannot make a bow. */
  sun->field_style.cloud.streak=streak;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));assert(b.vertex_count==24);
  mist->field_style.rainbow=0;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));assert(b.vertex_count==24);
  assert(!memcmp(a.vertices,b.vertices,24*sizeof(*a.vertices))); /* Original mist is untouched. */
  mist->field_style.rainbow=.8f;
  const ActionEffectLocalRect crop={0,64,100,192};
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,Clip,(void*)&crop,&b));Valid(&b);
  for(int i=24;i<a.vertex_count;++i) {
    const ArRenderVertex2D *v=&a.vertices[i];if(v->position.x>=100)continue;
    bool found=false;
    for(int j=0;j<b.vertex_count;++j)if(!memcmp(v,&b.vertices[j],sizeof(*v)))found=true;
    assert(found); /* Cropping never shifts the spectrum or changes its light. */
  }
  mist->tuning.intensity=0;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(b.vertex_count==24); /* No droplets, no detached rainbow. */
  frame.bg1_to_bg2_x=frame.bg1_to_bg2_y=0;
  const RainbowDirection red=RainbowSpectrum(cosf(42*.01745329252f));
  const RainbowDirection blue=RainbowSpectrum(cosf(40*.01745329252f));
  assert(red.x>red.y&&red.y>red.z&&blue.z>blue.y&&blue.y>blue.x);
  assert(RainbowSpectrum(1).x==0&&RainbowSpectrum(-1).z==0);
  /* Camera/output changes alter optical colour, never the retained packet's
   * droplets or incident-light weights. GPU projection supplies the view. */
  mist->tuning.intensity=1;
  DioramaProjection view={.valid=true,.output_width=640,.output_height=360,
      .texture_width=512,.texture_height=352,.aspect_x=2,.height_scale=1.5f,
      .bg1_plane={.valid=true,.u1=1,.v1=1}};
  ActionEffectProjectionContext camera={.diorama_projection=&view,.ws_extra=128,.ws_extra_top=64};
  Scene3DCamera orbit={.distance=3.25f,.fov_y=45};
  Scene3D_BuildViewProjection(&orbit,640,360,view.matrix);
  unsigned packet_count=SourceBuild(layer,true,true,&camera);
  assert(packet_count>12&&packet_count<=128);
  ActionEffectSourcePrimitive saved[128];memcpy(saved,primitives,packet_count*sizeof(*saved));
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,
      ActionEffectProjection_ProjectPoint,ActionEffectProjection_ClipBounds,&camera,&a));
  orbit.tilt_x=20;
  Scene3D_BuildViewProjection(&orbit,640,360,view.matrix);
  assert(SourceBuild(layer,true,true,&camera)==packet_count);
  assert(!memcmp(saved,primitives,packet_count*sizeof(*saved)));
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,
      ActionEffectProjection_ProjectPoint,ActionEffectProjection_ClipBounds,&camera,&b));
  assert(a.vertex_count==b.vertex_count);
  bool color_changed=false;
  for(int i=24;i<a.vertex_count;++i)
    color_changed|=fabsf(a.vertices[i].color.a-b.vertices[i].color.a)>.001f;
  assert(color_changed);
  view.output_width=1280;view.output_height=720;
  Scene3D_BuildViewProjection(&orbit,1280,720,view.matrix);
  assert(SourceBuild(layer,true,true,&camera)==packet_count);
  assert(!memcmp(saved,primitives,packet_count*sizeof(*saved)));
  puts("Mist rainbows: direct light/density/angle gating, intensity response, spectral ordering, background scrolling and crops passed.");
}
static void MistSkyLighting(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:cloud-bank:1]\nx=128\ny=128\n"
      "width=256\nheight=128\ntextured=1\nrolling=.8\nvolume=1\nstrands=3\n"
      "color=8090a0\ncolor-end=304050\ncanopy-response=.8\n"
      "[emitter:01:02:0:cloud-light:2]\nx=128\ny=200\nwidth=512\nheight=600\n"
      "color=ffb070\nangle=0\nstrands=5\nintensity=4\nfalloff=1\ncloud-cover=0\nseed=123\n");
  ActionEffectInstance *mist=&frame.authored[0],*sun=&frame.authored[1];
  const unsigned layer=mist->render_layer;
  frame.bg1_to_bg2_x=frame.bg1_to_bg2_y=0;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&a));Valid(&a);
  assert(a.vertex_count==24);
  sun->tuning.intensity=0;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&a.index_count==b.index_count);
  float red_gain=0,blue_gain=0;
  for(int i=0;i<a.vertex_count;++i) {
    assert(!memcmp(&a.vertices[i].position,&b.vertices[i].position,sizeof(a.vertices[i].position)));
    assert(a.vertices[i].color.a==b.vertices[i].color.a);
    red_gain+=a.vertices[i].color.r-b.vertices[i].color.r;
    blue_gain+=a.vertices[i].color.b-b.vertices[i].color.b;
  }
  assert(red_gain>.1f&&blue_gain>0&&red_gain>blue_gain);
  sun->tuning.intensity=4;
  frame.bg1_to_bg2_x=64;frame.bg1_to_bg2_y=32;
  sun->world_x+=64;sun->world_y+=32;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  const ActionEffectLocalRect crop={64,64,192,192};
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,Clip,(void*)&crop,&b));Valid(&b);
  unsigned matched=0;
  for(int i=0;i<a.vertex_count;++i) {
    const ArRenderVertex2D *v=&a.vertices[i];
    if(v->position.x<=crop.x0||v->position.x>=crop.x1||v->position.y<=crop.y0||v->position.y>=crop.y1)continue;
    bool found=false;for(int j=0;j<b.vertex_count;++j)if(!memcmp(v,&b.vertices[j],sizeof(*v)))found=true;
    assert(found);++matched;
  }
  assert(matched>0);
  /* Opting out or disabling lighting restores unchanged mist, including alpha. */
  mist->field_style.canopy_response=0;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&a));
  sun->tuning.intensity=0;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  mist->field_style.canopy_response=.8f;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&a));
  sun->tuning.intensity=4;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  ActionEffectProjectionContext unlit={.atmosphere_unlit=true};
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,ActionEffectProjection_ProjectPoint,NULL,&unlit,&a));
  mist->field_style.canopy_response=0;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,ActionEffectProjection_ProjectPoint,NULL,&unlit,&b));
  assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  frame.bg1_to_bg2_x=frame.bg1_to_bg2_y=0;
  puts("Mist sky lighting: opt-in warm scattering, stable alpha/geometry, BG1/BG2 translation, crops and disable passed.");
}
static void WaterSparkles(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:particle-area:1]\nx=128\ny=128\n"
      "width=256\nheight=256\nanchor=bg2-point\nplacement=background\npattern=water-glints\n"
      "particles=16\ncanopy-response=1\ntravel-y=96\nwander=0\nlifetime=120\n"
      "[emitter:01:02:0:cloud-light:2]\nx=128\ny=200\nwidth=512\nheight=600\n"
      "color=ffb070\nangle=0\nstrands=5\nintensity=4\nfalloff=1\ncloud-cover=0\nseed=123\n");
  ActionEffectInstance *water=&frame.authored[0],*sun=&frame.authored[1];
  const unsigned layer=water->render_layer;
  assert(layer==kActionEffectRenderLayer_Bg2Plane);
  frame.bg1_to_bg2_x=frame.bg1_to_bg2_y=0;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));assert(b.vertex_count==0);
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&a));Valid(&a);
  assert(a.vertex_count>0);
  for(int i=0;i<a.vertex_count;++i)assert(a.vertices[i].color.r>a.vertices[i].color.b);
  sun->tuning.intensity=0;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));assert(b.vertex_count==0);
  sun->tuning.intensity=4;sun->field_style.cloud.streak=0;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));assert(b.vertex_count==0);
  sun->field_style.cloud.streak=1.7f;
  frame.bg1_to_bg2_x=1000;frame.bg1_to_bg2_y=700;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  water->projection_plane=kActionEffectProjectionPlane_Bg1;
  frame.bg1_to_bg2_x=64;frame.bg1_to_bg2_y=32;sun->world_x+=64;sun->world_y+=32;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  water->pulse_ticks+=20;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));Valid(&b);
  assert(a.vertex_count!=b.vertex_count||memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  water->pulse_ticks-=20;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  frame.bg1_to_bg2_x=frame.bg1_to_bg2_y=0;
  water->kind=kActionEffect_AuthoredParticlePath;
  water->field_style.point_count=2;water->field_style.activity=1;
  water->field_style.points[0].x=0;water->field_style.points[0].y=-64;
  water->field_style.points[1].x=0;water->field_style.points[1].y=64;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(b.vertex_count>0);
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));
  assert(b.vertex_count==0);

  Parse("[effects]\nversion=1\n[emitter:01:02:0:cloud-bank:1]\nx=128\ny=128\n"
      "width=256\nheight=128\ntextured=1\nrolling=.8\nvolume=1\nstrands=4\nsparkle=1\n"
      "[emitter:01:02:0:cloud-light:2]\nx=128\ny=200\nwidth=512\nheight=600\n"
      "color=ffb070\nangle=0\nstrands=5\nintensity=4\nfalloff=1\ncloud-cover=0\nseed=123\n");
  ActionEffectInstance *mist=&frame.authored[0];sun=&frame.authored[1];
  const unsigned mist_layer=mist->render_layer;
  assert(ActionSceneDecorationRender_Build(&frame,mist_layer,true,true,Project,NULL,NULL,&a));Valid(&a);
  assert(a.vertex_count>32&&a.vertex_count<=32+4*4*kActionMistSparklesPerStrand);
  sun->tuning.intensity=0;
  assert(ActionSceneDecorationRender_Build(&frame,mist_layer,true,true,Project,NULL,NULL,&b));assert(b.vertex_count==32);
  sun->tuning.intensity=4;sun->world_x+=64;sun->world_y+=32;
  frame.bg1_to_bg2_x=64;frame.bg1_to_bg2_y=32;
  assert(ActionSceneDecorationRender_Build(&frame,mist_layer,true,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  const ActionEffectLocalRect crop={96,64,192,192};
  assert(ActionSceneDecorationRender_Build(&frame,mist_layer,true,true,Project,Clip,(void*)&crop,&b));Valid(&b);
  unsigned matched=0;
  for(int i=32;i<a.vertex_count;++i) {
    const ArRenderVertex2D *v=&a.vertices[i];
    if(v->position.x<=crop.x0||v->position.x>=crop.x1||v->position.y<=crop.y0||v->position.y>=crop.y1)continue;
    bool found=false;for(int j=0;j<b.vertex_count;++j)if(!memcmp(v,&b.vertices[j],sizeof(*v)))found=true;
    assert(found);++matched;
  }
  assert(matched>0);
  assert(ActionSceneDecorationRender_Build(&frame,mist_layer,false,true,Project,NULL,NULL,&b));assert(b.vertex_count==32);
  mist->field_style.sparkle=0;
  assert(ActionSceneDecorationRender_Build(&frame,mist_layer,true,true,Project,NULL,NULL,&b));assert(b.vertex_count==32);
  mist->field_style.sparkle=1;mist->tuning.intensity=0;
  assert(ActionSceneDecorationRender_Build(&frame,mist_layer,true,true,Project,NULL,NULL,&b));assert(b.vertex_count<=32);
  frame.bg1_to_bg2_x=frame.bg1_to_bg2_y=0;
  puts("Water sparkles: sunlight and mist-density gating, warm tint, layer anchoring, crops, clock seeking and disable passed.");
}
static void RollingMistFields(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:cloud-bank:123]\nx=128\ny=120\n"
        "width=256\nheight=96\ntextured=1\nrolling=.8\nwake=.8\ncanopy-response=.8\nstrands=4\n");
  const unsigned layer=frame.authored[0].render_layer;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&a));Valid(&a);
  const int baseline=a.vertex_count;
  assert(baseline==32); /* Two four-corner masks per roll, no extra pass. */
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));
  assert(!memcmp(a.vertices,b.vertices,baseline*sizeof(*a.vertices)));
  frame.actor_count=2;
  for(unsigned i=0;i<2;++i)frame.actors[i]=(ActionEffectActor){.generation=i+1,.body_radius=8,.visible=1,.player=i==0,
    .wake={.x=90+i*70,.y=140,.dx=i?-2:2,.strength=1}};
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));Valid(&b);
  assert(b.vertex_count==baseline+8);
  frame.actors[0].wake.age=frame.actors[1].wake.age=90;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));assert(b.vertex_count==baseline);
  frame.actors[0].wake.age=0;frame.actors[0].wake.y=50;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));assert(b.vertex_count==baseline); /* Above the fog. */
  for(unsigned i=0;i<2;++i)frame.actors[i].wake.age=90;
  /* A partial crop retains the exact original world positions/clock. */
  ActionEffectLocalRect crop={100,72,256,168};
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,Clip,&crop,&b));Valid(&b);
  for(int i=0;i<b.vertex_count;++i)assert(b.vertices[i].position.x>=100&&b.vertices[i].position.x<=256);
  frame.authored[0].pulse_ticks=frame.authored[0].particle_lifetime+37;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));
  assert(b.vertex_count==baseline&&!memcmp(a.vertices,b.vertices,baseline*sizeof(*a.vertices))); /* Loop is continuous. */
  /* Light sampling uses the rendered mask, shadow geometry and diagonal. */
  Parse("[effects]\nversion=1\n[emitter:01:02:0:canopy-light:1]\nx=64\ny=96\nwidth=128\nheight=192\nangle=19\nseed=93\nbounce=0\n");
  frame.light_receiver=kActionReceiver_Scenery;
  frame.authored_floor[0].count=8;
  for(unsigned i=0;i<8;++i)frame.authored_floor[0].spans[i]=(ActionEffectFloorSpan){.x0=i*16,.x1=(i+1)*16,.y=i==4?128:192};
  assert(ActionSceneDecorationRender_Build(&frame,frame.authored[0].render_layer,true,false,Project,NULL,NULL,&a));
  for(int i=0;i<a.index_count;i+=3) {
    float x=0,y=0,u=0,v=0,opacity=0;
    for(unsigned j=0;j<3;++j){const ArRenderVertex2D *p=&a.vertices[a.indices[i+j]];
      x+=p->position.x/3;y+=p->position.y/3;u+=p->tex_coord.x/3;v+=p->tex_coord.y/3;opacity+=p->color.a/3;}
    const float expected=opacity*ActionEffectMasks_Sample(u,v)*ActionLightKind_Illumination(kActionEffect_AuthoredCanopy);
    const float actual=ActionCanopy_Sample(&frame.authored[0],&frame.authored_floor[0],x,y);
    assert(fabsf(actual-expected)<.0001f);
  }
  frame.light_receiver=0;
  Parse("[effects]\nversion=1\n[emitter:01:02:0:cloud-bank:9]\nx=0\ny=0\nwidth=256\nheight=96\ntextured=1\ndrift=120\namplitude=32\nstrands=24\n");
  for(unsigned clock=0;clock<1000;clock+=37) {
    frame.authored[0].pulse_ticks=clock;
    assert(ActionSceneDecorationRender_Build(&frame,frame.authored[0].render_layer,false,true,Project,NULL,NULL,&a));
    for(int i=0;i<a.vertex_count;++i) {
      assert(fabsf(a.vertices[i].position.x)<=128.001f&&fabsf(a.vertices[i].position.y)<=48.001f);
      /* Whole masks fit without an authoring rectangle cutting their edges. */
      assert(ActionEffectMasks_Sample(a.vertices[i].tex_coord.x,a.vertices[i].tex_coord.y)<.0001f);
    }
  }
  frame.authored[0].particle_style.seed++;
  assert(ActionSceneDecorationRender_Build(&frame,frame.authored[0].render_layer,false,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  puts("Rolling mist: bounded banks and wakes, above-fog exclusion, clock/crop stability and exact canopy-mask sampling passed.");
}

static void SwampLifeFields(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:fireflies:123]\nx=128\ny=100\nwidth=96\nheight=48\n"
      "glow-style=wisps\ntrail=.85\nparticles=2\nwander=20\ntravel-x=28\ntravel-y=10\nsize-min=1.6\nsize-max=2.1\n");
  const unsigned layer=frame.authored[0].render_layer;
  frame.authored[0].pulse_ticks=241;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&a));Valid(&a);
  assert(a.vertex_count==32); /* Two heads and six tail masks. */
  frame.authored[0].pulse_ticks=970;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));
  assert(memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  frame.authored[0].pulse_ticks=241;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  const ActionEffectLocalRect crop={96,80,170,150};
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,Clip,(void*)&crop,&b));
  for(int i=0;i<a.vertex_count;++i) {
    const ArRenderVertex2D *v=&a.vertices[i];
    if(v->position.x<=crop.x0||v->position.x>=crop.x1||v->position.y<=crop.y0||v->position.y>=crop.y1)continue;
    bool found=false;for(int j=0;j<b.vertex_count;++j)if(!memcmp(v,&b.vertices[j],sizeof(*v)))found=true;
    assert(found);
  }
  frame.authored[0].tuning.light_receivers=0;
  assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));
  assert(!receiver.player.vertex_count&&!receiver.enemies.vertex_count);
  frame.authored[0].tuning.light_receivers=7;
  assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));
  assert(receiver.player.vertex_count&&receiver.enemies.vertex_count);
  ActionEffectProjectionContext context={.visible_width=256,.snes_height=224,.capture_height=224,.viewport={0,0,256,224}};
  assert(SourceBuild(layer,false,true,&context)==16);

  frame.authored[0].field_style.glow_style=kActionGlow_Moss;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&a));
  frame.authored[0].pulse_ticks=1741;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==8&&b.vertex_count==8);
  for(int i=0;i<a.vertex_count;++i)assert(!memcmp(&a.vertices[i].position,&b.vertices[i].position,sizeof(ArRenderPointF)));
  assert(memcmp(&a.vertices[0].color,&b.vertices[0].color,sizeof(ArRenderColorF)));
  for(unsigned cell=kActionMaskMossFirst;cell<kActionMaskMossFirst+kActionMaskMossCount;++cell) {
    unsigned dark=0,lit=0;
    for(unsigned y=24;y<104;y+=4)for(unsigned x=24;x<104;x+=4) {
      const ArRenderPointF uv=ActionEffectMasks_Uv(cell,x/127.f,y/127.f);
      const float alpha=ActionEffectMasks_Sample(uv.x,uv.y);
      if(alpha<.05f)dark++;if(alpha>.25f)lit++;
    }
    assert(dark>30&&lit>10); /* Porous vegetation, never a solid glow disk. */
  }
  Parse("[effects]\nversion=1\n[emitter:01:02:0:fireflies:123]\nparticles=6\nchorus=.8\n");
  bool answers=false;float peaks[6]={0};unsigned peak_ticks[6]={0};
  for(unsigned tick=0;tick<2400;tick+=71) {
    frame.authored[0].pulse_ticks=tick;frame.authored[0].field_style.chorus=.8f;
    assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&a));
    frame.authored[0].field_style.chorus=0;
    assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));
    for(int i=0;i<a.vertex_count;++i)assert(!memcmp(&a.vertices[i].position,&b.vertices[i].position,sizeof(ArRenderPointF)));
    if(memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)))answers=true;
    for(unsigned i=0;i<6;++i)if(a.vertices[i*4].color.a>peaks[i]){peaks[i]=a.vertices[i*4].color.a;peak_ticks[i]=tick;}
  }
  assert(answers);
  unsigned different=0;for(unsigned i=1;i<6;++i)different+=peak_ticks[i]!=peak_ticks[0];
  assert(different>=3); /* A conversation must not become a synchronized flash. */
  Parse("[effects]\nversion=1\n[emitter:01:02:0:drips:123]\nx=128\ny=100\nwidth=4\nheight=96\n"
      "particles=1\nactivity=.3\nripple=1\nlifetime=420\nsize-min=.5\nsize-max=.7\n");
  unsigned drops=0,ripples=0,quiet=0;int last_ripple=-1;
  for(unsigned tick=0;tick<840;++tick) {
    frame.authored[0].pulse_ticks=tick;
    assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&a));Valid(&a);
    if(!a.vertex_count){quiet++;continue;}
    assert(a.vertex_count==4);
    if(a.vertices[0].tex_coord.x>0) {
      ripples++;last_ripple=(int)tick;
      float mean=0;for(int i=0;i<4;++i)mean+=a.vertices[i].position.y*.25f;
      assert(fabsf(mean-148)<.001f); /* Landing never follows the falling drop or camera. */
    } else drops++;
  }
  assert(drops&&ripples&&quiet>500&&last_ripple>=0);
  frame.authored[0].pulse_ticks=(unsigned)last_ripple;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&a));
  frame.authored[0].pulse_ticks=17;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));
  frame.authored[0].pulse_ticks=(unsigned)last_ripple;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  puts("Swamp life: deterministic world-anchored trails, stationary porous moss, loose replies and intermittent water landings passed.");
}

static void MagicalSwampFields(void) {
  Parse("[effects]\nversion=1\n[emitter:01:02:0:fireflies:123]\nx=64\ny=64\nwidth=128\nheight=64\n"
      "particles=6\nwander=16\ntravel-x=12\ntravel-y=0\nsize-min=.7\nsize-max=1.2\nseed=4141\n"
      "light-scenery=1\nlight-player=1\nlight-enemies=1\n");
  const unsigned layer=frame.authored[0].render_layer;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&a));Valid(&a);
  assert(a.vertex_count==24); /* One feathered core/halo quad per insect. */
  for(unsigned tick=0;tick<2048;tick+=113) {
    frame.authored[0].pulse_ticks=tick;
    ActionEffectLocalRect crop={32,32,128,96};
    assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&a));
    assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,Clip,&crop,&b));Valid(&b);
    /* Interior source corners cannot slide as the view visits a subset. */
    for(int i=0;i<a.vertex_count;++i) {
      const ArRenderVertex2D *v=&a.vertices[i];
      if(v->position.x<=crop.x0||v->position.x>=crop.x1||v->position.y<=crop.y0||v->position.y>=crop.y1)continue;
      bool found=false;for(int j=0;j<b.vertex_count;++j)
        if(!memcmp(v,&b.vertices[j],sizeof(*v)))found=true;
      assert(found);
    }
  }
  frame.authored[0].pulse_ticks=37;
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&a));
  for(unsigned mask=0;mask<8;++mask) {
    frame.authored[0].tuning.light_receivers=mask;
    assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));
    assert(!!receiver.player.vertex_count==!!(mask&2));
    assert(!!receiver.enemies.vertex_count==!!(mask&4));
    assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));
    assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  }
  ActionEffectProjectionContext context={.visible_width=256,.snes_height=224,.capture_height=224,.viewport={0,0,256,224}};
  assert(SourceBuild(layer,false,true,&context)==12); /* No additional pass or per-insect draw. */
  uint8_t map[256]={0};
  ActionEnvironmentScene scene={.maps={{map,sizeof(map),256,256,1}},.collision={[1]=15}};
  for(unsigned x=0;x<16;++x)map[8*16+x]=1;
  Parse("[effects]\nversion=1\n[emitter:01:02:0:foliage-shadow:1]\nx=64\ny=96\nwidth=128\nheight=192\n"
      "textured=1\nshadows=1\nplacement=playfield\nstrands=2\nseed=93\n");
  ActionEffectRecipes_Apply(&recipes,1,2,0,37,&scene,&frame);
  const ActionEffectFloorField field=frame.authored_floor[0];assert(field.count==8);
  for(unsigned tick=0;tick<1000;tick+=71) {
    frame.authored[0].pulse_ticks=tick;
    assert(ActionSceneDecorationRender_Build(&frame,frame.authored[0].render_layer,false,true,Project,NULL,NULL,&a));
    assert(a.vertex_count);Valid(&a);
    for(int i=0;i<a.vertex_count;++i)assert(a.vertices[i].position.y<=128.001f);
  }
  scene.camera_x[0]=900;scene.camera_y[0]=300;
  ActionEffectRecipes_Apply(&recipes,1,2,0,37,&scene,&frame);
  assert(!memcmp(&field,&frame.authored_floor[0],sizeof(field)));
  Parse("[effects]\nversion=1\n[emitter:01:02:0:particle-path:1]\nx=64\ny=64\nwidth=64\nheight=4\n"
      "pattern=glints\npoints=-32,0 32,0\nparticles=4\nwander=0\ncanopy-response=1\n");
  assert(ActionSceneDecorationRender_Build(&frame,frame.authored[0].render_layer,false,true,Project,NULL,NULL,&a));
  for(int i=0;i<a.vertex_count;++i)assert(a.vertices[i].color.a==0); /* No light, no wet reflection. */
  Parse("[effects]\nversion=1\n[emitter:01:02:0:particle-area:1]\nx=128\ny=128\nwidth=256\nheight=256\n"
      "pattern=leaves\ntextured=1\nparticles=4\ntravel-x=0\ntravel-y=0\nwander=0\nseed=19\n");
  assert(ActionSceneDecorationRender_Build(&frame,frame.authored[0].render_layer,false,true,Project,NULL,NULL,&a));
  assert(a.vertex_count);Valid(&a);
  for(int i=0;i<a.vertex_count;++i)assert(ActionEffectMasks_Sample(a.vertices[i].tex_coord.x,a.vertices[i].tex_coord.y)<.001f);
  puts("Magical swamp: bounded glowing clusters, all receiver masks, world-stable crops, terrain-clipped vines and light-dependent glints passed.");
}

static void CloudBreakFans(void) {
  frame=(ActionSceneEffectFrame){0};
  Parse("[effects]\nversion=1\n[emitter:01:02:0:light-fan:123]\nx=128\ny=48\n"
      "width=64\nheight=96\nanchor=bg2-point\nplacement=background\nshadows=0\n"
      "lifetime=600\ntwinkle=.8\nlight-scenery=0\nlight-player=0\nlight-enemies=0\n");
  ActionEffectInstance *e=&frame.authored[0];
  const unsigned layer=e->render_layer;
  assert(layer==kActionEffectRenderLayer_Bg2Alpha);
  e->pulse_ticks=100;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&a));Valid(&a);
  e->pulse_ticks=400;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));Valid(&b);
  assert(a.vertex_count&&a.vertex_count==b.vertex_count&&a.index_count==b.index_count);
  bool changed=false;
  for(unsigned i=0;i<(unsigned)a.vertex_count;++i) {
    assert(!memcmp(&a.vertices[i].position,&b.vertices[i].position,sizeof(ArRenderPointF)));
    changed|=fabsf(a.vertices[i].color.a-b.vertices[i].color.a)>.01f;
  }
  assert(changed); /* Brightness varies without sliding/rotating the fan. */
  e->pulse_ticks=100;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  e->field_style.twinkle=0;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&a));
  e->pulse_ticks=400;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  ActionEffectProjectionContext view={.visible_width=512,.snes_height=256,.capture_height=256,.viewport={0,0,512,256}};
  assert(SourceBuild(layer,true,true,&view)>0);
  e->world_x=900;
  assert(!SourceBuild(layer,true,true,&view));
  e->world_x=128;e->field_style.strands=1;e->field_style.fan=0;e->field_style.softness=0;
  e->field_style.fade_in=.25f;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&a));
  bool source=false,interior=false;
  for(unsigned i=0;i<(unsigned)a.vertex_count;++i) {
    if(a.vertices[i].position.y==e->world_y) {source=true;assert(a.vertices[i].color.a==0);}
    interior|=a.vertices[i].color.a>0;
  }
  assert(source&&interior);
  e->field_style.fade_in=0;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  source=false;
  for(unsigned i=0;i<(unsigned)b.vertex_count;++i)
    source|=b.vertices[i].position.y==e->world_y&&b.vertices[i].color.a>0;
  assert(source); /* Window-style sources retain their sharp attachment. */
  puts("Cloud-break fans: animated transmission, fixed geometry, reverse seeking, source feathering, legacy steady fans and culling passed.");
}

static void StormGround(void) {
  uint16_t vram[32768] = {0};
  uint8_t map[256] = {0}, definitions[2048] = {0};
  for (unsigned i = 6 * 16; i < sizeof(map); ++i) map[i] = 1;
  for (unsigned q = 0; q < 4; ++q) {
    definitions[8 + q * 2] = 1;
    definitions[9 + q * 2] = 0x24; /* Palette 1, high priority: still terrain. */
  }
  for (unsigned row = 0; row < 8; ++row) vram[16 + row] = 255;
  ActionRoomSceneFrameState raster = {0};
  float depth[224];for (unsigned i=0;i<224;++i)depth[i]=.25f;
  ActionEnvironmentScene scene = {.vram = vram, .metatiles = {NULL, definitions},
      .word_mask = {65535,65535}, .background_frame = &raster,
      .maps = {{0},{map,sizeof(map),256,256,1}}};
  Parse("[effects]\nversion=1\n[emitter:01:02:0:storm-lightning:123]\nx=128\ny=80\n"
      "width=128\nheight=128\nlifetime=600\nactivity=.07\nstrands=4\namplitude=15\n"
      "seed=2027\nanchor=bg2-point\nparallax-x=.25\nground-palette=1\nground-colors=1\n");
  ActionEffectInstance *e = &frame.authored[0];
  const clock_t capture_start = clock();
  for (unsigned seed = 0; seed < 256; ++seed) {
    e->storm_contact.valid = 0;
    e->flags |= kActionEffectFlag_Visible;
    ActionStorm_CaptureGround(&scene, &raster, depth, e, seed);
    assert(e->storm_contact.valid && (e->flags & kActionEffectFlag_Visible));
    ActionStormChannel path;
    ActionStorm_Path(e, seed, &path);
    const ArRenderPointF hit = path.main[kActionStormMainSegments];
    assert(e->world_y + hit.y >= 96 && e->world_y + hit.y <= 96.5f);
    for (unsigned strand = 0; strand < 4; ++strand) {
      const ArRenderPointF *points = strand ? path.forks[strand-1] : path.main;
      const unsigned segments = strand ? kActionStormForkSegments : kActionStormMainSegments;
      if (!memcmp(&points[0], &points[segments], sizeof(*points))) continue;
      for (unsigned j = 0; j <= segments; ++j) assert(e->world_y + points[j].y <= 96.5f);
    }
  }
  printf("Storm terrain capture + path validation: %.4f ms/strike.\n",
      1000.0*(clock()-capture_start)/CLOCKS_PER_SEC/256);
  /* A visible ridge at another parallax depth cannot terminate this strike.
   * The source and receiving mountain must both match the cloud's rate. */
  for (unsigned row=96;row<112;++row) depth[row]=.125f;
  ActionStorm_CaptureGround(&scene,&raster,depth,e,31);
  ActionStormChannel matched;
  ActionStorm_Path(e,31,&matched);
  assert(e->storm_contact.valid);
  assert(e->world_y+matched.main[kActionStormMainSegments].y>=112);
  assert(e->world_y+matched.main[kActionStormMainSegments].y<=112.5f);
  const unsigned root=(unsigned)floorf(e->world_y+ActionStorm_SourceY(e));
  depth[root]=.125f;
  ActionStorm_CaptureGround(&scene,&raster,depth,e,31);
  assert(!e->storm_contact.valid && !(e->flags&kActionEffectFlag_Visible));
  e->flags|=kActionEffectFlag_Visible;
  ActionStorm_CaptureGround(&scene,&raster,NULL,e,31);
  assert(!e->storm_contact.valid && !(e->flags&kActionEffectFlag_Visible));
  for(unsigned row=0;row<224;++row)depth[row]=.25f;
  e->flags|=kActionEffectFlag_Visible;
  /* The same artwork at a different native raster offset moves the contact,
   * not the source, and a ray without a receiver stays hidden. */
  for (unsigned row = 0; row < 224; ++row) raster.bg_vscroll[1][row] = 16;
  e->storm_contact.valid = 0;
  ActionStorm_CaptureGround(&scene, &raster, depth, e, 31);
  ActionStormChannel path;
  ActionStorm_Path(e, 31, &path);
  assert(e->world_y + path.main[kActionStormMainSegments].y >= 80);
  assert(e->world_y + path.main[kActionStormMainSegments].y <= 80.5f);
  memset(map,0,sizeof(map));
  e->storm_contact.valid = 0;
  ActionStorm_CaptureGround(&scene, &raster, depth, e, 31);
  assert(!(e->flags & kActionEffectFlag_Visible));
  /* Explicit depth is fractional BG1 travel, independent of BG2's base scroll.
   * Presentation interpolation replaces only X, leaving vertical anchoring. */
  ActionEffectProjectionContext view = {.bg1_camera_x=128,.bg2_camera_x=64};
  ArRenderPointF start = ActionEffectProjection_CaptureOrigin(&view,e);
  view.bg1_camera_x += 32; view.bg2_camera_x += 16;
  ArRenderPointF end = ActionEffectProjection_CaptureOrigin(&view,e);
  assert(start.x-end.x == 8 && start.y == end.y);
  ++view.bg1_camera_x;
  assert(end.x-ActionEffectProjection_CaptureOrigin(&view,e).x == .25f);
  DioramaProjection p = {.bg1_plane={.valid=true,.capture_offset={3,0}},
      .bg2_plane={.valid=true,.capture_offset={9,0}}};
  view.diorama_projection = &p;
  assert(fabsf(ActionEffectProjection_CaptureOrigin(&view,e).x + 9 - (end.x-.25f+.75f)) < .001f);
  puts("Storm terrain: matched cloud/mountain depths, clipped forks, raster offsets, missing receivers and fractional parallax passed.");
}

static void StormChannels(void) {
  frame = (ActionSceneEffectFrame){0};
  Parse("[effects]\nversion=1\n[emitter:01:02:0:storm-lightning:123]\nx=128\ny=80\n"
      "width=256\nheight=128\nlifetime=600\nactivity=.07\nstrands=4\namplitude=15\n"
      "seed=2027\nanchor=bg2-point\n");
  ActionEffectInstance *e = &frame.authored[0];
  ActionStormFlash flash, initial = {0};
  unsigned first = 0, returning = 0;
  float trough = 1;
  bool started = false;
  for (unsigned tick = 0; tick < 600 && !returning; ++tick) {
    e->pulse_ticks = tick;
    if (!ActionStorm_Flash(e, &flash)) continue;
    if (!started) {
      started = true;
      initial = flash;
      first = tick;
    } else {
      assert(flash.seed == initial.seed);
      if (flash.forks[1] > .2f) returning = tick;
      trough = fminf(trough, flash.channel);
    }
  }
  assert(started && returning - first >= 12 && returning - first <= 16);
  assert(initial.channel > .8f && initial.forks[1] < .04f);
  assert(trough > .12f && trough < .4f); /* Still visible between discharges. */
  assert(flash.channel > trough * 3 && flash.forks[1] > .2f);
  unsigned readable = 0;
  float ending = 1;
  for (unsigned age = 0; age < 42; ++age) {
    e->pulse_ticks = first + age;
    assert(ActionStorm_Flash(e, &flash));
    if (age < 3) assert(flash.channel > .8f); /* A held peak, not a one-frame spark. */
    if (flash.channel > .2f) ++readable;
    if (age == 41) ending = flash.channel;
  }
  assert(readable >= 28 && ending < .01f);
  e->pulse_ticks = first;
  assert(ActionSceneDecorationRender_Build(&frame, e->render_layer,
      true, true, Project, NULL, NULL, &a));
  ActionStormChannel initial_path;
  ActionStorm_Path(e, initial.seed, &initial_path);
  /* The cloud flash must surround the channel's actual point of emergence,
   * not make a disconnected pool of light underneath the cloud. */
  assert(a.vertices[0].position.x == e->world_x + initial_path.main[0].x);
  assert(a.vertices[0].position.y == e->world_y + initial_path.main[0].y);
  e->pulse_ticks = returning;
  assert(ActionSceneDecorationRender_Build(&frame, e->render_layer,
      true, true, Project, NULL, NULL, &b));
  assert(a.vertex_count == b.vertex_count && a.vertex_count > 0);
  for (int i = 0; i < a.vertex_count; ++i)
    assert(a.vertices[i].position.x == b.vertices[i].position.x &&
        a.vertices[i].position.y == b.vertices[i].position.y);

  /* Skybox bands partition the frame. The cloud glow must be clipped too;
   * otherwise every intersecting band draws its entire billboard again. */
  for (unsigned part = 0; part < 4; ++part) {
    const ActionEffectLocalRect clip = {0, 32.f * part, 256, 32.f * (part + 1)};
    assert(ActionSceneDecorationRender_Build(&frame, e->render_layer,
        true, true, Project, Clip, (void *)&clip, &a));
    for (int i = 0; i < a.vertex_count; ++i) {
      const ArRenderPointF point = a.vertices[i].position;
      assert(point.x >= clip.x0 && point.x <= clip.x1);
      assert(point.y >= clip.y0 && point.y <= clip.y1);
    }
  }

  /* Every fork is attached, source coordinates survive return strokes, and
   * the admission envelope contains even small regions with maximum bends. */
  const float sizes[][2] = {{96, 28}, {144, 42}, {256, 128}, {1, 1}, {1, 256}, {256, 1}};
  unsigned upturns = 0;
  for (unsigned size = 0; size < sizeof(sizes) / sizeof(sizes[0]); ++size) {
    e->geometry.data.rect = (ActionEffectLocalRect){0, 0, sizes[size][0], sizes[size][1]};
    e->field_style.amplitude = 32;
    const ActionEffectLocalRect bounds = ActionStorm_Bounds(e);
    for (uint32_t seed = 0; seed < 256; ++seed) {
      ActionStormChannel path;
      ActionStorm_Path(e, seed, &path);
      const float rx = sizes[size][0] * .48f, ry = sizes[size][1] * .48f;
      assert(path.main[0].x - rx >= bounds.x0 && path.main[0].x + rx <= bounds.x1);
      assert(path.main[0].y - ry >= bounds.y0 && path.main[0].y + ry <= bounds.y1);
      for (unsigned strand = 0; strand < 4; ++strand) {
        const ArRenderPointF *points = strand ? path.forks[strand - 1] : path.main;
        const unsigned segments = strand ? kActionStormForkSegments : kActionStormMainSegments;
        for (unsigned i = 0; i <= segments; ++i) {
          assert(isfinite(points[i].x) && isfinite(points[i].y));
          assert(points[i].x >= bounds.x0 && points[i].x <= bounds.x1);
          assert(points[i].y >= bounds.y0 && points[i].y <= bounds.y1);
          if (i && points[i].y < points[i - 1].y) ++upturns;
        }
        if (strand) {
          bool attached = false;
          for (unsigned i = 0; i <= kActionStormMainSegments; ++i)
            attached |= points[0].x == path.main[i].x && points[0].y == path.main[i].y;
          assert(attached);
        }
      }
    }
  }
  assert(upturns > 0);
  puts("Storm channels: held discharge, lingering channel, same-path return and bounded forks.");
}

static void StormWeather(void) {
  frame=(ActionSceneEffectFrame){0};
  Parse("[effects]\nversion=1\n[emitter:01:02:0:storm-lightning:123]\nx=128\ny=80\n"
      "width=256\nheight=128\nlifetime=600\nactivity=.07\nstrands=4\namplitude=15\nseed=2027\nanchor=bg2-point\n");
  const unsigned layer=frame.authored[0].render_layer;
  assert(layer==kActionEffectRenderLayer_Bg2Weather);
  assert(frame.authored[0].projection_plane==kActionEffectProjectionPlane_Bg2);
  assert(!ActionEffectReceivers_HasActors(&frame,true));
  unsigned lit=0,first=0;
  for(unsigned tick=0;tick<1800;++tick) {
    frame.authored[0].pulse_ticks=tick;
    assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&a));Valid(&a);
    if(a.vertex_count) { if(!lit)first=tick;++lit; }
    assert((unsigned)a.vertex_count <= ActionStorm_VertexBudget(4));
  }
  assert(lit>60&&lit<150); /* Long quiet gaps; no camera-dependent trigger. */
  frame.authored[0].pulse_ticks=first+2;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&a));assert(a.vertex_count);
  frame.authored[0].pulse_ticks=1500;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  frame.authored[0].pulse_ticks=first+2;
  assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
  assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  assert(ActionSceneDecorationRender_Build(&frame,layer,false,true,Project,NULL,NULL,&b));assert(!b.vertex_count);
  frame.authored[0].tuning.light_receivers_set=1;
  for(unsigned mask=0;mask<8;++mask) {
    frame.authored[0].tuning.light_receivers=mask;
    assert(ActionEffectReceivers_Prepare(&frame,1,2,0,0,0,0,true,&receiver));
    assert(!!receiver.player.vertex_count==!!(mask&2));
    assert(!!receiver.enemies.vertex_count==!!(mask&4));
    assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
    assert(a.vertex_count==b.vertex_count&&!memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  }
  /* Changing the pattern produces different bolt geometry, not actor state. */
  frame.authored[0].particle_style.seed++;
  bool changed=false;
  for(unsigned tick=0;tick<600&&!changed;++tick) {
    frame.authored[0].pulse_ticks=tick;
    assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&b));
    changed=b.vertex_count&& (a.vertex_count!=b.vertex_count||memcmp(a.vertices,b.vertices,a.vertex_count*sizeof(*a.vertices)));
  }
  assert(changed);
  /* Pure sky clouds can roll on BG2; body wakes still require BG1. */
  Parse("[effects]\nversion=1\n[emitter:01:02:0:cloud-bank:1]\nanchor=bg2-point\ntextured=1\nrolling=.8\n");
  assert(ActionSceneDecorationRender_Build(&frame,frame.authored[0].render_layer,false,true,Project,NULL,NULL,&a));assert(a.vertex_count);
  Parse("[effects]\nversion=1\n[emitter:01:02:0:cloud-bank:1]\nx=4096\ny=128\nwidth=8192\nheight=160\n"
      "anchor=bg2-point\nplacement=background\ntextured=1\nrolling=.8\nstrands=24\n");
  ActionEffectProjectionContext view={.bg2_camera_x=1536,.visible_width=512,.snes_height=256,
      .capture_height=256,.viewport={0,0,512,256}};
  const unsigned cloud_layer=frame.authored[0].render_layer;
  const unsigned nearby=SourceBuild(cloud_layer,false,true,&view);
  assert(nearby>0&&nearby<32); /* Centre is offscreen; intersecting billows survive. */
  view.bg2_camera_x=0;view.visible_width=8192;view.viewport.w=8192;
  assert(SourceBuild(cloud_layer,false,true,&view)>nearby*3);
  DioramaProjection sky={.valid=true,.bg2_skybox={.count=2,.active_band=0,
      .bands={{.x0=0,.x1=512,.y0=0,.y1=128,.output_y0=0,.output_y1=.5f},
              {.x0=0,.x1=512,.y0=128,.y1=256,.output_y0=.5f,.output_y1=1}}}};
  view.diorama_projection=&sky;view.bg2_camera_x=1536;
  ActionEffectSourceBatch source={.context=view};ActionEffectLocalRect first_band,second_band;
  assert(ActionEffectSource_ClipBounds(&source,&frame.authored[0],&first_band));
  sky.bg2_skybox.active_band=1;
  assert(ActionEffectSource_ClipBounds(&source,&frame.authored[0],&second_band));
  assert(!memcmp(&first_band,&second_band,sizeof(first_band)));
  /* The resident sky publishes raw mapping inputs, not resolved CPU bands. */
  memset(sky.bg2_skybox.bands,0,sizeof(sky.bg2_skybox.bands));
  sky.bg2_skybox.resident=(DioramaSkyboxSourceMapping){.meta={2,512,256,1},
      .output={1920,1080,10,20},.vertical={0,256,0,1},.follow={-32,192,0,0},.fit={64},
      .bands={{0,1,0,128},{.1f,.9f,128,256}}};
  assert(ActionEffectSource_ParticleViewBounds(&view,&frame.authored[0],&first_band));
  assert(first_band.x0==-2586&&first_band.x1==-2042);
  assert(first_band.y0==-196&&first_band.y1==124);
  assert(SourceBuild(cloud_layer,false,true,&view)>0);
  puts("Storm weather: bounded bolts, quiet intervals, reverse seeking, background anchoring and BG2 rolling clouds passed.");
}

static void CloudLighting(void) {
  /* Real shipped seeds must produce unequal widths and gaps, not another
   * evenly spaced template with small jitter. Positions do not use count. */
  const uint32_t pattern=(uint32_t)DioramaCloudLight_Seed(512);
  CloudLightOpening opening[4];
  float left=1,right=0,narrow=1,wide=0,dim=2,bright=0;
  for(int i=0;i<4;++i) {
    opening[i]=CloudLightOpeningAt(pattern,i);
    left=fminf(left,opening[i].centre);right=fmaxf(right,opening[i].centre);
    narrow=fminf(narrow,opening[i].width);wide=fmaxf(wide,opening[i].width);
    dim=fminf(dim,opening[i].power);bright=fmaxf(bright,opening[i].power);
  }
  assert(right-left>.4f&&wide>narrow*1.5f&&bright>dim*2);
  assert(fabsf(opening[1].centre-opening[2].centre)<.04f); /* A cluster. */
  assert(fabsf(opening[0].centre-opening[3].centre)>.3f); /* A dark gap. */
  assert(DioramaCloudLight_Seed(104)!=DioramaCloudLight_Seed(104+4096));
  /* A shaft fills its opening without periodic dark gaps, and stays broad
   * beyond the surface halo. Animation changes transmission, not its axis. */
  const float ray_seed=DioramaCloudLight_Seed(2027);
  const CloudLightOpening ray=CloudLightOpeningAt((uint32_t)ray_seed,0);
  const float ray_top=CloudLightEdge(ray.centre,ray_seed,.15f)+ray.offset;
  for(int phase=0;phase<3;++phase)for(int depth=1;depth<=3;++depth) {
    const float dy=depth*.08f;
    const float width=CloudLightBeamWidth(ray.width,dy);
    const float x=ray.centre-dy*.62f*.68f;
    const CloudLightSample centre=CloudLightWeights(x,ray_top+dy,.68f,-.62f,
        phase*.7f,ray_seed,1,.5f,.85f,.55f,.15f,.8f);
    float minimum=1,maximum=0;
    for(int column=-8;column<=8;++column) {
      const CloudLightSample lit=CloudLightWeights(x+column*width/16,ray_top+dy,
          .68f,-.62f,phase*.7f,ray_seed,1,.5f,.85f,.55f,.15f,.8f);
      minimum=fminf(minimum,lit.streak);
      maximum=fmaxf(maximum,lit.streak);
    }
    const CloudLightSample outside=CloudLightWeights(x+width*3,ray_top+dy,
        .68f,-.62f,phase*.7f,ray_seed,1,.5f,.85f,.55f,.15f,.8f);
    assert(minimum>.05f&&minimum>maximum*.55f);
    assert(outside.streak<centre.streak*.015f);
    if(depth==3)assert(centre.streak>centre.halo*10);
  }
  const CloudLightSample above=CloudLightWeights(ray.centre,ray_top-.02f,
      .68f,-.62f,0,ray_seed,1,.5f,.65f,.55f,.15f,.8f);
  const CloudLightSample below=CloudLightWeights(ray.centre,ray_top+ray.reach,
      .68f,-.62f,0,ray_seed,1,.5f,.65f,.55f,.15f,.8f);
  assert(above.streak==0&&below.streak==0);
  Parse("[effects]\nversion=1\n[emitter:01:02:0:cloud-light:123]\nx=664\ny=134\nseed=104\n");
  assert(frame.authored_count==1);
  DioramaCloudLight light=ActionCloudLight_Prepare(&frame,100,200,640,352);
  assert(light.meta[0]==1&&light.source[0]==100&&light.source[1]==200);
  assert(light.meta[1]==0&&light.background_dim[0]==0); /* Existing rooms retain their base. */
  const DioramaCloudLightRegion saved=light.regions[0];
  assert(saved.bounds[0]==528&&saved.bounds[1]==42&&saved.core[0]==9);
  float sum=0,change=0;
  frame.authored[0].pulse_ticks+=360;
  DioramaCloudLight later=ActionCloudLight_Prepare(&frame,0,0,640,352);
  for(int y=0;y<184;y+=2)for(int x=0;x<272;x+=2) {
    const CloudLightSample a=DioramaCloudLight_Sample(&saved,528+x,42+y);
    const CloudLightSample b=DioramaCloudLight_Sample(&later.regions[0],528+x,42+y);
    sum+=a.edge+a.halo+a.streak+a.echo;
    change+=fabsf(a.edge-b.edge)+fabsf(a.echo-b.echo);
    assert(isfinite(a.streak)&&a.streak>=0);
  }
  assert(sum>100&&change>1);
  frame.authored[0].pulse_ticks-=360;
  light=ActionCloudLight_Prepare(&frame,-700,64,512,256);
  assert(!memcmp(&light.regions[0],&saved,sizeof(saved))); /* Camera never changes the field. */
  const CloudLightSample outside=DioramaCloudLight_Sample(&saved,527,134);
  assert(outside.edge==0&&outside.halo==0&&outside.streak==0&&outside.echo==0);
  frame.authored[0].world_x+=1000;frame.authored[0].world_y+=500;
  light=ActionCloudLight_Prepare(&frame,0,0,640,352);
  const CloudLightSample a=DioramaCloudLight_Sample(&saved,664,180);
  const CloudLightSample b=DioramaCloudLight_Sample(&light.regions[0],1664,680);
  assert(!memcmp(&a,&b,sizeof(a)));
  frame.authored[0].field_style.cloud.background_brightness=.72f;
  light=ActionCloudLight_Prepare(&frame,0,0,640,352);
  assert(fabsf(light.background_dim[0]-.28f)<.0001f&&light.meta[1]==light.background_dim[0]);
  assert(CloudLightDimming(.5f,.5f,.28f)==.28f);
  assert(CloudLightDimming(-.1f,.5f,.28f)==0&&CloudLightDimming(.5f,1.1f,.28f)==0);
  assert(fabsf(CloudLightDimming(.05f,.5f,.28f)-.14f)<.0001f);
  assert(fabsf(CloudLightDimming(.5f,.95f,.28f)-.14f)<.0001f);
  frame.authored[0].flags=0;
  light=ActionCloudLight_Prepare(&frame,0,0,640,352);assert(!DioramaCloudLight_Active(&light));
  light=ActionCloudLight_Prepare(NULL,0,0,640,352);assert(!DioramaCloudLight_Active(&light));
  puts("Cloud light: shared transmission, replay, world translation, camera independence and disable passed.");
}

int main(int argc, char **argv) {
  MistSkyLighting();
  WaterSparkles();
  MistRainbows();
  CloudVolumes();
  CloudLighting();
  CloudBreakFans();
  StormGround();
  StormChannels();
  StormWeather();
  MistLightingBounds();
  DropletEdges();
  SwampLifeFields();
  MagicalSwampFields();
  RollingMistFields();
  NativeSourceOverrides();
  NativeMemberReceivers();
  BillboardExtents();
  SoftLightScroll();
  NativeDaylightReceivers();
  LightResponseAudit();
  HighIntensityGeometry();
  SurfaceLightingSplit();
  CastleSkyDepth();
  DeferredAuthored();TorchShadows();CanopyFields();TexturedCanopy();DeferredParticleArea();
  LayerAnchors();
  Geometry();GroundDust();MovingGlints();HeatAndParticlePaths();HaloAndGradient();
  Receivers();TorchReceivers();ReceiverIllumination();ExposureBoxes();ExposureMeshScroll();ExposureGroupCapacity();
  Events();
  MovingLightReceivers();
  NativeMembers();
  EditedOcclusion();
  puts("Authored fields: CPU/source families, clipping, bounded areas, viewport-stable particles, reverse seeking, "
       "receiver separation, moving lights, glow centre and 19 preview families passed.");
  if (argc == 2 && !strcmp(argv[1], "--benchmark")) {BenchmarkReceivers();BenchmarkExposureMesh();BenchmarkSwampLighting();}
  return 0;
}
