#include "actraiser/actraiser_action_metatile.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "action/action_bg_metatile.h"
#include "cpu_65816_math.h"

enum {
  kStackBank = 0x00,
  kLowWramBank = 0x7E,
  kDataBank = 0x7F,
  kBankBytes = 0x10000,
  kDirectPage = 0x0200,
  kCommonAttributeBitsDp = 0x06,
  kPreservedBitMaskDp = 0x0A,
  kMetatilesRemainingDp = 0x0E,
  kDefinitionPointerDp = 0xA5,
  kMapAddress = 0x1000,
  kDefinitionAddress = 0x2000,
  kOutputAddress = 0x4000,
  kMetatilesPerStrip = 16,
  kDefinitionBytes = 8,
  kOutputAdvanceBytes = 4,
  kRightTileOffset = 2,
  kBottomTileOffset = 0x40,
  kBottomRightTileOffset = kBottomTileOffset + kRightTileOffset,
  kColumnSourceStride = 0x10,
  kWordBits = sizeof(uint16) * CHAR_BIT,
};

typedef RecompReturn (*ActionMetatileHle)(CpuState *cpu);

static uint8 memory[2 * kBankBytes];
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
  if (bank == kLowWramBank) return address;
  if (bank == kDataBank) return kBankBytes + address;
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

static uint16 DefinitionWord(uint8 metatile_id, unsigned tile) {
  const uint16 tile_index = (uint16)(metatile_id * 4u + tile);
  const uint16 flip = tile & 1u ? 0x4000 : 0;
  const uint16 priority = tile & 2u ? 0x8000 : 0;
  const uint16 palette = (uint16)((metatile_id & 7u) << 10);
  return (uint16)(tile_index | flip | priority | palette);
}

static uint16 ComposedWord(uint8 metatile_id, unsigned tile,
                           uint16 preserved_bit_mask,
                           uint16 common_attribute_bits) {
  return ActionBg_ComposeTilemapWord(
      DefinitionWord(metatile_id, tile), preserved_bit_mask,
      common_attribute_bits);
}

static void WriteDefinition(uint8 metatile_id) {
  const uint16 address = (uint16)(
      kDefinitionAddress + metatile_id * kDefinitionBytes);
  for (unsigned tile = 0; tile < 4; tile++)
    cpu_write16(NULL, kDataBank, (uint16)(address + tile * 2),
                DefinitionWord(metatile_id, tile));
}

static CpuState MakeCpu(bool decimal, uint16 saved_x, uint16 saved_y) {
  CpuState cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.A = 0xA55A;
  cpu.X = saved_x;
  cpu.Y = saved_y;
  cpu.S = 0x01F0;
  cpu.D = kDirectPage;
  cpu.DB = kDataBank;
  cpu.PB = 0x02;
  cpu.m_flag = 0;
  cpu.x_flag = 0;
  cpu._flag_C = 1;
  cpu._flag_Z = 1;
  cpu._flag_I = 1;
  cpu._flag_D = decimal;
  cpu._flag_V = 1;
  cpu._flag_N = 1;
  cpu.host_return_valid = 0;
  cpu.ram = memory;
  cpu_mirrors_to_p(&cpu);
  return cpu;
}

static void SetDescriptors(uint16 preserved_bit_mask,
                           uint16 common_attribute_bits) {
  cpu_write16(NULL, kLowWramBank,
              kDirectPage + kDefinitionPointerDp, kDefinitionAddress);
  cpu_write16(NULL, kLowWramBank,
              kDirectPage + kPreservedBitMaskDp, preserved_bit_mask);
  cpu_write16(NULL, kLowWramBank,
              kDirectPage + kCommonAttributeBitsDp,
              common_attribute_bits);
}

static uint16 AdvanceColumnSource(uint16 source, bool decimal,
                                  unsigned advances) {
  for (unsigned i = 0; i < advances; i++)
    source = Cpu65816_Add16(
        source, kColumnSourceStride, false, decimal).value;
  return source;
}

static Cpu65816Add16Result FinalColumnAdvance(uint16 source,
                                               bool decimal) {
  Cpu65816Add16Result result = {0};
  for (unsigned i = 0; i < kMetatilesPerStrip; i++) {
    result = Cpu65816_Add16(
        source, kColumnSourceStride, false, decimal);
    source = result.value;
  }
  return result;
}

static void PopulateMap(bool column, bool decimal, uint16 source) {
  for (unsigned i = 0; i < kMetatilesPerStrip; i++) {
    const uint8 metatile_id = (uint8)(0x20 + i);
    cpu_write16(NULL, kDataBank, source,
                (uint16)(0xA500u | metatile_id));
    WriteDefinition(metatile_id);
    if (column)
      source = Cpu65816_Add16(
          source, kColumnSourceStride, false, decimal).value;
    else
      source = (uint16)(source + 1);
  }
}

