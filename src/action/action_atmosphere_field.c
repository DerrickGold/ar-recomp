#include "action_atmosphere_field.h"
#include "action_effect_vectors.h"
#include <stdatomic.h>
static const ActionEffectVectorProperty properties[]={
#define ATMOSPHERE_VECTOR(symbol,name,width,low,high) {name,offsetof(ActionAtmosphereField,symbol),width,low,high},
#include "action_atmosphere_field_properties.inc"
#undef ATMOSPHERE_VECTOR
};
enum { kProperties=sizeof(properties)/sizeof(properties[0]) };
_Static_assert(kProperties<=128,"complete property presence fits the field mask");
bool ActionAtmosphereField_Set(ActionAtmosphereField *f,const char *key,const char *value) {
  return f&&ActionEffectVectors_Set(f,f->seen,properties,kProperties,key,value);
}
void ActionAtmosphereField_Prepare(ActionAtmosphereField *f) {
  if(!f)return;
  const float *ambient[]={f->Ambient1,f->Ambient2,f->Ambient3,f->Ambient4,f->Ambient5,f->Ambient6,f->Ambient7,f->Ambient8};
  const float *areas[]={f->Area1,f->Area2,f->Area3};
  const float *grit[]={f->Grit1,f->Grit2,f->Grit3,f->Grit4};
  const float *tower[]={f->Tower1,f->Tower2,f->Tower3};
  for(unsigned i=0;i<8;++i)memcpy(f->ambient[i],ambient[i],sizeof(f->ambient[i]));
  for(unsigned i=0;i<3;++i)memcpy(f->areas[i],areas[i],sizeof(f->areas[i]));
  for(unsigned i=0;i<4;++i)memcpy(f->grit[i],grit[i],sizeof(f->grit[i]));
  for(unsigned i=0;i<3;++i)memcpy(f->tower[i],tower[i],sizeof(f->tower[i]));
}
const float *ActionAtmosphereField_Witness(const ActionAtmosphereField *f,unsigned i) {
  switch(i) {
    case 0: return f->Witness1;
    case 1: return f->Witness2;
    case 2: return f->Witness3;
    case 3: return f->Witness4;
    default: return NULL;
  }
}
static bool Integer(float v,unsigned low,unsigned high) {
  return v>=low&&v<=high&&floorf(v)==v;
}
static bool Period(float v,unsigned high) {
  if(!Integer(v,16,high))return false;unsigned n=(unsigned)v;return !(n&(n-1));
}
bool ActionAtmosphereField_Valid(const ActionAtmosphereField *f) {
  if(!f||!ActionEffectVectors_Valid(f,f->seen,properties,kProperties))return false;
  if(f->Receivers[0]>7)return false;
  for(unsigned i=0;i<2;++i)if(f->Receivers[i]!=floorf(f->Receivers[i]))return false;
  const float *ambient[]={f->Ambient1,f->Ambient2,f->Ambient3,f->Ambient4,f->Ambient5,f->Ambient6,f->Ambient7,f->Ambient8};
  const float *areas[]={f->Area1,f->Area2,f->Area3};
  const float *grit[]={f->Grit1,f->Grit2,f->Grit3,f->Grit4};
  const float *tower[]={f->Tower1,f->Tower2,f->Tower3};
  const unsigned limits[]={8,3,4,3,4};
  for(unsigned i=0;i<5;++i)if(!Integer(f->Counts[i],0,limits[i]))return false;
  for(unsigned i=0;i<4;++i)if(!Integer(f->Dimensions[i],0,16384))return false;
  if(!Integer(f->Components[0],0,31)||f->Window[2]<=f->Window[0]||f->Window[3]<=f->Window[1]||f->Window[2]-f->Window[0]>768||f->Window[3]-f->Window[1]>544||f->Bounds[2]<=f->Bounds[0]||f->Bounds[3]<=f->Bounds[1])return false;
  for(unsigned i=0;i<8;++i) {
    const float *v=ambient[i];
    if(v[2]<=0||v[3]<=0||fabsf(v[4])>4||v[5]<0||v[5]>1||v[6]<0||v[6]>1)return false;
  }
  for(unsigned i=0;i<3;++i) {
    const float *v=areas[i];if(v[2]<=v[0]||v[3]<=v[1])return false;
    if(fabsf(tower[i][2])>4)return false;
  }
  for(unsigned i=0;i<4;++i) {
    const float *v=ActionAtmosphereField_Witness(f,i);
    if(!Integer(v[0],0,1)||!Integer(v[1],0,16384)||!Integer(v[2],0,16384)||!Integer(v[3],0,255))return false;
    v=grit[i];if(v[2]<v[1]||!Integer(v[3],1,65535))return false;
  }
  if(!Period(f->AmbientMotion[0],65536)||!Period(f->DustMotion[0],65536)||!Period(f->GritTiming[0],65536)||!Period(f->TowerMotion[0],65536)||!Period(f->MistVolume[1],8192))return false;
  if(f->AmbientMotion[2]+f->AmbientMotion[3]>1||f->AmbientMotion[2]<f->AmbientMotion[3]||f->TowerMotion[2]+f->TowerMotion[3]+f->TowerMotion[4]>1||f->TowerMotion[2]<f->TowerMotion[3]+f->TowerMotion[4])return false;
  /* Fixed budgets independent of room area and user dimensions. */
  if(!Integer(f->DustGrid[0],16,768)||!Integer(f->DustGrid[1],16,768)||!Integer(f->DustGrid[2],1,25)||!Integer(f->DustGrid[3],1,13)||!Integer(f->DustSkip[0],0,255)||!Integer(f->DustDensity[0],0,1)||f->DustDensity[2]>1)return false;
  if(!Integer(f->GritTiming[1],1,3)||!Integer(f->GritTiming[2],0,64)||!Integer(f->GritTiming[3],1,1024)||!Integer(f->GritTiming[4],1,2048)||f->GritTiming[4]<=f->GritTiming[3]||f->GritTiming[4]>f->GritTiming[0]||f->GritTiming[3]+(f->GritTiming[1]-1)*f->GritTiming[2]>f->GritTiming[0]||!Integer(f->GritBurst[1],1,9)||f->GrainTiming[1]<=0)return false;
  if(f->GrainShape[3]+f->GrainShape[4]>1||f->GrainShape[2]>1||f->DustShape[2]>1)return false;
  for(unsigned i=0;i<4;++i)if(f->FloorArea[i]!=floorf(f->FloorArea[i]/16)*16)return false;
  if(f->FloorArea[2]<=f->FloorArea[0]||f->FloorArea[2]-f->FloorArea[0]>880||f->FloorArea[3]<f->FloorArea[1]||f->FloorArea[3]-f->FloorArea[1]>128||f->FloorStyle[0]<1||f->FloorStyle[1]>1)return false;
  if(!Integer(f->FloorSource[0],0,65535)||!Integer(f->FloorSource[1],0,65535))return false;
  if(!Integer(f->MistVolume[0],1,3)||f->MistVolume[2]>1||f->MistVolume[3]>1||f->MistVolume[4]>1||f->MistVolume[3]-f->MistVolume[4]*2<=0||f->MistVolume[5]>2||f->MistVolume[6]>2||f->MistVolume[7]>1)return false;
  for(unsigned i=1;i<5;++i)if(f->TowerRows[i]<=f->TowerRows[i-1])return false;
  return true;
}
size_t ActionAtmosphereField_Write(const ActionAtmosphereField *f,char *text,size_t capacity) {
  return ActionAtmosphereField_Valid(f)?ActionEffectVectors_Write(f,properties,kProperties,text,capacity):0;
}
const ActionAtmosphereField *ActionAtmosphereField_Bundled(unsigned room) {
  static ActionAtmosphereField fields[3];static atomic_int state;
  if(room<2||room>4)return NULL;
  if(atomic_load_explicit(&state,memory_order_acquire)==2)return &fields[room-2];
  int expected=0;
  if(atomic_compare_exchange_strong(&state,&expected,1)) {
    static const char *texts[]={
#include "action_cave_atmosphere_default.inc"
      ,
#include "action_temple_atmosphere_default.inc"
      ,
#include "action_tower_atmosphere_default.inc"
    };
    bool ok=true;
    for(unsigned i=0;i<3&&ok;++i) {
      for(const char *at=texts[i];*at&&ok;) {
        const char *end=strchr(at,'\n');if(!end){ok=false;break;}
        const size_t n=(size_t)(end-at);char line[1024];if(n>=sizeof(line)){ok=false;break;}
        memcpy(line,at,n);line[n]=0;char *equals=strchr(line,'=');if(!equals){ok=false;break;}
        *equals=0;ok=ActionAtmosphereField_Set(&fields[i],line,equals+1);at=end+1;
      }
      ok=ok&&ActionAtmosphereField_Valid(&fields[i]);
      if(ok)ActionAtmosphereField_Prepare(&fields[i]);
    }
    atomic_store_explicit(&state,ok?2:3,memory_order_release);
  }
  while(atomic_load_explicit(&state,memory_order_acquire)==1) {}
  return atomic_load_explicit(&state,memory_order_acquire)==2?&fields[room-2]:NULL;
}

void ActionAtmosphereField_UpgradeExposure(ActionAtmosphereField *f,unsigned group,unsigned room){
  const ActionAtmosphereField *base=group==1?ActionAtmosphereField_Bundled(room):NULL;
  const float zero[4]={0};
  ActionEffectVectors_UpgradeExposure(f,f->seen,properties,kProperties,base?base->Dimming[0]:0,
    base?base->DimmingRamp:zero,8);
}
