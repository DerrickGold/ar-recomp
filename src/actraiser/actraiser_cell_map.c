#include "actraiser/actraiser_cell_map.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "cpu_65816_math.h"

enum {
  kCellMapTownIndex = 0x7BFB,
  kCellMapScratch = 0x7C05,
  kCellMapCellX = 0x7C4B,
  kCellMapCellY = 0x7C4D,
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
};

static uint16_t SwapWordBytes(uint16_t value) {
  return (uint16_t)((value << CHAR_BIT) | (value >> CHAR_BIT));
}

/* Every ADC in $03:9710 follows CLC. Keep that call-site invariant named while
 * sharing the exact CPU arithmetic with the other HLEs. */
static Cpu65816Add16Result AddCellMapWords(uint16_t left, uint16_t right,
                                          bool decimal) {
  return Cpu65816_Add16(left, right, false, decimal);
}

uint16_t ActRaiser_CellMarkIndex(unsigned town, uint8_t cell_x,
                                uint8_t cell_y) {
  const unsigned quadrant_column =
      (cell_x & kCellMapQuadrantCoordinateBit) != 0;
  const unsigned quadrant_row =
      (cell_y & kCellMapQuadrantCoordinateBit) != 0;
  const unsigned quadrant_index =
      quadrant_row * kCellMapQuadrantsPerAxis + quadrant_column;
  const unsigned local_x = cell_x & kCellMapLocalCoordinateMask;
  const unsigned local_y = cell_y & kCellMapLocalCoordinateMask;
  const unsigned within_quadrant_index =
      local_y * kCellMapQuadrantSide + local_x;
  return (uint16_t)(town * kCellMapTownCells +
                    quadrant_index * kCellMapQuadrantCells +
                    within_quadrant_index);
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
  if (cpu->x_flag || cpu->emulation) {
    fprintf(stderr,
            "FATAL: $03:9710 HLE requires native mode with 16-bit indexes\n");
    abort();
  }

  const uint16_t staged_cell_x =
      cpu_read16(cpu, cpu->DB, kCellMapCellX);
  const uint16_t staged_cell_y =
      cpu_read16(cpu, cpu->DB, kCellMapCellY);
  const uint16_t encoded_town_index =
      cpu_read16(cpu, cpu->DB, kCellMapTownIndex);
  const uint8_t cell_x =
      (uint8_t)(staged_cell_x & kCellMapCoordinateMask);
  const uint8_t cell_y =
      (uint8_t)(staged_cell_y & kCellMapCoordinateMask);
  const bool decimal = cpu->_flag_D != 0;

  const uint16_t quadrant_column =
      (cell_x & kCellMapQuadrantCoordinateBit) != 0;
  const uint16_t quadrant_row =
      (cell_y & kCellMapQuadrantCoordinateBit) != 0;
  const uint16_t quadrant_index =
      (uint16_t)(quadrant_row * kCellMapQuadrantsPerAxis + quadrant_column);
  const uint16_t quadrant_offset =
      (uint16_t)(quadrant_index * kCellMapQuadrantCells);
  uint16_t scratch_value;
  Cpu65816Add16Result final_addition;
  const bool can_use_binary_cell_index_formula =
      !decimal && encoded_town_index <= UINT8_MAX &&
      encoded_town_index % kEncodedTownIndexScale == 0;
  if (can_use_binary_cell_index_formula) {
    /* The game stores town*2 in $7BFB. This is every legitimate caller and
     * is the consolidation path shared with the bridge-sidecar consumers. */
    const unsigned town = encoded_town_index / kEncodedTownIndexScale;
    const uint16_t cell_index =
        ActRaiser_CellMarkIndex(town, cell_x, cell_y);
    scratch_value = (uint16_t)(cell_index - quadrant_offset);
    final_addition = AddCellMapWords(quadrant_offset, scratch_value, false);
  } else {
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
    scratch_value = scratch_addition.value;
    final_addition = AddCellMapWords(
        quadrant_offset, scratch_value, decimal);
  }
  cpu_write16(cpu, cpu->DB, kCellMapScratch, scratch_value);
  cpu->A = final_addition.value;
  cpu->X = final_addition.value;

  cpu->P = (uint8_t)((cpu->P & (CPU_P_I | CPU_P_D | CPU_P_X)) | CPU_P_M |
                     (final_addition.carry ? CPU_P_C : 0) |
                     (final_addition.value == 0 ? CPU_P_Z : 0) |
                     (final_addition.overflow ? CPU_P_V : 0) |
                     (final_addition.value & kWordSignBit ? CPU_P_N : 0));
  cpu_p_to_mirrors(cpu);
  cpu->S = (uint16_t)(cpu->S + k65816RtsStackBytes);
  return RECOMP_RETURN_NORMAL;
}
