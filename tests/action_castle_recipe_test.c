#include "action/action_effect_render.h"
#include "support/test_assert.h"
#include "action/action_environment_scene.h"
#include "action/action_effect_recipes.h"
#include "action/action_environment_exposure.h"
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
static void LegacyOracle(bool masonry){unsigned long vertices=0,indices=0;clock_t start=clock();hash=2166136261u;
 for(unsigned room=2;room<=8;++room)for(unsigned i=0;i<120;++i){
 memset(&frame,0,sizeof(frame));frame.castle_field=*ActionCastleField_Bundled(room);frame.castle_field_valid=true;frame.decoration_count=frame.decoration_visible_count=3;
 const int camera[]={room==5?(int)(i%12)*96:(int)(i%6)*96,room==3||room==5||room==7?(int)((i/6)%5)*160:0};
 /* These are receiving-surface highlights. The prior visual oracle must also
  * hold with a populated live scenery mask, not just synthetic empty space.
  * Otherwise a new shadow pass can quietly erase arch/sill/gallery details. */
 if(masonry){frame.scenery.valid=true;frame.scenery.count=1;
   frame.scenery.rectangles[0]=(ActionMoonlightOccluder){0,0,2048,2048};}
 frame.decorations[0]=(ActionEffectInstance){.kind=kActionEffect_CastleLight,.phase=kActionEffectPhase_CastleEnvironment,.environment_room=room,
 .world_x=camera[0]+128,.world_y=camera[1]-160,.source_mask=(uint16_t)(i%7?65535:0xAAAA),.phase_ticks=i*197,.pulse_ticks=i*197,
 .flags=kActionEffectFlag_Visible|kActionEffectFlag_ClipToRect,.render_layer=kActionEffectRenderLayer_Bg1Plane,.projection_plane=kActionEffectProjectionPlane_Bg1,
 .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-384,0,384,544}},.clip_rect={-384,0,384,544}};
 frame.decorations[1]=frame.decorations[0];frame.decorations[1].kind=kActionEffect_CastleMist;frame.decorations[1].world_y=room==5?976:208;
 frame.decorations[1].render_layer=kActionEffectRenderLayer_Bg1Mist;frame.decorations[1].geometry.data.rect=frame.decorations[1].clip_rect=(ActionEffectLocalRect){-384,-18,384,0};
 frame.decorations[2]=frame.decorations[0];frame.decorations[2].kind=room==5?kActionEffect_CastleWater:kActionEffect_CastleSky;
 frame.decorations[2].world_x=room==5?camera[0]+128:room<6?112:128;frame.decorations[2].world_y=room==5?944:room<6?62:48;
 frame.decorations[2].geometry.data.rect=frame.decorations[2].clip_rect=room==5?(ActionEffectLocalRect){-384,0,384,16}:(ActionEffectLocalRect){-384,-48,384,208};
 frame.decorations[2].render_layer=room==5?kActionEffectRenderLayer_Bg1Plane:kActionEffectRenderLayer_Bg2Plane;
 for(unsigned layer=0;layer<kActionEffectRenderLayer_Count;++layer){
 assert(ActionSceneDecorationRender_Build(&frame,layer,i%4!=1,i%4!=2,Project,Clip,(void*)camera,&batch));
 vertices+=batch.vertex_count;indices+=batch.index_count;Word(batch.vertex_count);Word(batch.index_count);
 for(int v=0;v<batch.vertex_count;++v){const ArRenderVertex2D *a=&batch.vertices[v];Float(a->position.x);Float(a->position.y);Float(a->color.r);Float(a->color.g);Float(a->color.b);Float(a->color.a);}
 for(int n=0;n<batch.index_count;++n)Word(batch.indices[n]);
 }
 }
 assert(vertices==260256&&indices==906489);
#if defined(__OPTIMIZE__)
 assert(hash==0xd6a30baau);
#endif
 printf("%08x %lu %lu %.6f ms/frame\n",hash,vertices,indices,(double)(clock()-start)*1000/CLOCKS_PER_SEC/840);
}

