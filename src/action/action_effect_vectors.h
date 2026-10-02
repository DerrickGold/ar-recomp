#ifndef AR_ACTION_EFFECT_VECTORS_H
#define AR_ACTION_EFFECT_VECTORS_H
/* Shared strict codec for complete vector recipes. Parsing is load-time only. */
#include <stdbool.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct ActionEffectVectorProperty {
  const char *name; size_t offset; unsigned width; float low,high;
} ActionEffectVectorProperty;
static inline const float *ActionEffectVectors_Values(const void *field,
    const ActionEffectVectorProperty *property) {
  return (const float *)((const unsigned char *)field+property->offset);
}
static inline bool ActionEffectVectors_Set(void *field,uint64_t *seen,
    const ActionEffectVectorProperty *properties,unsigned count,const char *key,const char *value) {
  if(!field||!key||!value)return false;
  unsigned i=0;while(i<count&&strcmp(key,properties[i].name))++i;
  if(i==count||(seen[i/64]&(UINT64_C(1)<<(i%64))))return false;
  float next[33];if(properties[i].width>33)return false;
  for(unsigned j=0;j<properties[i].width;++j) {
    char *end;next[j]=strtof(value,&end);
    if(end==value||!isfinite(next[j])||next[j]<properties[i].low||next[j]>properties[i].high)return false;
    value=end;if(*value&&!isspace((unsigned char)*value))return false;
    while(isspace((unsigned char)*value))++value;
  }
  if(*value)return false;
  memcpy((unsigned char *)field+properties[i].offset,next,properties[i].width*sizeof(float));
  seen[i/64]|=UINT64_C(1)<<(i%64);return true;
}
static inline bool ActionEffectVectors_Valid(const void *field,const uint64_t *seen,
    const ActionEffectVectorProperty *properties,unsigned count) {
  if(!field)return false;
  for(unsigned i=0;i<count;++i) {
    if(!(seen[i/64]&(UINT64_C(1)<<(i%64))))return false;
    const float *v=ActionEffectVectors_Values(field,&properties[i]);
    for(unsigned j=0;j<properties[i].width;++j)
      if(!isfinite(v[j])||v[j]<properties[i].low||v[j]>properties[i].high)return false;
  }
  return true;
}
/* Load-time upgrade for early version-1 complete fields, which predate
 * exposure controls. Existing values win; export writes the filled definition. */
static inline void ActionEffectVectors_UpgradeExposure(void *field,uint64_t *seen,
    const ActionEffectVectorProperty *properties,unsigned count,float dimming,const float *ramp,unsigned light_receivers){
  const char *names[]={"dimming","dimming-ramp","receivers"};char values[3][128];
  snprintf(values[0],sizeof(values[0]),"%.9g",(double)dimming);
  snprintf(values[1],sizeof(values[1]),"%.9g %.9g %.9g %.9g",(double)ramp[0],(double)ramp[1],(double)ramp[2],(double)ramp[3]);
  snprintf(values[2],sizeof(values[2]),"1 %u",light_receivers);
  for(unsigned n=0;n<3;++n)for(unsigned i=0;i<count;++i)
    if(!strcmp(names[n],properties[i].name)&&!(seen[i/64]&(UINT64_C(1)<<(i%64))))
      (void)ActionEffectVectors_Set(field,seen,properties,count,names[n],values[n]);
}
static inline size_t ActionEffectVectors_Write(const void *field,
    const ActionEffectVectorProperty *properties,unsigned count,char *text,size_t capacity) {
  size_t used=0;
  for(unsigned i=0;i<count;++i) {
    char line[1024];int n=snprintf(line,sizeof(line),"%s=",properties[i].name);
    const float *v=ActionEffectVectors_Values(field,&properties[i]);
    for(unsigned j=0;j<properties[i].width;++j)n+=snprintf(line+n,sizeof(line)-(size_t)n,"%s%.9g",j?" ":"",(double)v[j]);
    if(n<0||(size_t)n+1>=sizeof(line))return 0;
    line[n++]='\n';if(text&&used+(size_t)n<capacity)memcpy(text+used,line,(size_t)n);used+=(size_t)n;
  }
  if(text){if(used>=capacity)return 0;text[used]=0;}return used;
}
#endif
