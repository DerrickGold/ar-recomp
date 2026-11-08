#include "actraiser/actraiser_scene_music.h"
#include "actraiser/actraiser_death_heim_runtime.h"
#include "actraiser_game.h"
#include "regional/presentation/regional_music.h"
#include "byte_order.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t ram[65536], profile;
static unsigned target, origin, captures;
static bool rush_active;
static bool arena_loading;
static const ActRaiserDeathHeimEncounter *rush_encounter;
bool ActRaiserDeathHeim_Active(void) { return rush_active; }
bool ActRaiserDeathHeimArenas_Loading(CpuState *cpu) {
  const unsigned map = cpu_read8(cpu, 0, kActRaiserWram_CurrentMap);
  return arena_loading && rush_encounter && map && map <= rush_encounter->map;
}
const ActRaiserDeathHeimEncounter *ActRaiserDeathHeim_CurrentEncounter(void) {
  return rush_encounter;
}
bool ActRaiserRegional_BeginSceneMusic(uint8_t *out) {
  *out = profile;
  ++captures;
  return true;
}
int cpu_hle_tailcall_request(uint32_t pc, uint32_t site) {
  target = pc;
  origin = site;
  return 1;
}
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 at) {
  (void)cpu;
  assert(!bank);
  return ram[at];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 at) {
  return cpu_read8(cpu, bank, at) | (uint16)cpu_read8(cpu, bank, at + 1) << 8;
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 at, uint8 value) {
  (void)cpu;
  assert(!bank);
  ram[at] = value;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 at, uint16 value) {
  cpu_write8(cpu, bank, at, value);
  cpu_write8(cpu, bank, at + 1, value >> 8);
}
static void Adapters(void) {
  for (unsigned source = 0; source < 3; ++source) {
    assert(ArRegionalMusic_Resolve(source, &profile));
    assert(profile == (source == 1));
    for (unsigned flags = 0; flags < 256; ++flags) {
      if (!(flags & CPU_P_M) || (flags & (CPU_P_X | CPU_P_D))) continue;
      for (unsigned scene = 0; scene < 0x0909; scene += 1) {
        if ((scene & 255) > 8) continue;
        for (unsigned resource = 0; resource < 5; ++resource) {
          memset(ram, 0x5a, sizeof(ram));
          ByteOrder_WriteLe16(ram + 0x18, scene);
          const uint16_t pointers[] = {0xf69f, 0x947f, 0xabcd, 0, 0xf69f};
          ByteOrder_WriteLe16(ram + 0xa5, pointers[resource]);
          ram[0xa7] = resource == 1 ? 0x18 : 0x0e;
          ram[0x334] = ram[0x8d] = 0;
          ram[0x8c] = resource == 4 ? 2 : 1;
          CpuState cpu = {
              .PB = 2, .DB = 0, .A = 0xbe00, .X = 0xa5, .Y = 0x1234, .S = 0x1efc, .P = flags};
          cpu_p_to_mirrors(&cpu);
          CpuState before = cpu;
          uint8_t expected[65536];
          memcpy(expected, ram, sizeof(ram));
          const unsigned count = captures;
          assert(ActRaiser_SceneMusicEntry(&cpu) && captures == count);
          assert(!memcmp(&cpu, &before, sizeof(cpu)) && !memcmp(expected, ram, sizeof(ram)));
          const bool change = profile == 1 && (scene == 0x0201 || scene == 0x0301) && resource == 0;
          if (change) {
            ByteOrder_WriteLe16(expected + 0xa5, 0x947f);
            expected[0xa7] = 0x18;
          }
          before.X = ByteOrder_ReadLe16(expected + 0xa5);
          before.P = (before.P & ~(CPU_P_N | CPU_P_Z)) | (before.X ? 0 : CPU_P_Z) |
                     (before.X & 0x8000 ? CPU_P_N : 0);
          cpu_p_to_mirrors(&before);
          cpu.P ^= CPU_P_C | CPU_P_V; /* LDX must preserve live arithmetic flags. */
          assert(ActRaiser_SceneMusic(&cpu) == RECOMP_RETURN_TAILCALL);
          assert(captures == count + 1 && target == 0x02b655 && origin == 0x02b653);
          assert(!memcmp(&cpu, &before, sizeof(cpu)) && !memcmp(expected, ram, sizeof(ram)));
          /* No cache, selector, APU-port or handshake writes. A repeated
           * accepted source goes through the same native cache comparison. */
          cpu.P ^= CPU_P_C | CPU_P_V; /* LDX must preserve live arithmetic flags. */
          assert(ActRaiser_SceneMusic(&cpu) == RECOMP_RETURN_TAILCALL);
          assert(!memcmp(expected, ram, sizeof(ram)));
        }
      }
    }
  }
  for (unsigned bad = 0; bad < 9; ++bad) {
    CpuState cpu = {.PB = 2, .DB = 0, .P = CPU_P_M};
    cpu_p_to_mirrors(&cpu);
    ram[0x334] = ram[0x8d] = 0;
    switch (bad) {
    case 0:
      cpu.emulation = 1;
      break;
    case 1:
      cpu.m_flag = 0;
      break;
    case 2:
      cpu.x_flag = 1;
      break;
    case 3:
      cpu.D = 2;
      break;
    case 4:
      cpu.PB = 0;
      break;
    case 5:
      cpu.DB = 2;
      break;
    case 6:
      cpu._flag_D = 1;
      break;
    case 7:
      cpu.P |= CPU_P_D;
      break;
    case 8:
      ram[0x8d] = 1;
      break;
    }
    assert(!ActRaiser_SceneMusicEntry(&cpu));
  }
  assert(!ActRaiser_SceneMusicEntry(NULL));
  uint8_t unchanged = 0xff;
  assert(!ArRegionalMusic_Resolve(-1, &unchanged) && unchanged == 0xff);
  assert(!ArRegionalMusic_Resolve(3, &unchanged) && !ArRegionalMusic_Resolve(0, NULL));
  assert(!ArRegionalMusic_UseFillmore(2, 0x0201));
}

