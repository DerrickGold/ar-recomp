#include "regional/towns/regional_lair_history.h"
#include "regional/towns/regional_stock.h"
#include "regional/towns/regional_score_feedback.h"
#include "byte_order.h"
#include <string.h>

enum { kSeedJP=1, kHouseJP=2, kConversionJP=4, kOperationJP=8, kRouteJP=16 };
#define SEED(slot,us,jp) {"lair_seed_" #slot, {us,jp,us}}
static const ArRegionalLairSeedDescriptor kSeeds[kArRegionalLairCount] = {
  SEED(00,200,250), SEED(01,100,125), SEED(02,100,125), SEED(03,100,100),
  SEED(04,100,150), SEED(05,50,50), SEED(06,90,100), SEED(07,90,100),
  SEED(08,110,220), SEED(09,125,250), SEED(10,125,250), SEED(11,90,180),
  SEED(12,80,170), SEED(13,80,90), SEED(14,80,170), SEED(15,80,170),
  SEED(16,50,200), SEED(17,60,130), SEED(18,50,200), SEED(19,50,70),
  SEED(20,30,150), SEED(21,30,100), SEED(22,60,150), SEED(23,60,100),
};
#undef SEED
static const ArRegionalHouseCreditDescriptor kHouseCredit = {"house_credit_tiered", {1,0,1}};

const ArRegionalHouseCreditDescriptor *ArRegionalHouseCredit_Descriptor(void) {
  return &kHouseCredit;
}
bool ArRegionalHouseCredit_Units(ArRegionalSource source, uint8_t subtype, uint16_t *units) {
  if (!units || (unsigned)source >= kArRegionalSource_Count) return false;
  *units = (uint16_t)(4 + (kHouseCredit.tiered[source] ? ((subtype & 0x30) >> 3) : 0));
  return true;
}

bool ArRegionalLairHistory_Valid(const ArRegionalLairHistory *h) {
  return h && !(h->initialized_towns & ~0x3fu) &&
      !(h->approximate_towns & ~h->initialized_towns) &&
      !(h->diverged_towns & ~h->initialized_towns);
}
static bool Ready(const ArRegionalLairHistory *h, unsigned town) {
  return ArRegionalLairHistory_Valid(h) && town < kArRegionalLairTowns &&
      (h->initialized_towns & (1u << town)) && !(h->diverged_towns & (1u << town));
}
bool ArRegionalLairHistory_MarkDiverged(ArRegionalLairHistory *h, unsigned town) {
  if (!ArRegionalLairHistory_Valid(h) || town>=kArRegionalLairTowns ||
      !(h->initialized_towns & (1u << town))) return false;
  h->diverged_towns |= (uint8_t)(1u << town);
  return true;
}
bool ArRegionalLairAccounting_Projection(const ArRegionalLairAccounting *p, unsigned *out) {
  if (!p || !out || (unsigned)p->seeds >= kArRegionalSource_Count ||
      (unsigned)p->house_credit >= kArRegionalSource_Count ||
      (unsigned)p->score_conversion >= kArRegionalSource_Count ||
      (unsigned)p->score_operation >= kArRegionalSource_Count ||
      (unsigned)p->score_route >= kArRegionalSource_Count) return false;
  *out = (p->seeds == kArRegionalSource_Japan ? kSeedJP : 0) |
      (p->house_credit == kArRegionalSource_Japan ? kHouseJP : 0) |
      (p->score_conversion == kArRegionalSource_Japan ? kConversionJP : 0) |
      (p->score_operation == kArRegionalSource_Japan ? kOperationJP : 0) |
      (p->score_route == kArRegionalSource_Japan ? kRouteJP : 0);
  return true;
}
bool ArRegionalLair_Seed(ArRegionalSource source, unsigned lair, uint16_t *stock) {
  if (!stock || (unsigned)source >= kArRegionalSource_Count || lair >= kArRegionalLairCount)
    return false;
  *stock=kSeeds[lair].stock[source];
  return true;
}
const ArRegionalLairSeedDescriptor *ArRegionalLair_SeedDescriptor(unsigned lair) {
  return lair < kArRegionalLairCount ? &kSeeds[lair] : NULL;
}
bool ArRegionalLairHistory_InitTown(ArRegionalLairHistory *h, unsigned town) {
  if (!ArRegionalLairHistory_Valid(h) || town >= kArRegionalLairTowns ||
      (h->initialized_towns & (1u << town))) return false;
  for (unsigned p=0;p<kArRegionalLairProjections;++p)
    for (unsigned i=town*4;i<town*4+4;++i) h->stock[p][i]=kSeeds[i].stock[!!(p & kSeedJP)];
  h->initialized_towns |= (uint8_t)(1u << town);
  return true;
}
bool ArRegionalLairHistory_AdoptTown(ArRegionalLairHistory *h, unsigned town,
    ArRegionalSource source, const uint16_t remaining[4]) {
  if (!ArRegionalLairHistory_Valid(h) || !remaining || town >= kArRegionalLairTowns ||
      (h->initialized_towns & (1u << town)) || (unsigned)source >= kArRegionalSource_Count) return false;
  uint16_t estimates[2][4];
  for (unsigned seed=0;seed<2;++seed) for (unsigned n=0;n<4;++n) {
    uint32_t value;
    if (!ArRegionalStock_Estimate(remaining[n],kSeeds[town*4+n].stock[source],
                                  kSeeds[town*4+n].stock[seed],UINT16_MAX,&value)) return false;
    estimates[seed][n]=(uint16_t)value;
  }
  for (unsigned p=0;p<kArRegionalLairProjections;++p)
    for (unsigned n=0;n<4;++n) h->stock[p][town*4+n]=estimates[!!(p & kSeedJP)][n];
  h->initialized_towns |= (uint8_t)(1u << town);
  h->approximate_towns |= (uint8_t)(1u << town);
  return true;
}
bool ArRegionalLairHistory_Read(const ArRegionalLairHistory *h,
    const ArRegionalLairAccounting *policy, unsigned lair, uint16_t *remaining) {
  unsigned projection;
  if (!remaining || lair >= kArRegionalLairCount || !Ready(h,lair/4) ||
      !ArRegionalLairAccounting_Projection(policy,&projection)) return false;
  *remaining=h->stock[projection][lair];
  return true;
}
bool ArRegionalLairHistory_KillAttempt(ArRegionalLairHistory *h, unsigned lair) {
  if (lair >= kArRegionalLairCount || !Ready(h,lair/4)) return false;
  for (unsigned p=0;p<kArRegionalLairProjections;++p)
    if (h->stock[p][lair]) --h->stock[p][lair];
  return true;
}

