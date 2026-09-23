#ifndef AR_REGIONAL_SESSION_H
#define AR_REGIONAL_SESSION_H

#include "regional_rules.h"
#include "save_checkpoint.h"

/* Campaign identity is host-supplied (never inferred from name/locale/ROM).
 * Slot 0 is today's sole slot. This codec covers the integrated rule families;
 * it does not set a legacy-history acknowledgement or claim lair tracking. */
typedef struct ArRegionalSession {
  uint8_t campaign[16];
  uint32_t slot, revision;
  ArRegionalRules requested, effective;
} ArRegionalSession;

/* Caller-owned state; no singleton, filesystem access or old-save mutation. */
bool ArRegionalSession_NewGame(ArRegionalSession *session, uint32_t slot,
    const uint8_t campaign[16], const ArRegionalCostPolicy *defaults);
/* A preview must carry revision; stale confirmations fail without mutation. */
bool ArRegionalSession_RequestCosts(ArRegionalSession *session, uint32_t revision,
    ArRegionalCostGroup group, ArRegionalSource source);
/* Next cast/miracle only: atomically activate that group's requested prices
 * and copy a value snapshot. Keep the snapshot for its complete gate/debit
 * transaction; later requests cannot change an already accepted price. */
bool ArRegionalSession_BeginCosts(ArRegionalSession *session,
    ArRegionalCostGroup group, ArRegionalCostSnapshot *quote);
bool ArRegionalSession_RequestTimers(ArRegionalSession *session, uint32_t revision,
                                     ArRegionalSource source);
/* Native timer initialization only. Never called by a settings edit or tick. */
bool ArRegionalSession_BeginTimers(ArRegionalSession *session,
                                   ArRegionalTimerPolicy *snapshot);
bool ArRegionalSession_RequestRetryScore(ArRegionalSession *session, uint32_t revision,
                                         ArRegionalSource source);
/* Accepted nonzero native retry marker, before it is consumed. */
bool ArRegionalSession_BeginRetryScore(ArRegionalSession *session, bool *clear_score);
bool ArRegionalSession_RequestTownWait(ArRegionalSession *session, uint32_t revision,
                                      ArRegionalSource source);
/* Only when the current native state-3 countdown expires. */
bool ArRegionalSession_BeginTownWait(ArRegionalSession *session, uint16_t *updates);
bool ArRegionalSession_RequestFishing(ArRegionalSession *session, uint32_t revision,
                                     ArRegionalSource source);
/* Accepted Fillmore fishing callback only. Reconcile existing progress only
 * when the numerical target changed; source-label-only edits are not resets. */
bool ArRegionalSession_BeginFishing(ArRegionalSession *session, uint16_t *updates,
                                   bool *reconcile);
bool ArRegionalSession_RequestDevelopment(ArRegionalSession *session, uint32_t revision,
                                         ArRegionalSource source);
/* Eligible master entry at the start of a long cycle. Effects use the current
 * effective snapshot and cannot activate pending clock changes themselves. */
bool ArRegionalSession_BeginDevelopment(ArRegionalSession *session,
                                       ArRegionalDevelopmentSnapshot *snapshot);
bool ArRegionalSession_RequestRecovery(ArRegionalSession *session, uint32_t revision,
                                      ArRegionalSource source);
/* changed bits identify numerical changes requiring queue/phase retirement. */
bool ArRegionalSession_BeginRecovery(ArRegionalSession *session,
    ArRegionalRecoverySnapshot *snapshot, unsigned *changed);
bool ArRegionalSession_RequestQuake(ArRegionalSession *session, uint32_t revision,
                                    ArRegionalSource source);
/* Capture once for a complete player or posted earthquake, never per record. */
bool ArRegionalSession_BeginQuake(ArRegionalSession *session, ArRegionalQuakeSnapshot *snapshot);
bool ArRegionalSession_RequestScorePage(ArRegionalSession *session, uint32_t revision,
                                       ArRegionalSource source);
/* Shared Palace/SIM Master report opening, never when a button is pressed. */
bool ArRegionalSession_BeginScorePage(ArRegionalSession *session, bool *enabled);
bool ArRegionalSession_RequestMenuReturn(ArRegionalSession *session, uint32_t revision,
                                        ArRegionalSource source);
/* Accepted SIM report/log/speed command, independent of the report itself. */
bool ArRegionalSession_BeginMenuReturn(ArRegionalSession *session, bool *keep_open);
bool ArRegionalSession_RequestSpeedRange(ArRegionalSession *session, uint32_t revision,
                                       ArRegionalSource source);
/* Captured at selector opening. Does not modify the saved text-speed value. */
bool ArRegionalSession_BeginSpeedRange(ArRegionalSession *session, uint16_t *maximum);
bool ArRegionalSession_RequestMagicGesture(ArRegionalSession *session, uint32_t revision,
                                         ArRegionalSource source);
/* After a complete input sample. Held controls retain the effective gesture;
 * a released sample can activate the request without creating a button edge. */
bool ArRegionalSession_BeginMagicGesture(ArRegionalSession *session,
                                        bool controls_released, bool *up_attack);

/* Only Ready permits Continue. Missing requires a separate legacy adoption
 * decision; other statuses require recovery. Failure leaves session unchanged
 * but is NOT permission to resume it for the newly requested save. */
SaveCheckpointStatus ArRegionalSession_Load(ArRegionalSession *session,
    uint32_t slot, const char *native_path,
    const uint8_t image[kActRaiserSramSize], SaveError *error);
/* Explicit completed-save boundary, not NewGame/teardown/first SRAM change.
 * expected is the last loaded/successfully saved native image (NULL for a new
 * slot). Same-image commits support rule changes without editing SRAM.
 * Unknown/corrupt companions must be resolved by Load/recovery beforehand. */
bool ArRegionalSession_Save(const ArRegionalSession *session, SaveFileFormat format,
    const char *native_path, const uint8_t *expected,
    const uint8_t image[kActRaiserSramSize], SaveError *error);

#endif
