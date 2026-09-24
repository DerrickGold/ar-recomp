#include "regional_actor_art_residency.h"

#include "regional_actor_art_native.inc"

static uint16_t Read16(const uint8_t *p) {return (uint16_t)(p[0]|(uint16_t)p[1]<<8);}
static uint32_t Key(const ArRegionalActorArtBinding *r) {
  return (uint32_t)r->scene<<16|(uint32_t)r->kind<<8|r->slot;
}
const ArRegionalActorArtBinding *ArRegionalActorArt_Binding(uint16_t scene,
    ArRegionalActorArtKind kind,unsigned slot) {
  if(kind<1 || kind>3 || slot>1)return NULL;
  const uint32_t key=(uint32_t)scene<<16|(uint32_t)kind<<8|slot;
  size_t low=0,high=sizeof(kActorBindings)/sizeof(kActorBindings[0]);
  while(low<high) {
    const size_t mid=low+(high-low)/2;
    const uint32_t other=Key(&kActorBindings[mid]);
    if(other<key)low=mid+1;
    else if(other>key)high=mid;
    else return &kActorBindings[mid];
  }
  return NULL;
}
static bool Overlap(uint16_t destination,size_t size,unsigned begin,unsigned end) {
  const size_t finish=(size_t)destination+size;
  return ((unsigned)destination<end && finish>begin) ||
      (finish>0x10000 && finish-0x10000>begin);
}
bool ArRegionalActorArt_ObserveDecode(ArRegionalActorArtResidency *residency,
    uint32_t source,uint16_t destination,ArRegionalMediaBytes decoded) {
  if(!residency || !decoded.data || !decoded.size || decoded.size>0x10000)return false;
  for(unsigned slot=0;slot<2;++slot) {
    ArRegionalActorArtResidentBank *bank=&residency->banks[slot];
    if(!bank->binding)continue;
    const unsigned base=0x4000+slot*0x1000;
    if(Overlap(destination,decoded.size,base,base+bank->binding->table+2u*bank->binding->pictures)) {
      bank->binding=NULL;continue;
    }
    for(unsigned v=0;v<bank->binding->pictures;++v)
      if(Overlap(destination,decoded.size,bank->begin[v],bank->end[v]))bank->valid[v]=0;
  }
  if(destination!=0x4000 && destination!=0x5000)return false;
  const unsigned slot=(destination-0x4000)/0x1000;
  const ArRegionalActorArtBinding *binding=NULL;
  for(size_t i=0;i<sizeof(kActorBindings)/sizeof(kActorBindings[0]);++i) {
    const ArRegionalActorArtBinding *r=&kActorBindings[i];
    if(r->kind==kArRegionalActorArt_Pictures && r->slot==slot && r->source==source) {
      binding=r;break;
    }
  }
  if(!binding || decoded.size!=binding->size || decoded.size<2 ||
      Read16(decoded.data)!=binding->table || !binding->pictures ||
      binding->pictures>kArRegionalActorArt_MaximumPictures ||
      binding->table+2u*binding->pictures>decoded.size)return false;
  ArRegionalActorArtResidentBank candidate={.binding=binding};
  const unsigned first=binding->table+2u*binding->pictures;
  for(unsigned v=0;v<binding->pictures;++v) {
    const unsigned at=Read16(decoded.data+binding->table+v*2);
    if(at<first || at>decoded.size || decoded.size-at<5)return false;
    const unsigned count=decoded.data[at+4],end=at+5+count*7;
    if(!count || count>kArRegionalActorArt_MaximumParts || end>decoded.size || destination+end>0xffff)return false;
    candidate.begin[v]=(uint16_t)(destination+at);
    candidate.end[v]=(uint16_t)(destination+end);
    candidate.valid[v]=1;
  }
  residency->banks[slot]=candidate;return true;
}
const ArRegionalActorArtBinding *ArRegionalActorArt_Resident(
    const ArRegionalActorArtResidency *residency,uint16_t base,
    uint16_t composition,unsigned visual) {
  if(!residency || (base!=0x4000 && base!=0x5000))return NULL;
  const ArRegionalActorArtResidentBank *bank=&residency->banks[(base-0x4000)/0x1000];
  return bank->binding && visual<bank->binding->pictures && bank->valid[visual] &&
      bank->begin[visual]==composition ? bank->binding : NULL;
}
