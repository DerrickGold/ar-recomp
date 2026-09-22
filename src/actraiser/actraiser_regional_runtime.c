#include "actraiser_regional_runtime.h"

#include "actraiser/actraiser_hle_fatal.h"
#include "actraiser/actraiser_miracle.h"
#include "actraiser/actraiser_scroll_cast.h"
#include "actraiser/actraiser_regional_settings.h"
#include "input_replay.h"
#include "regional/regional_fingerprint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern RecompReturn bank_02_A622_M1X0(CpuState *cpu);
static ArRegionalCampaign s_campaign;
static bool s_delegate;
static ArRegionalCostPolicy s_boot_requested, s_boot_effective;
static bool s_boot_valid;
static bool s_miracle_active, s_prices_valid;
static bool s_trace;
static uint32_t s_prices_revision;
static ArRegionalCostSnapshot s_prices, s_miracle_prices;

bool ActRaiserRegional_Initialize(ArRegionalCampaignIdentity identity, void *context) {
  if (!identity) return false;
  s_delegate = false;
  s_miracle_active = s_prices_valid = false;
  s_trace = getenv("AR_REGIONAL_TRACE") != NULL;
  ArRegionalCampaign_Init(&s_campaign, 0, identity, context);
  ArRegionalCosts_Init(&s_boot_requested, kArRegionalCost_US);
  s_boot_effective = s_boot_requested;
  s_boot_valid = true;
  uint8_t image[kActRaiserSramSize];
  if (SaveSystem_CopyDurableImage(image)) {
    ArRegionalSession loaded;
    SaveError error = {{0}};
    SaveCheckpointStatus status = ArRegionalSession_Load(&loaded, 0,
        SaveSystem_ActivePath(), image, &error);
    s_boot_valid = status == kSaveCheckpoint_Ready || status == kSaveCheckpoint_Missing;
    if (status == kSaveCheckpoint_Ready) {
      s_boot_requested = loaded.requested;
      s_boot_effective = loaded.effective;
    }
  }
  SaveCommitHost host = ArRegionalCampaign_SaveHost(&s_campaign);
  return SaveSystem_SetCommitHost(&host);
}

bool ActRaiserRegional_CopyPrices(ArRegionalCostSnapshot *prices) {
  if (!prices) return false;
  if (s_miracle_active) { *prices = s_miracle_prices; return true; }
  if (!s_campaign.active_valid) {
    ArRegionalCostPolicy baseline;
    ArRegionalCosts_Init(&baseline, kArRegionalCost_US);
    return ArRegionalCosts_Resolve(&baseline, prices);
  }
  if (!s_prices_valid || s_prices_revision != s_campaign.active.revision) {
    if (!ArRegionalCosts_Resolve(&s_campaign.active.requested, &s_prices)) return false;
    s_prices_revision = s_campaign.active.revision;
    s_prices_valid = true;
  }
  *prices = s_prices;
  return true;
}

bool ActRaiserRegional_CopyPricingView(ActRaiserRegionalPricingView *out) {
  if (!out || !s_campaign.active_valid) return false;
  *out = (ActRaiserRegionalPricingView){.revision = s_campaign.active.revision,
      .requested = s_campaign.active.requested, .effective = s_campaign.active.effective,
      .editable = InputReplay_PolicyChangesAllowed(), .miracle_in_progress = s_miracle_active};
  memcpy(out->campaign, s_campaign.active.campaign, sizeof(out->campaign));
  return true;
}

ActRaiserRegionalEditResult ActRaiserRegional_RequestPricing(
    const ActRaiserRegionalPricingView *view, ArRegionalCostGroup group,
    ArRegionalCostSource source) {
  if (!view || !s_campaign.active_valid || (unsigned)group >= kArRegionalCostGroup_Count ||
      (unsigned)source >= kArRegionalCostSource_Count) return kActRaiserRegionalEdit_Invalid;
  if (!InputReplay_PolicyChangesAllowed()) return kActRaiserRegionalEdit_Locked;
  if (view->revision != s_campaign.active.revision ||
      memcmp(view->campaign, s_campaign.active.campaign, sizeof(view->campaign)))
    return kActRaiserRegionalEdit_Stale;
  if (!ArRegionalSession_RequestCosts(&s_campaign.active, view->revision, group, source))
    return kActRaiserRegionalEdit_Invalid;
  return view->revision == s_campaign.active.revision
      ? kActRaiserRegionalEdit_Unchanged : kActRaiserRegionalEdit_Applied;
}

bool ActRaiserRegional_MiracleEntry(const CpuState *cpu) {
  return s_campaign.active_valid && !s_miracle_active && cpu &&
      ActRaiserMiracle_Entry(cpu, (uint8_t)cpu->A);
}

