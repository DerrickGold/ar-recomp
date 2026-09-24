#ifndef ACTRAISER_TITLE_ART_H
#define ACTRAISER_TITLE_ART_H
#include "snesrecomp/game/cpu.h"
/* US title loaders: palette $02:B34D, characters $02:B2D2, map $02:B4AB.
 * The palette entry captures one media choice for all three planes. Each
 * guarded replacement preserves its native return/transfer contract and falls
 * back to US data if the donor is unavailable. Text/localization is separate. */
bool ActRaiser_TitlePaletteEntry(CpuState *cpu);
bool ActRaiser_TitleCharactersEntry(CpuState *cpu);
bool ActRaiser_TitleMapEntry(CpuState *cpu);
RecompReturn ActRaiser_LoadTitlePalette(CpuState *cpu);
RecompReturn ActRaiser_LoadTitleCharacters(CpuState *cpu);
RecompReturn ActRaiser_LoadTitleMap(CpuState *cpu);
#endif
