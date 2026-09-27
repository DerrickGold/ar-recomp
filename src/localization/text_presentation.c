#include "localization/text_presentation.h"

static uint64_t s_frame_ticket, s_failed_ticket;
static bool s_frame_ready;
static uint64_t s_page_ticket;
static uint32_t s_page_start, s_page_end, s_frame_start, s_frame_end;
static bool s_frame_page;

void ArTextPresentation_BeginFrame(uint64_t ticket) {
  s_frame_ticket = ticket;
  s_frame_ready = false;
  s_frame_page = false;
}

void ArTextPresentation_MarkReady(uint64_t ticket) {
  if (ticket && ticket == s_frame_ticket)
    s_frame_ready = true;
}

void ArTextPresentation_EndFrame(void) {
  if (!s_frame_ready && s_frame_ticket > s_failed_ticket)
    s_failed_ticket = s_frame_ticket;
  if (s_frame_ready && s_frame_page && s_frame_ticket >= s_page_ticket) {
    s_page_ticket = s_frame_ticket;
    s_page_start = s_frame_start;
    s_page_end = s_frame_end;
  }
  s_frame_ticket = 0;
}

bool ArTextPresentation_Failed(uint64_t ticket) {
  return ticket && ticket == s_failed_ticket;
}

void ArTextPresentation_ReportPage(uint64_t ticket, uint32_t start, uint32_t end) {
  if (!ticket || ticket != s_frame_ticket || s_frame_page || end <= start) return;
  s_frame_page = true;
  s_frame_start = start;
  s_frame_end = end;
}

bool ArTextPresentation_PageEnd(uint64_t ticket, uint32_t start, uint32_t *end) {
  if (!end || !ticket || ticket != s_page_ticket || start != s_page_start ||
      ArTextPresentation_Failed(ticket)) return false;
  *end = s_page_end;
  return true;
}
