#include "action/action_effect_render.h"
#include "action/action_effect_projection.h"
#include "diorama/diorama.h"
#include "support/test_assert.h"
#include "action/action_effect_recipes.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static ActionSceneEffectFrame frame;
static ActionSceneEffectRenderBatch batch;
static unsigned hash=2166136261u;
static void Word(unsigned v){hash=(hash^v)*16777619u;}
static void Float(float v){unsigned b;memcpy(&b,&v,4);Word(b);}
static void LegacyOracle(void){
 const unsigned kinds[]={kActionEffect_BloodpoolWater,kActionEffect_BloodpoolMist,kActionEffect_BloodpoolTimber,kActionEffect_BloodpoolAir};
 const unsigned layers[]={kActionEffectRenderLayer_Bg1HighPlane,kActionEffectRenderLayer_Bg2HighAlpha,kActionEffectRenderLayer_Bg1Plane,kActionEffectRenderLayer_Bg2HighAlpha};
 frame.decoration_count=frame.decoration_visible_count=5;frame.bloodpool.valid=frame.moonlight.valid=true;
 frame.bloodpool.timber_count=4;frame.bloodpool.post_count=3;
 frame.bloodpool.timber[0]=(ActionBloodpoolTimber){208,224,352,222,360,488,1};
 frame.bloodpool.timber[1]=(ActionBloodpoolTimber){480,496,320,490,330,400,0};
 frame.bloodpool.timber[2]=(ActionBloodpoolTimber){608,624,400,620,410,488,1};
 frame.bloodpool.timber[3]=(ActionBloodpoolTimber){720,736,352,722,360,488,1};
 frame.bloodpool.posts[0]=(ActionBloodpoolPost){278,283,485};
 frame.bloodpool.posts[1]=(ActionBloodpoolPost){678,683,495};
 frame.bloodpool.posts[2]=(ActionBloodpoolPost){768,775,500};
 frame.moonlight.count=2;frame.moonlight.rectangles[0]=(ActionMoonlightOccluder){200,290,224,352};
 frame.moonlight.rectangles[1]=(ActionMoonlightOccluder){600,340,680,356};
 frame.decorations[4]=(ActionEffectInstance){.kind=kActionEffect_BloodpoolMoonlight,.phase=kActionEffectPhase_BloodpoolEnvironment,.environment_room=1,
 .world_x=112,.world_y=62,.flags=kActionEffectFlag_Visible|kActionEffectFlag_StaticAnchor,
 .render_layer=kActionEffectRenderLayer_Bg2Plane,.projection_plane=kActionEffectProjectionPlane_Bg2,
 .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-384,-48,384,194}}};
 unsigned long vertices=0,indices=0;
 for(unsigned round=0;round<3;++round){clock_t start=clock();
 for(unsigned i=0;i<240;++i){
  const unsigned mode=i%3;const int camera=(i%12)*64;
  DioramaProjection p={.valid=true,.output_width=960,.output_height=600,
   .matrix={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},.aspect_x=2,.height_scale=1,
   .texture_width=512,.texture_height=352,
   .bg1_plane={.valid=true,.u1=1,.v1=1},.bg1_high_plane={.valid=true,.u1=1,.v1=1},.bg2_plane={.valid=mode!=1,.u1=1,.v1=1}};
  if(mode)p.bg2_skybox=(DioramaSkyboxProjection){.count=2,.active_band=-1,.bands={{44,64,476,200,0,136.f/224},{0,200,496,288,136.f/224,1}}};
  ActionEffectProjectionContext c={.ws_extra=120,.ws_extra_top=64,.bg1_camera_x=camera,.bg1_camera_y=256,.diorama_projection=&p};
  for(unsigned j=0;j<4;++j)frame.decorations[j]=(ActionEffectInstance){.kind=kinds[j],.phase=kActionEffectPhase_BloodpoolEnvironment,.environment_room=1,
   .world_x=camera+128,.world_y=j<2?480:0,.source_mask=255,.phase_ticks=i*197,.pulse_ticks=i*197,
   .flags=kActionEffectFlag_Visible|kActionEffectFlag_ClipToRect,.render_layer=layers[j],
   .projection_plane=j==0?kActionEffectProjectionPlane_Bg1High:kActionEffectProjectionPlane_Bg1,
   .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-384,j<2?-48:0,384,j<2?32:512}},
   .clip_rect={j<2?-(camera+128):-384,j<2?-48:0,j<2?4096-(camera+128):384,j<2?32:512}};
  frame.decorations[4].phase_ticks=i*197;
  for(unsigned layer=0;layer<kActionEffectRenderLayer_Count;++layer){if(layer==kActionEffectRenderLayer_Bg2Plane)continue;
   assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,ActionEffectProjection_ProjectPoint,ActionEffectProjection_ClipBounds,&c,&batch));
   vertices+=batch.vertex_count;indices+=batch.index_count;Word(batch.vertex_count);Word(batch.index_count);
   for(int v=0;v<batch.vertex_count;++v){const ArRenderVertex2D *a=&batch.vertices[v];Float(a->position.x);Float(a->position.y);Float(a->color.r);Float(a->color.g);Float(a->color.b);Float(a->color.a);}
   for(int n=0;n<batch.index_count;++n)Word(batch.indices[n]);
  }
 }
 printf("%.6f ms/frame\n",(double)(clock()-start)*1000/CLOCKS_PER_SEC/240);
 }
 printf("%08x %lu %lu\n",hash,vertices,indices);
 assert((hash==0x004603afu||hash==0x9dbb3787u)&&vertices==828519&&indices==3063978);
}

