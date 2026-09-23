#ifndef AR_REGIONAL_LAIR_RELOADS_H
#define AR_REGIONAL_LAIR_RELOADS_H

#include "regional_lair_history.h"

enum { kArRegionalLairReloadEncodedBytes = 12 + 2*24*2 };
typedef struct ArRegionalLairReloads {
  uint8_t initialized_towns, approximate_towns, diverged_towns;
  uint16_t delay[2][kArRegionalLairCount];
} ArRegionalLairReloads;
bool ArRegionalLair_Reload(ArRegionalSource source, unsigned lair, uint16_t *delay);
bool ArRegionalLairReloads_Valid(const ArRegionalLairReloads *history);
bool ArRegionalLairReloads_Init(ArRegionalLairReloads *history);
/* Old saves cannot reveal invisible US 1 -> 1 reductions. Infer the fewest
 * native quarter-plus-one transformations consistent with all four slots.
 * Preserve the actual source values. Unknown patterns quarantine the town;
 * every adopted town remains explicitly approximate, never claimed exact. */
bool ArRegionalLairReloads_Adopt(ArRegionalLairReloads *history, ArRegionalSource source,
                                const uint16_t native[kArRegionalLairCount]);
bool ArRegionalLairReloads_Check(ArRegionalLairReloads *history, ArRegionalSource source,
                                const uint16_t native[kArRegionalLairCount]);
/* One actual B6BF native modifier event, not a synthetic regional event. */
bool ArRegionalLairReloads_ReduceTown(ArRegionalLairReloads *history, unsigned town);
bool ArRegionalLairReloads_Encode(const ArRegionalLairReloads *history, uint8_t *out, size_t capacity);
bool ArRegionalLairReloads_Decode(const uint8_t *bytes, size_t size, ArRegionalLairReloads *history);

#endif
