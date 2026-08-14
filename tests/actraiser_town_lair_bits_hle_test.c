#include "actraiser/actraiser_town_lair_bits.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cpu_65816_math.h"

enum {
  kStackBank = 0x00,
  kRomBank = 0x03,
  kDataBank = 0x7F,
  kBankBytes = 0x10000,
  kCurrentTownIndex = 0x7BFB,
  kScratchByte = 0x914F,
  kGlobalFlagsBase = 0x90FF,
  kMaskTableAddress = 0xF4D7,
  kPerTownPointerTable = 0xDCA2,
};

typedef RecompReturn (*TownBitHle)(CpuState *cpu);

static const uint8 kBitMasks[] = {
  0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01,
};

static uint8 memory[3 * kBankBytes];
static int failures;

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      failures++;                                                          \
    }                                                                      \
  } while (0)

static size_t BankOffset(uint8 bank, uint16 address) {
  if (bank == kStackBank && address < 0x2000) return address;
  if (bank == kRomBank) return kBankBytes + address;
  if (bank == kDataBank) return 2 * kBankBytes + address;
  return 0;
}

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu;
  return memory[BankOffset(bank, address)];
}

uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  const uint8 low = cpu_read8(cpu, bank, address);
  const uint8 high = cpu_read8(cpu, bank, (uint16)(address + 1));
  return (uint16)(low | ((uint16)high << CHAR_BIT));
}

void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu;
  memory[BankOffset(bank, address)] = value;
}

void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(cpu, bank, address, (uint8)value);
  cpu_write8(cpu, bank, (uint16)(address + 1),
             (uint8)(value >> CHAR_BIT));
}

static void InitializeMemory(void) {
  memset(memory, 0xCC, sizeof(memory));
  for (unsigned bit = 0; bit < sizeof(kBitMasks); bit++)
    cpu_write8(NULL, kRomBank, (uint16)(kMaskTableAddress + bit),
               kBitMasks[bit]);
  /* $F4DF follows the eight mask bytes, so bit 7's 16-bit table load has
   * $DA (the following PHX opcode) as its architecturally visible high byte. */
  cpu_write8(NULL, kRomBank,
             (uint16)(kMaskTableAddress + sizeof(kBitMasks)), 0xDA);
}

static CpuState MakeCpu(uint16 accumulator, uint16 x, uint16 y,
                        bool decimal) {
  CpuState cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.A = accumulator;
  cpu.X = x;
  cpu.Y = y;
  cpu.S = 0x01F0;
  cpu.DB = kDataBank;
  cpu.PB = kRomBank;
  cpu.m_flag = 1;
  cpu.x_flag = 0;
  cpu._flag_C = 1;
  cpu._flag_Z = 1;
  cpu._flag_I = 1;
  cpu._flag_D = decimal;
  cpu._flag_V = 1;
  cpu._flag_N = 1;
  cpu.ram = memory;
  cpu_mirrors_to_p(&cpu);
  return cpu;
}

static uint8 ExpectedResolverP(bool decimal, bool overflow,
                               bool zero, bool negative) {
  return (uint8)(CPU_P_I | CPU_P_M |
                 (decimal ? CPU_P_D : 0) |
                 (overflow ? CPU_P_V : 0) |
                 (zero ? CPU_P_Z : 0) |
                 (negative ? CPU_P_N : 0));
}

static uint16 ConfigurePerTownMask(bool decimal, uint16 encoded_town_index,
                                   uint16 table_address,
                                   uint16 mask_base) {
  const Cpu65816Add16Result pointer = Cpu65816_Add16(
      encoded_town_index, table_address, false, decimal);
  cpu_write16(NULL, kRomBank, pointer.value, mask_base);
  cpu_write16(NULL, kDataBank, kCurrentTownIndex, encoded_town_index);
  return pointer.value;
}

