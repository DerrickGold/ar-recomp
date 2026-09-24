#ifndef ACTRAISER_TITLE_ART_H
#define ACTRAISER_TITLE_ART_H
#include "snesrecomp/game/cpu.h"
bool ActRaiser_TitlePaletteEntry(CpuState *cpu);
bool ActRaiser_TitleCharactersEntry(CpuState *cpu);
bool ActRaiser_TitleMapEntry(CpuState *cpu);
RecompReturn ActRaiser_LoadTitlePalette(CpuState *cpu);
RecompReturn ActRaiser_LoadTitleCharacters(CpuState *cpu);
RecompReturn ActRaiser_LoadTitleMap(CpuState *cpu);
#endif
