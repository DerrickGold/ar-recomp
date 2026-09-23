#include "actraiser/actraiser_construction_runtime.h"
#include "actraiser/actraiser_town_status_runtime.h"
#include "regional/regional_construction.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool available=true, requested, effective;
static ArRegionalTownStatusPolicy status_requested;
static ArRegionalTownStatusSnapshot status_effective;
static unsigned activations, reports, native_calls;
static uint32_t target, origin;
static RecompReturn token;
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 address) { (void)cpu;(void)bank;(void)address;assert(0);return 0; }
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 address) { (void)cpu;(void)bank;(void)address;assert(0);return 0; }
void cpu_write8(CpuState *cpu,uint8 bank,uint16 address,uint8 value) { (void)cpu;(void)bank;(void)address;(void)value;assert(0); }
void cpu_write16(CpuState *cpu,uint8 bank,uint16 address,uint16 value) { (void)cpu;(void)bank;(void)address;(void)value;assert(0); }
int cpu_hle_tailcall_request(uint32 pc,uint32 source) { target=pc;origin=source;return 1; }
bool ActRaiserRegional_ConstructionSnapshot(bool activate,bool *out) {
  if (!available) return false;
  if (activate) { ++activations;effective=requested; }
  *out=effective;return true;
}
bool ActRaiserRegional_TownStatusSnapshot(bool activate,ArRegionalTownStatusSnapshot *out) {
  if (!available) return false;
  if (activate) { ++reports;assert(ArRegionalTownStatus_Resolve(&status_requested,&status_effective)); }
  *out=status_effective;return true;
}
static RecompReturn Native(CpuState *cpu) {
  ++native_calls;
  assert(!ActRaiser_RegionalConstructionEntry(cpu));
  assert(!ActRaiser_RegionalTownStatusCycleEntry(cpu));
  requested=!effective;
  assert(ArRegionalTownStatus_Init(&status_requested,!status_effective.japanese[0]));
  for(unsigned town=0;town<6;++town)for(unsigned p=0;p<256;++p) {
    if(p&(CPU_P_M|CPU_P_X|CPU_P_D)) continue;
    cpu->X=2*town;cpu->A=0xbeef;cpu->P=p;cpu_p_to_mirrors(cpu);
    assert(ActRaiser_RegionalConstructionPriceEntry(cpu)==effective);
    assert(ActRaiser_RegionalTownStatusLowEntry(cpu)==!!status_effective.japanese[1]);
    if (!effective) continue;
    RecompReturn (*prefixes[])(CpuState *)={ActRaiser_RegionalConstructionBudget,
        ActRaiser_RegionalConstructionPayment,ActRaiser_RegionalConstructionReturn};
    const uint32_t origins[]={0x03853b,0x038425,0x03848e},targets[]={0x038544,0x03842e,0x038497};
    for(unsigned i=0;i<3;++i) {
      CpuState before=*cpu,expected=*cpu;expected.A=4;expected.P&=~(CPU_P_N|CPU_P_Z);
      expected._flag_N=expected._flag_Z=0;
      assert(prefixes[i](cpu)==RECOMP_RETURN_TAILCALL && target==targets[i] && origin==origins[i]);
      assert(!memcmp(cpu,&expected,sizeof(expected)));*cpu=before;
    }
  }
  return token;
}
RecompReturn bank_03_82DB_M0X0(CpuState *cpu) { return Native(cpu); }
RecompReturn bank_03_84B9_M0X0(CpuState *cpu) { return Native(cpu); }
RecompReturn bank_03_91AE_M0X0(CpuState *cpu) { (void)cpu;assert(0);return 0; }
RecompReturn bank_03_91BC_M0X0(CpuState *cpu) { (void)cpu;assert(0);return 0; }
RecompReturn bank_03_BF8C_M0X0(CpuState *cpu) { (void)cpu;assert(0);return 0; }
RecompReturn bank_03_BF8C_M1X0(CpuState *cpu) { (void)cpu;assert(0);return 0; }
int main(void) {
  for(unsigned source=0;source<3;++source)for(unsigned civ=1;civ<=3;++civ) {
    bool jp;uint16_t price;
    assert(ArRegionalConstruction_Resolve(source,&jp) && jp==(source==1));
    assert(ArRegionalConstruction_Price(jp,civ,&price) && price==(jp?4:2*civ+2));
  }
  bool jp=false;uint16_t price=0xbeef;
  assert(!ArRegionalConstruction_Resolve(3,&jp) && !jp);
  assert(!ArRegionalConstruction_Resolve(0,NULL));
  assert(!ArRegionalConstruction_Price(false,0,&price) && price==0xbeef);
  assert(!ArRegionalConstruction_Price(true,4,&price) && price==0xbeef);
  assert(!ArRegionalConstruction_Price(false,1,NULL));
  const RecompReturn tokens[]={RECOMP_RETURN_NORMAL,RECOMP_RETURN_SKIP_1,RECOMP_RETURN_SKIP_2,
      RECOMP_RETURN_SKIP_3,RECOMP_RETURN_TAILCALL,RECOMP_RETURN_PARKED_WAIT,RECOMP_RETURN_OWNED_UNWIND};
  for(unsigned offscreen=0;offscreen<2;++offscreen)for(unsigned cost=0;cost<2;++cost)
  for(unsigned report=0;report<2;++report)for(unsigned t=0;t<sizeof(tokens)/sizeof(tokens[0]);++t) {
    ActRaiserConstructionRuntime_Reset();ActRaiserTownStatusRuntime_Reset();
    requested=cost;assert(ArRegionalTownStatus_Init(&status_requested,report));
    CpuState cpu={.PB=3,.DB=0x7f,.S=0x1ef0,.Y=0x9876};
    assert(ActRaiser_RegionalConstructionEntry(&cpu) && !ActRaiser_RegionalConstructionPriceEntry(&cpu));
    activations=reports=native_calls=0;token=tokens[t];
    assert((offscreen?ActRaiser_RegionalOffscreenConstruction(&cpu):ActRaiser_RegionalConstruction(&cpu))==token);
    assert(activations==1 && reports==1 && native_calls==1 && effective==!!cost);
    assert(!ActRaiser_RegionalConstructionPriceEntry(&cpu) && !ActRaiser_RegionalTownStatusLowEntry(&cpu));
    assert(ActRaiser_RegionalConstructionEntry(&cpu));
  }
  CpuState cpu={.PB=3,.DB=0x7f};available=false;assert(!ActRaiser_RegionalConstructionEntry(&cpu));available=true;
  cpu.DB=0;assert(!ActRaiser_RegionalConstructionEntry(&cpu));cpu.DB=0x7f;
  cpu.PB=2;assert(!ActRaiser_RegionalConstructionEntry(&cpu));cpu.PB=3;
  cpu.m_flag=1;assert(!ActRaiser_RegionalConstructionEntry(&cpu));cpu.m_flag=0;
  cpu.x_flag=1;assert(!ActRaiser_RegionalConstructionEntry(&cpu));cpu.x_flag=0;
  cpu.D=1;assert(!ActRaiser_RegionalConstructionEntry(&cpu));cpu.D=0;
  cpu.emulation=1;assert(!ActRaiser_RegionalConstructionEntry(&cpu));cpu.emulation=0;
  cpu._flag_D=1;assert(!ActRaiser_RegionalConstructionEntry(&cpu));
  puts("construction: price tables, independent pinned transactions, prefix flags/frames and exit tokens passed");
  return 0;
}
