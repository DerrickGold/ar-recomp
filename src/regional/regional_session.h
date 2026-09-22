#ifndef AR_REGIONAL_SESSION_H
#define AR_REGIONAL_SESSION_H

#include "regional_costs.h"
#include "save_checkpoint.h"

/* Campaign identity is host-supplied (never inferred from name/locale/ROM).
 * Slot 0 is today's sole slot. This codec currently covers PRICING ONLY: it
 * does not set a legacy-history acknowledgement or claim lair tracking. */
typedef struct ArRegionalSession {
  uint8_t campaign[16];
  uint32_t slot, revision;
  ArRegionalCostPolicy requested, effective;
} ArRegionalSession;

/* Caller-owned state; no singleton, filesystem access or old-save mutation. */
bool ArRegionalSession_NewGame(ArRegionalSession *session, uint32_t slot,
    const uint8_t campaign[16], const ArRegionalCostPolicy *defaults);
/* A preview must carry revision; stale confirmations fail without mutation. */
bool ArRegionalSession_RequestCosts(ArRegionalSession *session, uint32_t revision,
    ArRegionalCostGroup group, ArRegionalCostSource source);
/* Next cast/miracle only: atomically activate that group's requested prices
 * and copy a value snapshot. Keep the snapshot for its complete gate/debit
 * transaction; later requests cannot change an already accepted price. */
bool ArRegionalSession_BeginCosts(ArRegionalSession *session,
    ArRegionalCostGroup group, ArRegionalCostSnapshot *quote);

/* Only Ready permits Continue. Missing requires a separate legacy adoption
 * decision; other statuses require recovery. Failure leaves session unchanged
 * but is NOT permission to resume it for the newly requested save. */
SaveCheckpointStatus ArRegionalSession_Load(ArRegionalSession *session,
    uint32_t slot, const char *native_path,
    const uint8_t image[kActRaiserSramSize], SaveError *error);
/* Explicit completed-save boundary, not NewGame/teardown/first SRAM change.
 * expected is the last loaded/successfully saved native image (NULL for a new
 * slot). Same-image commits support pricing changes without editing SRAM.
 * Unknown/corrupt companions must be resolved by Load/recovery beforehand. */
bool ArRegionalSession_Save(const ArRegionalSession *session, SaveFileFormat format,
    const char *native_path, const uint8_t *expected,
    const uint8_t image[kActRaiserSramSize], SaveError *error);

#endif
