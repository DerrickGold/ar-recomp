#include "actraiser_regional_runtime.h"

#include "actraiser/actraiser_hle_fatal.h"
#include "actraiser/actraiser_miracle.h"
#include "actraiser/actraiser_scroll_cast.h"
#include "actraiser/actraiser_retry.h"
#include "actraiser/actraiser_town_wait.h"
#include "actraiser/actraiser_fishing.h"
#include "actraiser/actraiser_development.h"
#include "actraiser/actraiser_recovery.h"
#include "actraiser/actraiser_quake.h"
#include "actraiser/actraiser_report_command.h"
#include "actraiser/actraiser_speed_selector.h"
#include "actraiser/actraiser_magic_gesture.h"
#include "actraiser/actraiser_localization_runtime.h"
#include "actraiser/actraiser_regional_settings.h"
#include "input_replay.h"
#include "regional/regional_fingerprint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern RecompReturn bank_02_A622_M1X0(CpuState *cpu);
extern RecompReturn bank_01_97E5_M1X0(CpuState *cpu);
extern RecompReturn bank_01_9840_M0X0(CpuState *cpu);
extern RecompReturn bank_01_899B_M1X0(CpuState *cpu);
extern RecompReturn bank_01_8AF5_M1X0(CpuState *cpu);
extern RecompReturn bank_01_8C98_M1X0(CpuState *cpu);
static ArRegionalCampaign s_campaign;
static bool s_delegate;
static ArRegionalRules s_boot_requested, s_boot_effective;
static bool s_boot_valid;
static bool s_miracle_active, s_prices_valid;
static bool s_trace;
static bool s_quake_active, s_quake_delegate;
static ArRegionalQuakeSnapshot s_quake;
static unsigned s_quake_selections[kArRegionalQuake_Count]; /* diagnostic only */
static bool s_report_active, s_report_delegate, s_report_score_page;
static bool s_report_command_active;
static bool s_speed_active, s_speed_delegate, s_speed_scale_delegate;
static uint16_t s_speed_maximum;
static uint32_t s_prices_revision;
static ArRegionalCostSnapshot s_prices, s_miracle_prices;

bool ActRaiserRegional_Initialize(ArRegionalCampaignIdentity identity, void *context) {
  if (!identity) return false;
  s_delegate = false;
  s_miracle_active = s_prices_valid = false;
  s_quake_active = s_quake_delegate = false;
  s_report_active = s_report_delegate = false;
  s_report_command_active = false;
  s_speed_active = s_speed_delegate = s_speed_scale_delegate = false;
  s_speed_maximum = 9;
  s_trace = getenv("AR_REGIONAL_TRACE") != NULL;
  ArRegionalCampaign_Init(&s_campaign, 0, identity, context);
  ArRegionalCosts_Init(&s_boot_requested.costs, kArRegionalSource_US);
  ArRegionalTimers_Init(&s_boot_requested.timers, kArRegionalSource_US);
  s_boot_requested.retry_score = kArRegionalSource_US;
  s_boot_requested.town_wait = kArRegionalSource_US;
  s_boot_requested.fishing = kArRegionalSource_US;
  ArRegionalDevelopment_Init(&s_boot_requested.development,kArRegionalSource_US);
  ArRegionalRecovery_Init(&s_boot_requested.recovery, kArRegionalSource_US);
  ArRegionalQuake_Init(&s_boot_requested.quake, kArRegionalSource_US);
  s_boot_requested.score_page = kArRegionalSource_US;
  s_boot_requested.menu_return = kArRegionalSource_US;
  s_boot_requested.speed_range = kArRegionalSource_US;
  s_boot_requested.magic_gesture = kArRegionalSource_US;
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
    ArRegionalCosts_Init(&baseline, kArRegionalSource_US);
    return ArRegionalCosts_Resolve(&baseline, prices);
  }
  if (!s_prices_valid || s_prices_revision != s_campaign.active.revision) {
    if (!ArRegionalCosts_Resolve(&s_campaign.active.requested.costs, &s_prices)) return false;
    s_prices_revision = s_campaign.active.revision;
    s_prices_valid = true;
  }
  *prices = s_prices;
  return true;
}

