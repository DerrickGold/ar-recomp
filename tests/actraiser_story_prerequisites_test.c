#include "actraiser/actraiser_story_prerequisites.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t rom[65536],town[65536];
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 address) {
  (void)cpu; assert(bank==3 || bank==0x7f);return (bank==3?rom:town)[address];
}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 address) {
  return cpu_read8(cpu,bank,address)|cpu_read8(cpu,bank,address+1)<<8;
}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 address,uint8 value) {
  (void)cpu;(void)bank;(void)address;(void)value;assert(!"prefix must not write story state");
}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 address,uint16 value) {
  (void)cpu;(void)bank;(void)address;(void)value;assert(!"prefix must not write story state");
}
static void Word(uint8_t *memory,unsigned address,unsigned value) {
  memory[address]=value;memory[address+1]=value>>8;
}
int main(void) {
  Word(rom,0xf543,110);rom[0xf545]=5;
  Word(rom,0xf56c,700);rom[0xf56e]=9;
  for(unsigned n=0;n<27;++n) {
    ArRegionalStoryPolicy policy;unsigned digits=n;
    for(unsigned i=0;i<3;++i){policy.source[i]=(ArRegionalSource)(digits%3);digits/=3;}
    ArRegionalStorySnapshot snapshot;assert(ArRegionalStory_Resolve(&policy,&snapshot));
    assert(snapshot.value[0]==(policy.source[0]==1?88:110));
    assert(snapshot.value[1]==(policy.source[1]==1?400:700));
    assert(snapshot.value[2]==(policy.source[2]==1?0:1));
    ArRegionalSource source;
    const bool grouped=(policy.source[0]==1)==(policy.source[1]==1) &&
        (policy.source[0]==1)==(policy.source[2]==1);
    assert(ArRegionalStory_GroupSource(&policy,&source)==grouped);
    if(grouped)assert((source==1)==(policy.source[0]==1));
    for(unsigned rule=0;rule<2;++rule)for(unsigned flags=0;flags<256;++flags) {
      if(flags&(CPU_P_M|CPU_P_X|CPU_P_D))continue;
      CpuState cpu={.PB=3,.DB=0x7f,.X=rule?0xf56c:0xf543,.Y=0xabcd,.S=0x1ee0,.A=0xbeef,.P=flags};
      cpu_p_to_mirrors(&cpu);Word(town,0x7bfb,rule?4:0);
      ArRegionalStoryRule found=kArRegionalStory_Count;
      assert(ActRaiserStory_ThresholdEntry(&cpu,&found) && found==(ArRegionalStoryRule)rule);
      CpuState expected=cpu;
      expected.A=snapshot.value[rule];expected.P &= ~(CPU_P_N|CPU_P_Z);cpu_p_to_mirrors(&expected);
      assert(ActRaiserStory_LoadThreshold(&cpu,snapshot.value[rule]));
      assert(!memcmp(&cpu,&expected,sizeof(cpu)));
      cpu.DB=1;found=kArRegionalStory_Count;
      assert(!ActRaiserStory_ThresholdEntry(&cpu,&found) && found==kArRegionalStory_Count);
    }
  }
  CpuState cpu={.PB=3,.DB=0x7f,.X=0xf543};
  Word(town,0x7bfb,0);ArRegionalStoryRule found;
  const CpuState before=cpu;
  rom[0xf545]=6;assert(!ActRaiserStory_LoadThreshold(&cpu,88));
  assert(!memcmp(&before,&cpu,sizeof(cpu)));rom[0xf545]=5;
  rom[0xf543]=111;assert(!ActRaiserStory_ThresholdEntry(&cpu,&found));rom[0xf543]=110;
  Word(town,0x7bfb,2);assert(!ActRaiserStory_ThresholdEntry(&cpu,&found));
  Word(town,0x7bfb,0);
  for(unsigned x=0;x<65536;++x) {
    cpu.X=x;
    assert(ActRaiserStory_ThresholdEntry(&cpu,&found)==(x==0xf543));
  }
  for(unsigned m=0;m<2;++m)for(unsigned d=0;d<2;++d)for(unsigned x=0;x<2;++x) {
    cpu.m_flag=m;cpu.D=d;cpu.x_flag=x;
    assert(ActRaiserStory_CompassEntry(&cpu)==(m && !d && !x));
  }
  ArRegionalStoryPolicy invalid={{0,0,3}};
  ArRegionalStorySnapshot sentinel={{1,2,3}},old=sentinel;
  assert(!ArRegionalStory_Resolve(&invalid,&sentinel) && !memcmp(&old,&sentinel,sizeof(old)));
  assert(!ArRegionalStory_Init(&invalid,3) && invalid.source[2]==3);
  assert(!ArRegionalStory_Descriptor(kArRegionalStory_Count));
  puts("story prerequisites: 27 mixed policies, guarded native loads and unchanged story state passed");
  return 0;
}
