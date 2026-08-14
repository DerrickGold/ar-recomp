#include "actraiser/actraiser_town_metatile.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
  kStackBank = 0x00,
  kLowWramBank = 0x7E,
  kHighWramBank = 0x7F,
  kBankBytes = 0x10000,
  kCellX = 0x7CAF,
  kCellY = 0x7CB1,
  kMetatileId = 0x7CB5,
  kTerrainDefinitionsBase = 0x2100,
  kStructureDefinitionsBase = 0x3100,
  kDefinitionBytes = 8,
  kTileWordBytes = 2,
  kDefinitionColumns = 2,
  kDefinitionRows = 2,
  kTilemapRowBytes = 0x40,
  kMetatileRowBytes = 0x80,
  kMetatileWidthBytes = 4,
  kQuadrantSideCells = 16,
  kQuadrantsPerRow = 2,
  kQuadrantBytes = 0x800,
  kLocalCoordinateMask = kQuadrantSideCells - 1,
  kCollisionAttributeBit = 0x0200,
  kAttributeMask = 0xFFFF ^ kCollisionAttributeBit,
};

typedef RecompReturn (*TownMetatileHle)(CpuState *cpu);

typedef struct ReferenceAdd {
  uint16 value;
  bool overflow;
} ReferenceAdd;

typedef struct ReferenceAddress {
  uint16 horizontal;
  ReferenceAdd within_quadrant;
  ReferenceAdd destination;
} ReferenceAddress;

static uint8_t memory[2 * kBankBytes];
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
  if (bank == kHighWramBank) return kBankBytes + address;
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

static uint16 DefinitionBase(ActRaiserTownMetatileAtlas atlas) {
  return atlas == kActRaiserTownMetatileAtlas_Terrain
             ? kTerrainDefinitionsBase
             : kStructureDefinitionsBase;
}

static uint16 ExpectedDestination(uint16 cell_x, uint16 cell_y) {
  const unsigned quadrant_column = cell_x >= kQuadrantSideCells;
  const unsigned quadrant_row = cell_y >= kQuadrantSideCells;
  const unsigned quadrant =
      quadrant_row * kQuadrantsPerRow + quadrant_column;
  return (uint16)(quadrant * kQuadrantBytes +
                  (cell_y & kLocalCoordinateMask) * kMetatileRowBytes +
                  (cell_x & kLocalCoordinateMask) * kMetatileWidthBytes);
}

static void WriteDefinition(ActRaiserTownMetatileAtlas atlas,
                            uint8 metatile_id,
                            const uint16 words[kDefinitionRows *
                                               kDefinitionColumns]) {
  const uint16 definition =
      (uint16)(DefinitionBase(atlas) + metatile_id * kDefinitionBytes);
  for (unsigned i = 0; i < kDefinitionRows * kDefinitionColumns; i++)
    cpu_write16(NULL, kLowWramBank,
                (uint16)(definition + i * kTileWordBytes), words[i]);
}

static void CheckCopiedWords(
    uint8 destination_bank, uint16 destination,
    const uint16 words[kDefinitionRows * kDefinitionColumns]) {
  for (unsigned row = 0; row < kDefinitionRows; row++) {
    for (unsigned column = 0; column < kDefinitionColumns; column++) {
      const unsigned definition_tile = row * kDefinitionColumns + column;
      const uint16 target = (uint16)(
          destination + row * kTilemapRowBytes + column * kTileWordBytes);
      CHECK(cpu_read16(NULL, destination_bank, target) ==
            (uint16)(words[definition_tile] & kAttributeMask));
    }
  }
}

