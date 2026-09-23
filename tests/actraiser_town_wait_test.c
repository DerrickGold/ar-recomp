#include "actraiser/actraiser_town_wait.h"
#include "actraiser/actraiser_cpu_hle_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[65536], original[65536], expected[65536], code[65536];
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu; assert(bank == 0x7f); return ram[address];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  return cpu_read8(cpu,bank,address) | cpu_read8(cpu,bank,address+1)<<8;
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu; assert(bank == 0x7f); ram[address] = value;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(cpu,bank,address,value); cpu_write8(cpu,bank,address+1,value>>8);
}

static void Native(CpuState *cpu, uint16_t pc) {
  for (unsigned n = 0; n < 12; ++n) {
    const uint8_t op = code[pc++];
    if (op == 0x60) { cpu->S += 2; return; }
    uint16_t operand = code[pc++];
    if (op == 0xd0) { if (!cpu->_flag_Z) pc += (int8_t)operand; continue; }
    operand |= code[pc++] << 8;
    if (op != 0xae) operand += cpu->X;
    uint16_t value;
    switch (op) {
      case 0xae:
        cpu->X = cpu_read16(cpu,0x7f,operand);
        ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->X); break;
      case 0xde: case 0xfe:
        value = (uint16_t)(cpu_read16(cpu,0x7f,operand) + (op == 0xfe ? 1 : -1));
        cpu_write16(cpu,0x7f,operand,value);
        ActRaiserCpuHle_SetNegativeZero16(cpu,value); break;
      case 0xbd:
        cpu->A = cpu_read16(cpu,0x7f,operand);
        ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A); break;
      case 0x9d: cpu_write16(cpu,0x7f,operand,cpu->A); break;
      default: assert(!"unexpected native town wait opcode");
    }
  }
  assert(!"unterminated native town wait");
}

static unsigned Cases(bool native, uint16_t entry) {
  const uint16_t remaining[] = {0,1,2,150,65535};
  const uint16_t states[] = {0,3,65535}, reloads[] = {0,1,150,65535};
  unsigned cases = 0;
  for (unsigned town = 0; town < 6; ++town)
    for (unsigned i = 0; i < 5; ++i)
      for (unsigned j = 0; j < 3; ++j)
        for (unsigned k = 0; k < 4; ++k)
          for (unsigned f = 0; f < 16; ++f) {
            memset(ram,0xa5,sizeof(ram));
            CpuState cpu = {.PB=3,.DB=0x7f,.S=0x1fa,.A=0x4321,.X=0xbeef,.Y=0x1234,
                ._flag_C=f&1,._flag_D=(f>>1)&1,._flag_I=(f>>2)&1,._flag_V=(f>>3)&1};
            cpu_mirrors_to_p(&cpu);
            cpu_write16(&cpu,0x7f,0x7bfb,town*2);
            cpu_write16(&cpu,0x7f,0x7ce1+town*2,remaining[i]);
            cpu_write16(&cpu,0x7f,0x7cc9+town*2,states[j]);
            cpu_write16(&cpu,0x7f,0x7cd5+town*2,reloads[k]);
            memcpy(original,ram,sizeof(ram));
            CpuState reference = cpu;
            assert(ActRaiserTownWait_Entry(&cpu));
            assert(ActRaiserTownWait_Step(&cpu,reloads[k]) == RECOMP_RETURN_NORMAL);
            const bool expired = remaining[i] == 1;
            assert(cpu_read16(&cpu,0x7f,0x7ce1+town*2) == (expired ? reloads[k] : (uint16_t)(remaining[i]-1)));
            assert(cpu_read16(&cpu,0x7f,0x7cc9+town*2) == (uint16_t)(states[j]+expired));
            assert(cpu.A == (expired ? reloads[k] : reference.A) && cpu.X == town*2 &&
                cpu.Y == reference.Y && cpu.S == reference.S+2 && cpu._flag_C == reference._flag_C &&
                cpu._flag_D == reference._flag_D && cpu._flag_I == reference._flag_I && cpu._flag_V == reference._flag_V);
            memcpy(expected,ram,sizeof(ram));
            if (native) {
              memcpy(ram,original,sizeof(ram));
              Native(&reference,entry);
              assert(!memcmp(ram,expected,sizeof(ram)) && !memcmp(&reference,&cpu,sizeof(cpu)));
            } else {
              memcpy(expected+0x7ce1+town*2,original+0x7ce1+town*2,2);
              memcpy(expected+0x7cc9+town*2,original+0x7cc9+town*2,2);
              assert(!memcmp(expected,original,sizeof(ram)));
            }
            ++cases;
          }
  return cases;
}

int main(int argc, char **argv) {
  assert(argc == 1 || argc == 6); /* US JP EU DE FR */
  unsigned cases = Cases(false,0);
  if (argc == 6) {
    for (unsigned i = 1; i < 6; ++i) {
      FILE *file = fopen(argv[i],"rb"); assert(file);
      assert(!fseek(file,0x18000,SEEK_SET));
      assert(fread(code+0x8000,1,0x8000,file) == 0x8000); fclose(file);
      const uint16_t entry = i == 2 ? 0x86da : 0x872a;
      assert(code[entry] == 0xae && code[entry+17] == 0x60);
      const uint16_t init = i == 2 ? 0xa866 : 0xaa9e;
      assert(code[init] == 0xa9 && code[init+1] == (i == 2 ? 150 : 1) && !code[init+2]);
      cases += Cases(true,entry);
    }
  }
  CpuState cpu = {.PB=3,.DB=0x7f};
  for (unsigned town = 0; town < 256; ++town) {
    cpu_write16(&cpu,0x7f,0x7bfb,town);
    assert(ActRaiserTownWait_Entry(&cpu) == (town < 12 && !(town&1)));
  }
  cpu_write16(&cpu,0x7f,0x7bfb,0);
  for (unsigned i = 0; i < 6; ++i) {
    CpuState bad=cpu;
    if(i==0)bad.PB=0; if(i==1)bad.DB=0; if(i==2)bad.D=1;
    if(i==3)bad.m_flag=1; if(i==4)bad.x_flag=1; if(i==5)bad.emulation=1;
    assert(!ActRaiserTownWait_Entry(&bad));
  }
  puts("town state-3 wait checked"); printf("%u cases\n",cases);
  return 0;
}
