#ifndef AR_REGIONAL_STOCK_H
#define AR_REGIONAL_STOCK_H

#include <stdbool.h>
#include <stdint.h>

/* One-time legacy bootstrap, NOT the live multi-profile stock accountant.
 * Preserve source stock/seals separately; never reverse-map this estimate on
 * each toggle. The save owner must validate seed provenance, distinguish an
 * uninitialized town, obtain consent and persist before advancing Continue.
 * Results remain approximate even after future events are tracked exactly.
 * Invalid inputs leave output unchanged. No SRAM or filesystem access. */
bool ArRegionalStock_Estimate(uint32_t remaining, uint32_t source_seed,
                              uint32_t target_seed, uint32_t storage_max,
                              uint32_t *estimate);

#endif
