#ifndef AR_REGIONAL_HAZARDS_H
#define AR_REGIONAL_HAZARDS_H

#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

enum { kArRegionalHazards_Max = 21 };
typedef struct ArRegionalHazardBox {
  uint16_t left, width, top, height, damage_or_flag;
} ArRegionalHazardBox;
typedef struct ArRegionalHazards {
  unsigned count;
  ArRegionalHazardBox boxes[kArRegionalHazards_Max];
} ArRegionalHazards;
typedef struct ArRegionalHazardDescriptor {
  const char *key;
  uint16_t profile[kArRegionalSource_Count];
} ArRegionalHazardDescriptor;

const ArRegionalHazardDescriptor *ArRegionalHazards_Descriptor(void);
/* Profile identity represents ordered geometry AND damage, not a multiplier.
 * All European releases and both of their modes share this hazard profile. */
bool ArRegionalHazards_Resolve(ArRegionalSource source, uint8_t *profile);
/* Caller-owned values. scene uses room-high / area-low ordering. Invalid
 * profiles/scenes leave output untouched. Native expansion uses padded,
 * unsigned 16-bit rectangles; $80 boxes request slowing, not HP damage. */
bool ArRegionalHazards_Copy(uint8_t profile, uint16_t scene, ArRegionalHazards *out);

#endif