static void CheckSemanticGrid(void) {
  static const uint16 terrain_words[] = {
    0x0200, 0x1234, 0xA255, 0xFFFF,
  };
  static const uint16 structure_words[] = {
    0x8001, 0x0202, 0x4567, 0xBEEF,
  };
  const uint8 metatile_id = 0x5A;
  WriteDefinition(kActRaiserTownMetatileAtlas_Terrain, metatile_id,
                  terrain_words);
  WriteDefinition(kActRaiserTownMetatileAtlas_Structure, metatile_id,
                  structure_words);

  CpuState cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.ram = memory;
  const CpuState initial_cpu = cpu;
  for (unsigned atlas = kActRaiserTownMetatileAtlas_Terrain;
       atlas <= kActRaiserTownMetatileAtlas_Structure; atlas++) {
    const uint16 *words =
        atlas == kActRaiserTownMetatileAtlas_Terrain
            ? terrain_words
            : structure_words;
    for (unsigned cell_y = 0; cell_y < 32; cell_y++) {
      for (unsigned cell_x = 0; cell_x < 32; cell_x++) {
        ActRaiser_CopyTownMetatile(
            &cpu, kHighWramBank, (uint16)cell_x, (uint16)cell_y,
            metatile_id, (ActRaiserTownMetatileAtlas)atlas);
        CheckCopiedWords(kHighWramBank,
                         ExpectedDestination((uint16)cell_x,
                                             (uint16)cell_y),
                         words);
      }
    }
  }

  /* The ROM treats every coordinate >=16 as the lower/right quadrant while
   * retaining only its low nibble for the local cell. */
  ActRaiser_CopyTownMetatile(
      &cpu, kHighWramBank, 0x0020, 0xFFFF, metatile_id,
      kActRaiserTownMetatileAtlas_Terrain);
  CheckCopiedWords(kHighWramBank,
                   ExpectedDestination(0x0020, 0xFFFF), terrain_words);
  CHECK(memcmp(&cpu, &initial_cpu, sizeof(cpu)) == 0);
}

/* Independent instruction-level oracle for the two CLC/ADC pairs. */
static ReferenceAdd ReferenceAdd16(uint16 left, uint16 right, bool decimal) {
  ReferenceAdd result = { 0 };
  if (!decimal) {
    const uint32 sum = (uint32)left + right;
    result.value = (uint16)sum;
    result.overflow =
        ((left ^ result.value) & (right ^ result.value) & 0x8000u) != 0;
    return result;
  }

  uint32 adjusted = 0;
  unsigned carry = 0;
  unsigned carry_into_sign_digit = 0;
  for (unsigned shift = 0; shift < 16; shift += 4) {
    unsigned digit =
        ((left >> shift) & 15u) + ((right >> shift) & 15u) + carry;
    carry = digit > 9;
    if (shift == 8) carry_into_sign_digit = carry;
    if (carry) digit += 6;
    adjusted |= (uint32)(digit & 15u) << shift;
  }
  result.value = (uint16)adjusted;
  const uint32 sign_sum =
      (left & 0xF000u) + (right & 0xF000u) +
      (carry_into_sign_digit << 12);
  result.overflow =
      ((left ^ sign_sum) & (right ^ sign_sum) & 0x8000u) != 0;
  return result;
}

static ReferenceAddress ReferenceCpuAddress(uint16 cell_x, uint16 cell_y,
                                             bool decimal) {
  ReferenceAddress result;
  result.horizontal = (uint16)(
      (cell_x & kLocalCoordinateMask) * kMetatileWidthBytes);
  const uint16 vertical = (uint16)(
      (cell_y & kLocalCoordinateMask) * kMetatileRowBytes);
  result.within_quadrant = ReferenceAdd16(vertical, result.horizontal,
                                          decimal);
  const unsigned quadrant_column = cell_x >= kQuadrantSideCells;
  const unsigned quadrant_row = cell_y >= kQuadrantSideCells;
  const uint16 quadrant = (uint16)(
      (quadrant_row * kQuadrantsPerRow + quadrant_column) *
      kQuadrantBytes);
  result.destination =
      ReferenceAdd16(quadrant, result.within_quadrant.value, decimal);
  return result;
}

