#include "actraiser/actraiser_cell_map.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
  kTownIndex = 0x7BFB,
  kScratch = 0x7C05,
  kCellX = 0x7C4B,
  kCellY = 0x7C4D,
};

static uint8_t memory[0x20000];
static int failures;

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      failures++;                                                          \
    }                                                                      \
  } while (0)

static size_t BankOffset(uint8 bank, uint16 address) {
  if (bank == 0x7E) return address;
  if (bank == 0x7F) return 0x10000u + address;
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

typedef struct ReferenceAdd {
  uint16 value;
  bool carry;
  bool overflow;
} ReferenceAdd;

typedef struct ReferenceResult {
  uint16 scratch;
  ReferenceAdd final;
} ReferenceResult;

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

int main(void) {
  memset(memory, 0, sizeof(memory));
  CheckCanonicalGrid();
  CheckWidthAndDecimalEdges();
  if (failures) {
    printf("actraiser cell-map HLE: %d failure(s)\n", failures);
    return 1;
  }
  printf("actraiser cell-map HLE: all checks passed\n");
  return 0;
}
