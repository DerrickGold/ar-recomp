#include "actraiser/actraiser_death_heim_runtime.h"
#include "actraiser/actraiser_death_heim_arena.h"
#include "actraiser_game.h"
#include "byte_order.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <string.h>

static uint8_t ram[65536];
static uint32_t target, origin;
static unsigned arena_resets;
static bool teleport_active, act1_beam;
static unsigned teleport_prepares, teleport_restores;
bool ActRaiserDeathHeimTeleportAssets_Prepare(CpuState *cpu) {
  assert(cpu && !teleport_active);
  teleport_active = true;
  ++teleport_prepares;
  return true;
}
void ActRaiserDeathHeimTeleportAssets_Restore(CpuState *cpu) {
  assert(cpu && teleport_active);
  teleport_active = false;
  ++teleport_restores;
}
void ActRaiserDeathHeimTeleportAssets_Reset(void) { teleport_active = false; }
void ActRaiserDeathHeimEyes_BeginHub(CpuState *cpu,
    const ActRaiserDeathHeimEncounter *next, const ActRaiserDeathHeimEncounter *last) {
  (void)cpu;
  assert(next);
  (void)last;
}
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
  (void)cpu;
  assert(bank == 0);
  return ram[address];
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 address) {
  return cpu_read8(cpu, bank, address) | (uint16)cpu_read8(cpu, bank, address + 1) << 8;
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
  (void)cpu;
  assert(bank == 0);
  ram[address] = value;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 address, uint16 value) {
  cpu_write8(cpu, bank, address, value);
  cpu_write8(cpu, bank, address + 1, value >> 8);
}
int cpu_hle_tailcall_request(uint32_t pc, uint32_t site) { target = pc; origin = site; return 1; }
void ActRaiserDeathHeimArenas_Reset(void) { ++arena_resets; }
bool ActRaiserDeathHeimArenas_Begin(CpuState *cpu, unsigned city) {
  const ActRaiserDeathHeimEncounter *e = ActRaiserDeathHeim_CurrentEncounter();
  assert(e && e->act == 1 && e->city == city);
  cpu_write8(cpu, 0, 0x1a, e->map);
  cpu_write8(cpu, 0, 0x1b, e->map_group);
  return true;
}
static void Set16(unsigned address, unsigned value) {
  ByteOrder_WriteLe16(ram + address, (uint16_t)value);
}
static unsigned Get16(unsigned address) { return ByteOrder_ReadLe16(ram + address); }
static CpuState Cpu(bool byte_a) {
  CpuState cpu = {.S=0x1f5,.A=0x7700,.X=0x0ae0,.Y=0x08a0,.m_flag=byte_a};
  cpu_mirrors_to_p(&cpu);
  return cpu;
}
static void New(const ActRaiserDeathHeimRushConfig *config) {
  ActRaiserDeathHeim_Reset();
  act1_beam = false;
  memset(ram, 0, sizeof(ram));
  assert(ActRaiserDeathHeim_Configure(config));
  Set16(0x18,0x0107);
  Set16(0x0ae0+0x32,0xf3c8);
  ram[0x1c]=4;
  ram[0x1d]=9;
  ram[0x1e]=24;
  ram[0x21]=3;
  Set16(0x1f,0x1234);
}
static void ProtectResources(void) {
  assert(ram[0x1c]==4 && ram[0x1d]==9 && ram[0x1e]==24 && ram[0x21]==3 && Get16(0x1f)==0x1234);
}
static ActRaiserDeathHeimEncounter Route(void) {
  Set16(0x18,0x0107);
  CpuState cpu=Cpu(true), before=cpu;
  assert(ActRaiser_DeathHeimHubRouteEntry(&cpu));
  assert(ActRaiser_DeathHeimHubRoute(&cpu)==RECOMP_RETURN_TAILCALL);
  assert(target==0xf3f2 && origin==0xf3e4 && !cpu.m_flag);
  assert(cpu.S==before.S && cpu.X==before.X && cpu.Y==before.Y);
  const ActRaiserDeathHeimEncounter *e=ActRaiserDeathHeim_CurrentEncounter();
  assert(e && ram[0x1a]==e->map && ram[0x1b]==e->map_group);
  ProtectResources();
  return *e;
}
static CpuState AdmitNative(ActRaiserDeathHeimEncounter e) {
  act1_beam = false;
  Set16(0x18,(e.map<<8)|e.map_group);
  const uint16_t actor=0x12e0;
  memset(ram+actor,0,0x40);
  Set16(actor+0x32,e.source);
  Set16(actor+0x16,0x5000);
  ram[actor+0x18]=0x7e;
  Set16(actor+0x30,0x4000);
  Set16(actor+0x3a,e.map_group==7?0x1c:0);
  CpuState cpu=Cpu(false);
  cpu.X=actor;
  if(e.act==1) {
    cpu.DB=0x0a;
    assert(ActRaiserDeathHeim_AdmitEncounter(&cpu,actor));
    cpu.DB=0;
  } else {
    assert(ActRaiser_DeathHeimAdmitEntry(&cpu));
    assert(ActRaiser_DeathHeimAdmit(&cpu)==RECOMP_RETURN_TAILCALL);
    assert(target==0xf7f7 && Get16(0xf4)==0xffff && cpu.S==0x1f5);
  }
  return cpu;
}
static void FinishArrival(CpuState *cpu) {
  const uint16_t stack = cpu->S;
  assert(ActRaiserDeathHeim_Phase() == kActRaiserDeathHeimRush_Preparing);
  assert(Get16(kActRaiserWram_PlayerObject) == 0x0800 && Get16(0xf4) == 0);
  Set16(0xf4, 0xffff); /* ordinary loader/player setup follows placement */
  assert(ActRaiser_DeathHeimArrivalStartEntry(cpu));
  assert(ActRaiser_DeathHeimArrivalStart(cpu) == RECOMP_RETURN_TAILCALL);
  assert(target == 0xf7b8 && !ActRaiser_DeathHeimArrivalStartEntry(cpu));
  assert(Get16(0xf4) == 0 && Get16(kActRaiserWram_PlayerObject) == 0x0800);
  assert(teleport_active);
  /* The unmodified F7B8/8657 native animation runs state14 before F7C9. */
  Set16(cpu->X + 0x16, 0x4000);
  Set16(cpu->X + 0x12, 0xf7c9);
  assert(ActRaiser_DeathHeimArrivalCompleteEntry(cpu));
  assert(ActRaiser_DeathHeimArrivalComplete(cpu) == RECOMP_RETURN_TAILCALL);
  assert(target == 0x868f && origin == 0xf7c9 && cpu->S == stack && !cpu->m_flag);
  assert(!teleport_active && ActRaiserDeathHeim_Phase() == kActRaiserDeathHeimRush_Combat);
  assert(!ActRaiser_DeathHeimArrivalCompleteEntry(cpu));
}
static CpuState Admit(ActRaiserDeathHeimEncounter encounter) {
  CpuState cpu = AdmitNative(encounter);
  if (encounter.act == 1) {
    Set16(kActRaiserWram_ActionCameraSubject, kActRaiserWram_PlayerObject);
    Set16(kActRaiserWram_PlayerObject, 0);
    Set16(0xf4, 0xffff);
    assert(ActRaiserDeathHeim_BeginArrival(&cpu, cpu.X));
    FinishArrival(&cpu);
  }
  return cpu;
}
static void CompletionShape(CpuState *cpu, ActRaiserDeathHeimEncounter encounter) {
  cpu->m_flag = 0;
  cpu_mirrors_to_p(cpu);
  if (encounter.act == 1 && !act1_beam) {
    /* Antlion stores its child controller here after native initialization. */
    if (encounter.city == 3) Set16(cpu->X + 0x3a, 0x10a0);
  } else {
    Set16(cpu->X + 0x16, 0x4000);
    Set16(cpu->X + 0x12, 0xfeec);
    Set16(cpu->X + 0x30, 0x0030);
  }
}
static void CompleteCallback(CpuState *cpu, ActRaiserDeathHeimEncounter encounter) {
  CompletionShape(cpu, encounter);
  if (encounter.act == 1 && !act1_beam) {
    assert(ActRaiser_DeathHeimAct1VictoryEntry(cpu));
    assert(ActRaiser_DeathHeimAct1Victory(cpu) == RECOMP_RETURN_TAILCALL);
  } else {
    assert(ActRaiser_DeathHeimVictoryEntry(cpu));
    assert(ActRaiser_DeathHeimVictory(cpu) == RECOMP_RETURN_TAILCALL);
  }
  assert(target == 0xff07 && cpu->m_flag);
}
static void Win(CpuState *cpu, ActRaiserDeathHeimEncounter e) {
  const uint16_t stack=cpu->S;
  CompletionShape(cpu, e);
  if(e.act==1) {
    const unsigned destination = Get16(0x1a), progress = Get16(0x347);
    const uint8_t eyes = ActRaiserDeathHeim_EyeMask(e.city);
    assert(ActRaiser_DeathHeimAct1VictoryEntry(cpu));
    assert(ActRaiser_DeathHeimAct1Victory(cpu)==RECOMP_RETURN_TAILCALL);
    assert(target == 0xfe89 && !cpu->m_flag && cpu->S == stack);
    assert(Get16(0x1a) == destination && Get16(0x347) == progress);
    assert(ActRaiserDeathHeim_EyeMask(e.city) == eyes);
    assert(!ActRaiserDeathHeim_RequestHubReturn(cpu));
    assert(ActRaiser_DeathHeimAct1Victory(cpu) == RECOMP_RETURN_TAILCALL);
    assert(target == 0x868f && origin == 0xa5fa && cpu->S == stack);
    /* Native FE89 waits64 frames before this continuation; FE9D owns the
     * state15 beam, then FEE6 waits32 more before the actual FEEC commit. */
    Set16(cpu->X + 0x12, 0xfe9d);
    Set16(cpu->X + 0x30, 0x0030);
    if (e.city == 4) {
      Set16(cpu->X + 0x28, 0xc123); /* Aitos can die inverted during flight */
      Set16(kActRaiserWram_PlayerObject + 0x28, 0x4000);
    }
    assert(ActRaiser_DeathHeimDepartureStartEntry(cpu));
    assert(ActRaiser_DeathHeimDepartureStart(cpu) == RECOMP_RETURN_TAILCALL);
    assert(target == 0xfe9d && !ActRaiser_DeathHeimDepartureStartEntry(cpu));
    if (e.city == 4) assert(Get16(cpu->X + 0x28) == 0x4123);
    assert(teleport_active && Get16(0x1a) == destination);
    act1_beam = true;
    CompleteCallback(cpu, e);
    assert(!teleport_active);
  } else {
    /* Real FE89 victory transforms the admitted boss into its beam driver. */
    assert(ActRaiser_DeathHeimVictoryEntry(cpu));
    assert(ActRaiser_DeathHeimVictory(cpu)==RECOMP_RETURN_TAILCALL);
  }
  assert(target==0xff07 && Get16(0x1a)==0x0701 && cpu->S==stack && cpu->m_flag);
  assert(!ActRaiser_DeathHeimVictoryEntry(cpu));
  assert(!ActRaiser_DeathHeimAct1VictoryEntry(cpu));
  ProtectResources();
  /* A repeated native continuation is consumed even after final completion. */
  uint8_t snapshot[sizeof(ram)];
  memcpy(snapshot, ram, sizeof(ram));
  CompleteCallback(cpu, e);
  assert(!memcmp(snapshot, ram, sizeof(ram)) && cpu->S == stack);
}
static void NativeBypass(void) {
  ActRaiserDeathHeimRushConfig config=ActRaiserDeathHeimRushConfig_Default();
  New(&config);
  CpuState cpu=Cpu(true);
  const CpuState before=cpu;
  uint8_t snapshot[sizeof(ram)];
  memcpy(snapshot,ram,sizeof(ram));
  assert(!ActRaiser_DeathHeimHubRouteEntry(&cpu));
  assert(!memcmp(&cpu,&before,sizeof(cpu)) && !memcmp(ram,snapshot,sizeof(ram)));
  ActRaiserDeathHeim_ObserveScene(&cpu);
  assert(ActRaiserDeathHeim_Active());
  config.shuffle_order=true;
  assert(!ActRaiserDeathHeim_Configure(&config));
  Set16(0x18,0x0700);
  ActRaiserDeathHeim_ObserveScene(&cpu);
  assert(!ActRaiserDeathHeim_Active() && ActRaiserDeathHeim_Configure(&config));
  assert(ActRaiserDeathHeim_RosterAvailable(kActRaiserDeathHeimRoster_Standard));
  assert(ActRaiserDeathHeim_RosterAvailable(kActRaiserDeathHeimRoster_Act1));
  assert(ActRaiserDeathHeim_RosterAvailable(kActRaiserDeathHeimRoster_Extended));
  assert(!ActRaiserDeathHeim_RosterAvailable(kActRaiserDeathHeimRoster_Extended + 1));
  assert(!ActRaiserDeathHeim_RosterAvailable((unsigned)-1));
}
static void FullRuns(void) {
  for(unsigned roster=0;roster<3;++roster)for(unsigned shuffled=0;shuffled<2;++shuffled) {
    if(!roster && !shuffled)continue;
    ActRaiserDeathHeimRushConfig config=ActRaiserDeathHeimRushConfig_Default();
    config.roster=roster;
    config.shuffle_order=shuffled;
    config.seed=218;
    New(&config);
    unsigned seen=0,count=roster==2?12:6;
    for(unsigned i=0;i<count;++i) {
      ActRaiserDeathHeimEncounter e=Route();
      assert(e.id<12 && !(seen&(1u<<e.id)));
      CpuState cpu=Admit(e), bad=cpu;
      bad.X+=0x40;
      assert(!ActRaiserDeathHeim_RequestHubReturn(&bad));
      if(i==2) {
        const uint8_t eyes=ActRaiserDeathHeim_EyeMask(e.city);
        assert(ActRaiserDeathHeim_RetryEncounter(&cpu));
        assert(!ActRaiserDeathHeim_RequestHubReturn(&cpu));
        CpuState retry=Admit(e);
        assert(ActRaiserDeathHeim_EyeMask(e.city)==eyes);
        cpu=retry;
        assert(ActRaiserDeathHeim_CurrentEncounter()->id==e.id);
      }
      Win(&cpu,e);
      seen|=1u<<e.id;
      assert(Get16(0x347)<7);
      if(roster==2)assert(!(ActRaiserDeathHeim_EyeMask(e.city)&(1u<<(e.act-1))));
      else assert(!ActRaiserDeathHeim_EyeMask(e.city));
      assert(!ActRaiserDeathHeim_Configure(&config));
    }
    assert(seen==(roster==0?0xaaa:roster==1?0x555:0xfff));
    assert(Get16(0x347)==6);
    ActRaiserDeathHeimEncounter final=Route();
    assert(final.id==12 && final.map==8);
    CpuState cpu=Admit(final);
    Win(&cpu,final);
    assert(Get16(0x347)==7 && ram[0x334]==1);
    Set16(0x18,0x0107);
    cpu=Cpu(true);
    assert(!ActRaiser_DeathHeimHubRouteEntry(&cpu));
  }
}
static CpuState SpawnHubEyes(void) {
  static const uint16_t rows[12][3]={{0xf408,16,144},{0xf494,16,144},
    {0xf3fa,240,144},{0xf486,240,144},{0xf408,48,128},{0xf494,48,128},
    {0xf430,192,128},{0xf486,208,128},{0xf408,80,112},{0xf494,80,112},
    {0xf3fa,176,112},{0xf486,176,112}};
  Set16(0x18,0x0107);
  CpuState cpu=Cpu(false);
  cpu.X=0xba0;
  for(unsigned i=0;i<12;++i) {
    unsigned at=cpu.X+(i+1)*0x40;
    memset(ram+at,0,0x40);
    Set16(at+0x32,rows[i][0]);
    Set16(at+0x34,rows[i][1]);
    Set16(at+0x36,rows[i][2]);
    Set16(at+0x16,0x4000);
    ram[at+0x18]=0x7e;
  }
  return cpu;
}
static void EyesAndIdentity(void) {
  ActRaiserDeathHeimRushConfig config=ActRaiserDeathHeimRushConfig_Default();
  config.roster=2;
  New(&config);
  ActRaiserDeathHeimEncounter first=Route();
  CpuState cpu=Admit(first);
  Win(&cpu,first);
  cpu=SpawnHubEyes();
  assert(ActRaiser_DeathHeimEyesEntry(&cpu));
  assert(ActRaiser_DeathHeimEyes(&cpu)==RECOMP_RETURN_TAILCALL);
  assert(target==0xf5a5 && origin==0xf534 && cpu.S==0x1f5);
  assert(ActRaiserDeathHeim_EyeMask(1)==2);
  assert(Get16(0xbe0)==0 && Get16(0xc20)==0); /* surviving eye actor never retired */
  assert(Get16(0xbe0+0x38)==0 && Get16(0xc20+0x38)==0xffff); /* next Act 2 same city */
  for(unsigned i=0;i<12;++i)assert(ActRaiserDeathHeim_EyeActorCity(&cpu,cpu.X+(i+1)*0x40)==i/2+1);
  Set16(0xbe0+0x34,17);
  assert(!ActRaiser_DeathHeimEyesEntry(&cpu));
  assert(!ActRaiserDeathHeim_EyeActorCity(&cpu,0xbe0));

  ActRaiserDeathHeimEncounter second = Route();
  assert(second.city == 1 && second.act == 2);
  cpu = Admit(second);
  Win(&cpu, second);
  cpu = SpawnHubEyes();
  assert(ActRaiser_DeathHeimEyes(&cpu) == RECOMP_RETURN_TAILCALL);
  assert(!ActRaiserDeathHeim_EyeMask(1));
  assert(Get16(0xbe0) == 0 && Get16(0xc20) == 0);
  assert(Get16(0xc20 + 0x38) == 0xffff); /* final eye still gets its victory flare */

  ActRaiserDeathHeimEncounter third = Route();
  assert(third.city == 2 && third.act == 1);
  cpu = Admit(third);
  Win(&cpu, third);
  cpu = SpawnHubEyes();
  assert(ActRaiser_DeathHeimEyes(&cpu) == RECOMP_RETURN_TAILCALL);
  assert(!ActRaiserDeathHeim_EyeMask(1));
  assert(Get16(0xbe0) == 0 && Get16(0xbe0 + 0x38) == 0);
  assert(Get16(0xc20) == 0x4000); /* idle flare must not relight a completed statue */
  assert(Get16(0xca0) == 0 && Get16(0xca0 + 0x38) == 0xffff);
}
static void Fingerprints(void) {
  uint8_t before[32]={1},first[32],retry[32],again[32];bool baseline;
  ActRaiserDeathHeim_Reset();
  assert(ActRaiserDeathHeim_Fingerprint(before,first,&baseline) && baseline &&
      !memcmp(first,before,32));
  ActRaiserDeathHeimRushConfig config=ActRaiserDeathHeimRushConfig_Default();
  config.shuffle_order=true;
  New(&config);
  ActRaiserDeathHeimEncounter e=Route();
  CpuState cpu=Admit(e);
  assert(ActRaiserDeathHeim_Fingerprint(before,first,&baseline) && !baseline);
  (void)Admit(e);
  assert(ActRaiserDeathHeim_Fingerprint(before,retry,&baseline) && memcmp(first,retry,32));
  New(&config);
  e=Route();
  cpu=Admit(e);
  assert(ActRaiserDeathHeim_Fingerprint(before,again,&baseline) && !memcmp(first,again,32));
  Set16(0x18,0x0700);
  ActRaiserDeathHeim_ObserveScene(&cpu);
  assert(!ActRaiserDeathHeim_Active());
  assert(ActRaiserDeathHeim_Fingerprint(before,again,&baseline) && baseline);
  assert(arena_resets);
}
static void ShuffledEyeFlares(void) {
  ActRaiserDeathHeimRushConfig config = ActRaiserDeathHeimRushConfig_Default();
  config.roster = kActRaiserDeathHeimRoster_Extended;
  config.shuffle_order = true;
  config.seed = 218;
  New(&config);
  bool dormant[7] = {false};
  bool partial_old = false, act2_first = false, reactivated = false;
  for (unsigned i = 0; i < 12; ++i) {
    ActRaiserDeathHeimEncounter encounter = Route();
    CpuState cpu = Admit(encounter);
    Win(&cpu, encounter);
    cpu = SpawnHubEyes();
    assert(ActRaiser_DeathHeimEyes(&cpu) == RECOMP_RETURN_TAILCALL);
    for (unsigned city = 1; city <= 6; ++city) {
      const uint8_t lit = ActRaiserDeathHeim_EyeMask(city);
      const bool just_beaten = city == encounter.city;
      const unsigned glow = 0xbe0 + (city - 1) * 0x80;
      const unsigned flare = glow + 0x40;
      assert(Get16(glow) == 0 && Get16(glow + 0x38) == 0);
      assert(Get16(flare) == (lit != 3 && !just_beaten ? 0x4000 : 0));
      if ((lit == 1 || lit == 2) && !just_beaten) {
        partial_old = true;
        act2_first |= lit == 1;
        dormant[city] = true;
      }
      if (just_beaten) {
        assert(Get16(flare + 0x38) == 0xffff);
        reactivated |= dormant[city];
      }
    }
  }
  assert(partial_old && act2_first && reactivated);
}
static void NativeTeleportLifecycle(void) {
  ActRaiserDeathHeimRushConfig config = ActRaiserDeathHeimRushConfig_Default();
  config.roster = kActRaiserDeathHeimRoster_Act1;
  New(&config);
  for (unsigned city = 1; city <= 6; ++city) {
    const ActRaiserDeathHeimEncounter encounter = Route();
    assert(encounter.city == city);
    CpuState cpu = AdmitNative(encounter);
    Set16(kActRaiserWram_ActionCameraSubject, kActRaiserWram_PlayerObject);
    Set16(kActRaiserWram_PlayerObject, 0);
    Set16(kActRaiserWram_PlayerObject + 2, 3800);
    Set16(kActRaiserWram_PlayerObject + 4, 700);
    Set16(0xf4, 0xffff);
    Set16(cpu.X + 2, 4000);
    Set16(cpu.X + 4, 720);
    Set16(cpu.X + 0x12, encounter.source + 12);
    Set16(cpu.X + 0x28, 0xc123);
    Set16(kActRaiserWram_PlayerObject + 0x28, 0x4000);
    uint8_t original[kActRaiserActionObjectStride];
    memcpy(original, ram + cpu.X, sizeof(original));
    const unsigned prepared = teleport_prepares, restored = teleport_restores;
    const unsigned progress = Get16(0x347), destination = Get16(0x1a);
    cpu.DB = 0x0a;
    assert(ActRaiserDeathHeim_BeginArrival(&cpu, cpu.X));
    assert(!ActRaiserDeathHeim_BeginArrival(&cpu, cpu.X));
    cpu.DB = 0;
    assert(Get16(cpu.X + 2) == 3800 && Get16(cpu.X + 4) == 700);
    assert(Get16(cpu.X + 0x12) == 0xf7b8 && Get16(cpu.X + 0x30) == 0x0030);
    assert(Get16(cpu.X + 0x28) == 0x4123); /* beam follows the player's full orientation */
    assert(teleport_prepares == prepared); /* native first tick owns the swap */
    FinishArrival(&cpu);
    assert(teleport_prepares == prepared + 1 && teleport_restores == restored + 1);
    assert(!memcmp(original, ram + cpu.X, sizeof(original)));
    assert(Get16(kActRaiserWram_PlayerObject) == 0 && Get16(0xf4) == 0xffff);
    assert(Get16(0x347) == progress && Get16(0x1a) == destination && cpu.S == 0x1f5);
    ProtectResources();
    Win(&cpu, encounter);
  }

  New(&config);
  ActRaiserDeathHeimEncounter encounter = Route();
  CpuState cpu = Admit(encounter);
  assert(ActRaiser_DeathHeimAct1Victory(&cpu) == RECOMP_RETURN_TAILCALL);
  Set16(cpu.X + 0x12, 0xfe9d);
  Set16(cpu.X + 0x30, 0x0030);
  assert(ActRaiser_DeathHeimDepartureStart(&cpu) == RECOMP_RETURN_TAILCALL);
  assert(ActRaiserDeathHeim_RetryEncounter(&cpu) && !teleport_active);
  assert(Get16(cpu.X) == 0x4000); /* no beam may read the restored ordinary blob */
  const unsigned destination = Get16(0x1a), progress = Get16(0x347);
  act1_beam = true;
  CompleteCallback(&cpu, encounter);
  assert(Get16(0x1a) == destination && Get16(0x347) == progress);

  New(&config);
  encounter = Route();
  cpu = AdmitNative(encounter);
  Set16(kActRaiserWram_ActionCameraSubject, kActRaiserWram_PlayerObject);
  assert(ActRaiserDeathHeim_BeginArrival(&cpu, cpu.X));
  assert(ActRaiser_DeathHeimArrivalStart(&cpu) == RECOMP_RETURN_TAILCALL);
  Set16(cpu.X + 0x16, 0x4000);
  assert(ActRaiserDeathHeim_RetryEncounter(&cpu) && !teleport_active);
  assert(Get16(cpu.X) == 0x4000);
  assert(ActRaiser_DeathHeimArrivalComplete(&cpu) == RECOMP_RETURN_TAILCALL);
  assert(target == 0x868f && origin == 0xf7c9);
  assert(Get16(cpu.X) == 0x4000 && ActRaiserDeathHeim_Phase() == kActRaiserDeathHeimRush_Retry);
  ActRaiserDeathHeim_Reset();
  assert(!ActRaiser_DeathHeimArrivalCompleteEntry(&cpu));
}
static void DeathLifecycle(void) {
  for (unsigned roster = 0; roster < 2; ++roster) {
    ActRaiserDeathHeimRushConfig config = ActRaiserDeathHeimRushConfig_Default();
    config.roster = roster;
    config.shuffle_order = true;
    New(&config);
    ActRaiserDeathHeimEncounter encounter = Route();
    CpuState cpu = Admit(encounter);
    Set16(kActRaiserWram_ActionCameraSubject, kActRaiserWram_PlayerObject);
    Set16(kActRaiserWram_PlayerFlags, 0x0400);
    Set16(kActRaiserWram_DestinationMap, 0xffff);
    const uint8_t eyes = ActRaiserDeathHeim_EyeMask(encounter.city);
    const unsigned progress = Get16(kActRaiserWram_DeathHeimProgress);
    CompleteCallback(&cpu, encounter);
    assert(ActRaiserDeathHeim_Phase() == kActRaiserDeathHeimRush_Retry);
    assert(ActRaiserDeathHeim_EyeMask(encounter.city) == eyes);
    assert(Get16(kActRaiserWram_DeathHeimProgress) == progress);
    assert(Get16(kActRaiserWram_DestinationMap) == 0xffff);
    assert(ActRaiserDeathHeim_RetryEncounter(&cpu));
    CompleteCallback(&cpu, encounter);
    assert(Get16(kActRaiserWram_DestinationMap) == 0xffff);
    ProtectResources();
    Set16(kActRaiserWram_PlayerFlags, 0);
    cpu = Admit(encounter);
    assert(ActRaiserDeathHeim_RetryEncounter(&cpu));
    CompleteCallback(&cpu, encounter);
    assert(ActRaiserDeathHeim_EyeMask(encounter.city) == eyes);
    cpu = Admit(encounter);
    Win(&cpu, encounter);

    /* Hub settlement changes Current(), but the retired actor is still old. */
    ActRaiserDeathHeimEncounter next = Route();
    const unsigned destination = Get16(kActRaiserWram_DestinationMap);
    Set16(0x18, (encounter.map << 8) | encounter.map_group);
    CompleteCallback(&cpu, encounter);
    assert(Get16(kActRaiserWram_DestinationMap) == destination);
    assert(ActRaiserDeathHeim_CurrentEncounter()->id == next.id);

    /* Story game over, Special game over, and title all end the controller. */
    static const unsigned exits[] = {0x0700, 0x0108, 0x0000};
    for (unsigned i = 0; i < sizeof(exits) / sizeof(exits[0]); ++i) {
      New(&config);
      encounter = Route();
      cpu = Admit(encounter);
      ram[kActRaiserWram_Lives] = 0;
      Set16(0x18, exits[i]);
      ActRaiserDeathHeim_ObserveScene(&cpu);
      assert(!ActRaiserDeathHeim_Active() && !ActRaiserDeathHeim_CurrentEncounter());
      Set16(0x18, (encounter.map << 8) | encounter.map_group);
      CompletionShape(&cpu, encounter);
      assert(!ActRaiser_DeathHeimVictoryEntry(&cpu));
      assert(!ActRaiser_DeathHeimAct1VictoryEntry(&cpu));
      ram[kActRaiserWram_Lives] = 4;
      Set16(kActRaiserWram_DeathHeimProgress, 0);
      assert(Route().id == encounter.id);
    }
  }
}
int main(void) {
  NativeBypass();
  FullRuns();
  EyesAndIdentity();
  ShuffledEyeFlares();
  NativeTeleportLifecycle();
  Fingerprints();
  DeathLifecycle();
  puts("Death Heim runtime: native bypass, frozen routes, root identity, retry, "
       "eyes and final gate passed");
  return 0;
}