static CpuState MakeCpu(uint8 data_bank, bool decimal) {
  CpuState cpu;
  memset(&cpu, 0, sizeof(cpu));
  cpu.A = 0xA55A;
  cpu.X = 0x5AA5;
  cpu.Y = 0xBEEF;
  cpu.S = 0x01D0;
  cpu.D = 0x2468;
  cpu.DB = data_bank;
  cpu.PB = 0x03;
  cpu.m_flag = 0;
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

static void CheckCpuCase(ActRaiserTownMetatileAtlas atlas,
                         TownMetatileHle hle, uint16 cell_x,
                         uint16 cell_y, uint8 metatile_id,
                         uint16 bottom_right_word, uint8 data_bank,
                         bool decimal) {
  memset(memory, 0xCC, sizeof(memory));
  const uint16 words[] = {
    0x0200, 0x9234, 0x4567, bottom_right_word,
  };
  WriteDefinition(atlas, metatile_id, words);
  cpu_write16(NULL, data_bank, kCellX, cell_x);
  cpu_write16(NULL, data_bank, kCellY, cell_y);
  cpu_write16(NULL, data_bank, kMetatileId,
              (uint16)(0xA500u | metatile_id));

  CpuState cpu = MakeCpu(data_bank, decimal);
  const uint16 entry_stack_pointer = cpu.S;
  const ReferenceAddress expected =
      ReferenceCpuAddress(cell_x, cell_y, decimal);
  CHECK(hle(&cpu) == RECOMP_RETURN_NORMAL);

  CheckCopiedWords(data_bank, expected.destination.value, words);
  const uint16 expected_accumulator =
      (uint16)(bottom_right_word & kAttributeMask);
  CHECK(cpu.A == expected_accumulator);
  CHECK(cpu.X == (uint16)(metatile_id * kDefinitionBytes));
  CHECK(cpu.Y == expected.destination.value);
  CHECK(cpu.S == (uint16)(entry_stack_pointer + 2));
  CHECK(cpu.D == 0x2468);
  CHECK(cpu.DB == data_bank);
  CHECK(cpu.PB == 0x03);
  CHECK(cpu.host_return_valid == 0);

  CHECK(cpu_read8(NULL, kStackBank, entry_stack_pointer) ==
        (uint8)(expected.horizontal >> 8));
  CHECK(cpu_read8(NULL, kStackBank,
                  (uint16)(entry_stack_pointer - 1)) ==
        (uint8)expected.horizontal);
  CHECK(cpu_read8(NULL, kStackBank,
                  (uint16)(entry_stack_pointer - 2)) ==
        (uint8)(expected.within_quadrant.value >> 8));
  CHECK(cpu_read8(NULL, kStackBank,
                  (uint16)(entry_stack_pointer - 3)) ==
        (uint8)expected.within_quadrant.value);

  const uint8 expected_p = (uint8)(
      CPU_P_I | (decimal ? CPU_P_D : 0) |
      (expected_accumulator == 0 ? CPU_P_Z : 0) |
      (expected.destination.overflow ? CPU_P_V : 0) |
      (expected_accumulator & 0x8000u ? CPU_P_N : 0));
  CHECK(cpu.P == expected_p);
  CHECK(cpu.m_flag == 0);
  CHECK(cpu.x_flag == 0);
  /* The third source-offset ASL always clears carry because an eight-bit
   * metatile id cannot shift beyond bit 10 in the 16-bit accumulator. */
  CHECK(cpu._flag_C == 0);
  CHECK(cpu._flag_Z == (expected_accumulator == 0));
  CHECK(cpu._flag_I == 1);
  CHECK(cpu._flag_D == decimal);
  CHECK(cpu._flag_V == expected.destination.overflow);
  CHECK(cpu._flag_N == ((expected_accumulator & 0x8000u) != 0));
}

static void CheckCpuContracts(void) {
  CheckCpuCase(kActRaiserTownMetatileAtlas_Terrain,
               ActRaiser_TownCopyTerrainMetatile,
               0x0003, 0x0005, 0x21, 0xBEEF, kHighWramBank, false);
  CheckCpuCase(kActRaiserTownMetatileAtlas_Structure,
               ActRaiser_TownCopyStructureMetatile,
               0x0014, 0x001A, 0x37, 0x0200, kLowWramBank, false);
  CheckCpuCase(kActRaiserTownMetatileAtlas_Terrain,
               ActRaiser_TownCopyTerrainMetatile,
               0xFFFF, 0x0020, 0xFF, 0x0100, kHighWramBank, false);
  CheckCpuCase(kActRaiserTownMetatileAtlas_Structure,
               ActRaiser_TownCopyStructureMetatile,
               0x0019, 0x0018, 0x5A, 0x8001, kHighWramBank, true);
  CheckCpuCase(kActRaiserTownMetatileAtlas_Terrain,
               ActRaiser_TownCopyTerrainMetatile,
               0xAB1A, 0xCD15, 0x7E, 0x0000, kHighWramBank, true);
}

int main(void) {
  memset(memory, 0, sizeof(memory));
  CheckSemanticGrid();
  CheckCpuContracts();
  if (failures) {
    printf("actraiser town-metatile HLE: %d failure(s)\n", failures);
    return 1;
  }
  printf("actraiser town-metatile HLE: all checks passed\n");
  return 0;
}
