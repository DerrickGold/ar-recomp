#ifndef AR_REGIONAL_PLATFORM_SKULL_H
#define AR_REGIONAL_PLATFORM_SKULL_H
#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>
enum { kArRegionalPlatformSkull_Armor, kArRegionalPlatformSkull_Reward,
  kArRegionalPlatformSkull_RangeX, kArRegionalPlatformSkull_RangeY, kArRegionalPlatformSkull_Count };
typedef struct ArRegionalPlatformSkullPolicy { ArRegionalSource source[kArRegionalPlatformSkull_Count]; } ArRegionalPlatformSkullPolicy;
typedef uint8_t ArRegionalPlatformSkullSnapshot;
typedef struct ArRegionalPlatformSkullDescriptor { const char *key; uint16_t value[kArRegionalSource_Count]; } ArRegionalPlatformSkullDescriptor;
const ArRegionalPlatformSkullDescriptor *ArRegionalPlatformSkull_Descriptor(unsigned rule);
bool ArRegionalPlatformSkull_Init(ArRegionalPlatformSkullPolicy *policy,ArRegionalSource source);
bool ArRegionalPlatformSkull_Resolve(const ArRegionalPlatformSkullPolicy *policy,ArRegionalPlatformSkullSnapshot *snapshot);
bool ArRegionalPlatformSkull_GroupSource(const ArRegionalPlatformSkullPolicy *policy,ArRegionalSource *source);
/* UINT16_MAX on invalid rule/snapshot. Reward is packed BCD in native tens;
 * ranges are exclusive hot-point distances, not placement coordinates. */
uint16_t ArRegionalPlatformSkull_Value(ArRegionalPlatformSkullSnapshot snapshot,unsigned rule);
#endif