bool ActRaiserRegional_CopyRulesView(ActRaiserRegionalRulesView *out) {
  if (!out || !s_campaign.active_valid) return false;
  *out = (ActRaiserRegionalRulesView){.revision = s_campaign.active.revision,
      .requested = s_campaign.active.requested, .effective = s_campaign.active.effective,
      .editable = InputReplay_PolicyChangesAllowed(), .miracle_in_progress = s_miracle_active};
  memcpy(out->campaign, s_campaign.active.campaign, sizeof(out->campaign));
  return true;
}

ActRaiserRegionalEditResult ActRaiserRegional_RequestRules(
    const ActRaiserRegionalRulesView *view, ActRaiserRegionalSettingGroup group,
    ArRegionalSource source) {
  if (!view || !s_campaign.active_valid || (unsigned)group >= kActRaiserRegionalSetting_Count ||
      (unsigned)source >= kArRegionalSource_Count) return kActRaiserRegionalEdit_Invalid;
  if (!InputReplay_PolicyChangesAllowed()) return kActRaiserRegionalEdit_Locked;
  if (view->revision != s_campaign.active.revision ||
      memcmp(view->campaign, s_campaign.active.campaign, sizeof(view->campaign)))
    return kActRaiserRegionalEdit_Stale;
  bool ok;
  switch (group) {
    case kActRaiserRegionalSetting_RoomTimes:
      ok = ArRegionalSession_RequestTimers(&s_campaign.active, view->revision, source); break;
    case kActRaiserRegionalSetting_RetryScore:
      ok = ArRegionalSession_RequestRetryScore(&s_campaign.active, view->revision, source); break;
    case kActRaiserRegionalSetting_TownWait:
      ok = ArRegionalSession_RequestTownWait(&s_campaign.active, view->revision, source); break;
    case kActRaiserRegionalSetting_Fishing:
      ok = ArRegionalSession_RequestFishing(&s_campaign.active, view->revision, source); break;
    case kActRaiserRegionalSetting_Development:
      ok = ArRegionalSession_RequestDevelopment(&s_campaign.active,view->revision,source);break;
    case kActRaiserRegionalSetting_Recovery:
      ok = ArRegionalSession_RequestRecovery(&s_campaign.active, view->revision, source); break;
    case kActRaiserRegionalSetting_Quake:
      ok = ArRegionalSession_RequestQuake(&s_campaign.active, view->revision, source); break;
    case kActRaiserRegionalSetting_ScorePage:
      ok = ArRegionalSession_RequestScorePage(&s_campaign.active, view->revision, source); break;
    case kActRaiserRegionalSetting_MenuReturn:
      ok = ArRegionalSession_RequestMenuReturn(&s_campaign.active, view->revision, source); break;
    case kActRaiserRegionalSetting_SpeedRange:
      ok = ArRegionalSession_RequestSpeedRange(&s_campaign.active, view->revision, source); break;
    case kActRaiserRegionalSetting_MagicGesture:
      ok = ArRegionalSession_RequestMagicGesture(&s_campaign.active, view->revision, source); break;
    case kActRaiserRegionalSetting_Scrolls:
      ok = ArRegionalSession_RequestCosts(&s_campaign.active, view->revision,
                                         kArRegionalCostGroup_Scrolls, source); break;
    case kActRaiserRegionalSetting_Miracles:
      ok = ArRegionalSession_RequestCosts(&s_campaign.active, view->revision,
                                         kArRegionalCostGroup_Miracles, source); break;
    default: return kActRaiserRegionalEdit_Invalid;
  }
  if (!ok)
    return kActRaiserRegionalEdit_Invalid;
  return view->revision == s_campaign.active.revision
      ? kActRaiserRegionalEdit_Unchanged : kActRaiserRegionalEdit_Applied;
}

bool ActRaiserRegional_BeginRoomTime(uint8_t profile, uint16_t native_bcd, uint16_t *out_bcd) {
  if (!out_bcd) return false;
  if (!s_campaign.active_valid) { *out_bcd = native_bcd; return true; }
  uint16_t resolved;
  ArRegionalTimerPolicy snapshot;
  if (!ArRegionalTimers_Resolve(&s_campaign.active.requested.timers, profile, native_bcd, &resolved) ||
      !ArRegionalSession_BeginTimers(&s_campaign.active, &snapshot)) return false;
  *out_bcd = resolved;
  if (s_trace) fprintf(stderr, "[regional] room profile=%02x time=%04x->%04x\n",
                       profile, native_bcd, resolved);
  return true;
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
    return ArRegionalRules_Fingerprint(&s_campaign.active.requested,
        &s_campaign.active.effective, out, baseline);
  return s_boot_valid && ArRegionalRules_Fingerprint(&s_boot_requested,
      &s_boot_effective, out, baseline);
}

