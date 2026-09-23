#ifndef AR_REGIONAL_LAIR_FINGERPRINT_H
#define AR_REGIONAL_LAIR_FINGERPRINT_H

#include "regional_lair_history.h"
#include "regional_lair_reloads.h"

bool ArRegionalLairReloads_Fingerprint(const uint8_t prior[32], const ArRegionalLairReloads *history,
    ArRegionalSource requested, ArRegionalSource effective, uint8_t out[32], bool *native);

/* Chain onto the other gameplay rules. Numerical US/Europe projections preserve
 * the prior digest exactly: their inactive histories cannot affect a replay
 * while policy edits are locked. Otherwise bind both selected projections and
 * the complete canonical history. Revision/UUID/source labels are excluded.
 * Failure leaves output/baseline unchanged. Caller owns baseline aggregation. */
bool ArRegionalLairHistory_Fingerprint(const uint8_t prior[32],
    const ArRegionalLairHistory *history,
    const ArRegionalLairAccounting *requested, const ArRegionalLairAccounting *effective,
    uint8_t out[32], bool *native);

#endif
