#include "actraiser/actraiser_town_structure_steps.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
  kStackBank = 0x00,
  kProgramRomBank = 0x03,
  kProgramRomOverflowBank = 0x04,
  kLowWramBank = 0x7E,
  kHighWramBank = 0x7F,
  kBankBytes = 0x10000,
  kMappedBankCount = 5,
  kConstructionClassTable = 0xD4D2,
  kRebuildClassTable = 0xD4E2,
  kStepSlotsBase = 0x77E7,
  kStepSlotBytes = 8,
  kStepCountdownOffset = 0,
  kStepRepeatCountOffset = 1,
  kStepCursorOffset = 2,
  kStepLoopAddressOffset = 4,
  kStepReservedOffset = 6,
  kStepReservedBytes = 2,
  kStepGate = 0x7BE9,
  kStepRecordIndex = 0x7BE7,
  kStepClassOffset = 0x7D1F,
  kStepVariantOffset = 0x7D21,
  kInitialCountdown = 1,
  kInitialRepeatCount = 0,
  kAccumulatorHighByteMask = 0xFF00,
  kRtsStackBytes = 2,
};

typedef RecompReturn (*TownStructureStepHle)(CpuState *cpu);

typedef struct ReferenceAdd16Result {
  uint16_t value;
  bool carry;
  bool overflow;
} ReferenceAdd16Result;

static uint8_t memory[kMappedBankCount * kBankBytes];
static int failures;

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      failures++;                                                          \
    }                                                                      \
  } while (0)

static size_t BankOffset(uint8_t bank, uint16_t address) {
  switch (bank) {
    case kStackBank:
      return address;
    case kLowWramBank:
      return kBankBytes + address;
    case kHighWramBank:
      return 2 * kBankBytes + address;
    case kProgramRomBank:
      return 3 * kBankBytes + address;
    case kProgramRomOverflowBank:
      return 4 * kBankBytes + address;
  }
  return 0;
}

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu;
  return memory[BankOffset(bank, address)];
}

uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  const uint8 low = cpu_read8(cpu, bank, address);
  const uint8 high = cpu_read8(cpu, bank, (uint16)(address + 1));
  return (uint16)(low | ((uint16)high << 8));
}

void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu;
  memory[BankOffset(bank, address)] = value;
}

void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(cpu, bank, address, (uint8)value);
  cpu_write8(cpu, bank, (uint16)(address + 1), (uint8)(value >> 8));
}

static ReferenceAdd16Result ReferenceAdd16(uint16_t left, uint16_t right,
                                            bool decimal) {
  ReferenceAdd16Result result = { 0 };
  const uint32_t binary_sum = (uint32_t)left + right;
  if (!decimal) {
    result.value = (uint16_t)binary_sum;
    result.carry = binary_sum > UINT16_MAX;
    result.overflow =
        ((left ^ result.value) & (right ^ result.value) & 0x8000u) != 0;
    return result;
  }

  uint32_t adjusted = 0;
  unsigned carry = 0;
  unsigned carry_into_sign_digit = 0;
  for (unsigned shift = 0; shift < 16; shift += 4) {
    unsigned digit =
        ((left >> shift) & 15u) + ((right >> shift) & 15u) + carry;
    carry = digit > 9;
    if (shift == 8) carry_into_sign_digit = carry;
    if (carry) digit += 6;
    adjusted |= (uint32_t)(digit & 15u) << shift;
  }
  result.value = (uint16_t)adjusted;
  result.carry = carry != 0;
  const uint32_t sign_sum =
      (left & 0xF000u) + (right & 0xF000u) +
      (carry_into_sign_digit << 12);
  result.overflow =
      ((left ^ sign_sum) & (right ^ sign_sum) & 0x8000u) != 0;
  return result;
}

static CpuState NewCpu(uint8_t data_bank, uint8_t status) {
  CpuState cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.ram = memory;
  cpu.DB = data_bank;
  cpu.PB = kProgramRomBank;
  cpu.P = status;
  cpu.S = 0x1FF0;
  cpu.host_return_valid = 1;
  cpu_p_to_mirrors(&cpu);
  return cpu;
}

static uint16_t SlotAddress(uint8_t record_index) {
  return (uint16_t)(kStepSlotsBase + record_index * kStepSlotBytes);
}

