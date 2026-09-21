#ifndef ACTRAISER_CREDITS_H
#define ACTRAISER_CREDITS_H

#include "snesrecomp/game/cpu.h"

enum { kActRaiserCreditsNoPage = -1, kActRaiserCreditsPageCount = 20 };

/* US native credits ownership, independent of the selected language/font.
 * A selected page becomes presentable only after its BG3 DMA completes. */
int ActRaiserCredits_PresentedPage(void);
void ActRaiserCredits_Reset(void);
void ActRaiserCredits_ObserveScene(uint8_t map_group, uint8_t map_number);
void ActRaiserCredits_ObserveClear(void);

/* Native seams: AB30 entry, A85E before its JSR frame is popped, and AEEB.
 * Selection/wait observers never change native CPU, memory, or timing. */
bool ActRaiser_CreditsObserveSelection(CpuState *cpu);
void ActRaiserCredits_ObserveWait(CpuState *cpu);
bool ActRaiser_CreditsUploadEntry(CpuState *cpu);
RecompReturn ActRaiser_CreditsUpload(CpuState *cpu);

#endif
