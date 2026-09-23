#include "actraiser/actraiser_arrival_runtime.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t low[65536],towns[65536];
static bool requested,effective,locked,available=true;
static unsigned departure_calls,palace_calls,announcements;
static RecompReturn native_result;
static uint8_t jp_code[65536];

/* Optional original-ROM differential control for the two translated JP
 * fragments. Stop before the announcement's JSR: the US message/cleanup
 * suffix is deliberately delegated, not a second dialogue implementation. */
static bool NativeJapanese(CpuState *cpu,bool palace) {
  uint16_t pc=palace?0x85c8:0xa335;
  for(unsigned step=0;step<100;++step) {
    if(palace && pc==0x85ee)return true;
    const uint8_t op=jp_code[pc++];
    if(op==0x60){cpu->S+=2;return false;}
    if(op==0xe8){++cpu->X;ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->X);continue;}
    if(op==0x88){--cpu->Y;ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->Y);continue;}
    uint16_t value=jp_code[pc++];
    switch(op) {
      case 0xc2:case 0xe2:
        cpu_mirrors_to_p(cpu);
        cpu->P=op==0xc2?cpu->P&~value:cpu->P|value;cpu_p_to_mirrors(cpu);break;
      case 0xad:
        value|=jp_code[pc++]<<8;assert(value==0x32f);value=low[0x341];
        cpu->A=(cpu->A&0xff00)|value;ActRaiserCpuHle_SetNegativeZero8(cpu,value);break;
      case 0x85:low[value]=(uint8_t)cpu->A;break;
      case 0x64:low[value]=0;break;
      case 0xaf:case 0xbf:case 0x8f:
        value|=jp_code[pc++]<<8;assert(jp_code[pc++]==0x7f);
        if(op==0xbf)value+=cpu->X;
        if(op==0x8f)towns[value]=(uint8_t)cpu->A;
        else {cpu->A=(cpu->A&0xff00)|towns[value];ActRaiserCpuHle_SetNegativeZero8(cpu,cpu->A);}
        break;
      case 0x29:case 0x09:
        cpu->A=(cpu->A&0xff00)|(op==0x29?(uint8_t)cpu->A&value:(uint8_t)cpu->A|value);
        ActRaiserCpuHle_SetNegativeZero8(cpu,cpu->A);break;
      case 0xc9:
        cpu->_flag_C=(uint8_t)cpu->A>=value;cpu->P=(cpu->P&~CPU_P_C)|(cpu->_flag_C?CPU_P_C:0);
        ActRaiserCpuHle_SetNegativeZero8(cpu,(uint8_t)(cpu->A-value));break;
      case 0xa0:case 0xa2:
        value|=jp_code[pc++]<<8;if(op==0xa0)cpu->Y=value;else cpu->X=value;
        ActRaiserCpuHle_SetNegativeZero16(cpu,value);break;
      case 0xf0:if(cpu->_flag_Z)pc+=(int8_t)value;break;
      case 0xd0:if(!cpu->_flag_Z)pc+=(int8_t)value;break;
      default:assert(!"unhandled original arrival opcode");
    }
  }
  assert(!"unterminated original arrival fragment");return false;
}
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 address) {
  (void)cpu;assert(bank==0 || bank==0x7f);return (bank?towns:low)[address];
}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 address) {
  return cpu_read8(cpu,bank,address)|(uint16)cpu_read8(cpu,bank,address+1)<<8;
}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 address,uint8 value) {
  (void)cpu;assert(bank==0 || bank==0x7f);(bank?towns:low)[address]=value;
}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 address,uint16 value) {
  cpu_write8(cpu,bank,address,value);cpu_write8(cpu,bank,address+1,value>>8);
}
bool ActRaiserRegional_ArrivalSnapshot(bool latch,bool continuing,bool *japanese) {
  if(!available)return false;
  if(latch && !locked){if(!continuing)effective=requested;locked=true;}
  *japanese=locked?effective:requested;return true;
}
RecompReturn bank_00_A343_M0X0(CpuState *cpu) {
  assert(!ActRaiser_RegionalArrivalDepartureEntry(cpu));++departure_calls;
  return native_result;
}
RecompReturn bank_01_861E_M1X0(CpuState *cpu) {
  assert(!ActRaiser_RegionalArrivalPalaceEntry(cpu));++palace_calls;
  if((towns[0x9101]&3)==1){++announcements;towns[0x9101]|=2;low[0x32c]=0;}
  return native_result;
}
static void Setup(bool japanese) {
  memset(low,0,sizeof(low));memset(towns,0,sizeof(towns));
  for(unsigned town=0;town<6;++town)towns[0x6b18+2*town]=2;
  low[0x341]=6;low[0x32c]=42;low[0x31a]=0x5a;low[0x334]=0x9a;
  requested=effective=japanese;locked=false;available=true;
  departure_calls=palace_calls=announcements=0;native_result=RECOMP_RETURN_NORMAL;
  ActRaiserArrivalRuntime_Reset();
}
static void CheckOriginal(const char *path) {
  FILE *file=fopen(path,"rb");assert(file);
  assert(!fseek(file,0x2335,SEEK_SET) && fread(jp_code+0xa335,1,12,file)==12);
  assert(!fseek(file,0x85c8,SEEK_SET) && fread(jp_code+0x85c8,1,51,file)==51);
  assert(!fclose(file));unsigned count=0;
  for(unsigned flags=0;flags<4;++flags)for(unsigned missing=0;missing<7;++missing)
  for(unsigned town=1;town<=6;++town)for(unsigned carry=0;carry<2;++carry) {
    Setup(true);low[0x341]=town;towns[0x9101]=0xa4|flags;
    if(missing<6)towns[0x6b18+2*missing]=1;
    CpuState cpu={.A=0xabcd,.X=0x4321,.Y=0x1234,.S=0x1ef0,.P=CPU_P_V|(carry?CPU_P_C:0)};
    cpu_p_to_mirrors(&cpu);CpuState reference=cpu;
    assert(ActRaiser_RegionalArrivalDeparture(&cpu)==RECOMP_RETURN_NORMAL);
    uint8_t saved_low[sizeof(low)],saved_towns[sizeof(towns)];
    memcpy(saved_low,low,sizeof(low));memcpy(saved_towns,towns,sizeof(towns));
    low[0x1a]=low[0x1b]=0;assert(!NativeJapanese(&reference,false));
    assert(!memcmp(&cpu,&reference,sizeof(cpu)) && !memcmp(saved_low,low,sizeof(low)) && !memcmp(saved_towns,towns,sizeof(towns)));
    cpu=(CpuState){.A=0xabcd,.X=0x4321,.Y=0x1234,.S=0x1ef0,.PB=1,.DB=1,.P=CPU_P_M|CPU_P_V|(carry?CPU_P_C:0)};
    cpu_p_to_mirrors(&cpu);reference=cpu;
    assert(ActRaiser_RegionalArrivalPalace(&cpu)==RECOMP_RETURN_NORMAL);
    memcpy(saved_low,low,sizeof(low));memcpy(saved_towns,towns,sizeof(towns));
    low[0x32c]=42;towns[0x9101]=0xa4|flags;
    const bool announce=NativeJapanese(&reference,true);
    assert(announce==(bool)announcements);
    if(announce)low[0x32c]=0; /* The delegated US cleanup, beyond this guard. */
    else assert(!memcmp(&cpu,&reference,sizeof(cpu)));
    assert(!memcmp(saved_low,low,sizeof(low)) && !memcmp(saved_towns,towns,sizeof(towns)));
    ++count;
  }
  printf("original JP ROM: %u differential departure/Palace guard cases\n",count);
}
int main(int argc,char **argv) {
  assert(argc==1 || argc==2);
  unsigned cases=0;
  for(unsigned jp=0;jp<2;++jp)for(unsigned flags=0;flags<4;++flags)
  for(unsigned missing=0;missing<7;++missing)for(unsigned current=1;current<=6;++current) {
    Setup(jp);towns[0x9101]=(uint8_t)(0xa4|flags);low[0x341]=current;
    if(missing<6)towns[0x6b18+2*missing]=1;
    CpuState cpu={.A=0xabab,.X=0x0810,.Y=0x4000,.S=0x1ee0,.P=CPU_P_C|CPU_P_V};cpu_p_to_mirrors(&cpu);
    const CpuState before=cpu;uint8_t saved[sizeof(towns)];memcpy(saved,towns,sizeof(saved));
    assert(ActRaiser_RegionalArrivalDepartureEntry(&cpu));
    assert(ActRaiser_RegionalArrivalDeparture(&cpu)==RECOMP_RETURN_NORMAL);
    assert(locked==((flags&1)!=0 || missing==6));
    assert(!memcmp(saved,towns,sizeof(saved)) && low[0x31a]==0x5a && low[0x334]==0x9a);
    if(jp) {
      assert(!departure_calls && cpu.A==(0xab00|current) && cpu.S==before.S+2 && cpu.X==before.X && cpu.Y==before.Y);
      assert(low[0x1a]==current && !low[0x1b] && cpu.P==(CPU_P_C|CPU_P_V));
    } else assert(departure_calls==1 && !memcmp(&cpu,&before,sizeof(cpu)));
    cpu=(CpuState){.A=0xabcd,.X=0x7890,.Y=0x2345,.S=0x1ee0,.PB=1,.DB=1,.P=CPU_P_M};cpu_p_to_mirrors(&cpu);
    assert(ActRaiser_RegionalArrivalPalaceEntry(&cpu));
    const bool announce=jp?!(flags&2)&&missing==6:(flags&3)==1;
    assert(ActRaiser_RegionalArrivalPalace(&cpu)==RECOMP_RETURN_NORMAL);
    assert(announcements==(unsigned)announce && low[0x32c]==(announce?0:42));
    assert(towns[0x9101]==(uint8_t)(0xa4|flags|(announce?3:0)));
    if(jp && !announce) {
      assert(!palace_calls && cpu.S==0x1ee2);
      if(flags&2)assert(cpu.A==0xab02 && cpu.X==0x7890 && cpu.Y==0x2345);
      else assert(cpu.A==0xab01 && cpu.X==2*missing && cpu.Y==6-missing && !cpu._flag_C);
    } else assert(palace_calls==1);
    assert(low[0x31a]==0x5a && low[0x334]==0x9a);
    ++cases;
  }
  for(unsigned token=0;token<=RECOMP_RETURN_OWNED_UNWIND;++token) {
    Setup(false);native_result=(RecompReturn)token;
    CpuState cpu={0};assert(ActRaiser_RegionalArrivalDeparture(&cpu)==native_result);
    cpu.PB=cpu.DB=1;cpu.P=CPU_P_M;cpu_p_to_mirrors(&cpu);
    assert(ActRaiser_RegionalArrivalPalace(&cpu)==native_result);
    assert(ActRaiser_RegionalArrivalPalaceEntry(&cpu));
  }
  /* A Western reveal already underway cannot become JP mid-transaction. */
  Setup(false);requested=true;towns[0x9101]=1;
  CpuState cpu={.PB=1,.DB=1,.P=CPU_P_M};cpu_p_to_mirrors(&cpu);
  assert(ActRaiser_RegionalArrivalPalace(&cpu)==RECOMP_RETURN_NORMAL && locked && !effective && announcements==1);
  requested=false;assert(ActRaiser_RegionalArrivalPalace(&cpu)==RECOMP_RETURN_NORMAL && announcements==1);
  /* Conversely, the decided JP route survives a later US request. */
  Setup(true);cpu=(CpuState){0};assert(ActRaiser_RegionalArrivalDeparture(&cpu)==RECOMP_RETURN_NORMAL && locked);
  requested=false;cpu.PB=cpu.DB=1;cpu.P=CPU_P_M;cpu_p_to_mirrors(&cpu);
  assert(ActRaiser_RegionalArrivalPalace(&cpu)==RECOMP_RETURN_NORMAL && effective && announcements==1);
  assert(ActRaiser_RegionalArrivalPalace(&cpu)==RECOMP_RETURN_NORMAL && announcements==1);
  for(unsigned kind=0;kind<8;++kind) {
    Setup(true);cpu=(CpuState){0};
    switch(kind) {
      case 0:cpu.DB=1;break;case 1:cpu.PB=1;break;case 2:cpu.m_flag=1;break;case 3:cpu.x_flag=1;break;
      case 4:cpu.D=1;break;case 5:cpu.emulation=1;break;case 6:low[0x347]=3;break;case 7:available=false;break;
    }
    assert(!ActRaiser_RegionalArrivalDepartureEntry(&cpu));
  }
  printf("final-island controller: %u source/guard/town cases, stable event policy and native escape tokens\n",cases);
  if(argc==2)CheckOriginal(argv[1]);
  return 0;
}
