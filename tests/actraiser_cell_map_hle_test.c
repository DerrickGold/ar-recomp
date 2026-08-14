#include "actraiser/actraiser_cell_map.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
  kStackBank = 0x00,
  kLowWramBank = 0x7E,
  kHighWramBank = 0x7F,
  kDataBankOverflowBank = 0x80,
  kBankAddressBits = 16,
  kTownIndex = 0x7BFB,
  kScratch = 0x7C05,
  kCellX = 0x7C4B,
  kCellY = 0x7C4D,
  kTerrainMapBase = 0x2000,
  kCellFlagsBase = 0x3800,
  kMetatileDefinitionsBase = 0x2100,
  kMetatileDefinitionBytes = 8,
  kMetatileCollisionBit = 0x0200,
  kTraversalVisitedBit = 0x04,
  kNestedCellIndexReturnAddress = 0x96F1,
  kStructureMarkCellReturnAddress = 0x9FDD,
  kStructureMarkBlockReturnAddress = 0x9FF4,
  kStructureMarkMapBase = 0x2000,
  kStructureMarkBlockRowStride = 0x10,
};

static uint8_t memory[0x30000];
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
  if (bank == kHighWramBank) return 0x10000u + address;
  if (bank == kDataBankOverflowBank) return 0x20000u + address;
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

static void WriteDataBankIndexedByte(uint8 data_bank, uint16 base_address,
                                     uint16 index, uint8 value) {
  const uint32 effective_address =
      ((uint32)data_bank << kBankAddressBits) + base_address + index;
  cpu_write8(NULL, (uint8)(effective_address >> kBankAddressBits),
             (uint16)effective_address, value);
}

typedef struct ReferenceAdd {
  uint16 value;
  bool carry;
  bool overflow;
} ReferenceAdd;

typedef struct ReferenceResult {
  uint16 scratch;
  ReferenceAdd final;
} ReferenceResult;

typedef struct ReferenceTraversalResult {
  ReferenceResult cell_index;
  uint16 metatile_definition_offset;
  uint16 final_accumulator;
  bool blocked;
  bool negative;
} ReferenceTraversalResult;

static uint16 Swap16(uint16 value) {
  return (uint16)((value << 8) | (value >> 8));
}

/* Independent instruction-level oracle for the three CLC/ADC pairs. */
static ReferenceAdd ReferenceAdd16(uint16 a, uint16 b, bool decimal) {
  ReferenceAdd result = { 0 };
  if (!decimal) {
    const uint32 sum = (uint32)a + b;
    result.value = (uint16)sum;
    result.carry = (sum & 0x10000u) != 0;
    result.overflow = ((a ^ result.value) & (b ^ result.value) & 0x8000u) != 0;
    return result;
  }

  uint32 adjusted = 0;
  unsigned carry = 0;
  unsigned carry_into_sign_digit = 0;
  for (unsigned shift = 0; shift < 16; shift += 4) {
    unsigned digit = ((a >> shift) & 15u) + ((b >> shift) & 15u) + carry;
    carry = digit > 9;
    if (shift == 8) carry_into_sign_digit = carry;
    if (carry) digit += 6;
    adjusted |= (uint32)(digit & 15u) << shift;
  }
  result.value = (uint16)adjusted;
  result.carry = carry != 0;
  const uint32 sign_sum =
      (a & 0xF000u) + (b & 0xF000u) + (carry_into_sign_digit << 12);
  result.overflow = ((a ^ sign_sum) & (b ^ sign_sum) & 0x8000u) != 0;
  return result;
}

static ReferenceResult ReferenceRoutine(uint16 town_index, uint16 cell_x_word,
                                        uint16 cell_y_word, bool decimal) {
  const uint16 cell_x = (uint16)(cell_x_word & 0x1Fu);
  const uint16 cell_y = (uint16)(cell_y_word & 0x1Fu);
  const uint16 row = (uint16)((cell_y & 0x0Fu) << 4);
  const ReferenceAdd within =
      ReferenceAdd16((uint16)(cell_x & 0x0Fu), row, decimal);
  const ReferenceAdd scratch =
      ReferenceAdd16((uint16)(Swap16(town_index) << 1), within.value, decimal);
  const uint16 quadrant =
      (uint16)(((cell_y & 0x10u) ? 2u : 0u) +
               ((cell_x & 0x10u) ? 1u : 0u));
  ReferenceResult result;
  result.scratch = scratch.value;
  result.final =
      ReferenceAdd16((uint16)(quadrant << 8), scratch.value, decimal);
  return result;
}

