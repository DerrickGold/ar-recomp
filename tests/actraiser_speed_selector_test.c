#include "actraiser/actraiser_speed_selector.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[65536], tiles[65536], code[65536];
uint8 cpu_read8(CpuState *c, uint8 b, uint16 a) { (void)c; return b == 0x7f ? tiles[a] : ram[a]; }
uint16 cpu_read16(CpuState *c, uint8 b, uint16 a) { return cpu_read8(c,b,a) | cpu_read8(c,b,(uint16)(a+1)) << 8; }
void cpu_write8(CpuState *c, uint8 b, uint16 a, uint8 v) { (void)c; (b == 0x7f ? tiles : ram)[a]=v; }
void cpu_write16(CpuState *c, uint8 b, uint16 a, uint16 v) { cpu_write8(c,b,a,v); cpu_write8(c,b,(uint16)(a+1),v>>8); }

/* Interpret only the audited LDA/CMP/BCS/INC/BRL movement prefix, from each
 * actual donor image when supplied. No generated implementation as oracle. */
static uint16_t NativeRight(CpuState *c, uint16_t pc) {
  for (unsigned steps = 0; steps < 6; ++steps) {
    const uint8_t op = code[pc++];
    if (op == 0xa5) {
      cpu_write_a8(c, ram[code[pc++]]);
      ActRaiserCpuHle_SetNegativeZero8(c,(uint8_t)c->A);
    } else if (op == 0xc9) {
      const unsigned value = code[pc++];
      c->_flag_C = (uint8_t)c->A >= value; c->P=(c->P & ~1u) | c->_flag_C;
      ActRaiserCpuHle_SetNegativeZero8(c,(uint8_t)(c->A-value));
    } else if (op == 0xb0) {
      const int8_t distance = (int8_t)code[pc++];
      if (c->_flag_C) return (uint16_t)(pc + distance);
    } else if (op == 0xe6) {
      const unsigned address = code[pc++];
      ++ram[address]; ActRaiserCpuHle_SetNegativeZero8(c,ram[address]);
    } else if (op == 0x82) {
      const int16_t distance = (int16_t)(code[pc] | code[pc+1]<<8);
      return (uint16_t)(pc + 2 + distance);
    } else assert(!"unexpected movement opcode");
  }
  assert(!"unterminated movement prefix"); return 0;
}

int main(int argc, char **argv) {
  for (unsigned maximum=7; maximum<=9; maximum+=2) {
    for (unsigned cursor=0; cursor<65536; ++cursor) {
      CpuState c={.PB=1,.DB=1,.P=0x45,.S=0x1f00,._flag_V=1,._flag_C=1};
      cpu_write16(&c,0,0x0a,(uint16_t)cursor); ram[0x200]=9;
      ActRaiserSpeedSelector_Position(&c,(uint16_t)maximum);
      const unsigned working=cursor>maximum ? maximum : cursor;
      assert(c.A==working+0x0b11+(maximum==7) && c.X==c.A && c.Y==0xfa94);
      assert(cpu_read16(&c,0,0x0a)==working && ram[0x200]==9);
      assert(c._flag_N && !c._flag_Z && !c._flag_C && !c._flag_V && c.P==0x84 && c.S==0x1f00);
    }
    for (unsigned value=0; value<256; ++value) {
      CpuState c={.PB=1,.DB=1,.m_flag=1,.P=0x20,.A=0x5a00,.S=0x1fee};
      ram[0x0a]=(uint8_t)value; ram[0x0b]=0xa5; ram[0x200]=9;
      uint32_t target=ActRaiserSpeedSelector_Right(&c,(uint16_t)maximum);
      assert(target==(value>=maximum ? 0x018b2f : 0x018b11));
      assert(c.A==(0x5a00|value) && c.S==0x1fee && ram[0x0b]==0xa5 && ram[0x200]==9);
      assert(ram[0x0a]==(value>=maximum ? value : value+1));
    }
    CpuState c={.PB=1,.DB=1,.m_flag=1};
    memset(tiles,0xa5,sizeof(tiles));
    ActRaiserSpeedSelector_DrawNativeScale(&c,(uint16_t)maximum);
    for (unsigned i=0;i<65536;++i) {
      const bool digit=maximum==7 && i>=0xb324 && i<=0xb336 && !(i&1);
      const unsigned column=(i-0xb324)/2;
      assert(tiles[i]==(digit ? (column==0 || column==9 ? ' ' : '0'+column-1) : 0xa5));
    }
  }
  for (int arg=1; arg<argc; ++arg) {
    assert(arg<=2); const bool jp=arg==2;
    FILE *f=fopen(argv[arg],"rb"); assert(f);
    assert(!fseek(f,0x8000,SEEK_SET)); assert(fread(code+0x8000,1,0x8000,f)==0x8000); assert(!fclose(f));
    for (unsigned value=0; value<256; ++value) for (unsigned flags=0; flags<256; ++flags) {
      CpuState h={.PB=1,.DB=1,.m_flag=1,.A=0x5a00,.P=(uint8_t)((flags&0xc7)|0x20)};
      h._flag_N=!!(h.P&0x80); h._flag_V=!!(h.P&0x40); h._flag_Z=!!(h.P&2); h._flag_C=h.P&1;
      CpuState n=h;
      ram[0x0a]=(uint8_t)value;
      uint32_t target=ActRaiserSpeedSelector_Right(&h,jp?7:9); const uint8_t working=ram[0x0a];
      ram[0x0a]=(uint8_t)value;
      const uint16_t native=NativeRight(&n,jp?0x8af0:0x8b59);
      assert(native==(target==0x018b11 ? (jp?0x8aa8:0x8b11) : (jp?0x8ac6:0x8b2f)));
      assert(working==ram[0x0a] && !memcmp(&h,&n,sizeof(h)));
    }
  }
  puts("speed selector: bounded cursor, native attributes and optional US/JP movement differential passed");
  return 0;
}
