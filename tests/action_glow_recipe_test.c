#include "action/action_effect_render.h"
#include "support/test_assert.h"
#include "action/action_effect_recipes.h"
#include "action/action_environment_scene.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static ActionSceneEffectFrame frame;
static ActionSceneEffectRenderBatch batch;
static unsigned hash=2166136261u;
static void Word(unsigned v){hash=(hash^v)*16777619u;}
static void Float(float v){unsigned b;memcpy(&b,&v,4);Word(b);}
static bool Project(void *ctx,const ActionEffectInstance *e,float x,float y,ArRenderPointF *p){(void)ctx;*p=(ArRenderPointF){e->world_x+x,e->world_y+y};return true;}
static bool Clip(void *ctx,const ActionEffectInstance *e,ActionEffectLocalRect *r){const int *c=ctx;*r=(ActionEffectLocalRect){c[0]-e->world_x,c[1]-e->world_y,c[0]+512-e->world_x,c[1]+352-e->world_y};return true;}
static void LegacyOracle(void){unsigned long vertices=0,indices=0;clock_t start=clock();
 for(unsigned room=0;room<3;++room)for(unsigned i=0;i<240;++i){
 memset(&frame,0,sizeof(frame));frame.glow_field=*ActionGlowField_Bundled(2,3);frame.glow_field_valid=true;frame.decoration_count=frame.decoration_visible_count=3;
 const int camera[]={0,0};
 for(unsigned j=0;j<3;++j)frame.decorations[j]=(ActionEffectInstance){
 .kind=kActionEffect_WallTorch,.phase=kActionEffectPhase_WallTorch,
 .generation=0x54000000u^(j*195719),.pulse_generation=0x74000000u^(j*195719),
 .world_x=80+j*128,.world_y=90+j*64,.phase_ticks=i*97,.pulse_ticks=i*97,
 .flags=kActionEffectFlag_Visible,.render_layer=kActionEffectRenderLayer_Bg1Plane,.projection_plane=kActionEffectProjectionPlane_Bg1,
 .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-5,-9,5,room==1?5:2}},
 .tuning={.active=room==2,.intensity=1,.reach=room==2?2:1,.color=0xFFFFFF}};
 for(unsigned layer=0;layer<kActionEffectRenderLayer_Count;++layer){
 assert(ActionSceneDecorationRender_Build(&frame,layer,i%4!=1,i%4!=2,Project,Clip,(void*)camera,&batch));
 vertices+=batch.vertex_count;indices+=batch.index_count;Word(batch.vertex_count);Word(batch.index_count);
 for(int v=0;v<batch.vertex_count;++v){const ArRenderVertex2D *a=&batch.vertices[v];Float(a->position.x);Float(a->position.y);Float(a->color.r);Float(a->color.g);Float(a->color.b);Float(a->color.a);}
 for(int n=0;n<batch.index_count;++n)Word(batch.indices[n]);
 }
 }
 assert(vertices==359640&&indices==1623240);
#if defined(__OPTIMIZE__)
 assert(hash==0x5ecc21e2u);
#endif
 printf("%08x %lu %lu %.6f ms/frame\n",hash,vertices,indices,(double)(clock()-start)*1000/CLOCKS_PER_SEC/720);
}

