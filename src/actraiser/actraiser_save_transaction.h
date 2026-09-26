#ifndef AR_ACTRAISER_SAVE_TRANSACTION_H
#define AR_ACTRAISER_SAVE_TRANSACTION_H
/* ActRaiser save-story hook: runs the native story save ($03:A656) with its
 * own frame and width, keeping host frame persistence inhibited while the
 * native writer copies and checksums SRAM.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_save_transaction_test.c */

#include "snesrecomp/game/cpu.h"

bool ActRaiser_SaveStoryEntry(CpuState *cpu);
RecompReturn ActRaiser_SaveStory(CpuState *cpu);

#endif
