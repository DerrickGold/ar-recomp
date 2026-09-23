#ifndef ACTRAISER_REGIONAL_RUNTIME_H
#define ACTRAISER_REGIONAL_RUNTIME_H

#include "regional/regional_campaign.h"
#include "snesrecomp/game/cpu.h"

/* After SaveSystem Attach/Load, before the first CPU dispatch. */
bool ActRaiserRegional_Initialize(ArRegionalCampaignIdentity identity, void *context);
bool ActRaiserRegional_ReplayDigest(void *unused, uint8_t out[32], bool *baseline);
/* Game thread, after completed NMI input sampling. Observes raw held buttons;
 * does not alter CPU, physical bindings, native input masks or the pad latch. */
void ActRaiserRegional_ObserveInputRelease(CpuState *cpu);
bool ActRaiser_RegionalMagicGestureEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalMagicDedicated(CpuState *cpu);
RecompReturn ActRaiser_RegionalMagicAttack(CpuState *cpu);
/* Game-thread snapshot. While a miracle is open, its complete captured prices
 * win over pending edits. Outside it, menus quote the next transaction. */
bool ActRaiserRegional_CopyPrices(ArRegionalCostSnapshot *prices);
/* Called at native action-profile initialization, never from a frame update
 * or settings edit. Validates the profile before activating pending limits. */
bool ActRaiserRegional_BeginRoomTime(uint8_t profile, uint16_t native_bcd, uint16_t *out_bcd);
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
