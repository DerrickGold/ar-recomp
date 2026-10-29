#include "action/action_effect_render.h"
#include "support/test_assert.h"
#include "action/action_effect_recipes.h"
#include "action/action_effect_receivers.h"
#include "action/action_effect_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static ActionSceneEffectFrame frame;
static ActionSceneEffectRenderBatch batch;
static ActionSurfaceField legacy_pit;
static unsigned hash=2166136261u;
static void Word(unsigned v){hash=(hash^v)*16777619u;}
static void Float(float v){unsigned b;memcpy(&b,&v,4);Word(b);}
static bool Project(void *ctx,const ActionEffectInstance *e,float x,float y,ArRenderPointF *p){(void)ctx;*p=(ArRenderPointF){e->world_x+x,e->world_y+y};return true;}
static void LegacyOracle(void){unsigned long vertices=0,indices=0;
 const unsigned kinds[]={kActionEffect_AitosLavaPit,kActionEffect_AitosLavaReservoir,kActionEffect_AitosWaterSplash,kActionEffect_AitosWaterfall,kActionEffect_AitosWaterfallMist};
 const unsigned phases[]={kActionEffectPhase_AitosLavaPit,kActionEffectPhase_AitosLavaReservoir,kActionEffectPhase_AitosWaterSplash,kActionEffectPhase_AitosWaterfallFlow,kActionEffectPhase_AitosWaterfallMist};
 for(unsigned mode=0;mode<5;++mode)for(unsigned shape=0;shape<3;++shape)for(unsigned i=0;i<80;++i){
 frame=(ActionSceneEffectFrame){.decoration_count=1,.decoration_visible_count=1};
 frame.surface_fields_valid=1;frame.surface_fields[0]=legacy_pit;
 float rx=mode>2?256:mode==1?48+shape*128:24+shape*16;
 frame.decorations[0]=(ActionEffectInstance){.kind=kinds[mode],.phase=phases[mode],.generation=0x1234,.pulse_generation=0x7364,
 .world_x=128,.world_y=100,.phase_ticks=i*101,.pulse_ticks=i*79,.flags=kActionEffectFlag_Visible,
 .render_layer=kActionEffectRenderLayer_WorldOverlay,.projection_plane=kActionEffectProjectionPlane_Bg1,
 .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-rx,-16,rx,mode>2?312:16}}};
 assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldOverlay,i%4!=1,i%4!=2,Project,NULL,NULL,&batch));
 vertices+=batch.vertex_count;indices+=batch.index_count;Word(batch.vertex_count);Word(batch.index_count);
 for(int v=0;v<batch.vertex_count;++v){const ArRenderVertex2D *a=&batch.vertices[v];Float(a->position.x);Float(a->position.y);Float(a->color.r);Float(a->color.g);Float(a->color.b);Float(a->color.a);}
 for(int n=0;n<batch.index_count;++n)Word(batch.indices[n]);

 }
 assert(vertices==559800&&indices==2220480);
#if defined(__OPTIMIZE__)
 assert(hash==0x957be29du);
#endif
 printf("%08x %lu %lu\n",hash,vertices,indices);
}

