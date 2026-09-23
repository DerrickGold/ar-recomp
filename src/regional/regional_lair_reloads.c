#include "regional_lair_reloads.h"
#include "byte_order.h"
#include <string.h>

static const uint16_t kReload[2][kArRegionalLairCount] = {
  {1,1,1,1, 1,1,1,1, 1,1,1,1, 1,141,1,1, 1,1,1,1, 100,100,1,1},
  {75,150,150,150, 200,100,150,150, 60,200,200,75,
   75,212,75,75, 180,37,75,90, 275,275,60,120},
};
bool ArRegionalLair_Reload(ArRegionalSource source, unsigned lair, uint16_t *delay) {
  if (!delay || (unsigned)source>=kArRegionalSource_Count || lair>=kArRegionalLairCount) return false;
  *delay=kReload[source==kArRegionalSource_Japan][lair]; return true;
}
bool ArRegionalLairReloads_Valid(const ArRegionalLairReloads *h) {
  return h && !(h->initialized_towns & ~0x3fu) &&
      !(h->approximate_towns & ~h->initialized_towns) &&
      !(h->diverged_towns & ~h->initialized_towns);
}
bool ArRegionalLairReloads_Init(ArRegionalLairReloads *h) {
  if (!ArRegionalLairReloads_Valid(h) || h->initialized_towns) return false;
  ArRegionalLairReloads next={.initialized_towns=0x3f};
  memcpy(next.delay,kReload,sizeof(next.delay));
  *h=next; return true;
}
static uint16_t Reduced(uint16_t value) { return (uint16_t)((value>>2)+1); }
bool ArRegionalLairReloads_Adopt(ArRegionalLairReloads *h, ArRegionalSource source, const uint16_t native[24]) {
  if (!native || !ArRegionalLairReloads_Valid(h) || h->initialized_towns ||
      (unsigned)source>=kArRegionalSource_Count) return false;
  ArRegionalLairReloads next={0};
  ArRegionalLairReloads_Init(&next);
  const unsigned active=source==kArRegionalSource_Japan;
  next.approximate_towns=0x3f;
  for (unsigned town=0;town<6;++town) {
    bool matches=false;
    /* Eight reductions reach the fixed point even for a 16-bit input. */
    for (unsigned step=0;step<=8;++step) {
      matches=true;
      for (unsigned i=town*4;i<town*4+4;++i) matches &= next.delay[active][i]==native[i];
      if (matches) break;
      if (step<8) for (unsigned p=0;p<2;++p) for (unsigned i=town*4;i<town*4+4;++i)
        next.delay[p][i]=Reduced(next.delay[p][i]);
    }
    if (!matches) next.diverged_towns |= (uint8_t)(1u<<town);
    for (unsigned i=town*4;i<town*4+4;++i) next.delay[active][i]=native[i];
  }
  *h=next; return true;
}
bool ArRegionalLairReloads_Check(ArRegionalLairReloads *h, ArRegionalSource source, const uint16_t native[24]) {
  if (!native || !ArRegionalLairReloads_Valid(h) || (unsigned)source>=kArRegionalSource_Count) return false;
  for (unsigned i=0;i<24;++i) if (h->initialized_towns & (1u<<(i/4)))
    if (h->delay[source==kArRegionalSource_Japan][i]!=native[i]) h->diverged_towns |= (uint8_t)(1u<<(i/4));
  return h->initialized_towns==0x3f && !h->diverged_towns;
}
bool ArRegionalLairReloads_ReduceTown(ArRegionalLairReloads *h, unsigned town) {
  if (!ArRegionalLairReloads_Valid(h) || town>=6 || !(h->initialized_towns & (1u<<town)) ||
      (h->diverged_towns & (1u<<town))) return false;
  for (unsigned p=0;p<2;++p) for (unsigned i=town*4;i<town*4+4;++i) h->delay[p][i]=Reduced(h->delay[p][i]);
  return true;
}
bool ArRegionalLairReloads_Encode(const ArRegionalLairReloads *h, uint8_t *out, size_t capacity) {
  if (!out || capacity<kArRegionalLairReloadEncodedBytes || !ArRegionalLairReloads_Valid(h)) return false;
  memcpy(out,"ARLDELY1",8); out[8]=h->initialized_towns; out[9]=h->approximate_towns;
  out[10]=h->diverged_towns; out[11]=0;
  for (unsigned p=0;p<2;++p) for (unsigned i=0;i<24;++i)
    ByteOrder_WriteLe16(out+12+(p*24+i)*2,(h->initialized_towns & (1u<<(i/4))) ? h->delay[p][i] : 0);
  return true;
}
bool ArRegionalLairReloads_Decode(const uint8_t *bytes, size_t size, ArRegionalLairReloads *h) {
  if (!bytes || !h || size!=kArRegionalLairReloadEncodedBytes || memcmp(bytes,"ARLDELY1",8) || bytes[11]) return false;
  ArRegionalLairReloads next={.initialized_towns=bytes[8],.approximate_towns=bytes[9],.diverged_towns=bytes[10]};
  if (!ArRegionalLairReloads_Valid(&next)) return false;
  for (unsigned p=0;p<2;++p) for (unsigned i=0;i<24;++i) {
    next.delay[p][i]=ByteOrder_ReadLe16(bytes+12+(p*24+i)*2);
    if (!(next.initialized_towns & (1u<<(i/4))) && next.delay[p][i]) return false;
  }
  *h=next; return true;
}
