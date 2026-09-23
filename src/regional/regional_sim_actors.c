#include "regional_sim_actors.h"
#include "byte_order.h"
#include <string.h>
static bool ValidRules(ArRegionalSimActorRules rules) { return !(rules.combat & ~0x1fu) && !(rules.ai & ~0x3fu); }
bool ArRegionalSimActors_Valid(const ArRegionalSimActors *a) {
  if (!a || a->active_town_tag>6) return false;
  for (unsigned i=0;i<24;++i) if (!ValidRules(a->cached[i])) return false;
  for (unsigned i=0;i<4;++i) if (!ValidRules(a->active[i]) || (!a->active_town_tag && (a->active[i].combat || a->active[i].ai))) return false;
  return true;
}
bool ArRegionalSimActors_LoadTown(ArRegionalSimActors *a,unsigned town) {
  if (town>=6 || !ArRegionalSimActors_Valid(a)) return false;
  memcpy(a->active,a->cached+town*4,sizeof(a->active));a->active_town_tag=(uint8_t)(town+1);return true;
}
bool ArRegionalSimActors_SaveTown(ArRegionalSimActors *a,unsigned town) {
  if (town>=6 || !ArRegionalSimActors_Valid(a) || a->active_town_tag!=town+1) return false;
  memcpy(a->cached+town*4,a->active,sizeof(a->active));return true;
}
bool ArRegionalSimActors_Birth(ArRegionalSimActors *a,unsigned town,unsigned slot,ArRegionalSimActorRules snapshot) {
  if (!a || town>=6 || slot>=4 || !ValidRules(snapshot) || a->active_town_tag!=town+1) return false;
  a->active[slot]=snapshot;return true;
}
bool ArRegionalSimActors_Read(const ArRegionalSimActors *a,unsigned town,unsigned slot,ArRegionalSimActorRules *snapshot) {
  if (!a || !snapshot || town>=6 || slot>=4 || a->active_town_tag!=town+1 || !ValidRules(a->active[slot])) return false;
  *snapshot=a->active[slot];return true;
}
bool ArRegionalSimActors_EncodeVersion(const ArRegionalSimActors *a,uint8_t *out,size_t capacity,unsigned version) {
  if ((version!=1 && version!=2) || !out || !ArRegionalSimActors_Valid(a)) return false;
  const unsigned stride=version*2, size=12+28*stride;
  if (capacity<size) return false;
  if (version==1) {
    for (unsigned i=0;i<24;++i) if (a->cached[i].ai) return false;
    for (unsigned i=0;i<4;++i) if (a->active[i].ai) return false;
  }
  memcpy(out,version==1?"ARSIMAC1":"ARSIMAC2",8);out[8]=a->active_town_tag;memset(out+9,0,3);
  for (unsigned i=0;i<28;++i) {
    const ArRegionalSimActorRules rules=i<24?a->cached[i]:a->active[i-24];
    ByteOrder_WriteLe16(out+12+stride*i,rules.combat);
    if (version==2) ByteOrder_WriteLe16(out+14+stride*i,rules.ai);
  }
  return true;
}
bool ArRegionalSimActors_Encode(const ArRegionalSimActors *a,uint8_t *out,size_t capacity) {
  return ArRegionalSimActors_EncodeVersion(a,out,capacity,2);
}
bool ArRegionalSimActors_Decode(const uint8_t *bytes,size_t size,ArRegionalSimActors *a) {
  if (!bytes || !a || size<12 || memcmp(bytes,"ARSIMAC",7) || (bytes[7]!='1' && bytes[7]!='2') || bytes[9] || bytes[10] || bytes[11]) return false;
  const unsigned stride=2*(bytes[7]-'0');
  if (size!=12+28*stride) return false;
  ArRegionalSimActors next={.active_town_tag=bytes[8]};
  for (unsigned i=0;i<28;++i) {
    ArRegionalSimActorRules *rules=i<24?&next.cached[i]:&next.active[i-24];
    rules->combat=ByteOrder_ReadLe16(bytes+12+stride*i);
    if (stride==4) rules->ai=ByteOrder_ReadLe16(bytes+14+stride*i);
  }
  if (!ArRegionalSimActors_Valid(&next)) return false;
  *a=next;return true;
}