bool ActRaiserRegional_ReportCommandEntry(const CpuState *cpu) {
  return s_campaign.active_valid && !s_report_command_active && cpu &&
      ActRaiserReportCommand_Entry(cpu, (uint8_t)cpu->A);
}
RecompReturn ActRaiserRegional_RunReportCommand(CpuState *cpu) {
  bool keep_open;
  if (!ActRaiserRegional_ReportCommandEntry(cpu) ||
      !ArRegionalSession_BeginMenuReturn(&s_campaign.active, &keep_open))
    ActRaiserHleFatal("Cannot capture regional command return");
  const unsigned action = (uint8_t)cpu->A;
  s_report_command_active = true;
  if (s_trace) fprintf(stderr, "[regional] report command=%u keep-open=%u begin\n", action, keep_open);
  const RecompReturn result = ActRaiserReportCommand_Run(cpu, action, keep_open);
  s_report_command_active = false;
  if (s_trace) fprintf(stderr, "[regional] report command=%u return=%u\n", action, (unsigned)result);
  return result;
}

bool ActRaiser_RegionalScrollEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserScrollCast_Entry(cpu) &&
      cpu_read16(cpu, 0, 0x02ac) <= 4;
}

bool ActRaiser_RegionalRetryEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserRetry_Entry(cpu);
}

bool ActRaiser_RegionalTownWaitEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserTownWait_Entry(cpu);
}

bool ActRaiser_RegionalFishingEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserFishing_Entry(cpu);
}

bool ActRaiser_RegionalRecoveryCycleEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserRecovery_CycleEntry(cpu);
}
bool ActRaiser_RegionalRecoveryDrainEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserRecovery_DrainEntry(cpu);
}
bool ActRaiser_RegionalRecoveryMotionEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserRecovery_MotionEntry(cpu);
}
static ArRegionalRecoverySnapshot CaptureRecovery(CpuState *cpu) {
  ArRegionalRecoverySnapshot snapshot;
  const bool pending = memcmp(&s_campaign.active.requested.recovery,
      &s_campaign.active.effective.recovery, sizeof(s_campaign.active.requested.recovery)) != 0;
  unsigned changed = 0;
  if (!(pending ? ArRegionalSession_BeginRecovery(&s_campaign.active, &snapshot, &changed) :
        ArRegionalRecovery_Resolve(&s_campaign.active.effective.recovery, &snapshot)))
    ActRaiserHleFatal("Cannot capture regional recovery policy");
  if (changed) ActRaiserRecovery_Reconcile(cpu, changed);
  if (s_trace && pending) fprintf(stderr, "[regional] recovery cycle-sp=%u angel-calls=%u retired=%u\n",
      snapshot.cycle_sp, snapshot.angel_calls, changed);
  return snapshot;
}
static RecompReturn RecoveryTransfer(RecompReturn result, uint32_t target, uint32_t source) {
  if (result != RECOMP_RETURN_NORMAL)
    return result >= RECOMP_RETURN_TAILCALL ? result : (RecompReturn)(result - 1);
  if (!cpu_hle_tailcall_request(target, source))
    ActRaiserHleFatal("Regional recovery requires a generated wrapper");
  return RECOMP_RETURN_TAILCALL;
}
RecompReturn ActRaiser_RegionalRecoveryCycle(CpuState *cpu) {
  if (!ActRaiser_RegionalRecoveryCycleEntry(cpu)) ActRaiserHleFatal("Invalid regional recovery cycle");
  const ArRegionalRecoverySnapshot snapshot = CaptureRecovery(cpu);
  return RecoveryTransfer(ActRaiserRecovery_Cycle(cpu, &snapshot), 0x038298, 0x038271);
}
RecompReturn ActRaiser_RegionalRecoveryDrain(CpuState *cpu) {
  if (!ActRaiser_RegionalRecoveryDrainEntry(cpu)) ActRaiserHleFatal("Invalid regional recovery drain");
  const ArRegionalRecoverySnapshot snapshot = CaptureRecovery(cpu);
  return RecoveryTransfer(ActRaiserRecovery_Drain(cpu, &snapshot), 0x01b281, 0x01b257);
}
static RecompReturn RecoveryMotion(CpuState *cpu, bool stopped) {
  if (!ActRaiser_RegionalRecoveryMotionEntry(cpu)) ActRaiserHleFatal("Invalid regional recovery motion");
  const ArRegionalRecoverySnapshot snapshot = CaptureRecovery(cpu);
  ActRaiserRecovery_Motion(cpu, &snapshot, stopped);
  return RecoveryTransfer(RECOMP_RETURN_NORMAL, stopped ? 0x019c3c : 0x019c32,
                          stopped ? 0x019c34 : 0x019c30);
}
RecompReturn ActRaiser_RegionalRecoveryMoving(CpuState *cpu) { return RecoveryMotion(cpu, false); }
RecompReturn ActRaiser_RegionalRecoveryStopped(CpuState *cpu) { return RecoveryMotion(cpu, true); }

