#include "actraiser/actraiser_regional_runtime.h"
#include "actraiser/actraiser_miracle.h"
#include "actraiser/actraiser_regional_settings.h"
#include "actraiser/actraiser_development.h"
#include "actraiser/actraiser_quake.h"
#include "actraiser/actraiser_report_command.h"
#include "actraiser/actraiser_town_status_runtime.h"
#include "actraiser/actraiser_level_goals_runtime.h"
#include "actraiser/actraiser_construction_runtime.h"
#include "actraiser/actraiser_population_conversion.h"
#include "actraiser/actraiser_arrival_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The transaction module has its own native-call harness. */
static unsigned population_native_calls,population_commit_calls;
static bool population_accept;
RecompReturn bank_01_85A2_M1X0(CpuState *cpu) {
  assert(!ActRaiser_RegionalPopulationEntry(cpu));++population_native_calls;return RECOMP_RETURN_TAILCALL;
}
ActRaiserPopulationResult ActRaiserPopulation_Preview(CpuState *cpu,const ArRegionalCampaign *campaign,
    ArRegionalSource source,ActRaiserPopulationPreview *out) {
  (void)cpu;(void)campaign;*out=(ActRaiserPopulationPreview){.source=source};
  out->town.removed[0]=4;return kActRaiserPopulation_Ready;
}
ActRaiserPopulationResult ActRaiserPopulation_Commit(CpuState *cpu,ArRegionalCampaign *campaign,
    const ActRaiserPopulationPreview *preview,const char *directory,SaveError *error) {
  (void)cpu;(void)error;assert(directory && strstr(directory,".redevelopment-"));++population_commit_calls;
  assert(ArRegionalSession_SetPopulationProfile(&campaign->active,campaign->active.revision,preview->source));
  return kActRaiserPopulation_Committed;
}
static bool PopulationPrompt(void *context,ActRaiserRegionalPopulationNotice notice,
    ArRegionalSource source,const uint16_t removed[6]) {
  (void)context;(void)source;assert(notice==kActRaiserRegionalPopulation_Failed || removed[0]==4);
  return population_accept;
}
void ActRaiserTownStatusRuntime_Reset(void) {}
void ActRaiserConstructionRuntime_Reset(void) {}
void ActRaiserLevelGoalsRuntime_Reset(void) {}
void ActRaiserArrivalRuntime_Reset(void) {}
void ActRaiserLevelGoalsRuntime_RefreshReport(CpuState *cpu) { (void)cpu; }
static uint8_t ram[65536];
static uint8_t town_ram[65536];
static uint8_t story_rom[65536];
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
static unsigned restore_calls, release_calls, prompt_calls;
static unsigned lives_native_calls;
static RecompReturn lives_return;
RecompReturn bank_02_C280_M0X0(CpuState *cpu) {
  assert(!ActRaiser_RegionalLivesDisplayEntry(cpu)); /* one-shot native delegation */
  ++lives_native_calls;
  return lives_return;
}
static bool prompt_accept;
static RecompReturn restore_result;
RecompReturn bank_03_A83A_M1X0(CpuState *cpu) {
  assert(cpu->PB==3 && cpu_read16(cpu,0,cpu->S+1)==0xa7a2);
  ++restore_calls; cpu->S+=3;
  return restore_result;
}
RecompReturn ActRaiser_WaitForVblank(CpuState *cpu) {
  assert(cpu->PB==2 && cpu_read16(cpu,0,cpu->S+1)==0xa75d);
  ++release_calls;
  if(release_calls==3)ram[0x4219]=0;
  cpu->S+=2;
  return RECOMP_RETURN_NORMAL;
}
static bool ContinuePrompt(void *context, ActRaiserRegionalContinueNotice notice) {
  assert(context==&prompt_calls && notice==kActRaiserRegionalContinue_Estimate);
  ++prompt_calls;
  return prompt_accept;
}
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
static bool skull_skip, skull_edit;
static unsigned skull_calls;
static RecompReturn skull_return;
static RecompReturn story_return;
static unsigned story_native_calls;
RecompReturn bank_03_EB35_M1X0(CpuState *cpu) {
  assert(!ActRaiser_RegionalStoryCompassEntry(cpu));
  ++story_native_calls;return story_return;
}
RecompReturn bank_01_9EE7_M1X0(CpuState *cpu) {
  ++skull_calls;
  assert(!ActRaiser_RegionalSkullUseEntry(cpu)); /* one-shot delegation */
  assert(!ActRaiser_RegionalSkullUseEntry(cpu)); /* no nested capture */
  assert(!ActRaiser_RegionalSkullSkipWaitEntry(cpu)); /* wait prefix is M0 */
  cpu->m_flag=0; cpu->P &= ~CPU_P_M;
  if(skull_edit) {
    ActRaiserRegionalRulesView view;
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_SkullWait,
        skull_skip?kArRegionalSource_US:kArRegionalSource_Japan)==kActRaiserRegionalEdit_Applied);
  }
  assert(ActRaiser_RegionalSkullSkipWaitEntry(cpu)==skull_skip);
  if(skull_skip) {
    const CpuState before=*cpu;
    assert(ActRaiser_RegionalSkullSkipWait(cpu)==RECOMP_RETURN_TAILCALL);
    assert(tail_pc==0x019f87 && tail_source==0x019f80 && !memcmp(&before,cpu,sizeof(before)));
  }
  return skull_return;
}
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
  if(bank==3)return story_rom[address];
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
/* Native bodies remain delegated. This fixture models only their stock side
 * effects and return-frame ownership; the separate adapter tests cover gates. */
