#include "action_projectile_field.h"
#include "action_effect_vectors.h"
#include "action_effects.h"
#include <stdatomic.h>
static const ActionEffectVectorProperty properties[]={
#define PROJECTILE_VECTOR(symbol,name,width,low,high) {name,offsetof(ActionProjectileField,symbol),width,low,high},
#include "action_projectile_field_properties.inc"
#undef PROJECTILE_VECTOR
};
enum {kProperties=sizeof(properties)/sizeof(properties[0])};
_Static_assert(kProperties<=64,"projectile property presence");
static bool Integer(float v,int lo,int hi){return isfinite(v)&&v>=lo&&v<=hi&&v==floorf(v);}
static ArRenderColorF Color(const float *v){return (ArRenderColorF){v[0],v[1],v[2],v[3]};}
int ActionProjectileField_Index(unsigned kind){return kind==kActionEffect_EnemyFireball?0:kind==kActionEffect_FillmoreStatueOrb?1:kind==kActionEffect_MarahnaFireball?2:kind==kActionEffect_AitosLavaFireball?3:-1;}
bool ActionProjectileField_Set(ActionProjectileField *f,const char *key,const char *value){return f&&ActionEffectVectors_Set(f,&f->seen,properties,kProperties,key,value);}
void ActionProjectileField_Prepare(ActionProjectileField *f){
  const float *values[]={f->Spill,f->Body};ActionEffectGlowStyle *styles[]={&f->spill,&f->body};
  for(unsigned i=0;i<2;++i){const float *v=values[i];*styles[i]=(ActionEffectGlowStyle){
    .radius_x=v[0],.radius_y=v[1],.ring_scale={v[2],v[3],v[4]},.centre=Color(v+5),
    .ring={Color(v+9),Color(v+13),Color(v+17)},.flare=v[21],.rise=v[22]};}
  f->hash=2166136261u;
  for(unsigned i=0;i<kProperties;++i){const float *v=ActionEffectVectors_Values(f,&properties[i]);
    for(unsigned j=0;j<properties[i].width;++j){uint32_t word;memcpy(&word,v+j,sizeof(word));f->hash=(f->hash^word)*16777619u;}}

}
bool ActionProjectileField_Valid(const ActionProjectileField *f){
  if(!f||!ActionEffectVectors_Valid(f,&f->seen,properties,kProperties)||!Integer(f->Components[0],0,3)||!Integer(f->Receivers[0],0,8))return false;
  for(unsigned i=0;i<5;++i)if(!Integer(f->Clock[i],i==1||i==4?0:1,i==2||i==3?65536:8))return false;
  if(!Integer(f->Particles[0],0,12)||!Integer(f->Particles[1],1,255)||!Integer(f->Particles[2],0,31)||!Integer(f->Particles[3],0,31)||f->Particles[1]+f->Particles[3]>255)return false;
  const float *s[]={f->Spill,f->Body},*h[]={f->RestHeading,f->OrbHeading};
  for(unsigned i=0;i<2;++i){const float *v=s[i];
    if(v[2]<=0||v[3]<=v[2]||v[4]<=v[3]||v[4]>1)return false;
    for(unsigned j=5;j<23;++j)if(v[j]>1)return false;
    const float length=h[i][0]*h[i][0]+h[i][1]*h[i][1];if(length<.99f||length>1.01f)return false;
  }
  return true;
}
size_t ActionProjectileField_Write(const ActionProjectileField *f,char *text,size_t capacity){return ActionProjectileField_Valid(f)?ActionEffectVectors_Write(f,properties,kProperties,text,capacity):0;}
const ActionProjectileField *ActionProjectileField_Bundled(unsigned index){
  if(index>=4)return NULL;
  static ActionProjectileField fields[4];static atomic_int state;
  if(atomic_load_explicit(&state,memory_order_acquire)==2)return &fields[index];
  int expected=0;if(atomic_compare_exchange_strong(&state,&expected,1)){
    static const char *const definitions[]={
#include "action_fireball_default.inc"
      ,
#include "action_orb_default.inc"
      ,
#include "action_jungle_fire_default.inc"
      ,
#include "action_lava_fire_default.inc"
    };
    bool ok=true;for(unsigned i=0;i<4&&ok;++i){
      for(const char *at=definitions[i];*at&&ok;){const char *end=strchr(at,'\n');if(!end){ok=false;break;}
        size_t n=(size_t)(end-at);char line[1024];if(n>=sizeof(line)){ok=false;break;}memcpy(line,at,n);line[n]=0;
        char *eq=strchr(line,'=');if(!eq){ok=false;break;}*eq=0;ok=ActionProjectileField_Set(&fields[i],line,eq+1);at=end+1;}
      ok=ok&&ActionProjectileField_Valid(&fields[i]);if(ok)ActionProjectileField_Prepare(&fields[i]);
    }
    atomic_store_explicit(&state,ok?2:3,memory_order_release);
  }
  while(atomic_load_explicit(&state,memory_order_acquire)==1){}
  return atomic_load_explicit(&state,memory_order_acquire)==2?&fields[index]:NULL;
}
