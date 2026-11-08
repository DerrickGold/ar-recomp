#include "actraiser/actraiser_death_heim_runtime.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include "actraiser/actraiser_death_heim_arena.h"
#include "actraiser/actraiser_death_heim_eyes.h"
#include "actraiser/actraiser_death_heim_teleport_assets.h"
#include "actraiser/actraiser_hle_fatal.h"
#include "actraiser_game.h"
#include "byte_order.h"
#include "snesrecomp/support/digest.h"
#include <string.h>

static ActRaiserDeathHeimRushConfig s_config = {
  .seed = 1, .algorithm = kActRaiserDeathHeimRushAlgorithm,
};
static ActRaiserDeathHeimRush s_rush;
static ActRaiserDeathHeimRushToken s_token;
static uint64_t s_generation;
static uint16_t s_owner;
static uint8_t s_admitted_id = kActRaiserDeathHeimRush_NoEncounter;
static bool s_admitted;
static bool s_native_active;
typedef struct Arrival {
  uint8_t actor[kActRaiserActionObjectStride];
  uint16_t owner, player_status, input;
  bool staged, prepared;
} Arrival;
static Arrival s_arrival;
static bool s_departing, s_departure_prepared, s_departure_restored;
static bool RunActive(void) { return ActRaiserDeathHeimRush_Active(&s_rush); }

typedef struct EyeIdentity { uint16_t source, x, y; } EyeIdentity;
/* $0A:C432..C45E: each city owns a glow actor and a separate effect actor.
 * A glow composition contains both eyes, except Aitos's single visible eye.
 * These slots are not acts. */
