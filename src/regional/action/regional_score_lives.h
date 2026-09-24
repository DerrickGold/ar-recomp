#ifndef AR_REGIONAL_SCORE_LIVES_H
#define AR_REGIONAL_SCORE_LIVES_H

#include "regional/regional_source.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct ArRegionalScoreLivesDescriptor {
  const char *key;
  uint16_t enabled[kArRegionalSource_Count];
} ArRegionalScoreLivesDescriptor;
const ArRegionalScoreLivesDescriptor *ArRegionalScoreLives_Descriptor(void);
bool ArRegionalScoreLives_Resolve(ArRegionalSource source, bool *enabled);
/* Native PAL tests score bands, not the number of thresholds crossed. The
 * score owner supplies its already saturated, stored BCD result. */
bool ArRegionalScoreLives_Award(uint16_t before, uint16_t after, bool action_mode);
/* Native decimal increment, including 99 -> 00. Do not use the capped pickup
 * helper: it intentionally has a different overflow contract. */
uint8_t ArRegionalScoreLives_Increment(uint8_t bcd_spares);

#endif
