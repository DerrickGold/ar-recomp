#include "actraiser/actraiser_level_goals_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t memory[65536],before[65536];
static unsigned writes,captures,calls;
static bool available=true,effective;
static ArRegionalSource requested;
static RecompReturn token;
static uint32_t target,origin;
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 address) { (void)cpu;assert(!bank);return memory[address]; }
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 address) { return cpu_read8(cpu,bank,address)|cpu_read8(cpu,bank,address+1)<<8; }
void cpu_write8(CpuState *cpu,uint8 bank,uint16 address,uint8 value) { (void)cpu;assert(!bank);++writes;memory[address]=value; }
void cpu_write16(CpuState *cpu,uint8 bank,uint16 address,uint16 value) {
  cpu_write8(cpu,bank,address,(uint8)value);cpu_write8(cpu,bank,address+1,(uint8)(value>>8));
}
int cpu_hle_tailcall_request(uint32 pc,uint32 source) { target=pc;origin=source;return 1; }
bool ActRaiserRegional_LevelGoalsSnapshot(bool activate,bool *japanese) {
  if (!available) return false;
  if (activate) { ++captures;assert(ArRegionalLevelGoals_Resolve(requested,&effective)); }
  *japanese=effective;return true;
}
static void Word(unsigned address,unsigned value) { memory[address]=value;memory[address+1]=value>>8; }
static RecompReturn Native(CpuState *cpu) {
  ++calls;assert(!ActRaiser_RegionalLevelLeafEntry(cpu) && !ActRaiser_RegionalLevelAwardEntry(cpu));
  requested=effective?0:1;cpu->m_flag=0;cpu->X=10;
  assert(ActRaiser_RegionalLevelPrefixEntry(cpu)==effective);
  if (effective) {
    assert(ActRaiser_RegionalLevelCompare(cpu)==RECOMP_RETURN_TAILCALL && target==0x03b3cb && origin==0x03b3c7);
    assert(ActRaiser_RegionalLevelLoad(cpu)==RECOMP_RETURN_TAILCALL && target==0x03b3d1 && origin==0x03b3cd && cpu->A==650);
    assert(ActRaiser_RegionalLevelNext(cpu)==RECOMP_RETURN_TAILCALL && target==0x03b40b && origin==0x03b407 && cpu->A==650);
  }
  Word(0x291,5);Word(0x297,0xffff);const unsigned captured=captures;
  ActRaiserLevelGoalsRuntime_RefreshReport(cpu);
  assert(captures==captured && cpu_read16(cpu,0,0x297)==(effective?650:0xffff));
  return token;
}
RecompReturn bank_03_E414_M0X0(CpuState *cpu) { assert(!cpu->m_flag);return Native(cpu); }
RecompReturn bank_03_B3BA_M0X0(CpuState *cpu) { assert(!cpu->m_flag);return Native(cpu); }
RecompReturn bank_03_B3BA_M1X0(CpuState *cpu) { assert(cpu->m_flag);return Native(cpu); }
int main(void) {
  const uint16_t thresholds[2][18]={
      {0,80,200,400,700,950,1200,1500,1700,1900,2200,2500,2900,3300,3700,4100,4600,9999},
      {0,80,200,400,550,650,750,1050,1400,1600,1800,1900,2000,2200,2400,2600,3000,9999}};
  for(unsigned source=0;source<3;++source)for(unsigned level=0;level<18;++level) {
    bool jp;assert(ArRegionalLevelGoals_Resolve((ArRegionalSource)source,&jp) && jp==(source==1));
    uint16_t value;assert(ArRegionalLevelGoals_Threshold(jp,level,&value) && value==thresholds[jp][level]);
    assert(ArRegionalLevelGoals_Display(jp,level,&value) && value==(level==17?0:thresholds[jp][level]));
    for(unsigned p=0;p<256;++p) {
      if(p&(CPU_P_M|CPU_P_X|CPU_P_D))continue;
      const uint16_t threshold=thresholds[jp][level];
      const uint16_t populations[]={0,(uint16_t)(threshold-1),threshold,(uint16_t)(threshold+1),9999,65535};
      for(unsigned i=0;i<sizeof(populations)/sizeof(populations[0]);++i) {
        CpuState cpu={.PB=3,.DB=0x7f,.S=0x1ed0,.X=2*level,.Y=0xabcd,.A=populations[i],.P=p};cpu_p_to_mirrors(&cpu);
        CpuState expected=cpu;const uint16_t diff=(uint16_t)(cpu.A-threshold);
        expected.P=(expected.P & ~(CPU_P_N|CPU_P_Z|CPU_P_C))|
            (diff&0x8000?CPU_P_N:0)|(!diff?CPU_P_Z:0)|(cpu.A>=threshold?CPU_P_C:0);cpu_p_to_mirrors(&expected);
        assert(ActRaiserLevelGoals_Compare(&cpu,jp) && !memcmp(&expected,&cpu,sizeof(cpu)));
        expected.A=threshold;expected.P=(expected.P & ~(CPU_P_N|CPU_P_Z))|(!threshold?CPU_P_Z:0);cpu_p_to_mirrors(&expected);
        assert(ActRaiserLevelGoals_Load(&cpu,jp) && !memcmp(&expected,&cpu,sizeof(cpu)));
      }
    }
  }
  assert(!writes);
  CpuState cpu={.PB=3};
  for(unsigned x=0;x<65536;++x) {cpu.X=x;assert(ActRaiserLevelGoals_PrefixEntry(&cpu)==(x<36 && !(x&1)));}
  cpu.X=0;cpu.m_flag=1;assert(!ActRaiserLevelGoals_PrefixEntry(&cpu));cpu.m_flag=0;
  cpu.x_flag=1;assert(!ActRaiserLevelGoals_PrefixEntry(&cpu));cpu.x_flag=0;
  cpu.D=1;assert(!ActRaiserLevelGoals_PrefixEntry(&cpu));cpu.D=0;
  cpu.emulation=1;assert(!ActRaiserLevelGoals_PrefixEntry(&cpu));cpu.emulation=0;
  cpu.P=CPU_P_D;assert(!ActRaiserLevelGoals_PrefixEntry(&cpu));cpu.P=0;
  cpu._flag_D=1;assert(!ActRaiserLevelGoals_PrefixEntry(&cpu));cpu._flag_D=0;
  for(unsigned old=0;old<2;++old)for(unsigned next=0;next<3;++next)for(unsigned level=0;level<19;++level) {
    ActRaiserLevelGoalsRuntime_Reset();effective=old;requested=(ArRegionalSource)next;
    memset(memory,0xa7,sizeof(memory));Word(0x291,level);Word(0x297,12345);memcpy(before,memory,sizeof(memory));
    writes=captures=0;const CpuState original=cpu;ActRaiserLevelGoalsRuntime_RefreshReport(&cpu);
    assert(captures==1 && !memcmp(&cpu,&original,sizeof(cpu)));
    if ((old || next==1) && level<18) {
      uint16_t display;assert(ArRegionalLevelGoals_Display(next==1,level,&display));
      before[0x297]=display;before[0x298]=display>>8;assert(writes==2);
    } else assert(!writes);
    assert(!memcmp(before,memory,sizeof(memory))); /* no rewards, HP/SP or level writes */
  }
  const RecompReturn results[]={RECOMP_RETURN_NORMAL,RECOMP_RETURN_SKIP_1,RECOMP_RETURN_SKIP_2,
      RECOMP_RETURN_SKIP_3,RECOMP_RETURN_TAILCALL,RECOMP_RETURN_PARKED_WAIT,RECOMP_RETURN_OWNED_UNWIND};
  for(unsigned owner=0;owner<3;++owner)for(unsigned source=0;source<3;++source)for(unsigned i=0;i<sizeof(results)/sizeof(results[0]);++i) {
    ActRaiserLevelGoalsRuntime_Reset();requested=(ArRegionalSource)source;token=results[i];captures=calls=0;
    cpu=(CpuState){.PB=3,.DB=0x7f,.m_flag=owner==2};
    assert(ActRaiser_RegionalLevelLeafEntry(&cpu));
    assert(ActRaiser_RegionalLevelAwardEntry(&cpu)==!cpu.m_flag);
    assert((owner?ActRaiser_RegionalLevelLeaf(&cpu):ActRaiser_RegionalLevelAward(&cpu))==token);
    assert(captures==1 && calls==1 && !ActRaiser_RegionalLevelPrefixEntry(&cpu));
    assert(ActRaiser_RegionalLevelLeafEntry(&cpu));
  }
  available=false;assert(!ActRaiser_RegionalLevelLeafEntry(&cpu));
  uint16_t untouched=0xdead;bool japanese=true;
  assert(!ArRegionalLevelGoals_Threshold(false,18,&untouched) && untouched==0xdead);
  assert(!ArRegionalLevelGoals_Resolve(3,&japanese) && japanese);
  puts("level goals: tables, native CMP/load flags, report-only refresh and pinned awards passed");
  return 0;
}
