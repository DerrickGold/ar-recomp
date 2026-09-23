#ifndef AR_REGIONAL_SESSION_H
#define AR_REGIONAL_SESSION_H

#include "regional_rules.h"
#include "regional_lair_history.h"
#include "regional_sim_actors.h"
#include "save_checkpoint.h"

/* Campaign identity is host-supplied (never inferred from name/locale/ROM).
 * Slot 0 is today's sole slot. This codec covers the integrated rule families;
 * legacy histories remain empty until the explicit adoption workflow. */
typedef struct ArRegionalSession {
  uint8_t campaign[16];
  uint32_t slot, revision;
  ArRegionalRules requested, effective;
  ArRegionalLairHistory lairs;
  ArRegionalLairReloads reloads;
  ArRegionalSimActors sim_actors;
  bool arrival_locked;
} ArRegionalSession;

/* Caller-owned state; no singleton, filesystem access or old-save mutation. */
bool ArRegionalSession_NewGame(ArRegionalSession *session, uint32_t slot,
    const uint8_t campaign[16], const ArRegionalCostPolicy *defaults);
bool ArRegionalSession_RequestArrival(ArRegionalSession *session,uint32_t revision,ArRegionalSource source);
bool ArRegionalSession_RequestActionMotion(ArRegionalSession *session,uint32_t revision,
    const ArRegionalActionMotionPolicy *policy);
/* Complete room initialization only: later settings never alter an existing
 * actor's current or future phases until the next room/retry. */
bool ArRegionalSession_BeginActionMotion(ArRegionalSession *session,ArRegionalActionMotionSnapshot *snapshot);
bool ArRegionalSession_RequestEmitters(ArRegionalSession *session,uint32_t revision,const ArRegionalEmitterPolicy *policy);
bool ArRegionalSession_BeginEmitters(ArRegionalSession *session,ArRegionalEmitterSnapshot *snapshot);
bool ArRegionalSession_RequestVolley(ArRegionalSession *session, uint32_t revision, ArRegionalSource source);
bool ArRegionalSession_BeginVolley(ArRegionalSession *session, bool *double_shot);
bool ArRegionalSession_RequestBosses(ArRegionalSession *session,uint32_t revision,const ArRegionalBossPolicy *policy);
bool ArRegionalSession_BeginBosses(ArRegionalSession *session,ArRegionalBossSnapshot *snapshot);
bool ArRegionalSession_RequestDifficulty(ArRegionalSession *session, uint32_t revision,
    const ArRegionalDifficultyPolicy *policy);
/* Room/retry boundary: never modify a live enemy, hit or countdown. */
bool ArRegionalSession_BeginDifficulty(ArRegionalSession *session, ArRegionalDifficultySnapshot *snapshot);
bool ArRegionalSession_RequestScoreLives(ArRegionalSession *session,uint32_t revision,ArRegionalSource source);
/* Room/retry boundary, before any score additions. No retroactive awards. */
bool ArRegionalSession_BeginScoreLives(ArRegionalSession *session,bool *enabled);
bool ArRegionalSession_RequestActionStart(ArRegionalSession *session,uint32_t revision,const ArRegionalActionStartPolicy *policy);
/* Confirmed Action Mode new run only, never a room/retry or settings edit. */
bool ArRegionalSession_BeginActionStart(ArRegionalSession *session,ArRegionalActionStartSnapshot *snapshot);
bool ArRegionalSession_RequestInventory(ArRegionalSession *session,uint32_t revision,ArRegionalSource source);
bool ArRegionalSession_RequestModeEntry(ArRegionalSession *session,uint32_t revision,const ArRegionalModePolicy *policy);
bool ArRegionalSession_BeginModeEntry(ArRegionalSession *session,uint8_t *snapshot);
/* New Action run only; never reinterprets a live collection. */
bool ArRegionalSession_BeginInventory(ArRegionalSession *session,bool *enabled);
bool ArRegionalSession_RequestCollision(ArRegionalSession *session,uint32_t revision,const ArRegionalCollisionPolicy *policy);
bool ArRegionalSession_BeginCollision(ArRegionalSession *session,ArRegionalCollisionSnapshot *snapshot);
bool ArRegionalSession_RequestPlatformSkull(ArRegionalSession *session,uint32_t revision,const ArRegionalPlatformSkullPolicy *policy);
bool ArRegionalSession_BeginPlatformSkull(ArRegionalSession *session,ArRegionalPlatformSkullSnapshot *snapshot);
bool ArRegionalSession_RequestActorStats(ArRegionalSession *session,uint32_t revision,const ArRegionalActorStatsPolicy *policy);
bool ArRegionalSession_BeginActorStats(ArRegionalSession *session,ArRegionalActorStatsSnapshot *snapshot);
bool ArRegionalSession_RequestCastHold(ArRegionalSession *session,uint32_t revision,const ArRegionalCastHoldPolicy *policy);
bool ArRegionalSession_RequestFire(ArRegionalSession *session,uint32_t revision,const ArRegionalFirePolicy *policy);
bool ArRegionalSession_BeginFire(ArRegionalSession *session,ArRegionalFireSnapshot *snapshot);
bool ArRegionalSession_BeginCastHold(ArRegionalSession *session,ArRegionalCastHoldSnapshot *snapshot);
/* Once per campaign, at the eligible final departure/Palace gate. Continuing
 * an already-unlocked native event retains the saved effective policy rather
 * than adopting a new request midway through its reveal/announcement. A later
 * request never changes a locked event or clears the native story flags. */