static unsigned lair_calls;
static RecompReturn lair_result;
static bool change_seeds_in_lair;
static bool change_house_in_lair;
static bool change_score_in_lair;
static unsigned clear_yields;
static RecompReturn clear_yield_result;
RecompReturn bank_00_85B7_M0X0(CpuState *cpu) {
  assert(cpu->PB==0 && cpu_read16(cpu,0,cpu->S+1)==0xa756);
  ++clear_yields;
  cpu->A=0x4000; cpu->_flag_N=cpu->_flag_Z=0; cpu->P&=~(CPU_P_N|CPU_P_Z);
  cpu_write16(cpu,cpu->DB,cpu->X,0x4000); cpu->S+=2;
  return clear_yield_result;
}
static RecompReturn NativeLair(CpuState *cpu,unsigned event,bool width) {
  assert(cpu->m_flag==width);
  if(event!=3) assert(!(event==4 ? ActRaiser_RegionalLairSeedEntry(cpu) : ActRaiser_RegionalLairEntry(cpu)));
  ++lair_calls;
  if(event==4)for(unsigned i=0;i<24;++i) {
    uint16_t seed; assert(ArRegionalLair_Seed(0,i,&seed)); cpu_write16(cpu,0x7f,0x96b8+2*i,seed);
    assert(ArRegionalLair_Reload(0,i,&seed)); cpu_write16(cpu,0x7f,0x9628+2*i,seed);
  }
  if(event==0) {
    const unsigned base=cpu_read16(cpu,0x7f,0x7bfb)*4;
    for(unsigned n=0;n<4;++n)if(cpu_read16(cpu,0x7f,0x9688+base+n*2)==cpu->X) {
      const unsigned value=cpu_read16(cpu,0x7f,0x96b8+base+n*2);
      if(value)cpu_write16(cpu,0x7f,0x96b8+base+n*2,value-1);
      break;
    }
  }
  if(event==2) {
    const unsigned base=cpu_read16(cpu,0x7f,0x7bfb)*4;
    const unsigned subtype=cpu_read8(cpu,cpu->DB,cpu->X+2);
    CpuState prefix=*cpu;
    prefix.m_flag=0; prefix.A=4+((subtype&0x30)>>3);
    if(ActRaiser_RegionalHouseUnitsEntry(&prefix)) {
      assert(ActRaiser_RegionalHouseUnits(&prefix)==RECOMP_RETURN_TAILCALL);
      assert(tail_pc==0x03b4bc && tail_source==0x03b4b8 && prefix.A==4 && prefix.Y==4);
    }
    unsigned mask=0;
    for(unsigned n=0;n<4;++n)if(cpu_read16(cpu,0x7f,0x95c8+base+2*n)&0x8000)mask|=1u<<n;
    if(mask==15) {
      const unsigned at=0x9efa+base/4;
      cpu_write16(cpu,0x7f,at,(uint16_t)(cpu_read16(cpu,0x7f,at)+prefix.A));
    } else for(unsigned n=0,left=prefix.A;left;n=(n+1)%4)if(!(mask&(1u<<n))) {
      const unsigned at=0x96b8+base+2*n;
      cpu_write16(cpu,0x7f,at,(uint16_t)(cpu_read16(cpu,0x7f,at)+1)); --left;
    }
  }
  if(event==3) {
    const unsigned region=ram[0x341]-1;
    cpu_write16(cpu,0x7f,0x7bf9,region); cpu_write16(cpu,0x7f,0x7bfb,region*2);
    const unsigned completed=cpu_read16(cpu,0x7f,0x6b18+region*2);
    uint32_t target=completed==2?0x03d0c7:completed==1?0x03d0be:0x03d0ce;
    CpuState prefix=*cpu; prefix.m_flag=0; prefix.DB=0x7f; prefix.X=(uint16_t)(region*2);
    if(ActRaiser_RegionalScoreRouteEntry(&prefix)) {
      assert(ActRaiser_RegionalScoreRoute(&prefix)==RECOMP_RETURN_TAILCALL);
      assert(tail_source==0x03d0b3); target=tail_pc;
    }
    if(target!=0x03d0ce) {
      const unsigned bcd=cpu_read16(cpu,0,0x1f);
      const unsigned score=(bcd&15)+((bcd>>4)&15)*10+((bcd>>8)&15)*100+(bcd>>12)*1000;
      prefix.A=(uint16_t)(score/10*2);
      if(ActRaiser_RegionalScoreConversionEntry(&prefix)) {
        assert(ActRaiser_RegionalScoreConversion(&prefix)==RECOMP_RETURN_TAILCALL);
        assert(tail_pc==0x03d10a && tail_source==0x03d0d4);
      }
      if(target==0x03d0c7) {
        const unsigned at=0x9efa+region*2;
        cpu_write16(cpu,0x7f,at,(uint16_t)(cpu_read16(cpu,0x7f,at)+prefix.A));
      } else {
        const unsigned delta=prefix.A>>2; prefix.X=(uint16_t)(region*8);
        if(ActRaiser_RegionalScoreSubtractEntry(&prefix)) {
          cpu_write16(&prefix,0,(uint16_t)(prefix.S+1),delta);
          assert(ActRaiser_RegionalScoreSubtract(&prefix)==RECOMP_RETURN_TAILCALL);
          assert(tail_pc==0x03b549 && tail_source==0x03b525);
        } else for(unsigned n=0;n<4;++n) {
          const unsigned at=0x96b8+prefix.X+2*n;
          cpu_write16(cpu,0x7f,at,(uint16_t)(cpu_read16(cpu,0x7f,at)+delta));
        }
      }
    }
  }
  if(change_score_in_lair) {
    change_score_in_lair=false; ActRaiserRegionalRulesView view;
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_ScoreFeedback,
        kArRegionalSource_US)==kActRaiserRegionalEdit_Applied);
    assert(ActRaiser_RegionalDevelopment(cpu)==RECOMP_RETURN_NORMAL);
    assert(ActRaiserRegional_CopyRulesView(&view));
    for(unsigned i=0;i<kArRegionalScore_Phase;++i)assert(view.effective.score_feedback.source[i]==kArRegionalSource_Japan);
  }
  if(change_house_in_lair) {
    change_house_in_lair=false;
    ActRaiserRegionalRulesView view;
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_HouseCredit,
        kArRegionalSource_US)==kActRaiserRegionalEdit_Applied);
    assert(ActRaiser_RegionalDevelopment(cpu)==RECOMP_RETURN_NORMAL);
    assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.house_credit==kArRegionalSource_Japan);
  }
  if(change_seeds_in_lair) {
    change_seeds_in_lair=false;
    ActRaiserRegionalRulesView view;
    assert(ActRaiserRegional_CopyRulesView(&view) && view.lair_history_ready);
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_LairReserves,
        kArRegionalSource_Japan)==kActRaiserRegionalEdit_Applied);
    assert(ActRaiser_RegionalDevelopment(cpu)==RECOMP_RETURN_NORMAL);
    assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.lair_seeds==kArRegionalSource_US);
    assert(cpu_read16(cpu,0x7f,0x96b8)==199); /* No switch inside the capture. */
  }
  cpu->S+=event==4 ? 2:3;
  if(event!=3) cpu->A=0x5678;
  return lair_result;
}
#define LAIR_STUB(pc,event) \
  RecompReturn bank_03_##pc##_M0X0(CpuState *cpu) { return NativeLair(cpu,event,false); } \
  RecompReturn bank_03_##pc##_M1X0(CpuState *cpu) { return NativeLair(cpu,event,true); }
