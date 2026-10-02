#include "action/action_effect_render.h"
#include "action/action_effect_projection.h"
#include "diorama/diorama.h"
#include "support/test_assert.h"
#include "action/action_effect_recipes.h"
#include <stdio.h>
#include <time.h>
#include <string.h>
static ActionSceneEffectFrame frame;
static ActionSceneEffectRenderBatch batch;
static unsigned hash=2166136261u;
static void Word(unsigned value) { hash=(hash^value)*16777619u; }
static void Float(float v) { unsigned bits;memcpy(&bits,&v,sizeof(bits));Word(bits); }
static ActionEffectRecipes recipes, previous;
static char text[16384];
static ActionEnvironmentScene scene;
static void Load(const ActionMoonField *f,unsigned group,unsigned room) {
  const int n=snprintf(text,sizeof(text),"[effects]\nversion=1\n[field:%02x:%02x:0:moon-field:b1000002]\n",group,room);
  assert(ActionMoonField_Write(f,text+n,sizeof(text)-(size_t)n));
  unsigned line;assert(ActionEffectRecipes_Parse(&recipes,text,strlen(text),&line));
}
static bool Project(void *context,const ActionEffectInstance *effect,float x,float y,ArRenderPointF *out) {
  (void)context;*out=(ArRenderPointF){x+effect->world_x,y+effect->world_y};return true;
}
static uint32_t RenderDigest(void) {
  const uint32_t saved=hash;hash=2166136261u;
  for(unsigned layer=0;layer<kActionEffectRenderLayer_Count;++layer) {
    assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&batch));
    Word(batch.vertex_count);Word(batch.index_count);
    for(int i=0;i<batch.vertex_count;++i) {
      const ArRenderVertex2D *v=&batch.vertices[i];Float(v->position.x);Float(v->position.y);
      Float(v->color.r);Float(v->color.g);Float(v->color.b);Float(v->color.a);
    }
  }
  const uint32_t result=hash;hash=saved;return result;
}
int main(void) {
  const ActionMoonField *bundled=ActionMoonField_Bundled();assert(bundled);
  FILE *file=fopen("assets/effects/moon-field.ini","rb");assert(file);
  const size_t bytes=fread(text,1,sizeof(text)-1,file);fclose(file);text[bytes]=0;unsigned line;
  assert(ActionEffectRecipes_Parse(&recipes,text,bytes,&line));
  assert(recipes.moon_field_count==1&&recipes.records[0].moon_field==1);
  assert(!memcmp(&recipes.moon_fields[0],bundled,sizeof(*bundled)));
  assert(ActionEffectRecipes_ReplacesMoonField(&recipes,2,1,0));
  assert(!ActionEffectRecipes_ReplacesMoonField(&recipes,2,1,1));
  assert(ActionEffectRecipes_NeedsBgMask(&recipes,2,1,0,0)&&ActionEffectRecipes_NeedsBgMask(&recipes,2,1,0,1));
  static uint8_t sky[256];sky[(48/16)*16+112/16]=60;sky[(64/16)*16+112/16]=68;
  scene=(ActionEnvironmentScene){.group=2,.room=1,.suppress_default_moon_field=true,
    .maps={{.world_width=4096,.world_height=512},{sky,sizeof(sky),256,256,1}}};

  unsigned long vertices=0,indices=0;
  frame.decoration_count=frame.decoration_visible_count=3;
  frame.moonlight.valid=true;frame.moonlight.count=3;
  frame.moonlight.rectangles[0]=(ActionMoonlightOccluder){40,90,110,100};
  frame.moonlight.rectangles[1]=(ActionMoonlightOccluder){170,120,220,136};
  frame.moonlight.rectangles[2]=(ActionMoonlightOccluder){0,160,50,170};
  frame.decoration_count=frame.decoration_visible_count=0;
  ActionEffectRecipes_Apply(&recipes,2,1,0,0,&scene,&frame);
  assert(frame.moon_field_valid&&frame.decoration_count==3&&frame.authored_count==0);
  assert(frame.decorations[0].flags&kActionEffectFlag_StaticAnchor);
  assert(!(frame.decorations[1].flags&kActionEffectFlag_StaticAnchor));
  assert(frame.decorations[2].flags&kActionEffectFlag_StaticAnchor);
  for(unsigned round=0;round<5;++round) {
    const clock_t start=clock();
    for(unsigned i=0;i<360;++i) {
      const unsigned mode=i%3;
      DioramaProjection p={.valid=true,.output_width=960,.output_height=600,
        .matrix={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},.aspect_x=2,.height_scale=1,
        .texture_width=512,.texture_height=352,
        .bg1_plane={.valid=true,.u1=1,.v1=1},.bg2_plane={.valid=mode!=1,.u1=1,.v1=1}};
      if(mode)p.bg2_skybox=(DioramaSkyboxProjection){.count=2,.active_band=-1,
        .bands={{44,64,476,200,0,136.f/224},{0,200,496,288,136.f/224,1}}};
      ActionEffectProjectionContext c={.ws_extra=120,.ws_extra_top=64,.bg1_camera_x=i%128,
        .bg1_camera_y=i%48,.diorama_projection=&p};
      for(unsigned e=0;e<3;++e)frame.decorations[e].pulse_ticks=frame.decorations[e].phase_ticks=i*17;
      for(unsigned band=0;band<(mode?2:1);++band) {
        p.bg2_skybox.active_band=mode?(int)band:-1;
        const unsigned layers[]={kActionEffectRenderLayer_Bg2Plane,kActionEffectRenderLayer_Bg2Alpha};
        for(unsigned l=0;l<2;++l) {
          assert(ActionSceneDecorationRender_Build(&frame,layers[l],true,true,
              ActionEffectProjection_ProjectPoint,ActionEffectProjection_ClipBounds,&c,&batch));
          vertices+=batch.vertex_count;indices+=batch.index_count;
          Word(batch.vertex_count);Word(batch.index_count);
          for(int v=0;v<batch.vertex_count;++v) {
            const ArRenderVertex2D *p=&batch.vertices[v];
            Float(p->position.x);Float(p->position.y);Float(p->color.r);Float(p->color.g);Float(p->color.b);Float(p->color.a);
          }
          for(int n=0;n<batch.index_count;++n)Word(batch.indices[n]);
        }
      }
    }
    printf("%.6f ms/frame (geometry + digest)\n",(double)(clock()-start)*1000/CLOCKS_PER_SEC/360);
  }
  printf("pre-migration moon oracle %08x, geometry %lu %lu\n",hash,vertices,indices);
  /* Both pinned before migration: optimized and unoptimized FP evaluation. */
  assert((hash==0x4b12f297u||hash==0xec229576u)&&vertices==5716240&&indices==22331610);
  const uint32_t held=RenderDigest();
  previous=recipes;
  /* Export to any room. Capture and render contain no room-number dispatch. */
  ActionMoonField edited=*bundled;memset(edited.Dimensions,0,sizeof(edited.Dimensions));edited.WitnessCount[0]=0;
  Load(&edited,5,3);scene.group=5;scene.room=3;
  ActionEffectRecipes_Apply(&recipes,5,3,0,scene.clock,&scene,&frame);
  for(unsigned i=0;i<3;++i)frame.decorations[i].phase_ticks=359*17;
  assert(frame.moon_field_valid&&frame.decoration_count==3&&RenderDigest()==held);
  /* Table mutations never alter a retained frame; next capture owns the edit. */
  recipes.moon_fields[0].RayColor[0]=.1f;assert(RenderDigest()==held);
  ActionEffectRecipes_Apply(&recipes,5,3,0,0,&scene,&frame);assert(RenderDigest()!=held);
  /* Animated water uses its real raster clock. No HDMA: omit caps only. */
  Load(&edited,5,3);scene.water_scroll_valid=true;
  for(unsigned i=0;i<96;++i)scene.water_scroll[i]=i*3;
  ActionEffectRecipes_Apply(&recipes,5,3,0,0,&scene,&frame);const uint32_t cap=RenderDigest();
  for(unsigned i=0;i<96;++i)scene.water_scroll[i]+=8;
  ActionEffectRecipes_Apply(&recipes,5,3,0,0,&scene,&frame);assert(RenderDigest()!=cap);
  edited.Anchor[0]+=30;edited.Anchor[1]+=15;Load(&edited,5,3);
  ActionEffectRecipes_Apply(&recipes,5,3,0,0,&scene,&frame);
  for(unsigned i=0;i<3;++i)assert(frame.decorations[i].world_x==142&&frame.decorations[i].world_y==77);
  /* Component gates leave the same bounded source topology and no drawn work. */
  edited.Components[0]=0;Load(&edited,5,3);ActionEffectRecipes_Apply(&recipes,5,3,0,0,&scene,&frame);
  assert(!frame.decoration_count&&frame.moon_field_valid);
  assert(!ActionEffectRecipes_NeedsBgMask(&recipes,5,3,0,0));
  assert(!ActionEffectRecipes_NeedsBgMask(&recipes,5,3,0,1));
  RenderDigest();
  recipes.records[0].enabled=0;ActionEffectRecipes_Apply(&recipes,5,3,0,0,&scene,&frame);
  assert(!frame.decoration_count&&!frame.moon_field_valid);
  recipes=previous;const char *invalid="[effects]\nversion=1\n[field:02:01:0:moon-field:1]\ncomponents=15\n";
  assert(!ActionEffectRecipes_Parse(&recipes,invalid,strlen(invalid),&line));assert(!memcmp(&recipes,&previous,sizeof(recipes)));
  /* Invalid budgets/widths/clock periods are rejected before narrowing or use. */
  edited=*bundled;edited.Low1[1]=0;assert(!ActionMoonField_Valid(&edited));
  edited=*bundled;edited.WaveRows[0]=13;assert(!ActionMoonField_Valid(&edited));
  edited=*bundled;edited.ReflectionRows[0]=24;assert(!ActionMoonField_Valid(&edited));
  edited=*bundled;edited.CloudMotion[0]=17;assert(!ActionMoonField_Valid(&edited));
  edited=*bundled;edited.Window[2]=700;assert(!ActionMoonField_Valid(&edited));
  edited=*bundled;edited.Shadow[5]=.9f;assert(!ActionMoonField_Valid(&edited));
  scene.group=2;scene.room=1;sky[3*16+7]=0;
  memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,2,1,0,0,&scene,&frame);assert(!frame.decoration_count);
  puts("complete moon export, generic-room reconstruction, retained ownership, HDMA, budgets and atomic rejection passed");
}
