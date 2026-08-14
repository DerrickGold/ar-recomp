#include "actraiser/actraiser_cell_map.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

enum {
  kCellMapTownIndex = 0x7BFB,
  kCellMapScratch = 0x7C05,
  kCellMapCellX = 0x7C4B,
  kCellMapCellY = 0x7C4D,
};

typedef struct Add16Result {
  uint16_t value;
  bool carry;
  bool overflow;
} Add16Result;

static uint16_t Swap16(uint16_t value) {
  return (uint16_t)((value << 8) | (value >> 8));
}

/* Match the recompiler's 65816 ADC model, including the decimal-mode edge.
 * Every ADC in $9710 is preceded by CLC, so this helper intentionally has no
 * carry input. */
static Add16Result Add16(uint16_t a, uint16_t b, bool decimal) {
  Add16Result result = { 0 };
  if (!decimal) {
    const uint32_t sum = (uint32_t)a + b;
    result.value = (uint16_t)sum;
    result.carry = (sum & 0x10000u) != 0;
    result.overflow = ((a ^ result.value) & (b ^ result.value) & 0x8000u) != 0;
    return result;
  }

  uint32_t adjusted = 0;
  unsigned carry = 0;
  unsigned high_nibble_carry = 0;
  for (unsigned shift = 0; shift < 16; shift += 4) {
    unsigned digit = ((a >> shift) & 0x0Fu) +
                     ((b >> shift) & 0x0Fu) + carry;
    carry = digit > 9;
    if (shift == 8) high_nibble_carry = carry;
    if (carry) digit += 6;
    adjusted |= (uint32_t)(digit & 0x0Fu) << shift;
  }
  result.value = (uint16_t)adjusted;
  result.carry = carry != 0;
  const uint32_t visible_high_sum =
      (a & 0xF000u) + (b & 0xF000u) + (high_nibble_carry << 12);
  result.overflow =
      ((a ^ visible_high_sum) & (b ^ visible_high_sum) & 0x8000u) != 0;
  return result;
}

uint16_t ActRaiser_CellMarkIndex(unsigned town, uint8_t cell_x,
                                uint8_t cell_y) {
  return (uint16_t)(town * 0x400u +
                    ((cell_y & 0x10u) ? 0x200u : 0) +
                    ((cell_x & 0x10u) ? 0x100u : 0) +
                    (cell_y & 0x0Fu) * 0x10u +
                    (cell_x & 0x0Fu));
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

  const uint16_t cell_x_word =
      cpu_read16(cpu, cpu->DB, kCellMapCellX);
  const uint16_t cell_y_word =
      cpu_read16(cpu, cpu->DB, kCellMapCellY);
  const uint16_t town_index =
      cpu_read16(cpu, cpu->DB, kCellMapTownIndex);
  const uint8_t cell_x = (uint8_t)(cell_x_word & 0x1Fu);
  const uint8_t cell_y = (uint8_t)(cell_y_word & 0x1Fu);
  const bool decimal = cpu->_flag_D != 0;

  const uint16_t quadrant =
      (uint16_t)(((cell_y & 0x10u) ? 2u : 0u) +
                 ((cell_x & 0x10u) ? 1u : 0u));
  uint16_t scratch_value;
  Add16Result final;
  if (!decimal && (town_index & 0xFF01u) == 0) {
    /* The game stores town*2 in $7BFB. This is every legitimate caller and
     * is the consolidation path shared with the bridge-sidecar consumers. */
    final.value = ActRaiser_CellMarkIndex(
        (unsigned)(town_index >> 1), cell_x, cell_y);
    scratch_value = (uint16_t)(final.value - (quadrant << 8));
    final = Add16((uint16_t)(quadrant << 8), scratch_value, false);
  } else {
    /* Preserve instruction-exact behavior for malformed raw town indexes and
     * for D=1, neither of which occurs in ordinary gameplay. */
    const uint16_t row = (uint16_t)((cell_y & 0x0Fu) << 4);
    const Add16Result within =
        Add16((uint16_t)(cell_x & 0x0Fu), row, decimal);
    const uint16_t town_base = (uint16_t)(Swap16(town_index) << 1);
    const Add16Result scratch = Add16(town_base, within.value, decimal);
    scratch_value = scratch.value;
    final = Add16((uint16_t)(quadrant << 8), scratch_value, decimal);
  }
  cpu_write16(cpu, cpu->DB, kCellMapScratch, scratch_value);
  cpu->A = final.value;
  cpu->X = final.value;

  cpu->P = (uint8_t)((cpu->P & (CPU_P_I | CPU_P_D | CPU_P_X)) | CPU_P_M |
                     (final.carry ? CPU_P_C : 0) |
                     (final.value == 0 ? CPU_P_Z : 0) |
                     (final.overflow ? CPU_P_V : 0) |
                     (final.value & 0x8000u ? CPU_P_N : 0));
  cpu_p_to_mirrors(cpu);
  cpu->S = (uint16_t)(cpu->S + 2);
  return RECOMP_RETURN_NORMAL;
}
