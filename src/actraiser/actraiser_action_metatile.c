#include "actraiser/actraiser_action_metatile.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "action/action_bg_metatile.h"
#include "constants.h"
#include "cpu_65816_math.h"

enum {
  kActionBgCommonAttributeBits = 0x06,
  kActionBgPreservedBitMask = 0x0A,
  kActionBgStripMetatilesRemaining = 0x0E,
  kActionBgMetatileDefinitionPointer = 0xA5,
  kActionBgMetatilesPerStrip = 16,
  kActionBgMetatileDefinitionBytes = 8,
  kActionBgTilemapWordBytes = sizeof(uint16_t),
  kActionBgMetatileOutputAdvanceBytes = 4,
  kActionBgOutputRightTileOffset = 2,
  kActionBgOutputBottomTileOffset = 0x40,
  kActionBgOutputBottomRightTileOffset =
      kActionBgOutputBottomTileOffset + kActionBgOutputRightTileOffset,
  kActionBgColumnSourceMetatileStride = 0x10,
  kWordBits = sizeof(uint16_t) * CHAR_BIT,
  kWordSignBit = 1u << (kWordBits - 1),
};

typedef enum ActionBgStripOrientation {
  kActionBgStripOrientation_Column = 0,
  kActionBgStripOrientation_Row,
} ActionBgStripOrientation;

static void RequireActionBgMetatileEntryMode(CpuState *cpu,
                                              const char *routine_name) {
  if (!cpu->m_flag && !cpu->x_flag && !cpu->emulation) return;
  fprintf(stderr,
          "FATAL: %s HLE requires native mode with 16-bit A/X/Y\n",
          routine_name);
  abort();
}

static uint16_t ReadDirectPageWord(CpuState *cpu, uint16_t offset) {
  return cpu_read16_dp(cpu, (uint16_t)(cpu->D + offset));
}

static void WriteDirectPageWord(CpuState *cpu, uint16_t offset,
                                uint16_t value) {
  cpu_write16(cpu, kSnesLowWramBank, (uint16_t)(cpu->D + offset), value);
}

static uint16_t ReadDataBankIndexedWord(CpuState *cpu, uint16_t base,
                                        uint16_t index) {
  const uint32_t address =
      ((uint32_t)cpu->DB << kWordBits) + base + index;
  return cpu_read16(cpu, (uint8_t)(address >> kWordBits),
                    (uint16_t)address);
}

static void WriteDataBankIndexedWord(CpuState *cpu, uint16_t base,
                                     uint16_t index, uint16_t value) {
  const uint32_t address =
      ((uint32_t)cpu->DB << kWordBits) + base + index;
  cpu_write16(cpu, (uint8_t)(address >> kWordBits),
              (uint16_t)address, value);
}

static void PushCpuWord(CpuState *cpu, uint16_t value) {
  cpu_write8(cpu, k65816StackBank, cpu->S,
             (uint8_t)(value >> CHAR_BIT));
  cpu->S = (uint16_t)(cpu->S - 1);
  cpu_write8(cpu, k65816StackBank, cpu->S, (uint8_t)value);
  cpu->S = (uint16_t)(cpu->S - 1);
}

static uint16_t PopCpuWord(CpuState *cpu) {
  cpu->S = (uint16_t)(cpu->S + 1);
  const uint16_t value = cpu_read16(cpu, k65816StackBank, cpu->S);
  cpu->S = (uint16_t)(cpu->S + 1);
  return value;
}

static void SetCpuWordNegativeZero(CpuState *cpu, uint16_t value) {
  cpu->_flag_Z = value == 0;
  cpu->_flag_N = (value & kWordSignBit) != 0;
  cpu->P = (uint8_t)((cpu->P & ~(CPU_P_N | CPU_P_Z)) |
                     (cpu->_flag_N ? CPU_P_N : 0) |
                     (cpu->_flag_Z ? CPU_P_Z : 0));
}