static void WriteProgramResolution(uint8_t class_table_bank,
                                   uint16_t class_table_address,
                                   uint16_t variant_table_address,
                                   uint16_t variant_offset,
                                   uint16_t program_address) {
  cpu_write16(NULL, class_table_bank, class_table_address,
              variant_table_address);
  cpu_write16(NULL, kProgramRomBank,
              (uint16_t)(variant_table_address + variant_offset),
              program_address);
}

static void CheckSemanticResolverAndArmer(void) {
  const uint16_t class_offset = 6;
  const uint16_t variant_offset = 10;
  const uint16_t construction_variant_table = 0xD800;
  const uint16_t rebuild_variant_table = 0xD900;
  const uint16_t construction_program = 0x8123;
  const uint16_t rebuild_program = 0x9345;
  const uint8_t record_index = 5;
  const uint16_t slot_address = SlotAddress(record_index);

  memset(memory, 0, sizeof(memory));
  WriteProgramResolution(
      kProgramRomBank, (uint16_t)(kConstructionClassTable + class_offset),
      construction_variant_table, variant_offset, construction_program);
  WriteProgramResolution(
      kProgramRomBank, (uint16_t)(kRebuildClassTable + class_offset),
      rebuild_variant_table, variant_offset, rebuild_program);

  CpuState cpu = NewCpu(kHighWramBank, CPU_P_I | CPU_P_C);
  cpu.A = 0x1234;
  cpu.X = 0x5678;
  cpu.Y = 0x9ABC;
  const CpuState initial_cpu = cpu;
  CHECK(ActRaiser_ResolveTownStructureStepProgram(
            &cpu, class_offset, variant_offset,
            kActRaiserTownStructureStepProgramFamily_Construction) ==
        construction_program);
  CHECK(ActRaiser_ResolveTownStructureStepProgram(
            &cpu, class_offset, variant_offset,
            kActRaiserTownStructureStepProgramFamily_Rebuild) ==
        rebuild_program);
  CHECK(memcmp(&cpu, &initial_cpu, sizeof(cpu)) == 0);

  memset(&memory[BankOffset(kLowWramBank, slot_address)], 0xA5,
         kStepSlotBytes);
  ActRaiser_ArmTownStructureStepProgram(
      &cpu, kLowWramBank, record_index, rebuild_program);
  CHECK(cpu_read8(NULL, kLowWramBank,
                  slot_address + kStepCountdownOffset) ==
        kInitialCountdown);
  CHECK(cpu_read8(NULL, kLowWramBank,
                  slot_address + kStepRepeatCountOffset) ==
        kInitialRepeatCount);
  CHECK(cpu_read16(NULL, kLowWramBank,
                   slot_address + kStepCursorOffset) == rebuild_program);
  CHECK(cpu_read16(NULL, kLowWramBank,
                   slot_address + kStepLoopAddressOffset) ==
        rebuild_program);
  for (unsigned byte = 0; byte < kStepReservedBytes; byte++) {
    CHECK(cpu_read8(NULL, kLowWramBank,
                    (uint16_t)(slot_address + kStepReservedOffset + byte)) ==
          0xA5);
  }
  CHECK(memcmp(&cpu, &initial_cpu, sizeof(cpu)) == 0);

  CHECK(ActRaiser_ResolveTownStructureStepProgram(
            NULL, class_offset, variant_offset,
            kActRaiserTownStructureStepProgramFamily_Rebuild) == 0);
  ActRaiser_ArmTownStructureStepProgram(
      NULL, kLowWramBank, record_index, rebuild_program);
}

static void CheckArmedSlot(uint8_t bank, uint16_t slot_address,
                           uint16_t program_address,
                           uint8_t reserved_fill) {
  CHECK(cpu_read8(NULL, bank, slot_address + kStepCountdownOffset) ==
        kInitialCountdown);
  CHECK(cpu_read8(NULL, bank, slot_address + kStepRepeatCountOffset) ==
        kInitialRepeatCount);
  CHECK(cpu_read16(NULL, bank, slot_address + kStepCursorOffset) ==
        program_address);
  CHECK(cpu_read16(NULL, bank, slot_address + kStepLoopAddressOffset) ==
        program_address);
  for (unsigned byte = 0; byte < kStepReservedBytes; byte++) {
    CHECK(cpu_read8(NULL, bank,
                    (uint16_t)(slot_address + kStepReservedOffset + byte)) ==
          reserved_fill);
  }
}

