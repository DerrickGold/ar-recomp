#include "actraiser/actraiser_lives_display.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t stock, tiles[4];
static unsigned reads, writes;
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu; assert(!bank && address==0x1c); ++reads; return stock;
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu; assert(bank==0x7f && (address==0xb050 || address==0xb052));
  ++writes; tiles[address-0xb050]=value;
}

int main(void) {
  unsigned cases=0;
  for (unsigned raw=0;raw<256;++raw) for (unsigned flags=0;flags<256;++flags)
    for (unsigned high=0;high<2;++high) {
      if(flags & (CPU_P_M|CPU_P_X|CPU_P_D)) continue;
      CpuState cpu={.A=(uint16_t)(high?0xa5fe:0x0000),.X=0x50,.Y=0x9abc,.S=0x1f4,.PB=2,.DB=0x7e,.P=(uint8_t)flags};
      cpu_p_to_mirrors(&cpu);
      stock=(uint8_t)raw; reads=writes=0; memset(tiles,0xa5,sizeof(tiles));
      assert(ActRaiserLivesDisplay_Entry(&cpu));
      CpuState expected=cpu;
      expected.A=(expected.A&0xff00) | (0x30+(raw%16));
      expected.P=(flags & CPU_P_I) | CPU_P_M;
      cpu_p_to_mirrors(&expected);
      ActRaiserLivesDisplay_DrawZeroBased(&cpu);
      assert(!memcmp(&cpu,&expected,sizeof(cpu)));
      assert(tiles[0]==0x30+raw/16 && tiles[2]==0x30+raw%16);
      assert(tiles[1]==0xa5 && tiles[3]==0xa5 && stock==raw);
      assert(reads==1 && writes==2);
      ++cases;
    }
  assert(!ActRaiserLivesDisplay_Entry(NULL));
  const CpuState valid={.PB=2,.X=0x50};
  for (unsigned kind=0;kind<8;++kind) {
    CpuState cpu=valid;
    switch(kind) {
      case 0: cpu.PB=3; break;
      case 1: cpu.D=1; break;
      case 2: cpu.emulation=1; break;
      case 3: cpu.m_flag=1; break;
      case 4: cpu.x_flag=1; break;
      case 5: cpu.X=0x51; break;
      case 6: cpu.P=CPU_P_D; break;
      case 7: cpu._flag_D=1; break;
    }
    assert(!ActRaiserLivesDisplay_Entry(&cpu));
  }
  printf("lives HUD: %u flag/stock/high-byte cases, only two glyph writes\n",cases);
  return 0;
}
