#include "actraiser/actraiser_town_metatile.h"

#include <limits.h>
#include <stdbool.h>

#include "actraiser/actraiser_cpu_hle_internal.h"
#include "actraiser/actraiser_hle_fatal.h"
#include "cpu_65816_math.h"

enum {
  kTownDrawCommandCount = 0x7C23,
  kTownDrawOriginCellX = 0x7CAB,
  kTownDrawOriginCellY = 0x7CAD,
  kTownMetatileCellX = 0x7CAF,
  kTownMetatileCellY = 0x7CB1,
  kTownMetatileId = 0x7CB5,
  kTownDrawStepListPointerOffset = 6,
  kTownDrawListRomBank = 0x03,
  kTownDrawListCountBytes = 1,
  kTownDrawCommandBytes = 3,
  kTownDrawCommandCellXOffset = 0,
  kTownDrawCommandCellYOffset = 1,
  kTownDrawCommandMetatileOffset = 2,
  kTownDrawMetatileCopyReturnAddress = 0xA5CF,
  kTerrainMetatileDefinitionsBase = 0x2100,
  kStructureMetatileDefinitionsBase = 0x3100,
  kMetatileDefinitionBytes = 8,
  kMetatileDefinitionColumns = 2,
  kMetatileDefinitionRows = 2,
  kTilemapWordBytes = sizeof(uint16_t),
  kMetatileCellWidthBytes =
      kMetatileDefinitionColumns * kTilemapWordBytes,
  kTownQuadrantTileRowTiles = 32,
  kTownTilemapTileRowBytes =
      kTownQuadrantTileRowTiles * kTilemapWordBytes,
  kMetatileCellRowBytes =
      kMetatileDefinitionRows * kTownTilemapTileRowBytes,
  kTownQuadrantSideCells = 16,
  kTownQuadrantsPerRow = 2,
  kTownQuadrantTileRows =
      kTownQuadrantSideCells * kMetatileDefinitionRows,
  kTownQuadrantBytes =
      kTownQuadrantTileRows * kTownTilemapTileRowBytes,
  kTownCellLocalCoordinateMask = kTownQuadrantSideCells - 1,
  kMetatileCollisionAttributeBit = 0x0200,
  kMetatileAttributeMask = UINT16_MAX ^ kMetatileCollisionAttributeBit,
  kWordBits = sizeof(uint16_t) * CHAR_BIT,
  kWordSignBit = 1u << (kWordBits - 1),
};

typedef struct TownMetatileAddressResult {
  uint16_t horizontal_byte_offset;
  Cpu65816Add16Result within_quadrant;
  Cpu65816Add16Result destination;
} TownMetatileAddressResult;

static uint16_t TownMetatileDefinitionBase(
    ActRaiserTownMetatileAtlas atlas) {
  switch (atlas) {
    case kActRaiserTownMetatileAtlas_Terrain:
      return kTerrainMetatileDefinitionsBase;
    case kActRaiserTownMetatileAtlas_Structure:
      return kStructureMetatileDefinitionsBase;
  }

  ActRaiserHleFatal("invalid simulation-town metatile atlas %d", (int)atlas);
}

static unsigned TownMetatileQuadrantIndex(uint16_t cell_x,
                                           uint16_t cell_y) {
  const unsigned quadrant_column = cell_x >= kTownQuadrantSideCells;
  const unsigned quadrant_row = cell_y >= kTownQuadrantSideCells;
  return quadrant_row * kTownQuadrantsPerRow + quadrant_column;
}

static uint16_t TownMetatileDestinationOffset(uint16_t cell_x,
                                               uint16_t cell_y) {
  const unsigned local_x = cell_x & kTownCellLocalCoordinateMask;
  const unsigned local_y = cell_y & kTownCellLocalCoordinateMask;
  return (uint16_t)(TownMetatileQuadrantIndex(cell_x, cell_y) *
                        kTownQuadrantBytes +
                    local_y * kMetatileCellRowBytes +
                    local_x * kMetatileCellWidthBytes);
}

