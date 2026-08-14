#include "actraiser/actraiser_lzss.h"

#include <stdio.h>
#include <string.h>

static uint8_t wram[0x20000];
static uint8_t source_bank[0x10000];
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
  if (bank == 0x05) return source_bank[address];
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

static void WriteWram16(uint16 address, uint16 value) {
  wram[address] = (uint8)value;
  wram[(uint16)(address + 1)] = (uint8)(value >> 8);
}

static uint16 ReadWram16(uint16 address) {
  return (uint16)(wram[address] | ((uint16)wram[(uint16)(address + 1)] << 8));
}

static void PutBits(size_t *bit, unsigned value, unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    const unsigned shift = count - 1 - i;
    if ((value >> shift) & 1u)
      source_bank[0x9000u + (*bit >> 3)] |=
          (uint8)(1u << (7 - (*bit & 7)));
    (*bit)++;
  }
}

static void TestCpuContract(void) {
  memset(wram, 0xCC, sizeof(wram));
  memset(source_bank, 0, sizeof(source_bank));

  size_t bit = 0;
  PutBits(&bit, 1, 1);       /* literal */
  PutBits(&bit, 'A', 8);
  PutBits(&bit, 0, 1);       /* match */
  PutBits(&bit, 0xEF, 8);
  PutBits(&bit, 3, 4);       /* length = 3 + 2 */
  CHECK(bit == 22);

  WriteWram16(0x00A5, 0x9000);
  wram[0x00A7] = 0x05;
  WriteWram16(0x00B3, 6);
  WriteWram16(0x00B5, 0x4000);

  CpuState cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.S = 0x01F0;
  cpu.D = 0;
  cpu.DB = 0x12;
  cpu.X = 0x3456;
  cpu.Y = 0x789A;
  cpu.m_flag = 1;
  cpu.x_flag = 0;
  cpu._flag_C = 1;
  cpu._flag_I = 1;
  cpu._flag_N = 1;
  cpu.ram = wram;

  CHECK(ActRaiser_LzssDecompress(&cpu) == RECOMP_RETURN_NORMAL);
  CHECK(!memcmp(&wram[0x4000], "AAAAAA", 6));
  for (unsigned i = 0; i < 6; i++) CHECK(wram[0x20EF + i] == 'A');
  CHECK(wram[0x2000] == 0x20);
  CHECK(wram[0x20EE] == 0x20);
  CHECK(wram[0x20F5] == 0x20);

  CHECK(ReadWram16(0x00A5) == 0x9002);
  CHECK(wram[0x00A7] == 0x05);
  CHECK(wram[0x00AE] == 0x02);
  CHECK(ReadWram16(0x00AF) == 0x20F5);
  CHECK(ReadWram16(0x00B1) == 0x20F4);
  CHECK(ReadWram16(0x00B3) == 6);
  CHECK(ReadWram16(0x00B5) == 0x4000);
  const uint16 source_window =
      (uint16)((uint16)source_bank[0x9001] << 8 | source_bank[0x9002]);
  CHECK(ReadWram16(0x00B7) == (uint16)(source_window << 2));

  CHECK(cpu.A == 0x0141);
  CHECK(cpu.S == 0x01F3);
  CHECK(cpu.DB == 0x12);
  CHECK(cpu.X == 0x3456);
  CHECK(cpu.Y == 0x789A);
  CHECK(cpu.P == (CPU_P_M | CPU_P_C | CPU_P_I | CPU_P_N));
  CHECK(cpu.m_flag == 1);
  CHECK(cpu.x_flag == 0);
  CHECK(cpu._flag_C == 1);
  CHECK(cpu._flag_I == 1);
  CHECK(cpu._flag_N == 1);
}

static void TestLiteralAccumulatorExit(void) {
  memset(wram, 0xCC, sizeof(wram));
  memset(source_bank, 0, sizeof(source_bank));

  size_t bit = 0;
  PutBits(&bit, 1, 1);
  PutBits(&bit, 'Z', 8);
  CHECK(bit == 9);

  WriteWram16(0x00A5, 0x9000);
  wram[0x00A7] = 0x05;
  wram[0x00AD] = 0xFF;
  WriteWram16(0x00B3, 1);
  WriteWram16(0x00B5, 0x4100);

  CpuState cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.S = 0x01E0;
  cpu.m_flag = 1;
  cpu.x_flag = 0;
  cpu.ram = wram;

  CHECK(ActRaiser_LzssDecompress(&cpu) == RECOMP_RETURN_NORMAL);
  CHECK(wram[0x4100] == 'Z');
  CHECK(wram[0x20EF] == 'Z');
  CHECK(ReadWram16(0x00A5) == 0x9001);
  CHECK(wram[0x00AE] == 0x40);
  CHECK(ReadWram16(0x00AF) == 0x20F0);
  CHECK(ReadWram16(0x00B1) == 0x2000);
  CHECK(ReadWram16(0x00B7) == 0x5A00);
  /* $C639 shifts the $AD/$AE helper word twice at bit phase one, then
   * overwrites only A.low with the literal from $B8. */
  CHECK(cpu.A == 0x035A);
  CHECK(cpu.S == 0x01E3);
}

int main(void) {
  TestCpuContract();
  TestLiteralAccumulatorExit();
  if (failures) {
    printf("actraiser lzss HLE: %d failure(s)\n", failures);
    return 1;
  }
  printf("actraiser lzss HLE: all checks passed\n");
  return 0;
}
