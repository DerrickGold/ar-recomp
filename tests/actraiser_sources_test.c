#include "actraiser/actraiser_sources.h"
#include "regional/regional_sources.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[65536];
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu; assert(!bank); return ram[address];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  return cpu_read8(cpu,bank,address) | cpu_read8(cpu,bank,(uint16)(address+1))<<8;
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu; (void)bank; (void)address; (void)value; assert(!"Source prefix must not mutate inventory");
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
  (void)cpu; (void)bank; (void)address; (void)value; assert(!"Source prefix must not mutate inventory");
}
static void Word(unsigned at,unsigned value) { ram[at]=value;ram[at+1]=value>>8; }

int main(void) {
  for(unsigned life=0;life<3;++life)for(unsigned magic=0;magic<3;++magic) {
    ArRegionalSourcesPolicy policy={{life,magic}};
    ArRegionalSourcesSnapshot snapshot;
    assert(ArRegionalSources_Resolve(&policy,&snapshot));
    assert(snapshot.automatic[0]==(life!=1) && snapshot.automatic[1]==(magic!=1));
    ArRegionalSource source=kArRegionalSource_Count;
    const bool grouped=ArRegionalSources_GroupSource(&policy,&source);
    assert(grouped==((life==1)==(magic==1)));
    if(grouped)assert(source==(life==magic?life:0));
  }
  ArRegionalSourcesPolicy invalid={{0,3}}, before=invalid;
  ArRegionalSourcesSnapshot snapshot={{true,true}};
  assert(!ArRegionalSources_Init(&invalid,3) && !memcmp(&invalid,&before,sizeof(before)));
  assert(!ArRegionalSources_Resolve(&invalid,&snapshot) && snapshot.automatic[0] && snapshot.automatic[1]);
  assert(!ArRegionalSources_Descriptor(2));
  unsigned cases=0;
  for(unsigned item=0;item<256;++item)for(unsigned flags=0;flags<256;++flags) {
    if(flags & (CPU_P_M|CPU_P_X|CPU_P_D))continue;
    CpuState cpu={.A=(uint16_t)(0xab00|item),.PB=1,.DB=1,.X=0x024c,.Y=0x024c,.S=0x1f0,.P=(uint8_t)(flags|CPU_P_M)};
    cpu_p_to_mirrors(&cpu);
    assert(ActRaiserSources_CollectionEntry(&cpu)==(item==5 || item==6));
    if(item!=5 && item!=6)continue;
    for(unsigned automatic=0;automatic<2;++automatic) {
      CpuState actual=cpu, expected=cpu;
      expected.P=(flags & (CPU_P_V|CPU_P_I)) | CPU_P_M | CPU_P_C |
          ((automatic || item==5)?CPU_P_Z:0);
      cpu_p_to_mirrors(&expected);
      assert(ActRaiserSources_CollectionRoute(&actual,automatic)==(automatic?0x018922:0x01892d));
      assert(!memcmp(&actual,&expected,sizeof(actual)));
      ++cases;
    }
    for(unsigned mask=0;mask<256;++mask) {
      memset(ram,0,sizeof(ram)); Word(cpu.S+1,0x9c82); Word(cpu.S+4,0x8924);
      for(unsigned i=0;i<8;++i)ram[0x2a2+i]=(mask&(1u<<i))?item:7;
      ram[0x2aa]=item; /* ninth byte is NOT a held slot */
      assert(ActRaiserSources_KeepCarried(&cpu,item)==(mask!=0));
      assert(!ActRaiserSources_KeepCarried(&cpu,item==5?6:5));
      Word(cpu.S+4,0x88ae); /* explicit Use must consume one held item */
      assert(!ActRaiserSources_KeepCarried(&cpu,item));
      Word(cpu.S+4,0x8924); Word(cpu.S+1,0x9c85);
      assert(!ActRaiserSources_KeepCarried(&cpu,item));
    }
  }
  const CpuState valid={.A=5,.PB=1,.DB=1,.m_flag=1};
  for(unsigned kind=0;kind<8;++kind) {
    CpuState cpu=valid;
    switch(kind) {
      case 0: cpu.PB=2;break; case 1: cpu.DB=0;break;
      case 2: cpu.D=1;break; case 3: cpu.emulation=1;break;
      case 4: cpu.m_flag=0;break; case 5: cpu.x_flag=1;break;
      case 6: cpu._flag_D=1;break; case 7: cpu.P=CPU_P_D;break;
    }
    assert(!ActRaiserSources_CollectionEntry(&cpu) && !ActRaiserSources_KeepCarried(&cpu,5));
  }
  printf("Source collection: %u prefix cases; all held-slot masks and native caller guards\n",cases);
  return 0;
}