static uint16_t CopyTownMetatileAtOffset(
    CpuState *cpu, uint8_t destination_bank, uint16_t destination,
    uint8_t metatile_id, ActRaiserTownMetatileAtlas atlas) {
  const uint16_t definition = (uint16_t)(
      TownMetatileDefinitionBase(atlas) +
      metatile_id * kMetatileDefinitionBytes);
  uint16_t last_copied_word = 0;

  for (unsigned tile_row = 0; tile_row < kMetatileDefinitionRows;
       tile_row++) {
    for (unsigned tile_column = 0;
         tile_column < kMetatileDefinitionColumns; tile_column++) {
      const unsigned definition_tile =
          tile_row * kMetatileDefinitionColumns + tile_column;
      const uint16_t source = (uint16_t)(
          definition + definition_tile * kTilemapWordBytes);
      const uint16_t target = (uint16_t)(
          destination + tile_row * kTownTilemapTileRowBytes +
          tile_column * kTilemapWordBytes);
      last_copied_word = (uint16_t)(
          cpu_read16(cpu, kSnesLowWramBank, source) &
          kMetatileAttributeMask);
      cpu_write16(cpu, destination_bank, target, last_copied_word);
    }
  }

  return last_copied_word;
}

void ActRaiser_CopyTownMetatile(CpuState *cpu, uint8_t destination_bank,
                                uint16_t cell_x, uint16_t cell_y,
                                uint8_t metatile_id,
                                ActRaiserTownMetatileAtlas atlas) {
  if (!cpu) return;
  CopyTownMetatileAtOffset(
      cpu, destination_bank,
      TownMetatileDestinationOffset(cell_x, cell_y), metatile_id, atlas);
}

static TownMetatileAddressResult CalculateCpuDestination(
    uint16_t cell_x, uint16_t cell_y, bool decimal) {
  TownMetatileAddressResult result;
  result.horizontal_byte_offset = (uint16_t)(
      (cell_x & kTownCellLocalCoordinateMask) * kMetatileCellWidthBytes);
  const uint16_t vertical_byte_offset = (uint16_t)(
      (cell_y & kTownCellLocalCoordinateMask) * kMetatileCellRowBytes);
  result.within_quadrant = Cpu65816_Add16(
      vertical_byte_offset, result.horizontal_byte_offset, false, decimal);
  const uint16_t quadrant_byte_offset = (uint16_t)(
      TownMetatileQuadrantIndex(cell_x, cell_y) * kTownQuadrantBytes);
  result.destination = Cpu65816_Add16(
      quadrant_byte_offset, result.within_quadrant.value, false, decimal);
  return result;
}

/* $03:9B5A and $03:9C43 are identical bounded leaves except for their source
 * definition tables ($7E:2100 terrain and $7E:3100 structures). Both derive
 * a quadrant-paged destination from $7CAF/$7CB1, clear attribute bit 9 from
 * four source tile words, and write the 2x2 result through DB. The shared copy
 * core also serves host-side bridge reconstruction; this wrapper adds the
 * architectural register, flag, stack-RAM, and RTS effects of the ROM body. */
