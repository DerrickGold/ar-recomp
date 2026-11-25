#include "actraiser/actraiser_rtl.h"
#include "actraiser_game.h"
#include "app/settings.h"
#include "support/test_assert.h"

uint8 g_ram[kActRaiserWramSize];
CpuState g_cpu;
Settings g_settings;

/* The other cheats and the level warp stay idle in this fixture. */
SrRunnerHandle *ActRaiser_Runner(void) {
  assert(!"unexpected runner query");
  return NULL;
}
const SnesRunnerApi *ActRaiser_RunnerApi(void) {
  assert(!"unexpected runner API query");
  return NULL;
}
bool ActRaiser_QueryInputState(SrInputStateSnapshot *state) {
  (void)state;
  assert(!"unexpected input query");
  return false;
}
bool ActRaiser_QueryPpuState(SrPpuStateSnapshot *state) {
  (void)state;
  assert(!"unexpected PPU query");
  return false;
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu;
  (void)bank;
  (void)address;
  assert(!"unexpected ROM read");
  return 0;
}
void ActRaiserDeathHeim_Reset(void) { assert(!"unexpected warp"); }

static void ExpectFrozenClock(uint16 time, uint16 time_after_tick) {
  g_ram[kActRaiserWram_ActionTimerStop] = 0;
  g_ram[kActRaiserWram_ActionTimerDivider] = 0;
  ActRaiser_WriteWram16(kActRaiserWram_ActionTimerLow, time);
  for (unsigned frame = 0; frame < 120; ++frame) {
    ActRaiser_ApplyCheats();
    assert(ActRaiser_ReadWram16(kActRaiserWram_ActionTimerLow) == time);
    /* Native $02:BC82 spends a second when decrementing the divider makes it negative. */
    if (--g_ram[kActRaiserWram_ActionTimerDivider] & 0x80) {
      g_ram[kActRaiserWram_ActionTimerDivider] = kActRaiserActionTimerInitialDivider;
      ActRaiser_WriteWram16(kActRaiserWram_ActionTimerLow, time_after_tick);
    }
  }
}

int main(void) {
  g_settings.cheat_freeze_timer = true;
  g_ram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  g_ram[kActRaiserWram_ActionTimerDivider] = 7;
  ActRaiser_ApplyCheats();
  assert(g_ram[kActRaiserWram_ActionTimerDivider] == 7);

  g_ram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_Fillmore;
  g_ram[kActRaiserWram_CurrentMap] = 3;
  ExpectFrozenClock(0x0010, 0x0009);

  /* After 0010, the score tally debits one BCD unit every frame down to zero. */
  g_ram[kActRaiserWram_ActionTimerStop] = 1;
  g_ram[kActRaiserWram_ActionTimerDivider] = 7;
  for (int balance = 9; balance >= 0; --balance) {
    ActRaiser_WriteWram16(kActRaiserWram_ActionTimerLow, (uint16)balance);
    ActRaiser_ApplyCheats();
    assert(ActRaiser_ReadWram16(kActRaiserWram_ActionTimerLow) == balance);
    assert(g_ram[kActRaiserWram_ActionTimerDivider] == 7);
    assert(g_ram[kActRaiserWram_ActionTimerStop] == 1);
  }

  /* The next act in the same region loads its own starting time. */
  g_ram[kActRaiserWram_CurrentMap] = 4;
  ExpectFrozenClock(0x0299, 0x0298);
  g_settings.cheat_freeze_timer = false;
  g_ram[kActRaiserWram_ActionTimerDivider] = 0;
  ActRaiser_ApplyCheats();
  assert(g_ram[kActRaiserWram_ActionTimerDivider] == 0);
  return 0;
}
