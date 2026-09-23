#include "actraiser/actraiser_score_feedback.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[65536], town[65536];
static unsigned writes;
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 address) {
  (void)cpu; assert(bank==0 || bank==0x7f); return (bank?town:ram)[address];
}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 address) {
  return cpu_read8(cpu,bank,address) | cpu_read8(cpu,bank,(uint16_t)(address+1))<<8;
}
static void Store(uint8_t *memory,uint16_t address,uint16_t value) {
  memory[address]=(uint8_t)value; memory[(uint16_t)(address+1)]=(uint8_t)(value>>8);
}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 address,uint16 value) {
  (void)cpu; assert(bank==0x7f && (address==0x7c05 || address==0x7c07 ||
      (address>=0x96b8 && address<=0x96e6 && !(address&1))));
  Store(town,address,value); ++writes;
}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 address,uint8 value) {
  (void)cpu;(void)bank;(void)address;(void)value; assert(!"prefix must not write byte memory");
}
static CpuState Cpu(void) {
  return (CpuState){.PB=3,.DB=0x7f,.A=0xabcd,.X=6,.Y=0xeeee,.S=0x1f00,
      .P=CPU_P_I|CPU_P_C|CPU_P_V|CPU_P_N,._flag_C=1,._flag_V=1,._flag_N=1};
}
static void NZ(CpuState *cpu,uint16_t result) {
  cpu->_flag_N=(result&0x8000)!=0; cpu->_flag_Z=result==0;
  cpu->P=(uint8_t)((cpu->P&~(CPU_P_N|CPU_P_Z)) | (cpu->_flag_N?CPU_P_N:0) | (cpu->_flag_Z?CPU_P_Z:0));
}
static void Conversion(void) {
  for(unsigned score=0;score<10000;++score) {
    const uint16_t bcd=(uint16_t)((score%10)|((score/10%10)<<4)|((score/100%10)<<8)|((score/1000)<<12));
    Store(ram,0x1f,bcd); writes=0;
    CpuState cpu=Cpu(), expected=cpu;
    const unsigned reduced=score<650?0:score-650;
    expected.A=(uint16_t)(reduced/32*10);
    expected.Y=(uint16_t)((reduced%10)|((reduced/10%10)<<4)|((reduced/100%10)<<8)|((reduced/1000)<<12));
    expected._flag_C=expected._flag_V=0;
    expected.P&=~(CPU_P_C|CPU_P_V); NZ(&expected,expected.A);
    assert(ActRaiserScoreFeedback_ConvertJP(&cpu));
    assert(writes==2 && !memcmp(&cpu,&expected,sizeof(cpu)));
    assert(cpu_read16(&cpu,0x7f,0x7c05)==reduced%1000);
    assert(cpu_read16(&cpu,0x7f,0x7c07)==reduced/32*2);
  }
  Store(ram,0x1f,0xabcd); CpuState cpu=Cpu(), before=cpu; writes=0;
  assert(!ActRaiserScoreFeedback_ConvertJP(&cpu) && !memcmp(&cpu,&before,sizeof(cpu)) && !writes);
}
static void Routing(void) {
  for(unsigned jp=0;jp<2;++jp)for(unsigned count=0;count<65536;++count) {
    CpuState cpu=Cpu(),expected=cpu; uint32_t target=0; writes=0;
    Store(town,0x6b18+cpu.X,(uint16_t)count);
    if(jp) {
      expected.A=(uint16_t)count; expected._flag_C=count>=2;
      expected.P=(uint8_t)((expected.P&~CPU_P_C)|(count>=2?CPU_P_C:0));
      NZ(&expected,(uint16_t)(count-2));
    } else { expected.A=(uint16_t)(count-(count==1?1:2)); NZ(&expected,expected.A); }
    assert(ActRaiserScoreFeedback_Route(&cpu,jp,&target));
    assert(!writes && !memcmp(&cpu,&expected,sizeof(cpu)));
    assert(target==(count==2?0x03d0c7u:jp||count==1?0x03d0beu:0x03d0ceu));
  }
}
static void Subtraction(void) {
  static const uint16_t deltas[]={0,1,499,730,0x4000,0xffff};
  for(unsigned d=0;d<sizeof(deltas)/sizeof(deltas[0]);++d)for(unsigned input=0;input<65536;++input) {
    CpuState cpu=Cpu(); cpu.X=(uint16_t)(input%6*8); const CpuState before=cpu;
    Store(ram,(uint16_t)(cpu.S+1),deltas[d]); writes=0;
    for(unsigned n=0;n<4;++n)Store(town,(uint16_t)(0x96b8+cpu.X+2*n),(uint16_t)(input+n));
    assert(ActRaiserScoreFeedback_Subtract(&cpu));
    assert(writes==4 && cpu.X==before.X && cpu.Y==before.Y && cpu.S==before.S && cpu.DB==before.DB && cpu.PB==before.PB);
    for(unsigned n=0;n<4;++n) {
      const unsigned initial=(uint16_t)(input+n);
      const uint16_t expected=(uint16_t)(initial>=deltas[d]?initial-deltas[d]:0);
      assert(cpu_read16(&cpu,0x7f,(uint16_t)(0x96b8+cpu.X+2*n))==expected);
      if(n==3) {
        assert(cpu.A==expected && cpu._flag_C==(initial>=deltas[d]));
        assert(cpu._flag_Z==(expected==0) && cpu._flag_N==!!(expected&0x8000));
        const int signed_result=(int)(int16_t)initial-(int)(int16_t)deltas[d];
        assert(cpu._flag_V==(signed_result < -32768 || signed_result>32767));
      }
    }
  }
  CpuState cpu=Cpu(); cpu.X=48; const CpuState before=cpu; writes=0;
  assert(!ActRaiserScoreFeedback_Subtract(&cpu) && !writes && !memcmp(&cpu,&before,sizeof(cpu)));
}
static void NativeReference(const char *path) {
  FILE *file=fopen(path,"r"); assert(file);
  unsigned score,a,y,scratch1,scratch2,p,count=0;
  while(fscanf(file,"%u %u %u %u %u %u",&score,&a,&y,&scratch1,&scratch2,&p)==6) {
    assert(score==count && score<10000);
    const uint16_t bcd=(uint16_t)((score%10)|((score/10%10)<<4)|((score/100%10)<<8)|((score/1000)<<12));
    Store(ram,0x1f,bcd); CpuState cpu=Cpu();
    assert(ActRaiserScoreFeedback_ConvertJP(&cpu));
    assert(cpu.A==a && cpu.Y==y && cpu.P==p);
    assert(cpu_read16(&cpu,0x7f,0x7c05)==scratch1 && cpu_read16(&cpu,0x7f,0x7c07)==scratch2);
    ++count;
  }
  assert(!ferror(file) && feof(file) && count==10000); fclose(file);
  puts("all 10000 JP native converter register/flag/scratch results match");
}
int main(int argc,char **argv) {
  assert(argc==1 || argc==2);
  Conversion(); Routing(); Subtraction();
  for(unsigned mode=0;mode<8;++mode) {
    CpuState cpu=Cpu();
    switch(mode) {
      case 0:cpu.PB=2;break;case 1:cpu.DB=0;break;case 2:cpu.D=2;break;
      case 3:cpu.emulation=1;break;case 4:cpu.m_flag=1;break;case 5:cpu.x_flag=1;break;
      case 6:cpu._flag_D=1;break;case 7:cpu.P|=CPU_P_D;break;
    }
    const CpuState before=cpu; uint32_t target=123; writes=0;
    assert(!ActRaiserScoreFeedback_Entry(&cpu));
    assert(!ActRaiserScoreFeedback_Route(&cpu,true,&target) && target==123);
    assert(!ActRaiserScoreFeedback_ConvertJP(&cpu));
    assert(!ActRaiserScoreFeedback_Subtract(&cpu));
    assert(!writes && !memcmp(&cpu,&before,sizeof(cpu)));
  }
  puts("score prefixes: conversion/scratch/flags, routing, saturating subtraction and failure atomicity passed");
  if(argc==2)NativeReference(argv[1]);
  return 0;
}