static void CheckCpuBody(TownStructureStepHle hle, uint16_t class_offset,
                         uint8_t class_table_bank,
                         uint16_t class_table_address, uint16_t saved_x,
                         uint8_t record_index) {
  const uint16_t variant_offset = 4;
  const uint16_t variant_table_address = 0xD700;
  const uint16_t program_address = 0xE2A5;
  const uint16_t slot_address = SlotAddress(record_index);
  const uint8_t slot_fill = 0xA5;
  const uint16_t entry_stack = 0x1FF0;

  memset(memory, 0, sizeof(memory));
  WriteProgramResolution(class_table_bank, class_table_address,
                         variant_table_address, variant_offset,
                         program_address);
  cpu_write16(NULL, kHighWramBank, kStepGate, 0);
  cpu_write16(NULL, kHighWramBank, kStepClassOffset, class_offset);
  cpu_write16(NULL, kHighWramBank, kStepVariantOffset, variant_offset);
  cpu_write16(NULL, kHighWramBank, kStepRecordIndex,
              (uint16_t)(0xA500u | record_index));
  memset(&memory[BankOffset(kHighWramBank, slot_address)], slot_fill,
         kStepSlotBytes);

  CpuState cpu = NewCpu(
      kHighWramBank, CPU_P_I | CPU_P_C | CPU_P_V | CPU_P_N | CPU_P_Z);
  cpu.A = 0x1357;
  cpu.X = saved_x;
  cpu.Y = 0xBEEF;
  cpu.D = 0x2468;
  const uint8_t initial_host_return_valid = cpu.host_return_valid;

  CHECK(hle(&cpu) == RECOMP_RETURN_NORMAL);
  CheckArmedSlot(kHighWramBank, slot_address, program_address, slot_fill);
  CHECK(cpu.A ==
        (uint16_t)((program_address & kAccumulatorHighByteMask) |
                   kInitialCountdown));
  CHECK(cpu.X == saved_x);
  CHECK(cpu.Y == slot_address);
  CHECK(cpu.S == entry_stack + kRtsStackBytes);
  CHECK(cpu.D == 0x2468);
  CHECK(cpu.DB == kHighWramBank);
  CHECK(cpu.PB == kProgramRomBank);
  CHECK(cpu.host_return_valid == initial_host_return_valid);

  uint8_t expected_status = CPU_P_I;
  if (saved_x == 0) expected_status |= CPU_P_Z;
  if (saved_x & 0x8000u) expected_status |= CPU_P_N;
  CHECK(cpu.P == expected_status);
  CHECK(cpu._flag_C == 0);
  CHECK(cpu._flag_V == 0);
  CHECK(cpu._flag_Z == (saved_x == 0));
  CHECK(cpu._flag_N == ((saved_x & 0x8000u) != 0));
  CHECK(cpu.m_flag == 0);
  CHECK(cpu.x_flag == 0);
  CHECK(cpu.emulation == 0);

  CHECK(cpu_read8(NULL, kStackBank, entry_stack) ==
        (uint8_t)(saved_x >> 8));
  CHECK(cpu_read8(NULL, kStackBank, entry_stack - 1) ==
        (uint8_t)saved_x);
  CHECK(cpu_read8(NULL, kStackBank, entry_stack - 2) ==
        (uint8_t)(variant_table_address >> 8));
  CHECK(cpu_read8(NULL, kStackBank, entry_stack - 3) ==
        (uint8_t)variant_table_address);
}

static void CheckBinaryCpuBodies(void) {
  const uint16_t construction_class_offset = 6;
  CheckCpuBody(
      ActRaiser_TownArmConstructionStepProgram, construction_class_offset,
      kProgramRomBank,
      (uint16_t)(kConstructionClassTable + construction_class_offset),
      0, 7);

  /* Long,X class-table reads carry into bank $04 when the synthetic class
   * offset crosses the end of bank $03. */
  const uint16_t rebuild_class_offset =
      (uint16_t)(kBankBytes - kRebuildClassTable);
  CheckCpuBody(
      ActRaiser_TownArmRebuildStepProgram, rebuild_class_offset,
      kProgramRomOverflowBank, 0,
      0x8A20, 127);
}

