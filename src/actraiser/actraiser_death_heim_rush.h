#ifndef AR_ACTRAISER_DEATH_HEIM_RUSH_H
#define AR_ACTRAISER_DEATH_HEIM_RUSH_H

/* Value-only Death Heim recipe and encounter lifecycle. Native arena loading,
 * CPU continuations, campaign storage and settings belong to their owners.
 * Tests: tests/actraiser_death_heim_rush_test.c */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum ActRaiserDeathHeimRoster {
  kActRaiserDeathHeimRoster_Standard,
  kActRaiserDeathHeimRoster_Act1,
  kActRaiserDeathHeimRoster_Extended,
} ActRaiserDeathHeimRoster;

enum {
  kActRaiserDeathHeimRushAlgorithm = 1,
  kActRaiserDeathHeimRushConfigBytes = 16,
  kActRaiserDeathHeimRushSeedMax = 999999999,
  kActRaiserDeathHeimRushCityCount = 6,
  kActRaiserDeathHeimRushMaxEncounters = 12,
  kActRaiserDeathHeimRush_FinalEncounter = 12,
  kActRaiserDeathHeimRush_NoEncounter = 255,
};

typedef struct ActRaiserDeathHeimRushConfig {
  uint32_t seed;
  uint8_t algorithm, roster;
  bool shuffle_order;
} ActRaiserDeathHeimRushConfig;

ActRaiserDeathHeimRushConfig ActRaiserDeathHeimRushConfig_Default(void);
bool ActRaiserDeathHeimRushConfig_Valid(const ActRaiserDeathHeimRushConfig *config);
bool ActRaiserDeathHeimRushConfig_IsNative(const ActRaiserDeathHeimRushConfig *config);
/* Canonical fixed-width codec. Invalid input leaves the destination unchanged.
 * Legacy absence is handled by the campaign owner using Default(). */
bool ActRaiserDeathHeimRushConfig_Encode(
    const ActRaiserDeathHeimRushConfig *config,
    uint8_t out[kActRaiserDeathHeimRushConfigBytes]);
bool ActRaiserDeathHeimRushConfig_Decode(const uint8_t *bytes, size_t size,
                                       ActRaiserDeathHeimRushConfig *out);

typedef struct ActRaiserDeathHeimEncounter {
  /* City is 1..6 and act is 1..2. The final boss has city=act=0.
   * Ordinary identity is (city - 1) * 2 + act - 1, independent of order.
   * Native map fields are separate: the packed ROM key is map<<8|group. */
  uint8_t id, city, act, map_group, map;
  uint16_t source;
} ActRaiserDeathHeimEncounter;

const ActRaiserDeathHeimEncounter *ActRaiserDeathHeimRush_Encounter(unsigned id);

typedef enum ActRaiserDeathHeimRushPhase {
  kActRaiserDeathHeimRush_Inactive,
  kActRaiserDeathHeimRush_Hub,
  kActRaiserDeathHeimRush_Preparing,
  kActRaiserDeathHeimRush_Combat,
  kActRaiserDeathHeimRush_Returning,
  kActRaiserDeathHeimRush_Retry,
  kActRaiserDeathHeimRush_Final,
  kActRaiserDeathHeimRush_Complete,
  kActRaiserDeathHeimRush_Abandoned,
} ActRaiserDeathHeimRushPhase;

typedef struct ActRaiserDeathHeimRushToken {
  uint64_t run_id;
  uint32_t attempt;
  uint8_t encounter_id;
} ActRaiserDeathHeimRushToken;

typedef struct ActRaiserDeathHeimRush {
  ActRaiserDeathHeimRushConfig config;
  uint64_t run_id;
  uint32_t attempt;
  uint16_t completed_mask;
  uint8_t sequence[kActRaiserDeathHeimRushMaxEncounters];
  uint8_t count, index, active_id, last_completed;
  ActRaiserDeathHeimRushPhase phase;
} ActRaiserDeathHeimRush;

/* Initialize storage to zero before first use. The runtime supplies a nonzero
 * increasing run ID; Begin rejects reuse and replacement of an active run.
 * A token must be captured when the verified root is admitted, never invented
 * from current state by a delayed victory callback. Retry invalidates it. */
bool ActRaiserDeathHeimRush_Begin(ActRaiserDeathHeimRush *rush,
                                const ActRaiserDeathHeimRushConfig *config,
                                uint64_t run_id);
bool ActRaiserDeathHeimRush_Active(const ActRaiserDeathHeimRush *rush);
const ActRaiserDeathHeimEncounter *ActRaiserDeathHeimRush_Current(
    const ActRaiserDeathHeimRush *rush);
bool ActRaiserDeathHeimRush_Prepare(ActRaiserDeathHeimRush *rush,
                                  ActRaiserDeathHeimRushToken *token);
bool ActRaiserDeathHeimRush_Ready(ActRaiserDeathHeimRush *rush,
                                ActRaiserDeathHeimRushToken token);
bool ActRaiserDeathHeimRush_Victory(ActRaiserDeathHeimRush *rush,
                                  ActRaiserDeathHeimRushToken token);
bool ActRaiserDeathHeimRush_ReturnToHub(ActRaiserDeathHeimRush *rush);
bool ActRaiserDeathHeimRush_RetryEncounter(ActRaiserDeathHeimRush *rush,
                                         ActRaiserDeathHeimRushToken token);
bool ActRaiserDeathHeimRush_Abandon(ActRaiserDeathHeimRush *rush);
bool ActRaiserDeathHeimRush_FinalReady(const ActRaiserDeathHeimRush *rush);
/* Bits 0/1 mean the screen-left Act 1 / screen-right Act 2 eyes are lit.
 * Six-fight rosters extinguish both for the selected boss of that city.
 * Returns zero for an invalid city or a run without a valid recipe. */
uint8_t ActRaiserDeathHeimRush_EyeMask(const ActRaiserDeathHeimRush *rush,
                                     unsigned city);
/* Compatibility only: incomplete rosters project to 0..5, all preliminary
 * victories to 6, and final victory to 7. Never counts twelve into native RAM.
 * Native epilogue advancement to 8 remains outside this model. */
uint8_t ActRaiserDeathHeimRush_NativeProgress(const ActRaiserDeathHeimRush *rush);

#endif