bool ActRaiser_RegionalDevelopmentEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserDevelopment_MasterEntry(cpu);
}
bool ActRaiser_RegionalEffectEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserDevelopment_EffectEntry(cpu);
}
RecompReturn ActRaiser_RegionalDevelopment(CpuState *cpu) {
  if(!ActRaiser_RegionalDevelopmentEntry(cpu))ActRaiserHleFatal("Invalid regional development entry");
  ArRegionalDevelopmentSnapshot snapshot;
  const bool new_cycle=cpu_read16(cpu,0,0x0347)!=7 &&
      !cpu_read16(cpu,0x7f,0x91fe) && !cpu_read16(cpu,0x7f,0x9200);
  const bool pending=memcmp(&s_campaign.active.requested.development,&s_campaign.active.effective.development,
                            sizeof(s_campaign.active.requested.development))!=0;
  if(!(new_cycle ? ArRegionalSession_BeginDevelopment(&s_campaign.active,&snapshot) :
       ArRegionalDevelopment_Resolve(&s_campaign.active.effective.development,&snapshot)))
    ActRaiserHleFatal("Cannot capture regional development cycle");
  if(s_trace && new_cycle && pending)fprintf(stderr,"[regional] development divider=%u cycle=%u effects=%u\n",
      snapshot.service_divider,snapshot.long_cycle,snapshot.effect_divider);
  return ActRaiserDevelopment_Master(cpu,&snapshot);
}
static RecompReturn RegionalEffect(CpuState *cpu,bool world_actors) {
  ArRegionalDevelopmentSnapshot snapshot;
  if(!ActRaiser_RegionalEffectEntry(cpu) ||
      !ArRegionalDevelopment_Resolve(&s_campaign.active.effective.development,&snapshot))
    ActRaiserHleFatal("Cannot capture regional effect service");
  return ActRaiserDevelopment_Effect(cpu,&snapshot,world_actors);
}
RecompReturn ActRaiser_RegionalEffect(CpuState *cpu) {return RegionalEffect(cpu,true);}
RecompReturn ActRaiser_RegionalEffectVisuals(CpuState *cpu) {return RegionalEffect(cpu,false);}

RecompReturn ActRaiser_RegionalFishing(CpuState *cpu) {
  uint16_t target, continuation;
  bool reconcile;
  if (!ActRaiser_RegionalFishingEntry(cpu) ||
      !ArRegionalSession_BeginFishing(&s_campaign.active, &target, &reconcile))
    ActRaiserHleFatal("Cannot capture regional fishing target");
  const RecompReturn result = ActRaiserFishing_Prefix(cpu, (uint8_t)target, reconcile, &continuation);
  if (result != RECOMP_RETURN_NORMAL)
    return result >= RECOMP_RETURN_TAILCALL ? result : (RecompReturn)(result - 1);
  if (s_trace && (reconcile || continuation == 0xe895))
    fprintf(stderr, "[regional] fishing progress=%u target=%u complete=%u\n",
            cpu_read8(cpu,0x7f,0x916e), target, continuation == 0xe895);
  if (!cpu_hle_tailcall_request(0x030000u | continuation, 0x03e865))
    ActRaiserHleFatal("Regional fishing requires a generated wrapper");
  return RECOMP_RETURN_TAILCALL;
}

