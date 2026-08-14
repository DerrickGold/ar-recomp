#include "actraiser/actraiser_save_checksum.h"

#include <stdio.h>
#include <string.h>

#include "save_system.h"

static uint8_t wram[0x20000];
static uint8_t sram[kActRaiserSramSize];
uint8 *g_sram = sram;
int g_sram_size = sizeof(sram);
static int failures;

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      failures++;                                                          \
    }                                                                      \
  } while (0)

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu;
  if (bank == 0x7E) return wram[address];
  if (bank == 0x7F) return wram[0x10000u + address];
  return 0;
}

uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  const uint8 low = cpu_read8(cpu, bank, address);
  const uint8 high = cpu_read8(cpu, bank, (uint16)(address + 1));
  return (uint16)(low | ((uint16)high << 8));
}

void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu;
  if (bank == 0x7E)
    wram[address] = value;
  else if (bank == 0x7F)
    wram[0x10000u + address] = value;
}

void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(cpu, bank, address, (uint8)value);
  cpu_write8(cpu, bank, (uint16)(address + 1), (uint8)(value >> 8));
}

static uint16 ReadWram16(uint16 address) {
  return (uint16)(wram[address] | ((uint16)wram[(uint16)(address + 1)] << 8));
}

static void ReferenceBinary(uint16 *summed, uint16 *xored) {
  *summed = 0;
  *xored = 0;
  for (int offset = 0; offset < kActRaiserSramChecksumOffset; offset += 2) {
    const uint16 word =
        (uint16)(sram[offset] | ((uint16)sram[offset + 1] << 8));
    *summed = (uint16)(*summed + word);
    *xored ^= word;
  }
}

static uint16 ReferenceBcdAdd(uint16 a, uint16 b) {
  uint16 result = 0;
  unsigned carry = 0;
  for (unsigned shift = 0; shift < 16; shift += 4) {
    unsigned digit = ((a >> shift) & 0x0F) + ((b >> shift) & 0x0F) + carry;
    carry = digit > 9;
    if (carry) digit += 6;
    result |= (uint16)((digit & 0x0F) << shift);
  }
  return result;
}

static void ReferenceDecimal(uint16 *summed, uint16 *xored) {
  *summed = 0;
  *xored = 0;
  for (int offset = 0; offset < kActRaiserSramChecksumOffset; offset += 2) {
    const uint16 word =
        (uint16)(sram[offset] | ((uint16)sram[offset + 1] << 8));
    *summed = ReferenceBcdAdd(word, *summed);
    *xored ^= word;
  }
}

static CpuState MakeCpu(bool decimal) {
  CpuState cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.A = 0x1111;
  cpu.X = 0x2222;
  cpu.Y = 0x3333;
  cpu.S = 0x01E0;
  cpu.D = 0x0100;
  cpu.DB = 0x45;
  cpu.PB = 0x67;
  cpu.m_flag = 1;
  cpu.x_flag = 0;
  cpu._flag_C = 1;
  cpu._flag_I = 1;
  cpu._flag_D = decimal;
  cpu._flag_N = 1;
  cpu.ram = wram;
  return cpu;
}

static void CheckContract(bool decimal) {
  memset(wram, 0xCC, sizeof(wram));
  for (int i = 0; i < (int)sizeof(sram); i++)
    sram[i] = (uint8)(i * 37 + i / 11 + 0x53);

  uint16 summed = 0, xored = 0;
  if (decimal)
    ReferenceDecimal(&summed, &xored);
  else
    ReferenceBinary(&summed, &xored);
  CpuState cpu = MakeCpu(decimal);

  CHECK(ActRaiser_SaveAccumulateChecksum(&cpu) == RECOMP_RETURN_NORMAL);
  CHECK(ReadWram16(0x0114) == summed);
  CHECK(ReadWram16(0x0116) == xored);
  CHECK(cpu.A == xored);
  CHECK(cpu.X == summed);
  CHECK(cpu.Y == xored);
  CHECK(cpu.S == 0x01E3);
  CHECK(cpu.D == 0x0100);
  CHECK(cpu.DB == 0x45);
  CHECK(cpu.PB == 0x67);
  CHECK(cpu.P ==
        (CPU_P_M | CPU_P_C | CPU_P_I | (decimal ? CPU_P_D : 0) | CPU_P_N));
  CHECK(cpu.m_flag == 1);
  CHECK(cpu.x_flag == 0);
  CHECK(cpu._flag_C == 1);
  CHECK(cpu._flag_I == 1);
  CHECK(cpu._flag_D == decimal);
  CHECK(cpu._flag_N == 1);
}

int main(void) {
  CheckContract(false);
  CheckContract(true);
  if (failures) {
    printf("actraiser save checksum HLE: %d failure(s)\n", failures);
    return 1;
  }
  printf("actraiser save checksum HLE: all checks passed\n");
  return 0;
}