static ReferenceTraversalResult ReferenceTraversalRoutine(
    uint16 town_index, uint16 cell_x_word, uint16 cell_y_word, bool decimal,
    uint8 terrain_id, uint16 metatile_top_left_word, uint8 cell_flags) {
  ReferenceTraversalResult result;
  result.cell_index =
      ReferenceRoutine(town_index, cell_x_word, cell_y_word, decimal);
  result.metatile_definition_offset =
      (uint16)(terrain_id * kMetatileDefinitionBytes);
  const bool terrain_blocks_traversal =
      (metatile_top_left_word & kMetatileCollisionBit) != 0;
  result.blocked = terrain_blocks_traversal ||
                   (cell_flags & kTraversalVisitedBit) != 0;
  if (terrain_blocks_traversal) {
    result.final_accumulator = metatile_top_left_word;
    result.negative = (metatile_top_left_word & 0x8000u) != 0;
  } else {
    result.final_accumulator =
        (uint16)((metatile_top_left_word & 0xFF00u) | cell_flags);
    result.negative = (cell_flags & 0x80u) != 0;
  }
  return result;
}

static CpuState MakeCpu(uint8 db, bool decimal, bool initial_m) {
  CpuState cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.A = 0xA55A;
  cpu.X = 0x5AA5;
  cpu.Y = 0xBEEF;
  cpu.S = 0x01D0;
  cpu.D = 0x2468;
  cpu.DB = db;
  cpu.PB = 0x03;
  cpu.m_flag = initial_m;
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

static void CheckCpuCase(uint16 town_index, uint16 cell_x, uint16 cell_y,
                         bool decimal, bool initial_m, uint8 db) {
  cpu_write16(NULL, db, kTownIndex, town_index);
  cpu_write16(NULL, db, kCellX, cell_x);
  cpu_write16(NULL, db, kCellY, cell_y);
  cpu_write16(NULL, db, kScratch, 0xDEAD);
  CpuState cpu = MakeCpu(db, decimal, initial_m);
  const ReferenceResult expected =
      ReferenceRoutine(town_index, cell_x, cell_y, decimal);

  CHECK(ActRaiser_TownCellMarkIndex(&cpu) == RECOMP_RETURN_NORMAL);
  CHECK(cpu_read16(NULL, db, kScratch) == expected.scratch);
  CHECK(cpu.A == expected.final.value);
  CHECK(cpu.X == expected.final.value);
  CHECK(cpu.Y == 0xBEEF);
  CHECK(cpu.S == 0x01D2);
  CHECK(cpu.D == 0x2468);
  CHECK(cpu.DB == db);
  CHECK(cpu.PB == 0x03);

  const uint8 expected_p =
      (uint8)(CPU_P_M | CPU_P_I | (decimal ? CPU_P_D : 0) |
              (expected.final.carry ? CPU_P_C : 0) |
              (expected.final.value == 0 ? CPU_P_Z : 0) |
              (expected.final.overflow ? CPU_P_V : 0) |
              (expected.final.value & 0x8000u ? CPU_P_N : 0));
  CHECK(cpu.P == expected_p);
  CHECK(cpu.m_flag == 1);
  CHECK(cpu.x_flag == 0);
  CHECK(cpu._flag_C == expected.final.carry);
  CHECK(cpu._flag_Z == (expected.final.value == 0));
  CHECK(cpu._flag_I == 1);
  CHECK(cpu._flag_D == decimal);
  CHECK(cpu._flag_V == expected.final.overflow);
  CHECK(cpu._flag_N == ((expected.final.value & 0x8000u) != 0));
}

static void CheckTraversalPredicate(void) {
  static const uint8 representative_flags[] = { 0x00, 0x04, 0x80, 0x84 };
  for (uint32 word = 0; word <= UINT16_MAX; word++) {
    for (unsigned i = 0;
         i < sizeof(representative_flags) / sizeof(representative_flags[0]);
         i++) {
      const uint8 flags = representative_flags[i];
      const bool expected = (word & kMetatileCollisionBit) != 0 ||
                            (flags & kTraversalVisitedBit) != 0;
      CHECK(ActRaiser_IsTownCellTraversalBlocked((uint16)word, flags) ==
            expected);
    }
  }

  static const uint16 representative_words[] = {
    0x0000, 0x0200, 0x8000, 0xA100,
  };
  for (unsigned flags = 0; flags <= UINT8_MAX; flags++) {
    for (unsigned i = 0;
         i < sizeof(representative_words) / sizeof(representative_words[0]);
         i++) {
      const uint16 word = representative_words[i];
      const bool expected = (word & kMetatileCollisionBit) != 0 ||
                            (flags & kTraversalVisitedBit) != 0;
      CHECK(ActRaiser_IsTownCellTraversalBlocked(word, (uint8)flags) ==
            expected);
    }
  }
}

static void CheckTraversalCase(uint16 town_index, uint16 cell_x,
                               uint16 cell_y, bool decimal, bool initial_m,
                               uint8 terrain_id,
                               uint16 metatile_top_left_word,
                               uint8 cell_flags) {
  memset(memory, 0, sizeof(memory));
  const ReferenceTraversalResult expected = ReferenceTraversalRoutine(
      town_index, cell_x, cell_y, decimal, terrain_id,
      metatile_top_left_word, cell_flags);
  const uint16 cell_index = expected.cell_index.final.value;
  cpu_write16(NULL, kHighWramBank, kTownIndex, town_index);
  cpu_write16(NULL, kHighWramBank, kCellX, cell_x);
  cpu_write16(NULL, kHighWramBank, kCellY, cell_y);
  cpu_write16(NULL, kHighWramBank, kScratch, 0xDEAD);
  WriteDataBankIndexedByte(kHighWramBank, kTerrainMapBase, cell_index,
                           terrain_id);
  cpu_write16(NULL, kLowWramBank,
              (uint16)(kMetatileDefinitionsBase +
                       expected.metatile_definition_offset),
              metatile_top_left_word);
  WriteDataBankIndexedByte(kHighWramBank, kCellFlagsBase, cell_index,
                           cell_flags);

  CpuState cpu = MakeCpu(kHighWramBank, decimal, initial_m);
  const uint16 entry_stack_pointer = cpu.S;
  CHECK(ActRaiser_TownCellTestTraversalBlocked(&cpu) ==
        RECOMP_RETURN_NORMAL);

  CHECK(cpu_read16(NULL, kHighWramBank, kScratch) ==
        expected.cell_index.scratch);
  CHECK(cpu.A == expected.final_accumulator);
  CHECK(cpu.X == expected.metatile_definition_offset);
  CHECK(cpu.Y == cell_index);
  CHECK(cpu.S == (uint16)(entry_stack_pointer + 2));
  CHECK(cpu.D == 0x2468);
  CHECK(cpu.DB == kHighWramBank);
  CHECK(cpu.PB == 0x03);
  CHECK(cpu_read8(NULL, kStackBank, entry_stack_pointer) ==
        (uint8)(kNestedCellIndexReturnAddress >> 8));
  CHECK(cpu_read8(NULL, kStackBank,
                  (uint16)(entry_stack_pointer - 1)) ==
        (uint8)kNestedCellIndexReturnAddress);
  CHECK(cpu.host_return_valid == 1);

  const uint8 expected_p =
      (uint8)(CPU_P_M | CPU_P_I | (decimal ? CPU_P_D : 0) |
              (!expected.blocked ? CPU_P_Z : 0) |
              (expected.cell_index.final.overflow ? CPU_P_V : 0) |
              (expected.negative ? CPU_P_N : 0));
  CHECK(cpu.P == expected_p);
  CHECK(cpu.m_flag == 1);
  CHECK(cpu.x_flag == 0);
  CHECK(cpu._flag_C == 0);
  CHECK(cpu._flag_Z == !expected.blocked);
  CHECK(cpu._flag_I == 1);
  CHECK(cpu._flag_D == decimal);
  CHECK(cpu._flag_V == expected.cell_index.final.overflow);
  CHECK(cpu._flag_N == expected.negative);
}

static void CheckCanonicalGrid(void) {
  for (unsigned town = 0; town < 6; town++) {
    for (unsigned y = 0; y < 32; y++) {
      for (unsigned x = 0; x < 32; x++) {
        const uint16 expected =
            (uint16)(town * 0x400u + (y >= 16 ? 0x200u : 0) +
                     (x >= 16 ? 0x100u : 0) + (y & 15u) * 16u + (x & 15u));
        CHECK(ActRaiser_CellMarkIndex(town, (uint8)x, (uint8)y) == expected);
        CheckCpuCase((uint16)(town * 2), (uint16)x, (uint16)y,
                     false, (x & 1u) != 0, 0x7F);
      }
    }
  }
}

static void CheckWidthAndDecimalEdges(void) {
  static const struct {
    uint16 town_index;
    uint16 x;
    uint16 y;
  } cases[] = {
    { 0x0000, 0x0000, 0x0000 },
    { 0x0002, 0x0010, 0x0010 },
    { 0x000A, 0x001F, 0x001F },
    { 0x00FE, 0xAB1A, 0xCD15 },
    { 0x0180, 0xFFFF, 0x8000 },
    { 0x7FFE, 0x0011, 0x001E },
    { 0xFFFE, 0x1234, 0x5678 },
  };
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    for (unsigned decimal = 0; decimal < 2; decimal++) {
      for (unsigned initial_m = 0; initial_m < 2; initial_m++) {
        CheckCpuCase(cases[i].town_index, cases[i].x, cases[i].y,
                     decimal != 0, initial_m != 0,
                     (i & 1u) ? 0x7E : 0x7F);
      }
    }
  }
}

