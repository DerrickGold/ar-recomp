#ifndef AR_REGIONAL_FIRE_ENEMY_H
#define AR_REGIONAL_FIRE_ENEMY_H
#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>
typedef enum ArRegionalFireRule {
  kArRegionalFire_Curve,
  kArRegionalFire_CloseStrategy,
  kArRegionalFire_ChildThreshold,
  kArRegionalFire_BounceThreshold,
  kArRegionalFire_Count
} ArRegionalFireRule;
typedef struct ArRegionalFirePolicy { ArRegionalSource source[kArRegionalFire_Count]; } ArRegionalFirePolicy;
typedef uint8_t ArRegionalFireSnapshot;
typedef struct ArRegionalFireDescriptor { const char *key;uint16_t value[kArRegionalSource_Count]; } ArRegionalFireDescriptor;
const ArRegionalFireDescriptor *ArRegionalFire_Descriptor(unsigned rule);
bool ArRegionalFire_Init(ArRegionalFirePolicy *policy,ArRegionalSource source);
bool ArRegionalFire_Resolve(const ArRegionalFirePolicy *policy,ArRegionalFireSnapshot *snapshot);
bool ArRegionalFire_GroupSource(const ArRegionalFirePolicy *policy,ArRegionalSource *source);
uint16_t ArRegionalFire_Value(ArRegionalFireSnapshot snapshot,unsigned rule);
/* Exact Western row input, signed movement before native facing transforms.
 * Rejected rows leave both outputs unchanged. No random draw or allocation. */
bool ArRegionalFire_CurveRow(ArRegionalFireSnapshot snapshot,unsigned state,unsigned row,
    uint8_t visual,uint16_t duration,int16_t *dx,int16_t *dy);
#endif
