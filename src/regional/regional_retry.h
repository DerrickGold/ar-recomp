#ifndef AR_REGIONAL_RETRY_H
#define AR_REGIONAL_RETRY_H

#include <stdbool.h>
#include <stdint.h>
#include "regional_source.h"

typedef struct ArRegionalRetryDescriptor {
  const char *key;
  uint16_t clear_score[kArRegionalSource_Count];
} ArRegionalRetryDescriptor;

/* Checkpoint retry only. Not the death branch, Palace departure, game-over,
 * act-clear reward or new-run reset. Those retain their distinct owners. */
const ArRegionalRetryDescriptor *ArRegionalRetry_Descriptor(void);
bool ArRegionalRetry_Resolve(ArRegionalSource source, bool *clear_score);

#endif