ArRegionalLairProjectionResult ArRegionalLairHistory_Project(
    const ArRegionalLairHistory *h, const ArRegionalLairAccounting *current,
    const ArRegionalLairAccounting *target, const uint16_t native[kArRegionalLairCount],
    uint16_t out[kArRegionalLairCount]) {
  unsigned from, to;
  if (!native || !out || !ArRegionalLairHistory_Valid(h) ||
      !ArRegionalLairAccounting_Projection(current, &from) ||
      !ArRegionalLairAccounting_Projection(target, &to)) return kArRegionalLairProjection_Invalid;
  if (h->diverged_towns) return kArRegionalLairProjection_Diverged;
  if (h->initialized_towns != 0x3f) return kArRegionalLairProjection_Unknown;
  for (unsigned i=0; i<kArRegionalLairCount; ++i)
    if (native[i] != h->stock[from][i]) return kArRegionalLairProjection_Mismatch;
  memmove(out, h->stock[to], sizeof(h->stock[to]));
  return kArRegionalLairProjection_Ready;
}
bool ArRegionalLairHistory_MiracleAttempt(ArRegionalLairHistory *h, unsigned lair) {
  if (lair >= kArRegionalLairCount || !Ready(h,lair/4)) return false;
  for (unsigned p=0;p<kArRegionalLairProjections;++p) {
    if (!h->stock[p][lair]) continue;
    const uint16_t result=(uint16_t)(h->stock[p][lair]-10u);
    /* Native SBC followed by BPL, not an unsigned saturating subtraction.
     * Preserve the sign-bit contract even for an externally edited high word. */
    h->stock[p][lair]=(result & 0x8000u) ? 0 : result;
  }
  return true;
}
bool ArRegionalLairHistory_HouseLost(ArRegionalLairHistory *h, unsigned town,
                                   uint8_t subtype, unsigned sealed_mask) {
  if (!Ready(h,town) || sealed_mask > 15) return false;
  if (sealed_mask==15) return true;
  for (unsigned p=0;p<kArRegionalLairProjections;++p) {
    uint16_t units;
    ArRegionalHouseCredit_Units((p & kHouseJP) ? kArRegionalSource_Japan : kArRegionalSource_US,
                                subtype, &units);
    for (unsigned awarded=0,slot=0;awarded<units;slot=(slot+1)&3) {
      if (sealed_mask & (1u << slot)) continue;
      /* Native INC wraps at 16 bits; the seed is not a storage limit. */
      h->stock[p][town*4+slot]=(uint16_t)(h->stock[p][town*4+slot]+1u);
      ++awarded;
    }
  }
  return true;
}
bool ArRegionalLairHistory_SettleScore(ArRegionalLairHistory *h, unsigned town,
                                      uint16_t bcd_score, uint16_t completed_acts) {
  if (!Ready(h,town)) return false;
  uint16_t converted[2];
  ArRegionalScoreDestination destination[2];
  for (unsigned source=0; source<2; ++source)
    if (!ArRegionalScore_Convert((ArRegionalSource)source,bcd_score,&converted[source]) ||
        !ArRegionalScore_Destination((ArRegionalSource)source,completed_acts,&destination[source])) return false;
  for (unsigned p=0;p<kArRegionalLairProjections;++p) {
    if (destination[!!(p & kRouteJP)]!=kArRegionalScoreDestination_Stocks) continue;
    const unsigned delta=converted[!!(p & kConversionJP)]>>2;
    for (unsigned n=town*4;n<town*4+4;++n) {
      const unsigned before=h->stock[p][n];
      h->stock[p][n]=(p & kOperationJP) ?
          (uint16_t)(before>=delta ? before-delta : 0) : (uint16_t)(before+delta);
    }
  }
  return true;
}