static void CheckGateEarlyReturn(void) {
  const uint16_t gate_value = 0x8000;
  const uint16_t entry_stack = 0x1FF0;

  memset(memory, 0x5A, sizeof(memory));
  cpu_write16(NULL, kHighWramBank, kStepGate, gate_value);
  uint8_t initial_memory[sizeof(memory)];
  memcpy(initial_memory, memory, sizeof(memory));

  CpuState cpu = NewCpu(
      kHighWramBank,
      CPU_P_I | CPU_P_D | CPU_P_C | CPU_P_V | CPU_P_Z);
  cpu.A = 0x1234;
  cpu.X = 0x5678;
  cpu.Y = 0x9ABC;
  const uint8_t initial_host_return_valid = cpu.host_return_valid;

  CHECK(ActRaiser_TownArmConstructionStepProgram(&cpu) ==
        RECOMP_RETURN_NORMAL);
  CHECK(memcmp(memory, initial_memory, sizeof(memory)) == 0);
  CHECK(cpu.A == gate_value);
  CHECK(cpu.X == 0x5678);
  CHECK(cpu.Y == 0x9ABC);
  CHECK(cpu.S == entry_stack + kRtsStackBytes);
  CHECK(cpu.P == (CPU_P_I | CPU_P_D | CPU_P_C | CPU_P_V | CPU_P_N));
  CHECK(cpu._flag_C == 1);
  CHECK(cpu._flag_V == 1);
  CHECK(cpu._flag_Z == 0);
  CHECK(cpu._flag_N == 1);
  CHECK(cpu.host_return_valid == initial_host_return_valid);
}

static void CheckDecimalCpuBody(void) {
  const uint16_t class_offset = 2;
  const uint16_t variant_offset = 1;
  const uint16_t variant_table_address = 0x1999;
  const uint16_t program_address = 0xB4C2;
  const uint8_t record_index = 3;
  const uint16_t scaled_record_index =
      (uint16_t)(record_index * kStepSlotBytes);
  const ReferenceAdd16Result program_pointer_address = ReferenceAdd16(
      variant_offset, variant_table_address, true);
  const ReferenceAdd16Result slot_address = ReferenceAdd16(
      scaled_record_index, kStepSlotsBase, true);
  const uint8_t slot_fill = 0xCC;
  const uint16_t entry_stack = 0x1FF0;

  memset(memory, 0, sizeof(memory));
  cpu_write16(NULL, kProgramRomBank,
              (uint16_t)(kRebuildClassTable + class_offset),
              variant_table_address);
  cpu_write16(NULL, kProgramRomBank, program_pointer_address.value,
              program_address);
  cpu_write16(NULL, kLowWramBank, kStepGate, 0);
  cpu_write16(NULL, kLowWramBank, kStepClassOffset, class_offset);
  cpu_write16(NULL, kLowWramBank, kStepVariantOffset, variant_offset);
  cpu_write16(NULL, kLowWramBank, kStepRecordIndex, record_index);
  memset(&memory[BankOffset(kLowWramBank, slot_address.value)], slot_fill,
         kStepSlotBytes);

  CpuState cpu = NewCpu(kLowWramBank, CPU_P_I | CPU_P_D | CPU_P_C);
  cpu.A = 0x7777;
  cpu.X = 0x8000;
  cpu.Y = 0x2222;

  CHECK(ActRaiser_TownArmRebuildStepProgram(&cpu) ==
        RECOMP_RETURN_NORMAL);
  CheckArmedSlot(kLowWramBank, slot_address.value,
                 program_address, slot_fill);
  CHECK(program_pointer_address.value == 0x2000);
  CHECK(cpu.A ==
        (uint16_t)((program_address & kAccumulatorHighByteMask) |
                   kInitialCountdown));
  CHECK(cpu.X == 0x8000);
  CHECK(cpu.Y == slot_address.value);
  CHECK(cpu.S == entry_stack + kRtsStackBytes);

  const uint8_t expected_status =
      (uint8_t)(CPU_P_I | CPU_P_D | CPU_P_N |
                (slot_address.carry ? CPU_P_C : 0) |
                (slot_address.overflow ? CPU_P_V : 0));
  CHECK(cpu.P == expected_status);
  CHECK(cpu._flag_C == slot_address.carry);
  CHECK(cpu._flag_V == slot_address.overflow);
  CHECK(cpu._flag_Z == 0);
  CHECK(cpu._flag_N == 1);
  CHECK(cpu_read8(NULL, kStackBank, entry_stack - 2) ==
        (uint8_t)(variant_table_address >> 8));
  CHECK(cpu_read8(NULL, kStackBank, entry_stack - 3) ==
        (uint8_t)variant_table_address);
}

int main(void) {
  CheckSemanticResolverAndArmer();
  CheckBinaryCpuBodies();
  CheckGateEarlyReturn();
  CheckDecimalCpuBody();

  if (failures != 0) {
    printf("%d town structure-step HLE test(s) failed\n", failures);
    return 1;
  }
  puts("town structure-step HLE tests passed");
  return 0;
}
