#include "regional/regional_lair_fingerprint.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ArRegionalLairAccounting Policy(unsigned bits) {
  return (ArRegionalLairAccounting){bits&1 ? 1:0, bits&2 ? 1:0,
      bits&4 ? 1:0, bits&8 ? 1:0, bits&16 ? 1:0};
}
int main(void) {
  ArRegionalLairHistory h={0};
  for(unsigned town=0; town<6; ++town) {
    assert(ArRegionalLairHistory_InitTown(&h,town));
    assert(ArRegionalLairHistory_HouseLost(&h,town,0x20,town&3));
    assert(ArRegionalLairHistory_SettleScore(&h,town,0x9800,town%3));
    assert(ArRegionalLairHistory_KillAttempt(&h,town*4));
  }
  const ArRegionalLairHistory before=h;
  uint8_t prior[32]={0x55}, digest[32], active_digest[32], pending_digest[32];
  bool native;
  uint16_t out[24], source[24], sentinel[24];
  memset(sentinel,0xa5,sizeof(sentinel));
  for(unsigned from=0; from<32; ++from) for(unsigned to=0; to<32; ++to) {
    const ArRegionalLairAccounting current=Policy(from), target=Policy(to);
    memcpy(source,h.stock[from],sizeof(source));
    assert(ArRegionalLairHistory_Project(&h,&current,&target,source,out)==kArRegionalLairProjection_Ready);
    assert(!memcmp(out,h.stock[to],sizeof(out)) && !memcmp(&h,&before,sizeof(h)));
    /* Every round trip recovers its retained source, not a reverse estimate. */
    assert(ArRegionalLairHistory_Project(&h,&target,&current,out,out)==kArRegionalLairProjection_Ready);
    assert(!memcmp(out,source,sizeof(out)));
    assert(ArRegionalLairHistory_Fingerprint(prior,&h,&target,&current,digest,&native));
    assert(native==(!from && !to));
    assert((!memcmp(prior,digest,sizeof(prior)))==native);
    for(unsigned lair=0; lair<24; ++lair) {
      source[lair]^=1;
      memcpy(out,sentinel,sizeof(out));
      assert(ArRegionalLairHistory_Project(&h,&current,&target,source,out)==kArRegionalLairProjection_Mismatch);
      assert(!memcmp(out,sentinel,sizeof(out)));
      source[lair]^=1;
    }
  }
  const ArRegionalLairAccounting us={0}, jp=Policy(1), eu={2,2,2,2,2};
  assert(ArRegionalLairHistory_Fingerprint(prior,&h,&eu,&us,digest,&native) && native);
  assert(!memcmp(prior,digest,sizeof(prior)));
  assert(ArRegionalLairHistory_Fingerprint(prior,&h,&jp,&us,pending_digest,&native) && !native);
  assert(ArRegionalLairHistory_Fingerprint(prior,&h,&us,&jp,active_digest,&native) && !native);
  assert(memcmp(pending_digest,active_digest,sizeof(active_digest)));
  for(unsigned p=0; p<32; ++p) for(unsigned i=0; i<24; ++i) {
    h.stock[p][i]^=1;
    assert(ArRegionalLairHistory_Fingerprint(prior,&h,&jp,&us,digest,&native));
    assert(memcmp(pending_digest,digest,sizeof(digest)));
    h.stock[p][i]^=1;
  }
  memcpy(source,h.stock[0],sizeof(source));
  h.diverged_towns=8;
  assert(ArRegionalLairHistory_Project(&h,&us,&jp,source,out)==kArRegionalLairProjection_Diverged);
  assert(ArRegionalLairHistory_Fingerprint(prior,&h,&jp,&us,digest,&native));
  assert(memcmp(pending_digest,digest,sizeof(digest)));
  h=(ArRegionalLairHistory){0};
  assert(ArRegionalLairHistory_Project(&h,&us,&jp,source,out)==kArRegionalLairProjection_Unknown);
  memset(digest,0xa5,sizeof(digest)); native=true;
  assert(!ArRegionalLairHistory_Fingerprint(prior,&h,&jp,&us,digest,&native));
  assert(digest[0]==0xa5 && native);
  assert(ArRegionalLairHistory_Fingerprint(prior,&h,&us,&eu,digest,&native) && native);
  assert(!ArRegionalLairHistory_Fingerprint(NULL,&h,&us,&eu,digest,&native));
  const ArRegionalLairAccounting invalid={.seeds=kArRegionalSource_Count};
  assert(ArRegionalLairHistory_Project(&h,&us,&invalid,source,out)==kArRegionalLairProjection_Invalid);
  assert(!ArRegionalLairHistory_Fingerprint(prior,&h,&invalid,&us,digest,&native));
  puts("lair projections: all 1024 switches, mismatch atomicity and retained-history replay identity passed");
  return 0;
}