static ActionEffectRecipes table,previous;
static char text[16384];
static unsigned TileIndex(unsigned x,unsigned y) {return ((y/256)*16+x/256)*256+(y%256)/16*16+(x%256)/16;}
static void Load(const ActionMarshField *f,unsigned group,unsigned room) {
  const int n=snprintf(text,sizeof(text),"[effects]\nversion=1\n[field:%02x:%02x:0:marsh-field:b1000000]\n",group,room);
  assert(ActionMarshField_Write(f,text+n,sizeof(text)-(size_t)n));unsigned line;
  assert(ActionEffectRecipes_Parse(&table,text,strlen(text),&line));
}
static bool Project(void *ctx,const ActionEffectInstance *e,float x,float y,ArRenderPointF *p) {
  (void)ctx;*p=(ArRenderPointF){e->world_x+x,e->world_y+y};return true;
}
static unsigned Digest(void) {
  const unsigned saved=hash;hash=2166136261u;
  for(unsigned layer=0;layer<kActionEffectRenderLayer_Count;++layer) {
    assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&batch));
    Word(batch.vertex_count);Word(batch.index_count);
    for(int i=0;i<batch.vertex_count;++i) {
      const ArRenderVertex2D *v=&batch.vertices[i];Float(v->position.x);Float(v->position.y);
      Float(v->color.r);Float(v->color.g);Float(v->color.b);Float(v->color.a);
    }
  }
  const unsigned result=hash;hash=saved;return result;
}
int main(void) {
 LegacyOracle();
 const ActionMarshField *bundled=ActionMarshField_Bundled();assert(bundled);
 FILE *file=fopen("assets/effects/marsh-field.ini","rb");assert(file);size_t bytes=fread(text,1,sizeof(text)-1,file);fclose(file);text[bytes]=0;
 unsigned line;assert(ActionEffectRecipes_Parse(&table,text,bytes,&line));
 assert(table.marsh_field_count==1&&!memcmp(&table.marsh_fields[0],bundled,sizeof(*bundled)));
 assert(ActionEffectRecipes_ReplacesMarshField(&table,2,1,0)&&!ActionEffectRecipes_ReplacesMarshField(&table,2,1,1));
 static uint8_t map[8192],sky[256],words[2048];
 map[TileIndex(0,432)]=138;map[TileIndex(736,320)]=185;
 for(unsigned i=0;i<8;++i)for(unsigned x=(unsigned)bundled->spans[i][0];x<(unsigned)bundled->spans[i][1];x+=16)map[TileIndex(x,480)]=32;
 sky[3*16+7]=60;sky[4*16+7]=68;
 ActionEnvironmentScene scene={.group=2,.room=1,.clock=999,.camera_x={512,0},.camera_y={256,0},
   .maps={{map,sizeof(map),4096,512,16},{sky,sizeof(sky),256,256,1}},
   .metatiles={words,words},.word_mask={0xECFF,0},.attributes={0x1000,0}};
 memset(&frame,0,sizeof(frame));ActionMarshField_Capture(&scene,&frame,bundled,0xB1000000);
 assert(frame.bloodpool.field_valid&&frame.bloodpool.valid&&frame.decoration_count==4);const unsigned original=Digest();
 memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&table,2,1,0,999,&scene,&frame);assert(Digest()==original);
 assert(frame.decorations[0].source_mask==255&&frame.decorations[0].world_y==480);
 ActionMarshField edited=*bundled;memset(edited.Dimensions,0,sizeof(edited.Dimensions));edited.WitnessCount[0]=0;
 Load(&edited,5,3);scene.group=5;scene.room=3;ActionEffectRecipes_Apply(&table,5,3,0,999,&scene,&frame);assert(Digest()==original);
 table.marsh_fields[0].WaterColor[0]=0;assert(Digest()==original);
 ActionEffectRecipes_Apply(&table,5,3,0,999,&scene,&frame);assert(Digest()!=original);
 Load(&edited,5,3);map[TileIndex(176,480)]=0;ActionEffectRecipes_Apply(&table,5,3,0,999,&scene,&frame);assert(frame.decorations[0].source_mask==254);
 map[TileIndex(176,480)]=32;
 sky[3*16+7]=0;ActionEffectRecipes_Apply(&table,5,3,0,999,&scene,&frame);assert(frame.decoration_count==2&&!frame.bloodpool.valid);sky[3*16+7]=60;
 edited.Components[0]=0;Load(&edited,5,3);ActionEffectRecipes_Apply(&table,5,3,0,999,&scene,&frame);
 assert(!frame.decoration_count&&frame.bloodpool.field_valid&&!frame.bloodpool.valid);
 assert(!ActionEffectRecipes_NeedsBgMask(&table,5,3,0,0)&&!ActionEffectRecipes_NeedsBgMask(&table,5,3,0,1));
 table.records[0].enabled=0;ActionEffectRecipes_Apply(&table,5,3,0,999,&scene,&frame);assert(!frame.bloodpool.field_valid&&!frame.decoration_count);
 previous=table;const char *invalid="[effects]\nversion=1\n[field:02:01:0:marsh-field:1]\ncomponents=15\n";
 assert(!ActionEffectRecipes_Parse(&table,invalid,strlen(invalid),&line)&&!memcmp(&table,&previous,sizeof(table)));
 invalid="[effects]\nversion=1\n[source:02:01:0:marsh-field:1]\n";
 assert(!ActionEffectRecipes_Parse(&table,invalid,strlen(invalid),&line));
 edited=*bundled;edited.WaterCells[0]=1;assert(!ActionMarshField_Valid(&edited));
 edited=*bundled;edited.MistCells[0]=1;assert(!ActionMarshField_Valid(&edited));
 edited=*bundled;edited.RippleShape[0]=0;assert(!ActionMarshField_Valid(&edited));
 edited=*bundled;edited.Span2[0]=200;assert(!ActionMarshField_Valid(&edited));
 edited=*bundled;edited.InsectCount[0]=5;assert(!ActionMarshField_Valid(&edited));
 edited=*bundled;edited.DripTiming[4]=17;assert(!ActionMarshField_Valid(&edited));
 edited=*bundled;edited.Bounds[3]=1024;assert(!ActionMarshField_Valid(&edited));
 puts("marsh baseline, complete export, generic-room capture, material readiness, retained ownership and bounded rejection passed");
}