static void CheckTraversalCpuContract(void) {
  /* Available terrain preserves the metatile word's high accumulator byte. */
  CheckTraversalCase(0x0004, 0x0003, 0x0005, false, false,
                     0x21, 0xA100, 0x00);
  /* Terrain collision exits before reading the visited map. */
  CheckTraversalCase(0x0002, 0x0014, 0x001A, false, true,
                     0x37, 0x8201, 0x80);
  /* A visited cell uses the 8-bit flags load for the final A/N state. */
  CheckTraversalCase(0x000A, 0x001F, 0x001F, false, false,
                     0xFF, 0x4100, 0x84);
  /* Non-blocking high-bit flags leave Z set while setting N. */
  CheckTraversalCase(0x0000, 0x0000, 0x0000, false, true,
                     0x00, 0x0100, 0x80);
  /* Preserve the nested helper's instruction-level decimal-mode behavior. */
  CheckTraversalCase(0x00FE, 0xAB1A, 0xCD15, true, false,
                     0x5A, 0x2100, 0x00);
}

static void CheckStructureMarkSemantic(void) {
  memset(memory, 0, sizeof(memory));
  CpuState cpu = MakeCpu(kLowWramBank, false, true);
  const uint8 mark = 0xE3;
  const uint16 cell_index = ActRaiser_WriteTownStructureMark(
      &cpu, kHighWramBank, 2, 20, 22, mark,
      kActRaiserTownStructureMarkShape_Block2x2);
  CHECK(cell_index == ActRaiser_CellMarkIndex(2, 20, 22));
  CHECK(cpu.DB == kLowWramBank);
  CHECK(cpu_read8(NULL, kHighWramBank,
                  (uint16)(kStructureMarkMapBase + cell_index)) == mark);
  CHECK(cpu_read8(NULL, kHighWramBank,
                  (uint16)(kStructureMarkMapBase + cell_index + 1)) == mark);
  CHECK(cpu_read8(
            NULL, kHighWramBank,
            (uint16)(kStructureMarkMapBase + cell_index +
                     kStructureMarkBlockRowStride)) == mark);
  CHECK(cpu_read8(
            NULL, kHighWramBank,
            (uint16)(kStructureMarkMapBase + cell_index +
                     kStructureMarkBlockRowStride + 1)) == mark);

  const uint16 single_index = ActRaiser_WriteTownStructureMark(
      &cpu, kHighWramBank, 1, 3, 5, 0xE0,
      kActRaiserTownStructureMarkShape_Cell);
  CHECK(cpu_read8(NULL, kHighWramBank,
                  (uint16)(kStructureMarkMapBase + single_index)) == 0xE0);
  CHECK(cpu_read8(NULL, kHighWramBank,
                  (uint16)(kStructureMarkMapBase + single_index + 1)) == 0);
}

