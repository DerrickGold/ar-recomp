#include "actraiser/actraiser_cell_map.h"

#include <limits.h>
#include <stdbool.h>

#include "actraiser/actraiser_cpu_hle_internal.h"
#include "actraiser/actraiser_hle_fatal.h"
#include "cpu_65816_math.h"

enum {
  kCellMapTownIndex = 0x7BFB,
  kCellMapScratch = 0x7C05,
  kCellMapCellX = 0x7C4B,
  kCellMapCellY = 0x7C4D,
  kTownCellMapBase = 0x2000,
  kTownCellFlagsBase = 0x3800,
  kTownMetatileDefinitionsBase = 0x2100,
  kTownMetatileDefinitionBytes = 8,
  kTownMetatileCollisionBit = 0x0200,
  kTownCellTraversalVisitedBit = 0x04,
  kTownCellIndexJsrReturnAddress = 0x96F1,
  kStructureMarkCellIndexReturnAddress = 0x9FDD,
  kStructureMarkBlockIndexReturnAddress = 0x9FF4,
  kStructureRecordCellXOffset = 0,
  kStructureRecordCellYOffset = 1,
  kCellMapQuadrantSide = 16,
  kCellMapQuadrantsPerAxis = 2,
  kCellMapQuadrantCells = kCellMapQuadrantSide * kCellMapQuadrantSide,
  kCellMapTownCells = kCellMapQuadrantCells *
                      kCellMapQuadrantsPerAxis * kCellMapQuadrantsPerAxis,
  kCellMapLocalCoordinateMask = kCellMapQuadrantSide - 1,
  kCellMapCoordinateMask =
      kCellMapQuadrantSide * kCellMapQuadrantsPerAxis - 1,
  kCellMapQuadrantCoordinateBit = kCellMapQuadrantSide,
  kEncodedTownIndexScale = 2,
  kWordBits = sizeof(uint16_t) * CHAR_BIT,
  kWordSignBit = 1u << (kWordBits - 1),
  kByteSignBit = 1u << (CHAR_BIT - 1),
  kWordHighByteMask = UINT16_MAX ^ UINT8_MAX,
};

typedef struct TownCellMarkIndexResult {
  uint16_t scratch_value;
  Cpu65816Add16Result final_addition;
} TownCellMarkIndexResult;

static uint16_t SwapWordBytes(uint16_t value) {
  return (uint16_t)((value << CHAR_BIT) | (value >> CHAR_BIT));
}

/* Every ADC in $03:9710 follows CLC. Keep that call-site invariant named while
 * sharing the exact CPU arithmetic with the other HLEs. */
static Cpu65816Add16Result AddCellMapWords(uint16_t left, uint16_t right,
                                          bool decimal) {
  return Cpu65816_Add16(left, right, false, decimal);
}

static unsigned CellMapQuadrantIndex(uint8_t cell_x, uint8_t cell_y) {
  const unsigned quadrant_column =
      (cell_x & kCellMapQuadrantCoordinateBit) != 0;
  const unsigned quadrant_row =
      (cell_y & kCellMapQuadrantCoordinateBit) != 0;
  return quadrant_row * kCellMapQuadrantsPerAxis + quadrant_column;
}

uint16_t ActRaiser_CellMarkIndex(unsigned town, uint8_t cell_x,
                                uint8_t cell_y) {
  const unsigned quadrant_index = CellMapQuadrantIndex(cell_x, cell_y);
  return (uint16_t)(
      ActRaiser_CellMarkPreQuadrantIndex(town, cell_x, cell_y) +
      quadrant_index * kCellMapQuadrantCells);
}

uint16_t ActRaiser_CellMarkPreQuadrantIndex(unsigned town, uint8_t cell_x,
                                           uint8_t cell_y) {
  const unsigned local_x = cell_x & kCellMapLocalCoordinateMask;
  const unsigned local_y = cell_y & kCellMapLocalCoordinateMask;
  const unsigned within_quadrant_index =
      local_y * kCellMapQuadrantSide + local_x;
  return (uint16_t)(town * kCellMapTownCells + within_quadrant_index);
}

static void WriteDataBankIndexedByte(CpuState *cpu, uint8_t data_bank,
                                     uint16_t base_address, uint16_t index,
                                     uint8_t value) {
  const uint32_t effective_address =
      ((uint32_t)data_bank << kWordBits) + base_address + index;
  cpu_write8(cpu, (uint8_t)(effective_address >> kWordBits),
             (uint16_t)effective_address, value);
}

