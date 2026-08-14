#ifndef ACTRAISER_TOWN_STRUCTURE_STEPS_H
#define ACTRAISER_TOWN_STRUCTURE_STEPS_H

#include <stdint.h>

#include "cpu_state.h"

typedef enum ActRaiserTownStructureStepProgramFamily {
  kActRaiserTownStructureStepProgramFamily_Construction = 0,
  kActRaiserTownStructureStepProgramFamily_Rebuild,
} ActRaiserTownStructureStepProgramFamily;

/* Resolve one town-structure visual step program through the class and
 * variant pointer tables in ROM bank $03. This semantic entry point does not
 * mutate architectural CPU state. */
uint16_t ActRaiser_ResolveTownStructureStepProgram(
    CpuState *cpu, uint16_t class_offset, uint16_t variant_offset,
    ActRaiserTownStructureStepProgramFamily program_family);

/* Initialize one record's eight-byte visual step-machine slot. This semantic
 * entry point changes only the four fields initialized by the native armer. */
void ActRaiser_ArmTownStructureStepProgram(
    CpuState *cpu, uint8_t destination_bank, uint8_t record_index,
    uint16_t program_address);

/* Whole-body HLEs for the rebuild and construction program armers. */
RecompReturn ActRaiser_TownArmRebuildStepProgram(CpuState *cpu);
RecompReturn ActRaiser_TownArmConstructionStepProgram(CpuState *cpu);

#endif /* ACTRAISER_TOWN_STRUCTURE_STEPS_H */