static void BorrowedArenaMusic(void) {
  profile = 0;
  for (unsigned city = 1; city <= 6; ++city) {
    rush_encounter = ActRaiserDeathHeimRush_Encounter((city - 1) * 2);
    assert(rush_encounter && rush_encounter->act == 1);
    rush_active = true;
    for (unsigned map = 1; map <= rush_encounter->map; ++map) {
      arena_loading = true;
      memset(ram, 0x5a, sizeof(ram));
      ram[kActRaiserWram_MapGroup] = city;
      ram[kActRaiserWram_CurrentMap] = map;
      ram[0x334] = ram[0x8d] = 0;
      CpuState cpu = {.PB = 2, .DB = 0, .A = 0xbe00, .X = 0xa5,
                     .Y = 0x1234, .S = 0x1efc, .P = CPU_P_M | CPU_P_C};
      cpu_p_to_mirrors(&cpu);
      const CpuState before = cpu;
      uint8_t expected[sizeof(ram)];
      memcpy(expected, ram, sizeof(ram));
      assert(ActRaiser_SceneMusicEntry(&cpu));
      assert(ActRaiser_SceneMusic(&cpu) == RECOMP_RETURN_TAILCALL);
      assert(target == 0x02b69b && origin == 0x02b653);
      assert(!memcmp(&cpu, &before, sizeof(cpu)) && !memcmp(expected, ram, sizeof(ram)));
      /* Both the ordinary stage and its retry are accepted without touching
       * the resident image, SFX mailboxes or any APU command port. */
      assert(ActRaiser_SceneMusic(&cpu) == RECOMP_RETURN_TAILCALL);
      assert(!memcmp(expected, ram, sizeof(ram)));
      arena_loading = false;
      assert(ActRaiser_SceneMusic(&cpu) == RECOMP_RETURN_TAILCALL);
      assert(target == (map == rush_encounter->map ? 0x02b69b : 0x02b655));
    }
    /* The same physical level outside a rush keeps its native song path. */
    CpuState cpu = {.PB = 2, .P = CPU_P_M};
    cpu_p_to_mirrors(&cpu);
    rush_active = false;
    assert(ActRaiser_SceneMusic(&cpu) == RECOMP_RETURN_TAILCALL && target == 0x02b655);
    rush_active = true;
    ram[kActRaiserWram_CurrentMap] = rush_encounter->map + 1;
    assert(ActRaiser_SceneMusic(&cpu) == RECOMP_RETURN_TAILCALL && target == 0x02b655);
  }
  /* A hub, native Act 2 rematch or final boss must still load its own song. */
  for (unsigned id = 0; id <= kActRaiserDeathHeimRush_FinalEncounter; ++id) {
    rush_encounter = ActRaiserDeathHeimRush_Encounter(id);
    ram[kActRaiserWram_MapGroup] = 7;
    ram[kActRaiserWram_CurrentMap] = id == 12 ? 8 : 1;
    CpuState cpu = {.PB = 2, .P = CPU_P_M};
    cpu_p_to_mirrors(&cpu);
    assert(ActRaiser_SceneMusic(&cpu) == RECOMP_RETURN_TAILCALL && target == 0x02b655);
  }
  rush_active = false;
  rush_encounter = NULL;
}