static void WriteTownStructureMarkAtIndex(
    CpuState *cpu, uint8_t data_bank, uint16_t mark_index, uint8_t mark,
    ActRaiserTownStructureMarkShape shape) {
  WriteDataBankIndexedByte(
      cpu, data_bank, kTownCellMapBase, mark_index, mark);
  if (shape == kActRaiserTownStructureMarkShape_Cell) return;
  if (shape != kActRaiserTownStructureMarkShape_Block2x2) {
    ActRaiserHleFatal("invalid town structure-mark shape %d", (int)shape);
  }

  const uint16_t right_index = (uint16_t)(mark_index + 1);
  const uint16_t lower_index = (uint16_t)(
      mark_index + kCellMapQuadrantSide);
  WriteDataBankIndexedByte(
      cpu, data_bank, kTownCellMapBase, right_index, mark);
  WriteDataBankIndexedByte(
      cpu, data_bank, kTownCellMapBase, lower_index, mark);
  WriteDataBankIndexedByte(cpu, data_bank, kTownCellMapBase,
                           (uint16_t)(lower_index + 1), mark);
}

uint16_t ActRaiser_WriteTownStructureMark(
    CpuState *cpu, uint8_t data_bank, unsigned town, uint8_t cell_x,
    uint8_t cell_y, uint8_t mark,
    ActRaiserTownStructureMarkShape shape) {
  const uint16_t mark_index =
      ActRaiser_CellMarkIndex(town, cell_x, cell_y);
  if (!cpu) return mark_index;

  WriteTownStructureMarkAtIndex(
      cpu, data_bank, mark_index, mark, shape);
  return mark_index;
}

bool ActRaiser_IsTownCellTraversalBlocked(uint16_t metatile_top_left_word,
                                          uint8_t cell_flags) {
  return (metatile_top_left_word & kTownMetatileCollisionBit) != 0 ||
         (cell_flags & kTownCellTraversalVisitedBit) != 0;
}

static TownCellMarkIndexResult CalculateTownCellMarkIndex(
    uint16_t encoded_town_index, uint16_t staged_cell_x,
    uint16_t staged_cell_y, bool decimal) {
  const uint8_t cell_x =
      (uint8_t)(staged_cell_x & kCellMapCoordinateMask);
  const uint8_t cell_y =
      (uint8_t)(staged_cell_y & kCellMapCoordinateMask);
  const uint16_t quadrant_offset = (uint16_t)(
      CellMapQuadrantIndex(cell_x, cell_y) * kCellMapQuadrantCells);
  TownCellMarkIndexResult result;
  const bool can_use_binary_cell_index_formula =
      !decimal && encoded_town_index <= UINT8_MAX &&
      encoded_town_index % kEncodedTownIndexScale == 0;
  if (can_use_binary_cell_index_formula) {
    const unsigned town = encoded_town_index / kEncodedTownIndexScale;
    result.scratch_value =
        ActRaiser_CellMarkPreQuadrantIndex(town, cell_x, cell_y);
    result.final_addition =
        AddCellMapWords(quadrant_offset, result.scratch_value, false);
    return result;
  }

  /* Preserve instruction-exact behavior for malformed raw town indexes and
   * for D=1, neither of which occurs in ordinary gameplay. */
  const uint16_t local_x = cell_x & kCellMapLocalCoordinateMask;
  const uint16_t local_y = cell_y & kCellMapLocalCoordinateMask;
  const uint16_t row_offset =
      (uint16_t)(local_y * kCellMapQuadrantSide);
  const Cpu65816Add16Result within_quadrant_addition =
      AddCellMapWords(local_x, row_offset, decimal);
  const uint16_t town_base = (uint16_t)(
      SwapWordBytes(encoded_town_index) * kEncodedTownIndexScale);
  const Cpu65816Add16Result scratch_addition =
      AddCellMapWords(town_base, within_quadrant_addition.value, decimal);
  result.scratch_value = scratch_addition.value;
  result.final_addition =
      AddCellMapWords(quadrant_offset, result.scratch_value, decimal);
  return result;
}

static void PushCpuByte(CpuState *cpu, uint8_t value) {
  cpu_write8(cpu, k65816StackBank, cpu->S, value);
  cpu->S = (uint16_t)(cpu->S - 1);
}

static uint8_t PopCpuByte(CpuState *cpu) {
  cpu->S = (uint16_t)(cpu->S + 1);
  return cpu_read8(cpu, k65816StackBank, cpu->S);
}

