#include "action_arc_field.h"
#include "action_effect_vectors.h"
#include "action_effects.h"
#include <stdatomic.h>
static const ActionEffectVectorProperty properties[]={
#define ARC_VECTOR(symbol,name,width,low,high) {name,offsetof(ActionArcField,symbol),width,low,high},
#include "action_arc_field_properties.inc"
#undef ARC_VECTOR
};
enum {kProperties=sizeof(properties)/sizeof(properties[0])};
_Static_assert(kProperties<=64,"arc property presence");
static bool Integer(float v,int lo,int hi){return isfinite(v)&&v>=lo&&v<=hi&&v==floorf(v);}
static ArRenderColorF Color(const float *v){return (ArRenderColorF){v[0],v[1],v[2],v[3]};}
int ActionArcField_Index(unsigned kind){return kind==kActionEffect_LightningTrap?0:kind==kActionEffect_BloodpoolBossLightning?1:kind==kActionEffect_CentaurLightning?2:-1;}
bool ActionArcField_Set(ActionArcField *f,const char *key,const char *value){return f&&ActionEffectVectors_Set(f,&f->seen,properties,kProperties,key,value);}
void ActionArcField_Prepare(ActionArcField *f){
  const float *s[]={f->StrikeSpill,f->StrikeBody,f->BurstSpill,f->BurstBody};
  for(unsigned i=0;i<4;++i){const float *v=s[i];f->styles[i]=(ActionEffectGlowStyle){
    .radius_x=v[0],.radius_y=v[1],.ring_scale={v[4],v[5],v[6]},.centre=Color(v+7),
    .ring={Color(v+11),Color(v+15),Color(v+19)},.flare=v[23],.rise=v[24],
    .axis_x=v[25],.axis_y=v[26],.lift_x=v[27],.lift_y=v[28]};}
  const float *p[]={f->Path1,f->Path2,f->Path3,f->Path4,f->Path5,f->Path6,f->Path7,f->Path8};
  for(unsigned i=0;i<8;++i)memcpy(f->paths[i],p[i],sizeof(f->paths[i]));
  f->hash=2166136261u;
  for(unsigned i=0;i<kProperties;++i){const float *v=ActionEffectVectors_Values(f,&properties[i]);
    for(unsigned j=0;j<properties[i].width;++j){uint32_t word;memcpy(&word,v+j,sizeof(word));f->hash=(f->hash^word)*16777619u;}}

}
bool ActionArcField_Valid(const ActionArcField *f){
  if(!f||!ActionEffectVectors_Valid(f,&f->seen,properties,kProperties)||!Integer(f->Components[0],0,3)||!Integer(f->Receivers[0],0,8))return false;
  for(unsigned i=0;i<5;++i)if(!Integer(f->Clock[i],i==1||i==4?0:1,i==2||i==3?65536:8))return false;
  if(!Integer(f->Particles[0],0,12)||!Integer(f->Particles[1],1,255)||!Integer(f->Particles[2],0,31)||!Integer(f->Particles[3],0,31)||f->Particles[1]+f->Particles[3]>255)return false;
  if(f->StrikeMotion[0]>1||f->StrikeMotion[4]>1||f->Ribbon[2]>1)return false;
  const float *s[]={f->StrikeSpill,f->StrikeBody,f->BurstSpill,f->BurstBody};
  for(unsigned i=0;i<4;++i){const float *v=s[i];
    for(unsigned j=0;j<4;++j)if(v[j]<0||v[j]>(j<2?384:4))return false;
    if(v[4]<=0||v[5]<=v[4]||v[6]<=v[5]||v[6]>1)return false;
    for(unsigned j=7;j<25;++j)if(v[j]<0||v[j]>1)return false;
    float axis=v[25]*v[25]+v[26]*v[26];if(axis<.99f||axis>1.01f||fabsf(v[27])>1||fabsf(v[28])>1)return false;
  }
  const float *paths[]={f->Path1,f->Path2,f->Path3,f->Path4,f->Path5,f->Path6,f->Path7,f->Path8};
  for(unsigned i=0;i<8;++i)if(!Integer(f->PathCounts[i],i<6?2:0,25)||paths[i][2]<=0||paths[i][2]>32)return false;
  return true;
}
size_t ActionArcField_Write(const ActionArcField *f,char *text,size_t capacity){return ActionArcField_Valid(f)?ActionEffectVectors_Write(f,properties,kProperties,text,capacity):0;}
const ActionArcField *ActionArcField_Bundled(unsigned index){
  if(index>=3)return NULL;
  static ActionArcField fields[3];static atomic_int state;
  if(atomic_load_explicit(&state,memory_order_acquire)==2)return &fields[index];
  int expected=0;if(atomic_compare_exchange_strong(&state,&expected,1)){
    static const char *const definitions[]={
#include "action_trap_default.inc"
      ,
#include "action_bolt_default.inc"
      ,
#include "action_centaur_default.inc"
    };
    bool ok=true;for(unsigned i=0;i<3&&ok;++i){
      for(const char *at=definitions[i];*at&&ok;){const char *end=strchr(at,'\n');if(!end){ok=false;break;}
        size_t n=(size_t)(end-at);char line[1024];if(n>=sizeof(line)){ok=false;break;}memcpy(line,at,n);line[n]=0;
        char *eq=strchr(line,'=');if(!eq){ok=false;break;}*eq=0;ok=ActionArcField_Set(&fields[i],line,eq+1);at=end+1;}
      ok=ok&&ActionArcField_Valid(&fields[i]);if(ok)ActionArcField_Prepare(&fields[i]);
    }
    atomic_store_explicit(&state,ok?2:3,memory_order_release);
  }
  while(atomic_load_explicit(&state,memory_order_acquire)==1){}
  return atomic_load_explicit(&state,memory_order_acquire)==2?&fields[index]:NULL;
}
