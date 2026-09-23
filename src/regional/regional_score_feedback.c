#include "regional_score_feedback.h"
#include <stddef.h>

static const ArRegionalScoreDescriptor kRules[kArRegionalScore_Count] = {
  {"score_jp_conversion", {0,1,0}},
  {"score_stock_subtract", {0,1,0}},
  {"score_stock_nonsecond", {0,1,0}},
  {"score_at_clear_card", {0,1,0}},
};
const ArRegionalScoreDescriptor *ArRegionalScore_Descriptor(ArRegionalScoreRule rule) {
  return (unsigned)rule < kArRegionalScore_Count ? &kRules[rule] : NULL;
}
bool ArRegionalScore_Init(ArRegionalScorePolicy *policy, ArRegionalSource source) {
  if (!policy || (unsigned)source >= kArRegionalSource_Count) return false;
  for (unsigned i=0; i<kArRegionalScore_Count; ++i) policy->source[i]=source;
  return true;
}
bool ArRegionalScore_Resolve(const ArRegionalScorePolicy *policy, ArRegionalScoreSnapshot *out) {
  if (!policy || !out) return false;
  ArRegionalScoreSnapshot next;
  for (unsigned i=0; i<kArRegionalScore_Count; ++i) {
    if ((unsigned)policy->source[i] >= kArRegionalSource_Count) return false;
    next.japanese[i]=kRules[i].japanese[policy->source[i]] != 0;
  }
  *out=next;
  return true;
}
bool ArRegionalScore_GroupSource(const ArRegionalScorePolicy *policy, ArRegionalSource *source) {
  ArRegionalScoreSnapshot snapshot;
  if (!source || !ArRegionalScore_Resolve(policy,&snapshot)) return false;
  bool same=true;
  for (unsigned i=1; i<kArRegionalScore_Count; ++i) same &= policy->source[i]==policy->source[0];
  if (same) { *source=policy->source[0]; return true; }
  for (unsigned i=1; i<kArRegionalScore_Count; ++i)
    if (snapshot.japanese[i]!=snapshot.japanese[0]) return false;
  *source=snapshot.japanese[0] ? kArRegionalSource_Japan : kArRegionalSource_US;
  return true;
}
bool ArRegionalScore_Convert(ArRegionalSource source, uint16_t bcd_score, uint16_t *units) {
  if (!units || (unsigned)source >= kArRegionalSource_Count) return false;
  unsigned score=0, place=1;
  for (unsigned n=0; n<4; ++n) {
    const unsigned digit=(bcd_score>>(n*4)) & 15;
    if (digit>9) return false;
    score+=digit*place; place*=10;
  }
  *units=(uint16_t)(source==kArRegionalSource_Japan ?
      (score>650 ? (score-650)/32*10 : 0) : score/10*2);
  return true;
}
bool ArRegionalScore_Destination(ArRegionalSource source, uint16_t completed_acts,
                                 ArRegionalScoreDestination *destination) {
  if (!destination || (unsigned)source >= kArRegionalSource_Count) return false;
  *destination=completed_acts==2 ? kArRegionalScoreDestination_Growth :
      (source==kArRegionalSource_Japan || completed_acts==1) ?
      kArRegionalScoreDestination_Stocks : kArRegionalScoreDestination_None;
  return true;
}