static void EmulateNestedCellIndexJsr(CpuState *cpu) {
  ActRaiserCpuHle_PushWord(cpu, kTownCellIndexJsrReturnAddress);

  /* $03:9710's RTS restores the caller-visible stack pointer. The bytes remain
   * in stack RAM, just as they do after the native nested call. */
  cpu->S = (uint16_t)(cpu->S + k65816RtsStackBytes);
  cpu->host_return_valid = 1;
}

static uint8_t ReadDataBankIndexedByte(CpuState *cpu, uint16_t base_address,
                                       uint16_t index) {
  const uint32_t effective_address =
      ((uint32_t)cpu->DB << kWordBits) + base_address + index;
  return cpu_read8(cpu, (uint8_t)(effective_address >> kWordBits),
                   (uint16_t)effective_address);
}

/* $03:9710 converts the staged word coordinates at $7C4B/$7C4D into the
 * quadrant-paged mark-map index. It is a bounded, yield-free arithmetic leaf
 * shared by 21 direct JSR sites. The ordinary binary-mode path now shares the
 * same semantic calculation as the bridge sidecar; the explicit instruction
 * model below retains the routine's full scratch and decimal-mode behavior.
 *
 * Valid callers enter with 16-bit indexes. With X=8-bit, the raw LDX #$0000
 * immediate becomes LDX #$00 followed by BRK, so fail closed rather than hide
 * an invalid-width dispatch behind the HLE. */
RecompReturn ActRaiser_TownCellMarkIndex(CpuState *cpu) {
  if (!cpu) return RECOMP_RETURN_NORMAL;

  cpu_mirrors_to_p(cpu);
  ActRaiserCpuHle_RequireEntryMode(
      cpu, "$03:9710", kActRaiserCpuHleEntryMode_Native16BitIndexes);

  const uint16_t staged_cell_x =
      cpu_read16(cpu, cpu->DB, kCellMapCellX);
  const uint16_t staged_cell_y =
      cpu_read16(cpu, cpu->DB, kCellMapCellY);
  const uint16_t encoded_town_index =
      cpu_read16(cpu, cpu->DB, kCellMapTownIndex);
  const bool decimal = cpu->_flag_D != 0;
  const TownCellMarkIndexResult result = CalculateTownCellMarkIndex(
      encoded_town_index, staged_cell_x, staged_cell_y, decimal);
  cpu_write16(cpu, cpu->DB, kCellMapScratch, result.scratch_value);
  cpu->A = result.final_addition.value;
  cpu->X = result.final_addition.value;

  cpu->P = (uint8_t)((cpu->P & (CPU_P_I | CPU_P_D | CPU_P_X)) | CPU_P_M |
                     (result.final_addition.carry ? CPU_P_C : 0) |
                     (result.final_addition.value == 0 ? CPU_P_Z : 0) |
                     (result.final_addition.overflow ? CPU_P_V : 0) |
                     (result.final_addition.value & kWordSignBit
                          ? CPU_P_N : 0));
  cpu_p_to_mirrors(cpu);
  cpu->S = (uint16_t)(cpu->S + k65816RtsStackBytes);
  return RECOMP_RETURN_NORMAL;
}

static RecompReturn WriteTownStructureMarkHle(
    CpuState *cpu, ActRaiserTownStructureMarkShape shape,
    uint16_t nested_return_address, const char *routine_name) {
  if (!cpu) return RECOMP_RETURN_NORMAL;

  cpu_mirrors_to_p(cpu);
  ActRaiserCpuHle_RequireEntryMode(
      cpu, routine_name,
      kActRaiserCpuHleEntryMode_Native8BitAccumulator16BitIndexes);

  const uint16_t saved_x = cpu->X;
  const uint8_t mark = (uint8_t)cpu->A;
  ActRaiserCpuHle_PushWord(cpu, saved_x);
  PushCpuByte(cpu, mark);

  const uint8_t cell_x = ReadDataBankIndexedByte(
      cpu, kStructureRecordCellXOffset, saved_x);
  const uint8_t cell_y = ReadDataBankIndexedByte(
      cpu, kStructureRecordCellYOffset, saved_x);
  cpu_write8(cpu, cpu->DB, kCellMapCellX, cell_x);
  cpu_write8(cpu, cpu->DB, kCellMapCellY, cell_y);

  ActRaiserCpuHle_PushWord(cpu, nested_return_address);
  cpu->host_return_valid = 1;
  const RecompReturn nested_result = ActRaiser_TownCellMarkIndex(cpu);
  if (nested_result != RECOMP_RETURN_NORMAL) return nested_result;

  const uint16_t mark_index = cpu->X;
  const uint8_t restored_mark = PopCpuByte(cpu);
  cpu->A = (uint16_t)((cpu->A & kWordHighByteMask) | restored_mark);
  WriteTownStructureMarkAtIndex(
      cpu, cpu->DB, mark_index, restored_mark, shape);
  cpu->X = ActRaiserCpuHle_PopWord(cpu);
  ActRaiserCpuHle_SetNegativeZero16(cpu, cpu->X);
  cpu_p_to_mirrors(cpu);
  cpu->S = (uint16_t)(cpu->S + k65816RtsStackBytes);
  return RECOMP_RETURN_NORMAL;
}