static void CheckPerTownResolver(void) {
  InitializeMemory();
  const uint16 mask_base = 0x9120;
  const uint8 flag_id = 0x0B;
  const uint16 byte_address = (uint16)(mask_base + flag_id / CHAR_BIT);
  const uint8 byte_value = 0xA5;
  ConfigurePerTownMask(false, 4, kPerTownPointerTable, mask_base);
  cpu_write8(NULL, kDataBank, byte_address, byte_value);
  CpuState cpu = MakeCpu((uint16)(0xB400u | flag_id), 0x2468,
                         kPerTownPointerTable, false);
  const uint16 entry_stack = cpu.S;

  CHECK(ActRaiser_TownLairMaskResolveBit(&cpu) == RECOMP_RETURN_NORMAL);
  CHECK(cpu.A == kBitMasks[flag_id & 7u]);
  CHECK(cpu.X == byte_address);
  CHECK(cpu.Y == (flag_id & 7u));
  CHECK(cpu_read8(NULL, kDataBank, kScratchByte) == byte_value);
  CHECK(cpu.S == (uint16)(entry_stack + 2));
  CHECK(cpu.host_return_valid == 0);
  CHECK(cpu.P == ExpectedResolverP(false, false, false, false));

  /* The final PHX/PLY pair overwrites the first resolver push, while the
   * selected mask base remains from the inner PHY. */
  CHECK(cpu_read16(NULL, kStackBank,
                   (uint16)(entry_stack - 1)) == (flag_id & 7u));
  CHECK(cpu_read16(NULL, kStackBank,
                   (uint16)(entry_stack - 3)) == mask_base);
}

static void CheckGlobalResolver(bool decimal, uint8 flag_id) {
  InitializeMemory();
  const Cpu65816Add16Result address = Cpu65816_Add16(
      flag_id / CHAR_BIT, kGlobalFlagsBase, false, decimal);
  cpu_write8(NULL, kDataBank, address.value, 0x69);
  CpuState cpu = MakeCpu((uint16)(0x5A00u | flag_id), 0x2468,
                         0x1357, decimal);
  const uint16 entry_stack = cpu.S;
  const unsigned bit_index = flag_id & 7u;
  const uint16 mask_word = cpu_read16(
      NULL, kRomBank, (uint16)(kMaskTableAddress + bit_index));

  CHECK(ActRaiser_TownGlobalFlagResolveBit(&cpu) ==
        RECOMP_RETURN_NORMAL);
  CHECK(cpu.A == mask_word);
  CHECK(cpu.X == bit_index);
  CHECK(cpu.Y == address.value);
  CHECK(cpu_read8(NULL, kDataBank, kScratchByte) == 0x69);
  CHECK(cpu.S == (uint16)(entry_stack + 2));
  CHECK(cpu.P == ExpectedResolverP(
      decimal, address.overflow, false, (mask_word & 0x8000u) != 0));
  CHECK(cpu_read16(NULL, kStackBank,
                   (uint16)(entry_stack - 1)) == flag_id);
  CHECK(cpu_read16(NULL, kStackBank,
                   (uint16)(entry_stack - 3)) == kGlobalFlagsBase);
}

static void CheckOuterStack(uint16 entry_stack, uint16 saved_x,
                            uint16 saved_y,
                            uint16 resolver_return_address) {
  CHECK(cpu_read16(NULL, kStackBank,
                   (uint16)(entry_stack - 1)) == saved_x);
  CHECK(cpu_read16(NULL, kStackBank,
                   (uint16)(entry_stack - 3)) == saved_y);
  CHECK(cpu_read16(NULL, kStackBank,
                   (uint16)(entry_stack - 5)) == resolver_return_address);
}

static void CheckPerTownOperation(TownBitHle hle, uint16 return_address,
                                  uint8 initial_value,
                                  uint8 expected_value, bool test) {
  InitializeMemory();
  const uint16 mask_base = 0x9120;
  const uint8 flag_id = 2;
  const uint16 saved_x = test ? 0x2345 : 0x8001;
  const uint16 saved_y = kPerTownPointerTable;
  ConfigurePerTownMask(false, 2, saved_y, mask_base);
  cpu_write8(NULL, kDataBank, mask_base, initial_value);
  CpuState cpu = MakeCpu((uint16)(0xB400u | flag_id), saved_x, saved_y,
                         false);
  const uint16 entry_stack = cpu.S;

  CHECK(hle(&cpu) == RECOMP_RETURN_NORMAL);
  CHECK(cpu.A == expected_value);
  CHECK(cpu.X == saved_x);
  CHECK(cpu.Y == saved_y);
  CHECK(cpu.S == (uint16)(entry_stack + 2));
  CHECK(cpu.host_return_valid == 1);
  CHECK(cpu_read8(NULL, kDataBank, kScratchByte) == initial_value);
  if (!test)
    CHECK(cpu_read8(NULL, kDataBank, mask_base) == expected_value);
  else
    CHECK(cpu_read8(NULL, kDataBank, mask_base) == initial_value);

  const bool result_zero = expected_value == 0;
  const bool result_negative = (expected_value & 0x80u) != 0;
  const bool final_zero = test ? result_zero : saved_x == 0;
  const bool final_negative = test
      ? result_negative
      : (saved_x & 0x8000u) != 0;
  CHECK(cpu.P == ExpectedResolverP(
      false, false, final_zero, final_negative));
  CheckOuterStack(entry_stack, saved_x, saved_y, return_address);

  /* The resolver's final bit-index push and selected-base push remain below
   * the nested JSR frame. */
  CHECK(cpu_read16(NULL, kStackBank,
                   (uint16)(entry_stack - 7)) == flag_id);
  CHECK(cpu_read16(NULL, kStackBank,
                   (uint16)(entry_stack - 9)) == mask_base);
}

