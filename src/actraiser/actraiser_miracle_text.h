#ifndef ACTRAISER_MIRACLE_TEXT_H
#define ACTRAISER_MIRACLE_TEXT_H
#include "regional/regional_costs.h"
#include "snesrecomp/game/cpu.h"

/* Audited $901C -> $9278 glyph-delay caller only, before its first frame wait.
 * Keeps the native digit field width and palette/flags; never edits ROM or
 * tries to reinterpret unrelated translator text. */
void ActRaiserMiracle_UpdateNativeDigit(CpuState *cpu,
                                       const ArRegionalCostSnapshot *prices);
#endif