static void BossMusicActor(void) {
  for (unsigned city = 1; city <= 6; ++city) {
    rush_encounter = ActRaiserDeathHeimRush_Encounter((city - 1) * 2);
    rush_active = true;
    memset(ram, 0x5a, sizeof(ram));
    ram[kActRaiserWram_MapGroup] = city;
    ram[kActRaiserWram_CurrentMap] = rush_encounter->map;
    const uint16_t slot = 0x1320;
    ByteOrder_WriteLe16(ram + slot, 0x0800);
    ByteOrder_WriteLe16(ram + slot + kActRaiserActionObject_Handler, 0xa3fe);
    ByteOrder_WriteLe16(ram + slot + kActRaiserActionObject_LocalCounter, 0);
    CpuState cpu = {.X = slot, .A = 0x1234, .S = 0x1efe, .P = CPU_P_C};
    cpu_p_to_mirrors(&cpu);
    const CpuState before = cpu;
    uint8_t expected[sizeof(ram)];
    memcpy(expected, ram, sizeof(ram));
    assert(ActRaiser_DeathHeimBossMusicEntry(&cpu));
    assert(ActRaiser_DeathHeimBossMusic(&cpu) == RECOMP_RETURN_TAILCALL);
    assert(target == 0x00a4b4 && origin == 0x00a3fe);
    assert(!memcmp(&cpu, &before, sizeof(cpu)) && !memcmp(expected, ram, sizeof(ram)));
    /* Authored trigger actors become status0 before this same music entry. */
    ByteOrder_WriteLe16(ram + slot, 0);
    assert(ActRaiser_DeathHeimBossMusicEntry(&cpu));
    for (unsigned bad = 0; bad < 11; ++bad) {
      cpu = before;
      memcpy(ram, expected, sizeof(ram));
      switch (bad) {
      case 0: rush_active = false; break;
      case 1: cpu.PB = 1; break;
      case 2: cpu.DB = 1; break;
      case 3: cpu.m_flag = 1; break;
      case 4: cpu.x_flag = 1; break;
      case 5: cpu.D = 1; break;
      case 6: cpu.X = slot + 1; break;
      case 7: cpu.X = 0xffff; break;
      case 8: ByteOrder_WriteLe16(ram + slot, 0x4000); break;
      case 9: ByteOrder_WriteLe16(ram + slot + kActRaiserActionObject_Handler, 0xa410); break;
      case 10: ByteOrder_WriteLe16(ram + slot + kActRaiserActionObject_LocalCounter, 1); break;
      }
      assert(!ActRaiser_DeathHeimBossMusicEntry(&cpu));
      rush_active = true;
    }
  }
  rush_active = false;
  rush_encounter = NULL;
  assert(!ActRaiser_DeathHeimBossMusicEntry(NULL));
}

