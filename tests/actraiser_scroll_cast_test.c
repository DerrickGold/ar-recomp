#include "actraiser/actraiser_scroll_cast.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include "actraiser/regional/actraiser_regional_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[65536], code[65536];
static ActRaiserInventoryView inventory;
ActRaiserInventoryView ActRaiserRegional_InventoryView(void) {return inventory;}
bool ActRaiserRegional_BeginSpell(uint8_t *spell) {
  assert(inventory.enabled && inventory.count && !inventory.casting);
  *spell=inventory.icon;inventory.casting=*spell;return true;
}
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu; assert(bank == 0); return ram[address];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  return cpu_read8(cpu,bank,address) | cpu_read8(cpu,bank,address+1)<<8;
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu; assert(bank == 0); ram[address]=value;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(cpu,bank,address,value); cpu_write8(cpu,bank,address+1,value>>8);
}
static void Load(CpuState *cpu, uint16_t value) {
  cpu->A=value; ActRaiserCpuHle_SetNegativeZero16(cpu,value);
}
static uint16_t Native(CpuState *cpu) {
  uint16_t pc=0x9de1;
  for (unsigned n=0;n<32;++n) {
    if (pc==0x984e || pc==0x9e0e) return pc;
    const uint8_t op=code[pc++];
    uint16_t value=code[pc];
    switch (op) {
      case 0xa5: ++pc; Load(cpu,cpu_read16(cpu,0,value)); break;
      case 0xad: case 0xbd:
        value|=code[pc+1]<<8; pc+=2;
        if (op==0xbd) value+=cpu->X;
        Load(cpu,cpu_read16(cpu,0,value)); break;
      case 0x29: value|=code[pc+1]<<8; pc+=2; Load(cpu,cpu->A&value); break;
      case 0x89: value|=code[pc+1]<<8; pc+=2;
        cpu->_flag_Z=!(cpu->A&value);
        cpu->P=(cpu->P&~CPU_P_Z) | (cpu->_flag_Z?CPU_P_Z:0); break;
      case 0xf0: case 0xd0:
        ++pc; if ((op==0xf0)==(cpu->_flag_Z!=0)) pc+=(int8_t)value; break;
      case 0x82: value|=code[pc+1]<<8; pc+=2; pc+=(int16_t)value; break;
      case 0xe2: cpu->P|=value; ++pc; cpu->m_flag=(cpu->P>>5)&1; break;
      case 0xc2: cpu->P&=~value; ++pc; cpu->m_flag=(cpu->P>>5)&1; break;
      case 0xc6: ++pc; assert(cpu->m_flag);
        --ram[value]; ActRaiserCpuHle_SetNegativeZero8(cpu,ram[value]); break;
      default: assert(!"unexpected opcode in native scroll gate");
    }
  }
  assert(!"native gate failed to terminate"); return 0;
}
int main(int argc, char **argv) {
  bool native=argc==2;
  if (native) {
    FILE *rom=fopen(argv[1],"rb"); assert(rom);
    assert(fread(code+0x8000,1,0x8000,rom)==0x8000); fclose(rom);
    assert(code[0x9de1]==0xa5 && code[0x9e0a]==0xc6);
  }
  unsigned cases=0;
  const unsigned stocks[]={0,1,2,3,4,5,127,128,255};
  const unsigned flags[]={0,8,0x2000,0x2008,0x8000,0x4000};
  for (unsigned source=0;source<kArRegionalSource_Count;++source) {
    ArRegionalCostPolicy policy;
    ArRegionalCostSnapshot quote;
    assert(ArRegionalCosts_Init(&policy,source) && ArRegionalCosts_Resolve(&policy,&quote));
    for (unsigned spell=0;spell<=4;++spell)
      for (unsigned cooldown=0;cooldown<2;++cooldown)
        for (unsigned f=0;f<sizeof(flags)/sizeof(flags[0]);++f)
          for (unsigned s=0;s<sizeof(stocks)/sizeof(stocks[0]);++s)
            for (unsigned status=0;status<4;++status) {
              memset(ram,0xa5,sizeof(ram));
              CpuState cpu={.A=0xbeef,.X=0xc00,.Y=0xaaaa,.S=0x1f0,
                  .P=(status&1)|((status&2)?CPU_P_V:0),._flag_C=status&1,
                  ._flag_V=(status>>1)&1};
              cpu_write16(&cpu,0,0x02ac,spell);
              ram[0xf8]=cooldown; ram[0x21]=stocks[s];
              cpu_write16(&cpu,0,cpu.X+0x30,flags[f]);
              uint8_t before[sizeof(ram)]; memcpy(before,ram,sizeof(ram));
              CpuState original=cpu;
              assert(ActRaiserScrollCast_Entry(&cpu));
              const uint16_t target=ActRaiserScrollCast_Gate(&cpu,&quote);
              const unsigned cost=spell ? quote.price[spell-1] : 0;
              const bool accepted=spell && !cooldown && !(flags[f]&0x2008) && stocks[s]>=cost;
              assert(target==(accepted?0x9e0e:0x984e));
              assert(ram[0x21]==stocks[s]-(accepted?cost:0));
              before[0x21]=ram[0x21]; assert(!memcmp(before,ram,sizeof(ram)));
              assert(cpu.S==original.S && cpu.X==original.X && cpu.Y==original.Y &&
                     !cpu.m_flag && cpu._flag_C==original._flag_C && cpu._flag_V==original._flag_V);
              if (native && source!=kArRegionalSource_Japan) {
                uint8_t after[sizeof(ram)]; memcpy(after,ram,sizeof(ram));
                ram[0x21]=stocks[s];
                assert(Native(&original)==target);
                assert(!memcmp(&original,&cpu,sizeof(cpu)) && !memcmp(after,ram,sizeof(ram)));
              }
              ++cases;
            }
  }
  CpuState valid={0}; assert(ActRaiserScrollCast_Entry(&valid));
  for (unsigned i=0;i<6;++i) {
    CpuState bad=valid;
    if(i==0)bad.PB=1; if(i==1)bad.DB=1; if(i==2)bad.D=1;
    if(i==3)bad.m_flag=1; if(i==4)bad.x_flag=1; if(i==5)bad.emulation=1;
    assert(!ActRaiserScrollCast_Entry(&bad));
  }
  printf("PASS %u scroll gate cases%s\n",cases,native?" with US-ROM differential checks":"");
  return 0;
}
