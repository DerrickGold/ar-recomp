#include "actraiser/actraiser_town_structure_steps.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "cpu_65816_math.h"

enum {
  kTownStructureStepGate = 0x7BE9,
  kTownStructureStepRecordIndex = 0x7BE7,
  kTownStructureStepClassOffset = 0x7D1F,
  kTownStructureStepVariantOffset = 0x7D21,
  kTownStructureStepProgramRomBank = 0x03,
  kTownStructureStepConstructionClassTable = 0xD4D2,
  kTownStructureStepRebuildClassTable = 0xD4E2,
  kTownStructureStepSlotsBase = 0x77E7,
  kTownStructureStepSlotBytes = 8,
  kTownStructureStepCountdownOffset = 0,
  kTownStructureStepRepeatCountOffset = 1,
  kTownStructureStepCursorOffset = 2,
  kTownStructureStepLoopAddressOffset = 4,
  kTownStructureStepInitialCountdown = 1,
  kTownStructureStepInitialRepeatCount = 0,
  kAccumulatorHighByteMask = 0xFF00,
  kWordBits = sizeof(uint16_t) * CHAR_BIT,
  kWordSignBit = 1u << (kWordBits - 1),
};

static uint16_t TownStructureStepClassTable(
    ActRaiserTownStructureStepProgramFamily program_family) {
  switch (program_family) {
    case kActRaiserTownStructureStepProgramFamily_Construction:
      return kTownStructureStepConstructionClassTable;
    case kActRaiserTownStructureStepProgramFamily_Rebuild:
      return kTownStructureStepRebuildClassTable;
  }

  fprintf(stderr, "FATAL: invalid town structure step program family %d\n",
          (int)program_family);
  abort();
}

static uint16_t ReadLongIndexedWord(CpuState *cpu, uint8_t bank,
                                    uint16_t base, uint16_t index) {
  const uint32_t address =
      ((uint32_t)bank << kWordBits) + base + index;
  return cpu_read16(cpu, (uint8_t)(address >> kWordBits),
                    (uint16_t)address);
}

static void WriteDataBankIndexedByte(CpuState *cpu, uint16_t base,
                                     uint16_t index, uint8_t value) {
  const uint32_t address =
      ((uint32_t)cpu->DB << kWordBits) + base + index;
  cpu_write8(cpu, (uint8_t)(address >> kWordBits),
             (uint16_t)address, value);
}

static void WriteDataBankIndexedWord(CpuState *cpu, uint16_t base,
                                     uint16_t index, uint16_t value) {
  const uint32_t address =
      ((uint32_t)cpu->DB << kWordBits) + base + index;
  cpu_write16(cpu, (uint8_t)(address >> kWordBits),
              (uint16_t)address, value);
}

static uint16_t ResolveTownStructureStepProgramAtAddress(
    CpuState *cpu, uint16_t class_offset,
    uint16_t class_pointer_table_address,
    uint16_t variant_offset, bool decimal) {
  const uint16_t variant_pointer_table_address = ReadLongIndexedWord(
      cpu, kTownStructureStepProgramRomBank, class_pointer_table_address,
      class_offset);
  const Cpu65816Add16Result program_pointer_address = Cpu65816_Add16(
      variant_pointer_table_address, variant_offset, false, decimal);
  return ReadLongIndexedWord(cpu, kTownStructureStepProgramRomBank, 0,
                             program_pointer_address.value);
}

uint16_t ActRaiser_ResolveTownStructureStepProgram(
    CpuState *cpu, uint16_t class_offset, uint16_t variant_offset,
    ActRaiserTownStructureStepProgramFamily program_family) {
  if (!cpu) return 0;
  return ResolveTownStructureStepProgramAtAddress(
      cpu, class_offset, TownStructureStepClassTable(program_family),
      variant_offset, false);
}

static uint16_t TownStructureStepSlotAddress(uint8_t record_index) {
  return (uint16_t)(kTownStructureStepSlotsBase +
                    record_index * kTownStructureStepSlotBytes);
}

static void WriteTownStructureStepSlot(CpuState *cpu,
                                       uint8_t destination_bank,
                                       uint16_t slot_address,
                                       uint16_t program_address) {
  cpu_write8(cpu, destination_bank,
             (uint16_t)(slot_address + kTownStructureStepCountdownOffset),
             kTownStructureStepInitialCountdown);
  cpu_write8(cpu, destination_bank,
             (uint16_t)(slot_address + kTownStructureStepRepeatCountOffset),
             kTownStructureStepInitialRepeatCount);
  cpu_write16(cpu, destination_bank,
              (uint16_t)(slot_address + kTownStructureStepCursorOffset),
              program_address);
  cpu_write16(cpu, destination_bank,
              (uint16_t)(slot_address + kTownStructureStepLoopAddressOffset),
              program_address);
}

