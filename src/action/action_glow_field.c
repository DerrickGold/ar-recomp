#include "action_glow_field.h"
#include "action_effect_vectors.h"
#include <stdatomic.h>
static const ActionEffectVectorProperty properties[]={
#define GLOW_VECTOR(symbol,name,width,low,high) {name,offsetof(ActionGlowField,symbol),width,low,high},
#include "action_glow_field_properties.inc"
#undef GLOW_VECTOR
};
enum { kProperties=sizeof(properties)/sizeof(properties[0]) };
_Static_assert(kProperties<=64,"glow property presence");
static bool Integer(float v,int lo,int hi){return isfinite(v)&&v>=lo&&v<=hi&&v==floorf(v);}
static ArRenderColorF Color(const float *v){return (ArRenderColorF){v[0],v[1],v[2],v[3]};}
static ActionEffectGlowStyle Style(const float *radius,const float *rings,const float *centre,
    const float *ring1,const float *ring2,const float *ring3,const float *flame){
  return (ActionEffectGlowStyle){.radius_x=radius[0],.radius_y=radius[1],.ring_scale={rings[0],rings[1],rings[2]},
    .centre=Color(centre),.ring={Color(ring1),Color(ring2),Color(ring3)},.flare=flame[0],.rise=flame[1],
    .axis_x=flame[2],.axis_y=flame[3],.lift_x=flame[4],.lift_y=flame[5]};
}
bool ActionGlowField_Set(ActionGlowField *f,const char *key,const char *value){
  return f&&ActionEffectVectors_Set(f,&f->seen,properties,kProperties,key,value);
}
void ActionGlowField_Prepare(ActionGlowField *f){
  f->spill=Style(f->SpillRadius,f->SpillRings,f->SpillCentre,f->SpillRing1,f->SpillRing2,f->SpillRing3,f->SpillFlame);
  f->body=Style(f->BodyRadius,f->BodyRings,f->BodyCentre,f->BodyRing1,f->BodyRing2,f->BodyRing3,f->BodyFlame);
  const float *sources[]={f->Source1,f->Source2,f->Source3,f->Source4,f->Source5,f->Source6,f->Source7,f->Source8,
    f->Source9,f->Source10,f->Source11,f->Source12,f->Source13,f->Source14,f->Source15,f->Source16};
  for(unsigned i=0;i<16;++i)memcpy(f->sources[i],sources[i],sizeof(f->sources[i]));
}
bool ActionGlowField_Valid(const ActionGlowField *f){
  if(!f||!ActionEffectVectors_Valid(f,&f->seen,properties,kProperties)||!Integer(f->Components[0],0,3)||
      !Integer(f->Mode[0],0,1)||!Integer(f->SourceCount[0],0,16)||!Integer(f->Receivers[0],0,8))return false;
  for(unsigned i=0;i<4;++i)if(!Integer(f->MapRule[i],0,i<2?255:1)||!Integer(f->Geometry[i],-256,256))return false;
  if(f->Geometry[0]>0||f->Geometry[1]>0||f->Geometry[2]<=0||f->Geometry[3]<0)return false;
  for(unsigned i=0;i<2;++i)if(!Integer(f->Anchor[i],-64,64))return false;
  for(unsigned i=0;i<5;++i)if(!Integer(f->Clock[i],i==1||i==4?0:1,i==2||i==3?65536:8))return false;
  if(!Integer(f->Particles[0],0,7)||!Integer(f->Particles[1],1,255)||!Integer(f->Particles[2],0,31)||!Integer(f->Particles[3],0,31)||f->Particles[1]+f->Particles[3]>255)return false;
  const float *rings[]={f->SpillRings,f->BodyRings};const float *flames[]={f->SpillFlame,f->BodyFlame};
  for(unsigned i=0;i<2;++i){if(rings[i][0]<=0||rings[i][1]<=rings[i][0]||rings[i][2]<=rings[i][1])return false;
    const float axis=flames[i][2]*flames[i][2]+flames[i][3]*flames[i][3];
    if(axis<.99f||axis>1.01f||flames[i][0]<0||flames[i][1]<0)return false;}
  const float *sources[]={f->Source1,f->Source2,f->Source3,f->Source4,f->Source5,f->Source6,f->Source7,f->Source8,
    f->Source9,f->Source10,f->Source11,f->Source12,f->Source13,f->Source14,f->Source15,f->Source16};
  for(unsigned i=0;i<(unsigned)f->SourceCount[0];++i){
    for(unsigned j=0;j<3;++j)if(!Integer(sources[i][j],j==2?0:-16384,j==2?65535:16384))return false;
    for(unsigned j=0;j<i;++j)if(sources[i][2]==sources[j][2])return false;}
  return true;
}
size_t ActionGlowField_Write(const ActionGlowField *f,char *text,size_t capacity){
  return ActionGlowField_Valid(f)?ActionEffectVectors_Write(f,properties,kProperties,text,capacity):0;
}
const ActionGlowField *ActionGlowField_Bundled(unsigned group,unsigned room){
  unsigned index;
  if(group==2)index=0;
  else if((group==5&&room>=4&&room<=8)||(group==7&&room==6))index=1;
  else return NULL;
  static ActionGlowField fields[2];static atomic_int state;
  if(atomic_load_explicit(&state,memory_order_acquire)==2)return &fields[index];
  int expected=0;if(atomic_compare_exchange_strong(&state,&expected,1)){
    static const char *const definitions[]={
#include "action_torch_glow_default.inc"
      ,
#include "action_temple_glow_default.inc"
    };
    bool ok=true;for(unsigned i=0;i<2&&ok;++i){
      for(const char *at=definitions[i];*at&&ok;){const char *end=strchr(at,'\n');if(!end){ok=false;break;}
        size_t n=(size_t)(end-at);char line[1024];if(n>=sizeof(line)){ok=false;break;}memcpy(line,at,n);line[n]=0;
        char *equals=strchr(line,'=');if(!equals){ok=false;break;}*equals=0;ok=ActionGlowField_Set(&fields[i],line,equals+1);at=end+1;}
      ok=ok&&ActionGlowField_Valid(&fields[i]);if(ok)ActionGlowField_Prepare(&fields[i]);
    }
    atomic_store_explicit(&state,ok?2:3,memory_order_release);
  }
  while(atomic_load_explicit(&state,memory_order_acquire)==1){}
  return atomic_load_explicit(&state,memory_order_acquire)==2?&fields[index]:NULL;
}