static ActionEffectRecipes recipes,previous;
static ActionSceneEffectFrame native_frame;
static uint8_t map[32768],sky[32768],words[2048];
static char text[16384],roundtrip[16384];
static unsigned At(unsigned x,unsigned y,unsigned width){return ((y/256)*(width/256)+x/256)*256+(y%256)/16*16+(x%256)/16;}
static void Put(ActionEnvironmentScene *s,unsigned bg,int x,int y,unsigned tile){
  if(x>=0&&y>=0&&x<s->maps[bg].world_width&&y<s->maps[bg].world_height)
    (bg?sky:map)[At((unsigned)x,(unsigned)y,(unsigned)s->maps[bg].world_width)]=(uint8_t)tile;
}
static void ConfigureScene(ActionEnvironmentScene *s,const ActionCastleField *f,unsigned room){
  memset(s,0,sizeof(*s));memset(map,0,sizeof(map));memset(sky,0,sizeof(sky));memset(words,0,sizeof(words));
  s->group=2;s->room=room;s->clock=743;s->camera_x[0]=112;s->camera_y[0]=96;
  for(unsigned bg=0;bg<2;++bg){s->maps[bg]=(ActionBgMapView){bg?sky:map,sizeof(map),f->Dimensions[bg*2],f->Dimensions[bg*2+1],(unsigned)f->Dimensions[bg*2]/256};s->metatiles[bg]=words;}
  const float *witness[]={f->Witness1,f->Witness2,f->SkyWitness1,f->SkyWitness2};
  for(unsigned i=0;i<4;++i)Put(s,witness[i][0],witness[i][1],witness[i][2],witness[i][3]);
  for(unsigned q=0;q<4;++q){unsigned at=(unsigned)f->Material[0]*8+q*2,word=f->Material[q+1];words[at]=word;words[at+1]=word>>8;}
  for(unsigned i=0;i<(unsigned)f->SourceCount[0];++i){const ActionCastleSource *v=&f->sources[i];
    Put(s,0,v->check_x,v->check_y,v->top_tile);Put(s,0,v->check_x,v->check_y+16,v->below_tile);
    if(v->kind==kActionCastleSource_Window)Put(s,0,v->x,v->sill-1,v->sill_tile);}
}
int main(void){
  for(unsigned room=2;room<=8;++room)assert(ActionCastleField_Bundled(room));
  LegacyOracle(false);
  LegacyOracle(true);
  for(unsigned room=2;room<=8;++room){
    const ActionCastleField *f=ActionCastleField_Bundled(room);char file[96];snprintf(file,sizeof(file),"assets/effects/castle-%u-field.ini",room);
    FILE *in=fopen(file,"rb");assert(in);size_t n=fread(text,1,sizeof(text)-1,in);fclose(in);text[n]=0;
    unsigned line;assert(ActionEffectRecipes_Parse(&recipes,text,n,&line));assert(recipes.castle_field_count==1);
    assert(!memcmp(&recipes.castle_fields[0],f,sizeof(*f)));
    size_t used=0;
    for(const char *at=text;*at;){const char *end=strchr(at,'\n');size_t count=end?(size_t)(end-at)+1:strlen(at);
      if(strncmp(at,"dimming=",8)&&strncmp(at,"dimming-ramp=",13)&&strncmp(at,"receivers=",10)){memcpy(roundtrip+used,at,count);used+=count;}
      at+=count;}
    roundtrip[used]=0;assert(ActionEffectRecipes_Parse(&recipes,roundtrip,used,&line));
    assert(!memcmp(&recipes.castle_fields[0],f,sizeof(*f))); /* version-1 upgrade */
    assert(ActionCastleField_Write(f,roundtrip,sizeof(roundtrip)));ActionCastleField copy={0};
    for(char *at=roundtrip;*at;){char *end=strchr(at,'\n');assert(end);*end=0;char *eq=strchr(at,'=');assert(eq);*eq=0;assert(ActionCastleField_Set(&copy,at,eq+1));at=end+1;}
    assert(ActionCastleField_Valid(&copy));ActionCastleField_Prepare(&copy);assert(!memcmp(&copy,f,sizeof(copy)));
    ActionEnvironmentScene scene;ConfigureScene(&scene,f,room);memset(&native_frame,0,sizeof(native_frame));
    ActionCastleField_Capture(&scene,&native_frame,f,0xca000000u|room);assert(native_frame.castle_field_valid);
    memset(&frame,0,sizeof(frame));scene.suppress_default_castle_field=true;
    ActionEffectRecipes_Apply(&recipes,2,room,0,743,&scene,&frame);
    assert(frame.castle_field_valid&&frame.decoration_count==native_frame.decoration_count);
    assert(!memcmp(frame.decorations,native_frame.decorations,frame.decoration_count*sizeof(frame.decorations[0])));
    assert(!memcmp(&frame.castle_field,&native_frame.castle_field,sizeof(frame.castle_field)));
    // A copied definition is reusable in another room; source style and seeds
    // no longer come from its room number. The complete field owns its sky.
    recipes.records[0].group=5;recipes.records[0].room=3;scene.group=5;scene.room=3;
    memset(&frame,0,sizeof(frame));ActionEffectRecipes_Apply(&recipes,5,3,0,743,&scene,&frame);
    assert(frame.castle_field_valid&&frame.decoration_count==native_frame.decoration_count);
    assert(!memcmp(&frame.castle_field,&native_frame.castle_field,sizeof(frame.castle_field)));
    if(frame.decoration_count&&room==3){
      recipes.castle_fields[0].Dimming[0]=.65f;recipes.castle_fields[0].Receivers[0]=6;recipes.castle_fields[0].Receivers[1]=0;
      memcpy(recipes.castle_fields[0].DimmingRamp,(float[]){10,20,30,40},4*sizeof(float));
      ActionEffectRecipes_Apply(&recipes,5,3,0,743,&scene,&frame);
      assert(ActionEnvironment_NativeBg1Dimming(&frame,5,3)==.65f);
      assert(ActionEnvironment_Bg1Dimming(&frame,5,3)==0);
      const ArRenderRectF ramp=ActionEnvironment_Bg1DimmingRamp(&frame,5,3);assert(ramp.x==10&&ramp.y==20&&ramp.w==30&&ramp.h==40);
      assert(frame.decorations[0].tuning.dim_receivers==6&&frame.decorations[0].tuning.light_receivers==0);
    }
    recipes.castle_fields[0].Opening[0]=0;assert(frame.castle_field.Opening[0]==f->Opening[0]);
    recipes.records[0].enabled=0;ActionEffectRecipes_Apply(&recipes,5,3,0,743,&scene,&frame);assert(!frame.castle_field_valid&&!frame.decoration_count);
  }
  ActionCastleField bad=*ActionCastleField_Bundled(3);bad.DustPeriods[0]=17;assert(!ActionCastleField_Valid(&bad));
  bad=*ActionCastleField_Bundled(3);bad.SourceCount[0]=17;assert(!ActionCastleField_Valid(&bad));
  bad=*ActionCastleField_Bundled(3);bad.FanPattern[4]=0;assert(!ActionCastleField_Valid(&bad));
  bad=*ActionCastleField_Bundled(3);bad.MistCells[0]=1;assert(!ActionCastleField_Valid(&bad));
  bad=*ActionCastleField_Bundled(3);bad.Source2[16]=bad.Source1[16];assert(!ActionCastleField_Valid(&bad));
  previous=recipes;const char *invalid="[effects]\nversion=1\n[source:02:03:0:castle-field:1]\n";
  unsigned line;assert(!ActionEffectRecipes_Parse(&recipes,invalid,strlen(invalid),&line)&&!memcmp(&recipes,&previous,sizeof(recipes)));
  puts("castle baseline, seven complete definitions, config-only reconstruction, independent ownership and bounded rejection passed");
}
