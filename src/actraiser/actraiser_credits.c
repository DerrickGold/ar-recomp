#include "actraiser/actraiser_credits.h"

#include "actraiser_game.h"

/* Keep these phases separate: AB30 may spend 32 waits fading the OLD page
 * before it copies the selected one. Neither selection nor a staging clear
 * changes what is still in VRAM. -1 means no observed owner, never page zero. */
static int s_selected = kActRaiserCreditsNoPage;
static int s_staged = kActRaiserCreditsNoPage;
static int s_presented = kActRaiserCreditsNoPage;
static bool s_awaiting_copy;

int ActRaiserCredits_PresentedPage(void) { return s_presented; }

void ActRaiserCredits_Reset(void) {
  s_selected = s_staged = s_presented = kActRaiserCreditsNoPage;
  s_awaiting_copy = false;
}

void ActRaiserCredits_ObserveScene(uint8_t map_group, uint8_t map_number) {
  if (map_group != 8 || map_number != 1) ActRaiserCredits_Reset();
}

static bool InCredits(CpuState *cpu) {
  return cpu && cpu_read8(cpu, 0, kActRaiserWram_MapGroup) == 8 &&
      cpu_read8(cpu, 0, kActRaiserWram_CurrentMap) == 1;
}

bool ActRaiser_CreditsObserveSelection(CpuState *cpu) {
  s_selected = kActRaiserCreditsNoPage;
  s_awaiting_copy = false;
  if (InCredits(cpu) && cpu->PB == 2 && cpu->m_flag && !cpu->x_flag &&
      !cpu->emulation && cpu->D == 0 &&
      (uint8_t)cpu->A < kActRaiserCreditsPageCount) {
    s_selected = (uint8_t)cpu->A;
    s_awaiting_copy = true;
  }
  return false; /* Always execute the original page presenter. */
}

void ActRaiserCredits_ObserveClear(void) {
  s_staged = kActRaiserCreditsNoPage;
}

void ActRaiserCredits_ObserveWait(CpuState *cpu) {
  if (!s_awaiting_copy || !InCredits(cpu) || cpu->PB != 2 ||
      !cpu->m_flag || cpu->x_flag || cpu->emulation || cpu->D != 0)
    return;
  /* AB65 is the first incoming-fade wait, after MVN and INC $F1. Its JSR
   * return is AB67; the saved page lies immediately above that return frame.
   * Observe this once per selection, so later fade waits cannot resurrect a
   * cleared page. Do not sample A: it is now the brightness counter. */
  if (cpu_read16(cpu, 0, (uint16_t)(cpu->S + 1)) != 0xab67 ||
      cpu_read8(cpu, 0, (uint16_t)(cpu->S + 3)) != s_selected)
    return;
  s_staged = s_selected;
  s_awaiting_copy = false;
}

void ActRaiserCredits_ObserveUpload(bool lower_rows) {
  /* The unconditional top-four-row upload alone never commits a new page. */
  if (lower_rows) s_presented = s_staged;
}
