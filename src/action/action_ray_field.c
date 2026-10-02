/* Reusable grouped light field codec. All visual values come from data. */
#include "action_ray_field.h"
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *name; float low, high; } kProperties[] = {
#define RAY_PROPERTY(symbol, name, low, high) {name, low, high},
#include "action_ray_field_properties.inc"
#undef RAY_PROPERTY
};
static const struct { const char *name; unsigned width; } kVectors[] = {
  {"ray-count",1}, {"fan-count",1}, {"profile-count",1}, {"witness-count",1},
  {"components",1}, {"pulse-seed",1}, {"dimensions",4}, {"anchor",2},
  {"window",4}, {"bounds",4}, {"mote-color",4}, {"cluster-color",4},
  {"leaf-color",4}, {"leaf-rim-color",4}, {"leaf-shape",12},
};
enum { kVectorCount=sizeof(kVectors)/sizeof(kVectors[0]),
       kRayBegin=kActionRay_PropertyCount+kVectorCount,
       kFanBegin=kRayBegin+kActionRayFieldMaxRays,
       kProfileBegin=kFanBegin+kActionRayFieldMaxFans,
       kWitnessBegin=kProfileBegin+kActionRayFieldMaxProfiles,
       kPropertyEnd=kWitnessBegin+kActionRayFieldMaxWitnesses };
_Static_assert(kPropertyEnd<=128,"ray definition presence mask must fit");
static const char *const kArrayNames[]={"ray","fan","profile","witness"};
static const unsigned kArrayBegin[]={kRayBegin,kFanBegin,kProfileBegin,kWitnessBegin};
static const unsigned kArrayCount[]={kActionRayFieldMaxRays,kActionRayFieldMaxFans,
    kActionRayFieldMaxProfiles,kActionRayFieldMaxWitnesses}, kArrayWidth[]={6,2,16,4};

