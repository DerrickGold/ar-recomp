#include "actraiser/actraiser_town_metatile.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "cpu_65816_math.h"

enum {
  kTownMetatileCellX = 0x7CAF,
  kTownMetatileCellY = 0x7CB1,
  kTownMetatileId = 0x7CB5,
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

  fprintf(stderr, "FATAL: invalid simulation-town metatile atlas %d\n",
          (int)atlas);
  abort();
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

static uint16_t EmulateWordPush(CpuState *cpu, uint16_t stack_pointer,
                                uint16_t value) {
  cpu_write8(cpu, k65816StackBank, stack_pointer,
             (uint8_t)(value >> CHAR_BIT));
  stack_pointer = (uint16_t)(stack_pointer - 1);
  cpu_write8(cpu, k65816StackBank, stack_pointer, (uint8_t)value);
  return (uint16_t)(stack_pointer - 1);
}

static void RequireTownMetatileEntryMode(CpuState *cpu,
                                          const char *routine_name) {
  if (!cpu->m_flag && !cpu->x_flag && !cpu->emulation) return;
  fprintf(stderr,
          "FATAL: %s HLE requires native mode with 16-bit A/X/Y\n",
          routine_name);
  abort();
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
  RequireTownMetatileEntryMode(cpu, routine_name);

  const uint16_t cell_x =
      cpu_read16(cpu, cpu->DB, kTownMetatileCellX);
  const uint16_t cell_y =
      cpu_read16(cpu, cpu->DB, kTownMetatileCellY);
  const uint8_t metatile_id =
      (uint8_t)cpu_read16(cpu, cpu->DB, kTownMetatileId);
  const TownMetatileAddressResult address = CalculateCpuDestination(
      cell_x, cell_y, cpu->_flag_D != 0);

  uint16_t stack_pointer = cpu->S;
  stack_pointer = EmulateWordPush(
      cpu, stack_pointer, address.horizontal_byte_offset);
  EmulateWordPush(cpu, stack_pointer, address.within_quadrant.value);

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
