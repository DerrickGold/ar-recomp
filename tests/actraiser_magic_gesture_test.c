#include "actraiser/actraiser_magic_gesture.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[65536], code[65536];
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at) { (void)cpu; assert(!bank); return ram[at]; }
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at) { return cpu_read8(cpu,bank,at) | cpu_read8(cpu,bank,(uint16)(at+1))<<8; }
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value) { (void)cpu; assert(!bank); ram[at]=value; }
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value) { cpu_write8(cpu,bank,at,value); cpu_write8(cpu,bank,(uint16)(at+1),value>>8); }

/* Bounded interpretation of the actual JP prefix; only its release-mask
 * address is mapped into the baseline ABI. No generated code as oracle. */
static uint16_t NativeAttack(CpuState *cpu) {
  uint16_t pc=0x9a6d;
  for (unsigned step=0;step<7;++step) {
    const uint8_t op=code[pc++];
    if (op==0xa9 || op==0xad || op==0x89) {
      const uint16_t operand=code[pc] | code[pc+1]<<8; pc+=2;
      if (op==0x89) {
        cpu->_flag_Z=!(cpu->A & operand);
        cpu->P=(cpu->P & ~CPU_P_Z) | (cpu->_flag_Z ? CPU_P_Z : 0);
      } else {
        cpu->A=op==0xa9 ? operand : cpu_read16(cpu,0,operand);
        ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
      }
    } else if (op==0x14) {
      const unsigned operand=code[pc++]; assert(operand==0xf9);
      const uint16_t old=cpu_read16(cpu,0,0xf6);
      cpu->_flag_Z=!(old & cpu->A);
      cpu->P=(cpu->P & ~CPU_P_Z) | (cpu->_flag_Z ? CPU_P_Z : 0);
      cpu_write16(cpu,0,0xf6,(uint16_t)(old & ~cpu->A));
    } else if (op==0xf0) {
      const int8_t distance=(int8_t)code[pc++];
      if (cpu->_flag_Z) return (uint16_t)(pc+distance);
    } else if (op==0x82) {
      const int16_t distance=(int16_t)(code[pc] | code[pc+1]<<8);
      return (uint16_t)(pc+2+distance);
    } else assert(!"unexpected ground-attack prefix opcode");
  }
  assert(!"unbounded ground-attack prefix"); return 0;
}

int main(int argc,char **argv) {
  assert(argc<=2);
  if (argc==2) {
    FILE *file=fopen(argv[1],"rb"); assert(file);
    assert(fread(code+0x8000,1,0x8000,file)==0x8000 && !fclose(file));
  }
  const unsigned masks[]={0,0x4000,0xffff,0xa5a5};
  for (unsigned buttons=0; buttons<65536; ++buttons) {
    assert(ActRaiserMagicGesture_ControlsReleased((uint16_t)buttons)==!(buttons & 0x48c0));
    for (unsigned flags=0; flags<16; ++flags) for (unsigned m=0;m<4;++m) {
      CpuState c={.P=(uint8_t)(((flags&12)<<4) | (flags&3)),
          .A=0xa5a5,.X=0x8a0,.Y=0xbeef,.S=0x1fd0};
      cpu_p_to_mirrors(&c);
      const CpuState before=c;
      cpu_write16(&c,0,0xf6,(uint16_t)masks[m]);
      cpu_write16(&c,0,0xa1,(uint16_t)buttons);
      const uint32_t target=ActRaiserMagicGesture_Attack(&c);
      assert(target==((buttons&8) ? 0x009de1u : 0x009a73u));
      assert(c.A==buttons && c.X==before.X && c.Y==before.Y && c.S==before.S);
      assert(c._flag_N==!!(buttons&0x8000) && c._flag_Z==!(buttons&8));
      assert((c.P & ~0x82u)==(before.P & ~0x82u));
      const uint16_t masked=cpu_read16(&c,0,0xf6);
      assert(masked==(masks[m]&~0x4000u));
      if (argc==2) {
        CpuState native=before;
        cpu_write16(&native,0,0xf6,(uint16_t)masks[m]);
        assert(NativeAttack(&native)==((buttons&8) ? 0x9deb : 0x9a7d));
        assert(!memcmp(&c,&native,sizeof(c)) && masked==cpu_read16(&native,0,0xf6));
      }
    }
  }
  CpuState c={0}; assert(ActRaiserMagicGesture_Entry(&c));
  c.PB=1; assert(!ActRaiserMagicGesture_Entry(&c)); c.PB=0;
  c.DB=1; assert(!ActRaiserMagicGesture_Entry(&c)); c.DB=0;
  c.D=1; assert(!ActRaiserMagicGesture_Entry(&c)); c.D=0;
  c.m_flag=1; assert(!ActRaiserMagicGesture_Entry(&c)); c.m_flag=0;
  c.x_flag=1; assert(!ActRaiserMagicGesture_Entry(&c)); c.x_flag=0;
  c.emulation=1; assert(!ActRaiserMagicGesture_Entry(&c));
  assert(!ActRaiserMagicGesture_Entry(NULL));
  puts("magic gesture: release masks, attack prefix and optional JP ROM differential passed");
  return 0;
}