static void CheckGlobalOperation(TownBitHle hle, uint16 return_address,
                                 uint8 flag_id, uint8 initial_value,
                                 uint8 expected_value, bool test) {
  InitializeMemory();
  const uint16 byte_address = (uint16)(
      kGlobalFlagsBase + flag_id / CHAR_BIT);
  const uint16 saved_x = test ? 0x1357 : 0;
  const uint16 saved_y = 0x2468;
  cpu_write8(NULL, kDataBank, byte_address, initial_value);
  CpuState cpu = MakeCpu((uint16)(0xB400u | flag_id), saved_x, saved_y,
                         false);
  const uint16 entry_stack = cpu.S;
  const uint16 mask_word = cpu_read16(
      NULL, kRomBank,
      (uint16)(kMaskTableAddress + (flag_id & 7u)));

  CHECK(hle(&cpu) == RECOMP_RETURN_NORMAL);
  CHECK(cpu.A == (uint16)((mask_word & 0xFF00u) | expected_value));
  CHECK(cpu.X == saved_x);
  CHECK(cpu.Y == saved_y);
  CHECK(cpu.S == (uint16)(entry_stack + 2));
  CHECK(cpu.host_return_valid == 1);
  if (!test)
    CHECK(cpu_read8(NULL, kDataBank, byte_address) == expected_value);
  else
    CHECK(cpu_read8(NULL, kDataBank, byte_address) == initial_value);

  const bool final_zero = test ? expected_value == 0 : saved_x == 0;
  const bool final_negative = test
      ? (expected_value & 0x80u) != 0
      : (saved_x & 0x8000u) != 0;
  CHECK(cpu.P == ExpectedResolverP(
      false, false, final_zero, final_negative));
  CheckOuterStack(entry_stack, saved_x, saved_y, return_address);
  CHECK(cpu_read16(NULL, kStackBank,
                   (uint16)(entry_stack - 7)) == flag_id);
  CHECK(cpu_read16(NULL, kStackBank,
                   (uint16)(entry_stack - 9)) == kGlobalFlagsBase);
}

int main(void) {
  CheckPerTownResolver();
  CheckGlobalResolver(false, 7);
  CheckGlobalResolver(true, 8);

  CheckPerTownOperation(ActRaiser_TownLairMaskTest, 0xF472,
                        0x81, 0, true);
  CheckPerTownOperation(ActRaiser_TownLairMaskSet, 0xF47D,
                        0x81, 0xA1, false);
  CheckPerTownOperation(ActRaiser_TownLairMaskClear, 0xF48B,
                        0xA1, 0x81, false);

  CheckGlobalOperation(ActRaiser_TownGlobalFlagTest, 0xF4E3,
                       7, 0x01, 0x01, true);
  CheckGlobalOperation(ActRaiser_TownGlobalFlagSet, 0xF4EE,
                       3, 0x80, 0x90, false);
  CheckGlobalOperation(ActRaiser_TownGlobalFlagClear, 0xF4FC,
                       3, 0x90, 0x80, false);

  if (failures) {
    printf("actraiser town-lair-bit HLE: %d failure(s)\n", failures);
    return 1;
  }
  printf("actraiser town-lair-bit HLE: all checks passed\n");
  return 0;
}
