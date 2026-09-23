#include "actraiser/actraiser_regional_runtime.h"
#include "actraiser/actraiser_miracle.h"
#include "actraiser/actraiser_regional_settings.h"
#include "actraiser/actraiser_development.h"
#include "actraiser/actraiser_quake.h"
#include "actraiser/actraiser_report_command.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[65536];
static uint8_t town_ram[65536];
CpuReturnScope *g_cpu_return_scope, *g_cpu_owned_unwind_scope;
int g_recomp_stack_top;
int cpu_finish_owned_unwind(CpuReturnScope *scope, CpuState *cpu) {
  (void)scope; (void)cpu; return 0;
}
RecompReturn bank_03_F4DF_M1X0(CpuState *cpu) {
  const uint8_t value = town_ram[0x9102] & 0x40;
  cpu_write_a8(cpu,value); cpu->_flag_Z=!value; cpu->_flag_N=0;
  cpu->P=(cpu->P & ~0x82u) | (cpu->_flag_Z << 1); cpu->S+=2;
  return RECOMP_RETURN_NORMAL;
}
RecompReturn bank_03_F4EA_M1X0(CpuState *cpu) {
  town_ram[0x9102] |= 0x40; cpu->S+=2; return RECOMP_RETURN_NORMAL;
}
RecompReturn bank_03_CA93_M1X0(CpuState *cpu) { cpu->S+=2; return RECOMP_RETURN_NORMAL; }
static ArRegionalCostSnapshot expected;
static RecompReturn title_return, miracle_return;
static unsigned title_calls, miracle_calls;
static bool edits_allowed = true, edit_during_miracle;
static ArRegionalDevelopmentSnapshot last_development;
static bool last_world_actors;
bool ActRaiserDevelopment_MasterEntry(const CpuState *cpu) {return cpu && cpu->PB==3;}
bool ActRaiserDevelopment_EffectEntry(const CpuState *cpu) {return cpu && cpu->PB==1;}
RecompReturn ActRaiserDevelopment_Master(CpuState *cpu,const ArRegionalDevelopmentSnapshot *snapshot) {
  (void)cpu;last_development=*snapshot;return RECOMP_RETURN_NORMAL;
}
RecompReturn ActRaiserDevelopment_Effect(CpuState *cpu,const ArRegionalDevelopmentSnapshot *snapshot,bool actors) {
  (void)cpu;last_development=*snapshot;last_world_actors=actors;return RECOMP_RETURN_NORMAL;
}
bool InputReplay_PolicyChangesAllowed(void) { return edits_allowed; }
static ArRegionalRecoverySnapshot last_recovery;
static unsigned retired_recovery;
bool ActRaiserRecovery_CycleEntry(const CpuState *cpu) { return cpu && cpu->PB == 3; }
bool ActRaiserRecovery_DrainEntry(const CpuState *cpu) { return cpu && cpu->PB == 1; }
bool ActRaiserRecovery_MotionEntry(const CpuState *cpu) { return cpu && cpu->PB == 1; }
RecompReturn ActRaiserRecovery_Cycle(CpuState *cpu, const ArRegionalRecoverySnapshot *snapshot) {
  (void)cpu; last_recovery = *snapshot; return RECOMP_RETURN_NORMAL;
}
RecompReturn ActRaiserRecovery_Drain(CpuState *cpu, const ArRegionalRecoverySnapshot *snapshot) {
  (void)cpu; last_recovery = *snapshot; return RECOMP_RETURN_NORMAL;
}
void ActRaiserRecovery_Motion(CpuState *cpu, const ArRegionalRecoverySnapshot *snapshot, bool stopped) {
  (void)cpu; (void)stopped; last_recovery = *snapshot;
}
void ActRaiserRecovery_Reconcile(CpuState *cpu, unsigned changed) {
  (void)cpu; retired_recovery = changed;
}
static uint32_t tail_pc, tail_source;
static unsigned captured_speed_maximum;
static bool edit_during_speed;
static RecompReturn speed_return;
void ActRaiserLocalizationRuntime_SetMessageSpeedMaximum(unsigned maximum) {
  captured_speed_maximum = maximum;
}
RecompReturn bank_01_8C98_M1X0(CpuState *cpu) {
  assert(!ActRaiser_RegionalSpeedScaleEntry(cpu));
  cpu->S += 2;
  return speed_return;
}
RecompReturn bank_01_8AF5_M1X0(CpuState *cpu) {
  assert(!ActRaiser_RegionalSpeedEntry(cpu));
  const unsigned maximum = captured_speed_maximum;
  assert(ActRaiser_RegionalSpeedRightEntry(cpu) == (maximum == 7));
  if (edit_during_speed) {
    ActRaiserRegionalRulesView view;
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view, kActRaiserRegionalSetting_SpeedRange,
        maximum == 7 ? kArRegionalSource_US : kArRegionalSource_Japan) == kActRaiserRegionalEdit_Applied);
    assert(captured_speed_maximum == maximum);
    assert(ActRaiser_RegionalSpeedRightEntry(cpu) == (maximum == 7));
  }
  if (maximum == 7) {
    cpu->m_flag = 0; cpu->P &= (uint8_t)~0x20;
    cpu_write16(cpu, 0, 0x0a, 9);
    assert(ActRaiser_RegionalSpeedPositionEntry(cpu));
    assert(ActRaiser_RegionalSpeedPosition(cpu) == RECOMP_RETURN_TAILCALL);
    assert(tail_pc == 0x018b22 && tail_source == 0x018b18);
    assert(cpu->X == 0x0b19 && cpu_read16(cpu, 0, 0x0a) == 7);
    cpu->m_flag = 1; cpu->P |= 0x20;
    assert(ActRaiser_RegionalSpeedRight(cpu) == RECOMP_RETURN_TAILCALL);
    assert(tail_pc == 0x018b2f && tail_source == 0x018b59);
    cpu_write8(cpu, 0, 0x0a, 6);
    assert(ActRaiser_RegionalSpeedRight(cpu) == RECOMP_RETURN_TAILCALL);
    assert(tail_pc == 0x018b11 && cpu_read8(cpu, 0, 0x0a) == 7);
    cpu->X = 0xfa98; cpu->S = 0x1fee;
    cpu_write16(cpu, 0, cpu->S + 1, 0x8b04);
    assert(ActRaiser_RegionalSpeedScaleEntry(cpu));
    assert(ActRaiser_RegionalSpeedScale(cpu) == speed_return);
  }
  return speed_return;
}
static bool quake_random, edit_during_quake;
static unsigned quake_calls;
static RecompReturn quake_return;
RecompReturn bank_03_AF65_M1X0(CpuState *cpu) {
  cpu_write_a8(cpu, 0x7f); cpu->S += 3; return RECOMP_RETURN_NORMAL;
}
static RecompReturn QuakeNative(CpuState *cpu, bool posted) {
  assert(!(posted ? ActRaiser_RegionalQuakePostedEntry(cpu) : ActRaiser_RegionalQuakePlayerEntry(cpu)));
  ++quake_calls;
  CpuState selector = {.PB = 3, .DB = 0x7f, .m_flag = 1, .S = 0x1ff0};
  assert(ActRaiser_RegionalQuakeHousesEntry(&selector) == quake_random);
  if (edit_during_quake) {
    ActRaiserRegionalRulesView view;
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view, kActRaiserRegionalSetting_Quake,
        kArRegionalSource_US) == kActRaiserRegionalEdit_Applied);
    assert(ActRaiser_RegionalQuakeHousesEntry(&selector) == quake_random);
  }
  if (quake_random) {
    assert(ActRaiser_RegionalQuakeHouses(&selector) == RECOMP_RETURN_TAILCALL);
    assert(tail_pc == 0x03a075 && tail_source == 0x03a066);
    assert(ActRaiser_RegionalQuakeFields(&selector) == RECOMP_RETURN_TAILCALL);
    assert(tail_pc == 0x03a151 && tail_source == 0x03a144);
    assert(ActRaiser_RegionalQuakeClass3(&selector) == RECOMP_RETURN_TAILCALL);
    assert(tail_pc == 0x03a1ee && tail_source == 0x03a1e8);
    assert(ActRaiser_RegionalQuakeClass4(&selector) == RECOMP_RETURN_TAILCALL);
    assert(tail_pc == 0x03a28a && tail_source == 0x03a284);
    assert(ActRaiser_RegionalQuakeClass5(&selector) == RECOMP_RETURN_TAILCALL);
    assert(tail_pc == 0x03a34c && tail_source == 0x03a2e3);
  }
  return quake_return;
}
RecompReturn bank_01_97E5_M1X0(CpuState *cpu) { return QuakeNative(cpu, false); }
RecompReturn bank_01_9840_M0X0(CpuState *cpu) { return QuakeNative(cpu, true); }
static bool report_scores = true, edit_during_report;
static bool expected_menu_return, edit_during_command;
static RecompReturn command_return;
bool ActRaiserReportCommand_Entry(const CpuState *cpu, unsigned action) {
  return cpu && cpu->PB == 1 && cpu->DB == 1 && !cpu->D && !cpu->emulation &&
      cpu->m_flag && !cpu->x_flag && action >= 12 && action <= 15;
}
RecompReturn ActRaiserReportCommand_Run(CpuState *cpu, unsigned action, bool keep_open) {
  assert(action >= 12 && action <= 15 && keep_open == expected_menu_return);
  assert(!ActRaiserRegional_ReportCommandEntry(cpu));
  if (edit_during_command) {
    ActRaiserRegionalRulesView view;
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view, kActRaiserRegionalSetting_MenuReturn,
        keep_open ? kArRegionalSource_US : kArRegionalSource_Japan) == kActRaiserRegionalEdit_Applied);
    assert(ActRaiserRegional_CopyRulesView(&view));
    bool effective;
    assert(ArRegionalMenuReturn_Resolve(view.effective.menu_return, &effective) && effective == keep_open);
  }
  return command_return;
}
static unsigned report_calls;
static RecompReturn report_return;
RecompReturn bank_01_899B_M1X0(CpuState *cpu) {
  assert(!ActRaiser_RegionalMasterReportEntry(cpu));
  ++report_calls;
  assert(ActRaiser_RegionalSkipScoreEntry(cpu) == !report_scores);
  if (edit_during_report) {
    ActRaiserRegionalRulesView view;
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view, kActRaiserRegionalSetting_ScorePage,
        report_scores ? kArRegionalSource_Japan : kArRegionalSource_US) == kActRaiserRegionalEdit_Applied);
    assert(ActRaiser_RegionalSkipScoreEntry(cpu) == !report_scores);
  }
  if (!report_scores) {
    const CpuState before = *cpu;
    assert(ActRaiser_RegionalSkipScore(cpu) == RECOMP_RETURN_TAILCALL);
    assert(tail_pc == 0x018a2f && tail_source == 0x0189ee && !memcmp(&before, cpu, sizeof(before)));
  }
  return report_return;
}
int cpu_hle_tailcall_request(uint32_t pc, uint32_t source) {
  tail_pc=pc; tail_source=source;
  return 1; /* runtime infra tests own inherited-stack/paired-return semantics */
}

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu; assert(bank == 0 || bank == 0x7f); return bank ? town_ram[address] : ram[address];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  return cpu_read8(cpu, bank, address) | cpu_read8(cpu, bank, address + 1) << 8;
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu; assert(bank == 0 || bank == 0x7f);
  if (bank) town_ram[address] = value; else ram[address] = value;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(cpu,bank,address,value); cpu_write8(cpu,bank,address+1,value>>8);
}
RecompReturn bank_02_A622_M1X0(CpuState *cpu) {
  assert(!ActRaiser_RegionalTitleEntry(cpu)); /* delegates exactly once */
  ++title_calls;
  return title_return;
}
bool ActRaiserMiracle_Entry(const CpuState *cpu, unsigned action) {
  return cpu && action >= 5 && action <= 9 && cpu->PB == 1 && cpu->DB == 1 &&
      cpu->m_flag && !cpu->x_flag && !cpu->D && !cpu->emulation;
}
bool ActRaiserMiracle_Rule(unsigned action, ArRegionalCostRule *rule) {
  static const ArRegionalCostRule rules[] = {kArRegionalCost_Lightning,
      kArRegionalCost_Rain, kArRegionalCost_Sunlight, kArRegionalCost_Wind,
      kArRegionalCost_Earthquake};
  if (action < 5 || action > 9 || !rule) return false;
  *rule = rules[action - 5]; return true;
}
RecompReturn ActRaiserMiracle_Run(CpuState *cpu, unsigned action,
                                 const ArRegionalCostSnapshot *quote) {
  assert(action >= 5 && action <= 9);
  assert(!ActRaiserRegional_MiracleEntry(cpu)); /* no nested transaction */
  assert(!memcmp(quote, &expected, sizeof(expected)));
  ArRegionalCostSnapshot displayed;
  assert(ActRaiserRegional_CopyPrices(&displayed));
  assert(!memcmp(&displayed, quote, sizeof(displayed)));
  if (edit_during_miracle) {
    ActRaiserRegionalRulesView view;
    assert(ActRaiserRegional_CopyRulesView(&view) && view.miracle_in_progress);
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Miracles,
        kArRegionalSource_Japan)==kActRaiserRegionalEdit_Applied);
    assert(ActRaiserRegional_CopyPrices(&displayed));
    assert(!memcmp(&displayed, quote, sizeof(displayed)));
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(view.requested.costs.source[kArRegionalCost_Rain]==kArRegionalSource_Japan);
    assert(view.effective.costs.source[kArRegionalCost_Rain]==kArRegionalSource_Europe);
  }
  /* Render consumers own copies, not writable aliases to the accepted quote. */
  memset(&displayed, 0, sizeof(displayed));
  assert(ActRaiserRegional_CopyPrices(&displayed));
  assert(!memcmp(&displayed, quote, sizeof(displayed)));
  ++miracle_calls;
  return miracle_return;
}
static bool Identity(void *unused, uint8_t id[16]) {
  (void)unused; memset(id, 0, 16); id[0] = 42; return true;
}
static void CheckPrices(ArRegionalSource source) {
  ArRegionalCostPolicy policy;
  assert(ArRegionalCosts_Init(&policy, source));
  assert(ArRegionalCosts_Resolve(&policy, &expected));
  ArRegionalCostSnapshot actual;
  assert(ActRaiserRegional_CopyPrices(&actual));
  assert(!memcmp(&actual, &expected, sizeof(actual)));
}
static void Install(const char *path, uint8_t *image, ArRegionalSource source) {
  ArRegionalCostPolicy policy;
  ArRegionalSession session;
  const uint8_t id[16] = {(uint8_t)(source + 1)};
  SaveError error = {{0}};
  assert(ArRegionalCosts_Init(&policy, source));
  assert(ArRegionalSession_NewGame(&session, 0, id, &policy));
  assert(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm,
                                path, image, image, &error));
  assert(SaveSystem_LoadActive(&error));
}
int main(void) {
  const char *path = "actraiser-regional-runtime-test.srm";
  const char *companion = "actraiser-regional-runtime-test.srm.archeckpoint";
  remove(path); remove(companion);
  uint8_t image[kActRaiserSramSize] = {0};
  SaveError error = {{0}};
  Save_RecomputeChecksum(image);
  assert(Save_WriteFile(kSaveFileFormat_NativeSrm, path, image, &error));
  assert(SaveSystem_Attach(image, sizeof(image), kSaveBackend_NativeSrm,
                          path, "unused-regional-runtime.ini", &error));
  assert(SaveSystem_LoadActive(&error));
  assert(ActRaiserRegional_Initialize(Identity, NULL));
  ActRaiserRegionalRulesView view;
  assert(!ActRaiserRegional_CopyRulesView(&view));
  uint16_t timer;
  assert(ActRaiserRegional_BeginRoomTime(3, 0x300, &timer) && timer == 0x300);
  CheckPrices(kArRegionalSource_US);
  CpuState cpu = {.PB = 2, .DB = 1, .m_flag = 1};
  ram[0x336] = 1;
  /* Different loaded campaigns deliberately share revision 1. */
  for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
    Install(path, image, (ArRegionalSource)source);
    CheckPrices(kArRegionalSource_US); /* unloaded campaign never leaks */
    cpu.PB = 2;
    assert(ActRaiser_RegionalTitleEntry(&cpu));
    assert(ActRaiser_RegionalTitle(&cpu) == RECOMP_RETURN_NORMAL);
    CheckPrices((ArRegionalSource)source);
    CpuState casting={.S=0x1f0,.X=0xc00,.host_return_valid=1};
    ram[0x2ac]=4; ram[0x21]=5;
    assert(ActRaiser_RegionalScrollEntry(&casting));
    assert(ActRaiser_RegionalScrollCast(&casting)==RECOMP_RETURN_TAILCALL);
    assert(ram[0x21]==5-expected.price[kArRegionalCost_Light]);
    assert(tail_pc==0x9e0e && tail_source==0x9de1);
    ram[0x2ac]=5;
    assert(!ActRaiser_RegionalScrollEntry(&casting)); /* native fallback outside bounded domain */
    cpu.PB = 1;
    for (unsigned action = 5; action <= 9; ++action) {
      cpu.A = action;
      for (unsigned interrupted = 0; interrupted < 2; ++interrupted) {
        miracle_return = interrupted ? RECOMP_RETURN_PARKED_WAIT : RECOMP_RETURN_NORMAL;
        assert(ActRaiserRegional_MiracleEntry(&cpu));
        assert(ActRaiserRegional_RunMiracle(&cpu) == miracle_return);
        assert(ActRaiserRegional_MiracleEntry(&cpu));
        CheckPrices((ArRegionalSource)source);
      }
    }
  }
  assert(ActRaiserRegional_CopyRulesView(&view));
  edits_allowed=false;
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Miracles,
      kArRegionalSource_Japan)==kActRaiserRegionalEdit_Locked);
  edits_allowed=true;
  ActRaiserRegionalRulesView stale=view;
  stale.campaign[0]^=1;
  assert(ActRaiserRegional_RequestRules(&stale,kActRaiserRegionalSetting_Miracles,
      kArRegionalSource_Japan)==kActRaiserRegionalEdit_Stale);
  edit_during_miracle=true; miracle_return=RECOMP_RETURN_NORMAL; cpu.A=5;
  assert(ActRaiserRegional_RunMiracle(&cpu)==RECOMP_RETURN_NORMAL);
  edit_during_miracle=false;
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Miracles,
      kArRegionalSource_US)==kActRaiserRegionalEdit_Stale);
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(!view.miracle_in_progress);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Miracles,
      kArRegionalSource_Japan)==kActRaiserRegionalEdit_Unchanged);
  assert(ActRaiserRegional_CopyPrices(&expected));
  assert(expected.price[kArRegionalCost_Rain]==16);
  assert(expected.price[kArRegionalCost_Light]==1); /* independent group */
  assert(ActRaiserRegional_RunMiracle(&cpu)==RECOMP_RETURN_NORMAL);
  /* Timer changes do not edit RAM; native room initialization owns the write.
   * Pending/effective choices stay distinct, including after a save/reload. */
  assert(ActRaiserRegional_CopyRulesView(&view));
  const ActRaiserRegionalRulesView before_time = view;
  ram[0xe6] = 0x75; ram[0xe7] = 1;
  uint8_t before_digest[32], pending_digest[32], active_digest[32];
  bool baseline;
  assert(ActRaiserRegional_ReplayDigest(NULL, before_digest, &baseline));
  edits_allowed = false;
  assert(ActRaiserRegional_RequestRules(&view, kActRaiserRegionalSetting_RoomTimes,
      kArRegionalSource_Japan) == kActRaiserRegionalEdit_Locked);
  edits_allowed = true;
  assert(ActRaiserRegional_RequestRules(&view, kActRaiserRegionalSetting_RoomTimes,
      kArRegionalSource_Japan) == kActRaiserRegionalEdit_Applied);
  assert(ram[0xe6] == 0x75 && ram[0xe7] == 1);
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(!memcmp(&before_time.requested.costs, &view.requested.costs, sizeof(view.requested.costs)));
  assert(view.requested.timers.source[0] == kArRegionalSource_Japan);
  assert(view.effective.timers.source[0] == kArRegionalSource_US);
  assert(ActRaiserRegional_ReplayDigest(NULL, pending_digest, &baseline) && !baseline);
  assert(memcmp(before_digest, pending_digest, 32));
  timer = 0x777;
  assert(!ActRaiserRegional_BeginRoomTime(3, 0x555, &timer) && timer == 0x777);
  ActRaiserRegionalRulesView unchanged;
  assert(ActRaiserRegional_CopyRulesView(&unchanged) && unchanged.revision == view.revision);
  assert(ActRaiserRegional_BeginRoomTime(3, 0x300, &timer) && timer == 0x200);
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(view.effective.timers.source[0] == kArRegionalSource_Japan);
  assert(ActRaiserRegional_ReplayDigest(NULL, active_digest, &baseline) && !baseline);
  assert(memcmp(active_digest, pending_digest, 32));
  assert(ActRaiserRegional_RequestRules(&unchanged, kActRaiserRegionalSetting_RoomTimes,
      kArRegionalSource_US) == kActRaiserRegionalEdit_Stale);
  assert(ActRaiserRegional_RequestRules(&view, kActRaiserRegionalSetting_RoomTimes,
      kArRegionalSource_US) == kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_BeginRoomTime(3, 0x300, &timer) && timer == 0x300);
  assert(ActRaiserRegional_ReplayDigest(NULL, active_digest, &baseline));
  assert(!memcmp(active_digest, before_digest, 32));
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view, kActRaiserRegionalSetting_RetryScore,
      kArRegionalSource_Japan) == kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_CopyRulesView(&view));
  CpuState retry_cpu = {.S=0x1fa, .m_flag=1};
  ram[0x32c] = ram[0x32d] = 0;
  cpu_write16(&retry_cpu,0,0x1f,0x1234);
  assert(ActRaiser_RegionalRetryEntry(&retry_cpu));
  assert(ActRaiser_RegionalRetry(&retry_cpu) == RECOMP_RETURN_TAILCALL);
  assert(tail_pc == 0x982f && tail_source == 0x981c);
  assert(cpu_read16(&retry_cpu,0,0x1f) == 0x1234);
  assert(ActRaiserRegional_CopyRulesView(&unchanged));
  assert(unchanged.revision == view.revision && unchanged.effective.retry_score == kArRegionalSource_US);
  ram[0x32c] = 1;
  assert(ActRaiser_RegionalRetry(&retry_cpu) == RECOMP_RETURN_TAILCALL);
  assert(tail_pc == 0x9826 && cpu_read16(&retry_cpu,0,0x1f) == 0);
  assert(cpu_read16(&retry_cpu,0,0x32c) == 0);
  assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.retry_score == kArRegionalSource_Japan);
  assert(ActRaiserRegional_RequestRules(&view, kActRaiserRegionalSetting_RetryScore,
      kArRegionalSource_US) == kActRaiserRegionalEdit_Applied);
  ram[0x32c] = 1; cpu_write16(&retry_cpu,0,0x1f,0x5678);
  assert(ActRaiser_RegionalRetry(&retry_cpu) == RECOMP_RETURN_TAILCALL);
  assert(tail_pc == 0x9826 && cpu_read16(&retry_cpu,0,0x1f) == 0x5678);
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view, kActRaiserRegionalSetting_TownWait,
      kArRegionalSource_Japan) == kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_CopyRulesView(&view));
  CpuState wait_cpu = {.PB=3, .DB=0x7f, .S=0x1fa};
  cpu_write16(&wait_cpu,0x7f,0x7bfb,4);
  cpu_write16(&wait_cpu,0x7f,0x7ce5,37);
  cpu_write16(&wait_cpu,0x7f,0x7ccd,3);
  cpu_write16(&wait_cpu,0x7f,0x7cd9,1);
  for (unsigned n = 0; n < 36; ++n) {
    assert(ActRaiser_RegionalTownWaitEntry(&wait_cpu));
    assert(ActRaiser_RegionalTownWait(&wait_cpu) == RECOMP_RETURN_NORMAL);
    assert(cpu_read16(&wait_cpu,0x7f,0x7ce5) == 36-n);
    assert(ActRaiserRegional_CopyRulesView(&unchanged));
    assert(unchanged.revision == view.revision && unchanged.effective.town_wait == kArRegionalSource_US);
  }
  assert(ActRaiser_RegionalTownWait(&wait_cpu) == RECOMP_RETURN_NORMAL);
  assert(cpu_read16(&wait_cpu,0x7f,0x7ce5) == 150 && cpu_read16(&wait_cpu,0x7f,0x7ccd) == 4);
  assert(cpu_read16(&wait_cpu,0x7f,0x7cd9) == 1); /* no native reload-table edit */
  assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.town_wait == kArRegionalSource_Japan);
  assert(ActRaiserRegional_RequestRules(&view, kActRaiserRegionalSetting_TownWait,
      kArRegionalSource_US) == kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_CopyRulesView(&view));
  for (unsigned n = 0; n < 149; ++n) {
    assert(ActRaiser_RegionalTownWait(&wait_cpu) == RECOMP_RETURN_NORMAL);
    assert(ActRaiserRegional_CopyRulesView(&unchanged) && unchanged.revision == view.revision);
  }
  assert(ActRaiser_RegionalTownWait(&wait_cpu) == RECOMP_RETURN_NORMAL);
  assert(cpu_read16(&wait_cpu,0x7f,0x7ce5) == 1 && cpu_read16(&wait_cpu,0x7f,0x7ccd) == 5);
  assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.town_wait == kArRegionalSource_US);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Fishing,
      kArRegionalSource_Japan) == kActRaiserRegionalEdit_Applied);
  CpuState fish_cpu={.PB=3,.DB=0x7f,.S=0x1fa};
  town_ram[0x9102] = 0x40; town_ram[0x916e] = 200;
  assert(ActRaiser_RegionalFishingEntry(&fish_cpu));
  assert(ActRaiser_RegionalFishing(&fish_cpu) == RECOMP_RETURN_TAILCALL);
  assert(tail_pc == 0x03e895 && tail_source == 0x03e865 && town_ram[0x916e] == 200);
  assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.fishing == kArRegionalSource_Japan);
  /* Higher target keeps progress; the native reward continuation is untouched. */
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Fishing,
      kArRegionalSource_US) == kActRaiserRegionalEdit_Applied);
  fish_cpu.m_flag=0; town_ram[0x916e]=100;
  assert(ActRaiser_RegionalFishing(&fish_cpu) == RECOMP_RETURN_TAILCALL);
  assert(tail_pc == 0x03e88c && town_ram[0x916e] == 101);
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Development,
      kArRegionalSource_Japan)==kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_CopyRulesView(&view));
  CpuState dev_cpu={.PB=3,.DB=0x7f};
  cpu_write16(&dev_cpu,0,0x0347,1);cpu_write16(&dev_cpu,0x7f,0x91fe,719);
  cpu_write16(&dev_cpu,0x7f,0x9200,7);cpu_write16(&dev_cpu,0x7f,0x7ced,3);
  assert(ActRaiser_RegionalDevelopment(&dev_cpu)==RECOMP_RETURN_NORMAL);
  assert(last_development.service_divider==1 && last_development.long_cycle==720);
  assert(ActRaiserRegional_CopyRulesView(&unchanged) && unchanged.revision==view.revision);
  dev_cpu.PB=1;assert(ActRaiser_RegionalEffect(&dev_cpu)==RECOMP_RETURN_NORMAL);
  assert(last_world_actors && last_development.effect_divider==1);
  assert(ActRaiser_RegionalEffectVisuals(&dev_cpu)==RECOMP_RETURN_NORMAL && !last_world_actors);
  dev_cpu.PB=3;cpu_write16(&dev_cpu,0x7f,0x91fe,0);cpu_write16(&dev_cpu,0x7f,0x9200,0);
  cpu_write16(&dev_cpu,0,0x0347,7);
  assert(ActRaiser_RegionalDevelopment(&dev_cpu)==RECOMP_RETURN_NORMAL && last_development.service_divider==1);
  cpu_write16(&dev_cpu,0,0x0347,1);
  assert(ActRaiser_RegionalDevelopment(&dev_cpu)==RECOMP_RETURN_NORMAL);
  assert(last_development.service_divider==5 && last_development.long_cycle==480);
  assert(cpu_read16(&dev_cpu,0x7f,0x7ced)==3); /* never reset the shared effect phase */
  assert(ActRaiserRegional_CopyRulesView(&view));
  dev_cpu.PB=1;assert(ActRaiser_RegionalEffect(&dev_cpu)==RECOMP_RETURN_NORMAL);
  assert(last_development.effect_divider==5);
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view, kActRaiserRegionalSetting_Recovery,
      kArRegionalSource_Japan) == kActRaiserRegionalEdit_Applied);
  retired_recovery = 0;
  assert(ActRaiser_RegionalRecoveryMoving(&dev_cpu) == RECOMP_RETURN_TAILCALL);
  assert(tail_pc == 0x019c32 && tail_source == 0x019c30 && retired_recovery == 3);
  assert(!last_recovery.cycle_sp && last_recovery.angel_calls == 60);
  retired_recovery = 0;
  assert(ActRaiser_RegionalRecoveryStopped(&dev_cpu) == RECOMP_RETURN_TAILCALL);
  assert(tail_pc == 0x019c3c && tail_source == 0x019c34 && !retired_recovery);
  assert(ActRaiser_RegionalRecoveryDrain(&dev_cpu) == RECOMP_RETURN_TAILCALL);
  assert(tail_pc == 0x01b281 && tail_source == 0x01b257 && !retired_recovery);
  dev_cpu.PB = 3;
  assert(ActRaiser_RegionalRecoveryCycle(&dev_cpu) == RECOMP_RETURN_TAILCALL);
  assert(tail_pc == 0x038298 && tail_source == 0x038271 && !retired_recovery);
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view, kActRaiserRegionalSetting_Recovery,
      kArRegionalSource_Europe) == kActRaiserRegionalEdit_Applied);
  assert(ActRaiser_RegionalRecoveryCycle(&dev_cpu) == RECOMP_RETURN_TAILCALL && retired_recovery == 3);
  assert(last_recovery.cycle_sp && !last_recovery.angel_calls);
  CpuState quake_cpu = {.PB = 1, .DB = 1, .m_flag = 1, .A = 4};
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view, kActRaiserRegionalSetting_Quake,
      kArRegionalSource_Japan) == kActRaiserRegionalEdit_Applied);
  quake_random = edit_during_quake = true;
  assert(ActRaiser_RegionalQuakePlayerEntry(&quake_cpu));
  assert(ActRaiser_RegionalQuakePlayer(&quake_cpu) == RECOMP_RETURN_NORMAL && quake_calls == 1);
  CpuState selector = {.PB = 3, .DB = 0x7f, .m_flag = 1};
  assert(!ActRaiser_RegionalQuakeHousesEntry(&selector));
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(view.requested.quake.source[0] == kArRegionalSource_US && view.effective.quake.source[0] == kArRegionalSource_Japan);
  quake_random = edit_during_quake = false;
  quake_cpu.m_flag = 0; cpu_write16(&quake_cpu, 0x7f, 0x90eb, 4);
  assert(ActRaiser_RegionalQuakePostedEntry(&quake_cpu));
  for (unsigned token = RECOMP_RETURN_NORMAL; token <= RECOMP_RETURN_OWNED_UNWIND; ++token) {
    quake_return = (RecompReturn)token;
    assert(ActRaiser_RegionalQuakePosted(&quake_cpu) == quake_return);
    assert(!ActRaiser_RegionalQuakeHousesEntry(&selector));
  }
  cpu_write16(&quake_cpu, 0x7f, 0x90eb, 3);
  assert(!ActRaiser_RegionalQuakePostedEntry(&quake_cpu));
  quake_cpu.m_flag = 1; quake_cpu.A = 3;
  assert(!ActRaiser_RegionalQuakePlayerEntry(&quake_cpu));
  CpuState report_cpu = {.PB = 1, .DB = 1, .m_flag = 1};
  assert(ActRaiser_RegionalMasterReportEntry(&report_cpu));
  edit_during_report = true;
  assert(ActRaiser_RegionalMasterReport(&report_cpu) == RECOMP_RETURN_NORMAL && report_calls == 1);
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(view.requested.score_page == kArRegionalSource_Japan && view.effective.score_page == kArRegionalSource_US);
  report_scores = false;
  assert(ActRaiser_RegionalMasterReport(&report_cpu) == RECOMP_RETURN_NORMAL && report_calls == 2);
  assert(!ActRaiser_RegionalSkipScoreEntry(&report_cpu));
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(view.requested.score_page == kArRegionalSource_US && view.effective.score_page == kArRegionalSource_Japan);
  report_scores = true; edit_during_report = false;
  for (unsigned token = RECOMP_RETURN_NORMAL; token <= RECOMP_RETURN_OWNED_UNWIND; ++token) {
    report_return = (RecompReturn)token;
    assert(ActRaiser_RegionalMasterReport(&report_cpu) == report_return);
    assert(!ActRaiser_RegionalSkipScoreEntry(&report_cpu));
    assert(ActRaiser_RegionalMasterReportEntry(&report_cpu));
  }
  report_cpu.PB = 3; assert(!ActRaiser_RegionalMasterReportEntry(&report_cpu)); report_cpu.PB = 1;
  report_cpu.DB = 0x7f; assert(!ActRaiser_RegionalMasterReportEntry(&report_cpu)); report_cpu.DB = 1;
  report_cpu.m_flag = 0; assert(!ActRaiser_RegionalMasterReportEntry(&report_cpu));
  report_cpu.m_flag = 1;
  for (unsigned action = 12; action <= 15; ++action) {
    report_cpu.A = (uint16_t)action;
    edit_during_command = true; expected_menu_return = false;
    assert(ActRaiserRegional_ReportCommandEntry(&report_cpu));
    assert(ActRaiserRegional_RunReportCommand(&report_cpu) == RECOMP_RETURN_NORMAL);
    expected_menu_return = true;
    assert(ActRaiserRegional_RunReportCommand(&report_cpu) == RECOMP_RETURN_NORMAL);
    expected_menu_return = false; edit_during_command = false;
    for (unsigned token = RECOMP_RETURN_NORMAL; token <= RECOMP_RETURN_OWNED_UNWIND; ++token) {
      command_return = (RecompReturn)token;
      assert(ActRaiserRegional_RunReportCommand(&report_cpu) == command_return);
      assert(ActRaiserRegional_ReportCommandEntry(&report_cpu));
    }
    command_return = RECOMP_RETURN_NORMAL;
  }
  report_cpu.A = 11; assert(!ActRaiserRegional_ReportCommandEntry(&report_cpu));
  report_cpu.A = 16; assert(!ActRaiserRegional_ReportCommandEntry(&report_cpu));
  edit_during_speed = true;
  assert(ActRaiser_RegionalSpeed(&report_cpu) == RECOMP_RETURN_NORMAL && captured_speed_maximum == 9);
  assert(ActRaiser_RegionalSpeed(&report_cpu) == RECOMP_RETURN_NORMAL && captured_speed_maximum == 7);
  assert(!ActRaiser_RegionalSpeedRightEntry(&report_cpu));
  edit_during_speed = false;
  for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
    assert(ActRaiserRegional_CopyRulesView(&view));
    (void)ActRaiserRegional_RequestRules(&view, kActRaiserRegionalSetting_SpeedRange, (ArRegionalSource)source);
    for (unsigned token = 0; token <= RECOMP_RETURN_OWNED_UNWIND; ++token) {
      speed_return = (RecompReturn)token;
      assert(ActRaiser_RegionalSpeed(&report_cpu) == speed_return);
      assert(captured_speed_maximum == (source == kArRegionalSource_Japan ? 7u : 9u));
      assert(!ActRaiser_RegionalSpeedRightEntry(&report_cpu));
      assert(ActRaiser_RegionalSpeedEntry(&report_cpu));
    }
  }
  CpuState gesture_cpu={.S=0x1fee,.X=0x8a0,.Y=0xa55a,.A=0xbeef};
  for (unsigned source=0;source<kArRegionalSource_Count;++source) {
    assert(ActRaiserRegional_CopyRulesView(&view));
    const ArRegionalSource previous=view.effective.magic_gesture;
    (void)ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_MagicGesture,(ArRegionalSource)source);
    const CpuState before=gesture_cpu;
    for (unsigned buttons=1;buttons<65536;++buttons) {
      if (!(buttons&0x48c0)) continue;
      cpu_write16(&gesture_cpu,0,0x4218,(uint16_t)buttons);
      ram[0xa1]=0; /* Debounced input is not evidence of a physical release. */
      ActRaiserRegional_ObserveInputRelease(&gesture_cpu);
      assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.magic_gesture==previous);
      assert(!memcmp(&before,&gesture_cpu,sizeof(before)));
    }
    cpu_write16(&gesture_cpu,0,0x4218,0x8300); /* Jump/Left/Right don't arm magic. */
    ActRaiserRegional_ObserveInputRelease(&gesture_cpu);
    assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.magic_gesture==(ArRegionalSource)source);
    assert(!memcmp(&before,&gesture_cpu,sizeof(before)));
    assert(ActRaiser_RegionalMagicGestureEntry(&gesture_cpu)==(source==kArRegionalSource_Japan));
    if (source==kArRegionalSource_Japan) {
      assert(ActRaiser_RegionalMagicDedicated(&gesture_cpu)==RECOMP_RETURN_TAILCALL);
      assert(tail_pc==0x984e && tail_source==0x9843 && !memcmp(&before,&gesture_cpu,sizeof(before)));
      for (unsigned up=0;up<2;++up) {
        cpu_write16(&gesture_cpu,0,0xa1,(uint16_t)(0x40+up*8));
        cpu_write16(&gesture_cpu,0,0xf6,0xffff);
        assert(ActRaiser_RegionalMagicAttack(&gesture_cpu)==RECOMP_RETURN_TAILCALL);
        assert(tail_pc==(up ? 0x9de1u : 0x9a73u) && tail_source==0x9a6e);
        assert(cpu_read16(&gesture_cpu,0,0xf6)==0xbfff);
      }
    }
  }
  Install(path, image, kArRegionalSource_Japan);
  cpu.PB = 2; ram[0x336] = 0;
  title_return = RECOMP_RETURN_TAILCALL;
  assert(ActRaiser_RegionalTitle(&cpu) == title_return);
  CheckPrices(kArRegionalSource_US); /* escape is not an accepted selection */
  title_return = RECOMP_RETURN_NORMAL;
  assert(ActRaiser_RegionalTitle(&cpu) == RECOMP_RETURN_NORMAL);
  CheckPrices(kArRegionalSource_US); /* New Game doesn't adopt saved JP */
  assert(title_calls == 5 && miracle_calls == 32);
  remove(path); remove(companion);
  return 0;
}