RecompReturn ActRaiser_RegionalTownWait(CpuState *cpu) {
  if (!ActRaiser_RegionalTownWaitEntry(cpu)) ActRaiserHleFatal("Invalid regional town wait");
  const unsigned town = cpu_read16(cpu, 0x7f, 0x7bfb);
  uint16_t reload = 0; /* Unused until expiry; no per-tick policy resolution. */
  if (cpu_read16(cpu, 0x7f, (uint16_t)(0x7ce1 + town)) == 1) {
    if (!ArRegionalSession_BeginTownWait(&s_campaign.active, &reload))
      ActRaiserHleFatal("Cannot capture regional town wait policy");
    if (s_trace) fprintf(stderr, "[regional] town=%u next-wait=%u\n", town / 2, reload);
  }
  return ActRaiserTownWait_Step(cpu, reload);
}

RecompReturn ActRaiser_RegionalRetry(CpuState *cpu) {
  if (!ActRaiser_RegionalRetryEntry(cpu)) ActRaiserHleFatal("Invalid regional checkpoint retry");
  const bool retry = cpu_read16(cpu, 0, 0x032c) != 0;
  bool clear_score = false;
  if (retry && !ArRegionalSession_BeginRetryScore(&s_campaign.active, &clear_score))
    ActRaiserHleFatal("Cannot capture regional checkpoint-retry policy");
  const uint16_t before = cpu_read16(cpu, 0, 0x001f);
  const uint16_t continuation = ActRaiserRetry_Prefix(cpu, clear_score);
  if (s_trace && retry) fprintf(stderr, "[regional] checkpoint retry score=%04x->%04x\n",
                                before, cpu_read16(cpu, 0, 0x001f));
  if (!cpu_hle_tailcall_request(continuation, 0x00981c))
    ActRaiserHleFatal("Regional checkpoint retry requires a generated wrapper");
  return RECOMP_RETURN_TAILCALL;
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

static bool QuakeEffectEntry(CpuState *cpu, bool posted) {
  if (s_quake_delegate) { s_quake_delegate = false; return false; }
  return !s_quake_active && s_campaign.active_valid && cpu && cpu->PB == 1 && cpu->DB == 1 &&
      !cpu->D && !cpu->emulation && !cpu->x_flag && cpu->m_flag == !posted &&
      (posted ? cpu_read16(cpu, 0x7f, 0x90eb) == 4 : (uint8_t)cpu->A == 4);
}
bool ActRaiser_RegionalQuakePlayerEntry(CpuState *cpu) { return QuakeEffectEntry(cpu, false); }
bool ActRaiser_RegionalQuakePostedEntry(CpuState *cpu) { return QuakeEffectEntry(cpu, true); }
static RecompReturn QuakeEffect(CpuState *cpu, bool posted) {
  if (!QuakeEffectEntry(cpu, posted) || !ArRegionalSession_BeginQuake(&s_campaign.active, &s_quake))
    ActRaiserHleFatal("Cannot capture regional earthquake transaction");
  s_quake_active = s_quake_delegate = true;
  if (s_trace) memset(s_quake_selections, 0, sizeof(s_quake_selections));
  if (s_trace) fprintf(stderr, "[regional] quake origin=%s selectors=%u%u%u%u%u begin\n",
      posted ? "posted" : "player", s_quake.random[0], s_quake.random[1], s_quake.random[2],
      s_quake.random[3], s_quake.random[4]);
  /* Delegate the complete native effect with its original return frame.
   * The one-shot recursion gate leaves descendant selector hooks active. */
  RecompReturn result = posted ? bank_01_9840_M0X0(cpu) : bank_01_97E5_M1X0(cpu);
  s_quake_active = s_quake_delegate = false;
  if (s_trace) fprintf(stderr, "[regional] quake return=%u random-selectors=%u/%u/%u/%u/%u\n",
      (unsigned)result, s_quake_selections[0], s_quake_selections[1], s_quake_selections[2],
      s_quake_selections[3], s_quake_selections[4]);
  return result;
}
RecompReturn ActRaiser_RegionalQuakePlayer(CpuState *cpu) { return QuakeEffect(cpu, false); }
RecompReturn ActRaiser_RegionalQuakePosted(CpuState *cpu) { return QuakeEffect(cpu, true); }
static bool QuakeSelectorEntry(CpuState *cpu, ArRegionalQuakeRule rule) {
  return s_quake_active && s_quake.random[rule] && ActRaiserQuake_SelectorEntry(cpu);
}
static RecompReturn QuakeSelect(CpuState *cpu, ArRegionalQuakeRule rule) {
  if (!QuakeSelectorEntry(cpu, rule)) ActRaiserHleFatal("Earthquake selector outside its captured effect");
  if (s_trace) ++s_quake_selections[rule];
  uint32_t target;
  RecompReturn result = ActRaiserQuake_Select(cpu, rule, &target);
  if (result != RECOMP_RETURN_NORMAL)
    return result >= RECOMP_RETURN_TAILCALL ? result : (RecompReturn)(result - 1);
  if (!cpu_hle_tailcall_request(target, ActRaiserQuake_SelectorPC(rule)))
    ActRaiserHleFatal("Earthquake selector has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}
#define QUAKE_SELECTOR(name, rule) \
  bool ActRaiser_RegionalQuake##name##Entry(CpuState *cpu) { return QuakeSelectorEntry(cpu, rule); } \
  RecompReturn ActRaiser_RegionalQuake##name(CpuState *cpu) { return QuakeSelect(cpu, rule); }
QUAKE_SELECTOR(Houses, kArRegionalQuake_Houses)
QUAKE_SELECTOR(Fields, kArRegionalQuake_Fields)
QUAKE_SELECTOR(Class3, kArRegionalQuake_Class3)
QUAKE_SELECTOR(Class4, kArRegionalQuake_Class4)
QUAKE_SELECTOR(Class5, kArRegionalQuake_Class5)
#undef QUAKE_SELECTOR

static bool ReportShape(const CpuState *cpu) {
  return cpu && cpu->PB == 1 && cpu->DB == 1 && !cpu->D && !cpu->emulation &&
      cpu->m_flag && !cpu->x_flag;
}
bool ActRaiser_RegionalMasterReportEntry(CpuState *cpu) {
  if (s_report_delegate) { s_report_delegate = false; return false; }
  return !s_report_active && s_campaign.active_valid && ReportShape(cpu);
}
RecompReturn ActRaiser_RegionalMasterReport(CpuState *cpu) {
  if (!ActRaiser_RegionalMasterReportEntry(cpu) ||
      !ArRegionalSession_BeginScorePage(&s_campaign.active, &s_report_score_page))
    ActRaiserHleFatal("Cannot capture regional Master report");
  s_report_active = s_report_delegate = true;
  if (s_trace) fprintf(stderr, "[regional] Master report score-page=%u begin\n", s_report_score_page);
  /* The native body still composes the report and owns inventory objects,
   * button release/press waits and cleanup. Only its optional branch changes. */
  RecompReturn result = bank_01_899B_M1X0(cpu);
  s_report_active = s_report_delegate = false;
  if (s_trace) fprintf(stderr, "[regional] Master report return=%u\n", (unsigned)result);
  return result;
}
bool ActRaiser_RegionalSkipScoreEntry(CpuState *cpu) {
  return s_report_active && !s_report_score_page && ReportShape(cpu);
}
RecompReturn ActRaiser_RegionalSkipScore(CpuState *cpu) {
  if (!ActRaiser_RegionalSkipScoreEntry(cpu) || !cpu_hle_tailcall_request(0x018a2f, 0x0189ee))
    ActRaiserHleFatal("Cannot resume native Master report cleanup");
  return RECOMP_RETURN_TAILCALL;
}

void ActRaiserRegional_ObserveInputRelease(CpuState *cpu) {
  /* Cheap unchanged path; no MMIO, policy walk, allocations or I/O per tick.
   * Native $A1 is debounced via $F6 and can be zero while Y is still held.
   * Observe the completed auto-joypad sample instead, without remapping it. */
  if (!cpu || !s_campaign.active_valid ||
      s_campaign.active.requested.magic_gesture == s_campaign.active.effective.magic_gesture)
    return;
  if (!ActRaiserMagicGesture_ControlsReleased(cpu_read16(cpu,0,0x4218))) return;
  bool up_attack;
  if (!ArRegionalSession_BeginMagicGesture(&s_campaign.active, true, &up_attack))
    ActRaiserHleFatal("Cannot activate regional magic gesture");
  if (s_trace) fprintf(stderr,"[regional] magic gesture=%s after release\n",
      up_attack ? "up+attack" : "A/X");
}

bool ActRaiser_RegionalMagicGestureEntry(CpuState *cpu) {
  bool up_attack;
  return s_campaign.active_valid && ActRaiserMagicGesture_Entry(cpu) &&
      ArRegionalMagicGesture_Resolve(s_campaign.active.effective.magic_gesture, &up_attack) && up_attack;
}
RecompReturn ActRaiser_RegionalMagicDedicated(CpuState *cpu) {
  if (!ActRaiser_RegionalMagicGestureEntry(cpu) ||
      !cpu_hle_tailcall_request(0x00984e,0x009843))
    ActRaiserHleFatal("Cannot bypass dedicated magic input for JP gesture");
  return RECOMP_RETURN_TAILCALL;
}
RecompReturn ActRaiser_RegionalMagicAttack(CpuState *cpu) {
  const uint32_t target=ActRaiserMagicGesture_Attack(cpu);
  if (!cpu_hle_tailcall_request(target,0x009a6e))
    ActRaiserHleFatal("Regional ground-attack prefix has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}

bool ActRaiser_RegionalSpeedEntry(CpuState *cpu) {
  if (s_speed_delegate) { s_speed_delegate = false; return false; }
  return s_campaign.active_valid && !s_speed_active && ActRaiserSpeedSelector_Entry(cpu, false);
}
RecompReturn ActRaiser_RegionalSpeed(CpuState *cpu) {
  if (!ActRaiser_RegionalSpeedEntry(cpu) ||
      !ArRegionalSession_BeginSpeedRange(&s_campaign.active, &s_speed_maximum))
    ActRaiserHleFatal("Cannot capture regional message-speed range");
  s_speed_active = s_speed_delegate = true;
  ActRaiserLocalizationRuntime_SetMessageSpeedMaximum(s_speed_maximum);
  if (s_trace) fprintf(stderr, "[regional] speed range=0-%u stored=%u begin\n",
      s_speed_maximum, cpu_read8(cpu, 0, 0x0200));
  RecompReturn result = bank_01_8AF5_M1X0(cpu);
  s_speed_active = s_speed_delegate = s_speed_scale_delegate = false;
  if (s_trace) fprintf(stderr, "[regional] speed return=%u stored=%u\n",
      (unsigned)result, cpu_read8(cpu, 0, 0x0200));
  return result;
}
bool ActRaiser_RegionalSpeedPositionEntry(CpuState *cpu) {
  return s_speed_active && s_speed_maximum == 7 && ActRaiserSpeedSelector_Entry(cpu, true);
}
RecompReturn ActRaiser_RegionalSpeedPosition(CpuState *cpu) {
  ActRaiserSpeedSelector_Position(cpu, s_speed_maximum);
  if (!cpu_hle_tailcall_request(0x018b22, 0x018b18))
    ActRaiserHleFatal("Message-speed position has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_RegionalSpeedRightEntry(CpuState *cpu) {
  return s_speed_active && s_speed_maximum == 7 && ActRaiserSpeedSelector_Entry(cpu, false);
}
RecompReturn ActRaiser_RegionalSpeedRight(CpuState *cpu) {
  const uint32_t target = ActRaiserSpeedSelector_Right(cpu, s_speed_maximum);
  if (!cpu_hle_tailcall_request(target, 0x018b59))
    ActRaiserHleFatal("Message-speed movement has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_RegionalSpeedScaleEntry(CpuState *cpu) {
  if (s_speed_scale_delegate) { s_speed_scale_delegate = false; return false; }
  return s_speed_active && s_speed_maximum == 7 && ActRaiserSpeedSelector_Entry(cpu, false) &&
      cpu->X == 0xfa98 && cpu_read16(cpu, 0, cpu->S + 1) == 0x8b04;
}
RecompReturn ActRaiser_RegionalSpeedScale(CpuState *cpu) {
  s_speed_scale_delegate = true;
  const RecompReturn result = bank_01_8C98_M1X0(cpu);
  s_speed_scale_delegate = false;
  if (result == RECOMP_RETURN_NORMAL) ActRaiserSpeedSelector_DrawNativeScale(cpu, s_speed_maximum);
  return result;
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
    ArRegionalCosts_Init(&defaults, kArRegionalSource_US);
    ok = ArRegionalCampaign_NewGame(&s_campaign, &defaults, &error);
  } else {
    ActRaiserHleFatal("Unknown accepted title selection: %u", selection);
  }
  if (!ok) ActRaiserHleFatal("Cannot enter regional campaign; saves preserved: %s",
                            error.message[0] ? error.message : "no durable save image");
  s_prices_valid = false; /* a different campaign can have the same revision */
  fprintf(stderr, "[regional] %s campaign rules session ready\n",
          selection == 1 ? "continued" : "new");
  return result;
}