RecompReturn ActRaiser_WriteTownStructureMarkCell(CpuState *cpu) {
  return WriteTownStructureMarkHle(
      cpu, kActRaiserTownStructureMarkShape_Cell,
      kStructureMarkCellIndexReturnAddress, "$03:9FCD");
}

RecompReturn ActRaiser_WriteTownStructureMarkBlock(CpuState *cpu) {
  return WriteTownStructureMarkHle(
      cpu, kActRaiserTownStructureMarkShape_Block2x2,
      kStructureMarkBlockIndexReturnAddress, "$03:9FE4");
}

/* $03:96EF is the build-direction pathfinder's bounded cell predicate. It
 * calls $03:9710, reads the terrain id from the active town map, tests the
 * collision marker carried by the metatile's top-left tile word, then falls
 * back to the traversal's per-cell visited bit. Z=1 means the cell is
 * available. Preserve the native routine's accumulator-width transitions,
 * flags, scratch, registers, nested-JSR stack writes, and RTS behavior. */
RecompReturn ActRaiser_TownCellTestTraversalBlocked(CpuState *cpu) {
  if (!cpu) return RECOMP_RETURN_NORMAL;

  cpu_mirrors_to_p(cpu);
  ActRaiserCpuHle_RequireEntryMode(
      cpu, "$03:96EF", kActRaiserCpuHleEntryMode_Native16BitIndexes);
  EmulateNestedCellIndexJsr(cpu);

  const uint16_t staged_cell_x =
      cpu_read16(cpu, cpu->DB, kCellMapCellX);
  const uint16_t staged_cell_y =
      cpu_read16(cpu, cpu->DB, kCellMapCellY);
  const uint16_t encoded_town_index =
      cpu_read16(cpu, cpu->DB, kCellMapTownIndex);
  const TownCellMarkIndexResult index_result = CalculateTownCellMarkIndex(
      encoded_town_index, staged_cell_x, staged_cell_y,
      cpu->_flag_D != 0);
  const uint16_t cell_index = index_result.final_addition.value;
  cpu_write16(cpu, cpu->DB, kCellMapScratch, index_result.scratch_value);

  const uint8_t terrain_id =
      ReadDataBankIndexedByte(cpu, kTownCellMapBase, cell_index);
  const uint16_t metatile_definition_offset =
      (uint16_t)(terrain_id * kTownMetatileDefinitionBytes);
  const uint16_t metatile_top_left_word = cpu_read16(
      cpu, kSnesLowWramBank,
      (uint16_t)(kTownMetatileDefinitionsBase + metatile_definition_offset));

  uint16_t final_accumulator = metatile_top_left_word;
  bool final_negative =
      (metatile_top_left_word & kWordSignBit) != 0;
  bool traversal_blocked;
  if ((metatile_top_left_word & kTownMetatileCollisionBit) != 0) {
    traversal_blocked = true;
  } else {
    const uint8_t cell_flags =
        ReadDataBankIndexedByte(cpu, kTownCellFlagsBase, cell_index);
    traversal_blocked = ActRaiser_IsTownCellTraversalBlocked(
        metatile_top_left_word, cell_flags);
    final_accumulator = (uint16_t)(
        (metatile_top_left_word & kWordHighByteMask) | cell_flags);
    final_negative = (cell_flags & kByteSignBit) != 0;
  }

  cpu->A = final_accumulator;
  cpu->X = metatile_definition_offset;
  cpu->Y = cell_index;
  cpu->P = (uint8_t)(
      (cpu->P & (CPU_P_I | CPU_P_D | CPU_P_X)) | CPU_P_M |
      (!traversal_blocked ? CPU_P_Z : 0) |
      (index_result.final_addition.overflow ? CPU_P_V : 0) |
      (final_negative ? CPU_P_N : 0));
  cpu_p_to_mirrors(cpu);
  cpu->S = (uint16_t)(cpu->S + k65816RtsStackBytes);
  return RECOMP_RETURN_NORMAL;
}
