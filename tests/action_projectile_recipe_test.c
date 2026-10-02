#include "action/action_effect_render.h"
#include "support/test_assert.h"
#include "action/action_effect_recipes.h"
#include <stdio.h>
#include <string.h>
static ActionSceneEffectFrame frame;
static ActionSceneEffectRenderBatch batch;
static unsigned hash=2166136261u;
static void Word(unsigned v){hash=(hash^v)*16777619u;}
static void Float(float v){unsigned b;memcpy(&b,&v,4);Word(b);}
static bool Project(void *ctx,const ActionEffectInstance *e,float x,float y,ArRenderPointF *p){(void)ctx;*p=(ArRenderPointF){e->world_x+x,e->world_y+y};return true;}
static void LegacyOracle(void){unsigned long vertices=0,indices=0;
 for(unsigned mode=0;mode<4;++mode)for(unsigned heading=0;heading<5;++heading)for(unsigned i=0;i<80;++i){
 frame=(ActionSceneEffectFrame){.effect_count=1,.visible_count=1};
 const unsigned kinds[]={kActionEffect_EnemyFireball,kActionEffect_FillmoreStatueOrb,kActionEffect_MarahnaFireball,kActionEffect_AitosLavaFireball};
 const unsigned phases[]={kActionEffectPhase_EnemyFireballFlight,kActionEffectPhase_EnemyFireballFlight,kActionEffectPhase_MarahnaFireballOrb,kActionEffectPhase_AitosLavaFireballFlight};
 frame.effects[0]=(ActionEffectInstance){.kind=kinds[mode],.phase=phases[mode],.visual=mode==1?0x1b:mode==2?8:0,
 .generation=0x1234,.record_address=0x620,.pulse_generation=0x7364,.velocity_x=heading==1?-256:heading==2?256:0,.velocity_y=heading==3?-256:heading==4?256:0,
 .world_x=128,.world_y=100,.phase_ticks=i*101,.pulse_ticks=i*79,.flags=kActionEffectFlag_Visible,
 .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-8,-8,8,8}}};
 assert(ActionSceneEffectRender_Build(&frame,i%4!=1,i%4!=2,Project,NULL,&batch));
 vertices+=batch.vertex_count;indices+=batch.index_count;Word(batch.vertex_count);Word(batch.index_count);
 for(int v=0;v<batch.vertex_count;++v){const ArRenderVertex2D *a=&batch.vertices[v];Float(a->position.x);Float(a->position.y);Float(a->color.r);Float(a->color.g);Float(a->color.b);Float(a->color.a);}
 for(int n=0;n<batch.index_count;++n)Word(batch.indices[n]);
 }
 assert(vertices==283200&&indices==1227600);
#if defined(__OPTIMIZE__)
 assert(hash==0x6cc4d63eu);
#endif
 printf("%08x %lu %lu\n",hash,vertices,indices);
}

static ActionEffectRecipes recipes;
static char text[16384],exported[16384];
int main(void){
 LegacyOracle();
 const char *names[]={"fireball","orb","jungle-fire","lava-fire"};
 for(unsigned index=0;index<4;++index){
  char path[96];snprintf(path,sizeof(path),"assets/effects/%s-field.ini",names[index]);
  FILE *file=fopen(path,"rb");assert(file);size_t n=fread(text,1,sizeof(text)-1,file);fclose(file);text[n]=0;
  unsigned line;assert(ActionEffectRecipes_Parse(&recipes,text,n,&line));assert(recipes.projectile_field_count==1);
  const ActionProjectileField *field=ActionProjectileField_Bundled(index);assert(field&&!memcmp(field,&recipes.projectile_fields[0],sizeof(*field)));
  assert(ActionProjectileField_Write(field,exported,sizeof(exported)));ActionProjectileField copy={0};
  for(char *at=exported;*at;){char *end=strchr(at,'\n');assert(end);*end=0;char *eq=strchr(at,'=');assert(eq);*eq=0;assert(ActionProjectileField_Set(&copy,at,eq+1));at=end+1;}
  assert(ActionProjectileField_Valid(&copy));ActionProjectileField_Prepare(&copy);assert(!memcmp(&copy,field,sizeof(copy)));
  frame=(ActionSceneEffectFrame){.effect_count=1,.visible_count=1};
  const unsigned phases[]={kActionEffectPhase_EnemyFireballFlight,kActionEffectPhase_EnemyFireballFlight,kActionEffectPhase_MarahnaFireballOrb,kActionEffectPhase_AitosLavaFireballFlight};
  frame.effects[0]=(ActionEffectInstance){.kind=recipes.records[0].kind,.phase=phases[index],
   .visual=index==1?0x1b:index==2?8:0,.phase_ticks=123,.pulse_ticks=456,.flags=kActionEffectFlag_Visible,
   .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-8,-8,8,8}}};
  recipes.records[0].group=6;recipes.records[0].room=2;
  ActionEffectRecipes_Apply(&recipes,6,2,0,456,NULL,&frame);assert(frame.projectile_fields_valid==(1u<<index));
  assert(!memcmp(&frame.projectile_fields[index],field,sizeof(*field)));
  assert(ActionSceneEffectRender_Build(&frame,true,true,Project,NULL,&batch));assert(batch.vertex_count>0);
  recipes.projectile_fields[0].Receivers[0]=6;recipes.projectile_fields[0].Spill[0]=120;ActionProjectileField_Prepare(&recipes.projectile_fields[0]);
  ActionEffectRecipes_Apply(&recipes,6,2,0,456,NULL,&frame);
  assert(frame.effects[0].tuning.light_receivers==6&&frame.effects[0].tuning.light_receivers_set);
  assert(frame.projectile_fields[index].spill.radius_x==120);
  recipes.projectile_fields[0].Spill[0]=20;assert(frame.projectile_fields[index].spill.radius_x==120);
  frame.effects[0].tuning.light_receivers_set=0;recipes.records[0].enabled=0;
  ActionEffectRecipes_Apply(&recipes,6,2,0,456,NULL,&frame);assert(frame.projectile_fields[index].Components[0]==0);
  assert(ActionSceneEffectRender_Build(&frame,true,true,Project,NULL,&batch));assert(!batch.vertex_count);
  recipes.count=0;ActionEffectRecipes_Apply(&recipes,6,2,0,456,NULL,&frame);assert(!frame.projectile_fields_valid);
  ActionProjectileField bad=*field;bad.RestHeading[0]=2;assert(!ActionProjectileField_Valid(&bad));
  bad=*field;bad.Spill[4]=2;assert(!ActionProjectileField_Valid(&bad));bad=*field;bad.Clock[2]=0;assert(!ActionProjectileField_Valid(&bad));
  bad=*field;bad.Spill[7]=2;assert(!ActionProjectileField_Valid(&bad));bad=*field;bad.Particles[0]=13;assert(!ActionProjectileField_Valid(&bad));
 }
 assert(!ActionProjectileField_Bundled(4));puts("Complete fireball profiles, scoped binding, ownership, export/reload and bounds passed");return 0;
}
