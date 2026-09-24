#ifndef AR_REGIONAL_SCORE_FEEDBACK_H
#define AR_REGIONAL_SCORE_FEEDBACK_H

#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

/* Independent rules. Conversion also determines the second-act growth
 * award; subtraction applies only to the stock route. No rewards are emitted
 * by this value layer. Phase is prospective: one actual score event updates
 * every retained stock projection, not an invented second clear/tally. */
typedef enum ArRegionalScoreRule {
  kArRegionalScore_Conversion,
  kArRegionalScore_Operation,
  kArRegionalScore_Route,
  kArRegionalScore_Phase,
  kArRegionalScore_Count,
} ArRegionalScoreRule;
typedef struct ArRegionalScorePolicy {
  ArRegionalSource source[kArRegionalScore_Count];
} ArRegionalScorePolicy;
typedef struct ArRegionalScoreSnapshot {
  bool japanese[kArRegionalScore_Count];
} ArRegionalScoreSnapshot;
typedef struct ArRegionalScoreDescriptor {
  const char *key;
  uint16_t japanese[kArRegionalSource_Count];
} ArRegionalScoreDescriptor;
typedef enum ArRegionalScoreDestination {
  kArRegionalScoreDestination_None,
  kArRegionalScoreDestination_Stocks,
  kArRegionalScoreDestination_Growth,
} ArRegionalScoreDestination;

const ArRegionalScoreDescriptor *ArRegionalScore_Descriptor(ArRegionalScoreRule rule);
bool ArRegionalScore_Init(ArRegionalScorePolicy *policy, ArRegionalSource source);
bool ArRegionalScore_Resolve(const ArRegionalScorePolicy *policy, ArRegionalScoreSnapshot *out);
bool ArRegionalScore_GroupSource(const ArRegionalScorePolicy *policy, ArRegionalSource *source);
/* Valid four-digit packed-BCD score only. Failure leaves output unchanged. */
bool ArRegionalScore_Convert(ArRegionalSource source, uint16_t bcd_score, uint16_t *units);
bool ArRegionalScore_Destination(ArRegionalSource source, uint16_t completed_acts,
                                 ArRegionalScoreDestination *destination);

#endif
