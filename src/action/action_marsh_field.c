/* Complete marsh surface/material/particle data shared by native and WASM. */
#include "action_marsh_field.h"
#include "action_effect_vectors.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>
static const ActionEffectVectorProperty properties[]={
#define MARSH_VECTOR(symbol,name,width,low,high) {name,offsetof(ActionMarshField,symbol),width,low,high},
#include "action_marsh_field_properties.inc"
#undef MARSH_VECTOR
};
enum { kProperties=sizeof(properties)/sizeof(properties[0]) };
_Static_assert(kProperties<=128,"field presence mask");
static bool Integer(float v,unsigned lo,unsigned hi) {return isfinite(v)&&v>=lo&&v<=hi&&v==floorf(v);}
static bool Period(float v) {return Integer(v,16,65536)&&!((unsigned)v&((unsigned)v-1));}
bool ActionMarshField_Set(ActionMarshField *f,const char *key,const char *value) {
  return f&&ActionEffectVectors_Set(f,f->seen,properties,kProperties,key,value);
}
void ActionMarshField_Prepare(ActionMarshField *f) {
  const float *spans[]={f->Span1,f->Span2,f->Span3,f->Span4,f->Span5,f->Span6,f->Span7,f->Span8};
  const float *materials[]={f->Material1,f->Material2,f->Material3,f->Material4,f->Material5,f->Material6,f->Material7,f->Material8,f->Material9};
  for(unsigned i=0;i<8;++i)memcpy(f->spans[i],spans[i],sizeof(f->spans[i]));
  for(unsigned i=0;i<9;++i)memcpy(f->materials[i],materials[i],sizeof(f->materials[i]));
}
bool ActionMarshField_Valid(const ActionMarshField *f) {
  if(!f||!ActionEffectVectors_Valid(f,f->seen,properties,kProperties))return false;
  if(!Integer(f->DetailWitnessCount[0],0,2)||!Integer(f->DetailWords[0],0,65535)||!Integer(f->DetailWords[1],0,65535)||!Integer(f->Components[0],0,15)||!Integer(f->SpanCount[0],0,8)||!Integer(f->WitnessCount[0],0,2)||!Integer(f->InsectCount[0],0,4))return false;
  for(unsigned i=0;i<4;++i)if(!Integer(f->Dimensions[i],0,16384)||!Integer(f->Bounds[i],0,16384))return false;
  if(f->Bounds[2]<=f->Bounds[0]||f->Bounds[3]<=f->Bounds[1]||f->Bounds[3]-f->Bounds[1]>512)return false;
  for(unsigned i=0;i<4;++i)if(!Integer(f->Surface[i],0,16384))return false;
  if(f->Surface[0]<f->Bounds[1]||f->Surface[2]>=f->Bounds[3]||f->Surface[2]-f->Surface[0]>31||f->Surface[1]<f->Surface[0]||f->Surface[1]>f->Surface[2])return false;
  const float *spans[]={f->Span1,f->Span2,f->Span3,f->Span4,f->Span5,f->Span6,f->Span7,f->Span8};
  for(unsigned i=0;i<(unsigned)f->SpanCount[0];++i) {
    const float *p=spans[i];
    if(!Integer(p[0],0,16384)||!Integer(p[1],0,16384)||p[0]<f->Bounds[0]||p[1]>f->Bounds[2]||p[1]-p[0]<=2*f->Surface[3]||(i&&p[0]<spans[i-1][1]))return false;
  }
  const float *witness[]={f->Witness1,f->Witness2,f->DetailWitness1,f->DetailWitness2};
  for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)if(!Integer(witness[i][j],0,j==0?1:j==3?255:16384))return false;
  const float *materials[]={f->Material1,f->Material2,f->Material3,f->Material4,f->Material5,f->Material6,f->Material7,f->Material8,f->Material9};
  for(unsigned i=0;i<9;++i)for(unsigned j=0;j<5;++j)if(!Integer(materials[i][j],0,j?65535:255))return false;
  for(unsigned i=0;i<9;++i)if(!Integer(f->WaterTiles[i],0,255))return false;
  for(unsigned i=0;i<4;++i)if(!Integer(f->PostTiles[i],0,255))return false;
  for(unsigned i=0;i<10;++i)if(!Integer(f->Seeds[i],0,16777215))return false;
  for(unsigned i=0;i<5;++i)if(f->WaterRows[i]<0||f->WaterRows[i]>f->Surface[2]-f->Surface[0]||(i&&f->WaterRows[i]<=f->WaterRows[i-1]))return false;
  const float *cells[]={f->WaterCells,f->MistCells};
  for(unsigned i=0;i<2;++i) {
    const float *c=cells[i];
    if(!Integer(c[0],i?64:24,128)||c[1]>(i?64:8)||c[2]<=0||c[3]>1||c[4]>1||c[3]+c[4]>1)return false;
  }
  if(!Period(f->WaterMotion[0])||!Period(f->WaterMotion[1])||!Period(f->MistMotion[0])||!Period(f->TimberMotion[0])||!Period(f->DripTiming[4])||!Period(f->DripTiming[5])||!Period(f->RipplePost[0])||!Period(f->InsectMotion[0])||!Period(f->UnderMist[2]))return false;
  if(f->MistWindow[1]<=f->MistWindow[0]||f->MistShape[0]+f->MistShape[1]>64||f->MistShape[2]+f->MistShape[3]>24)return false;
  if(f->WaterShape[0]+f->WaterShape[1]>13||f->WaterShape[2]>1.2f||f->WaterShape[3]>2||f->WaterShape[4]>1||f->WaterShape[5]>1)return false;
  if(f->MistGain[0]+f->MistGain[1]>1||f->MistGain[2]+f->MistGain[3]>1||f->WaterGain[0]+f->WaterGain[1]>1||f->WaterGain[2]+f->WaterGain[3]>1)return false;
  if(f->DripTiming[0]<1||f->DripTiming[1]<f->DripTiming[0]||f->DripTiming[1]>480||f->DripTiming[2]<=0||f->DripShape[0]<=0||f->DripShape[1]>1)return false;
  if(f->RippleShape[0]<=0||f->RippleShape[1]+f->RippleShape[2]>20||f->RippleShape[3]>1||f->RippleShape[4]>.25f||f->RippleDrip[1]>1||f->RippleDrip[3]>1||f->RipplePost[1]>1||f->RipplePost[3]>1)return false;
  if(f->InsectGain[0]+f->InsectGain[1]+f->InsectGain[2]>1||f->InsectMotion[2]+f->InsectMotion[4]>16||f->UnderMist[5]+f->UnderMist[6]>32)return false;
  return true;
}
size_t ActionMarshField_Write(const ActionMarshField *f,char *text,size_t capacity) {
  return ActionMarshField_Valid(f)?ActionEffectVectors_Write(f,properties,kProperties,text,capacity):0;
}
const ActionMarshField *ActionMarshField_Bundled(void) {
  static ActionMarshField field;static atomic_int state;
  if(atomic_load_explicit(&state,memory_order_acquire)==2)return &field;
  int expected=0;
  if(atomic_compare_exchange_strong(&state,&expected,1)) {
    static const char text[]=
#include "action_marsh_field_default.inc"
    ;
    bool ok=true;
    for(const char *at=text;*at&&ok;) {
      const char *end=strchr(at,'\n');if(!end){ok=false;break;}
      const size_t n=(size_t)(end-at);char line[1024];if(n>=sizeof(line)){ok=false;break;}
      memcpy(line,at,n);line[n]=0;char *equals=strchr(line,'=');if(!equals){ok=false;break;}
      *equals=0;ok=ActionMarshField_Set(&field,line,equals+1);at=end+1;
    }
    ok=ok&&ActionMarshField_Valid(&field);
    if(ok)ActionMarshField_Prepare(&field);
    atomic_store_explicit(&state,ok?2:3,memory_order_release);
  }
  while(atomic_load_explicit(&state,memory_order_acquire)==1) {}
  return atomic_load_explicit(&state,memory_order_acquire)==2?&field:NULL;
}
