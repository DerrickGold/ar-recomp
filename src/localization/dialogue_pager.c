#include "localization/dialogue_pager.h"
#include "localization/text_presentation.h"

static uint64_t s_next_ticket;

void ArDialoguePager_Begin(ArDialoguePager *pager, bool enhanced) {
  *pager = (ArDialoguePager){.ticket = enhanced ? ++s_next_ticket : 0};
}

ArDialoguePageState ArDialoguePager_Update(
    ArDialoguePager *pager, uint32_t text_bytes, uint32_t revealed_bytes) {
  pager->state = kArDialoguePage_Revealing;
  if (!pager->ticket) return pager->state;
  if (ArTextPresentation_Failed(pager->ticket))
    pager->state = kArDialoguePage_Failed;
  else if (text_bytes &&
           (!ArTextPresentation_PageEnd(pager->ticket, pager->start, &pager->end) ||
            pager->end <= pager->start || pager->end > text_bytes))
    pager->state = kArDialoguePage_Measuring;
  else if (text_bytes && pager->end < text_bytes && revealed_bytes >= pager->end)
    pager->state = kArDialoguePage_AwaitingInput;
  return pager->state;
}

bool ArDialoguePager_Advance(ArDialoguePager *pager, uint64_t ticket) {
  if (!ticket || ticket != pager->ticket ||
      pager->state != kArDialoguePage_AwaitingInput) return false;
  pager->start = pager->end;
  pager->state = kArDialoguePage_Measuring;
  return true;
}
