#include "actraiser/actraiser_world_travel.h"
#include "actraiser/actraiser_rtl.h"
#include "actraiser/actraiser_native_call.h"
#include "actraiser/actraiser_hle_fatal.h"
#include "actraiser_game.h"
#include "action/action_load_pacing.h"
#include "byte_order.h"
#include "replacements/music_replacements.h"
#include <string.h>

extern RecompReturn bank_02_98B7_M1X0(CpuState *cpu);
extern RecompReturn bank_02_847C_M1X0(CpuState *cpu);
extern RecompReturn bank_00_82D1_M1X0(CpuState *cpu);

enum TravelState { kIdle, kRequested, kDescending, kBlack, kReady, kReturning, kComplete };
static enum TravelState s_state;
static bool s_return;
static uint8_t s_world[kSnesWramSize];
static CpuState s_cpu;

bool ActRaiserWorldTravel_Begin(void) {
  if (s_state != kIdle || g_ram[kActRaiserWram_MapGroup] != kActRaiserMapGroup_NonAction ||
      g_ram[kActRaiserWram_CurrentMap] != kActRaiserNonActionMap_WorldMap) return false;
  s_return = false;
  s_state = kRequested;
  return true;
}
bool ActRaiserWorldTravel_Ready(void) { return s_state == kReady; }
bool ActRaiserWorldTravel_Complete(void) { return s_state == kComplete || s_state == kIdle; }
void ActRaiserWorldTravel_Return(void) {
  s_return = true;
  if (s_state == kRequested) s_state = kIdle;
}
void ActRaiserWorldTravel_Reset(void) { s_state = kIdle; s_return = false; }
bool ActRaiserWorldTravel_HoldsNativeTicks(void) {
  return s_state == kBlack || s_state == kReady || s_state == kComplete;
}

static void Native(ActRaiserNativeLeaf leaf, uint8_t bank, uint16_t caller, bool long_call) {
  if (ActRaiserNativeCall(&g_cpu, leaf, bank, caller, long_call) != RECOMP_RETURN_NORMAL)
    ActRaiserHleFatal("World travel escaped its native call frame");
}

static void Music(unsigned song) {
  g_cpu.P = CPU_P_M;
  cpu_p_to_mirrors(&g_cpu);
  g_cpu.PB = 2;
  g_cpu.DB = 0;
  g_cpu.D = 0;
  g_cpu.A = song;
  /* Same indexed-image call frame as $02:984D. This uses the native SPC
   * uploader, including regional routing and the shared replacement hooks. */
  Native(bank_02_98B7_M1X0, 2, 0x9850, true);
}

void ActRaiserWorldTravel_ServiceYield(void) {
  if (s_state != kRequested) return;
  s_state = kDescending;
  s_cpu = g_cpu;
  memcpy(s_world, g_ram, sizeof(s_world));
  const uint8_t display = RTL_GAME_PPU_DISPLAY_CONTROL(RtlGamePpuDisplayState());
  Music(9); /* Advent, table entry 8. */
  /* $02:8134's Advent initialization, retaining the imported destination's
   * exact palace focus instead of selecting an AR1 region's tile coordinate. */
  g_ram[0xfb] = 1;
  ByteOrder_WriteLe16(g_ram + 0x314, 0);
  ByteOrder_WriteLe16(g_ram + 0x316, 0x5e6);
  g_cpu.PB = 0;
  g_cpu.DB = 1;
  Native(bank_02_847C_M1X0, 2, 0x8140, true);

  /* Stop before the original main loop can consume Advent's AR1 destination
   * writes. Match the existing action loader's black/audio hold; cold imported
   * extraction may extend only this black endpoint, never the animation. */
  cpu_write8(&g_cpu, 0, 0x2100, 0x80);
  s_state = kBlack;
  bool completed = false;
  const uint64_t token = MusicReplacements_GetOneShotSnapshot(&completed);
  for (unsigned remaining = kActionLoadPacingInsertedFrames; remaining && !s_return; --remaining) {
    const uint64_t current = MusicReplacements_GetOneShotSnapshot(&completed);
    if (ActionLoadPacing_ShouldReleaseForOneShot(remaining, token, current, completed)) break;
    ActRaiser_YieldToHost();
  }
  s_state = kReady;
  while (!s_return) ActRaiser_YieldToHost();

  s_state = kReturning;
  memcpy(g_ram, s_world, sizeof(s_world));
  Music(2); /* Sky Palace, via the same native upload path. */
  g_cpu.PB = 0;
  g_cpu.DB = 0;
  cpu_write8(&g_cpu, 0, 0x2100, 0);
  /* Original $00:8241 incoming fade tail, with its $00:805C JSR frame.
   * AR1 returns to the palace with this fade, not a reversed Advent spin. */
  Native(bank_00_82D1_M1X0, 0, 0x805e, false);
  cpu_write8(&g_cpu, 0, 0x2100, display);
  memcpy(g_ram, s_world, sizeof(s_world));
  g_cpu = s_cpu;
  s_state = kComplete;
  /* Let the host release imported resources before resuming the exact world
   * continuation. The runtime suppresses NMI on this restored checkpoint. */
  ActRaiser_YieldToHost();
  s_state = kIdle;
}
