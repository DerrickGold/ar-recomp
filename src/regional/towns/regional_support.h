#ifndef AR_REGIONAL_SUPPORT_H
#define AR_REGIONAL_SUPPORT_H
#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

/* Support admits future houses; it is not the population in standing houses.
 * Native structure flags and bridge storage belong to the game adapter. */
typedef enum ArRegionalSupportRule {
  kArRegionalSupport_RegularField,
  kArRegionalSupport_UpgradedField,
  kArRegionalSupport_Factory3,
  kArRegionalSupport_Factory4,
  kArRegionalSupport_Other,
  kArRegionalSupport_Count
} ArRegionalSupportRule;
typedef struct ArRegionalSupportPolicy { ArRegionalSource source[kArRegionalSupport_Count]; } ArRegionalSupportPolicy;
typedef struct ArRegionalSupportSnapshot { uint16_t amount[kArRegionalSupport_Count]; } ArRegionalSupportSnapshot;
typedef struct ArRegionalSupportDescriptor { const char *key; uint16_t amount[kArRegionalSource_Count]; } ArRegionalSupportDescriptor;
const ArRegionalSupportDescriptor *ArRegionalSupport_Descriptor(ArRegionalSupportRule rule);
bool ArRegionalSupport_Init(ArRegionalSupportPolicy *policy,ArRegionalSource source);
bool ArRegionalSupport_Resolve(const ArRegionalSupportPolicy *policy,ArRegionalSupportSnapshot *snapshot);
bool ArRegionalSupport_Valid(const ArRegionalSupportSnapshot *snapshot);
bool ArRegionalSupport_GroupSource(const ArRegionalSupportPolicy *policy,ArRegionalSource *source);
#endif