static void CheckOutput(bool column, uint16 output_origin,
                        uint16 preserved_bit_mask,
                        uint16 common_attribute_bits) {
  for (unsigned i = 0; i < kMetatilesPerStrip; i++) {
    const uint8 metatile_id = (uint8)(0x20 + i);
    const uint16 base = (uint16)(
        output_origin + i * kOutputAdvanceBytes);
    CHECK(cpu_read16(NULL, kDataBank, base) ==
          ComposedWord(metatile_id, 0, preserved_bit_mask,
                       common_attribute_bits));
    CHECK(cpu_read16(NULL, kDataBank,
                     (uint16)(base + kBottomRightTileOffset)) ==
          ComposedWord(metatile_id, 3, preserved_bit_mask,
                       common_attribute_bits));
    if (column) {
      CHECK(cpu_read16(NULL, kDataBank,
                       (uint16)(base + kBottomTileOffset)) ==
            ComposedWord(metatile_id, 1, preserved_bit_mask,
                         common_attribute_bits));
      CHECK(cpu_read16(NULL, kDataBank,
                       (uint16)(base + kRightTileOffset)) ==
            ComposedWord(metatile_id, 2, preserved_bit_mask,
                         common_attribute_bits));
    } else {
      CHECK(cpu_read16(NULL, kDataBank,
                       (uint16)(base + kRightTileOffset)) ==
            ComposedWord(metatile_id, 1, preserved_bit_mask,
                         common_attribute_bits));
      CHECK(cpu_read16(NULL, kDataBank,
                       (uint16)(base + kBottomTileOffset)) ==
            ComposedWord(metatile_id, 2, preserved_bit_mask,
                         common_attribute_bits));
    }
  }
}

static void CheckCpuCase(bool column, bool decimal, uint16 saved_x,
                         uint16 saved_y) {
  const uint16 preserved_bit_mask = 0xECFF;
  const uint16 common_attribute_bits = 0x2000;
  memset(memory, 0xCC, sizeof(memory));
  SetDescriptors(preserved_bit_mask, common_attribute_bits);
  PopulateMap(column, decimal, saved_y);
  CpuState cpu = MakeCpu(decimal, saved_x, saved_y);
  const uint16 entry_stack_pointer = cpu.S;
  const bool entry_overflow = cpu._flag_V != 0;
  ActionMetatileHle hle = column
      ? ActRaiser_ExpandActionBgMetatileColumn
      : ActRaiser_ExpandActionBgMetatileRow;

  CHECK(hle(&cpu) == RECOMP_RETURN_NORMAL);
  CheckOutput(column, saved_x, preserved_bit_mask, common_attribute_bits);
  CHECK(cpu_read16(NULL, kLowWramBank,
                   kDirectPage + kMetatilesRemainingDp) == 0);
  CHECK(cpu.X == saved_x);
  CHECK(cpu.Y == saved_y);
  CHECK(cpu.S == (uint16)(entry_stack_pointer + 2));
  CHECK(cpu.D == kDirectPage);
  CHECK(cpu.DB == kDataBank);
  CHECK(cpu.PB == 0x02);
  CHECK(cpu.host_return_valid == 0);

  CHECK(cpu_read8(NULL, kStackBank, entry_stack_pointer) ==
        (uint8)(saved_x >> CHAR_BIT));
  CHECK(cpu_read8(NULL, kStackBank,
                  (uint16)(entry_stack_pointer - 1)) == (uint8)saved_x);
  CHECK(cpu_read8(NULL, kStackBank,
                  (uint16)(entry_stack_pointer - 2)) ==
        (uint8)(saved_y >> CHAR_BIT));
  CHECK(cpu_read8(NULL, kStackBank,
                  (uint16)(entry_stack_pointer - 3)) == (uint8)saved_y);

  /* Column PHY occurs before ADC, so its last value is the source of command
   * 15. Row PHY occurs after INY, so its last value is source + 16. */
  const uint16 expected_inner_source = column
      ? AdvanceColumnSource(
            saved_y, decimal, kMetatilesPerStrip - 1)
      : (uint16)(saved_y + kMetatilesPerStrip);
  CHECK(cpu_read8(NULL, kStackBank,
                  (uint16)(entry_stack_pointer - 4)) ==
        (uint8)(expected_inner_source >> CHAR_BIT));
  CHECK(cpu_read8(NULL, kStackBank,
                  (uint16)(entry_stack_pointer - 5)) ==
        (uint8)expected_inner_source);

  bool expected_carry;
  bool expected_overflow;
  if (column) {
    const Cpu65816Add16Result final_advance =
        FinalColumnAdvance(saved_y, decimal);
    CHECK(cpu.A == final_advance.value);
    expected_carry = final_advance.carry;
    expected_overflow = final_advance.overflow;
  } else {
    CHECK(cpu.A == ComposedWord(
        (uint8)(0x20 + kMetatilesPerStrip - 1), 3,
        preserved_bit_mask, common_attribute_bits));
    expected_carry = false;
    expected_overflow = entry_overflow;
  }

  const uint8 expected_p = (uint8)(
      CPU_P_I | (decimal ? CPU_P_D : 0) |
      (expected_carry ? CPU_P_C : 0) |
      (expected_overflow ? CPU_P_V : 0) |
      (saved_x == 0 ? CPU_P_Z : 0) |
      (saved_x & 0x8000u ? CPU_P_N : 0));
  CHECK(cpu.P == expected_p);
  CHECK(cpu.m_flag == 0);
  CHECK(cpu.x_flag == 0);
  CHECK(cpu._flag_C == expected_carry);
  CHECK(cpu._flag_Z == (saved_x == 0));
  CHECK(cpu._flag_I == 1);
  CHECK(cpu._flag_D == decimal);
  CHECK(cpu._flag_V == expected_overflow);
  CHECK(cpu._flag_N == ((saved_x & 0x8000u) != 0));
}

int main(void) {
  CheckCpuCase(true, false, kOutputAddress, kMapAddress);
  CheckCpuCase(true, true, kOutputAddress, 0x0990);
  CheckCpuCase(false, false, kOutputAddress, kMapAddress);
  CheckCpuCase(false, false, 0, kMapAddress);
  if (failures) {
    printf("actraiser action-metatile HLE: %d failure(s)\n", failures);
    return 1;
  }
  printf("actraiser action-metatile HLE: all checks passed\n");
  return 0;
}
