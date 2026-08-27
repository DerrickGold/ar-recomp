/* Own the complete developed world tilemap without observing the game's shared
 * $7E:C000 scratch.
 *
 * $02:B475 has three separable phases: load the pristine 16 KiB base, call
 * $02:865C to stamp development, then upload through $2118. Production now
 * performs the middle phase with the pure SimWorldMap_ComposeDeveloped HLE.
 * It reads explicit simulation inputs and immutable ROM tables, and touches no
 * CPU, WRAM, PPU, stack, direct page, or math-unit state.
 *
 * The old transactional call remains only as the opt-in
 * AR_WORLDMAP_HLE_COMPARE diagnostic oracle. The traced closure
 * {$02:865C,$02:86D1,$02:8726} is bounded and yield-free, so it is safe for
 * differential diagnosis, but normal loading no longer depends on a synthetic
 * CPU call or its fake $19/$AA preconditions. */

#include "sim_world_map_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actraiser_game.h"
#include "snesrecomp/game/runtime.h"
#include "snesrecomp/game/cpu.h"
#include "funcs.h"
#include "sim_world_map.h"
#include "sim_world_map_compose.h"

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
  /* `$02:AF86` drains this descriptor during world navigation but leaves its
   * source address intact. It is one of $B000/$B040/$B080/$B0C0. */
  kWorldWaterAnimationSource = 0x00D7,
  /* kWorldWaterSourceFirst / kWorldWaterSourceStride shared via sim_world_map.h. */
  kWorldWaterFrameMask = 3,
  /* $02:B499 JSL $02:865C leaves bank $02 + $B49C on the emulated stack. */
  kOverlayReturnBank = 0x02,
  kOverlayReturnAddress = 0xB49C,
};

typedef enum SimWorldMapBuildConsumer {
  kBuildConsumer_None,
  kBuildConsumer_Town,
  kBuildConsumer_WorldNavigation,
} SimWorldMapBuildConsumer;

static SimWorldMapRomTables s_rom_tables;
static bool s_rom_tables_available;
static uint8 s_cached_source[kOverlaySourceBytes];
static uint8 s_cached_tiers[kDevelopmentTierBytes];
static uint8 s_cached_world_flag;
static bool s_have_cached_inputs;
static SimWorldMapBuildConsumer s_previous_consumer;
static uint8_t s_built_tilemap[kSimWorldMapBytes];
static uint8_t s_oracle_tilemap[kSimWorldMapBytes];
static SrRunnerHandle *s_runner;

void SimWorldMapBuild_BindRunner(SrRunnerHandle *runner) {
  s_runner = runner;
}

bool SimWorldMapBuild_Init(const uint8_t *rom_data, size_t rom_size) {
  s_rom_tables_available =
      SimWorldMap_LoadRomTables(&s_rom_tables, rom_data, rom_size);
  s_have_cached_inputs = false;
  s_previous_consumer = kBuildConsumer_None;
  if (!s_rom_tables_available)
    fprintf(stderr,
            "[sim-worldmap] HLE unavailable: ROM is too short for build tables\n");
  return s_rom_tables_available;
}

/* Diagnostic-only copy of the bridge Step 1 replaces. This deliberately stays
 * out of the production path unless AR_WORLDMAP_HLE_COMPARE is enabled. */
static bool BuildTilemapOracle(const uint8_t *baseline, uint8_t *out) {
  if (!baseline || !out) return false;

  const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  SrCpuMathState math_state = {
    .struct_size = sizeof(math_state),
  };
  if (!s_runner || !api ||
      api->struct_size < SNES_RUNNER_API_CPU_MATH_STATE_SIZE ||
      !(api->capabilities & SR_RUNNER_CAP_CPU_MATH_STATE) ||
      api->query_cpu_math_state(s_runner, &math_state) != SR_RESULT_OK)
    return false;

  uint8_t *wram_snapshot = (uint8_t *)malloc(kActRaiserWramSize);
  if (!wram_snapshot) return false;
  CpuState cpu_snapshot = g_cpu;
  memcpy(wram_snapshot, g_ram, kActRaiserWramSize);

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
  memcpy(g_ram, wram_snapshot, kActRaiserWramSize);
  g_cpu = cpu_snapshot;
  if (api->restore_cpu_math_state(s_runner, &math_state) != SR_RESULT_OK)
    ok = false;
  free(wram_snapshot);
  return ok;
}

static bool OracleComparisonEnabled(void) {
  static bool initialized;
  static bool enabled;
  if (!initialized) {
    const char *value = getenv("AR_WORLDMAP_HLE_COMPARE");
    enabled = value && value[0] && strcmp(value, "0") != 0;
    initialized = true;
  }
  return enabled;
}

