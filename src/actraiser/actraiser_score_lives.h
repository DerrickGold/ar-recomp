#ifndef AR_ACTRAISER_SCORE_LIVES_H
#define AR_ACTRAISER_SCORE_LIVES_H
/* ActRaiser score hook: runs the native score helper ($00:873C) and applies the
 * regional score-lives rules around it.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_score_lives_test.c */
#include "snesrecomp/game/cpu.h"

bool ActRaiser_ScoreLivesEntry(CpuState *cpu);
RecompReturn ActRaiser_ScoreLives(CpuState *cpu);
#endif
