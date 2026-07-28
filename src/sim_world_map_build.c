/* sim_world_map_build — build the world-map tilemap ourselves, on demand.
 *
 * The underlay used to OBSERVE the live Mode-7 shadow at $7E:C000 and guess
 * whether it was trustworthy. That was forced by not being able to see the
 * game's write cursor, and every piece of trust machinery (the map-identity
 * gate, the rows-8+ fingerprint, the coherence policy, the staleness trade)
 * existed only to serve that guess. It is not needed if we can produce the
 * tilemap ourselves whenever we want it, which is what this file does.
 *
 * HOW THE GAME BUILDS IT (traced 2026-07-27, PC-capturing WRAM write trace over
 * $7E:C000..FFFF, saves/fillmore-sim.rec):
 *
 *   $02:B475  — reached from the bank-02 display-list interpreter ($02:B3EB
 *               opcode, script pointer in $A5). Three phases, in order:
 *                 1. fill $7E:C000..FFFF with a 16 KB base tilemap, either a
 *                    raw copy from the script's source pointer or, if $03 is
 *                    positive, an LZSS decompress via $02:C5C9;
 *                 2. JSL $02:865C — stamp the developed-region overlay;
 *                 3. upload $7E:C000..FFFF to VRAM through $2118.
 *
 * Phase 1's output is BYTE-IDENTICAL to the ROM blob at $06:B341 that
 * SimWorldMap_Init already loads as its baseline — verified by reconstructing
 * the traced writes and diffing: of the 447 bytes that differed from the ROM
 * baseline after a full build, ZERO came from phase 1 and all 447 came from
 * phase 2. So we do not need phase 1: we already have its result. Phase 3 is
 * pure presentation (Mode-7 VRAM DMA) and is exactly what we must NOT do.
 *
 * That leaves phase 2 as the only part worth owning, and it is small and safe:
 *
 *   $02:865C  — reads $19 (must be $09), $7F:9101 bit 0, and then for each of
 *               the six towns reads $7F:6B18+2n (0 = undeveloped, else the
 *               development tier) and stamps a 32x32-tile block via $02:86D1
 *               and $02:8726, sourced from $7F:2000 through the $02:80xx
 *               translation table and placed by the $02:87A5 offset table.
 *
 * Why calling it synchronously is safe, all measured rather than assumed:
 *
 *   - the static call closure is exactly {865C, 86D1, 8726}; 86D1 and 8726 are
 *     leaves. No yield helper, no vblank wait, no BRK/COP trap, and no
 *     hardware-register access anywhere in the closure — so this is NOT
 *     coroutine-based and cannot suspend mid-build;
 *   - its entire write footprint outside $7E:C000..FFFF is DP $0C/$0E/$10/$12,
 *     DP $A5..$A9, and stack $01DD..$01EA. All scratch, and all restored here
 *     anyway by the transaction below;
 *   - its inputs survive an act. $7F:2000..37FF (6 KB of stamp source) was
 *     byte-identical between a world-map-visited state and an act->town state,
 *     and $7F:6B18 correctly read 1 then 2 for Fillmore across a development
 *     step, which is the sim state we want to see.
 *
 * Preconditions it needs and does not set for itself:
 *   - $19 == $09. It returns immediately otherwise, which is why an act->town
 *     transition could never have produced a rebuild on its own. $18 is never
 *     read by this closure and does not need to impersonate world-map mode.
 *   - $AA == $7E. It sets $A8/$A9 to $C000 but never the bank byte of that
 *     `[$A8],Y` pointer, inheriting it from whoever ran before. Measured $7E in
 *     both sampled states; set explicitly here rather than trusted.
 *   - direct page zero, binary arithmetic, and 16-bit A/X at entry. The routine
 *     changes and restores its own width flags, but its first direct-page and
 *     stack operations assume the normal bank-02 calling convention.
 *
 * Both are forced inside the transaction and rolled back after, so the game
 * never observes either.
 *
 * The transaction protocol (snapshot g_cpu + all of g_ram + the emulated
 * multiply/divide unit, run the recompiled routine, harvest, restore) is the
 * same one actraiser_widescreen_bg.c uses to drive $02:B825/$02:B8A0 from the
 * host. */

#include "sim_world_map_build.h"

#include <stdio.h>
#include <string.h>

#include "actraiser_game.h"
#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "funcs.h"
#include "sim_world_map.h"
#include "snes/snes.h"

RecompReturn bank_02_865C_M0X0(CpuState *cpu);

enum {
  kShadowBase = 0xC000,      /* $7E:C000, as a g_ram offset */
  kShadowBytes = 0x4000,
  kDpPointerBank = 0x00AA,   /* bank byte of the `[$A8],Y` destination */
  kWorldMapId = 0x09,        /* $19 value $02:865C demands */
  kOverlaySource = 0x12000,  /* flat g_ram offset of $7F:2000 */
  kOverlaySourceBytes = 0x1800,
  kDevelopmentTiers = 0x16B18, /* flat g_ram offset of $7F:6B18 */
  kDevelopmentTierBytes = 12,
  kWorldStateFlags = 0x19101,  /* flat g_ram offset of $7F:9101 */
  /* $02:B499 JSL $02:865C leaves bank $02 + $B49C on the emulated stack. */
  kOverlayReturnBank = 0x02,
  kOverlayReturnAddress = 0xB49C,
};