/* ARLHIST1: initialized/approximate/diverged masks at8/9/10, reserved zero
 * byte11, then LE16 words in
 * projection-major / stable lair-table order. Projection bits are this
 * format's fixed behavior identities (seed, house, conversion, operation,
 * route; 1=JP behavior), never ArRegionalSource enum ordinals. */
static const uint8_t kHistoryMagic[8]={'A','R','L','H','I','S','T','1'};
static bool Canonical(const ArRegionalLairHistory *h) {
  if (!ArRegionalLairHistory_Valid(h)) return false;
  for (unsigned l=0;l<kArRegionalLairCount;++l)
    if (!(h->initialized_towns & (1u << (l/4))))
      for (unsigned p=0;p<kArRegionalLairProjections;++p)
        if (h->stock[p][l]) return false;
  return true;
}
bool ArRegionalLairHistory_Encode(const ArRegionalLairHistory *h, uint8_t *bytes, size_t capacity) {
  if (!bytes || capacity<kArRegionalLairHistoryEncodedBytes || !Canonical(h)) return false;
  memcpy(bytes,kHistoryMagic,8);
  bytes[8]=h->initialized_towns; bytes[9]=h->approximate_towns;
  bytes[10]=h->diverged_towns; bytes[11]=0;
  size_t at=12;
  for (unsigned p=0;p<kArRegionalLairProjections;++p)
    for (unsigned l=0;l<kArRegionalLairCount;++l,at+=2) ByteOrder_WriteLe16(bytes+at,h->stock[p][l]);
  return true;
}
bool ArRegionalLairHistory_Decode(const uint8_t *bytes, size_t size, ArRegionalLairHistory *h) {
  if (!bytes || !h || size!=kArRegionalLairHistoryEncodedBytes ||
      memcmp(bytes,kHistoryMagic,8) || bytes[11]) return false;
  ArRegionalLairHistory next={.initialized_towns=bytes[8],.approximate_towns=bytes[9],
                             .diverged_towns=bytes[10]};
  size_t at=12;
  for (unsigned p=0;p<kArRegionalLairProjections;++p)
    for (unsigned l=0;l<kArRegionalLairCount;++l,at+=2) next.stock[p][l]=ByteOrder_ReadLe16(bytes+at);
  if (!Canonical(&next)) return false;
  *h=next;
  return true;
}
