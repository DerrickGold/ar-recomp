/* Complete damp surface definitions, parsed once and copied into retained frames. */
#include "action_water_field.h"
#include "action_effect_vectors.h"
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const ActionEffectVectorProperty properties[]={
#define WATER_VECTOR(symbol,name,width,low,high) {name,offsetof(ActionWaterField,symbol),width,low,high},
#include "action_water_field_properties.inc"
#undef WATER_VECTOR
};
enum { kProperties=sizeof(properties)/sizeof(properties[0]) };
_Static_assert(kProperties<=128,"complete property presence fits the field mask");
static const float *Values(const ActionWaterField *f,unsigned i) {
  return (const float *)((const unsigned char *)f+properties[i].offset);
}
static bool Integer(float v,unsigned low,unsigned high) {
  return isfinite(v)&&v>=low&&v<=high&&floorf(v)==v;
}
static bool Period(float v,unsigned high) {
  if(!Integer(v,16,high))return false;
  const unsigned n=(unsigned)v;return !(n&(n-1));
}
bool ActionWaterField_Set(ActionWaterField *f,const char *key,const char *value) {
  return f&&ActionEffectVectors_Set(f,f->seen,properties,kProperties,key,value);
}
void ActionWaterField_Prepare(ActionWaterField *f) {
  if(!f)return;
  const float *pools[]={f->Pool1,f->Pool2,f->Pool3};
  const float *falls[]={f->Fall1,f->Fall2,f->Fall3,f->Fall4};
  const float *wet[]={f->Wet1,f->Wet2,f->Wet3,f->Wet4,f->Wet5,f->Wet6,f->Wet7,f->Wet8};
  const float *contours[]={f->Contour1,f->Contour2,f->Contour3,f->Contour4,f->Contour5,f->Contour6,f->Contour7,f->Contour8};
  for(unsigned i=0;i<kActionWaterFieldMaxPools;++i) {
    const float *v=pools[i];f->pools[i]=(ActionCaveWaterRegion){(int16_t)v[0],(int16_t)v[1],(int16_t)v[2]};
  }
  for(unsigned i=0;i<kActionWaterFieldMaxFalls;++i) {
    const float *v=falls[i];f->falls[i]=(ActionCaveWaterfall){(int16_t)v[0],(int16_t)v[1]};
  }
  for(unsigned i=0;i<kActionWaterFieldMaxWet;++i) {
    const float *v=wet[i];
    f->wet[i]=(ActionCaveWetSource){(int16_t)v[0],(int16_t)v[1],(int16_t)v[2],(uint8_t)v[3],(uint8_t)v[4],v[5]!=0,{0}};
    for(unsigned j=0;j<kActionWaterContourColumns;++j)f->wet[i].contour[j]=(int8_t)contours[i][j];
  }
}
const float *ActionWaterField_Witness(const ActionWaterField *f,unsigned i) {
  return i==0?f->Witness1:i==1?f->Witness2:i==2?f->Witness3:f->Witness4;
}
bool ActionWaterField_Valid(const ActionWaterField *f) {
  if(!f||!ActionEffectVectors_Valid(f,f->seen,properties,kProperties))return false;
  for(unsigned i=0;i<kProperties;++i)if(!strncmp(properties[i].name,"contour-",8)) {
    const float *v=Values(f,i);
    for(unsigned j=0;j<properties[i].width;++j)if(v[j]!=floorf(v[j]))return false;
  }
  const unsigned limits[]={kActionWaterFieldMaxPools,kActionWaterFieldMaxFalls,kActionWaterFieldMaxWet,kActionWaterFieldMaxWitnesses};
  for(unsigned i=0;i<4;++i)
    if(!Integer(f->Counts[i],0,limits[i])||!Integer(f->Dimensions[i],0,16384))return false;
  if(!Integer(f->Components[0],0,15)||f->PoolInsets[0]!=floorf(f->PoolInsets[0])||f->PoolInsets[1]!=floorf(f->PoolInsets[1]))return false;
  if(f->Window[2]<=f->Window[0]||f->Window[3]<=f->Window[1]||f->Window[2]-f->Window[0]>768||f->Window[3]-f->Window[1]>544||f->Bounds[2]<=f->Bounds[0]||f->Bounds[3]<=f->Bounds[1])return false;
  for(unsigned i=0;i<kActionWaterFieldMaxPools;++i) {
    const float *v=i==0?f->Pool1:i==1?f->Pool2:f->Pool3;
    for(unsigned j=0;j<3;++j)if(v[j]!=floorf(v[j]))return false;
    if(i<(unsigned)f->Counts[0]&&(v[1]<=v[0]||v[1]-v[0]>2048))return false;
  }
  for(unsigned i=0;i<kActionWaterFieldMaxFalls;++i) {
    const float *v=i==0?f->Fall1:i==1?f->Fall2:i==2?f->Fall3:f->Fall4;
    if(v[0]!=floorf(v[0])||!Integer(v[1],0,2048))return false;
  }
  for(unsigned i=0;i<kActionWaterFieldMaxWet;++i) {
    /* Inspect floats before narrowing the material IDs to bytes. */
    const float *v=i==0?f->Wet1:i==1?f->Wet2:i==2?f->Wet3:i==3?f->Wet4:i==4?f->Wet5:i==5?f->Wet6:i==6?f->Wet7:f->Wet8;
    for(unsigned j=0;j<3;++j)if(v[j]!=floorf(v[j]))return false;
    if(!Integer(v[3],0,255)||!Integer(v[4],0,255)||!Integer(v[5],0,1)||v[2]<v[1])return false;
  }
  for(unsigned i=0;i<kActionWaterFieldMaxWitnesses;++i) {
    const float *v=ActionWaterField_Witness(f,i);
    if(!Integer(v[0],0,1)||!Integer(v[1],0,16384)||!Integer(v[2],0,16384)||!Integer(v[3],0,255))return false;
  }
  if(!Period(f->GlintMotion[4],65536)||!Period(f->FallMotion[1],65536)||!Period(f->FallPulse[0],65536)||!Period(f->DripTiming[0],65536)||!Period(f->DripTiming[1],65536)||!Period(f->SheenMotion[0],65536)||!Period(f->MistVolume[1],8192))return false;
  /* Counts and sampling density cannot enlarge the existing geometry budget. */
  if(f->GlintMotion[3]<16||f->FallMotion[0]<48||!Integer(f->MistVolume[0],1,3)||f->RippleShape[7]<=0||f->DripTiming[2]<=0||f->DripTiming[3]<=0||f->DripTiming[4]<=0||f->DripTiming[2]+f->DripTiming[3]+f->DripTiming[4]>fminf(f->DripTiming[0],f->DripTiming[1])||f->SheenShape[1]<=0)return false;
  for(unsigned i=2;i<5;++i)if(!Integer(f->DripTiming[i],1,65536))return false;
  if(f->GlintShape[3]>1||f->FallGlint[2]>1||f->PoolRipple[2]>1||f->DripGather[4]>1||f->DripFall[3]>1||f->DripContact[1]>1||f->DripContact[6]>1)return false;
  if(f->FallMotion[0]!=floorf(f->FallMotion[0])||f->GlintMotion[3]!=floorf(f->GlintMotion[3])||f->MistVolume[3]>1||f->MistVolume[3]-f->MistVolume[4]*2<=0)return false;
  unsigned glints=0,fall_rows=0;
  const float *pools[]={f->Pool1,f->Pool2,f->Pool3};
  const float *falls[]={f->Fall1,f->Fall2,f->Fall3,f->Fall4};
  for(unsigned i=0;i<(unsigned)f->Counts[0];++i)glints+=(unsigned)ceilf((pools[i][1]-pools[i][0])/f->GlintMotion[3]);
  for(unsigned i=0;i<(unsigned)f->Counts[1];++i)fall_rows+=(unsigned)ceilf(falls[i][1]/f->FallMotion[0]);
  if(glints>82||fall_rows>48||f->FallGlow[0]<=0||f->FallGlow[1]<=0||f->SprayShape[0]<=0||f->SprayShape[1]<=0)return false;
  if(f->MistVolume[2]>1||f->MistVolume[4]>1||f->MistVolume[5]>2||f->MistVolume[6]>2||f->MistVolume[7]>1||f->SheenShape[0]>32)return false;
  if(f->FallRings[0]<=0||f->FallRings[1]<f->FallRings[0]||f->FallRings[2]<f->FallRings[1])return false;
  return true;
}
size_t ActionWaterField_Write(const ActionWaterField *f,char *text,size_t capacity) {
  return ActionWaterField_Valid(f)?ActionEffectVectors_Write(f,properties,kProperties,text,capacity):0;
}
const ActionWaterField *ActionWaterField_Bundled(void) {
  static ActionWaterField field;static atomic_int state;
  if(atomic_load_explicit(&state,memory_order_acquire)==2)return &field;
  int expected=0;
  if(atomic_compare_exchange_strong(&state,&expected,1)) {
    static const char text[]=
#include "action_water_field_default.inc"
    ;
    bool ok=true;
    for(const char *at=text;*at&&ok;) {
      const char *end=strchr(at,'\n');if(!end){ok=false;break;}
      const size_t n=(size_t)(end-at);char line[1024];if(n>=sizeof(line)){ok=false;break;}
      memcpy(line,at,n);line[n]=0;char *equals=strchr(line,'=');if(!equals){ok=false;break;}
      *equals=0;ok=ActionWaterField_Set(&field,line,equals+1);at=end+1;
    }
    ok=ok&&ActionWaterField_Valid(&field);
    if(ok)ActionWaterField_Prepare(&field);
    atomic_store_explicit(&state,ok?2:3,memory_order_release);
  }
  while(atomic_load_explicit(&state,memory_order_acquire)==1) {}
  return atomic_load_explicit(&state,memory_order_acquire)==2?&field:NULL;
}