static uint8 s_wram_snapshot[kActRaiserWramSize];
static uint8 s_cached_source[kOverlaySourceBytes];
static uint8 s_cached_tiers[kDevelopmentTierBytes];
static uint8 s_cached_world_flag;
static bool s_have_cached_inputs;
static bool s_was_in_town;
static uint8_t s_built_tilemap[kSimWorldMapBytes];

static bool BuildTilemap(const uint8_t *baseline, uint8_t *out) {
  if (!baseline || !out) return false;

  CpuState cpu_snapshot = g_cpu;
  uint8 multiply_a = g_snes->multiplyA;
  uint16 multiply_result = g_snes->multiplyResult;
  uint16 divide_a = g_snes->divideA;
  uint16 divide_result = g_snes->divideResult;
  memcpy(s_wram_snapshot, g_ram, sizeof(s_wram_snapshot));

  /* Phase 1, done for free: the base tilemap the game would have copied or
   * decompressed into the shadow is the ROM blob we already hold. */
  memcpy(g_ram + kShadowBase, baseline, kShadowBytes);

  /* Calling convention and data preconditions. All are rolled back with the
   * rest of the transaction. $18 is deliberately untouched: the overlay does
   * not inspect it, so presentation mode never changes even transiently. */
  g_ram[kActRaiserWram_CurrentMap] = kWorldMapId;
  g_ram[kDpPointerBank] = 0x7E;

  CpuState *cpu = &g_cpu;
  cpu->D = 0;
  cpu->m_flag = 0;
  cpu->x_flag = 0;
  cpu->_flag_D = 0;
  cpu_mirrors_to_p(cpu);
  cpu->host_return_valid = 0;

  /* Paired-call protocol, matching a generated JSL site: push the return frame
   * the routine's RTL will consume, then restore S regardless of how it
   * returned. The outer snapshot removes the frame either way. */
  uint16 entry_s = cpu->S;
  cpu_write8(cpu, 0x00, cpu->S, kOverlayReturnBank);
  cpu->S = (uint16)(cpu->S - 1);
  cpu_write8(cpu, 0x00, cpu->S, (uint8)(kOverlayReturnAddress >> 8));
  cpu->S = (uint16)(cpu->S - 1);
  cpu_write8(cpu, 0x00, cpu->S, (uint8)kOverlayReturnAddress);
  cpu->S = (uint16)(cpu->S - 1);
  cpu->host_return_valid = 1;
  uint8 saved_pb = cpu->PB;
  cpu->PB = 0x02;

  RecompReturn r = bank_02_865C_M0X0(cpu);

  cpu->PB = saved_pb;
  cpu->S = entry_s;

  /* Harvest before the rollback, since the rollback is what puts the game's
   * own bytes back into the shadow. */
  bool ok = (r == RECOMP_RETURN_NORMAL);
  if (ok) memcpy(out, g_ram + kShadowBase, kShadowBytes);

  /* Transaction boundary: discard every game-visible write, including the
   * seeded baseline, the two forced preconditions and all the routine's
   * scratch. The game cannot tell this ran. */
  memcpy(g_ram, s_wram_snapshot, sizeof(s_wram_snapshot));
  g_cpu = cpu_snapshot;
  g_snes->multiplyA = multiply_a;
  g_snes->multiplyResult = multiply_result;
  g_snes->divideA = divide_a;
  g_snes->divideResult = divide_result;
  return ok;
}

static bool InputsChanged(void) {
  return !s_have_cached_inputs ||
      s_cached_world_flag != (uint8)(g_ram[kWorldStateFlags] & 1) ||
      memcmp(s_cached_tiers, g_ram + kDevelopmentTiers,
             sizeof(s_cached_tiers)) != 0 ||
      memcmp(s_cached_source, g_ram + kOverlaySource,
             sizeof(s_cached_source)) != 0;
}

static void CacheInputs(void) {
  s_cached_world_flag = (uint8)(g_ram[kWorldStateFlags] & 1);
  memcpy(s_cached_tiers, g_ram + kDevelopmentTiers, sizeof(s_cached_tiers));
  memcpy(s_cached_source, g_ram + kOverlaySource, sizeof(s_cached_source));
  s_have_cached_inputs = true;
}

void SimWorldMap_BuildIfNeeded(void) {
  const uint8 map_group = g_ram[kActRaiserWram_MapGroup];
  const uint8 map_number = g_ram[kActRaiserWram_CurrentMap];
  const bool in_town = ActRaiser_IsSimulationTown(map_group, map_number);
  if (!in_town) {
    s_was_in_town = false;
    return;
  }
  if (s_was_in_town && !InputsChanged()) return;

  const bool town_entry = !s_was_in_town;
  const uint8_t *baseline = SimWorldMap_Baseline();
  if (!baseline || !BuildTilemap(baseline, s_built_tilemap)) {
    /* Do not cache a failed attempt. The next rendered town frame retries. */
    s_was_in_town = false;
    fprintf(stderr, "[sim-worldmap] owned tilemap build failed; retrying\n");
    return;
  }

  const int changed = SimWorldMap_PublishBuiltTilemap(s_built_tilemap);
  CacheInputs();
  s_was_in_town = true;
  fprintf(stderr,
          "[sim-worldmap] built from sim state at gf=%u after %s "
          "(%d tile%s changed)\n",
          ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
          town_entry ? "town entry" : "development change",
          changed, changed == 1 ? "" : "s");
}