static ActionEffectRecipes recipes;
static ActionSceneEffectFrame native_frame;
static char text[16384],exported[16384];
static uint8_t map[256];
int main(void){
  LegacyOracle();
  const char *names[]={"torch","temple"};
  for(unsigned variant=0;variant<2;++variant){
    char path[128];snprintf(path,sizeof(path),"assets/effects/%s-glow-field.ini",names[variant]);
    FILE *file=fopen(path,"rb");assert(file);size_t n=fread(text,1,sizeof(text)-1,file);fclose(file);text[n]=0;
    unsigned line;assert(ActionEffectRecipes_Parse(&recipes,text,n,&line));
    const unsigned group=variant?5:2,room=variant?4:3;
    const ActionGlowField *f=ActionGlowField_Bundled(group,room);assert(f);
    assert(recipes.glow_field_count==1&&!memcmp(&recipes.glow_fields[0],f,sizeof(*f)));
    assert(ActionGlowField_Write(f,exported,sizeof(exported)));ActionGlowField copy={0};
    for(char *at=exported;*at;){char *end=strchr(at,'\n');assert(end);*end=0;char *eq=strchr(at,'=');assert(eq);*eq=0;assert(ActionGlowField_Set(&copy,at,eq+1));at=end+1;}
    assert(ActionGlowField_Valid(&copy));ActionGlowField_Prepare(&copy);assert(!memcmp(&copy,f,sizeof(copy)));
    memset(map,0,sizeof(map));map[2+3*16]=(uint8_t)f->MapRule[0];map[2+4*16]=(uint8_t)f->MapRule[1];
    ActionEnvironmentScene scene={.group=group,.room=room,.clock=731,
      .maps={{.map=map,.map_size=sizeof(map),.world_width=256,.world_height=256,.pages_wide=1}}};
    memset(&native_frame,0,sizeof(native_frame));ActionGlowField_Capture(&scene,&native_frame,f,0x54000000);
    assert(native_frame.glow_field_valid&&native_frame.decoration_count==1);
    assert(native_frame.decorations[0].world_x==32+f->Anchor[0]);assert(native_frame.decorations[0].world_y==48+f->Anchor[1]);
    assert(native_frame.decorations[0].generation==(0x54000000u^(3u<<16)^2));
    memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,group,room,0,731,&scene,&frame);
    assert(frame.decoration_count==1&&!memcmp(frame.decorations,native_frame.decorations,sizeof(frame.decorations[0])));
    assert(!memcmp(&frame.glow_field,&native_frame.glow_field,sizeof(frame.glow_field)));
    recipes.records[0].group=6;recipes.records[0].room=2;scene.group=6;scene.room=2;
    ActionGlowField *edited=&recipes.glow_fields[0];edited->Mode[0]=1;edited->SourceCount[0]=2;
    memcpy(edited->Source1,(float[]){80,90,1},sizeof(edited->Source1));memcpy(edited->Source2,(float[]){160,240,2},sizeof(edited->Source2));
    edited->Receivers[0]=6;edited->Clock[0]=1;edited->SpillRadius[0]=70;assert(ActionGlowField_Valid(edited));ActionGlowField_Prepare(edited);
    memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,6,2,0,731,&scene,&frame);
    assert(frame.decoration_count==2&&frame.decorations[1].world_x==160&&frame.decorations[1].world_y==240);
    assert(frame.decorations[0].tuning.light_receivers_set&&frame.decorations[0].tuning.light_receivers==6);
    assert(frame.glow_field.spill.radius_x==70);edited->SpillRadius[0]=100;assert(frame.glow_field.SpillRadius[0]==70);
    recipes.records[0].enabled=0;ActionEffectRecipes_Apply(&recipes,6,2,0,731,&scene,&frame);
    assert(!frame.glow_field_valid&&!frame.decoration_count);
    ActionGlowField bad=*f;bad.Clock[2]=0;assert(!ActionGlowField_Valid(&bad));
    bad=*f;bad.Particles[0]=8;assert(!ActionGlowField_Valid(&bad));
    bad=*f;bad.SourceCount[0]=2;assert(!ActionGlowField_Valid(&bad));
    bad=*f;bad.BodyFlame[2]=.5f;assert(!ActionGlowField_Valid(&bad));
    bad=*f;bad.SpillRings[2]=bad.SpillRings[1];assert(!ActionGlowField_Valid(&bad));
  }
  assert(!ActionGlowField_Bundled(1,1));assert(ActionGlowField_Bundled(7,6));
  puts("Complete glow reconstruction, explicit placement, ownership and bounded recipes passed");return 0;
}
