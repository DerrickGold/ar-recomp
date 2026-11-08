#include "actraiser/actraiser_death_heim_arena.h"
#include "actraiser/actraiser_death_heim_runtime.h"
#include "actraiser/actraiser_native_call.h"
#include "actraiser_game.h"
#include "byte_order.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>

static uint8_t memory[65536], placements[65536], profiles[65536];
static const ActRaiserDeathHeimEncounter *encounter;
static unsigned target, origin, admitted, native_calls, timer_calls, timer_profile;
static bool japanese_timer;
static RecompReturn native_result;
static unsigned retry_calls, arrival_calls;
static unsigned capture_calls;
static uint8_t animation_atlas[4096], animation_cache[4096];
static ActRaiserDeathHeimRushPhase phase;

const ActRaiserDeathHeimEncounter *ActRaiserDeathHeim_CurrentEncounter(void) {
  return encounter;
}
bool ActRaiserDeathHeim_Active(void) { return encounter != NULL; }
ActRaiserDeathHeimRushPhase ActRaiserDeathHeim_Phase(void) { return phase; }
bool ActRaiserDeathHeim_RetryEncounter(CpuState *cpu) {
  (void)cpu;
  ++retry_calls;
  return true;
}
bool ActRaiserDeathHeim_AdmitEncounter(CpuState *cpu, uint16_t owner) {
  (void)cpu;
  assert(encounter && owner == 0x12e0);
  assert(ByteOrder_ReadLe16(memory + owner + 0x32) == encounter->source);
  ++admitted;
  return true;
}
bool ActRaiserDeathHeim_BeginArrival(CpuState *cpu, uint16_t owner) {
  assert(admitted == 1 && owner == 0x12e0 && cpu->X == owner);
  assert(ByteOrder_ReadLe16(memory + owner + 0x32) == encounter->source);
  ++arrival_calls;
  cpu_write16(cpu, 0, owner + 0x12, 0xf7b8);
  return true;
}
bool ActRaiserRegional_BeginActionRoom(uint8_t profile, uint16_t native_bcd, uint16_t *out) {
  ++timer_calls;
  timer_profile = profile;
  *out = profile == 0x24 && japanese_timer ? 0x0200 : native_bcd;
  return true;
}
int cpu_hle_tailcall_request(uint32_t pc, uint32_t from) {
  target = pc;
  origin = from;
  return 1;
}
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 at) {
  (void)cpu;
  if (at < 0x2000 || bank == 0x7e) return memory[at];
  if (bank == 0x0a) return placements[at];
  if (bank == 2) return profiles[at];
  assert(!bank);
  return memory[at];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 at) {
  return cpu_read8(cpu, bank, at) | (uint16)cpu_read8(cpu, bank, at + 1) << 8;
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 at, uint8 value) {
  (void)cpu;
  assert(!bank || bank == 0x7e || at < 0x2000);
  memory[at] = value;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 at, uint16 value) {
  cpu_write8(cpu, bank, at, value);
  cpu_write8(cpu, bank, at + 1, value >> 8);
}
RecompReturn bank_00_9557_M0X0(CpuState *cpu) {
  const ActRaiserDeathHeimArena *arena = ActRaiserDeathHeimArenas_Find(encounter->city);
  if (cpu->X == 0x0ae0) {
    assert(encounter->city == 2 && cpu->Y == 0xb4cb && cpu->A == 7);
    assert(cpu_read16(cpu, 0, cpu->X + 0x34) == 0);
    assert(cpu_read16(cpu, 0, cpu->X + 0x36) == 0);
    assert(cpu_read16(cpu, 0, cpu->X + 0x38) == 0xff);
    if (native_result == RECOMP_RETURN_NORMAL) {
      cpu_write16(cpu, 0, cpu->X, 0x0800);
      cpu_write16(cpu, 0, cpu->X + 0x12, 0xb919);
      cpu_write16(cpu, 0, cpu->X + 0x24, 0);
      cpu_write16(cpu, 0, cpu->X + 0x26, 0);
      cpu_write16(cpu, 0, cpu->X + 0x30, 0);
    }
    return native_result;
  }
  assert(cpu->X == 0x12e0 && cpu->Y == arena->placement && cpu->A == arena->type);
  assert(cpu_read16(cpu, 0, cpu->X + 0x34) == arena->boss_x * 16);
  assert(cpu_read16(cpu, 0, cpu->X + 0x36) == arena->boss_y * 16);
  assert(cpu_read16(cpu, 0, cpu->X + 0x38) == arena->parameter);
  if (native_result == RECOMP_RETURN_NORMAL) {
    cpu_write16(cpu, 0, cpu->X + 0x32, arena->source);
    cpu_write16(cpu, 0, cpu->X + 0x30, 0x4000);
    cpu_write16(cpu, 0, cpu->X + 0x16, 0x5000);
    memory[cpu->X + 0x18] = 0x7e;
  }
  return native_result;
}
RecompReturn bank_02_BAF5_M1X0(CpuState *cpu) {
  assert(cpu->PB == 2 && !cpu->DB && !cpu->D && cpu->m_flag && !cpu->x_flag);
  ++capture_calls;
  if (native_result != RECOMP_RETURN_NORMAL) return native_result;
  /* Native leaf double: continuation retains the previous room's full
   * capture, while a disabled animation profile leaves it untouched. */
  if (cpu_read16(cpu, 0, 0xe1)) {
    if (memory[0xde] & 0x80) memory[0xde] &= 0x7f;
    else memcpy(animation_cache, animation_atlas, sizeof(animation_cache));
  }
  cpu->A = 0x5a7e;
  cpu->X = cpu_read16(cpu, 0, 0xda);
  cpu->Y = 0x1000;
  return RECOMP_RETURN_NORMAL;
}
RecompReturn ActRaiserNativeCall(CpuState *cpu, ActRaiserNativeLeaf leaf,
                                uint8_t bank, uint16_t caller, bool long_call) {
  if (leaf == bank_02_BAF5_M1X0) {
    assert(bank == 2 && caller == 0xb24d && long_call);
    assert(ActRaiserDeathHeimArenas_Loading(cpu));
    assert(memory[0x19] < ActRaiserDeathHeimArenas_Find(encounter->city)->map);
    return leaf(cpu);
  }
  assert(leaf == bank_00_9557_M0X0 && !bank && caller == 0x9461 && !long_call);
  ++native_calls;
  return leaf(cpu);
}
static void Put(unsigned at, uint16_t value) { ByteOrder_WriteLe16(memory + at, value); }
static CpuState Setup(unsigned city) {
  memset(memory, 0, sizeof(memory));
  memset(placements, 0, sizeof(placements));
  memset(profiles, 0, sizeof(profiles));
  ActRaiserDeathHeimArenas_Reset();
  phase = kActRaiserDeathHeimRush_Combat;
  encounter = ActRaiserDeathHeimRush_Encounter((city - 1) * 2);
  const ActRaiserDeathHeimArena *arena = ActRaiserDeathHeimArenas_Find(city);
  memory[0x18] = arena->city;
  memory[0x19] = arena->map;
  memory[0x1d] = 7;
  memory[0x1e] = 24;
  memory[0x21] = 3;
  memory[0x1c] = 2;
  memory[0xe4] = 128;
  const uint8_t bytes[] = {arena->boss_x, arena->boss_y, arena->parameter, arena->type};
  memcpy(placements + arena->placement, bytes, sizeof(bytes));
  const uint8_t environment[] = {0, 0, 0xff, 7};
  memcpy(placements + 0xb4cb, environment, sizeof(environment));
  static const uint8_t ids[] = {6, 15, 21, 28, 36, 44};
  for (unsigned i = 0; i < 6; ++i)
    ByteOrder_WriteLe16(profiles + 0x893e + 28 * ids[i] + 25, i == 4 ? 0x300 : 0x200);
  target = origin = admitted = native_calls = timer_calls = timer_profile = retry_calls = 0;
  arrival_calls = 0;
  capture_calls = 0;
  memset(animation_atlas, 0, sizeof(animation_atlas));
  memset(animation_cache, 0xcc, sizeof(animation_cache));
  native_result = RECOMP_RETURN_NORMAL;
  japanese_timer = false;
  CpuState cpu = {.PB = 2, .DB = 0, .S = 0x1e0, .A = 0x1234, .X = 0x789,
                  .Y = 0xa100, .P = CPU_P_M | CPU_P_C | CPU_P_V};
  cpu_p_to_mirrors(&cpu);
  return cpu;
}
static void CheckAssets(void) {
  static const unsigned expected_profiles[] = {6, 15, 21, 28, 36, 44};
  for (unsigned city = 1; city <= 6; ++city) {
    for (unsigned retry = 0; retry < 2; ++retry) {
      CpuState cpu = Setup(city), before = cpu;
      const ActRaiserDeathHeimArena *arena = ActRaiserDeathHeimArenas_Find(city);
      Put(0x032c, retry);
      assert(!ActRaiserDeathHeimArenas_Loading(&cpu));
      assert(ActRaiser_DeathHeimArenaAssetsBeginEntry(&cpu));
      assert(ActRaiser_DeathHeimArenaAssetsBegin(&cpu) == RECOMP_RETURN_TAILCALL);
      assert(target == 0x02b1fc && origin == 0x02b1fc && memory[0x19] == 1);
      for (unsigned map = 1; map <= arena->map; ++map) {
        assert(ActRaiserDeathHeimArenas_Loading(&cpu));
        assert(!ActRaiserDeathHeimArenas_Loading(NULL));
        assert(!ActRaiser_DeathHeimArenaAssetsBeginEntry(&cpu));
        assert(ActRaiser_DeathHeimArenaAssetsEndEntry(&cpu));
        assert(ActRaiser_DeathHeimArenaAssetsEnd(&cpu) == RECOMP_RETURN_TAILCALL);
        assert(origin == 0x02b24e);
        assert(target == (map < arena->map ? 0x02b1fc : 0x02b24e));
        assert(memory[0x19] == (map < arena->map ? map + 1 : map));
        CpuState control = cpu;
        control.A = before.A;
        control.X = before.X;
        control.Y = before.Y;
        assert(!memcmp(&before, &control, sizeof(control))); /* Stack and flags survive. */
        assert(capture_calls == (map < arena->map ? map : map - 1));
      }
      assert(!ActRaiserDeathHeimArenas_Loading(&cpu));
      assert(!ActRaiser_DeathHeimArenaAssetsEndEntry(&cpu));
      assert(timer_calls == 1 && timer_profile == expected_profiles[city - 1]);
      assert(cpu_read16(&cpu, 0, 0xe6) == (city == 5 ? 0x300 : 0x200));
      assert(memory[0xe5] == 0x3b && !memory[0xe8]);
      assert(cpu_read16(&cpu, 0, 0x032c) == retry);
      assert(memory[0x1d] == 7 && memory[0x1c] == 2 && memory[0x21] == 3 && memory[0xe4] == 128);
    }
  }
  CpuState cpu = Setup(5);
  japanese_timer = true;
  ActRaiser_DeathHeimArenaAssetsBegin(&cpu);
  for (unsigned i = 0; i < 3; ++i) ActRaiser_DeathHeimArenaAssetsEnd(&cpu);
  assert(cpu_read16(&cpu, 0, 0xe6) == 0x200);
}
static void CheckInheritedAnimation(void) {
  /* Kasandora captures room1, Marahna captures room1 then continues, and
   * Northwall starts disabled before capturing room2 then continuing. */
  static const unsigned cities[] = {3, 5, 6};
  for (unsigned i = 0; i < sizeof(cities) / sizeof(cities[0]); ++i) {
    CpuState cpu = Setup(cities[i]);
    const unsigned last = ActRaiserDeathHeimArenas_Find(cities[i])->map;
    const unsigned first_capture = cities[i] == 6 ? 2 : 1;
    ActRaiser_DeathHeimArenaAssetsBegin(&cpu);
    for (unsigned map = 1; map <= last; ++map) {
      memset(animation_atlas, 0x30 + map, sizeof(animation_atlas));
      Put(0xe1, map < first_capture ? 0 : 0x0200);
      memory[0xde] = map > first_capture ? 0x88 : 0x08;
      ActRaiser_DeathHeimArenaAssetsEnd(&cpu);
      const unsigned expected = map < first_capture ? 0xcc : 0x30 + first_capture;
      for (unsigned byte = 0; byte < sizeof(animation_cache); ++byte)
        assert(animation_cache[byte] == expected);
      /* The final profile is deliberately left for the ordinary loader's
       * BAF5 call, including its native continuation-bit consumption. */
      if (map == last) {
        assert(memory[0xde] == 0x88);
        bank_02_BAF5_M1X0(&cpu);
        assert(memory[0xde] == 0x08);
        for (unsigned byte = 0; byte < sizeof(animation_cache); ++byte)
          assert(animation_cache[byte] == expected);
      }
    }
    assert(capture_calls == last);
  }
  CpuState cpu = Setup(3);
  ActRaiser_DeathHeimArenaAssetsBegin(&cpu);
  target = 0;
  native_result = RECOMP_RETURN_OWNED_UNWIND;
  assert(ActRaiser_DeathHeimArenaAssetsEnd(&cpu) == native_result);
  assert(capture_calls == 1 && memory[0x19] == 1 && !target && !timer_calls);
  assert(ActRaiserDeathHeimArenas_Loading(&cpu));
  ActRaiserDeathHeimArenas_Reset();
}
static void CheckPlayersAndPlacements(void) {
  for (unsigned city = 1; city <= 6; ++city) {
    CpuState cpu = Setup(city);
    const ActRaiserDeathHeimArena *arena = ActRaiserDeathHeimArenas_Find(city);
    cpu.PB = 0;
    cpu.DB = 0x0a;
    cpu.P &= ~CPU_P_M;
    cpu_p_to_mirrors(&cpu);
    cpu.X = 0x08a0;
    Put(0x032c, 1);
    Put(0x032e, 1234);
    Put(0x0330, 5678);
    const CpuState before = cpu;
    assert(ActRaiser_DeathHeimArenaPlayerEntry(&cpu));
    assert(ActRaiser_DeathHeimArenaPlayer(&cpu) == RECOMP_RETURN_TAILCALL);
    assert(target == 0x939c && origin == 0x9354);
    assert(!memcmp(&cpu, &before, sizeof(cpu)));
    assert(cpu_read16(&cpu, 0, 0x08d4) == arena->player_x);
    assert(cpu_read16(&cpu, 0, 0x08d6) == arena->player_ground_y);
    assert(!cpu_read16(&cpu, 0, 0x032e) && !cpu_read16(&cpu, 0, 0x0330));
    assert(cpu_read16(&cpu, 0, 0x032c) == 1);
    cpu.X = 0x0ae0;
    assert(ActRaiser_DeathHeimArenaPlacementsEntry(&cpu));
    assert(ActRaiser_DeathHeimArenaPlacements(&cpu) == RECOMP_RETURN_TAILCALL);
    assert(target == 0x9311 && origin == 0x930e && cpu.X == 0x1320);
    for (unsigned slot = 0x0ae0; slot < 0x12e0; slot += 0x40)
      assert(cpu_read16(&cpu, 0, slot) == (city == 2 && slot == 0x0ae0 ? 0x0800 : 0x4000));
    if (city == 2) {
      assert(cpu_read16(&cpu, 0, 0x0af2) == 0xb919);
      assert(!cpu_read16(&cpu, 0, 0x0b10)); /* Persistent hidden controller, not a boss. */
      assert(!cpu_read16(&cpu, 0, 0x0b12)); /* Handler-only spawn has no boss source. */
    }
    assert(cpu_read16(&cpu, 0, 0x1320) == 0x8000);
    assert(cpu_read16(&cpu, 0, 0x12f2) == 0xf7b8);
    assert(admitted == 1 && native_calls == (city == 2 ? 2 : 1) && arrival_calls == 1);
    assert(memory[0x1d] == 7 && memory[0x1c] == 2 && memory[0x21] == 3 && memory[0xe4] == 128);
  }
  /* Propagate an unwind from either the environment or boss allocator before
   * admission, beam setup or publication of the completed placement list. */
  for (unsigned city = 2; city <= 4; city += 2) {
    CpuState cpu = Setup(city);
    cpu.PB = 0;
    cpu.DB = 0x0a;
    cpu.P &= ~CPU_P_M;
    cpu_p_to_mirrors(&cpu);
    cpu.X = 0x0ae0;
    native_result = RECOMP_RETURN_OWNED_UNWIND;
    assert(ActRaiser_DeathHeimArenaPlacements(&cpu) == native_result);
    assert(native_calls == 1 && !admitted && !arrival_calls && !target);
    assert(!cpu_read16(&cpu, 0, 0x1320));
  }
}
static void CheckGates(void) {
  for (unsigned bad = 0; bad < 10; ++bad) {
    CpuState cpu = Setup(3);
    switch (bad) {
      case 0: cpu.PB = 0; break;
      case 1: cpu.DB = 1; break;
      case 2: cpu.D = 1; break;
      case 3: cpu.emulation = 1; break;
      case 4: cpu.m_flag = 0; break;
      case 5: cpu.x_flag = 1; break;
      case 6: cpu._flag_D = 1; break;
      case 7: memory[0x18] = 7; break;
      case 8: encounter = NULL; break;
      case 9: encounter = ActRaiserDeathHeimRush_Encounter(5); break;
    }
    uint8_t before[sizeof(memory)];
    memcpy(before, memory, sizeof(memory));
    assert(!ActRaiser_DeathHeimArenaAssetsBeginEntry(&cpu));
    assert(!ActRaiser_DeathHeimArenaAssetsEndEntry(&cpu));
    assert(!memcmp(before, memory, sizeof(memory)));
  }
  CpuState cpu = Setup(3);
  assert(!ActRaiserDeathHeimArenas_Find(0) && !ActRaiserDeathHeimArenas_Find(7));
  assert(!ActRaiserDeathHeimArenas_Begin(NULL, 1));
  assert(!ActRaiserDeathHeimArenas_Begin(&cpu, 7));
  Put(0x032c, 1);
  Put(0x032e, 2);
  Put(0x0330, 3);
  assert(ActRaiserDeathHeimArenas_Begin(&cpu, 3));
  assert(memory[0x1a] == 2 && memory[0x1b] == 3);
  assert(!cpu_read16(&cpu, 0, 0x032c) && !cpu_read16(&cpu, 0, 0x032e) &&
         !cpu_read16(&cpu, 0, 0x0330));
  ActRaiser_DeathHeimArenaAssetsBegin(&cpu);
  assert(!ActRaiserDeathHeimArenas_Begin(&cpu, 4));
  ActRaiserDeathHeimArenas_Reset();
  assert(!ActRaiser_DeathHeimArenaAssetsEndEntry(&cpu));
}
static void CheckLoadingOwnership(void) {
  for (unsigned bad = 0; bad < 7; ++bad) {
    CpuState cpu = Setup(6);
    ActRaiser_DeathHeimArenaAssetsBegin(&cpu);
    assert(ActRaiserDeathHeimArenas_Loading(&cpu));
    assert(ActRaiser_DeathHeimArenaAssetsEndEntry(&cpu));
    switch (bad) {
      case 0: encounter = NULL; break;
      case 1: encounter = ActRaiserDeathHeimRush_Encounter(11); break;
      case 2: encounter = ActRaiserDeathHeimRush_Encounter(8); break;
      case 3: memory[0x18] = 5; break;
      case 4: memory[0x19] = 0; break;
      case 5: memory[0x19] = 5; break;
      case 6: ActRaiserDeathHeimArenas_Reset(); break;
    }
    assert(!ActRaiserDeathHeimArenas_Loading(&cpu));
    assert(!ActRaiser_DeathHeimArenaAssetsEndEntry(&cpu));
  }
}
static void CheckRetries(void) {
  for (unsigned id = 0; id <= kActRaiserDeathHeimRush_FinalEncounter; ++id) {
    CpuState cpu = Setup(id < 12 ? id / 2 + 1 : 1);
    encounter = ActRaiserDeathHeimRush_Encounter(id);
    memory[0x18] = encounter->map_group;
    memory[0x19] = encounter->map;
    cpu.X = 0x8fc0;
    profiles[cpu.X] = 1; /* Native city checkpoint-table result. */
    Put(0x8a, 0x8a0);
    Put(0x8d0, 0x0040);
    const uint16_t saved_stack = cpu.S;
    assert(ActRaiser_DeathHeimArenaRetryEntry(&cpu));
    assert(ActRaiser_DeathHeimArenaRetry(&cpu) == RECOMP_RETURN_TAILCALL);
    assert(target == 0x02bd65 && origin == 0x02bd61 && retry_calls == 1);
    assert(cpu.A == (0x1200 | (encounter->act == 1 ? encounter->map : 1)));
    assert(cpu.S == saved_stack && memory[0x1c] == 2 && memory[0x1d] == 7);
    assert(!memory[0x1a] && !memory[0x1b]); /* Native continuation owns stores. */
    memory[0x1c] = 0;
    assert(!ActRaiser_DeathHeimArenaRetryEntry(&cpu));
    memory[0x1c] = 2;
    Put(0x8d0, 0);
    assert(!ActRaiser_DeathHeimArenaRetryEntry(&cpu));
    Put(0x8d0, 0x0400);
    assert(ActRaiser_DeathHeimArenaRetryEntry(&cpu));
    phase = kActRaiserDeathHeimRush_Retry;
    assert(ActRaiser_DeathHeimArenaRetryEntry(&cpu));
    phase = kActRaiserDeathHeimRush_Final;
    assert(ActRaiser_DeathHeimArenaRetryEntry(&cpu));
    phase = kActRaiserDeathHeimRush_Preparing;
    assert(ActRaiser_DeathHeimArenaRetryEntry(&cpu) == (encounter->act == 1));
    phase = kActRaiserDeathHeimRush_Returning;
    assert(!ActRaiser_DeathHeimArenaRetryEntry(&cpu));
    phase = kActRaiserDeathHeimRush_Combat;
    memory[0x19]++;
    assert(!ActRaiser_DeathHeimArenaRetryEntry(&cpu));
  }
}
int main(void) {
  CheckAssets();
  CheckInheritedAnimation();
  CheckPlayersAndPlacements();
  CheckGates();
  CheckLoadingOwnership();
  CheckRetries();
  puts("Death Heim arenas: native asset chains, spawn and retry contracts passed");
  return 0;
}
