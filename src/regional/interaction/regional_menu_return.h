#ifndef AR_REGIONAL_MENU_RETURN_H
#define AR_REGIONAL_MENU_RETURN_H
/* RegionalMenuReturn: whether the town menu stays open after a command, per region.
 * Phase: pure.
 * Tests: tests/actraiser_regional_runtime_test.c */

#include "regional/regional_costs.h"

typedef struct ArRegionalMenuReturnDescriptor {
  const char *key;
  uint16_t keep_open[kArRegionalSource_Count];
} ArRegionalMenuReturnDescriptor;

const ArRegionalMenuReturnDescriptor *ArRegionalMenuReturn_Descriptor(void);
bool ArRegionalMenuReturn_Resolve(ArRegionalSource source, bool *keep_open);

#endif