static const EyeIdentity kEyes[12] = {
  {0xf408,16,144},{0xf494,16,144},
  {0xf3fa,240,144},{0xf486,240,144},
  {0xf408,48,128},{0xf494,48,128},
  {0xf430,192,128},{0xf486,208,128},
  {0xf408,80,112},{0xf494,80,112},
  {0xf3fa,176,112},{0xf486,176,112},
};
static bool Shape(const CpuState *cpu, bool byte_a) {
  return cpu && !cpu->PB && !cpu->DB && !cpu->D && !cpu->emulation &&
      !cpu->x_flag && cpu->m_flag == byte_a && !cpu->_flag_D && !(cpu->P & CPU_P_D);
}
static bool Slot(unsigned owner) {
  return owner >= kActRaiserWram_ActionObjectTable &&
      owner < kActRaiserWram_ActionObjectTable +
                  kActRaiserActionObjectCount * kActRaiserActionObjectStride &&
      (owner - kActRaiserWram_ActionObjectTable) % kActRaiserActionObjectStride == 0;
}
static bool Hub(CpuState *cpu) {
  return cpu_read16(cpu, 0, kActRaiserWram_MapGroup) == 0x0107 &&
      cpu_read16(cpu, 0, kActRaiserWram_DeathHeimProgress) < 7;
}
static bool Enabled(void) {
  return !ActRaiserDeathHeimRushConfig_IsNative(&s_config) &&
      ActRaiserDeathHeim_RosterAvailable(s_config.roster);
}
static bool CanBegin(CpuState *cpu) {
  return Enabled() && Hub(cpu) &&
      (RunActive() ||
       cpu_read16(cpu, 0, kActRaiserWram_DeathHeimProgress) == 0);
}
static void BeginHub(CpuState *cpu) {
  if (!RunActive()) {
    if (!CanBegin(cpu) || s_generation == UINT64_MAX ||
        !ActRaiserDeathHeimRush_Begin(&s_rush, &s_config, ++s_generation))
      ActRaiserHleFatal("Death Heim has no valid encounter recipe");
    s_admitted = false;
  }
  if (s_rush.phase == kActRaiserDeathHeimRush_Returning &&
      !ActRaiserDeathHeimRush_ReturnToHub(&s_rush))
    ActRaiserHleFatal("Death Heim could not settle its hub return");
}
static RecompReturn Tail(uint32_t target, uint32_t site) {
  if (!cpu_hle_tailcall_request(target, site))
    ActRaiserHleFatal("Death Heim lost its native continuation");
  return RECOMP_RETURN_TAILCALL;
}
static void Width(CpuState *cpu, bool byte_a) {
  cpu_mirrors_to_p(cpu);
  cpu->P = (uint8_t)((cpu->P & ~CPU_P_M) | (byte_a ? CPU_P_M : 0));
  cpu_p_to_mirrors(cpu);
}
static bool Root(CpuState *cpu, uint16_t owner,
                 const ActRaiserDeathHeimEncounter *encounter, uint16_t animation) {
  return encounter && Slot(owner) &&
      cpu_read8(cpu, 0, kActRaiserWram_MapGroup) == encounter->map_group &&
      cpu_read8(cpu, 0, kActRaiserWram_CurrentMap) == encounter->map &&
      cpu_read16(cpu, 0, owner + 0x32) == encounter->source &&
      cpu_read16(cpu, 0, owner + 0x16) == animation &&
      cpu_read8(cpu, 0, owner + 0x18) == 0x7e;
}
void ActRaiserDeathHeim_Reset(void) {
  ActRaiserDeathHeimTeleportAssets_Reset();
  memset(&s_arrival, 0, sizeof(s_arrival));
  s_departing = s_departure_prepared = s_departure_restored = false;
  (void)ActRaiserDeathHeimRush_Abandon(&s_rush);
  s_admitted = false;
  s_native_active = false;
  s_owner = 0;
  s_admitted_id = kActRaiserDeathHeimRush_NoEncounter;
  memset(&s_token, 0, sizeof(s_token));
  ActRaiserDeathHeimArenas_Reset();
}
bool ActRaiserDeathHeim_Configure(const ActRaiserDeathHeimRushConfig *config) {
  if (ActRaiserDeathHeim_Active() || !ActRaiserDeathHeimRushConfig_Valid(config)) return false;
  s_config = *config;
  return true;
}
bool ActRaiserDeathHeim_Active(void) { return s_native_active || RunActive(); }
bool ActRaiserDeathHeim_RosterAvailable(unsigned roster) {
  return roster == kActRaiserDeathHeimRoster_Standard ||
      roster == kActRaiserDeathHeimRoster_Act1 ||
      roster == kActRaiserDeathHeimRoster_Extended;
}
bool ActRaiserDeathHeim_ExtendedEnabled(void) {
  return s_config.roster == kActRaiserDeathHeimRoster_Extended;
}
const ActRaiserDeathHeimEncounter *ActRaiserDeathHeim_CurrentEncounter(void) {
  return ActRaiserDeathHeimRush_Current(&s_rush);
}
const ActRaiserDeathHeimEncounter *ActRaiserDeathHeim_LastCompletedEncounter(void) {
  return RunActive() ? ActRaiserDeathHeimRush_Encounter(s_rush.last_completed) : NULL;
}
ActRaiserDeathHeimRushPhase ActRaiserDeathHeim_Phase(void) { return s_rush.phase; }
bool ActRaiserDeathHeim_RetryEncounter(CpuState *cpu) {
  const ActRaiserDeathHeimEncounter *encounter = ActRaiserDeathHeim_CurrentEncounter();
  if (!cpu || !encounter ||
      cpu_read8(cpu, 0, kActRaiserWram_MapGroup) != encounter->map_group ||
      cpu_read8(cpu, 0, kActRaiserWram_CurrentMap) != encounter->map) return false;
  if (s_rush.phase == kActRaiserDeathHeimRush_Retry) return true;
  if (s_admitted && encounter->act == 1 &&
      s_rush.phase == kActRaiserDeathHeimRush_Preparing &&
      !ActRaiserDeathHeimRush_Ready(&s_rush, s_token)) return false;
  if (!s_admitted ||
      !ActRaiserDeathHeimRush_RetryEncounter(&s_rush, s_token)) return false;
  s_admitted = false;
  if (s_arrival.prepared || (s_departure_prepared && !s_departure_restored)) {
    ActRaiserDeathHeimTeleportAssets_Restore(cpu);
    s_departure_restored = true;
    s_arrival.prepared = false;
  }
  if (encounter->act == 1 && (s_arrival.staged || s_departing))
    cpu_write16(cpu, 0, s_owner, 0x4000);
  /* Keep the retired admission identity until the next room admits its root.
   * A late death animation must be consumed, not fall into native act clear. */
  return true;
}
bool ActRaiserDeathHeim_AdmitEncounter(CpuState *cpu, uint16_t owner) {
  const ActRaiserDeathHeimEncounter *encounter = ActRaiserDeathHeim_CurrentEncounter();
  const bool arena_shape = cpu && encounter && encounter->act == 1 &&
      !cpu->PB && cpu->DB == 0x0a && !cpu->D && !cpu->m_flag &&
      !cpu->x_flag && !cpu->emulation && !cpu->_flag_D && !(cpu->P & CPU_P_D);
  /* At initialization roots have no action-object backlink. Native Antlion
   * later reuses $3A for a child controller, so this is an admission check,
   * not a continuing identity invariant for the admitted owner. */
  if ((!Shape(cpu, false) && !arena_shape) || !RunActive() ||
      !Root(cpu, owner, encounter, 0x5000) ||
      Slot(cpu_read16(cpu, 0, owner + 0x3a)) ||
      !(cpu_read16(cpu, 0, owner + 0x30) & 0x4000)) return false;
  if (s_rush.phase == kActRaiserDeathHeimRush_Combat ||
      s_rush.phase == kActRaiserDeathHeimRush_Final) {
    /* A new native room initializer is a retry, not a new shuffle. */
    if (!s_admitted || !ActRaiserDeathHeimRush_RetryEncounter(&s_rush, s_token) ||
        !ActRaiserDeathHeimRush_Prepare(&s_rush, &s_token)) return false;
  }
  if (s_rush.phase == kActRaiserDeathHeimRush_Retry &&
      !ActRaiserDeathHeimRush_Prepare(&s_rush, &s_token)) return false;
  if (encounter->act != 1 && !ActRaiserDeathHeimRush_Ready(&s_rush, s_token)) return false;
  if (encounter->act == 1 && s_rush.phase != kActRaiserDeathHeimRush_Preparing) return false;
  s_owner = owner;
  s_admitted_id = encounter->id;
  s_admitted = true;
  memset(&s_arrival, 0, sizeof(s_arrival));
  s_departing = s_departure_prepared = s_departure_restored = false;
  return true;
}
bool ActRaiserDeathHeim_BeginArrival(CpuState *cpu, uint16_t owner) {
  const ActRaiserDeathHeimEncounter *encounter = ActRaiserDeathHeim_CurrentEncounter();
  if (!cpu || !s_admitted || s_arrival.staged || owner != s_owner ||
      !encounter || encounter->act != 1 || !Root(cpu, owner, encounter, 0x5000) ||
      cpu_read16(cpu, 0, kActRaiserWram_ActionCameraSubject) != kActRaiserWram_PlayerObject)
    return false;
  s_arrival.owner = owner;
  s_arrival.player_status = cpu_read16(cpu, 0, kActRaiserWram_PlayerObject);
  /* Native $F7F2 explicitly enables every input bit on arrival completion. */
  s_arrival.input = 0xffff;
  for (unsigned i = 0; i < sizeof(s_arrival.actor); ++i)
    s_arrival.actor[i] = cpu_read8(cpu, 0, (uint16_t)(owner + i));
  s_arrival.staged = true;
  cpu_write16(cpu, 0, owner, 0);
  cpu_write16(cpu, 0, owner + 0x06, 0);
  cpu_write16(cpu, 0, owner + 0x08, 0);
  cpu_write16(cpu, 0, owner + 0x12, 0xf7b8);
  cpu_write16(cpu, 0, owner + 0x24, 0);
  cpu_write16(cpu, 0, owner + 0x30, 0x0030);
  cpu_write16(cpu, 0, owner + 0x02,
      cpu_read16(cpu, 0, kActRaiserWram_PlayerObject + 0x02));
  cpu_write16(cpu, 0, owner + 0x04,
      cpu_read16(cpu, 0, kActRaiserWram_PlayerObject + 0x04));
  const uint8_t facing = cpu_read8(cpu, 0, kActRaiserWram_PlayerObject + 0x29) & 0xc0;
  cpu_write8(cpu, 0, owner + 0x29,
      (uint8_t)((cpu_read8(cpu, 0, owner + 0x29) & 0x3f) | facing));
  cpu_write16(cpu, 0, kActRaiserWram_PlayerObject, 0x0800);
  cpu_write16(cpu, 0, 0x00f4, 0);
  return true;
}