bool ArRegionalSession_BeginArrival(ArRegionalSession *session,bool continuing,bool *japanese);
/* Conversion-owner ONLY: prepare a candidate session for an already confirmed,
 * recoverable town reset. Sets support, level goals and the two population
 * prerequisites together; preserves the independent Compass prerequisite.
 * No queued destructive intent is persisted. Caller commits this candidate
 * with the converted town image before publishing it as the active session.
 * A raw settings callback must not use this as a support activation shortcut. */
bool ArRegionalSession_SetPopulationProfile(ArRegionalSession *candidate,
    uint32_t revision, ArRegionalSource source);
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
bool ArRegionalSession_RequestLivesDisplay(ArRegionalSession *session, uint32_t revision,
                                         ArRegionalSource source);
/* Next action HUD redraw; never changes the stored number of attempts. */
bool ArRegionalSession_BeginLivesDisplay(ArRegionalSession *session, bool *zero_based);
bool ArRegionalSession_RequestSources(ArRegionalSession *session, uint32_t revision,
                                     const ArRegionalSourcesPolicy *policy);
/* Accepted Source collection only, not menu opening or explicit Use. */
bool ArRegionalSession_BeginSources(ArRegionalSession *session, ArRegionalSourcesSnapshot *snapshot);
bool ArRegionalSession_RequestSkullWait(ArRegionalSession *session, uint32_t revision, ArRegionalSource source);
bool ArRegionalSession_RequestStory(ArRegionalSession *session, uint32_t revision, const ArRegionalStoryPolicy *policy);
/* Next applicable native prerequisite check. No event-state mutation here. */
bool ArRegionalSession_BeginStory(ArRegionalSession *session, ArRegionalStorySnapshot *snapshot);
/* Capture before the complete Magic Skull use, including its picker/cancel. */
bool ArRegionalSession_BeginSkullWait(ArRegionalSession *session, uint16_t *frames);
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
bool ArRegionalSession_RequestLairSeeds(ArRegionalSession *session, uint32_t revision,
                                      ArRegionalSource source);
bool ArRegionalSession_RequestLairReloads(ArRegionalSession *session, uint32_t revision,
                                         ArRegionalSource source);
/* Stage on a copy; publish only after the adapter validates/replaces the
 * complete native reload table. Never touches running countdowns. */
bool ArRegionalSession_BeginLairReloads(ArRegionalSession *session);
bool ArRegionalSession_RequestTownStatus(ArRegionalSession *session, uint32_t revision,
                                        const ArRegionalTownStatusPolicy *policy);
/* Capture a whole construction/report transaction. No flag mutation here. */
bool ArRegionalSession_BeginTownStatus(ArRegionalSession *session, ArRegionalTownStatusSnapshot *snapshot);
bool ArRegionalSession_RequestLevelGoals(ArRegionalSession *session,uint32_t revision,ArRegionalSource source);
bool ArRegionalSession_BeginLevelGoals(ArRegionalSession *session,bool *japanese);
bool ArRegionalSession_RequestSimCombat(ArRegionalSession *session,uint32_t revision,
                                      const ArRegionalSimCombatPolicy *policy);
/* Verified native birth only. Existing/cached actors retain their snapshots. */
bool ArRegionalSession_BeginSimActor(ArRegionalSession *session,unsigned town,unsigned slot);
bool ArRegionalSession_RequestSimAi(ArRegionalSession *session,uint32_t revision,const ArRegionalSimAiPolicy *policy);
bool ArRegionalSession_RequestConstruction(ArRegionalSession *session, uint32_t revision, ArRegionalSource source);
/* Capture budget, animated payment and support-building return together.
 * Off-screen batches capture at their own outer native entry. */
bool ArRegionalSession_BeginConstruction(ArRegionalSession *session, bool *japanese);
bool ArRegionalSession_RequestHouseCredit(ArRegionalSession *session, uint32_t revision,
                                        ArRegionalSource source);
/* Internal leaves stay independently selectable; the ordinary overlay passes
 * a uniform policy for its grouped Act-score feedback control. */
bool ArRegionalSession_RequestScoreFeedback(ArRegionalSession *session, uint32_t revision,
                                          const ArRegionalScorePolicy *policy);
/* Called at the accepted clear-card boundary, after any safe stock-policy
 * activation. Captures phase without changing counters or awarding anything. */
bool ArRegionalSession_BeginScoreCompletion(ArRegionalSession *session,
                                           ArRegionalScoreSnapshot *snapshot);
/* Stage on a copied session at a safe town tick. Publish that session only if
 * the game adapter successfully projects all native stocks; never activate a
 * policy just because the UI requested it. Requires complete healthy history. */
bool ArRegionalSession_BeginLairAccounting(ArRegionalSession *session,
                                         ArRegionalLairAccounting *snapshot);

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