static ActionEffectRecipes recipes;
static char text[16384],exported[16384];
static void MoltenDetail(void){
 static ActionSceneEffectRenderBatch first,repeat;
 static ActionEffectSourcePrimitive primitives[kActionSourceMaximumPrimitives];
 frame=(ActionSceneEffectFrame){.decoration_count=1,.decoration_visible_count=1,.surface_fields_valid=1};
 frame.surface_fields[0]=*ActionSurfaceField_Bundled(0);
 frame.surface_fields[0].Particles[0]=0; /* Isolate the new detail. */
 frame.decorations[0]=(ActionEffectInstance){.kind=kActionEffect_AitosLavaPit,.phase=kActionEffectPhase_AitosLavaPit,
   .generation=31,.pulse_generation=173,.world_x=128,.world_y=100,.flags=kActionEffectFlag_Visible,
   .render_layer=kActionEffectRenderLayer_WorldDust,.projection_plane=kActionEffectProjectionPlane_Bg1,
   .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-64,-16,64,16}},
   .tuning={.light_receivers_set=1,.light_receivers=7}};
 bool airborne=false,dark=false,changed=false;
 for(unsigned tick=0;tick<960;tick+=11){
   frame.decorations[0].pulse_ticks=tick;
   assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,Project,NULL,NULL,&batch));
   assert(batch.vertex_count>0&&batch.vertex_count<=kActionSceneEffectMoltenVerticesPerPit);
   for(int i=0;i<batch.vertex_count;++i){const ArRenderVertex2D *v=&batch.vertices[i];
     assert(isfinite(v->position.x)&&isfinite(v->position.y)&&isfinite(v->color.a));
     assert(v->position.x>=64&&v->position.x<=192); /* No crust or droplets over the banks. */
     assert(v->position.y>30&&v->position.y<116);
     airborne|=v->position.y<65&&v->color.a>.1f;
     dark|=v->color.r<.25f&&v->color.a>.25f;
   }
   if(!tick)first=batch;
   else changed|=batch.vertex_count!=first.vertex_count||memcmp(batch.vertices,first.vertices,(size_t)batch.vertex_count*sizeof(batch.vertices[0]))!=0;
 }
 assert(airborne&&dark&&changed);
 frame.decorations[0].pulse_ticks=0;
 assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,Project,NULL,NULL,&repeat));
 assert(first.vertex_count==repeat.vertex_count&&!memcmp(first.vertices,repeat.vertices,(size_t)first.vertex_count*sizeof(first.vertices[0])));
 frame.decorations[0].world_x+=37;
 assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,Project,NULL,NULL,&repeat));
 assert(first.vertex_count==repeat.vertex_count);
 for(int i=0;i<first.vertex_count;++i){assert(fabsf(repeat.vertices[i].position.x-first.vertices[i].position.x-37)<.001f);assert(fabsf(repeat.vertices[i].position.y-first.vertices[i].position.y)<.001f);}
 for(unsigned mask=0;mask<8;++mask){frame.decorations[0].tuning.light_receivers=mask;
   assert(ActionEffectReceivers_Layer(&frame.decorations[0])==kActionEffectRenderLayer_Bg1Plane);
   for(unsigned role=1;role<=4;role*=2){frame.light_receiver=role;
     assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_Bg1Plane,true,false,Project,NULL,NULL,&batch));
     assert((batch.vertex_count>0)==!!(mask&role));}}
 frame.light_receiver=0;
 /* The maximum editable source/cell/burst counts fit both CPU and GPU
  * packet budgets, with lighting in its separate existing receiver batch. */
 frame.surface_fields[0].MoltenSurface[1]=16;frame.surface_fields[0].MoltenBursts[0]=4;
 frame.surface_fields[0].Particles[0]=12;
 frame.decoration_count=frame.decoration_visible_count=16;
 for(unsigned i=1;i<16;++i){frame.decorations[i]=frame.decorations[0];frame.decorations[i].pulse_generation+=i*97;}
 for(unsigned tick=0;tick<480;tick+=17){
   for(unsigned i=0;i<16;++i)frame.decorations[i].pulse_ticks=tick;
   assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,Project,NULL,NULL,&batch));
   ActionEffectSourceBatch source={.primitives=primitives,.capacity=kActionSourceMaximumPrimitives};
   assert(ActionSceneDecorationRender_Build(&frame,kActionEffectRenderLayer_WorldDust,false,true,ActionEffectSource_ProjectPoint,ActionEffectSource_ClipBounds,&source,&batch));
   assert(!source.failed&&source.count<=kActionSourceMaximumPrimitives);
 }
 ActionSurfaceField bad=*ActionSurfaceField_Bundled(0);bad.MoltenSurface[1]=17;assert(!ActionSurfaceField_Valid(&bad));
 bad=*ActionSurfaceField_Bundled(0);bad.MoltenBursts[2]=.9f;assert(!ActionSurfaceField_Valid(&bad));
 bad=*ActionSurfaceField_Bundled(0);bad.Projection[1]=0;assert(!ActionSurfaceField_Valid(&bad));
}
int main(void){
 FILE *legacy=fopen("tests/fixtures/legacy-lava-pit-field.ini","rb");assert(legacy);
 size_t bytes=fread(text,1,sizeof(text)-1,legacy);fclose(legacy);text[bytes]=0;
 unsigned line;assert(ActionEffectRecipes_Parse(&recipes,text,bytes,&line));
 legacy_pit=recipes.surface_fields[0];
 assert(legacy_pit.MoltenSurface[0]==0&&legacy_pit.MoltenBursts[0]==0);
 assert(legacy_pit.Projection[1]==0&&legacy_pit.Receivers[0]==8);
 assert(ActionSurfaceField_Write(&legacy_pit,exported,sizeof(exported))&&strstr(exported,"molten-surface=0 "));
 LegacyOracle();
 MoltenDetail();
 const char *names[]={"lava-pit","lava-lake","splash","waterfall","waterfall-mist"};
 for(unsigned index=0;index<5;++index){
  char path[96];snprintf(path,sizeof(path),"assets/effects/%s-field.ini",names[index]);
  FILE *file=fopen(path,"rb");assert(file);size_t n=fread(text,1,sizeof(text)-1,file);fclose(file);text[n]=0;
  unsigned line;assert(ActionEffectRecipes_Parse(&recipes,text,n,&line));assert(recipes.surface_field_count==1);
  const ActionSurfaceField *field=ActionSurfaceField_Bundled(index);assert(field&&!memcmp(field,&recipes.surface_fields[0],sizeof(*field)));
  assert(ActionSurfaceField_Write(field,exported,sizeof(exported)));ActionSurfaceField copy={0};
  for(char *at=exported;*at;){char *end=strchr(at,'\n');assert(end);*end=0;char *eq=strchr(at,'=');assert(eq);*eq=0;assert(ActionSurfaceField_Set(&copy,at,eq+1));at=end+1;}
  assert(ActionSurfaceField_Valid(&copy));ActionSurfaceField_Prepare(&copy);assert(!memcmp(&copy,field,sizeof(copy)));
  ActionSurfaceField *f=&recipes.surface_fields[0];f->Mode[0]=1;f->SourceCount[0]=1;
  memcpy(f->Source1,(float[]){128,100,-32,-16,32,16,7},sizeof(f->Source1));f->Receivers[0]=7;
  ActionSurfaceField_Prepare(f);assert(ActionSurfaceField_Valid(f));
  recipes.records[0].group=6;recipes.records[0].room=2;
  ActionEnvironmentScene scene={.group=6,.room=2,.clock=456};
  frame=(ActionSceneEffectFrame){0};
  ActionEffectRecipes_Apply(&recipes,6,2,0,456,&scene,&frame);assert(frame.surface_fields_valid==(1u<<index));
  assert(frame.decoration_count==1&&frame.decorations[0].world_x==128&&frame.decorations[0].world_y==100);
  assert(frame.decorations[0].tuning.light_receivers==7&&frame.decorations[0].tuning.light_receivers_set);
  frame.decorations[0].tuning.light_receivers_set=0;unsigned vertices=0;
  for(unsigned layer=0;layer<kActionEffectRenderLayer_Count;++layer){
    assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&batch));vertices+=batch.vertex_count;}
  assert(vertices>0);unsigned previous_hash=frame.surface_fields[index].hash;
  f->Motion[1]+=1;ActionSurfaceField_Prepare(f);assert(frame.surface_fields[index].hash==previous_hash);
  ActionEffectRecipes_Apply(&recipes,6,2,0,456,&scene,&frame);assert(frame.decoration_count==1&&frame.surface_fields[index].hash!=previous_hash);
  recipes.records[0].enabled=0;ActionEffectRecipes_Apply(&recipes,6,2,0,456,&scene,&frame);
  assert(frame.surface_fields[index].Components[0]==0);vertices=0;
  for(unsigned layer=0;layer<kActionEffectRenderLayer_Count;++layer){assert(ActionSceneDecorationRender_Build(&frame,layer,true,true,Project,NULL,NULL,&batch));vertices+=batch.vertex_count;}
  assert(!vertices);
  ActionSurfaceField bad=*field;bad.Spill[8]=2;assert(!ActionSurfaceField_Valid(&bad));
  bad=*field;bad.Clock[2]=0;assert(!ActionSurfaceField_Valid(&bad));bad=*field;bad.Particles[0]=129;assert(!ActionSurfaceField_Valid(&bad));
  bad=*field;bad.Mode[0]=1;bad.SourceCount[0]=1;assert(!ActionSurfaceField_Valid(&bad));
 }
 ActionHeatRenderMesh before,after;
 ActionSurfaceField heat=*ActionSurfaceField_Bundled(1);
 assert(ActionHeatRender_Build(219,(ArRenderRectI){0,0,640,360},640,360,256,&before));
 assert(ActionHeatRender_BuildWithField(&heat,219,(ArRenderRectI){0,0,640,360},640,360,256,&after));
 assert(!memcmp(&before,&after,sizeof(before)));
 heat.HeatAmplitude[1]*=.5f;ActionSurfaceField_Prepare(&heat);
 assert(ActionHeatRender_BuildWithField(&heat,219,(ArRenderRectI){0,0,640,360},640,360,256,&after));
 assert(before.vertex_count==after.vertex_count&&before.index_count==after.index_count);
 assert(memcmp(before.vertices,after.vertices,sizeof(before.vertices)));
 puts("surface definitions: exact native geometry, source placement, ownership, toggles and heat passed");
}
