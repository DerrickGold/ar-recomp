#ifndef AR_REGIONAL_LAIR_HISTORY_H
#define AR_REGIONAL_LAIR_HISTORY_H

#include "regional_costs.h"
#include <stddef.h>

enum {
  kArRegionalLairTowns = 6,
  kArRegionalLairsPerTown = 4,
  kArRegionalLairCount = 24,
  /* Independent binary behavior axes, not three redundant regional copies. */
  kArRegionalLairProjections = 32,
  kArRegionalLairHistoryEncodedBytes = 12 + 32*24*2,
};

typedef struct ArRegionalLairAccounting {
  ArRegionalSource seeds;
  ArRegionalSource house_credit;
  ArRegionalSource score_conversion;
  ArRegionalSource score_operation;
  ArRegionalSource score_route;
} ArRegionalLairAccounting;

/* Caller-owned retained projections of the SAME ordered semantic events.
 * Not a second town simulation. Native seals, actors and growth/rewards remain
 * authoritative; this accountant never produces any of those side effects.
 * Storage is bounded, with no allocation, WRAM, filesystem or renderer access.
 * Zero initialization means no town history, not exhausted/sealed lairs.
 * The arrays are internal projection order, NOT a public persistence format. */
typedef struct ArRegionalLairHistory {
  uint8_t initialized_towns;
  uint8_t approximate_towns;
  uint8_t diverged_towns; /* Retain evidence after an unaccounted native/cheat write. */
  uint16_t stock[kArRegionalLairProjections][kArRegionalLairCount];
} ArRegionalLairHistory;

bool ArRegionalLairHistory_Valid(const ArRegionalLairHistory *history);
bool ArRegionalLairHistory_MarkDiverged(ArRegionalLairHistory *history, unsigned town);
bool ArRegionalLairAccounting_Projection(const ArRegionalLairAccounting *policy,
                                         unsigned *projection);
typedef struct ArRegionalLairSeedDescriptor {
  const char *key;
  uint16_t stock[kArRegionalSource_Count];
} ArRegionalLairSeedDescriptor;
/* Stable table-slot keys and resolved counts for persistence/validation. */
const ArRegionalLairSeedDescriptor *ArRegionalLair_SeedDescriptor(unsigned lair);
bool ArRegionalLair_Seed(ArRegionalSource source, unsigned lair, uint16_t *stock);
typedef struct ArRegionalHouseCreditDescriptor {
  const char *key;
  uint16_t tiered[kArRegionalSource_Count];
} ArRegionalHouseCreditDescriptor;
const ArRegionalHouseCreditDescriptor *ArRegionalHouseCredit_Descriptor(void);
/* Actual destroyed-house subtype, not the town's current technology level. */
bool ArRegionalHouseCredit_Units(ArRegionalSource source, uint8_t subtype, uint16_t *units);
/* Fresh native initialization only; rejects an already initialized town. */
bool ArRegionalLairHistory_InitTown(ArRegionalLairHistory *history, unsigned town);
/* After explicit legacy acknowledgement. Known native source, not UI locale.
 * Preserve the source stock, including over-seed values; estimate once only.
 * Seals are neither inferred from the counts nor modified by adoption. */
bool ArRegionalLairHistory_AdoptTown(ArRegionalLairHistory *history, unsigned town,
    ArRegionalSource source, const uint16_t remaining[kArRegionalLairsPerTown]);
bool ArRegionalLairHistory_Read(const ArRegionalLairHistory *history,
    const ArRegionalLairAccounting *policy, unsigned lair, uint16_t *remaining);

typedef enum ArRegionalLairProjectionResult {
  kArRegionalLairProjection_Ready,
  kArRegionalLairProjection_Unknown,
  kArRegionalLairProjection_Diverged,
  kArRegionalLairProjection_Mismatch,
  kArRegionalLairProjection_Invalid,
} ArRegionalLairProjectionResult;
/* Safe-boundary projection, not a new estimate. Validate ALL current native
 * counters before producing any replacement. No history/reward/seal mutation;
 * failure leaves output untouched. Caller owns native writes and activation. */
ArRegionalLairProjectionResult ArRegionalLairHistory_Project(
    const ArRegionalLairHistory *history,
    const ArRegionalLairAccounting *current, const ArRegionalLairAccounting *target,
    const uint16_t native[kArRegionalLairCount], uint16_t out[kArRegionalLairCount]);

/* Observe candidate attempts BEFORE the active stock's zero early-out. Caller
 * owns matching an actor/lair and miracle flag/position/busy eligibility. Each
 * projection applies its own zero/sign gate. Native return owns the one reward.
 * A killed actor can belong to a sealed lair: do not invent a seal guard here. */
bool ArRegionalLairHistory_KillAttempt(ArRegionalLairHistory *history, unsigned lair);
bool ArRegionalLairHistory_MiracleAttempt(ArRegionalLairHistory *history, unsigned lair);
/* One actual house, in native callback order. Restart distribution per house;
 * sealed_mask contains only this town's four seal bits. All sealed is a stock
 * no-op, NOT permission to award the inactive profiles' growth. */
bool ArRegionalLairHistory_HouseLost(ArRegionalLairHistory *history, unsigned town,
                                   uint8_t subtype, unsigned sealed_mask);
/* One completed action-score settlement. Not the stock helper's intermediate
 * argument: each history independently converts the original stored BCD score
 * and applies its completion-count route. Never call on every scene/frame. */
bool ArRegionalLairHistory_SettleScore(ArRegionalLairHistory *history, unsigned town,
                                      uint16_t bcd_score, uint16_t completed_acts);

/* Explicit versioned feature block, not fwrite(struct). Capacity is checked
 * before any write; invalid/truncated/future blocks leave output unchanged.
 * The enclosing checkpoint owns checksums and binding to the native save.
 * Empty towns serialize only zero counters, never uninitialized host bytes. */
bool ArRegionalLairHistory_Encode(const ArRegionalLairHistory *history,
                                 uint8_t *bytes, size_t capacity);
bool ArRegionalLairHistory_Decode(const uint8_t *bytes, size_t size,
                                 ArRegionalLairHistory *history);

#endif