static void CheckStructureMarkCpuCase(
    RecompReturn (*hle)(CpuState *),
    ActRaiserTownStructureMarkShape shape, uint16 nested_return_address,
    uint16 record_address) {
  memset(memory, 0xCC, sizeof(memory));
  const uint16 town_index = 4;
  const uint8 cell_x = 20;
  const uint8 cell_y = 22;
  const uint8 mark = 0xE6;
  const uint16 staged_cell_x = (uint16)(0xAA00u | cell_x);
  const uint16 staged_cell_y = (uint16)(0xBB00u | cell_y);
  const ReferenceResult expected = ReferenceRoutine(
      town_index, staged_cell_x, staged_cell_y, false);
  cpu_write16(NULL, kHighWramBank, kTownIndex, town_index);
  cpu_write16(NULL, kHighWramBank, kCellX, staged_cell_x);
  cpu_write16(NULL, kHighWramBank, kCellY, staged_cell_y);
  cpu_write8(NULL, kHighWramBank, record_address, cell_x);
  cpu_write8(NULL, kHighWramBank, (uint16)(record_address + 1), cell_y);

  CpuState cpu = MakeCpu(kHighWramBank, false, true);
  cpu.A = (uint16)(0xA500u | mark);
  cpu.X = record_address;
  cpu.Y = 0xBEEF;
  cpu_mirrors_to_p(&cpu);
  const uint16 entry_stack = cpu.S;

  CHECK(hle(&cpu) == RECOMP_RETURN_NORMAL);
  CHECK(cpu_read16(NULL, kHighWramBank, kCellX) == staged_cell_x);
  CHECK(cpu_read16(NULL, kHighWramBank, kCellY) == staged_cell_y);
  CHECK(cpu_read16(NULL, kHighWramBank, kScratch) == expected.scratch);
  CHECK(cpu.A == (uint16)((expected.final.value & 0xFF00u) | mark));
  CHECK(cpu.X == record_address);
  CHECK(cpu.Y == 0xBEEF);
  CHECK(cpu.S == (uint16)(entry_stack + 2));
  CHECK(cpu.host_return_valid == 1);

  CHECK(cpu_read16(NULL, kStackBank,
                   (uint16)(entry_stack - 1)) == record_address);
  CHECK(cpu_read8(NULL, kStackBank,
                  (uint16)(entry_stack - 2)) == mark);
  CHECK(cpu_read16(NULL, kStackBank,
                   (uint16)(entry_stack - 4)) == nested_return_address);

  const uint16 map_address =
      (uint16)(kStructureMarkMapBase + expected.final.value);
  CHECK(cpu_read8(NULL, kHighWramBank, map_address) == mark);
  if (shape == kActRaiserTownStructureMarkShape_Block2x2) {
    CHECK(cpu_read8(NULL, kHighWramBank,
                    (uint16)(map_address + 1)) == mark);
    CHECK(cpu_read8(NULL, kHighWramBank,
                    (uint16)(map_address + kStructureMarkBlockRowStride)) ==
          mark);
    CHECK(cpu_read8(
              NULL, kHighWramBank,
              (uint16)(map_address + kStructureMarkBlockRowStride + 1)) ==
          mark);
  } else {
    CHECK(cpu_read8(NULL, kHighWramBank,
                    (uint16)(map_address + 1)) == 0xCC);
  }

  const uint8 expected_p = (uint8)(
      CPU_P_M | CPU_P_I |
      (expected.final.carry ? CPU_P_C : 0) |
      (expected.final.overflow ? CPU_P_V : 0) |
      (record_address == 0 ? CPU_P_Z : 0) |
      (record_address & 0x8000u ? CPU_P_N : 0));
  CHECK(cpu.P == expected_p);
  CHECK(cpu.m_flag == 1);
  CHECK(cpu.x_flag == 0);
}

static void CheckStructureMarkCpuContracts(void) {
  CheckStructureMarkCpuCase(
      ActRaiser_WriteTownStructureMarkCell,
      kActRaiserTownStructureMarkShape_Cell,
      kStructureMarkCellReturnAddress, 0x6A20);
  CheckStructureMarkCpuCase(
      ActRaiser_WriteTownStructureMarkBlock,
      kActRaiserTownStructureMarkShape_Block2x2,
      kStructureMarkBlockReturnAddress, 0);
}

int main(void) {
  memset(memory, 0, sizeof(memory));
  CheckTraversalPredicate();
  CheckCanonicalGrid();
  CheckWidthAndDecimalEdges();
  CheckTraversalCpuContract();
  CheckStructureMarkSemantic();
  CheckStructureMarkCpuContracts();
  if (failures) {
    printf("actraiser cell-map HLE: %d failure(s)\n", failures);
    return 1;
  }
  printf("actraiser cell-map HLE: all checks passed\n");
  return 0;
}