LAIR_STUB(B7C6,4)
LAIR_STUB(BADD,0)
LAIR_STUB(BA42,1)
LAIR_STUB(B4A6,2)
#undef LAIR_STUB
static RecompReturn reload_return;
static RecompReturn ReduceLairDelays(CpuState *cpu) {
  assert(!ActRaiser_RegionalLairReductionEntry(cpu));
  assert(!ActRaiser_RegionalLairReductionEntry(cpu));
  const unsigned base=cpu_read16(cpu,0x7f,0x7bfb)*4;
  for(unsigned n=0;n<4;++n) {
    const unsigned at=0x9628+base+2*n;
    cpu_write16(cpu,0x7f,at,(cpu_read16(cpu,0x7f,at)>>2)+1);
  }
  return reload_return;
}
RecompReturn bank_03_B6BF_M0X0(CpuState *cpu) { return ReduceLairDelays(cpu); }
RecompReturn bank_03_B6BF_M1X0(CpuState *cpu) { return ReduceLairDelays(cpu); }
RecompReturn bank_03_D095_M0X0(CpuState *cpu) {
  if(ActRaiser_RegionalLairEntry(cpu))return ActRaiser_RegionalLairScore(cpu);
  return NativeLair(cpu,3,false);
}
RecompReturn bank_03_D095_M1X0(CpuState *cpu) {
  if(ActRaiser_RegionalLairEntry(cpu))return ActRaiser_RegionalLairScore(cpu);
  return NativeLair(cpu,3,true);
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
  assert(ActRaiserRegional_BeginActionRoom(3, 0x300, &timer) && timer == 0x300);
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
    ArRegionalTownStatusSnapshot status;
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_TownStatus,
        kArRegionalSource_Japan)==kActRaiserRegionalEdit_Applied);
    assert(ActRaiserRegional_TownStatusSnapshot(false,&status) && !status.japanese[0]);
    assert(ActRaiserRegional_TownStatusSnapshot(true,&status));
    for(unsigned i=0;i<kArRegionalTownStatus_Count;++i) assert(status.japanese[i]);
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_TownStatus,
        kArRegionalSource_US)==kActRaiserRegionalEdit_Applied);
    assert(ActRaiserRegional_TownStatusSnapshot(true,&status) && !status.japanese[0]);
    bool level_jp;
    bool construction_jp;
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Construction,1)==kActRaiserRegionalEdit_Applied);
    assert(ActRaiserRegional_ConstructionSnapshot(false,&construction_jp) && !construction_jp);
    assert(ActRaiserRegional_ConstructionSnapshot(true,&construction_jp) && construction_jp);
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Construction,0)==kActRaiserRegionalEdit_Applied);
    assert(ActRaiserRegional_ConstructionSnapshot(true,&construction_jp) && !construction_jp);
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_LevelGoals,1)==kActRaiserRegionalEdit_Applied);
    assert(ActRaiserRegional_LevelGoalsSnapshot(false,&level_jp) && !level_jp);
    assert(ActRaiserRegional_LevelGoalsSnapshot(true,&level_jp) && level_jp);
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_LevelGoals,0)==kActRaiserRegionalEdit_Applied);
    assert(ActRaiserRegional_LevelGoalsSnapshot(true,&level_jp) && !level_jp);
    uint16_t combat=0xdead;
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_SimCombat,1)==kActRaiserRegionalEdit_Applied);
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_SimAi,1)==kActRaiserRegionalEdit_Applied);
    uint16_t ai=0xdead;
    assert(!ActRaiserRegional_SimActorAiSnapshot(0,0,&ai) && ai==0xdead);
    assert(!ActRaiserRegional_SimActorSnapshot(0,0,&combat) && combat==0xdead);
    ActRaiserRegional_SimActorCache(true,0);ActRaiserRegional_SimActorBirth(0,0);
    assert(ActRaiserRegional_SimActorSnapshot(0,0,&combat) && combat==31);
    assert(ActRaiserRegional_SimActorAiSnapshot(0,0,&ai) && ai==63);
    ActRaiserRegional_SimActorCache(false,0);
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_SimCombat,0)==kActRaiserRegionalEdit_Applied);
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_SimAi,0)==kActRaiserRegionalEdit_Applied);
    ActRaiserRegional_SimActorBirth(0,1);
    assert(ActRaiserRegional_SimActorAiSnapshot(0,1,&ai) && !ai);
    assert(ActRaiserRegional_SimActorAiSnapshot(0,0,&ai) && ai==63);
    assert(ActRaiserRegional_SimActorSnapshot(0,0,&combat) && combat==31);
    ActRaiserRegional_SimActorCache(true,1);ActRaiserRegional_SimActorCache(true,0);
    assert(ActRaiserRegional_SimActorAiSnapshot(0,0,&ai) && ai==63);
    assert(ActRaiserRegional_SimActorSnapshot(0,0,&combat) && combat==31);
    ActRaiserRegional_SimActorBirth(0,0);ActRaiserRegional_SimActorCache(false,0);
    assert(ActRaiserRegional_SimActorSnapshot(0,0,&combat) && !combat);
    assert(ActRaiserRegional_SimActorAiSnapshot(0,0,&ai) && !ai);
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
  assert(!ActRaiserRegional_BeginActionRoom(3, 0x555, &timer) && timer == 0x777);
  ActRaiserRegionalRulesView unchanged;
  assert(ActRaiserRegional_CopyRulesView(&unchanged) && unchanged.revision == view.revision);
  assert(ActRaiserRegional_BeginActionRoom(3, 0x300, &timer) && timer == 0x200);
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(view.effective.timers.source[0] == kArRegionalSource_Japan);
  assert(ActRaiserRegional_ReplayDigest(NULL, active_digest, &baseline) && !baseline);
  assert(memcmp(active_digest, pending_digest, 32));
  assert(ActRaiserRegional_RequestRules(&unchanged, kActRaiserRegionalSetting_RoomTimes,
      kArRegionalSource_US) == kActRaiserRegionalEdit_Stale);
  assert(ActRaiserRegional_RequestRules(&view, kActRaiserRegionalSetting_RoomTimes,
      kArRegionalSource_US) == kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_BeginActionRoom(3, 0x300, &timer) && timer == 0x300);
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
  CpuState source_cpu={.PB=1,.DB=1,.A=5,.X=0x24c,.Y=0x24c,.S=0x1f0,.m_flag=1,.P=CPU_P_M};
  for(unsigned source=0;source<3;++source)for(unsigned item=5;item<=6;++item) {
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Sources,(ArRegionalSource)source)>=kActRaiserRegionalEdit_Unchanged);
    source_cpu.A=item;
    assert(ActRaiser_RegionalSourceCollectionEntry(&source_cpu));
    assert(ActRaiser_RegionalSourceCollection(&source_cpu)==RECOMP_RETURN_TAILCALL);
    assert(tail_source==0x018916 && tail_pc==(source==1?0x01892d:0x018922));
    assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.sources.source[item-5]==(ArRegionalSource)source);
    memset(ram+0x2a2,0,8);ram[0x2a2]=item;
    cpu_write16(&source_cpu,0,source_cpu.S+1,0x9c82);
    cpu_write16(&source_cpu,0,source_cpu.S+4,0x8924);
    const bool keep=item==5?ActRaiser_RegionalSourceLifeKeepEntry(&source_cpu):ActRaiser_RegionalSourceMagicKeepEntry(&source_cpu);
    assert(keep); /* current policy is irrelevant to an already-started auto effect */
    assert((item==5?ActRaiser_RegionalSourceLifeKeep(&source_cpu):ActRaiser_RegionalSourceMagicKeep(&source_cpu))==RECOMP_RETURN_TAILCALL);
    assert(tail_pc==(item==5?0x019cd1:0x019cf3) && ram[0x2a2]==item);
    cpu_write16(&source_cpu,0,source_cpu.S+4,0x88ae);
    assert(!ActRaiser_RegionalSourceLifeKeepEntry(&source_cpu) && !ActRaiser_RegionalSourceMagicKeepEntry(&source_cpu));
  }
  source_cpu.A=7;assert(!ActRaiser_RegionalSourceCollectionEntry(&source_cpu));
  story_rom[0xf543]=110;story_rom[0xf545]=5;
  story_rom[0xf56c]=700&255;story_rom[0xf56d]=700>>8;story_rom[0xf56e]=9;
  for(unsigned source=0;source<3;++source) {
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Story,(ArRegionalSource)source)>=kActRaiserRegionalEdit_Unchanged);
    CpuState story_cpu={.PB=3,.DB=0x7f,.X=0xf543,.S=0x1ef0};
    cpu_write16(&story_cpu,0x7f,0x7bfb,0);
    assert(ActRaiser_RegionalStoryThresholdEntry(&story_cpu));
    assert(ActRaiser_RegionalStoryThreshold(&story_cpu)==RECOMP_RETURN_TAILCALL);
    assert(story_cpu.A==(source==1?88:110) && tail_pc==0x03e142 && tail_source==0x03e13e);
    story_cpu.X=0xf56c;cpu_write16(&story_cpu,0x7f,0x7bfb,4);
    assert(ActRaiser_RegionalStoryThreshold(&story_cpu)==RECOMP_RETURN_TAILCALL);
    assert(story_cpu.A==(source==1?400:700) && story_cpu.S==0x1ef0);
    for(unsigned token=RECOMP_RETURN_NORMAL;token<=RECOMP_RETURN_OWNED_UNWIND;++token) {
      story_cpu.m_flag=1;story_cpu.P|=CPU_P_M;
      story_return=(RecompReturn)token;
      const unsigned calls=story_native_calls;const CpuState before=story_cpu;
      assert(ActRaiser_RegionalStoryCompassEntry(&story_cpu));
      assert(ActRaiser_RegionalStoryCompass(&story_cpu)==(source==1?RECOMP_RETURN_TAILCALL:story_return));
      assert(!memcmp(&before,&story_cpu,sizeof(before)));
      assert(story_native_calls==calls+(source==1?0:1));
      if(source==1)assert(tail_pc==0x03eb3d && tail_source==0x03eb35);
    }
    assert(ActRaiserRegional_CopyRulesView(&view));
    for(unsigned rule=0;rule<kArRegionalStory_Count;++rule)assert(view.effective.story.source[rule]==(ArRegionalSource)source);
  }
  for(unsigned source=0;source<3;++source)for(unsigned edit=0;edit<2;++edit)
    for(unsigned token=RECOMP_RETURN_NORMAL;token<=RECOMP_RETURN_OWNED_UNWIND;++token) {
      assert(ActRaiserRegional_CopyRulesView(&view));
      assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_SkullWait,
          (ArRegionalSource)source)>=kActRaiserRegionalEdit_Unchanged);
      CpuState skull_cpu={.PB=1,.DB=1,.A=14,.S=0x1ee0,.m_flag=1,.P=CPU_P_M};
      skull_return=(RecompReturn)token;skull_skip=source==1;skull_edit=edit;
      assert(ActRaiser_RegionalSkullUseEntry(&skull_cpu));
      const unsigned calls=skull_calls;
      assert(ActRaiser_RegionalSkullUse(&skull_cpu)==skull_return && skull_calls==calls+1);
      assert(!ActRaiser_RegionalSkullSkipWaitEntry(&skull_cpu) && skull_cpu.S==0x1ee0);
      skull_cpu.m_flag=1;skull_cpu.P|=CPU_P_M;
      assert(ActRaiser_RegionalSkullUseEntry(&skull_cpu));
      assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.skull_wait==(ArRegionalSource)source);
      assert(view.requested.skull_wait==(edit?(source==1?kArRegionalSource_US:kArRegionalSource_Japan):(ArRegionalSource)source));
      skull_cpu.DB=0x7f;assert(!ActRaiser_RegionalSkullUseEntry(&skull_cpu));
    }
  CpuState lives_cpu={.PB=2,.X=0x50,.A=0xabcd,.S=0x1ef};
  ram[0x1c]=2; town_ram[0xb051]=0x20; town_ram[0xb053]=0x20;
  for (unsigned source=0; source<3; ++source) {
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_LivesDisplay,(ArRegionalSource)source)>=kActRaiserRegionalEdit_Unchanged);
    lives_cpu.m_flag=0; lives_cpu.P=0; cpu_p_to_mirrors(&lives_cpu);
    if (source==0) assert(!ActRaiser_RegionalLivesDisplayEntry(&lives_cpu));
    else {
      assert(ActRaiser_RegionalLivesDisplayEntry(&lives_cpu));
      assert(ActRaiser_RegionalLivesDisplay(&lives_cpu)==(source==1?RECOMP_RETURN_TAILCALL:lives_return));
      assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.lives_display==(ArRegionalSource)source);
      if (source==1) {
        assert(tail_pc==0x02c2a4 && tail_source==0x02c280);
        assert(town_ram[0xb050]=='0' && town_ram[0xb052]=='2');
        assert(town_ram[0xb051]==0x20 && town_ram[0xb053]==0x20);
        assert(lives_cpu.A==0xab32 && lives_cpu.S==0x1ef && ram[0x1c]==2);
        const uint32_t revision=view.revision;
        lives_cpu.P=0; cpu_p_to_mirrors(&lives_cpu);
        assert(ActRaiser_RegionalLivesDisplay(&lives_cpu)==RECOMP_RETURN_TAILCALL);
        assert(ActRaiserRegional_CopyRulesView(&view) && view.revision==revision);
      } else assert(lives_native_calls==1 && !ActRaiser_RegionalLivesDisplayEntry(&lives_cpu));
    }
  }
  for (unsigned token=0;token<=RECOMP_RETURN_OWNED_UNWIND;++token) {
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_LivesDisplay,kArRegionalSource_Japan)==kActRaiserRegionalEdit_Applied);
    lives_cpu.P=0; cpu_p_to_mirrors(&lives_cpu);
    assert(ActRaiser_RegionalLivesDisplay(&lives_cpu)==RECOMP_RETURN_TAILCALL);
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_LivesDisplay,kArRegionalSource_US)==kActRaiserRegionalEdit_Applied);
    lives_cpu.P=0; cpu_p_to_mirrors(&lives_cpu); lives_return=(RecompReturn)token;
    assert(ActRaiser_RegionalLivesDisplay(&lives_cpu)==lives_return);
    assert(!ActRaiser_RegionalLivesDisplayEntry(&lives_cpu));
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
  for(unsigned width=0;width<2;++width) {
    memset(town_ram+0x96b8,0,48);
    cpu=(CpuState){.PB=2,.m_flag=1}; ram[0x336]=0;
    assert(ActRaiser_RegionalTitle(&cpu)==RECOMP_RETURN_NORMAL);
    cpu=(CpuState){.PB=3,.DB=0x7f,.m_flag=width,.S=0x1e00};
    assert(ActRaiser_RegionalLairSeedEntry(&cpu));
    lair_result=RECOMP_RETURN_NORMAL;
    unsigned count=lair_calls;
    assert(ActRaiser_RegionalLairSeed(&cpu)==RECOMP_RETURN_NORMAL);
    assert(lair_calls==count+1 && cpu.A==0x5678 && cpu.S==0x1e02);
    assert(!ActRaiser_RegionalLairSeedEntry(&cpu) && ActRaiser_RegionalLairEntry(&cpu));
    cpu.X=0xb30; cpu_write16(&cpu,0x7f,0x9688,cpu.X); cpu_write16(&cpu,0x7f,0x7bfb,0);
    count=lair_calls;
    change_seeds_in_lair=true;
    assert(ActRaiser_RegionalLairKill(&cpu)==RECOMP_RETURN_NORMAL);
    assert(lair_calls==count+1 && cpu.S==0x1e05 && cpu_read16(&cpu,0x7f,0x96b8)==199);
    assert(ActRaiserRegional_ReplayDigest(NULL,pending_digest,&baseline) && !baseline);
    assert(ActRaiser_RegionalDevelopment(&cpu)==RECOMP_RETURN_NORMAL);
    assert(cpu_read16(&cpu,0x7f,0x96b8)==249);
    assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.lair_seeds==kArRegionalSource_Japan);
    assert(ActRaiserRegional_ReplayDigest(NULL,active_digest,&baseline) && !baseline);
    assert(memcmp(pending_digest,active_digest,32));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_LairReserves,
        kArRegionalSource_US)==kActRaiserRegionalEdit_Applied);
    assert(ActRaiser_RegionalDevelopment(&cpu)==RECOMP_RETURN_NORMAL);
    assert(cpu_read16(&cpu,0x7f,0x96b8)==199 && cpu.S==0x1e05);
    assert(ActRaiserRegional_ReplayDigest(NULL,active_digest,&baseline) && baseline);
    ActRaiserRegional_CheckLairHistory(&cpu);
    SaveError error={{0}};
    assert(SaveSystem_BeginNativeWrite(&error) && SaveSystem_EndNativeWrite(true,&error));
    assert(SaveSystem_AutoPersistIfChanged(&error));
    uint8_t durable[kActRaiserSramSize]; assert(SaveSystem_CopyDurableImage(durable));
    ArRegionalSession loaded;
    assert(ArRegionalSession_Load(&loaded,0,path,durable,&error)==kSaveCheckpoint_Ready);
    assert(loaded.lairs.initialized_towns==63 && !loaded.lairs.diverged_towns);
    assert(loaded.lairs.stock[0][0]==199 && loaded.lairs.stock[1][0]==249);
    /* Preserve escape exactly; retain pre-event history as diverged. */
    lair_result=RECOMP_RETURN_PARKED_WAIT;
    assert(ActRaiser_RegionalLairKill(&cpu)==RECOMP_RETURN_PARKED_WAIT);
    assert(SaveSystem_BeginNativeWrite(&error) && SaveSystem_EndNativeWrite(true,&error));
    assert(SaveSystem_AutoPersistIfChanged(&error));
    assert(ArRegionalSession_Load(&loaded,0,path,durable,&error)==kSaveCheckpoint_Ready);
    assert(loaded.lairs.diverged_towns==1 && loaded.lairs.stock[0][0]==199);
    assert(ActRaiserRegional_CopyRulesView(&view) && !view.lair_history_ready);
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_LairReserves,
        kArRegionalSource_Japan)==kActRaiserRegionalEdit_HistoryUnavailable);
  }
  /* Shared real events, independent seed/house choices. Switching projects
   * only stock history; already awarded all-sealed growth is never replayed. */
  for(unsigned width=0;width<2;++width)for(unsigned mask=0;mask<16;++mask)
  for(unsigned tier=0;tier<4;++tier) {
    memset(town_ram,0,sizeof(town_ram));
    cpu=(CpuState){.PB=2,.m_flag=1}; ram[0x336]=0;
    assert(ActRaiser_RegionalTitle(&cpu)==RECOMP_RETURN_NORMAL);
    cpu=(CpuState){.PB=3,.DB=0x7f,.m_flag=width,.S=0x1e00};
    lair_result=RECOMP_RETURN_NORMAL;
    assert(ActRaiser_RegionalLairSeed(&cpu)==RECOMP_RETURN_NORMAL);
    cpu.X=0x6be7; town_ram[cpu.X+2]=(uint8_t)(tier<<4);
    for(unsigned n=0;n<4;++n)cpu_write16(&cpu,0x7f,0x95c8+2*n,mask&(1u<<n)?0x8000:0);
    assert(ActRaiser_RegionalLairHouse(&cpu)==RECOMP_RETURN_NORMAL);
    assert(ActRaiserRegional_CopyRulesView(&view) && view.lair_history_ready);
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_HouseCredit,
        kArRegionalSource_Japan)==kActRaiserRegionalEdit_Applied);
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_LairReserves,
        kArRegionalSource_Japan)==kActRaiserRegionalEdit_Applied);
    assert(ActRaiser_RegionalDevelopment(&cpu)==RECOMP_RETURN_NORMAL);
    unsigned sum=0;
    for(unsigned n=0;n<4;++n)sum+=cpu_read16(&cpu,0x7f,0x96b8+2*n);
    assert(sum==600u+(mask==15?0u:4u));
    assert(cpu_read16(&cpu,0x7f,0x9efa)==(mask==15?4+tier*2:0));
    change_house_in_lair=true;
    assert(ActRaiser_RegionalLairHouse(&cpu)==RECOMP_RETURN_NORMAL);
    assert(ActRaiserRegional_CopyRulesView(&view) && view.lair_history_ready);
    assert(view.effective.house_credit==kArRegionalSource_Japan && view.requested.house_credit==kArRegionalSource_US);
    assert(ActRaiser_RegionalDevelopment(&cpu)==RECOMP_RETURN_NORMAL);
    sum=0;
    for(unsigned n=0;n<4;++n) {
      sum+=cpu_read16(&cpu,0x7f,0x96b8+2*n);
      assert(cpu_read16(&cpu,0x7f,0x95c8+2*n)==(mask&(1u<<n)?0x8000:0));
    }
    assert(sum==600u+(mask==15?0u:2*(4+tier*2)));
    assert(cpu_read16(&cpu,0x7f,0x9efa)==(mask==15?8+tier*2:0));
    assert(ActRaiserRegional_CopyRulesView(&view) && view.lair_history_ready);
  }
  /* Score capture belongs to the completed town, not the previous selection.
   * A request made during action mode activates before this settlement; a
   * nested edit stays pending until the native transaction has returned. */
  const unsigned scores[]={0,650,682,9999};
  for(unsigned source=0;source<3;++source)for(unsigned completed=0;completed<4;++completed)
  for(unsigned sample=0;sample<4;++sample)for(unsigned width=0;width<2;++width) {
    memset(town_ram,0,sizeof(town_ram));
    cpu=(CpuState){.PB=2,.m_flag=1}; ram[0x336]=0;
    assert(ActRaiser_RegionalTitle(&cpu)==RECOMP_RETURN_NORMAL);
    cpu=(CpuState){.PB=3,.DB=0x7f,.m_flag=width,.S=0x1e00}; lair_result=RECOMP_RETURN_NORMAL;
    assert(ActRaiser_RegionalLairSeed(&cpu)==RECOMP_RETURN_NORMAL);
    ram[0x341]=2; cpu_write16(&cpu,0x7f,0x6b1a,completed); /* Bloodpool, while selected town is Fillmore. */
    cpu_write16(&cpu,0x7f,0x9efc,10);
    const unsigned score=scores[sample];
    const uint16_t bcd=(uint16_t)((score%10)|((score/10%10)<<4)|((score/100%10)<<8)|((score/1000)<<12));
    cpu_write16(&cpu,0,0x1f,bcd);
    assert(ActRaiserRegional_CopyRulesView(&view));
    const ActRaiserRegionalEditResult edit=ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_ScoreFeedback,(ArRegionalSource)source);
    assert(edit==(source?kActRaiserRegionalEdit_Applied:kActRaiserRegionalEdit_Unchanged));
    change_score_in_lair=source==1;
    assert(ActRaiser_RegionalLairScore(&cpu)==RECOMP_RETURN_NORMAL);
    assert(!ActRaiser_RegionalScoreConversionEntry(&cpu)); /* transaction snapshot retired */
    assert(ActRaiserRegional_CopyRulesView(&view) && view.lair_history_ready);
    const unsigned converted=source==1?(score<650?0:(score-650)/32*10):score/10*2;
    assert(cpu_read16(&cpu,0x7f,0x9efc)==10+(completed==2?converted:0));
    for(unsigned n=0;n<4;++n) {
      uint16_t seed; assert(ArRegionalLair_Seed(kArRegionalSource_US,4+n,&seed));
      unsigned expected=seed;
      if(completed!=2 && (source==1 || completed==1)) {
        const unsigned delta=converted/4;
        expected=source==1?(seed>=delta?seed-delta:0):(uint16_t)(seed+delta);
      }
      assert(cpu_read16(&cpu,0x7f,0x96c0+n*2)==expected);
    }
    const unsigned earned=cpu_read16(&cpu,0x7f,0x9efc);
    assert(ActRaiser_RegionalDevelopment(&cpu)==RECOMP_RETURN_NORMAL);
    assert(cpu_read16(&cpu,0x7f,0x9efc)==earned); /* no retroactive growth */
    assert(ActRaiserRegional_CopyRulesView(&view) && view.lair_history_ready);
    if(source==1)for(unsigned n=0;n<4;++n) {
      uint16_t seed; assert(ArRegionalLair_Seed(kArRegionalSource_US,4+n,&seed));
      assert(cpu_read16(&cpu,0x7f,0x96c0+n*2)==seed+(completed==1?(score/10*2)/4:0));
    }
  }
  /* The same native clear/departure calls see different scores. Changing
   * the policy during the tally must not move or duplicate this settlement. */
  for(unsigned source=0;source<3;++source)for(unsigned completed=1;completed<=2;++completed)
  for(unsigned region=1;region<=6;++region) {
    memset(town_ram,0,sizeof(town_ram));
    cpu=(CpuState){.PB=2,.m_flag=1}; ram[0x336]=0;
    assert(ActRaiser_RegionalTitle(&cpu)==RECOMP_RETURN_NORMAL);
    cpu=(CpuState){.PB=3,.DB=0x7f,.S=0x1e00}; lair_result=RECOMP_RETURN_NORMAL;
    assert(ActRaiser_RegionalLairSeed(&cpu)==RECOMP_RETURN_NORMAL);
    ram[0x341]=(uint8_t)region; ram[0x18]=(uint8_t)region; ram[0x19]=(uint8_t)completed;
    cpu_write16(&cpu,0x7f,0x6b18+(region-1)*2,completed);
    cpu_write16(&cpu,0,0x1f,0x1314);
    assert(ActRaiserRegional_CopyRulesView(&view));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_ScoreFeedback,
        (ArRegionalSource)source)==(source?kActRaiserRegionalEdit_Applied:kActRaiserRegionalEdit_Unchanged));
    const unsigned start_calls=lair_calls;
    clear_yield_result=RECOMP_RETURN_NORMAL;
    cpu=(CpuState){.PB=0,.DB=0,.S=0x1f00,.A=0x0c0d,.Y=0xa8d8,.X=0xe20};
    assert(ActRaiser_RegionalScoreCardEntry(&cpu));
    assert(ActRaiser_RegionalScoreCard(&cpu)==RECOMP_RETURN_TAILCALL);
    assert(tail_pc==0x00a757 && tail_source==0x00a754 && cpu.S==0x1f00 && cpu.PB==0);
    assert(lair_calls==start_calls+(source==1));
    assert(ActRaiserRegional_CopyRulesView(&view) && view.lair_history_ready);
    for(unsigned i=0;i<kArRegionalScore_Count;++i)assert(view.effective.score_feedback.source[i]==(ArRegionalSource)source);
    uint8_t captured[32], repeated[32]; bool baseline;
    assert(ActRaiserRegional_ReplayDigest(NULL,captured,&baseline));
    /* Reentry at the same clear boundary does not credit a second event. */
    assert(ActRaiser_RegionalScoreCard(&cpu)==RECOMP_RETURN_TAILCALL);
    assert(lair_calls==start_calls+(source==1));
    assert(ActRaiserRegional_ReplayDigest(NULL,repeated,&baseline) && !memcmp(captured,repeated,32));
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_ScoreFeedback,
        source==1?kArRegionalSource_US:kArRegionalSource_Japan)==kActRaiserRegionalEdit_Applied);
    CpuState town_cpu={.PB=3,.S=0x1f00};
    assert(ActRaiser_RegionalDevelopment(&town_cpu)==RECOMP_RETURN_NORMAL);
    assert(ActRaiserRegional_CopyRulesView(&view));
    for(unsigned i=0;i<kArRegionalScore_Count;++i)assert(view.effective.score_feedback.source[i]==(ArRegionalSource)source);
    cpu_write16(&cpu,0,0x1f,0x1799);
    assert(ActRaiser_RegionalScoreDeparture(&cpu)==RECOMP_RETURN_TAILCALL);
    assert(tail_pc==0x00a311 && tail_source==0x00a30d && cpu.S==0x1f00 && cpu.PB==0);
    assert(lair_calls==start_calls+1 && cpu.A==0x4000 && cpu.Y==0xa8d8);
    assert(cpu_read16(&cpu,0,0xe20)==0x4000);
    const unsigned converted=source==1?200:358;
    assert(cpu_read16(&cpu,0x7f,0x9efa+(region-1)*2)==(completed==2?converted:0));
    for(unsigned n=0;n<4;++n) {
      uint16_t seed; assert(ArRegionalLair_Seed(kArRegionalSource_US,(region-1)*4+n,&seed));
      const unsigned expected=completed==2?seed:source==1?(seed>=50?seed-50:0):seed+89;
      assert(cpu_read16(&cpu,0x7f,0x96b8+(region-1)*8+n*2)==expected);
    }
    assert(ActRaiserRegional_CopyRulesView(&view) && view.lair_history_ready);
    const unsigned growth=cpu_read16(&cpu,0x7f,0x9efa+(region-1)*2);
    assert(ActRaiser_RegionalDevelopment(&town_cpu)==RECOMP_RETURN_NORMAL);
    assert(cpu_read16(&cpu,0x7f,0x9efa+(region-1)*2)==growth);
    assert(ActRaiserRegional_CopyRulesView(&view) && view.lair_history_ready);
    /* Phase itself waits for the next accepted clear, even once stocks switch. */
    assert(view.effective.score_feedback.source[kArRegionalScore_Phase]==(ArRegionalSource)source);
  }
  remove(path); remove(companion);
  for(unsigned source=0;source<3;++source)for(unsigned width=0;width<2;++width)
  for(unsigned token=0;token<=RECOMP_RETURN_OWNED_UNWIND;++token) {
    memset(town_ram,0,sizeof(town_ram));
    cpu=(CpuState){.PB=2,.m_flag=1};ram[0x336]=0;
    assert(ActRaiser_RegionalTitle(&cpu)==RECOMP_RETURN_NORMAL);
    cpu=(CpuState){.PB=3,.DB=0x7f,.S=0x1e00};lair_result=RECOMP_RETURN_NORMAL;
    assert(ActRaiser_RegionalLairSeed(&cpu)==RECOMP_RETURN_NORMAL);
    assert(ActRaiserRegional_CopyRulesView(&view) && view.lair_reload_ready);
    assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_LairReloads,(ArRegionalSource)source)>=kActRaiserRegionalEdit_Unchanged);
    for(unsigned i=0;i<24;++i) cpu_write16(&cpu,0x7f,0x9658+2*i,0x1234+i);
    cpu.DB=1;assert(ActRaiser_RegionalDevelopment(&cpu)==RECOMP_RETURN_NORMAL);cpu.DB=0x7f;
    assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.lair_reloads==(ArRegionalSource)source);
    for(unsigned i=0;i<24;++i) {
      uint16_t expected;assert(ArRegionalLair_Reload((ArRegionalSource)source,i,&expected));
      assert(cpu_read16(&cpu,0x7f,0x9628+2*i)==expected);
      assert(cpu_read16(&cpu,0x7f,0x9658+2*i)==0x1234+i);
    }
    cpu_write16(&cpu,0x7f,0x7bfb,6);cpu.m_flag=width;reload_return=(RecompReturn)token;
    assert(ActRaiser_RegionalLairReductionEntry(&cpu));const CpuState previous=cpu;
    assert(ActRaiser_RegionalLairReduction(&cpu)==reload_return && !memcmp(&cpu,&previous,sizeof(cpu)));
    assert(ActRaiserRegional_CopyRulesView(&view) && view.lair_reload_ready==(token==RECOMP_RETURN_NORMAL));
    if(token==RECOMP_RETURN_NORMAL) {
      assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_LairReloads,source==1?0:1)==kActRaiserRegionalEdit_Applied);
      assert(ActRaiser_RegionalDevelopment(&cpu)==RECOMP_RETURN_NORMAL);
      uint16_t expected;assert(ArRegionalLair_Reload(source==1?0:1,13,&expected));
      assert(cpu_read16(&cpu,0x7f,0x9642)==(expected>>2)+1);
    }
  }
  cpu=(CpuState){.PB=2,.m_flag=1};ram[0x336]=0;
  assert(ActRaiser_RegionalTitle(&cpu)==RECOMP_RETURN_NORMAL);
  assert(ActRaiserRegional_CopyRulesView(&view) && !view.arrival_locked);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Arrival,1)==kActRaiserRegionalEdit_Applied);
  bool arrival_jp;
  assert(ActRaiserRegional_ArrivalSnapshot(false,false,&arrival_jp) && arrival_jp);
  assert(ActRaiserRegional_CopyRulesView(&view) && !view.arrival_locked && view.effective.arrival==0);
  uint8_t before_arrival[32],after_arrival[32];bool native_arrival;
  assert(ActRaiserRegional_ReplayDigest(NULL,before_arrival,&native_arrival) && !native_arrival);
  assert(ActRaiserRegional_ArrivalSnapshot(true,false,&arrival_jp) && arrival_jp);
  assert(ActRaiserRegional_CopyRulesView(&view) && view.arrival_locked && view.effective.arrival==1);
  assert(ActRaiserRegional_ReplayDigest(NULL,after_arrival,&native_arrival) && memcmp(before_arrival,after_arrival,32));
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Arrival,0)==kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_ArrivalSnapshot(true,false,&arrival_jp) && arrival_jp);
  /* Conversion is only queued by settings; cancellation, stale previews and
   * replay locks never change support. The selector's native token survives. */
  cpu=(CpuState){.PB=2,.m_flag=1};ram[0x336]=0;
  assert(ActRaiser_RegionalTitle(&cpu)==RECOMP_RETURN_NORMAL);
  assert(!ActRaiserRegional_ActionMotionSnapshot());
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_ActionMotion,1)==kActRaiserRegionalEdit_Applied);
  assert(!ActRaiserRegional_ActionMotionSnapshot());
  uint16_t motion_time;
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && ActRaiserRegional_ActionMotionSnapshot()==0x3fff);
  assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.action_motion.source[0]==1);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_ActionMotion,0)==kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_ActionMotionSnapshot()==0x3fff);
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && !ActRaiserRegional_ActionMotionSnapshot());
  assert(!ActRaiserRegional_EmitterSnapshot());
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Emitters,2)==kActRaiserRegionalEdit_Applied);
  assert(!ActRaiserRegional_EmitterSnapshot());
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && ActRaiserRegional_EmitterSnapshot()==6);
  assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.emitters.source[0]==2);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Emitters,0)==kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_EmitterSnapshot()==6);
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && !ActRaiserRegional_EmitterSnapshot());
  assert(!ActRaiserRegional_DoubleStatueVolley());
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_StatueVolley,2)==kActRaiserRegionalEdit_Applied);
  assert(!ActRaiserRegional_DoubleStatueVolley());
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && ActRaiserRegional_DoubleStatueVolley());
  assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.statue_volley==2);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_StatueVolley,0)==kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_DoubleStatueVolley());
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && !ActRaiserRegional_DoubleStatueVolley());
  assert(!ActRaiserRegional_BossSnapshot());
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Bosses,1)==kActRaiserRegionalEdit_Applied);
  assert(!ActRaiserRegional_BossSnapshot());
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time));
  const uint64_t boss_rules=ActRaiserRegional_BossSnapshot();assert(boss_rules);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_MinoIdle)==15);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_TanzraClosing)==3);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_TanzraClock)==0);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_TanzraUpperTurn)==11);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_TanzraMinionTurn)==16);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_AntlionTrigger)==2304);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_AntlionStrategy)==1);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_ViperChoice)==1);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_ViperLightning)==21);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_ViperRematchLightning)==10);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_ViperFloor)==22);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_PharaohLanding)==24);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_PharaohRematchLanding)==24);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_PharaohHeads)==1);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_PlantCycle)==1);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_PlantOpen)==8);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_PlantHighWindup)==7);
  assert(ArRegionalBoss_Value(boss_rules,kArRegionalBoss_PlantLowWindup)==7);
  assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.bosses.source[0]==1);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Bosses,0)==kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_BossSnapshot()==boss_rules);
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && !ActRaiserRegional_BossSnapshot());
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Bosses,2)==kActRaiserRegionalEdit_Applied);
  assert(ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_DragonProjectileDelay)==0);
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time));
  assert(ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_DragonProjectileDelay)==15);
  assert(ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_DragonProjectileFlight)==5);
  assert(ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_ViperChoice)==2);
  assert(ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_ViperLightning)==17);
  assert(ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_ViperRematchLightning)==8);
  assert(ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_ViperFloor)==15);
  assert(ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_PharaohLanding)==40);
  assert(ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_PharaohRematchLanding)==56);
  assert(!ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_PharaohHeads));
  assert(ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_PlantCycle)==1);
  assert(ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_PlantOpen)==16);
  assert(ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_PlantHighWindup)==23);
  assert(ArRegionalBoss_Value(ActRaiserRegional_BossSnapshot(),kArRegionalBoss_PlantLowWindup)==23);
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Bosses,0)==kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && !ActRaiserRegional_BossSnapshot());
  assert(!ActRaiserRegional_CollisionSnapshot());
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Collision,1)==kActRaiserRegionalEdit_Applied);
  assert(!ActRaiserRegional_CollisionSnapshot());
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && ActRaiserRegional_CollisionSnapshot()==3);
  assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.collision.source[0]==1);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Collision,0)==kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_CollisionSnapshot()==3);
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && !ActRaiserRegional_CollisionSnapshot());
  assert(!ActRaiserRegional_PlatformSkullSnapshot());
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_PlatformSkull,1)==kActRaiserRegionalEdit_Applied);
  assert(!ActRaiserRegional_PlatformSkullSnapshot());
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && ActRaiserRegional_PlatformSkullSnapshot()==15);
  assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.platform_skull.source[0]==1);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_PlatformSkull,0)==kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_PlatformSkullSnapshot()==15);
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && !ActRaiserRegional_PlatformSkullSnapshot());
  assert(!ActRaiserRegional_ActorStatsEnabled());
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_ActorStats,2)==kActRaiserRegionalEdit_Applied);
  assert(!ActRaiserRegional_ActorStatsEnabled());
  assert(ActRaiserRegional_ActorChildStat(kArRegionalActorStat_TanzraMinionHp)==2);
  assert(ActRaiserRegional_ActorChildStat(kArRegionalActorStat_TanzraProjectileAttack)==3);
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && ActRaiserRegional_ActorStatsEnabled());
  assert(ActRaiserRegional_ActorChildStat(kArRegionalActorStat_TanzraMinionHp)==1);
  assert(ActRaiserRegional_ActorChildStat(kArRegionalActorStat_TanzraMinionReward)==1);
  assert(ActRaiserRegional_ActorChildStat(kArRegionalActorStat_TanzraProjectileAttack)==5);
  assert(ActRaiserRegional_ActorChildStat(0)==UINT16_MAX && ActRaiserRegional_ActorChildStat(kArRegionalActorStat_Count)==UINT16_MAX);
  uint16_t stat_hp=0,stat_attack=0;
  assert(ActRaiserRegional_ActorStats(0x040c,0,1,&stat_hp,&stat_attack) && stat_hp==2 && stat_attack==2);
  assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.actor_stats.source[0]==2);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_ActorStats,0)==kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_ActorStatsEnabled());
  assert(ActRaiserRegional_ActorChildStat(kArRegionalActorStat_TanzraProjectileAttack)==5);
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && !ActRaiserRegional_ActorStatsEnabled());
  assert(ActRaiserRegional_ActorChildStat(kArRegionalActorStat_TanzraMinionHp)==2);
  assert(ActRaiserRegional_ActorChildStat(kArRegionalActorStat_TanzraProjectileAttack)==3);
  assert(!ActRaiserRegional_CastHoldSnapshot());
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_CastHold,2)==kActRaiserRegionalEdit_Applied);
  assert(!ActRaiserRegional_CastHoldSnapshot());
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && ActRaiserRegional_CastHoldSnapshot()==7);
  assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.cast_hold.source[0]==2);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_CastHold,1)==kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_CastHoldSnapshot()==7);
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && !ActRaiserRegional_CastHoldSnapshot());
  assert(!ActRaiserRegional_FireSnapshot());
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_FireEnemy,1)==kActRaiserRegionalEdit_Applied);
  assert(!ActRaiserRegional_FireSnapshot());
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && ActRaiserRegional_FireSnapshot()==15);
  assert(ActRaiserRegional_CopyRulesView(&view) && view.effective.fire_enemy.source[0]==1);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_FireEnemy,2)==kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_FireSnapshot()==15);
  assert(ActRaiserRegional_BeginActionRoom(3,0x300,&motion_time) && !ActRaiserRegional_FireSnapshot());
  ActRaiserRegional_SetPopulationPrompt(PopulationPrompt,NULL);
  assert(ActRaiserRegional_CopyRulesView(&view));
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Population,1)==kActRaiserRegionalEdit_Deferred);
  assert(ActRaiserRegional_CopyRulesView(&view) && view.population_pending && view.pending_population==1);
  ArRegionalSupportSnapshot support;
  assert(ActRaiserRegional_CopySupport(&support) && support.amount[0]==32);
  cpu=(CpuState){.PB=1,.DB=1,.m_flag=1,.S=0x1ee0};
  assert(ActRaiser_RegionalPopulationEntry(&cpu));
  population_accept=false;
  assert(ActRaiser_RegionalPopulation(&cpu)==RECOMP_RETURN_TAILCALL);
  assert(population_native_calls==1 && !population_commit_calls && !ActRaiser_RegionalPopulationEntry(&cpu));
  assert(ActRaiserRegional_CopyRulesView(&view) && !view.population_pending);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Population,1)==kActRaiserRegionalEdit_Deferred);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Population,0)==kActRaiserRegionalEdit_Unchanged);
  assert(!ActRaiser_RegionalPopulationEntry(&cpu));
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Population,1)==kActRaiserRegionalEdit_Deferred);
  edits_allowed=false;assert(!ActRaiser_RegionalPopulationEntry(&cpu));
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Population,2)==kActRaiserRegionalEdit_Locked);
  edits_allowed=true;population_accept=true;
  assert(ActRaiser_RegionalPopulation(&cpu)==RECOMP_RETURN_TAILCALL);
  assert(population_native_calls==2 && population_commit_calls==1);
  assert(ActRaiserRegional_CopySupport(&support) && support.amount[0]==16);
  assert(ActRaiserRegional_CopyRulesView(&view) && !view.population_pending);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_LevelGoals,0)==kActRaiserRegionalEdit_Incompatible);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Story,0)==kActRaiserRegionalEdit_Incompatible);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Population,0)==kActRaiserRegionalEdit_Deferred);
  assert(ActRaiserRegional_RequestRules(&view,kActRaiserRegionalSetting_Construction,1)==kActRaiserRegionalEdit_Applied);
  assert(ActRaiserRegional_CopyRulesView(&view) && !view.population_pending);
  assert(ActRaiser_RegionalPopulation(&cpu)==RECOMP_RETURN_TAILCALL);
  assert(population_native_calls==3 && population_commit_calls==1);
  assert(ActRaiserRegional_CopySupport(&support) && support.amount[0]==16);
  /* Accepted Continue owns its original title frame, including cancellation
   * and a held native accept button. Replays cannot create an acknowledgement. */
  memset(image,0,sizeof(image)); Save_RecomputeChecksum(image);
  assert(Save_WriteFile(kSaveFileFormat_NativeSrm,path,image,&error));
  assert(SaveSystem_LoadActive(&error));
  assert(ActRaiserRegional_Initialize(Identity,NULL));
  ActRaiserRegional_SetContinuePrompt(ContinuePrompt,&prompt_calls);
  cpu=(CpuState){.PB=2,.DB=2,.m_flag=1,.S=0x1ee0}; ram[0x336]=1;
  edits_allowed=false; assert(!ActRaiser_RegionalContinueEntry(&cpu));
  edits_allowed=true; assert(ActRaiser_RegionalContinueEntry(&cpu));
  ram[0x4219]=0x80; prompt_accept=false;
  assert(ActRaiser_RegionalContinue(&cpu)==RECOMP_RETURN_TAILCALL);
  assert(prompt_calls==1 && release_calls==3 && !restore_calls);
  assert(cpu.S==0x1ee0 && cpu.PB==2 && tail_pc==0x02a75b && tail_source==0x02a79f);
  assert(!ActRaiserRegional_CopyRulesView(&view));
  ArRegionalSession loaded;
  assert(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Missing);
  prompt_accept=true;
  assert(ActRaiser_RegionalContinue(&cpu)==RECOMP_RETURN_TAILCALL);
  assert(prompt_calls==2 && restore_calls==1 && tail_pc==0x02a7a3);
  assert(cpu.S==0x1ee0 && cpu.PB==2);
  assert(ArRegionalSession_Load(&loaded,0,path,image,&error)==kSaveCheckpoint_Ready);
  assert(loaded.lairs.initialized_towns==63 && loaded.lairs.approximate_towns==63);
  assert(ActRaiser_RegionalContinue(&cpu)==RECOMP_RETURN_TAILCALL);
  assert(prompt_calls==2 && restore_calls==2); /* No repeated warning. */
  restore_result=RECOMP_RETURN_PARKED_WAIT;
  assert(ActRaiser_RegionalContinue(&cpu)==RECOMP_RETURN_PARKED_WAIT);
  assert(cpu.PB==3 && prompt_calls==2);
  remove(path); remove(companion);
  return 0;
}
