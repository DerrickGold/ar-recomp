#include "actraiser/actraiser_town_status_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t low[65536], town[65536], before[65536];
static unsigned writes, activations, native_calls;
static uint32_t tail, origin;
static bool available=true;
static RecompReturn result;
static ArRegionalTownStatusPolicy requested;
static ArRegionalTownStatusSnapshot effective;
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 address) {
  (void)cpu; assert(bank==0 || bank==0x7f); return (bank?town:low)[address];
}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 address) {
  return cpu_read8(cpu,bank,address)|cpu_read8(cpu,bank,(uint16_t)(address+1))<<8;
}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 address,uint8 value) {
  (void)cpu; assert(bank==0x7f); ++writes; town[address]=value;
}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 address,uint16 value) {
  cpu_write8(cpu,bank,address,(uint8)value);cpu_write8(cpu,bank,address+1,(uint8)(value>>8));
}
int cpu_hle_tailcall_request(uint32 pc,uint32 source) { tail=pc;origin=source;return 1; }
bool ActRaiserRegional_TownStatusSnapshot(bool activate,ArRegionalTownStatusSnapshot *out) {
  if (!available) return false;
  if (activate) { ++activations; assert(ArRegionalTownStatus_Resolve(&requested,&effective)); }
  *out=effective;return true;
}
static void Word(uint8_t *memory,unsigned address,unsigned value) { memory[address]=value;memory[address+1]=value>>8; }
static RecompReturn Native(CpuState *cpu) {
  ++native_calls;
  assert(!ActRaiser_RegionalTownStatusCycleEntry(cpu));
  assert(!ActRaiser_RegionalTownStatusReportEntry(cpu));
  cpu->m_flag=0;cpu->DB=0x7f;cpu->X=0;
  /* Editing while the native body is suspended must not alter its snapshot. */
  assert(ArRegionalTownStatus_Init(&requested,effective.japanese[0]?0:1));
  assert(ActRaiser_RegionalTownStatusLowEntry(cpu)==!!effective.japanese[1]);
  assert(ActRaiser_RegionalTownStatusFoodEntry(cpu)==!!effective.japanese[3]);
  if (effective.japanese[0]) {
    assert(ActRaiser_RegionalTownStatusClassifier(cpu)==RECOMP_RETURN_TAILCALL);
    assert(tail==0x03c022 && origin==0x03bf9e);
    assert(ActRaiser_RegionalTownStatusLow(cpu)==RECOMP_RETURN_TAILCALL);
    assert(tail==0x03856b && origin==0x038566);
    assert(ActRaiser_RegionalTownStatusReportLow(cpu)==RECOMP_RETURN_TAILCALL);
    assert(tail==0x03bfa7 && origin==0x03bfa2);
    assert(ActRaiser_RegionalTownStatusPlotCount(cpu)==RECOMP_RETURN_TAILCALL);
    assert(tail==0x0385a7 && origin==0x0385a3);
    CpuState saved=*cpu;
    assert(ActRaiser_RegionalTownStatusMerge(cpu)==RECOMP_RETURN_TAILCALL);
    assert(tail==0x0385c6 && origin==0x0385c3 && !memcmp(cpu,&saved,sizeof(saved)));
    assert(ActRaiser_RegionalTownStatusFood(cpu)==RECOMP_RETURN_TAILCALL);
    assert(tail==0x039274 && origin==0x039271);
  }
  return result;
}
RecompReturn bank_03_82DB_M0X0(CpuState *cpu) { return Native(cpu); }
RecompReturn bank_03_91AE_M0X0(CpuState *cpu) { return Native(cpu); }
RecompReturn bank_03_91BC_M0X0(CpuState *cpu) { return Native(cpu); }
RecompReturn bank_03_BF8C_M0X0(CpuState *cpu) { assert(!cpu->m_flag);return Native(cpu); }
RecompReturn bank_03_BF8C_M1X0(CpuState *cpu) { assert(cpu->m_flag);return Native(cpu); }
static void CheckAdapter(void) {
  const unsigned us[6]={28,29,39,25,15,20};
  CpuState cpu={.PB=3,.DB=0x7f,.S=0x1ef0,.Y=0xabcd};
  for (unsigned t=0;t<6;++t) for (unsigned flags=0;flags<65536;++flags) {
    const unsigned code=flags&1?5:flags&2?1:flags&0x54?2:flags&0x80?1:3;
    assert(ArRegionalTownStatus_JapaneseCode(0,0,flags)==0);
    assert(ArRegionalTownStatus_JapaneseCode(1,1,flags)==1);
    assert(ArRegionalTownStatus_JapaneseCode(1,0,flags)==code);
    cpu.X=(uint16_t)(2*t);Word(low,0x021c+cpu.X,1);Word(town,0x7cef+cpu.X,0);Word(town,0x91da+cpu.X,flags);
    assert(ActRaiserTownStatus_JapaneseReport(&cpu) && cpu.A==code);
  }
  assert(!writes);
  for(unsigned t=0;t<6;++t)for(unsigned p=0;p<256;++p) {
    if(p&(CPU_P_M|CPU_P_X|CPU_P_D))continue;
    for(unsigned a=0;a<65536;a+=17) {
      cpu.X=(uint16_t)(2*t);cpu.A=(uint16_t)a;cpu.P=p;cpu_p_to_mirrors(&cpu);
      CpuState expected=cpu;const uint16_t difference=(uint16_t)(a-us[t]-1);
      expected.P=(expected.P & ~(CPU_P_N|CPU_P_Z|CPU_P_C))|
          (difference&0x8000?CPU_P_N:0)|(!difference?CPU_P_Z:0)|(a>=us[t]+1?CPU_P_C:0);
      cpu_p_to_mirrors(&expected);
      assert(ActRaiserTownStatus_ComparePlots(&cpu) && !memcmp(&expected,&cpu,sizeof(cpu)));
    }
  }
  for(unsigned a=0;a<65536;++a) {
    cpu.X=0;cpu.A=0xbeef;cpu.P=CPU_P_C|CPU_P_V;cpu_p_to_mirrors(&cpu);
    Word(town,0x7c19,a);memcpy(before,town,sizeof(town));writes=0;
    assert(ActRaiserTownStatus_FoodAttempt(&cpu));
    Word(before,0x7c3d,1);
    assert(writes==2 && !memcmp(town,before,sizeof(town)) && cpu.A==a);
    assert(cpu._flag_Z==!a && !!cpu._flag_N==!!(a&0x8000));
    assert(cpu._flag_C && cpu._flag_V);
    assert(ActRaiserTownStatus_FixedThreshold(&cpu) && cpu.A==4 && !cpu._flag_Z && !cpu._flag_N);
  }
  for(unsigned x=0;x<65536;++x) { cpu.X=x;assert(ActRaiserTownStatus_Entry(&cpu,true)==(x<12 && !(x&1))); }
  cpu.X=0;cpu.DB=1;assert(!ActRaiserTownStatus_Entry(&cpu,false));cpu.DB=0x7f;
  cpu.D=1;assert(!ActRaiserTownStatus_Entry(&cpu,false));cpu.D=0;
  cpu.emulation=1;assert(!ActRaiserTownStatus_Entry(&cpu,false));cpu.emulation=0;
  cpu.x_flag=1;assert(!ActRaiserTownStatus_Entry(&cpu,false));cpu.x_flag=0;
  cpu.m_flag=1;assert(!ActRaiserTownStatus_Entry(&cpu,false));cpu.m_flag=0;
  cpu._flag_D=1;assert(!ActRaiserTownStatus_Entry(&cpu,false));cpu._flag_D=0;
  cpu.P=CPU_P_D;assert(!ActRaiserTownStatus_Entry(&cpu,false));
}
int main(void) {
  for (unsigned n=0;n<243;++n) {
    ArRegionalTownStatusPolicy policy;unsigned digits=n;bool grouped=true;
    for(unsigned i=0;i<5;++i) { policy.source[i]=(ArRegionalSource)(digits%3);digits/=3; }
    ArRegionalTownStatusSnapshot snapshot;assert(ArRegionalTownStatus_Resolve(&policy,&snapshot));
    for(unsigned i=0;i<5;++i) { assert(snapshot.japanese[i]==(policy.source[i]==1));grouped &= snapshot.japanese[i]==snapshot.japanese[0]; }
    ArRegionalSource source;assert(ArRegionalTownStatus_GroupSource(&policy,&source)==grouped);
    if(grouped) assert((source==1)==!!snapshot.japanese[0]);
  }
  CheckAdapter();
  RecompReturn (*wrappers[])(CpuState *)={ActRaiser_RegionalTownStatusCycle,ActRaiser_RegionalTownStatusPlot,
      ActRaiser_RegionalTownStatusVisiblePlot,ActRaiser_RegionalTownStatusReport};
  const RecompReturn tokens[]={RECOMP_RETURN_NORMAL,RECOMP_RETURN_SKIP_1,RECOMP_RETURN_SKIP_2,
      RECOMP_RETURN_SKIP_3,RECOMP_RETURN_TAILCALL,RECOMP_RETURN_PARKED_WAIT,RECOMP_RETURN_OWNED_UNWIND};
  for(unsigned w=0;w<4;++w)for(unsigned source=0;source<3;++source)for(unsigned token=0;token<sizeof(tokens)/sizeof(tokens[0]);++token)for(unsigned m=0;m<2;++m) {
    if(m && w!=3)continue;
    ActRaiserTownStatusRuntime_Reset();ArRegionalTownStatus_Init(&requested,(ArRegionalSource)source);
    CpuState cpu={.PB=3,.DB=w==3?1:0x7f,.m_flag=m};
    assert(w==3?ActRaiser_RegionalTownStatusReportEntry(&cpu):ActRaiser_RegionalTownStatusCycleEntry(&cpu));
    result=tokens[token];activations=native_calls=0;
    assert(wrappers[w](&cpu)==result && activations==1 && native_calls==1);
    assert(!ActRaiser_RegionalTownStatusLowEntry(&cpu) && !ActRaiser_RegionalTownStatusFoodEntry(&cpu));
    assert(ActRaiser_RegionalTownStatusCycleEntry(&cpu));
  }
  available=false;CpuState cpu={.PB=3,.DB=0x7f};
  assert(!ActRaiser_RegionalTownStatusCycleEntry(&cpu) && !ActRaiser_RegionalTownStatusReportEntry(&cpu));
  puts("town status: regional truth tables, native prefix ABI and transaction ownership passed");
  return 0;
}
