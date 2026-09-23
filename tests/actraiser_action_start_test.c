#include "actraiser/actraiser_action_start.h"
#include "regional/regional_action_start.h"
#include "byte_order.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t memory[65536];
static ArRegionalActionStartPolicy policy;
static unsigned activations,native_calls;
static RecompReturn native_result;
bool ActRaiserRegional_StartInventory(void) {return true;}
bool ActRaiserRegional_BeginActionStart(ArRegionalActionStartSnapshot *snapshot) {
  ++activations;return ArRegionalActionStart_Resolve(&policy,snapshot);
}
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at) {(void)cpu;assert(!bank);return memory[at];}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at) {return cpu_read8(cpu,bank,at)|(uint16)cpu_read8(cpu,bank,at+1)<<8;}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value) {(void)cpu;assert(!bank);memory[at]=value;}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value) {cpu_write8(cpu,bank,at,value);cpu_write8(cpu,bank,at+1,value>>8);}
RecompReturn bank_02_AB05_M1X0(CpuState *cpu) {
  assert(!ActRaiser_ActionStartEntry(cpu));++native_calls;
  if(native_result!=RECOMP_RETURN_NORMAL) {cpu->A=0xbeef;return native_result;}
  /* Native-call boundary sentinel: all fields except the chosen allowances
   * are delegated. Integration fixtures also execute the generated body. */
  memory[0x347]=memory[0xe4]=0;memory[0xfc]=memory[0x349]=1;
  memory[0x1c]=4;memory[0x1e]=memory[0x1d]=24;memory[0xfb]=255;
  ByteOrder_WriteLe16(memory+0x1a,0x0101);memory[0x21]=memory[0x1f]=memory[0x20]=memory[0x2ac]=0;
  cpu->A=(cpu->A&0xff00)|24;cpu->X=0x0101;cpu->S+=3;cpu->PB=0;
  cpu->P&=~(CPU_P_N|CPU_P_Z);cpu_p_to_mirrors(cpu);return RECOMP_RETURN_NORMAL;
}
static void Prefix(void) {
  for(unsigned spares=0;spares<3;++spares)for(unsigned health=0;health<3;++health)
  for(unsigned p=0;p<256;++p)for(unsigned high=0;high<2;++high) {
    if(!(p&CPU_P_M) || p&CPU_P_X)continue;
    policy=(ArRegionalActionStartPolicy){{spares,health}};
    ArRegionalActionStartSnapshot resolved;assert(ArRegionalActionStart_Resolve(&policy,&resolved));
    assert(resolved.spares==(spares==1?2:4) && resolved.health==(health==2?8:24));
    memset(memory,0x5a,sizeof(memory));uint8_t expected[65536];memcpy(expected,memory,sizeof(expected));
    expected[0x1c]=resolved.spares;expected[0x1d]=expected[0x1e]=resolved.health;
    expected[0xfb]=255;ByteOrder_WriteLe16(expected+0x1a,0x0101);
    expected[0x347]=expected[0xe4]=expected[0x21]=expected[0x1f]=expected[0x20]=expected[0x2ac]=0;
    expected[0xfc]=expected[0x349]=1;
    CpuState cpu={.A=(uint16_t)(high?0xa566:0x66),.X=0x9876,.Y=0xabcd,.PB=2,.P=(uint8_t)p,.S=0x1e00};
    cpu_p_to_mirrors(&cpu);const unsigned count=activations;
    assert(ActRaiser_ActionStartEntry(&cpu));
    const unsigned native_before=native_calls;
    assert(ActRaiser_ActionStart(&cpu)==RECOMP_RETURN_NORMAL && native_calls==native_before+1);
    assert(activations==count+1 && cpu.A==((high?0xa500:0)|resolved.health) && cpu.X==0x0101 &&
      cpu.Y==0xabcd && cpu.S==0x1e03 && !cpu.PB && cpu.P==(p&~(CPU_P_N|CPU_P_Z)));
    assert(!memcmp(expected,memory,sizeof(memory)));
    ArRegionalSource source;
    const bool matches=ArRegionalActionStart_GroupSource(&policy,&source);
    assert(matches==!(spares==1 && health==2));
    if(matches) {
      assert(ArRegionalActionStart_Descriptor(0)->value[source]==resolved.spares);
      assert(ArRegionalActionStart_Descriptor(1)->value[source]==resolved.health);
    }
  }
  for(unsigned invalid=0;invalid<6;++invalid) {
    CpuState cpu={.PB=2,.m_flag=1};
    switch(invalid) {
      case 0: cpu.PB=0;break;case 1: cpu.DB=1;break;case 2: cpu.D=1;break;
      case 3: cpu.m_flag=0;break;case 4: cpu.x_flag=1;break;case 5:cpu.emulation=1;break;
    }
    const unsigned count=activations;assert(!ActRaiser_ActionStartEntry(&cpu) && activations==count);
  }
  assert(!ActRaiser_ActionStartEntry(NULL) && !ArRegionalActionStart_Descriptor(2));
  assert(!ArRegionalActionStart_Init(NULL,0) && !ArRegionalActionStart_Init(&policy,3));
  ArRegionalActionStartSnapshot result={7,7};policy.source[0]=3;
  assert(!ArRegionalActionStart_Resolve(&policy,&result) && result.spares==7 && result.health==7);
  const RecompReturn escapes[]={RECOMP_RETURN_TAILCALL,RECOMP_RETURN_PARKED_WAIT,RECOMP_RETURN_OWNED_UNWIND,RECOMP_RETURN_SKIP_1};
  for(unsigned i=0;i<sizeof(escapes)/sizeof(escapes[0]);++i) {
    CpuState cpu={.PB=2,.m_flag=1};native_result=escapes[i];ArRegionalActionStart_Init(&policy,1);
    uint8_t before[65536];memcpy(before,memory,sizeof(before));
    assert(ActRaiser_ActionStart(&cpu)==escapes[i] && cpu.A==0xbeef && !memcmp(before,memory,sizeof(before)));
  }
}
static void Roms(char **paths) {
  uint8_t rom[1048576];const unsigned starts[]={0x12b05,0x1284b,0x12b9e,0x12ba7,0x12b90};
  for(unsigned r=0;r<5;++r) {
    FILE *file=fopen(paths[r],"rb");assert(file && fread(rom,1,sizeof(rom),file)==sizeof(rom) && !fclose(file));
    const unsigned at=starts[r];
    /* Decode the complete initializer only at its independently verified
     * entry. PAL fields differ; these checks do not transplant its RAM map. */
    if(r<2) {
      assert(!memcmp(rom+at,(uint8_t[]){0x9c,r?0x35:0x47,3,0x9c,0xe4,0,0xa9,1,0x8d,r?0xff:0xfc,0,0x8d,r?0x37:0x49,3,0xa9},15));
      assert(rom[at+15]==(r?2:4));
      assert(!memcmp(rom+at+16,(uint8_t[]){0x85,0x1c,0xa9,0xff,0x85,r?0xfe:0xfb,0xa2,1,1,0x86,0x1a,0xa9,24,0x85,0x1e,0x85,0x1d},17));
    } else {
      assert(!memcmp(rom+at,(uint8_t[]){0x9c,0x49,3,0x9c,0xe5,0,0xa9,1,0x8d,0xfd,0,0x8d,0x4b,3,0xa9,4,0x85,0x1c},18));
      assert(!memcmp(rom+at+18,(uint8_t[]){0xa9,8,0x85,0x1e,0x85,0x1d},6));
    }
  }
  puts("five-ROM Action Mode starting allowances verified");
}
int main(int argc,char **argv) {
  assert(argc==1 || argc==6);Prefix();if(argc==6)Roms(argv+1);
  puts("action-run starts: independent rules, exact prefix, guards passed");return 0;
}