static RecompReturn TownCopyMetatileHle(CpuState *cpu,
                                        ActRaiserTownMetatileAtlas atlas,
                                        const char *routine_name) {
  if (!cpu) return RECOMP_RETURN_NORMAL;

  cpu_mirrors_to_p(cpu);
  ActRaiserCpuHle_RequireEntryMode(
      cpu, routine_name,
      kActRaiserCpuHleEntryMode_Native16BitAccumulatorAndIndexes);

  const uint16_t cell_x =
      cpu_read16(cpu, cpu->DB, kTownMetatileCellX);
  const uint16_t cell_y =
      cpu_read16(cpu, cpu->DB, kTownMetatileCellY);
  const uint8_t metatile_id =
      (uint8_t)cpu_read16(cpu, cpu->DB, kTownMetatileId);
  const TownMetatileAddressResult address = CalculateCpuDestination(
      cell_x, cell_y, cpu->_flag_D != 0);

  uint16_t stack_pointer = cpu->S;
  stack_pointer = ActRaiserCpuHle_PushWordAt(
      cpu, stack_pointer, address.horizontal_byte_offset);
  ActRaiserCpuHle_PushWordAt(
      cpu, stack_pointer, address.within_quadrant.value);

  const uint16_t final_accumulator = CopyTownMetatileAtOffset(
      cpu, cpu->DB, address.destination.value, metatile_id, atlas);
  cpu->A = final_accumulator;
  cpu->X = (uint16_t)(metatile_id * kMetatileDefinitionBytes);
  cpu->Y = address.destination.value;
  /* The third source-offset ASL always clears C: an eight-bit metatile id
   * reaches at most bit 10 in the 16-bit accumulator. */
  cpu->P = (uint8_t)(
      (cpu->P & (CPU_P_I | CPU_P_D)) |
      (final_accumulator == 0 ? CPU_P_Z : 0) |
      (address.destination.overflow ? CPU_P_V : 0) |
      (final_accumulator & kWordSignBit ? CPU_P_N : 0));
  cpu_p_to_mirrors(cpu);
  cpu->S = (uint16_t)(cpu->S + k65816RtsStackBytes);
  return RECOMP_RETURN_NORMAL;
}

RecompReturn ActRaiser_TownCopyTerrainMetatile(CpuState *cpu) {
  return TownCopyMetatileHle(
      cpu, kActRaiserTownMetatileAtlas_Terrain, "$03:9B5A");
}

RecompReturn ActRaiser_TownCopyStructureMetatile(CpuState *cpu) {
  return TownCopyMetatileHle(
      cpu, kActRaiserTownMetatileAtlas_Structure, "$03:9C43");
}

static bool ExecuteTownDrawList(
    CpuState *cpu, uint8_t destination_bank, uint16_t origin_cell_x,
    uint16_t origin_cell_y, uint16_t draw_list_address,
    unsigned maximum_commands, bool emulate_cpu_contract) {
  if (!cpu) return false;

  const uint8_t command_count = cpu_read8(
      cpu, kTownDrawListRomBank, draw_list_address);
  if (command_count == 0 || command_count > maximum_commands) return false;

  if (emulate_cpu_contract)
    cpu_write16(cpu, cpu->DB, kTownDrawCommandCount, command_count);

  uint16_t command_address = (uint16_t)(
      draw_list_address + kTownDrawListCountBytes);
  for (unsigned command_index = 0; command_index < command_count;
       command_index++) {
    const uint8_t cell_x_offset = cpu_read8(
        cpu, kTownDrawListRomBank,
        (uint16_t)(command_address + kTownDrawCommandCellXOffset));
    const uint8_t cell_y_offset = cpu_read8(
        cpu, kTownDrawListRomBank,
        (uint16_t)(command_address + kTownDrawCommandCellYOffset));
    const uint8_t metatile_id = cpu_read8(
        cpu, kTownDrawListRomBank,
        (uint16_t)(command_address + kTownDrawCommandMetatileOffset));

    if (!emulate_cpu_contract) {
      ActRaiser_CopyTownMetatile(
          cpu, destination_bank,
          (uint16_t)(origin_cell_x + cell_x_offset),
          (uint16_t)(origin_cell_y + cell_y_offset), metatile_id,
          kActRaiserTownMetatileAtlas_Structure);
    } else {
      const bool decimal = cpu->_flag_D != 0;
      const Cpu65816Add16Result cell_x = Cpu65816_Add16(
          cell_x_offset, origin_cell_x, false, decimal);
      const Cpu65816Add16Result cell_y = Cpu65816_Add16(
          cell_y_offset, origin_cell_y, false, decimal);
      cpu_write16(cpu, cpu->DB, kTownMetatileCellX, cell_x.value);
      cpu_write16(cpu, cpu->DB, kTownMetatileCellY, cell_y.value);
      cpu_write16(cpu, cpu->DB, kTownMetatileId, metatile_id);

      /* PHX / JSR $9C43. Calling the CPU-facing metatile HLE retains the
       * nested routine's own two temporary word pushes and final ABI. */
      cpu->X = command_address;
      ActRaiserCpuHle_PushWord(cpu, cpu->X);
      ActRaiserCpuHle_PushWord(cpu, kTownDrawMetatileCopyReturnAddress);
      cpu->host_return_valid = 1;
      if (ActRaiser_TownCopyStructureMetatile(cpu) !=
          RECOMP_RETURN_NORMAL)
        return false;

      cpu->X = ActRaiserCpuHle_PopWord(cpu);
      ActRaiserCpuHle_SetNegativeZero16(cpu, cpu->X);
      cpu->X = (uint16_t)(cpu->X + kTownDrawCommandBytes);

      const uint16_t commands_remaining = (uint16_t)(
          cpu_read16(cpu, cpu->DB, kTownDrawCommandCount) - 1);
      cpu_write16(cpu, cpu->DB, kTownDrawCommandCount,
                  commands_remaining);
      ActRaiserCpuHle_SetNegativeZero16(cpu, commands_remaining);
    }

    command_address = (uint16_t)(
        command_address + kTownDrawCommandBytes);
  }
  return true;
}

