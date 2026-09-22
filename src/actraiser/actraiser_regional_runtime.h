#ifndef ACTRAISER_REGIONAL_RUNTIME_H
#define ACTRAISER_REGIONAL_RUNTIME_H

#include "regional/regional_campaign.h"
#include "snesrecomp/game/cpu.h"

/* After SaveSystem Attach/Load, before the first CPU dispatch. */
bool ActRaiserRegional_Initialize(ArRegionalCampaignIdentity identity, void *context);
bool ActRaiserRegional_ReplayDigest(void *unused, uint8_t out[32], bool *baseline);
/* Game-thread snapshot. While a miracle is open, its complete captured prices
 * win over pending edits. Outside it, menus quote the next transaction. */
bool ActRaiserRegional_CopyPrices(ArRegionalCostSnapshot *prices);
bool ActRaiserRegional_MiracleEntry(const CpuState *cpu);
RecompReturn ActRaiserRegional_RunMiracle(CpuState *cpu);
bool ActRaiser_RegionalScrollEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalScrollCast(CpuState *cpu);
bool ActRaiser_RegionalTitleEntry(CpuState *cpu);
RecompReturn ActRaiser_RegionalTitle(CpuState *cpu);

#endif
