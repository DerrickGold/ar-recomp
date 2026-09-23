#include "actraiser/actraiser_miracle_text.h"
#include <assert.h>
#include <string.h>

static uint8_t rom[65536], ram[0x20000];
uint8 cpu_read8(CpuState *c, uint8 bank, uint16 addr) {
  (void)c; return bank==1 ? rom[addr] : ram[(bank==0x7f?0x10000:0)+addr];
}
uint16 cpu_read16(CpuState *c, uint8 bank, uint16 addr) {
  return cpu_read8(c,bank,addr) | cpu_read8(c,bank,addr+1)<<8;
}
void cpu_write8(CpuState *c,uint8 bank,uint16 addr,uint8 value) {
  (void)c; assert(bank!=1); ram[(bank==0x7f?0x10000:0)+addr]=value;
}
int main(void) {
  static const struct { uint16_t source; const char *us,*jp; } fields[]={
    {0xfcd5,"10","12"},{0xfd76,"20","16"},{0xff13,"30","18"},
    {0xfde9,"80","24"},{0xfe71,"160"," 60"},
  };
  ArRegionalCostPolicy policy; ArRegionalCostSnapshot prices;
  for (unsigned region=0;region<3;++region) {
    assert(ArRegionalCosts_Init(&policy,region) && ArRegionalCosts_Resolve(&policy,&prices));
    for (unsigned i=0;i<5;++i) {
      memcpy(rom+fields[i].source,fields[i].us,strlen(fields[i].us));
      for (unsigned n=0;n<strlen(fields[i].us);++n) {
        CpuState c={0}; c.PB=c.DB=1; c.m_flag=1; c.S=0x1ef0;
        c.X=0x152; c.Y=fields[i].source+n+1; c.A=0x5522; c.P=0x65;
        ram[c.S+1]=0x26; ram[c.S+2]=0x90;
        ram[0x1b150]=fields[i].us[n]; ram[0x1b151]=0x38;
        CpuState before=c;
        ActRaiserMiracle_UpdateNativeDigit(&c,&prices);
        assert(ram[0x1b150]==(region==kArRegionalSource_Japan?fields[i].jp[n]:fields[i].us[n]));
        assert(ram[0x1b151]==0x38 && !memcmp(&c,&before,sizeof(c)));
        for (unsigned bad=0;bad<8;++bad) {
          c=before; ram[0x1b150]=fields[i].us[n];
          switch(bad) {
            case 0: c.DB=2; break; case 1: c.PB=2; break;
            case 2: c.X=0; break; case 3: c.X=0x1002; break;
            case 4: c.X=0x153; break; case 5: c.Y=0xffff; break;
            case 6: c.x_flag=1; break; case 7: ram[c.S+1]=0x28; break;
          }
          ActRaiserMiracle_UpdateNativeDigit(&c,&prices);
          assert(ram[0x1b150]==fields[i].us[n]);
          ram[before.S+1]=0x26;
        }
      }
    }
  }
  return 0;
}
