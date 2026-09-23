#include "actraiser/actraiser_lair_history.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t native[65536], town[65536];
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at) {
  (void)cpu; assert(bank==0 || bank==0x7f); return (bank ? town:native)[at];
}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at) {
  return cpu_read8(cpu,bank,at) | cpu_read8(cpu,bank,(uint16)(at+1))<<8;
}
static void Write(uint8_t *memory,unsigned at,unsigned value) { memory[at]=value; memory[at+1]=value>>8; }
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value) {
  (void)cpu; assert(bank==0x7f && at>=0x96b8 && at<0x96e8 && !(at&1));
  Write(town,at,value);
}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value) {
  (void)cpu; (void)bank; (void)at; (void)value;
  assert(!"house-unit prefix must not write memory");
}
static void HouseUnits(void) {
  for(unsigned tiered=0;tiered<2;++tiered)for(unsigned a=0;a<65536;++a)
  for(unsigned flags=0;flags<4;++flags) {
    CpuState cpu={.PB=3,.DB=0x7f,.A=a,.X=0x7010,.Y=0xabcd,.S=0x1ff0,
        .P=CPU_P_N|CPU_P_Z|CPU_P_I,._flag_N=1,._flag_Z=1};
    if(flags&1){cpu.P|=CPU_P_C;cpu._flag_C=1;}
    if(flags&2){cpu.P|=CPU_P_V;cpu._flag_V=1;}
    CpuState expected=cpu;
    expected.A=expected.Y=tiered?(a&255):4;
    expected._flag_N=0;expected._flag_Z=!expected.A;
    expected.P=(uint8_t)((expected.P&~(CPU_P_N|CPU_P_Z)) | (expected._flag_Z?CPU_P_Z:0));
    assert(ActRaiserLairHouse_Units(&cpu,tiered));
    assert(!memcmp(&cpu,&expected,sizeof(cpu)));
  }
  CpuState cpu={.PB=3,.m_flag=1,.A=8}; const CpuState before=cpu;
  assert(!ActRaiserLairHouse_Units(&cpu,false) && !memcmp(&cpu,&before,sizeof(cpu)));
  for(unsigned source=0;source<3;++source)for(unsigned subtype=0;subtype<256;++subtype) {
    uint16_t units=99;
    assert(ArRegionalHouseCredit_Units((ArRegionalSource)source,(uint8_t)subtype,&units));
    assert(units==(source==1?4:4+((subtype&0x30)>>3)));
  }
  uint16_t units=99;
  assert(!ArRegionalHouseCredit_Units(kArRegionalSource_Count,0,&units) && units==99);
}
static unsigned Word(const uint8_t *memory,unsigned at) { return memory[at] | memory[at+1]<<8; }
static void Setup(ArRegionalLairHistory *history) {
  memset(native,0,sizeof(native)); memset(town,0,sizeof(town)); memset(history,0,sizeof(*history));
  for(unsigned i=0;i<24;++i) { uint16_t seed; assert(ArRegionalLair_Seed(0,i,&seed)); Write(town,0x96b8+2*i,seed); }
  CpuState cpu={0}; assert(ActRaiserLairHistory_Initialize(history,&cpu));
}
static void NativeHouse(unsigned region,unsigned mask,unsigned subtype,unsigned source) {
  if(mask==15)return;
  unsigned left=source==kArRegionalSource_Japan ? 4 : 4+((subtype&0x30)>>3);
  for(unsigned n=0;left;n=(n+1)%4)if(!(mask&(1u<<n))) {
    const unsigned at=0x96b8+region*8+n*2;
    Write(town,at,(uint16_t)(Word(town,at)+1)); --left;
  }
}
static void Houses(void) {
  for(unsigned source=0;source<3;++source) {
  const ArRegionalLairAccounting policy={.house_credit=(ArRegionalSource)source};
  for(unsigned region=0;region<6;++region)for(unsigned mask=0;mask<16;++mask)
  for(unsigned subtype=0;subtype<4;++subtype)for(unsigned m=0;m<2;++m) {
    ArRegionalLairHistory history; Setup(&history);
    CpuState cpu={.PB=3,.DB=0x7f,.m_flag=m,.A=0xbeef,.X=0x6be7,.Y=0xaaa,.S=0x1f00};
    const CpuState before=cpu;
    Write(town,0x7bfb,region*2); town[cpu.X+2]=subtype<<4;
    for(unsigned n=0;n<4;++n)Write(town,0x95c8+region*8+n*2,mask&(1u<<n)?0x8000:0);
    ActRaiserLairCapture capture;
    assert(ActRaiserLairHistory_Begin(&history,&cpu,kActRaiserLairEvent_House,&policy,&capture));
    assert(capture.town==region && capture.sealed_mask==mask && capture.subtype==subtype*16);
    NativeHouse(region,mask,subtype<<4,source);
    assert(ActRaiserLairHistory_End(&history,&cpu,&capture,RECOMP_RETURN_NORMAL));
    assert(!memcmp(&cpu,&before,sizeof(cpu)) && !history.diverged_towns);
    assert(ActRaiserLairHistory_Check(&history,&cpu,&policy));
  }
  }
}
static void Miracles(void) {
  const ArRegionalLairAccounting policy={0};
  for(unsigned flag_mask=0;flag_mask<256;++flag_mask)for(unsigned kind=0;kind<5;++kind)
  for(unsigned busy=0;busy<2;++busy) {
    ArRegionalLairHistory history={0}; CpuState cpu={.PB=3};
    memset(native,0,sizeof(native)); memset(town,0,sizeof(town));
    const uint16_t stocks[]={0,9,10,65535};
    assert(ArRegionalLairHistory_AdoptTown(&history,0,0,stocks));
    unsigned candidates=0;
    for(unsigned n=0;n<4;++n) {
      const unsigned flags=((flag_mask>>(n*2))&3)<<14;
      Write(town,0x95c8+n*2,flags); Write(town,0x96b8+n*2,stocks[n]);
      if(!flags && !busy && kind!=2 && kind!=3)candidates|=1u<<n;
    }
    Write(town,0x90eb,kind); Write(town,0x90f5,busy);
    ActRaiserLairCapture capture;
    assert(ActRaiserLairHistory_Begin(&history,&cpu,kActRaiserLairEvent_Miracle,&policy,&capture));
    assert(capture.candidates==candidates);
    for(unsigned n=0;n<4;++n)if((candidates&(1u<<n)) && stocks[n]) {
      unsigned value=(stocks[n]+65536-10)%65536;
      Write(town,0x96b8+n*2,value>=32768?0:value);
    }
    assert(ActRaiserLairHistory_End(&history,&cpu,&capture,RECOMP_RETURN_NORMAL));
    if(candidates&1)assert(history.stock[1][0]==40); /* Active zero must not hide JP debit. */
  }
}
static void KillAndScore(void) {
  ArRegionalLairHistory history={0}; const ArRegionalLairAccounting policy={0};
  CpuState cpu={.PB=3,.X=0xb30}; ActRaiserLairCapture capture;
  memset(native,0,sizeof(native)); memset(town,0,sizeof(town));
  assert(ArRegionalLairHistory_AdoptTown(&history,0,0,(uint16_t[4]){0,1,2,3}));
  for(unsigned n=0;n<4;++n) { Write(town,0x96b8+n*2,n); Write(town,0x9688+n*2,0xb30); }
  Write(town,0x95c8,0x8000);
  assert(ActRaiserLairHistory_Begin(&history,&cpu,kActRaiserLairEvent_Kill,&policy,&capture));
  assert(capture.candidates==1);
  assert(ActRaiserLairHistory_End(&history,&cpu,&capture,RECOMP_RETURN_NORMAL));
  assert(history.stock[0][0]==0 && history.stock[1][0]==49 && history.stock[0][1]==1);
  Setup(&history);
  native[0x341]=3; Write(town,0x7bfb,0); Write(town,0x6b1c,1); Write(native,0x1f,0x1000);
  assert(ActRaiserLairHistory_Begin(&history,&cpu,kActRaiserLairEvent_Score,&policy,&capture));
  assert(capture.town==2 && capture.completed_acts==1 && capture.bcd_score==0x1000);
  for(unsigned n=8;n<12;++n)Write(town,0x96b8+n*2,Word(town,0x96b8+n*2)+50);
  assert(ActRaiserLairHistory_End(&history,&cpu,&capture,RECOMP_RETURN_NORMAL));
  assert(history.stock[0][8]==160 && history.stock[31][8]==195);
}
static void Divergence(void) {
  ArRegionalLairHistory history; Setup(&history);
  const ArRegionalLairAccounting policy={0}; CpuState cpu={.PB=3};
  ActRaiserLairCapture capture={.town=999}, sentinel=capture;
  Write(town,0x96b8,1234);
  assert(!ActRaiserLairHistory_Begin(&history,&cpu,kActRaiserLairEvent_Kill,&policy,&capture));
  assert(history.diverged_towns==1 && history.stock[0][0]==200);
  assert(!memcmp(&capture,&sentinel,sizeof(capture)));
  assert(!ActRaiserLairHistory_Initialize(&history,&cpu));
  Setup(&history); Write(town,0x9688,0); /* Match first lair */
  assert(ActRaiserLairHistory_Begin(&history,&cpu,kActRaiserLairEvent_Kill,&policy,&capture));
  assert(!ActRaiserLairHistory_End(&history,&cpu,&capture,RECOMP_RETURN_NORMAL)); /* Native didn't debit */
  assert(history.diverged_towns==1 && history.stock[0][0]==200);
  Setup(&history);
  assert(ActRaiserLairHistory_Begin(&history,&cpu,kActRaiserLairEvent_Kill,&policy,&capture));
  assert(!ActRaiserLairHistory_End(&history,&cpu,&capture,RECOMP_RETURN_PARKED_WAIT));
  assert(history.diverged_towns==1 && history.stock[0][0]==200);
  Setup(&history); Write(town,0x96b8+5*8,1234);
  assert(!ActRaiserLairHistory_Check(&history,&cpu,&policy) && history.diverged_towns==32);
  assert(history.stock[0][20]==30 && Word(town,0x96b8+5*8)==1234);
  memset(&history,0,sizeof(history));
  assert(!ActRaiserLairHistory_Begin(&history,&cpu,kActRaiserLairEvent_Kill,&policy,&capture));
  assert(!history.initialized_towns && !history.diverged_towns);
  assert(ActRaiserLairHistory_Entry(&cpu)); cpu.PB=1; assert(!ActRaiserLairHistory_Entry(&cpu)); cpu.PB=3;
  cpu.D=1; assert(!ActRaiserLairHistory_Entry(&cpu)); cpu.D=0;
  cpu.x_flag=1; assert(!ActRaiserLairHistory_Entry(&cpu)); cpu.x_flag=0;
  cpu.emulation=1; assert(!ActRaiserLairHistory_Entry(&cpu)); cpu.emulation=0;
  cpu.P=CPU_P_D; assert(!ActRaiserLairHistory_Entry(&cpu));
}
static void Projection(void) {
  ArRegionalLairHistory h; Setup(&h);
  const ArRegionalLairAccounting us={0}, jp={.seeds=kArRegionalSource_Japan};
  CpuState cpu={.PB=3,.DB=0x7f,.S=0x1e00,.A=0x5555,.X=0x1234,.Y=0x4321};
  const CpuState before=cpu;
  uint8_t original[sizeof(town)], expected[sizeof(town)];
  /* A live actor, sealed flags, countdowns, growth and soul cache are all
   * represented by unrelated bytes that the projection must leave untouched. */
  for(unsigned i=0; i<sizeof(town); ++i) if(i<0x96b8 || i>=0x96e8)town[i]=(uint8_t)(i*37);
  memcpy(original,town,sizeof(town)); memcpy(expected,town,sizeof(town));
  for(unsigned i=0; i<24; ++i)Write(expected,0x96b8+2*i,h.stock[1][i]);
  assert(ActRaiserLairHistory_Project(&h,&cpu,&us,&jp)==kArRegionalLairProjection_Ready);
  assert(!memcmp(town,expected,sizeof(town)) && !memcmp(&cpu,&before,sizeof(cpu)));
  assert(ActRaiserLairHistory_Project(&h,&cpu,&jp,&us)==kArRegionalLairProjection_Ready);
  assert(!memcmp(town,original,sizeof(town)));
  /* Late-table failure cannot partially project earlier towns. */
  Write(town,0x96e6,999);
  memcpy(original,town,sizeof(town));
  assert(ActRaiserLairHistory_Project(&h,&cpu,&us,&jp)==kArRegionalLairProjection_Mismatch);
  assert(h.diverged_towns==32 && !memcmp(town,original,sizeof(town)));
  assert(!memcmp(&cpu,&before,sizeof(cpu)));
}
int main(void) {
  uint8_t image[kActRaiserSramSize] = {0};
  uint16_t stocks[24], before[24];
  for (unsigned i=0; i<24; ++i) Write(image,0x1603+i*2,65535-i*100);
  Save_RecomputeChecksum(image);
  assert(ActRaiserLairHistory_ReadSavedStocks(image,stocks));
  for (unsigned i=0; i<24; ++i) assert(stocks[i]==65535-i*100);
  memcpy(before,stocks,sizeof(stocks)); image[1]^=1;
  assert(!ActRaiserLairHistory_ReadSavedStocks(image,stocks));
  assert(!memcmp(stocks,before,sizeof(stocks)));
  HouseUnits(); Houses(); Miracles(); KillAndScore(); Divergence(); Projection();
  puts("native lair observation: candidate gates, order, no CPU/WRAM mutation and divergence passed");
  return 0;
}