static bool BuildTilemapHle(const uint8_t *baseline, uint8_t *out) {
  if (!s_rom_tables_available) return false;

  uint16_t enabled[kSimWorldMapTownCount];
  for (int town = 0; town < kSimWorldMapTownCount; town++) {
    const uint8_t *word = g_ram + kDevelopmentTiers + town * 2;
    enabled[town] = (uint16_t)(word[0] | ((uint16_t)word[1] << 8));
  }
  const uint8_t (*town_maps)[kSimWorldMapTownCells] =
      (const uint8_t (*)[kSimWorldMapTownCells])
          (g_ram + kOverlaySource);
  if (!SimWorldMap_ComposeDeveloped(
          out, baseline, town_maps, enabled, g_ram[kWorldStateFlags],
          &s_rom_tables))
    return false;

  if (!OracleComparisonEnabled()) return true;
  if (!BuildTilemapOracle(baseline, s_oracle_tilemap)) {
    fprintf(stderr, "[sim-worldmap] HLE oracle call failed\n");
    return false;
  }
  if (memcmp(out, s_oracle_tilemap, kSimWorldMapBytes) == 0) {
    fprintf(stderr, "[sim-worldmap] HLE/oracle parity: 16384/16384 bytes\n");
    return true;
  }

  int mismatches = 0;
  size_t first = 0;
  for (size_t i = 0; i < kSimWorldMapBytes; i++) {
    if (out[i] == s_oracle_tilemap[i]) continue;
    if (!mismatches) first = i;
    mismatches++;
  }
  fprintf(stderr,
          "[sim-worldmap] HLE/oracle MISMATCH: %d bytes; first +$%04zX "
          "HLE=$%02X oracle=$%02X\n",
          mismatches, first, out[first], s_oracle_tilemap[first]);
  return false;
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
  SimWorldMapBuildConsumer consumer = kBuildConsumer_None;
  if (ActRaiser_IsSimulationTown(map_group, map_number))
    consumer = kBuildConsumer_Town;
  else if (map_group == kActRaiserMapGroup_NonAction &&
           map_number == kActRaiserNonActionMap_WorldMap)
    consumer = kBuildConsumer_WorldNavigation;

  if (consumer == kBuildConsumer_None) {
    s_previous_consumer = kBuildConsumer_None;
    return;
  }

  /* The tilemap is owned by the HLE, but the authentic navigation clock still
   * selects the water phase. Consume only that two-byte phase result; the art
   * itself is copied from immutable ROM by SimWorldMap.
   *
   * Town mode reuses the same SimWorldMap as its outer underlay, but $D7 then
   * belongs to the town's unrelated WRAM-backed tile animation. The captured
   * navigation sequence pins the world-water cadence to the global game
   * clock: B0C0 at gf381-384, B000 at 385-392, B040 at 393-400, B080 at
   * 401-408, then B0C0. Continue that exact four-frame/eight-tick cycle in
   * towns so the underlay does not freeze when navigation is absent. */
  if (consumer == kBuildConsumer_WorldNavigation) {
    const uint16_t source =
        (uint16_t)(g_ram[kWorldWaterAnimationSource] |
                   ((uint16_t)g_ram[kWorldWaterAnimationSource + 1] << 8));
    SimWorldMap_SetWaterAnimationSource(source);
  } else {
    const uint16_t game_frame =
        ActRaiser_ReadWram16(kActRaiserWram_GameFrame);
    const unsigned phase =
        (((uint16_t)(game_frame - 1)) >> 3) & kWorldWaterFrameMask;
    SimWorldMap_SetWaterAnimationSource(
        (uint16_t)(kWorldWaterSourceFirst +
                   phase * kWorldWaterSourceStride));
  }

  if (s_previous_consumer == consumer && !InputsChanged()) return;

  const bool entry = s_previous_consumer != consumer;
  const uint8_t *baseline = SimWorldMap_Baseline();
  if (!baseline || !BuildTilemapHle(baseline, s_built_tilemap)) {
    /* Do not cache a failed attempt. The next supported frame retries. */
    s_previous_consumer = kBuildConsumer_None;
    fprintf(stderr, "[sim-worldmap] HLE tilemap build failed; retrying\n");
    return;
  }

  const int changed = SimWorldMap_PublishBuiltTilemap(s_built_tilemap);
  CacheInputs();
  s_previous_consumer = consumer;
  const char *reason = "development change";
  if (entry)
    reason = consumer == kBuildConsumer_WorldNavigation
        ? "world-navigation entry"
        : "town entry";
  fprintf(stderr,
          "[sim-worldmap] HLE built from sim state at gf=%u after %s "
          "(%d tile%s changed)\n",
          ActRaiser_ReadWram16(kActRaiserWram_GameFrame),
          reason,
          changed, changed == 1 ? "" : "s");
}
