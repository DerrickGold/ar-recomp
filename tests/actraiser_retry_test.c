#include "actraiser/actraiser_retry.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include "regional/regional_retry.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[65536], us_code[65536], jp_code[65536];
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu; assert(bank == 0); return ram[address];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  return cpu_read8(cpu,bank,address) | cpu_read8(cpu,bank,address+1)<<8;
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu; assert(bank == 0); ram[address] = value;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(cpu,bank,address,value); cpu_write8(cpu,bank,address+1,value>>8);
}

static uint16_t Native(CpuState *cpu, bool jp) {
  const uint8_t *code = jp ? jp_code : us_code;
  uint16_t pc = jp ? 0x9844 : 0x981c;
  for (unsigned n = 0; n < 8; ++n) {
    if (pc == (jp ? 0x9859 : 0x982f)) return 0x982f;
    if (pc == (jp ? 0x9850 : 0x9826)) return 0x9826;
    const uint8_t op = code[pc++];
    uint16_t value = code[pc++];
    switch (op) {
      case 0xc2:
        cpu_mirrors_to_p(cpu); cpu->P &= (uint8_t)~value; cpu_p_to_mirrors(cpu); break;
      case 0xad: case 0x9c:
        value |= code[pc++] << 8;
        /* Relocate JP's respawn marker to the US host's semantic field. */
        if (jp && value == 0x31a) value = 0x32c;
        if (op == 0xad) {
          cpu->A = cpu_read16(cpu,0,value);
          ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
        } else cpu_write16(cpu,0,value,0);
        break;
      case 0xf0: if (cpu->_flag_Z) pc += (int8_t)value; break;
      case 0x64: cpu_write16(cpu,0,value,0); break;
      default: assert(!"unexpected opcode in native retry prefix");
    }
  }
  assert(!"unterminated retry prefix"); return 0;
}

int main(int argc, char **argv) {
  assert(argc == 1 || argc == 3);
  const bool native = argc == 3;
  if (native) {
    for (unsigned i = 0; i < 2; ++i) {
      FILE *file = fopen(argv[i+1], "rb"); assert(file);
      assert(fread((i ? jp_code : us_code)+0x8000,1,0x8000,file) == 0x8000);
      fclose(file);
    }
    assert(us_code[0x981c] == 0xc2 && jp_code[0x9844] == 0xc2 && jp_code[0x984e] == 0x64);
  }
  const uint16_t markers[] = {0,1,0xffff}, scores[] = {0,1,0x1234,0xffff};
  unsigned cases = 0;
  for (unsigned source = 0; source < kArRegionalSource_Count; ++source)
    for (unsigned m = 0; m < 2; ++m)
      for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 4; ++j)
          for (unsigned flags = 0; flags < 16; ++flags) {
            memset(ram,0xa5,sizeof(ram));
            CpuState cpu = {.A=0xbeef,.X=0xc00,.Y=0x1357,.S=0x1fa,.m_flag=m,
                ._flag_C=flags&1,._flag_D=(flags>>1)&1,._flag_I=(flags>>2)&1,._flag_V=(flags>>3)&1};
            cpu_mirrors_to_p(&cpu);
            cpu_write16(&cpu,0,0x32c,markers[i]); cpu_write16(&cpu,0,0x1f,scores[j]);
            const CpuState before = cpu;
            uint8_t expected[sizeof(ram)]; memcpy(expected,ram,sizeof(ram));
            bool clear; assert(ArRegionalRetry_Resolve((ArRegionalSource)source,&clear));
            const uint16_t pc = ActRaiserRetry_Prefix(&cpu,clear);
            assert(pc == (markers[i] ? 0x9826 : 0x982f));
            expected[0x32c] = expected[0x32d] = 0;
            if (markers[i] && clear) expected[0x1f] = expected[0x20] = 0;
            assert(!memcmp(expected,ram,sizeof(ram)));
            assert(cpu.A == markers[i] && !cpu.m_flag && cpu.S == before.S &&
                cpu.X == before.X && cpu.Y == before.Y && cpu._flag_C == before._flag_C &&
                cpu._flag_D == before._flag_D && cpu._flag_I == before._flag_I && cpu._flag_V == before._flag_V);
            assert(cpu._flag_Z == (markers[i] == 0) && cpu._flag_N == ((markers[i] & 0x8000) != 0));
            if (native) {
              CpuState reference = before;
              cpu_write16(&reference,0,0x32c,markers[i]); cpu_write16(&reference,0,0x1f,scores[j]);
              assert(Native(&reference,source == kArRegionalSource_Japan) == pc);
              assert(!memcmp(expected,ram,sizeof(ram)) && !memcmp(&reference,&cpu,sizeof(cpu)));
            }
            ++cases;
          }
  CpuState cpu = {0}; assert(ActRaiserRetry_Entry(&cpu));
  for (unsigned i = 0; i < 5; ++i) {
    CpuState bad = cpu;
    if (i == 0) bad.PB = 1; if (i == 1) bad.DB = 1; if (i == 2) bad.D = 1;
    if (i == 3) bad.x_flag = 1; if (i == 4) bad.emulation = 1;
    assert(!ActRaiserRetry_Entry(&bad));
  }
  bool clear = true;
  assert(!ArRegionalRetry_Resolve(kArRegionalSource_Count,&clear) && clear);
  assert(!ArRegionalRetry_Resolve(kArRegionalSource_US,NULL));
  printf("PASS %u retry prefix cases%s\n", cases, native ? " with US/JP native differential checks" : "");
  return 0;
}
