#ifndef AR_REGIONAL_CAST_HOLD_H
#define AR_REGIONAL_CAST_HOLD_H
#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>
typedef enum ArRegionalCastHoldRule {
  kArRegionalCastHold_Left,
  kArRegionalCastHold_Upper,
  kArRegionalCastHold_Right,
  kArRegionalCastHold_Count
} ArRegionalCastHoldRule;
typedef struct ArRegionalCastHoldPolicy { ArRegionalSource source[kArRegionalCastHold_Count]; } ArRegionalCastHoldPolicy;
typedef uint8_t ArRegionalCastHoldSnapshot;
typedef struct ArRegionalCastHoldDescriptor { const char *key; uint16_t enabled[kArRegionalSource_Count]; } ArRegionalCastHoldDescriptor;
const ArRegionalCastHoldDescriptor *ArRegionalCastHold_Descriptor(unsigned rule);
bool ArRegionalCastHold_Init(ArRegionalCastHoldPolicy *policy,ArRegionalSource source);
bool ArRegionalCastHold_Resolve(const ArRegionalCastHoldPolicy *policy,ArRegionalCastHoldSnapshot *snapshot);
bool ArRegionalCastHold_GroupSource(const ArRegionalCastHoldPolicy *policy,ArRegionalSource *source);
#endif