bool ActRaiser_ExecuteTownDrawList(CpuState *cpu, uint8_t destination_bank,
                                  uint16_t origin_cell_x,
                                  uint16_t origin_cell_y,
                                  uint16_t draw_list_address,
                                  unsigned maximum_commands) {
  return ExecuteTownDrawList(
      cpu, destination_bank, origin_cell_x, origin_cell_y,
      draw_list_address, maximum_commands, false);
}

/* $03:A591 resolves a draw-list pointer from the current reconstruction-step
 * record, then executes count + {dx,dy,metatile} commands through $03:9C43.
 * The shared executor also serves sidecar bridge restoration; this wrapper
 * adds the pointer indirection, scratch, register, flag, stack-RAM, nested-JSR,
 * and outer RTS effects of the original routine. */
RecompReturn ActRaiser_TownExecuteDrawList(CpuState *cpu) {
  if (!cpu) return RECOMP_RETURN_NORMAL;

  cpu_mirrors_to_p(cpu);
  ActRaiserCpuHle_RequireEntryMode(
      cpu, "$03:A591",
      kActRaiserCpuHleEntryMode_Native16BitAccumulatorAndIndexes);

  const uint16_t saved_x = cpu->X;
  ActRaiserCpuHle_PushWord(cpu, saved_x);
  const uint16_t draw_step_address = cpu_read16(
      cpu, cpu->DB,
      (uint16_t)(saved_x + kTownDrawStepListPointerOffset));
  const uint16_t draw_list_address = cpu_read16(
      cpu, kTownDrawListRomBank, draw_step_address);
  const uint16_t origin_cell_x = cpu_read16(
      cpu, cpu->DB, kTownDrawOriginCellX);
  const uint16_t origin_cell_y = cpu_read16(
      cpu, cpu->DB, kTownDrawOriginCellY);

  if (!ExecuteTownDrawList(
          cpu, cpu->DB, origin_cell_x, origin_cell_y, draw_list_address,
          UINT8_MAX, true)) {
    ActRaiserHleFatal(
        "$03:A591 HLE received an invalid town draw list at $03:%04X",
        draw_list_address);
  }

  cpu->X = ActRaiserCpuHle_PopWord(cpu);
  ActRaiserCpuHle_SetNegativeZero16(cpu, cpu->X);
  cpu->S = (uint16_t)(cpu->S + k65816RtsStackBytes);
  return RECOMP_RETURN_NORMAL;
}
