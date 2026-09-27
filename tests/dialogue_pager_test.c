#include "localization/dialogue_pager.h"
#include "localization/text_presentation.h"
#include <assert.h>
#include <stdio.h>

static void Present(const ArDialoguePager *pager, uint32_t end, bool drawn) {
  ArTextPresentation_BeginFrame(pager->ticket);
  ArTextPresentation_ReportPage(pager->ticket, pager->start, end);
  if (drawn) ArTextPresentation_MarkReady(pager->ticket);
  ArTextPresentation_EndFrame();
}

int main(void) {
  ArDialoguePager pager;
  ArDialoguePager_Begin(&pager, true);
  assert(ArDialoguePager_Update(&pager, 40, 40) == kArDialoguePage_Measuring);
  assert(!ArDialoguePager_Advance(&pager, pager.ticket));
  Present(&pager, 16, true);
  assert(ArDialoguePager_Update(&pager, 40, 15) == kArDialoguePage_Revealing);
  assert(!ArDialoguePager_Advance(&pager, pager.ticket));
  assert(ArDialoguePager_Update(&pager, 40, 16) == kArDialoguePage_AwaitingInput);
  assert(ArDialoguePager_Advance(&pager, pager.ticket));
  assert(pager.start == 16);
  assert(!ArDialoguePager_Advance(&pager, pager.ticket));
  /* Instant reveal cannot reuse the previous viewport's measurement. */
  assert(ArDialoguePager_Update(&pager, 40, 40) == kArDialoguePage_Measuring);
  Present(&pager, 32, true);
  assert(ArDialoguePager_Update(&pager, 40, 40) == kArDialoguePage_AwaitingInput);
  assert(ArDialoguePager_Advance(&pager, pager.ticket));
  Present(&pager, 40, true);
  assert(ArDialoguePager_Update(&pager, 40, 40) == kArDialoguePage_Revealing);
  assert(!ArDialoguePager_Advance(&pager, pager.ticket));

  const uint64_t retired = pager.ticket;
  ArDialoguePager_Begin(&pager, true);
  Present(&pager, 16, true);
  assert(ArDialoguePager_Update(&pager, 40, 40) == kArDialoguePage_AwaitingInput);
  assert(!ArDialoguePager_Advance(&pager, retired));
  assert(pager.start == 0);
  ArDialoguePager_Begin(&pager, true);
  Present(&pager, 16, false);
  assert(ArDialoguePager_Update(&pager, 40, 40) == kArDialoguePage_Failed);
  assert(!ArDialoguePager_Advance(&pager, pager.ticket));
  ArDialoguePager_Begin(&pager, false);
  assert(!pager.ticket && !pager.start);
  assert(ArDialoguePager_Update(&pager, 40, 40) == kArDialoguePage_Revealing);
  ArDialoguePager_Begin(&pager, true);
  assert(ArDialoguePager_Update(&pager, 0, 0) == kArDialoguePage_Revealing);
  Present(&pager, 8, true);
  assert(ArDialoguePager_Update(&pager, 8, 8) == kArDialoguePage_Revealing);
  puts("Dialogue paging preserves reveal, acknowledgements and window identity");
  return 0;
}