static bool ArrivalRoot(CpuState *cpu, uint16_t animation) {
  return Shape(cpu, false) && s_arrival.staged && cpu->X == s_arrival.owner &&
      Root(cpu, cpu->X, ActRaiserDeathHeimRush_Encounter(s_admitted_id), animation);
}
bool ActRaiser_DeathHeimArrivalStartEntry(CpuState *cpu) {
  return !s_arrival.prepared && ArrivalRoot(cpu, 0x5000) &&
      cpu_read16(cpu, 0, cpu->X + 0x12) == 0xf7b8;
}
RecompReturn ActRaiser_DeathHeimArrivalStart(CpuState *cpu) {
  if (!ActRaiser_DeathHeimArrivalStartEntry(cpu))
    ActRaiserHleFatal("Invalid Death Heim arrival start");
  if (!s_admitted || s_rush.phase != kActRaiserDeathHeimRush_Preparing) {
    memset(&s_arrival, 0, sizeof(s_arrival));
    Width(cpu, true);
    return Tail(0x00ff07, 0x00f7b8);
  }
  /* The ordinary player initializer runs after placement and enables input.
   * Reassert the native $F778 lock at the first actual beam update. */
  cpu_write16(cpu, 0, 0x00f4, 0);
  cpu_write16(cpu, 0, kActRaiserWram_PlayerObject, 0x0800);
  if (!ActRaiserDeathHeimTeleportAssets_Prepare(cpu))
    ActRaiserHleFatal("Cannot prepare native Death Heim arrival assets");
  s_arrival.prepared = true;
  /* Re-enter the original beam script setup. Unlike $F778, no caller return
   * address was popped and no natural level scroll registers are cleared. */
  return Tail(0x00f7b8, 0x00f7b8);
}
bool ActRaiser_DeathHeimArrivalCompleteEntry(CpuState *cpu) {
  return ArrivalRoot(cpu, 0x4000);
}
RecompReturn ActRaiser_DeathHeimArrivalComplete(CpuState *cpu) {
  if (!ActRaiser_DeathHeimArrivalCompleteEntry(cpu))
    ActRaiserHleFatal("Invalid Death Heim arrival completion");
  if (s_arrival.prepared) ActRaiserDeathHeimTeleportAssets_Restore(cpu);
  if (s_admitted && s_rush.phase == kActRaiserDeathHeimRush_Preparing) {
    if (!ActRaiserDeathHeimRush_Ready(&s_rush, s_token))
      ActRaiserHleFatal("Cannot admit Death Heim combat after arrival");
    for (unsigned i = 0; i < sizeof(s_arrival.actor); ++i)
      cpu_write8(cpu, 0, (uint16_t)(cpu->X + i), s_arrival.actor[i]);
    cpu_write16(cpu, 0, kActRaiserWram_PlayerObject, s_arrival.player_status);
    cpu_write16(cpu, 0, 0x00f4, s_arrival.input);
  } else {
    cpu_write16(cpu, 0, cpu->X, 0x4000);
  }
  memset(&s_arrival, 0, sizeof(s_arrival));
  /* This handler was reached through the native animation RTS trampoline.
   * A C NORMAL return does not consume its native return address and leaks
   * the actor loop's saved P into the main loop. Let the existing pure RTS
   * leaf retain paired-return ownership and perform the actual unwind. */
  return Tail(0x00868f, 0x00f7c9);
}
void ActRaiserDeathHeim_ObserveScene(CpuState *cpu) {
  if (!cpu) return;
  const uint8_t group = cpu_read8(cpu, 0, kActRaiserWram_MapGroup);
  const uint8_t map = cpu_read8(cpu, 0, kActRaiserWram_CurrentMap);
  s_native_active = group == kActRaiserMapGroup_DeathHeim && map >= 1 && map <= 8;
  if ((RunActive() || s_owner) &&
      (group == kActRaiserMapGroup_NonAction || group == kActRaiserMapGroup_Ending))
    ActRaiserDeathHeim_Reset();
}

