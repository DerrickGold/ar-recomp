#ifndef AR_ACTRAISER_CREDITS_H
#define AR_ACTRAISER_CREDITS_H
/* ActRaiserCredits: follows which native credits page is on screen (a page
 * counts as presented only after its BG3 DMA), independent of language and
 * font, for the localized credits. Its observers never change native state.
 * Phase: game (native CPU hooks).
 * Tests: tests/actraiser_credits_test.c */

#include "snesrecomp/game/cpu.h"

enum { kActRaiserCreditsNoPage = -1, kActRaiserCreditsPageCount = 20 };

/* US native credits ownership, independent of the selected language/font.
 * A selected page becomes presentable only after its BG3 DMA completes. */
int ActRaiserCredits_PresentedPage(void);
void ActRaiserCredits_Reset(void);
void ActRaiserCredits_ObserveScene(uint8_t map_group, uint8_t map_number);
void ActRaiserCredits_ObserveClear(void);
void ActRaiserCredits_ObserveUpload(bool lower_rows);

/* Native seams: AB30 entry, A85E before its JSR frame is popped, and AEEB.
 * Selection/wait observers never change native CPU, memory, or timing. */
bool ActRaiser_CreditsObserveSelection(CpuState *cpu);
void ActRaiserCredits_ObserveWait(CpuState *cpu);

#endif
