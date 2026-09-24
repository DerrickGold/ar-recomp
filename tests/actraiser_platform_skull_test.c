#include "actraiser/actraiser_platform_skull.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include "regional/action/regional_platform_skull.h"
#include "byte_order.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t memory[65536],snapshot;
static unsigned target,origin;
uint8_t ActRaiserRegional_PlatformSkullSnapshot(void){return snapshot;}
int cpu_hle_tailcall_request(uint32 pc,uint32 from){target=pc;origin=from;return 1;}
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at){(void)cpu;assert(!bank);return memory[at];}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at){return cpu_read8(cpu,bank,at)|(uint16)cpu_read8(cpu,bank,at+1)<<8;}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value){(void)cpu;assert(!bank);memory[at]=value;}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value){cpu_write8(cpu,bank,at,value);cpu_write8(cpu,bank,at+1,value>>8);}
static void Write(unsigned at,uint16_t value){ByteOrder_WriteLe16(memory+at,value);}
static CpuState Setup(unsigned slot) {
  memset(memory,0,sizeof(memory));memory[0x18]=4;
  const unsigned x=0x6a0+64*slot;
  Write(x+0x16,0x4000);memory[x+0x18]=0x7e;Write(x+0x1a,47);Write(x+0x32,0xd382);
  Write(x+0x2e,0x20);Write(x+0x2a,1);
  CpuState cpu={.A=0xffff,.X=x,.Y=0xd382,.S=0x1ef0,.P=CPU_P_V|CPU_P_C};cpu_p_to_mirrors(&cpu);return cpu;
}
static void CheckPolicies(void) {
  for(unsigned n=0;n<81;++n) {
    unsigned digits=n;ArRegionalPlatformSkullPolicy policy;uint8_t expected=0,resolved=255;
    for(unsigned i=0;i<4;++i){policy.source[i]=digits%3;digits/=3;if(policy.source[i]==1)expected|=1u<<i;}
    assert(ArRegionalPlatformSkull_Resolve(&policy,&resolved) && resolved==expected);
    for(unsigned i=0;i<4;++i)assert(ArRegionalPlatformSkull_Value(resolved,i)==ArRegionalPlatformSkull_Descriptor(i)->value[policy.source[i]]);
  }
  assert(ArRegionalPlatformSkull_Value(16,0)==UINT16_MAX);
}
static void CheckBirth(void) {
  for(unsigned bits=0;bits<16;++bits)for(unsigned slot=0;slot<80;++slot)for(unsigned stale=0;stale<3;++stale) {
    CpuState cpu=Setup(slot),reference=cpu;snapshot=bits;
    Write(cpu.X+0x32,stale==0?0:stale==1?0xc961:0xd382);
    uint8_t wanted[sizeof(memory)];memcpy(wanted,memory,sizeof(memory));
    assert(ActRaiser_PlatformSkullSpawnEntry(&cpu)==!!(bits&3));
    if(bits&3) {
      const uint16_t flags=bits&1?0x800:0;
      reference.A=flags;ActRaiserCpuHle_SetNegativeZero16(&reference,flags);
      ByteOrder_WriteLe16(wanted+cpu.X+0x30,flags);
      ByteOrder_WriteLe16(wanted+cpu.X+0x2e,bits&2?0:0x20);
      assert(ActRaiser_PlatformSkullSpawn(&cpu)==RECOMP_RETURN_TAILCALL && target==0x966f && origin==0x966c);
    }
    assert(!memcmp(&cpu,&reference,sizeof(cpu)) && !memcmp(memory,wanted,sizeof(memory)));
  }
}
static void CheckRanges(void) {
  for(unsigned bits=0;bits<16;++bits)for(unsigned axis=0;axis<2;++axis) {
    CpuState cpu=Setup(9);snapshot=bits;
    const bool enabled=(bits&(axis?8:4))!=0;
    assert((axis?ActRaiser_PlatformSkullRangeYEntry(&cpu):ActRaiser_PlatformSkullRangeXEntry(&cpu))==enabled);
    if(!enabled)continue;
    for(unsigned value=0;value<=65535;++value) {
      cpu.A=value;cpu.P=CPU_P_V|CPU_P_I;cpu_p_to_mirrors(&cpu);CpuState expected=cpu;
      const uint16_t result=(uint16_t)(value-24);
      expected.P|=(value>=24?CPU_P_C:0)|(result?0:CPU_P_Z)|(result&0x8000?CPU_P_N:0);
      cpu_p_to_mirrors(&expected);
      assert((axis?ActRaiser_PlatformSkullRangeY(&cpu):ActRaiser_PlatformSkullRangeX(&cpu))==RECOMP_RETURN_TAILCALL);
      assert(origin==(axis?0xd3a2:0xd39a) && target==origin+3 && !memcmp(&cpu,&expected,sizeof(cpu)));
    }
  }
  for(unsigned bad=0;bad<17;++bad) {
    CpuState cpu=Setup(9);snapshot=15;
    switch(bad) {
      case 0:cpu.PB=1;break;case 1:cpu.DB=1;break;case 2:cpu.D=1;break;
      case 3:cpu.m_flag=1;break;case 4:cpu.x_flag=1;break;case 5:cpu.emulation=1;break;
      case 6:cpu._flag_D=1;break;case 7:cpu.P|=CPU_P_D;break;case 8:cpu.X++;break;
      case 9:cpu.X=0x1aa0;break;case 10:memory[0x18]=7;break;
      case 11:Write(cpu.X+0x32,0xc961);cpu.Y=0xc961;break;
      case 12:Write(cpu.X+0x16,0x5000);break;case 13:memory[cpu.X+0x18]=0x7f;break;
      case 14:Write(cpu.X+0x1a,48);break;case 15:Write(cpu.X+0x30,0x400);break;case 16:snapshot=255;break;
    }
    assert(!ActRaiser_PlatformSkullSpawnEntry(&cpu) && !ActRaiser_PlatformSkullRangeXEntry(&cpu) && !ActRaiser_PlatformSkullRangeYEntry(&cpu));
  }
}
static void CheckRoms(char **paths) {
  const unsigned records[]={0xd382,0xd404,0xd049,0xd04b,0xd04e};
  static uint8_t rom[1048576];
  for(unsigned region=0;region<5;++region) {
    FILE *file=fopen(paths[region],"rb");assert(file && fread(rom,1,sizeof(rom),file)==sizeof(rom) && !fclose(file));
    ArRegionalPlatformSkullPolicy policy;assert(ArRegionalPlatformSkull_Init(&policy,region>=2?2:region) && ArRegionalPlatformSkull_Resolve(&policy,&snapshot));
    const uint8_t *record=rom+records[region]-0x8000,*entry=record+12;
    assert(ByteOrder_ReadLe16(record)==0x4000 && record[2]==0x7e && record[6]==47);
    assert(ByteOrder_ReadLe16(record+4)==(ArRegionalPlatformSkull_Value(snapshot,0)?0x800:0));
    assert(record[9]==ArRegionalPlatformSkull_Value(snapshot,1));
    assert(entry[12]==0xc9 && ByteOrder_ReadLe16(entry+13)==ArRegionalPlatformSkull_Value(snapshot,2));
    assert(entry[20]==0xc9 && ByteOrder_ReadLe16(entry+21)==ArRegionalPlatformSkull_Value(snapshot,3));
    /* Stat policy is separate: PAL's base HP2/ATK2 must not be attributed to
     * the four skull behavior leaves or overwritten by their spawn prefix. */
    assert(record[7]==(region>=2?2:1) && record[8]==(region>=2?2:0));
    assert(!memcmp(entry+15,(uint8_t[]){0xb0,0x17,0x20},3));
    const uint8_t continuation[]={0xb0,0x0f,0xa9,0x21,0,region==1?2:0,0,0xa9,0x30,0,0x20};
    assert(!memcmp(entry+23,continuation,sizeof(continuation))); /* JP COP versus Western BRK audio service. */
  }
  puts("five-ROM skull records, gates, unchanged branches and independent PAL stats verified");
}
int main(int argc,char **argv) {
  assert(argc==1 || argc==6);CheckPolicies();CheckBirth();CheckRanges();if(argc==6)CheckRoms(argv+1);
  puts("platform skull: 3840 spawn/reused-slot cases, exhaustive 16-bit CMPs, mixed policies and guards passed");return 0;
}