/* Array keys are formatted by the caller; fixed names are immutable. */
static const char *PropertyName(unsigned index) {
  if(index<kActionRay_PropertyCount)return kProperties[index].name;
  if(index<kRayBegin)return kVectors[index-kActionRay_PropertyCount].name;
  return NULL;
}
static unsigned PropertyWidth(unsigned index) {
  if(index<kActionRay_PropertyCount)return 1;
  if(index<kRayBegin)return kVectors[index-kActionRay_PropertyCount].width;
  for(unsigned a=0;a<4;++a)if(index>=kArrayBegin[a]&&index<kArrayBegin[a]+kArrayCount[a])return kArrayWidth[a];
  return 0;
}
static unsigned KeyIndex(const char *key) {
  for(unsigned i=0;i<kRayBegin;++i)if(!strcmp(key,PropertyName(i)))return i;
  for(unsigned a=0;a<4;++a) {
    char name[24];
    for(unsigned i=0;i<kArrayCount[a];++i) {
      snprintf(name,sizeof(name),"%s-%u",kArrayNames[a],i+1);
      if(!strcmp(name,key))return kArrayBegin[a]+i;
    }
  }
  return kPropertyEnd;
}
static bool Numbers(const char *text, float *values, unsigned count) {
  for(unsigned i=0;i<count;++i) {
    char *end;
    values[i]=strtof(text,&end);
    if(end==text||!isfinite(values[i]))return false;
    text=end;
    if(*text&&!isspace((unsigned char)*text))return false;
    while(isspace((unsigned char)*text))++text;
  }
  return !*text;
}
static bool Integer(float value,unsigned low,unsigned high) {
  return value>=low&&value<=high&&floorf(value)==value;
}
static bool Seen(const ActionRayField *f,unsigned index) {
  return (f->seen[index/64] & (UINT64_C(1)<<(index%64)))!=0;
}
bool ActionRayField_Set(ActionRayField *f,const char *key,const char *value) {
  if(!f||!key||!value)return false;
  const unsigned index=KeyIndex(key);
  if(index==kPropertyEnd||Seen(f,index))return false;
  float v[16];const unsigned width=PropertyWidth(index);
  if(index==kActionRay_PropertyCount+5) {
    uint32_t n=0;
    if(!*value)return false;
    for(const char *s=value;*s;++s) {
      if(*s<'0'||*s>'9'||n>(UINT32_MAX-(unsigned)(*s-'0'))/10)return false;
      n=n*10+(unsigned)(*s-'0');
    }
    f->pulse_seed=n;
  } else {
    if(!Numbers(value,v,width))return false;
    if(index<kActionRay_PropertyCount) {
      if(v[0]<kProperties[index].low||v[0]>kProperties[index].high)return false;
      f->values[index]=v[0];
    } else if(index<kRayBegin) {
      const unsigned vector=index-kActionRay_PropertyCount;
      switch(vector) {
        case 0: if(!Integer(v[0],1,kActionRayFieldMaxRays))return false;f->ray_count=(unsigned)v[0];break;
        case 1: if(!Integer(v[0],0,kActionRayFieldMaxFans))return false;f->fan_count=(unsigned)v[0];break;
        case 2: if(!Integer(v[0],1,kActionRayFieldMaxProfiles))return false;f->profile_count=(unsigned)v[0];break;
        case 3: if(!Integer(v[0],0,kActionRayFieldMaxWitnesses))return false;f->witness_count=(unsigned)v[0];break;
        case 4: if(!Integer(v[0],0,15))return false;f->components=(unsigned)v[0];break;
        case 6:
          for(unsigned i=0;i<4;++i) {if(!Integer(v[i],0,16384))return false;f->dimensions[i]=(int)v[i];}break;
        case 7:
          for(unsigned i=0;i<2;++i) {if(fabsf(v[i])>8192)return false;f->anchor[i]=v[i];}break;
        case 8: case 9: {
          float *out=vector==8?f->window:f->bounds;
          for(unsigned i=0;i<4;++i) {if(fabsf(v[i])>16384)return false;out[i]=v[i];}break;
        }
        case 10: case 11: case 12: case 13: {
          for(unsigned i=0;i<4;++i)if(v[i]<0||v[i]>1)return false;
          ArRenderColorF *out=vector==10?&f->mote_color:vector==11?&f->cluster_color:vector==12?&f->leaf_color:&f->leaf_rim_color;
          *out=(ArRenderColorF){v[0],v[1],v[2],v[3]};break;
        }
        case 14:
          for(unsigned i=0;i<6;++i) {if(fabsf(v[i*2])>16||fabsf(v[i*2+1])>16)return false;f->leaf_shape[i]=(ArRenderPointF){v[i*2],v[i*2+1]};}break;
        default:return false;
      }
    } else if(index<kFanBegin) {
      if(fabsf(v[0])>16384||v[1]<=0||v[1]>224||v[2]<0||v[2]>4||v[3]<0||v[3]>1||!Integer(v[4],0,kActionRayFieldMaxFans)||!Integer(v[5],0,kActionRayFieldMaxProfiles-1))return false;
      f->rays[index-kRayBegin]=(ActionRayOpening){v[0],v[1],v[2],v[3],(unsigned)v[4],(unsigned)v[5]};
    } else if(index<kProfileBegin) {
      if(fabsf(v[0])>16384||v[1]>=-1||v[1]<-8192)return false;
      f->fans[index-kFanBegin+1]=(ArRenderPointF){v[0],v[1]};
    } else if(index<kWitnessBegin) {
      for(unsigned i=0;i<6;++i)if(fabsf(v[i])>8192||(i&&v[i]<=v[i-1]))return false;
      if(fabsf(v[6])>8192||v[7]<=0||v[7]>8192)return false;
      for(unsigned i=8;i<16;++i)if(v[i]<0||v[i]>1)return false;
      ActionRayProfile *p=&f->profiles[index-kProfileBegin];
      memcpy(p->rows,v,6*sizeof(float));p->front_start=v[6];p->front_range=v[7];
      p->rear=(ArRenderColorF){v[8],v[9],v[10],v[11]};
      p->front=(ArRenderColorF){v[12],v[13],v[14],v[15]};
    } else {
      if(!Integer(v[0],0,1)||!Integer(v[1],0,16384)||!Integer(v[2],0,16384)||!Integer(v[3],0,255))return false;
      f->witnesses[index-kWitnessBegin]=(ActionRayWitness){(unsigned)v[0],(int)v[1],(int)v[2],(uint8_t)v[3]};
    }
  }
  f->seen[index/64]|=UINT64_C(1)<<(index%64);
  return true;
}
static bool Range(float value, float low, float high) {
  return isfinite(value) && value >= low && value <= high;
}
static bool ColorValid(ArRenderColorF color) {
  return Range(color.r,0,1) && Range(color.g,0,1) &&
      Range(color.b,0,1) && Range(color.a,0,1);
}
bool ActionRayField_Valid(const ActionRayField *f) {
  if(!f||f->ray_count<1||f->ray_count>kActionRayFieldMaxRays||f->fan_count>kActionRayFieldMaxFans||
     f->profile_count<1||f->profile_count>kActionRayFieldMaxProfiles||f->witness_count>kActionRayFieldMaxWitnesses)return false;
  if(f->components>15)return false;
  for(unsigned i=0;i<kActionRay_PropertyCount;++i)
    if(!Range(f->values[i],kProperties[i].low,kProperties[i].high))return false;
  for(unsigned i=0;i<4;++i) {
    if(f->dimensions[i]<0||f->dimensions[i]>16384||
       !Range(f->window[i],-16384,16384)||!Range(f->bounds[i],-16384,16384))return false;
  }
  for(unsigned i=0;i<2;++i)if(!Range(f->anchor[i],-8192,8192))return false;
  if(!ColorValid(f->mote_color)||!ColorValid(f->cluster_color)||
     !ColorValid(f->leaf_color)||!ColorValid(f->leaf_rim_color))return false;
  for(unsigned i=0;i<6;++i)
    if(!Range(f->leaf_shape[i].x,-16,16)||!Range(f->leaf_shape[i].y,-16,16))return false;
  for(unsigned i=0;i<kRayBegin;++i)if(!Seen(f,i))return false;
  const unsigned counts[]={f->ray_count,f->fan_count,f->profile_count,f->witness_count};
  for(unsigned a=0;a<4;++a)for(unsigned i=0;i<kArrayCount[a];++i)
    if(Seen(f,kArrayBegin[a]+i)!=(i<counts[a]))return false;
  for(unsigned i=0;i<f->ray_count;++i) {
    const ActionRayOpening *r=&f->rays[i];
    if(!Range(r->x,-16384,16384)||!Range(r->half_width,0,224)||r->half_width==0||
       !Range(r->strength,0,4)||!Range(r->shoulder,0,1)||
       r->fan>f->fan_count||r->profile>=f->profile_count)return false;
  }
  for(unsigned i=1;i<=f->fan_count;++i)
    if(!Range(f->fans[i].x,-16384,16384)||!Range(f->fans[i].y,-8192,-1)||f->fans[i].y==-1)return false;
  for(unsigned i=0;i<f->profile_count;++i) {
    const ActionRayProfile *p=&f->profiles[i];
    for(unsigned row=0;row<kActionRayFieldRows;++row)
      if(!Range(p->rows[row],-8192,8192)||(row&&p->rows[row]<=p->rows[row-1]))return false;
    if(!Range(p->front_start,-8192,8192)||!Range(p->front_range,0,8192)||p->front_range==0||
       !ColorValid(p->rear)||!ColorValid(p->front))return false;
  }
  for(unsigned i=0;i<f->witness_count;++i) {
    const ActionRayWitness *w=&f->witnesses[i];
    if(w->bg>1||w->x<0||w->x>16384||w->y<0||w->y>16384)return false;
  }
  const unsigned periods[]={kActionRay_SwayPeriod,kActionRay_MotePeriod,kActionRay_ClusterPeriod,kActionRay_LeafPeriod};
  for(unsigned i=0;i<4;++i) {
    const float value=f->values[periods[i]];
    if(!Integer(value,16,65536))return false;
    const unsigned n=(unsigned)value;if(n&(n-1))return false;
  }
  const unsigned ints[]={kActionRay_MoteCount,kActionRay_ClusterCount,kActionRay_ClusterParticles,kActionRay_LeafCount};
  for(unsigned i=0;i<4;++i)if(floorf(f->values[ints[i]])!=f->values[ints[i]])return false;
  if(f->window[2]<=f->window[0]||f->window[3]<=f->window[1]||
     f->window[2]-f->window[0]>768||f->window[3]-f->window[1]>544||
     f->bounds[2]<=f->bounds[0]||f->bounds[3]<=f->bounds[1])return false;
  return true;
}
static unsigned Values(const ActionRayField *f,unsigned index,float *v) {
  const unsigned n=PropertyWidth(index);
  if(index<kActionRay_PropertyCount)v[0]=f->values[index];
  else if(index<kRayBegin) {
    switch(index-kActionRay_PropertyCount) {
      case 0:v[0]=f->ray_count;break;case 1:v[0]=f->fan_count;break;case 2:v[0]=f->profile_count;break;
      case 3:v[0]=f->witness_count;break;case 4:v[0]=f->components;break;case 5:return 0;
      case 6:for(unsigned i=0;i<4;++i)v[i]=(float)f->dimensions[i];break;
      case 7:memcpy(v,f->anchor,n*sizeof(float));break;
      case 8:memcpy(v,f->window,n*sizeof(float));break;
      case 9:memcpy(v,f->bounds,n*sizeof(float));break;
      case 10:case 11:case 12:case 13: {
        const unsigned color=index-kActionRay_PropertyCount;
        const ArRenderColorF *c=color==10?&f->mote_color:color==11?&f->cluster_color:color==12?&f->leaf_color:&f->leaf_rim_color;
        v[0]=c->r;v[1]=c->g;v[2]=c->b;v[3]=c->a;break;
      }
      case 14:for(unsigned i=0;i<6;++i){v[i*2]=f->leaf_shape[i].x;v[i*2+1]=f->leaf_shape[i].y;}break;
    }
  } else if(index<kFanBegin) {
    const unsigned i=index-kRayBegin;if(i>=f->ray_count)return 0;
    const ActionRayOpening *r=&f->rays[i];v[0]=r->x;v[1]=r->half_width;v[2]=r->strength;v[3]=r->shoulder;v[4]=r->fan;v[5]=r->profile;
  } else if(index<kProfileBegin) {
    const unsigned i=index-kFanBegin+1;if(i>f->fan_count)return 0;v[0]=f->fans[i].x;v[1]=f->fans[i].y;
  } else if(index<kWitnessBegin) {
    const unsigned i=index-kProfileBegin;if(i>=f->profile_count)return 0;
    const ActionRayProfile *p=&f->profiles[i];memcpy(v,p->rows,6*sizeof(float));v[6]=p->front_start;v[7]=p->front_range;
    v[8]=p->rear.r;v[9]=p->rear.g;v[10]=p->rear.b;v[11]=p->rear.a;v[12]=p->front.r;v[13]=p->front.g;v[14]=p->front.b;v[15]=p->front.a;
  } else {
    const unsigned i=index-kWitnessBegin;if(i>=f->witness_count)return 0;
    const ActionRayWitness *w=&f->witnesses[i];v[0]=w->bg;v[1]=(float)w->x;v[2]=(float)w->y;v[3]=w->tile;
  }
  return n;
}
size_t ActionRayField_Write(const ActionRayField *f,char *text,size_t capacity) {
  if(!ActionRayField_Valid(f))return 0;
  size_t used=0;
  for(unsigned index=0;index<kPropertyEnd;++index) {
    float v[16];const unsigned n=Values(f,index,v);
    const bool seed=index==kActionRay_PropertyCount+5;
    if(!n&&!seed)continue;
    char key[24];const char *name=PropertyName(index);
    if(!name)for(unsigned a=0;a<4;++a)if(index>=kArrayBegin[a]&&index<kArrayBegin[a]+kArrayCount[a]) {
      snprintf(key,sizeof(key),"%s-%u",kArrayNames[a],index-kArrayBegin[a]+1);name=key;
    }
    char line[256];int written=snprintf(line,sizeof(line),"%s=",name);
    if(seed)written+=snprintf(line+written,sizeof(line)-(size_t)written,"%u",f->pulse_seed);
    for(unsigned j=0;j<n;++j)written+=snprintf(line+written,sizeof(line)-(size_t)written,"%s%.9g",j?" ":"",(double)v[j]);
    if(written<0||(size_t)written+1>=sizeof(line))return 0;
    line[written++]='\n';
    if(text&&used+(size_t)written<capacity)memcpy(text+used,line,(size_t)written);
    used+=(size_t)written;
  }
  if(text) {if(used>=capacity)return 0;text[used]=0;}
  return used;
}
const ActionRayField *ActionRayField_Bundled(void) {
  static ActionRayField field;
  static atomic_int state;
  if(atomic_load_explicit(&state,memory_order_acquire)==2)return &field;
  int expected=0;
  if(atomic_compare_exchange_strong(&state,&expected,1)) {
    static const char text[]=
#include "action_ray_field_default.inc"
    ;
    bool ok=true;
    for(const char *at=text;*at&&ok;) {
      const char *end=strchr(at,'\n');
      if(!end){ok=false;break;}
      const size_t n=(size_t)(end-at);
      char line[256];if(n>=sizeof(line)){ok=false;break;}
      memcpy(line,at,n);line[n]=0;char *equals=strchr(line,'=');
      if(!equals){ok=false;break;}*equals=0;ok=ActionRayField_Set(&field,line,equals+1);at=end+1;
    }
    ok=ok&&ActionRayField_Valid(&field);
    atomic_store_explicit(&state,ok?2:3,memory_order_release);
  }
  while(atomic_load_explicit(&state,memory_order_acquire)==1) {}
  return atomic_load_explicit(&state,memory_order_acquire)==2?&field:NULL;
}
