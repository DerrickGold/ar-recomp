#ifndef ACTRAISER_REGIONAL_RUNTIME_H
#define ACTRAISER_REGIONAL_RUNTIME_H

#include "regional/regional_campaign.h"
#include "snesrecomp/game/cpu.h"

/* After SaveSystem Attach/Load, before the first CPU dispatch. */
bool ActRaiserRegional_Initialize(ArRegionalCampaignIdentity identity, void *context);
typedef enum ActRaiserRegionalPopulationNotice {
  kActRaiserRegionalPopulation_Confirm,
  kActRaiserRegionalPopulation_Failed,
  kActRaiserRegionalPopulation_Complete,
  kActRaiserRegionalPopulation_NamePending,
} ActRaiserRegionalPopulationNotice;
/* Value-only host decision. Counts identify buildings to remove in each of
 * six towns. The host does not receive a CPU, session or transaction pointer.
 * Only Confirm consumes the boolean response; other notices acknowledge. */
typedef bool (*ActRaiserRegionalPopulationPrompt)(void *context,
    ActRaiserRegionalPopulationNotice notice,ArRegionalSource source,const uint16_t removed[6]);
void ActRaiserRegional_SetPopulationPrompt(ActRaiserRegionalPopulationPrompt prompt,void *context);
bool ActRaiser_RegionalPopulationEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalPopulation(CpuState *cpu);
/* Read effective coefficients only; no activation in a census. US before a
 * campaign is accepted. Fixed small snapshot, not full session validation. */
bool ActRaiserRegional_CopySupport(ArRegionalSupportSnapshot *snapshot);
/* Cached numerical room policy, O(1), no per-animation session validation. */
ArRegionalActionMotionSnapshot ActRaiserRegional_ActionMotionSnapshot(void);
ArRegionalEmitterSnapshot ActRaiserRegional_EmitterSnapshot(void);
bool ActRaiserRegional_DoubleStatueVolley(void);
ArRegionalBossSnapshot ActRaiserRegional_BossSnapshot(void);
ArRegionalCollisionSnapshot ActRaiserRegional_CollisionSnapshot(void);
ArRegionalPlatformSkullSnapshot ActRaiserRegional_PlatformSkullSnapshot(void);
ArRegionalCastHoldSnapshot ActRaiserRegional_CastHoldSnapshot(void);
ArRegionalFireSnapshot ActRaiserRegional_FireSnapshot(void);
bool ActRaiserRegional_ActorStatsEnabled(void);
bool ActRaiserRegional_ActorStats(uint16_t actor,uint16_t native_hp,uint16_t native_attack,uint16_t *hp,uint16_t *attack);
/* Cached child value by semantic rule ID; UINT16_MAX before room activation
 * or for a non-child rule. No session or native memory exposed to callers. */
uint16_t ActRaiserRegional_ActorChildStat(unsigned rule);
typedef enum ActRaiserRegionalContinueNotice {
  kActRaiserRegionalContinue_Estimate,
  kActRaiserRegionalContinue_LoadFailed,
  kActRaiserRegionalContinue_SaveFailed,
} ActRaiserRegionalContinueNotice;
/* Host presents a localized, cancellable decision while suspending the game
 * coroutine. No renderer, SDL types or save paths cross this boundary. True
 * means acknowledge/retry; false means return to the unchanged title menu. */
typedef bool (*ActRaiserRegionalContinuePrompt)(void *context, ActRaiserRegionalContinueNotice notice);
void ActRaiserRegional_SetContinuePrompt(ActRaiserRegionalContinuePrompt prompt, void *context);
bool ActRaiser_RegionalContinueEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalContinue(CpuState *cpu);
bool ActRaiserRegional_ReplayDigest(void *unused, uint8_t out[32], bool *baseline);
/* SIM generation ownership. Cache notifications follow completed native
 * copies; collision lookup is O(1) and never activates pending settings. */
bool ActRaiserRegional_SimActorsReady(void);
void ActRaiserRegional_SimActorCache(bool load,unsigned town);
void ActRaiserRegional_SimActorBirth(unsigned town,unsigned slot);
bool ActRaiserRegional_SimActorSnapshot(unsigned town,unsigned slot,uint16_t *snapshot);
bool ActRaiserRegional_SimActorAiSnapshot(unsigned town,unsigned slot,uint16_t *snapshot);
/* Before completed native save capture, not every frame. Never repairs WRAM. */
void ActRaiserRegional_CheckLairHistory(CpuState *cpu);
bool ActRaiser_RegionalLairEntry(CpuState *cpu);
bool ActRaiser_RegionalLairSeedEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalLairSeed(CpuState *cpu);
RecompReturn ActRaiser_RegionalLairKill(CpuState *cpu);
RecompReturn ActRaiser_RegionalLairMiracle(CpuState *cpu);
RecompReturn ActRaiser_RegionalLairHouse(CpuState *cpu);
bool ActRaiser_RegionalHouseUnitsEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalHouseUnits(CpuState *cpu);
RecompReturn ActRaiser_RegionalLairScore(CpuState *cpu);
bool ActRaiser_RegionalScoreRouteEntry(CpuState *cpu);
bool ActRaiser_RegionalScoreConversionEntry(CpuState *cpu);
bool ActRaiser_RegionalScoreSubtractEntry(CpuState *cpu);
bool ActRaiser_RegionalScoreCardEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalScoreCard(CpuState *cpu);
bool ActRaiser_RegionalScoreDepartureEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalScoreDeparture(CpuState *cpu);
RecompReturn ActRaiser_RegionalScoreRoute(CpuState *cpu);
RecompReturn ActRaiser_RegionalScoreConversion(CpuState *cpu);
RecompReturn ActRaiser_RegionalScoreSubtract(CpuState *cpu);
/* Game thread, after completed NMI input sampling. Observes raw held buttons;
 * does not alter CPU, physical bindings, native input masks or the pad latch. */
