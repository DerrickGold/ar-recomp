#include "localization/text_presentation.h"

static uint64_t s_frame_ticket, s_failed_ticket;
static bool s_frame_ready;

void ArTextPresentation_BeginFrame(uint64_t ticket) {
  s_frame_ticket = ticket;
  s_frame_ready = false;
}

void ArTextPresentation_MarkReady(uint64_t ticket) {
  if (ticket && ticket == s_frame_ticket)
    s_frame_ready = true;
}

void ArTextPresentation_EndFrame(void) {
  if (!s_frame_ready && s_frame_ticket > s_failed_ticket)
    s_failed_ticket = s_frame_ticket;
  s_frame_ticket = 0;
}

bool ArTextPresentation_Failed(uint64_t ticket) {
  return ticket && ticket == s_failed_ticket;
}
