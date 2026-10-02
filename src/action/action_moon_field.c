/* Immutable linked moon field shared by native and browser renderers. */
#include "action_moon_field.h"
#include "action_effect_vectors.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>
static const ActionEffectVectorProperty properties[]={
#define MOON_VECTOR(symbol,name,width,low,high) {name,offsetof(ActionMoonField,symbol),width,low,high},
#include "action_moon_field_properties.inc"
#undef MOON_VECTOR
};
enum { kProperties=sizeof(properties)/sizeof(properties[0]) };
_Static_assert(kProperties<=128,"field presence mask");
static bool Integer(float v,unsigned lo,unsigned hi) {return isfinite(v)&&v>=lo&&v<=hi&&v==floorf(v);}
static bool Period(float v) {return Integer(v,16,65536)&&!((unsigned)v&((unsigned)v-1));}
bool ActionMoonField_Set(ActionMoonField *f,const char *key,const char *value) {
  return f&&ActionEffectVectors_Set(f,f->seen,properties,kProperties,key,value);
}
static float SoftFalloff(float x) {
  const float t=fmaxf(0,1-x*x);return t*t;
}
void ActionMoonField_Prepare(ActionMoonField *f) {
  if(!f)return;
  const float *low[]={f->Low1,f->Low2,f->Low3,f->Low4,f->Low5,f->Low6};
  const float *middle[]={f->Middle1,f->Middle2,f->Middle3,f->Middle4,f->Middle5};
  for(unsigned i=0;i<6;++i)memcpy(f->low[i],low[i],sizeof(f->low[i]));
  for(unsigned i=0;i<5;++i)memcpy(f->middle[i],middle[i],sizeof(f->middle[i]));
  /* Separable angular/radial profiles are clock-independent. Prepare them at
   * load time, retaining the original arithmetic order and fixed mesh budget. */
  for(unsigned i=0;i<kActionMoonFieldColumns;++i) {
    float *c=f->columns[i];const float slope=f->RayDomain[0]+i*(f->RayDomain[1]/(kActionMoonFieldColumns-1));
    float light=0;
    for(unsigned j=0;j<6;++j)light+=f->low[j][2]*SoftFalloff((slope-f->low[j][0])/f->low[j][1]);
    c[0]=slope;c[1]=f->RayShape[0]+light;c[2]=SoftFalloff(slope/f->RayShape[1]);
    for(unsigned j=0;j<5;++j)c[3+j]=SoftFalloff((slope-f->middle[j][0])/f->middle[j][1]);
  }
  for(unsigned i=0;i<kActionMoonFieldRows;++i) {
    float *r=f->rows[i];const float y=f->RayDomain[2]+i*(f->RayDomain[3]/(kActionMoonFieldRows-1));
    r[0]=y;r[1]=fmaxf(0,fminf(1,(y-f->RayShape[2])/f->RayShape[3]));
    r[2]=fminf(1,(f->RayShape[4]-y)/f->RayShape[5]);
    for(unsigned j=0;j<5;++j) {
      const float remaining=fmaxf(0,fminf(1,(f->middle[j][3]-y)/f->RayShape[6]));
      const float fade=remaining*remaining*(3-2*remaining);
      r[3+j]=f->middle[j][2]*fade;
    }
  }

}
bool ActionMoonField_Valid(const ActionMoonField *f) {
  if(!f||!ActionEffectVectors_Valid(f,f->seen,properties,kProperties))return false;
  if(!Integer(f->Components[0],0,15)||!Integer(f->WitnessCount[0],0,2))return false;
  for(unsigned i=0;i<4;++i)if(!Integer(f->Dimensions[i],0,16384))return false;
  for(unsigned i=0;i<2;++i)if(f->Anchor[i]!=floorf(f->Anchor[i])||!Integer(f->Seeds[i],0,16777215))return false;
  const float *witness[]={f->Witness1,f->Witness2};
  for(unsigned i=0;i<2;++i) {
    const float *w=witness[i];
    if(!Integer(w[0],0,1)||!Integer(w[1],0,16384)||!Integer(w[2],0,16384)||!Integer(w[3],0,255))return false;
  }
  const float *rects[]={f->Window,f->CloudWindow};
  for(unsigned i=0;i<2;++i)if(rects[i][2]<=rects[i][0]||rects[i][3]<=rects[i][1]||rects[i][2]-rects[i][0]>768||rects[i][3]-rects[i][1]>384)return false;
  const float *low[]={f->Low1,f->Low2,f->Low3,f->Low4,f->Low5,f->Low6};
  const float *middle[]={f->Middle1,f->Middle2,f->Middle3,f->Middle4,f->Middle5};
  for(unsigned i=0;i<6;++i)if(low[i][1]<=0||low[i][2]<0)return false;
  for(unsigned i=0;i<5;++i)if(middle[i][1]<=0||middle[i][2]<0||middle[i][3]<0)return false;
  if(f->RayDomain[1]<=0||f->RayDomain[2]<0||f->RayDomain[3]<=0||f->RayShape[1]<=0||f->RayShape[3]<=0||f->RayShape[5]<=0||f->RayShape[6]<=0)return false;
  if(f->RayDomain[2]+f->RayDomain[3]>f->RayShape[4]||f->Window[3]>f->RayShape[4])return false;
  for(unsigned i=1;i<6;++i)if(f->Shadow[i]>1)return false;
  if(fabsf(f->Shadow[4]+f->Shadow[5]-1)>.00001f)return false;
  if(!Period(f->Pulse[0])||!Period(f->CloudMotion[0])||!Period(f->ReflectionMotion[0])||!Period(f->WaveMotion[0]))return false;
  if(f->Pulse[2]>1||f->Pulse[3]>f->Pulse[2]||f->Pulse[2]+f->Pulse[3]>1||f->CloudOpacity[0]+f->CloudOpacity[1]>1||f->CloudShape[0]<=0||f->CloudShape[1]<=0)return false;
  /* Counts can only reduce the existing fixed mesh / crest budget. */
  if(!Integer(f->ReflectionRows[0],0,23)||!Integer(f->WaveRows[0],0,12)||!Integer(f->WaveRows[1],127,222)||!Integer(f->WaveRows[2],1,95))return false;
  if(f->WaveRows[0]>0&&f->WaveRows[1]+(f->WaveRows[0]-1)*f->WaveRows[2]>255)return false;
  if(f->ReflectionGain[4]<=0||f->ReflectionGain[5]<=0||f->ReflectionGlow[2]<=0||f->ReflectionGlow[3]<=0||f->WaveGain[2]<=0)return false;
  for(unsigned i=0;i<4;++i)if(f->ReflectionGain[i]>1)return false;
  if(f->ReflectionGain[2]<f->ReflectionGain[3]||f->WaveShape[3]>.5f||f->WaveShape[6]>.5f)return false;
  if(f->WaveShape[0]<=0||f->WaveShape[0]+11*f->WaveShape[1]+f->WaveShape[2]>=128||f->WaveShape[4]>1||f->WaveShape[5]>f->WaveShape[0])return false;
  for(unsigned i=0;i<7;++i)if(i!=2&&i!=3&&f->WaveGain[i]>1)return false;
  return true;
}
size_t ActionMoonField_Write(const ActionMoonField *f,char *text,size_t capacity) {
  return ActionMoonField_Valid(f)?ActionEffectVectors_Write(f,properties,kProperties,text,capacity):0;
}
const ActionMoonField *ActionMoonField_Bundled(void) {
  static ActionMoonField field;static atomic_int state;
  if(atomic_load_explicit(&state,memory_order_acquire)==2)return &field;
  int expected=0;
  if(atomic_compare_exchange_strong(&state,&expected,1)) {
    static const char text[]=
#include "action_moon_field_default.inc"
    ;
    bool ok=true;
    for(const char *at=text;*at&&ok;) {
      const char *end=strchr(at,'\n');if(!end){ok=false;break;}
      const size_t n=(size_t)(end-at);char line[1024];if(n>=sizeof(line)){ok=false;break;}
      memcpy(line,at,n);line[n]=0;char *equals=strchr(line,'=');if(!equals){ok=false;break;}
      *equals=0;ok=ActionMoonField_Set(&field,line,equals+1);at=end+1;
    }
    ok=ok&&ActionMoonField_Valid(&field);
    if(ok)ActionMoonField_Prepare(&field);
    atomic_store_explicit(&state,ok?2:3,memory_order_release);
  }
  while(atomic_load_explicit(&state,memory_order_acquire)==1) {}
  return atomic_load_explicit(&state,memory_order_acquire)==2?&field:NULL;
}
