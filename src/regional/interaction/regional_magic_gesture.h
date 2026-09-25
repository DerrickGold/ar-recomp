#ifndef AR_REGIONAL_MAGIC_GESTURE_H
#define AR_REGIONAL_MAGIC_GESTURE_H
/* RegionalMagicGesture: whether magic is cast with the up-attack gesture, per region.
 * Phase: pure.
 * Tests: tests/actraiser_magic_gesture_test.c */

#include "regional/regional_costs.h"

typedef struct ArRegionalMagicGestureDescriptor {
  const char *key;
  uint16_t up_attack[kArRegionalSource_Count];
} ArRegionalMagicGestureDescriptor;

const ArRegionalMagicGestureDescriptor *ArRegionalMagicGesture_Descriptor(void);
bool ArRegionalMagicGesture_Resolve(ArRegionalSource source, bool *up_attack);

#endif
