#include "action_surface_field.h"
#include "action_effect_vectors.h"
#include "action_effects.h"
#include <stdatomic.h>
static const ActionEffectVectorProperty properties[]={
#define SURFACE_VECTOR(symbol,name,width,low,high) {name,offsetof(ActionSurfaceField,symbol),width,low,high},
#include "action_surface_field_properties.inc"
#undef SURFACE_VECTOR
};
enum {kProperties=sizeof(properties)/sizeof(properties[0])};
_Static_assert(kProperties<=64,"surface property presence");
static bool Integer(float v,int lo,int hi){return isfinite(v)&&v>=lo&&v<=hi&&v==floorf(v);}
static ArRenderColorF Color(const float *v){return (ArRenderColorF){v[0],v[1],v[2],v[3]};}
int ActionSurfaceField_Index(unsigned kind){switch(kind){
case kActionEffect_AitosLavaPit:return 0;case kActionEffect_AitosLavaReservoir:return 1;
case kActionEffect_AitosWaterSplash:return 2;case kActionEffect_AitosWaterfall:return 3;
case kActionEffect_AitosWaterfallMist:return 4;default:return -1;}}
bool ActionSurfaceField_Set(ActionSurfaceField *f,const char *key,const char *value){return f&&ActionEffectVectors_Set(f,&f->seen,properties,kProperties,key,value);}
void ActionSurfaceField_Prepare(ActionSurfaceField *f){
  const float *values[]={f->Spill,f->Body};
  for(unsigned i=0;i<2;++i){const float *v=values[i];f->styles[i]=(ActionEffectGlowStyle){
    .radius_x=v[0],.radius_y=v[1],.ring_scale={v[6],v[7],v[8]},.centre=Color(v+9),
    .ring={Color(v+13),Color(v+17),Color(v+21)},.flare=v[25],.rise=v[26],
    .axis_x=v[27],.axis_y=v[28],.lift_x=v[29],.lift_y=v[30]};}
  const float *sources[]={f->Source1,f->Source2,f->Source3,f->Source4,f->Source5,f->Source6,f->Source7,f->Source8,f->Source9,f->Source10,f->Source11,f->Source12,f->Source13,f->Source14,f->Source15,f->Source16};
  const float *tiers[]={f->Tier1,f->Tier2,f->Tier3,f->Tier4};
  for(unsigned i=0;i<16;++i)memcpy(f->sources[i],sources[i],sizeof(f->sources[i]));
  for(unsigned i=0;i<4;++i)memcpy(f->tiers[i],tiers[i],sizeof(f->tiers[i]));
  f->hash=2166136261u;
  for(unsigned i=0;i<kProperties;++i){const float *v=ActionEffectVectors_Values(f,&properties[i]);
    for(unsigned j=0;j<properties[i].width;++j){uint32_t word;memcpy(&word,v+j,sizeof(word));f->hash=(f->hash^word)*16777619u;}}
}
bool ActionSurfaceField_Valid(const ActionSurfaceField *f){
  if(!f||!ActionEffectVectors_Valid(f,&f->seen,properties,kProperties)||!Integer(f->Kind[0],0,4)||!Integer(f->Components[0],0,3)||!Integer(f->Receivers[0],0,8)||!Integer(f->Mode[0],0,1))return false;
  if(!Integer(f->Heat[0],0,1)||f->HeatAmplitude[0]>f->HeatAmplitude[2]||f->HeatWave[2]<0||f->HeatWave[2]>1||f->HeatCross[2]<0||f->HeatCross[2]>1)return false;
  const unsigned kind=(unsigned)f->Kind[0];const unsigned max_particles[]={12,32,12,128,32};
  const unsigned max_sources[]={16,4,14,1,1};
  if(!Integer(f->SourceCount[0],0,max_sources[kind])||!Integer(f->Projection[0],0,kActionEffectProjectionPlane_BetweenBackgrounds)||!Integer(f->Projection[1],0,kActionEffectRenderLayer_Count-1))return false;
  for(unsigned i=0;i<16;++i)if(!Integer(f->MapRule[i],0,255))return false;
  if(kind==0&&!Integer(f->MapRule[5],1,6))return false;
  if(kind==1&&(!Integer(f->MapRule[5],3,64)||!Integer(f->MapRule[6],3,64)||f->MapRule[6]>=f->MapRule[5]))return false;
  if(kind==2&&(!Integer(f->MapRule[9],2,8)||!Integer(f->MapRule[10],1,14)))return false;
  if(f->Bounds[0]>=f->Bounds[2]||f->Bounds[1]>=f->Bounds[3])return false;
  for(unsigned i=0;i<4;++i)if(!Integer(f->Identity[i],0,65535))return false;
  for(unsigned i=0;i<5;++i)if(!Integer(f->Clock[i],i==1||i==4?0:1,i==2||i==3?65536:8))return false;
  if(!Integer(f->Particles[0],0,max_particles[kind])||!Integer(f->Particles[1],1,128)||!Integer(f->Particles[2],0,31)||!Integer(f->Particles[3],0,31)||f->Particles[1]+f->Particles[3]>255)return false;
  if(kind==3&&(!Integer(f->Motion[0],1,32)||f->Motion[4]<=0||f->Motion[5]<0))return false;
  if(!Integer(f->CloudCounts[0],1,6)||!Integer(f->CloudCounts[1],1,4))return false;
  for(unsigned i=0;i<8;++i)if(!Integer(f->CloudPeriods[i],i==0||i==3||i==6?1:0,i==7?1:65536))return false;
  if(f->CloudShape[2]<=0)return false;
  const float *styles[]={f->Spill,f->Body};
  for(unsigned i=0;i<2;++i){const float *v=styles[i];
    for(unsigned j=0;j<6;++j)if(v[j]<0)return false;
    if(v[6]<=0||v[7]<=v[6]||v[8]<=v[7]||v[8]>1)return false;
    for(unsigned j=9;j<27;++j)if(v[j]<0||v[j]>1)return false;
    for(unsigned j=27;j<31;++j)if(v[j]<-1||v[j]>1)return false;
  }
  const float *sources[]={f->Source1,f->Source2,f->Source3,f->Source4,f->Source5,f->Source6,f->Source7,f->Source8,f->Source9,f->Source10,f->Source11,f->Source12,f->Source13,f->Source14,f->Source15,f->Source16};
  unsigned segments=0;
  for(unsigned i=0;i<(unsigned)f->SourceCount[0];++i){const float *v=sources[i];
    if(!Integer(v[0],-2048,16384)||!Integer(v[1],-2048,16384)||!Integer(v[6],0,16384)||v[2]>=v[4]||v[3]>=v[5])return false;
    for(unsigned j=2;j<6;++j)if(v[j]<-2048||v[j]>2048)return false;
    if(kind==1){segments+=(unsigned)ceilf((v[4]-v[2])/f->Segments[0]);if(segments>12)return false;}
    for(unsigned j=0;j<i;++j)if(v[6]==sources[j][6])return false;
  }
  const float *tiers[]={f->Tier1,f->Tier2,f->Tier3,f->Tier4};
  for(unsigned i=0;i<4;++i){const float *v=tiers[i];if(v[1]<=0||v[2]<=0||v[3]<0||v[3]>1)return false;
    for(unsigned j=5;j<9;++j)if(v[j]<0||v[j]>1)return false;}
  return true;
}
size_t ActionSurfaceField_Write(const ActionSurfaceField *f,char *text,size_t capacity){return ActionSurfaceField_Valid(f)?ActionEffectVectors_Write(f,properties,kProperties,text,capacity):0;}
const ActionSurfaceField *ActionSurfaceField_Bundled(unsigned index){
  if(index>=5)return NULL;
  static ActionSurfaceField fields[5];static atomic_int state;
  if(atomic_load_explicit(&state,memory_order_acquire)==2)return &fields[index];
  int expected=0;if(atomic_compare_exchange_strong(&state,&expected,1)){
    static const char *const definitions[]={
#include "action_lava_pit_default.inc"
      ,
#include "action_lava_lake_default.inc"
      ,
#include "action_splash_default.inc"
      ,
#include "action_waterfall_default.inc"
      ,
#include "action_waterfall_mist_default.inc"
    };
    bool ok=true;for(unsigned i=0;i<5&&ok;++i){
      for(const char *at=definitions[i];*at&&ok;){const char *end=strchr(at,'\n');if(!end){ok=false;break;}
        size_t n=(size_t)(end-at);char line[1024];if(n>=sizeof(line)){ok=false;break;}memcpy(line,at,n);line[n]=0;
        char *eq=strchr(line,'=');if(!eq){ok=false;break;}*eq=0;ok=ActionSurfaceField_Set(&fields[i],line,eq+1);at=end+1;}
      ok=ok&&ActionSurfaceField_Valid(&fields[i]);if(ok)ActionSurfaceField_Prepare(&fields[i]);
    }
    atomic_store_explicit(&state,ok?2:3,memory_order_release);
  }
  while(atomic_load_explicit(&state,memory_order_acquire)==1){}
  return atomic_load_explicit(&state,memory_order_acquire)==2?&fields[index]:NULL;
}