static bool CompletionRoot(CpuState *cpu) {
  const ActRaiserDeathHeimEncounter *encounter =
      ActRaiserDeathHeimRush_Encounter(s_admitted_id);
  if (!Shape(cpu, false) || !s_owner || cpu->X != s_owner || !encounter) return false;
  /* FE89 deliberately replaces boss graphics with the hub teleport beam at
   * FEB7. The retained source and admitted actor still identify this victory;
   * requiring the combat $5000 animation here would miss every native win. */
  const bool beam = encounter->act != 1 || s_departure_prepared;
  return !s_arrival.staged && Root(cpu, cpu->X, encounter, beam ? 0x4000 : 0x5000) &&
      (!beam ||
       (cpu_read16(cpu, 0, cpu->X + 0x12) == 0xfeec &&
        cpu_read16(cpu, 0, cpu->X + 0x30) == 0x0030));
}

static bool VictoryRoot(CpuState *cpu) {
  const ActRaiserDeathHeimEncounter *encounter = ActRaiserDeathHeim_CurrentEncounter();
  return CompletionRoot(cpu) && s_admitted && encounter &&
      encounter->id == s_admitted_id &&
      (s_rush.phase == kActRaiserDeathHeimRush_Combat ||
       s_rush.phase == kActRaiserDeathHeimRush_Final);
}

