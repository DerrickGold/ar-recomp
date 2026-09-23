#include "actraiser/actraiser_score_lives.h"
#include "regional/regional_score_lives.h"
#include "byte_order.h"
#include "cpu_65816_math.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t memory[65536];
static bool enabled;
static unsigned calls,events;
static RecompReturn native_result;
bool ActRaiserRegional_ScoreLivesEnabled(void) {return enabled;}
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at) {(void)cpu;assert(!bank);return memory[at];}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at) {return cpu_read8(cpu,bank,at)|(uint16)cpu_read8(cpu,bank,at+1)<<8;}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value) {(void)cpu;assert(!bank);memory[at]=value;}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value) {cpu_write8(cpu,bank,at,value);cpu_write8(cpu,bank,at+1,value>>8);}
static void Cop(CpuState *cpu) {assert(cpu->A==0x8d);++events;}
void (*g_cpu_cop_hook)(CpuState *)=Cop;
static RecompReturn Native(CpuState *cpu) {
  assert(!ActRaiser_ScoreLivesEntry(cpu));++calls;
  if(native_result!=RECOMP_RETURN_NORMAL) {cpu->A=0xbeef;return native_result;}
  const Cpu65816Add16Result sum=Cpu65816_Add16(cpu->A,ByteOrder_ReadLe16(memory+0x1f),false,true);
  cpu->A=sum.carry?0x9999:sum.value;ByteOrder_WriteLe16(memory+0x1f,cpu->A);
  cpu->S+=2;return RECOMP_RETURN_NORMAL;
}
RecompReturn bank_00_873C_M0X0(CpuState *cpu) {assert(!cpu->m_flag);return Native(cpu);}
RecompReturn bank_00_873C_M1X0(CpuState *cpu) {assert(cpu->m_flag);return Native(cpu);}
static uint16_t Bcd(unsigned value) {
  return (uint16_t)((value%10)|((value/10%10)<<4)|((value/100%10)<<8)|((value/1000%10)<<12));
}
static void Transactions(void) {
  const unsigned old_values[]={0,1998,1999,2000,3999,5999,7999,8000,9998,9999};
  const unsigned increments[]={0,1,2,50,100,2000,4000,8000,9999};
  for(unsigned o=0;o<10;++o)for(unsigned a=0;a<9;++a)for(unsigned lives=0;lives<100;++lives)
  for(unsigned action=0;action<2;++action)for(unsigned m=0;m<2;++m) {
    memset(memory,0x5a,sizeof(memory));memory[0x1c]=Bcd(lives);memory[0x349]=action?7:0;
    ByteOrder_WriteLe16(memory+0x1f,Bcd(old_values[o]));
    CpuState cpu={.A=Bcd(increments[a]),.X=0x960,.Y=0x2b,.S=0x1ef0,
        .P=(uint8_t)((lives&(CPU_P_C|CPU_P_V|CPU_P_D|CPU_P_N|CPU_P_Z|CPU_P_I))|(m?CPU_P_M:0))};
    cpu_p_to_mirrors(&cpu);enabled=true;native_result=RECOMP_RETURN_NORMAL;events=calls=0;
    const CpuState before=cpu;
    uint8_t expected[65536];memcpy(expected,memory,sizeof(expected));
    unsigned after=old_values[o]+increments[a];if(after>9999)after=9999;
    const bool award=action && ((Bcd(old_values[o])^Bcd(after))&0xe000);
    ByteOrder_WriteLe16(expected+0x1f,Bcd(after));
    if(award)expected[0x1c]=Bcd((lives+1)%100);
    assert(ActRaiser_ScoreLivesEntry(&cpu));
    assert(ActRaiser_ScoreLives(&cpu)==RECOMP_RETURN_NORMAL && calls==1 && events==(unsigned)award);
    assert(cpu.A==Bcd(old_values[o]) && cpu.S==before.S+2 && cpu.P==before.P &&
        cpu.X==before.X && cpu.Y==before.Y && cpu.m_flag==before.m_flag &&
        cpu._flag_C==before._flag_C && cpu._flag_D==before._flag_D &&
        cpu._flag_N==before._flag_N && cpu._flag_Z==before._flag_Z && cpu._flag_V==before._flag_V);
    assert(!memcmp(memory,expected,sizeof(memory)));
  }
  for(unsigned source=0;source<3;++source) {
    assert(ArRegionalScoreLives_Resolve(source,&enabled) && enabled==(source==2));
    CpuState cpu={0};assert(ActRaiser_ScoreLivesEntry(&cpu)==enabled);
  }
  for(unsigned invalid=0;invalid<6;++invalid) {
    CpuState cpu={0};enabled=true;
    switch(invalid) {
      case 0: cpu.PB=1;break;case 1: cpu.DB=1;break;case 2: cpu.D=1;break;
      case 3: cpu.x_flag=1;break;case 4: cpu.emulation=1;break;case 5:enabled=false;break;
    }
    assert(!ActRaiser_ScoreLivesEntry(&cpu));
  }
  assert(!ActRaiser_ScoreLivesEntry(NULL) && !ArRegionalScoreLives_Resolve(3,&enabled) &&
      !ArRegionalScoreLives_Resolve(0,NULL));
  for(unsigned life=0;life<256;++life) {
    /* Independent hardware-style correction covers malformed debug BCD too. */
    unsigned sum=life+1;if((life&15)==15 || (life&15)+1>9)sum+=6;
    if(sum>0x9f)sum+=0x60;
    assert(ArRegionalScoreLives_Increment(life)==(uint8_t)sum);
  }
  const RecompReturn escapes[]={RECOMP_RETURN_TAILCALL,RECOMP_RETURN_PARKED_WAIT,RECOMP_RETURN_OWNED_UNWIND,RECOMP_RETURN_SKIP_1};
  for(unsigned n=0;n<sizeof(escapes)/sizeof(escapes[0]);++n) {
    CpuState cpu={0};enabled=true;events=0;native_result=escapes[n];
    uint8_t before[65536];memcpy(before,memory,sizeof(before));
    assert(ActRaiser_ScoreLives(&cpu)==escapes[n] && cpu.A==0xbeef && !events);
    assert(!memcmp(before,memory,sizeof(before)) && ActRaiser_ScoreLivesEntry(&cpu));
  }
}
static void Roms(char **paths) {
  uint8_t rom[1048576];
  for(unsigned r=0;r<5;++r) {
    FILE *file=fopen(paths[r],"rb");assert(file && fread(rom,1,sizeof(rom),file)==sizeof(rom) && !fclose(file));
    if(r<2) {
      const uint8_t code[]={8,0xf8,0xc2,0x20,0x18,0x65,0x1f,0x85,0x1f,0x90,5,0xa9,0x99,0x99,0x85,0x1f,0x28,0x60};
      assert(!memcmp(rom+(r?0x72b:0x73c),code,sizeof(code)));
    } else {
      assert(!memcmp(rom+0x654,(uint8_t[]){8,0xf8,0xc2,0x20,0xd4,0x1f,0x18,0x65,0x1f},9));
      assert(!memcmp(rom+0x666,(uint8_t[]){0x43,1,0x29,0,0xe0,0xf0,0x18,0xad,0x36,3,0xc9,2,0},13));
      assert(!memcmp(rom+0x675,(uint8_t[]){0xe2,0x20,0xa5,0x1c,0x18,0x69,1,0x85,0x1c,0xc2,0x20,0xa9,0x8d,0,2,0,0x68,0x28,0x60},19));
    }
  }
  puts("five-ROM score-life helpers verified");
}
int main(int argc,char **argv) {
  assert(argc==1 || argc==6);Transactions();if(argc==6)Roms(argv+1);
  puts("score-life native delegation, decimal boundaries, guards and escapes passed");return 0;
}
