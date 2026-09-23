#ifndef AR_REGIONAL_FINGERPRINT_H
#define AR_REGIONAL_FINGERPRINT_H

#include "regional_rules.h"
#include "regional_sim_actors.h"

/* Pricing-only replay identity. Includes requested AND effective numerical
 * rules, not campaign UUID, revision, source labels or presentation settings.
 * Equal US/European prices therefore retain baseline replay compatibility.
 * Invalid input leaves outputs unchanged. No allocation or filesystem access. */
bool ArRegionalCosts_Fingerprint(const ArRegionalCostPolicy *requested,
    const ArRegionalCostPolicy *effective, uint8_t out[32], bool *baseline);

/* Includes gameplay rules and report branches that change recorded input flow.
 * Adding numerically native rules preserves older digests byte-for-byte. Each
 * family's values retain their units, not source labels or profile IDs. */
/* Replay owners must also chain ArRegionalLairHistory_Fingerprint: rule values
 * alone cannot identify retained alternative stock histories. */
bool ArRegionalRules_Fingerprint(const ArRegionalRules *requested,
    const ArRegionalRules *effective,
    uint8_t out[32], bool *baseline);
/* A decided JP/mixed final arrival must not become eligible again on replay.
 * All-Western policy aliases preserve old replay identities. */
bool ArRegionalArrivalLock_Fingerprint(const uint8_t previous[32],ArRegionalSource requested,
    ArRegionalSource effective,bool locked,uint8_t out[32]);
/* Retained generations can remain Japanese after requested/effective return
 * to US. Include their identity separately, just like retained lair history. */
bool ArRegionalSimActors_Fingerprint(const uint8_t previous[32],const ArRegionalSimActors *actors,
                                    uint8_t out[32],bool *baseline);

#endif