static bool PlayerDying(CpuState *cpu) {
  return cpu_read16(cpu, 0, kActRaiserWram_ActionCameraSubject) == kActRaiserWram_PlayerObject &&
      (cpu_read16(cpu, 0, kActRaiserWram_PlayerFlags) & 0x0440) != 0;
}

bool ActRaiserDeathHeim_RequestHubReturn(CpuState *cpu) {
  const ActRaiserDeathHeimEncounter *encounter = ActRaiserDeathHeim_CurrentEncounter();
  if (!VictoryRoot(cpu) || PlayerDying(cpu) ||
      (encounter->act == 1 && !s_departure_prepared) ||
      !ActRaiserDeathHeimRush_Victory(&s_rush, s_token)) return false;
  s_admitted = false;
  cpu_write16(cpu, 0, kActRaiserWram_DeathHeimProgress,
              ActRaiserDeathHeimRush_NativeProgress(&s_rush));
  cpu_write16(cpu, 0, kActRaiserWram_DestinationMap, 0x0701);
  return true;
}
uint8_t ActRaiserDeathHeim_EyeMask(unsigned city) {
  return ActRaiserDeathHeimRush_EyeMask(&s_rush, city);
}
static bool Eye(CpuState *cpu, uint16_t owner, unsigned index) {
  return Slot(owner) && index < 12 &&
      cpu_read16(cpu, 0, owner + 0x32) == kEyes[index].source &&
      cpu_read16(cpu, 0, owner + 0x34) == kEyes[index].x &&
      cpu_read16(cpu, 0, owner + 0x36) == kEyes[index].y &&
      cpu_read16(cpu, 0, owner + 0x16) == 0x4000 &&
      cpu_read8(cpu, 0, owner + 0x18) == 0x7e;
}
unsigned ActRaiserDeathHeim_EyeActorCity(CpuState *cpu, uint16_t owner) {
  if (!cpu || !RunActive() ||
      s_rush.config.roster != kActRaiserDeathHeimRoster_Extended || !Hub(cpu)) return 0;
  for (unsigned i = 0; i < 12; ++i) if (Eye(cpu, owner, i)) return i / 2 + 1;
  return 0;
}
bool ActRaiser_DeathHeimHubRouteEntry(CpuState *cpu) {
  return Shape(cpu, true) && CanBegin(cpu) && Slot(cpu->X) &&
      cpu_read16(cpu, 0, cpu->X + 0x32) == 0xf3c8;
}
RecompReturn ActRaiser_DeathHeimHubRoute(CpuState *cpu) {
  if (!ActRaiser_DeathHeimHubRouteEntry(cpu)) ActRaiserHleFatal("Invalid Death Heim hub route");
  BeginHub(cpu);
  if (!ActRaiserDeathHeimRush_Prepare(&s_rush, &s_token))
    ActRaiserHleFatal("Death Heim route already has an encounter");
  const ActRaiserDeathHeimEncounter *encounter = ActRaiserDeathHeim_CurrentEncounter();
  if (!encounter) ActRaiserHleFatal("Death Heim route is missing its encounter");
  s_admitted = false;
  if (encounter->act == 1) {
    if (!ActRaiserDeathHeimArenas_Begin(cpu, encounter->city))
      ActRaiserHleFatal("Death Heim Act 1 arena preparation failed");
  } else {
    cpu_write8(cpu, 0, kActRaiserWram_DestinationMap, encounter->map);
    cpu_write8(cpu, 0, kActRaiserWram_DestinationMapGroup, encounter->map_group);
  }
  cpu_write16(cpu, 0, kActRaiserWram_DeathHeimProgress,
              ActRaiserDeathHeimRush_NativeProgress(&s_rush));
  /* Native final instruction is LDA $18; STA $1B; REP #$20. */
  cpu->A = (uint16_t)((cpu->A & 0xff00) | cpu_read8(cpu, 0, kActRaiserWram_DestinationMapGroup));
  ActRaiserCpuHle_SetNegativeZero8(cpu, (uint8_t)cpu->A);
  cpu->_flag_C = 0;
  cpu->_flag_V = 0;
  Width(cpu, false);
  return Tail(0x00f3f2, 0x00f3e4);
}
bool ActRaiser_DeathHeimEyesEntry(CpuState *cpu) {
  if (!Shape(cpu, false) || !CanBegin(cpu) || !Slot(cpu->X)) return false;
  for (unsigned i = 0; i < 12; ++i)
    if (!Eye(cpu, (uint16_t)(cpu->X + (i + 1) * 0x40), i)) return false;
  return true;
}
RecompReturn ActRaiser_DeathHeimEyes(CpuState *cpu) {
  if (!ActRaiser_DeathHeimEyesEntry(cpu)) ActRaiserHleFatal("Invalid Death Heim statue actors");
  BeginHub(cpu);
  const ActRaiserDeathHeimEncounter *next = ActRaiserDeathHeim_CurrentEncounter();
  const ActRaiserDeathHeimEncounter *last = ActRaiserDeathHeimRush_Encounter(s_rush.last_completed);
  ActRaiserDeathHeimEyes_BeginHub(cpu, next, last);
  for (unsigned city = 1; city <= 6; ++city) {
    const uint8_t lit = ActRaiserDeathHeimRush_EyeMask(&s_rush, city);
    const bool just_beaten = last && last->city == city;
    for (unsigned part = 0; part < 2; ++part) {
      const uint16_t actor = (uint16_t)(cpu->X + ((city - 1) * 2 + part + 1) * 0x40);
      if (s_rush.config.roster == kActRaiserDeathHeimRoster_Extended) {
        /* Shared sprite emission plays each physical eye independently. Keep
         * the glow actor idle/alive even with both eyes dark; it emits zero
         * parts then. The separate native flare still runs for every victory,
         * but its idle visual contains a glow pixel. Retire it on later hub
         * visits once either of that city's eyes has been extinguished. */
        if (part == 1 && lit != 3 && !just_beaten) cpu_write16(cpu, 0, actor, 0x4000);
        cpu_write16(cpu, 0, actor + 0x38, part == 1 && just_beaten ? 0xffff : 0);
      } else {
        if (!lit && !just_beaten) cpu_write16(cpu, 0, actor, 0x4000);
        cpu_write16(cpu, 0, actor + 0x38,
            !lit && just_beaten ? 0xffff : next && next->city == city ? 1 : 0);
      }
    }
  }
  cpu_write16(cpu, 0, kActRaiserWram_DeathHeimProgress,
              ActRaiserDeathHeimRush_NativeProgress(&s_rush));
  cpu->A = 1;
  ActRaiserCpuHle_SetNegativeZero16(cpu, cpu->A);
  return Tail(0x00f5a5, 0x00f534);
}
bool ActRaiser_DeathHeimAdmitEntry(CpuState *cpu) {
  const ActRaiserDeathHeimEncounter *encounter = ActRaiserDeathHeim_CurrentEncounter();
  return Shape(cpu, false) && RunActive() && encounter &&
      encounter->map_group == 7 && Root(cpu, cpu->X, encounter, 0x5000);
}
RecompReturn ActRaiser_DeathHeimAdmit(CpuState *cpu) {
  if (!ActRaiser_DeathHeimAdmitEntry(cpu) || !ActRaiserDeathHeim_AdmitEncounter(cpu, cpu->X))
    ActRaiserHleFatal("Invalid Death Heim boss admission");
  cpu->A = 0xffff;
  ActRaiserCpuHle_SetNegativeZero16(cpu, cpu->A);
  cpu_write16(cpu, 0, 0x00f4, cpu->A);
  return Tail(0x00f7f7, 0x00f7f2);
}
bool ActRaiser_DeathHeimVictoryEntry(CpuState *cpu) {
  return CompletionRoot(cpu) &&
      (cpu_read8(cpu, 0, kActRaiserWram_MapGroup) == 7 || s_departure_prepared);
}
RecompReturn ActRaiser_DeathHeimVictory(CpuState *cpu) {
  if (!ActRaiser_DeathHeimVictoryEntry(cpu)) ActRaiserHleFatal("Invalid Death Heim victory");
  if (s_departure_prepared && !s_departure_restored) {
    ActRaiserDeathHeimTeleportAssets_Restore(cpu);
    s_departure_restored = true;
  }
  s_departing = false;
  if (!VictoryRoot(cpu)) {
    Width(cpu, true);
    return Tail(0x00ff07, 0x00feec);
  }
  if (PlayerDying(cpu)) {
    if (!ActRaiserDeathHeim_RetryEncounter(cpu))
      ActRaiserHleFatal("Cannot retire Death Heim victory during player death");
    Width(cpu, true);
    return Tail(0x00ff07, 0x00feec);
  }
  if (!ActRaiserDeathHeim_RequestHubReturn(cpu)) ActRaiserHleFatal("Invalid Death Heim victory");
  const uint8_t map = cpu_read8(cpu, 0, kActRaiserWram_CurrentMap);
  if (map == 8) cpu_write8(cpu, 0, 0x0334, 1);
  cpu->A = (uint16_t)(0x0700 | (map == 8 ? 1 : map));
  cpu->_flag_C = map >= 8;
  cpu->_flag_Z = map == 8;
  cpu->_flag_N = map < 8;
  if (map == 8) ActRaiserCpuHle_SetNegativeZero8(cpu, 1);
  Width(cpu, true);
  return Tail(0x00ff07, 0x00feec);
}
bool ActRaiser_DeathHeimAct1VictoryEntry(CpuState *cpu) {
  const ActRaiserDeathHeimEncounter *encounter =
      ActRaiserDeathHeimRush_Encounter(s_admitted_id);
  return CompletionRoot(cpu) && encounter && encounter->act == 1 &&
      cpu_read16(cpu, 0, cpu->X + 0x14) == 0;
}
RecompReturn ActRaiser_DeathHeimAct1Victory(CpuState *cpu) {
  if (!ActRaiser_DeathHeimAct1VictoryEntry(cpu))
    ActRaiserHleFatal("Invalid Death Heim Act 1 victory");
  if (VictoryRoot(cpu)) {
    if (s_departing) return Tail(0x00868f, 0x00a5fa);
    if (PlayerDying(cpu)) {
      if (!ActRaiserDeathHeim_RetryEncounter(cpu))
        ActRaiserHleFatal("Cannot retire Death Heim Act 1 victory during player death");
    } else {
      s_departing = true;
      /* Native $FE89 owns the 64-frame pause, player engulf, state 15, and
       * final 32-frame wait. Progress remains uncommitted until $FEEC. */
      cpu_write16(cpu, 0, cpu->X + 0x12, 0xfe89);
      return Tail(0x00fe89, 0x00a5fa);
    }
  }
  /* A retired attempt must not fall through to an ordinary act clear. */
  Width(cpu, true);
  return Tail(0x00ff07, 0x00a5fa);
}
bool ActRaiser_DeathHeimDepartureStartEntry(CpuState *cpu) {
  return s_departing && !s_departure_prepared && Shape(cpu, false) &&
      cpu->X == s_owner &&
      Root(cpu, cpu->X, ActRaiserDeathHeimRush_Encounter(s_admitted_id), 0x5000) &&
      cpu_read16(cpu, 0, cpu->X + 0x12) == 0xfe9d &&
      cpu_read16(cpu, 0, cpu->X + 0x30) == 0x0030;
}
RecompReturn ActRaiser_DeathHeimDepartureStart(CpuState *cpu) {
  if (!ActRaiser_DeathHeimDepartureStartEntry(cpu))
    ActRaiserHleFatal("Invalid Death Heim departure start");
  if (!s_admitted || s_rush.phase != kActRaiserDeathHeimRush_Combat) {
    s_departing = false;
    Width(cpu, true);
    return Tail(0x00ff07, 0x00fe9d);
  }
  if (!ActRaiserDeathHeimTeleportAssets_Prepare(cpu))
    ActRaiserHleFatal("Cannot prepare native Death Heim departure assets");
  /* Aitos can die while vertically flipped. Native FF0D copies only the
   * horizontal bit because rematch roots are never vertically flipped. */
  const uint8_t facing = cpu_read8(cpu, 0, kActRaiserWram_PlayerObject + 0x29) & 0xc0;
  cpu_write8(cpu, 0, cpu->X + 0x29,
      (uint8_t)((cpu_read8(cpu, 0, cpu->X + 0x29) & 0x3f) | facing));
  s_departure_prepared = true;
  return Tail(0x00fe9d, 0x00fe9d);
}
bool ActRaiserDeathHeim_Fingerprint(const uint8_t previous[32], uint8_t out[32], bool *baseline) {
  if (!previous || !out || !baseline) return false;
  if (!RunActive()) { memmove(out, previous, 32); *baseline = true; return true; }
  uint8_t bytes[176] = "ARDEATHHEIM-R1";
  memcpy(bytes + 16, previous, 32);
  if (!ActRaiserDeathHeimRushConfig_Encode(&s_rush.config, bytes + 48)) return false;
  memcpy(bytes + 64, s_rush.sequence, 12);
  bytes[76] = s_rush.count;
  bytes[77] = s_rush.index;
  bytes[78] = s_rush.active_id;
  bytes[79] = s_rush.last_completed;
  bytes[80] = (uint8_t)s_rush.phase;
  bytes[81] = s_admitted;
  bytes[82] = (uint8_t)s_rush.completed_mask;
  bytes[83] = (uint8_t)(s_rush.completed_mask >> 8);
  ByteOrder_WriteLe32(bytes + 84, s_rush.attempt);
  bytes[88] = (uint8_t)s_owner;
  bytes[89] = (uint8_t)(s_owner >> 8);
  bytes[90] = s_admitted_id;
  bytes[91] = s_departing;
  bytes[92] = s_departure_prepared;
  bytes[93] = s_departure_restored;
  bytes[94] = s_arrival.staged;
  bytes[95] = s_arrival.prepared;
  memcpy(bytes + 96, s_arrival.actor, sizeof(s_arrival.actor));
  ByteOrder_WriteLe32(bytes + 160, s_arrival.owner);
  ByteOrder_WriteLe32(bytes + 164, s_arrival.player_status);
  ByteOrder_WriteLe32(bytes + 168, s_arrival.input);
  /* The process-local generation prevents stale callbacks; it is deliberately
   * excluded because the same recorded attempt must replay after a reset. */
  if (!sr_support_sha256(bytes, sizeof(bytes), out)) return false;
  *baseline = false;
  return true;
}
