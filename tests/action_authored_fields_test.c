#include "action/action_effect_recipes.h"
#include "action/action_effect_receivers.h"
#include "action/action_effect_preview.h"
#include "action/action_environment_exposure.h"
#include "action/action_effect_projection.h"
#include "support/test_assert.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static ActionEffectRecipes recipes;
static ActionSceneEffectFrame frame;
static ActionSceneEffectRenderBatch a,b;
static ActionReceiverLighting receiver;
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
  const char *names[]={"particle-area","light-fan","water-surface","drips","waterfall-spray","cloud-bank","exposure","wet-contour"};
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
int main(int argc,char **argv){Geometry();Receivers();Events();MovingLightReceivers();puts("Authored fields: clipping, bounded areas, viewport-stable particles, reverse seeking, receiver separation, moving lights, glow centre and 19 preview families passed.");if(argc==2&&!strcmp(argv[1],"--benchmark"))BenchmarkReceivers();return 0;}