static RecompReturn ExpandActionBgMetatileStrip(
    CpuState *cpu, ActionBgStripOrientation orientation,
    const char *routine_name) {
  if (!cpu) return RECOMP_RETURN_NORMAL;

  cpu_mirrors_to_p(cpu);
  RequireActionBgMetatileEntryMode(cpu, routine_name);

  const uint16_t saved_x = cpu->X;
  const uint16_t saved_y = cpu->Y;
  const bool saved_overflow = cpu->_flag_V != 0;
  PushCpuWord(cpu, saved_x);
  PushCpuWord(cpu, saved_y);

  const uint16_t definition_base = ReadDirectPageWord(
      cpu, kActionBgMetatileDefinitionPointer);
  const uint16_t preserved_bit_mask = ReadDirectPageWord(
      cpu, kActionBgPreservedBitMask);
  const uint16_t common_attribute_bits = ReadDirectPageWord(
      cpu, kActionBgCommonAttributeBits);
  WriteDirectPageWord(cpu, kActionBgStripMetatilesRemaining,
                      kActionBgMetatilesPerStrip);

  uint16_t source_address = saved_y;
  uint16_t output_address = saved_x;
  uint16_t final_accumulator = 0;
  bool final_carry = false;
  bool final_overflow = saved_overflow;

  for (unsigned metatile_index = 0;
       metatile_index < kActionBgMetatilesPerStrip; metatile_index++) {
    const uint8_t metatile_id = (uint8_t)ReadDataBankIndexedWord(
        cpu, 0, source_address);
    if (orientation == kActionBgStripOrientation_Row)
      source_address = (uint16_t)(source_address + 1);
    PushCpuWord(cpu, source_address);

    const uint16_t metatile_definition_offset = (uint16_t)(
        metatile_id * kActionBgMetatileDefinitionBytes);
    uint16_t composed_words[4];
    for (unsigned tile = 0; tile < 4; tile++) {
      const uint16_t definition_word = ReadDataBankIndexedWord(
          cpu, definition_base,
          (uint16_t)(metatile_definition_offset +
                     tile * kActionBgTilemapWordBytes));
      composed_words[tile] = ActionBg_ComposeTilemapWord(
          definition_word, preserved_bit_mask, common_attribute_bits);
    }

    if (orientation == kActionBgStripOrientation_Column) {
      WriteDataBankIndexedWord(cpu, 0, output_address,
                               composed_words[0]);
      WriteDataBankIndexedWord(
          cpu, kActionBgOutputBottomTileOffset, output_address,
          composed_words[1]);
      WriteDataBankIndexedWord(
          cpu, kActionBgOutputRightTileOffset, output_address,
          composed_words[2]);
    } else {
      WriteDataBankIndexedWord(cpu, 0, output_address,
                               composed_words[0]);
      WriteDataBankIndexedWord(
          cpu, kActionBgOutputRightTileOffset, output_address,
          composed_words[1]);
      WriteDataBankIndexedWord(
          cpu, kActionBgOutputBottomTileOffset, output_address,
          composed_words[2]);
    }
    WriteDataBankIndexedWord(
        cpu, kActionBgOutputBottomRightTileOffset, output_address,
        composed_words[3]);
    final_accumulator = composed_words[3];
    output_address = (uint16_t)(
        output_address + kActionBgMetatileOutputAdvanceBytes);

    source_address = PopCpuWord(cpu);
    if (orientation == kActionBgStripOrientation_Column) {
      const Cpu65816Add16Result next_source = Cpu65816_Add16(
          source_address, kActionBgColumnSourceMetatileStride, false,
          cpu->_flag_D != 0);
      source_address = next_source.value;
      final_accumulator = next_source.value;
      final_carry = next_source.carry;
      final_overflow = next_source.overflow;
    } else {
      /* The third ASL of an eight-bit metatile id always clears carry. */
      final_carry = false;
    }

    WriteDirectPageWord(
        cpu, kActionBgStripMetatilesRemaining,
        (uint16_t)(kActionBgMetatilesPerStrip - metatile_index - 1));
  }

  cpu->Y = PopCpuWord(cpu);
  cpu->X = PopCpuWord(cpu);
  cpu->A = final_accumulator;
  cpu->_flag_C = final_carry;
  cpu->_flag_V = final_overflow;
  cpu->P = (uint8_t)((cpu->P & ~(CPU_P_C | CPU_P_V)) |
                     (final_carry ? CPU_P_C : 0) |
                     (final_overflow ? CPU_P_V : 0));
  SetCpuWordNegativeZero(cpu, cpu->X);
  cpu_p_to_mirrors(cpu);
  cpu->S = (uint16_t)(cpu->S + k65816RtsStackBytes);
  return RECOMP_RETURN_NORMAL;
}

RecompReturn ActRaiser_ExpandActionBgMetatileColumn(CpuState *cpu) {
  return ExpandActionBgMetatileStrip(
      cpu, kActionBgStripOrientation_Column, "$02:B90D");
}

RecompReturn ActRaiser_ExpandActionBgMetatileRow(CpuState *cpu) {
  return ExpandActionBgMetatileStrip(
      cpu, kActionBgStripOrientation_Row, "$02:B95A");
}