void ActRaiserRegional_ObserveInputRelease(CpuState *cpu);
bool ActRaiser_RegionalMagicGestureEntry(CpuState *cpu);
bool ActRaiser_RegionalLivesDisplayEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalLivesDisplay(CpuState *cpu);
bool ActRaiser_RegionalSourceCollectionEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalSourceCollection(CpuState *cpu);
bool ActRaiser_RegionalSourceLifeKeepEntry(CpuState *cpu);
bool ActRaiser_RegionalSourceMagicKeepEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalSourceLifeKeep(CpuState *cpu);
RecompReturn ActRaiser_RegionalSourceMagicKeep(CpuState *cpu);
bool ActRaiser_RegionalSkullUseEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalSkullUse(CpuState *cpu);
bool ActRaiser_RegionalSkullSkipWaitEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalSkullSkipWait(CpuState *cpu);
bool ActRaiser_RegionalStoryThresholdEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalStoryThreshold(CpuState *cpu);
bool ActRaiser_RegionalStoryCompassEntry(CpuState *cpu);
bool ActRaiser_RegionalLairReductionEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalLairReduction(CpuState *cpu);
RecompReturn ActRaiser_RegionalStoryCompass(CpuState *cpu);
RecompReturn ActRaiser_RegionalMagicDedicated(CpuState *cpu);
RecompReturn ActRaiser_RegionalMagicAttack(CpuState *cpu);
/* Game-thread snapshot. While a miracle is open, its complete captured prices
 * win over pending edits. Outside it, menus quote the next transaction. */
bool ActRaiserRegional_CopyPrices(ArRegionalCostSnapshot *prices);
/* Called at native action-profile initialization, never from a frame update
 * or settings edit. Validates the profile and atomically captures pending
 * limits and motion for the entire room, including later actor spawns. */
bool ActRaiserRegional_BeginActionRoom(uint8_t profile, uint16_t native_bcd, uint16_t *out_bcd);
bool ActRaiserRegional_MiracleEntry(const CpuState *cpu);
RecompReturn ActRaiserRegional_RunMiracle(CpuState *cpu);
bool ActRaiserRegional_ReportCommandEntry(const CpuState *cpu);
RecompReturn ActRaiserRegional_RunReportCommand(CpuState *cpu);
bool ActRaiser_RegionalScrollEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalScrollCast(CpuState *cpu);
bool ActRaiser_RegionalRetryEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalRetry(CpuState *cpu);
bool ActRaiser_RegionalTownWaitEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalTownWait(CpuState *cpu);
bool ActRaiser_RegionalFishingEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalFishing(CpuState *cpu);
bool ActRaiser_RegionalDevelopmentEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalDevelopment(CpuState *cpu);
bool ActRaiser_RegionalEffectEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalEffect(CpuState *cpu);
RecompReturn ActRaiser_RegionalEffectVisuals(CpuState *cpu);
bool ActRaiser_RegionalRecoveryCycleEntry(CpuState *cpu);
bool ActRaiser_RegionalRecoveryDrainEntry(CpuState *cpu);
bool ActRaiser_RegionalRecoveryMotionEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalRecoveryCycle(CpuState *cpu);
RecompReturn ActRaiser_RegionalRecoveryDrain(CpuState *cpu);
RecompReturn ActRaiser_RegionalRecoveryMoving(CpuState *cpu);
RecompReturn ActRaiser_RegionalRecoveryStopped(CpuState *cpu);
bool ActRaiser_RegionalQuakePlayerEntry(CpuState *cpu);
bool ActRaiser_RegionalQuakePostedEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalQuakePlayer(CpuState *cpu);
RecompReturn ActRaiser_RegionalQuakePosted(CpuState *cpu);
bool ActRaiser_RegionalQuakeHousesEntry(CpuState *cpu);
bool ActRaiser_RegionalQuakeFieldsEntry(CpuState *cpu);
bool ActRaiser_RegionalQuakeClass3Entry(CpuState *cpu);
bool ActRaiser_RegionalQuakeClass4Entry(CpuState *cpu);
bool ActRaiser_RegionalQuakeClass5Entry(CpuState *cpu);
RecompReturn ActRaiser_RegionalQuakeHouses(CpuState *cpu);
RecompReturn ActRaiser_RegionalQuakeFields(CpuState *cpu);
RecompReturn ActRaiser_RegionalQuakeClass3(CpuState *cpu);
RecompReturn ActRaiser_RegionalQuakeClass4(CpuState *cpu);
RecompReturn ActRaiser_RegionalQuakeClass5(CpuState *cpu);
bool ActRaiser_RegionalMasterReportEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalMasterReport(CpuState *cpu);
bool ActRaiser_RegionalSkipScoreEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalSkipScore(CpuState *cpu);
bool ActRaiser_RegionalTitleEntry(CpuState *cpu);
bool ActRaiser_RegionalSpeedEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalSpeed(CpuState *cpu);
bool ActRaiser_RegionalSpeedPositionEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalSpeedPosition(CpuState *cpu);
bool ActRaiser_RegionalSpeedRightEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalSpeedRight(CpuState *cpu);
bool ActRaiser_RegionalSpeedScaleEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalSpeedScale(CpuState *cpu);
RecompReturn ActRaiser_RegionalTitle(CpuState *cpu);

#endif
