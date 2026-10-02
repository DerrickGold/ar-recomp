#include "action_castle_field.h"
#include "action_effect_vectors.h"
#include <stdatomic.h>
static const ActionEffectVectorProperty properties[]={
#define CASTLE_VECTOR(symbol,name,width,low,high) {name,offsetof(ActionCastleField,symbol),width,low,high},
#include "action_castle_field_properties.inc"
#undef CASTLE_VECTOR
};
enum { kProperties=sizeof(properties)/sizeof(properties[0]) };
_Static_assert(kProperties<=128,"castle property mask");
static bool Integer(float x,int lo,int hi){return isfinite(x)&&x>=lo&&x<=hi&&x==floorf(x);}
static bool Period(float x){return Integer(x,16,65536)&&!((unsigned)x&((unsigned)x-1));}
bool ActionCastleField_Set(ActionCastleField *f,const char *key,const char *value){
  if(!f||!key)return false;
  return !strncmp(key,"moon-",5)?ActionMoonField_Set(&f->moon,key+5,value):
      ActionEffectVectors_Set(f,f->seen,properties,kProperties,key,value);
}
void ActionCastleField_Prepare(ActionCastleField *f){
  const float *source[]={f->Source1,f->Source2,f->Source3,f->Source4,f->Source5,f->Source6,f->Source7,f->Source8,
    f->Source9,f->Source10,f->Source11,f->Source12,f->Source13,f->Source14,f->Source15,f->Source16};
  for(unsigned i=0;i<16;++i){const float *s=source[i];f->sources[i]=(ActionCastleSource){
    .style=(uint8_t)f->Style[0],.kind=(uint8_t)s[0],.check_x=(int16_t)s[1],.check_y=(int16_t)s[2],
    .top_tile=(uint8_t)s[3],.below_tile=(uint8_t)s[4],.x=(int16_t)s[5],.y=(int16_t)s[6],.length=(int16_t)s[7],
    .width=(int16_t)s[8],.spread=(int16_t)s[9],.lean=(int16_t)s[10],.left=(int16_t)s[11],.right=(int16_t)s[12],
    .bottom=(int16_t)s[13],.sill=(int16_t)s[14],.sill_tile=(uint8_t)s[15],.identity=(uint16_t)s[16],.arch=(uint8_t)s[17]};}
  const float *rows[]={f->NarrowRows,f->WideRows,f->BossRows};const unsigned counts[]={4,15,31};
  for(unsigned i=0;i<3;++i){ActionCastleArchProfile *a=&f->arches[i];
    memcpy(a->rows,rows[i],counts[i]*sizeof(float));a->count=counts[i];a->width=f->ArchWidths[i];a->haze_inset=f->ArchInsets[i];
    a->min_row=a->max_row=rows[i][0];
    for(unsigned j=1;j<counts[i];++j){a->min_row=fminf(a->min_row,rows[i][j]);a->max_row=fmaxf(a->max_row,rows[i][j]);}}
  ActionMoonField_Prepare(&f->moon);
}
bool ActionCastleField_Valid(const ActionCastleField *f){
  if(!f||!ActionEffectVectors_Valid(f,f->seen,properties,kProperties)||!ActionMoonField_Valid(&f->moon))return false;
  if(!Integer(f->Receivers[0],0,7)||!Integer(f->Receivers[1],0,8))return false;
  if(!Integer(f->Components[0],0,15)||!Integer(f->Style[0],0,2)||!Integer(f->SourceCount[0],0,16)||!Integer(f->WitnessCount[0],0,2))return false;
  const float *source[]={f->Source1,f->Source2,f->Source3,f->Source4,f->Source5,f->Source6,f->Source7,f->Source8,
    f->Source9,f->Source10,f->Source11,f->Source12,f->Source13,f->Source14,f->Source15,f->Source16};
  unsigned windows=0,torches=0,ambient=0;
  for(unsigned i=0;i<(unsigned)f->SourceCount[0];++i){const float *s=source[i];
    for(unsigned j=0;j<18;++j)if(!Integer(s[j],j==10?-4096:0,j==0||j==17?2:j==3||j==4||j==15?255:j==16?65535:16384))return false;
    if(s[7]<=0||s[8]<=0||s[9]<=0||s[11]>=s[12]||s[13]<=s[6]||(s[0]==0&&s[14]<=s[6]))return false;
    if(s[0]==0)++windows;else if(s[0]==2)++torches;else ++ambient;
    for(unsigned j=0;j<i;++j)if(source[j][16]==s[16])return false;
  }
  /* Keep the established family budgets, including clipped cells and native
   * flame accents. Density edits trade against source count, never pool size. */
  const unsigned cells=f->Style[0]==2?68:44+(f->Style[0]==1?16:0);
  const unsigned cost=windows*(cells+(unsigned)f->DustCounts[0])+torches*36+ambient*(24+(unsigned)f->DustCounts[1]);
  if(cost>748||torches>3||ambient>4||(f->Style[0]==2&&windows>1))return false;
  for(unsigned i=0;i<2;++i)if(!Integer(f->DustCounts[i],0,32)||!Period(f->DustPeriods[i]))return false;
  const float *witness[]={f->Witness1,f->Witness2,f->SkyWitness1,f->SkyWitness2};
  for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)if(!Integer(witness[i][j],0,j==0?1:j==3?255:16384))return false;
  for(unsigned i=0;i<4;++i)if(!Integer(f->Dimensions[i],0,16384)||!Integer(f->FloorExclude[i],0,255))return false;
  for(unsigned i=0;i<5;++i)if(!Integer(f->Material[i],0,i?65535:255)||!Integer(f->WaterMaterial[i],0,i?65535:255))return false;
  for(unsigned i=0;i<3;++i)if(!Integer(f->Floor[i],0,16384)||!Integer(f->Seeds[i],0,16777215))return false;
  for(unsigned i=0;i<2;++i)if(!Integer(f->WaterTiles[i],0,255))return false;
  if(f->Floor[1]<f->Floor[0]||!Integer(f->SkyRays[0],0,1))return false;
  for(unsigned i=0;i<4;++i)if(!Integer(f->WaterSurface[i],0,i==3?8:16384))return false;
  if(f->WaterSurface[2]<16||f->WaterSurface[2]>144||f->WaterSurface[0]+f->WaterSurface[2]*f->WaterSurface[3]>16384)return false;
  if(!Period(f->WaterMotion[0])||!Period(f->RippleTiming[0])||!Period(f->Breath[0])||!Period(f->MistCells[1])||!Period(f->TorchPulse[0])||!Period(f->TorchShape[3]))return false;
  if(f->MistCells[0]<48||f->MistCells[0]>128||!Integer(f->MistCells[0],48,128)||f->MistShape[0]+f->MistShape[1]>18)return false;
  if(f->FanPattern[4]<=0||f->FanPattern[5]<=0||f->FanPattern[8]<=0||f->UpperShape[0]>128||f->UpperShape[2]>128)return false;
  for(unsigned i=0;i<4;++i)if((i&&f->FanDepths[i]<f->FanDepths[i-1])||f->SkyWindow[i]<-768||f->SkyWindow[i]>768)return false;
  return f->SkyWindow[0]<f->SkyWindow[2]&&f->SkyWindow[1]<f->SkyWindow[3]&&f->SkyClip[0]<f->SkyClip[2]&&f->SkyClip[1]<f->SkyClip[3];
}
size_t ActionCastleField_Write(const ActionCastleField *f,char *text,size_t capacity){
  if(!ActionCastleField_Valid(f))return 0;
  size_t used=ActionEffectVectors_Write(f,properties,kProperties,text,capacity);
  char moon[16384];if(!used||!ActionMoonField_Write(&f->moon,moon,sizeof(moon)))return 0;
  for(const char *at=moon;*at;){const char *end=strchr(at,'\n');if(!end)return 0;size_t n=(size_t)(end-at)+1;
    if(text&&used+5+n<capacity){memcpy(text+used,"moon-",5);memcpy(text+used+5,at,n);}used+=5+n;at=end+1;}
  if(text){if(used>=capacity)return 0;text[used]=0;}return used;
}
const ActionCastleField *ActionCastleField_Bundled(unsigned room){
  if(room<2||room>8)return NULL;
  static ActionCastleField fields[7];static atomic_int state[7];unsigned i=room-2;
  if(atomic_load_explicit(&state[i],memory_order_acquire)==2)return &fields[i];
  int expected=0;if(atomic_compare_exchange_strong(&state[i],&expected,1)){
    static const char *const definitions[]={
#include "action_castle_2_default.inc"
      ,
#include "action_castle_3_default.inc"
      ,
#include "action_castle_4_default.inc"
      ,
#include "action_castle_5_default.inc"
      ,
#include "action_castle_6_default.inc"
      ,
#include "action_castle_7_default.inc"
      ,
#include "action_castle_8_default.inc"
    };
    bool ok=true;for(const char *at=definitions[i];*at&&ok;){const char *end=strchr(at,'\n');if(!end){ok=false;break;}
      size_t n=(size_t)(end-at);char line[1024];if(n>=sizeof(line)){ok=false;break;}memcpy(line,at,n);line[n]=0;
      char *eq=strchr(line,'=');if(!eq){ok=false;break;}*eq=0;ok=ActionCastleField_Set(&fields[i],line,eq+1);at=end+1;}
    ok=ok&&ActionCastleField_Valid(&fields[i]);if(ok)ActionCastleField_Prepare(&fields[i]);
    atomic_store_explicit(&state[i],ok?2:3,memory_order_release);
  }
  while(atomic_load_explicit(&state[i],memory_order_acquire)==1){}
  return atomic_load_explicit(&state[i],memory_order_acquire)==2?&fields[i]:NULL;
}

void ActionCastleField_UpgradeExposure(ActionCastleField *f,unsigned group,unsigned room){
  const ActionCastleField *base=group==2?ActionCastleField_Bundled(room):NULL;
  const float zero[4]={0};
  ActionEffectVectors_UpgradeExposure(f,f->seen,properties,kProperties,base?base->Dimming[0]:0,
    base?base->DimmingRamp:zero,1);
}