static void RetryMusicCache(void) {
  for (unsigned city = 1; city <= 6; ++city) {
    rush_encounter = ActRaiserDeathHeimRush_Encounter((city - 1) * 2);
    rush_active = true;
    memset(ram, 0x5a, sizeof(ram));
    ram[kActRaiserWram_MapGroup] = city;
    ram[kActRaiserWram_CurrentMap] = rush_encounter->map;
    ram[kActRaiserWram_Lives] = 1;
    ByteOrder_WriteLe16(ram + 0xab, 0x9470);
    ram[0xad] = 0x1b;
    CpuState cpu = {.PB = 2, .X = kActRaiserWram_PlayerObject,
                   .A = 0x0040, .S = 0x1efb, .P = CPU_P_C | CPU_P_V};
    cpu_p_to_mirrors(&cpu);
    const CpuState before = cpu;
    uint8_t expected[sizeof(ram)];
    memcpy(expected, ram, sizeof(ram));
    assert(ActRaiser_DeathHeimRetryMusicEntry(&cpu));
    assert(ActRaiser_DeathHeimRetryMusic(&cpu) == RECOMP_RETURN_TAILCALL);
    assert(target == 0x02bd16 && origin == 0x02bd14);
    assert(!memcmp(&cpu, &before, sizeof(cpu)) && !memcmp(expected, ram, sizeof(ram)));
    /* Skip only the cache invalidation. Native code still owns death effects,
     * retry animation, resources and the life debit after this boundary. */
    for (unsigned bad = 0; bad < 12; ++bad) {
      cpu = before;
      memcpy(ram, expected, sizeof(ram));
      switch (bad) {
      case 0: rush_active = false; break;
      case 1: ram[kActRaiserWram_Lives] = 0; break;
      case 2: cpu.PB = 0; break;
      case 3: cpu.DB = 1; break;
      case 4: cpu.D = 1; break;
      case 5: cpu.emulation = 1; break;
      case 6: cpu.m_flag = 1; break;
      case 7: cpu.x_flag = 1; break;
      case 8: cpu._flag_D = 1; break;
      case 9: cpu.P |= CPU_P_D; break;
      case 10: ram[kActRaiserWram_MapGroup] = 7; break;
      case 11: ram[kActRaiserWram_CurrentMap] = rush_encounter->map + 1; break;
      }
      assert(!ActRaiser_DeathHeimRetryMusicEntry(&cpu));
      rush_active = true;
    }
    cpu = before;
    memcpy(ram, expected, sizeof(ram));
    /* Even an owned prerequisite room is not a live encounter retry. */
    if (rush_encounter->map > 1) {
      arena_loading = true;
      ram[kActRaiserWram_CurrentMap] = 1;
      assert(!ActRaiser_DeathHeimRetryMusicEntry(&cpu));
      arena_loading = false;
    }
    /* Act 2 and final-boss retries retain their native cache invalidation. */
    for (unsigned id = 1; id <= kActRaiserDeathHeimRush_FinalEncounter; ++id) {
      if (!(id & 1) && id != kActRaiserDeathHeimRush_FinalEncounter) continue;
      rush_encounter = ActRaiserDeathHeimRush_Encounter(id);
      ram[kActRaiserWram_MapGroup] = rush_encounter->map_group;
      ram[kActRaiserWram_CurrentMap] = rush_encounter->map;
      assert(!ActRaiser_DeathHeimRetryMusicEntry(&cpu));
    }
  }
  rush_active = false;
  rush_encounter = NULL;
  assert(!ActRaiser_DeathHeimRetryMusicEntry(NULL));
}
static size_t SongSize(const uint8_t *r, size_t offset) {
  const size_t start = offset;
  for (unsigned block = 0; block < 64; ++block) {
    assert(offset <= 0xffffc);
    const unsigned size = ByteOrder_ReadLe16(r + offset), dest = ByteOrder_ReadLe16(r + offset + 2);
    offset += 4;
    if (!size) {
      assert(dest & 255);
      assert(offset - 1 + (dest & 255) <= 0x100000);
      return offset - 1 + (dest & 255) - start;
    }
    assert(size <= 32768 && size <= 0x100000 - offset);
    offset += size;
  }
  assert(false);
  return 0;
}
static void RomCheck(char **paths) {
  static const unsigned lengths[] = {6, 5, 3, 1, 4, 7, 6, 6};
  uint8_t *r[5];
  for (unsigned i = 0; i < 5; ++i) {
    FILE *f = fopen(paths[i], "rb");
    assert(f);
    r[i] = malloc(0x100000);
    assert(r[i]);
    assert(fread(r[i], 1, 0x100000, f) == 0x100000 && fgetc(f) == EOF);
    fclose(f);
    size_t cursor = 0x28003;
    unsigned caves = 0;
    for (unsigned guard = 0; guard < 64; ++guard) {
      const unsigned group = r[i][cursor++], room = r[i][cursor++];
      if (group > 1) break;
      while (r[i][cursor]) {
        unsigned byte = r[i][cursor++], bit = 0;
        while (byte >> 1) {
          byte >>= 1;
          ++bit;
        }
        if (bit == 1 && group == 1 && (room == 2 || room == 3)) {
          assert(r[i][cursor] == 1 && !r[i][cursor + 1]);
          const size_t file = r[i][cursor + 2] | r[i][cursor + 3] << 8 | r[i][cursor + 4] << 16;
          assert(file == (i == 1 ? 0xbf04c : 0x7769f));
          ++caves;
        }
        cursor += lengths[bit];
        assert(cursor < 0x30000);
      }
      ++cursor;
    }
    assert(caves == 2);
  }
  const size_t size = SongSize(r[0], 0xc147f);
  assert(size == SongSize(r[1], 0xbf04c) && !memcmp(r[0] + 0xc147f, r[1] + 0xbf04c, size));
  for (unsigned i = 0; i < 5; ++i)
    free(r[i]);
}
int main(int argc, char **argv) {
  Adapters();
  BorrowedArenaMusic();
  BossMusicActor();
  RetryMusicCache();
  if (argc == 6)
    RomCheck(argv + 1);
  else
    assert(argc == 1);
  puts("Scene music: regional routing, borrowed arenas, boss music "
       "and native cache preservation passed");
  return 0;
}
