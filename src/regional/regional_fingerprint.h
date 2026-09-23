#ifndef AR_REGIONAL_FINGERPRINT_H
#define AR_REGIONAL_FINGERPRINT_H

#include "regional_rules.h"

/* Pricing-only replay identity. Includes requested AND effective numerical
 * rules, not campaign UUID, revision, source labels or presentation settings.
 * Equal US/European prices therefore retain baseline replay compatibility.
 * Invalid input leaves outputs unchanged. No allocation or filesystem access. */
bool ArRegionalCosts_Fingerprint(const ArRegionalCostPolicy *requested,
    const ArRegionalCostPolicy *effective, uint8_t out[32], bool *baseline);

/* Includes gameplay rules and report branches that change recorded input flow.
 * Adding numerically native rules preserves older digests byte-for-byte. Each
 * family's values retain their units, not source labels or profile IDs. */
bool ArRegionalRules_Fingerprint(const ArRegionalRules *requested,
    const ArRegionalRules *effective,
    uint8_t out[32], bool *baseline);

#endif
