#ifndef AR_DIALOGUE_PAGER_H
#define AR_DIALOGUE_PAGER_H
/* ArDialoguePager: shared screenful progression for measured dialogue.
 * The presenter supplies boundaries; callers supply reveal progress and a
 * fresh acknowledgement. Native execution and input policy stay with callers.
 * Phase: game/presenter thread.
 * Tests: tests/dialogue_pager_test.c */

#include <stdbool.h>
#include <stdint.h>

typedef enum ArDialoguePageState {
  kArDialoguePage_Revealing,
  kArDialoguePage_Measuring,
  kArDialoguePage_AwaitingInput,
  kArDialoguePage_Failed,
} ArDialoguePageState;

typedef struct ArDialoguePager {
  uint64_t ticket;
  uint32_t start, end;
  ArDialoguePageState state;
} ArDialoguePager;

/* Each new window retires the previous ticket, even across game resets. A
 * disabled pager permits native reveal without waiting for font feedback. */
void ArDialoguePager_Begin(ArDialoguePager *pager, bool enhanced);
ArDialoguePageState ArDialoguePager_Update(
    ArDialoguePager *pager, uint32_t text_bytes, uint32_t revealed_bytes);
/* Supply the ticket captured before yielding to input. A replacement window
 * cannot consume an acknowledgement intended for the old one. */
bool ArDialoguePager_Advance(ArDialoguePager *pager, uint64_t ticket);

#endif