void ActRaiser_ArmTownStructureStepProgram(
    CpuState *cpu, uint8_t destination_bank, uint8_t record_index,
    uint16_t program_address) {
  if (!cpu) return;
  WriteTownStructureStepSlot(
      cpu, destination_bank, TownStructureStepSlotAddress(record_index),
      program_address);
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

static void RequireTownStructureStepEntryMode(CpuState *cpu,
                                               const char *routine_name) {
  if (!cpu->m_flag && !cpu->x_flag && !cpu->emulation) return;
  fprintf(stderr,
          "FATAL: %s HLE requires native mode with 16-bit A/X/Y\n",
          routine_name);
  abort();
}

/* $03:A4A8 and $03:A4B8 share the same gated slot-armer body. Their sole
 * semantic difference is the rebuild ($D4E2) versus construction ($D4D2)
 * class table. This wrapper retains the native temporary pushes, decimal ADC
 * edges, DB-relative writes, register/flag results, and outer RTS. */
static RecompReturn ArmTownStructureStepProgramHle(
    CpuState *cpu,
    ActRaiserTownStructureStepProgramFamily program_family,
    const char *routine_name) {
  if (!cpu) return RECOMP_RETURN_NORMAL;

  cpu_mirrors_to_p(cpu);
  RequireTownStructureStepEntryMode(cpu, routine_name);

  const uint16_t gate =
      cpu_read16(cpu, cpu->DB, kTownStructureStepGate);
  cpu->A = gate;
  SetCpuWordNegativeZero(cpu, gate);
  if (gate != 0) {
    cpu_p_to_mirrors(cpu);
    cpu->S = (uint16_t)(cpu->S + k65816RtsStackBytes);
    return RECOMP_RETURN_NORMAL;
  }

  const uint16_t saved_x = cpu->X;
  PushCpuWord(cpu, saved_x);

  const uint16_t class_offset =
      cpu_read16(cpu, cpu->DB, kTownStructureStepClassOffset);
  const uint16_t variant_pointer_table_address = ReadLongIndexedWord(
      cpu, kTownStructureStepProgramRomBank,
      TownStructureStepClassTable(program_family), class_offset);
  PushCpuWord(cpu, variant_pointer_table_address);

  const uint16_t variant_offset =
      cpu_read16(cpu, cpu->DB, kTownStructureStepVariantOffset);
  const Cpu65816Add16Result program_pointer_address = Cpu65816_Add16(
      variant_offset, variant_pointer_table_address, false,
      cpu->_flag_D != 0);
  const uint16_t program_address = ReadLongIndexedWord(
      cpu, kTownStructureStepProgramRomBank, 0,
      program_pointer_address.value);
  PopCpuWord(cpu);

  const uint8_t record_index = (uint8_t)cpu_read16(
      cpu, cpu->DB, kTownStructureStepRecordIndex);
  const uint16_t scaled_record_index =
      (uint16_t)(record_index * kTownStructureStepSlotBytes);
  const Cpu65816Add16Result slot_address = Cpu65816_Add16(
      scaled_record_index, kTownStructureStepSlotsBase, false,
      cpu->_flag_D != 0);

  WriteDataBankIndexedWord(
      cpu, kTownStructureStepCursorOffset, slot_address.value,
      program_address);
  WriteDataBankIndexedWord(
      cpu, kTownStructureStepLoopAddressOffset, slot_address.value,
      program_address);
  WriteDataBankIndexedByte(
      cpu, kTownStructureStepRepeatCountOffset, slot_address.value,
      kTownStructureStepInitialRepeatCount);
  WriteDataBankIndexedByte(
      cpu, kTownStructureStepCountdownOffset, slot_address.value,
      kTownStructureStepInitialCountdown);

  cpu->A = (uint16_t)((program_address & kAccumulatorHighByteMask) |
                      kTownStructureStepInitialCountdown);
  cpu->Y = slot_address.value;
  cpu->X = PopCpuWord(cpu);
  cpu->P = (uint8_t)((cpu->P & (CPU_P_I | CPU_P_D)) |
                     (slot_address.carry ? CPU_P_C : 0) |
                     (slot_address.overflow ? CPU_P_V : 0));
  SetCpuWordNegativeZero(cpu, cpu->X);
  cpu_p_to_mirrors(cpu);
  cpu->S = (uint16_t)(cpu->S + k65816RtsStackBytes);
  return RECOMP_RETURN_NORMAL;
}

RecompReturn ActRaiser_TownArmRebuildStepProgram(CpuState *cpu) {
  return ArmTownStructureStepProgramHle(
      cpu, kActRaiserTownStructureStepProgramFamily_Rebuild, "$03:A4A8");
}

RecompReturn ActRaiser_TownArmConstructionStepProgram(CpuState *cpu) {
  return ArmTownStructureStepProgramHle(
      cpu, kActRaiserTownStructureStepProgramFamily_Construction,
      "$03:A4B8");
}
