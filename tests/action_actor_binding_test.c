#include "action/action_effect_recipes.h"
#include "action/action_effect_render.h"
#include "action/action_effect_projection.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>
static ActionEffectRecipes recipes,previous;
static ActionSceneEffectFrame frame;
static ActionSceneEffectRenderBatch batch;
static bool Parse(const char *text){unsigned line;return ActionEffectRecipes_Parse(&recipes,text,strlen(text),&line);}
static bool Project(void *ctx,const ActionEffectInstance *e,float x,float y,ArRenderPointF *p){(void)ctx;*p=(ArRenderPointF){e->world_x+x,e->world_y+y};return true;}
static bool Render(bool light,bool particles){
 unsigned vertices=0;
 for(unsigned layer=0;layer<kActionEffectRenderLayer_Count;++layer){
   if(!ActionSceneDecorationRender_Build(&frame,layer,light,particles,Project,NULL,NULL,&batch))return false;
   vertices+=batch.vertex_count;
 }
 batch.vertex_count=vertices;return true;
}
int main(void){
 const char *header="[effects]\nversion=1\n[emitter:02:01:0:flame:123]\n";
 char text[2048];
 snprintf(text,sizeof(text),"%sactor-target=family\nactor-source=B786\nactor-parent=B786\nactor-state=0,1\nactor-animation=5000\nactor-limit=2\nx=4\ny=-8\nwidth=64\nheight=96\nparticles=12\nlifetime=80\ncolor=ff6628\ncolor-end=ffe6a0\nlight-scenery=1\nlight-player=1\nlight-enemies=1\n",header);
 assert(Parse(text));
 frame.actor_count=5;
 for(unsigned i=0;i<5;++i)frame.actors[i]=(ActionEffectActor){.generation=i+19,.source=0xb786,.parent_source=0xb786,.animation=0x5000,.state=i?1:7,.age=49,.phase_ticks=17,.x=120+i*16,.y=100,.priority=2,.visible=1,.vx=-512};
 frame.actors[1].visible=0;frame.actors[2].source=0xb787;
 ActionEffectRecipes_Apply(&recipes,2,1,0,100,NULL,&frame);
 assert(frame.authored_count==2);assert(frame.authored[0].world_x==172&&frame.authored[0].world_y==92);
 assert(frame.authored[0].generation!=frame.authored[1].generation);
 assert(frame.authored_sources[0]==0x123&&frame.authored_sources[1]==0x123);
 assert(frame.authored[0].age_ticks==49&&frame.authored[0].phase_ticks==17);
 assert(frame.authored[0].projection_plane==kActionEffectProjectionPlane_Obj&&frame.authored[0].obj_priority==2);
 assert(ActionEffectProjection_RequiredObjPriorityMask(NULL,&frame)==(1u<<2));
 assert(frame.authored[0].velocity_x==-512&&frame.authored[0].tuning.light_receivers==7);
 assert(Render(true,true)&&batch.vertex_count>0);
 const unsigned combined=batch.vertex_count;
 assert(Render(true,false)&&batch.vertex_count>0&&batch.vertex_count<combined);
 assert(Render(false,true)&&batch.vertex_count==96);
 ActionEffectRecipes_Apply(&recipes,2,1,1,100,NULL,&frame);assert(!frame.authored_count);
 previous=recipes;
 const char *invalid[]={"actor-source=B786\n","actor-target=family\n","actor-target=family\nactor-source=0000\n",
  "actor-target=player\nactor-state=2,1\n","actor-target=player\nactor-visual=0,65536\n",
  "actor-target=player\nactor-limit=17\n","actor-target=player\nactor-handler=12345\n",
  "actor-target=player\nactor-limit=16\nparticles=128\n","actor-target=player\nanchor=bg2-point\n",
  "actor-target=player\nplacement=playfield\n","actor-target=player\nactor-state=0,1\nactor-state=0,2\n"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i){snprintf(text,sizeof(text),"%s%s",header,invalid[i]);assert(!Parse(text));assert(!memcmp(&recipes,&previous,sizeof(recipes)));}
 assert(Parse("[effects]\nversion=1\n[emitter:01:01:0:cloud-bank:0]\nactor-target=player\nactor-limit=1\nstrands=2\n"));
 frame=(ActionSceneEffectFrame){.actor_count=1,.actors={{.generation=1,.visible=1,.player=1,.x=100,.y=60}}};
 ActionEffectRecipes_Apply(&recipes,1,1,0,100,NULL,&frame);assert(frame.authored_count==1);
 assert(frame.authored[0].render_layer==kActionEffectRenderLayer_WorldDust);
 assert(ActionEffectProjection_RequiredObjPriorityMask(NULL,&frame)==1);
 assert(Render(true,true)&&batch.vertex_count>0);
 assert(Parse("[effects]\nversion=1\n[emitter:02:01:0:torch:3]\nx=128\ny=96\nplacement=foreground\nreach=2\n"));
 frame=(ActionSceneEffectFrame){0};ActionEffectRecipes_Apply(&recipes,2,1,0,100,NULL,&frame);
 assert(Render(true,true)&&batch.vertex_count>0);
 puts("Generic actor selectors, previously unrecognized boss fire, independent instances, budgets, room/terrain scope and flame/cloud/torch rendering passed");
}
