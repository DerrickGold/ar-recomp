#include "regional/media/regional_media.h"

#include <string.h>
#include "snesrecomp/support/digest.h"

typedef struct MediaDefinition {
  uint32_t id,size;
  uint8_t hashes[kArRegionalMediaRelease_Count-1][32];
} MediaDefinition;
#include "regional/media/regional_media_catalog.inc"
_Static_assert(sizeof(kDonors)/sizeof(kDonors[0])==kArRegionalMediaRelease_Count-1,
               "media donor IDs");

static uint32_t Read32(const uint8_t *p) {
  return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
bool ArRegionalMedia_Parse(const void *data,size_t size,ArRegionalMediaView *out) {
  const uint8_t *p=data;
  if(!p || !out || size<48 || size>kArRegionalMediaMaximumBytes ||
      memcmp(p,"ARMEDIA\0",8) || p[8]!=kArRegionalMediaVersion || p[9] ||
      Read32(p+12)!=size || !p[10] || p[10]>=kArRegionalMediaRelease_Count ||
      !p[11] || p[11]>kArRegionalMediaMaximumEntries ||
      size<48u+12u*p[11] || memcmp(p+16,kDonors[p[10]-1],32))return false;
  ArRegionalMediaView candidate={.release=(ArRegionalMediaRelease)p[10]};
  size_t cursor=48u+12u*p[11];
  static const uint8_t zero[32];
  for(size_t i=0;i<sizeof(kMediaDefinitions)/sizeof(kMediaDefinitions[0]);++i) {
    const MediaDefinition *definition=&kMediaDefinitions[i];
    const uint8_t *expected=definition->hashes[p[10]-1];
    if(!memcmp(expected,zero,32))continue;
    if(candidate.count>=p[11])return false;
    const uint8_t *record=p+48+candidate.count*12;
    if(Read32(record)!=definition->id || Read32(record+4)!=cursor ||
        Read32(record+8)!=definition->size || definition->size>size-cursor)return false;
    uint8_t digest[32];
    if(!sr_support_sha256(p+cursor,definition->size,digest) || memcmp(digest,expected,32))return false;
    candidate.entries[candidate.count++]=(ArRegionalMediaEntry){definition->id,{p+cursor,definition->size}};
    cursor+=definition->size;
  }
  if(candidate.count!=p[11] || cursor!=size)return false;
  *out=candidate;return true;
}
ArRegionalMediaBytes ArRegionalMedia_Find(const ArRegionalMediaView *view,uint32_t id) {
  if(view && view->count<=kArRegionalMediaMaximumEntries)
    for(size_t i=0;i<view->count;++i)if(view->entries[i].id==id)return view->entries[i].bytes;
  return (ArRegionalMediaBytes){0};
}