RecompReturn ActRaiserRegional_RunMiracle(CpuState *cpu) {
  if (!ActRaiserRegional_MiracleEntry(cpu) ||
      !ArRegionalSession_BeginCosts(&s_campaign.active, kArRegionalCostGroup_Miracles,
                                   &s_miracle_prices))
    ActRaiserHleFatal("Cannot capture regional miracle transaction");
  s_miracle_active = true;
  const unsigned action=(uint8_t)cpu->A;
  if (s_trace) {
    ArRegionalCostRule rule;
    ActRaiserMiracle_Rule(action,&rule);
    fprintf(stderr,"[regional] miracle action=%u price=%u SP=%u begin\n",
            action,s_miracle_prices.price[rule],cpu_read16(cpu,0,0x0282));
  }
  const RecompReturn result = ActRaiserMiracle_Run(cpu, (uint8_t)cpu->A, &s_miracle_prices);
  s_miracle_active = false;
  if (s_trace) fprintf(stderr,"[regional] miracle action=%u return=%u SP=%u\n",
                       action,(unsigned)result,cpu_read16(cpu,0,0x0282));
  return result;
}

bool ActRaiserRegional_ReplayDigest(void *unused, uint8_t out[32], bool *baseline) {
  (void)unused;
  if (s_campaign.active_valid)
    return ArRegionalCosts_Fingerprint(&s_campaign.active.requested,
        &s_campaign.active.effective, out, baseline);
  return s_boot_valid && ArRegionalCosts_Fingerprint(&s_boot_requested,
      &s_boot_effective, out, baseline);
}

bool ActRaiser_RegionalScrollEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserScrollCast_Entry(cpu) &&
      cpu_read16(cpu, 0, 0x02ac) <= 4;
}

RecompReturn ActRaiser_RegionalScrollCast(CpuState *cpu) {
  ArRegionalCostSnapshot quote;
  if (!ActRaiser_RegionalScrollEntry(cpu) ||
      !ArRegionalSession_BeginCosts(&s_campaign.active, kArRegionalCostGroup_Scrolls, &quote))
    ActRaiserHleFatal("Cannot capture regional scroll transaction");
  const unsigned before = cpu_read8(cpu, 0, 0x21);
  const uint16_t continuation = ActRaiserScrollCast_Gate(cpu, &quote);
  if (s_trace) fprintf(stderr,"[regional] scroll spell=%u stock=%u->%u target=%04x\n",
      cpu_read16(cpu,0,0x02ac),before,cpu_read8(cpu,0,0x21),continuation);
  /* The surrounding native actor dispatch has pushed data below its original
   * host frame. Preserve the wrapper's inherited context, NOT current S.
   * The runtime owns that context and the wrapper still owns its own pop. */
  if (!cpu_hle_tailcall_request(continuation, 0x009de1))
    ActRaiserHleFatal("Regional scroll transfer requires a generated wrapper");
  return RECOMP_RETURN_TAILCALL;
}

bool ActRaiser_RegionalTitleEntry(CpuState *cpu) {
  if (s_delegate) {
    s_delegate = false;
    return false;
  }
  return cpu && cpu->PB == 2 && cpu->m_flag && !cpu->x_flag &&
      cpu->D == 0 && !cpu->emulation;
}

RecompReturn ActRaiser_RegionalTitle(CpuState *cpu) {
  /* The native title routine owns selection, checksum validation, restoration
   * and fades. Its normal return is accepted entry, including the no-save
   * path (which bypasses Continue's selection loop). No resources have run yet.
   * The original JSL frame remains owned by the generated routine. */
  s_delegate = true;
  RecompReturn result = bank_02_A622_M1X0(cpu);
  s_delegate = false;
  if (result != RECOMP_RETURN_NORMAL) return result;
  SaveError error = {{0}};
  bool ok;
  const unsigned selection = cpu_read8(cpu, 0, 0x0336);
  if (selection == 1) {
    uint8_t image[kActRaiserSramSize];
    ok = SaveSystem_CopyDurableImage(image) &&
        ArRegionalCampaign_Continue(&s_campaign, SaveSystem_ActivePath(), image, &error);
  } else if (selection == 0 || selection == 2) {
    ArRegionalCostPolicy defaults;
    ArRegionalCosts_Init(&defaults, kArRegionalCost_US);
    ok = ArRegionalCampaign_NewGame(&s_campaign, &defaults, &error);
  } else {
    ActRaiserHleFatal("Unknown accepted title selection: %u", selection);
  }
  if (!ok) ActRaiserHleFatal("Cannot enter regional campaign; saves preserved: %s",
                            error.message[0] ? error.message : "no durable save image");
  s_prices_valid = false; /* a different campaign can have the same revision */
  fprintf(stderr, "[regional] %s campaign pricing session ready\n",
          selection == 1 ? "continued" : "new");
  return result;
}
